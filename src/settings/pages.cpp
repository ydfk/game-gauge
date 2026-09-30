#include "window.h"
#include "common/platform.h"
#include <algorithm>
#include <format>

namespace gauge {
namespace {
const auto background = D2D1::ColorF(0x0B111B);
const auto panel = D2D1::ColorF(0x152130);
const auto panel_high = D2D1::ColorF(0x1B2B3C);
const auto edge = D2D1::ColorF(0x2A3B4D);
const auto white = D2D1::ColorF(0xF2F6FA);
const auto muted = D2D1::ColorF(0x93A7BA);
const auto mint = D2D1::ColorF(0x6BE3C3);
const auto amber = D2D1::ColorF(0xFFB75E);
const auto blue = D2D1::ColorF(0x71B8FA);
const std::vector<std::pair<std::string, std::wstring>> all_metrics{
    {"fps", L"FPS"}, {"frametime", L"帧时间"}, {"low1", L"1% Low"}, {"low01", L"0.1% Low"},
    {"cpu_temperature", L"CPU 温度"}, {"cpu_load", L"CPU 占用"}, {"cpu_clock", L"CPU 频率"},
    {"gpu_temperature", L"GPU 温度"}, {"gpu_load", L"GPU 占用"}, {"gpu_clock", L"GPU 频率"},
    {"gpu_power", L"GPU 功耗"}, {"gpu_fan", L"GPU 风扇"}, {"vram", L"显存用量"},
    {"memory_load", L"内存占用"}, {"memory_used", L"内存用量"}, {"process_cpu", L"游戏 CPU"},
    {"process_memory", L"游戏内存"}, {"disk_temperature", L"硬盘温度"}, {"session", L"运行时长"}
};
std::wstring reading(const Json& metric, const wchar_t* unit = L"", int precision = 0) {
    if (!metric.is_object() || metric.value("state", std::string{}) != "valid" || !metric.contains("value") || metric["value"].is_null()) return L"—";
    return std::format(L"{:.{}f}{}", metric["value"].get<double>(), precision, unit);
}
std::wstring text_field(const Json& object, const char* field) {
    return object.is_object() && object.contains(field) && object[field].is_string() ? wide(object[field].get<std::string>()) : L"—";
}
}
std::wstring SettingsWindow::metric_name(const std::string& id) const {
    auto it = std::find_if(all_metrics.begin(), all_metrics.end(), [&](const auto& item) { return item.first == id; });
    return it == all_metrics.end() ? wide(id) : it->second;
}
std::wstring SettingsWindow::metric_value(const std::string& id) const {
    if (!status_.is_object() || !status_.contains("snapshot")) return L"—";
    const auto& snapshot = status_["snapshot"];
    if (id == "session") {
        const int seconds = static_cast<int>(snapshot.value("session_seconds", 0.0));
        return std::format(L"{:02}:{:02}:{:02}", seconds / 3600, seconds / 60 % 60, seconds % 60);
    }
    if (id == "vram") {
        const auto selected = snapshot.value("selected_gpu", std::string{});
        for (const auto& gpu : snapshot.value("gpus", Json::array())) if (gpu.value("id", std::string{}) == selected) {
            const auto memory = gpu.value("memory_used", Json::object());
            if (memory.value("state", std::string{}) != "valid" || memory["value"].is_null()) return L"—";
            return std::format(L"{:.1f} GiB", memory["value"].get<double>() / 1073741824.0);
        }
        return L"—";
    }
    if (id.starts_with("gpu_")) {
        const auto selected = snapshot.value("selected_gpu", std::string{});
        for (const auto& gpu : snapshot.value("gpus", Json::array())) if (gpu.value("id", std::string{}) == selected) {
            const auto key = id.substr(4);
            return reading(gpu.value(key, Json::object()), key == "temperature" ? L"°C" : key == "load" || key == "fan" ? L"%" : key == "clock" ? L" MHz" : L" W");
        }
        return L"—";
    }
    const wchar_t* unit = L"";
    if (id == "cpu_temperature" || id == "disk_temperature") unit = L"°C";
    else if (id == "cpu_load" || id == "memory_load" || id == "process_cpu") unit = L"%";
    else if (id == "cpu_clock") unit = L" MHz";
    else if (id == "frametime") unit = L" ms";
    else if (id == "memory_used" || id == "process_memory") {
        const auto item = snapshot.value(id, Json::object());
        if (!item.is_object() || item.value("state", std::string{}) != "valid") return L"—";
        return std::format(L"{:.1f} GiB", item["value"].get<double>() / 1073741824.0);
    }
    return reading(snapshot.value(id, Json::object()), unit, id == "frametime" ? 1 : 0);
}
std::wstring SettingsWindow::metric_detail(const std::string& id) const {
    if (!status_.is_object() || !status_.contains("snapshot")) return L"主程序未连接";
    const auto& snapshot = status_["snapshot"];
    Json metric;
    if (id == "session") return L"本次游戏目标的运行时长";
    if (id.starts_with("gpu_") || id == "vram") {
        const auto selected = snapshot.value("selected_gpu", std::string{});
        if (selected.empty()) return L"尚未可靠确定游戏使用的显卡，可在“设备与游戏”指定";
        for (const auto& gpu : snapshot.value("gpus", Json::array())) if (gpu.value("id", std::string{}) == selected) {
            metric = gpu.value(id == "vram" ? "memory_used" : id.substr(4), Json::object()); break;
        }
    } else metric = snapshot.value(id, Json::object());
    if (!metric.is_object()) return L"等待指标数据";
    const auto source = text_field(metric, "source"), reason = text_field(metric, "reason");
    if (metric.value("state", std::string{}) == "valid") return L"数据来源：" + source;
    return reason == L"—" ? L"当前无可用读数" : L"不可用原因：" + reason;
}
void SettingsWindow::metric_preview(float x, float y, float width) {
    fill(D2D1::RectF(x, y, x + width, y + 142), panel, 14);
    label(L"实时预览", x + 21, y + 16, 170, 25, heading_.Get(), white);
    label(L"实际悬浮条会根据游戏窗口宽度自动裁切", x + 168, y + 19, width - 190, 22, small_.Get(), muted);
    fill(D2D1::RectF(x + 20, y + 58, x + width - 20, y + 112), background, 8);
    float cursor = x + 32;
    for (size_t i = 0; i < config_.metrics.size() && i < 7; ++i) {
        auto name = metric_name(config_.metrics[i]), value = metric_value(config_.metrics[i]);
        if (name.size() > 7) name.resize(7);
        const float cell = std::max(82.f, std::min(142.f, (width - 65) / 7.f));
        if (cursor + cell > x + width - 16) break;
        label(name, cursor, y + 64, cell - 6, 18, small_.Get(), muted);
        label(value, cursor, y + 83, cell - 6, 23, mono_.Get(), i % 3 == 0 ? mint : i % 3 == 1 ? amber : blue);
        cursor += cell;
    }
}
void SettingsWindow::overview(float width) {
    const float left = 272, right = width - 28, span = right - left;
    metric_preview(left, 148, span);
    const float gap = 14, half = (span - gap) / 2;
    toggle(L"显示游戏内监控", L"默认跟随正在运行的无边框游戏", config_.enabled,
        D2D1::RectF(left, 310, left + half, 372), [this](float) { config_.enabled = !config_.enabled; apply(); });
    toggle(L"录制时隐藏", L"请求 Windows 排除捕获，需实际录制验证", config_.exclude_capture,
        D2D1::RectF(left + half + gap, 310, right, 372), [this](float) { config_.exclude_capture = !config_.exclude_capture; apply(); });
    toggle(L"失去焦点时隐藏", L"切回桌面后不覆盖其他窗口", config_.hide_on_blur,
        D2D1::RectF(left, 385, left + half, 447), [this](float) { config_.hide_on_blur = !config_.hide_on_blur; apply(); });
    toggle(L"帧时间曲线", L"可视化最近一段时间的帧波动", config_.graph,
        D2D1::RectF(left + half + gap, 385, right, 447), [this](float) { config_.graph = !config_.graph; apply(); });
    slider(L"文字大小", std::format(L"{:.0f} px", config_.font_size), static_cast<float>((config_.font_size - 10) / 22),
        D2D1::RectF(left, 461, left + half, 533), [this, left, half](float x) {
            config_.font_size = 10 + 22 * std::clamp((x - left - 18) / (half - 36), 0.f, 1.f); apply(); });
    slider(L"背景不透明度", std::format(L"{:.0f}%", config_.opacity * 100), static_cast<float>((config_.opacity - .1) / .9),
        D2D1::RectF(left + half + gap, 461, right, 533), [this, left, half, gap](float x) {
            config_.opacity = .1 + .9 * std::clamp((x - left - half - gap - 18) / (half - 36), 0.f, 1.f); apply(); });
    label(L"位置", left, 552, 100, 25, heading_.Get(), white);
    const wchar_t* anchors[]{L"顶部居中", L"左上角", L"右上角", L"底部居中"};
    const float button_width = (span - 3 * gap) / 4;
    for (int i = 0; i < 4; ++i) button(anchors[i], D2D1::RectF(left + i * (button_width + gap), 587,
        left + i * (button_width + gap) + button_width, 627), [this, i](float) { config_.anchor = i; apply(); }, config_.anchor == i);
    button(L"编辑位置", D2D1::RectF(left, 645, left + 160, 682), [this](float) { command("edit"); });
    button(L"显示预览", D2D1::RectF(left + 174, 645, left + 334, 682), [this](float) { config_.preview = !config_.preview; apply(); }, config_.preview);
    label(L"快捷键  Ctrl + Alt + Shift + F6 显示   ·   F7 移动   ·   F8 重置统计", left + 355, 651,
        span - 355, 26, small_.Get(), muted);
}
void SettingsWindow::metrics(float width) {
    const float left = 272, right = width - 28, span = right - left, gap = 14, half = (span - gap) / 2;
    label(std::format(L"已选择 {} 项", config_.metrics.size()), left, 147, 160, 26, heading_.Get(), white);
    label(L"保持常用指标在前方；未读到的传感器显示“—”，不会伪造数值。", left + 165, 150, span - 165, 23, small_.Get(), muted);
    for (size_t i = 0; i < all_metrics.size(); ++i) {
        const int column = static_cast<int>(i % 2), row = static_cast<int>(i / 2);
        const float x = left + column * (half + gap), y = 187.f + row * 48.f;
        const auto id = all_metrics[i].first;
        const bool selected = std::find(config_.metrics.begin(), config_.metrics.end(), id) != config_.metrics.end();
        auto rect = D2D1::RectF(x, y, x + half, y + 41);
        fill(rect, selected ? panel_high : panel, 8);
        fill(D2D1::RectF(x + 13, y + 11, x + 32, y + 30), selected ? mint : edge, 4);
        if (selected) label(L"✓", x + 15, y + 10, 16, 18, small_.Get(), background);
        label(all_metrics[i].second, x + 44, y + 9, half - 150, 23, body_.Get(), selected ? white : muted);
        label(metric_value(id), x + half - 116, y + 9, 102, 23, mono_.Get(), selected ? mint : muted);
        add(rect, [this, id](float) {
            selected_metric_ = id;
            auto& values = config_.metrics;
            auto it = std::find(values.begin(), values.end(), id);
            if (it != values.end()) { if (values.size() > 1) values.erase(it); }
            else values.push_back(id);
            apply();
        });
    }
    label(L"顶部条顺序", left, 653, 160, 26, heading_.Get(), white);
    label(L"点击下方指标将它移到最前面", left + 165, 657, span - 170, 20, small_.Get(), muted);
    float x = left;
    for (size_t i = 0; i < config_.metrics.size(); ++i) {
        const auto id = config_.metrics[i];
        const float box = std::max(72.f, std::min(115.f, (span - 7 * 8) / 8.f));
        if (x + box > right) break;
        button(metric_name(id), D2D1::RectF(x, 683, x + box, 716), [this, id](float) {
            auto& values = config_.metrics;
            auto it = std::find(values.begin(), values.end(), id);
            if (it != values.end()) { values.erase(it); values.insert(values.begin(), id); apply(); }
        }, i == 0);
        x += box + 8;
    }
    label(selected_metric_.empty() ? L"点击上方指标可查看来源或不可用原因" : metric_name(selected_metric_) + L" · " + metric_detail(selected_metric_),
        left, 733, span, 24, small_.Get(), muted);
}
void SettingsWindow::hardware(float width) {
    const float left = 272, right = width - 28, span = right - left;
    const auto snapshot = status_.value("snapshot", Json::object());
    fill(D2D1::RectF(left, 148, right, 214), panel, 12);
    label(L"CPU", left + 18, 163, 68, 22, small_.Get(), mint);
    label(text_field(snapshot, "cpu"), left + 90, 159, span - 290, 30, heading_.Get(), white);
    const auto cpu_temperature = snapshot.value("cpu_temperature", Json::object());
    const bool cpu_live = cpu_temperature.value("state", std::string{}) == "valid";
    label(cpu_live ? L"Tctl " + reading(cpu_temperature, L"°C") + L" · PawnIO 原生采集" :
        L"Tctl 暂不可用 · 可启动独立传感器", left + 90, 186, span - 290, 18,
        small_.Get(), cpu_live ? mint : muted);
    if (!cpu_live) button(L"启动温度采集", D2D1::RectF(right - 164, 163, right - 14, 200),
        [this](float) { start_cpu_sensor(); });
    label(L"用于监控的显卡", left, 232, 280, 28, heading_.Get(), white);
    button(L"自动选择", D2D1::RectF(right - 154, 228, right, 265), [this](float) { config_.gpu_id.clear(); apply(); }, config_.gpu_id.empty());
    float y = 275;
    for (const auto& gpu : snapshot.value("gpus", Json::array())) {
        const auto id = gpu.value("id", std::string{});
        fill(D2D1::RectF(left, y, right, y + 68), panel, 10);
        label(text_field(gpu, "name"), left + 18, y + 9, span - 170, 26, body_.Get(), white);
        label(L"温度 " + reading(gpu.value("temperature", Json::object()), L"°C") + L"   ·   占用 " +
            reading(gpu.value("load", Json::object()), L"%"), left + 18, y + 37, span - 170, 22, small_.Get(), muted);
        button(config_.gpu_id == id ? L"已选择" : L"选择", D2D1::RectF(right - 126, y + 15, right - 14, y + 52),
            [this, id](float) { config_.gpu_id = id; apply(); }, config_.gpu_id == id);
        y += 78;
    }
    label(L"游戏窗口", left, y + 6, 200, 30, heading_.Get(), white);
    const auto target_count = targets_.is_array() ? targets_.size() : 0;
    const auto target_pages = std::max(size_t{1}, (target_count + 1) / 2);
    target_page_ = std::min(target_page_, target_pages - 1);
    if (target_pages > 1) {
        button(L"上一组", D2D1::RectF(right - 404, y, right - 290, y + 37), [this](float) {
            if (target_page_) --target_page_; InvalidateRect(window_, nullptr, FALSE);
        });
        button(L"下一组", D2D1::RectF(right - 281, y, right - 167, y + 37), [this, target_pages](float) {
            target_page_ = (target_page_ + 1) % target_pages; InvalidateRect(window_, nullptr, FALSE);
        });
    }
    button(L"自动识别", D2D1::RectF(right - 154, y, right, y + 37), [this](float) {
        config_.auto_target = true; config_.target_pid = 0; apply(); }, config_.auto_target);
    y += 48;
    int count{};
    for (size_t index = target_page_ * 2; index < std::min(target_count, target_page_ * 2 + 2); ++index) {
        const auto& target = targets_[index];
        ++count;
        const auto pid = target.value("pid", 0u);
        fill(D2D1::RectF(left, y, right, y + 44), panel, 8);
        label(text_field(target, "name") + std::format(L"   ·   PID {}", pid), left + 15, y + 9, span - 153, 26, body_.Get(), white);
        button(config_.target_pid == pid && !config_.auto_target ? L"监控中" : L"监控", D2D1::RectF(right - 126, y + 5, right - 14, y + 39),
            [this, pid](float) { config_.auto_target = false; config_.target_pid = pid; apply(); }, config_.target_pid == pid && !config_.auto_target);
        y += 50;
    }
    if (!count) {
        fill(D2D1::RectF(left, y, right, y + 53), panel, 8);
        label(L"当前没有可选的游戏窗口。启动游戏后，此处会自动刷新。", left + 16, y + 13,
            span - 32, 26, body_.Get(), muted);
        y += 60;
    }
    const auto displays = snapshot.value("displays", Json::array());
    label(std::format(L"显示器 · 发现 {} 块", displays.size()), left, y + 7, span, 27, heading_.Get(), white);
    y += 42;
    for (size_t index = 0; index < displays.size() && index < 3; ++index) {
        const auto& display = displays[index];
        fill(D2D1::RectF(left, y, right, y + 46), panel, 8);
        label(std::format(L"屏幕 {}", index + 1), left + 15, y + 9, 100, 24, body_.Get(), white);
        label(std::format(L"{} × {}   ·   {}% 缩放", display.value("width", 0), display.value("height", 0),
            display.value("dpi", 96) * 100 / 96), left + 118, y + 9, span - 305, 24, body_.Get(), muted);
        label(display.value("hdr", false) ? L"HDR 已开启" : L"SDR", right - 145, y + 9, 125, 24,
            body_.Get(), display.value("hdr", false) ? mint : muted);
        y += 53;
    }
}
void SettingsWindow::capture(float width) {
    const float left = 272, right = width - 28, span = right - left;
    const auto snapshot = status_.value("snapshot", Json::object());
    const auto capture = status_.value("capture", Json::object());
    fill(D2D1::RectF(left, 148, right, 267), panel, 14);
    label(L"录制排除", left + 22, 165, 250, 29, heading_.Get(), white);
    const bool accepted = capture.value("accepted", false);
    label(accepted ? L"Windows 已接受排除请求" : L"排除请求尚未成功", left + 22, 203, span - 44, 29,
        body_.Get(), accepted ? mint : amber);
    label(L"此状态只表示系统 API 接受；OBS 窗口采集仍需检查实际录制文件。", left + 22, 236, span - 44, 23, small_.Get(), muted);
    toggle(L"向 Windows 请求录制排除", L"不同 OBS 采集后端的表现可能不同", config_.exclude_capture,
        D2D1::RectF(left, 280, right, 344), [this](float) { config_.exclude_capture = !config_.exclude_capture; apply(); });
    fill(D2D1::RectF(left, 360, right, 506), panel, 14);
    label(L"采集状态", left + 22, 378, 190, 26, heading_.Get(), white);
    const auto frame_status = text_field(snapshot, "frame_status");
    const auto fps = snapshot.value("fps", Json::object());
    const bool frames_valid = fps.is_object() && fps.value("state", std::string{}) == "valid";
    label(frame_status, left + 22, 415, span - 44, 24, body_.Get(),
        frames_valid ? mint : amber);
    label(L"当前目标：" + text_field(snapshot.value("target", Json::object()), "name"), left + 22, 445,
        span - 44, 22, body_.Get(), muted);
    label(L"所选显卡：" + text_field(snapshot, "selected_gpu") + L"   ·   " + text_field(snapshot, "gpu_selection_reason"),
        left + 22, 474, span - 44, 21, small_.Get(), muted);
    label(L"诊断与会话", left, 528, 260, 29, heading_.Get(), white);
    const float gap = 14, button_width = (span - gap * 2) / 3;
    button(L"重新发现设备", D2D1::RectF(left, 573, left + button_width, 618), [this](float) { command("rediscover"); });
    button(L"重置本次统计", D2D1::RectF(left + button_width + gap, 573, left + 2 * button_width + gap, 618),
        [this](float) { command("reset"); });
    button(L"导出会话 JSON", D2D1::RectF(right - button_width, 573, right, 618), [this](float) { command("export"); });
    label(L"导出的会话位于 %LOCALAPPDATA%\\GameGauge\\exports；包含指标来源和不可用原因。", left, 640,
        span, 23, small_.Get(), muted);
}
}
