#include "worker.h"
#include <algorithm>

namespace gauge::saves {
using namespace std::chrono_literals;
namespace {
uint64_t number_or(const Json& request, const char* key, uint64_t fallback) {
    return request.contains(key) && request[key].is_number_unsigned() ? request[key].get<uint64_t>() : fallback;
}
std::string record_id(const Json& input) {
    const auto id = required_string(input, "record_id");
    require(uuid(id), "备份 UUID 无效。");
    return id;
}
std::string_view trigger_name(std::string_view trigger) {
    return trigger == "scheduled" ? "定时" : trigger == "exit" ? "退出游戏" : "手动";
}
}
std::string_view Schedule::observe(bool running, bool enabled, std::chrono::seconds period, Clock::time_point now) {
    const bool previous = std::exchange(was_running, running);
    if (!enabled) { due.reset(); return {}; }
    if (previous != running || (running && !due)) {
        due = now + (running ? period : 6s);
        return {};
    }
    if (!due || *due > now) return {};
    if (running) { due = now + period; return "scheduled"; }
    due.reset();
    return "exit";
}
Json record_page(const std::vector<Json>& records, const Json& request) {
    const auto offset = static_cast<size_t>(std::min<uint64_t>(records.size(), number_or(request, "offset", 0)));
    const auto limit = static_cast<size_t>(std::clamp<uint64_t>(number_or(request, "limit", 10), 1, 16));
    const auto end = std::min(records.size(), offset + limit);
    auto rows = Json::array();
    for (auto i = offset; i < end; ++i) {
        Json row = Json::object();
        for (const auto key : {"id", "at", "title", "locked", "profile", "trigger", "bytes"}) row[key] = records[i].at(key);
        rows.push_back(std::move(row));
    }
    return {{"records", std::move(rows)}, {"total", records.size()}, {"offset", offset}, {"next", end < records.size() ? Json(end) : Json(nullptr)}};
}

Worker::Worker(fs::path root) : root_(std::move(root)), catalog_(Catalog::load(root_)), cloud_(root_) {}
const std::string& Worker::hotkey() const { return catalog_.games.front().hotkey; }
size_t Worker::target_index(const Json& input) const {
    const auto game = required_string(input, "game_id");
    for (size_t i = 0; i < catalog_.games.size(); ++i) if (catalog_.games[i].identity().first == game) return i;
    throw std::runtime_error("游戏未登记。");
}
const Settings& Worker::target(const Json& input) const {
    const auto& settings = catalog_.games[target_index(input)];
    require(settings.identity().second == required_string(input, "profile"), "档案已经变化，请刷新再操作。");
    return settings;
}
Json Worker::change_catalog(const std::function<void(Catalog&)>& change) {
    auto next = catalog_;
    change(next);
    next.save(root_);
    catalog_ = std::move(next);
    preview_.reset();
    return {{"message", "设置已保存"}};
}
Json Worker::configure(const Json& input) {
    const auto index = target_index(input);
    auto settings = target(input);
    settings.save_dir = path_of(required_string(input, "save_dir"));
    settings.backup_dir = path_of(required_string(input, "backup_dir"));
    if (settings.backup_dir.empty()) settings.backup_dir = root_ / L"Backups";
    settings.interval_minutes = required_number(input, "interval_minutes");
    settings.keep_auto = required_number(input, "keep_auto");
    settings.auto_enabled = flag(input, "auto_enabled");
    settings.hotkey = required_string(input, "hotkey");
    require(settings.interval_minutes >= 1 && settings.interval_minutes <= 1440, "检查间隔须为 1–1440 分钟。");
    require(settings.keep_auto >= 1 && settings.keep_auto <= 9999, "保留数量须为 1–9999 份。");
    require(settings.hotkey.size() <= 30, "快捷键过长。");
    if (!settings.save_dir.empty()) {
        settings.save_dir = resolve_save(settings);
        settings.backup_dir = backup_location(settings.save_dir, settings.backup_dir);
        catalog_.check_overlap(settings.save_dir, settings.backup_dir, index);
    }
    auto result = change_catalog([&](Catalog& next) {
        next.games[index] = settings;
        next.selected = index;
        next.enabled = flag(input, "enabled");
        for (auto& game : next.games) game.hotkey = settings.hotkey;
    });
    schedules_.clear();
    result["hotkey"] = hotkey();
    return result;
}
Json Worker::games() const {
    auto games = Json::array(), slots = Json::array();
    for (size_t i = 0; i < catalog_.games.size(); ++i) {
        auto settings = catalog_.games[i];
        settings.hotkey = hotkey();
        settings.dark = catalog_.games.front().dark;
        const auto [game, profile] = settings.identity();
        games.push_back({{"name", settings.title()}, {"game_id", game}, {"profile", profile}, {"selected", i == catalog_.selected},
            {"executable", settings.custom ? Json(settings.custom->executable) : Json(nullptr)}, {"settings", settings.json()}});
    }
    for (const auto& slot : discover_slots()) slots.push_back(path_text(slot));
    return {{"games", std::move(games)}, {"slots", std::move(slots)}};
}
Json Worker::backup(const Settings& settings, std::string_view trigger) {
    const Library library(settings);
    const auto record = library.backup(trigger);
    if (!record) return {{"message", "存档内容没有变化"}};
    auto status = std::string(trigger_name(trigger)) + "备份已保存并校验";
    try {
        uint64_t kept{};
        for (const auto& row : library.records()) {
            if (flag(row, "locked") || (row.at("trigger") != "scheduled" && row.at("trigger") != "exit")) continue;
            if (kept++ >= settings.keep_auto && cloud_.may_prune(library, row)) delete_record(library.root, row);
        }
    } catch (const std::exception& error) { status = "备份成功，自动清理失败：" + failure_message(error); }
    return {{"message", status}, {"record_id", record->at("id")}};
}
Json Worker::backup_request(const Json& input) {
    if (!input.value("shortcut", false)) return backup(target(input), "manual");
    const auto names = processes();
    const Settings* selected{};
    for (const auto& game : catalog_.games) {
        if (game.save_dir.empty() || !running(game, names)) continue;
        if (selected) return {{"choose_target", true}, {"message", "多款游戏正在运行，请选择档案后备份。"}};
        selected = &game;
    }
    return backup(selected ? *selected : catalog_.games[catalog_.selected], "manual");
}
Json Worker::record_action(std::string_view command, const Json& input) {
    const Library library(target(input));
    const auto id = record_id(input);
    const auto directory = library.root / path_of(id);
    auto record = read_record(directory, library.game, library.profile, id);
    if (command == "saves.records.detail") return {{"record", record}};
    if (command == "saves.records.verify") {
        verify_record(directory / archive_name, record);
        return {{"message", "备份校验通过"}};
    }
    if (command == "saves.records.edit") {
        const auto title = trim(required_string(input, "title")), note = required_string(input, "note");
        require(!title.empty() && text_length(title) <= 100 && text_length(note) <= 2000, "名称须为 1–100 个字符，备注最多 2000 个字符。");
        record["title"] = title;
        record["note"] = note;
        write_json(directory / L"record.json", record);
        return {{"message", "已保存名称与备注"}};
    }
    if (command == "saves.records.protect") {
        record["locked"] = flag(input, "locked");
        write_json(directory / L"record.json", record);
        return {{"message", "已更新保护状态"}};
    }
    if (command == "saves.records.delete") {
        require(flag(input, "confirmed"), "需要确认删除。");
        delete_record(library.root, record);
        return {{"message", "已删除本地备份"}};
    }
    if (command == "saves.restore.enqueue") {
        require(flag(input, "confirmed"), "请关闭游戏并确认恢复。");
        return {{"message", library.restore(record)}};
    }
    if (command == "saves.cloud.upload.enqueue") {
        cloud_.upload(library, record);
        return {{"message", "WebDAV 上传完成"}};
    }
    throw std::runtime_error("未知备份记录命令。");
}
Json Worker::cloud_action(std::string_view command, const Json& input) {
    if (command == "saves.cloud.configure") {
        cloud_.configure(DavConfig::make(required_string(input, "url"), required_string(input, "user"), flag(input, "automatic")));
        remote_.clear();
        next_cloud_ = Clock::now();
        return {{"message", "WebDAV 设置已保存"}, {"credential_target", cloud_.config.credential_target()}};
    }
    if (command == "saves.cloud.test.enqueue") {
        Remote(cloud_.config).test();
        return {{"message", "WebDAV 连接成功"}};
    }
    if (command == "saves.cloud.retry.enqueue") {
        cloud_.retry();
        next_cloud_ = Clock::now();
        return {{"message", "已重新排队"}};
    }
    const auto& settings = target(input);
    const auto [game, profile] = settings.identity();
    if (command == "saves.cloud.list.enqueue") {
        remote_ = Remote(cloud_.config).list(game, profile);
        return record_page(remote_, input);
    }
    if (command == "saves.cloud.page") {
        std::vector<Json> rows;
        for (const auto& record : remote_) {
            if (record.at("game_id") == game && (profile.empty() || record.at("profile") == profile)) rows.push_back(record);
        }
        return record_page(rows, input);
    }
    if (command == "saves.cloud.download.enqueue") {
        const Library library(settings);
        const auto id = record_id(input);
        const auto entry = std::find_if(remote_.begin(), remote_.end(), [&](const Json& row) {
            return row.at("game_id") == game && row.at("profile") == profile && row.at("id") == id;
        });
        require(entry != remote_.end(), "请刷新云端列表并选择当前档案中的备份。");
        Remote(cloud_.config).download(library, *entry);
        cloud_.record_receipt(library.root, *entry);
        return {{"message", "云端备份已校验并导入本地库"}};
    }
    throw std::runtime_error("未知 WebDAV 命令。");
}
Json Worker::migration_action(std::string_view command, const Json& input) {
    if (command == "saves.migration.preview") {
        const auto directory = environment(L"LOCALAPPDATA");
        require(!directory.empty(), "无法获取留档数据目录。");
        preview_ = Migration::inspect(fs::path(directory) / L"SaveKeep", root_, catalog_);
        return preview_->summary();
    }
    require(command == "saves.migration.apply", "未知迁移命令。");
    require(!processes().contains("savekeep.exe"), "请退出留档 SaveKeep，再复制旧库。");
    require(preview_ && preview_->summary().at("token") == required_string(input, "token"), "请先预览本次迁移。");
    auto current = std::exchange(preview_, std::nullopt);
    auto result = current->apply(root_, catalog_);
    cloud_ = Cloud(root_);
    return result;
}
Json Worker::request(const Json& input) {
    const auto command = required_string(input, "command");
    if (command == "saves.status") {
        auto cloud = cloud_.config.json();
        cloud["credential_target"] = cloud_.config.credential_target();
        cloud["status"] = cloud_status_;
        return {{"enabled", catalog_.enabled}, {"hotkey", hotkey()}, {"automatic_status", automatic_status_}, {"cloud", std::move(cloud)}};
    }
    if (command == "saves.games.list") return games();
    if (command == "saves.games.select") {
        const auto index = target_index(input);
        return change_catalog([&](Catalog& catalog) { catalog.selected = index; });
    }
    if (command == "saves.games.add") {
        const auto id = required_number(input, "app_id");
        require(id <= UINT32_MAX, "Steam App ID 超出范围。");
        return change_catalog([&](Catalog& catalog) {
            catalog.add(static_cast<uint32_t>(id), required_string(input, "executable"), path_of(required_string(input, "save_dir")));
        });
    }
    if (command == "saves.configure") return configure(input);
    if (command == "saves.backup.enqueue") return backup_request(input);
    if (command == "saves.records.list") {
        const auto& settings = target(input);
        const auto [game, profile] = settings.identity();
        auto rows = profile.empty() || settings.save_dir.empty() ? std::vector<Json>{} :
            records(profile_path(settings.backup_dir, game, profile), game, profile);
        return record_page(rows, input);
    }
    if (command.starts_with("saves.migration.")) return migration_action(command, input);
    if (command.starts_with("saves.records.") || command == "saves.restore.enqueue" || command == "saves.cloud.upload.enqueue")
        return record_action(command, input);
    if (command.starts_with("saves.cloud.")) return cloud_action(command, input);
    throw std::runtime_error("未知存档命令。");
}
void Worker::tick() {
    if (catalog_.enabled) {
        try {
            const auto names = processes();
            const auto now = Clock::now();
            for (const auto& game : catalog_.games) {
                if (game.save_dir.empty()) continue;
                const auto period = std::chrono::minutes(std::clamp<uint64_t>(game.interval_minutes, 1, 1440));
                const auto trigger = schedules_[game.identity()].observe(running(game, names), game.auto_enabled, period, now);
                if (trigger.empty()) continue;
                try { automatic_status_ = required_string(backup(game, trigger), "message"); }
                catch (const std::exception& error) { automatic_status_ = failure_message(error); }
            }
        } catch (const std::exception& error) { automatic_status_ = failure_message(error); }
    }
    if (cloud_.config.automatic && Clock::now() >= next_cloud_) {
        try { cloud_status_ = cloud_.upload_next(catalog_.games); }
        catch (const std::exception& error) { cloud_status_ = failure_message(error); }
        next_cloud_ = Clock::now() + 10s;
    }
}
}
