#pragma once
#include <algorithm>
#include <cstdint>

namespace gauge {
// 在原位置周围往返移动物理像素，边缘向内移动，不裁切监控条。
inline int oled_shift_axis(int origin, int minimum, int maximum, uint64_t step) {
    maximum = std::max(minimum, maximum);
    origin = std::clamp(origin, minimum, maximum);
    const int low = std::max(minimum, origin - 4), high = std::min(maximum, origin + 4);
    const int distance = high - low;
    if (!distance) return origin;
    const auto phase = static_cast<int>((step + origin - low) % (2 * distance));
    return low + (phase <= distance ? phase : 2 * distance - phase);
}
}