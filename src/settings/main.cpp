#include "window.h"
#include "common/platform.h"
#include "host/ipc_server.h"
#include <shellapi.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    try {
        if (auto existing = FindWindowW(L"GameGauge.Settings", nullptr)) {
            ShowWindow(existing, SW_RESTORE); SetForegroundWindow(existing);
            CoUninitialize(); return 0;
        }
        int count{}; auto arguments = CommandLineToArgvW(GetCommandLineW(), &count);
        wchar_t snapshot[32768]{};
        const auto snapshot_length = GetEnvironmentVariableW(L"GAMEGAUGE_SETTINGS_SNAPSHOT", snapshot, 32768);
        wchar_t page_text[16]{};
        GetEnvironmentVariableW(L"GAMEGAUGE_SETTINGS_PAGE", page_text, 16);
        const int page = _wtoi(page_text);
        const bool capture = snapshot_length || (count >= 3 && std::wstring_view(arguments[1]) == L"--snapshot");
        DWORD host_pid{};
        for (int i = 1; i + 1 < count; ++i) {
            if (std::wstring_view(arguments[i]) == L"--host-pid") host_pid = std::stoul(arguments[++i]);
        }
        if (!capture) {
            try {
                const auto status = gauge::ipc_request({{"command", "status"}});
                if (!host_pid) host_pid = status.at("host_pid").get<DWORD>();
            }
            catch (...) {
                if (host_pid) { LocalFree(arguments); CoUninitialize(); return 0; }
                LocalFree(arguments);
                const auto host = gauge::executable_dir() / L"GameGauge.exe";
                if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", host.c_str(), nullptr, gauge::executable_dir().c_str(), SW_SHOWNORMAL)) <= 32)
                    throw std::runtime_error("无法启动游戏仪表主程序");
                CoUninitialize(); return 0;
            }
        }
        gauge::SettingsWindow window(instance, host_pid);
        if (snapshot_length && snapshot_length < 32768) {
            LocalFree(arguments);
            window.render_to_png(snapshot, page); CoUninitialize(); return 0;
        }
        if (count >= 3 && std::wstring_view(arguments[1]) == L"--snapshot") {
            const std::wstring path = arguments[2]; LocalFree(arguments);
            window.render_to_png(path, page); CoUninitialize(); return 0;
        }
        LocalFree(arguments);
        const int result = window.run();
        CoUninitialize();
        return result;
    } catch (const std::exception& error) {
        if (GetEnvironmentVariableW(L"GAMEGAUGE_SETTINGS_SNAPSHOT", nullptr, 0)) { CoUninitialize(); return 1; }
        MessageBoxW(nullptr, gauge::wide(error.what()).c_str(), L"游戏仪表设置", MB_OK | MB_ICONERROR);
        CoUninitialize();
        return 1;
    }
}
