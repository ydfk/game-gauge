#include "sampler.h"
#include "hardware.h"
#include "cpu_temperature.h"
#include "nvidia.h"
#include "presentmon.h"
#include "target.h"
#include "common/platform.h"
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#include <powrprof.h>
#include <algorithm>
#include <format>
#include <map>
#include <regex>
#include <utility>

namespace gauge {
namespace {
struct ProcessorPowerInformation {
    ULONG number;
    ULONG max_mhz;
    ULONG current_mhz;
    ULONG mhz_limit;
    ULONG max_idle_state;
    ULONG current_idle_state;
};
struct SystemCounters {
    PDH_HQUERY query{};
    PDH_HCOUNTER cpu{}, gpu_engines{};
    uint64_t previous_process{}, previous_time{};
    uint64_t opened_at{GetTickCount64()};
    uint32_t previous_pid{};
    SystemCounters() {
        if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) return;
        PdhAddEnglishCounterW(query, L"\\Processor Information(_Total)\\% Processor Time", 0, &cpu);
        PdhAddEnglishCounterW(query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &gpu_engines);
        PdhCollectQueryData(query);
    }
    ~SystemCounters() { if (query) PdhCloseQuery(query); }
    void sample(Snapshot& s, const Config& config) {
        if (query) PdhCollectQueryData(query);
        PDH_FMT_COUNTERVALUE value{};
        if (GetTickCount64() - opened_at >= 800 && cpu &&
            PdhGetFormattedCounterValue(cpu, PDH_FMT_DOUBLE, nullptr, &value) == ERROR_SUCCESS &&
            (value.CStatus == PDH_CSTATUS_VALID_DATA || value.CStatus == PDH_CSTATUS_NEW_DATA))
            s.cpu_load = available(std::clamp(value.doubleValue, 0.0, 100.0), "PDH · 全处理器组总占用");
        else s.cpu_load = missing(State::waiting, "CPU 计数器等待第二次采样", "Windows PDH");
        MEMORYSTATUSEX memory{sizeof(memory)};
        if (GlobalMemoryStatusEx(&memory)) {
            s.memory_total = available(static_cast<double>(memory.ullTotalPhys), "Windows · 物理内存");
            s.memory_used = available(static_cast<double>(memory.ullTotalPhys - memory.ullAvailPhys), "Windows · 物理内存");
            s.memory_load = available(memory.dwMemoryLoad, "Windows · 物理内存占用");
        }
        std::vector<ProcessorPowerInformation> power(s.hardware.logical_processors);
        if (!power.empty() && CallNtPowerInformation(ProcessorInformation, nullptr, 0, power.data(), static_cast<ULONG>(power.size() * sizeof(power[0]))) == 0) {
            double sum{}; for (const auto& core : power) sum += core.current_mhz;
            s.cpu_clock = available(sum / power.size(), "Windows · 报告频率 MHz，非有效时钟");
        }
        sample_process(s);
        select_gpu(s, config);
    }
    void sample_process(Snapshot& s) {
        s.process_cpu = missing(State::waiting, "等待进程采样");
        s.process_memory = missing(State::waiting, "没有目标进程");
        UniqueHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, s.target.pid));
        if (!process) return;
        PROCESS_MEMORY_COUNTERS_EX memory{sizeof(memory)};
        if (GetProcessMemoryInfo(process.value, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)))
            s.process_memory = available(static_cast<double>(memory.WorkingSetSize), "Windows · 游戏 Working Set");
        FILETIME started{}, exited{}, kernel{}, user{};
        if (!GetProcessTimes(process.value, &started, &exited, &kernel, &user)) return;
        const auto time = GetTickCount64(), consumed = file_ticks(kernel) + file_ticks(user);
        if (s.target.pid == previous_pid && time > previous_time && consumed >= previous_process)
            s.process_cpu = available(100.0 * (consumed - previous_process) / ((time - previous_time) * 10000.0 * s.hardware.logical_processors), "Windows · 游戏进程，整机归一化");
        previous_time = time; previous_process = consumed; previous_pid = s.target.pid;
    }
    void select_gpu(Snapshot& s, const Config& config) {
        if (!config.gpu_id.empty()) {
            const bool exists = std::any_of(s.hardware.gpus.begin(), s.hardware.gpus.end(), [&](const Gpu& gpu) { return gpu.id == config.gpu_id; });
            s.selected_gpu = exists ? config.gpu_id : std::string{};
            s.gpu_selection_reason = exists ? "用户指定的 GPU" : "指定 GPU 暂不可用";
            return;
        }
        std::map<std::string, double> target_load;
        if (gpu_engines && s.target.pid) {
            DWORD bytes{}, count{};
            if (PdhGetFormattedCounterArrayW(gpu_engines, PDH_FMT_DOUBLE, &bytes, &count, nullptr) == PDH_MORE_DATA && bytes <= 8 * 1024 * 1024) {
                std::vector<unsigned char> buffer(bytes);
                auto items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
                if (PdhGetFormattedCounterArrayW(gpu_engines, PDH_FMT_DOUBLE, &bytes, &count, items) == ERROR_SUCCESS) {
                    const auto pid_prefix = L"pid_" + std::to_wstring(s.target.pid) + L"_";
                    static const std::wregex pattern(L"luid_0x([0-9a-fA-F]+)_0x([0-9a-fA-F]+)");
                    for (DWORD i = 0; i < count; ++i) {
                        std::wstring name = items[i].szName;
                        if (!name.starts_with(pid_prefix) || name.find(L"engtype_3D") == std::wstring::npos) continue;
                        if (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA && items[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) continue;
                        std::wsmatch match;
                        if (std::regex_search(name, match, pattern)) {
                            const auto high = static_cast<uint32_t>(std::stoul(match[1], nullptr, 16));
                            const auto low = static_cast<uint32_t>(std::stoul(match[2], nullptr, 16));
                            auto id = std::format("{:08x}:{:08x}", high, low);
                            target_load[id] = std::max(target_load[id], items[i].FmtValue.doubleValue);
                        }
                    }
                }
            }
        }
        auto best = std::max_element(target_load.begin(), target_load.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
        if (best != target_load.end() && best->second > .05) {
            s.selected_gpu = best->first; s.gpu_selection_reason = "Windows 游戏进程 3D 引擎 LUID";
        } else if (s.hardware.gpus.size() == 1) {
            s.selected_gpu = s.hardware.gpus.front().id; s.gpu_selection_reason = "唯一硬件适配器";
        } else {
            s.selected_gpu.clear(); s.gpu_selection_reason = "无法可靠确定游戏 GPU，可在设置中指定";
        }
    }
};
}
Sampler::Sampler(Config config) : config_(std::move(config)), worker_([this](std::stop_token stop) { run(stop); }) {}
Sampler::~Sampler() { worker_.request_stop(); wake_.notify_all(); }
void Sampler::configure(Config config) { std::lock_guard lock(mutex_); config_ = std::move(config); wake_.notify_all(); }
void Sampler::reset_statistics() { std::lock_guard lock(mutex_); reset_ = true; wake_.notify_all(); }
void Sampler::refresh_hardware() { std::lock_guard lock(mutex_); rediscover_ = true; wake_.notify_all(); }
Snapshot Sampler::snapshot() const { std::lock_guard lock(mutex_); return snapshot_; }
void Sampler::run(std::stop_token stop) {
    Snapshot current;
    current.hardware = discover_hardware();
    NvidiaProvider nvidia;
    PresentMonProvider presentmon;
    FrameStatistics statistics;
    SystemCounters counters;
    uint64_t last_hardware{}, last_session = GetTickCount64();
    bool was_paused{}, was_foreground{};
    while (!stop.stop_requested()) {
        Config config; bool reset{}, rediscover{};
        {
            std::lock_guard lock(mutex_); config = config_;
            reset = std::exchange(reset_, false); rediscover = std::exchange(rediscover_, false);
        }
        if (rediscover) current.hardware = discover_hardware();
        const auto previous = current.target;
        current.target = find_target(config, previous);
        const bool target_changed = previous.pid != current.target.pid || previous.started != current.target.started;
        if (reset || target_changed) {
            statistics.reset(); presentmon.reset(); current.session_seconds = 0;
        } else if (was_paused != config.paused) {
            statistics.reset(); presentmon.reset();
        } else if (was_foreground != current.target.foreground) {
            statistics.reset();
        }
        current.timestamp_ms = GetTickCount64(); current.paused = config.paused;
        current.cpu_temperature = sample_cpu_temperature(current.hardware);
        if (!config.paused) {
            if (current.timestamp_ms - last_hardware >= 1000) {
                counters.sample(current, config); nvidia.sample(current.hardware); last_hardware = current.timestamp_ms;
            }
            const bool active = current.target.pid != 0;
            const bool collecting = active && current.target.foreground;
            presentmon.poll(active ? current.target : Target{}, current, statistics, collecting);
            statistics.publish(current, current.timestamp_ms);
            if (collecting && was_foreground && !was_paused && !target_changed)
                current.session_seconds += (current.timestamp_ms - last_session) / 1000.0;
            current.frame_status = presentmon.status();
            if (active && !collecting) current.frame_status += " · 游戏已失焦，统计暂停";
        } else {
            current.frame_status = "采集已暂停";
            current.fps = current.frametime = current.low1 = current.low01 = missing(State::stale, "采集已暂停");
            current.frame_samples = 0;
            current.recent_frames.clear();
        }
        last_session = current.timestamp_ms; was_paused = config.paused; was_foreground = current.target.foreground;
        {
            std::lock_guard lock(mutex_); snapshot_ = current;
        }
        std::unique_lock lock(mutex_);
        wake_.wait_for(lock, std::chrono::milliseconds(current.target.pid || config.preview ? config.refresh_ms : 1000));
    }
}
}
