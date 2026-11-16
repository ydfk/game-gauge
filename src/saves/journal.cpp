#include "worker.h"
#include <set>

namespace gauge::saves {
namespace {
const std::set<std::string, std::less<>> mutations{
    "saves.games.select", "saves.games.add", "saves.configure", "saves.backup.enqueue",
    "saves.records.edit", "saves.records.protect", "saves.records.delete", "saves.restore.enqueue",
    "saves.migration.apply", "saves.cloud.configure", "saves.cloud.download.enqueue", "saves.cloud.upload.enqueue"
};
std::string checked_id(const Json& request, const char* key) {
    const auto id = required_string(request, key);
    require(uuid(id), "存档任务需要 UUID 标识。");
    return id;
}
}
Journal::Journal(const fs::path& root) : directory_(root / L"jobs") {
    fs::create_directory(directory_);
    plain_directory(directory_);
}
Json Journal::lookup(const Json& request) const {
    const auto id = checked_id(request, "job_id");
    const auto path = directory_ / (wide(id) + L".json");
    if (!saves::exists(path)) return {{"ok", false}, {"error", "找不到此存档任务"}};
    const auto stored = read_json(path);
    const auto& outcome = stored.at("outcome");
    if (outcome.is_null()) return {{"ok", true}, {"job_id", id}, {"state", "interrupted"},
        {"error", "任务在完成前被中断，请检查存档目录与备份库。"}};
    if (outcome.contains("Completed"))
        return {{"ok", true}, {"job_id", id}, {"state", "succeeded"}, {"result", outcome.at("Completed")}};
    return {{"ok", true}, {"job_id", id}, {"state", "failed"}, {"error", outcome.at("Failed")}};
}
Json Journal::execute(const Json& request, const std::function<Json()>& action) const {
    if (!mutations.contains(required_string(request, "command"))) return action();
    const auto id = checked_id(request, "request_id");
    const auto path = directory_ / (wide(id) + L".json");
    auto input = request;
    input.erase("id");
    const auto digest = sha256(input.dump());
    if (saves::exists(path)) {
        const auto stored = read_json(path);
        require(stored.at("digest") == digest, "请求标识已用于另一个存档任务。");
        const auto& outcome = stored.at("outcome");
        require(!outcome.is_null(), "上次任务被中断。请先检查存档目录与备份库，再决定是否发起新任务。");
        if (outcome.contains("Completed")) return outcome.at("Completed");
        throw std::runtime_error(required_string(outcome, "Failed"));
    }
    Json entry{{"digest", digest}, {"outcome", nullptr}};
    write_json(path, entry);
    Json output;
    try { output = action(); }
    catch (const std::exception& error) {
        entry["outcome"] = {{"Failed", failure_message(error)}};
        write_json(path, entry);
        throw;
    }
    entry["outcome"] = {{"Completed", output}};
    try { write_json(path, entry); }
    catch (const std::exception&) { throw std::runtime_error("任务已执行，保存结果失败，请检查目录和备份库。"); }
    return output;
}
}
