#include "migration.h"
#include <algorithm>

namespace gauge::saves {
namespace {
fs::path backup_root(const fs::path& root, const Catalog& catalog, std::string_view game) {
    for (const auto& settings : catalog.games) {
        if (settings.identity().first == game && settings.backup_dir.is_absolute()) return settings.backup_dir;
    }
    return root / L"Backups";
}
fs::path known_path(const fs::path& path) {
    require(path.is_absolute(), "迁移目标必须是绝对路径。");
    auto ancestor = path;
    while (!saves::exists(ancestor)) {
        require(ancestor != ancestor.parent_path(), "找不到目标磁盘。");
        ancestor = ancestor.parent_path();
    }
    plain_directory(ancestor);
    auto resolved = extended_path(fs::canonical(extended_path(ancestor)));
    if (ancestor != path) {
        for (const auto& part : path.lexically_relative(ancestor)) {
            require(part != L".." && part != L"." && !part.empty(), "迁移目标路径无效。");
            resolved /= part;
        }
    }
    return resolved;
}
bool plain_child(const fs::directory_entry& item) {
    const auto attr = attributes(item.path());
    return (attr & FILE_ATTRIBUTE_DIRECTORY) && !(attr & FILE_ATTRIBUTE_REPARSE_POINT);
}
}
Migration Migration::inspect(const fs::path& legacy, const fs::path& root, const Catalog& current) {
    plain_directory(legacy);
    Migration result;
    result.catalog_ = Catalog::load(legacy);
    const auto default_root = legacy / L"Backups";
    std::set<fs::path> roots{default_root};
    for (const auto& game : result.catalog_.games) if (game.backup_dir.is_absolute()) roots.insert(game.backup_dir);
    std::map<fs::path, std::string> seen;
    std::set<fs::path> blocked;
    std::set<std::pair<std::string, std::string>> profiles;
    Json locations = Json::array(), conflict_ids = Json::array();
    size_t errors{}, conflicts{}, duplicates{};
    uint64_t total{}, bytes{}, target_bytes{};
    const auto add_conflict = [&](const std::string& id) {
        ++conflicts;
        if (conflict_ids.size() < 12) conflict_ids.push_back(id);
    };
    for (const auto& source : roots) {
        try {
            if (!saves::exists(source)) {
                if (source != default_root) ++errors;
                continue;
            }
            plain_directory(source);
            const auto canonical = fs::canonical(extended_path(source));
            if (std::any_of(result.sources_.begin(), result.sources_.end(), [&](const auto& known) {
                return within(known, canonical) && within(canonical, known);
            })) continue;
            result.sources_.push_back(canonical);
            const auto library = source / library_name;
            if (!saves::exists(library)) continue;
            if (locations.size() < 12) locations.push_back(path_text(source.native().substr(0, 300)));
            plain_directory(library);
            for (const auto& game_dir : fs::directory_iterator(library)) {
                if (!plain_child(game_dir)) continue;
                const auto game = path_text(game_dir.path().filename());
                for (const auto& profile_dir : fs::directory_iterator(game_dir)) {
                    if (!plain_child(profile_dir)) continue;
                    const auto profile = path_text(profile_dir.path().filename());
                    if (!valid_identity(game, profile)) continue;
                    for (const auto& entry : fs::directory_iterator(profile_dir)) {
                        const auto id = path_text(entry.path().filename());
                        if (!uuid(id)) continue;
                        try {
                            auto record = read_record(entry.path(), game, profile, id);
                            const auto archive = entry.path() / archive_name;
                            regular_file(archive);
                            const auto size = required_number(record, "bytes");
                            require(fs::file_size(archive) == size, "旧 ZIP 大小与清单不一致。");
                            total += size;
                            profiles.emplace(game, profile);
                            const auto base = backup_root(root, current, game);
                            const auto key = profile_path(base, game, profile) / path_of(id);
                            const auto hash = required_string(record, "sha256");
                            bool ready{};
                            if (const auto prior = seen.find(key); prior != seen.end()) {
                                if (prior->second == hash) ++duplicates;
                                else { blocked.insert(key); add_conflict(id); }
                            } else if (saves::exists(key)) {
                                try {
                                    const auto current_record = read_record(key, game, profile, id);
                                    regular_file(key / archive_name);
                                    target_bytes += fs::file_size(key / archive_name);
                                    require(current_record.at("sha256") == hash, "目标已存在其他内容。");
                                    verify_record(key / archive_name, record);
                                    ++duplicates;
                                } catch (const std::exception&) { blocked.insert(key); add_conflict(id); }
                            } else ready = true;
                            seen.try_emplace(key, hash);
                            result.entries_.push_back({entry.path(), base, key, std::move(record), ready});
                        } catch (const std::exception&) { ++errors; }
                    }
                }
            }
        } catch (const std::exception&) { ++errors; }
    }
    for (auto& entry : result.entries_) {
        if (blocked.contains(entry.destination)) entry.ready = false;
        if (entry.ready) bytes += required_number(entry.record, "bytes");
    }
    const auto cloud = legacy / L"webdav.json";
    try {
        if (saves::exists(cloud)) {
            auto config = DavConfig::parse(read_json(cloud));
            config.automatic = false;
            if (!config.url.empty()) result.cloud_ = std::move(config);
        }
    } catch (const std::exception&) { ++errors; }
    result.summary_ = {{"token", new_request_id()}, {"sources", locations}, {"records", result.entries_.size()},
        {"profiles", profiles.size()}, {"bytes_to_copy", bytes}, {"total_bytes", total}, {"existing_target_bytes", target_bytes},
        {"duplicates", duplicates}, {"conflicts", conflicts}, {"conflict_ids", conflict_ids}, {"unreadable", errors},
        {"webdav", result.cloud_.has_value()}};
    return result;
}
Json Migration::apply(const fs::path& root, Catalog& current) const {
    Json failures = Json::array();
    size_t copied{}, failed{};
    for (const auto& entry : entries_) {
        if (!entry.ready) continue;
        const auto id = required_string(entry.record, "id");
        try {
            const auto game = required_string(entry.record, "game_id"), profile = required_string(entry.record, "profile");
            const auto record = read_record(entry.source, game, profile, id);
            require(record_digest(record) == record_digest(entry.record), "原记录在预览后发生变化，请重新预览。");
            const auto base = known_path(entry.base);
            require(std::none_of(sources_.begin(), sources_.end(), [&](const auto& source) { return overlaps(base, source); }),
                "迁移目标与旧备份目录重叠。");
            fs::create_directories(base);
            const auto profile_dir = create_profile(base, game, profile);
            import_record(profile_dir, record, [&](const Sink& sink) {
                read_file(entry.source / archive_name, required_number(record, "bytes"), sink);
            });
            ++copied;
        } catch (const std::exception& error) {
            ++failed;
            if (failures.size() < 12) failures.push_back({{"id", id}, {"error", failure_message(error)}});
        }
    }
    auto next = current;
    auto& builtin = next.games.front();
    if (builtin.save_dir.empty()) {
        auto seed = catalog_.games.front();
        seed.backup_dir = builtin.backup_dir;
        builtin = std::move(seed);
    }
    for (size_t i = 1; i < catalog_.games.size(); ++i) {
        auto settings = catalog_.games[i];
        const auto id = settings.custom->app_id;
        if (std::any_of(next.games.begin(), next.games.end(), [&](const Settings& game) { return game.custom && game.custom->app_id == id; })) continue;
        settings.backup_dir = root / L"Backups";
        next.games.push_back(std::move(settings));
    }
    next.save(root);
    current = std::move(next);
    if (cloud_ && !saves::exists(root / L"webdav.json")) write_json(root / L"webdav.json", cloud_->json());
    return {{"copied", copied}, {"failed", failed}, {"failures", failures}, {"preview", summary_}};
}
}
