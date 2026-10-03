#include "metric_style.h"
#include <cmath>

namespace gauge {
namespace {
std::optional<double> value_of(const Metric& metric) {
    return metric.state == State::valid ? metric.value : std::nullopt;
}
std::optional<double> value_of(const Json& metric) {
    if (!metric.is_object() || metric.value("state", std::string{}) != "valid") return {};
    const auto value = metric.value("value", Json{});
    return value.is_number() ? std::optional<double>(value.get<double>()) : std::nullopt;
}
std::optional<double> percent(std::optional<double> used, std::optional<double> total) {
    return used && total && *total > 0 ? std::optional<double>(*used / *total * 100) : std::nullopt;
}
MetricTone rising(double value, double watch, double high, double critical) {
    return value >= critical ? MetricTone::critical : value >= high ? MetricTone::high :
        value >= watch ? MetricTone::watch : MetricTone::good;
}
}
uint32_t tone_rgb(MetricTone tone) {
    switch (tone) {
    case MetricTone::muted: return 0x9AACC0;
    case MetricTone::good: return 0x50FF00;
    case MetricTone::excellent: return 0x00C853;
    case MetricTone::cool: return 0x30C8FF;
    case MetricTone::watch: return 0xFFE000;
    case MetricTone::high: return 0xFFA000;
    case MetricTone::critical: return 0xFF4040;
    default: return 0xFFFFFF;
    }
}
MetricTone metric_tone(std::string_view id, std::optional<double> value) {
    if (!value || !std::isfinite(*value)) return MetricTone::muted;
    const double v = *value;
    if (id == "fps" || id == "low1" || id == "low01")
        return v < 15 ? MetricTone::critical : v < 30 ? MetricTone::high : v < 60 ? MetricTone::watch : v > 144 ? MetricTone::excellent : MetricTone::good;
    if (id == "frametime") return rising(v, 16.8, 25, 33.4);
    if (id == "cpu_temperature") return rising(v, 70, 80, 90);
    if (id == "gpu_temperature") return rising(v, 65, 75, 85);
    if (id == "disk_temperature") return rising(v, 45, 55, 65);
    if (id == "memory_load" || id == "memory_used" || id == "vram") return rising(v, 70, 80, 90);
    // 占用率颜色表示负载等级，红色提醒接近满载。
    if (id == "gpu_load") return rising(v, 80, 90, 95);
    if (id == "cpu_load" || id == "process_cpu")
        return rising(v, 70, 80, 90);
    if (id.ends_with("clock") || id == "gpu_power" || id == "gpu_fan") return MetricTone::cool;
    return MetricTone::neutral;
}
MetricTone metric_tone(std::string_view id, const Snapshot& s) {
    if (id == "session") return MetricTone::neutral;
    if (id == "memory_used") return metric_tone(id, percent(value_of(s.memory_used), value_of(s.memory_total)));
    if (id.starts_with("gpu_") || id == "vram") {
        for (const auto& gpu : s.hardware.gpus) if (gpu.id == s.selected_gpu) {
            if (id == "vram") return metric_tone(id, percent(value_of(gpu.memory_used), value_of(gpu.memory_total)));
            const Metric* metric = id == "gpu_temperature" ? &gpu.temperature : id == "gpu_load" ? &gpu.load :
                id == "gpu_clock" ? &gpu.clock : id == "gpu_power" ? &gpu.power : &gpu.fan;
            return metric_tone(id, value_of(*metric));
        }
        return MetricTone::muted;
    }
    const Metric* metric = id == "fps" ? &s.fps : id == "low1" ? &s.low1 : id == "low01" ? &s.low01 :
        id == "frametime" ? &s.frametime : id == "cpu_temperature" ? &s.cpu_temperature : id == "cpu_load" ? &s.cpu_load :
        id == "cpu_clock" ? &s.cpu_clock : id == "memory_load" ? &s.memory_load : id == "process_cpu" ? &s.process_cpu :
        id == "disk_temperature" ? &s.disk_temperature : &s.process_memory;
    return metric_tone(id, value_of(*metric));
}
MetricTone metric_tone(std::string_view id, const Json& s) {
    if (!s.is_object()) return MetricTone::muted;
    if (id == "session") return MetricTone::neutral;
    if (id == "memory_used") return metric_tone(id, percent(value_of(s.value("memory_used", Json{})), value_of(s.value("memory_total", Json{}))));
    if (id.starts_with("gpu_") || id == "vram") {
        for (const auto& gpu : s.value("gpus", Json::array())) if (gpu.value("id", std::string{}) == s.value("selected_gpu", std::string{})) {
            if (id == "vram") return metric_tone(id, percent(value_of(gpu.value("memory_used", Json{})), value_of(gpu.value("memory_total", Json{}))));
            return metric_tone(id, value_of(gpu.value(std::string(id.substr(4)), Json{})));
        }
        return MetricTone::muted;
    }
    return metric_tone(id, value_of(s.value(std::string(id), Json{})));
}
MetricTone obs_tone(std::string_view state) {
    return state == "recording" ? MetricTone::good : state == "paused" ? MetricTone::critical : MetricTone::high;
}
std::wstring obs_label(std::string_view state) {
    return state == "recording" ? L"● 录制中" : state == "paused" ? L"Ⅱ 已暂停" : state == "idle" ? L"● 未录制" :
        state == "disabled" || state == "off" ? L"● 连接未启用" : state == "connecting" ? L"● 正在连接" : L"● 未连接";
}
}
