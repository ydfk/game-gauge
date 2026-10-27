#pragma once
#include "common/components.h"
#include <condition_variable>
#include <mutex>
#include <thread>

namespace gauge {
class ComponentMonitor {
public:
    ComponentMonitor();
    ~ComponentMonitor();
    Json status(const Snapshot& snapshot) const;
    void repair();
private:
    void run(std::stop_token stop);
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    Json components_ = Json::array();
    std::string state_{"idle"}, message_;
    bool requested_{};
    std::jthread worker_;
};
}
