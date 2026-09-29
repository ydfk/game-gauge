#pragma once
#include "common/types.h"
namespace gauge {
Target find_target(const Config& config, const Target& previous);
bool target_alive(const Target& target);
std::vector<Target> enumerate_targets();
}
