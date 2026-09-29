#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include "version.h"

namespace {
HWND window{}, heading{}, status{}, progress{}, finish{}, logs{};
HFONT title_font{}, body_font{};
std::filesystem::path directory;
bool running = true;
DWORD outcome = 1;
void extract(int id, const std::filesystem::path& path) {
    auto resource = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!resource) throw std::runtime_error("Missing installer resource");
    auto data = LockResource(LoadResource(nullptr, resource));
    std::ofstream file(path, std::ios::binary);
    file.write(static_cast<const char*>(data), SizeofResource(nullptr, resource));
    if (!file) throw std::runtime_error("Cannot write installer resource");
}
void install() {
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
            throw std::runtime_error("Cannot start installation");
        WaitForSingleObject(process.hProcess, INFINITE);
        GetExitCodeProcess(process.hProcess, &result); CloseHandle(process.hProcess); CloseHandle(process.hThread);
    } catch (...) {
        std::ofstream log(directory / L"install.log", std::ios::app);
        log << "Unable to extract payload or start installation.\n";
    }
    PostMessageW(window, WM_APP + 1, result, 0);
}
LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_TIMER) {
        std::ifstream stream(directory / L"progress.txt"); int stage{};
        if (stream >> stage) {
            SendMessageW(progress, PBM_SETPOS, stage, 0);
            const auto text = stage >= 90 ? L"正在完成安装并启动游戏仪表…" : stage >= 75 ? L"正在创建快捷方式和卸载入口…" :
                stage >= 60 ? L"正在配置温度采集服务…" : stage >= 40 ? L"正在安装所需采集组件…" : stage >= 25 ? L"正在更新程序文件…" : L"正在解压并准备安装…";
            SetWindowTextW(status, text);
        }
        return 0;
    }
    if (message == WM_APP + 1) {
        running = false; outcome = static_cast<DWORD>(wparam); KillTimer(hwnd, 1);
        SetWindowTextW(heading, outcome ? L"安装未完成" : L"安装完成");
        SetWindowTextW(status, outcome ? L"安装遇到问题。可打开日志查看详细原因，再重新安装。" : L"游戏仪表已启动。桌面和开始菜单均可打开，开始游戏后自动监控。");
        SendMessageW(progress, PBM_SETPOS, outcome ? 0 : 100, 0);
        SetWindowTextW(finish, outcome ? L"关闭" : L"完成"); EnableWindow(finish, TRUE);
        ShowWindow(logs, outcome ? SW_SHOW : SW_HIDE);
        return 0;
    }
    if (message == WM_COMMAND) {
        if (LOWORD(wparam) == 1 && !running) DestroyWindow(hwnd);
        if (LOWORD(wparam) == 2) {
            const auto file = directory / L"install.log";
            ShellExecuteW(hwnd, L"open", file.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        return 0;
    }
    // 写入系统文件期间不允许关闭，以免留下半安装状态。
    if (message == WM_CLOSE && running) return 0;
    if (message == WM_DESTROY) { PostQuitMessage(static_cast<int>(outcome)); return 0; }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // 已完成的旧安装窗口不应挡住下一次更新；进行中的安装仍保持互斥。
    if (auto previous = FindWindowW(L"GameGauge.Installer", nullptr)) {
        if (IsWindowEnabled(GetDlgItem(previous, 1))) {
            SendMessageW(previous, WM_CLOSE, 0, 0);
            for (int i = 0; i < 30 && IsWindow(previous); ++i) Sleep(50);
        }
    }
    const auto installation = CreateMutexW(nullptr, FALSE, L"Global\\GameGauge.Setup");
    if (!installation) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (auto existing = FindWindowW(L"GameGauge.Installer", nullptr)) { ShowWindow(existing, SW_RESTORE); SetForegroundWindow(existing); }
        CloseHandle(installation); return 0;
    }
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_PROGRESS_CLASS}; InitCommonControlsEx(&controls);
    wchar_t temporary[MAX_PATH]{}; GetTempPathW(MAX_PATH, temporary);
    directory = std::filesystem::path(temporary) / (L"GameGauge-setup-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    WNDCLASSW cls{}; cls.hInstance = instance; cls.lpfnWndProc = procedure; cls.lpszClassName = L"GameGauge.Installer";
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1); RegisterClassW(&cls);
    const UINT dpi = GetDpiForSystem(); const auto scale = [dpi](int n) { return MulDiv(n, dpi, 96); };
    RECT area{0, 0, scale(620), scale(320)}; AdjustWindowRectExForDpi(&area, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, 0, dpi);
    window = CreateWindowW(cls.lpszClassName, L"游戏仪表 · 安装", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, area.right-area.left, area.bottom-area.top, nullptr, nullptr, instance, nullptr);
    if (!window) { CloseHandle(installation); return 1; }
    title_font = CreateFontW(-scale(25),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
    body_font = CreateFontW(-scale(14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
    const auto control = [&](const wchar_t* type, const wchar_t* text, int x, int y, int w, int h, int id = 0, DWORD style = 0) {
        auto handle = CreateWindowW(type,text,WS_CHILD | WS_VISIBLE | style,scale(x),scale(y),scale(w),scale(h),window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),instance,nullptr);
        SendMessageW(handle,WM_SETFONT,reinterpret_cast<WPARAM>(body_font),TRUE); return handle;
    };
    heading = control(L"STATIC", L"正在安装游戏仪表", 32,30,550,40); SendMessageW(heading,WM_SETFONT,reinterpret_cast<WPARAM>(title_font),TRUE);
    std::string version = GAMEGAUGE_VERSION; const std::wstring version_text(version.begin(), version.end());
    control(L"STATIC", (L"GameGauge " + version_text + L"  ·  Windows x64").c_str(),32,82,550,24);
    control(L"STATIC",L"安装到 Program Files · 保留已有配置和游戏记录",32,119,550,24);
    progress = control(PROGRESS_CLASSW,L"",32,161,556,18);
    status = control(L"STATIC",L"正在准备安装…",32,195,556,46);
    logs = control(L"BUTTON",L"打开日志",32,264,108,34,2,WS_TABSTOP); ShowWindow(logs,SW_HIDE);
    finish = control(L"BUTTON",L"正在安装",480,264,108,34,1,WS_TABSTOP); EnableWindow(finish,FALSE);
    ShowWindow(window,SW_SHOWNORMAL); UpdateWindow(window); SetTimer(window,1,300,nullptr);
    std::jthread worker(install);
    MSG message{}; while (GetMessageW(&message,nullptr,0,0)>0) { if (!IsDialogMessageW(window,&message)) { TranslateMessage(&message); DispatchMessageW(&message); } }
    worker.join(); DeleteObject(title_font); DeleteObject(body_font); CloseHandle(installation);
    return static_cast<int>(message.wParam);
}
