#include "pause_notice.h"
#include <windowsx.h>
#include <algorithm>
#include <dwmapi.h>
#include <stdexcept>
namespace gauge {
PauseNotice::PauseNotice(HINSTANCE instance) {
    WNDCLASSW cls{}; cls.hInstance = instance; cls.lpszClassName = L"GameGauge.PauseNotice";
    cls.lpfnWndProc = procedure; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); RegisterClassW(&cls);
    window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
        cls.lpszClassName, L"OBS 录屏暂停", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, this);
    if (!window_) throw std::runtime_error("无法创建 OBS 暂停提醒");
}
PauseNotice::~PauseNotice() {
    if (hotkey_) UnregisterHotKey(window_, 1);
    renderer_.reset(); if (window_) DestroyWindow(window_);
}
void PauseNotice::update(const Snapshot& s, const Config& config) {
    const bool paused = s.obs_state == "paused";
    if (!paused || !config.obs_pause_notice || pid_ != s.target.pid) dismissed_ = false;
    if (paused && !paused_) dismissed_ = false;
    paused_ = paused; pid_ = s.target.pid;
    const auto game = reinterpret_cast<HWND>(s.target.window);
    DWORD pid{}; if (game) GetWindowThreadProcessId(game, &pid);
    BOOL cloaked{}; if (game) DwmGetWindowAttribute(game, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (!config.enabled || !config.obs_pause_notice || !paused || dismissed_ || !s.game_confirmed ||
        !s.target.foreground || !game || !IsWindow(game) || pid != s.target.pid || !IsWindowVisible(game) || IsIconic(game) || cloaked) {
        ShowWindow(window_, SW_HIDE);
        if (hotkey_) { UnregisterHotKey(window_, 1); hotkey_ = false; }
        hotkey_state_ = NoticeHotkeyState::inactive; retry_at_ = 0;
        return;
    }
    RECT bounds{}; GetClientRect(game, &bounds); POINT origin{}; ClientToScreen(game, &origin);
    const UINT dpi = GetDpiForWindow(game);
    const SIZE size{std::min<LONG>(bounds.right, MulDiv(720, dpi, 96)), std::min<LONG>(bounds.bottom, MulDiv(210, dpi, 96))};
    if (size.cx <= 0 || size.cy <= 0) {
        ShowWindow(window_, SW_HIDE);
        if (hotkey_) { UnregisterHotKey(window_, 1); hotkey_ = false; }
        hotkey_state_ = NoticeHotkeyState::inactive; retry_at_ = 0;
        return;
    }
    SetWindowDisplayAffinity(window_, config.exclude_capture ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE);
    if (!(active_hotkey_ == config.obs_pause_hotkey)) {
        if (hotkey_) UnregisterHotKey(window_, 1);
        hotkey_ = false; retry_at_ = 0; active_hotkey_ = config.obs_pause_hotkey;
    }
    if (active_hotkey_.key && !hotkey_ && GetTickCount64() >= retry_at_) {
        hotkey_ = RegisterHotKey(window_, 1, active_hotkey_.modifiers | MOD_NOREPEAT, active_hotkey_.key) != FALSE;
        retry_at_ = GetTickCount64() + 5000;
    }
    hotkey_state_ = !active_hotkey_.key ? NoticeHotkeyState::disabled :
        hotkey_ ? NoticeHotkeyState::registered : NoticeHotkeyState::conflict;
    const auto hint = hotkey_ ? L"点击或 " + notice_hotkey_name(active_hotkey_) + L" 关闭本次提醒" :
        active_hotkey_.key ? L"快捷键冲突 · 点击关闭本次提醒" : L"点击关闭本次提醒";
    if (!renderer_) renderer_ = std::make_unique<Renderer>(window_);
    if (drawn_.cx != size.cx || drawn_.cy != size.cy || dpi_ != dpi || drawn_hint_ != hint) {
        renderer_->render_notice(dpi, size, hint); drawn_ = size; dpi_ = dpi; drawn_hint_ = hint;
    }
    SetWindowPos(window_, HWND_TOPMOST, origin.x + (bounds.right - size.cx) / 2,
        origin.y + (bounds.bottom - size.cy) / 2, size.cx, size.cy, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}
LRESULT CALLBACK PauseNotice::procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto self = reinterpret_cast<PauseNotice*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<PauseNotice*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(window, message, wparam, lparam);
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT) { PAINTSTRUCT ps{}; BeginPaint(window, &ps); EndPaint(window, &ps); return 0; }
    if ((message == WM_HOTKEY && wparam == 1 && self->hotkey_) || message == WM_CLOSE || message == WM_LBUTTONUP) {
        if (message == WM_LBUTTONUP) {
            RECT rect{}; GetClientRect(window, &rect);
            if (GET_Y_LPARAM(lparam) < rect.bottom * .65f) return 0;
        }
        self->dismissed_ = true; ShowWindow(window, SW_HIDE);
        if (self->hotkey_) { UnregisterHotKey(window, 1); self->hotkey_ = false; }
        self->hotkey_state_ = NoticeHotkeyState::inactive;
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
}
