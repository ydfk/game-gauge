#include "window.h"
#include "theme.h"
#include "popup_menu.h"
#include "common/platform.h"
#include "host/ipc_server.h"
#include "version.h"
#include <windowsx.h>
#include <wincodec.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <algorithm>
#include <format>
#include <stdexcept>

namespace gauge {
using namespace theme;
namespace {
void check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("设置窗口图形初始化失败：" + std::to_string(hr)); }
RECT monitor_work(HMONITOR monitor) {
    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(monitor, &info);
    return info.rcWork;
}
float ui_scale(UINT dpi, const RECT& work) {
    const float width = static_cast<float>(work.right - work.left - 48) / 1180;
    const float height = static_cast<float>(work.bottom - work.top - 48) / 810;
    return std::max(.75f, std::min({dpi / 96.f, 1.4f, width, height}));
}
}
SettingsWindow::SettingsWindow(HINSTANCE instance, DWORD host_pid, Json fixture, bool open_saves) : instance_(instance), fixture_mode_(fixture.is_object()) {
    if (fixture_mode_) {
        status_ = fixture.at("status"); config_ = config_from_json(status_.at("config"));
        targets_ = fixture.value("targets", Json::array()); history_ = fixture.value("history", Json::array());
        history_detail_ = fixture.value("history_detail", Json::object()); history_count_ = history_.size();
        history_dates_ = fixture.value("dates", Json::array()); history_seconds_ = fixture.value("active_seconds", 0.0);
        const auto saves = fixture.value("saves", Json::object());
        saves_available_ = saves.value("available", false);
        saves_state_ = saves.value("state", Json::object()); saves_game_ = saves.value("game", Json::object());
        saves_rows_ = saves.value("records", Json::object()); saves_tab_ = std::clamp(saves.value("tab", 0), 0, 2);
        saves_record_ = saves.value("record", Json::object());
        saves_remote_rows_ = saves.value("remote_records", Json::object());
        saves_remote_record_ = saves.value("remote_record", Json::object());
    }
    if (host_pid) {
        host_process_.reset(OpenProcess(SYNCHRONIZE, FALSE, host_pid));
        if (!host_process_ || WaitForSingleObject(host_process_.value, 0) != WAIT_TIMEOUT)
            throw std::runtime_error("游戏仪表主程序已退出");
    }
    WNDCLASSEXW cls{sizeof(cls)};
    cls.hInstance = instance; cls.lpfnWndProc = procedure; cls.lpszClassName = L"GameGauge.Settings";
    cls.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(101), IMAGE_ICON, 32, 32, LR_SHARED));
    cls.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(101), IMAGE_ICON, 16, 16, LR_SHARED));
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); RegisterClassExW(&cls);
    POINT cursor{}; GetCursorPos(&cursor);
    const auto work = monitor_work(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY));
    const auto initial_scale = ui_scale(GetDpiForSystem(), work);
    const int window_width = static_cast<int>(1180 * initial_scale);
    const int window_height = static_cast<int>(810 * initial_scale);
    window_ = CreateWindowExW(0, cls.lpszClassName, L"游戏仪表 · 设置", settings_window_style,
        work.left + (work.right - work.left - window_width) / 2,
        work.top + (work.bottom - work.top - window_height) / 2,
        window_width, window_height, nullptr, nullptr, instance, this);
    if (!window_) throw std::runtime_error(error_text(GetLastError()));
    DeleteMenu(GetSystemMenu(window_, FALSE), SC_MINIMIZE, MF_BYCOMMAND);
    const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
    DwmSetWindowAttribute(window_, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(window_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    const COLORREF border = native_color(edge);
    DwmSetWindowAttribute(window_, DWMWA_BORDER_COLOR, &border, sizeof(border));
    const MARGINS margins{1, 1, 1, 1}; DwmExtendFrameIntoClientArea(window_, &margins);
    const auto scale = ui_scale(GetDpiForWindow(window_), work);
    const int final_width = static_cast<int>(1180 * scale), final_height = static_cast<int>(810 * scale);
    SetWindowPos(window_, nullptr, work.left + (work.right - work.left - final_width) / 2,
        work.top + (work.bottom - work.top - final_height) / 2, final_width, final_height,
        SWP_NOZORDER | SWP_NOACTIVATE);
    const D2D1_FACTORY_OPTIONS options{};
    check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), &options,
        reinterpret_cast<void**>(factory_.GetAddressOf())));
    check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(write_.GetAddressOf())));
    auto format = [&](const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight, Microsoft::WRL::ComPtr<IDWriteTextFormat>& out) {
        check(write_->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN", &out));
        out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        Microsoft::WRL::ComPtr<IDWriteInlineObject> ellipsis;
        check(write_->CreateEllipsisTrimmingSign(out.Get(), &ellipsis));
        const DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        out->SetTrimming(&trim, ellipsis.Get());
    };
    format(L"Microsoft YaHei UI", 29, DWRITE_FONT_WEIGHT_SEMI_BOLD, title_);
    format(L"Microsoft YaHei UI", 16, DWRITE_FONT_WEIGHT_SEMI_BOLD, heading_);
    format(L"Microsoft YaHei UI", 14, DWRITE_FONT_WEIGHT_NORMAL, body_);
    format(L"Microsoft YaHei UI", 12.5f, DWRITE_FONT_WEIGHT_NORMAL, small_);
    format(L"Consolas", 15, DWRITE_FONT_WEIGHT_NORMAL, mono_);
    format(L"Consolas", 22, DWRITE_FONT_WEIGHT_NORMAL, data_);
    format(L"Microsoft YaHei UI", 13, DWRITE_FONT_WEIGHT_MEDIUM, button_format_);
    button_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    motion_ = motion_enabled();
    refresh(); SetTimer(window_, 1, 1200, nullptr);
    if (!fixture_mode_) SetTimer(window_, 3, 150, nullptr);
    if (open_saves) saves_open();
}
int SettingsWindow::run() {
    ShowWindow(window_, SW_SHOW); UpdateWindow(window_);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    return static_cast<int>(message.wParam);
}
void SettingsWindow::refresh() {
    if (fixture_mode_) return;
    if (host_process_ && WaitForSingleObject(host_process_.value, 0) == WAIT_OBJECT_0) {
        DestroyWindow(window_); return;
    }
    try {
        if (page_ == 3) { request_history(); return; }
        if (page_ == 5) return;
        status_ = ipc_request({{"command", "status"}});
        if (!host_process_) host_process_.reset(OpenProcess(SYNCHRONIZE, FALSE, status_.at("host_pid").get<DWORD>()));
        config_ = config_from_json(status_.at("config"));
        targets_ = ipc_request({{"command", "targets"}}).value("targets", Json::array());
        connection_failures_ = 0; error_.clear();
    } catch (const IpcConnectionError& e) {
        if (host_process_ && WaitForSingleObject(host_process_.value, 0) == WAIT_OBJECT_0) { DestroyWindow(window_); return; }
        if (++connection_failures_ >= 3) error_ = e.what();
    } catch (const std::exception& e) { connection_failures_ = 0; error_ = e.what(); }
    if (window_) InvalidateRect(window_, nullptr, FALSE);
}
void SettingsWindow::apply() {
    try {
        ipc_request({{"command", "apply"}, {"config", config_json(config_)}});
        refresh();
    } catch (const std::exception& e) { error_ = e.what(); InvalidateRect(window_, nullptr, FALSE); }
}
void SettingsWindow::command(const char* action) {
    try { ipc_request({{"command", action}}); refresh(); }
    catch (const std::exception& e) { error_ = e.what(); InvalidateRect(window_, nullptr, FALSE); }
}
void SettingsWindow::start_cpu_sensor() {
    try {
        const auto host_pid = status_.at("host_pid").get<DWORD>();
        const auto probe = executable_dir() / L"GameGauge.CpuProbe.exe";
        if (!std::filesystem::is_regular_file(probe))
            throw std::runtime_error("当前构建没有 CPU 温度探针");
        auto folder = executable_dir();
        std::filesystem::path module;
        for (int i = 0; i < 6; ++i, folder = folder.parent_path()) {
            auto candidate = folder / L".deps/AMDFamily17-0.2.11.bin";
            if (std::filesystem::is_regular_file(candidate)) { module = std::move(candidate); break; }
            if (folder == folder.parent_path()) break;
        }
        if (module.empty()) throw std::runtime_error("缺少已校验的 PawnIO 官方 AMD 模块");
        const auto parameters = std::format(L"--serve \"{}\" {}", module.wstring(), host_pid);
        const auto result = ShellExecuteW(window_, L"runas", probe.c_str(), parameters.c_str(), nullptr, SW_HIDE);
        if (reinterpret_cast<INT_PTR>(result) <= 32)
            throw std::runtime_error("无法启动管理员 CPU 温度采集，Windows 错误 " +
                std::to_string(reinterpret_cast<INT_PTR>(result)));
        error_.clear();
    } catch (const std::exception& e) { error_ = e.what(); }
    InvalidateRect(window_, nullptr, FALSE);
}
void SettingsWindow::recreate_target() {
    RECT client{}; GetClientRect(window_, &client);
    const auto size = D2D1::SizeU(std::max(1L, client.right), std::max(1L, client.bottom));
    check(factory_->CreateHwndRenderTarget(D2D1::RenderTargetProperties(),
        D2D1::HwndRenderTargetProperties(window_, size, D2D1_PRESENT_OPTIONS_NONE), &hwnd_target_));
    // HWND 渲染目标默认已使用显示器 DPI；界面变换会再缩放一次，必须统一到 96 DPI。
    hwnd_target_->SetDpi(96.f, 96.f);
    target_ = hwnd_target_;
}
void SettingsWindow::render_to_png(const std::wstring& path, int page) {
    page_ = std::clamp(page, 0, 5);
    if (page_ == 3) request_history(true); else refresh();
    wchar_t selected[128]{}, tab[16]{}, disk[16]{};
    // 仅离屏验证模式读取这些参数，不改变正常用户配置。
    if (GetEnvironmentVariableW(L"GAMEGAUGE_SETTINGS_HISTORY_ID", selected, 128)) {
        history_selected_ = utf8(selected); request_history(true);
        GetEnvironmentVariableW(L"GAMEGAUGE_SETTINGS_HISTORY_TAB", tab, 16);
        history_tab_ = std::clamp(_wtoi(tab), 0, 3);
    }
    disk_details_ = GetEnvironmentVariableW(L"GAMEGAUGE_SETTINGS_DISKS", disk, 16) > 0;
    wchar_t dpi[16]{}, dimensions[32]{};
    const bool custom_dpi = GetEnvironmentVariableW(L"GAMEGAUGE_SETTINGS_DPI", dpi, 16) != 0;
    const bool custom_size = GetEnvironmentVariableW(L"GAMEGAUGE_SETTINGS_SIZE", dimensions, 32) != 0;
    if (custom_dpi) snapshot_dpi_ = static_cast<UINT>(std::clamp(_wtoi(dpi), 96, 192));
    if (custom_dpi || custom_size) {
        const auto factor = std::min((snapshot_dpi_ ? snapshot_dpi_ : 96) / 96.f, 1.4f);
        int width = static_cast<int>(1164 * factor), height = static_cast<int>(770 * factor);
        int requested_width{}, requested_height{};
        if (custom_size && swscanf_s(dimensions, L"%dx%d", &requested_width, &requested_height) == 2) {
            width = requested_width; height = requested_height;
        }
        SetWindowPos(window_, nullptr, 0, 0, std::clamp(width, 720, 4096), std::clamp(height, 480, 2400),
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    RECT client{}; GetClientRect(window_, &client);
    const auto width = static_cast<UINT>(client.right), height = static_cast<UINT>(client.bottom);
    Microsoft::WRL::ComPtr<IWICImagingFactory> imaging;
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&imaging)));
    Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
    check(imaging->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap));
    check(factory_->CreateWicBitmapRenderTarget(bitmap.Get(), D2D1::RenderTargetProperties(), &target_));
    target_->SetDpi(96.f, 96.f);
    motion_ = false;
    paint();
    target_.Reset();
    Microsoft::WRL::ComPtr<IWICStream> stream;
    check(imaging->CreateStream(&stream)); check(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE));
    Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
    check(imaging->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder));
    check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
    Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
    Microsoft::WRL::ComPtr<IPropertyBag2> options;
    check(encoder->CreateNewFrame(&frame, &options)); check(frame->Initialize(options.Get()));
    check(frame->SetSize(width, height));
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    check(frame->SetPixelFormat(&format)); check(frame->WriteSource(bitmap.Get(), nullptr));
    check(frame->Commit()); check(encoder->Commit());
}
void SettingsWindow::paint() {
    if (!target_) recreate_target();
    RECT client{}; GetClientRect(window_, &client);
    const auto now = GetTickCount64();
    scale_ = snapshot_dpi_ ? std::min(snapshot_dpi_ / 96.f, 1.4f) :
        ui_scale(GetDpiForWindow(window_), monitor_work(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST)));
    scale_ = std::max(scale_, std::min({1.8f, client.right / 1870.f, client.bottom / 1000.f}));
    if (!nav_toggled_) compact_nav_ = client.right / scale_ < 1116.f;
    scale_ = std::max(.1f, std::min(scale_, client.right / (compact_nav_ ? 980.f : 1116.f)));
    const float width = client.right / scale_, height = client.bottom / scale_;
    const float desired = compact_nav_ ? 68.f : 216.f;
    if (desired != sidebar_goal_) {
        sidebar_from_ = sidebar_width_; sidebar_goal_ = desired;
        sidebar_start_ = motion_ ? now : 0;
        pressed_.reset();
    }
    const auto tween = [now](ULONGLONG start) {
        if (!start) return 1.f;
        const float rest = 1.f - std::min(1.f, static_cast<float>(now - start) / motion_ms);
        return 1.f - rest * rest * rest;
    };
    const float side_progress = tween(sidebar_start_), page_progress = tween(page_start_);
    sidebar_width_ = sidebar_from_ + (desired - sidebar_from_) * side_progress;
    // 收起侧栏后，主区继续使用同一套页面坐标，命中区域同步变换。
    content_shift_ = sidebar_width_ - 236.f;
    const float page_width = width - content_shift_;
    const float left = 272 + content_shift_;
    const auto items = status_.value("components", Json::object()).value("items", Json::array());
    const bool unhealthy = page_ < 4 && std::any_of(items.begin(), items.end(), [](const Json& item) {
        const auto state = item.value("state", std::string{});
        return state == "missing" || state == "stopped" || state == "error";
    });
    viewport_bottom_ = std::max(page_top + 38, height - (!error_.empty() || unhealthy ? 53.f : 13.f));
    hotspots_.clear();
    const auto base = D2D1::Matrix3x2F::Scale(scale_, scale_);
    target_->BeginDraw(); target_->SetTransform(base);
    target_->Clear(background);
    const auto scene = D2D1::RectF(0, 0, width, height);
    glow(target_.Get(), scene, D2D1::Point2F(width * .77f, 42), D2D1::SizeF(width * .64f, height * .82f), tint(accent, .15f));
    glow(target_.Get(), scene, D2D1::Point2F(width * .18f, height * .85f), D2D1::SizeF(width * .44f, height * .49f), tint(blue, .062f));
    navigation(width, height);
    window_controls(width);
    label(page_names[page_], left, 70, width - left - 230, 48, title_.Get(), white);
    const auto active_game = wide(status_.value("snapshot", Json::object())
        .value("target", Json::object()).value("name", std::string{}));
    if (!active_game.empty()) badge(active_game, D2D1::RectF(width - 234, 80, width - 28, 108), blue);
    line(left, 120, width - 28, 120, ink_edge);
    const auto viewport = D2D1::RectF(sidebar_width_ + 1, page_top, width - 21, viewport_bottom_);
    target_->PushAxisAlignedClip(viewport, D2D1_ANTIALIAS_MODE_ALIASED);
    page_offset_ = (1.f - page_progress) * 5.f;
    target_->SetTransform(D2D1::Matrix3x2F::Translation(content_shift_, page_offset_ - scroll_) * base);
    content_bottom_ = page_top; in_page_ = true;
    switch (page_) {
    case 0: appearance_page(page_width); break;
    case 1: metrics_page(page_width); break;
    case 2: games_page(page_width); break;
    case 3: history_page(page_width); break;
    case 4: updates_page(page_width); break;
    case 5: saves_page(page_width); break;
    }
    in_page_ = false;
    target_->SetTransform(base);
    if (page_progress < 1) fill(viewport, tint(background, .57f * (1.f - page_progress)));
    target_->PopAxisAlignedClip();
    scroll_limit_ = std::max(0.f, content_bottom_ + 9.f - viewport_bottom_);
    if (scroll_ > scroll_limit_) scroll_by(0);
    if (!error_.empty()) {
        badge(wide(error_), D2D1::RectF(left, height - 43, width - 28, height - 10), amber);
    } else if (unhealthy) {
        const auto banner = D2D1::RectF(left, height - 43, width - 28, height - 10);
        fill(banner, tint(amber, .07f), control_radius);
        outline(banner, tint(amber, .20f), control_radius);
        label(L"部分采集组件异常，可能影响帧率或温度", left + 12, height - 41, width - left - 170, 30, small_.Get(), amber);
        row_action(L"检查采集组件", D2D1::RectF(width - 162, height - 42, width - 31, height - 11),
            [this](float) { select_page(4); }, true);
    }
    draw_scrollbar(width, viewport_bottom_);
    if (sidebar_width_ < 180 && pointer_inside_ && pointer_.x < sidebar_width_ && hover_index_ >= 0) {
        const bool exit = pointer_.y > height - 54;
        const int nav = static_cast<int>((pointer_.y - 126) / 52);
        if (exit || pointer_.y >= 126 && nav < static_cast<int>(std::size(page_names)))
            badge(exit ? L"退出" : page_names[nav], D2D1::RectF(sidebar_width_ + 2, pointer_.y - 15,
                sidebar_width_ + 105, pointer_.y + 15), lilac);
        else if (pointer_.y >= 76 && pointer_.y <= 106)
            badge(L"展开侧栏", D2D1::RectF(sidebar_width_ + 2, 76, sidebar_width_ + 105, 106), lilac);
    }
    if (focused_ >= 0 && focused_ < static_cast<int>(hotspots_.size())) {
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
        check(target_->CreateSolidColorBrush(lilac, &brush));
        const auto& item = hotspots_[focused_];
        const auto rect = item.rect;
        if (item.page) target_->PushAxisAlignedClip(viewport, D2D1_ANTIALIAS_MODE_ALIASED);
        target_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(rect.left + 1, rect.top + 1,
            rect.right - 1, rect.bottom - 1), 9, 9), brush.Get(), 2);
        if (item.page) target_->PopAxisAlignedClip();
    }
    const bool animate = motion_ && (side_progress < 1 || page_progress < 1 || tween(hover_start_) < 1);
    if (animate && !animation_timer_) { SetTimer(window_, 4, 16, nullptr); animation_timer_ = true; }
    if (!animate && animation_timer_) { KillTimer(window_, 4); animation_timer_ = false; }
    auto hr = target_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) { target_.Reset(); hwnd_target_.Reset(); }
    else check(hr);
}
LRESULT CALLBACK SettingsWindow::procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<SettingsWindow*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->handle(message, wparam, lparam) : DefWindowProcW(window, message, wparam, lparam);
}
LRESULT SettingsWindow::handle(UINT message, WPARAM wparam, LPARAM lparam) {
    try {
        if (const auto handled = chrome_message(message, wparam, lparam)) return *handled;
        if (const auto handled = interaction_message(message, wparam, lparam)) return *handled;
        switch (message) {
        case WM_MEASUREITEM: if (measure_menu(window_, *reinterpret_cast<MEASUREITEMSTRUCT*>(lparam))) return TRUE; break;
        case WM_DRAWITEM: if (draw_menu(window_, *reinterpret_cast<DRAWITEMSTRUCT*>(lparam))) return TRUE; break;
        case WM_GETMINMAXINFO: {
            auto info = reinterpret_cast<MINMAXINFO*>(lparam);
            const auto work = monitor_work(MonitorFromWindow(window_, MONITOR_DEFAULTTOPRIMARY));
            const auto scale = ui_scale(snapshot_dpi_ ? snapshot_dpi_ : GetDpiForWindow(window_), work);
            info->ptMinTrackSize = POINT{static_cast<LONG>(940 * scale), static_cast<LONG>(570 * scale)}; return 0;
        }
        case WM_SIZE:
            if (hwnd_target_) hwnd_target_->Resize(D2D1::SizeU(std::max(1u, static_cast<unsigned>(LOWORD(lparam))),
                std::max(1u, static_cast<unsigned>(HIWORD(lparam)))));
            InvalidateRect(window_, nullptr, FALSE); return 0;
        case WM_DPICHANGED: {
            const auto suggested = *reinterpret_cast<RECT*>(lparam);
            const auto work = monitor_work(MonitorFromRect(&suggested, MONITOR_DEFAULTTONEAREST));
            if (IsZoomed(window_)) {
                SetWindowPos(window_, nullptr, work.left, work.top, work.right - work.left, work.bottom - work.top, SWP_NOZORDER | SWP_NOACTIVATE);
                InvalidateRect(window_, nullptr, FALSE); return 0;
            }
            const auto scale = ui_scale(HIWORD(wparam), work);
            const int width = static_cast<int>(1180 * scale), height = static_cast<int>(810 * scale);
            const int x = std::clamp(suggested.left, work.left, work.right - width);
            const int y = std::clamp(suggested.top, work.top, work.bottom - height);
            SetWindowPos(window_, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(window_, nullptr, FALSE); return 0;
        }
        case open_saves_message: select_page(5); return 0;
        case WM_SETTINGCHANGE: motion_ = motion_enabled(); return 0;
        case WM_TIMER:
            if (wparam == 2) finish_history();
            else if (wparam == 3) saves_poll();
            else if (wparam == 4) InvalidateRect(window_, nullptr, FALSE);
            else refresh();
            return 0;
        case WM_KEYDOWN:
            if (wparam == VK_TAB && !hotspots_.empty()) {
                reveal_focus(GetKeyState(VK_SHIFT) < 0 ? -1 : 1);
                InvalidateRect(window_, nullptr, FALSE); return 0;
            }
            if (wparam == VK_PRIOR || wparam == VK_NEXT || wparam == VK_HOME || wparam == VK_END) {
                scroll_by(wparam == VK_PRIOR ? -viewport_bottom_ + page_top + 32 :
                    wparam == VK_NEXT ? viewport_bottom_ - page_top - 32 : wparam == VK_HOME ? -scroll_limit_ : scroll_limit_);
                return 0;
            }
            if (focused_ >= 0 && focused_ < static_cast<int>(hotspots_.size())) {
                auto item = hotspots_[focused_];
                if ((wparam == VK_LEFT || wparam == VK_RIGHT) && item.adjust) {
                    item.adjust(wparam == VK_LEFT ? -1 : 1); InvalidateRect(window_, nullptr, FALSE); return 0;
                }
                if (wparam == VK_RETURN || wparam == VK_SPACE) {
                    if (!item.adjust) item.click((item.rect.left + item.rect.right) / 2 - item.shift);
                    InvalidateRect(window_, nullptr, FALSE); return 0;
                }
            }
            break;
        case WM_SETCURSOR: {
            POINT cursor{}; GetCursorPos(&cursor); ScreenToClient(window_, &cursor);
            const float x = cursor.x / scale_, y = cursor.y / scale_;
            for (const auto& item : hotspots_) if ((!item.page || y >= page_top && y <= viewport_bottom_) &&
                x >= item.rect.left && x <= item.rect.right &&
                y >= item.rect.top && y <= item.rect.bottom) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE;
            }
            break;
        }
        case WM_PAINT: {
            PAINTSTRUCT paint_info{}; BeginPaint(window_, &paint_info);
            try { paint(); } catch (...) { EndPaint(window_, &paint_info); throw; }
            EndPaint(window_, &paint_info); return 0;
        }
        case WM_DESTROY: KillTimer(window_, 1); KillTimer(window_, 2); KillTimer(window_, 3); KillTimer(window_, 4); PostQuitMessage(0); return 0;
        }
    } catch (const std::exception& e) { error_ = e.what(); }
    return DefWindowProcW(window_, message, wparam, lparam);
}
}
