#pragma once
#include <windows.h>

namespace gauge {
inline constexpr wchar_t cpu_telemetry_name[] = L"Local\\GameGauge.CpuTemperature.v1";
inline constexpr DWORD cpu_telemetry_magic = 0x47474354;
struct CpuTelemetryRecord {
    volatile LONG sequence{};
    DWORD magic{};
    DWORD process_id{};
    ULONGLONG timestamp_ms{};
    double tctl_celsius{};
    DWORD valid{};
};
}
