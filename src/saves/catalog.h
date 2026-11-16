#pragma once
#include "store.h"
#include <optional>
#include <set>

namespace gauge::saves {
struct SteamGame {
    uint32_t app_id{};
    std::string title, executable;
};
struct Settings {
    fs::path save_dir, backup_dir;
    uint64_t interval_minutes{10}, keep_auto{100};
    bool auto_enabled{true}, dark{};
    std::string hotkey{"Ctrl+Alt+F8"};
    std::optional<SteamGame> custom;
    std::pair<std::string, std::string> identity() const;
    std::string title() const;
    Json json() const;
    static Settings parse(const Json& json);
};
struct Catalog {
    std::vector<Settings> games;
    size_t selected{};
    bool enabled{};
    static Catalog load(const fs::path& root);
    void save(const fs::path& root) const;
    void add(uint32_t id, const std::string& exe, const fs::path& save_dir);
    void check_overlap(const fs::path& save, const fs::path& backup, std::optional<size_t> exclude = {}) const;
};
std::vector<fs::path> discover_slots();
fs::path resolve_save(const Settings& settings);
fs::path backup_location(const fs::path& save, const fs::path& requested);
std::set<std::string> processes();
bool running(const Settings& settings, const std::set<std::string>& processes);
}
