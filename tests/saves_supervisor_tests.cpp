#include "host/ipc_server.h"
#include "host/saves.h"
#include "saves/store.h"
#include <chrono>
#include <fstream>
#include <iostream>

using namespace gauge;
using namespace gauge::saves;
using namespace std::chrono_literals;

namespace {
bool elevated() {
    UniqueHandle token;
    require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value) != FALSE, "无法读取测试进程权限。");
    TOKEN_ELEVATION level{};
    DWORD length{};
    require(GetTokenInformation(token.value, TokenElevation, &level, sizeof(level), &length) != FALSE, "无法核对测试进程权限。");
    return level.TokenIsElevated != 0;
}
class TestIpc {
public:
    TestIpc() : pipe_(L"\\\\.\\pipe\\GameGauge-Saves-Test-" + wide(new_request_id())),
        server_([this](const Json& input) {
            if (input.at("command") == "status") return Json{{"ok", true}, {"fixture", true}};
            return supervisor_.request(input);
        }, pipe_) {}
    Json send(const Json& input) const { return ipc_request(input, pipe_); }
    void ready() const {
        const auto until = std::chrono::steady_clock::now() + 10s;
        Json response;
        while (std::chrono::steady_clock::now() < until) {
            response = send({{"command", "saves.host.status"}});
            if (response.at("state") == "ready") return;
            std::this_thread::sleep_for(50ms);
        }
        throw std::runtime_error("工作进程未通过握手：" + response.dump());
    }
    std::string enqueue(Json input) const {
        if (!input.contains("request_id")) input["request_id"] = new_request_id();
        const auto response = send(input);
        if (!response.value("ok", false)) throw std::runtime_error(response.dump());
        return required_string(response, "job_id");
    }
    Json finish(const std::string& id) const {
        const auto until = std::chrono::steady_clock::now() + 10s;
        while (std::chrono::steady_clock::now() < until) {
            const auto response = send({{"command", "saves.job.status"}, {"job_id", id}});
            if (response.contains("state")) {
                const auto state = required_string(response, "state");
                if (state == "succeeded") return response.at("result");
                if (state == "queued" || state == "running") {
                    std::this_thread::sleep_for(60ms);
                    continue;
                }
            }
            throw std::runtime_error("存档任务失败：" + response.dump());
        }
        throw std::runtime_error("存档任务超时。");
    }
private:
    std::wstring pipe_;
    Saves supervisor_;
    IpcServer server_;
};
}

int main() {
    try {
        if (elevated()) {
            std::cout << "Elevated session: normal-user sidecar contract skipped.\n";
            return 77;
        }
        const TemporaryDirectory fixture(fs::temp_directory_path(), L"GameGauge-sidecar-");
        const auto slot = fixture.path() / L"76561198000000001";
        fs::create_directory(slot);
        {
            std::ofstream file(slot / main_save, std::ios::binary);
            file << "isolated pipe save";
            require(static_cast<bool>(file), "创建合成存档失败。");
        }
        set_data_dir(fixture.path() / L"host");
        const auto request = Json{{"command", "saves.backup.enqueue"}, {"request_id", new_request_id()},
            {"game_id", pbz_id}, {"profile", "76561198000000001"}};
        std::string job, record;
        {
            const TestIpc ipc;
            ipc.ready();
            ipc.finish(ipc.enqueue({{"command", "saves.configure"}, {"game_id", pbz_id}, {"profile", ""},
                {"save_dir", path_text(slot)}, {"backup_dir", ""}, {"hotkey", ""}, {"interval_minutes", 10u},
                {"keep_auto", 100u}, {"enabled", false}, {"auto_enabled", true}}));
            job = ipc.enqueue(request);
            require(ipc.enqueue(request) == job && ipc.send({{"command", "status"}}).at("fixture") == true, "宿主任务去重或快速 IPC 失败。");
            record = required_string(ipc.finish(job), "record_id");
        }
        const TestIpc restarted;
        restarted.ready();
        require(restarted.finish(job).at("record_id") == record, "跨工作进程退出后无法恢复原任务。");
        require(restarted.finish(restarted.enqueue(request)).at("record_id") == record, "原任务被重新执行。");
        restarted.finish(restarted.enqueue({{"command", "saves.records.verify"}, {"game_id", pbz_id}, {"profile", "76561198000000001"},
            {"record_id", record}}));
        std::cout << "Host supervisor, private IPC, restart, job replay and archive verification passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << failure_message(error) << '\n';
        return 1;
    }
}
