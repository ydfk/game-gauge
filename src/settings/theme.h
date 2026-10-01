#pragma once
#include "common/metric_style.h"
#include <d2d1.h>

namespace gauge::theme {
inline const auto background = D2D1::ColorF(0x0D1520);
inline const auto panel = D2D1::ColorF(0x172332);
inline const auto panel_high = D2D1::ColorF(0x223449);
inline const auto edge = D2D1::ColorF(0x30445B);
inline const auto white = D2D1::ColorF(tone_rgb(MetricTone::neutral));
inline const auto muted = D2D1::ColorF(tone_rgb(MetricTone::muted));
inline const auto mint = D2D1::ColorF(tone_rgb(MetricTone::good));
inline const auto blue = D2D1::ColorF(tone_rgb(MetricTone::cool));
inline const auto amber = D2D1::ColorF(tone_rgb(MetricTone::watch));
inline const auto red = D2D1::ColorF(tone_rgb(MetricTone::critical));
inline D2D1::ColorF tint(D2D1_COLOR_F color, float alpha) { return D2D1::ColorF(color.r, color.g, color.b, alpha); }
}
