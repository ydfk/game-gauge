#pragma once
#include "renderer.h"
#include "pause_notice.h"
#include <memory>
#include <optional>
#include <utility>
namespace gauge {
class Overlay {
public:
    explicit Overlay(HINSTANCE instance, std::function<void()> resume_obs = {});
    ~Overlay();
    void update(const Snapshot& snapshot, const Config& config, HWND obs_window = nullptr, ObsResumeState resume_state = {});
    void edit(bool enabled);
    bool editing() const { return editing_; }
    std::optional<bool> take_edit_result() { return std::exchange(edit_result_, std::nullopt); }
    std::optional<POINT> take_position_change() { return std::exchange(position_change_, std::nullopt); }
    std::optional<bool> take_lock_change() { return std::exchange(lock_change_, std::nullopt); }
    bool capture_requested() const { return capture_requested_; }
    DWORD capture_error() const { return capture_error_; }
    const std::string& error() const { return error_; }
    HWND window() const { return window_; }
    POINT position() const;
private:
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    void set_foreground_target(bool foreground);
    void update_input_style();
    void show_position_menu(POINT screen_point);
    void begin_drag(POINT grip);
    void move_drag(POINT screen_point);
    void end_drag();
    bool on_position_badge(POINT client_point) const;
    struct Drag { POINT grip, origin; bool moved{}; };
    HWND window_{};
    HWND target_{};
    std::function<void()> resume_obs_;
    std::unique_ptr<PauseNotice> pause_notice_;
    std::unique_ptr<PauseNotice> obs_screen_notice_;
    std::unique_ptr<Renderer> renderer_;
    bool editing_{}, edit_placed_{}, capture_requested_{}, affinity_enabled_{};
    bool locked_{}, drawn_locked_{}, foreground_target_{}, menu_open_{};
    std::optional<bool> edit_result_, lock_change_;
    std::optional<POINT> position_change_;
    std::optional<Drag> drag_;
    DWORD capture_error_{};
    std::string error_, last_draw_;
    uint64_t oled_started_{};
    RECT bounds_{};
    SIZE badge_size_{};
    UINT target_dpi_{96};
};
}
