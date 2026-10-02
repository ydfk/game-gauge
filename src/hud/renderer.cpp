#include "renderer.h"
#include "common/platform.h"
#include "common/metric_style.h"
#include <dxgi1_2.h>
#include <format>
#include <stdexcept>
#include <algorithm>
#include <cmath>

namespace gauge {
namespace {
void check(HRESULT result) { if (FAILED(result)) throw std::runtime_error("HUD 图形错误 " + std::to_string(result)); }
std::wstring number(const Metric& metric, const wchar_t* unit = L"", int digits = 0) {
    if (!metric.value || metric.state != State::valid) return L"—";
    return std::format(L"{:.{}f}{}", *metric.value, digits, unit);
}
D2D1_COLOR_F hud_color(MetricTone tone) {
    switch (tone) {
    case MetricTone::watch: return D2D1::ColorF(0xFFE09A);
    case MetricTone::high: return D2D1::ColorF(0xFFC078);
    case MetricTone::critical: return D2D1::ColorF(0xFF8790);
    case MetricTone::muted: return D2D1::ColorF(0xB6BEC9);
    default: return D2D1::ColorF(0xF6F8FC);
    }
}
D2D1_COLOR_F group_color(const std::wstring& group) {
    if (group == L"CPU") return D2D1::ColorF(0x9FDCCB);
    if (group == L"GPU") return D2D1::ColorF(0xA9CEFF);
    if (group == L"内存") return D2D1::ColorF(0xD5C4F3);
    return D2D1::ColorF(0xC8D0DC);
}
}
std::vector<HudItem> hud_items(const Snapshot& s, const Config& config) {
    auto gpu = std::find_if(s.hardware.gpus.begin(), s.hardware.gpus.end(), [&](const Gpu& item) { return item.id == s.selected_gpu; });
    const auto empty = missing(State::waiting, "GPU 选择未确定");
    const auto& temperature = gpu == s.hardware.gpus.end() ? empty : gpu->temperature;
    const auto& load = gpu == s.hardware.gpus.end() ? empty : gpu->load;
    const auto& clock = gpu == s.hardware.gpus.end() ? empty : gpu->clock;
    const auto& power = gpu == s.hardware.gpus.end() ? empty : gpu->power;
    const auto& fan = gpu == s.hardware.gpus.end() ? empty : gpu->fan;
    const auto white = D2D1::ColorF(tone_rgb(MetricTone::neutral));
    std::vector<HudItem> items;
    std::wstring previous_group;
    for (const auto& id : config.metrics) {
        HudItem item; item.color = white;
        if (id == "fps") { item.label = L"FPS"; item.value = number(s.fps); }
        else if (id == "frametime") { item.label = L"帧时间"; item.value = number(s.frametime, L" ms", 1); }
        else if (id == "low1") { item.label = L"1% Low"; item.value = number(s.low1); }
        else if (id == "low01") { item.label = L"0.1% Low"; item.value = number(s.low01); }
        else if (id == "cpu_temperature") { item.label = L"CPU"; item.value = number(s.cpu_temperature, L"°C"); }
        else if (id == "disk_temperature") { item.label = L"硬盘"; item.value = number(s.disk_temperature, L"°C"); }
        else if (id == "cpu_load") { item.label = L"CPU"; item.value = number(s.cpu_load, L"%"); }
        else if (id == "cpu_clock") { item.label = L"CPU"; item.value = number(s.cpu_clock, L" MHz"); }
        else if (id == "gpu_temperature") { item.label = L"GPU"; item.value = number(temperature, L"°C"); }
        else if (id == "gpu_load") { item.label = L"GPU"; item.value = number(load, L"%"); }
        else if (id == "gpu_clock") { item.label = L"GPU"; item.value = number(clock, L" MHz"); }
        else if (id == "gpu_power") { item.label = L"GPU"; item.value = number(power, L" W"); }
        else if (id == "gpu_fan") { item.label = L"风扇"; item.value = number(fan, L"%"); }
        else if (id == "vram") {
            item.label = L"显存";
            if (gpu != s.hardware.gpus.end() && gpu->memory_used.value && gpu->memory_total.value)
                item.value = std::format(L"{:.1f}/{:.0f}G", *gpu->memory_used.value / 1073741824.0, *gpu->memory_total.value / 1073741824.0);
            else item.value = L"—";
        }
        else if (id == "memory_load") { item.label = L"内存"; item.value = number(s.memory_load, L"%"); }
        else if (id == "memory_used") { item.label = L"内存"; item.value = s.memory_used.value ? std::format(L"{:.1f} GiB", *s.memory_used.value / 1073741824.0) : L"—"; }
        else if (id == "process_cpu") { item.label = L"游戏 CPU"; item.value = number(s.process_cpu, L"%"); }
        else if (id == "process_memory") { item.label = L"游戏内存"; item.value = s.process_memory.value ? std::format(L"{:.1f} GiB", *s.process_memory.value / 1073741824.0) : L"—"; }
        else if (id == "session") { item.label = L"游玩"; const auto t = static_cast<int>(s.session_seconds); item.value = std::format(L"{:02}:{:02}:{:02}", t / 3600, t / 60 % 60, t % 60); }
        else continue;
        item.color = hud_color(metric_tone(id, s));
        // 高 GPU 占用是正常游戏负载，常规读数保持中性，仅异常温度等使用警示色。
        if ((id == "gpu_load" || id == "cpu_load" || id == "process_cpu") && item.value != L"—")
            item.color = hud_color(MetricTone::neutral);
        item.group = id.starts_with("cpu_") || id == "process_cpu" ? L"CPU" :
            id.starts_with("gpu_") || id == "vram" ? L"GPU" :
            id.starts_with("memory_") || id == "process_memory" ? L"内存" : id == "disk_temperature" ? L"硬盘" : id == "session" ? L"时间" : L"帧率";
        const auto group = item.label;
        if (previous_group == group && (group == L"CPU" || group == L"GPU")) item.label.clear();
        previous_group = group;
        items.push_back(std::move(item));
    }
    if (config.show_obs) {
        const auto value = s.obs_state == "paused" ? L"Ⅱ 录制已暂停 · 记得恢复" : obs_label(s.obs_state);
        // 状态放在最前面，窄游戏窗口裁切时仍优先可见。
        items.insert(items.begin(), {L"OBS", value, hud_color(obs_tone(s.obs_state)), L"OBS", s.obs_state});
    }
    return items;
}
namespace {
Microsoft::WRL::ComPtr<IDWriteTextFormat> hud_format(IDWriteFactory* write, const Config& config, DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_SEMI_BOLD) {
    Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
    check(write->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, static_cast<float>(config.font_size), L"zh-CN", &format));
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP); return format;
}
float item_width(IDWriteFactory* write, IDWriteTextFormat* format, const std::wstring& text) {
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    check(write->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format, 8192, 128, &layout));
    DWRITE_TEXT_METRICS metrics{}; check(layout->GetMetrics(&metrics)); return metrics.widthIncludingTrailingWhitespace;
}
}
D2D1_SIZE_F measure_hud_items(IDWriteFactory* write, const std::vector<HudItem>& items, const Config& config) {
    const auto format = hud_format(write, config); float width = 10; std::wstring previous;
    const auto labels = hud_format(write, config, DWRITE_FONT_WEIGHT_MEDIUM);
    for (const auto& item : items) {
        if (!previous.empty()) width += previous != item.group ? 20.f : 8.f;
        if (!item.label.empty()) width += item_width(write, labels.Get(), item.label) + 6;
        width += item_width(write, format.Get(), item.value); previous = item.group;
        if (!item.status.empty()) width += 16;
    }
    return D2D1::SizeF(width, static_cast<float>(config.font_size * 1.35 + 2 + (config.graph ? 40 : 0)));
}
void draw_hud_items(ID2D1RenderTarget* target, IDWriteFactory* write, const std::vector<HudItem>& items, const Config& config, float width, float height) {
    const auto format = hud_format(write, config);
    const auto labels = hud_format(write, config, DWRITE_FONT_WEIGHT_MEDIUM);
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    check(target->CreateSolidColorBrush(D2D1::ColorF(0x20252D, static_cast<float>(config.opacity)), &brush));
    const auto surface = D2D1::RoundedRect(D2D1::RectF(.5f, .5f, width - .5f, height - .5f), 4, 4);
    target->FillRoundedRectangle(surface, brush.Get());
    brush->SetColor(D2D1::ColorF(0xA7B3C5, static_cast<float>(config.opacity) * .38f));
    target->DrawRoundedRectangle(surface, brush.Get(), 1);
    float x = 5; std::wstring previous;
    const float bottom = static_cast<float>(config.font_size * 1.35 + 2);
    for (const auto& item : items) {
        if (!previous.empty()) {
            if (previous != item.group) {
                brush->SetColor(D2D1::ColorF(0x8490A2, .48f));
                target->DrawLine(D2D1::Point2F(x + 10, 4), D2D1::Point2F(x + 10, bottom - 4), brush.Get(), 1);
                x += 20;
            } else x += 8;
        }
        const bool badge = !item.status.empty();
        if (badge) {
            const float badge_width = item_width(write, labels.Get(), item.label) + 6 + item_width(write, format.Get(), item.value) + 16;
            // 仅底色缓慢变化，暂停文字始终保持高对比度，不闪烁或消失。
            const float pulse = .5f + .5f * std::sin(static_cast<float>(GetTickCount64() % 3000) * 6.2831853f / 3000.f);
            const auto color = item.status == "paused" ? D2D1::ColorF(1.f, .75f + .08f * pulse, .30f) :
                item.status == "recording" ? D2D1::ColorF(0x44272F) : D2D1::ColorF(0x343B46);
            brush->SetColor(color);
            target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, 0, x + badge_width, bottom), 3, 3), brush.Get());
            x += 8;
        }
        if (!item.label.empty()) {
            brush->SetColor(badge ? D2D1::ColorF(item.status == "paused" ? 0x272018 : 0xF3E9E9) : group_color(item.group));
            const auto w = item_width(write, labels.Get(), item.label);
            target->DrawTextW(item.label.c_str(), static_cast<UINT32>(item.label.size()), labels.Get(), D2D1::RectF(x, 0, x + w + 2, bottom), brush.Get()); x += w + 6;
        }
        brush->SetColor(badge ? D2D1::ColorF(item.status == "paused" ? 0x272018 :
            item.status == "recording" ? 0xFFADB5 : 0xFFE0A6) : item.color); const auto w = item_width(write, format.Get(), item.value);
        target->DrawTextW(item.value.c_str(), static_cast<UINT32>(item.value.size()), format.Get(), D2D1::RectF(x, 0, x + w + 2, bottom), brush.Get());
        x += w + (badge ? 8 : 0); previous = item.group;
    }
}
Renderer::Renderer(HWND window) {
    check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, &d3d_, nullptr, nullptr));
    Microsoft::WRL::ComPtr<IDXGIDevice> device; check(d3d_.As(&device));
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter; check(device->GetAdapter(&adapter));
    Microsoft::WRL::ComPtr<IDXGIFactory2> factory; check(adapter->GetParent(IID_PPV_ARGS(&factory)));
    DXGI_SWAP_CHAIN_DESC1 descriptor{};
    descriptor.Width = descriptor.Height = 1; descriptor.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    descriptor.SampleDesc.Count = 1; descriptor.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    descriptor.BufferCount = 2; descriptor.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    descriptor.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    check(factory->CreateSwapChainForComposition(d3d_.Get(), &descriptor, nullptr, &swap_));
    Microsoft::WRL::ComPtr<ID2D1Factory1> d2d_factory;
    D2D1_FACTORY_OPTIONS options{};
    check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &options,
        reinterpret_cast<void**>(d2d_factory.GetAddressOf())));
    Microsoft::WRL::ComPtr<ID2D1Device> d2d_device; check(d2d_factory->CreateDevice(device.Get(), &d2d_device));
    check(d2d_device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context_));
    check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(write_.GetAddressOf())));
    check(DCompositionCreateDevice(device.Get(), IID_PPV_ARGS(&composition_)));
    check(composition_->CreateTargetForHwnd(window, TRUE, &target_));
    check(composition_->CreateVisual(&visual_)); check(visual_->SetContent(swap_.Get()));
    check(target_->SetRoot(visual_.Get())); check(composition_->Commit());
}
void Renderer::prepare_format(float size) {
    if (size == font_size_ && format_) return;
    format_.Reset(); font_size_ = size;
    check(write_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN", &format_));
    format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
}
float Renderer::text_width(const std::wstring& text) {
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    check(write_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format_.Get(), 4096, 128, &layout));
    DWRITE_TEXT_METRICS metrics{}; layout->GetMetrics(&metrics);
    return metrics.widthIncludingTrailingWhitespace;
}
SIZE Renderer::measure(const Snapshot& s, const Config& config, UINT dpi) {
    prepare_format(static_cast<float>(config.font_size));
    const auto size = measure_hud_items(write_.Get(), hud_items(s, config), config);
    return {static_cast<LONG>(std::ceil(size.width * dpi / 96)), static_cast<LONG>(std::ceil(size.height * dpi / 96))};
}
void Renderer::resize(UINT width, UINT height) {
    if (width == width_ && height == height_) return;
    context_->SetTarget(nullptr);
    check(swap_->ResizeBuffers(2, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, 0));
    width_ = width; height_ = height;
    Microsoft::WRL::ComPtr<IDXGISurface> surface; check(swap_->GetBuffer(0, IID_PPV_ARGS(&surface)));
    auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap;
    check(context_->CreateBitmapFromDxgiSurface(surface.Get(), &properties, &bitmap)); context_->SetTarget(bitmap.Get());
}
void Renderer::render(const Snapshot& s, const Config& config, UINT dpi, SIZE size) {
    prepare_format(static_cast<float>(config.font_size));
    resize(static_cast<UINT>(size.cx), static_cast<UINT>(size.cy));
    context_->SetDpi(static_cast<float>(dpi), static_cast<float>(dpi));
    const float width = size.cx * 96.f / dpi, height = size.cy * 96.f / dpi;
    context_->BeginDraw(); context_->Clear(D2D1::ColorF(0, 0, 0, 0));
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    check(context_->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f), &brush));
    draw_hud_items(context_.Get(), write_.Get(), hud_items(s, config), config, width, height);
    if (config.graph && s.recent_frames.size() > 1) {
        brush->SetColor(D2D1::ColorF(.34f, .76f, 1.f));
        const float spacing = (width - 12) / static_cast<float>(s.recent_frames.size() - 1);
        auto point = [&](size_t i) { return D2D1::Point2F(6 + spacing * i, height - 5 - static_cast<float>(std::min(s.recent_frames[i], 50.0) / 50 * 32)); };
        for (size_t i = 1; i < s.recent_frames.size(); ++i) context_->DrawLine(point(i - 1), point(i), brush.Get(), 1.2f);
    }
    check(context_->EndDraw()); check(swap_->Present(1, 0));
}
}
