#include "window.h"
#include <shellapi.h>
#include <fstream>

// 独立普通权限预览程序只调用界面，不链接安装执行入口和内置安装资源。
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int result = 1;
    try {
        int count{}; auto arguments = CommandLineToArgvW(GetCommandLineW(), &count);
        std::filesystem::path snapshot, action_log;
        std::wstring state = L"installing";
        UINT dpi = 96; int progress = 60; bool lock_close = false;
        for (int i = 1; i < count; ++i) {
            const std::wstring_view argument = arguments[i];
            if (argument == L"--lock-close") lock_close = true;
            else if (i + 1 < count) {
                if (argument == L"--snapshot") snapshot = arguments[++i];
                else if (argument == L"--action-log") action_log = arguments[++i];
                else if (argument == L"--state") state = arguments[++i];
                else if (argument == L"--progress") progress = _wtoi(arguments[++i]);
                else if (argument == L"--dpi") dpi = static_cast<UINT>(_wtoi(arguments[++i]));
            }
        }
        LocalFree(arguments);
        gauge::SetupWindow window(instance, [&] {
            if (!action_log.empty()) { std::ofstream stream(action_log, std::ios::app); stream << "logs\n"; }
        }, {}, true, !lock_close);
        window.progress(progress);
        if (state == L"success") window.complete(0);
        else if (state == L"failure") window.complete(1);
        if (!snapshot.empty()) { window.snapshot(snapshot, dpi); result = 0; }
        else result = window.run();
    } catch (...) { result = 1; }
    CoUninitialize(); return result;
}
