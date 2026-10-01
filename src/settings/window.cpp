#include "window.h"
#include "theme.h"
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
SettingsWindow::SettingsWindow(HINSTANCE instance, DWORD host_pid, Json fixture) : instance_(instance), fixture_mode_(fixture.is_object()) {
    if (fixture_mode_) {
        status_ = fixture.at("status"); config_ = config_from_json(status_.at("config"));
        targets_ = fixture.value("targets", Json::array()); history_ = fixture.value("history", Json::array());
        history_detail_ = fixture.value("history_detail", Json::object()); history_count_ = history_.size();
        history_dates_ = fixture.value("dates", Json::array()); history_seconds_ = fixture.value("active_seconds", 0.0);
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
    const COLORREF border = RGB(42, 59, 77);
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
    format(L"Microsoft YaHei UI", 28, DWRITE_FONT_WEIGHT_SEMI_BOLD, title_);
    format(L"Microsoft YaHei UI", 16, DWRITE_FONT_WEIGHT_SEMI_BOLD, heading_);
    format(L"Microsoft YaHei UI", 14, DWRITE_FONT_WEIGHT_NORMAL, body_);
    format(L"Microsoft YaHei UI", 12.5f, DWRITE_FONT_WEIGHT_NORMAL, small_);
    format(L"Consolas", 15, DWRITE_FONT_WEIGHT_NORMAL, mono_);
    format(L"Consolas", 22, DWRITE_FONT_WEIGHT_NORMAL, data_);
    format(L"Microsoft YaHei UI", 13, DWRITE_FONT_WEIGHT_MEDIUM, button_format_);
    button_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    refresh(); SetTimer(window_, 1, 1200, nullptr);
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
        status_ = ipc_request({{"command", "status"}});
        if (!host_process_) host_process_.reset(OpenProcess(SYNCHRONIZE, FALSE, status_.at("host_pid").get<DWORD>()));
        config_ = config_from_json(status_.at("config"));
        targets_ = ipc_request({{"command", "targets"}}).value("targets", Json::array());
        if (page_ == 3) {
            const auto history = ipc_request({{"command", "history"}, {"offset", history_page_index_ * 2}, {"limit", 2}, {"day", history_day_}});
            history_ = history.value("history", Json::array());
            history_count_ = history.value("total", size_t{});
            history_dates_ = history.value("dates", Json::array()); history_seconds_ = history.value("active_seconds", 0.0);
            if (!history_selected_.empty()) history_detail_ = ipc_request({{"command", "history_detail"}, {"id", history_selected_}}).value("record", Json::object());
        }
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
    page_ = std::clamp(page, 0, 4);
    refresh();
    wchar_t selected[128]{}, tab[16]{}, disk[16]{};
    // 仅离屏验证模式读取这些参数，不改变正常用户配置。
    if (GetEnvironmentVariableW(L"GAMEGAUGE_SETTINGS_HISTORY_ID", selected, 128)) {
        history_selected_ = utf8(selected); refresh();
        GetEnvironmentVariableW(L"GAMEGAUGE_SETTINGS_HISTORY_TAB", tab, 16);
        history_tab_ = std::clamp(_wtoi(tab), 0, 3);
    }
    disk_details_ = GetEnvironmentVariableW(L"GAMEGAUGE_SETTINGS_DISKS", disk, 16) > 0;
    wchar_t dpi[16]{};
    if (GetEnvironmentVariableW(L"GAMEGAUGE_SETTINGS_DPI", dpi, 16)) {
        snapshot_dpi_ = static_cast<UINT>(std::clamp(_wtoi(dpi), 96, 192));
        const auto factor = std::min(snapshot_dpi_ / 96.f, 1.4f);
        RECT size{0, 0, static_cast<LONG>(1164 * factor), static_cast<LONG>(770 * factor)};
        SetWindowPos(window_, nullptr, 0, 0, size.right - size.left, size.bottom - size.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    RECT client{}; GetClientRect(window_, &client);
    const auto width = static_cast<UINT>(client.right), height = static_cast<UINT>(client.bottom);
    Microsoft::WRL::ComPtr<IWICImagingFactory> imaging;
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&imaging)));
    Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
    check(imaging->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap));
    check(factory_->CreateWicBitmapRenderTarget(bitmap.Get(), D2D1::RenderTargetProperties(), &target_));
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
    scale_ = ui_scale(snapshot_dpi_ ? snapshot_dpi_ : GetDpiForWindow(window_), monitor_work(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST)));
    scale_ = std::max(.1f, std::min({scale_, client.right / 1164.f, client.bottom / 770.f}));
    const float width = client.right / scale_, height = client.bottom / scale_;
    hotspots_.clear();
    target_->BeginDraw(); target_->SetTransform(D2D1::Matrix3x2F::Scale(scale_, scale_));
    target_->Clear(background);
    navigation(width, height);
    window_controls(width);
    const wchar_t* page_titles[]{L"外观", L"监控项目", L"游戏与排除", L"游戏历史", L"版本与更新"};
    const wchar_t* page_subtitles[]{L"调整监控条的样式和位置。", L"选择常用指标，数值颜色随负载、温度和流畅度变化。",
        L"监控随游戏打开和关闭。", L"每次游戏自动保存到本机。", L"保持最新，保留你的设置和游戏历史。"};
    label(page_titles[page_], 272, 35, width - 420, 43, title_.Get(), white);
    label(page_subtitles[page_], 273, 83, width - 340, 26, body_.Get(), muted);
    line(272, 122, width - 28, 122, edge);
    switch (page_) {
    case 0: appearance_page(width); break;
    case 1: metrics_page(width); break;
    case 2: games_page(width); break;
    case 3: history_page(width); break;
    case 4: updates_page(width); break;
    }
    if (!error_.empty()) {
        fill(D2D1::RectF(272, height - 39, width - 28, height - 7), tint(amber, .10f), 6);
        label(L"提示：" + wide(error_), 284, height - 35, width - 324, 24, small_.Get(), amber);
    }
    if (focused_ >= 0 && focused_ < static_cast<int>(hotspots_.size())) {
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
        check(target_->CreateSolidColorBrush(mint, &brush));
        const auto rect = hotspots_[focused_].rect;
        target_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(rect.left + 1, rect.top + 1,
            rect.right - 1, rect.bottom - 1), 9, 9), brush.Get(), 2);
    }
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
        case WM_GETMINMAXINFO: {
            auto info = reinterpret_cast<MINMAXINFO*>(lparam);
            const auto work = monitor_work(MonitorFromWindow(window_, MONITOR_DEFAULTTOPRIMARY));
            const auto scale = ui_scale(snapshot_dpi_ ? snapshot_dpi_ : GetDpiForWindow(window_), work);
            info->ptMinTrackSize = POINT{static_cast<LONG>(950 * scale), static_cast<LONG>(720 * scale)}; return 0;
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
        case WM_TIMER: refresh(); return 0;
        case WM_KEYDOWN:
            if (wparam == VK_TAB && !hotspots_.empty()) {
                const auto size = static_cast<int>(hotspots_.size());
                focused_ = (focused_ + (GetKeyState(VK_SHIFT) < 0 ? size - 1 : 1) + size) % size;
                InvalidateRect(window_, nullptr, FALSE); return 0;
            }
            if (focused_ >= 0 && focused_ < static_cast<int>(hotspots_.size())) {
                auto item = hotspots_[focused_];
                if ((wparam == VK_LEFT || wparam == VK_RIGHT) && item.adjust) {
                    item.adjust(wparam == VK_LEFT ? -1 : 1); InvalidateRect(window_, nullptr, FALSE); return 0;
                }
                if (wparam == VK_RETURN || wparam == VK_SPACE) {
                    if (!item.adjust) item.click((item.rect.left + item.rect.right) / 2);
                    InvalidateRect(window_, nullptr, FALSE); return 0;
                }
            }
            break;
        case WM_SETCURSOR: {
            POINT cursor{}; GetCursorPos(&cursor); ScreenToClient(window_, &cursor);
            const float x = cursor.x / scale_, y = cursor.y / scale_;
            for (const auto& item : hotspots_) if (x >= item.rect.left && x <= item.rect.right &&
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
        case WM_DESTROY: KillTimer(window_, 1); PostQuitMessage(0); return 0;
        }
    } catch (const std::exception& e) { error_ = e.what(); }
    return DefWindowProcW(window_, message, wparam, lparam);
}
}
