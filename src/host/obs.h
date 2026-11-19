#pragma once
#include "common/config.h"
#include <windows.h>
#include <mutex>
#include <thread>
#include <atomic>

namespace gauge {
HWND find_obs_main_window();

class ObsMonitor {
public:
    ObsMonitor();
    ~ObsMonitor();
    std::string state() const;
    ObsResumeState resume_state() const;
    void resume_recording();
    void configure(bool enabled);
private:
    void run(std::stop_token stop);
    void publish(std::string state);
    mutable std::mutex mutex_;
    std::string state_{"disconnected"};
    ObsResumeState resume_state_{};
    std::atomic<bool> enabled_{};
    std::jthread worker_;
};
}
