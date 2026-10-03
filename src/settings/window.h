#pragma once
#include "common/config.h"
#include "common/platform.h"
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <functional>
#include <string>
#include <vector>

namespace gauge {
inline constexpr DWORD settings_window_style = WS_OVERLAPPEDWINDOW & ~WS_MINIMIZEBOX;
class SettingsWindow {
public:
    explicit SettingsWindow(HINSTANCE instance, DWORD host_pid = 0, Json fixture = Json{});
    int run();
    void render_to_png(const std::wstring& path, int page = 0);
private:
    struct Hotspot {
        D2D1_RECT_F rect;
        std::function<void(float)> click;
        std::function<void(int)> adjust;
    };
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam);
    void refresh();
    void apply();
    void command(const char* action);
    void start_cpu_sensor();
    void paint();
    void recreate_target();
    void fill(D2D1_RECT_F rect, D2D1_COLOR_F color, float radius = 0);
    void line(float x1, float y1, float x2, float y2, D2D1_COLOR_F color, float stroke = 1);
    void text(const std::wstring& value, D2D1_RECT_F rect, IDWriteTextFormat* format, D2D1_COLOR_F color);
    void label(const std::wstring& value, float x, float y, float width, float height, IDWriteTextFormat* format, D2D1_COLOR_F color);
    void button(const std::wstring& title, D2D1_RECT_F rect, std::function<void(float)> click, bool active = false, bool danger = false);
    void outline(D2D1_RECT_F rect, D2D1_COLOR_F color, float radius = 8, float stroke = 1);
    bool hovered(D2D1_RECT_F rect) const;
    bool pressed(D2D1_RECT_F rect) const;
    std::optional<LRESULT> interaction_message(UINT message, WPARAM wparam, LPARAM lparam);
    void toggle(const std::wstring& title, const std::wstring& hint, bool value, D2D1_RECT_F rect, std::function<void(float)> click);
    void slider(const std::wstring& title, const std::wstring& value, float selected, D2D1_RECT_F rect, std::function<void(float)> click);
    void navigation(float width, float height);
    void window_controls(float width);
    std::optional<LRESULT> chrome_message(UINT message, WPARAM wparam, LPARAM lparam);
    void quit_program();
    void overview(float width);
    void metrics(float width);
    void hardware(float width);
    void capture(float width);
    void appearance_page(float width);
    void metrics_page(float width);
    void games_page(float width);
    void history_page(float width);
    void game_identity(const Json& row, float x, float y, float width);
    void history_detail(float width);
    void history_chart(const Json& record, const std::string& metric, D2D1_RECT_F rect, D2D1_COLOR_F color);
    void export_history(const Json& record);
    void history_exclusion_action(const Json& record, D2D1_RECT_F rect);
    void updates_page(float width);
    void list_surface(D2D1_RECT_F rect, bool selected = false);
    void row_action(const std::wstring& title, D2D1_RECT_F rect, std::function<void(float)> action, bool accent = false);
    void page_buttons(float right, float y, size_t& page, size_t pages, bool reload = false);
    void choose_process(bool excluded);
    void metric_preview(float x, float y, float width);
    std::wstring metric_name(const std::string& id) const;
    std::wstring metric_value(const std::string& id) const;
    std::wstring metric_detail(const std::string& id) const;
    D2D1::ColorF metric_color(const std::string& id) const;
    void add(D2D1_RECT_F rect, std::function<void(float)> click, std::function<void(int)> adjust = {});
    HINSTANCE instance_{};
    UniqueHandle host_process_;
    HWND window_{};
    Microsoft::WRL::ComPtr<ID2D1Factory> factory_;
    Microsoft::WRL::ComPtr<ID2D1RenderTarget> target_;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> hwnd_target_;
    Microsoft::WRL::ComPtr<IDWriteFactory> write_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> title_, heading_, body_, small_, mono_, data_, button_format_;
    Json status_{Json::object()}, targets_{Json::array()};
    Json history_{Json::array()};
    Json history_dates_{Json::array()}, history_detail_{Json::object()};
    std::string history_day_, history_selected_;
    double history_seconds_{};
    size_t history_date_page_{};
    int history_tab_{}, history_metric_{};
    bool disk_details_{};
    size_t disk_page_{};
    Config config_;
    std::vector<Hotspot> hotspots_;
    std::string error_;
    std::string selected_metric_;
    int page_{};
    size_t target_page_{};
    size_t blacklist_page_{}, history_page_index_{};
    size_t history_count_{};
    int focused_{-1};
    float scale_{1};
    UINT snapshot_dpi_{};
    int chrome_hover_{};
    bool quitting_{};
    unsigned connection_failures_{};
    bool fixture_mode_{}, pointer_inside_{};
    D2D1_POINT_2F pointer_{};
    std::optional<Hotspot> pressed_;
};
}
