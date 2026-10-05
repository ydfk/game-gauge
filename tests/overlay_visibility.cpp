#include "hud/overlay.h"
#include <iostream>
#include <stdexcept>
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
int main() {
    const auto instance = GetModuleHandleW(nullptr);
    HWND game = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"HUD visibility fixture",
        WS_POPUP, 80, 80, 960, 180, nullptr, nullptr, instance, nullptr);
    HWND cover{};
    try {
        ShowWindow(game, SW_SHOWNOACTIVATE);
        gauge::Overlay overlay(instance);
        gauge::Config config; config.exclude_capture = false; config.hide_on_blur = false;
        gauge::Snapshot snapshot; snapshot.game_confirmed = true;
        snapshot.target.window = reinterpret_cast<uintptr_t>(game); snapshot.target.pid = GetCurrentProcessId();
        snapshot.target.foreground = true;
        overlay.update(snapshot, config);
        require(overlay.error().empty() && IsWindowVisible(overlay.window()), "foreground HUD missing");
        cover = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"Cover fixture", WS_POPUP,
            80, 80, 960, 180, nullptr, nullptr, instance, nullptr);
        ShowWindow(cover, SW_SHOWNOACTIVATE);
        SetWindowPos(cover, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        snapshot.target.foreground = false;
        overlay.update(snapshot, config);
        require(IsWindowVisible(overlay.window()), "background game must retain HUD");
        require(!(GetWindowLongPtrW(overlay.window(), GWL_EXSTYLE) & WS_EX_TOPMOST), "background HUD must not stay topmost");
        require(GetWindow(game, GW_HWNDPREV) == overlay.window(), "HUD must follow game Z order");
        config.hide_on_blur = true; overlay.update(snapshot, config);
        require(!IsWindowVisible(overlay.window()), "explicit hide on blur ignored");
        config.hide_on_blur = false;
        ShowWindow(game, SW_SHOWMINNOACTIVE); overlay.update(snapshot, config);
        require(!IsWindowVisible(overlay.window()), "minimized game must hide HUD");
        ShowWindow(game, SW_SHOWNOACTIVATE); overlay.update(snapshot, config);
        require(IsWindowVisible(overlay.window()), "restored game must restore HUD");
        gauge::PauseNotice notice(instance);
        config.obs_pause_notice = true; snapshot.obs_state = "paused"; snapshot.target.foreground = true;
        notice.update(snapshot, config);
        require(IsWindowVisible(notice.window()), "OBS pause notice missing");
        SendMessageW(notice.window(), WM_CLOSE, 0, 0); notice.update(snapshot, config);
        require(!IsWindowVisible(notice.window()), "dismissal must persist during current pause");
        snapshot.obs_state = "recording"; notice.update(snapshot, config);
        require(!IsWindowVisible(notice.window()), "recording must hide pause notice");
        snapshot.obs_state = "paused"; notice.update(snapshot, config);
        require(IsWindowVisible(notice.window()), "new pause must restore reminder");
        snapshot.target.foreground = false; notice.update(snapshot, config);
        require(!IsWindowVisible(notice.window()), "pause reminder must not cover other applications");
        snapshot.target.foreground = true; config.obs_pause_notice = false; notice.update(snapshot, config);
        require(!IsWindowVisible(notice.window()), "pause reminder preference ignored");
        DestroyWindow(game); game = nullptr; overlay.update(snapshot, config);
        require(!IsWindowVisible(overlay.window()), "closed game must hide HUD");
        DestroyWindow(cover);
        std::cout << "Foreground, background Z order, blur preference, minimize, restore, close passed\n";
        return 0;
    } catch (const std::exception& error) {
        if (game) DestroyWindow(game);
        if (cover) DestroyWindow(cover);
        std::cerr << error.what(); return 1;
    }
}