#pragma once
#include "config.h"
#include <string_view>

namespace gauge {
bool valid_recorded_value(std::string_view metric, double value);
bool valid_frame_interval(double milliseconds);
bool frame_inside_active_period(uint64_t timestamp, uint64_t active_since, double milliseconds, uint64_t frequency);
void filter_history_view(Json& record);
}
