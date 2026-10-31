#include "components.h"
#include "platform.h"
#include <cmath>
#include <shlobj.h>

namespace gauge {
namespace {
struct ServiceHandle {
    SC_HANDLE value{};
    ~ServiceHandle() { if (value) CloseServiceHandle(value); }
};
bool file_exists(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error);
}
std::filesystem::path program_files() {
    PWSTR path{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &path))) return {};
    const std::filesystem::path result(path);
    CoTaskMemFree(path);
    return result;
}
Json service(const wchar_t* name, const char* id, const char* title) {
    Json result{{"id", id}, {"name", title}, {"state", "error"}, {"detail", "无法检查服务状态"}};
    ServiceHandle manager{OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)};
    if (!manager.value) return result;
    ServiceHandle handle{OpenServiceW(manager.value, name, SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG)};
    if (!handle.value) {
        const auto error = GetLastError();
        result["state"] = error == ERROR_SERVICE_DOES_NOT_EXIST ? "missing" : "error";
        result["detail"] = error == ERROR_SERVICE_DOES_NOT_EXIST ? "组件未安装，可修复" : "无法检查服务：" + error_text(error);
        return result;
    }
    SERVICE_STATUS_PROCESS status{}; DWORD bytes{};
    if (!QueryServiceStatusEx(handle.value, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&status), sizeof(status), &bytes)) return result;
    result["state"] = status.dwCurrentState == SERVICE_RUNNING ? "ready" : "stopped";
    result["detail"] = status.dwCurrentState == SERVICE_RUNNING ? "服务运行中" :
        status.dwCurrentState == SERVICE_START_PENDING ? "服务正在启动" : "服务未运行，可修复";
    // 服务注册记录仍在时，也检查它实际指向的可执行文件。
    QueryServiceConfigW(handle.value, nullptr, 0, &bytes);
    std::vector<BYTE> buffer(bytes);
    if (bytes && QueryServiceConfigW(handle.value, reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data()), bytes, &bytes)) {
        std::wstring path = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data())->lpBinaryPathName;
        if (!path.empty() && path.front() == L'"') path = path.substr(1, path.find(L'"', 1) - 1);
        else if (const auto end = path.find(L".exe"); end != std::wstring::npos) path.resize(end + 4);
        if (path.starts_with(L"\\??\\")) path.erase(0, 4);
        if (path.starts_with(L"\\SystemRoot\\")) {
            wchar_t windows[32768]{}; GetWindowsDirectoryW(windows, 32768);
            path = (std::filesystem::path(windows) / path.substr(12)).wstring();
        } else if (path.starts_with(L"System32\\")) {
            wchar_t windows[32768]{}; GetWindowsDirectoryW(windows, 32768);
            path = (std::filesystem::path(windows) / path).wstring();
        }
        wchar_t expanded[32768]{};
        const auto count = ExpandEnvironmentStringsW(path.c_str(), expanded, 32768);
        if (count && count <= 32768 && !file_exists(expanded)) {
            result["state"] = "missing"; result["detail"] = "服务文件缺失，可修复";
        } else if (count && count <= 32768 && std::string_view(id) == "sensor" &&
            !file_exists(std::filesystem::path(expanded).parent_path() / L".deps/AMDFamily17-0.2.11.bin")) {
            result["state"] = "missing"; result["detail"] = "温度采集模块缺失，请重新安装游戏仪表";
        }
    }
    return result;
}
}
bool component_repair_available() {
    const auto root = program_files() / L"GameGauge";
    if (_wcsicmp(root.c_str(), executable_dir().c_str()) != 0) return false;
    return file_exists(root / L"scripts/repair-components.ps1") &&
        file_exists(root / L"scripts/install-presentmon.ps1") && file_exists(root / L"GameGauge.CpuProbe.exe") &&
        file_exists(root / L".deps/AMDFamily17-0.2.11.bin") &&
        file_exists(root / L"components/PresentMon-2.6.0.msi") && file_exists(root / L"components/PawnIO_setup.exe");
}
Json inspect_components() {
    auto presentmon = service(L"PresentMonSharedService", "presentmon", "帧率采集");
    if (presentmon["state"] == "ready" && !file_exists(program_files() / L"Intel/PresentMonSharedService/PresentMonAPI2.dll") &&
        !file_exists(executable_dir() / L"PresentMonAPI2.dll")) {
        presentmon["state"] = "missing"; presentmon["detail"] = "PresentMon 接口文件缺失，可修复";
    }
    auto pawnio = service(L"PawnIO", "pawnio", "温度驱动");
    wchar_t location[32768]{}; DWORD size = sizeof(location);
    const auto error = RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\PawnIO",
        L"InstallLocation", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, location, &size);
    if (error != ERROR_SUCCESS || !file_exists(std::filesystem::path(location) / L"PawnIOLib.dll")) {
        pawnio["state"] = "missing"; pawnio["detail"] = "PawnIO 驱动或接口缺失，可修复";
    }
    auto sensor = service(L"GameGauge.Sensor", "sensor", "CPU 温度采集");
    return Json::array({presentmon, pawnio, sensor});
}
Json component_readings(Json components, const Snapshot& snapshot) {
    for (auto& item : components) {
        const auto id = item.value("id", std::string{});
        if (id != "presentmon" && !snapshot.hardware.cpu_vendor.empty() && snapshot.hardware.cpu_vendor != "AuthenticAMD") {
            item["state"] = "unsupported"; item["detail"] = "当前 CPU 不使用 AMD 温度组件"; continue;
        }
        if (item.value("state", std::string{}) != "ready" || id == "pawnio") continue;
        const auto& metric = id == "presentmon" ? snapshot.fps : snapshot.cpu_temperature;
        if (id == "presentmon" && (!snapshot.target.pid || snapshot.paused || !snapshot.target.foreground)) {
            item["detail"] = "服务就绪，等待前台游戏采样"; continue;
        }
        if (metric.state == State::valid && metric.value && std::isfinite(*metric.value)) {
            item["detail"] = id == "presentmon" ? "正在接收游戏帧" : "正在接收温度读数";
        } else {
            const bool known_failure = metric.reason.find("不可用") != std::string::npos || metric.reason.find("失败") != std::string::npos ||
                metric.reason.find("断开") != std::string::npos || metric.reason.find("异常") != std::string::npos ||
                metric.reason.find("无数据") != std::string::npos || metric.reason.find("无有效样本") != std::string::npos || metric.reason.find("缺少") != std::string::npos;
            item["state"] = metric.state == State::waiting && !known_failure ? "waiting" : "error";
            item["detail"] = metric.reason.empty() ? "服务运行中，尚无有效读数" : metric.reason;
        }
    }
    return components;
}
}
