#include "common/metric_style.h"
#include <cmath>
#include <stdexcept>

void metric_style_contracts() {
    using namespace gauge;
    const auto expect = [](std::string_view id, double value, MetricTone expected) {
        if (metric_tone(id, std::optional<double>(value)) != expected) throw std::runtime_error("metric color boundary mismatch");
    };
    expect("cpu_load", 59.9, MetricTone::good); expect("cpu_load", 60, MetricTone::cool);
    expect("cpu_load", 80, MetricTone::watch); expect("cpu_load", 95, MetricTone::high); expect("cpu_load", 100, MetricTone::critical);
    expect("gpu_load", 80, MetricTone::cool); expect("gpu_load", 95, MetricTone::watch); expect("gpu_load", 100, MetricTone::high);
    expect("memory_load", 79.9, MetricTone::good); expect("memory_load", 80, MetricTone::watch);
    expect("memory_load", 90, MetricTone::high); expect("memory_load", 95, MetricTone::critical);
    expect("cpu_temperature", 74.9, MetricTone::good); expect("cpu_temperature", 75, MetricTone::watch);
    expect("cpu_temperature", 85, MetricTone::high); expect("cpu_temperature", 95, MetricTone::critical);
    expect("gpu_temperature", 70, MetricTone::watch); expect("gpu_temperature", 80, MetricTone::high); expect("gpu_temperature", 90, MetricTone::critical);
    expect("disk_temperature", 50, MetricTone::watch); expect("disk_temperature", 60, MetricTone::high); expect("disk_temperature", 70, MetricTone::critical);
    expect("fps", 0, MetricTone::critical); expect("fps", 15, MetricTone::high); expect("fps", 30, MetricTone::watch);
    expect("fps", 60, MetricTone::good); expect("fps", 144, MetricTone::cool);
    expect("frametime", 16.7, MetricTone::good); expect("frametime", 16.8, MetricTone::watch);
    expect("frametime", 33.4, MetricTone::high); expect("frametime", 66.7, MetricTone::critical);
    if (metric_tone("cpu_load", std::nullopt) != MetricTone::muted ||
        metric_tone("fps", std::optional<double>(NAN)) != MetricTone::muted) throw std::runtime_error("missing values must be muted");
    if (obs_tone("recording") != MetricTone::good || obs_tone("paused") != MetricTone::critical ||
        obs_tone("idle") != MetricTone::high || obs_tone("disconnected") != MetricTone::high || obs_tone("disabled") != MetricTone::high)
        throw std::runtime_error("OBS status color mismatch");
    Snapshot snapshot; snapshot.cpu_load = available(100, "test"); snapshot.memory_used = available(90, "test"); snapshot.memory_total = available(100, "test");
    Gpu gpu; gpu.id = "test"; gpu.load = available(100, "test"); gpu.memory_used = available(95, "test"); gpu.memory_total = available(100, "test");
    snapshot.hardware.gpus.push_back(gpu); snapshot.selected_gpu = "test";
    for (const auto* id : {"cpu_load", "gpu_load", "memory_used", "vram", "fps", "gpu_temperature"})
        if (metric_tone(id, snapshot) != metric_tone(id, snapshot_json(snapshot))) throw std::runtime_error("HUD and settings colors must match");
    if (metric_tone("memory_used", snapshot) != MetricTone::high || metric_tone("vram", snapshot) != MetricTone::critical)
        throw std::runtime_error("capacity metrics must use utilization rather than absolute bytes");
    snapshot.hardware.gpus[0].memory_total = missing(State::unsupported, "test");
    if (metric_tone("vram", snapshot) != MetricTone::muted) throw std::runtime_error("missing capacity must not look healthy");
}
