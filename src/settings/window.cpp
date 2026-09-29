#include "window.h"
#include "common/platform.h"
#include "host/ipc_server.h"
#include <windowsx.h>
#include <wincodec.h>
#include <shellapi.h>
#include <algorithm>
#include <format>
#include <stdexcept>

namespace gauge {
namespace {
const auto background = D2D1::ColorF(0x0B111B);
const auto panel = D2D1::ColorF(0x152130);
const auto panel_high = D2D1::ColorF(0x1B2B3C);
const auto edge = D2D1::ColorF(0x2A3B4D);
const auto white = D2D1::ColorF(0xF2F6FA);
const auto muted = D2D1::ColorF(0x93A7BA);
const auto mint = D2D1::ColorF(0x6BE3C3);
const auto blue = D2D1::ColorF(0x71B8FA);
const auto amber = D2D1::ColorF(0xFFB75E);
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
SettingsWindow::SettingsWindow(HINSTANCE instance) : instance_(instance) {
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
    window_ = CreateWindowExW(0, cls.lpszClassName, L"游戏仪表 · 设置", WS_OVERLAPPEDWINDOW,
        work.left + (work.right - work.left - window_width) / 2,
        work.top + (work.bottom - work.top - window_height) / 2,
        window_width, window_height, nullptr, nullptr, instance, this);
    if (!window_) throw std::runtime_error(error_text(GetLastError()));
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
    auto format = [&](float size, DWRITE_FONT_WEIGHT weight, Microsoft::WRL::ComPtr<IDWriteTextFormat>& out) {
        check(write_->CreateTextFormat(L"Segoe UI Variable", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN", &out));
        out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    };
    format(29, DWRITE_FONT_WEIGHT_SEMI_BOLD, title_);
    format(17, DWRITE_FONT_WEIGHT_SEMI_BOLD, heading_);
    format(14, DWRITE_FONT_WEIGHT_NORMAL, body_);
    format(12, DWRITE_FONT_WEIGHT_NORMAL, small_);
    format(14, DWRITE_FONT_WEIGHT_SEMI_BOLD, mono_);
    format(14, DWRITE_FONT_WEIGHT_SEMI_BOLD, button_format_);
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
    try {
        status_ = ipc_request({{"command", "status"}});
        config_ = config_from_json(status_.at("config"));
        targets_ = ipc_request({{"command", "targets"}}).value("targets", Json::array());
        const auto history = ipc_request({{"command", "history"}, {"offset", history_page_index_ * 4}, {"limit", 4}});
        history_ = history.value("history", Json::array());
        history_count_ = history.value("total", size_t{});
        error_.clear();
    } catch (const std::exception& e) { error_ = e.what(); }
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
void SettingsWindow::fill(D2D1_RECT_F rect, D2D1_COLOR_F color, float radius) {
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    check(target_->CreateSolidColorBrush(color, &brush));
    if (radius) target_->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush.Get());
    else target_->FillRectangle(rect, brush.Get());
}
void SettingsWindow::line(float x1, float y1, float x2, float y2, D2D1_COLOR_F color, float stroke) {
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    check(target_->CreateSolidColorBrush(color, &brush));
    target_->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), brush.Get(), stroke);
}
void SettingsWindow::text(const std::wstring& value, D2D1_RECT_F rect, IDWriteTextFormat* format, D2D1_COLOR_F color) {
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    check(target_->CreateSolidColorBrush(color, &brush));
    target_->DrawTextW(value.c_str(), static_cast<UINT32>(value.size()), format, rect, brush.Get());
}
void SettingsWindow::label(const std::wstring& value, float x, float y, float width, float height,
    IDWriteTextFormat* format, D2D1_COLOR_F color) {
    text(value, D2D1::RectF(x, y, x + width, y + height), format, color);
}
void SettingsWindow::add(D2D1_RECT_F rect, std::function<void(float)> click, std::function<void(int)> adjust) {
    hotspots_.push_back({rect, std::move(click), std::move(adjust)});
}
void SettingsWindow::button(const std::wstring& title, D2D1_RECT_F rect, std::function<void(float)> click, bool active) {
    fill(rect, active ? mint : panel_high, 9);
    text(title, rect, button_format_.Get(), active ? background : white);
    add(rect, std::move(click));
}
void SettingsWindow::toggle(const std::wstring& title, const std::wstring& hint, bool value,
    D2D1_RECT_F rect, std::function<void(float)> click) {
    fill(rect, panel, 12);
    label(title, rect.left + 18, rect.top + 9, rect.right - rect.left - 90, 22, body_.Get(), white);
    label(hint, rect.left + 18, rect.top + 32, rect.right - rect.left - 90, 18, small_.Get(), muted);
    auto track = D2D1::RectF(rect.right - 68, rect.top + 19, rect.right - 24, rect.top + 43);
    fill(track, value ? mint : edge, 12);
    fill(D2D1::RectF(value ? track.right - 21 : track.left + 3, track.top + 3,
        value ? track.right - 3 : track.left + 21, track.bottom - 3), white, 10);
    add(rect, std::move(click));
}
void SettingsWindow::slider(const std::wstring& title, const std::wstring& value, float selected,
    D2D1_RECT_F rect, std::function<void(float)> click) {
    fill(rect, panel, 12);
    label(title, rect.left + 18, rect.top + 12, 180, 22, body_.Get(), white);
    label(value, rect.right - 110, rect.top + 12, 92, 22, body_.Get(), mint);
    const float left = rect.left + 18, right = rect.right - 18, y = rect.bottom - 21;
    fill(D2D1::RectF(left, y, right, y + 5), edge, 3);
    fill(D2D1::RectF(left, y, left + (right - left) * selected, y + 5), mint, 3);
    const float center = left + (right - left) * selected;
    fill(D2D1::RectF(center - 7, y - 5, center + 7, y + 9), white, 7);
    auto adjust = [selected, rect, click](int direction) {
        const float amount = std::clamp(selected + direction * .05f, 0.f, 1.f);
        click(rect.left + 18 + amount * (rect.right - rect.left - 36));
    };
    add(rect, std::move(click), std::move(adjust));
}
void SettingsWindow::navigation(float width, float height) {
    fill(D2D1::RectF(0, 0, 236, height), D2D1::ColorF(0x101A27));
    fill(D2D1::RectF(27, 34, 65, 72), mint, 10);
    line(33, 58, 39, 53, background, 2.5f);
    line(39, 53, 45, 58, background, 2.5f);
    line(45, 58, 52, 53, background, 2.5f);
    line(52, 53, 59, 43, background, 2.5f);
    label(L"游戏仪表", 77, 35, 145, 27, heading_.Get(), white);
    label(L"GAMEGAUGE", 78, 61, 145, 18, small_.Get(), muted);
    const wchar_t* names[]{L"外观", L"监控项目", L"游戏与排除", L"游戏历史", L"版本与更新"};
    const wchar_t* symbols[]{L"01", L"02", L"03", L"04", L"05"};
    for (int i = 0; i < 5; ++i) {
        const float y = 132.f + i * 60;
        if (page_ == i) fill(D2D1::RectF(16, y, 220, y + 48), panel_high, 10);
        label(symbols[i], 32, y + 12, 28, 22, mono_.Get(), page_ == i ? mint : muted);
        label(names[i], 74, y + 11, 130, 24, body_.Get(), page_ == i ? white : muted);
        add(D2D1::RectF(16, y, 220, y + 48), [this, i](float) {
            page_ = i; focused_ = -1; InvalidateRect(window_, nullptr, FALSE);
        });
    }
    line(28, height - 104, 208, height - 104, edge);
    label(L"游戏仪表", 28, height - 81, 190, 20, small_.Get(), muted);
    label(error_.empty() ? L"主程序已连接" : L"主程序未连接", 28, height - 55, 180, 20,
        small_.Get(), error_.empty() ? mint : amber);
    fill(D2D1::RectF(236, 0, width, height), background);
}
void SettingsWindow::paint() {
    if (!target_) recreate_target();
    RECT client{}; GetClientRect(window_, &client);
    scale_ = ui_scale(GetDpiForWindow(window_), monitor_work(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST)));
    scale_ = std::max(.1f, std::min({scale_, client.right / 1164.f, client.bottom / 770.f}));
    const float width = client.right / scale_, height = client.bottom / scale_;
    hotspots_.clear();
    target_->BeginDraw(); target_->SetTransform(D2D1::Matrix3x2F::Scale(scale_, scale_));
    target_->Clear(background);
    navigation(width, height);
    const wchar_t* page_titles[]{L"外观", L"监控项目", L"游戏与排除", L"游戏历史", L"版本与更新"};
    const wchar_t* page_subtitles[]{L"调整监控条的样式和位置。", L"同类指标会在监控条中显示在一起。",
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
    if (!error_.empty()) label(L"连接提示：" + wide(error_), 274, height - 37, width - 310, 24, small_.Get(), amber);
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
        switch (message) {
        case WM_GETMINMAXINFO: {
            auto info = reinterpret_cast<MINMAXINFO*>(lparam);
            const auto work = monitor_work(MonitorFromWindow(window_, MONITOR_DEFAULTTOPRIMARY));
            const auto scale = ui_scale(GetDpiForWindow(window_), work);
            info->ptMinTrackSize = POINT{static_cast<LONG>(950 * scale), static_cast<LONG>(720 * scale)}; return 0;
        }
        case WM_SIZE:
            if (hwnd_target_) hwnd_target_->Resize(D2D1::SizeU(std::max(1u, static_cast<unsigned>(LOWORD(lparam))),
                std::max(1u, static_cast<unsigned>(HIWORD(lparam)))));
            InvalidateRect(window_, nullptr, FALSE); return 0;
        case WM_DPICHANGED: {
            const auto suggested = *reinterpret_cast<RECT*>(lparam);
            const auto work = monitor_work(MonitorFromRect(&suggested, MONITOR_DEFAULTTONEAREST));
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
        case WM_LBUTTONUP: {
            const float x = GET_X_LPARAM(lparam) / scale_, y = GET_Y_LPARAM(lparam) / scale_;
            for (auto item = hotspots_.rbegin(); item != hotspots_.rend(); ++item) {
                if (x >= item->rect.left && x <= item->rect.right && y >= item->rect.top && y <= item->rect.bottom) {
                    auto click = item->click; click(x); InvalidateRect(window_, nullptr, FALSE); break;
                }
            }
            return 0;
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
