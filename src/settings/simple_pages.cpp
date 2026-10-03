#include "window.h"
#include "theme.h"
#include "common/platform.h"
#include "hud/renderer.h"
#include "version.h"
#include "host/ipc_server.h"
#include <commdlg.h>
#include <algorithm>
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
    fill(rect, checked ? panel_high : panel);
    line(rect.left, rect.bottom, rect.right, rect.bottom, D2D1::ColorF(0x2A3B4D));
}
void SettingsWindow::row_action(const std::wstring& title, D2D1_RECT_F rect, std::function<void(float)> action, bool accent) {
    const bool danger = title == L"删除" || title == L"移除" || title == L"排除";
    const auto ink = danger ? red : accent ? blue : muted;
    if (hovered(rect)) fill(rect, tint(ink, pressed(rect) ? .20f : .10f), 6);
    text(title, rect, button_format_.Get(), hovered(rect) ? ink : accent || danger ? ink : muted);
    add(rect, std::move(action));
}
void SettingsWindow::page_buttons(float right, float y, size_t& page, size_t pages, bool reload) {
    if (pages <= 1) return;
    label(std::format(L"{} / {}", page + 1, pages), right - 104, y, 52, 30, small_.Get(), muted);
    if (page) row_action(L"‹ 上页", D2D1::RectF(right - 166, y, right - 111, y + 30), [this, &page, reload](float) { --page; if (reload) refresh(); });
    if (page + 1 < pages) row_action(L"下页 ›", D2D1::RectF(right - 49, y, right, y + 30), [this, &page, reload](float) { ++page; if (reload) refresh(); });
}
void SettingsWindow::appearance_page(float width) {
    const float left = 272, right = width - 28, span = right - left, half = (span - 14) / 2;
    fill(D2D1::RectF(left, 148, right, 276), panel, 12);
    label(L"实时预览", left + 18, 160, span - 36, 24, heading_.Get(), white);
    std::vector<HudItem> items;
    for (const auto& row : status_.value("hud_preview", Json::array())) {
        const auto color = row.at("color");
        items.push_back({wide(row.value("label", std::string{})), wide(row.value("value", std::string{})),
            D2D1::ColorF(color[0].get<float>(), color[1].get<float>(), color[2].get<float>()), wide(row.value("group", std::string{})), row.value("status", std::string{})});
    }
    if (items.empty()) items = hud_items(Snapshot{}, config_);
    const auto size = measure_hud_items(write_.Get(), items, config_);
    const float fit = std::min(1.f, (span - 36) / std::max(1.f, size.width));
    float x = left + (span - size.width * fit) / 2;
    if (config_.anchor == 1) x = left + 18;
    if (config_.anchor == 2) x = right - 18 - size.width * fit;
    const float y = config_.anchor == 3 ? 244.f - size.height * fit : 207.f;
    D2D1_MATRIX_3X2_F saved; target_->GetTransform(&saved);
    target_->SetTransform(D2D1::Matrix3x2F::Scale(fit, fit) * D2D1::Matrix3x2F::Translation(x, y) * saved);
    draw_hud_items(target_.Get(), write_.Get(), items, config_, size.width, size.height);
    target_->SetTransform(saved);
    if (fit < 1) label(L"预览已按宽度缩放", right - 180, 160, 162, 24, small_.Get(), muted);
    const float column = left + half + 14;
    label(L"样式与位置", left, 302, half, 28, heading_.Get(), white);
    label(L"显示规则", column, 302, half, 28, heading_.Get(), white);
    slider(L"文字大小", std::format(L"{:.0f} px", config_.font_size), static_cast<float>((config_.font_size - 10) / 22),
        D2D1::RectF(left, 344, left + half, 416), [this, left, half](float x) {
            config_.font_size = 10 + 22 * std::clamp((x - left - 18) / (half - 36), 0.f, 1.f); apply(); });
    slider(L"背景不透明度", std::format(L"{:.0f}%", config_.opacity * 100), static_cast<float>(config_.opacity),
        D2D1::RectF(left, 428, left + half, 500), [this, left, half](float x) {
            config_.opacity = std::clamp((x - left - 18) / (half - 36), 0.f, 1.f); apply(); });
    label(L"显示位置", left, 525, half, 24, body_.Get(), muted);
    const wchar_t* anchors[]{L"顶部居中", L"左上角", L"右上角", L"底部居中"};
    const float box = (half - 10) / 2;
    for (int i = 0; i < 4; ++i) {
        const float anchor_x = left + (i % 2) * (box + 10), anchor_y = 563.f + (i / 2) * 52;
        button(anchors[i], D2D1::RectF(anchor_x, anchor_y, anchor_x + box, anchor_y + 42),
            [this, i](float) { config_.anchor = i; config_.margin_x = 0; config_.margin_y = 0; config_.preview = false; apply(); }, config_.anchor == i);
    }
    toggle(L"游戏内监控", L"游戏启动后显示，退出后关闭", config_.enabled,
        D2D1::RectF(column, 344, right, 414), [this](float) { config_.enabled = !config_.enabled; apply(); });
    toggle(L"切出游戏后隐藏", L"关闭后继续跟随游戏窗口", config_.hide_on_blur,
        D2D1::RectF(column, 426, right, 496), [this](float) { config_.hide_on_blur = !config_.hide_on_blur; apply(); });
    toggle(L"录屏与截图隐藏", L"捕获画面中不包含监控条", config_.exclude_capture,
        D2D1::RectF(column, 508, right, 578), [this](float) { config_.exclude_capture = !config_.exclude_capture; apply(); });
}
void SettingsWindow::metrics_page(float width) {
    const float left = 272, span = width - 300, box = (span - 30) / 4;
    if (disk_details_) {
        const auto disks = status_.value("snapshot", Json::object()).value("disks", Json::array());
        label(L"硬盘温度", left, 148, span - 120, 30, heading_.Get(), mint);
        row_action(L"‹ 返回项目", D2D1::RectF(width - 150, 148, width - 28, 182), [this](float) { disk_details_ = false; });
        const auto pages = std::max(size_t{1}, (disks.size() + 5) / 6); disk_page_ = std::min(disk_page_, pages - 1);
        float y = 205;
        for (size_t i = disk_page_ * 6; i < std::min(disks.size(), disk_page_ * 6 + 6); ++i) {
            const auto& disk = disks[i]; const auto sensor = disk.value("temperature", Json::object());
            const bool valid = sensor.value("state", std::string{}) == "valid" && sensor.value("value", Json{}).is_number();
            list_surface(D2D1::RectF(left, y, left + span, y + 70));
            label(wide(disk.value("name", std::string{})), left + 14, y + 5, span - 130, 27, body_.Get(), white);
            label(valid ? std::format(L"{:.0f}°C", sensor["value"].get<double>()) : L"不可用", left + span - 108, y + 5, 94, 27, mono_.Get(),
                valid ? D2D1::ColorF(tone_rgb(metric_tone("disk_temperature", std::optional<double>(sensor["value"].get<double>())))) : muted);
            label(wide(sensor.value(valid ? "source" : "reason", std::string{})), left + 14, y + 37, span - 28, 25, small_.Get(), muted); y += 78;
        }
        if (disks.empty()) label(L"未发现可读取的硬盘，等待设备采样。", left, 245, span, 28, body_.Get(), muted);
        page_buttons(width - 28, 694, disk_page_, pages); return;
    }
    const std::vector<std::pair<std::wstring, std::vector<std::string>>> groups{
        {L"帧率", {"fps", "frametime", "low1", "low01"}},
        {L"CPU", {"cpu_temperature", "cpu_load", "cpu_clock", "process_cpu"}},
        {L"GPU", {"gpu_temperature", "gpu_load", "gpu_clock", "gpu_power", "gpu_fan", "vram"}},
        {L"内存、硬盘与游戏", {"memory_load", "memory_used", "process_memory", "session", "disk_temperature", "obs"}}};
    float y = 148;
    for (const auto& [name, ids] : groups) {
        label(name, left, y, span, 25, heading_.Get(), white);
        const float label_width = name.size() * 17.f + 18;
        line(left + label_width, y + 13, left + span, y + 13, tint(edge, .6f)); y += 32;
        for (size_t i = 0; i < ids.size(); ++i) {
            const auto id = ids[i]; const float x = left + static_cast<float>(i % 4) * (box + 10), row = y + static_cast<float>(i / 4) * 52;
            const auto rect = D2D1::RectF(x, row, x + box, row + 46); const bool checked = id == "obs" ? config_.show_obs : contains(config_.metrics, id);
            fill(rect, hovered(rect) ? panel_high : panel, 8);
            outline(rect, checked ? tint(blue, .5f) : hovered(rect) ? edge : tint(edge, .35f), 8);
            const auto mark = D2D1::RectF(x + 12, row + 9, x + 27, row + 24);
            fill(mark, checked ? blue : tint(edge, .65f), 4);
            if (checked) {
                line(x + 15, row + 16, x + 18, row + 19, background, 1.5f);
                line(x + 18, row + 19, x + 24, row + 13, background, 1.5f);
            }
            label(id == "obs" ? L"OBS 录制状态" : metric_name(id), x + 36, row + 2, box - 46, 22, body_.Get(), checked ? white : muted);
            const auto value = id == "obs" ? (checked ? obs_label(status_.value("snapshot", Json::object()).value("obs_state", std::string{})) : L"默认关闭") : metric_value(id);
            label(value, x + 36, row + 24, box - 46, 19, id == "obs" ? small_.Get() : mono_.Get(), checked ? metric_color(id) : muted);
            add(rect, [this, id](float) {
                if (id == "obs") { config_.show_obs = !config_.show_obs; apply(); return; }
                auto& metrics = config_.metrics; const auto it = std::find(metrics.begin(), metrics.end(), id);
                if (it == metrics.end()) metrics.push_back(id); else if (metrics.size() > 1) metrics.erase(it);
                apply();
            });
        }
        y += static_cast<float>((ids.size() + 3) / 4) * 52 + 10;
    }
    row_action(L"查看各硬盘温度 ›", D2D1::RectF(left, y + 2, left + 166, y + 32), [this](float) { disk_details_ = true; }, true);
    const wchar_t* labels[]{L"常规", L"活跃", L"留意", L"偏高 / 偏低", L"重点留意"};
    const MetricTone tones[]{MetricTone::good, MetricTone::cool, MetricTone::watch, MetricTone::high, MetricTone::critical};
    for (int i = 0; i < 5; ++i) {
        const float x = left + i * 130.f;
        fill(D2D1::RectF(x, 686, x + 6, 692), D2D1::ColorF(tone_rgb(tones[i])), 3);
        label(labels[i], x + 15, 678, 114, 22, small_.Get(), muted);
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
    const auto surface = panel;
    const auto name = status_.value("snapshot", Json::object()).value("target", Json::object()).value("name", std::string{});
    label(L"游戏", left, 148, 200, 28, heading_.Get(), white);
    row_action(L"＋ 添加游戏", D2D1::RectF(right - 110, 146, right, 180), [this](float) { choose_process(false); }, true);
    if (!name.empty()) label(L"正在监控  " + wide(name), left, 183, span, 24, small_.Get(), mint);
    const auto pages = std::max(size_t{1}, (targets_.size() + 3) / 4); target_page_ = std::min(target_page_, pages - 1);
    fill(D2D1::RectF(left, 222, right, 472), surface, 10);
    label(L"程序", left + 16, 228, span * .48f, 26, small_.Get(), muted);
    label(L"识别方式", left + span * .49f, 228, 120, 26, small_.Get(), muted);
    label(L"状态", right - 264, 228, 70, 26, small_.Get(), muted);
    label(L"操作", right - 160, 228, 140, 26, small_.Get(), muted);
    line(left + 16, 261, right - 16, 261, edge);
    float y = 263;
    if (targets_.empty()) label(L"暂无游戏  ·  点击右上方添加程序", left + 16, 330, span - 32, 30, body_.Get(), muted);
    for (size_t i = target_page_ * 4; i < std::min(targets_.size(), target_page_ * 4 + 4); ++i) {
        const auto item = targets_[i]; const auto path = item.value("path", std::string{});
        const bool known = contains(config_.known_games, path), active = item.value("pid", 0u) != 0;
        label(wide(item.value("name", std::string{})), left + 16, y + 8, span * .46f - 16, 30, body_.Get(), white);
        label(known ? L"手动添加" : L"自动识别", left + span * .49f, y + 8, 100, 30, small_.Get(), muted);
        label(active ? L"运行中" : L"未运行", right - 264, y + 8, 76, 30, small_.Get(), active ? mint : muted);
        row_action(known ? L"移除" : L"记住", D2D1::RectF(right - 160, y + 5, right - 102, y + 44), [this, path, known](float) {
            if (known) forget(config_.known_games, path); else remember(config_, path, false); apply();
        }, !known);
        row_action(L"排除", D2D1::RectF(right - 74, y + 5, right - 16, y + 44), [this, path](float) { remember(config_, path, true); apply(); });
        y += 50; if (y < 460) line(left + 16, y, right - 16, y, edge);
    }
    page_buttons(right, 474, target_page_, pages);
    label(L"排除", left, 518, 200, 28, heading_.Get(), white);
    row_action(L"＋ 添加排除", D2D1::RectF(right - 110, 516, right, 550), [this](float) { choose_process(true); });
    const auto blocked_pages = std::max(size_t{1}, (config_.ignored_processes.size() + 2) / 3);
    blacklist_page_ = std::min(blacklist_page_, blocked_pages - 1);
    const float bottom = config_.ignored_processes.empty() ? 650.f : 704.f;
    fill(D2D1::RectF(left, 565, right, bottom), surface, 10);
    y = 571;
    for (size_t i = blacklist_page_ * 3; i < std::min(config_.ignored_processes.size(), blacklist_page_ * 3 + 3); ++i) {
        const auto process = config_.ignored_processes[i];
        label(wide(process), left + 16, y + 5, span - 240, 30, body_.Get(), white);
        label(L"不监控", right - 264, y + 5, 90, 30, small_.Get(), muted);
        row_action(L"移除", D2D1::RectF(right - 74, y + 2, right - 16, y + 39), [this, process](float) { forget(config_.ignored_processes, process); apply(); });
        y += 42; if (i + 1 < std::min(config_.ignored_processes.size(), blacklist_page_ * 3 + 3)) line(left + 16, y, right - 16, y, edge);
    }
    if (config_.ignored_processes.empty()) {
        label(L"还没有排除规则", left + 16, 580, span - 32, 26, body_.Get(), white);

    }
    page_buttons(right, 711, blacklist_page_, blocked_pages);
}
void SettingsWindow::updates_page(float width) {
    const float left = 272, right = width - 28, span = right - left;
    const auto update = status_.value("update", Json::object());
    const auto state = update.value("state", std::string{});
    const bool available = state == "available", ready = state == "ready";
    const bool busy = state == "checking" || state == "downloading" || state == "installing";
    label(L"当前版本", left, 158, 180, 24, small_.Get(), muted);
    label(L"v" + wide(GAMEGAUGE_VERSION), left, 193, span - 230, 44, title_.Get(), white);
    const auto action = D2D1::RectF(right - 150, 194, right, 236);
    if (busy) {
        fill(action, panel, 7);
        text(state == "checking" ? L"检查中…" : state == "downloading" ? L"下载中…" : L"安装中…", action, button_format_.Get(), muted);
    } else button(ready ? L"安装更新" : available ? L"下载更新" : L"检查更新", action,
        [this, ready, available](float) { command(ready ? "install_update" : available ? "download_update" : "check_update"); }, true);
    const auto status_ink = state == "error" ? amber : available || ready ? blue : muted;
    fill(D2D1::RectF(left, 263, left + 5, 268), status_ink, 3);
    auto message = wide(update.value("message", std::string("尚未检查更新")));
    if (available) message = L"发现新版本 v" + wide(update.value("version", std::string{}));
    label(message, left + 16, 250, span - 16, 32, small_.Get(), status_ink);
    line(left, 309, right, 309, edge);
    label(L"自动更新", left, 341, span, 28, heading_.Get(), white);
    toggle(L"检查新版本", L"每六小时检查一次", config_.check_updates,
        D2D1::RectF(left, 388, right, 454), [this](float) { config_.check_updates = !config_.check_updates; if (!config_.check_updates) config_.auto_update = false; apply(); });
    toggle(L"下载并安装", L"游戏退出后安装", config_.auto_update,
        D2D1::RectF(left, 464, right, 530), [this](float) { config_.auto_update = !config_.auto_update; if (config_.auto_update) config_.check_updates = true; apply(); });
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