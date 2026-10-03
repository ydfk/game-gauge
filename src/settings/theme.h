#pragma once
#include "common/metric_style.h"
#include <d2d1.h>

namespace gauge::theme {
inline const auto background = D2D1::ColorF(0x15191F);
inline const auto panel = D2D1::ColorF(0x1E242C);
inline const auto panel_high = D2D1::ColorF(0x28313C);
inline const auto edge = D2D1::ColorF(0x35404C);
inline const auto white = D2D1::ColorF(0xEDF3FA);
inline const auto muted = D2D1::ColorF(0x9AACC0);
inline const auto mint = D2D1::ColorF(0x75DFB0);
inline const auto blue = D2D1::ColorF(0x83BEFF);
inline const auto amber = D2D1::ColorF(0xF1CC75);
inline const auto red = D2D1::ColorF(0xFF7D8C);
inline D2D1::ColorF tint(D2D1_COLOR_F color, float alpha) { return D2D1::ColorF(color.r, color.g, color.b, alpha); }
}
