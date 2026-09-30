#include "window.h"
#include <shellapi.h>
#include <fstream>
#include <thread>

namespace {
void extract(int id, const std::filesystem::path& path) {
    auto resource = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!resource) throw std::runtime_error("安装包缺少内置文件");
    auto data = LockResource(LoadResource(nullptr, resource));
    std::ofstream file(path, std::ios::binary);
    file.write(static_cast<const char*>(data), SizeofResource(nullptr, resource));
    if (!file) throw std::runtime_error("无法写入临时安装文件");
}
void install(HWND window, const std::filesystem::path& directory) {
    DWORD result = 1;
    try {
        std::filesystem::create_directory(directory);
        extract(101, directory / L"payload.zip"); extract(102, directory / L"install.ps1");
        wchar_t windows[MAX_PATH]{}; GetWindowsDirectoryW(windows, MAX_PATH);
        const auto powershell = std::filesystem::path(windows) / L"System32/WindowsPowerShell/v1.0/powershell.exe";
        std::wstring command = L"\"" + powershell.wstring() + L"\" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"" + (directory / L"install.ps1").wstring() + L"\"";
        STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
        // Windows PowerShell 使用自己的系统模块，避免继承 PowerShell 7 的路径。
        SetEnvironmentVariableW(L"PSModulePath", nullptr);
        if (!CreateProcessW(powershell.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, directory.c_str(), &startup, &process))
            throw std::runtime_error("无法启动安装过程，Windows 错误：" + std::to_string(GetLastError()));
        WaitForSingleObject(process.hProcess, INFINITE);
        GetExitCodeProcess(process.hProcess, &result); CloseHandle(process.hProcess); CloseHandle(process.hThread);
    } catch (const std::exception& error) {
        std::ofstream log(directory / L"install.log", std::ios::app); log << error.what() << '\n';
    }
    PostMessageW(window, gauge::SetupWindow::completed_message, result, 0);
}
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    // 兼容旧安装器按钮和新版完成标记；正在安装时仍保持互斥。
    if (auto previous = FindWindowW(L"GameGauge.Installer", nullptr)) {
        if (GetPropW(previous, L"GameGauge.Setup.Completed") || IsWindowEnabled(GetDlgItem(previous, 1))) {
            SendMessageW(previous, WM_CLOSE, 0, 0);
            for (int i = 0; i < 30 && IsWindow(previous); ++i) Sleep(50);
        }
    }
    const auto installation = CreateMutexW(nullptr, FALSE, L"Global\\GameGauge.Setup");
    if (!installation) { CoUninitialize(); return 1; }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (auto existing = FindWindowW(L"GameGauge.Installer", nullptr)) { ShowWindowAsync(existing, SW_SHOW); SetForegroundWindow(existing); }
        CloseHandle(installation); CoUninitialize(); return 0;
    }
    int result = 1;
    try {
        wchar_t temporary[MAX_PATH]{}; GetTempPathW(MAX_PATH, temporary);
        const auto directory = std::filesystem::path(temporary) / (L"GameGauge-setup-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        gauge::SetupWindow window(instance, [&] {
            const auto file = directory / L"install.log";
            ShellExecuteW(nullptr, L"open", file.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }, [&] { std::ifstream stream(directory / L"progress.txt"); int stage{}; stream >> stage; return stage; });
        std::jthread worker([&] { install(window.handle(), directory); });
        result = window.run(); worker.join();
    } catch (...) {
        MessageBoxW(nullptr, L"无法打开安装界面，请重新运行安装包。", L"游戏仪表", MB_OK | MB_ICONERROR);
    }
    CloseHandle(installation); CoUninitialize(); return result;
}
