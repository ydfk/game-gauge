#include "window.h"
#include "saves_dialog.h"
#include "theme.h"
#include "popup_menu.h"
#include "host/ipc_server.h"
#include <format>

namespace gauge {
using namespace theme;
namespace {
std::wstring field(const Json& object, const char* key) { return wide(object.value(key, std::string{})); }
std::wstring on(bool enabled) { return enabled ? L"1" : L"0"; }
int number(const std::wstring& text) {
    size_t used{};
    const auto parsed = std::stoi(text, &used);
    if (used != text.size() || parsed < 0) throw std::runtime_error("请输入有效数字");
    return parsed;
}
uint32_t app_id(const std::wstring& text) {
    size_t used{};
    const auto parsed = std::stoull(text, &used);
    if (used != text.size() || !parsed || parsed > UINT32_MAX) throw std::runtime_error("请输入有效的 Steam App ID");
    return static_cast<uint32_t>(parsed);
}
bool confirm(HWND owner, const wchar_t* message) {
    return MessageBoxW(owner, message, L"存档备份", MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) == IDOK;
}
}

void SettingsWindow::saves_open() {
    page_ = 5;
    focused_ = -1;
    error_.clear();
    saves_refresh();
    InvalidateRect(window_, nullptr, FALSE);
}

void SettingsWindow::saves_send(Json request, std::function<void(Json)> done) {
    if (fixture_mode_ || saves_request_.valid() || !saves_job_.empty()) return;
    if (!request.contains("request_id")) request["request_id"] = new_request_id();
    saves_done_ = std::move(done);
    saves_request_ = std::async(std::launch::async, [request = std::move(request)] { return ipc_request(request); });
    InvalidateRect(window_, nullptr, FALSE);
}

void SettingsWindow::saves_poll() {
    using namespace std::chrono_literals;
    const auto now = GetTickCount64();
    try {
        if (saves_request_.valid()) {
            if (saves_request_.wait_for(0ms) != std::future_status::ready) return;
            auto response = saves_request_.get();
            if (!response.value("ok", false)) throw std::runtime_error(response.value("error", std::string("存档任务失败")));
            if (response.contains("job_id")) {
                const auto state = response.value("state", std::string{});
                if (state.empty() || state == "queued" || state == "running") {
                    saves_job_ = response.at("job_id").get<std::string>();
                    saves_next_poll_ = now + 250;
                    return;
                }
                if (state != "succeeded") throw std::runtime_error(response.value("error", std::string("存档任务失败")));
                response = Json(response.value("result", Json::object()));
            }
            saves_job_.clear();
            if (response.contains("message")) {
                saves_message_ = field(response, "message");
                saves_error_ = false;
            }
            auto done = std::exchange(saves_done_, {});
            if (done) done(std::move(response));
            InvalidateRect(window_, nullptr, FALSE);
        } else if (!saves_job_.empty() && now >= saves_next_poll_) {
            Json query{{"command", "saves.job.status"}, {"job_id", saves_job_}};
            saves_request_ = std::async(std::launch::async, [query = std::move(query)] { return ipc_request(query); });
        }
    } catch (const std::exception& error) {
        saves_job_.clear();
        saves_done_ = {};
        saves_message_ = wide(error.what());
        saves_error_ = true;
        InvalidateRect(window_, nullptr, FALSE);
    }
}

void SettingsWindow::saves_refresh() {
    saves_send({{"command", "saves.host.status"}}, [this](Json host) {
        const bool was_available = saves_available_;
        saves_available_ = host.value("state", std::string{}) == "ready";
        saves_hotkey_conflict_ = host.value("hotkey_conflict", false);
        if (!saves_available_) {
            saves_message_ = field(host, "error");
            if (saves_message_.empty()) saves_message_ = L"正在启动存档工作进程，请刷新。";
            saves_error_ = true;
            return;
        }
        if (!was_available) { saves_message_.clear(); saves_error_ = false; }
        saves_send({{"command", "saves.status"}}, [this](Json state) {
            saves_state_ = std::move(state);
            saves_send({{"command", "saves.games.list"}}, [this](Json games) {
                saves_games_ = games.value("games", Json::array());
                saves_slots_ = games.value("slots", Json::array());
                for (const auto& row : saves_games_) if (row.value("selected", false)) saves_game_ = row;
                if (saves_tab_ == 1) saves_list();
            });
        });
    });
}

Json SettingsWindow::saves_target(const char* command) const {
    return {{"command", command}, {"game_id", saves_game_.value("game_id", std::string{})},
        {"profile", saves_game_.value("profile", std::string{})}};
}

void SettingsWindow::saves_list(int offset, bool remote, bool fresh) {
    auto request = saves_target(remote ? (fresh ? "saves.cloud.list.enqueue" : "saves.cloud.page") : "saves.records.list");
    request["offset"] = std::max(0, offset);
    request["limit"] = 6;
    saves_send(std::move(request), [this, remote](Json response) {
        (remote ? saves_remote_rows_ : saves_rows_) = std::move(response);
        (remote ? saves_remote_record_ : saves_record_) = Json::object();
    });
}

void SettingsWindow::saves_page(float width) {
    constexpr float left = 272;
    const float right = width - 28;
    const bool busy = saves_request_.valid() || !saves_job_.empty();
    auto action = [&](const wchar_t* title, D2D1_RECT_F rect, std::function<void(float)> click, bool primary = false) {
        if (busy) {
            badge(title, rect, primary ? lilac : muted);
        } else button(title, rect, std::move(click), primary);
    };
    const wchar_t* tabs[]{L"游戏与规则", L"备份库", L"云端"};
    const auto strip = D2D1::RectF(left, 141, left + 417, 185);
    fill(strip, tint(panel, .72f), control_radius);
    outline(strip, edge, control_radius);
    for (int index = 0; index < 3; ++index) {
        const float x = left + 4 + index * 137;
        tab(tabs[index], D2D1::RectF(x, 145, x + 134, 181), [this, index](float) {
            saves_tab_ = index; scroll_ = 0;
            if (index == 1) saves_list();
            if (index == 2) { saves_remote_rows_ = Json::object(); saves_remote_record_ = Json::object(); }
        }, index == saves_tab_, !busy);
    }
    action(L"刷新", D2D1::RectF(right - 96, 142, right, 184), [this](float) { saves_refresh(); });
    if (saves_available_ && !saves_game_.empty()) {
        const auto game_button = D2D1::RectF(left, 200, left + 258, 242);
        action(field(saves_game_, "name").append(L"  ▾").c_str(), game_button, [this, anchor = screen_rect(game_button)](float) {
            std::vector<MenuOption> names;
            for (const auto& row : saves_games_) names.push_back({field(row, "name"), row.value("selected", false)});
            const int index = show_options(window_, scale_, anchor, names);
            if (index < 0 || static_cast<size_t>(index) >= names.size()) return;
            saves_send({{"command", "saves.games.select"}, {"game_id", saves_games_[index]["game_id"]}},
                [this](Json) { saves_remote_rows_ = Json::object(); saves_remote_record_ = Json::object(); saves_refresh(); });
        });
        const auto profile = field(saves_game_, "profile");
        label(L"档案  " + (profile.empty() ? L"待配置" : profile), left + 278, 206, right - left - 278, 30, body_.Get(), muted);
        if (saves_tab_ == 0) saves_section(width);
        if (saves_tab_ == 1) saves_records(width, false);
        if (saves_tab_ == 2) saves_cloud(width);
    } else {
        surface(D2D1::RectF(left, 200, right, 343));
        icon(5, left + 18, 246, lilac);
        label(busy ? L"正在读取存档配置…" : L"存档备份暂不可用", left + 48, 235, right - left - 66, 43, heading_.Get(), white);
    }
    const auto feedback = busy ? L"正在处理存档任务…" : saves_message_;
    if (!feedback.empty()) {
        const float y = std::max(content_bottom_ + 14, 357.f);
        const auto ink = saves_error_ && !busy ? amber : blue;
        const auto rect = D2D1::RectF(left, y, right, y + 36);
        fill(rect, tint(ink, .08f), control_radius);
        outline(rect, tint(ink, .22f), control_radius);
        label(feedback, left + 13, y, right - left - 26, 36, body_.Get(), ink);
    }
}

void SettingsWindow::saves_configure(std::wstring slot) {
    const auto settings = saves_game_.at("settings");
    if (slot.empty()) slot = field(settings, "save_dir");
    auto fields = saves_form(window_, L"存档目录与自动备份规则", {
        {L"游戏存档目录", std::move(slot), 4096, true},
        {L"备份根目录（留空使用游戏仪表默认位置）", field(settings, "backup_dir"), 4096, true},
        {L"定时检查间隔（分钟）", std::to_wstring(settings.value("interval_minutes", 10)), 4},
        {L"自动备份保留份数", std::to_wstring(settings.value("keep_auto", 100)), 4},
        {L"备份快捷键（示例 Ctrl+Alt+F8，留空停用）", field(saves_state_, "hotkey"), 30},
        {L"启用存档自动备份", on(saves_state_.value("enabled", false)), 1, false, true},
        {L"此游戏启用定时与退出备份", on(settings.value("auto_enabled", true)), 1, false, true},
    });
    if (!fields) return;
    const auto& values = *fields;
    auto request = saves_target("saves.configure");
    request.update(Json{{"save_dir", utf8(values[0])}, {"backup_dir", utf8(values[1])},
        {"interval_minutes", number(values[2])}, {"keep_auto", number(values[3])},
        {"hotkey", utf8(values[4])}, {"enabled", values[5] == L"1"}, {"auto_enabled", values[6] == L"1"}});
    saves_send(std::move(request), [this](Json) { saves_refresh(); });
}

void SettingsWindow::saves_section(float width) {
    constexpr float left = 272;
    const float right = width - 28;
    const bool busy = saves_request_.valid() || !saves_job_.empty();
    const auto settings = saves_game_.value("settings", Json::object());
    const auto save = field(settings, "save_dir");
    const auto backup = field(settings, "backup_dir");
    surface(D2D1::RectF(left, 262, right, 550), true);
    label(L"游戏存档目录", left + 18, 272, 148, 30, body_.Get(), muted);
    label(save.empty() ? L"请选择专用存档目录" : save, left + 176, 272, right - left - 194, 30, body_.Get(), white);
    label(L"备份根目录", left + 18, 308, 148, 30, body_.Get(), muted);
    label(backup, left + 176, 308, right - left - 194, 30, body_.Get(), white);
    label(L"自动备份", left + 18, 344, 148, 30, body_.Get(), muted);
    const bool automatic = saves_state_.value("enabled", false) && settings.value("auto_enabled", true);
    label(automatic ? std::format(L"已启用 · 每 {} 分钟检查 · 保留 {} 份",
        settings.value("interval_minutes", 10), settings.value("keep_auto", 100)) : L"已暂停",
        left + 176, 344, right - left - 194, 30, body_.Get(), white);
    label(L"备份快捷键", left + 18, 380, 148, 30, body_.Get(), muted);
    const auto key = field(saves_state_, "hotkey");
    label((key.empty() ? L"已停用" : key) + (saves_hotkey_conflict_ ? L" · 已占用，请修改" : L""),
        left + 176, 380, right - left - 194, 30, body_.Get(), saves_hotkey_conflict_ ? amber : white);
    label(field(saves_state_, "automatic_status"), left + 18, 418, right - left - 36, 30, small_.Get(), muted);
    for (float y : {304.f, 340.f, 376.f, 412.f, 466.f}) line(left + 16, y, right - 16, y, ink_edge);
    if (busy) return;
    button(L"编辑备份规则", D2D1::RectF(left + 17, 486, left + 173, 530), [this](float) { saves_configure(); }, true);
    const auto slots_button = D2D1::RectF(left + 185, 486, left + 353, 530);
    if (saves_game_.value("game_id", std::string{}) == "steam-4115450" && !saves_slots_.empty()) {
        button(L"选择 Steam 账号", slots_button, [this, anchor = screen_rect(slots_button)](float) {
            std::vector<MenuOption> slots;
            for (const auto& path : saves_slots_) slots.push_back({wide(path.get<std::string>())});
            const int index = show_options(window_, scale_, anchor, slots);
            if (index >= 0 && static_cast<size_t>(index) < slots.size()) saves_configure(slots[index].title);
        });
    }
    button(L"添加 Steam 游戏", D2D1::RectF(left + 365, 486, left + 529, 530), [this](float) {
        auto fields = saves_form(window_, L"添加已安装的 Steam 游戏", {
            {L"Steam App ID", L"", 10},
            {L"游戏进程文件名（含 .exe）", L"", 120},
            {L"游戏专用存档目录", L"", 4096, true},
        });
        if (fields) saves_send({{"command", "saves.games.add"}, {"app_id", app_id((*fields)[0])},
            {"executable", utf8((*fields)[1])}, {"save_dir", utf8((*fields)[2])}}, [this](Json) { saves_refresh(); });
    });
    surface(D2D1::RectF(left, 565, right, 657));
    label(L"从留档 SaveKeep 迁移", left + 17, 576, right - left - 175, 30, heading_.Get(), white);
    label(L"预览原库和冲突，再复制可导入的备份。", left + 17, 610, right - left - 175, 28, body_.Get(), muted);
    button(L"预览旧库", D2D1::RectF(right - 147, 589, right - 17, 633), [this](float) {
        saves_send({{"command", "saves.migration.preview"}}, [this](Json result) {
            auto summary = std::format(L"发现 {} 份备份，{} 个档案，预计复制 {:.2f} MiB。\n跳过 {} 份重复备份，{} 处冲突，{} 处不可读。\n\n",
                result.value("records", 0), result.value("profiles", 0), result.value("bytes_to_copy", 0.0) / (1024 * 1024),
                result.value("duplicates", 0), result.value("conflicts", 0), result.value("unreadable", 0));
            summary += std::format(L"原库记录共 {:.2f} MiB，目标中同 ID 备份占 {:.2f} MiB。\n\n",
                result.value("total_bytes", 0.0) / (1024 * 1024),
                result.value("existing_target_bytes", 0.0) / (1024 * 1024));
            for (const auto& path : result.value("sources", Json::array())) summary += wide(path.get<std::string>()) + L"\n";
            if (result.value("webdav", false)) summary += L"\nWebDAV 地址和用户名可迁移。自动上传初始暂停。\n";
            summary += L"\n请退出旧版留档后再复制。继续吗？";
            if (!confirm(window_, summary.c_str())) return;
            saves_send({{"command", "saves.migration.apply"}, {"token", result.at("token")}}, [this](Json applied) {
                auto message = std::format(L"迁移复制 {} 份，失败 {} 份。", applied.value("copied", 0), applied.value("failed", 0));
                for (const auto& failure : applied.value("failures", Json::array()))
                    message += L"\n" + field(failure, "id") + L"  " + field(failure, "error");
                MessageBoxW(window_, message.c_str(), L"迁移结果", MB_OK | MB_ICONINFORMATION);
                saves_error_ = applied.value("failed", 0) != 0;
                saves_message_ = std::format(L"迁移复制 {} 份，失败 {} 份", applied.value("copied", 0), applied.value("failed", 0));
                saves_refresh();
            });
        });
    });
}
}
