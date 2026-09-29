#include <windows.h>
#include <filesystem>
#include <fstream>
#include <string>

namespace {
void extract(int id, const std::filesystem::path& path) {
    auto resource = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!resource) throw std::runtime_error("Missing installer resource");
    auto data = LockResource(LoadResource(nullptr, resource));
    std::ofstream file(path, std::ios::binary);
    file.write(static_cast<const char*>(data), SizeofResource(nullptr, resource));
    if (!file) throw std::runtime_error("Cannot write installer resource");
}
}
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const auto installation = CreateMutexW(nullptr, FALSE, L"Global\\GameGauge.Setup");
    if (!installation) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(installation);
        MessageBoxW(nullptr, L"另一个游戏仪表安装程序仍在运行，请先完成或关闭它。", L"安装游戏仪表", MB_OK | MB_ICONINFORMATION);
        return 1;
    }
    if (MessageBoxW(nullptr, L"安装游戏仪表及所需的帧率和温度采集组件？\n安装完成后会自动启动，并随 Windows 登录运行。", L"安装游戏仪表", MB_OKCANCEL | MB_ICONINFORMATION) != IDOK) return 0;
    try {
        wchar_t temporary[MAX_PATH]{}, windows[MAX_PATH]{};
        GetTempPathW(MAX_PATH, temporary); GetWindowsDirectoryW(windows, MAX_PATH);
        const auto directory = std::filesystem::path(temporary) / (L"GameGauge-setup-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        std::filesystem::create_directory(directory);
        extract(101, directory / L"payload.zip"); extract(102, directory / L"install.ps1");
        const auto powershell = std::filesystem::path(windows) / L"System32/WindowsPowerShell/v1.0/powershell.exe";
        std::wstring command = L"\"" + powershell.wstring() + L"\" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"" + (directory / L"install.ps1").wstring() + L"\"";
        STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
        // 从 PowerShell 7 启动安装器时也使用 Windows PowerShell 自己的系统模块目录。
        SetEnvironmentVariableW(L"PSModulePath", nullptr);
        if (!CreateProcessW(powershell.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, directory.c_str(), &startup, &process))
            throw std::runtime_error("Cannot start installation");
        WaitForSingleObject(process.hProcess, INFINITE);
        DWORD result{}; GetExitCodeProcess(process.hProcess, &result); CloseHandle(process.hProcess); CloseHandle(process.hThread);
        const auto message = result == 0 ? L"安装完成。游戏仪表已启动，开始游戏后会自动显示监控。" : L"安装未完成。请查看日志：" + (directory / L"install.log").wstring();
        MessageBoxW(nullptr, message.c_str(), L"游戏仪表", MB_OK | (result ? MB_ICONERROR : MB_ICONINFORMATION));
        return static_cast<int>(result);
    } catch (...) { MessageBoxW(nullptr, L"无法解压或启动安装，请检查临时目录是否可写。", L"游戏仪表", MB_OK | MB_ICONERROR); return 1; }
}
