#include <windows.h>
#include "common/cpu_telemetry.h"
#include <bcrypt.h>
#include <sddl.h>
#include <intrin.h>
#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {
constexpr wchar_t pawnio_registry[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\PawnIO";
constexpr char module_sha256[] = "dae74615761b78bdf064dfb3e136252ddcc6fc727d88f14738d0e5800d427a91";
constexpr unsigned long long temperature_register = 0x00059800;

std::string hex(const unsigned char* bytes, size_t count) {
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (size_t i = 0; i < count; ++i) output << std::setw(2) << static_cast<unsigned>(bytes[i]);
    return output.str();
}

std::vector<unsigned char> verified_module(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("无法读取指定的 PawnIO 模块");
    const auto size = input.tellg();
    if (size != 10652) throw std::runtime_error("PawnIO 模块大小不符合 0.2.11 官方发行版");
    std::vector<unsigned char> bytes(static_cast<size_t>(size));
    input.seekg(0);
    if (!input.read(reinterpret_cast<char*>(bytes.data()), size)) throw std::runtime_error("PawnIO 模块读取不完整");
    BCRYPT_ALG_HANDLE algorithm{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("无法初始化 SHA-256");
    std::array<unsigned char, 32> digest{};
    const auto result = BCryptHash(algorithm, nullptr, 0, bytes.data(), static_cast<ULONG>(bytes.size()),
        digest.data(), static_cast<ULONG>(digest.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (result < 0 || hex(digest.data(), digest.size()) != module_sha256)
        throw std::runtime_error("PawnIO 模块 SHA-256 不符合官方 0.2.11 发行资产");
    return bytes;
}

std::filesystem::path installed_library() {
    std::wstring location(32768, L'\0');
    DWORD size = static_cast<DWORD>(location.size() * sizeof(wchar_t));
    const auto error = RegGetValueW(HKEY_LOCAL_MACHINE, pawnio_registry, L"InstallLocation",
        RRF_RT_REG_SZ, nullptr, location.data(), &size);
    if (error != ERROR_SUCCESS) throw std::runtime_error("没有找到已安装的 PawnIO");
    location.resize(wcsnlen_s(location.c_str(), location.size()));
    return std::filesystem::path(location) / L"PawnIOLib.dll";
}

struct CpuId {
    int family{};
    int model{};
};
CpuId cpu_id() {
    int registers[4]{};
    __cpuid(registers, 0);
    const std::string vendor(reinterpret_cast<char*>(&registers[1]), 4);
    const std::string vendor_tail(reinterpret_cast<char*>(&registers[3]), 4);
    const std::string vendor_middle(reinterpret_cast<char*>(&registers[2]), 4);
    if (vendor + vendor_tail + vendor_middle != "AuthenticAMD")
        throw std::runtime_error("此探针仅用于 AMD CPU");
    __cpuid(registers, 1);
    const auto eax = static_cast<unsigned>(registers[0]);
    int family = static_cast<int>((eax >> 8) & 0xf);
    int model = static_cast<int>((eax >> 4) & 0xf);
    if (family == 0xf) family += static_cast<int>((eax >> 20) & 0xff);
    if (family >= 0x6) model |= static_cast<int>((eax >> 12) & 0xf0);
    if (family < 0x17 || family > 0x1a) throw std::runtime_error("此探针仅用于 AMD Zen CPU");
    return {family, model};
}

using Open = HRESULT(__stdcall*)(PHANDLE);
using Load = HRESULT(__stdcall*)(HANDLE, const UCHAR*, SIZE_T);
using Execute = HRESULT(__stdcall*)(HANDLE, PCSTR, const ULONG64*, SIZE_T, PULONG64, SIZE_T, PSIZE_T);
using Close = HRESULT(__stdcall*)(HANDLE);
template <typename T> T resolve(HMODULE module, const char* name) {
    auto address = GetProcAddress(module, name);
    if (!address) throw std::runtime_error(std::string("PawnIOLib 缺少接口：") + name);
    return reinterpret_cast<T>(address);
}

unsigned long long read_temperature_register(const std::vector<unsigned char>& blob) {
    const auto library_path = installed_library();
    HMODULE library = LoadLibraryExW(library_path.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!library) throw std::runtime_error("无法加载已安装的 PawnIOLib.dll");
    try {
        const auto open = resolve<Open>(library, "pawnio_open");
        const auto load = resolve<Load>(library, "pawnio_load");
        const auto execute = resolve<Execute>(library, "pawnio_execute");
        const auto close = resolve<Close>(library, "pawnio_close");
        HANDLE handle{};
        const auto open_result = open(&handle);
        if (FAILED(open_result) || !handle)
            throw std::runtime_error("无法连接已安装的 PawnIO 驱动，HRESULT=" +
                std::to_string(static_cast<unsigned long>(open_result)));
        try {
            const auto load_result = load(handle, blob.data(), blob.size());
            if (FAILED(load_result))
                throw std::runtime_error("官方 AMDFamily17 模块未被 PawnIO 驱动接受，HRESULT=" +
                    std::to_string(static_cast<unsigned long>(load_result)));
            HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Global\\Access_PCI");
            if (!mutex) throw std::runtime_error("无法获取 PCI 访问互斥锁");
            try {
                const auto wait = WaitForSingleObject(mutex, 2000);
                if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED)
                    throw std::runtime_error("等待 PCI 访问互斥锁超时");
                ULONG64 input[] = {temperature_register};
                ULONG64 output[1]{};
                SIZE_T written{};
                const auto result = execute(handle, "ioctl_read_smn", input, 1, output, 1, &written);
                ReleaseMutex(mutex);
                if (FAILED(result) || written != 1)
                    throw std::runtime_error("读取 AMD 温度 SMN 寄存器失败，HRESULT=" +
                        std::to_string(static_cast<unsigned long>(result)) + ",written=" + std::to_string(written));
                CloseHandle(mutex);
                close(handle);
                FreeLibrary(library);
                return output[0];
            } catch (...) { CloseHandle(mutex); throw; }
        } catch (...) { close(handle); throw; }
    } catch (...) { FreeLibrary(library); throw; }
}
double tctl_celsius(unsigned long long raw) {
    const auto value = static_cast<unsigned>(raw);
    if (value == 0 || value == 0xffffffff) throw std::runtime_error("温度寄存器返回无效原始值");
    double celsius = static_cast<double>(value >> 21) * 0.125;
    if ((value & 0x80000) || (value & 0x30000) == 0x30000) celsius -= 49.0;
    if (celsius < 0 || celsius > 130) throw std::runtime_error("温度原始值超出合理范围");
    return celsius;
}
void serve(const std::vector<unsigned char>& blob, DWORD host_pid, HANDLE stop = nullptr) {
    HANDLE host = stop ? stop : OpenProcess(SYNCHRONIZE, FALSE, host_pid);
    if (!host) throw std::runtime_error("无法绑定 GameGauge 宿主进程");
    PSECURITY_DESCRIPTOR descriptor{};
    if (stop && !ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;IU)", SDDL_REVISION_1, &descriptor, nullptr))
        throw std::runtime_error("无法设置温度共享数据权限");
    SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, stop ? &security : nullptr, PAGE_READWRITE, 0,
        sizeof(gauge::CpuTelemetryRecord), stop ? gauge::cpu_service_telemetry_name : gauge::cpu_telemetry_name);
    const DWORD mapping_error = GetLastError();
    if (descriptor) LocalFree(descriptor);
    if (!mapping || mapping_error == ERROR_ALREADY_EXISTS) {
        if (mapping) CloseHandle(mapping);
        if (!stop) CloseHandle(host);
        throw std::runtime_error("CPU 温度采集已运行或无法创建共享数据");
    }
    auto* shared = static_cast<gauge::CpuTelemetryRecord*>(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0,
        sizeof(gauge::CpuTelemetryRecord)));
    if (!shared) { CloseHandle(mapping); if (!stop) CloseHandle(host); throw std::runtime_error("无法映射 CPU 温度数据"); }
    shared->magic = gauge::cpu_telemetry_magic;
    shared->process_id = GetCurrentProcessId();
    while (WaitForSingleObject(host, 0) == WAIT_TIMEOUT) {
        double value{};
        DWORD valid{};
        try { value = tctl_celsius(read_temperature_register(blob)); valid = 1; }
        catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
        InterlockedIncrement(&shared->sequence);
        MemoryBarrier();
        shared->timestamp_ms = GetTickCount64();
        shared->tctl_celsius = value;
        shared->valid = valid;
        MemoryBarrier();
        InterlockedIncrement(&shared->sequence);
        if (WaitForSingleObject(host, 1000) != WAIT_TIMEOUT) break;
    }
    UnmapViewOfFile(shared);
    CloseHandle(mapping);
    if (!stop) CloseHandle(host);
}
SERVICE_STATUS_HANDLE service_handle{};
HANDLE service_stop{};
void service_status(DWORD state, DWORD error = NO_ERROR) {
    SERVICE_STATUS status{}; status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState = state; status.dwWin32ExitCode = error;
    status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    SetServiceStatus(service_handle, &status);
}
void WINAPI service_control(DWORD control) {
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
        service_status(SERVICE_STOP_PENDING); SetEvent(service_stop);
    }
}
void WINAPI service_main(DWORD, wchar_t**) {
    service_handle = RegisterServiceCtrlHandlerW(L"GameGauge.Sensor", service_control);
    if (!service_handle) return;
    service_status(SERVICE_START_PENDING);
    service_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD error = NO_ERROR;
    try {
        if (!service_stop) throw std::runtime_error("无法创建停止事件");
        wchar_t executable[32768]{}; GetModuleFileNameW(nullptr, executable, 32768);
        cpu_id();
        const auto blob = verified_module(std::filesystem::path(executable).parent_path() / L".deps" / L"AMDFamily17-0.2.11.bin");
        service_status(SERVICE_RUNNING); serve(blob, 0, service_stop);
    } catch (...) { error = ERROR_SERVICE_SPECIFIC_ERROR; }
    if (service_stop) CloseHandle(service_stop);
    service_status(SERVICE_STOPPED, error);
}
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc == 2 && std::wstring_view(argv[1]) == L"--service") {
        SERVICE_TABLE_ENTRYW table[] = {{const_cast<wchar_t*>(L"GameGauge.Sensor"), service_main}, {nullptr, nullptr}};
        return StartServiceCtrlDispatcherW(table) ? 0 : static_cast<int>(GetLastError());
    }
    try {
        if (argc != 2 && (argc != 4 || std::wstring_view(argv[1]) != L"--serve"))
            throw std::runtime_error("用法：GameGauge.CpuProbe.exe [--serve <模块路径> <宿主 PID>] <官方模块路径>");
        const auto id = cpu_id();
        const auto blob = verified_module(argv[argc == 2 ? 1 : 2]);
        if (argc == 4) {
            const auto pid = std::stoul(argv[3]);
            if (!pid) throw std::runtime_error("宿主 PID 无效");
            serve(blob, static_cast<DWORD>(pid));
            return 0;
        }
        const auto raw = read_temperature_register(blob);
        const auto value = static_cast<unsigned>(raw);
        const double celsius = tctl_celsius(raw);
        std::cout << "{\"family\":" << id.family << ",\"model\":" << id.model
                  << ",\"register\":\"0x" << std::hex << value << std::dec
                  << "\",\"tctl_celsius\":" << celsius
                  << ",\"source\":\"PawnIO AMDFamily17 0.2.11, SMN 0x59800\"}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
