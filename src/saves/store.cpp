#include "store.h"
#include <algorithm>
#include <charconv>
#include <format>
#include <fstream>

namespace gauge::saves {
namespace {
bool missing(DWORD error) { return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND; }
fs::path comparison_path(const fs::path& path) {
    const auto& value = path.native();
    if (value.starts_with(L"\\\\?\\UNC\\")) return fs::path(L"\\\\" + value.substr(8));
    if (value.size() >= 7 && value.starts_with(L"\\\\?\\") && value[5] == L':') return fs::path(value.substr(4));
    return path;
}
bool hex_digit(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }
bool valid_time(std::string_view value) {
    // v1 用 UTC RFC 3339，可选的小数部分最多九位。
    if (value.size() < 20 || value.size() > 30 || !value.ends_with('Z')) return false;
    auto number = [&](size_t from, size_t count) -> int {
        int parsed{};
        const auto begin = value.data() + from;
        const auto [end, error] = std::from_chars(begin, begin + count, parsed);
        return error == std::errc{} && end == begin + count ? parsed : -1;
    };
    if (value[4] != '-' || value[7] != '-' || value[10] != 'T' || value[13] != ':' || value[16] != ':') return false;
    const auto year = number(0, 4), month = number(5, 2), day = number(8, 2);
    const auto hour = number(11, 2), minute = number(14, 2), second = number(17, 2);
    if (year < 1 || month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 || second < 0 || second > 60) return false;
    return value.size() == 20 || (value.size() >= 22 && value[19] == '.' &&
        std::all_of(value.begin() + 20, value.end() - 1, [](char c) { return c >= '0' && c <= '9'; }));
}
}

void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
std::string failure_message(const std::exception& error) {
    if (const auto* filesystem = dynamic_cast<const fs::filesystem_error*>(&error)) {
        auto message = "文件操作失败：" + error_text(static_cast<DWORD>(filesystem->code().value()));
        if (!filesystem->path1().empty()) message += " " + path_text(filesystem->path1());
        return message;
    }
    return error.what();
}
bool exists(const fs::path& path) {
    const auto result = GetFileAttributesW(extended_path(path).c_str());
    if (result != INVALID_FILE_ATTRIBUTES) return true;
    const auto error = GetLastError();
    if (missing(error)) return false;
    throw std::runtime_error("无法访问存档路径：" + error_text(error));
}
DWORD attributes(const fs::path& path) {
    const auto result = GetFileAttributesW(extended_path(path).c_str());
    if (result == INVALID_FILE_ATTRIBUTES) throw std::runtime_error("无法读取存档路径：" + error_text(GetLastError()));
    return result;
}
void plain_directory(const fs::path& path) {
    require(path.is_absolute(), "请选择绝对目录。");
    // 旧工作进程将规范化路径写成 \\?\F:\...。先去掉这个命名空间前缀再逐级检查。
    for (auto part = comparison_path(path); !part.empty(); part = part.parent_path()) {
        const auto attr = attributes(part);
        require((attr & FILE_ATTRIBUTE_DIRECTORY) && !(attr & FILE_ATTRIBUTE_REPARSE_POINT), "目录包含重定向或文件。");
        if (part == part.parent_path()) break;
    }
}
void regular_file(const fs::path& path) {
    require(!(attributes(path) & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)), "所选路径必须是普通文件。");
}
bool equal_name(std::wstring_view a, std::wstring_view b) {
    if (a.empty() || b.empty()) return a.empty() && b.empty();
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}
bool within(const fs::path& path, const fs::path& root) {
    const auto checked = comparison_path(path), base = comparison_path(root);
    auto part = checked.begin();
    for (const auto& prefix : base) {
        if (part == checked.end() || !equal_name(part->native(), prefix.native())) return false;
        ++part;
    }
    return true;
}
bool overlaps(const fs::path& a, const fs::path& b) { return within(a, b) || within(b, a); }
fs::path path_of(std::string_view value) {
    require(value.find('\0') == std::string_view::npos, "路径中有无效字符。");
    auto path = fs::path(wide(value));
    return path.make_preferred();
}
fs::path extended_path(const fs::path& path) {
    if (!path.is_absolute()) return path;
    auto value = path.native();
    std::replace(value.begin(), value.end(), L'/', L'\\');
    if (value.starts_with(L"\\\\?\\")) return fs::path(value);
    if (value.starts_with(L"\\\\")) return fs::path(L"\\\\?\\UNC\\" + value.substr(2));
    return fs::path(L"\\\\?\\" + value);
}
std::string path_text(const fs::path& path) { return utf8(path.native()); }
std::string ascii_lower(std::string text) {
    for (auto& c : text) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return text;
}
std::wstring environment(const wchar_t* name) {
    std::wstring result(32768, L'\0');
    const auto size = GetEnvironmentVariableW(name, result.data(), static_cast<DWORD>(result.size()));
    result.resize(size < result.size() ? size : 0);
    return result;
}
std::string timestamp() {
    SYSTEMTIME now{};
    GetSystemTime(&now);
    return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
}
std::string trim(std::string text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    return first == std::string::npos ? std::string{} : text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}
size_t text_length(std::string_view text) {
    return std::count_if(text.begin(), text.end(), [](unsigned char c) { return (c & 0xc0) != 0x80; });
}

