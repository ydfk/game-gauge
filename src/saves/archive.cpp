#include "archive.h"
#include <miniz.h>
#include <io.h>
#include <fcntl.h>
#include <algorithm>
#include <bit>
#include <cstdio>
#include <memory>
#include <ranges>
#include <thread>

namespace gauge::saves {
namespace {
const std::array<std::wstring_view, 3> rotation{L"1RealTime", L"2Hourly", L"3Daily"};
struct Entry {
    std::string name;
    uint64_t bytes;
    fs::file_time_type modified;
    bool operator==(const Entry&) const = default;
};

bool reserved(std::string_view part) {
    auto stem = ascii_lower(trim(std::string(part.substr(0, part.find('.')))));
    if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul" || stem == "conin$" || stem == "conout$") return true;
    if (!stem.starts_with("com") && !stem.starts_with("lpt")) return false;
    const auto number = std::string_view(stem).substr(3);
    return (number.size() == 1 && number.front() >= '1' && number.front() <= '9') || number == "¹" || number == "²" || number == "³";
}
bool save_file(const fs::path& path) { return equal_name(path.extension().native(), L".sav"); }
std::string relative_name(const fs::path& path) {
    auto result = path_text(path);
    std::replace(result.begin(), result.end(), '\\', '/');
    return result;
}
std::vector<Entry> inventory(const fs::path& root, bool custom) {
    plain_directory(root);
    std::vector<Entry> result;
    uint64_t total{};
    const auto add = [&](const fs::path& path) {
        regular_file(path);
        const auto name = relative_name(path.lexically_relative(root));
        const auto bytes = fs::file_size(path);
        require(managed_name(name, custom), "存档文件名不安全。");
        require(result.size() < max_files && bytes <= max_bytes - total, "存档超过单份备份限制。");
        result.push_back({name, bytes, fs::last_write_time(path)});
        total += bytes;
    };
    if (custom) {
        for (const auto& item : fs::recursive_directory_iterator(root)) {
            const auto name = item.path().filename().native();
            const auto attr = attributes(item.path());
            require(!equal_name(name, library_name) && !(attr & FILE_ATTRIBUTE_REPARSE_POINT), "存档包含备份库或重定向项。");
            if (!(attr & FILE_ATTRIBUTE_DIRECTORY) && !equal_name(name, L"steam_autocloud.vdf")) add(item.path());
        }
    } else {
        for (const auto& item : fs::directory_iterator(root)) {
            if (std::ranges::find(rotation, item.path().filename().native()) != rotation.end()) {
                plain_directory(item.path());
                for (const auto& child : fs::recursive_directory_iterator(item.path())) {
                    const auto attr = attributes(child.path());
                    require(!(attr & FILE_ATTRIBUTE_REPARSE_POINT), "游戏轮转备份目录包含重定向。");
                    if (!(attr & FILE_ATTRIBUTE_DIRECTORY) && save_file(child.path())) add(child.path());
                }
            } else if (save_file(item.path())) add(item.path());
        }
        require(std::any_of(result.begin(), result.end(), [](const Entry& entry) {
            return ascii_lower(entry.name) == "pbzsavegame.sav" && entry.bytes;
        }), "找不到可用的主存档。");
    }
    std::ranges::sort(result, {}, &Entry::name);
    return result;
}
void mix(Sha256& hash, std::string_view name, uint64_t bytes, const Digest& full) {
    // v1 内容指纹使用 UTF-8 路径和小端整数。
    static_assert(std::endian::native == std::endian::little);
    const auto length = static_cast<uint64_t>(name.size());
    hash.add({reinterpret_cast<const unsigned char*>(&length), sizeof(length)});
    hash.add(name);
    hash.add({reinterpret_cast<const unsigned char*>(&bytes), sizeof(bytes)});
    hash.add(full);
}
std::string fingerprint(const fs::path& root, bool custom) {
    Sha256 combined;
    for (const auto& entry : inventory(root, custom))
        mix(combined, entry.name, entry.bytes, read_file(root / path_of(entry.name), entry.bytes));
    return hex(combined.finish());
}

using File = std::unique_ptr<FILE, decltype(&fclose)>;
File zip_file(const fs::path& path, bool write = false) {
    UniqueHandle handle(CreateFileW(extended_path(path).c_str(), write ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ,
        write ? 0 : FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, write ? CREATE_NEW : OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    require(static_cast<bool>(handle), "无法打开存档 ZIP。");
    BY_HANDLE_FILE_INFORMATION info{};
    require(GetFileInformationByHandle(handle.value, &info) &&
        !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)), "ZIP 或源文件被重定向。");
    const auto descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(handle.value), _O_BINARY | (write ? _O_RDWR : _O_RDONLY));
    require(descriptor >= 0, "无法打开 ZIP 数据流。");
    handle.value = INVALID_HANDLE_VALUE;
    File file(_fdopen(descriptor, write ? "w+b" : "rb"), fclose);
    if (!file) _close(descriptor);
    require(static_cast<bool>(file), "无法打开 ZIP 数据流。");
    return file;
}
struct Zip {
    mz_zip_archive handle{};
    ~Zip() { if (handle.m_pState) mz_zip_end(&handle); }
};
void pack(const fs::path& root, const fs::path& archive, const std::vector<Entry>& entries) {
    auto output = zip_file(archive, true);
    Zip zip;
    require(mz_zip_writer_init_cfile(&zip.handle, output.get(), MZ_ZIP_FLAG_WRITE_ZIP64), "无法创建存档 ZIP。");
    for (const auto& entry : entries) {
        auto input = zip_file(root / path_of(entry.name));
        require(mz_zip_writer_add_cfile(&zip.handle, entry.name.c_str(), input.get(), entry.bytes, nullptr,
            nullptr, 0, MZ_BEST_SPEED, nullptr, 0, nullptr, 0), "压缩存档文件失败。");
    }
    require(mz_zip_writer_finalize_archive(&zip.handle) && mz_zip_writer_end(&zip.handle) &&
        fflush(output.get()) == 0 && _commit(_fileno(output.get())) == 0, "无法提交存档 ZIP。");
}
struct Extract {
    Sha256 hash;
    UniqueHandle output;
    uint64_t size{}, read{};
    std::exception_ptr error;
};
size_t extract_chunk(void* opaque, mz_uint64 offset, const void* data, size_t length) noexcept {
    auto& state = *static_cast<Extract*>(opaque);
    try {
        require(offset == state.read && length <= state.size - state.read, "ZIP 解压大小与清单不一致。");
        const std::span chunk(static_cast<const unsigned char*>(data), length);
        state.hash.add(chunk);
        if (state.output) append_file(state.output.value, chunk);
        state.read += length;
        return length;
    } catch (...) { state.error = std::current_exception(); return 0; }
}
std::string snapshot(const fs::path& archive, bool custom, const fs::path& destination = {}) {
    auto file = zip_file(archive);
    Zip zip;
    require(mz_zip_reader_init_cfile(&zip.handle, file.get(), 0, 0), "无法读取 ZIP 目录。");
    const auto count = mz_zip_reader_get_num_files(&zip.handle);
    require(count > 0 && count <= max_files, "备份内文件数超出限制。");
    struct Item {
        std::string name;
        uint64_t bytes;
        mz_uint index;
    };
    struct CaseInsensitive {
        bool operator()(const std::wstring& a, const std::wstring& b) const {
            return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
        }
    };
    std::set<std::wstring, CaseInsensitive> names;
    std::vector<Item> items;
    uint64_t total{};
    bool main{};
    for (mz_uint i{}; i < count; ++i) {
        mz_zip_archive_file_stat stat{};
        require(mz_zip_reader_file_stat(&zip.handle, i, &stat) && stat.m_is_supported && !stat.m_is_encrypted && !stat.m_is_directory,
            "ZIP 包含不支持的文件类型。");
        const auto mode = (stat.m_external_attr >> 16) & 0170000;
        require(mode == 0 || mode == 0100000, "备份内含链接或特殊文件。");
        const auto length = mz_zip_reader_get_filename(&zip.handle, i, nullptr, 0);
        require(length > 1 && length <= 32768, "ZIP 文件名长度无效。");
        std::string name(length, '\0');
        require(mz_zip_reader_get_filename(&zip.handle, i, name.data(), length) == length, "无法读取 ZIP 文件名。");
        name.pop_back();
        require(managed_name(name, custom) && names.insert(wide(name)).second, "备份含有重复或不安全的文件路径。");
        require(stat.m_uncomp_size <= max_bytes - total, "备份体积超出限制。");
        total += stat.m_uncomp_size;
        main |= ascii_lower(name) == "pbzsavegame.sav" && stat.m_uncomp_size != 0;
        items.push_back({std::move(name), stat.m_uncomp_size, i});
    }
    require(custom || main, "备份中没有可用的主存档。");
    std::ranges::sort(items, {}, &Item::name);
    Sha256 combined;
    for (const auto& item : items) {
        Extract extract;
        extract.size = item.bytes;
        if (!destination.empty()) {
            const auto path = destination / path_of(item.name);
            fs::create_directories(path.parent_path());
            plain_directory(path.parent_path());
            auto output = create_file(path);
            extract.output.reset(std::exchange(output.value, INVALID_HANDLE_VALUE));
        }
        const auto ok = mz_zip_reader_extract_to_callback(&zip.handle, item.index, extract_chunk, &extract, 0);
        if (extract.error) std::rethrow_exception(extract.error);
        require(ok && extract.read == item.bytes, "ZIP 解压或 CRC 校验失败。");
        if (extract.output) flush_file(extract.output.value);
        mix(combined, item.name, item.bytes, extract.hash.finish());
    }
    return hex(combined.finish());
}
void copy_unmanaged(const fs::path& source, const fs::path& dest, bool custom) {
    for (const auto& entry : fs::recursive_directory_iterator(source)) {
        const auto attr = attributes(entry.path());
        require(!(attr & FILE_ATTRIBUTE_REPARSE_POINT), "存档目录含重定向项，已停止恢复。");
        const auto relative = entry.path().lexically_relative(source);
        const auto target = dest / relative;
        if (attr & FILE_ATTRIBUTE_DIRECTORY) fs::create_directories(target);
        else if (!managed_name(relative_name(relative), custom)) {
            auto output = create_file(target);
            read_file(entry.path(), fs::file_size(entry.path()), [&](auto chunk) { append_file(output.value, chunk); });
            flush_file(output.value);
        }
    }
}
std::string label(std::string_view trigger) {
    if (trigger == "scheduled") return "定时备份";
    if (trigger == "exit") return "退出游戏备份";
    if (trigger == "before_restore") return "恢复前保护备份";
    return "手动备份";
}
}

