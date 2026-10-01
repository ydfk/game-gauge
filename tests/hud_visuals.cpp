#include "hud/renderer.h"
#include <wincodec.h>
#include <stdexcept>
#include <iostream>
using Microsoft::WRL::ComPtr;
void check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("HUD preview failed"); }
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    try {
        ComPtr<IWICImagingFactory> wic;
        check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)));
        ComPtr<IWICBitmap> bitmap; check(wic->CreateBitmap(1500, 330, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap));
        ComPtr<ID2D1Factory> d2d; check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf()));
        ComPtr<ID2D1RenderTarget> target;
        check(d2d->CreateWicBitmapRenderTarget(bitmap.Get(), D2D1::RenderTargetProperties(), &target));
        ComPtr<IDWriteFactory> write;
        check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(write.GetAddressOf())));
        gauge::Snapshot s; s.fps = gauge::available(144, "preview"); s.cpu_load = gauge::available(52, "preview");
        s.cpu_temperature = gauge::available(78, "preview"); s.memory_load = gauge::available(42, "preview");
        gauge::Gpu gpu; gpu.id = "preview"; gpu.temperature = gauge::available(67, "preview"); gpu.load = gauge::available(96, "preview");
        gpu.memory_used = gauge::available(8.5 * 1073741824, "preview"); gpu.memory_total = gauge::available(24.0 * 1073741824, "preview");
        s.hardware.gpus.push_back(gpu); s.selected_gpu = gpu.id; s.session_seconds = 1850;
        gauge::Config config; config.font_size = 18; config.show_obs = true;
        target->BeginDraw(); target->Clear(D2D1::ColorF(0x33465B));
        float y = 25;
        for (const char* state : {"recording", "paused", "idle", "disconnected", "disabled"}) {
            s.obs_state = state; const auto items = gauge::hud_items(s, config);
            if (items.empty() || items.front().status != state) throw std::runtime_error("OBS must be first and carry state");
            const auto size = gauge::measure_hud_items(write.Get(), items, config);
            if (size.width > 1450) throw std::runtime_error("HUD preview clipped");
            target->SetTransform(D2D1::Matrix3x2F::Translation(20, y));
            gauge::draw_hud_items(target.Get(), write.Get(), items, config, size.width, size.height);
            y += 60;
        }
        check(target->EndDraw());
        ComPtr<IWICStream> stream; check(wic->CreateStream(&stream)); check(stream->InitializeFromFilename(argv[1], GENERIC_WRITE));
        ComPtr<IWICBitmapEncoder> encoder; check(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder));
        check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
        ComPtr<IWICBitmapFrameEncode> frame; check(encoder->CreateNewFrame(&frame, nullptr)); check(frame->Initialize(nullptr));
        check(frame->SetSize(1500, 330)); WICPixelFormatGUID pixel = GUID_WICPixelFormat32bppBGRA; check(frame->SetPixelFormat(&pixel));
        check(frame->WriteSource(bitmap.Get(), nullptr)); check(frame->Commit()); check(encoder->Commit());
        std::cout << "OBS states rendered without clipping\n";
    } catch (const std::exception& e) { std::cerr << e.what(); return 1; }
    CoUninitialize(); return 0;
}
