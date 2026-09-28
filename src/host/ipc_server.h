#pragma once
#include "common/config.h"
#include <functional>
#include <thread>
#include <windows.h>
namespace gauge {
class IpcServer {
public:
    explicit IpcServer(std::function<Json(const Json&)> handler);
    ~IpcServer();
private:
    void run(std::stop_token stop);
    std::function<Json(const Json&)> handler_;
    HANDLE stopped_{};
    std::jthread worker_;
};
Json ipc_request(const Json& request);
}

