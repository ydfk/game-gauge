#include "saves.h"
#include "common/platform.h"
#include "version.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <unordered_set>

using namespace std::chrono_literals;

namespace gauge {
namespace {
constexpr size_t response_limit = 96 * 1024;
constexpr size_t request_limit = 32 * 1024;
const std::unordered_set<std::string> commands{
    "saves.status", "saves.games.list", "saves.games.select", "saves.games.add", "saves.configure",
    "saves.backup.enqueue", "saves.records.list", "saves.records.detail", "saves.records.verify",
    "saves.records.edit", "saves.records.protect", "saves.records.delete", "saves.restore.enqueue",
    "saves.migration.preview", "saves.migration.apply", "saves.cloud.configure", "saves.cloud.test.enqueue",
    "saves.cloud.retry.enqueue", "saves.cloud.upload.enqueue", "saves.cloud.list.enqueue",
    "saves.cloud.page", "saves.cloud.download.enqueue"
};
const std::array<const char*, 20> fields{
    "request_id", "game_id", "profile", "app_id", "executable", "save_dir", "backup_dir", "interval_minutes",
    "keep_auto", "auto_enabled", "hotkey", "enabled", "offset", "limit", "shortcut", "record_id", "title",
    "note", "locked", "confirmed"
};
void checked(BOOL ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
void require_standard_token() {
    HANDLE raw{};
    checked(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw), "无法核对工作进程权限");
    UniqueHandle token(raw);
    TOKEN_ELEVATION elevation{};
    DWORD length{};
    checked(GetTokenInformation(raw, TokenElevation, &elevation, sizeof(elevation), &length), "无法核对工作进程权限");
    if (elevation.TokenIsElevated) throw std::runtime_error("请用普通用户权限运行游戏仪表以使用存档备份");
}
struct Attributes {
    LPPROC_THREAD_ATTRIBUTE_LIST ptr{};
    ~Attributes() { if (ptr) DeleteProcThreadAttributeList(ptr); }
};
struct Child {
    UniqueHandle job, process, input, output;
    std::string buffered;
    uint64_t sequence{};

