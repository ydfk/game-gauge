#include "window.h"
#include "version.h"
#include <windowsx.h>
#include <dwmapi.h>
#include <wincodec.h>
#include <algorithm>
#include <array>
#include <stdexcept>

namespace gauge {
namespace {
constexpr float width = 760, height = 520;
const auto background = D2D1::ColorF(0x0B111B), panel = D2D1::ColorF(0x152130);
const auto edge = D2D1::ColorF(0x2A3B4D), white = D2D1::ColorF(0xF2F6FA);
const auto muted = D2D1::ColorF(0x93A7BA), mint = D2D1::ColorF(0x6BE3C3), error = D2D1::ColorF(0xF28D9A);
const std::array<int, 6> starts{0, 25, 40, 60, 75, 90};
const std::array<const wchar_t*, 6> names{L"准备安装", L"更新程序", L"性能采集", L"温度监控", L"创建入口", L"启动应用"};
const std::array<const wchar_t*, 6> descriptions{L"正在解压文件并准备安装…", L"正在更新游戏仪表的程序文件…",
    L"正在安装所需的性能采集组件…", L"正在配置硬件温度监控…", L"正在创建桌面和开始菜单入口…", L"正在完成安装并启动游戏仪表…"};
int current_stage(int value) {
    return static_cast<int>(std::upper_bound(starts.begin(), starts.end(), value) - starts.begin()) - 1;
}
void check(HRESULT result) { if (FAILED(result)) throw std::runtime_error("安装界面初始化失败：" + std::to_string(result)); }
}
SetupWindow::SetupWindow(HINSTANCE instance, std::function<void()> logs, std::function<int()> progress, bool preview, bool allow_close)
    : logs_(std::move(logs)), read_progress_(std::move(progress)), allow_close_(allow_close) {
    wchar_t programs[32768]{}; GetEnvironmentVariableW(L"ProgramFiles", programs, 32768);
    destination_ = (std::filesystem::path(programs) / L"GameGauge").wstring();
    check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory_.GetAddressOf()));
    check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(write_.GetAddressOf())));
    const auto format = [&](const wchar_t* font, float size, DWRITE_FONT_WEIGHT weight, Microsoft::WRL::ComPtr<IDWriteTextFormat>& out) {
        check(write_->CreateTextFormat(font, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN", &out));
        out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    };
    format(L"Microsoft YaHei UI", 20, DWRITE_FONT_WEIGHT_SEMI_BOLD, brand_);
    format(L"Microsoft YaHei UI", 30, DWRITE_FONT_WEIGHT_SEMI_BOLD, title_);
    format(L"Microsoft YaHei UI", 14, DWRITE_FONT_WEIGHT_NORMAL, body_);
    format(L"Microsoft YaHei UI", 12, DWRITE_FONT_WEIGHT_NORMAL, small_);
    format(L"Segoe UI Variable", 48, DWRITE_FONT_WEIGHT_SEMI_BOLD, number_);
    WNDCLASSEXW cls{sizeof(cls)}; cls.hInstance = instance; cls.lpfnWndProc = procedure;
    cls.lpszClassName = preview ? L"GameGauge.Installer.Preview" : L"GameGauge.Installer";
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101)); cls.hIconSm = cls.hIcon;
    RegisterClassExW(&cls);
    window_ = CreateWindowExW(0, cls.lpszClassName, preview ? L"游戏仪表 · 安装界面预览" : L"游戏仪表 · 安装",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME, CW_USEDEFAULT, CW_USEDEFAULT,
        static_cast<int>(width), static_cast<int>(height), nullptr, nullptr, instance, this);
    if (!window_) throw std::runtime_error("无法创建安装窗口");
    DeleteMenu(GetSystemMenu(window_, FALSE), SC_SIZE, MF_BYCOMMAND);
    DeleteMenu(GetSystemMenu(window_, FALSE), SC_MINIMIZE, MF_BYCOMMAND);
    DeleteMenu(GetSystemMenu(window_, FALSE), SC_MAXIMIZE, MF_BYCOMMAND);
    const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
    DwmSetWindowAttribute(window_, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
    const COLORREF border = RGB(42, 59, 77);
    DwmSetWindowAttribute(window_, DWMWA_BORDER_COLOR, &border, sizeof(border));
    const MARGINS margins{1, 1, 1, 1}; DwmExtendFrameIntoClientArea(window_, &margins);
    resize_for_dpi(GetDpiForWindow(window_));
}
SetupWindow::~SetupWindow() { if (IsWindow(window_)) DestroyWindow(window_); }
void SetupWindow::resize_for_dpi(UINT dpi, const RECT* suggested) {
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(suggested ? MonitorFromRect(suggested, MONITOR_DEFAULTTONEAREST) : MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor);
    const auto& work = monitor.rcWork;
    scale_ = std::max(.5f, std::min({dpi / 96.f, (work.right - work.left - 48) / width, (work.bottom - work.top - 48) / height}));
    const int w = static_cast<int>(width * scale_), h = static_cast<int>(height * scale_);
    const int x = suggested ? std::clamp(suggested->left, work.left, work.right - w) : work.left + (work.right - work.left - w) / 2;
    const int y = suggested ? std::clamp(suggested->top, work.top, work.bottom - h) : work.top + (work.bottom - work.top - h) / 2;
    SetWindowPos(window_, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}
void SetupWindow::progress(int value) {
    if (!running_) return;
    progress_ = std::max(progress_, std::clamp(value, 0, 99));
    InvalidateRect(window_, nullptr, FALSE);
}
void SetupWindow::complete(DWORD result) {
    if (!running_) return;
    // 退出通知可能早于下一次轮询，先读最终阶段，避免失败界面落后一个步骤。
    if (read_progress_) progress(read_progress_());
    running_ = false; result_ = result; if (!result) progress_ = 100;
    KillTimer(window_, 1); SetPropW(window_, L"GameGauge.Setup.Completed", reinterpret_cast<HANDLE>(1));
    SetWindowTextW(window_, result ? L"游戏仪表 · 安装未完成" : L"游戏仪表 · 安装完成");
    EnableMenuItem(GetSystemMenu(window_, FALSE), SC_CLOSE, MF_BYCOMMAND | MF_ENABLED);
    InvalidateRect(window_, nullptr, FALSE);
}
int SetupWindow::run() {
    if (running_ && !allow_close_) EnableMenuItem(GetSystemMenu(window_, FALSE), SC_CLOSE, MF_BYCOMMAND | MF_GRAYED);
    ShowWindow(window_, SW_SHOWNORMAL); UpdateWindow(window_);
    if (read_progress_) SetTimer(window_, 1, 300, nullptr);
    MSG event{};
    while (GetMessageW(&event, nullptr, 0, 0) > 0) { TranslateMessage(&event); DispatchMessageW(&event); }
    return static_cast<int>(event.wParam);
}
void SetupWindow::create_target() {
    RECT client{}; GetClientRect(window_, &client);
    check(factory_->CreateHwndRenderTarget(D2D1::RenderTargetProperties(),
        D2D1::HwndRenderTargetProperties(window_, D2D1::SizeU(client.right, client.bottom)), &hwnd_target_));
    // 所有位置只在绘制变换中缩放一次，避免高 DPI 重复放大。
    hwnd_target_->SetDpi(96, 96); target_ = hwnd_target_;
}
void SetupWindow::text(const std::wstring& value, D2D1_RECT_F rect, IDWriteTextFormat* format, D2D1_COLOR_F color, DWRITE_TEXT_ALIGNMENT alignment) {
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush; check(target_->CreateSolidColorBrush(color, &brush));
    format->SetTextAlignment(alignment);
    target_->DrawTextW(value.c_str(), static_cast<UINT32>(value.size()), format, rect, brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}
void SetupWindow::fill(D2D1_RECT_F rect, D2D1_COLOR_F color, float radius) {
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush; check(target_->CreateSolidColorBrush(color, &brush));
    if (radius) target_->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush.Get());
    else target_->FillRectangle(rect, brush.Get());
}
void SetupWindow::line(float x1, float y1, float x2, float y2, D2D1_COLOR_F color, float thickness) {
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush; check(target_->CreateSolidColorBrush(color, &brush));
    target_->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), brush.Get(), thickness);
}
void SetupWindow::circle(float x, float y, float radius, D2D1_COLOR_F color, bool filled, float thickness) {
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush; check(target_->CreateSolidColorBrush(color, &brush));
    const auto shape = D2D1::Ellipse(D2D1::Point2F(x, y), radius, radius);
    if (filled) target_->FillEllipse(shape, brush.Get()); else target_->DrawEllipse(shape, brush.Get(), thickness);
}
void SetupWindow::button(const wchar_t* caption, D2D1_RECT_F rect, int action, bool primary, bool enabled) {
    auto color = primary ? mint : panel;
    if (!enabled) color = panel;
    else if (hovered_ == action) color = primary ? D2D1::ColorF(0x8CECD3) : D2D1::ColorF(0x223347);
    fill(rect, color, 9);
    text(caption, rect, body_.Get(), enabled ? (primary ? background : white) : muted, DWRITE_TEXT_ALIGNMENT_CENTER);
    if (!running_ && keyboard_focus_ && focused_ == action) {
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush; check(target_->CreateSolidColorBrush(white, &brush));
        target_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(rect.left + 3, rect.top + 3, rect.right - 3, rect.bottom - 3), 7, 7), brush.Get());
    }
}
void SetupWindow::paint() {
    if (!target_) create_target();
    const bool failed = !running_ && result_;
    const auto accent = failed ? error : mint;
    const int stage = current_stage(progress_);
    target_->BeginDraw(); target_->SetTransform(D2D1::Matrix3x2F::Scale(scale_, scale_)); target_->Clear(background);
    fill(D2D1::RectF(36, 32, 80, 76), mint, 12);
    line(43, 60, 50, 53, background, 2.5f); line(50, 53, 57, 59, background, 2.5f);
    line(57, 59, 64, 53, background, 2.5f); line(64, 53, 73, 42, background, 2.5f);
    text(L"游戏仪表", D2D1::RectF(96, 30, 290, 59), brand_.Get(), white);
    text(L"性能监控 · 游戏记录", D2D1::RectF(97, 59, 320, 80), small_.Get(), muted);
    const std::string version = GAMEGAUGE_VERSION;
    text(L"版本 " + std::wstring(version.begin(), version.end()) + L"  ·  Windows x64", D2D1::RectF(430, 38, 670, 65), small_.Get(), muted, DWRITE_TEXT_ALIGNMENT_TRAILING);
    if (hovered_ == 3 && (!running_ || allow_close_)) fill(D2D1::RectF(696, 12, 744, 48), D2D1::ColorF(0xB93F51), 7);
    const auto close_color = running_ && !allow_close_ ? edge : muted;
    line(715, 25, 725, 35, close_color, 1.5f); line(715, 35, 725, 25, close_color, 1.5f);
    line(36, 104, 724, 104, edge);
    text(running_ ? L"正在安装游戏仪表" : failed ? L"安装未完成" : L"安装完成", D2D1::RectF(36, 139, 520, 187), title_.Get(), white);
    const std::wstring description = running_ ? descriptions[stage] : failed ? L"安装在“" + std::wstring(names[stage]) + L"”阶段停止，请打开日志查看原因。" : L"游戏仪表已启动，打开游戏即可开始监控。";
    text(description, D2D1::RectF(37, 195, 724, 224), body_.Get(), muted);
    text(std::to_wstring(progress_) + L"%", D2D1::RectF(540, 131, 724, 189), number_.Get(), accent, DWRITE_TEXT_ALIGNMENT_TRAILING);
    fill(D2D1::RectF(36, 245, 724, 253), panel, 4);
    if (progress_ > 0) fill(D2D1::RectF(36, 245, 36 + 688 * progress_ / 100.f, 253), accent, 4);
    for (int i = 0; i < 6; ++i) {
        const float x = 72 + i * 123.2f;
        const bool done = !running_ && !failed ? true : i < stage;
        const bool active = (running_ || failed) && i == stage;
        if (i < 5) line(x + 14, 310, x + 109.2f, 310, i < stage ? mint : edge);
        if (done) {
            circle(x, 310, 11, mint); line(x - 5, 310, x - 1, 314, background, 1.6f); line(x - 1, 314, x + 5, 306, background, 1.6f);
        } else if (active) {
            circle(x, 310, 11, accent, false, 1.5f);
            if (failed) { line(x - 3, 307, x + 3, 313, error, 1.5f); line(x - 3, 313, x + 3, 307, error, 1.5f); }
            else circle(x, 310, 4, mint);
        } else circle(x, 310, 5, edge);
        text(names[i], D2D1::RectF(x - 45, 331, x + 45, 356), small_.Get(), active ? accent : done ? white : muted, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    line(36, 394, 724, 394, edge);
    text(failed ? L"安装日志会保留，方便排查问题。" : L"保留已有设置和游戏历史。", D2D1::RectF(36, 418, 425, 443), body_.Get(), muted);
    text(L"安装至 " + destination_, D2D1::RectF(36, 449, 480, 474), small_.Get(), muted);
    if (failed) {
        button(L"关闭", D2D1::RectF(484, 428, 590, 472), 2, false);
        button(L"打开日志", D2D1::RectF(604, 428, 724, 472), 1, true);
    } else button(running_ ? L"正在安装…" : L"完成", D2D1::RectF(604, 428, 724, 472), 1, !running_, !running_);
    const auto result = target_->EndDraw();
    if (result == D2DERR_RECREATE_TARGET) { target_.Reset(); hwnd_target_.Reset(); } else check(result);
}
int SetupWindow::hit(float x, float y) const {
    if (x >= 696 && x <= 744 && y >= 12 && y <= 48 && (!running_ || allow_close_)) return 3;
    if (running_) return 0;
    if (y >= 428 && y <= 472) {
        if (x >= 604 && x <= 724) return 1;
        if (result_ && x >= 484 && x <= 590) return 2;
    }
    return 0;
}
void SetupWindow::activate(int action) {
    if (action == 3 && (!running_ || allow_close_)) DestroyWindow(window_);
    else if (!running_ && action == 1 && result_) { if (logs_) logs_(); }
    else if (!running_ && action) DestroyWindow(window_);
}
LRESULT CALLBACK SetupWindow::procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto self = reinterpret_cast<SetupWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<SetupWindow*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        self->window_ = window; SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->message(message, wparam, lparam) : DefWindowProcW(window, message, wparam, lparam);
}
LRESULT SetupWindow::message(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_NCCALCSIZE: return 0;
    case WM_NCHITTEST: {
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}; ScreenToClient(window_, &point);
        const float x = point.x / scale_, y = point.y / scale_;
        return y < 104 && !(x >= 696 && y <= 48) ? HTCAPTION : HTCLIENT;
    }
    case WM_SYSCOMMAND:
        if ((wparam & 0xFFF0) == SC_SIZE || (wparam & 0xFFF0) == SC_MINIMIZE || (wparam & 0xFFF0) == SC_MAXIMIZE) return 0;
        break;
    case WM_DPICHANGED: resize_for_dpi(HIWORD(wparam), reinterpret_cast<RECT*>(lparam)); return 0;
    case WM_SIZE:
        if (hwnd_target_) hwnd_target_->Resize(D2D1::SizeU(std::max(1u, static_cast<unsigned>(LOWORD(lparam))), std::max(1u, static_cast<unsigned>(HIWORD(lparam)))));
        InvalidateRect(window_, nullptr, FALSE); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_TIMER: if (read_progress_) progress(read_progress_()); return 0;
    case progress_message: progress(static_cast<int>(wparam)); return 0;
    case completed_message: complete(static_cast<DWORD>(wparam)); return 0;
    case WM_MOUSEMOVE: {
        const int action = hit(GET_X_LPARAM(lparam) / scale_, GET_Y_LPARAM(lparam) / scale_);
        if (hovered_ != action) { hovered_ = action; InvalidateRect(window_, nullptr, FALSE); }
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window_, 0}; TrackMouseEvent(&track); return 0;
    }
    case WM_MOUSELEAVE: hovered_ = 0; InvalidateRect(window_, nullptr, FALSE); return 0;
    case WM_SETCURSOR:
        if (LOWORD(lparam) == HTCLIENT) { SetCursor(LoadCursorW(nullptr, hovered_ ? IDC_HAND : IDC_ARROW)); return TRUE; }
        break;
    case WM_LBUTTONUP: keyboard_focus_ = false; activate(hit(GET_X_LPARAM(lparam) / scale_, GET_Y_LPARAM(lparam) / scale_)); return 0;
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE && (!running_ || allow_close_)) { DestroyWindow(window_); return 0; }
        if (!running_) {
            if (wparam == VK_TAB) { focused_ = result_ && keyboard_focus_ ? (focused_ == 1 ? 2 : 1) : 1; keyboard_focus_ = true; InvalidateRect(window_, nullptr, FALSE); return 0; }
            if (wparam == VK_RETURN || wparam == VK_SPACE) { activate(focused_); return 0; }
        }
        break;
    case WM_CLOSE: if (!running_ || allow_close_) DestroyWindow(window_); return 0;
    case WM_PAINT: {
        PAINTSTRUCT info{}; BeginPaint(window_, &info);
        // 绘图失败不能中断正在写入系统文件的安装线程；后续重绘重新创建目标。
        try { paint(); } catch (...) { target_.Reset(); hwnd_target_.Reset(); }
        EndPaint(window_, &info); return 0;
    }
    case WM_DESTROY: KillTimer(window_, 1); RemovePropW(window_, L"GameGauge.Setup.Completed"); PostQuitMessage(static_cast<int>(result_)); return 0;
    }
    return DefWindowProcW(window_, message, wparam, lparam);
}
void SetupWindow::snapshot(const std::filesystem::path& path, UINT dpi) {
    scale_ = std::clamp(dpi / 96.f, .5f, 2.f);
    Microsoft::WRL::ComPtr<IWICImagingFactory> imaging;
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&imaging)));
    Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
    check(imaging->CreateBitmap(static_cast<UINT>(width * scale_), static_cast<UINT>(height * scale_), GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap));
    check(factory_->CreateWicBitmapRenderTarget(bitmap.Get(), D2D1::RenderTargetProperties(), &target_));
    target_->SetDpi(96, 96); paint();
    Microsoft::WRL::ComPtr<IWICStream> stream; check(imaging->CreateStream(&stream)); check(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE));
    Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder; check(imaging->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)); check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
    Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame; check(encoder->CreateNewFrame(&frame, nullptr)); check(frame->Initialize(nullptr));
    check(frame->WriteSource(bitmap.Get(), nullptr)); check(frame->Commit()); check(encoder->Commit());
}
}
