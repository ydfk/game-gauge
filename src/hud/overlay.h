#pragma once
#include "renderer.h"
#include <atomic>
#include <memory>
#include <optional>
#include <utility>
namespace gauge {
class Overlay {
public:
    explicit Overlay(HINSTANCE instance);
    ~Overlay();
    void update(const Snapshot& snapshot, const Config& config);
    void edit(bool enabled);
    bool editing() const { return editing_; }
    std::optional<bool> take_edit_result() { return std::exchange(edit_result_, std::nullopt); }
    bool capture_requested() const { return capture_requested_; }
    DWORD capture_error() const { return capture_error_; }
    const std::string& error() const { return error_; }
    HWND window() const { return window_; }
    int dragged_x() const;
    int dragged_y() const;
private:
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    HWND window_{};
    std::unique_ptr<Renderer> renderer_;
    bool editing_{}, edit_placed_{}, capture_requested_{}, affinity_enabled_{};
    std::optional<bool> edit_result_;
    DWORD capture_error_{};
    std::string error_, last_draw_;
    uint64_t oled_started_{};
    RECT bounds_{};
};
}
