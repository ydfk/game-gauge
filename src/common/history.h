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
private:
    struct Entry { Target target; Json data; uint64_t last_tick{}, saved_at{}; double fps_sum{}, fps_seconds{}; bool was_active{}; };
    void save(Entry& entry, uint64_t wall_ms, const char* status);
    std::filesystem::path directory_;
    std::map<uint32_t, Entry> sessions_;
};
Json read_history(const std::filesystem::path& directory);
uint64_t wall_time_ms();
}
