#pragma once
#include "common/types.h"
#include <condition_variable>
#include <mutex>
#include <thread>
namespace gauge {
class Sampler {
public:
    explicit Sampler(Config config);
    ~Sampler();
    void configure(Config config);
    void reset_statistics();
    void refresh_hardware();
    Snapshot snapshot() const;
private:
    void run(std::stop_token stop);
    mutable std::mutex mutex_;
    std::condition_variable_any wake_;
    Config config_;
    Snapshot snapshot_;
    bool reset_{}, rediscover_{};
    std::jthread worker_;
};
}

