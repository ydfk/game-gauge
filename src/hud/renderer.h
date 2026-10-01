#pragma once
#include "common/types.h"
#include <windows.h>
#include <d3d11.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <dcomp.h>
#include <wrl/client.h>

namespace gauge {
struct HudItem { std::wstring label, value; D2D1_COLOR_F color; std::wstring group; std::string status; };
std::vector<HudItem> hud_items(const Snapshot& snapshot, const Config& config);
D2D1_SIZE_F measure_hud_items(IDWriteFactory* write, const std::vector<HudItem>& items, const Config& config);
void draw_hud_items(ID2D1RenderTarget* target, IDWriteFactory* write, const std::vector<HudItem>& items, const Config& config, float width, float height);
class Renderer {
public:
    explicit Renderer(HWND window);
    SIZE measure(const Snapshot& snapshot, const Config& config, UINT dpi);
    void render(const Snapshot& snapshot, const Config& config, UINT dpi, SIZE size);
private:
    float text_width(const std::wstring& text);
    void resize(UINT width, UINT height);
    void prepare_format(float size);
    Microsoft::WRL::ComPtr<ID3D11Device> d3d_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDWriteFactory> write_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> format_;
    Microsoft::WRL::ComPtr<IDCompositionDevice> composition_;
    Microsoft::WRL::ComPtr<IDCompositionTarget> target_;
    Microsoft::WRL::ComPtr<IDCompositionVisual> visual_;
    UINT width_{1}, height_{1};
    float font_size_{};
};
}
