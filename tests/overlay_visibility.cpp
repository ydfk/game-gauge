#include "hud/overlay.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string_view>
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

LPARAM button_point(HWND window, bool resume) {
    RECT client{};
    require(GetClientRect(window, &client), "cannot find pause notice buttons");
    const auto buttons = gauge::pause_notice_buttons(static_cast<float>(client.right), static_cast<float>(client.bottom));
    const auto& rect = resume ? buttons.resume : buttons.dismiss;
    return MAKELPARAM(std::lround((rect.left + rect.right) / 2), std::lround((rect.top + rect.bottom) / 2));
}

void click(HWND window, LPARAM point) {
    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, point);
    SendMessageW(window, WM_LBUTTONUP, 0, point);
}

void test_hud_position(gauge::Overlay& overlay, const gauge::Snapshot& snapshot, gauge::Config config) {
    const auto window = overlay.window();
    const auto bounds = [&] {
        RECT rect{};
        require(GetWindowRect(window, &rect), "cannot locate HUD");
        return rect;
    };
    const auto hit = [&](int x, int y) {
        POINT point{x, y};
        require(ClientToScreen(window, &point), "cannot locate HUD hit target");
        return SendMessageW(window, WM_NCHITTEST, 0, MAKELPARAM(point.x, point.y));
    };
    const auto choose_lock_action = [&](int x, int y) {
        require(SetTimer(window, 2, 50, [](HWND hwnd, UINT, UINT_PTR id, DWORD) {
            KillTimer(hwnd, id);
            PostMessageW(hwnd, WM_KEYDOWN, VK_DOWN, 1);
            PostMessageW(hwnd, WM_KEYDOWN, VK_RETURN, 1);
        }) && SetTimer(window, 3, 600, [](HWND hwnd, UINT, UINT_PTR id, DWORD) {
            KillTimer(hwnd, id);
            EndMenu();
        }), "cannot schedule HUD menu input");
        SendMessageW(window, WM_RBUTTONUP, 0, MAKELPARAM(x, y));
        KillTimer(window, 2);
        KillTimer(window, 3);
        return overlay.take_lock_change();
    };
    config.metrics = {"fps"};
    config.anchor = 1;
    config.margin_x = 5;
    config.margin_y = 5;
    overlay.update(snapshot, config);
    const auto dpi = GetDpiForWindow(window);
    const int metric_x = MulDiv(gauge::hud_position_badge_width + 10, dpi, 96);
    require(hit(metric_x, 8) == HTCLIENT, "unlocked HUD must be draggable from the metrics");
    const auto before = bounds();
    const auto focus = GetForegroundWindow();
    click(window, MAKELPARAM(metric_x, 8));
    require(!overlay.take_position_change(), "a click must not change the placement");
    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(metric_x, 8));
    SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(metric_x + 40, 68));
    const auto dragged = bounds();
    require(dragged.left == before.left + 40 && dragged.top == before.top + 60 &&
        GetForegroundWindow() == focus, "drag must move both axes without stealing game focus");
    overlay.update(snapshot, config);
    const auto refreshed = bounds();
    require(EqualRect(&dragged, &refreshed), "telemetry refresh must not reset an active drag");
    SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(metric_x, 8));
    const auto saved = overlay.take_position_change();
    require(saved && GetCapture() != window, "releasing the drag must save placement and release capture");
    config.margin_x = saved->x;
    config.margin_y = saved->y;
    const auto lock = choose_lock_action(metric_x, 8);
    require(lock && *lock, "the context menu must lock the HUD");
    config.hud_position_locked = *lock;
    overlay.update(snapshot, config);
    require(hit(8, 8) == HTCLIENT && hit(metric_x, 8) == HTTRANSPARENT,
        "the locked badge must accept a context click while metrics let input through");
    wchar_t title[64]{};
    GetWindowTextW(window, title, ARRAYSIZE(title));
    require(std::wstring_view(title).ends_with(L"已锁定"), "locked HUD must expose its current status");
    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(8, 8));
    SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(48, 68));
    SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(48, 68));
    const auto locked = bounds();
    require(!overlay.take_position_change() && std::abs(locked.left - dragged.left) <= 1 &&
        std::abs(locked.top - dragged.top) <= 1, "locking must preserve the saved placement and block drags");
    const auto unlock = choose_lock_action(8, 8);
    require(unlock && !*unlock, "the locked badge context menu must unlock the HUD");
    config.hud_position_locked = *unlock;
    overlay.update(snapshot, config);
    require(hit(metric_x, 8) == HTCLIENT, "unlocking must allow another drag");
    require(GetForegroundWindow() == focus, "the HUD context menu must leave the game focused");
}

