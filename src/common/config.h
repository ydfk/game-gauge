#pragma once
#include "types.h"
#include <filesystem>
#include <nlohmann/json.hpp>
namespace gauge {
using Json = nlohmann::json;
Config config_from_json(const Json& json);
Json config_json(const Config& config);
Config load_config(const std::filesystem::path& path, std::string& warning);
void save_config(const std::filesystem::path& path, const Config& config);
Json snapshot_json(const Snapshot& snapshot);
}

