#pragma once
#include "common/metric_style.h"
#include "common/ui_theme.h"

namespace gauge::theme {
inline D2D1::ColorF reading_color(MetricTone tone) {
    switch (tone) {
    case MetricTone::good: return mint;
    case MetricTone::cool: return blue;
    case MetricTone::watch: return amber;
    case MetricTone::high: return peach;
    case MetricTone::critical: return red;
    default: return white;
    }
}
}
