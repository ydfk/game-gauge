#pragma once
#include <algorithm>
#include <cstdint>

namespace gauge {
class FrameStreamHealth {
public:
    bool reconnect_due(uint64_t now, bool foreground, bool received_frames) {
        if (received_frames) { reset(); last_progress_ = now; return false; }
        if (!foreground || !last_progress_) { last_progress_ = now; return false; }
        const uint64_t timeout = std::min(uint64_t{60000}, uint64_t{10000} << std::min(retries_, 3u));
        if (now - last_progress_ < timeout) return false;
        last_progress_ = now; ++retries_; return true;
    }
    void reset() { last_progress_ = 0; retries_ = 0; }
    unsigned retries() const { return retries_; }
private:
    uint64_t last_progress_{};
    unsigned retries_{};
};
}
