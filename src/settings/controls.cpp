#include "window.h"
#include "theme.h"
#include "version.h"
#include <algorithm>
#include <windowsx.h>

namespace gauge {
using namespace theme;
namespace {
void check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("界面绘制失败"); }
}
void SettingsWindow::fill(D2D1_RECT_F rect, D2D1_COLOR_F color, float radius) {
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    check(target_->CreateSolidColorBrush(color, &brush));
    if (radius) target_->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush.Get());
    else target_->FillRectangle(rect, brush.Get());
}
void SettingsWindow::line(float x1, float y1, float x2, float y2, D2D1_COLOR_F color, float stroke) {
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    check(target_->CreateSolidColorBrush(color, &brush));
    target_->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), brush.Get(), stroke);
}
void SettingsWindow::text(const std::wstring& value, D2D1_RECT_F rect, IDWriteTextFormat* format, D2D1_COLOR_F color) {
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    check(target_->CreateSolidColorBrush(color, &brush));
    target_->DrawTextW(value.c_str(), static_cast<UINT32>(value.size()), format, rect, brush.Get());
}
void SettingsWindow::label(const std::wstring& value, float x, float y, float width, float height,
    IDWriteTextFormat* format, D2D1_COLOR_F color) {
    text(value, D2D1::RectF(x, y, x + width, y + height), format, color);
}
void SettingsWindow::add(D2D1_RECT_F rect, std::function<void(float)> click, std::function<void(int)> adjust) {
    hotspots_.push_back({rect, std::move(click), std::move(adjust)});
}
void SettingsWindow::outline(D2D1_RECT_F rect, D2D1_COLOR_F color, float radius, float stroke) {
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    check(target_->CreateSolidColorBrush(color, &brush));
    target_->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush.Get(), stroke);
}
bool SettingsWindow::hovered(D2D1_RECT_F rect) const {
    return pointer_inside_ && pointer_.x >= rect.left && pointer_.x <= rect.right && pointer_.y >= rect.top && pointer_.y <= rect.bottom;
}
bool SettingsWindow::pressed(D2D1_RECT_F rect) const {
    return pressed_ && hovered(rect) && pressed_->rect.left == rect.left && pressed_->rect.top == rect.top;
}
void SettingsWindow::button(const std::wstring& title, D2D1_RECT_F rect, std::function<void(float)> click, bool active, bool danger) {
    const auto accent = danger ? red : blue;
    fill(rect, active || danger ? tint(accent, pressed(rect) ? .28f : hovered(rect) ? .22f : .12f) :
        pressed(rect) ? edge : hovered(rect) ? panel_high : panel, 7);
    outline(rect, active || hovered(rect) ? tint(accent, .6f) : danger ? tint(red, .3f) : edge, 7);
    text(title, rect, button_format_.Get(), active || danger ? accent : white);
    add(rect, std::move(click));
}
void SettingsWindow::toggle(const std::wstring& title, const std::wstring& hint, bool value,
    D2D1_RECT_F rect, std::function<void(float)> click) {
    fill(rect, hovered(rect) ? panel_high : panel, 10);
    outline(rect, hovered(rect) ? edge : tint(edge, .4f), 10);
    label(title, rect.left + 18, rect.top + 9, rect.right - rect.left - 90, 22, body_.Get(), white);
    label(hint, rect.left + 18, rect.top + 32, rect.right - rect.left - 90, 18, small_.Get(), muted);
    auto track = D2D1::RectF(rect.right - 68, rect.top + 19, rect.right - 24, rect.top + 43);
    fill(track, value ? mint : D2D1::ColorF(0x3C4D61), 12);
    fill(D2D1::RectF(value ? track.right - 21 : track.left + 3, track.top + 3,
        value ? track.right - 3 : track.left + 21, track.bottom - 3), value ? background : white, 10);
    add(rect, std::move(click));
}
void SettingsWindow::slider(const std::wstring& title, const std::wstring& value, float selected,
    D2D1_RECT_F rect, std::function<void(float)> click) {
    fill(rect, hovered(rect) ? panel_high : panel, 10);
    outline(rect, hovered(rect) ? edge : tint(edge, .4f), 10);
    label(title, rect.left + 18, rect.top + 12, 180, 22, body_.Get(), white);
    label(value, rect.right - 110, rect.top + 12, 92, 22, body_.Get(), mint);
    const float left = rect.left + 18, right = rect.right - 18, y = rect.bottom - 21;
    fill(D2D1::RectF(left, y, right, y + 5), edge, 3);
    fill(D2D1::RectF(left, y, left + (right - left) * selected, y + 5), mint, 3);
    const float center = left + (right - left) * selected;
    fill(D2D1::RectF(center - 9, y - 7, center + 9, y + 11), tint(mint, .18f), 9);
    fill(D2D1::RectF(center - 5, y - 3, center + 5, y + 7), white, 5);
    auto adjust = [selected, rect, click](int direction) {
        const float amount = std::clamp(selected + direction * .05f, 0.f, 1.f);
        click(rect.left + 18 + amount * (rect.right - rect.left - 36));
    };
    add(rect, std::move(click), std::move(adjust));
}
void SettingsWindow::navigation(float width, float height) {
    fill(D2D1::RectF(0, 0, 236, height), D2D1::ColorF(0x101A27));
    fill(D2D1::RectF(27, 34, 65, 72), mint, 10);
    line(33, 58, 39, 53, background, 2.5f);
    line(39, 53, 45, 58, background, 2.5f);
    line(45, 58, 52, 53, background, 2.5f);
    line(52, 53, 59, 43, background, 2.5f);
    label(L"游戏仪表", 77, 35, 145, 27, heading_.Get(), white);
    label(L"GAMEGAUGE", 78, 61, 145, 18, small_.Get(), muted);
    const wchar_t* names[]{L"外观", L"监控项目", L"游戏与排除", L"游戏历史", L"版本与更新"};
    for (int i = 0; i < 5; ++i) {
        const float y = 132.f + i * 60;
        const auto rect = D2D1::RectF(16, y, 220, y + 48);
        if (page_ == i || hovered(rect)) fill(rect, page_ == i ? tint(blue, .12f) : panel, 8);
        if (page_ == i) fill(D2D1::RectF(16, y + 13, 19, y + 35), blue, 2);
        const auto ink = page_ == i ? blue : muted;
        const float x = 41, top = y + 17;
        if (i == 0) {
            outline(D2D1::RectF(x, top, x + 16, top + 14), ink, 3);
            line(x + 4, top + 7, x + 12, top + 7, ink);
        } else if (i == 1) {
            line(x, top + 12, x + 4, top + 12, ink, 1.5f); line(x + 4, top + 12, x + 7, top, ink, 1.5f);
            line(x + 7, top, x + 11, top + 14, ink, 1.5f); line(x + 11, top + 14, x + 16, top + 6, ink, 1.5f);
        } else if (i == 2) {
            outline(D2D1::RectF(x, top + 2, x + 17, top + 14), ink, 4);
            line(x + 3, top + 8, x + 8, top + 8, ink); line(x + 5.5f, top + 5.5f, x + 5.5f, top + 10.5f, ink);
            fill(D2D1::RectF(x + 12, top + 7, x + 14, top + 9), ink, 1);
        } else if (i == 3) {
            outline(D2D1::RectF(x, top, x + 16, top + 16), ink, 8);
            line(x + 8, top + 3, x + 8, top + 8, ink); line(x + 8, top + 8, x + 12, top + 10, ink);
        } else {
            line(x + 8, top + 1, x + 8, top + 12, ink, 1.5f);
            line(x + 3, top + 7, x + 8, top + 12, ink, 1.5f); line(x + 8, top + 12, x + 13, top + 7, ink, 1.5f);
            line(x + 1, top + 16, x + 15, top + 16, ink);
        }
        label(names[i], 74, y + 11, 130, 24, body_.Get(), page_ == i ? white : muted);
        add(D2D1::RectF(16, y, 220, y + 48), [this, i](float) {
            page_ = i; focused_ = -1; if (page_ == 3) refresh(); InvalidateRect(window_, nullptr, FALSE);
        });
    }
    button(quitting_ ? L"正在退出…" : L"退出程序", D2D1::RectF(28, height - 151, 208, height - 113),
        [this](float) { quit_program(); }, false, true);
    line(28, height - 104, 208, height - 104, edge);
    label(L"本机采集 · 自动保存", 28, height - 81, 190, 20, small_.Get(), muted);
    label(L"版本 " + wide(GAMEGAUGE_VERSION), 28, height - 55, 180, 20, small_.Get(), muted);
    fill(D2D1::RectF(236, 0, width, height), background);
}
std::optional<LRESULT> SettingsWindow::interaction_message(UINT message, WPARAM, LPARAM lparam) {
    const auto hit = [&](float x, float y) -> int {
        for (int i = static_cast<int>(hotspots_.size()) - 1; i >= 0; --i) {
            const auto rect = hotspots_[i].rect;
            if (x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom) return i;
        }
        return -1;
    };
    switch (message) {
    case WM_MOUSEMOVE: {
        const int previous = pointer_inside_ ? hit(pointer_.x, pointer_.y) : -1;
        pointer_ = D2D1::Point2F(GET_X_LPARAM(lparam) / scale_, GET_Y_LPARAM(lparam) / scale_); pointer_inside_ = true;
        if (previous != hit(pointer_.x, pointer_.y)) InvalidateRect(window_, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSELEAVE:
        pointer_inside_ = false; InvalidateRect(window_, nullptr, FALSE); return 0;
    case WM_NCMOUSEMOVE:
        if (pointer_inside_) { pointer_inside_ = false; InvalidateRect(window_, nullptr, FALSE); }
        break;
    case WM_LBUTTONDOWN: {
        const float x = GET_X_LPARAM(lparam) / scale_, y = GET_Y_LPARAM(lparam) / scale_;
        const int index = hit(x, y);
        if (index >= 0) {
            pointer_ = D2D1::Point2F(x, y); pointer_inside_ = true; focused_ = index;
            pressed_ = hotspots_[index]; SetCapture(window_); InvalidateRect(window_, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        auto item = pressed_; pressed_.reset(); ReleaseCapture();
        const float x = GET_X_LPARAM(lparam) / scale_, y = GET_Y_LPARAM(lparam) / scale_;
        if (item && x >= item->rect.left && x <= item->rect.right && y >= item->rect.top && y <= item->rect.bottom) item->click(x);
        InvalidateRect(window_, nullptr, FALSE); return 0;
    }
    case WM_CAPTURECHANGED: case WM_KILLFOCUS:
        pressed_.reset(); InvalidateRect(window_, nullptr, FALSE); break;
    }
    return {};
}
}