    void launch() {
        require_standard_token();
        const auto exe = executable_dir() / L"GameGauge.Saves.exe";
        if (!std::filesystem::is_regular_file(exe)) throw std::runtime_error("缺少 GameGauge.Saves.exe");
        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        HANDLE read{}, write{};
        checked(CreatePipe(&read, &write, &security, 65536), "无法创建工作进程输入管道");
        UniqueHandle child_input(read);
        input.reset(write);
        checked(SetHandleInformation(input.value, HANDLE_FLAG_INHERIT, 0), "无法限制输入句柄继承");
        checked(CreatePipe(&read, &write, &security, 65536), "无法创建工作进程输出管道");
        output.reset(read);
        UniqueHandle child_output(write);
        checked(SetHandleInformation(output.value, HANDLE_FLAG_INHERIT, 0), "无法限制输出句柄继承");
        UniqueHandle discard(CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        checked(static_cast<bool>(discard), "无法配置工作进程诊断输出");

        SIZE_T length{};
        InitializeProcThreadAttributeList(nullptr, 1, 0, &length);
        std::vector<std::byte> memory(length);
        auto list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(memory.data());
        checked(InitializeProcThreadAttributeList(list, 1, 0, &length), "无法配置工作进程句柄");
        Attributes attributes{list};
        HANDLE allowed[]{child_input.value, child_output.value, discard.value};
        checked(UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            allowed, sizeof(allowed), nullptr, nullptr), "无法限制工作进程句柄继承");
        job.reset(CreateJobObjectW(nullptr, nullptr));
        checked(static_cast<bool>(job), "无法创建工作进程退出控制");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        checked(SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)), "无法配置工作进程退出控制");

        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = allowed[0];
        startup.StartupInfo.hStdOutput = allowed[1];
        startup.StartupInfo.hStdError = allowed[2];
        startup.lpAttributeList = list;
        PROCESS_INFORMATION child{};
        auto root = std::filesystem::absolute(data_dir() / L"saves");
        auto command = L"\"" + exe.wstring() + L"\" --data-dir \"" + root.wstring() + L"\"";
        checked(CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE,
            EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW | CREATE_SUSPENDED | BELOW_NORMAL_PRIORITY_CLASS,
            nullptr, nullptr, &startup.StartupInfo, &child), "无法启动存档工作进程");
        process.reset(child.hProcess);
        UniqueHandle process_thread(child.hThread);
        if (!AssignProcessToJobObject(job.value, process.value)) {
            TerminateProcess(process.value, 1);
            throw std::runtime_error("无法管理存档工作进程退出");
        }
        checked(ResumeThread(process_thread.value) != DWORD(-1), "无法运行存档工作进程");
    }

    void send(Json request) {
        request["id"] = ++sequence;
        request["version"] = 1;
        auto bytes = request.dump() + "\n";
        DWORD wrote{};
        checked(bytes.size() <= request_limit && WriteFile(input.value, bytes.data(), static_cast<DWORD>(bytes.size()), &wrote, nullptr)
            && wrote == bytes.size(), "无法投递存档任务");
    }

    Json exchange(const Json& command, std::stop_token stop, std::chrono::seconds timeout) {
        send(command);
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline) {
            const auto newline = buffered.find('\n');
            if (newline != std::string::npos) {
                auto response = Json::parse(buffered.begin(), buffered.begin() + newline);
                buffered.erase(0, newline + 1);
                if (!response.is_object() || response.value("version", 0) != 1 ||
                    response.value("id", uint64_t{}) != sequence || !response.contains("ok") || !response["ok"].is_boolean())
                    throw std::runtime_error("存档工作进程响应协议不匹配");
                return response;
            }
            DWORD available{};
            checked(PeekNamedPipe(output.value, nullptr, 0, nullptr, &available, nullptr), "存档工作进程已退出");
            if (available) {
                char chunk[8192]; DWORD count{};
                checked(ReadFile(output.value, chunk, std::min<DWORD>(available, sizeof(chunk)), &count, nullptr),
                    "无法读取存档任务响应");
                buffered.append(chunk, count);
                if (buffered.size() > response_limit) throw std::runtime_error("存档工作进程响应过大");
            } else if (WaitForSingleObject(process.value, 25) == WAIT_OBJECT_0) {
                throw std::runtime_error("存档工作进程已退出");
            }
        }
        throw std::runtime_error(stop.stop_requested() ? "宿主正在关闭，任务可能已提交" : "存档任务超时，任务可能已提交");
    }

    void close() {
        input.reset();
        if (process) {
            // 等待当前事务到达安全点。进程仍持有的句柄最终由 Job Object 收尾。
            const auto deadline = std::chrono::steady_clock::now() + 12s;
            while (std::chrono::steady_clock::now() < deadline &&
                WaitForSingleObject(process.value, 40) == WAIT_TIMEOUT) {
                DWORD available{};
                if (PeekNamedPipe(output.value, nullptr, 0, nullptr, &available, nullptr) && available) {
                    char discard[8192];
                    ReadFile(output.value, discard, std::min<DWORD>(available, sizeof(discard)), &available, nullptr);
                }
            }
        }
    }
};

