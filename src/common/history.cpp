#include "history.h"
#include "platform.h"
#include "sample_validation.h"
#include "game_identity.h"
#include <chrono>
#include <fstream>
#include <algorithm>
#include <mutex>
#include <set>
#include <ctime>
#include <cmath>

namespace gauge {
namespace {
std::recursive_mutex history_mutex;
std::set<std::filesystem::path> deleted_sessions;
}
bool delete_history(const std::filesystem::path& directory, const std::string& id) {
    if (id.empty() || id.size() > 64 || id.find_first_not_of("0123456789-") != std::string::npos)
        throw std::runtime_error("无效的游戏记录编号");
    std::lock_guard lock(history_mutex);
    const auto path = directory / (wide(id) + L".json");
    const bool removed = std::filesystem::remove(path);
    // 删除当前会话后，本次运行不再写回同一条记录。
    deleted_sessions.insert(path);
    return removed;
}
uint64_t wall_time_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}
std::string history_day(uint64_t milliseconds) {
    const time_t seconds = static_cast<time_t>(milliseconds / 1000); tm value{}; localtime_s(&value, &seconds);
    char text[32]{}; strftime(text, sizeof(text), "%Y-%m-%d", &value); return text;
}
Json read_history_entry(const std::filesystem::path& directory, const std::string& id) {
    if (id.empty() || id.size() > 64 || id.find_first_not_of("0123456789-") != std::string::npos)
        throw std::runtime_error("无效的游戏记录编号");
    std::lock_guard lock(history_mutex);
    const auto path = directory / (wide(id) + L".json");
    if (std::filesystem::file_size(path) > 1048576) throw std::runtime_error("游戏记录超过大小限制");
    std::ifstream stream(path); auto row = Json::parse(stream);
    if (!row.is_object() || row.value("version", 0) != 1 || row.value("id", std::string{}) != id)
        throw std::runtime_error("游戏记录格式无效");
    return row;
}
Json read_history(const std::filesystem::path& directory, bool details) {
    std::lock_guard lock(history_mutex);
    std::vector<Json> rows;
    if (!std::filesystem::exists(directory)) return Json::array();
    for (const auto& file : std::filesystem::directory_iterator(directory)) {
        if (file.path().extension() != L".json" || !file.is_regular_file() || file.file_size() > 1048576) continue;
        try {
            std::ifstream stream(file.path()); auto row = Json::parse(stream);
            if (!row.is_object() || row.value("version", 0) != 1) continue;
            const auto id = row.value("id", std::string{});
            if (id.empty() || id.size() > 64 || id.find_first_not_of("0123456789-") != std::string::npos) continue;
            if (!row.value("started_ms", Json{}).is_number_unsigned()) continue;
            if (!details) { row.erase("series"); row.erase("events"); row.erase("hardware"); }
            rows.push_back(std::move(row));
        } catch (...) { /* 单条损坏不影响其他历史记录。 */ }
    }
    std::sort(rows.begin(), rows.end(), [](const Json& a, const Json& b) { return a.value("started_ms", 0ull) > b.value("started_ms", 0ull); });
    return rows;
}
SessionHistory::SessionHistory(std::filesystem::path directory) : directory_(std::move(directory)) {
    // 上次进程异常退出时，保留已有采样并显式标记中断。
    for (auto row : read_history(directory_)) if (row.value("status", std::string{}) == "running") {
        Entry entry; entry.data = std::move(row);
        save(entry, entry.data.value("updated_ms", 0ull), "interrupted");
    }
}
std::optional<double> SessionHistory::active_seconds(const Target& target) const {
    const auto found = sessions_.find(target.pid);
    if (found == sessions_.end() || found->second.target.started != target.started) return {};
    return found->second.data.value("active_seconds", 0.0);
}
void SessionHistory::save(Entry& entry, uint64_t wall_ms, const char* status) {
    std::lock_guard lock(history_mutex);
    if (deleted_sessions.contains(directory_ / (wide(entry.data.at("id").get<std::string>()) + L".json"))) {
        entry.saved_at = wall_ms; return;
    }
    std::filesystem::create_directories(directory_);
    entry.data["status"] = status; entry.data["updated_ms"] = wall_ms;
    entry.data["duration_seconds"] = (wall_ms - std::min(wall_ms, entry.data.value("started_ms", wall_ms))) / 1000.0;
    if (entry.fps_seconds > 0) entry.data["average_fps"] = entry.fps_sum / entry.fps_seconds;
    const auto path = directory_ / (wide(entry.data.at("id").get<std::string>()) + L".json");
    auto temporary = path; temporary += L".tmp";
    { std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
      output << entry.data.dump(2); output.flush(); if (!output) throw std::runtime_error("无法保存游戏历史"); }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error(error_text(GetLastError()));
    entry.saved_at = wall_ms;
}
void SessionHistory::update(const Snapshot& s, uint64_t wall_ms, const std::function<bool(const Target&)>& alive) {
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (!alive(it->second.target)) { save(it->second, wall_ms, "completed"); it = sessions_.erase(it); }
        else {
            auto& entry = it->second;
            if (it->first != s.target.pid && entry.was_active) {
                entry.was_active = false;
                Json gap = Json::array({(wall_ms - std::min(wall_ms, entry.data.value("started_ms", wall_ms))) / 1000.0});
                for (size_t i = 0; i < 10; ++i) gap.push_back(nullptr);
                entry.data["series"].push_back(std::move(gap));
                if (entry.data["events"].size() < 128) entry.data["events"].push_back({{"at_ms", wall_ms}, {"type", "background"}});
                entry.last_foreground = false;
            }
            ++it;
        }
    }
    if (!s.target.pid || !s.game_confirmed) return;
    auto found = sessions_.find(s.target.pid);
    if (found != sessions_.end() && found->second.target.started != s.target.started) {
        save(found->second, wall_ms, "completed"); sessions_.erase(found);
    }
    auto [it, inserted] = sessions_.try_emplace(s.target.pid);
    auto& e = it->second;
    if (inserted) {
        e.target = s.target;
        e.data = {{"version", 1}, {"id", std::to_string(wall_ms) + "-" + std::to_string(s.target.pid)},
            {"game", s.target.name}, {"path", s.target.path}, {"started_ms", wall_ms}, {"active_seconds", 0.0},
            {"average_fps", nullptr}, {"maximum_fps", nullptr}, {"cpu_max_celsius", nullptr}, {"gpu_max_celsius", nullptr}};
        e.data["sample_policy"] = 1; e.data["filtered_samples"] = Json::object();
        e.data["display_name"] = game_display_name(s.target.path, s.target.name, s.target.window);
        e.data["stats"] = Json::object(); e.data["series"] = Json::array(); e.data["events"] = Json::array();
        e.data["series_metrics"] = {"fps", "cpu_load", "cpu_temperature", "gpu_load", "gpu_temperature", "memory_load", "disk_temperature", "frametime", "gpu_power", "vram"};
        e.data["hardware"] = snapshot_json(s);
        e.last_foreground = s.target.foreground; e.last_paused = s.paused;
    }
    const double elapsed = e.was_active && e.last_tick && s.timestamp_ms > e.last_tick ? std::min(2.0, (s.timestamp_ms - e.last_tick) / 1000.0) : 0;
    e.last_tick = s.timestamp_ms;
    e.was_active = s.target.foreground && !s.paused;
    const bool state_changed = s.target.foreground != e.last_foreground || s.paused != e.last_paused;
    if (state_changed) {
        auto& events = e.data["events"];
        if (events.size() < 128) events.push_back({{"at_ms", wall_ms}, {"type", s.paused ? "paused" : !s.target.foreground ? "background" : "resumed"}});
        e.last_foreground = s.target.foreground; e.last_paused = s.paused;
    }
    std::vector<Metric> metrics{s.fps, s.cpu_load, s.cpu_temperature, missing(State::waiting, "等待显卡"),
        missing(State::waiting, "等待显卡"), s.memory_load, s.disk_temperature, s.frametime,
        missing(State::waiting, "等待显卡"), missing(State::waiting, "等待显卡")};
    for (const auto& gpu : s.hardware.gpus) if (gpu.id == s.selected_gpu) {
        metrics[3] = gpu.load; metrics[4] = gpu.temperature; metrics[8] = gpu.power; metrics[9] = gpu.memory_used;
        if (metrics[9].value) *metrics[9].value /= 1073741824.0;
    }
    const auto& ids = e.data["series_metrics"];
    Json point = Json::array({(wall_ms - std::min(wall_ms, e.data.value("started_ms", wall_ms))) / 1000.0});
    for (size_t index = 0; index < metrics.size(); ++index) {
        const auto& metric = metrics[index];
        const auto id = ids[index].get<std::string>();
        const bool readable = e.was_active && metric.state == State::valid && metric.value;
        const bool valid = readable && valid_recorded_value(id, *metric.value);
        if (readable && !valid) e.data["filtered_samples"][id] = e.data["filtered_samples"].value(id, 0ull) + 1;
        point.push_back(valid ? Json(*metric.value) : Json(nullptr));
        if (!valid) continue;
        auto& stat = e.data["stats"][id];
        if (stat.is_null()) stat = {{"minimum", *metric.value}, {"maximum", *metric.value}, {"sum", 0.0}, {"seconds", 0.0}, {"average", nullptr}};
        stat["minimum"] = std::min(stat["minimum"].get<double>(), *metric.value);
        stat["maximum"] = std::max(stat["maximum"].get<double>(), *metric.value);
        stat["sum"] = stat["sum"].get<double>() + *metric.value * elapsed;
        stat["seconds"] = stat["seconds"].get<double>() + elapsed;
        if (stat["seconds"].get<double>() > 0) stat["average"] = stat["sum"].get<double>() / stat["seconds"].get<double>();
    }
    // 长会话逐级降低曲线分辨率，完整统计仍由所有有效采样累计。
    if (!e.sampled_at || wall_ms - e.sampled_at >= e.sample_interval || !e.was_active || state_changed) {
        auto& series = e.data["series"];
        const bool last_valid = !series.empty() && std::any_of(series.back().begin() + 1, series.back().end(), [](const Json& value) { return value.is_number(); });
        if (e.was_active || series.empty() || last_valid) series.push_back(std::move(point));
        e.sampled_at = wall_ms;
        if (series.size() > 1024) {
            Json reduced = Json::array();
            for (size_t i = 0; i < series.size(); i += 2) {
                auto sample = series[i];
                // 合并区间含失焦或暂停时保留缺口，不能把两侧读数连成连续采样。
                if (i + 1 < series.size()) for (size_t j = 1; j < sample.size(); ++j)
                    if (series[i + 1][j].is_null()) sample[j] = nullptr;
                reduced.push_back(std::move(sample));
            }
            series = std::move(reduced); e.sample_interval *= 2;
        }
        e.data["series_interval_seconds"] = e.sample_interval / 1000.0;
    }
    if (e.was_active && s.low1.state == State::valid && s.low1.value && valid_recorded_value("low1", *s.low1.value)) e.data["latest_low1"] = *s.low1.value;
    if (e.was_active && s.low01.state == State::valid && s.low01.value && valid_recorded_value("low01", *s.low01.value)) e.data["latest_low01"] = *s.low01.value;
    if (s.target.foreground && !s.paused) {
        e.data["active_seconds"] = e.data.value("active_seconds", 0.0) + elapsed;
        if (s.fps.state == State::valid && s.fps.value && valid_recorded_value("fps", *s.fps.value)) { e.fps_sum += *s.fps.value * elapsed; e.fps_seconds += elapsed; }
        auto peak = [&](const char* name, const char* id, const Metric& metric) {
            if (metric.state == State::valid && metric.value && valid_recorded_value(id, *metric.value) &&
                (e.data[name].is_null() || *metric.value > e.data[name].get<double>())) e.data[name] = *metric.value;
        };
        peak("maximum_fps", "fps", s.fps); peak("cpu_max_celsius", "cpu_temperature", s.cpu_temperature);
        for (const auto& gpu : s.hardware.gpus) if (gpu.id == s.selected_gpu) peak("gpu_max_celsius", "gpu_temperature", gpu.temperature);
    }
    if (inserted || wall_ms - e.saved_at >= 5000) save(e, wall_ms, "running");
}
void SessionHistory::finish_all(uint64_t wall_ms) {
    for (auto& [pid, entry] : sessions_) save(entry, wall_ms, "monitor_closed");
    sessions_.clear();
}
}
