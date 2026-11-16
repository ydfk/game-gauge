#include "saves/worker.h"
#include <fstream>
#include <iostream>

using namespace gauge;
using namespace gauge::saves;
using namespace std::chrono_literals;

namespace {
constexpr auto account = "76561198000000001";
void file(const fs::path& path, std::string_view bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(output), "写入测试文件失败。");
}
void rejected(const std::function<void()>& action) {
    try { action(); }
    catch (const std::exception&) { return; }
    throw std::runtime_error("无效测试操作被接受。");
}
Settings fixture(const fs::path& directory, const fs::path& backups) {
    Settings settings;
    settings.save_dir = directory / path_of(account);
    settings.backup_dir = backups;
    fs::create_directories(settings.save_dir / L"1RealTime" / L"中文");
    file(settings.save_dir / main_save, "first-save");
    file(settings.save_dir / L"1RealTime" / L"中文" / L"轮转.sav", "rotated-1");
    file(settings.save_dir / L"steam_autocloud.vdf", "steam marker");
    return settings;
}
void archive_contracts(const fs::path& directory) {
    for (const auto name : {"../save.sav", "1RealTime//x.sav", "1RealTime/COM1.sav", "1RealTime/NUL.sav",
        "1RealTime/LPT².sav", "1RealTime/a.sav:stream", "1RealTime\\a.sav", "/x.sav", "1RealTime/x.sav "})
        require(!managed_name(name, false), "不安全的 ZIP 路径被接受。");
    require(managed_name("1RealTime/中文/轮转.sav", false) && !managed_name("other/轮转.sav", false), "影之刃零文件规则错误。");
    auto settings = fixture(directory / L"game", directory / L"Backups");
    const Library library(settings);
    const auto first = library.backup("manual");
    require(first.has_value(), "手动快照失败。");
    const auto zip = library.root / path_of(required_string(*first, "id")) / archive_name;
    verify_record(zip, *first);
    require(!library.backup("scheduled"), "内容未变仍产生自动快照。");
    file(library.save / main_save, "second-save");
    file(library.save / L"steam_autocloud.vdf", "new marker");
    const auto original = library.restore(*first);
    require(original.find(".savekeep-preserved-") != std::string::npos &&
        read_text(library.save / main_save) == "first-save" &&
        read_text(library.save / L"steam_autocloud.vdf") == "new marker", "恢复或 Steam 同步标记处理错误。");
    const auto rows = library.records();
    require(rows.size() == 2 && rows.front().at("locked") == true && rows.front().at("trigger") == "before_restore",
        "恢复前保护备份未保留。");
    rejected([&] { delete_record(library.root, rows.front()); });
    const auto rollback = rollback_path(library.save);
    rejected([&] { commit_restore(library.save, directory / L"不存在", rollback); });
    require(!saves::exists(rollback) && read_text(library.save / main_save) == "first-save", "目录替换失败后未回滚。");
    file(zip, "bad");
    rejected([&] { library.restore(*first); });
    require(read_text(library.save / main_save) == "first-save", "损坏的 ZIP 改变了当前存档。");

    Settings custom;
    custom.save_dir = directory / L"custom" / L"nested-game";
    custom.backup_dir = settings.backup_dir;
    custom.custom = SteamGame{42, "第二款游戏", "gamegauge-saves-fixture-42.exe"};
    fs::create_directories(custom.save_dir / L"测试");
    file(custom.save_dir / L"测试" / L"中文档案.bin", "custom");
    file(custom.save_dir / L"steam_autocloud.vdf", "custom cloud");
    const Library custom_library(custom);
    const auto record = custom_library.backup("manual");
    require(record && record->at("game_id") == "steam-42" && record->at("profile") == "default", "自定义游戏身份错误。");
    file(custom_library.save / L"测试" / L"中文档案.bin", "changed");
    custom_library.restore(*record);
    require(read_text(custom_library.save / L"测试" / L"中文档案.bin") == "custom" &&
        read_text(custom_library.save / L"steam_autocloud.vdf") == "custom cloud", "自定义游戏恢复失败。");
}
void migration_contracts(const fs::path& directory) {
    auto root = directory / L"新目录", legacy = directory / L"旧目录";
    fs::create_directory(root);
    fs::create_directory(legacy);
    auto old = Catalog::load(legacy);
    old.games.front() = fixture(directory / L"steam", directory / L"外部备份库");
    old.save(legacy);
    write_json(legacy / L"webdav.json", {{"url", "https://example.invalid/dav/"}, {"user", "test"}, {"secret", {1, 2}}, {"automatic", true}});
    const Library source(old.games.front());
    auto record = *source.backup("manual");
    record["note"] = "中文备注";
    record["locked"] = true;
    const auto id = required_string(record, "id");
    const auto path = source.root / path_of(id);
    write_json(path / L"record.json", record);
    const auto before = read_text(path / L"record.json");
    auto target = Catalog::load(root);
    const auto preview = Migration::inspect(legacy, root, target);
    require(preview.summary().at("records") == 1 && !saves::exists(root / L"Backups"), "预览改动了目标目录。");
    const auto applied = preview.apply(root, target);
    if (applied.at("copied") != 1) throw std::runtime_error("迁移失败：" + applied.at("failures").dump());
    require(read_text(path / L"record.json") == before, "原清单发生修改。");
    const Library output(target.games.front());
    verify_record(output.root / path_of(id) / archive_name, record);
    const auto cloud = read_json(root / L"webdav.json");
    require(cloud.at("automatic") == false && !cloud.contains("secret"), "迁移了旧密码或自动上传状态。");
    require(Migration::inspect(legacy, root, target).summary().at("duplicates") == 1, "重复迁移未去重。");
    record["sha256"] = std::string(64, '0');
    write_json(path / L"record.json", record);
    const auto conflict = Migration::inspect(legacy, root, target);
    require(conflict.summary().at("conflicts") == 1 && conflict.apply(root, target).at("copied") == 0,
        "迁移未拒绝同 UUID 异哈希存档。");
}
void journal_contracts(const fs::path& directory) {
    const Journal journal(directory);
    const auto id = new_request_id();
    const auto request = Json{{"command", "saves.restore.enqueue"}, {"request_id", id}, {"id", 5}, {"version", 1}};
    const auto result = journal.execute(request, [] { return Json{{"message", "ok"}}; });
    require(result.at("message") == "ok", "任务结果错误。");
    const Journal reopened(directory);
    auto replay = request;
    replay["id"] = 6;
    require(reopened.execute(replay, []() -> Json { throw std::runtime_error("不能重复执行"); }) == result,
        "重连时任务结果不可重用。");
    require(reopened.lookup({{"job_id", id}}).at("state") == "succeeded", "任务查找失败。");
    const auto path = directory / L"jobs" / (wide(id) + L".json");
    auto entry = read_json(path);
    entry["outcome"] = nullptr;
    write_json(path, entry);
    rejected([&] { reopened.execute(replay, [] { return Json{}; }); });
    require(reopened.lookup({{"job_id", id}}).at("state") == "interrupted", "中断任务状态错误。");
}
void schedule_contracts() {
    Settings custom;
    custom.custom = SteamGame{42, "测试", "äpc.exe"};
    require(running(custom, {"Äpc.exe"}), "Windows 非 ASCII 进程名大小写匹配失败。");
    const auto now = Clock::now();
    Schedule schedule;
    require(schedule.observe(false, true, 10min, now).empty(), "空闲时开始调度。");
    require(schedule.observe(true, true, 10min, now + 1s).empty(), "启动时提前触发备份。");
    require(schedule.observe(true, true, 10min, now + 11min) == "scheduled", "未触发定时备份。");
    require(schedule.observe(false, true, 10min, now + 12min).empty(), "退出等待期丢失。");
    require(schedule.observe(false, true, 10min, now + 12min + 7s) == "exit", "未触发退出备份。");
    require(schedule.observe(false, true, 10min, now + 12min + 8s).empty(), "重复触发退出备份。");
    std::vector<Json> rows(1500, {{"id", new_request_id()}, {"at", timestamp()}, {"title", "测试"}, {"note", "大备注"},
        {"locked", false}, {"profile", account}, {"trigger", "manual"}, {"bytes", 512u}});
    const auto page = record_page(rows, {{"offset", 1498u}, {"limit", 6u}});
    require(page.at("records").size() == 2 && page.at("next").is_null() && !page.at("records")[0].contains("note"), "分页响应错误。");
}
}
int main() {
    try {
        const TemporaryDirectory folder(fs::temp_directory_path(), L"GameGauge-saves-");
        for (const auto name : {L"archive", L"migration", L"journal"}) fs::create_directory(folder.path() / name);
        archive_contracts(folder.path() / L"archive");
        migration_contracts(folder.path() / L"migration");
        journal_contracts(folder.path() / L"journal");
        schedule_contracts();
        std::cout << "ZIP, rollback, migration, tasks, schedule and pagination passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
