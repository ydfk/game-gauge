#include "dav.h"
#include <algorithm>
#include <numeric>

namespace gauge::saves {
namespace {
fs::path receipt_path(const fs::path& root, const Json& record) {
    return root / path_of(required_string(record, "id")) / L"webdav-receipt.json";
}
}
Cloud::Cloud(const fs::path& directory) : path_(directory / L"webdav.json") {
    if (saves::exists(path_)) config = DavConfig::parse(read_json(path_));
}
void Cloud::configure(DavConfig next) {
    write_json(path_, next.json());
    config = std::move(next);
    retry();
}
void Cloud::retry() { retries_.clear(); }
std::pair<bool, bool> Cloud::receipt(const fs::path& root, const Json& record) const {
    try {
        const auto stored = read_json(receipt_path(root, record));
        const bool archive = stored.at("destination") == config.destination() && stored.at("sha256") == record.at("sha256");
        return {archive, archive && stored.at("metadata") == record_digest(record)};
    } catch (const std::exception&) { return {false, false}; }
}
bool Cloud::may_prune(const Library& library, const Json& record) const {
    return config.url.empty() || receipt(library.root, record).second;
}
void Cloud::record_receipt(const fs::path& root, const Json& record) const {
    require(!config.url.empty(), "WebDAV 尚未配置。");
    write_json(receipt_path(root, record), {{"destination", config.destination()},
        {"sha256", record.at("sha256")}, {"metadata", record_digest(record)}});
}
void Cloud::upload_at(const fs::path& root, const Json& record) const {
    Remote(config).upload(record, root / path_of(required_string(record, "id")) / archive_name, receipt(root, record).first);
    record_receipt(root, record);
}
void Cloud::upload(const Library& library, const Json& record) const { upload_at(library.root, record); }
std::string Cloud::upload_next(const std::vector<Settings>& targets) {
    if (config.url.empty()) return "未配置 WebDAV";
    if (!config.automatic) return "已暂停自动上传";
    struct Pending { fs::path root; Json record; };
    std::map<std::string, Pending> pending;
    size_t uploaded{}, unavailable{};
    for (const auto& target : targets) {
        if (target.save_dir.empty()) continue;
        try {
            const Library library(target);
            for (const auto& record : library.records()) {
                if (receipt(library.root, record).second) ++uploaded;
                else pending.emplace(required_string(record, "id"), Pending{library.root, record});
            }
        } catch (const std::exception&) { ++unavailable; }
    }
    std::erase_if(retries_, [&](const auto& retry) { return !pending.contains(retry.first); });
    std::vector<Json> order;
    for (const auto& [id, item] : pending) order.push_back(item.record);
    sort_records(order);
    size_t waiting = pending.size();
    const auto now = std::chrono::steady_clock::now();
    for (auto item = order.rbegin(); item != order.rend(); ++item) {
        const auto id = required_string(*item, "id");
        const auto previous = retries_.find(id);
        if (previous != retries_.end() && previous->second.next > now) continue;
        try {
            upload_at(pending.at(id).root, *item);
            retries_.erase(id);
            ++uploaded;
            --waiting;
        } catch (const std::exception& error) {
            auto& retry = retries_[id];
            retry.attempts = std::min(retry.attempts + 1, 6u);
            retry.next = std::chrono::steady_clock::now() + std::chrono::seconds(std::min(5u << retry.attempts, 300u));
            retry.message = failure_message(error);
        }
        break;
    }
    auto status = "本地库已上传 " + std::to_string(uploaded) + " 份 · 待上传 " + std::to_string(waiting) + " 份";
    if (!retries_.empty()) return status + " · 将自动重试：" + retries_.begin()->second.message;
    if (unavailable) status += " · " + std::to_string(unavailable) + " 个备份库暂不可用";
    return status;
}
}
