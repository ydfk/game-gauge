#pragma once
#include "common/frame_statistics.h"
#include <windows.h>
#include <memory>
namespace gauge {
struct PresentMonCapability {
    std::string metric;
    std::string device;
    std::string device_luid;
    std::string availability;
};
class PresentMonProvider {
public:
    PresentMonProvider();
    ~PresentMonProvider();
    void poll(const Target& target, Snapshot& snapshot, FrameStatistics& statistics, bool collect_frames = true);
    void reset();
    const std::string& status() const;
    uint64_t raw_frame_count() const;
    uint64_t accepted_frame_count() const;
    std::vector<PresentMonCapability> capabilities();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