TemporaryDirectory::TemporaryDirectory(const fs::path& parent, std::wstring_view prefix)
    : directory_(extended_path(parent / (std::wstring(prefix) + wide(new_request_id())))) {
    plain_directory(parent);
    require(fs::create_directory(directory_), "无法创建暂存目录。");
}
TemporaryDirectory::~TemporaryDirectory() {
    if (!directory_.empty()) {
        std::error_code ignored;
        fs::remove_all(directory_, ignored);
    }
}
void TemporaryDirectory::commit(const fs::path& destination) {
    require(MoveFileExW(directory_.c_str(), extended_path(destination).c_str(), MOVEFILE_WRITE_THROUGH) != FALSE, "无法提交存档目录。");
    directory_.clear();
}

Sha256::Sha256() {
    require(BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &state_, nullptr, 0, nullptr, 0, 0) >= 0, "无法初始化 SHA-256。");
}
Sha256::~Sha256() { if (state_) BCryptDestroyHash(state_); }
void Sha256::add(std::span<const unsigned char> bytes) {
    if (!bytes.empty())
        require(BCryptHashData(state_, const_cast<unsigned char*>(bytes.data()), static_cast<ULONG>(bytes.size()), 0) >= 0, "SHA-256 读取失败。");
}
void Sha256::add(std::string_view text) { add({reinterpret_cast<const unsigned char*>(text.data()), text.size()}); }
Digest Sha256::finish() {
    Digest result{};
    require(BCryptFinishHash(state_, result.data(), static_cast<ULONG>(result.size()), 0) >= 0, "SHA-256 校验失败。");
    return result;
}
std::string hex(const Digest& digest) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result;
    for (auto byte : digest) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 15]);
    }
    return result;
}
std::string sha256(std::string_view text) { Sha256 hash; hash.add(text); return hex(hash.finish()); }
Digest read_file(const fs::path& path, uint64_t size, const Sink& sink) {
    UniqueHandle file(CreateFileW(extended_path(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    require(static_cast<bool>(file), "无法读取存档文件。");
    BY_HANDLE_FILE_INFORMATION info{};
    require(GetFileInformationByHandle(file.value, &info) &&
        !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)), "存档包含重定向或非普通文件。");
    std::array<unsigned char, 65536> buffer{};
    Sha256 hash;
    uint64_t read{};
    for (DWORD count{};;) {
        require(ReadFile(file.value, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr) != FALSE, "读取存档文件失败。");
        if (!count) break;
        require(count <= size - read, "读取期间存档大小变化。");
        read += count;
        const auto chunk = std::span(buffer).first(count);
        hash.add(chunk);
        if (sink) sink(chunk);
    }
    require(read == size, "读取期间存档大小变化。");
    return hash.finish();
}
void append_file(HANDLE file, std::span<const unsigned char> data) {
    while (!data.empty()) {
        const auto amount = static_cast<DWORD>(std::min<size_t>(data.size(), 65536));
        DWORD count{};
        require(WriteFile(file, data.data(), amount, &count, nullptr) && count, "写入存档文件失败。");
        data = data.subspan(count);
    }
}
UniqueHandle create_file(const fs::path& path) {
    UniqueHandle file(CreateFileW(extended_path(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    require(static_cast<bool>(file), "无法创建存档暂存文件。");
    return file;
}
void flush_file(HANDLE file) { require(FlushFileBuffers(file) != FALSE, "无法刷新存档文件。"); }
std::string read_text(const fs::path& path, uint64_t limit) {
    regular_file(path);
    const auto size = fs::file_size(extended_path(path));
    require(size <= limit, "数据文件超过大小限制。");
    std::string text;
    text.reserve(static_cast<size_t>(size));
    read_file(path, size, [&](auto chunk) { text.append(reinterpret_cast<const char*>(chunk.data()), chunk.size()); });
    return text;
}
Json read_json(const fs::path& path) { return Json::parse(read_text(path)); }
void write_json(const fs::path& path, const Json& json) {
    TemporaryDirectory staged(path.parent_path(), L".json-");
    const auto temporary = staged.path() / L"value.json";
    auto file = create_file(temporary);
    const auto text = json.dump(2) + "\n";
    append_file(file.value, {reinterpret_cast<const unsigned char*>(text.data()), text.size()});
    flush_file(file.value);
    file.reset();
    require(MoveFileExW(temporary.c_str(), extended_path(path).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE,
        "无法提交存档设置或清单。");
}

bool uuid(std::string_view id) {
    if (id.size() != 36) return false;
    for (size_t i = 0; i < id.size(); ++i) {
        const bool separator = i == 8 || i == 13 || i == 18 || i == 23;
        if (separator ? id[i] != '-' : !hex_digit(id[i])) return false;
    }
    return true;
}
bool steam_profile(std::string_view profile) {
    return profile.size() == 17 && std::all_of(profile.begin(), profile.end(), [](char ch) { return ch >= '0' && ch <= '9'; });
}
bool valid_identity(std::string_view game, std::string_view profile) {
    if (!game.starts_with("steam-")) return false;
    const auto value = game.substr(6);
    uint32_t id{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), id);
    return error == std::errc{} && end == value.data() + value.size() && id && std::to_string(id) == value &&
        (game == pbz_id ? steam_profile(profile) : profile == "default");
}
std::string required_string(const Json& json, const char* key) { return json.at(key).get<std::string>(); }
uint64_t required_number(const Json& json, const char* key) {
    require(json.contains(key) && json[key].is_number_unsigned(), "存档参数应为非负整数。");
    return json[key].get<uint64_t>();
}
bool flag(const Json& json, const char* key) { return json.at(key).get<bool>(); }
void validate_record(const Json& record, std::string_view game, std::string_view profile, std::string_view id) {
    require(valid_identity(game, profile) && uuid(id) && record.at("format") == 1 && required_string(record, "id") == id &&
        required_string(record, "game_id") == game && required_string(record, "profile") == profile, "备份清单与目录身份不一致。");
    const auto trigger = required_string(record, "trigger");
    require(trigger == "manual" || trigger == "scheduled" || trigger == "exit" || trigger == "before_restore", "备份触发类型无效。");
    require(valid_time(required_string(record, "at")), "备份时间格式无效。");
    require(record.at("locked").is_boolean() && text_length(required_string(record, "title")) <= 100 &&
        text_length(required_string(record, "note")) <= 2000, "备份元数据无效。");
    const auto bytes = required_number(record, "bytes");
    require(bytes && bytes <= max_zip_bytes, "压缩包体积超出限制。");
    for (const auto key : {"sha256", "content_sha256"}) {
        const auto hash = required_string(record, key);
        require(hash.size() == 64 && std::all_of(hash.begin(), hash.end(), hex_digit), "备份哈希格式无效。");
    }
}
Json read_record(const fs::path& directory, std::string_view game, std::string_view profile, std::string_view id) {
    plain_directory(directory);
    auto record = read_json(directory / L"record.json");
    validate_record(record, game, profile, id);
    return record;
}
void sort_records(std::vector<Json>& rows) {
    // RFC 3339 在小数点或 Z 之前的部分等长。补齐小数后可按字符串比较。
    auto key = [](const Json& row) {
        auto date = required_string(row, "at");
        date.pop_back();
        if (date.size() == 19) date += ".";
        date.resize(29, '0');
        return date;
    };
    std::sort(rows.begin(), rows.end(), [&](const Json& a, const Json& b) {
        const auto left = key(a), right = key(b);
        return left == right ? a.at("id") > b.at("id") : left > right;
    });
}
std::vector<Json> records(const fs::path& directory, std::string_view game, std::string_view profile) {
    if (!saves::exists(directory)) return {};
    plain_directory(directory);
    std::vector<Json> result;
    for (const auto& entry : fs::directory_iterator(extended_path(directory))) {
        const auto id = path_text(entry.path().filename());
        if (!uuid(id)) continue;
        const auto attr = attributes(entry.path());
        if (!(attr & FILE_ATTRIBUTE_DIRECTORY) || (attr & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        result.push_back(read_record(entry.path(), game, profile, id));
    }
    sort_records(result);
    return result;
}
std::string record_digest(const Json& record) {
    // 回执需要与旧版 serde 的字段顺序及紧凑编码一致。
    nlohmann::ordered_json ordered = nlohmann::ordered_json::object();
    for (const auto key : {"format", "id", "game_id", "profile", "at", "trigger", "title", "note", "locked", "bytes", "sha256", "content_sha256"})
        ordered[key] = record.at(key);
    return sha256(ordered.dump());
}
void delete_record(const fs::path& directory, const Json& record) {
    require(!flag(record, "locked"), "请先取消保护，再删除这份备份。");
    const auto id = wide(required_string(record, "id"));
    const auto path = extended_path(directory / id);
    plain_directory(path);
    const auto removed = extended_path(directory / (L".deleted-" + id));
    require(MoveFileExW(path.c_str(), removed.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE, "无法移除本地备份。");
    fs::remove_all(removed);
}
fs::path profile_path(const fs::path& root, std::string_view game, std::string_view profile) {
    require(valid_identity(game, profile), "备份库身份不合法。");
    return root / library_name / path_of(game) / path_of(profile);
}
fs::path create_profile(const fs::path& root, std::string_view game, std::string_view profile) {
    auto current = extended_path(root);
    const auto directory = profile_path(current, game, profile);
    plain_directory(current);
    for (const auto& component : directory.lexically_relative(current)) {
        current /= component;
        fs::create_directory(current);
        plain_directory(current);
    }
    return directory;
}
}
