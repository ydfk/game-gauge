#include "window.h"
#include "host/ipc_server.h"
#include <windowsx.h>
#include <algorithm>

namespace gauge {
void SettingsWindow::quit_program() {
    if (quitting_) return;
    try {
        const auto response = ipc_request({{"command", "quit"}});
        if (!response.value("ok", false)) throw std::runtime_error(response.value("error", std::string("无法退出程序")));
        quitting_ = true;
    } catch (const std::exception& error) { error_ = error.what(); }
}
void SettingsWindow::window_controls(float width) {
    const auto color = D2D1::ColorF(0xF2F6FA);
    const auto maximize = D2D1::RectF(width - 108, 12, width - 64, 46);
    const auto close = D2D1::RectF(width - 60, 12, width - 16, 46);
    if (chrome_hover_ == 1) fill(maximize, D2D1::ColorF(0x1B2B3C), 6);
    if (chrome_hover_ == 2) fill(close, D2D1::ColorF(0xB93F51), 6);
    const float x = width - 86, y = 29;
    const auto square = [&](float left, float top, float size) {
        line(left, top, left + size, top, color, 1.4f); line(left + size, top, left + size, top + size, color, 1.4f);
        line(left + size, top + size, left, top + size, color, 1.4f); line(left, top + size, left, top, color, 1.4f);
    };
    if (IsZoomed(window_)) {
        line(x - 3, y - 7, x + 7, y - 7, color, 1.4f);
        line(x + 7, y - 7, x + 7, y + 3, color, 1.4f);
        square(x - 7, y - 3, 10);
    } else square(x - 6, y - 6, 12);
    line(width - 43, y - 5, width - 33, y + 5, color, 1.4f);
    line(width - 43, y + 5, width - 33, y - 5, color, 1.4f);
    add(maximize, [this](float) { ShowWindow(window_, IsZoomed(window_) ? SW_RESTORE : SW_MAXIMIZE); });
    add(close, [this](float) { PostMessageW(window_, WM_CLOSE, 0, 0); });
}
std::optional<LRESULT> SettingsWindow::chrome_message(UINT message, WPARAM wparam, LPARAM lparam) {
    const auto update_hover = [&](int value) {
        if (chrome_hover_ != value) { chrome_hover_ = value; InvalidateRect(window_, nullptr, FALSE); }
    };
    switch (message) {
    case WM_NCCALCSIZE: {
        // 保留系统缩放和吸附能力，标题栏由客户区绘制；最大化不能遮住任务栏。
        if (IsZoomed(window_)) {
            MONITORINFO monitor{sizeof(monitor)};
            if (GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor)) {
                auto rect = wparam ? &reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam)->rgrc[0] : reinterpret_cast<RECT*>(lparam);
                *rect = monitor.rcWork;
            }
        }
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((wparam & 0xFFF0) == SC_MINIMIZE) return 0;
        break;
    case WM_NCHITTEST: {
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}; ScreenToClient(window_, &point);
        RECT client{}; GetClientRect(window_, &client);
        if (!IsZoomed(window_)) {
            const UINT dpi = GetDpiForWindow(window_);
            const int border = GetSystemMetricsForDpi(SM_CXFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            const bool left = point.x < border, right = point.x >= client.right - border;
            const bool top = point.y < border, bottom = point.y >= client.bottom - border;
            if (top && left) return HTTOPLEFT; if (top && right) return HTTOPRIGHT;
            if (bottom && left) return HTBOTTOMLEFT; if (bottom && right) return HTBOTTOMRIGHT;
            if (top) return HTTOP; if (bottom) return HTBOTTOM; if (left) return HTLEFT; if (right) return HTRIGHT;
        }
        const float x = point.x / scale_, y = point.y / scale_, width = client.right / scale_;
        if (y >= 12 && y <= 46 && x >= width - 108 && x <= width - 64) return HTMAXBUTTON;
        if (y >= 12 && y <= 46 && x >= width - 60 && x <= width - 16) return HTCLIENT;
        if (y < 120) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_NCMOUSEMOVE: {
        update_hover(wparam == HTMAXBUTTON ? 1 : 0);
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE | TME_NONCLIENT, window_, 0}; TrackMouseEvent(&track);
        break;
    }
    case WM_MOUSEMOVE: {
        RECT client{}; GetClientRect(window_, &client);
        const float x = GET_X_LPARAM(lparam) / scale_, y = GET_Y_LPARAM(lparam) / scale_, width = client.right / scale_;
        update_hover(y >= 12 && y <= 46 && x >= width - 60 && x <= width - 16 ? 2 : 0);
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window_, 0}; TrackMouseEvent(&track);
        break;
    }
    case WM_MOUSELEAVE: case WM_NCMOUSELEAVE: update_hover(0); break;
    case WM_NCLBUTTONDOWN:
        if (wparam == HTMAXBUTTON) return 0;
        break;
    case WM_NCLBUTTONUP:
        if (wparam == HTMAXBUTTON) { ShowWindow(window_, IsZoomed(window_) ? SW_RESTORE : SW_MAXIMIZE); return 0; }
        break;
    }
    return {};
}
}
