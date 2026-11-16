#pragma once
#include <d2d1.h>
#include <wrl/client.h>
#include <stdexcept>

namespace gauge::theme {
inline constexpr unsigned background_rgb = 0x09090F, sidebar_rgb = 0x101019;
inline constexpr unsigned panel_rgb = 0x151524, edge_rgb = 0x302747;
inline constexpr unsigned accent_rgb = 0x8B5CF6;
inline constexpr float card_radius = 14.f, control_radius = 9.f;
inline constexpr unsigned motion_ms = 180;
inline const auto background = D2D1::ColorF(background_rgb);
inline const auto sidebar = D2D1::ColorF(sidebar_rgb);
inline const auto panel = D2D1::ColorF(panel_rgb);
inline const auto panel_high = D2D1::ColorF(0x242139);
inline const auto edge = D2D1::ColorF(edge_rgb);
inline const auto ink_edge = D2D1::ColorF(0xFFFFFF, .08f);
inline const auto accent = D2D1::ColorF(accent_rgb);
inline const auto lilac = D2D1::ColorF(0xC8B5FF);
inline const auto blue = D2D1::ColorF(0x38BDF8);
inline const auto white = D2D1::ColorF(0xF4F4F5);
inline const auto muted = D2D1::ColorF(0xA1A1AA);
inline const auto mint = D2D1::ColorF(0x75DFB0);
inline const auto amber = D2D1::ColorF(0xF1CC75);
inline const auto peach = D2D1::ColorF(0xFFAA78);
inline const auto red = D2D1::ColorF(0xFF7D8C);

inline D2D1::ColorF tint(D2D1_COLOR_F color, float alpha) { return {color.r, color.g, color.b, alpha}; }
inline COLORREF native_color(D2D1_COLOR_F color) {
    return RGB(static_cast<BYTE>(color.r * 255 + .5f), static_cast<BYTE>(color.g * 255 + .5f),
        static_cast<BYTE>(color.b * 255 + .5f));
}
inline bool motion_enabled() {
    BOOL enabled = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0);
    return enabled != FALSE;
}
inline void require_brush(HRESULT hr) {
    if (FAILED(hr)) throw std::runtime_error("无法创建界面画刷");
}
inline Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> linear_brush(ID2D1RenderTarget* target,
    D2D1_RECT_F rect, D2D1_COLOR_F from, D2D1_COLOR_F to, bool horizontal = false) {
    const D2D1_GRADIENT_STOP colors[]{{0, from}, {1, to}};
    Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> stops;
    require_brush(target->CreateGradientStopCollection(colors, 2, &stops));
    Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> brush;
    require_brush(target->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(
        D2D1::Point2F(rect.left, rect.top), horizontal ? D2D1::Point2F(rect.right, rect.top)
            : D2D1::Point2F(rect.left, rect.bottom)), stops.Get(), &brush));
    return brush;
}
inline void gradient(ID2D1RenderTarget* target, D2D1_RECT_F rect, D2D1_COLOR_F from,
    D2D1_COLOR_F to, float radius, bool horizontal = false) {
    const auto brush = linear_brush(target, rect, from, to, horizontal);
    if (radius > 0) target->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush.Get());
    else target->FillRectangle(rect, brush.Get());
}
inline void glow(ID2D1RenderTarget* target, D2D1_RECT_F clip, D2D1_POINT_2F center,
    D2D1_SIZE_F radius, D2D1_COLOR_F color) {
    const D2D1_GRADIENT_STOP colors[]{{0, color}, {.48f, tint(color, color.a * .36f)}, {1, tint(color, 0)}};
    Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> stops;
    require_brush(target->CreateGradientStopCollection(colors, 3, &stops));
    Microsoft::WRL::ComPtr<ID2D1RadialGradientBrush> brush;
    require_brush(target->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(
        center, D2D1::Point2F(), radius.width, radius.height), stops.Get(), &brush));
    target->FillRectangle(clip, brush.Get());
}
}
