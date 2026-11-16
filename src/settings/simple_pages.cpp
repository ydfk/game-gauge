#include "window.h"
#include "theme.h"
#include "common/platform.h"
#include "hud/renderer.h"
#include "version.h"
#include "host/ipc_server.h"
#include <commdlg.h>
#include <shellapi.h>
#include <algorithm>
#include <array>
#include <format>
#include <ctime>

namespace gauge {
using namespace theme;
namespace {
bool contains(const std::vector<std::string>& values, const std::string& value) {
    return std::any_of(values.begin(), values.end(), [&](const auto& item) { return _stricmp(item.c_str(), value.c_str()) == 0; });
}
void forget(std::vector<std::string>& values, const std::string& value) {
    std::erase_if(values, [&](const auto& item) { return _stricmp(item.c_str(), value.c_str()) == 0; });
}
void remember(Config& config, const std::string& path, bool excluded) {
    const auto name = utf8(std::filesystem::path(wide(path)).filename().wstring());
    if (excluded) {
        std::erase_if(config.known_games, [&](const auto& item) { return _stricmp(utf8(std::filesystem::path(wide(item)).filename().wstring()).c_str(), name.c_str()) == 0; });
        if (!contains(config.ignored_processes, name)) config.ignored_processes.push_back(name);
    } else {
        forget(config.ignored_processes, name);
        if (!contains(config.known_games, path)) config.known_games.push_back(path);
    }
    config.auto_target = true;
}

}
void SettingsWindow::list_surface(D2D1_RECT_F rect, bool checked) {
    if (checked) fill(rect, tint(accent, .15f), control_radius);
    line(rect.left, rect.bottom, rect.right, rect.bottom, ink_edge);
}
void SettingsWindow::row_action(const std::wstring& title, D2D1_RECT_F rect, std::function<void(float)> action, bool emphasized) {
    const bool danger = title == L"删除" || title == L"移除" || title == L"排除";
    const auto ink = danger ? red : emphasized ? lilac : muted;
    const float hover = hover_strength(rect);
    if (hover > 0 || pressed(rect)) fill(rect, tint(danger ? red : theme::accent, pressed(rect) ? .25f : .15f * hover), control_radius);
    text(title, rect, button_format_.Get(), !emphasized && !danger && hover > 0 ? white : ink);
    add(rect, std::move(action));
}
void SettingsWindow::page_buttons(float right, float y, size_t& page, size_t pages, bool reload) {
    if (pages <= 1) return;
    label(std::format(L"{} / {}", page + 1, pages), right - 104, y, 52, 30, small_.Get(), muted);
    if (page) row_action(L"‹ 上页", D2D1::RectF(right - 166, y, right - 111, y + 30), [this, &page, reload](float) { --page; if (reload) refresh(); });
    if (page + 1 < pages) row_action(L"下页 ›", D2D1::RectF(right - 49, y, right, y + 30), [this, &page, reload](float) { ++page; if (reload) refresh(); });
}
void SettingsWindow::appearance_page(float width) {
    const float left = 272, right = width - 28, span = right - left, gap = 13;
    const float box = (span - gap * 3) / 4;
    const std::array<const char*, 4> readings{"fps", "frametime", "cpu_load", "gpu_load"};
    const std::array<D2D1_COLOR_F, 4> colors{lilac, blue, mint, lilac};
    for (size_t i = 0; i < readings.size(); ++i) {
        const auto* id = readings[i];
        const float x = left + (box + gap) * i;
        const auto rect = D2D1::RectF(x, 140, x + box, 233);
        surface(rect, i == 0);
        const auto mark = D2D1::RectF(x + box - 46, 153, x + box - 15, 184);
        fill(mark, tint(colors[i], .10f), control_radius);
        icon(i == 0 ? 8 : i == 1 ? 1 : 0, mark.left + 6.5f, mark.top + 6.5f, colors[i]);
        label(metric_name(id), x + 16, 151, box - 64, 25, small_.Get(), muted);
        const auto value = metric_value(id);
        label(value, x + 16, 182, box - 32, 35, data_.Get(), value == L"—" ? muted : metric_color(id));
    }
    const auto preview = D2D1::RectF(left, 248, right, 415);
    surface(preview, true);
    icon(0, left + 17, 265, lilac);
    label(L"实时预览", left + 45, 258, span - 170, 31, heading_.Get(), white);
    const auto stage = D2D1::RectF(left + 15, 299, right - 15, 400);
    gradient(target_.Get(), stage, tint(background, .89f), tint(sidebar, .82f), control_radius);
    aurora(D2D1::RectF(stage.left + 2, stage.top + 2, stage.right - 2, stage.bottom - 2));
    outline(stage, tint(lilac, .18f), control_radius);
    std::vector<HudItem> items;
    for (const auto& row : status_.value("hud_preview", Json::array())) {
        const auto color = row.at("color");
        items.push_back({wide(row.value("label", std::string{})), wide(row.value("value", std::string{})),
            D2D1::ColorF(color[0].get<float>(), color[1].get<float>(), color[2].get<float>()), wide(row.value("group", std::string{})), row.value("status", std::string{})});
    }
    if (items.empty()) items = hud_items(Snapshot{}, config_);
    const auto size = measure_hud_items(write_.Get(), items, config_);
    const float usable = stage.right - stage.left - 24;
    const float fit = std::min({1.f, usable / std::max(1.f, size.width), 72.f / std::max(1.f, size.height)});
    const float pref_width = size.width * fit;
    float x = (stage.left + stage.right - pref_width) / 2;
    if (config_.anchor == 1) x = stage.left + 12;
    if (config_.anchor == 2) x = stage.right - 12 - pref_width;
    const float y = config_.anchor == 3 ? stage.bottom - 12 - size.height * fit : stage.top + 12;
    D2D1_MATRIX_3X2_F saved; target_->GetTransform(&saved);
    target_->SetTransform(D2D1::Matrix3x2F::Scale(fit, fit) * D2D1::Matrix3x2F::Translation(x, y) * saved);
    draw_hud_items(target_.Get(), write_.Get(), items, config_, size.width, size.height);
    target_->SetTransform(saved);
    if (fit < 1) label(L"预览已缩放", right - 130, 261, 112, 23, small_.Get(), muted);

    const float left_width = (span - gap) * .49f, column = left + left_width + gap;
    surface(D2D1::RectF(left, 430, column - gap, 738));
    surface(D2D1::RectF(column, 430, right, 738));
    label(L"样式与位置", left + 17, 443, left_width - 34, 28, heading_.Get(), white);
    label(L"显示规则", column + 17, 443, right - column - 34, 28, heading_.Get(), white);
    const float slider_left = left + 3, slider_right = column - gap - 3;
    const float slider_width = slider_right - slider_left;
    slider(L"文字大小", std::format(L"{:.0f} px", config_.font_size), static_cast<float>((config_.font_size - 10) / 22),
        D2D1::RectF(slider_left, 475, slider_right, 534), [this, slider_left, slider_width](float x) {
            config_.font_size = 10 + 22 * std::clamp((x - slider_left - 18) / (slider_width - 36), 0.f, 1.f); apply(); });
    slider(L"背景不透明度", std::format(L"{:.0f}%", config_.opacity * 100), static_cast<float>(config_.opacity),
        D2D1::RectF(slider_left, 535, slider_right, 594), [this, slider_left, slider_width](float x) {
            config_.opacity = std::clamp((x - slider_left - 18) / (slider_width - 36), 0.f, 1.f); apply(); });
    const float inner = left + 17;
    label(L"显示位置", inner, 596, left_width - 34, 25, body_.Get(), muted);
    const wchar_t* anchors[]{L"顶部居中", L"左上角", L"右上角", L"底部居中"};
    const float anchor_width = (left_width - 44) / 2;
    for (int i = 0; i < 4; ++i) {
        const float ax = inner + (i % 2) * (anchor_width + 10), ay = 630.f + (i / 2) * 45;
        button(anchors[i], D2D1::RectF(ax, ay, ax + anchor_width, ay + 36),
            [this, i](float) { config_.anchor = i; config_.margin_x = 0; config_.margin_y = 0; config_.preview = false; apply(); }, config_.anchor == i);
    }
    const auto rule = [&](int row, const wchar_t* title, const wchar_t* hint, bool Config::*setting) {
        const float y = 477.f + row * 50;
        if (row) line(column + 17, y, right - 17, y, ink_edge);
        toggle(title, hint, config_.*setting, D2D1::RectF(column + 3, y, right - 3, y + 50),
            [this, setting](float) { config_.*setting = !(config_.*setting); apply(); });
    };
    rule(0, L"游戏内监控", L"游戏启动后显示，退出后关闭", &Config::enabled);
    rule(1, L"切出游戏后隐藏", L"关闭后继续跟随游戏窗口", &Config::hide_on_blur);
    rule(2, L"录屏与截图隐藏", L"捕获画面中不包含监控条", &Config::exclude_capture);
    rule(3, L"OLED 防烧屏", L"仅左右微移，最大偏移 4 像素", &Config::oled_protection);
    rule(4, L"OBS 暂停大字提醒", L"显示在游戏中央，点击关闭本次提醒", &Config::obs_pause_notice);
}

void SettingsWindow::metrics_page(float width) {
    const float left = 272, right = width - 28, span = right - left;
    if (disk_details_) {
        const auto disks = status_.value("snapshot", Json::object()).value("disks", Json::array());
        label(L"硬盘温度", left, 140, span - 140, 34, heading_.Get(), white);
        button(L"‹ 返回项目", D2D1::RectF(right - 122, 141, right, 175), [this](float) { disk_details_ = false; scroll_ = 0; });
        const auto pages = std::max(size_t{1}, (disks.size() + 5) / 6); disk_page_ = std::min(disk_page_, pages - 1);
        const size_t first = disk_page_ * 6, last = std::min(disks.size(), first + 6);
        surface(D2D1::RectF(left, 190, right, 242 + static_cast<float>(std::max(size_t{1}, last - first)) * 66));
        label(L"设备与采集来源", left + 16, 196, span - 148, 33, small_.Get(), muted);
        label(L"温度", right - 110, 196, 94, 33, small_.Get(), muted);
        float y = 234;
        for (size_t i = first; i < last; ++i) {
            const auto& disk = disks[i]; const auto sensor = disk.value("temperature", Json::object());
            const bool valid = sensor.value("state", std::string{}) == "valid" && sensor.value("value", Json{}).is_number();
            line(left + 16, y, right - 16, y, ink_edge);
            label(wide(disk.value("name", std::string{})), left + 16, y + 5, span - 140, 29, body_.Get(), white);
            label(valid ? std::format(L"{:.0f}°C", sensor["value"].get<double>()) : L"不可用", right - 110, y + 16, 94, 32,
                valid ? mono_.Get() : small_.Get(), valid ? reading_color(metric_tone("disk_temperature",
                    std::optional<double>(sensor["value"].get<double>()))) : muted);
            label(wide(sensor.value(valid ? "source" : "reason", std::string{})), left + 16, y + 33, span - 140, 23, small_.Get(), muted);
            y += 66;
        }
        if (disks.empty()) label(L"未发现可读取的硬盘，等待设备采样。", left + 16, y + 10, span - 32, 34, body_.Get(), muted);
        page_buttons(right, y + 20, disk_page_, pages);
        return;
    }

    struct Group { const wchar_t* name; std::vector<std::string> ids; };
    const Group groups[]{
        {L"帧率", {"fps", "frametime", "low1", "low01"}},
        {L"CPU", {"cpu_temperature", "cpu_load", "cpu_clock", "process_cpu"}},
        {L"GPU", {"gpu_temperature", "gpu_load", "gpu_clock", "gpu_power", "gpu_fan", "vram"}},
        {L"内存、硬盘与游戏", {"memory_load", "memory_used", "process_memory", "session", "disk_temperature", "obs"}}
    };
    const auto count = config_.metrics.size() + (config_.show_obs ? 1 : 0);
    label(std::format(L"悬浮条已启用 {} 项", count), left, 141, span - 190, 33, body_.Get(), muted);
    button(L"查看各硬盘温度 ›", D2D1::RectF(right - 163, 140, right, 176),
        [this](float) { disk_details_ = true; scroll_ = 0; }, true);
    const float gap = 14, group_width = (span - gap) / 2;
    for (size_t group = 0; group < std::size(groups); ++group) {
        const auto& [name, ids] = groups[group];
        const float x = left + group % 2 * (group_width + gap), y = group < 2 ? 192.f : 408.f;
        surface(D2D1::RectF(x, y, x + group_width, y + (group < 2 ? 202 : 270)), group == 0);
        icon(group == 0 ? 1 : 0, x + 16, y + 16, group == 0 ? lilac : blue);
        label(name, x + 44, y + 8, group_width - 60, 35, heading_.Get(), white);
        line(x + 15, y + 48, x + group_width - 15, y + 48, ink_edge);
        const float tile = (group_width - 36) / 2;
        for (size_t i = 0; i < ids.size(); ++i) {
            const auto id = ids[i];
            const float cell = x + 12 + i % 2 * (tile + 12), row = y + 53 + i / 2 * 69;
            const auto rect = D2D1::RectF(cell, row, cell + tile, row + 62);
            const bool checked = id == "obs" ? config_.show_obs : contains(config_.metrics, id);
            fill(rect, tint(checked ? accent : panel_high, checked ? .09f + hover_strength(rect) * .1f : hover_strength(rect) * .62f),
                control_radius);
            if (checked) outline(rect, tint(accent, .30f), control_radius);
            const auto mark = D2D1::RectF(cell + 10, row + 11, cell + 25, row + 26);
            fill(mark, checked ? accent : tint(edge, .65f), 4);
            if (checked) {
                line(cell + 13, row + 18, cell + 16, row + 21, white, 1.4f);
                line(cell + 16, row + 21, cell + 22, row + 15, white, 1.4f);
            }
            label(id == "obs" ? L"OBS 录制状态" : metric_name(id), cell + 33, row + 5, tile - 42, 25, body_.Get(), checked ? white : muted);
            const auto value = id == "obs" ? (checked ? obs_label(status_.value("snapshot", Json::object()).value("obs_state", std::string{})) : L"默认关闭") : metric_value(id);
            label(value, cell + 33, row + 28, tile - 42, 26, id == "obs" ? small_.Get() : mono_.Get(), checked ? metric_color(id) : muted);
            add(rect, [this, id](float) {
                if (id == "obs") { config_.show_obs = !config_.show_obs; apply(); return; }
                auto& metrics = config_.metrics; const auto it = std::find(metrics.begin(), metrics.end(), id);
                if (it == metrics.end()) metrics.push_back(id); else if (metrics.size() > 1) metrics.erase(it);
                apply();
            });
        }
    }
    const wchar_t* labels[]{L"常规", L"活跃", L"留意", L"偏高 / 偏低", L"重点留意"};
    const MetricTone tones[]{MetricTone::good, MetricTone::cool, MetricTone::watch, MetricTone::high, MetricTone::critical};
    for (int i = 0; i < 5; ++i) {
        const float x = left + 4 + i * 126.f;
        fill(D2D1::RectF(x, 705, x + 6, 711), reading_color(tones[i]), 3);
        label(labels[i], x + 14, 695, 110, 25, small_.Get(), muted);
    }
}

void SettingsWindow::choose_process(bool excluded) {
    wchar_t path[32768]{}; OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = window_; dialog.lpstrFile = path; dialog.nMaxFile = 32768;
    dialog.lpstrFilter = L"程序 (*.exe)\0*.exe\0\0"; dialog.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&dialog)) return;
    remember(config_, utf8(path), excluded); apply();
}
void SettingsWindow::games_page(float width) {
    const float left = 272, right = width - 28, span = right - left;
    const bool wide_layout = span >= 1240;
    const float games_right = wide_layout ? left + span * .67f : right, games_width = games_right - left;
    const float excluded_left = wide_layout ? games_right + 14 : left;
    const auto name = status_.value("snapshot", Json::object()).value("target", Json::object()).value("name", std::string{});
    const auto pages = std::max(size_t{1}, (targets_.size() + 3) / 4); target_page_ = std::min(target_page_, pages - 1);
    const size_t game_count = std::min<size_t>(4, targets_.size() - target_page_ * 4);
    const float games_bottom = std::max(362.f, 268.f + static_cast<float>(game_count) * 48 + (pages > 1 ? 32.f : 0.f));
    const float excluded_top = wide_layout ? 140.f : games_bottom + 16;
    surface(D2D1::RectF(left, 140, games_right, games_bottom), true);
    label(L"游戏", left + 16, 151, 130, 32, heading_.Get(), white);
    button(L"添加游戏", D2D1::RectF(games_right - 125, 151, games_right - 16, 185),
        [this](float) { choose_process(false); }, true);
    if (!name.empty()) label(L"正在监控  " + wide(name), left + 16, 184, games_width - 32, 24, small_.Get(), mint);
    const float detection = left + games_width * .47f, state = games_right - 239, actions = games_right - 157;
    label(L"程序", left + 16, 214, games_width * .44f, 29, small_.Get(), muted);
    label(L"识别方式", detection, 214, state - detection - 8, 29, small_.Get(), muted);
    label(L"状态", state, 214, 72, 29, small_.Get(), muted);
    label(L"操作", actions, 214, 140, 29, small_.Get(), muted);
    line(left + 16, 248, games_right - 16, 248, ink_edge);
    float y = 249;
    if (targets_.empty()) label(L"暂无游戏 · 可添加要监控的程序", left + 16, 313, games_width - 32, 32, body_.Get(), muted);
    for (size_t i = target_page_ * 4; i < std::min(targets_.size(), target_page_ * 4 + 4); ++i) {
        const auto item = targets_[i]; const auto path = item.value("path", std::string{});
        const bool known = contains(config_.known_games, path), active = item.value("pid", 0u) != 0;
        label(wide(item.value("name", std::string{})), left + 16, y + 8, detection - left - 30, 30, body_.Get(), white);
        label(known ? L"手动添加" : L"自动识别", detection, y + 8, state - detection - 8, 30, small_.Get(), muted);
        badge(active ? L"运行中" : L"未运行", D2D1::RectF(state - 4, y + 11, state + 61, y + 35), active ? mint : muted);
        row_action(known ? L"移除" : L"记住", D2D1::RectF(actions, y + 6, actions + 60, y + 41), [this, path, known](float) {
            if (known) forget(config_.known_games, path); else remember(config_, path, false); apply();
        }, !known);
        row_action(L"排除", D2D1::RectF(games_right - 75, y + 6, games_right - 16, y + 41),
            [this, path](float) { remember(config_, path, true); apply(); });
        y += 48;
        line(left + 16, y, games_right - 16, y, ink_edge);
    }
    page_buttons(games_right - 16, games_bottom - 40, target_page_, pages);

    const auto blocked_pages = std::max(size_t{1}, (config_.ignored_processes.size() + 2) / 3);
    blacklist_page_ = std::min(blacklist_page_, blocked_pages - 1);
    const size_t blocked_count = std::min<size_t>(3, config_.ignored_processes.size() - blacklist_page_ * 3);
    const float blocked_bottom = excluded_top + std::max(136.f, 69.f + static_cast<float>(blocked_count) * 42
        + (blocked_pages > 1 ? 37.f : 0.f));
    surface(D2D1::RectF(excluded_left, excluded_top, right, blocked_bottom));
    label(L"排除", excluded_left + 16, excluded_top + 11, 100, 32, heading_.Get(), white);
    button(L"添加排除", D2D1::RectF(right - 125, excluded_top + 11, right - 16, excluded_top + 45),
        [this](float) { choose_process(true); });
    y = excluded_top + 57;
    for (size_t i = blacklist_page_ * 3; i < std::min(config_.ignored_processes.size(), blacklist_page_ * 3 + 3); ++i) {
        const auto process = config_.ignored_processes[i];
        line(excluded_left + 16, y, right - 16, y, ink_edge);
        label(wide(process), excluded_left + 16, y + 6, right - excluded_left - 168, 30, body_.Get(), white);
        label(L"不监控", right - 151, y + 6, 70, 30, small_.Get(), muted);
        row_action(L"移除", D2D1::RectF(right - 75, y + 3, right - 16, y + 37), [this, process](float) {
            forget(config_.ignored_processes, process); apply();
        });
        y += 42;
    }
    if (config_.ignored_processes.empty())
        label(L"还没有排除规则", excluded_left + 16, excluded_top + 79, right - excluded_left - 32, 28, body_.Get(), muted);
    page_buttons(right - 16, blocked_bottom - 36, blacklist_page_, blocked_pages);
}

