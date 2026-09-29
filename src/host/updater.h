#pragma once
#include "common/config.h"
#include <mutex>
#include <condition_variable>
#include <thread>
namespace gauge {
class Updater {
public:
    Updater();
    ~Updater();
    Json status() const;
    void configure(bool automatic_check, bool automatic_update);
    void check();
    void download();
    void install();
    void idle(bool game_running);
private:
    void run(std::stop_token stop);
    void publish(Json value);
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    Json status_;
    bool check_{}, download_{}, automatic_check_{true}, automatic_update_{}, game_running_{true};
    uint64_t next_check_{};
    void* installer_{};
    std::string deferred_version_;
    std::jthread worker_;
};
}
