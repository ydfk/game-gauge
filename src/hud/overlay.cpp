#include "overlay.h"
#include "common/platform.h"
#include "common/oled_shift.h"
#include <windowsx.h>
#include <dwmapi.h>
#include <sstream>
#include <algorithm>
#include <cmath>

namespace gauge {
namespace {
constexpr UINT_PTR drag_timer = 1;
constexpr UINT toggle_position_lock = 1;
}
Overlay::Overlay(HINSTANCE instance, std::function<void()> resume_obs) : resume_obs_(std::move(resume_obs)) {
    WNDCLASSW cls{}; cls.hInstance = instance; cls.lpszClassName = L"GameGauge.Overlay";
    cls.lpfnWndProc = procedure; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); RegisterClassW(&cls);
    window_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_NOREDIRECTIONBITMAP,
        cls.lpszClassName, L"游戏仪表 · 顶部监控 · 可拖动", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, this);
    if (!window_) throw std::runtime_error(error_text(GetLastError()));
    renderer_ = std::make_unique<Renderer>(window_);
    pause_notice_ = std::make_unique<PauseNotice>(instance, resume_obs_);
}
Overlay::~Overlay() { end_drag(); renderer_.reset(); if (window_) DestroyWindow(window_); }
void Overlay::edit(bool enabled) {
    end_drag();
    editing_ = enabled;
    edit_placed_ = false;
    edit_result_.reset();
    update_input_style();
    last_draw_.clear();
    if (enabled) { ShowWindow(window_, SW_SHOW); SetForegroundWindow(window_); }
}
void Overlay::set_foreground_target(bool foreground) {
    if (foreground_target_ == foreground) return;
    foreground_target_ = foreground;
    if (!foreground) end_drag();
    update_input_style();
}
void Overlay::update_input_style() {
    auto style = GetWindowLongPtrW(window_, GWL_EXSTYLE) & ~(WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
    if (!editing_) style |= WS_EX_NOACTIVATE;
    if (!editing_ && !foreground_target_) style |= WS_EX_TRANSPARENT;
    SetWindowLongPtrW(window_, GWL_EXSTYLE, style);
}
POINT Overlay::position() const {
    RECT rect{};
    GetWindowRect(window_, &rect);
    return {std::max(0, MulDiv(rect.left - bounds_.left, 96, target_dpi_)),
        std::max(0, MulDiv(rect.top - bounds_.top, 96, target_dpi_))};
}
bool Overlay::on_position_badge(POINT point) const {
    return point.x >= 0 && point.x < badge_size_.cx && point.y >= 0 && point.y < badge_size_.cy;
}
void Overlay::show_position_menu(POINT point) {
    if (menu_open_ || !foreground_target_ || editing_ || drag_) return;
    const auto menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, toggle_position_lock, locked_ ? L"解锁监控条位置" : L"锁定监控条位置");
    menu_open_ = true;
    const auto action = TrackPopupMenuEx(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
        point.x, point.y, window_, nullptr);
    menu_open_ = false;
    DestroyMenu(menu);
    if (action == toggle_position_lock) lock_change_ = !locked_;
}
void Overlay::begin_drag(POINT grip) {
    if (!foreground_target_ || locked_ || editing_ || drag_) return;
    RECT rect{};
    if (!GetWindowRect(window_, &rect)) return;
    drag_.emplace(Drag{grip, {rect.left, rect.top}});
    SetCapture(window_);
    SetTimer(window_, drag_timer, 16, nullptr);
}
void Overlay::move_drag(POINT cursor) {
    auto& drag = *drag_;
    const LONG desired_x = cursor.x - drag.grip.x, desired_y = cursor.y - drag.grip.y;
    if (!drag.moved &&
        std::abs(desired_x - drag.origin.x) < GetSystemMetricsForDpi(SM_CXDRAG, target_dpi_) &&
        std::abs(desired_y - drag.origin.y) < GetSystemMetricsForDpi(SM_CYDRAG, target_dpi_)) return;
    RECT current{};
    if (!GetWindowRect(window_, &current)) return;
    const LONG x = std::clamp(desired_x, bounds_.left, std::max(bounds_.left, bounds_.right - (current.right - current.left)));
    const LONG y = std::clamp(desired_y, bounds_.top, std::max(bounds_.top, bounds_.bottom - (current.bottom - current.top)));
    if ((x != current.left || y != current.top) &&
        SetWindowPos(window_, nullptr, x, y, 0, 0, SWP_NOACTIVATE | SWP_NOSIZE | SWP_NOZORDER)) drag.moved = true;
}
void Overlay::end_drag() {
    if (!drag_) return;
    if (drag_->moved) position_change_ = position();
    drag_.reset();
    KillTimer(window_, drag_timer);
    if (GetCapture() == window_) ReleaseCapture();
}
void Overlay::update(const Snapshot& snapshot, const Config& config, HWND obs_window, ObsResumeState resume_state) {
    try {
        pause_notice_->update(snapshot, config, resume_state);
        if (obs_window && !obs_screen_notice_) obs_screen_notice_ = std::make_unique<PauseNotice>(GetModuleHandleW(nullptr), resume_obs_);
        if (obs_screen_notice_) obs_screen_notice_->update_on_obs_screen(obs_window, snapshot.obs_state == "paused", config, resume_state);
    }
    catch (const std::exception& exception) { error_ = exception.what(); return; }
    if (locked_ != config.hud_position_locked) {
        if (config.hud_position_locked) end_drag();
        locked_ = config.hud_position_locked;
    }
    if (affinity_enabled_ != config.exclude_capture || (!capture_requested_ && !capture_error_)) {
        affinity_enabled_ = config.exclude_capture;
        capture_requested_ = SetWindowDisplayAffinity(window_, config.exclude_capture ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE) != FALSE;
        capture_error_ = capture_requested_ ? 0 : GetLastError();
    }
    if (!config.oled_protection) oled_started_ = 0;
    else if (!oled_started_) oled_started_ = GetTickCount64();
    const auto target = reinterpret_cast<HWND>(snapshot.target.window);
    DWORD target_pid{};
    if (target) GetWindowThreadProcessId(target, &target_pid);
    BOOL cloaked{};
    if (target) DwmGetWindowAttribute(target, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    const bool real_target = target && IsWindow(target) && target_pid == snapshot.target.pid &&
        !IsIconic(target) && IsWindowVisible(target) && !cloaked && snapshot.game_confirmed;
    if (menu_open_) {
        if (real_target && config.enabled) return;
        EndMenu();
    }
    // 编辑也必须依附仍然存在的游戏，退出游戏后绝不回退到桌面。
    if (!real_target || (!config.enabled && !editing_) || (config.hide_on_blur && !snapshot.target.foreground && !editing_)) {
        set_foreground_target(false);
        if (!real_target && editing_) edit(false);
        ShowWindow(window_, SW_HIDE); return;
    }
    RECT bounds{};
    POINT origin{};
    const UINT dpi = GetDpiForWindow(target);
    if (!dpi || !GetClientRect(target, &bounds) || !ClientToScreen(target, &origin)) {
        set_foreground_target(false);
        ShowWindow(window_, SW_HIDE);
        return;
    }
    OffsetRect(&bounds, origin.x, origin.y);
    if (target_ != target) end_drag();
    target_ = target;
    set_foreground_target(snapshot.target.foreground);
    bounds_ = bounds;
    target_dpi_ = dpi;
    try {
        if (!renderer_) renderer_ = std::make_unique<Renderer>(window_);
        auto size = renderer_->measure(snapshot, config, dpi);
        // 初版以窗口边界裁剪，保证长条不会越到另一块屏幕。
        size.cx = std::min(size.cx, bounds.right - bounds.left);
        size.cy = std::min(size.cy, bounds.bottom - bounds.top);
        if (size.cx <= 0 || size.cy <= 0) { end_drag(); ShowWindow(window_, SW_HIDE); return; }
        badge_size_ = {std::min<LONG>(size.cx, MulDiv(hud_position_badge_width, dpi, 96)),
            std::min<LONG>(size.cy, static_cast<LONG>(std::ceil(hud_row_height(config) * dpi / 96)))};
        const int x_margin = MulDiv(config.margin_x, dpi, 96);
        const int y_margin = MulDiv(config.margin_y, dpi, 96);
        int x = bounds.left + (bounds.right - bounds.left - size.cx) / 2;
        if (config.anchor == 1) x = bounds.left + x_margin;
        else if (config.anchor == 2) x = bounds.right - size.cx - x_margin;
        int y = config.anchor == 3 ? bounds.bottom - size.cy - y_margin : bounds.top + y_margin;
        x = std::clamp<LONG>(x, bounds.left, std::max(bounds.left, bounds.right - size.cx));
        y = std::clamp<LONG>(y, bounds.top, std::max(bounds.top, bounds.bottom - size.cy));
        if (config.oled_protection && !editing_ && !drag_ && !position_change_) {
            const auto step = (GetTickCount64() - oled_started_) / 60000;
            x = oled_shift_axis(x, bounds.left, bounds.right - size.cx, step);
        }
        HWND above = HWND_TOPMOST;
        if (!snapshot.target.foreground && !editing_) {
            // 后台监控紧随游戏的 Z 序，被其他应用覆盖时也一起被覆盖。
            SetWindowPos(window_, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            above = GetWindow(target, GW_HWNDPREV);
            if (above == window_) above = GetWindow(window_, GW_HWNDPREV);
            if (!above || (GetWindowLongPtrW(above, GWL_EXSTYLE) & WS_EX_TOPMOST)) above = HWND_TOP;
        }
        if (editing_ && edit_placed_) {
            RECT current{}; GetWindowRect(window_, &current);
            if (current.right - current.left != size.cx || current.bottom - current.top != size.cy)
                SetWindowPos(window_, HWND_TOPMOST, current.left, current.top, size.cx, size.cy, SWP_NOACTIVATE);
        } else {
            if (!editing_ && (drag_ || position_change_)) {
                RECT current{}; GetWindowRect(window_, &current);
                x = std::clamp<LONG>(current.left, bounds.left, bounds.right - size.cx);
                y = std::clamp<LONG>(current.top, bounds.top, bounds.bottom - size.cy);
            }
            SetWindowPos(window_, above, x, y, size.cx, size.cy, SWP_NOACTIVATE);
            if (editing_) edit_placed_ = true;
        }
        ShowWindow(window_, SW_SHOWNOACTIVATE);
        const bool position_locked = locked_ && !editing_;
        const auto title = position_locked ? L"游戏仪表 · 顶部监控 · 已锁定" : L"游戏仪表 · 顶部监控 · 可拖动";
        if (drawn_locked_ != position_locked) { SetWindowTextW(window_, title); drawn_locked_ = position_locked; }
        std::wostringstream key;
        key << config.font_size << config.opacity << config.graph << dpi << size.cx << size.cy << position_locked;
        for (const auto& item : hud_items(snapshot, config)) key << item.label << item.value;
        if (config.graph) for (double point : snapshot.recent_frames) key << point;
        const auto signature = utf8(key.str());
        if (signature != last_draw_) { renderer_->render(snapshot, config, dpi, size, position_locked); last_draw_ = signature; }
        error_.clear();
    } catch (const std::exception& exception) {
        error_ = exception.what(); end_drag(); last_draw_.clear(); renderer_.reset(); ShowWindow(window_, SW_HIDE);
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
    case WM_NCHITTEST: {
        if (self->editing_) return HTCAPTION;
        if (!self->foreground_target_) return HTTRANSPARENT;
        if (!self->locked_) return HTCLIENT;
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        return ScreenToClient(window, &point) && self->on_position_badge(point) ? HTCLIENT : HTTRANSPARENT;
    }
    case WM_MOUSEACTIVATE: return self->editing_ ? MA_ACTIVATE : MA_NOACTIVATE;
    case WM_SETCURSOR:
        if (LOWORD(lparam) == HTCLIENT && self->foreground_target_ && !self->locked_) {
            SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
            return TRUE;
        }
        break;
    case WM_CONTEXTMENU: {
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (point.x == -1 && point.y == -1) {
            point = {0, self->badge_size_.cy};
            if (!ClientToScreen(window, &point)) return 0;
        } else if (self->locked_) {
            POINT client = point;
            if (!ScreenToClient(window, &client) || !self->on_position_badge(client)) return 0;
        }
        self->show_position_menu(point);
        return 0;
    }
    case WM_LBUTTONDOWN:
        self->begin_drag({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
        return 0;
    case WM_MOUSEMOVE:
        if (self->drag_ && (wparam & MK_LBUTTON)) {
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            if (ClientToScreen(window, &point)) self->move_drag(point);
        }
        return 0;
    case WM_TIMER:
        if (wparam != drag_timer || !self->drag_) break;
        // 监控条不抢焦点，鼠标离开窗口之后仍要跟踪拖动。
        if (GetAsyncKeyState(GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON) >= 0) self->end_drag();
        else if (POINT point{}; GetCursorPos(&point)) self->move_drag(point);
        return 0;
    case WM_CANCELMODE:
        self->end_drag();
        break;
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
        self->end_drag();
        return 0;
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
