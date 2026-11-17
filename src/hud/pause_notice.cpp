#include "pause_notice.h"
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <dwmapi.h>
#include <stdexcept>
namespace gauge {
namespace {
constexpr UINT_PTR pointer_timer = 1;

bool over_dismiss_area(HWND window, POINT point) {
    RECT client{};
    GetClientRect(window, &client);
    client.top = static_cast<LONG>(client.bottom * .65f);
    return PtInRect(&client, point);
}
}
PauseNotice::PauseNotice(HINSTANCE instance) {
    WNDCLASSW cls{}; cls.hInstance = instance; cls.lpszClassName = L"GameGauge.PauseNotice";
    cls.lpfnWndProc = procedure; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); RegisterClassW(&cls);
    window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
        cls.lpszClassName, L"OBS 录屏暂停", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, this);
    if (!window_) throw std::runtime_error("无法创建 OBS 暂停提醒");
}
PauseNotice::~PauseNotice() {
    end_pointer_gesture();
    renderer_.reset(); if (window_) DestroyWindow(window_);
}
void PauseNotice::update(const Snapshot& s, const Config& config) {
    if (pid_ != s.target.pid) dismissed_ = false;
    pid_ = s.target.pid;
    if (!should_show(s.obs_state == "paused", config.obs_pause_notice)) return;
    const auto game = reinterpret_cast<HWND>(s.target.window);
    DWORD pid{}; if (game) GetWindowThreadProcessId(game, &pid);
    BOOL cloaked{}; if (game) DwmGetWindowAttribute(game, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (!config.enabled || !s.game_confirmed ||
        !s.target.foreground || !game || !IsWindow(game) || pid != s.target.pid || !IsWindowVisible(game) || IsIconic(game) || cloaked) {
        hide();
        return;
    }
    RECT bounds{};
    POINT origin{};
    if (!GetClientRect(game, &bounds) || !ClientToScreen(game, &origin)) {
        hide();
        return;
    }
    OffsetRect(&bounds, origin.x, origin.y);
    show(bounds, GetDpiForWindow(game), config.exclude_capture);
}

void PauseNotice::update_on_obs_screen(HWND obs_window, bool paused, const Config& config) {
    if (!should_show(paused, config.obs_screen_pause_notice)) return;
    MONITORINFO monitor{sizeof(monitor)};
    if (!obs_window || !GetMonitorInfoW(MonitorFromWindow(obs_window, MONITOR_DEFAULTTONEAREST), &monitor)) {
        hide();
        return;
    }
    if (!screen_) screen_.emplace();
    show(monitor.rcWork, GetDpiForWindow(obs_window), config.exclude_capture);
}

bool PauseNotice::should_show(bool paused, bool enabled) {
    if (!paused || !enabled) dismissed_ = false;
    if (paused && enabled && !dismissed_) return true;
    hide();
    return false;
}

void PauseNotice::show(const RECT& bounds, UINT dpi, bool exclude_capture) {
    const LONG width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
    const SIZE size{std::min<LONG>(width, MulDiv(720, dpi, 96)),
        std::min<LONG>(height, MulDiv(210, dpi, 96))};
    if (!dpi || size.cx <= 0 || size.cy <= 0) {
        hide();
        return;
    }
    SetWindowDisplayAffinity(window_, exclude_capture ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE);
    if (!renderer_) renderer_ = std::make_unique<Renderer>(window_);
    if (drawn_.cx != size.cx || drawn_.cy != size.cy || dpi_ != dpi) {
        renderer_->render_notice(dpi, size); drawn_ = size; dpi_ = dpi;
    }
    const LONG span_x = width - size.cx, span_y = height - size.cy;
    POINT position{bounds.left + span_x / 2, bounds.top + span_y / 2};
    if (screen_) {
        screen_->limits = {bounds.left, bounds.top, bounds.right - size.cx, bounds.bottom - size.cy};
        position = {bounds.left + std::lround(span_x * screen_->x), bounds.top + std::lround(span_y * screen_->y)};
    }
    SetWindowPos(window_, HWND_TOPMOST, position.x, position.y, size.cx, size.cy, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void PauseNotice::hide() {
    end_pointer_gesture();
    ShowWindow(window_, SW_HIDE);
}

void PauseNotice::move_drag(POINT cursor) {
    auto& screen = *screen_;
    const auto& limits = screen.limits;
    const LONG x = std::clamp(cursor.x - screen.drag_grip->x, limits.left, limits.right);
    const LONG y = std::clamp(cursor.y - screen.drag_grip->y, limits.top, limits.bottom);
    if (limits.right > limits.left) screen.x = static_cast<double>(x - limits.left) / (limits.right - limits.left);
    if (limits.bottom > limits.top) screen.y = static_cast<double>(y - limits.top) / (limits.bottom - limits.top);
    RECT current{};
    if (GetWindowRect(window_, &current) && (current.left != x || current.top != y))
        SetWindowPos(window_, nullptr, x, y, 0, 0, SWP_NOACTIVATE | SWP_NOSIZE | SWP_NOZORDER);
}

void PauseNotice::end_pointer_gesture() {
    if (!screen_ || (!screen_->drag_grip && !screen_->dismiss_pressed)) return;
    screen_->drag_grip.reset();
    screen_->dismiss_pressed = false;
    KillTimer(window_, pointer_timer);
    if (GetCapture() == window_) ReleaseCapture();
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
    auto& screen = self->screen_;
    if (screen) {
        switch (message) {
        case WM_SETCURSOR: {
            POINT point{};
            if (LOWORD(lparam) == HTCLIENT && GetCursorPos(&point) && ScreenToClient(window, &point) &&
                (screen->drag_grip || !over_dismiss_area(window, point))) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
                return TRUE;
            }
            break;
        }
        case WM_LBUTTONDOWN: {
            self->end_pointer_gesture();
            const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            screen->dismiss_pressed = over_dismiss_area(window, point);
            if (!screen->dismiss_pressed) screen->drag_grip = point;
            SetCapture(window);
            SetTimer(window, pointer_timer, 16, nullptr);
            return 0;
        }
        case WM_MOUSEMOVE:
            if (screen->drag_grip && (wparam & MK_LBUTTON)) {
                POINT cursor{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                if (ClientToScreen(window, &cursor)) self->move_drag(cursor);
            }
            return 0;
        case WM_TIMER:
            if (wparam != pointer_timer || (!screen->drag_grip && !screen->dismiss_pressed)) break;
            // 提醒不抢焦点，鼠标移出窗口后仍需跟踪指针和松键。
            if (GetAsyncKeyState(GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON) >= 0) {
                self->end_pointer_gesture();
            } else if (POINT cursor{}; screen->drag_grip && GetCursorPos(&cursor)) {
                self->move_drag(cursor);
            }
            return 0;
        case WM_CANCELMODE:
        case WM_CAPTURECHANGED:
            self->end_pointer_gesture();
            return 0;
        }
    }
    if (message == WM_LBUTTONUP) {
        const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        const bool dismiss = (!screen || screen->dismiss_pressed) && over_dismiss_area(window, point);
        self->end_pointer_gesture();
        if (dismiss) {
            self->dismissed_ = true;
            self->hide();
        }
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
}
