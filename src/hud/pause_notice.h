#pragma once
#include "renderer.h"
#include <functional>
#include <memory>
#include <optional>
namespace gauge {
class PauseNotice {
public:
    explicit PauseNotice(HINSTANCE instance, std::function<void()> resume = {});
    ~PauseNotice();
    HWND window() const { return window_; }
    void update(const Snapshot& snapshot, const Config& config, ObsResumeState resume_state = {});
    void update_on_obs_screen(HWND obs_window, bool paused, const Config& config, ObsResumeState resume_state = {});
private:
    enum class Action { none, resume, dismiss };
    struct ScreenPosition {
        RECT limits{};
        double x{.5};
        std::optional<double> y;
        std::optional<POINT> drag_grip;
    };
    static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
    Action action_at(POINT point) const;
    bool should_show(bool paused, bool enabled);
    void show(const RECT& bounds, UINT dpi, bool exclude_capture, ObsResumeState resume_state);
    void hide();
    void move_drag(POINT cursor);
    void end_pointer_gesture();
    HWND window_{};
    std::unique_ptr<Renderer> renderer_;
    std::function<void()> resume_;
    std::optional<ScreenPosition> screen_;
    Action pressed_{};
    ObsResumeState drawn_resume_state_{};
    bool dismissed_{};
    uint32_t pid_{};
    SIZE drawn_{};
    UINT dpi_{};
};
}