void test_obs_screen_notice(HINSTANCE instance, HWND obs) {
    int resumes{};
    gauge::PauseNotice notice(instance, [&] { ++resumes; });
    gauge::Config config;
    config.enabled = false;
    config.exclude_capture = false;
    notice.update_on_obs_screen(obs, true, config);
    require(!IsWindowVisible(notice.window()), "screen reminder must default off");
    config.obs_screen_pause_notice = true;
    notice.update_on_obs_screen(nullptr, true, config);
    require(!IsWindowVisible(notice.window()), "screen reminder needs an OBS window");
    const auto verify_position = [&](HMONITOR expected) {
        notice.update_on_obs_screen(obs, true, config);
        MONITORINFO monitor{sizeof(monitor)};
        RECT bounds{};
        require(IsWindowVisible(notice.window()) && GetMonitorInfoW(expected, &monitor) &&
            GetWindowRect(notice.window(), &bounds), "screen reminder missing with HUD disabled");
        const auto& work = monitor.rcWork;
        const LONG horizontal_space = (work.right - work.left) - (bounds.right - bounds.left);
        const LONG height = work.bottom - work.top, notice_height = bounds.bottom - bounds.top;
        const LONG top = std::clamp<LONG>(std::lround(height * .8 - notice_height * .5), 0, height - notice_height);
        require(bounds.left == work.left + (horizontal_space + 1) / 2 &&
            bounds.top == work.top + top, "reminder center must start at 80 percent of the OBS work area");
        return monitor.rcWork;
    };
    const auto first = MonitorFromWindow(obs, MONITOR_DEFAULTTONEAREST);
    verify_position(first);
    HMONITOR second = first;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM result) -> BOOL {
        auto& other = *reinterpret_cast<HMONITOR*>(result);
        if (monitor == other) return TRUE;
        other = monitor;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&second));
    if (first != second) {
        MONITORINFO monitor{sizeof(monitor)};
        require(GetMonitorInfoW(second, &monitor), "cannot locate second screen");
        SetWindowPos(obs, nullptr, monitor.rcWork.left + 24, monitor.rcWork.top + 24, 480, 240, SWP_NOACTIVATE | SWP_NOZORDER);
        verify_position(second);
    }
    ShowWindow(obs, SW_SHOWMINNOACTIVE);
    require(IsIconic(obs), "could not minimize OBS fixture");
    const auto work = verify_position(second);
    const auto window = notice.window();
    const auto position = [&]() {
        RECT bounds{};
        require(IsWindowVisible(window) && GetWindowRect(window, &bounds), "cannot locate visible screen reminder");
        return bounds;
    };

    RECT client{};
    GetClientRect(window, &client);
    const POINT grip{client.right / 2, client.bottom / 4};
    const auto dismiss_point = button_point(window, false);
    const auto resume_point = button_point(window, true);
    const auto before = position();
    const auto foreground = GetForegroundWindow();
    const LONG dx = (work.left - before.left) / 2, dy = (work.top - before.top) / 2;
    require(dx || dy, "display is too small for the drag fixture");
    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(grip.x, grip.y));
    SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(grip.x + dx, grip.y + dy));
    const auto dragged = position();
    require(dragged.left == before.left + dx && dragged.top == before.top + dy &&
        GetForegroundWindow() == foreground, "drag must move the reminder without changing focus");
    notice.update_on_obs_screen(obs, true, config);
    const auto refreshed = position();
    require(EqualRect(&refreshed, &dragged), "refresh must preserve the position during drag");
    SendMessageW(window, WM_LBUTTONUP, 0, resume_point);
    require(IsWindowVisible(window) && GetCapture() != window && resumes == 0, "finishing a drag must not trigger a button");
    click(window, resume_point);
    require(resumes == 1 && IsWindowVisible(window), "screen notice must request resume and wait for OBS");
    notice.update_on_obs_screen(obs, true, config, gauge::ObsResumeState::pending);
    click(window, resume_point);
    require(resumes == 1, "pending resume must disable additional clicks");
    notice.update_on_obs_screen(obs, true, config, gauge::ObsResumeState::failed);
    click(window, resume_point);
    require(resumes == 2, "a failed resume must be retryable");

    notice.update_on_obs_screen(obs, false, config);
    require(!IsWindowVisible(window), "resuming must hide screen reminder");
    notice.update_on_obs_screen(obs, true, config);
    const auto next_pause = position();
    require(EqualRect(&next_pause, &dragged), "the next pause must keep the dragged position");

    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(grip.x, grip.y));
    SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(work.left - dragged.left - 10, work.top - dragged.top - 10));
    const auto edge = position();
    require(edge.left == work.left && edge.top == work.top, "screen drag must stay within the OBS work area");
    SendMessageW(window, WM_CANCELMODE, 0, 0);
    require(GetCapture() != window, "cancelled drag must release capture");

    click(window, dismiss_point);
    notice.update_on_obs_screen(obs, true, config);
    require(!IsWindowVisible(window) && resumes == 2, "screen dismissal must persist without resuming");
    notice.update_on_obs_screen(obs, false, config);
    notice.update_on_obs_screen(obs, true, config);
    const auto after_dismissal = position();
    require(EqualRect(&after_dismissal, &edge), "dismissing the reminder must keep its position for later pauses");
}

