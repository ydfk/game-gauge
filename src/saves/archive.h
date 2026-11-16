#pragma once
#include "catalog.h"

namespace gauge::saves {
inline constexpr std::wstring_view archive_name = L"save.zip";
bool managed_name(std::string_view name, bool custom);
void verify_archive(const fs::path& archive, const Json& record);
void verify_record(const fs::path& archive, const Json& record);
fs::path rollback_path(const fs::path& save);
void commit_restore(const fs::path& save, const fs::path& staged, const fs::path& rollback);
using Receive = std::function<void(const Sink&)>;
void import_record(const fs::path& root, const Json& record, const Receive& receive);

class Library {
public:
    explicit Library(const Settings& settings);
    fs::path save, root;
    std::string game, profile;
    bool custom() const { return settings_.custom.has_value(); }
    std::vector<Json> records() const { return saves::records(root, game, profile); }
    std::optional<Json> backup(std::string_view trigger) const;
    std::string restore(const Json& record) const;
private:
    Settings settings_;
};
}
