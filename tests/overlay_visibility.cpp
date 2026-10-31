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
        UINT test_key{};
        for (UINT key = VK_F13; key <= VK_F24; ++key) {
            if (RegisterHotKey(game, 4242, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, key)) { test_key = key; break; }
        }
        require(test_key != 0, "no free test shortcut available");
        config.obs_pause_hotkey = {MOD_CONTROL | MOD_ALT | MOD_SHIFT, test_key};
        notice.update(snapshot, config);
        require(IsWindowVisible(notice.window()), "OBS pause notice missing");
        require(notice.hotkey_state() == gauge::NoticeHotkeyState::conflict, "occupied shortcut must report conflict");
        UnregisterHotKey(game, 4242);
        config.obs_pause_hotkey = {0, 0}; notice.update(snapshot, config);
        require(notice.hotkey_state() == gauge::NoticeHotkeyState::disabled, "disabled shortcut must not be registered");
        require(RegisterHotKey(game, 4242, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, test_key) != FALSE, "disabled shortcut must stay available");
        UnregisterHotKey(game, 4242);
        config.obs_pause_hotkey = {MOD_CONTROL | MOD_ALT | MOD_SHIFT, test_key}; notice.update(snapshot, config);
        require(notice.hotkey_state() == gauge::NoticeHotkeyState::registered, "custom shortcut must register after conflict resolves");
        SendMessageW(notice.window(), WM_HOTKEY, 2, 0);
        require(IsWindowVisible(notice.window()), "unrelated hotkey must not dismiss reminder");
        SendMessageW(notice.window(), WM_HOTKEY, 1, 0); notice.update(snapshot, config);
        require(!IsWindowVisible(notice.window()), "dismissal must persist during current pause");
        require(RegisterHotKey(game, 4242, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, test_key) != FALSE, "dismissal must release shortcut");
        UnregisterHotKey(game, 4242);
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
