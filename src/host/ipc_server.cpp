#include "ipc_server.h"
#include "common/platform.h"
#include <sddl.h>
#include <array>

namespace gauge {
namespace {
bool finish_io(HANDLE pipe, OVERLAPPED& operation, HANDLE stopped, DWORD& bytes, DWORD timeout = 2000) {
    HANDLE events[]{operation.hEvent, stopped};
    const auto result = WaitForMultipleObjects(2, events, FALSE, timeout);
    if (result != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &operation);
        WaitForSingleObject(operation.hEvent, INFINITE);
        return false;
    }
    return GetOverlappedResult(pipe, &operation, &bytes, FALSE) != FALSE;
}
}
IpcServer::IpcServer(std::function<Json(const Json&)> handler) : handler_(std::move(handler)), stopped_(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
    worker_([this](std::stop_token stop) { run(stop); }) {}
IpcServer::~IpcServer() { worker_.request_stop(); SetEvent(stopped_); worker_.join(); CloseHandle(stopped_); }
void IpcServer::run(std::stop_token stop) {
    auto token = user_token(); token.erase(token.find_last_of(L'-'));
    const auto descriptor = L"D:P(A;;GA;;;" + token + L")";
    PSECURITY_DESCRIPTOR security{};
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(descriptor.c_str(), SDDL_REVISION_1, &security, nullptr)) return;
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), security, FALSE};
    while (!stop.stop_requested()) {
        UniqueHandle pipe(CreateNamedPipeW(pipe_name().c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 262144, 32768, 2000, &attributes));
        if (!pipe) break;
        UniqueHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        OVERLAPPED operation{}; operation.hEvent = event.value;
        DWORD bytes{};
        BOOL connected = ConnectNamedPipe(pipe.value, &operation);
        if (!connected) {
            auto error = GetLastError();
            if (error != ERROR_PIPE_CONNECTED && (error != ERROR_IO_PENDING || !finish_io(pipe.value, operation, stopped_, bytes, INFINITE))) continue;
        }
        std::array<char, 32768> buffer{};
        ResetEvent(event.value); operation = {}; operation.hEvent = event.value;
        bool read = ReadFile(pipe.value, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes, &operation) != FALSE;
        if (!read && GetLastError() == ERROR_IO_PENDING) read = finish_io(pipe.value, operation, stopped_, bytes);
        if (read && bytes > 0) {
            Json response;
            try { response = handler_(Json::parse(buffer.data(), buffer.data() + bytes)); }
            catch (const std::exception& error) { response = {{"ok", false}, {"error", error.what()}}; }
            auto text = response.dump();
            if (text.size() > 262144) text = R"({"ok":false,"error":"响应超过大小限制"})";
            ResetEvent(event.value); operation = {}; operation.hEvent = event.value;
            if (!WriteFile(pipe.value, text.data(), static_cast<DWORD>(text.size()), &bytes, &operation) && GetLastError() == ERROR_IO_PENDING)
                finish_io(pipe.value, operation, stopped_, bytes);
        }
        DisconnectNamedPipe(pipe.value);
    }
    LocalFree(security);
}
Json ipc_request(const Json& request) {
    const auto text = request.dump();
    std::vector<char> response(262144);
    DWORD bytes{};
    if (!CallNamedPipeW(pipe_name().c_str(), const_cast<char*>(text.data()), static_cast<DWORD>(text.size()), response.data(),
        static_cast<DWORD>(response.size()), &bytes, 1000)) throw std::runtime_error(error_text(GetLastError()));
    return Json::parse(response.data(), response.data() + bytes);
}
}

