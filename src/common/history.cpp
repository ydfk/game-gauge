#include "history.h"
#include "platform.h"
#include <chrono>
#include <fstream>
#include <algorithm>

namespace gauge {
uint64_t wall_time_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}
Json read_history(const std::filesystem::path& directory) {
    std::vector<Json> rows;
    if (!std::filesystem::exists(directory)) return Json::array();
    for (const auto& file : std::filesystem::directory_iterator(directory)) {
        if (file.path().extension() != L".json" || !file.is_regular_file() || file.file_size() > 65536) continue;
        try {
            std::ifstream stream(file.path()); auto row = Json::parse(stream);
            if (!row.is_object() || row.value("version", 0) != 1) continue;
            const auto id = row.value("id", std::string{});
            if (id.empty() || id.size() > 64 || id.find_first_not_of("0123456789-") != std::string::npos) continue;
            if (!row.value("started_ms", Json{}).is_number_unsigned()) continue;
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
void SessionHistory::save(Entry& entry, uint64_t wall_ms, const char* status) {
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
        else { if (it->first != s.target.pid) it->second.was_active = false; ++it; }
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
    }
    const double elapsed = e.was_active && e.last_tick && s.timestamp_ms > e.last_tick ? std::min(2.0, (s.timestamp_ms - e.last_tick) / 1000.0) : 0;
    e.last_tick = s.timestamp_ms;
    e.was_active = s.target.foreground && !s.paused;
    if (s.target.foreground && !s.paused) {
        e.data["active_seconds"] = e.data.value("active_seconds", 0.0) + elapsed;
        if (s.fps.state == State::valid && s.fps.value) { e.fps_sum += *s.fps.value * elapsed; e.fps_seconds += elapsed; }
        auto peak = [&](const char* name, const Metric& metric) {
            if (metric.state == State::valid && metric.value &&
                (e.data[name].is_null() || *metric.value > e.data[name].get<double>())) e.data[name] = *metric.value;
        };
        peak("maximum_fps", s.fps); peak("cpu_max_celsius", s.cpu_temperature);
        for (const auto& gpu : s.hardware.gpus) if (gpu.id == s.selected_gpu) peak("gpu_max_celsius", gpu.temperature);
    }
    if (inserted || wall_ms - e.saved_at >= 5000) save(e, wall_ms, "running");
}
void SessionHistory::finish_all(uint64_t wall_ms) {
    for (auto& [pid, entry] : sessions_) save(entry, wall_ms, "monitor_closed");
    sessions_.clear();
}
}
