#pragma once
#include "common/types.h"
namespace gauge {
Target find_target(const Config& config, const Target& previous);
std::vector<Target> enumerate_targets();
}

