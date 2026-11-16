#include "worker.h"
#include "version.h"
#include <objbase.h>
#include <shellapi.h>

namespace gauge::saves {
namespace {
using namespace std::chrono_literals;
constexpr size_t max_request = 32 * 1024, max_response = 96 * 1024;
constexpr unsigned protocol = 1;

class Pipe {
public:
    std::optional<Json> read_until(Clock::time_point deadline) {
        while (Clock::now() < deadline) {
            if (const auto newline = buffered_.find('\n'); newline != std::string::npos) {
                require(newline < max_request, "存档请求过大。");
                auto message = Json::parse(buffered_.begin(), buffered_.begin() + newline);
                buffered_.erase(0, newline + 1);
                require(message.is_object() && message.value("version", 0) == protocol &&
                    message.contains("id") && message["id"].is_number_unsigned(), "存档协议不匹配。");
                return message;
            }
            require(buffered_.size() < max_request, "存档请求过大。");
            DWORD bytes{};
            if (!PeekNamedPipe(input_, nullptr, 0, nullptr, &bytes, nullptr)) {
                closed_ = true;
                return {};
            }
            if (!bytes) { Sleep(30); continue; }
            char chunk[4096];
            DWORD count{};
            if (!ReadFile(input_, chunk, std::min<DWORD>(bytes, sizeof(chunk)), &count, nullptr) || !count) {
                closed_ = true;
                return {};
            }
            buffered_.append(chunk, count);
        }
        return {};
    }
    bool closed() const { return closed_; }
    void respond(const Json& request, const std::function<Json()>& action) const {
        Json response{{"version", protocol}, {"id", request.at("id")}};
        try {
            response["result"] = action();
            response["ok"] = true;
        } catch (const std::exception& error) {
            response["ok"] = false;
            response["error"] = failure_message(error).substr(0, 4000);
        }
        auto bytes = response.dump(-1, ' ', false, Json::error_handler_t::replace) + "\n";
        if (bytes.size() > max_response)
            bytes = Json{{"version", protocol}, {"id", request.at("id")}, {"ok", false}, {"error", "响应过大，请缩小查询范围"}}.dump() + "\n";
        append_file(output_, {reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()});
    }
private:
    HANDLE input_{GetStdHandle(STD_INPUT_HANDLE)}, output_{GetStdHandle(STD_OUTPUT_HANDLE)};
    std::string buffered_;
    bool closed_{};
};
fs::path root_path() {
    int count{};
    auto args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!args) throw std::runtime_error("无法读取存档工作进程参数。");
    const auto options = std::unique_ptr<wchar_t*, decltype(&LocalFree)>(args, LocalFree);
    require(count == 3 && std::wstring_view(args[1]) == L"--data-dir", "缺少存档数据目录。");
    const fs::path root(args[2]);
    require(root.is_absolute(), "请选择绝对存档数据目录。");
    auto ancestor = root;
    while (!saves::exists(ancestor)) {
        require(ancestor != ancestor.parent_path(), "无法找到存档数据磁盘。");
        ancestor = ancestor.parent_path();
    }
    plain_directory(ancestor);
    fs::create_directories(extended_path(root));
    plain_directory(root);
    return extended_path(root);
}
int serve() {
    Pipe pipe;
    const auto hello = pipe.read_until(Clock::now() + 8s);
    require(hello && hello->value("command", std::string{}) == "hello", "缺少存档握手。");
    UniqueHandle lock;
    std::optional<Worker> worker;
    std::optional<Journal> journal;
    pipe.respond(*hello, [&]() -> Json {
        const auto root = root_path();
        lock.reset(CreateFileW((root / L"worker.lock").c_str(), GENERIC_WRITE, 0, nullptr,
            OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        require(static_cast<bool>(lock), "存档数据目录正由另一个进程使用。");
        BY_HANDLE_FILE_INFORMATION info{};
        require(GetFileInformationByHandle(lock.value, &info) && !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT),
            "存档锁文件被重定向。");
        journal.emplace(root);
        worker.emplace(root);
        return {{"protocol", protocol}, {"app_version", GAMEGAUGE_VERSION}, {"hotkey", worker->hotkey()}};
    });
    if (!worker) return 1;
    auto tick = Clock::now() + 2s;
    while (!pipe.closed()) {
        if (const auto message = pipe.read_until(tick)) {
            const auto command = required_string(*message, "command");
            if (command == "shutdown") break;
            pipe.respond(*message, [&] {
                if (command == "lookup_job") return journal->lookup(*message);
                return journal->execute(*message, [&] { return worker->request(*message); });
            });
        }
        if (!pipe.closed() && Clock::now() >= tick) {
            worker->tick();
            tick = Clock::now() + 2s;
        }
    }
    return 0;
}
}
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int result = 1;
    try { result = gauge::saves::serve(); }
    catch (const std::exception&) { /* 早期管道错误由宿主处理。 */ }
    if (SUCCEEDED(com)) CoUninitialize();
    return result;
}
