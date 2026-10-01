#pragma once
#include "config.h"
#include <optional>
#include <string_view>

namespace gauge {
enum class MetricTone { neutral, muted, good, cool, watch, high, critical };
uint32_t tone_rgb(MetricTone tone);
MetricTone metric_tone(std::string_view id, std::optional<double> value);
MetricTone metric_tone(std::string_view id, const Snapshot& snapshot);
MetricTone metric_tone(std::string_view id, const Json& snapshot);
MetricTone obs_tone(std::string_view state);
std::wstring obs_label(std::string_view state);
}
