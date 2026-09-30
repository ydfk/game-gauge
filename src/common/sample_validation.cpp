#include "sample_validation.h"
#include <cmath>

namespace gauge {
bool valid_recorded_value(std::string_view metric, double value) {
    if (!std::isfinite(value)) return false;
    if (metric == "fps" || metric == "low1" || metric == "low01") return value > 0 && value <= 10000;
    if (metric == "frametime") return value >= .1 && value <= 60000;
    if (metric == "cpu_load" || metric == "gpu_load" || metric == "memory_load") return value >= 0 && value <= 100;
    if (metric == "cpu_temperature" || metric == "gpu_temperature") return value >= -40 && value <= 150;
    if (metric == "disk_temperature") return value >= -40 && value <= 125;
    if (metric == "gpu_power") return value >= 0 && value <= 3000;
    if (metric == "vram") return value >= 0 && value <= 1024;
    return true;
}
bool valid_frame_interval(double milliseconds) { return valid_recorded_value("frametime", milliseconds); }
bool frame_inside_active_period(uint64_t timestamp, uint64_t active_since, double milliseconds, uint64_t frequency) {
    if (!frequency || !active_since || timestamp < active_since || !valid_frame_interval(milliseconds)) return false;
    // 恢复后的首个间隔可能跨越整个后台时间，不计入游戏内的帧率和 Low。
    return milliseconds <= static_cast<double>(timestamp - active_since) * 1000.0 / frequency;
}
void filter_history_view(Json& record) {
    bool hidden = false;
    const auto filter = [&](Json& value, std::string_view id) {
        if (value.is_number() && !valid_recorded_value(id, value.get<double>())) { value = nullptr; hidden = true; }
    };
    if (record.contains("stats") && record["stats"].is_object()) {
        for (auto& [id, stat] : record["stats"].items()) if (stat.is_object())
            for (const auto* field : {"minimum", "maximum", "average"}) if (stat.contains(field)) filter(stat[field], id);
    }
    for (const auto& [field, metric] : {std::pair{"average_fps", "fps"}, {"maximum_fps", "fps"},
        {"cpu_max_celsius", "cpu_temperature"}, {"gpu_max_celsius", "gpu_temperature"}, {"latest_low1", "low1"}, {"latest_low01", "low01"}})
        if (record.contains(field)) filter(record[field], metric);
    const auto ids = record.value("series_metrics", Json::array());
    if (ids.is_array() && record.contains("series") && record["series"].is_array()) {
        for (auto& point : record["series"]) if (point.is_array())
            for (size_t i = 0; i < ids.size() && i + 1 < point.size(); ++i)
                if (ids[i].is_string()) filter(point[i + 1], ids[i].get<std::string>());
    }
    // 旧记录缺少全部原始采样，隐藏无效极值，不从降采样曲线补造最低值或平均值。
    if (hidden && !record.contains("sample_policy")) record["legacy_values_hidden"] = true;
}
}
