#pragma once
#include "renderer.h"
#include <memory>
namespace gauge {
enum class NoticeHotkeyState { inactive, registered, conflict, disabled };
class PauseNotice {
public:
    explicit PauseNotice(HINSTANCE instance);
    ~PauseNotice();
    HWND window() const { return window_; }
    NoticeHotkeyState hotkey_state() const { return hotkey_state_; }
    void update(const Snapshot& snapshot, const Config& config);
private:
    static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
    HWND window_{};
    std::unique_ptr<Renderer> renderer_;
    bool paused_{}, dismissed_{}, hotkey_{};
    NoticeHotkey active_hotkey_{};
    NoticeHotkeyState hotkey_state_{NoticeHotkeyState::inactive};
    uint64_t retry_at_{};
    std::wstring drawn_hint_;
    uint32_t pid_{};
    SIZE drawn_{};
    UINT dpi_{};
};
}
