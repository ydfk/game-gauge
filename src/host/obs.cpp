#include "obs.h"
#include "common/platform.h"
#include <winhttp.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <atomic>
#include <fstream>

namespace gauge {
namespace {
struct Internet {
    HINTERNET value{};
    ~Internet() { if (value) WinHttpCloseHandle(value); }
};
void require(bool ok) { if (!ok) throw std::runtime_error("OBS connection failed"); }
std::string digest(const std::string& text) {
    unsigned char hash[32]{};
    require(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
        reinterpret_cast<PUCHAR>(const_cast<char*>(text.data())), static_cast<ULONG>(text.size()), hash, sizeof(hash)) >= 0);
    DWORD size{};
    require(CryptBinaryToStringA(hash, sizeof(hash), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &size));
    std::string result(size, '\0');
    require(CryptBinaryToStringA(hash, sizeof(hash), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, result.data(), &size));
    result.resize(size); return result;
}
struct Socket {
    std::atomic<HINTERNET> handle;
    std::atomic<ULONGLONG> deadline{GetTickCount64() + 3000};
    std::jthread watchdog;
    Socket(HINTERNET value, std::stop_token stop) : handle(value), watchdog([this, stop](std::stop_token own) {
        while (!own.stop_requested() && !stop.stop_requested() && GetTickCount64() < deadline.load()) Sleep(50);
        close();
    }) { require(value != nullptr); }
    ~Socket() { watchdog.request_stop(); watchdog.join(); close(); }
    void close() { if (auto value = handle.exchange(nullptr)) WinHttpCloseHandle(value); }
    void send(const Json& json) {
        deadline = GetTickCount64() + 3000;
        auto text = json.dump();
        require(WinHttpWebSocketSend(handle.load(), WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
            text.data(), static_cast<DWORD>(text.size())) == NO_ERROR);
    }
    Json receive() {
        std::string result;
        for (;;) {
            char buffer[4096]; DWORD count{}; WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
            require(WinHttpWebSocketReceive(handle.load(), buffer, sizeof(buffer), &count, &type) == NO_ERROR);
            require(type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE);
            result.append(buffer, count); require(result.size() <= 65536);
            if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) return Json::parse(result);
        }
    }
};
void pause(std::stop_token stop, unsigned milliseconds) {
    for (unsigned i = 0; i < milliseconds && !stop.stop_requested(); i += 50) Sleep(50);
}
}
ObsMonitor::ObsMonitor() : worker_([this](std::stop_token stop) { run(stop); }) {}
ObsMonitor::~ObsMonitor() { worker_.request_stop(); worker_.join(); }
std::string ObsMonitor::state() const { std::lock_guard lock(mutex_); return state_; }
void ObsMonitor::publish(std::string state) { std::lock_guard lock(mutex_); state_ = std::move(state); }
void ObsMonitor::run(std::stop_token stop) {
    while (!stop.stop_requested()) {
        try {
            wchar_t roaming[32768]{};
            require(GetEnvironmentVariableW(L"APPDATA", roaming, 32768) > 0);
            std::ifstream file(std::filesystem::path(roaming) / L"obs-studio/plugin_config/obs-websocket/config.json");
            const auto config = Json::parse(file);
            if (!config.value("server_enabled", false)) {
                publish("disabled"); pause(stop, 3000); continue;
            }
            const auto port = config.value("server_port", 4455); require(port > 0 && port <= 65535);
            Internet session{WinHttpOpen(L"GameGauge OBS", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0)};
            require(session.value != nullptr); WinHttpSetTimeouts(session.value, 1000, 1000, 2000, 2000);
            Internet connection{WinHttpConnect(session.value, L"127.0.0.1", static_cast<INTERNET_PORT>(port), 0)};
            require(connection.value != nullptr);
            Internet request{WinHttpOpenRequest(connection.value, L"GET", L"/", nullptr, nullptr, nullptr, 0)};
            require(request.value && WinHttpSetOption(request.value, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0));
            require(WinHttpSendRequest(request.value, nullptr, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(request.value, nullptr));
            DWORD status{}, size = sizeof(status);
            require(WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &size, nullptr) && status == 101);
            Socket socket(WinHttpWebSocketCompleteUpgrade(request.value, 0), stop);
            const auto hello = socket.receive(); require(hello.at("op") == 0);
            Json identify{{"rpcVersion", 1}, {"eventSubscriptions", 0}};
            if (hello.at("d").contains("authentication")) {
                const auto& auth = hello.at("d").at("authentication");
                // 凭据仅在本机内存中使用，不写入日志、IPC 或应用配置。
                identify["authentication"] = digest(digest(config.at("server_password").get<std::string>() + auth.at("salt").get<std::string>()) + auth.at("challenge").get<std::string>());
            }
            socket.send({{"op", 1}, {"d", identify}});
            require(socket.receive().at("op") == 2);
            while (!stop.stop_requested()) {
                socket.send({{"op", 6}, {"d", {{"requestType", "GetRecordStatus"}, {"requestId", "gamegauge-record"}}}});
                const auto reply = socket.receive();
                require(reply.at("op") == 7 && reply.at("d").at("requestId") == "gamegauge-record" && reply.at("d").at("requestStatus").at("result") == true);
                const auto& data = reply.at("d").at("responseData");
                publish(!data.at("outputActive").get<bool>() ? "idle" : data.at("outputPaused").get<bool>() ? "paused" : "recording");
                pause(stop, 750);
            }
        } catch (...) { publish("disconnected"); }
        pause(stop, 2000);
    }
}
}
