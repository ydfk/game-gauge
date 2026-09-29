#include "target.h"
#include "common/platform.h"
#include <dwmapi.h>
#include <algorithm>
#include <cwctype>
#include <tlhelp32.h>
#include <unordered_map>

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
    if (!window || !IsWindowVisible(window) || IsIconic(window)) return false;
    BOOL cloaked{}; DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    RECT rect{}; GetClientRect(window, &rect);
    return !cloaked && rect.right >= 300 && rect.bottom >= 200;
}
bool ignored(const Target& target, const Config& config) {
    std::string name = target.name;
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
    const std::vector<std::string> system{"explorer.exe", "dwm.exe", "applicationframehost.exe", "searchhost.exe", "startmenuexperiencehost.exe", "gamegauge.exe", "gamegauge.settings.exe", "obs64.exe", "chrome.exe", "msedge.exe", "firefox.exe", "code.exe", "chatgpt.exe", "douyin.exe", "steam.exe", "steamwebhelper.exe", "epicgameslauncher.exe", "vlc.exe", "mpv.exe"};
    if (std::any_of(config.ignored_processes.begin(), config.ignored_processes.end(), [&](std::string item) {
        std::transform(item.begin(), item.end(), item.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
        return item == name;
    })) return true;
    for (const auto& known : config.known_games) if (_stricmp(known.c_str(), target.path.c_str()) == 0) return false;
    if (!config.auto_target && config.target_pid == target.pid) return false;
    if (std::find(system.begin(), system.end(), name) != system.end()) return true;
    if (name.find(" trainer") != std::string::npos) return true;
    const std::vector<std::string> utilities{"systemsettings.exe", "taskmgr.exe", "cmd.exe", "powershell.exe", "pwsh.exe", "windowsterminal.exe", "notepad.exe", "devenv.exe", "codex.exe", "discord.exe", "wechat.exe", "weixin.exe", "qq.exe", "msiexec.exe", "7zfm.exe", "nvidia overlay.exe", "nvidia app.exe", "redlauncher.exe", "redprelauncher.exe", "eadesktop.exe", "battle.net.exe", "ubisoftconnect.exe", "cheatengine-x86_64.exe", "wemod.exe"};
    return std::find(utilities.begin(), utilities.end(), name) != utilities.end();
}
std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
    return value;
}
bool game_candidate(const Target& target, HWND window, const Config& config) {
    const auto path = lower(target.path);
    for (const auto& known : config.known_games) if (lower(known) == path) return true;
    if (path.find("\\steamapps\\common\\") != std::string::npos || path.find("\\epic games\\") != std::string::npos ||
        path.find("\\games\\") != std::string::npos || path.find("\\xboxgames\\") != std::string::npos) return true;
    wchar_t class_name[256]{}; GetClassNameW(window, class_name, 256);
    const auto cls = lower(utf8(class_name));
    if (cls.find("unreal") != std::string::npos || cls.find("unity") != std::string::npos ||
        cls.find("northlight") != std::string::npos || cls == "sdl_app") return true;
    // 引擎模块用于识别普通窗口游戏；缓存按进程启动时间隔离 PID 复用。
    struct EngineCache { uint64_t started{}, checked{}; bool found{}; };
    static std::unordered_map<uint32_t, EngineCache> engines;
    if (engines.size() > 256) engines.clear();
    auto& cached = engines[target.pid];
    if (cached.started != target.started || (!cached.found && GetTickCount64() - cached.checked > 5000)) {
        cached = {target.started, GetTickCount64(), false};
        UniqueHandle modules(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, target.pid));
        MODULEENTRY32W item{sizeof(item)};
        if (modules && Module32FirstW(modules.value, &item)) do {
            const auto name = lower(utf8(item.szModule));
            if (name == "unityplayer.dll" || name == "steam_api64.dll" || name == "steam_api.dll" ||
                name == "cryengine.dll" || name == "gameassembly.dll") cached.found = true;
        } while (Module32NextW(modules.value, &item));
    }
    if (cached.found) return true;
    RECT rect{}; GetClientRect(window, &rect);
    MONITORINFO monitor{sizeof(monitor)}; GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
    return rect.right >= (monitor.rcMonitor.right - monitor.rcMonitor.left) * .85 &&
        rect.bottom >= (monitor.rcMonitor.bottom - monitor.rcMonitor.top) * .75 &&
        !(GetWindowLongPtrW(window, GWL_STYLE) & WS_THICKFRAME);
}
}
bool target_alive(const Target& target) {
    UniqueHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, target.pid));
    if (!process || WaitForSingleObject(process.value, 0) != WAIT_TIMEOUT) return false;
    FILETIME started{}, exited{}, kernel{}, user{};
    return GetProcessTimes(process.value, &started, &exited, &kernel, &user) && file_ticks(started) == target.started;
}
bool target_listed(const Target& target, const Config& config) { return !ignored(target, config); }
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
        if (target != targets.end()) return ignored(*target, config) ? Target{} : *target;
        if (previous.pid == config.target_pid && target_alive(previous) && !ignored(previous, config)) {
            auto retained = previous; retained.foreground = false; return retained;
        }
        return {};
    }
    auto keep_previous = [&]() -> Target {
        HWND old = reinterpret_cast<HWND>(previous.window);
        if (!previous.pid || !target_alive(previous) || ignored(previous, config)) return {};
        auto retained = old && IsWindow(old) ? inspect(old) : previous;
        if (retained.pid != previous.pid || retained.started != previous.started) { retained = previous; retained.window = 0; }
        DWORD foreground_pid{}; GetWindowThreadProcessId(GetForegroundWindow(), &foreground_pid);
        retained.foreground = foreground_pid == retained.pid;
        return retained;
    };
    HWND window = GetForegroundWindow();
    if (!usable(window)) return keep_previous();
    auto target = inspect(window);
    if (!target.pid || ignored(target, config)) return keep_previous();
    if (previous.pid == target.pid && previous.started == target.started) return target;
    if (game_candidate(target, window, config)) return target;
    return keep_previous();
}
}
