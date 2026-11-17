#pragma once
#include "renderer.h"
#include <memory>
#include <optional>
namespace gauge {
class PauseNotice {
public:
    explicit PauseNotice(HINSTANCE instance);
    ~PauseNotice();
    HWND window() const { return window_; }
    void update(const Snapshot& snapshot, const Config& config);
    void update_on_obs_screen(HWND obs_window, bool paused, const Config& config);
private:
    struct ScreenPosition {
        RECT limits{};
        double x{.5}, y{.75};
        std::optional<POINT> drag_grip;
        bool dismiss_pressed{};
    };
    static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
    bool should_show(bool paused, bool enabled);
    void show(const RECT& bounds, UINT dpi, bool exclude_capture);
    void hide();
    void move_drag(POINT cursor);
    void end_pointer_gesture();
    HWND window_{};
    std::unique_ptr<Renderer> renderer_;
    std::optional<ScreenPosition> screen_;
    bool dismissed_{};
    uint32_t pid_{};
    SIZE drawn_{};
    UINT dpi_{};
};
}
