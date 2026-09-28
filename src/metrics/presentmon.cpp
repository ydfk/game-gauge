#include "presentmon.h"
#include "common/platform.h"
#include "PresentMonAPI.h"
#include <cstring>
#include <format>
#include <unordered_map>
#include <algorithm>
#include <cmath>

namespace gauge {
struct PresentMonProvider::Impl {
    HMODULE library{};
    PM_SESSION_HANDLE session{};
    PM_FRAME_QUERY_HANDLE frames{};
    uint32_t frame_bytes{}, pid{};
    uint64_t started{}, retry_at{}, raw_frames{}, accepted_frames{};
    std::string message{"PresentMon 服务尚未连接"};
    PM_QUERY_ELEMENT frame_elements[3]{{PM_METRIC_SWAP_CHAIN_ADDRESS, PM_STAT_NONE},
        {PM_METRIC_PRESENT_START_QPC, PM_STAT_NONE}, {PM_METRIC_BETWEEN_PRESENTS, PM_STAT_NONE}};
    struct Telemetry { PM_DYNAMIC_QUERY_HANDLE query{}; PM_QUERY_ELEMENT element{}; PM_DATA_TYPE type{}; std::string device; };
    std::vector<Telemetry> telemetry;
    decltype(&pmOpenSession) open{};
    decltype(&pmOpenSessionWithPipe) open_with_pipe{};
    decltype(&pmCloseSession) close{};
    decltype(&pmStartTrackingProcess) start{};
    decltype(&pmStopTrackingProcess) stop{};
    decltype(&pmRegisterFrameQuery) register_frames{};
    decltype(&pmConsumeFrames) consume{};
    decltype(&pmFreeFrameQuery) free_frames{};
    decltype(&pmGetIntrospectionRoot) introspect{};
    decltype(&pmFreeIntrospectionRoot) free_root{};
    decltype(&pmRegisterDynamicQuery) register_dynamic{};
    decltype(&pmPollDynamicQuery) poll_dynamic{};
    decltype(&pmFreeDynamicQuery) free_dynamic{};
    template<class T> bool bind(T& pointer, const char* name) {
        pointer = reinterpret_cast<T>(GetProcAddress(library, name)); return pointer != nullptr;
    }
    void disconnect() {
        if (frames && free_frames) free_frames(frames);
        frames = nullptr;
        for (auto& item : telemetry) if (item.query && free_dynamic) free_dynamic(item.query);
        telemetry.clear();
        if (session && close) close(session);
        session = nullptr; pid = 0; started = 0; raw_frames = accepted_frames = 0;
    }
    bool connect() {
        if (session) return true;
        if (GetTickCount64() < retry_at) return false;
        retry_at = GetTickCount64() + 5000;
        if (!library) {
            // 优先使用与 Intel 已安装服务同目录的 SDK DLL，避免服务与私有 DLL 版本错配。
            wchar_t program_files[32768]{};
            std::filesystem::path dll;
            if (GetEnvironmentVariableW(L"ProgramW6432", program_files, 32768))
                dll = std::filesystem::path(program_files) / L"Intel/PresentMonSharedService/PresentMonAPI2.dll";
            if (!dll.empty()) library = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!library) {
                dll = executable_dir() / L"PresentMonAPI2.dll";
                library = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
            }
            if (!library) { message = "缺少 PresentMon SDK：请运行依赖安装脚本"; return false; }
            const bool loaded = bind(open, "pmOpenSession") && bind(open_with_pipe, "pmOpenSessionWithPipe") &&
                bind(close, "pmCloseSession") && bind(start, "pmStartTrackingProcess") &&
                bind(stop, "pmStopTrackingProcess") && bind(register_frames, "pmRegisterFrameQuery") && bind(consume, "pmConsumeFrames") &&
                bind(free_frames, "pmFreeFrameQuery") && bind(introspect, "pmGetIntrospectionRoot") && bind(free_root, "pmFreeIntrospectionRoot") &&
                bind(register_dynamic, "pmRegisterDynamicQuery") && bind(poll_dynamic, "pmPollDynamicQuery") && bind(free_dynamic, "pmFreeDynamicQuery");
            if (!loaded) { message = "PresentMon API 版本不兼容"; FreeLibrary(library); library = nullptr; return false; }
        }
        char named_pipe[512]{};
        const auto pipe_length = GetEnvironmentVariableA("GAMEGAUGE_PRESENTMON_PIPE", named_pipe, sizeof(named_pipe));
        auto result = pipe_length && pipe_length < sizeof(named_pipe) ? open_with_pipe(&session, named_pipe) : open(&session);
        if (result != PM_STATUS_SUCCESS) { session = nullptr; message = "PresentMon 服务不可用，状态 " + std::to_string(result); return false; }
        const PM_INTROSPECTION_ROOT* root{};
        if (introspect(session, &root) == PM_STATUS_SUCCESS) {
            std::unordered_map<uint32_t, std::string> device_ids;
            for (size_t i = 0; i < root->pDevices->size; ++i) {
                auto device = static_cast<const PM_INTROSPECTION_DEVICE*>(root->pDevices->pData[i]);
                if (device->pLuid && device->pLuid->size == sizeof(LUID)) {
                    LUID luid{}; memcpy(&luid, device->pLuid->pData, sizeof(luid));
                    device_ids[device->id] = std::format("{:08x}:{:08x}", static_cast<uint32_t>(luid.HighPart), luid.LowPart);
                }
            }
            const std::vector<PM_METRIC> wanted{PM_METRIC_GPU_UTILIZATION, PM_METRIC_GPU_TEMPERATURE, PM_METRIC_GPU_POWER,
                PM_METRIC_GPU_FREQUENCY, PM_METRIC_GPU_MEM_USED, PM_METRIC_CPU_TEMPERATURE};
            for (size_t i = 0; i < root->pMetrics->size; ++i) {
                auto metric = static_cast<const PM_INTROSPECTION_METRIC*>(root->pMetrics->pData[i]);
                if (std::find(wanted.begin(), wanted.end(), metric->id) == wanted.end()) continue;
                for (size_t d = 0; d < metric->pDeviceMetricInfo->size; ++d) {
                    auto info = static_cast<const PM_INTROSPECTION_DEVICE_METRIC_INFO*>(metric->pDeviceMetricInfo->pData[d]);
                    if (info->availability != PM_METRIC_AVAILABILITY_AVAILABLE) continue;
                    Telemetry item{};
                    item.element = {metric->id, PM_STAT_AVG, info->deviceId};
                    item.type = metric->pTypeInfo->polledType;
                    if (device_ids.contains(info->deviceId)) item.device = device_ids[info->deviceId];
                    if (register_dynamic(session, &item.query, &item.element, 1, 1000, 0) == PM_STATUS_SUCCESS) telemetry.push_back(item);
                }
            }
            free_root(root);
        }
        message = "PresentMon 已连接 · 呈现事件口径";
        return true;
    }
    void poll_telemetry(Snapshot& snapshot) {
        for (const auto& item : telemetry) {
            std::vector<uint8_t> blob(std::max<uint64_t>(item.element.dataOffset + item.element.dataSize, 16) * 8);
            uint32_t chains = 8;
            if (poll_dynamic(item.query, pid, blob.data(), &chains) != PM_STATUS_SUCCESS || !chains) continue;
            double value{};
            const auto* data = blob.data() + item.element.dataOffset;
            if (item.type == PM_DATA_TYPE_DOUBLE && item.element.dataSize == 8) memcpy(&value, data, 8);
            else if (item.type == PM_DATA_TYPE_UINT64 && item.element.dataSize == 8) { uint64_t raw{}; memcpy(&raw, data, 8); value = static_cast<double>(raw); }
            else continue;
            if (!std::isfinite(value)) continue;
            auto reading = available(value, "PresentMon · 厂商遥测");
            if (item.element.metric == PM_METRIC_CPU_TEMPERATURE) { snapshot.cpu_temperature = reading; continue; }
            auto gpu = std::find_if(snapshot.hardware.gpus.begin(), snapshot.hardware.gpus.end(), [&](const Gpu& g) { return g.id == item.device; });
            if (gpu == snapshot.hardware.gpus.end()) continue;
            switch (item.element.metric) {
            case PM_METRIC_GPU_UTILIZATION: gpu->load = reading; break;
            case PM_METRIC_GPU_TEMPERATURE: gpu->temperature = reading; break;
            case PM_METRIC_GPU_POWER: gpu->power = reading; break;
            case PM_METRIC_GPU_FREQUENCY: gpu->clock = reading; break;
            case PM_METRIC_GPU_MEM_USED: gpu->memory_used = reading; break;
            default: break;
            }
        }
    }
};
PresentMonProvider::PresentMonProvider() : impl_(std::make_unique<Impl>()) {}
PresentMonProvider::~PresentMonProvider() { impl_->disconnect(); if (impl_->library) FreeLibrary(impl_->library); }
const std::string& PresentMonProvider::status() const { return impl_->message; }
void PresentMonProvider::reset() { impl_->disconnect(); impl_->retry_at = 0; }
void PresentMonProvider::poll(const Target& target, Snapshot& snapshot, FrameStatistics& statistics) {
    if (!target.pid) {
        if (impl_->pid) { impl_->disconnect(); statistics.reset(); }
        impl_->message = "等待游戏目标 · 帧采集按需连接";
        return;
    }
    if (!impl_->connect()) return;
    if (impl_->pid != target.pid || impl_->started != target.started) {
        if (impl_->pid) impl_->stop(impl_->session, impl_->pid);
        if (impl_->frames) impl_->free_frames(impl_->frames);
        impl_->frames = nullptr;
        statistics.reset();
        auto result = impl_->start(impl_->session, target.pid);
        if (result != PM_STATUS_SUCCESS && result != PM_STATUS_ALREADY_TRACKING_PROCESS) {
            impl_->message = "帧采集目标不可用，状态 " + std::to_string(result); impl_->disconnect(); return;
        }
        impl_->pid = target.pid; impl_->started = target.started;
        result = impl_->register_frames(impl_->session, &impl_->frames, impl_->frame_elements, 3, &impl_->frame_bytes);
        if (result != PM_STATUS_SUCCESS) { impl_->message = "帧查询不支持，状态 " + std::to_string(result); impl_->disconnect(); return; }
    }
    if (impl_->frame_bytes == 0 || impl_->frame_bytes > 65536) { impl_->disconnect(); return; }
    std::vector<uint8_t> frames(static_cast<size_t>(impl_->frame_bytes) * 1024);
    uint32_t count = 1024;
    const auto result = impl_->consume(impl_->frames, target.pid, frames.data(), &count);
    if (result != PM_STATUS_SUCCESS) { impl_->message = "帧流已断开，状态 " + std::to_string(result); impl_->disconnect(); statistics.reset(); return; }
    LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER qpc{}; QueryPerformanceCounter(&qpc);
    const auto now = GetTickCount64();
    for (uint32_t i = 0; i < count; ++i) {
        const auto* frame = frames.data() + static_cast<size_t>(i) * impl_->frame_bytes;
        uint64_t chain{}, timestamp{}; double ms{};
        memcpy(&chain, frame + impl_->frame_elements[0].dataOffset, std::min<uint64_t>(8, impl_->frame_elements[0].dataSize));
        memcpy(&timestamp, frame + impl_->frame_elements[1].dataOffset, std::min<uint64_t>(8, impl_->frame_elements[1].dataSize));
        memcpy(&ms, frame + impl_->frame_elements[2].dataOffset, std::min<uint64_t>(8, impl_->frame_elements[2].dataSize));
        const auto age = timestamp <= static_cast<uint64_t>(qpc.QuadPart) ? (static_cast<uint64_t>(qpc.QuadPart) - timestamp) * 1000 / frequency.QuadPart : 0;
        if (age < now && age < 60000) { statistics.add(now - age, chain, ms); ++impl_->accepted_frames; }
    }
    impl_->raw_frames += count;
    impl_->poll_telemetry(snapshot);
    impl_->message = std::format("PresentMon 已连接 · 原始帧 {} · 有效帧 {}", impl_->raw_frames, impl_->accepted_frames);
}
}
