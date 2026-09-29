#include "window.h"
#include "common/platform.h"
#include "hud/renderer.h"
#include "version.h"
#include "host/ipc_server.h"
#include <commdlg.h>
#include <algorithm>
#include <format>
#include <ctime>

namespace gauge {
namespace {
const auto panel = D2D1::ColorF(0x152130), selected = D2D1::ColorF(0x1B2B3C);
const auto white = D2D1::ColorF(0xF2F6FA), muted = D2D1::ColorF(0x93A7BA);
const auto mint = D2D1::ColorF(0x6BE3C3), background = D2D1::ColorF(0x0B111B);
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
std::wstring value_text(const Json& row, const char* key, const wchar_t* unit = L"") {
    return row.contains(key) && row[key].is_number() ? std::format(L"{:.0f}{}", row[key].get<double>(), unit) : L"—";
}
std::wstring local_time(uint64_t milliseconds) {
    const time_t seconds = static_cast<time_t>(milliseconds / 1000); tm value{}; localtime_s(&value, &seconds);
    wchar_t text[64]{}; wcsftime(text, 64, L"%Y-%m-%d %H:%M", &value); return text;
}
}
void SettingsWindow::list_surface(D2D1_RECT_F rect, bool checked) {
    fill(rect, checked ? selected : panel);
    line(rect.left, rect.bottom, rect.right, rect.bottom, D2D1::ColorF(0x2A3B4D));
}
void SettingsWindow::row_action(const std::wstring& title, D2D1_RECT_F rect, std::function<void(float)> action, bool accent) {
    text(title, rect, small_.Get(), accent ? mint : muted);
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
    fill(D2D1::RectF(left, 148, right, 345), panel, 12);
    label(L"监控条实时预览", left + 18, 160, span - 36, 24, heading_.Get(), white);
    std::vector<HudItem> items;
    for (const auto& row : status_.value("hud_preview", Json::array())) {
        const auto color = row.at("color");
        items.push_back({wide(row.value("label", std::string{})), wide(row.value("value", std::string{})),
            D2D1::ColorF(color[0].get<float>(), color[1].get<float>(), color[2].get<float>()), wide(row.value("group", std::string{}))});
    }
    if (items.empty()) items = hud_items(Snapshot{}, config_);
    const auto size = measure_hud_items(write_.Get(), items, config_);
    const float fit = std::min(1.f, (span - 36) / std::max(1.f, size.width));
    float x = left + (span - size.width * fit) / 2;
    if (config_.anchor == 1) x = left + 18;
    if (config_.anchor == 2) x = right - 18 - size.width * fit;
    const float y = config_.anchor == 3 ? 324.f - size.height * fit : 202.f;
    D2D1_MATRIX_3X2_F saved; target_->GetTransform(&saved);
    target_->SetTransform(D2D1::Matrix3x2F::Scale(fit, fit) * D2D1::Matrix3x2F::Translation(x, y) * saved);
    draw_hud_items(target_.Get(), write_.Get(), items, config_, size.width, size.height);
    target_->SetTransform(saved);
    label(fit < 1 ? L"按可用宽度缩小显示" : L"与游戏内共用字号、颜色、间距及实时读数", left + 18, 253, span - 36, 22, small_.Get(), muted);
    toggle(L"游戏内监控", L"识别游戏后自动显示，退出后关闭", config_.enabled,
        D2D1::RectF(left, 365, left + half, 427), [this](float) { config_.enabled = !config_.enabled; apply(); });
    toggle(L"录屏隐藏监控", L"让录制画面保持干净", config_.exclude_capture,
        D2D1::RectF(left + half + 14, 365, right, 427), [this](float) { config_.exclude_capture = !config_.exclude_capture; apply(); });
    slider(L"字号", std::format(L"{:.0f}", config_.font_size), static_cast<float>((config_.font_size - 10) / 22),
        D2D1::RectF(left, 445, left + half, 517), [this, left, half](float x) {
            config_.font_size = 10 + 22 * std::clamp((x - left - 18) / (half - 36), 0.f, 1.f); apply(); });
    slider(L"背景深浅", std::format(L"{:.0f}%", config_.opacity * 100), static_cast<float>(config_.opacity),
        D2D1::RectF(left + half + 14, 445, right, 517), [this, left, half](float x) {
            config_.opacity = std::clamp((x - left - half - 32) / (half - 36), 0.f, 1.f); apply(); });
    label(L"位置", left, 545, 150, 28, heading_.Get(), white);
    const wchar_t* anchors[]{L"顶部居中", L"左上角", L"右上角", L"底部居中"};
    const float box = (span - 36) / 4;
    for (int i = 0; i < 4; ++i) button(anchors[i], D2D1::RectF(left + i * (box + 12), 587, left + i * (box + 12) + box, 629),
        [this, i](float) { config_.anchor = i; config_.margin_x = 0; config_.margin_y = 0; config_.preview = false; apply(); }, config_.anchor == i);
    label(L"切换位置会立即更新预览；回到游戏后自动应用。", left, 649, span, 24, small_.Get(), muted);
    toggle(L"OBS 录制状态", L"显示录制、暂停和连接状态", config_.show_obs,
        D2D1::RectF(left, 685, right, 747), [this](float) { config_.show_obs = !config_.show_obs; apply(); });
}
void SettingsWindow::metrics_page(float width) {
    const float left = 272, span = width - 300, box = (span - 30) / 4;
    const std::vector<std::pair<std::wstring, std::vector<std::string>>> groups{
        {L"帧率", {"fps", "frametime", "low1", "low01"}},
        {L"CPU", {"cpu_temperature", "cpu_load", "cpu_clock", "process_cpu"}},
        {L"GPU", {"gpu_temperature", "gpu_load", "gpu_clock", "gpu_power", "gpu_fan", "vram"}},
        {L"内存与游戏", {"memory_load", "memory_used", "process_memory", "session"}}};
    float y = 148;
    for (const auto& [name, ids] : groups) {
        label(name, left, y, span, 27, heading_.Get(), mint); y += 38;
        for (size_t i = 0; i < ids.size(); ++i) {
            const auto id = ids[i]; const float x = left + static_cast<float>(i % 4) * (box + 10), row = y + static_cast<float>(i / 4) * 57;
            const auto rect = D2D1::RectF(x, row, x + box, row + 48); const bool checked = contains(config_.metrics, id);
            fill(rect, checked ? selected : panel, 8);
            label((checked ? L"✓ " : L"   ") + metric_name(id), x + 14, row + 3, box - 28, 22, body_.Get(), white);
            label(metric_value(id), x + 28, row + 24, box - 38, 19, small_.Get(), checked ? mint : muted);
            add(rect, [this, id](float) {
                auto& metrics = config_.metrics; const auto it = std::find(metrics.begin(), metrics.end(), id);
                if (it == metrics.end()) metrics.push_back(id); else if (metrics.size() > 1) metrics.erase(it);
                apply();
            });
        }
        y += static_cast<float>((ids.size() + 3) / 4) * 57 + 26;
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
    const auto edge = D2D1::ColorF(0x233140), surface = D2D1::ColorF(0x101A27);
    const auto name = status_.value("snapshot", Json::object()).value("target", Json::object()).value("name", std::string{});
    label(L"游戏", left, 148, 200, 28, heading_.Get(), white);
    row_action(L"＋ 添加游戏", D2D1::RectF(right - 110, 146, right, 180), [this](float) { choose_process(false); }, true);
    label(name.empty() ? L"游戏启动后自动识别；也可手动添加。" : L"正在监控  " + wide(name), left, 183, span, 24, small_.Get(), muted);
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
        label(L"系统和常见工具已自动过滤，这里只显示你添加的程序。", left + 16, 611, span - 32, 22, small_.Get(), muted);
    }
    page_buttons(right, 711, blacklist_page_, blocked_pages);
}
void SettingsWindow::history_page(float width) {
    const float left = 272, right = width - 28, span = right - left;
    const auto edge = D2D1::ColorF(0x233140);
    const auto pages = std::max(size_t{1}, (history_count_ + 3) / 4); history_page_index_ = std::min(history_page_index_, pages - 1);
    label(std::format(L"共 {} 次游戏", history_count_), left, 148, span, 28, heading_.Get(), white);
    const float duration_x = left + span * .35f, fps_x = left + span * .47f, temp_x = left + span * .60f, state_x = right - 172;
    fill(D2D1::RectF(left, 201, right, 590), D2D1::ColorF(0x101A27), 10);
    label(L"游戏 / 开始时间", left + 16, 212, span * .32f, 26, small_.Get(), muted);
    label(L"游玩时长", duration_x, 212, 90, 26, small_.Get(), muted);
    label(L"平均 FPS", fps_x, 212, 90, 26, small_.Get(), muted);
    label(L"最高温度", temp_x, 212, 110, 26, small_.Get(), muted);
    label(L"状态", state_x, 212, 85, 26, small_.Get(), muted);
    line(left + 16, 248, right - 16, 248, edge);
    float y = 252;
    for (const auto& row : history_) {
        label(wide(row.value("game", std::string{})), left + 16, y + 12, span * .33f - 20, 26, body_.Get(), white);
        label(local_time(row.value("started_ms", 0ull)), left + 16, y + 43, span * .33f - 20, 23, small_.Get(), muted);
        const auto seconds = static_cast<unsigned>(row.value("active_seconds", 0.0));
        const auto duration = seconds >= 3600 ? std::format(L"{} 小时 {} 分", seconds / 3600, seconds / 60 % 60) :
            seconds >= 60 ? std::format(L"{} 分 {} 秒", seconds / 60, seconds % 60) : std::format(L"{} 秒", seconds);
        label(duration, duration_x, y + 13, span * .12f - 8, 27, small_.Get(), white);
        label(value_text(row, "average_fps"), fps_x, y + 12, 90, 28, mono_.Get(), white);
        label(L"峰值 " + value_text(row, "maximum_fps"), fps_x, y + 43, 95, 23, small_.Get(), muted);
        label(L"CPU " + value_text(row, "cpu_max_celsius", L"°C"), temp_x, y + 13, 110, 26, small_.Get(), white);
        label(L"GPU " + value_text(row, "gpu_max_celsius", L"°C"), temp_x, y + 43, 110, 23, small_.Get(), muted);
        const bool active = row.value("status", std::string{}) == "running";
        label(active ? L"进行中" : L"已结束", state_x, y + 13, 85, 27, small_.Get(), active ? mint : muted);
        row_action(L"删除", D2D1::RectF(right - 62, y + 14, right - 12, y + 58), [this, id = row.at("id").get<std::string>()](float) {
            try { ipc_request({{"command", "delete_history"}, {"id", id}}); history_page_index_ = 0; refresh(); }
            catch (const std::exception& e) { error_ = e.what(); }
        });
        y += 82; if (y < 575) line(left + 16, y, right - 16, y, edge);
    }
    if (history_.empty()) {
        label(L"还没有游戏记录", left + 24, 334, span - 48, 32, heading_.Get(), white);
        label(L"开始游戏后会自动记录，结束后可在这里查看。", left + 24, 376, span - 48, 28, body_.Get(), muted);
    }
    page_buttons(right, 608, history_page_index_, pages, true);
    const auto error = status_.value("snapshot", Json::object()).value("history_error", std::string{});
    if (!error.empty()) label(L"历史保存失败：" + wide(error), left, 665, span, 25, small_.Get(), D2D1::ColorF(0xFFB75E));
}
void SettingsWindow::updates_page(float width) {
    const float left = 272, right = width - 28, span = right - left;
    const auto update = status_.value("update", Json::object());
    const auto state = update.value("state", std::string{});
    list_surface(D2D1::RectF(left, 148, right, 272));
    label(L"游戏仪表 " + wide(update.value("current", std::string(GAMEGAUGE_VERSION))), left + 14, 162, span - 28, 28, heading_.Get(), white);
    label(wide(update.value("message", std::string("等待主程序连接"))), left + 14, 203, span - 28, 26, body_.Get(), muted);
    button(L"检查更新", D2D1::RectF(left, 294, left + 150, 334), [this](float) { command("check_update"); });
    if (state == "available") button(L"下载更新", D2D1::RectF(left + 164, 294, left + 314, 334), [this](float) { command("download_update"); }, true);
    if (state == "ready") button(L"安装更新", D2D1::RectF(left + 164, 294, left + 314, 334), [this](float) { command("install_update"); }, true);
    toggle(L"自动检查更新", L"启动后及每六小时检查一次稳定版", config_.check_updates,
        D2D1::RectF(left, 366, right, 438), [this](float) { config_.check_updates = !config_.check_updates; if (!config_.check_updates) config_.auto_update = false; apply(); });
    toggle(L"自动下载并更新", L"游戏退出后启动安装，Windows 可能要求管理员确认", config_.auto_update,
        D2D1::RectF(left, 452, right, 524), [this](float) { config_.auto_update = !config_.auto_update; if (config_.auto_update) config_.check_updates = true; apply(); });
    const auto repository = update.value("repository", std::string{});
    label(repository.empty() ? L"本地构建尚未配置 GitHub 发布仓库" : L"更新来源：GitHub / " + wide(repository), left, 557, span, 26, small_.Get(), muted);
    label(L"手动更新也可直接运行新版安装包，配置与历史会保留。", left, 591, span, 26, small_.Get(), muted);
}
}
