#include "application.h"
#include "common/platform.h"
#include "metrics/target.h"
#include "common/history.h"
#include "version.h"
#include <commctrl.h>
#include <fstream>
#include <format>

namespace gauge {
namespace {
constexpr UINT tray_message = WM_APP + 1, action_message = WM_APP + 2;
enum Action : UINT { open_settings = 100, toggle_hud, toggle_pause, edit_hud, reset_stats, export_data, quit_app, save_position };
HICON make_icon() {
    HDC screen = GetDC(nullptr), dc = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, 32, 32), mask = CreateBitmap(32, 32, 1, 1, nullptr);
    auto old = SelectObject(dc, bitmap);
    RECT rectangle{0, 0, 32, 32};
    FillRect(dc, &rectangle, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    auto navy = CreateSolidBrush(RGB(16, 26, 39)); auto navy_pen = CreatePen(PS_SOLID, 1, RGB(16, 26, 39));
    auto old_brush = SelectObject(dc, navy), old_pen = SelectObject(dc, navy_pen);
    RoundRect(dc, 1, 1, 31, 31, 8, 8);
    auto mint_pen = CreatePen(PS_SOLID, 3, RGB(107, 227, 195)); SelectObject(dc, mint_pen);
    MoveToEx(dc, 5, 19, nullptr); LineTo(dc, 7, 14); LineTo(dc, 10, 10); LineTo(dc, 15, 7);
    LineTo(dc, 20, 8); LineTo(dc, 25, 12); LineTo(dc, 28, 18);
    MoveToEx(dc, 5, 24, nullptr); LineTo(dc, 10, 20); LineTo(dc, 14, 23); LineTo(dc, 18, 19); LineTo(dc, 26, 10);
    auto amber_brush = CreateSolidBrush(RGB(255, 183, 94)); SelectObject(dc, amber_brush);
    auto amber_pen = CreatePen(PS_SOLID, 1, RGB(255, 183, 94)); SelectObject(dc, amber_pen);
    Ellipse(dc, 25, 24, 29, 28);
    SelectObject(dc, old_brush); SelectObject(dc, old_pen);
    DeleteObject(navy); DeleteObject(navy_pen); DeleteObject(mint_pen); DeleteObject(amber_brush); DeleteObject(amber_pen);
    SelectObject(dc, old);
    HDC mask_dc = CreateCompatibleDC(screen); auto old_mask = SelectObject(mask_dc, mask);
    PatBlt(mask_dc, 0, 0, 32, 32, WHITENESS);
    auto opaque = CreateSolidBrush(RGB(0, 0, 0)); auto outline = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
    auto old_mask_brush = SelectObject(mask_dc, opaque), old_mask_pen = SelectObject(mask_dc, outline);
    RoundRect(mask_dc, 1, 1, 31, 31, 8, 8);
    SelectObject(mask_dc, old_mask_brush); SelectObject(mask_dc, old_mask_pen);
    DeleteObject(opaque); DeleteObject(outline); SelectObject(mask_dc, old_mask); DeleteDC(mask_dc);
    ICONINFO info{TRUE, 0, 0, mask, bitmap}; auto icon = CreateIconIndirect(&info);
    DeleteObject(bitmap); DeleteObject(mask); DeleteDC(dc); ReleaseDC(nullptr, screen); return icon;
}
}
Application::Application(HINSTANCE instance, Config config, bool show, uint32_t timed_exit) : instance_(instance), config_(std::move(config)),
    sampler_(config_), show_settings_(show), timed_exit_(timed_exit) {
    WNDCLASSW cls{}; cls.hInstance = instance; cls.lpfnWndProc = procedure; cls.lpszClassName = L"GameGauge.Host"; RegisterClassW(&cls);
    window_ = CreateWindowW(cls.lpszClassName, L"游戏仪表", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, instance, this);
    if (!window_) throw std::runtime_error(error_text(GetLastError()));
    icon_ = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(101), IMAGE_ICON, 32, 32, 0));
    if (!icon_) icon_ = make_icon();
    taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");
    overlay_ = std::make_unique<Overlay>(instance);
    updater_.configure(config_.check_updates, config_.auto_update);
    add_tray();
    const auto mods = MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT;
    if (!RegisterHotKey(window_, 1, mods, VK_F6) || !RegisterHotKey(window_, 2, mods, VK_F7) || !RegisterHotKey(window_, 3, mods, VK_F8))
        warning_ = "部分热键被其他软件占用，请使用托盘菜单";
    SetTimer(window_, 1, 250, nullptr);
    if (timed_exit_) SetTimer(window_, 2, timed_exit_, nullptr);
    ipc_ = std::make_unique<IpcServer>([this](const Json& command) { return request(command); });
}
Application::~Application() {
    if (auto settings = FindWindowW(L"GameGauge.Settings", nullptr)) PostMessageW(settings, WM_CLOSE, 0, 0);
    ipc_.reset(); overlay_.reset();
    for (int hotkey = 1; hotkey <= 3; ++hotkey) UnregisterHotKey(window_, hotkey);
    NOTIFYICONDATAW tray{sizeof(tray)}; tray.hWnd = window_; tray.uID = 1; Shell_NotifyIconW(NIM_DELETE, &tray);
    if (icon_) DestroyIcon(icon_);
    if (window_) DestroyWindow(window_);
}
Config Application::config() const { std::lock_guard lock(mutex_); return config_; }
Snapshot Application::snapshot() const { auto result = sampler_.snapshot(); result.obs_state = obs_.state(); return result; }
void Application::update_config(Config next) {
    save_config(data_dir() / L"config.json", next);
    { std::lock_guard lock(mutex_); config_ = next; }
    sampler_.configure(std::move(next));
    const auto saved = config(); updater_.configure(saved.check_updates, saved.auto_update);
}
void Application::add_tray() {
    NOTIFYICONDATAW tray{sizeof(tray)}; tray.hWnd = window_; tray.uID = 1;
    tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP; tray.uCallbackMessage = tray_message; tray.hIcon = icon_;
    wcscpy_s(tray.szTip, L"游戏仪表 · 右键打开监控菜单");
    if (!Shell_NotifyIconW(NIM_ADD, &tray)) {
        if (!Shell_NotifyIconW(NIM_MODIFY, &tray)) SetTimer(window_, 3, 2000, nullptr);
    } else KillTimer(window_, 3);
}
void Application::settings() {
    auto path = executable_dir() / L"GameGauge.Settings.exe";
    if (!std::filesystem::exists(path)) {
        MessageBoxW(window_, L"尚未构建设置窗口。请运行 scripts/build-core.ps1。", L"游戏仪表", MB_OK | MB_ICONINFORMATION); return;
    }
    const auto argument = L"--host-pid " + std::to_wstring(GetCurrentProcessId()) + L" --data-dir \"" + data_dir().wstring() + L"\"";
    ShellExecuteW(window_, L"open", path.c_str(), argument.c_str(), executable_dir().c_str(), SW_SHOWNORMAL);
}
void Application::menu() {
    auto current = config(); auto snapshot = sampler_.snapshot();
    auto menu = CreatePopupMenu();
    auto title = snapshot.target.pid ? L"正在监控：" + wide(snapshot.target.name) : L"等待游戏 / 可在设置中选择窗口";
    AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, title.c_str()); AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (current.enabled ? MF_CHECKED : 0), toggle_hud, L"显示监控条\tCtrl+Alt+Shift+F6");
    AppendMenuW(menu, MF_STRING | (current.paused ? MF_CHECKED : 0), toggle_pause, L"暂停采集");
    AppendMenuW(menu, MF_STRING, edit_hud, overlay_->editing() ? L"保存监控条位置" : L"编辑监控条位置\tCtrl+Alt+Shift+F7");
    AppendMenuW(menu, MF_STRING, reset_stats, L"重置本次统计\tCtrl+Alt+Shift+F8");
    AppendMenuW(menu, MF_STRING, export_data, L"导出当前会话 JSON");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, open_settings, L"打开设置"); AppendMenuW(menu, MF_STRING, quit_app, L"退出");
    POINT cursor{}; GetCursorPos(&cursor); SetForegroundWindow(window_);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, window_, nullptr); DestroyMenu(menu);
    PostMessageW(window_, WM_NULL, 0, 0);
}
void Application::export_session() {
    auto directory = data_dir() / L"exports"; std::filesystem::create_directories(directory);
    auto file = directory / (L"session-" + std::to_wstring(GetTickCount64()) + L".json");
    std::ofstream output(file, std::ios::binary); output << snapshot_json(sampler_.snapshot()).dump(2);
    if (!output) throw std::runtime_error("导出会话失败");
}
Json Application::request(const Json& command) {
    if (!command.is_object() || command.value("version", 1) != 1) throw std::runtime_error("不支持的 IPC 协议");
    const auto action = command.value("command", std::string("status"));
    if (action == "status") {
        std::lock_guard lock(mutex_);
        const auto snapshot = this->snapshot();
        Json preview = Json::array();
        for (const auto& item : hud_items(snapshot, config_)) preview.push_back({{"label", utf8(item.label)}, {"value", utf8(item.value)},
            {"group", utf8(item.group)}, {"color", {item.color.r, item.color.g, item.color.b}}});
        return {{"ok", true}, {"version", 1}, {"host_pid", GetCurrentProcessId()}, {"config", config_json(config_)}, {"snapshot", snapshot_json(snapshot)},
            {"hud_preview", preview}, {"app_version", GAMEGAUGE_VERSION}, {"update", updater_.status()},
            {"capture", {{"requested", config_.exclude_capture}, {"accepted", capture_requested_.load()}, {"error", capture_error_.load()}, {"verified", false}}},
            {"warning", warning_}, {"hud_error", hud_error_}};
    }
    if (action == "apply") { update_config(config_from_json(command.at("config"))); return {{"ok", true}}; }
    if (action == "check_update") { updater_.check(); return {{"ok", true}}; }
    if (action == "download_update") { updater_.download(); return {{"ok", true}}; }
    if (action == "install_update") { updater_.install(); return {{"ok", true}}; }
    if (action == "targets") {
        Json targets = Json::array();
        for (const auto& target : enumerate_targets()) targets.push_back({{"pid", target.pid}, {"name", target.name}, {"path", target.path}, {"foreground", target.foreground}});
        return {{"ok", true}, {"targets", targets}};
    }
    if (action == "history") {
        const auto rows = read_history(data_dir() / L"history");
        const auto offset = std::min(command.value("offset", size_t{}), rows.size());
        const auto count = std::clamp(command.value("limit", size_t{4}), size_t{1}, size_t{32});
        Json page = Json::array();
        for (size_t i = offset; i < std::min(rows.size(), offset + count); ++i) page.push_back(rows[i]);
        return {{"ok", true}, {"total", rows.size()}, {"history", page}};
    }
    if (action == "reset") sampler_.reset_statistics();
    else if (action == "rediscover") sampler_.refresh_hardware();
    else if (action == "settings") PostMessageW(window_, action_message, open_settings, 0);
    else if (action == "hide") PostMessageW(window_, action_message, toggle_hud, 0);
    else if (action == "edit") PostMessageW(window_, action_message, edit_hud, 0);
    else if (action == "export") PostMessageW(window_, action_message, export_data, 0);
    else if (action == "quit") PostMessageW(window_, action_message, quit_app, 0);
    else throw std::runtime_error("未知命令");
    return {{"ok", true}};
}
int Application::run() {
    if (show_settings_) PostMessageW(window_, action_message, open_settings, 0);
    MSG message{}; while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    return static_cast<int>(message.wParam);
}
LRESULT CALLBACK Application::procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto self = reinterpret_cast<Application*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) { self = static_cast<Application*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams); SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); }
    if (!self) return DefWindowProcW(window, message, wparam, lparam);
    try {
        if (message == self->taskbar_created_ && self->taskbar_created_) { self->add_tray(); return 0; }
        if (message == tray_message) { if (lparam == WM_RBUTTONUP) self->menu(); else if (lparam == WM_LBUTTONUP || lparam == WM_LBUTTONDBLCLK) self->settings(); return 0; }
        if (message == WM_TIMER) {
            if (wparam == 3) { self->add_tray(); return 0; }
            if (wparam == 2) { PostQuitMessage(0); return 0; }
            self->updater_.idle(self->sampler_.snapshot().target.pid != 0);
            if (auto edit_result = self->overlay_->take_edit_result()) {
                if (*edit_result) {
                    auto config = self->config();
                    config.anchor = 1;
                    config.margin_x = self->overlay_->dragged_x();
                    config.margin_y = self->overlay_->dragged_y();
                    self->update_config(config);
                }
                self->overlay_->edit(false);
            }
            self->overlay_->update(self->snapshot(), self->config());
            self->capture_requested_ = self->overlay_->capture_requested(); self->capture_error_ = self->overlay_->capture_error();
            { std::lock_guard lock(self->mutex_); self->hud_error_ = self->overlay_->error(); }
            return 0;
        }
        if (message == WM_DISPLAYCHANGE || message == WM_POWERBROADCAST) { self->sampler_.refresh_hardware(); self->sampler_.reset_statistics(); }
        if (message == WM_HOTKEY) { PostMessageW(window, action_message, wparam == 1 ? toggle_hud : wparam == 2 ? edit_hud : reset_stats, 0); return 0; }
        if (message == WM_COMMAND || message == action_message) {
            const auto action = LOWORD(wparam); auto config = self->config();
            switch (action) {
            case open_settings: self->settings(); break;
            case toggle_hud: config.enabled = !config.enabled; self->update_config(config); break;
            case toggle_pause: config.paused = !config.paused; self->update_config(config); break;
            case edit_hud:
                if (self->overlay_->editing()) { config.anchor = 1; config.margin_x = self->overlay_->dragged_x(); config.margin_y = self->overlay_->dragged_y(); self->update_config(config); }
                self->overlay_->edit(!self->overlay_->editing()); break;
            case reset_stats: self->sampler_.reset_statistics(); break;
            case export_data: self->export_session(); break;
            case quit_app: PostQuitMessage(0); break;
            }
            return 0;
        }
    } catch (const std::exception& error) { std::lock_guard lock(self->mutex_); self->warning_ = error.what(); }
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, wparam, lparam);
}
}
