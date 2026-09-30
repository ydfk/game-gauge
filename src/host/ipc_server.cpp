#include "ipc_server.h"
#include "common/platform.h"
#include <sddl.h>
#include <array>
#include <algorithm>

namespace gauge {
namespace {
bool finish_io(HANDLE pipe, OVERLAPPED& operation, HANDLE stopped, DWORD& bytes, DWORD timeout = 2000) {
    HANDLE events[]{operation.hEvent, stopped};
    const auto result = stopped ? WaitForMultipleObjects(2, events, FALSE, timeout) : WaitForSingleObject(operation.hEvent, timeout);
    if (result != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &operation);
        WaitForSingleObject(operation.hEvent, INFINITE);
        SetLastError(result == WAIT_TIMEOUT ? ERROR_TIMEOUT : ERROR_OPERATION_ABORTED);
        return false;
    }
    return GetOverlappedResult(pipe, &operation, &bytes, FALSE) != FALSE;
}
}
IpcServer::IpcServer(std::function<Json(const Json&)> handler, std::wstring pipe) : handler_(std::move(handler)), pipe_(pipe.empty() ? pipe_name() : std::move(pipe)), stopped_(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
    worker_([this](std::stop_token stop) { run(stop); }) {}
IpcServer::~IpcServer() { worker_.request_stop(); SetEvent(stopped_); worker_.join(); CloseHandle(stopped_); }
void IpcServer::run(std::stop_token stop) {
    auto token = user_token(); token.erase(token.find_last_of(L'-'));
    const auto descriptor = L"D:P(A;;GA;;;" + token + L")";
    PSECURITY_DESCRIPTOR security{};
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(descriptor.c_str(), SDDL_REVISION_1, &security, nullptr)) return;
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), security, FALSE};
    // 保持管道实例存在，避免连续请求在销毁和重建之间遇到“找不到文件”。
    UniqueHandle pipe(CreateNamedPipeW(pipe_.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 262144, 32768, 2000, &attributes));
    LocalFree(security);
    UniqueHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!pipe || !event) return;
    while (!stop.stop_requested()) {
        ResetEvent(event.value);
        OVERLAPPED operation{}; operation.hEvent = event.value;
        DWORD bytes{};
        BOOL connected = ConnectNamedPipe(pipe.value, &operation);
        if (!connected) {
            auto error = GetLastError();
            if (error != ERROR_PIPE_CONNECTED && (error != ERROR_IO_PENDING || !finish_io(pipe.value, operation, stopped_, bytes, INFINITE))) {
                DisconnectNamedPipe(pipe.value); continue;
            }
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
            bool written = WriteFile(pipe.value, text.data(), static_cast<DWORD>(text.size()), &bytes, &operation) != FALSE;
            if (!written && GetLastError() == ERROR_IO_PENDING) written = finish_io(pipe.value, operation, stopped_, bytes);
            if (written) {
                // 立即断开会丢弃尚未被客户端读取的响应；等待客户端读完并关闭，且可取消。
                char closed{};
                ResetEvent(event.value); operation = {}; operation.hEvent = event.value;
                if (!ReadFile(pipe.value, &closed, 1, &bytes, &operation) && GetLastError() == ERROR_IO_PENDING)
                    finish_io(pipe.value, operation, stopped_, bytes);
            }
        }
        DisconnectNamedPipe(pipe.value);
    }
}
IpcConnectionError::IpcConnectionError(DWORD error) : std::runtime_error("游戏仪表主程序连接失败：" + error_text(error)) {}
Json ipc_request(const Json& request, const std::wstring& name) {
    const auto text = request.dump();
    if (text.size() > 32768) throw std::runtime_error("请求超过大小限制");
    const auto path = name.empty() ? pipe_name() : name;
    const auto deadline = GetTickCount64() + 2000;
    UniqueHandle pipe;
    for (;;) {
        pipe.reset(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
        if (pipe) break;
        const DWORD error = GetLastError();
        if ((error != ERROR_PIPE_BUSY && error != ERROR_FILE_NOT_FOUND) || GetTickCount64() >= deadline) throw IpcConnectionError(error);
        // 仅在连接建立前重试，已发送的配置或删除命令不能重复执行。
        if (error == ERROR_PIPE_BUSY) WaitNamedPipeW(path.c_str(), 20); else Sleep(10);
    }
    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(pipe.value, &mode, nullptr, nullptr)) throw IpcConnectionError(GetLastError());
    UniqueHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) throw IpcConnectionError(GetLastError());
    const auto remaining = [&]() { const auto now = GetTickCount64(); return static_cast<DWORD>(now < deadline ? deadline - now : 0); };
    OVERLAPPED operation{}; operation.hEvent = event.value;
    DWORD bytes{};
    bool written = WriteFile(pipe.value, text.data(), static_cast<DWORD>(text.size()), &bytes, &operation) != FALSE;
    if (written) written = GetOverlappedResult(pipe.value, &operation, &bytes, FALSE) != FALSE;
    else if (GetLastError() == ERROR_IO_PENDING) written = finish_io(pipe.value, operation, nullptr, bytes, remaining());
    if (!written) throw IpcConnectionError(GetLastError());
    if (bytes != text.size()) throw std::runtime_error("请求未完整写入");
    std::vector<char> response(262144);
    ResetEvent(event.value); operation = {}; operation.hEvent = event.value;
    bool read = ReadFile(pipe.value, response.data(), static_cast<DWORD>(response.size()), &bytes, &operation) != FALSE;
    if (read) read = GetOverlappedResult(pipe.value, &operation, &bytes, FALSE) != FALSE;
    else if (GetLastError() == ERROR_IO_PENDING) read = finish_io(pipe.value, operation, nullptr, bytes, remaining());
    if (!read) throw IpcConnectionError(GetLastError());
    if (!bytes) throw std::runtime_error("主程序返回空响应");
    return Json::parse(response.data(), response.data() + bytes);
}
}
