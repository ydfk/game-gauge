#pragma once
#include "common/config.h"
#include <mutex>
#include <thread>
#include <atomic>

namespace gauge {
// 只读取 OBS 状态，不改变用户的录制操作。
class ObsMonitor {
public:
    ObsMonitor();
    ~ObsMonitor();
    std::string state() const;
    void configure(bool enabled);
private:
    void run(std::stop_token stop);
    void publish(std::string state);
    mutable std::mutex mutex_;
    std::string state_{"disconnected"};
    std::atomic<bool> enabled_{};
    std::jthread worker_;
};
}
