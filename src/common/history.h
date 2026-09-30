#pragma once
#include "config.h"
#include <functional>
#include <map>

namespace gauge {
class SessionHistory {
public:
    explicit SessionHistory(std::filesystem::path directory);
    void update(const Snapshot& snapshot, uint64_t wall_ms, const std::function<bool(const Target&)>& alive);
    void finish_all(uint64_t wall_ms);
    std::optional<double> active_seconds(const Target& target) const;
private:
    struct Entry {
        Target target; Json data;
        uint64_t last_tick{}, saved_at{}, sampled_at{}, sample_interval{1000};
        double fps_sum{}, fps_seconds{};
        bool was_active{}, last_foreground{}, last_paused{};
    };
    void save(Entry& entry, uint64_t wall_ms, const char* status);
    std::filesystem::path directory_;
    std::map<uint32_t, Entry> sessions_;
};
Json read_history(const std::filesystem::path& directory, bool details = true);
Json read_history_entry(const std::filesystem::path& directory, const std::string& id);
std::string history_day(uint64_t milliseconds);
bool delete_history(const std::filesystem::path& directory, const std::string& id);
uint64_t wall_time_ms();
}
