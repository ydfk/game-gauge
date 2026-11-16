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
    if (rect.right <= rect.left || rect.bottom <= rect.top) return;
    if (in_page_) content_bottom_ = std::max(content_bottom_, rect.bottom);
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
    if (value.empty() || rect.right <= rect.left || rect.bottom <= rect.top) return;
    if (in_page_) content_bottom_ = std::max(content_bottom_, rect.bottom);
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    check(target_->CreateSolidColorBrush(color, &brush));
    target_->DrawTextW(value.c_str(), static_cast<UINT32>(value.size()), format, rect, brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}
void SettingsWindow::label(const std::wstring& value, float x, float y, float width, float height,
    IDWriteTextFormat* format, D2D1_COLOR_F color) {
    text(value, D2D1::RectF(x, y, x + width, y + height), format, color);
}
void SettingsWindow::add(D2D1_RECT_F rect, std::function<void(float)> click, std::function<void(int)> adjust) {
    if (in_page_) content_bottom_ = std::max(content_bottom_, rect.bottom);
    hotspots_.push_back({screen_rect(rect), std::move(click), std::move(adjust),
        in_page_ ? content_shift_ : 0.f, in_page_});
}
void SettingsWindow::outline(D2D1_RECT_F rect, D2D1_COLOR_F color, float radius, float stroke) {
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    check(target_->CreateSolidColorBrush(color, &brush));
    target_->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush.Get(), stroke);
}
bool SettingsWindow::hovered(D2D1_RECT_F rect) const {
    if (in_page_ && (pointer_.y < page_top || pointer_.y > viewport_bottom_)) return false;
    rect = screen_rect(rect);
    return pointer_inside_ && pointer_.x >= rect.left && pointer_.x <= rect.right && pointer_.y >= rect.top && pointer_.y <= rect.bottom;
}
bool SettingsWindow::pressed(D2D1_RECT_F rect) const {
    const auto actual = screen_rect(rect);
    return pressed_ && hovered(rect) && pressed_->rect.left == actual.left && pressed_->rect.top == actual.top;
}
D2D1_RECT_F SettingsWindow::screen_rect(D2D1_RECT_F rect) const {
    if (in_page_) {
        rect.left += content_shift_; rect.right += content_shift_;
        rect.top += page_offset_ - scroll_; rect.bottom += page_offset_ - scroll_;
    }
    return rect;
}
float SettingsWindow::hover_strength(D2D1_RECT_F rect) const {
    if (!hovered(rect)) return 0;
    if (!motion_ || !hover_start_) return 1;
    const float progress = std::min(1.f, static_cast<float>(GetTickCount64() - hover_start_) / motion_ms);
    return 1 - (1 - progress) * (1 - progress);
}
void SettingsWindow::surface(D2D1_RECT_F rect, bool featured) {
    fill(D2D1::RectF(rect.left + 2, rect.top + 4, rect.right - 2, rect.bottom + 4), tint(background, .45f), card_radius);
    gradient(target_.Get(), rect, tint(panel_high, featured ? .77f : .63f),
        tint(panel, featured ? .86f : .91f), card_radius);
    if (featured) {
        const auto rim = linear_brush(target_.Get(), rect, tint(blue, .30f), tint(accent, .51f));
        target_->DrawRoundedRectangle(D2D1::RoundedRect(rect, card_radius, card_radius), rim.Get());
    } else outline(rect, edge, card_radius);
    line(rect.left + card_radius, rect.top + 1, rect.right - card_radius, rect.top + 1, ink_edge);
}
void SettingsWindow::badge(const std::wstring& title, D2D1_RECT_F rect, D2D1_COLOR_F color) {
    fill(rect, tint(color, .09f), control_radius);
    outline(rect, tint(color, .23f), control_radius);
    text(title, rect, button_format_.Get(), color);
}
void SettingsWindow::button(const std::wstring& title, D2D1_RECT_F rect, std::function<void(float)> click, bool active, bool danger) {
    const auto color = danger ? red : accent;
    const float hover = hover_strength(rect);
    fill(rect, tint(panel, .84f), control_radius);
    if (active || danger)
        gradient(target_.Get(), rect, tint(color, pressed(rect) ? .43f : .26f + hover * .13f),
            tint(color, .11f + hover * .07f), control_radius);
    else fill(rect, tint(panel_high, pressed(rect) ? .95f : hover * .8f), control_radius);
    outline(rect, active || danger || hover > 0 ? tint(color, .38f + hover * .30f) : edge, control_radius);
    text(title, rect, button_format_.Get(), danger ? red : active ? lilac : white);
    add(rect, std::move(click));
}
void SettingsWindow::tab(const std::wstring& title, D2D1_RECT_F rect, std::function<void(float)> click, bool active, bool enabled) {
    const float hover = enabled ? hover_strength(rect) : 0.f;
    if (active || hover > 0) {
        const float strength = pressed(rect) ? .30f : active ? .17f + hover * .09f : .09f * hover;
        fill(rect, tint(active ? accent : lilac, strength), control_radius);
    }
    if (active) fill(D2D1::RectF(rect.left + 13, rect.bottom - 3, rect.right - 13, rect.bottom - 1), lilac, 1);
    text(title, rect, button_format_.Get(), active ? lilac : hover > 0 ? white : muted);
    if (enabled) add(rect, std::move(click));
}
void SettingsWindow::toggle(const std::wstring& title, const std::wstring& hint, bool value,
    D2D1_RECT_F rect, std::function<void(float)> click) {
    fill(rect, tint(panel_high, pressed(rect) ? .8f : hover_strength(rect) * .55f), control_radius);
    const float center = (rect.top + rect.bottom) / 2;
    label(title, rect.left + 15, center - 21, rect.right - rect.left - 80, 22, body_.Get(), white);
    label(hint, rect.left + 15, center + 2, rect.right - rect.left - 80, 18, small_.Get(), muted);
    auto track = D2D1::RectF(rect.right - 61, center - 11, rect.right - 17, center + 11);
    gradient(target_.Get(), track, value ? accent : edge, value ? tint(accent, .54f) : panel_high, 12, true);
    outline(track, value ? tint(lilac, .36f) : ink_edge, 12);
    fill(D2D1::RectF(value ? track.right - 21 : track.left + 3, track.top + 3,
        value ? track.right - 3 : track.left + 21, track.bottom - 3), value ? white : muted, 10);
    add(rect, std::move(click));
}
void SettingsWindow::slider(const std::wstring& title, const std::wstring& value, float selected,
    D2D1_RECT_F rect, std::function<void(float)> click) {
    fill(rect, tint(panel_high, hover_strength(rect) * .54f), control_radius);
    label(title, rect.left + 18, rect.top + 12, 180, 22, body_.Get(), white);
    label(value, rect.right - 106, rect.top + 12, 88, 22, mono_.Get(), lilac);
    const float left = rect.left + 18, right = rect.right - 18, y = rect.bottom - 21;
    fill(D2D1::RectF(left, y, right, y + 5), edge, 3);
    if (selected > 0) gradient(target_.Get(), D2D1::RectF(left, y, left + (right - left) * selected, y + 5),
        accent, blue, 3, true);
    const float center = left + (right - left) * selected;
    fill(D2D1::RectF(center - 9, y - 7, center + 9, y + 11), tint(accent, .25f), 9);
    fill(D2D1::RectF(center - 5, y - 3, center + 5, y + 7), white, 5);
    auto adjust = [selected, rect, click](int direction) {
        const float amount = std::clamp(selected + direction * .05f, 0.f, 1.f);
        click(rect.left + 18 + amount * (rect.right - rect.left - 36));
    };
    add(rect, std::move(click), std::move(adjust));
}
void SettingsWindow::icon(int id, float x, float y, D2D1_COLOR_F color) {
    const auto stroke = [&](float x1, float y1, float x2, float y2) { line(x + x1, y + y1, x + x2, y + y2, color, 1.45f); };
    const auto box = [&](float l, float t, float r, float b, float radius) {
        outline(D2D1::RectF(x + l, y + t, x + r, y + b), color, radius, 1.35f);
    };
    switch (id) {
    case 0:
        box(0, 1, 18, 13, 2); stroke(9, 13, 9, 17); stroke(5, 17, 13, 17); break;
    case 1:
        stroke(0, 9, 4, 9); stroke(4, 9, 7, 2); stroke(7, 2, 11, 16); stroke(11, 16, 14, 9); stroke(14, 9, 18, 9); break;
    case 2:
        box(0, 4, 18, 15, 4); stroke(3, 9, 8, 9); stroke(5.5f, 6.5f, 5.5f, 11.5f);
        fill(D2D1::RectF(x + 12, y + 8, x + 14.5f, y + 10.5f), color, 1.25f); break;
    case 3:
        box(1, 1, 17, 17, 8); stroke(9, 4.5f, 9, 9); stroke(9, 9, 12.5f, 11); break;
    case 4:
        stroke(9, 1, 9, 12); stroke(4, 7, 9, 12); stroke(9, 12, 14, 7);
        stroke(1, 12, 1, 17); stroke(1, 17, 17, 17); stroke(17, 17, 17, 12); break;
    case 5:
        box(1, 1, 17, 6, 1.5f); box(2.5f, 6, 15.5f, 17, 2); stroke(6.5f, 10, 11.5f, 10); break;
    case 6:
        box(1, 2, 17, 16, 2.5f); stroke(6, 2, 6, 16);
        stroke(13, 6.5f, 10.5f, 9); stroke(10.5f, 9, 13, 11.5f); break;
    case 7:
        stroke(7, 2, 2, 2); stroke(2, 2, 2, 16); stroke(2, 16, 7, 16);
        stroke(6, 9, 17, 9); stroke(13, 5, 17, 9); stroke(17, 9, 13, 13); break;
    case 8:
        stroke(2, 12, 6, 8); stroke(6, 8, 10, 11); stroke(10, 11, 17, 3); stroke(13, 3, 17, 3); stroke(17, 3, 17, 7); break;
    }
}
void SettingsWindow::aurora(D2D1_RECT_F rect) {
    const float w = rect.right - rect.left, h = rect.bottom - rect.top;
    const auto point = [&](float x, float y) { return D2D1::Point2F(rect.left + w * x, rect.top + h * y); };
    target_->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_ALIASED);
    glow(target_.Get(), rect, point(.78f, .12f), D2D1::SizeF(w * .50f, h * 1.05f), tint(accent, .32f));
    glow(target_.Get(), rect, point(.13f, .95f), D2D1::SizeF(w * .46f, h * .88f), tint(blue, .21f));
    for (int star = 0; star < 72; ++star) {
        const auto p = point((star * 61 % 139) / 139.f, (star * 29 % 101) / 101.f);
        const float size = star % 13 == 0 ? 1.35f : .75f;
        fill(D2D1::RectF(p.x, p.y, p.x + size, p.y + size), tint(white, star % 7 ? .12f : .33f), size);
    }
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> ribbon;
    Microsoft::WRL::ComPtr<ID2D1GeometrySink> path;
    check(factory_->CreatePathGeometry(&ribbon)); check(ribbon->Open(&path));
    path->BeginFigure(point(-.07f, .94f), D2D1_FIGURE_BEGIN_HOLLOW);
    path->AddBezier(D2D1::BezierSegment(point(.19f, .20f), point(.33f, 1.06f), point(.54f, .77f)));
    path->AddBezier(D2D1::BezierSegment(point(.72f, .51f), point(.81f, -.26f), point(1.07f, .43f)));
    path->EndFigure(D2D1_FIGURE_END_OPEN); check(path->Close());
    const D2D1_GRADIENT_STOP colors[]{{0, tint(blue, .12f)}, {.18f, tint(blue, .64f)},
        {.48f, tint(lilac, .12f)}, {.82f, tint(accent, .72f)}, {1, tint(accent, .12f)}};
    Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> stops;
    check(target_->CreateGradientStopCollection(colors, ARRAYSIZE(colors), &stops));
    Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> brush;
    check(target_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(point(0, 0), point(1, 0)), stops.Get(), &brush));
    brush->SetOpacity(.06f); target_->DrawGeometry(ribbon.Get(), brush.Get(), 24);
    brush->SetOpacity(.18f); target_->DrawGeometry(ribbon.Get(), brush.Get(), 7);
    brush->SetOpacity(.76f); target_->DrawGeometry(ribbon.Get(), brush.Get(), 1.2f);
    target_->PopAxisAlignedClip();
}
void SettingsWindow::navigation(float width, float height) {
    const float side = sidebar_width_;
    const auto rail = D2D1::RectF(0, 0, side, height);
    gradient(target_.Get(), rail, sidebar, tint(sidebar, .90f), 0);
    glow(target_.Get(), rail, D2D1::Point2F(18, 32), D2D1::SizeF(210, 240), tint(accent, .10f));
    line(side, 0, side, height, edge);
    line(0, 63, width, 63, ink_edge);
    const auto brand = D2D1::RectF(17, 15, 49, 47);
    gradient(target_.Get(), brand, tint(accent, .56f), tint(blue, .20f), control_radius);
    outline(brand, tint(lilac, .44f), control_radius);
    icon(8, 24, 23, white);
    const bool expanded = side >= 180;
    if (expanded) {
        label(L"游戏仪表", 60, 12, side - 105, 23, heading_.Get(), white);
        label(L"GameGauge", 60, 34, side - 105, 16, small_.Get(), muted);
    }
    const float collapse_y = expanded ? 17.f : 76.f;
    const auto collapse = D2D1::RectF(side - (expanded ? 44 : 55), collapse_y,
        side - (expanded ? 14 : 17), collapse_y + 30);
    fill(collapse, tint(panel_high, hover_strength(collapse)), control_radius);
    icon(6, (collapse.left + collapse.right) / 2 - 9, collapse_y + 6, muted);
    add(collapse, [this](float) {
        nav_toggled_ = true; compact_nav_ = !compact_nav_; focused_ = -1;
    });
    for (int i = 0; i < 6; ++i) {
        const float y = 126.f + i * 52;
        const auto rect = D2D1::RectF(12, y, side - 12, y + 42);
        const bool active = page_ == i;
        const float hover = hover_strength(rect);
        if (active) {
            gradient(target_.Get(), rect, tint(accent, .29f + hover * .1f), tint(accent, .07f), control_radius, true);
            outline(rect, tint(accent, .35f), control_radius);
            fill(D2D1::RectF(rect.left, y + 10, rect.left + 2, y + 32), lilac, 1);
        } else if (hover > 0) fill(rect, tint(panel_high, hover), control_radius);
        icon(i, 24, y + 12, active ? lilac : muted);
        if (expanded) label(page_names[i], 55, y + 3, side - 78, 36, body_.Get(), active ? white : muted);
        add(rect, [this, i](float) { select_page(i); });
    }
    line(15, height - 64, side - 15, height - 64, ink_edge);
    if (expanded) {
        row_action(L"v" + wide(GAMEGAUGE_VERSION), D2D1::RectF(14, height - 56, side - 77, height - 23),
            [this](float) { select_page(4); });
        row_action(quitting_ ? L"退出中…" : L"退出", D2D1::RectF(side - 73, height - 56, side - 12, height - 23),
            [this](float) { quit_program(); });
    } else {
        const auto exit = D2D1::RectF(12, height - 54, side - 12, height - 16);
        fill(exit, tint(panel_high, hover_strength(exit)), control_radius);
        icon(7, 24, height - 44, muted);
        add(exit, [this](float) { quit_program(); });
    }
}
void SettingsWindow::select_page(int next) {
    if (next != page_) {
        scroll_ = 0; focused_ = -1; page_start_ = motion_ ? GetTickCount64() : 0;
    }
    if (next == 5) saves_open();
    else {
        page_ = next;
        if (next == 3 || next == 4) refresh();
    }
    InvalidateRect(window_, nullptr, FALSE);
}
void SettingsWindow::scroll_by(float distance) {
    scroll_ = std::clamp(scroll_ + distance, 0.f, scroll_limit_);
    InvalidateRect(window_, nullptr, FALSE);
}
void SettingsWindow::reveal_focus(int direction) {
    const auto size = static_cast<int>(hotspots_.size());
    if (!size) return;
    if (focused_ < 0) focused_ = direction > 0 ? size - 1 : 0;
    focused_ = (focused_ + direction + size) % size;
    const auto& item = hotspots_[focused_];
    if (!item.page) return;
    if (item.rect.top < page_top + 5) scroll_by(item.rect.top - page_top - 5);
    if (item.rect.bottom > viewport_bottom_ - 5) scroll_by(item.rect.bottom - viewport_bottom_ + 5);
}
void SettingsWindow::draw_scrollbar(float width, float height) {
    scrollbar_ = {};
    if (scroll_limit_ <= 1) return;
    const float track = height - page_top;
    const float thumb = std::max(34.f, track * track / (track + scroll_limit_));
    const float top = page_top + (track - thumb) * scroll_ / scroll_limit_;
    fill(D2D1::RectF(width - 13, page_top, width - 9, height), tint(edge, .55f), 2);
    scrollbar_ = D2D1::RectF(width - 20, page_top, width - 3, height);
    scroll_thumb_ = D2D1::RectF(width - 14, top, width - 8, top + thumb);
    fill(scroll_thumb_, tint(lilac, scroll_grab_ || hovered(scrollbar_) ? .72f : .43f), 3);
}
void SettingsWindow::drag_scrollbar(float y) {
    const float travel = scrollbar_.bottom - scrollbar_.top - (scroll_thumb_.bottom - scroll_thumb_.top);
    if (travel <= 0) return;
    scroll_ = std::clamp((y - scrollbar_.top - *scroll_grab_) / travel, 0.f, 1.f) * scroll_limit_;
    InvalidateRect(window_, nullptr, FALSE);
}

