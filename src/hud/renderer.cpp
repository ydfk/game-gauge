#include "renderer.h"
#include "common/platform.h"
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
}
std::vector<HudItem> hud_items(const Snapshot& s, const Config& config) {
    auto gpu = std::find_if(s.hardware.gpus.begin(), s.hardware.gpus.end(), [&](const Gpu& item) { return item.id == s.selected_gpu; });
    const auto empty = missing(State::waiting, "GPU 选择未确定");
    const auto& temperature = gpu == s.hardware.gpus.end() ? empty : gpu->temperature;
    const auto& load = gpu == s.hardware.gpus.end() ? empty : gpu->load;
    const auto& clock = gpu == s.hardware.gpus.end() ? empty : gpu->clock;
    const auto& power = gpu == s.hardware.gpus.end() ? empty : gpu->power;
    const auto& fan = gpu == s.hardware.gpus.end() ? empty : gpu->fan;
    const auto green = D2D1::ColorF(.36f, 1.f, .22f), amber = D2D1::ColorF(1.f, .65f, .13f), blue = D2D1::ColorF(.34f, .76f, 1.f), white = D2D1::ColorF(.93f, .95f, .98f);
    std::vector<HudItem> items;
    std::wstring previous_group;
    for (const auto& id : config.metrics) {
        HudItem item; item.color = white;
        if (id == "fps") { item.label = L"FPS"; item.value = number(s.fps); item.color = green; }
        else if (id == "frametime") { item.label = L"帧时间"; item.value = number(s.frametime, L" ms", 1); item.color = blue; }
        else if (id == "low1") { item.label = L"1% Low"; item.value = number(s.low1); item.color = green; }
        else if (id == "low01") { item.label = L"0.1% Low"; item.value = number(s.low01); item.color = green; }
        else if (id == "cpu_temperature") { item.label = L"CPU"; item.value = number(s.cpu_temperature, L"°C"); item.color = amber; }
        else if (id == "cpu_load") { item.label = L"CPU"; item.value = number(s.cpu_load, L"%"); item.color = green; }
        else if (id == "cpu_clock") { item.label = L"CPU"; item.value = number(s.cpu_clock, L" MHz"); item.color = blue; }
        else if (id == "gpu_temperature") { item.label = L"GPU"; item.value = number(temperature, L"°C"); item.color = amber; }
        else if (id == "gpu_load") { item.label = L"GPU"; item.value = number(load, L"%"); item.color = green; }
        else if (id == "gpu_clock") { item.label = L"GPU"; item.value = number(clock, L" MHz"); item.color = blue; }
        else if (id == "gpu_power") { item.label = L"GPU"; item.value = number(power, L" W"); item.color = amber; }
        else if (id == "gpu_fan") { item.label = L"风扇"; item.value = number(fan, L"%"); }
        else if (id == "vram") {
            item.label = L"显存";
            if (gpu != s.hardware.gpus.end() && gpu->memory_used.value && gpu->memory_total.value)
                item.value = std::format(L"{:.1f}/{:.0f}G", *gpu->memory_used.value / 1073741824.0, *gpu->memory_total.value / 1073741824.0);
            else item.value = L"—";
        }
        else if (id == "memory_load") { item.label = L"内存"; item.value = number(s.memory_load, L"%"); item.color = green; }
        else if (id == "memory_used") { item.label = L"内存"; item.value = s.memory_used.value ? std::format(L"{:.1f} GiB", *s.memory_used.value / 1073741824.0) : L"—"; }
        else if (id == "process_cpu") { item.label = L"游戏 CPU"; item.value = number(s.process_cpu, L"%"); item.color = green; }
        else if (id == "process_memory") { item.label = L"游戏内存"; item.value = s.process_memory.value ? std::format(L"{:.1f} GiB", *s.process_memory.value / 1073741824.0) : L"—"; }
        else if (id == "session") { item.label = L"运行"; const auto t = static_cast<int>(s.session_seconds); item.value = std::format(L"{:02}:{:02}:{:02}", t / 3600, t / 60 % 60, t % 60); }
        else continue;
        item.group = id.starts_with("cpu_") || id == "process_cpu" ? L"CPU" :
            id.starts_with("gpu_") || id == "vram" ? L"GPU" :
            id.starts_with("memory_") || id == "process_memory" ? L"内存" : id == "session" ? L"时间" : L"帧率";
        const auto group = item.label;
        if (previous_group == group && (group == L"CPU" || group == L"GPU")) item.label.clear();
        previous_group = group;
        items.push_back(std::move(item));
    }
    if (config.show_obs) {
        const auto value = s.obs_state == "recording" ? L"● 录制中" : s.obs_state == "paused" ? L"Ⅱ 已暂停" :
            s.obs_state == "idle" ? L"未录制" : s.obs_state == "disabled" ? L"未启用连接" : L"未连接";
        const auto color = s.obs_state == "recording" ? D2D1::ColorF(1.f, .3f, .3f) :
            s.obs_state == "paused" ? amber : D2D1::ColorF(.6f, .65f, .7f);
        items.push_back({L"OBS", value, color, L"OBS"});
    }
    return items;
}
namespace {
Microsoft::WRL::ComPtr<IDWriteTextFormat> hud_format(IDWriteFactory* write, const Config& config) {
    Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
    check(write->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_FONT_STYLE_NORMAL,
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
    for (const auto& item : items) {
        if (!previous.empty()) width += previous != item.group ? 20.f : 8.f;
        if (!item.label.empty()) width += item_width(write, format.Get(), item.label) + 4;
        width += item_width(write, format.Get(), item.value); previous = item.group;
    }
    return D2D1::SizeF(width, static_cast<float>(config.font_size * 1.35 + 2 + (config.graph ? 40 : 0)));
}
void draw_hud_items(ID2D1RenderTarget* target, IDWriteFactory* write, const std::vector<HudItem>& items, const Config& config, float width, float height) {
    const auto format = hud_format(write, config);
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    check(target->CreateSolidColorBrush(D2D1::ColorF(.025f, .035f, .05f, static_cast<float>(config.opacity)), &brush));
    target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, width, height), 3, 3), brush.Get());
    float x = 5; std::wstring previous;
    const float bottom = static_cast<float>(config.font_size * 1.35 + 2);
    for (const auto& item : items) {
        if (!previous.empty()) {
            if (previous != item.group) {
                brush->SetColor(D2D1::ColorF(.5f, .6f, .7f, .55f));
                target->DrawLine(D2D1::Point2F(x + 10, 4), D2D1::Point2F(x + 10, bottom - 4), brush.Get(), 1);
                x += 20;
            } else x += 8;
        }
        if (!item.label.empty()) {
            brush->SetColor(D2D1::ColorF(.88f, .9f, .94f));
            const auto w = item_width(write, format.Get(), item.label);
            target->DrawTextW(item.label.c_str(), static_cast<UINT32>(item.label.size()), format.Get(), D2D1::RectF(x, 0, x + w + 2, bottom), brush.Get()); x += w + 4;
        }
        brush->SetColor(item.color); const auto w = item_width(write, format.Get(), item.value);
        target->DrawTextW(item.value.c_str(), static_cast<UINT32>(item.value.size()), format.Get(), D2D1::RectF(x, 0, x + w + 2, bottom), brush.Get());
        x += w; previous = item.group;
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