bool managed_name(std::string_view name, bool custom) {
    if (name.empty() || name.size() >= 32768 || utf8(wide(name)) != name) return false;
    const auto first = name.find('/');
    std::string_view last;
    for (size_t begin = 0;;) {
        const auto end = name.find('/', begin);
        last = name.substr(begin, end == std::string_view::npos ? end : end - begin);
        if (last.empty() || last.ends_with('.') || last.ends_with(' ') || reserved(last) ||
            std::any_of(last.begin(), last.end(), [](unsigned char c) {
                return c < 32 || c == 127 || std::string_view("\\:*?\"<>|").find(static_cast<char>(c)) != std::string_view::npos;
            })) return false;
        if (end == std::string_view::npos) break;
        begin = end + 1;
    }
    if (custom) return ascii_lower(std::string(last)) != "steam_autocloud.vdf";
    return (first == std::string_view::npos || std::ranges::find(rotation, wide(name.substr(0, first))) != rotation.end()) &&
        save_file(path_of(last));
}
fs::path rollback_path(const fs::path& save) {
    return save.parent_path() / (L".savekeep-original-" + save.filename().native());
}
void commit_restore(const fs::path& save, const fs::path& staged, const fs::path& rollback) {
    require(MoveFileExW(save.c_str(), rollback.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE, "无法暂存原始存档目录。");
    if (MoveFileExW(staged.c_str(), save.c_str(), MOVEFILE_WRITE_THROUGH)) return;
    if (MoveFileExW(rollback.c_str(), save.c_str(), MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("替换失败，原始存档已恢复。");
    throw std::runtime_error("自动回滚失败，原始存档在 " + path_text(rollback));
}
void verify_archive(const fs::path& archive, const Json& record) {
    const auto bytes = required_number(record, "bytes");
    require(bytes && bytes <= max_zip_bytes, "压缩包大小无效。");
    require(hex(read_file(archive, bytes)) == required_string(record, "sha256"), "备份 ZIP 未通过 SHA-256 校验。");
}
void verify_record(const fs::path& archive, const Json& record) {
    verify_archive(archive, record);
    require(snapshot(archive, required_string(record, "game_id") != pbz_id) == required_string(record, "content_sha256"),
        "备份内容校验失败。");
}
void import_record(const fs::path& root, const Json& record, const Receive& receive) {
    const auto game = required_string(record, "game_id"), profile = required_string(record, "profile"), id = required_string(record, "id");
    validate_record(record, game, profile, id);
    const auto destination = root / path_of(id);
    if (saves::exists(destination)) {
        const auto existing = read_record(destination, game, profile, id);
        require(existing.at("sha256") == record.at("sha256"), "本地同 ID 备份内容不同，已停止覆盖。");
        verify_record(destination / archive_name, record);
        return;
    }
    TemporaryDirectory staged(root);
    const auto zip = staged.path() / archive_name;
    auto output = create_file(zip);
    const auto expected = required_number(record, "bytes");
    uint64_t received{};
    receive([&](auto chunk) {
        require(chunk.size() <= expected - received, "复制的备份大小与清单不一致。");
        append_file(output.value, chunk);
        received += chunk.size();
    });
    require(received == expected, "复制的备份大小与清单不一致。");
    flush_file(output.value);
    output.reset();
    verify_record(zip, record);
    write_json(staged.path() / L"record.json", record);
    staged.commit(destination);
}
Library::Library(const Settings& settings) : settings_(settings) {
    require(!settings.save_dir.empty(), "请先选择存档目录。");
    const auto original = rollback_path(settings.save_dir);
    if (!saves::exists(settings.save_dir) && saves::exists(original))
        throw std::runtime_error("上次恢复被中断，请先检查原始存档目录 " + path_text(original));
    save = resolve_save(settings);
    std::tie(game, profile) = settings.identity();
    const auto base = backup_location(save, settings.backup_dir);
    fs::create_directories(base);
    root = create_profile(base, game, profile);
}
std::optional<Json> Library::backup(std::string_view trigger) const {
    std::optional<Json> previous;
    if (trigger == "scheduled" || trigger == "exit") {
        const auto rows = records();
        if (!rows.empty() && rows.front().at("trigger") != "before_restore") previous = rows.front();
    }
    if (previous && fingerprint(save, custom()) == required_string(*previous, "content_sha256")) return {};
    std::string last_error;
    for (int attempt = 0; attempt < 3; ++attempt) {
        try {
            TemporaryDirectory staged(root);
            const auto file = staged.path() / archive_name;
            const auto before = inventory(save, custom());
            require(!before.empty(), "存档目录内没有可备份的文件。");
            std::this_thread::sleep_for(std::chrono::milliseconds(450));
            require(before == inventory(save, custom()), "存档仍在写入。");
            pack(save, file, before);
            const auto content = snapshot(file, custom());
            require(content == fingerprint(save, custom()), "压缩期间存档内容发生变化。");
            if (previous && previous->at("content_sha256") == content) return {};
            const auto bytes = fs::file_size(file);
            const auto id = new_request_id();
            Json record{{"format", 1}, {"id", id}, {"game_id", game}, {"profile", profile}, {"at", timestamp()},
                {"trigger", trigger}, {"title", label(trigger)}, {"note", ""}, {"locked", trigger == "before_restore"},
                {"bytes", bytes}, {"sha256", hex(read_file(file, bytes))}, {"content_sha256", content}};
            write_json(staged.path() / L"record.json", record);
            staged.commit(root / path_of(id));
            return record;
        } catch (const std::exception& error) { last_error = failure_message(error); }
    }
    throw std::runtime_error("备份失败：" + last_error);
}
std::string Library::restore(const Json& record) const {
    require(!running(settings_, processes()), "请关闭游戏后再恢复。");
    const auto rollback = rollback_path(save);
    require(!saves::exists(rollback), "上次恢复的原始目录仍存在，请先检查同级的 .savekeep-original-* 目录。");
    const auto zip = root / path_of(required_string(record, "id")) / archive_name;
    verify_record(zip, record);
    const bool has_directory = saves::exists(save);
    auto has_content = [&] {
        if (!saves::exists(save)) return false;
        if (custom()) return !inventory(save, true).empty();
        const auto main = save / main_save;
        if (!saves::exists(main)) return false;
        regular_file(main);
        return fs::file_size(main) > 0;
    };
    std::optional<Json> protected_save;
    if (has_content()) {
        protected_save = backup("before_restore");
        require(protected_save.has_value(), "无法保护当前存档。");
    }
    TemporaryDirectory staged(save.parent_path(), L".savekeep-restore-");
    if (has_directory) copy_unmanaged(save, staged.path(), custom());
    require(snapshot(zip, custom(), staged.path()) == required_string(record, "content_sha256"), "待恢复的存档未通过校验。");
    require(saves::exists(save) == has_directory &&
        (protected_save ? fingerprint(save, custom()) == required_string(*protected_save, "content_sha256") : !has_content()),
        "当前存档已变化，恢复已取消。");
    require(!running(settings_, processes()), "游戏已启动，恢复已取消。");
    if (!has_directory) {
        staged.commit(save);
        return "已从备份重建丢失的存档目录。";
    }
    commit_restore(save, staged.path(), rollback);
    const auto preserved = save.parent_path() / (L".savekeep-preserved-" + wide(new_request_id()));
    const bool moved = MoveFileExW(rollback.c_str(), preserved.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
    return "恢复完成。原始目录保留在 " + path_text(moved ? preserved : rollback) + "。";
}
}
