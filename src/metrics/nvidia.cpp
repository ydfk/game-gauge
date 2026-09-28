#include "nvidia.h"
#include <algorithm>
#include <filesystem>

namespace gauge {
namespace {
using Device = void*;
struct Utilization { unsigned gpu, memory; };
struct Memory { unsigned long long total, free, used; };
template<class T> T endpoint(HMODULE library, const char* name) { return reinterpret_cast<T>(GetProcAddress(library, name)); }
Metric status_metric(int status, double value, const char* source) {
    if (!status) return available(value, source);
    return missing(status == 4 ? State::permission : State::unsupported, "NVML 返回状态 " + std::to_string(status), source);
}
}
NvidiaProvider::NvidiaProvider() {
    module_ = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module_) {
        wchar_t program_files[32768]{};
        if (GetEnvironmentVariableW(L"ProgramW6432", program_files, 32768)) {
            auto path = std::filesystem::path(program_files) / L"NVIDIA Corporation/NVSMI/nvml.dll";
            module_ = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        }
    }
    if (module_) {
        auto init = endpoint<int(*)()>(module_, "nvmlInit_v2");
        initialized_ = init && init() == 0;
    }
}
NvidiaProvider::~NvidiaProvider() {
    if (!module_) return;
    if (initialized_) if (auto shutdown = endpoint<int(*)()>(module_, "nvmlShutdown")) shutdown();
    FreeLibrary(module_);
}
void NvidiaProvider::sample(Hardware& hardware) {
    if (!initialized_) return;
    auto count_fn = endpoint<int(*)(unsigned*)>(module_, "nvmlDeviceGetCount_v2");
    auto device_fn = endpoint<int(*)(unsigned, Device*)>(module_, "nvmlDeviceGetHandleByIndex_v2");
    auto name_fn = endpoint<int(*)(Device, char*, unsigned)>(module_, "nvmlDeviceGetName");
    if (!count_fn || !device_fn || !name_fn) return;
    unsigned count{};
    if (count_fn(&count)) return;
    for (unsigned index = 0; index < count; ++index) {
        Device device{}; char name[128]{};
        if (device_fn(index, &device) || name_fn(device, name, sizeof(name))) continue;
        auto matching = std::count_if(hardware.gpus.begin(), hardware.gpus.end(), [&](const Gpu& gpu) { return gpu.vendor_id == 0x10de && gpu.name == name; });
        // 同型号多卡无法凭名称可靠映射，交由 PresentMon 的 LUID 遥测路径处理。
        if (matching != 1) continue;
        auto gpu = std::find_if(hardware.gpus.begin(), hardware.gpus.end(), [&](const Gpu& item) { return item.vendor_id == 0x10de && item.name == name; });
        Utilization utilization{};
        if (auto fn = endpoint<int(*)(Device, Utilization*)>(module_, "nvmlDeviceGetUtilizationRates")) {
            const int result = fn(device, &utilization);
            gpu->load = status_metric(result, utilization.gpu, "NVML · 整卡忙碌率");
        }
        unsigned value{};
        if (auto fn = endpoint<int(*)(Device, unsigned, unsigned*)>(module_, "nvmlDeviceGetTemperature")) {
            const int result = fn(device, 0, &value); gpu->temperature = status_metric(result, value, "NVML · GPU 核心温度");
        }
        if (auto fn = endpoint<int(*)(Device, unsigned*)>(module_, "nvmlDeviceGetPowerUsage")) {
            const int result = fn(device, &value); gpu->power = status_metric(result, value / 1000.0, "NVML · 功耗 W");
        }
        if (auto fn = endpoint<int(*)(Device, unsigned, unsigned*)>(module_, "nvmlDeviceGetClockInfo")) {
            const int result = fn(device, 0, &value); gpu->clock = status_metric(result, value, "NVML · 图形时钟 MHz");
        }
        if (auto fn = endpoint<int(*)(Device, unsigned*)>(module_, "nvmlDeviceGetFanSpeed")) {
            const int result = fn(device, &value); gpu->fan = status_metric(result, value, "NVML · 风扇 %（非 RPM）");
        }
        Memory memory{};
        if (auto fn = endpoint<int(*)(Device, Memory*)>(module_, "nvmlDeviceGetMemoryInfo")) {
            const int result = fn(device, &memory);
            gpu->memory_used = status_metric(result, static_cast<double>(memory.used), "NVML · 整卡显存已用字节");
            gpu->memory_total = status_metric(result, static_cast<double>(memory.total), "NVML · 显存总字节");
        }
    }
}
}
