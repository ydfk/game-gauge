#include "overlay.h"
#include "common/platform.h"
#include <windowsx.h>
#include <sstream>
#include <algorithm>

namespace gauge {
Overlay::Overlay(HINSTANCE instance) {
    WNDCLASSW cls{}; cls.hInstance = instance; cls.lpszClassName = L"GameGauge.Overlay";
    cls.lpfnWndProc = procedure; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); RegisterClassW(&cls);
    window_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_NOREDIRECTIONBITMAP,
        cls.lpszClassName, L"游戏仪表 · 顶部监控", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, this);
    if (!window_) throw std::runtime_error(error_text(GetLastError()));
    renderer_ = std::make_unique<Renderer>(window_);
}
Overlay::~Overlay() { renderer_.reset(); if (window_) DestroyWindow(window_); }
void Overlay::edit(bool enabled) {
    editing_ = enabled;
    edit_placed_ = false;
    edit_result_.reset();
    auto style = GetWindowLongPtrW(window_, GWL_EXSTYLE);
    if (enabled) style &= ~(WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
    else style |= WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
    SetWindowLongPtrW(window_, GWL_EXSTYLE, style);
    last_draw_.clear();
    if (enabled) { ShowWindow(window_, SW_SHOW); SetForegroundWindow(window_); }
}
int Overlay::dragged_x() const {
    RECT rect{}; GetWindowRect(window_, &rect);
    return std::max(0, MulDiv(rect.left - bounds_.left, 96, GetDpiForWindow(window_)));
}
int Overlay::dragged_y() const {
    RECT rect{}; GetWindowRect(window_, &rect);
    return std::max(0, MulDiv(rect.top - bounds_.top, 96, GetDpiForWindow(window_)));
}
void Overlay::update(const Snapshot& snapshot, const Config& config) {
    if (affinity_enabled_ != config.exclude_capture || (!capture_requested_ && !capture_error_)) {
        affinity_enabled_ = config.exclude_capture;
        capture_requested_ = SetWindowDisplayAffinity(window_, config.exclude_capture ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE) != FALSE;
        capture_error_ = capture_requested_ ? 0 : GetLastError();
    }
    const auto target = reinterpret_cast<HWND>(snapshot.target.window);
    DWORD target_pid{};
    if (target) GetWindowThreadProcessId(target, &target_pid);
    const bool real_target = target && IsWindow(target) && target_pid == snapshot.target.pid &&
        !IsIconic(target) && snapshot.game_confirmed;
    // 编辑也必须依附仍然存在的游戏，退出游戏后绝不回退到桌面。
    if (!real_target || (!config.enabled && !editing_) || (!snapshot.target.foreground && !editing_)) {
        if (!real_target && editing_) edit(false);
        ShowWindow(window_, SW_HIDE); return;
    }
    RECT bounds{};
    UINT dpi = 96;
    if (target && IsWindow(target)) {
        GetClientRect(target, &bounds);
        POINT origin{}; ClientToScreen(target, &origin); OffsetRect(&bounds, origin.x, origin.y);
        dpi = GetDpiForWindow(target);
    } else {
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromPoint(POINT{}, MONITOR_DEFAULTTOPRIMARY), &monitor);
        bounds = monitor.rcWork;
        dpi = GetDpiForWindow(window_);
    }
    bounds_ = bounds;
    try {
        if (!renderer_) renderer_ = std::make_unique<Renderer>(window_);
        auto size = renderer_->measure(snapshot, config, dpi);
        // 初版以窗口边界裁剪，保证长条不会越到另一块屏幕。
        size.cx = std::min(size.cx, bounds.right - bounds.left);
        const int x_margin = MulDiv(config.margin_x, dpi, 96);
        const int y_margin = MulDiv(config.margin_y, dpi, 96);
        int x = bounds.left + (bounds.right - bounds.left - size.cx) / 2;
        if (config.anchor == 1) x = bounds.left + x_margin;
        else if (config.anchor == 2) x = bounds.right - size.cx - x_margin;
        int y = config.anchor == 3 ? bounds.bottom - size.cy - y_margin : bounds.top + y_margin;
        x = std::clamp<LONG>(x, bounds.left, std::max(bounds.left, bounds.right - size.cx));
        y = std::clamp<LONG>(y, bounds.top, std::max(bounds.top, bounds.bottom - size.cy));
        if (!editing_ || !edit_placed_) {
            SetWindowPos(window_, HWND_TOPMOST, x, y, size.cx, size.cy, SWP_NOACTIVATE);
            if (editing_) edit_placed_ = true;
        } else {
            RECT current{}; GetWindowRect(window_, &current);
            if (current.right - current.left != size.cx || current.bottom - current.top != size.cy)
                SetWindowPos(window_, HWND_TOPMOST, current.left, current.top, size.cx, size.cy, SWP_NOACTIVATE);
        }
        ShowWindow(window_, SW_SHOWNOACTIVATE);
        std::wostringstream key;
        key << config.font_size << config.opacity << config.graph << dpi << size.cx << size.cy;
        for (const auto& item : hud_items(snapshot, config)) key << item.label << item.value;
        if (config.graph) for (double point : snapshot.recent_frames) key << point;
        const auto signature = utf8(key.str());
        if (signature != last_draw_) { renderer_->render(snapshot, config, dpi, size); last_draw_ = signature; }
        error_.clear();
    } catch (const std::exception& exception) {
        error_ = exception.what(); renderer_.reset(); ShowWindow(window_, SW_HIDE);
    }
}
LRESULT CALLBACK Overlay::procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto self = reinterpret_cast<Overlay*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<Overlay*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(window, message, wparam, lparam);
    switch (message) {
    case WM_NCHITTEST: return self->editing_ ? HTCAPTION : HTTRANSPARENT;
    case WM_MOUSEACTIVATE: return self->editing_ ? MA_ACTIVATE : MA_NOACTIVATE;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT paint{}; BeginPaint(window, &paint); EndPaint(window, &paint); return 0; }
    case WM_DPICHANGED: self->last_draw_.clear(); return 0;
    case WM_KEYDOWN:
        if (self->editing_ && (wparam == VK_ESCAPE || wparam == VK_RETURN)) {
            self->edit_result_ = wparam == VK_RETURN;
            return 0;
        }
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
}