int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
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
        test_hud_position(overlay, snapshot, config);
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
        int resumes{};
        gauge::PauseNotice notice(instance, [&] { ++resumes; });
        config.obs_pause_notice = true; snapshot.obs_state = "paused"; snapshot.target.foreground = true;
        notice.update(snapshot, config);
        require(IsWindowVisible(notice.window()), "OBS pause notice missing");
        RECT bounds{}, client{};
        POINT origin{};
        require(GetClientRect(game, &client) && ClientToScreen(game, &origin) &&
            GetWindowRect(notice.window(), &bounds), "cannot locate pause notice");
        require(bounds.left == origin.x + (client.right - (bounds.right - bounds.left)) / 2 &&
            bounds.top == origin.y + (client.bottom - (bounds.bottom - bounds.top)) / 2 &&
            bounds.right <= origin.x + client.right && bounds.bottom <= origin.y + client.bottom,
            "pause notice must fit at the center of the game client area");
        SendMessageW(notice.window(), WM_HOTKEY, 1, MAKELPARAM(MOD_CONTROL | MOD_SHIFT, VK_F10));
        SendMessageW(notice.window(), WM_CLOSE, 0, 0);
        SendMessageW(notice.window(), WM_LBUTTONUP, 0, MAKELPARAM(12, 12));
        require(IsWindowVisible(notice.window()), "only the notice dismissal area may close the reminder");
        const auto game_focus = GetForegroundWindow();
        click(notice.window(), button_point(notice.window(), true));
        require(resumes == 1 && IsWindowVisible(notice.window()) && GetForegroundWindow() == game_focus,
            "game notice must request resume without stealing focus");
        click(notice.window(), button_point(notice.window(), false));
        notice.update(snapshot, config);
        require(!IsWindowVisible(notice.window()) && resumes == 1, "dismissal must persist without resuming");
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
        test_obs_screen_notice(instance, cover);
        DestroyWindow(cover);
        std::cout << "HUD visibility, drag, position lock and OBS game/screen reminders passed\n";
        return 0;
    } catch (const std::exception& error) {
        if (game) DestroyWindow(game);
        if (cover) DestroyWindow(cover);
        std::cerr << error.what(); return 1;
    }
}