void SettingsWindow::updates_page(float width) {
    const float left = 272, right = width - 28, span = right - left, split = left + span * .48f;
    const auto update = status_.value("update", Json::object());
    const auto state = update.value("state", std::string{});
    const bool available = state == "available", ready = state == "ready";
    const bool busy = state == "checking" || state == "downloading" || state == "installing";
    surface(D2D1::RectF(left, 140, split - 7, 356), true);
    surface(D2D1::RectF(split + 7, 140, right, 356));
    label(L"当前版本", left + 17, 153, 160, 30, small_.Get(), muted);
    label(L"v" + wide(GAMEGAUGE_VERSION), left + 17, 188, split - left - 34, 43, title_.Get(), white);
    const auto action = D2D1::RectF(left + 17, 289, left + 168, 330);
    if (busy) badge(state == "checking" ? L"检查中…" : state == "downloading" ? L"下载中…" : L"安装中…", action, muted);
    else button(ready ? L"安装更新" : available ? L"下载更新" : L"检查更新", action,
        [this, ready, available](float) { command(ready ? "install_update" : available ? "download_update" : "check_update"); }, true);
    const auto status_ink = state == "error" ? amber : available || ready ? blue : muted;
    fill(D2D1::RectF(left + 18, 253, left + 24, 259), status_ink, 3);
    auto message = wide(update.value("message", std::string("尚未检查更新")));
    if (available) message = L"发现新版本 v" + wide(update.value("version", std::string{}));
    label(message, left + 34, 240, split - left - 57, 32, small_.Get(), status_ink);
    label(L"自动更新", split + 24, 152, right - split - 41, 34, heading_.Get(), white);
    toggle(L"检查新版本", L"每六小时检查一次", config_.check_updates,
        D2D1::RectF(split + 10, 196, right - 3, 261),
        [this](float) { config_.check_updates = !config_.check_updates; if (!config_.check_updates) config_.auto_update = false; apply(); });
    line(split + 24, 266, right - 17, 266, ink_edge);
    toggle(L"下载并安装", L"游戏退出后安装", config_.auto_update,
        D2D1::RectF(split + 10, 271, right - 3, 336),
        [this](float) { config_.auto_update = !config_.auto_update; if (config_.auto_update) config_.check_updates = true; apply(); });

    const auto components = status_.value("components", Json::object());
    const auto items = components.value("items", Json::array());
    const auto repair_state = components.value("repair_state", std::string{});
    const bool repairing = repair_state == "running";
    const float bottom = 472 + static_cast<float>(std::max<size_t>(1, items.size())) * 56;
    surface(D2D1::RectF(left, 371, right, bottom));
    label(L"采集组件", left + 17, 383, 175, 36, heading_.Get(), white);
    const auto repair = D2D1::RectF(right - 146, 383, right - 16, 418);
    if (repairing || !components.value("can_repair", false))
        badge(repairing ? L"修复中…" : L"需要完整安装包", repair, muted);
    else button(L"修复采集组件", repair, [this](float) { command("repair_components"); });
    const auto log = executable_dir() / L"component-repair.log";
    if (std::filesystem::is_regular_file(log)) row_action(L"修复日志", D2D1::RectF(right - 238, 383, right - 158, 418),
        [this, log](float) { ShellExecuteW(window_, L"open", log.c_str(), nullptr, nullptr, SW_SHOWNORMAL); });
    float y = 429;
    for (const auto& item : items) {
        const auto component_state = item.value("state", std::string{});
        const auto ink = component_state == "ready" ? mint : component_state == "unsupported" ? muted : amber;
        line(left + 17, y, right - 17, y, ink_edge);
        fill(D2D1::RectF(left + 18, y + 23, left + 24, y + 29), ink, 3);
        label(wide(item.value("name", std::string{})), left + 35, y + 11, 126, 31, body_.Get(), white);
        label(wide(item.value("detail", std::string{})), left + 166, y + 11, span - 183, 31, small_.Get(), ink);
        y += 56;
    }
    auto component_message = components.value("message", std::string{});
    if (component_message.empty()) component_message = components.value("can_repair", false) ?
        "持续检查 · 离线修复需要管理员授权" : "安装新版完整安装包后，可在这里离线修复组件";
    label(wide(component_message), left + 17, y + 4, span - 34, 28, small_.Get(), repair_state == "error" || repair_state == "reboot" ? amber : muted);
}

void SettingsWindow::history_exclusion_action(const Json& record, D2D1_RECT_F rect) {
    auto path = record.value("path", std::string{});
    if (path.empty()) path = record.value("game", std::string{});
    const auto name = utf8(std::filesystem::path(wide(path)).filename().wstring());
    if (name.empty()) { text(L"不可排除", rect, button_format_.Get(), muted); return; }
    if (contains(config_.ignored_processes, name)) { text(L"已排除", rect, button_format_.Get(), muted); return; }
    row_action(L"排除", rect, [this, path](float) { remember(config_, path, true); apply(); });
}
}
