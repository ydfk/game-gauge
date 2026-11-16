#include "window.h"
#include "saves_dialog.h"
#include "theme.h"
#include <format>

namespace gauge {
using namespace theme;
namespace {
std::wstring field(const Json& object, const char* key) { return wide(object.value(key, std::string{})); }
std::wstring utc_date(const Json& record) {
    auto date = field(record, "at");
    if (date.size() < 16) return date;
    date.resize(16);
    date[10] = L' ';
    return date + L" UTC";
}
const wchar_t* trigger(const Json& record) {
    const auto value = record.value("trigger", std::string{});
    if (value == "scheduled") return L"定时";
    if (value == "exit") return L"游戏退出";
    if (value == "before_restore") return L"恢复前保护";
    return L"手动";
}
bool confirmed(HWND owner, const wchar_t* message) {
    return MessageBoxW(owner, message, L"存档备份", MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) == IDOK;
}
}

void SettingsWindow::saves_records(float width, bool cloud) {
    constexpr float left = 272;
    const float right = width - 28;
    const float pane = right - std::max(314.f, (right - left) * .34f);
    const float top = cloud ? 393.f : 319.f;
    const bool busy = saves_request_.valid() || !saves_job_.empty();
    const auto& page = cloud ? saves_remote_rows_ : saves_rows_;
    const auto& record = cloud ? saves_remote_record_ : saves_record_;
    const int offset = page.value("offset", 0);
    const int total = page.value("total", 0);
    const auto id = record.value("id", std::string{});
    if (!cloud && !busy) {
        button(L"立即备份", D2D1::RectF(left, 260, left + 124, 304), [this](float) {
            saves_send(saves_target("saves.backup.enqueue"), [this](Json) { saves_list(); });
        }, true);
    }
    surface(D2D1::RectF(left, top, pane - 14, top + 330), true);
    label(cloud ? L"云端备份" : L"本地备份", left + 15, top + 5, 160, 29, heading_.Get(), white);
    label(std::format(L"{} 份", total), pane - 114, top + 5, 85, 29, mono_.Get(), muted);
    line(left + 15, top + 40, pane - 29, top + 40, ink_edge);
    auto rows = page.value("records", Json::array());
    if (rows.empty()) label(cloud && page.empty() ? L"点击「浏览远端」加载备份。" : L"这个档案还没有备份。",
        left + 15, top + 54, pane - left - 42, 34, body_.Get(), muted);
    float y = top + 45;
    for (const auto& row : rows) {
        const auto row_id = row.value("id", std::string{});
        const auto rect = D2D1::RectF(left + 12, y, pane - 26, y + 34);
        list_surface(rect, row_id == id || hovered(rect));
        const auto title = field(row, "title") + (row.value("locked", false) ? L"  ·  已保护" : L"");
        label(title, left + 20, y + 2, pane - left - 214, 30, body_.Get(), white);
        label(utc_date(row), pane - 180, y + 2, 150, 30, small_.Get(), muted);
        if (!busy) add(rect, [this, cloud, row, row_id](float) {
            if (cloud) { saves_remote_record_ = row; return; }
            auto request = saves_target("saves.records.detail");
            request["record_id"] = row_id;
            saves_send(std::move(request), [this](Json detail) { saves_record_ = std::move(detail.at("record")); });
        });
        y += 37;
    }
    const float nav = top + 278;
    if (!busy && offset > 0)
        button(L"上一页", D2D1::RectF(left + 15, nav, left + 111, nav + 36), [this, offset, cloud](float) { saves_list(offset - 6, cloud); });
    if (!busy && page.contains("next") && page["next"].is_number())
        button(L"下一页", D2D1::RectF(left + 121, nav, left + 217, nav + 36),
            [this, next = page["next"].get<int>(), cloud](float) { saves_list(next, cloud); });

    surface(D2D1::RectF(pane, top, right, top + 330));
    const float x = pane + 16;
    label(id.empty() ? L"选择一份备份" : field(record, "title"), x, top + 8, right - x - 14, 31, heading_.Get(), white);
    if (id.empty()) return;
    const auto details = std::format(L"{} · {:.2f} MiB{}", trigger(record), record.value("bytes", 0.0) / (1024 * 1024),
        record.value("locked", false) ? L" · 已保护" : L"");
    label(details, x, top + 44, right - x - 14, 24, small_.Get(), muted);
    label(L"档案 " + field(record, "profile"), x, top + 69, right - x - 14, 24, small_.Get(), muted);
    if (!cloud) label(field(record, "note"), x, top + 95, right - x - 14, 26, body_.Get(), white);
    if (busy) return;
    auto action = [this, id, offset, cloud](const char* command, Json fields = Json::object()) {
        auto request = saves_target(command);
        request["record_id"] = id;
        request.update(fields);
        saves_send(std::move(request), [this, offset, cloud](Json result) {
            if (cloud) { saves_list(offset, true); return; }
            if (field(result, "message").starts_with(L"恢复完成") && page_ == 5)
                MessageBoxW(window_, field(result, "message").c_str(), L"恢复完成", MB_OK | MB_ICONINFORMATION);
            saves_list(offset);
        });
    };
    if (cloud) {
        if (field(record, "profile") == field(saves_game_, "profile")) {
            button(L"校验并下载", D2D1::RectF(x, top + 109, right - 14, top + 153),
                [action](float) { action("saves.cloud.download.enqueue"); }, true);
        } else label(L"请先配置对应的本地 Steam 档案", x, top + 109, right - x - 14, 32, body_.Get(), amber);
        return;
    }
    const bool locked = record.value("locked", false);
    const float mid = (pane + right) / 2;
    const auto left_button = [&](float y) { return D2D1::RectF(x, y, mid - 5, y + 40); };
    const auto right_button = [&](float y) { return D2D1::RectF(mid + 5, y, right - 16, y + 40); };
    button(L"改名和备注", left_button(top + 130), [this, action](float) {
        auto fields = saves_form(window_, L"编辑备份", {
            {L"名称", field(saves_record_, "title"), 200},
            {L"备注", field(saves_record_, "note"), 4000, false, false, true},
        });
        if (fields) action("saves.records.edit", {{"title", utf8((*fields)[0])}, {"note", utf8((*fields)[1])}});
    });
    button(locked ? L"取消保护" : L"保护备份", right_button(top + 130),
        [action, locked](float) { action("saves.records.protect", {{"locked", !locked}}); });
    button(L"校验", left_button(top + 180), [action](float) { action("saves.records.verify"); });
    button(L"上传 WebDAV", right_button(top + 180), [action](float) { action("saves.cloud.upload.enqueue"); });
    button(L"恢复存档", left_button(top + 230), [this, action](float) {
        if (confirmed(window_, L"确认游戏已退出，且 Steam Cloud 已同步完毕？\n\n游戏仪表会先校验备份并保护当前存档，再替换存档目录。"))
            action("saves.restore.enqueue", {{"confirmed", true}});
    }, false, true);
    if (!locked) button(L"删除备份", right_button(top + 230), [this, action](float) {
        if (confirmed(window_, L"永久删除这份本地备份？云端副本会保留。"))
            action("saves.records.delete", {{"confirmed", true}});
    }, false, true);
}

void SettingsWindow::saves_cloud(float width) {
    constexpr float left = 272;
    const float right = width - 28;
    const auto cloud = saves_state_.value("cloud", Json::object());
    const bool busy = saves_request_.valid() || !saves_job_.empty();
    const bool automatic = cloud.value("automatic", false);
    const auto url = field(cloud, "url");
    const auto username = field(cloud, "user");
    surface(D2D1::RectF(left, 249, right, 379), true);
    const float controls_left = left + 16, controls_right = right - 16;
    const float step = std::min(150.f, (controls_right - controls_left + 10) / 5);
    const auto control = [&](int i) { return D2D1::RectF(controls_left + step * i, 292, controls_left + step * (i + 1) - 10, 331); };
    label(url.empty() ? L"尚未设置 WebDAV" : url, controls_left, 254, right - controls_left - 175, 29, body_.Get(), white);
    badge(automatic ? L"自动上传已启用" : L"自动上传已暂停",
        D2D1::RectF(right - 163, 257, controls_right, 282), automatic ? mint : muted);
    if (!busy) {
        button(L"编辑连接", control(0), [this, url, username, automatic](float) {
            auto fields = saves_form(window_, L"WebDAV 连接", {
                {L"已存在的 HTTPS 目录", url, 2048},
                {L"用户名", username, 512},
                {L"启用自动上传", automatic ? L"1" : L"0", 1, false, true},
            });
            if (fields) saves_send({{"command", "saves.cloud.configure"}, {"url", utf8((*fields)[0])},
                {"user", utf8((*fields)[1])}, {"automatic", (*fields)[2] == L"1"}}, [this](Json) {
                    saves_remote_rows_ = Json::object(); saves_remote_record_ = Json::object(); saves_refresh();
                });
        });
        if (!username.empty()) button(L"输入密码", control(1),
            [this, cloud, username](float) {
                if (save_webdav_password(window_, field(cloud, "credential_target"), username)) {
                    saves_message_ = L"密码已存入 Windows 凭据管理器";
                    saves_error_ = false;
                }
            });
        if (!url.empty()) {
            button(L"测试连接", control(2),
                [this](float) { saves_send({{"command", "saves.cloud.test.enqueue"}}); });
            button(L"重试上传", control(3),
                [this](float) { saves_send({{"command", "saves.cloud.retry.enqueue"}}, [this](Json) { saves_refresh(); }); });
            button(L"浏览远端", D2D1::RectF(controls_right - step + 10, 292, controls_right, 331),
                [this](float) { saves_list(0, true, true); }, true);
        }
    }
    label(field(cloud, "status"), controls_left, 340, controls_right - controls_left, 28, small_.Get(), muted);
    saves_records(width, true);
}
}
