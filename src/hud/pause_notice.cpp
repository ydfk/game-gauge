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
        return;
    }
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(MonitorFromWindow(game, MONITOR_DEFAULTTONEAREST), &monitor)) {
        ShowWindow(window_, SW_HIDE);
        return;
    }
    const UINT dpi = GetDpiForWindow(game);
    const auto& work = monitor.rcWork;
    const int margin = MulDiv(24, dpi, 96);
    const SIZE size{std::min<LONG>(work.right - work.left - margin * 2, MulDiv(720, dpi, 96)),
        std::min<LONG>(work.bottom - work.top - margin * 2, MulDiv(210, dpi, 96))};
    if (!dpi || size.cx <= 0 || size.cy <= 0) {
        ShowWindow(window_, SW_HIDE);
        return;
    }
    SetWindowDisplayAffinity(window_, config.exclude_capture ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE);
    if (!renderer_) renderer_ = std::make_unique<Renderer>(window_);
    if (drawn_.cx != size.cx || drawn_.cy != size.cy || dpi_ != dpi) {
        renderer_->render_notice(dpi, size); drawn_ = size; dpi_ = dpi;
    }
    SetWindowPos(window_, HWND_TOPMOST, work.left + margin, work.top + margin,
        size.cx, size.cy, SWP_NOACTIVATE | SWP_SHOWWINDOW);
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
    if (message == WM_CLOSE) return 0;
    if (message == WM_LBUTTONUP) {
        RECT rect{}; GetClientRect(window, &rect);
        const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        rect.top = static_cast<LONG>(rect.bottom * .65f);
        if (!PtInRect(&rect, point)) return 0;
        self->dismissed_ = true; ShowWindow(window, SW_HIDE);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
}
