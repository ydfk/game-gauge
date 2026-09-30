#pragma once
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <filesystem>
#include <functional>
#include <string>

namespace gauge {
class SetupWindow {
public:
    static constexpr UINT completed_message = WM_APP + 1, progress_message = WM_APP + 2;
    SetupWindow(HINSTANCE instance, std::function<void()> logs, std::function<int()> progress = {},
        bool preview = false, bool allow_close = false);
    ~SetupWindow();
    HWND handle() const { return window_; }
    void progress(int value);
    void complete(DWORD result);
    int run();
    void snapshot(const std::filesystem::path& path, UINT dpi);
private:
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT message(UINT message, WPARAM wparam, LPARAM lparam);
    void paint();
    void create_target();
    void activate(int action);
    int hit(float x, float y) const;
    void text(const std::wstring& value, D2D1_RECT_F rect, IDWriteTextFormat* format, D2D1_COLOR_F color,
        DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING);
    void fill(D2D1_RECT_F rect, D2D1_COLOR_F color, float radius = 0);
    void line(float x1, float y1, float x2, float y2, D2D1_COLOR_F color, float thickness = 1);
    void circle(float x, float y, float radius, D2D1_COLOR_F color, bool filled = true, float thickness = 1);
    void button(const wchar_t* caption, D2D1_RECT_F rect, int action, bool primary, bool enabled = true);
    void resize_for_dpi(UINT dpi, const RECT* suggested = nullptr);
    HWND window_{};
    std::function<void()> logs_;
    std::function<int()> read_progress_;
    int progress_{}, hovered_{}, focused_{1};
    DWORD result_{1};
    bool running_{true}, allow_close_{}, keyboard_focus_{};
    float scale_{1};
    std::wstring destination_;
    Microsoft::WRL::ComPtr<ID2D1Factory> factory_;
    Microsoft::WRL::ComPtr<ID2D1RenderTarget> target_;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> hwnd_target_;
    Microsoft::WRL::ComPtr<IDWriteFactory> write_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> brand_, title_, body_, small_, number_;
};
}
