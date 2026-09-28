#include "cpu_temperature.h"
#include "common/cpu_telemetry.h"
#include <windows.h>
#include <cmath>

namespace gauge {
Metric sample_cpu_temperature(const Hardware& hardware) {
    if (hardware.cpu_vendor != "AuthenticAMD")
        return missing(State::unsupported, "当前原生温度组件仅支持 AMD Zen", "PawnIO");
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, cpu_telemetry_name);
    if (!mapping) return missing(State::permission, "需要从设备页启动管理员 CPU 温度采集", "PawnIO · AMD Tctl");
    const auto* shared = static_cast<const CpuTelemetryRecord*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(CpuTelemetryRecord)));
    if (!shared) {
        CloseHandle(mapping);
        return missing(State::error, "无法读取 CPU 温度共享数据", "PawnIO · AMD Tctl");
    }
    CpuTelemetryRecord reading{};
    bool consistent{};
    for (int attempt = 0; attempt < 3; ++attempt) {
        const LONG before = shared->sequence;
        if (before & 1) continue;
        MemoryBarrier();
        reading.magic = shared->magic;
        reading.process_id = shared->process_id;
        reading.timestamp_ms = shared->timestamp_ms;
        reading.tctl_celsius = shared->tctl_celsius;
        reading.valid = shared->valid;
        MemoryBarrier();
        if (before == shared->sequence) { consistent = true; break; }
    }
    UnmapViewOfFile(shared);
    CloseHandle(mapping);
    const auto now = GetTickCount64();
    if (!consistent || reading.magic != cpu_telemetry_magic || reading.timestamp_ms > now ||
        now - reading.timestamp_ms > 3000)
        return missing(State::stale, "CPU 温度采集尚无新读数", "PawnIO · AMD Tctl");
    if (!reading.valid || !std::isfinite(reading.tctl_celsius) ||
        reading.tctl_celsius < 0 || reading.tctl_celsius > 130)
        return missing(State::error, "CPU 温度传感器读取失败", "PawnIO · AMD Tctl");
    return available(reading.tctl_celsius, "PawnIO · AMD Tctl (SMN 0x59800)");
}
}
