#include "host/ipc_server.h"
#include "common/platform.h"
#include <atomic>
#include <iostream>
#include <map>
#include <memory>
#include <vector>

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
}
int main() {
    try {
        const auto name = L"\\\\.\\pipe\\GameGauge-IpcTests-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
        std::map<int, int> visits;
        const auto handler = [&](const gauge::Json& request) {
            const int id = request.at("id").get<int>();
            const int count = ++visits[id];
            if (request.value("slow", false)) Sleep(2200);
            return gauge::Json{{"id", id}, {"visits", count}, {"payload", request.value("large", false) ? std::string(240000, 'x') : "ok"}};
        };
        {
            gauge::IpcServer server(handler, name);
            for (int id = 0; id < 100; ++id) {
                const auto response = gauge::ipc_request({{"id", id}}, name);
                require(response["id"] == id && response["visits"] == 1, "sequential commands must not be lost or replayed");
            }
            std::atomic<unsigned> failures{};
            std::vector<std::jthread> clients;
            for (int client = 0; client < 4; ++client) clients.emplace_back([&, client] {
                for (int i = 0; i < 80; ++i) try {
                    const int id = 100 + client * 80 + i;
                    const auto response = gauge::ipc_request({{"id", id}}, name);
                    if (response["id"] != id || response["visits"] != 1) ++failures;
                } catch (...) { ++failures; }
            });
            clients.clear();
            require(failures == 0, "concurrent clients must not see pipe gaps or discarded replies");
            for (int id = 500; id < 540; ++id) {
                const auto request = gauge::Json{{"id", id}, {"large", true}}.dump();
                std::vector<char> bytes(262144); DWORD read{};
                require(CallNamedPipeW(name.c_str(), const_cast<char*>(request.data()), static_cast<DWORD>(request.size()), bytes.data(),
                    static_cast<DWORD>(bytes.size()), &read, 2000) != FALSE, "legacy clients must receive complete responses");
                const auto response = gauge::Json::parse(bytes.data(), bytes.data() + read);
                require(response["payload"].get<std::string>().size() == 240000, "large response must not be discarded before client reads it");
            }
            bool timed_out{}; const auto start = GetTickCount64();
            try { gauge::ipc_request({{"id", 600}, {"slow", true}}, name); }
            catch (const gauge::IpcConnectionError&) { timed_out = true; }
            require(timed_out && GetTickCount64() - start < 3000, "client response waits must have a deadline");
            const auto response = gauge::ipc_request({{"id", 601}}, name);
            require(response["id"] == 601, "server must recover after a client timeout");
        }
        require(visits[600] == 1, "timed out commands must never be replayed");
        auto server = std::make_unique<gauge::IpcServer>(handler, name);
        gauge::ipc_request({{"id", 700}}, name);
        require(WaitNamedPipeW(name.c_str(), 1000) != FALSE, "idle listener must become ready");
        gauge::UniqueHandle client(CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr));
        require(static_cast<bool>(client), "idle client must connect");
        const auto stop = GetTickCount64(); server.reset();
        require(GetTickCount64() - stop < 1000, "shutdown must cancel an idle peer without blocking");
        std::cout << "IPC sequential/concurrent, legacy large replies, timeout/no replay and shutdown contracts passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
