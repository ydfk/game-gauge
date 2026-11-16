#include "window.h"
#include "theme.h"
#include "popup_menu.h"
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
    return reading_color(metric_tone(id, numeric));
}
std::wstring status_text(const Json& row) {
    const auto status = row.value("status", std::string{});
    return status == "running" ? L"进行中" : status == "interrupted" ? L"意外中断" : status == "monitor_closed" ? L"监控已关闭" : L"已结束";
}
}
Json SettingsWindow::history_query() const {
    if (!history_selected_.empty()) return {{"command", "history_detail"}, {"id", history_selected_}};
    return {{"command", "history"}, {"offset", history_page_index_ * 6}, {"limit", 6}, {"day", history_day_}, {"game", history_game_}};
}
void SettingsWindow::request_history(bool synchronous) {
    if (fixture_mode_ || history_request_.valid()) return;
    pending_history_query_ = history_query();
    const bool cached_detail = !history_selected_.empty() && displayed_history_query_ == pending_history_query_ &&
        history_detail_.value("status", std::string{}) != "running";
    history_request_ = std::async(std::launch::async, [query = pending_history_query_, cached_detail] {
        Json response = cached_detail ? Json::object() : ipc_request(query);
        if (!cached_detail && !response.value("ok", false)) throw std::runtime_error(response.value("error", std::string("读取历史失败")));
        response["host_status"] = ipc_request({{"command", "status"}});
        return response;
    });
    if (synchronous) { history_request_.wait(); finish_history(); }
    else SetTimer(window_, 2, 20, nullptr);
}
void SettingsWindow::finish_history() {
    if (!history_request_.valid() || history_request_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    KillTimer(window_, 2);
    const bool current = page_ == 3 && pending_history_query_ == history_query();
    try {
        auto response = history_request_.get();
        if (current) {
            status_ = std::move(response.at("host_status"));
            config_ = config_from_json(status_.at("config"));
            if (history_selected_.empty()) {
                history_ = std::move(response.at("history"));
                history_games_ = response.value("games", Json::object());
                history_dates_ = std::move(response.at("dates"));
                history_count_ = response.at("total").get<size_t>();
                history_seconds_ = response.at("active_seconds").get<double>();
                history_page_index_ = std::min(history_page_index_, (std::max(size_t{1}, history_count_) - 1) / 6);
            } else if (response.contains("record")) history_detail_ = std::move(response.at("record"));
            displayed_history_query_ = pending_history_query_;
            error_.clear();
        }
    } catch (const std::exception& e) { if (current) error_ = e.what(); }
    if (page_ == 3 && pending_history_query_ != history_query()) request_history();
    InvalidateRect(window_, nullptr, FALSE);
}
void SettingsWindow::choose_history_game(D2D1_RECT_F rect) {
    const auto games = history_games_;
    std::vector<MenuOption> options{{L"全部游戏", history_game_.empty()}};
    for (const auto& [exe, title] : games.items()) {
        auto name = title.get<std::string>();
        auto label = wide(name.empty() || name == exe ? exe : name + " (" + exe + ")");
        options.push_back({std::move(label), exe == history_game_});
    }
    const int choice = show_options(window_, scale_, rect, options);
    if (choice < 0 || static_cast<size_t>(choice) >= options.size()) return;
    history_game_ = choice == 0 ? std::string{} : std::next(games.begin(), choice - 1).key();
    history_page_index_ = 0;
    request_history();
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
    const bool loaded = fixture_mode_ || displayed_history_query_ == history_query();
    const auto filter = D2D1::RectF(right - 210, 146, right, 182);
    button(history_game_.empty() ? L"全部游戏 ▾" : wide(history_games_.value(history_game_, history_game_)) + L" ▾",
        filter, [this, anchor = screen_rect(filter)](float) { choose_history_game(anchor); }, !history_game_.empty());
    const float summary = (span - 242) / 2;
    surface(D2D1::RectF(left, 140, left + summary, 217), true);
    surface(D2D1::RectF(left + summary + 12, 140, left + 2 * summary + 12, 217));
    label(L"游戏次数", left + 14, 149, summary - 28, 23, small_.Get(), muted);
    label(loaded ? std::to_wstring(history_count_) : L"—", left + 14, 174, summary - 28, 32, data_.Get(), lilac);
    label(L"游玩时长", left + summary + 26, 149, summary - 28, 23, small_.Get(), muted);
    label(loaded ? duration(history_seconds_) : L"—", left + summary + 26, 174, summary - 28, 32, data_.Get(), blue);
    button(L"全部日期", D2D1::RectF(left, 232, left + 82, 265), [this](float) { history_day_.clear(); history_page_index_ = 0; refresh(); }, history_day_.empty());
    const auto date_pages = std::max(size_t{1}, (history_dates_.size() + 4) / 5);
    history_date_page_ = std::min(history_date_page_, date_pages - 1);
    const float date_width = std::min(148.f, (span - 295) / 5);
    for (size_t i = history_date_page_ * 5; i < std::min(history_dates_.size(), history_date_page_ * 5 + 5); ++i) {
        const auto day = history_dates_[i].get<std::string>(); const float x = left + 92 + static_cast<float>(i % 5) * (date_width + 6);
        button(wide(day), D2D1::RectF(x, 232, x + date_width, 265), [this, day](float) { history_day_ = day; history_page_index_ = 0; refresh(); }, history_day_ == day);
    }
    page_buttons(right, 233, history_date_page_, date_pages);
    if (!loaded) {
        surface(D2D1::RectF(left, 280, right, 442));
        label(error_.empty() ? L"正在读取游戏记录…" : L"暂时无法读取游戏记录", left + 18, 331, span - 36, 36, heading_.Get(), muted);
        return;
    }
    const float table_bottom = std::max(456.f, 323.f + history_.size() * 62.f + 9);
    surface(D2D1::RectF(left, 280, right, table_bottom));
    label(L"游戏", left + 16, 285, span * .35f, 28, small_.Get(), muted);
    label(L"开始时间 / 时长", left + span * .38f, 285, span * .24f, 28, small_.Get(), muted);
    label(L"平均 FPS", left + span * .63f, 285, 92, 28, small_.Get(), muted);
    label(L"操作", right - 164, 285, 148, 28, small_.Get(), muted);
    line(left + 16, 321, right - 16, 321, ink_edge);
    float y = 323;
    for (const auto& row : history_) {
        game_identity(row, left + 16, y + 10, span * .35f - 16);
        label(time_text(row.value("started_ms", 0ull)), left + span * .38f, y + 6, span * .24f, 26, small_.Get(), white);
        label(duration(row.value("active_seconds", 0.0)) + L" · " + status_text(row),
            left + span * .38f, y + 32, span * .24f, 22, small_.Get(), muted);
        const auto fps = stat_for(row, "fps").value("average", Json{});
        label(reading(fps), left + span * .63f, y + 15, 80, 28, mono_.Get(), stat_color("fps", fps));
        row_action(L"详情", D2D1::RectF(right - 170, y + 3, right - 94, y + 33), [this, id = row.at("id").get<std::string>()](float) {
            history_selected_ = id; history_tab_ = 0; history_metric_ = 0; scroll_ = 0; focused_ = -1; refresh();
        }, true);
        row_action(L"导出", D2D1::RectF(right - 86, y + 3, right - 10, y + 33), [this, row](float) { export_history(row); });
        row_action(L"删除", D2D1::RectF(right - 86, y + 32, right - 10, y + 59), [this, id = row.at("id").get<std::string>(),
            name = wide(row.value("display_name", row.value("game", std::string{}))), started = time_text(row.value("started_ms", 0ull))](float) {
            const auto prompt = std::format(L"删除“{}”在 {} 开始的这条游戏记录？\n\n删除后无法恢复。", name, started);
            if (MessageBoxW(window_, prompt.c_str(), L"删除游戏记录", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
            try {
                const auto result = ipc_request({{"command", "delete_history"}, {"id", id}});
                if (!result.value("ok", false)) throw std::runtime_error(result.value("error", std::string("删除失败")));
                displayed_history_query_ = nullptr; pending_history_query_ = nullptr;
                request_history();
            }
            catch (const std::exception& e) { error_ = e.what(); }
        });
        history_exclusion_action(row, D2D1::RectF(right - 170, y + 32, right - 94, y + 59));
        y += 62;
        if (y + 10 < table_bottom) line(left + 16, y, right - 16, y, ink_edge);
    }
    if (history_.empty()) {
        icon(3, left + 19, 353, lilac);
        label(L"还没有游戏记录", left + 46, 344, span - 62, 36, heading_.Get(), white);
        label(history_game_.empty() && history_day_.empty() ? L"游戏结束后，记录会显示在这里。" : L"当前筛选下没有记录，可以切换游戏或日期。", left + 46, 382, span - 62, 28, body_.Get(), muted);
    }
    const auto pages = std::max(size_t{1}, (history_count_ + 5) / 6);
    page_buttons(right, table_bottom + 8, history_page_index_, pages, true);
    const auto error = status_.value("snapshot", Json::object()).value("history_error", std::string{});
    if (!error.empty()) label(L"历史保存失败：" + wide(error), left, table_bottom + 45, span, 25, small_.Get(), amber);
}
void SettingsWindow::history_chart(const Json& row, const std::string& metric, D2D1_RECT_F rect, D2D1_COLOR_F color) {
    const auto stat = stat_for(row, metric);
    const auto column = std::find_if(std::begin(columns), std::end(columns), [&](const Column& item) { return item.id == metric; });
    if (column == std::end(columns)) return;
    surface(rect, metric == "fps");
    rect.left += 16; rect.right -= 16; rect.top += 9; rect.bottom -= 8;
    const float name_width = rect.right - rect.left > 560 ? 150.f : 115.f;
    label(column->title, rect.left, rect.top, name_width, 24, heading_.Get(), white);
    label(L"最高 " + reading(stat.value("maximum", Json{}), column->unit) + L"   最低 " + reading(stat.value("minimum", Json{}), column->unit) +
        L"   平均 " + reading(stat.value("average", Json{}), column->unit), rect.left + name_width, rect.top, rect.right - rect.left - name_width, 24, small_.Get(), muted);
    const auto ids = row.value("series_metrics", Json::array());
    const auto found = std::find(ids.begin(), ids.end(), Json(metric));
    const Json empty_series = Json::array();
    const auto& series = row.contains("series") ? row.at("series") : empty_series;
    const size_t index = found == ids.end() ? 0 : static_cast<size_t>(std::distance(ids.begin(), found)) + 1;
    const auto plot = D2D1::RectF(rect.left, rect.top + 35, rect.right - 58, rect.bottom - 24);
    fill(plot, tint(background, .65f), 5);
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
        line(plot.left, y, plot.right, y, tint(edge, .8f));
    }
    for (int i = 1; i < 12; ++i) {
        const float x = plot.left + (plot.right - plot.left) * i / 12;
        line(x, plot.top, x, plot.bottom, tint(edge, .47f));
    }
    const auto yy = [&](double value) { return plot.bottom - static_cast<float>(std::clamp((value - low) / (high - low), 0.0, 1.0)) * (plot.bottom - plot.top); };
    const char* fields[]{"maximum", "average", "minimum"}; const auto colors = std::array{blue, mint, amber};
    std::vector<float> labeled;
    for (int i = 0; i < 3; ++i) if (stat.value(fields[i], Json{}).is_number()) {
        const auto value = stat[fields[i]].get<double>(); const float y = yy(value);
        for (float x = plot.left; x < plot.right; x += 12) line(x, y, std::min(x + 6, plot.right), y, tint(colors[i], .46f));
        if (std::none_of(labeled.begin(), labeled.end(), [&](float previous) { return std::abs(previous - y) < 19; })) {
            label(reading(value), plot.right + 6, y - 10, 52, 20, small_.Get(), colors[i]); labeled.push_back(y);
        }
    }
    std::vector<D2D1_POINT_2F> segment;
    const auto area = linear_brush(target_.Get(), plot, tint(color, .34f), tint(color, .025f));
    const auto draw_segment = [&] {
        if (segment.empty()) return;
        Microsoft::WRL::ComPtr<ID2D1PathGeometry> geometry;
        Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
        if (SUCCEEDED(factory_->CreatePathGeometry(&geometry)) && SUCCEEDED(geometry->Open(&sink))) {
            sink->BeginFigure(D2D1::Point2F(segment.front().x, plot.bottom), D2D1_FIGURE_BEGIN_FILLED);
            for (const auto& point : segment) sink->AddLine(point);
            sink->AddLine(D2D1::Point2F(segment.back().x, plot.bottom));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            if (SUCCEEDED(sink->Close())) target_->FillGeometry(geometry.Get(), area.Get());
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
    row_action(L"‹ 返回记录", D2D1::RectF(left, 142, left + 100, 174),
        [this](float) { history_selected_.clear(); scroll_ = 0; focused_ = -1; refresh(); }, true);
    if (!fixture_mode_ && displayed_history_query_ != history_query()) {
        label(error_.empty() ? L"正在读取游戏详情…" : L"暂时无法读取游戏详情", left + 120, 142, span - 120, 32, heading_.Get(), muted);
        return;
    }
    game_identity(row, left + 120, 135, span - 290);
    history_exclusion_action(row, D2D1::RectF(right - 145, 142, right - 65, 174));
    row_action(L"导出", D2D1::RectF(right - 55, 142, right, 174), [this](float) { export_history(history_detail_); });
    label(time_text(row.value("started_ms", 0ull)) + L" → " + time_text(row.value("updated_ms", 0ull)) + L"  ·  游玩 " + duration(row.value("active_seconds", 0.0)), left, 182, span, 25, small_.Get(), muted);
    const wchar_t* tabs[]{L"性能报告", L"性能图表", L"会话事件", L"设备信息"};
    fill(D2D1::RectF(left, 212, left + 494, 254), tint(panel, .72f), control_radius);
    outline(D2D1::RectF(left, 212, left + 494, 254), edge, control_radius);
    for (int i = 0; i < 4; ++i) tab(tabs[i], D2D1::RectF(left + 4 + i * 122, 216, left + 122 + i * 122, 250),
        [this, i](float) { history_tab_ = i; scroll_ = 0; }, history_tab_ == i);
    if (history_tab_ == 0) {
        label(L"平均值  ·  ↑最高  ↓最低", right - 270, 219, 270, 28, small_.Get(), muted);
        const int report_columns[]{0, 1, 2, 3, 4, 5, 6, 9};
        const bool wide_layout = span >= 1320;
        const float stat_width = wide_layout ? span * .57f - 13 : span, box = (stat_width - 36) / 4;
        const float chart_left = wide_layout ? left + stat_width + 14 : left;
        const float selector_y = wide_layout ? 271.f : 448.f;
        for (int i = 0; i < 8; ++i) {
            const auto& column = columns[report_columns[i]]; const auto stat = stat_for(row, column.id);
            const float x = left + i % 4 * (box + 12), y = 268.f + i / 4 * 85;
            surface(D2D1::RectF(x, y, x + box, y + 74), i == 0);
            label(column.title, x + 12, y + 5, box - 24, 22, small_.Get(), muted);
            label(reading(stat.value("average", Json{}), column.unit), x + 12, y + 25, box - 24, 26, data_.Get(), stat_color(column.id, stat.value("average", Json{})));
            label(L"↑ " + reading(stat.value("maximum", Json{})) + L"   ↓ " + reading(stat.value("minimum", Json{})), x + 12, y + 51, box - 24, 20, small_.Get(), muted);
        }
        const wchar_t* selectors[]{L"FPS", L"CPU", L"GPU", L"CPU 温度", L"内存", L"硬盘"}; const int indexes[]{0, 1, 3, 2, 5, 6};
        const float box_width = std::min(150.f, (right - chart_left - 40) / 6);
        for (int i = 0; i < 6; ++i) button(selectors[i], D2D1::RectF(chart_left + i * (box_width + 8), selector_y,
            chart_left + i * (box_width + 8) + box_width, selector_y + 31),
            [this, i](float) { history_metric_ = i; }, history_metric_ == i);
        const float chart_bottom = wide_layout ? 548.f : 642.f;
        history_chart(row, columns[indexes[history_metric_]].id, D2D1::RectF(chart_left, selector_y + 43, right, chart_bottom), blue);
        const auto hw = row.value("hardware", Json::object());
        const float summary_top = wide_layout ? 442.f : chart_bottom + 14;
        surface(D2D1::RectF(left, summary_top, left + stat_width, summary_top + (row.value("legacy_values_hidden", false) ? 119 : 94)));
        label(L"处理器  " + wide(hw.value("cpu", std::string("旧记录未保存设备信息"))), left + 14, summary_top + 8, stat_width - 28, 22, small_.Get(), muted);
        std::wstring gpu_name;
        for (const auto& gpu : hw.value("gpus", Json::array())) if (gpu.value("id", std::string{}) == hw.value("selected_gpu", std::string{})) gpu_name = wide(gpu.value("name", std::string{}));
        label(L"显卡  " + (gpu_name.empty() ? L"—" : gpu_name) + L"  ·  状态 " + status_text(row), left + 14, summary_top + 34, stat_width - 28, 22, small_.Get(), muted);
        label(L"Low（最近60秒）  1% " + reading(row.value("latest_low1", Json{})) + L"  ·  0.1% " + reading(row.value("latest_low01", Json{})), left + 14, summary_top + 60, stat_width - 28, 22, small_.Get(), muted);
        if (row.value("legacy_values_hidden", false)) label(L"旧记录的无效值已隐藏；原有平均值保留，不重算缺失采样。",
            left + 14, summary_top + 88, stat_width - 28, 20, small_.Get(), muted);
    } else if (history_tab_ == 1) {
        const int sets[][3]{{0, 1, 3}, {2, 4, 6}, {5, 9, 8}};
        const wchar_t* labels[]{L"帧率与负载", L"硬件温度", L"内存与功耗"};
        for (int i = 0; i < 3; ++i) button(labels[i], D2D1::RectF(left + i * 130, 269, left + 120 + i * 130, 299),
            [this, i](float) { history_metric_ = i; }, history_metric_ % 3 == i);
        const auto colors = std::array{blue, lilac, mint};
        const float gap = 14, box = (span - gap) / 2;
        history_chart(row, columns[sets[history_metric_ % 3][0]].id, D2D1::RectF(left, 314, right, 520), colors[0]);
        for (int i = 1; i < 3; ++i) {
            const float x = left + (i - 1) * (box + gap);
            history_chart(row, columns[sets[history_metric_ % 3][i]].id, D2D1::RectF(x, 534, x + box, 716), colors[i]);
        }
        label(L"虚线：最高 / 平均 / 最低  ·  失焦、暂停与无效读数显示为缺口", left, 726, span, 22, small_.Get(), muted);
    } else if (history_tab_ == 2) {
        const auto events = row.value("events", Json::array());
        const auto first = events.size() > 9 ? events.size() - 9 : 0;
        const float bottom = 414 + static_cast<float>(events.size() - first) * 34;
        surface(D2D1::RectF(left, 271, right, bottom), true);
        label(L"会话状态", left + 17, 280, 150, 32, heading_.Get(), white);
        badge(status_text(row), D2D1::RectF(right - 113, 284, right - 16, 309), mint);
        line(left + 27, 336, left + 27, bottom - 49, edge);
        float y = 322;
        const auto event_row = [&](uint64_t at, const std::wstring& description, D2D1_COLOR_F ink) {
            fill(D2D1::RectF(left + 24, y + 11, left + 30, y + 17), ink, 3);
            label(time_text(at), left + 44, y, 175, 28, small_.Get(), muted);
            label(description, left + 222, y, span - 243, 28, body_.Get(), ink);
            y += 34;
        };
        event_row(row.value("started_ms", 0ull), L"开始监控", mint);
        for (size_t i = first; i < events.size(); ++i) {
            const auto event = events[i]; const auto type = event.value("type", std::string{});
            event_row(event.value("at_ms", 0ull), type == "paused" ? L"暂停采集" : type == "background" ? L"游戏失焦" : L"恢复前台采集",
                type == "paused" ? amber : type == "background" ? muted : blue);
        }
        event_row(row.value("updated_ms", 0ull), status_text(row), mint);
        label(L"这里只记录本应用观察到的状态变化；没有事件时保留会话起止。", left + 17, bottom + 8, span - 34, 24, small_.Get(), muted);
    } else {
        const auto hw = row.value("hardware", Json::object());
        const auto gpus = hw.value("gpus", Json::array()), disks = hw.value("disks", Json::array());
        const auto displays = hw.value("displays", Json::array());
        const float bottom = 327 + static_cast<float>(3 + gpus.size() + disks.size() + displays.size()) * 39 + 10;
        surface(D2D1::RectF(left, 271, right, bottom));
        icon(0, left + 17, 286, blue);
        label(L"会话开始时的设备信息", left + 43, 278, span - 60, 35, heading_.Get(), white);
        float y = 327;
        const auto device = [&](const std::wstring& title, const std::wstring& description) {
            line(left + 16, y, right - 16, y, ink_edge);
            label(title, left + 17, y + 2, 115, 36, small_.Get(), muted);
            label(description, left + 137, y + 2, span - 154, 36, body_.Get(), white); y += 39;
        };
        device(L"处理器", wide(hw.value("cpu", std::string("—"))));
        device(L"逻辑处理器", std::to_wstring(hw.value("logical_processors", 0u)));
        for (const auto& gpu : gpus) device(L"显卡", wide(gpu.value("name", std::string{})));
        const auto memory = hw.value("memory_total", Json::object()).value("value", Json{});
        device(L"内存容量", memory.is_number() ? reading(memory.get<double>() / 1073741824.0, L" GiB") : L"—");
        for (const auto& disk : disks) device(L"硬盘", wide(disk.value("name", std::string{})));
        for (const auto& display : displays) device(L"显示器", wide(display.value("name", std::string{})) +
            std::format(L"  {} × {}  ·  {} DPI{}", display.value("width", 0), display.value("height", 0), display.value("dpi", 96), display.value("hdr", false) ? L"  HDR" : L""));
    }
}
}
