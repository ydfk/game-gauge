#pragma once
#include "common/config.h"
#include <functional>
#include <stdexcept>
#include <thread>
#include <windows.h>
namespace gauge {
class IpcServer {
public:
    explicit IpcServer(std::function<Json(const Json&)> handler, std::wstring pipe = {});
    ~IpcServer();
private:
    void run(std::stop_token stop);
    std::function<Json(const Json&)> handler_;
    std::wstring pipe_;
    HANDLE stopped_{};
    std::jthread worker_;
};
class IpcConnectionError : public std::runtime_error {
public:
    explicit IpcConnectionError(DWORD error);
};
Json ipc_request(const Json& request, const std::wstring& pipe = {});
}
