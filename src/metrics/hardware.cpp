#include "hardware.h"
#include "common/platform.h"
#include <dxgi1_6.h>
#include <intrin.h>
#include <wrl/client.h>
#include <format>
#include <cstring>

namespace gauge {
Hardware discover_hardware() {
    Hardware hardware;
    hardware.logical_processors = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    int registers[4]{};
    __cpuid(registers, 0);
    char vendor[13]{};
    memcpy(vendor, &registers[1], 4); memcpy(vendor + 4, &registers[3], 4); memcpy(vendor + 8, &registers[2], 4);
    hardware.cpu_vendor = vendor;
    __cpuid(registers, static_cast<int>(0x80000000));
    if (static_cast<uint32_t>(registers[0]) >= 0x80000004) {
        char brand[49]{};
        for (int part = 0; part < 3; ++part) { __cpuid(registers, static_cast<int>(0x80000002 + part)); memcpy(brand + part * 16, registers, 16); }
        hardware.cpu = brand;
        hardware.cpu.erase(hardware.cpu.find_last_not_of(' ') + 1);
    }
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return hardware;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    for (UINT index = 0; factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND; ++index) {
        DXGI_ADAPTER_DESC1 descriptor{};
        adapter->GetDesc1(&descriptor);
        if (descriptor.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) { adapter.Reset(); continue; }
        Gpu gpu;
        gpu.id = std::format("{:08x}:{:08x}", static_cast<uint32_t>(descriptor.AdapterLuid.HighPart), descriptor.AdapterLuid.LowPart);
        gpu.name = utf8(descriptor.Description);
        gpu.vendor_id = descriptor.VendorId;
        gpu.vendor = descriptor.VendorId == 0x10de ? "NVIDIA" : descriptor.VendorId == 0x1002 ? "AMD" : descriptor.VendorId == 0x8086 ? "Intel" : "Unknown";
        gpu.dedicated_bytes = descriptor.DedicatedVideoMemory;
        gpu.memory_total = available(static_cast<double>(gpu.dedicated_bytes), "DXGI · 专用显存容量");
        gpu.load = gpu.temperature = gpu.power = gpu.clock = gpu.memory_used = gpu.fan = missing(State::unsupported, "等待可用的厂商遥测接口");
        hardware.gpus.push_back(gpu);
        Microsoft::WRL::ComPtr<IDXGIOutput> output;
        for (UINT screen = 0; adapter->EnumOutputs(screen, &output) != DXGI_ERROR_NOT_FOUND; ++screen) {
            DXGI_OUTPUT_DESC desc{}; output->GetDesc(&desc);
            if (desc.AttachedToDesktop) {
                Display display{utf8(desc.DeviceName), desc.DesktopCoordinates.left, desc.DesktopCoordinates.top,
                    desc.DesktopCoordinates.right - desc.DesktopCoordinates.left, desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top};
                Microsoft::WRL::ComPtr<IDXGIOutput6> hdr_output;
                if (SUCCEEDED(output.As(&hdr_output))) {
                    DXGI_OUTPUT_DESC1 extended{};
                    if (SUCCEEDED(hdr_output->GetDesc1(&extended))) display.hdr = extended.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                }
                UINT dpi_x = 96, dpi_y = 96;
                auto shcore = LoadLibraryExW(L"shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
                if (shcore) {
                    using GetDpi = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
                    auto get_dpi = reinterpret_cast<GetDpi>(GetProcAddress(shcore, "GetDpiForMonitor"));
                    if (get_dpi) get_dpi(desc.Monitor, 0, &dpi_x, &dpi_y);
                    FreeLibrary(shcore);
                }
                display.dpi = dpi_x;
                hardware.displays.push_back(display);
            }
            output.Reset();
        }
        adapter.Reset();
    }
    return hardware;
}
}

