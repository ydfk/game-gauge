#include "window.h"
#include "common/platform.h"
#include "hud/renderer.h"
#include "version.h"
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
    return std::find(values.begin(), values.end(), value) != values.end();
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
    fill(rect, checked ? selected : panel, 8);
}
void SettingsWindow::page_buttons(float right, float y, size_t& page, size_t pages, bool reload) {
    button(L"上一页", D2D1::RectF(right - 190, y, right - 101, y + 36), [this, &page, reload](float) { if (page) --page; if (reload) refresh(); });
    button(L"下一页", D2D1::RectF(right - 89, y, right, y + 36), [this, &page, pages, reload](float) { if (page + 1 < pages) ++page; if (reload) refresh(); });
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
            list_surface(rect, checked);
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
    const auto value = utf8(excluded ? std::filesystem::path(path).filename().wstring() : std::wstring(path));
    auto& list = excluded ? config_.ignored_processes : config_.known_games;
    if (!contains(list, value)) list.push_back(value);
    config_.auto_target = true; apply();
}
void SettingsWindow::games_page(float width) {
    const float left = 272, right = width - 28, span = right - left;
    const auto snapshot = status_.value("snapshot", Json::object());
    const auto name = snapshot.value("target", Json::object()).value("name", std::string{});
    label(name.empty() ? L"等待游戏启动" : L"当前游戏 · " + wide(name), left, 148, span, 30, heading_.Get(), white);
    label(L"自动识别全屏和窗口游戏；未识别的程序可设为游戏。", left, 183, span, 25, small_.Get(), muted);
    button(L"添加游戏…", D2D1::RectF(left, 222, left + 165, 260), [this](float) { choose_process(false); });
    button(L"排除程序…", D2D1::RectF(left + 178, 222, left + 343, 260), [this](float) { choose_process(true); });
    const auto pages = std::max(size_t{1}, (targets_.size() + 3) / 4); target_page_ = std::min(target_page_, pages - 1);
    page_buttons(right, 222, target_page_, pages);
    float y = 282;
    for (size_t i = target_page_ * 4; i < std::min(targets_.size(), target_page_ * 4 + 4); ++i) {
        const auto item = targets_[i]; const auto process = item.value("name", std::string{}), path = item.value("path", std::string{});
        list_surface(D2D1::RectF(left, y, right, y + 48));
        label(wide(process), left + 14, y + 11, span - 265, 26, body_.Get(), white);
        button(contains(config_.known_games, path) ? L"已记住" : L"设为游戏", D2D1::RectF(right - 244, y + 6, right - 129, y + 42),
            [this, path](float) { if (!path.empty() && !contains(config_.known_games, path)) config_.known_games.push_back(path); config_.auto_target = true; apply(); });
        button(contains(config_.ignored_processes, process) ? L"已排除" : L"排除", D2D1::RectF(right - 117, y + 6, right - 12, y + 42),
            [this, process](float) { if (!contains(config_.ignored_processes, process)) config_.ignored_processes.push_back(process); apply(); });
        y += 58;
    }
    label(L"已排除的程序", left, 530, span - 185, 28, heading_.Get(), white);
    const auto blocked_pages = std::max(size_t{1}, (config_.ignored_processes.size() + 2) / 3);
    blacklist_page_ = std::min(blacklist_page_, blocked_pages - 1);
    page_buttons(right, 526, blacklist_page_, blocked_pages);
    y = 578;
    for (size_t i = blacklist_page_ * 3; i < std::min(config_.ignored_processes.size(), blacklist_page_ * 3 + 3); ++i) {
        const auto process = config_.ignored_processes[i]; list_surface(D2D1::RectF(left, y, right, y + 48));
        label(wide(process), left + 14, y + 11, span - 135, 26, body_.Get(), white);
        button(L"移除", D2D1::RectF(right - 97, y + 6, right - 12, y + 42), [this, process](float) {
            std::erase(config_.ignored_processes, process); apply(); }); y += 58;
    }
    if (config_.ignored_processes.empty()) label(L"暂无排除程序", left, y, span, 25, small_.Get(), muted);
}
void SettingsWindow::history_page(float width) {
    const float left = 272, right = width - 28, span = right - left;
    const auto pages = std::max(size_t{1}, (history_count_ + 3) / 4); history_page_index_ = std::min(history_page_index_, pages - 1);
    label(std::format(L"{} 次游戏记录", history_count_), left, 148, span - 220, 28, heading_.Get(), white);
    page_buttons(right, 144, history_page_index_, pages, true);
    float y = 205;
    for (size_t i = 0; i < history_.size(); ++i) {
        const auto& row = history_[i]; list_surface(D2D1::RectF(left, y, right, y + 117));
        label(wide(row.value("game", std::string{})), left + 14, y + 11, span - 210, 26, body_.Get(), white);
        const auto state = row.value("status", std::string{});
        label(state == "running" ? L"进行中" : state == "completed" ? L"已结束" : L"监控已结束", right - 140, y + 10, 120, 27, small_.Get(), mint);
        label(local_time(row.value("started_ms", 0ull)) + L"  ·  游玩 " + value_text(row, "active_seconds", L" 秒"), left + 14, y + 42, span - 28, 25, small_.Get(), muted);
        label(L"平均 FPS " + value_text(row, "average_fps") + L"    最高 FPS " + value_text(row, "maximum_fps") +
            L"    CPU 最高 " + value_text(row, "cpu_max_celsius", L"°C") + L"    GPU 最高 " + value_text(row, "gpu_max_celsius", L"°C"),
            left + 14, y + 77, span - 28, 23, small_.Get(), white); y += 127;
    }
    if (history_.empty()) label(L"开始游戏后自动记录，游戏退出后可在这里查看。", left, 230, span, 30, body_.Get(), muted);
    const auto error = status_.value("snapshot", Json::object()).value("history_error", std::string{});
    if (!error.empty()) label(L"历史保存失败：" + wide(error), left, 745, span, 25, small_.Get(), D2D1::ColorF(0xFFB75E));
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