std::string identifier(const Json& input, const char* key = "request_id") {
    const auto id = input.value(key, std::string{});
    if (id.size() != 36) throw std::runtime_error("存档任务需要 UUID 请求标识");
    for (size_t index = 0; index < id.size(); ++index) {
        const auto ch = id[index];
        const bool separator = index == 8 || index == 13 || index == 18 || index == 23;
        if (separator ? ch != '-' : !((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
            throw std::runtime_error("存档任务需要 UUID 请求标识");
    }
    return id;
}
}

std::optional<std::pair<UINT, UINT>> saves_hotkey(std::string_view value) {
    const auto trim = [](std::string_view text) {
        const auto first = text.find_first_not_of(" \t\r\n");
        if (first == std::string_view::npos) return std::string_view{};
        return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
    };
    value = trim(value);
    if (value.empty()) return std::pair<UINT, UINT>{0, 0};
    std::string normalized(value);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char key) {
        return static_cast<char>(std::toupper(key));
    });
    value = normalized;
    UINT mods = MOD_NOREPEAT;
    while (value.find('+') != std::string_view::npos) {
        const auto next = value.find('+');
        const auto part = trim(value.substr(0, next));
        const UINT bit = (part == "CTRL" || part == "CONTROL") ? MOD_CONTROL : part == "ALT" ? MOD_ALT : part == "SHIFT" ? MOD_SHIFT : 0;
        if (!bit || (mods & bit)) return std::nullopt;
        mods |= bit;
        value.remove_prefix(next + 1);
    }
    value = trim(value);
    if (!(mods & (MOD_CONTROL | MOD_ALT))) return std::nullopt;
    if (value.size() == 1 && ((value[0] >= '0' && value[0] <= '9') || (value[0] >= 'A' && value[0] <= 'Z')))
        return std::pair{mods, static_cast<UINT>(value[0])};
    if (value.size() < 2 || value.size() > 3 || value[0] != 'F')
        return std::nullopt;
    UINT number{};
    for (const char digit : value.substr(1)) {
        if (digit < '0' || digit > '9') return std::nullopt;
        number = number * 10 + digit - '0';
    }
    const auto reserved = (mods & (MOD_CONTROL | MOD_ALT | MOD_SHIFT)) ==
        (MOD_CONTROL | MOD_ALT | MOD_SHIFT) && number >= 6 && number <= 8;
    if (!number || number > 24 || number == 12 || reserved) return std::nullopt;
    return std::pair{mods, VK_F1 + number - 1};
}

Saves::Saves() : thread_([this](std::stop_token stop) { run(stop); }) {}
Saves::~Saves() { thread_.request_stop(); wake_.notify_all(); }

void Saves::drain() {
    std::lock_guard lock(mutex_);
    draining_ = true;
    while (!pending_.empty()) {
        complete(pending_.front(), "failed", {{"error", "游戏仪表正在退出，任务未开始"}});
        pending_.pop_front();
    }
}

bool Saves::restoring() {
    std::lock_guard lock(mutex_);
    return active_command_ == "saves.restore.enqueue";
}

Json Saves::request(const Json& input) {
    const auto command = input.at("command").get<std::string>();
    std::lock_guard lock(mutex_);
    if (command == "saves.host.status")
        return {{"ok", true}, {"state", state_}, {"error", error_}, {"hotkey", hotkey_}};
    if (command == "saves.job.status") {
        const auto id = identifier(input, "job_id");
        if (const auto job = jobs_.find(id); job != jobs_.end()) return job->second.status;
        const auto lookup = "lookup:" + id;
        if (const auto job = jobs_.find(lookup); job != jobs_.end()) return job->second.status;
        if (draining_ || state_ != "ready" || pending_.size() >= 12)
            return {{"ok", false}, {"error", "暂时无法读取原任务，请稍后重试"}};
        Json status{{"ok", true}, {"job_id", id}, {"state", "queued"}};
        jobs_.emplace(lookup, Job{Json{{"command", "lookup_job"}, {"job_id", id}}, status});
        pending_.push_back(lookup);
        wake_.notify_one();
        return status;
    }
    if (!commands.contains(command)) throw std::runtime_error("未知存档命令");
    if (draining_) return {{"ok", false}, {"error", "游戏仪表正在退出，任务未开始"}};
    const auto id = identifier(input);
    Json forwarded{{"command", command}};
    const auto add = [&](const char* key) {
        if (input.contains(key)) {
            if (input[key].is_structured()) throw std::runtime_error("存档任务参数必须是单值");
            forwarded[key] = input[key];
        }
    };
    for (const auto key : fields) add(key);
    for (const auto key : {"token", "url", "user", "automatic"}) add(key);
    if (forwarded.dump().size() > request_limit - 128) throw std::runtime_error("存档任务请求过大");
    if (command == "saves.configure" && !saves_hotkey(forwarded.value("hotkey", std::string{})))
        throw std::runtime_error("请使用 Ctrl 或 Alt 加字母、数字或 F1–F24。F12 和监控快捷键已保留。");
    if (const auto existing = jobs_.find(id); existing != jobs_.end()) {
        if (existing->second.input != forwarded) throw std::runtime_error("请求标识已用于另一个存档任务");
        return {{"ok", true}, {"job_id", id}};
    }
    if (state_ != "ready") return {{"ok", false}, {"error", error_.empty() ? "存档工作进程正在启动" : error_}};
    if (pending_.size() >= 12) return {{"ok", false}, {"error", "存档任务队列已满，请稍后重试"}};
    jobs_.emplace(id, Job{std::move(forwarded), {{"ok", true}, {"job_id", id}, {"state", "queued"}}});
    pending_.push_back(id);
    wake_.notify_one();
    return {{"ok", true}, {"job_id", id}};
}

