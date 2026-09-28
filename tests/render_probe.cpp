#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <chrono>
#include <cmath>

LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, wparam, lparam);
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSW cls{}; cls.hInstance = instance; cls.lpfnWndProc = procedure; cls.lpszClassName = L"GameGauge.RenderProbe"; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); RegisterClassW(&cls);
    auto window = CreateWindowW(cls.lpszClassName, L"GameGauge · DX11 真实帧采集验证窗口", WS_OVERLAPPEDWINDOW, 100, 100, 1100, 700, nullptr, nullptr, instance, nullptr);
    DXGI_SWAP_CHAIN_DESC descriptor{};
    descriptor.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; descriptor.BufferDesc.Width = 1100; descriptor.BufferDesc.Height = 700;
    descriptor.SampleDesc.Count = 1; descriptor.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; descriptor.BufferCount = 2;
    descriptor.OutputWindow = window; descriptor.Windowed = TRUE; descriptor.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    Microsoft::WRL::ComPtr<ID3D11Device> device; Microsoft::WRL::ComPtr<ID3D11DeviceContext> context; Microsoft::WRL::ComPtr<IDXGISwapChain> swap;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &descriptor, &swap, &device, nullptr, &context))) return 1;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> buffer; swap->GetBuffer(0, IID_PPV_ARGS(&buffer));
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target; device->CreateRenderTargetView(buffer.Get(), nullptr, &target);
    ShowWindow(window, SW_SHOW); SetForegroundWindow(window);
    auto started = std::chrono::steady_clock::now();
    MSG message{};
    while (std::chrono::steady_clock::now() - started < std::chrono::seconds(45)) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { if (message.message == WM_QUIT) return 0; TranslateMessage(&message); DispatchMessageW(&message); }
        const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        const float color[]{.025f, .07f + static_cast<float>(std::sin(seconds)) * .025f, .12f, 1};
        context->ClearRenderTargetView(target.Get(), color); swap->Present(1, 0);
    }
    DestroyWindow(window); return 0;
}

