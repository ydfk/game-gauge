#pragma once
#include "common/config.h"
#include "common/platform.h"
#include <bcrypt.h>
#include <array>
#include <functional>
#include <span>
#include <string_view>

namespace gauge::saves {
namespace fs = std::filesystem;
inline constexpr std::string_view pbz_id = "steam-4115450";
inline constexpr std::wstring_view main_save = L"PBZSaveGame.sav";
inline constexpr std::wstring_view library_name = L"SaveKeep Library";
inline constexpr uint64_t max_bytes = 4ull * 1024 * 1024 * 1024;
inline constexpr uint64_t max_zip_bytes = max_bytes + 16 * 1024 * 1024;
inline constexpr size_t max_files = 10000;

void require(bool condition, const char* message);
std::string failure_message(const std::exception& error);
bool exists(const fs::path& path);
DWORD attributes(const fs::path& path);
void plain_directory(const fs::path& path);
void regular_file(const fs::path& path);
bool within(const fs::path& path, const fs::path& root);
bool overlaps(const fs::path& a, const fs::path& b);
bool equal_name(std::wstring_view a, std::wstring_view b);
fs::path path_of(std::string_view value);
fs::path extended_path(const fs::path& path);
std::string path_text(const fs::path& path);
std::string ascii_lower(std::string text);
std::wstring environment(const wchar_t* name);
std::string timestamp();
std::string trim(std::string text);
size_t text_length(std::string_view text);

class TemporaryDirectory {
public:
    explicit TemporaryDirectory(const fs::path& parent, std::wstring_view prefix = L".incoming-");
    ~TemporaryDirectory();
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    const fs::path& path() const { return directory_; }
    void commit(const fs::path& destination);
private:
    fs::path directory_;
};

using Digest = std::array<unsigned char, 32>;
class Sha256 {
public:
    Sha256();
    ~Sha256();
    Sha256(const Sha256&) = delete;
    void add(std::span<const unsigned char> bytes);
    void add(std::string_view text);
    Digest finish();
private:
    BCRYPT_HASH_HANDLE state_{};
};
std::string hex(const Digest& digest);
std::string sha256(std::string_view text);
using Sink = std::function<void(std::span<const unsigned char>)>;
Digest read_file(const fs::path& path, uint64_t size, const Sink& sink = {});
void append_file(HANDLE file, std::span<const unsigned char> data);
UniqueHandle create_file(const fs::path& path);
void flush_file(HANDLE file);
std::string read_text(const fs::path& path, uint64_t limit = 1024 * 1024);
Json read_json(const fs::path& path);
void write_json(const fs::path& path, const Json& json);

bool uuid(std::string_view id);
bool steam_profile(std::string_view profile);
bool valid_identity(std::string_view game, std::string_view profile);
std::string required_string(const Json& json, const char* key);
uint64_t required_number(const Json& json, const char* key);
bool flag(const Json& json, const char* key);
void validate_record(const Json& record, std::string_view game, std::string_view profile, std::string_view id);
Json read_record(const fs::path& directory, std::string_view game, std::string_view profile, std::string_view id);
std::vector<Json> records(const fs::path& directory, std::string_view game, std::string_view profile);
void sort_records(std::vector<Json>& rows);
std::string record_digest(const Json& record);
void delete_record(const fs::path& directory, const Json& record);
fs::path profile_path(const fs::path& root, std::string_view game, std::string_view profile);
fs::path create_profile(const fs::path& root, std::string_view game, std::string_view profile);
}
