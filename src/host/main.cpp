#include "application.h"
#include "ipc_server.h"
#include "common/platform.h"
#include <shellapi.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int count{}; auto arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    try {
        bool settings = true, preview = false; uint32_t timed_exit{}, target{};
        for (int i = 1; i < count; ++i) {
            const std::wstring argument = arguments[i];
            if (argument == L"--data-dir" && i + 1 < count) gauge::set_data_dir(arguments[++i]);
            else if (argument == L"--background") settings = false;
            else if (argument == L"--preview") preview = true;
            else if (argument == L"--target-pid" && i + 1 < count) target = std::stoul(arguments[++i]);
            else if (argument == L"--exit-after-ms" && i + 1 < count) timed_exit = std::stoul(arguments[++i]);
        }
        LocalFree(arguments); arguments = nullptr;
        gauge::UniqueHandle mutex(CreateMutexW(nullptr, FALSE, (L"Local\\GameGauge-" + gauge::user_token()).c_str()));
        if (!mutex) throw std::runtime_error(gauge::error_text(GetLastError()));
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            try { gauge::ipc_request({{"command", "settings"}}); } catch (...) {}
            return 0;
        }
        std::string warning;
        auto config = gauge::load_config(gauge::data_dir() / L"config.json", warning);
        if (preview) config.preview = true;
        if (target) { config.target_pid = target; config.auto_target = false; }
        gauge::Application application(instance, config, settings, timed_exit);
        return application.run();
    } catch (const std::exception& error) {
        if (arguments) LocalFree(arguments);
        std::filesystem::create_directories(gauge::data_dir());
        MessageBoxW(nullptr, gauge::wide(error.what()).c_str(), L"游戏仪表启动失败", MB_OK | MB_ICONERROR);
        return 1;
    }
}

