#pragma once
#include "renderer.h"
#include <memory>
namespace gauge {
class PauseNotice {
public:
    explicit PauseNotice(HINSTANCE instance);
    ~PauseNotice();
    HWND window() const { return window_; }
    void update(const Snapshot& snapshot, const Config& config);
private:
    static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
    HWND window_{};
    std::unique_ptr<Renderer> renderer_;
    bool paused_{}, dismissed_{}, hotkey_{};
    uint32_t pid_{};
    SIZE drawn_{};
    UINT dpi_{};
};
}