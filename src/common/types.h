#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace gauge {
enum class State { valid, unsupported, permission, waiting, stale, error };
struct Metric {
    std::optional<double> value;
    State state{State::unsupported};
    std::string source;
    std::string reason;
};
inline Metric available(double value, std::string source) {
    return {value, State::valid, std::move(source), {}};
}
inline Metric missing(State state, std::string reason, std::string source = {}) {
    return {{}, state, std::move(source), std::move(reason)};
}
struct Gpu {
    std::string id;
    std::string name;
    std::string vendor;
    std::string pci;
    uint32_t vendor_id{};
    uint64_t dedicated_bytes{};
    Metric load, temperature, power, clock, memory_used, memory_total, fan;
};
struct Display {
    std::string name;
    int x{}, y{}, width{}, height{};
    uint32_t dpi{96};
    bool hdr{};
};
struct Hardware {
    std::string cpu;
    std::string cpu_vendor;
    uint32_t logical_processors{};
    std::vector<Gpu> gpus;
    std::vector<Display> displays;
};
struct Target {
    uint32_t pid{};
    uint64_t started{};
    uintptr_t window{};
    std::string name;
    std::string path;
    bool foreground{};
};
struct Snapshot {
    std::string obs_state{"disconnected"};
    uint64_t timestamp_ms{};
    Target target;
    Hardware hardware;
    Metric cpu_load, cpu_clock, cpu_temperature, memory_load, memory_used, memory_total;
    Metric process_cpu, process_memory, fps, frametime, low1, low01;
    uint64_t frame_samples{};
    double session_seconds{};
    std::vector<double> recent_frames;
    std::string frame_status;
    std::string selected_gpu;
    std::string gpu_selection_reason;
    bool paused{};
    bool game_confirmed{};
    std::string history_error;
};
struct Config {
    bool show_obs{true};
    bool check_updates{true};
    bool auto_update{};
    bool enabled{true};
    bool paused{};
    bool exclude_capture{true};
    bool graph{};
    bool preview{};
    bool hide_on_blur{true};
    bool auto_target{true};
    uint32_t target_pid{};
    uint32_t refresh_ms{250};
    double font_size{14};
    double opacity{0.86};
    int anchor{};
    int margin_x{12}, margin_y{};
    std::string gpu_id;
    std::vector<std::string> metrics{"fps", "cpu_temperature", "cpu_load", "gpu_temperature", "gpu_load", "vram", "memory_load", "session"};
    std::vector<std::string> ignored_processes;
    std::vector<std::string> known_games;
};
}
