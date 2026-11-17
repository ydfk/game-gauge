#pragma once
#include "common/config.h"
#include <windows.h>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <thread>
namespace gauge {
class Updater {
public:
    struct Operations {
        std::function<Json(std::stop_token)> check;
        std::function<void(const Json&, std::stop_token)> download;
        std::function<void(const Json&)> verify;
        std::function<HANDLE(const Json&)> launch;
    };

    Updater();
    explicit Updater(Operations operations);
    ~Updater();
    Json status() const;
    void configure(bool automatic_check, bool automatic_update);
    void check();
    void download();
    void install();
    void update();
    void idle(bool game_running);
private:
    enum class Request { none, check, download, install, update };
    enum class Step { none, check, download, install };

    void request(Request request);
    Step next_step();
    void install_ready(const Json& release, std::stop_token stop);
    void run(std::stop_token stop);
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    Operations operations_;
    Json status_;
    Request request_{};
    bool automatic_check_{}, automatic_update_{}, game_running_{true};
    uint64_t next_check_{};
    HANDLE installer_{};
    std::string deferred_version_;
    std::jthread worker_;
};
}
