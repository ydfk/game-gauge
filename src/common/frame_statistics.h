#pragma once
#include "types.h"
#include <deque>
#include <unordered_map>
namespace gauge {
struct FrameSample { uint64_t timestamp_ms{}; uint64_t chain{}; double milliseconds{}; };
class FrameStatistics {
public:
    void add(uint64_t timestamp_ms, uint64_t chain, double milliseconds);
    void reset();
    void publish(Snapshot& snapshot, uint64_t now);
private:
    std::deque<FrameSample> frames_;
    uint64_t selected_chain_{};
};
double slow_tail_fps(const std::vector<double>& frames, double fraction);
}

