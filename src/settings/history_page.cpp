#include "window.h"
#include "theme.h"
#include "host/ipc_server.h"
#include <commdlg.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <ctime>
#include <format>
#include <fstream>

namespace gauge {
using namespace theme;
namespace {
struct Column { const char* id; const wchar_t* title; const wchar_t* unit; };
const Column columns[]{
    {"fps", L"FPS", L""}, {"cpu_load", L"CPU 占用", L"%"}, {"cpu_temperature", L"CPU 温度", L"°C"},
    {"gpu_load", L"GPU 占用", L"%"}, {"gpu_temperature", L"GPU 温度", L"°C"}, {"memory_load", L"内存占用", L"%"},
    {"disk_temperature", L"硬盘温度", L"°C"}, {"frametime", L"帧时间", L" ms"}, {"gpu_power", L"GPU 功耗", L" W"}, {"vram", L"显存用量", L" GiB"}};
std::wstring time_text(uint64_t milliseconds) {
    const time_t seconds = static_cast<time_t>(milliseconds / 1000); tm value{}; localtime_s(&value, &seconds);
    wchar_t text[64]{}; wcsftime(text, 64, L"%Y-%m-%d %H:%M:%S", &value); return text;
}
std::wstring duration(double value) {
    const auto seconds = static_cast<uint64_t>(std::max(0.0, value));
    return std::format(L"{}时{:02}分{:02}秒", seconds / 3600, seconds / 60 % 60, seconds % 60);
}
std::wstring reading(const Json& value, const wchar_t* unit = L"") {
    if (value.is_number() && value.get<double>() > 0 && value.get<double>() < .1) return L"<0.1" + std::wstring(unit);
    return value.is_number() ? std::format(L"{:.1f}{}", value.get<double>(), unit) : L"—";
}
Json stat_for(const Json& row, const std::string& id) {
    auto stat = row.value("stats", Json::object()).value(id, Json::object());
    if (!stat.empty()) return stat;
    // 旧记录只有少量汇总数据，保留展示，不补造曲线或平均温度。
    if (id == "fps") return {{"average", row.value("average_fps", Json{})}, {"maximum", row.value("maximum_fps", Json{})}, {"minimum", nullptr}};
    if (id == "cpu_temperature" || id == "gpu_temperature") return {{"average", nullptr}, {"maximum", row.value(id == "cpu_temperature" ? "cpu_max_celsius" : "gpu_max_celsius", Json{})}, {"minimum", nullptr}};
    return Json::object();
}
D2D1_COLOR_F stat_color(const std::string& id, const Json& value) {
    if (id == "vram") return value.is_number() ? white : muted;
    const auto numeric = value.is_number() ? std::optional<double>(value.get<double>()) : std::nullopt;
    return D2D1::ColorF(tone_rgb(metric_tone(id, numeric)));
}
std::wstring status_text(const Json& row) {
    const auto status = row.value("status", std::string{});
    return status == "running" ? L"进行中" : status == "interrupted" ? L"意外中断" : status == "monitor_closed" ? L"监控已关闭" : L"已结束";
}
}
void SettingsWindow::export_history(const Json& record) {
    try {
        Json full = record;
        if (!full.contains("series")) full = ipc_request({{"command", "history_detail"}, {"id", record.at("id")}}).at("record");
        wchar_t path[32768]{};
        const auto name = L"game-" + wide(record.value("id", std::string{})) + L".json";
        wcsncpy_s(path, name.c_str(), _TRUNCATE);
        OPENFILENAMEW dialog{sizeof(dialog)}; dialog.hwndOwner = window_; dialog.lpstrFile = path; dialog.nMaxFile = 32768;
        dialog.lpstrFilter = L"游戏报告 (*.json)\0*.json\0\0"; dialog.lpstrDefExt = L"json";
        dialog.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
        if (!GetSaveFileNameW(&dialog)) return;
        std::ofstream output(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
        output << full.dump(2); output.flush();
        if (!output) throw std::runtime_error("无法导出游戏报告");
    } catch (const std::exception& error) { error_ = error.what(); }
}
void SettingsWindow::history_page(float width) {
    if (!history_selected_.empty()) { history_detail(width); return; }
    const float left = 272, right = width - 28, span = right - left;
    label(history_day_.empty() ? L"全部游戏记录" : wide(history_day_), left, 146, 250, 30, heading_.Get(), white);
    label(std::format(L"{} 次游戏  ·  游玩 {}", history_count_, duration(history_seconds_)), left + 265, 146, span - 265, 30, body_.Get(), mint);
    button(L"全部", D2D1::RectF(left, 191, left + 65, 226), [this](float) { history_day_.clear(); history_page_index_ = 0; refresh(); }, history_day_.empty());
    const auto date_pages = std::max(size_t{1}, (history_dates_.size() + 4) / 5);
    history_date_page_ = std::min(history_date_page_, date_pages - 1);
    const float date_width = (span - 280) / 5;
    for (size_t i = history_date_page_ * 5; i < std::min(history_dates_.size(), history_date_page_ * 5 + 5); ++i) {
        const auto day = history_dates_[i].get<std::string>(); const float x = left + 77 + static_cast<float>(i % 5) * (date_width + 6);
        button(wide(day), D2D1::RectF(x, 191, x + date_width, 226), [this, day](float) { history_day_ = day; history_page_index_ = 0; refresh(); }, history_day_ == day);
    }
    page_buttons(right, 194, history_date_page_, date_pages);
    float y = 248;
    for (const auto& row : history_) {
        fill(D2D1::RectF(left, y, right, y + 190), panel, 10);
        label(wide(row.value("game", std::string{})), left + 16, y + 10, span - 280, 27, heading_.Get(), white);
        row_action(L"详情", D2D1::RectF(right - 178, y + 7, right - 123, y + 38), [this, id = row.at("id").get<std::string>()](float) {
            history_selected_ = id; history_tab_ = 0; history_metric_ = 0; refresh();
        }, true);
        row_action(L"导出", D2D1::RectF(right - 115, y + 7, right - 63, y + 38), [this, row](float) { export_history(row); });
        row_action(L"删除", D2D1::RectF(right - 57, y + 7, right - 10, y + 38), [this, id = row.at("id").get<std::string>()](float) {
            try { ipc_request({{"command", "delete_history"}, {"id", id}}); history_page_index_ = 0; refresh(); }
            catch (const std::exception& e) { error_ = e.what(); }
        });
        label(time_text(row.value("started_ms", 0ull)) + L"  ·  " + duration(row.value("active_seconds", 0.0)) + L"  ·  " + status_text(row),
            left + 16, y + 41, span - 32, 24, small_.Get(), muted);
        const float box = (span - 32) / 6;
        for (int i = 0; i < 6; ++i) {
            const auto& column = columns[i]; const auto stat = stat_for(row, column.id); const float x = left + 16 + i * box;
            if (i) line(x - 8, y + 82, x - 8, y + 174, D2D1::ColorF(0x2A3B4D));
            label(std::wstring(column.title) + L" · 平均", x, y + 75, box - 12, 24, small_.Get(), muted);
            label(reading(stat.value("average", Json{}), column.unit), x, y + 101, box - 12, 29, data_.Get(), stat_color(column.id, stat.value("average", Json{})));
            label(L"最高 " + reading(stat.value("maximum", Json{})), x, y + 136, box - 12, 20, small_.Get(), muted);
            label(L"最低 " + reading(stat.value("minimum", Json{})), x, y + 159, box - 12, 20, small_.Get(), muted);
        }
        y += 206;
    }
    if (history_.empty()) {
        label(L"还没有游戏记录", left, 335, span, 36, heading_.Get(), white);
        label(L"开始游戏后自动保存性能数据，结束后可查看报告和曲线。", left, 379, span, 28, body_.Get(), muted);
    }
    const auto pages = std::max(size_t{1}, (history_count_ + 1) / 2);
    if (history_page_index_ >= pages) { history_page_index_ = pages - 1; refresh(); }
    page_buttons(right, 672, history_page_index_, pages, true);
    const auto error = status_.value("snapshot", Json::object()).value("history_error", std::string{});
    if (!error.empty()) label(L"历史保存失败：" + wide(error), left, 714, span, 25, small_.Get(), amber);
}
void SettingsWindow::history_chart(const Json& row, const std::string& metric, D2D1_RECT_F rect, D2D1_COLOR_F color) {
    const auto stat = stat_for(row, metric);
    const auto column = std::find_if(std::begin(columns), std::end(columns), [&](const Column& item) { return item.id == metric; });
    if (column == std::end(columns)) return;
    label(column->title, rect.left, rect.top, 170, 24, heading_.Get(), color);
    label(L"最高 " + reading(stat.value("maximum", Json{}), column->unit) + L"   最低 " + reading(stat.value("minimum", Json{}), column->unit) +
        L"   平均 " + reading(stat.value("average", Json{}), column->unit), rect.left + 170, rect.top, rect.right - rect.left - 170, 24, small_.Get(), muted);
    const auto ids = row.value("series_metrics", Json::array());
    const auto found = std::find(ids.begin(), ids.end(), Json(metric));
    const auto series = row.value("series", Json::array());
    const size_t index = found == ids.end() ? 0 : static_cast<size_t>(std::distance(ids.begin(), found)) + 1;
    const auto plot = D2D1::RectF(rect.left, rect.top + 35, rect.right - 58, rect.bottom - 24);
    fill(plot, D2D1::ColorF(0x101A27), 4);
    if (!index || series.empty() || !stat.value("maximum", Json{}).is_number()) {
        label(L"没有该指标的有效曲线数据", plot.left + 14, plot.top + 20, plot.right - plot.left - 28, 28, small_.Get(), muted); return;
    }
    const auto minimum_value = stat.value("minimum", Json{});
    const double minimum = minimum_value.is_number() ? minimum_value.get<double>() : 0.0, maximum = stat["maximum"].get<double>();
    const double low = std::max(0.0, minimum - std::max(1.0, (maximum - minimum) * .15));
    const double high = std::max(low + 1, maximum + std::max(1.0, (maximum - minimum) * .15));
    const double end = std::max({1.0, series.back()[0].get<double>(), row.value("duration_seconds", 0.0)});
    for (int i = 0; i <= 4; ++i) {
        const float y = plot.top + (plot.bottom - plot.top) * i / 4;
        line(plot.left, y, plot.right, y, D2D1::ColorF(0x233140));
    }
    for (int i = 1; i < 12; ++i) {
        const float x = plot.left + (plot.right - plot.left) * i / 12;
        line(x, plot.top, x, plot.bottom, D2D1::ColorF(0x1B2B3C));
    }
    const auto yy = [&](double value) { return plot.bottom - static_cast<float>(std::clamp((value - low) / (high - low), 0.0, 1.0)) * (plot.bottom - plot.top); };
    const char* fields[]{"maximum", "average", "minimum"}; const auto colors = std::array{blue, mint, amber};
    std::vector<float> labeled;
    for (int i = 0; i < 3; ++i) if (stat.value(fields[i], Json{}).is_number()) {
        const auto value = stat[fields[i]].get<double>(); const float y = yy(value);
        for (float x = plot.left; x < plot.right; x += 12) line(x, y, std::min(x + 6, plot.right), y, colors[i]);
        if (std::none_of(labeled.begin(), labeled.end(), [&](float previous) { return std::abs(previous - y) < 19; })) {
            label(reading(value), plot.right + 6, y - 10, 52, 20, small_.Get(), colors[i]); labeled.push_back(y);
        }
    }
    std::vector<D2D1_POINT_2F> segment;
    const auto draw_segment = [&] {
        if (segment.empty()) return;
        Microsoft::WRL::ComPtr<ID2D1PathGeometry> geometry;
        Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
        if (SUCCEEDED(factory_->CreatePathGeometry(&geometry)) && SUCCEEDED(geometry->Open(&sink))) {
            sink->BeginFigure(D2D1::Point2F(segment.front().x, plot.bottom), D2D1_FIGURE_BEGIN_FILLED);
            for (const auto& point : segment) sink->AddLine(point);
            sink->AddLine(D2D1::Point2F(segment.back().x, plot.bottom));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            if (SUCCEEDED(sink->Close()) && SUCCEEDED(target_->CreateSolidColorBrush(D2D1::ColorF(color.r, color.g, color.b, .18f), &brush)))
                target_->FillGeometry(geometry.Get(), brush.Get());
        }
        for (size_t i = 1; i < segment.size(); ++i) line(segment[i - 1].x, segment[i - 1].y, segment[i].x, segment[i].y, color, 1.6f);
        segment.clear();
    };
    for (const auto& point : series) {
        if (!point.is_array() || point.size() <= index || !point[index].is_number()) { draw_segment(); continue; }
        const auto current = D2D1::Point2F(plot.left + static_cast<float>(point[0].get<double>() / end) * (plot.right - plot.left), yy(point[index].get<double>()));
        segment.push_back(current);
    }
    draw_segment();
    label(L"开始", plot.left, plot.bottom + 3, 100, 20, small_.Get(), muted);
    label(duration(end), plot.right - 130, plot.bottom + 3, 130, 20, small_.Get(), muted);
}
void SettingsWindow::history_detail(float width) {
    const float left = 272, right = width - 28, span = right - left;
    const auto& row = history_detail_;
    row_action(L"‹ 返回记录", D2D1::RectF(left, 142, left + 100, 174), [this](float) { history_selected_.clear(); refresh(); }, true);
    label(wide(row.value("game", std::string{})), left + 120, 142, span - 205, 30, heading_.Get(), white);
    row_action(L"导出", D2D1::RectF(right - 55, 142, right, 174), [this, row](float) { export_history(row); });
    label(time_text(row.value("started_ms", 0ull)) + L" → " + time_text(row.value("updated_ms", 0ull)) + L"  ·  游玩 " + duration(row.value("active_seconds", 0.0)), left, 182, span, 25, small_.Get(), muted);
    const wchar_t* tabs[]{L"性能报告", L"性能图表", L"会话事件", L"设备信息"};
    for (int i = 0; i < 4; ++i) button(tabs[i], D2D1::RectF(left + i * 126, 217, left + i * 126 + 114, 250), [this, i](float) { history_tab_ = i; }, history_tab_ == i);
    if (history_tab_ == 0) {
        label(L"平均值  ·  ↑最高  ↓最低", right - 270, 219, 270, 28, small_.Get(), muted);
        const int report_columns[]{0, 1, 2, 3, 4, 5, 6, 9}; const float box = (span - 30) / 4;
        for (int i = 0; i < 8; ++i) {
            const auto& column = columns[report_columns[i]]; const auto stat = stat_for(row, column.id);
            const float x = left + i % 4 * (box + 10), y = 268.f + i / 4 * 99;
            fill(D2D1::RectF(x, y, x + box, y + 88), panel, 7);
            label(column.title, x + 12, y + 5, box - 24, 22, small_.Get(), muted);
            label(reading(stat.value("average", Json{}), column.unit), x + 12, y + 29, box - 24, 26, data_.Get(), stat_color(column.id, stat.value("average", Json{})));
            label(L"↑ " + reading(stat.value("maximum", Json{})) + L"   ↓ " + reading(stat.value("minimum", Json{})), x + 12, y + 60, box - 24, 22, small_.Get(), muted);
        }
        const wchar_t* selectors[]{L"FPS", L"CPU", L"GPU", L"CPU 温度", L"内存", L"硬盘"}; const int indexes[]{0, 1, 3, 2, 5, 6};
        const float box_width = (span - 40) / 6;
        for (int i = 0; i < 6; ++i) button(selectors[i], D2D1::RectF(left + i * (box_width + 8), 477, left + i * (box_width + 8) + box_width, 506),
            [this, i](float) { history_metric_ = i; }, history_metric_ == i);
        history_chart(row, columns[indexes[history_metric_]].id, D2D1::RectF(left, 518, right, 651), blue);
        const auto hw = row.value("hardware", Json::object());
        label(L"处理器  " + wide(hw.value("cpu", std::string("旧记录未保存设备信息"))), left, 659, span, 22, small_.Get(), muted);
        std::wstring gpu_name;
        for (const auto& gpu : hw.value("gpus", Json::array())) if (gpu.value("id", std::string{}) == hw.value("selected_gpu", std::string{})) gpu_name = wide(gpu.value("name", std::string{}));
        label(L"显卡  " + (gpu_name.empty() ? L"—" : gpu_name) + L"  ·  状态 " + status_text(row), left, 685, span, 22, small_.Get(), muted);
        label(L"Low（最近60秒）  1% " + reading(row.value("latest_low1", Json{})) + L"  ·  0.1% " + reading(row.value("latest_low01", Json{})), left, 711, span, 22, small_.Get(), muted);
        if (row.value("legacy_values_hidden", false)) label(L"旧记录的无效值已隐藏；原有平均值保留，不重算缺失采样。", left, 737, span, 20, small_.Get(), muted);
    } else if (history_tab_ == 1) {
        const int sets[][3]{{0, 1, 3}, {2, 4, 6}, {5, 9, 8}};
        const wchar_t* labels[]{L"帧率与负载", L"硬件温度", L"内存与功耗"};
        for (int i = 0; i < 3; ++i) button(labels[i], D2D1::RectF(left + i * 130, 269, left + 120 + i * 130, 299),
            [this, i](float) { history_metric_ = i; }, history_metric_ % 3 == i);
        const auto colors = std::array{blue, amber, mint};
        for (int i = 0; i < 3; ++i) history_chart(row, columns[sets[history_metric_ % 3][i]].id,
            D2D1::RectF(left, 313.f + i * 139, right, 441.f + i * 139), colors[i]);
        label(L"虚线：最高 / 平均 / 最低  ·  失焦、暂停与无效读数显示为缺口", left, 730, span, 22, small_.Get(), muted);
    } else if (history_tab_ == 2) {
        label(L"会话状态  " + status_text(row), left, 277, span, 28, heading_.Get(), white);
        const auto events = row.value("events", Json::array());
        label(time_text(row.value("started_ms", 0ull)) + L"  ·  开始监控", left, 320, span, 26, body_.Get(), mint);
        float y = 356;
        const auto first = events.size() > 9 ? events.size() - 9 : 0;
        for (size_t i = first; i < events.size(); ++i) {
            const auto event = events[i]; const auto type = event.value("type", std::string{});
            label(time_text(event.value("at_ms", 0ull)) + L"  ·  " + (type == "paused" ? L"暂停采集" : type == "background" ? L"游戏失焦" : L"恢复前台采集"), left, y, span, 27, body_.Get(), muted); y += 34;
        }
        label(time_text(row.value("updated_ms", 0ull)) + L"  ·  " + status_text(row), left, y + 4, span, 27, body_.Get(), mint);
        label(L"这里只记录本应用观察到的状态变化；没有事件时保留会话起止。", left, 722, span, 24, small_.Get(), muted);
    } else {
        const auto hw = row.value("hardware", Json::object());
        label(L"会话开始时的设备信息", left, 275, span, 28, heading_.Get(), mint);
        float y = 322;
        const auto device = [&](const std::wstring& title, const std::wstring& description) {
            if (y > 716) return;
            label(title, left, y, 110, 28, small_.Get(), muted);
            label(description, left + 120, y, span - 120, 28, body_.Get(), white); y += 39;
        };
        device(L"处理器", wide(hw.value("cpu", std::string("—"))));
        device(L"逻辑处理器", std::to_wstring(hw.value("logical_processors", 0u)));
        for (const auto& gpu : hw.value("gpus", Json::array())) device(L"显卡", wide(gpu.value("name", std::string{})));
        const auto memory = hw.value("memory_total", Json::object()).value("value", Json{});
        device(L"内存容量", memory.is_number() ? reading(memory.get<double>() / 1073741824.0, L" GiB") : L"—");
        for (const auto& disk : hw.value("disks", Json::array())) device(L"硬盘", wide(disk.value("name", std::string{})));
        for (const auto& display : hw.value("displays", Json::array())) device(L"显示器", wide(display.value("name", std::string{})) +
            std::format(L"  {} × {}  ·  {} DPI{}", display.value("width", 0), display.value("height", 0), display.value("dpi", 96), display.value("hdr", false) ? L"  HDR" : L""));
    }
}
}