void Saves::complete(const std::string& id, std::string state, Json response) {
    auto& status = jobs_.at(id).status;
    status["state"] = std::move(state);
    if (id.starts_with("lookup:") && response.contains("result")) {
        status = std::move(response["result"]);
    } else if (response.contains("result")) {
        status["result"] = std::move(response["result"]);
        if (status["result"].contains("hotkey")) hotkey_ = status["result"]["hotkey"].get<std::string>();
    }
    if (response.contains("error")) status["error"] = std::move(response["error"]);
    finished_.push_back(id);
    if (finished_.size() > 64) {
        jobs_.erase(finished_.front());
        finished_.pop_front();
    }
}

void Saves::run(std::stop_token stop) {
    auto delay = 1s;
    while (!stop.stop_requested()) {
        Child child;
        std::string active;
        try {
            child.launch();
            const auto hello = child.exchange({{"command", "hello"}}, stop, 6s);
            if (!hello.value("ok", false)) throw std::runtime_error(hello.value("error", std::string("存档工作进程启动失败")));
            const auto& result = hello.at("result");
            if (result.value("protocol", 0) != 1 ||
                result.value("app_version", std::string{}) != GAMEGAUGE_VERSION)
                throw std::runtime_error("存档工作进程版本不匹配");
            const auto started = std::chrono::steady_clock::now();
            {
                std::lock_guard lock(mutex_);
                state_ = "ready"; error_.clear();
                hotkey_ = result.value("hotkey", hotkey_);
            }
            while (!stop.stop_requested()) {
                Json input;
                {
                    std::unique_lock lock(mutex_);
                    wake_.wait_for(lock, stop, 1s, [&] { return !pending_.empty(); });
                    if (stop.stop_requested()) break;
                    if (WaitForSingleObject(child.process.value, 0) == WAIT_OBJECT_0)
                        throw std::runtime_error("存档工作进程异常退出");
                    if (pending_.empty()) continue;
                    active = std::move(pending_.front());
                    pending_.pop_front();
                    auto& job = jobs_.at(active);
                    job.status["state"] = "running";
                    input = job.input;
                    active_command_ = input["command"].get<std::string>();
                }
                auto response = child.exchange(input, stop, 30min);
                std::lock_guard lock(mutex_);
                const auto success = response.value("ok", false);
                complete(active, success ? "succeeded" : "failed", std::move(response));
                active.clear();
                active_command_.clear();
                if (std::chrono::steady_clock::now() - started > 60s) delay = 1s;
            }
        } catch (const std::exception& error) {
            std::lock_guard lock(mutex_);
            state_ = "unavailable"; error_ = error.what();
            if (!active.empty()) complete(active, "interrupted", {{"error", error_}});
            active_command_.clear();
            while (!pending_.empty()) {
                complete(pending_.front(), "failed", {{"error", "工作进程不可用，任务未开始"}});
                pending_.pop_front();
            }
        }
        child.close();
        std::unique_lock lock(mutex_);
        wake_.wait_for(lock, stop, delay, [] { return false; });
        delay = std::min(delay * 2, 60s);
    }
}
}
