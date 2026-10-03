#include "renderer.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace gauge {
namespace {
using Microsoft::WRL::ComPtr;
constexpr float padding = 8, group_gap = 5, item_gap = 10, label_gap = 6;
void check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("无法绘制监控条布局"); }
ComPtr<IDWriteTextFormat> format(IDWriteFactory* write, float size, DWRITE_FONT_WEIGHT weight) {
    ComPtr<IDWriteTextFormat> result;
    check(write->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN", &result));
    result->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    result->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    return result;
}
float text_width(IDWriteFactory* write, IDWriteTextFormat* format, const std::wstring& text) {
    ComPtr<IDWriteTextLayout> layout;
    check(write->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format, 8192, 128, &layout));
    DWRITE_TEXT_METRICS metrics{}; check(layout->GetMetrics(&metrics)); return metrics.widthIncludingTrailingWhitespace;
}
struct Item { const HudItem* source; float label_width{}, value_width{}; IDWriteTextFormat* value_format{}; };
struct Group { std::wstring name; std::string status; std::vector<Item> items; float width{padding * 2}; };
struct Palette { unsigned surface, accent, label; };
Palette palette(const Group& group) {
    if (group.status == "paused") return {0xFFD46B, 0xAE6A0A, 0x60400D};
    if (group.status == "recording") return {0xBEEBD4, 0x35835A, 0x28563D};
    if (group.status == "idle") return {0xFFE8AE, 0xA2782C, 0x655022};
    if (!group.status.empty()) return {0xE1E7EC, 0x788793, 0x415361};
    if (group.name == L"帧率") return {0xACE8B7, 0x2F8D4A, 0x214B30};
    if (group.name == L"CPU") return {0xFFD3A3, 0xB77331, 0x5E371B};
    if (group.name == L"GPU") return {0xB6DEFA, 0x3C86BA, 0x204460};
    if (group.name == L"内存") return {0xD8C6F6, 0x8B65B6, 0x472C60};
    if (group.name == L"硬盘") return {0xF6C8D7, 0xAC647E, 0x63354A};
    return {0xE0E8EC, 0x7A929C, 0x445762};
}
struct Layout {
    ComPtr<IDWriteTextFormat> labels, values, primary;
    std::vector<Group> groups;
    float width{}, row_height{};
};
Layout layout(IDWriteFactory* write, const std::vector<HudItem>& items, const Config& config) {
    Layout result;
    const auto size = static_cast<float>(config.font_size);
    result.labels = format(write, size * .82f, DWRITE_FONT_WEIGHT_MEDIUM);
    result.values = format(write, size, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    result.primary = format(write, size * 1.12f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    result.row_height = std::ceil(size * 1.4f + 4);
    for (const auto& item : items) {
        if (result.groups.empty() || result.groups.back().name != item.group || !item.status.empty())
            result.groups.push_back({item.group, item.status});
        auto& group = result.groups.back();
        auto* value_format = item.label == L"FPS" ? result.primary.Get() : result.values.Get();
        const float label = item.label.empty() ? 0 : text_width(write, result.labels.Get(), item.label);
        const float value = text_width(write, value_format, item.value);
        if (!group.items.empty()) group.width += item_gap;
        group.width += value + (label > 0 ? label + label_gap : 0) + (!item.status.empty() ? 12 : 0);
        group.items.push_back({&item, label, value, value_format});
    }
    for (const auto& group : result.groups) result.width += group.width + group_gap;
    result.width = std::max(1.f, result.width - group_gap);
    return result;
}
void draw_group(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush, const Group& group,
    const Layout& layout, const Config& config, float left, float width) {
    if (width <= 0) return;
    const bool paused = group.status == "paused";
    const auto colors = palette(group);
    const float height = layout.row_height;
    const auto rect = D2D1::RectF(left + .5f, .5f, left + width - .5f, height - .5f);
    const auto round = D2D1::RoundedRect(rect, 4, 4);
    brush->SetColor(D2D1::ColorF(colors.surface,
        paused ? 1.f : static_cast<float>(config.opacity)));
    target->FillRoundedRectangle(round, brush);
    brush->SetColor(D2D1::ColorF(colors.accent, .75f));
    target->DrawRoundedRectangle(round, brush, 1);
    target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(left + 1, 5, left + 3, height - 5), 1, 1), brush);
    target->PushAxisAlignedClip(D2D1::RectF(left + 2, 0, left + width - 2, height), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    float x = left + padding;
    for (size_t i = 0; i < group.items.size(); ++i) {
        const auto& item = group.items[i]; const auto& source = *item.source;
        if (i) x += item_gap;
        if (item.label_width > 0) {
            brush->SetColor(D2D1::ColorF(colors.label));
            target->DrawTextW(source.label.c_str(), static_cast<UINT32>(source.label.size()), layout.labels.Get(),
                D2D1::RectF(x, 0, x + item.label_width + 2, height), brush);
            x += item.label_width + label_gap;
        }
        if (!source.status.empty()) {
            brush->SetColor(D2D1::ColorF(paused ? 0x60400D : source.status == "recording" ? 0xB83232 : colors.label));
            const float cy = height / 2;
            if (paused) {
                target->FillRectangle(D2D1::RectF(x + 1, cy - 4, x + 3, cy + 4), brush);
                target->FillRectangle(D2D1::RectF(x + 5, cy - 4, x + 7, cy + 4), brush);
            } else {
                const auto dot = D2D1::Ellipse(D2D1::Point2F(x + 4, cy), 3, 3);
                if (source.status == "recording") target->FillEllipse(dot, brush);
                else target->DrawEllipse(dot, brush, 1);
            }
            x += 12;
        }
        brush->SetColor(source.status.empty() ? source.color : D2D1::ColorF(0x25313A));
        target->DrawTextW(source.value.c_str(), static_cast<UINT32>(source.value.size()), item.value_format,
            D2D1::RectF(x, 0, x + item.value_width + 2, height), brush);
        x += item.value_width;
    }
    target->PopAxisAlignedClip();
}
}
D2D1_SIZE_F measure_hud_items(IDWriteFactory* write, const std::vector<HudItem>& items, const Config& config) {
    const auto measured = layout(write, items, config);
    return D2D1::SizeF(measured.width, measured.row_height + (config.graph ? 40.f : 0));
}
void draw_hud_items(ID2D1RenderTarget* target, IDWriteFactory* write, const std::vector<HudItem>& items,
    const Config& config, float width, float height) {
    const auto measured = layout(write, items, config);
    ComPtr<ID2D1SolidColorBrush> brush;
    check(target->CreateSolidColorBrush(D2D1::ColorF(0xE0E8EC, static_cast<float>(config.opacity)), &brush));
    if (config.graph) target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, measured.row_height, width, height), 4, 4), brush.Get());
    const bool obs = !measured.groups.empty() && !measured.groups.back().status.empty();
    const auto count = measured.groups.size() - (obs ? 1 : 0);
    const float obs_width = obs ? std::min(width, measured.groups.back().width) : 0;
    // 窄窗口也为末尾 OBS 保留空间，防止暂停提醒被其他指标挤出窗口。
    const float metrics_right = obs ? width - obs_width - group_gap : width;
    float x{};
    for (size_t i = 0; i < count && x < metrics_right; ++i) {
        const auto& group = measured.groups[i];
        if (group.width > metrics_right - x) break;
        draw_group(target, brush.Get(), group, measured, config, x, group.width);
        x += group.width + group_gap;
    }
    if (obs) draw_group(target, brush.Get(), measured.groups.back(), measured, config, width - obs_width, obs_width);
}
}
