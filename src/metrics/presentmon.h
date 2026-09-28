#pragma once
#include "common/frame_statistics.h"
#include <windows.h>
#include <memory>
namespace gauge {
class PresentMonProvider {
public:
    PresentMonProvider();
    ~PresentMonProvider();
    void poll(const Target& target, Snapshot& snapshot, FrameStatistics& statistics);
    void reset();
    const std::string& status() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}

