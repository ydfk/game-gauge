#include "window.h"
#include "common/platform.h"
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
void SettingsWindow::appearance_page(float width) {
    const float left = 272, right = width - 28, span = right - left, half = (span - 14) / 2;
    fill(D2D1::RectF(left, 148, right, 345), panel, 12);
    label(L"游戏窗口预览", left + 18, 160, 200, 24, heading_.Get(), white);
    const float bar_width = std::min(span - 48, 395.f);
    float x = left + (span - bar_width) / 2;
    if (config_.anchor == 1) x = left + 18;
    if (config_.anchor == 2) x = right - bar_width - 18;
    const float y = config_.anchor == 3 ? 297.f : 205.f;
    fill(D2D1::RectF(x, y, x + bar_width, y + 25), background, 3);
    label(L"FPS 144   CPU 65°C 24%   GPU 62°C 97%", x + 5, y + 2, bar_width - 10, 21, small_.Get(), mint);
    label(L"位置示意 · 仅在游戏窗口内显示", left + 18, 253, span - 36, 22, small_.Get(), muted);
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
            label((checked ? L"✓ " : L"   ") + metric_name(id), x + 10, row + 3, box - 20, 22, small_.Get(), checked ? white : muted);
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
    button(L"上一页", D2D1::RectF(right - 176, 222, right - 94, 260), [this](float) { if (target_page_) --target_page_; });
    button(L"下一页", D2D1::RectF(right - 82, 222, right, 260), [this, pages](float) { target_page_ = (target_page_ + 1) % pages; });
    float y = 282;
    for (size_t i = target_page_ * 4; i < std::min(targets_.size(), target_page_ * 4 + 4); ++i) {
        const auto item = targets_[i]; const auto process = item.value("name", std::string{}), path = item.value("path", std::string{});
        fill(D2D1::RectF(left, y, right, y + 48), panel, 8);
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
    button(L"翻页", D2D1::RectF(right - 82, 526, right, 562), [this, blocked_pages](float) { blacklist_page_ = (blacklist_page_ + 1) % blocked_pages; });
    y = 578;
    for (size_t i = blacklist_page_ * 3; i < std::min(config_.ignored_processes.size(), blacklist_page_ * 3 + 3); ++i) {
        const auto process = config_.ignored_processes[i]; fill(D2D1::RectF(left, y, right, y + 42), panel, 8);
        label(wide(process), left + 14, y + 8, span - 135, 25, body_.Get(), muted);
        button(L"移除", D2D1::RectF(right - 97, y + 4, right - 12, y + 38), [this, process](float) {
            std::erase(config_.ignored_processes, process); apply(); }); y += 51;
    }
    if (config_.ignored_processes.empty()) label(L"暂无排除程序", left, y, span, 25, small_.Get(), muted);
}
void SettingsWindow::history_page(float width) {
    const float left = 272, right = width - 28, span = right - left;
    const auto pages = std::max(size_t{1}, (history_count_ + 3) / 4); history_page_index_ = std::min(history_page_index_, pages - 1);
    label(std::format(L"{} 次游戏记录", history_count_), left, 148, span - 220, 28, heading_.Get(), white);
    button(L"上一页", D2D1::RectF(right - 190, 144, right - 101, 182), [this](float) { if (history_page_index_) --history_page_index_; refresh(); });
    button(L"下一页", D2D1::RectF(right - 89, 144, right, 182), [this, pages](float) { history_page_index_ = (history_page_index_ + 1) % pages; refresh(); });
    float y = 205;
    for (size_t i = 0; i < history_.size(); ++i) {
        const auto& row = history_[i]; fill(D2D1::RectF(left, y, right, y + 117), panel, 10);
        label(wide(row.value("game", std::string{})), left + 17, y + 10, span - 210, 27, heading_.Get(), white);
        const auto state = row.value("status", std::string{});
        label(state == "running" ? L"进行中" : state == "completed" ? L"已结束" : L"监控已结束", right - 140, y + 10, 120, 27, small_.Get(), mint);
        label(local_time(row.value("started_ms", 0ull)) + L"  ·  游玩 " + value_text(row, "active_seconds", L" 秒"), left + 17, y + 42, span - 34, 25, small_.Get(), muted);
        label(L"平均 FPS " + value_text(row, "average_fps") + L"    最高 FPS " + value_text(row, "maximum_fps") +
            L"    CPU 最高 " + value_text(row, "cpu_max_celsius", L"°C") + L"    GPU 最高 " + value_text(row, "gpu_max_celsius", L"°C"),
            left + 17, y + 77, span - 34, 23, small_.Get(), white); y += 133;
    }
    if (history_.empty()) label(L"开始游戏后自动记录，游戏退出后可在这里查看。", left, 230, span, 30, body_.Get(), muted);
    const auto error = status_.value("snapshot", Json::object()).value("history_error", std::string{});
    if (!error.empty()) label(L"历史保存失败：" + wide(error), left, 745, span, 25, small_.Get(), D2D1::ColorF(0xFFB75E));
}
}