std::optional<LRESULT> SettingsWindow::interaction_message(UINT message, WPARAM wparam, LPARAM lparam) {
    const auto hit = [&](float x, float y) -> int {
        for (int i = static_cast<int>(hotspots_.size()) - 1; i >= 0; --i) {
            const auto& item = hotspots_[i];
            if (item.page && (y < page_top || y > viewport_bottom_)) continue;
            const auto rect = item.rect;
            if (x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom) return i;
        }
        return -1;
    };
    switch (message) {
    case WM_MOUSEMOVE: {
        pointer_ = D2D1::Point2F(GET_X_LPARAM(lparam) / scale_, GET_Y_LPARAM(lparam) / scale_); pointer_inside_ = true;
        if (scroll_grab_) { drag_scrollbar(pointer_.y); return 0; }
        const int index = hit(pointer_.x, pointer_.y);
        if (index != hover_index_) {
            hover_index_ = index; hover_start_ = motion_ && index >= 0 ? GetTickCount64() : 0;
            InvalidateRect(window_, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        pointer_inside_ = false; hover_index_ = -1; InvalidateRect(window_, nullptr, FALSE); return 0;
    case WM_NCMOUSEMOVE:
        if (pointer_inside_) { pointer_inside_ = false; hover_index_ = -1; InvalidateRect(window_, nullptr, FALSE); }
        break;
    case WM_MOUSEWHEEL:
        if (scroll_limit_ > 0) {
            scroll_by(-static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) * 58.f / WHEEL_DELTA);
            return 0;
        }
        break;
    case WM_LBUTTONDOWN: {
        const float x = GET_X_LPARAM(lparam) / scale_, y = GET_Y_LPARAM(lparam) / scale_;
        if (x >= scrollbar_.left && x <= scrollbar_.right && y >= scrollbar_.top && y <= scrollbar_.bottom &&
            scrollbar_.bottom > scrollbar_.top) {
            scroll_grab_ = y >= scroll_thumb_.top && y <= scroll_thumb_.bottom ? y - scroll_thumb_.top
                : (scroll_thumb_.bottom - scroll_thumb_.top) / 2;
            SetCapture(window_);
            drag_scrollbar(y);
            return 0;
        }
        const int index = hit(x, y);
        if (index >= 0) {
            pointer_ = D2D1::Point2F(x, y); pointer_inside_ = true; focused_ = index;
            pressed_ = hotspots_[index]; SetCapture(window_); InvalidateRect(window_, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (scroll_grab_) { scroll_grab_.reset(); ReleaseCapture(); InvalidateRect(window_, nullptr, FALSE); return 0; }
        auto item = pressed_; pressed_.reset(); ReleaseCapture();
        const float x = GET_X_LPARAM(lparam) / scale_, y = GET_Y_LPARAM(lparam) / scale_;
        if (item && (!item->page || y >= page_top && y <= viewport_bottom_) &&
            x >= item->rect.left && x <= item->rect.right && y >= item->rect.top && y <= item->rect.bottom) item->click(x - item->shift);
        InvalidateRect(window_, nullptr, FALSE); return 0;
    }
    case WM_CAPTURECHANGED: case WM_KILLFOCUS:
        scroll_grab_.reset(); pressed_.reset(); InvalidateRect(window_, nullptr, FALSE); break;
    }
    return {};
}
}
