#include "window.h"
#include "common/platform.h"
#include <shellapi.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    try {
        if (auto existing = FindWindowW(L"GameGauge.Settings", nullptr)) {
            ShowWindow(existing, SW_RESTORE); SetForegroundWindow(existing);
            CoUninitialize(); return 0;
        }
        gauge::SettingsWindow window(instance);
        int count{}; auto arguments = CommandLineToArgvW(GetCommandLineW(), &count);
        wchar_t snapshot[32768]{};
        const auto snapshot_length = GetEnvironmentVariableW(L"GAMEGAUGE_SETTINGS_SNAPSHOT", snapshot, 32768);
        wchar_t page_text[16]{};
        GetEnvironmentVariableW(L"GAMEGAUGE_SETTINGS_PAGE", page_text, 16);
        const int page = _wtoi(page_text);
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
