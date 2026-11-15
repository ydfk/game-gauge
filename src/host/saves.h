#pragma once
#include "common/config.h"
#include <windows.h>
#include <condition_variable>
#include <deque>
#include <optional>
#include <thread>
#include <unordered_map>

namespace gauge {
class Saves {
public:
    Saves();
    ~Saves();
    Json request(const Json& request);
    void drain();
    bool restoring();

private:
    struct Job {
        Json input;
        Json status;
    };
    void run(std::stop_token stop);
    void complete(const std::string& id, std::string state, Json result);
    std::mutex mutex_;
    std::condition_variable_any wake_;
    std::unordered_map<std::string, Job> jobs_;
    std::deque<std::string> pending_, finished_;
    std::string state_{"starting"}, error_, hotkey_{"Ctrl+Alt+F8"};
    std::string active_command_;
    bool draining_{};
    std::jthread thread_;
};

std::optional<std::pair<UINT, UINT>> saves_hotkey(std::string_view text);
}
