#pragma once
#include "common/types.h"
namespace gauge {
Target find_target(const Config& config, const Target& previous);
bool target_alive(const Target& target);
std::vector<Target> enumerate_targets();
bool target_listed(const Target& target, const Config& config);
bool recognized_game(const Target& target, const Config& config);
}
