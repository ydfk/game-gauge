#pragma once
#include "migration.h"

namespace gauge::saves {
using Clock = std::chrono::steady_clock;
struct Schedule {
    bool was_running{};
    std::optional<Clock::time_point> due;
    std::string_view observe(bool running, bool enabled, std::chrono::seconds period, Clock::time_point now);
};
Json record_page(const std::vector<Json>& records, const Json& request);
class Journal {
public:
    explicit Journal(const fs::path& root);
    Json lookup(const Json& request) const;
    Json execute(const Json& request, const std::function<Json()>& action) const;
private:
    fs::path directory_;
};
class Worker {
public:
    explicit Worker(fs::path root);
    const std::string& hotkey() const;
    Json request(const Json& input);
    void tick();
private:
    size_t target_index(const Json& input) const;
    const Settings& target(const Json& input) const;
    Json change_catalog(const std::function<void(Catalog&)>& change);
    Json configure(const Json& input);
    Json games() const;
    Json backup(const Settings& settings, std::string_view trigger);
    Json backup_request(const Json& input);
    Json record_action(std::string_view command, const Json& input);
    Json cloud_action(std::string_view command, const Json& input);
    Json migration_action(std::string_view command, const Json& input);
    fs::path root_;
    Catalog catalog_;
    Cloud cloud_;
    std::map<std::pair<std::string, std::string>, Schedule> schedules_;
    Clock::time_point next_cloud_{Clock::now()};
    std::vector<Json> remote_;
    std::optional<Migration> preview_;
    std::string automatic_status_, cloud_status_;
};
}
