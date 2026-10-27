#pragma once
#include "config.h"
namespace gauge {
Json inspect_components();
Json component_readings(Json components, const Snapshot& snapshot);
bool component_repair_available();
}
