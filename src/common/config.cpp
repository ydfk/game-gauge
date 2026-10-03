#include "config.h"
#include "platform.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <set>
#include <stdexcept>

namespace gauge {
namespace {
const std::set<std::string> metric_ids{"fps", "frametime", "low1", "low01", "cpu_load", "cpu_clock", "cpu_temperature",
    "gpu_load", "gpu_temperature", "gpu_power", "gpu_clock", "gpu_fan", "vram", "memory_load", "memory_used", "process_cpu", "process_memory", "disk_temperature", "session"};
Json metric_json(const Metric& metric) {
    const char* states[]{"valid", "unsupported", "permission", "waiting", "stale", "error"};
    return {{"value", metric.value ? Json(*metric.value) : Json(nullptr)}, {"state", states[static_cast<int>(metric.state)]},
        {"source", metric.source}, {"reason", metric.reason}};
}
}
Config config_from_json(const Json& json) {
    if (!json.is_object() || json.value("version", 1) != 1) throw std::runtime_error("不支持的配置版本");
    Config config;
    config.show_obs = json.value("show_obs", false);
    config.check_updates = json.value("check_updates", true);
    config.auto_update = json.value("auto_update", false);
    config.enabled = json.value("enabled", true);
    config.paused = json.value("paused", false);
    config.exclude_capture = json.value("exclude_capture", true);
    config.oled_protection = json.value("oled_protection", false);
    config.graph = json.value("graph", false);
    config.preview = json.value("preview", false);
    config.hide_on_blur = json.value("hide_on_blur", false);
    config.auto_target = json.value("auto_target", true);
    config.target_pid = json.value("target_pid", 0u);
    config.refresh_ms = std::clamp(json.value("refresh_ms", 250u), 100u, 2000u);
    config.font_size = json.value("font_size", 14.0);
    config.opacity = json.value("opacity", 0.86);
    if (!std::isfinite(config.font_size) || !std::isfinite(config.opacity)) throw std::runtime_error("配置中有无效数值");
    config.font_size = std::clamp(config.font_size, 10.0, 32.0);
    config.opacity = std::clamp(config.opacity, 0.0, 1.0);
    config.anchor = std::clamp(json.value("anchor", 0), 0, 3);
    config.margin_x = std::clamp(json.value("margin_x", 12), 0, 2000);
    config.margin_y = std::clamp(json.value("margin_y", 0), 0, 2000);
    config.gpu_id = json.value("gpu_id", std::string{});
    if (json.contains("metrics")) {
        config.metrics.clear();
        std::set<std::string> selected;
        for (const auto& item : json.at("metrics")) {
            auto id = item.get<std::string>();
            if (metric_ids.contains(id) && selected.insert(id).second) config.metrics.push_back(id);
        }
        if (config.metrics.empty()) config.metrics.push_back("fps");
    }
    config.ignored_processes = json.value("ignored_processes", std::vector<std::string>{});
    const auto group = [](const std::string& id) {
        if (id.starts_with("cpu_") || id == "process_cpu") return 1;
        if (id.starts_with("gpu_") || id == "vram") return 2;
        if (id.starts_with("memory_") || id == "process_memory") return 3;
        if (id == "disk_temperature") return 4;
        if (id == "session") return 5;
        return 0;
    };
    std::stable_sort(config.metrics.begin(), config.metrics.end(), [&](const auto& a, const auto& b) { return group(a) < group(b); });
    config.known_games = json.value("known_games", std::vector<std::string>{});
    if (config.ignored_processes.size() > 128) throw std::runtime_error("忽略列表过长");
    if (config.known_games.size() > 512) throw std::runtime_error("游戏列表过长");
    return config;
}
Json config_json(const Config& c) {
    return {{"version", 1}, {"show_obs", c.show_obs}, {"check_updates", c.check_updates}, {"auto_update", c.auto_update}, {"enabled", c.enabled}, {"paused", c.paused}, {"exclude_capture", c.exclude_capture},
        {"oled_protection", c.oled_protection}, {"graph", c.graph}, {"preview", c.preview}, {"hide_on_blur", c.hide_on_blur}, {"auto_target", c.auto_target},
        {"target_pid", c.target_pid}, {"refresh_ms", c.refresh_ms}, {"font_size", c.font_size}, {"opacity", c.opacity},
        {"anchor", c.anchor}, {"margin_x", c.margin_x}, {"margin_y", c.margin_y}, {"gpu_id", c.gpu_id},
        {"metrics", c.metrics}, {"ignored_processes", c.ignored_processes}, {"known_games", c.known_games}};
}
Config load_config(const std::filesystem::path& path, std::string& warning) {
    if (!std::filesystem::exists(path)) return {};
    try {
        if (std::filesystem::file_size(path) > 65536) throw std::runtime_error("配置超过大小限制");
        std::ifstream input(path);
        return config_from_json(Json::parse(input));
    } catch (const std::exception& error) {
        warning = std::string("配置读取失败，使用默认值：") + error.what();
        return {};
    }
}
void save_config(const std::filesystem::path& path, const Config& config) {
    std::filesystem::create_directories(path.parent_path());
    auto temporary = path; temporary += L".tmp";
    std::ofstream output(temporary, std::ios::trunc | std::ios::binary);
    output << config_json(config).dump(2);
    output.flush();
    if (!output) throw std::runtime_error("无法写入配置");
    output.close();
    if (std::filesystem::exists(path)) {
        auto backup = path; backup += L".bak";
        CopyFileW(path.c_str(), backup.c_str(), FALSE);
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error(error_text(GetLastError()));
}
Json snapshot_json(const Snapshot& s) {
    Json devices = Json::array(), displays = Json::array(), disks = Json::array();
    for (const auto& disk : s.hardware.disks) disks.push_back({{"id", disk.id}, {"name", disk.name}, {"temperature", metric_json(disk.temperature)}});
    for (const auto& gpu : s.hardware.gpus) devices.push_back({{"id", gpu.id}, {"name", gpu.name}, {"vendor", gpu.vendor},
        {"pci", gpu.pci}, {"memory_total_bytes", gpu.dedicated_bytes}, {"load", metric_json(gpu.load)},
        {"temperature", metric_json(gpu.temperature)}, {"power", metric_json(gpu.power)}, {"clock", metric_json(gpu.clock)},
        {"memory_used", metric_json(gpu.memory_used)}, {"memory_total", metric_json(gpu.memory_total)}, {"fan", metric_json(gpu.fan)}});
    for (const auto& d : s.hardware.displays) displays.push_back({{"name", d.name}, {"x", d.x}, {"y", d.y},
        {"width", d.width}, {"height", d.height}, {"dpi", d.dpi}, {"hdr", d.hdr}});
    return {{"obs_state", s.obs_state}, {"timestamp_ms", s.timestamp_ms}, {"target", {{"pid", s.target.pid}, {"name", s.target.name},
        {"path", s.target.path}, {"foreground", s.target.foreground}}}, {"cpu", s.hardware.cpu},
        {"cpu_vendor", s.hardware.cpu_vendor}, {"logical_processors", s.hardware.logical_processors},
        {"gpus", devices}, {"displays", displays}, {"disks", disks}, {"disk_temperature", metric_json(s.disk_temperature)}, {"cpu_load", metric_json(s.cpu_load)}, {"cpu_clock", metric_json(s.cpu_clock)},
        {"cpu_temperature", metric_json(s.cpu_temperature)}, {"memory_load", metric_json(s.memory_load)},
        {"memory_used", metric_json(s.memory_used)}, {"memory_total", metric_json(s.memory_total)},
        {"process_cpu", metric_json(s.process_cpu)}, {"process_memory", metric_json(s.process_memory)},
        {"fps", metric_json(s.fps)}, {"frametime", metric_json(s.frametime)}, {"low1", metric_json(s.low1)},
        {"low01", metric_json(s.low01)}, {"frame_samples", s.frame_samples}, {"session_seconds", s.session_seconds},
        {"recent_frames", s.recent_frames}, {"frame_status", s.frame_status}, {"selected_gpu", s.selected_gpu},
        {"gpu_selection_reason", s.gpu_selection_reason}, {"paused", s.paused}, {"game_confirmed", s.game_confirmed}, {"history_error", s.history_error}};
}
}
