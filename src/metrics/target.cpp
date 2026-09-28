#include "target.h"
#include "common/platform.h"
#include <dwmapi.h>
#include <algorithm>
#include <cwctype>

namespace gauge {
namespace {
Target inspect(HWND window) {
    Target target;
    DWORD pid{}; GetWindowThreadProcessId(window, &pid);
    if (!pid || pid == GetCurrentProcessId()) return target;
    UniqueHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process) return target;
    wchar_t path[32768]{}; DWORD count = 32768;
    if (!QueryFullProcessImageNameW(process.value, 0, path, &count)) return target;
    target.pid = pid; target.path = utf8(path);
    target.name = utf8(std::filesystem::path(path).filename().wstring());
    FILETIME started{}, exited{}, kernel{}, user{};
    if (GetProcessTimes(process.value, &started, &exited, &kernel, &user)) target.started = file_ticks(started);
    target.window = reinterpret_cast<uintptr_t>(window);
    DWORD foreground_pid{}; GetWindowThreadProcessId(GetForegroundWindow(), &foreground_pid);
    target.foreground = foreground_pid == pid;
    return target;
}
bool usable(HWND window) {
    if (!IsWindowVisible(window) || IsIconic(window) || GetWindow(window, GW_OWNER)) return false;
    BOOL cloaked{}; DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    RECT rect{}; GetClientRect(window, &rect);
    return !cloaked && rect.right >= 300 && rect.bottom >= 200;
}
bool ignored(const Target& target, const Config& config) {
    std::string name = target.name;
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
    const std::vector<std::string> system{"explorer.exe", "dwm.exe", "applicationframehost.exe", "searchhost.exe", "startmenuexperiencehost.exe", "gamegauge.settings.exe", "obs64.exe"};
    if (std::find(system.begin(), system.end(), name) != system.end()) return true;
    return std::any_of(config.ignored_processes.begin(), config.ignored_processes.end(), [&](std::string item) {
        std::transform(item.begin(), item.end(), item.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
        return item == name;
    });
}
}
std::vector<Target> enumerate_targets() {
    std::vector<Target> targets;
    EnumWindows([](HWND window, LPARAM data) -> BOOL {
        if (usable(window)) {
            auto target = inspect(window);
            if (target.pid && target.name != "GameGauge.Settings.exe")
                reinterpret_cast<std::vector<Target>*>(data)->push_back(std::move(target));
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&targets));
    return targets;
}
Target find_target(const Config& config, const Target& previous) {
    if (!config.auto_target && config.target_pid) {
        auto targets = enumerate_targets();
        auto target = std::find_if(targets.begin(), targets.end(), [&](const Target& item) { return item.pid == config.target_pid; });
        return target == targets.end() ? Target{} : *target;
    }
    auto keep_previous = [&]() -> Target {
        HWND old = reinterpret_cast<HWND>(previous.window);
        if (!old || !usable(old)) return {};
        auto retained = inspect(old);
        return retained.pid == previous.pid && retained.started == previous.started && !ignored(retained, config) ? retained : Target{};
    };
    HWND window = GetForegroundWindow();
    if (!usable(window)) return keep_previous();
    auto target = inspect(window);
    if (!target.pid || ignored(target, config)) return keep_previous();
    RECT rect{}; GetClientRect(window, &rect);
    MONITORINFO monitor{sizeof(monitor)}; GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
    const bool covers_screen = rect.right >= (monitor.rcMonitor.right - monitor.rcMonitor.left) * .85 &&
        rect.bottom >= (monitor.rcMonitor.bottom - monitor.rcMonitor.top) * .75;
    const auto style = GetWindowLongPtrW(window, GWL_STYLE);
    // 无边框大窗口仅作为候选，真实游戏帧是否存在由 PresentMon 确认。
    if (covers_screen && !(style & WS_THICKFRAME)) return target;
    if (previous.pid == target.pid && previous.started == target.started) return target;
    return keep_previous();
}
}
