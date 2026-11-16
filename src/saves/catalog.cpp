#include "catalog.h"
#include <tlhelp32.h>
#include <algorithm>
#include <sstream>

namespace gauge::saves {
namespace {
constexpr uint32_t pbz_app_id = 4115450;

std::optional<std::string> vdf_field(const std::string& line, std::string_view key) {
    const auto start = line.find_first_not_of(" \t\r");
    if (start == std::string::npos || line[start] != '"') return {};
    const auto end = line.find('"', start + 1);
    if (end == std::string::npos || std::string_view(line).substr(start + 1, end - start - 1) != key) return {};
    auto value = trim(line.substr(end + 1));
    if (value.size() < 2 || value.front() != '"' || value.back() != '"') return {};
    value = value.substr(1, value.size() - 2);
    for (size_t offset = 0; (offset = value.find("\\\\", offset)) != std::string::npos; ++offset) value.erase(offset, 1);
    return value;
}
struct RegistrySteam {
    HKEY hive;
    const wchar_t* key;
    const wchar_t* value;
};
std::set<fs::path> steam_libraries() {
    std::set<fs::path> libraries;
    const RegistrySteam sources[]{
        {HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath"},
        {HKEY_LOCAL_MACHINE, L"Software\\WOW6432Node\\Valve\\Steam", L"InstallPath"},
        {HKEY_LOCAL_MACHINE, L"Software\\Valve\\Steam", L"InstallPath"},
    };
    for (const auto& source : sources) {
        std::wstring path(32768, L'\0');
        auto length = static_cast<DWORD>(path.size() * sizeof(wchar_t));
        if (RegGetValueW(source.hive, source.key, source.value, RRF_RT_REG_SZ, nullptr, path.data(), &length) != ERROR_SUCCESS ||
            length <= sizeof(wchar_t)) continue;
        path.resize(length / sizeof(wchar_t) - 1);
        const fs::path steam(path);
        libraries.insert(steam);
        try {
            std::istringstream list(read_text(steam / L"steamapps" / L"libraryfolders.vdf"));
            for (std::string line; std::getline(list, line);) {
                if (const auto value = vdf_field(line, "path")) {
                    const auto library = path_of(*value);
                    if (library.is_absolute()) libraries.insert(library);
                }
            }
        } catch (const std::exception&) { /* Steam 可能尚未生成库列表。 */ }
    }
    return libraries;
}
std::optional<std::pair<fs::path, std::string>> manifest(uint32_t app_id) {
    for (const auto& library : steam_libraries()) {
        try {
            const auto apps = library / L"steamapps";
            std::istringstream file(read_text(apps / (L"appmanifest_" + std::to_wstring(app_id) + L".acf")));
            std::optional<std::string> folder, title;
            for (std::string line; std::getline(file, line) && !(folder && title);) {
                if (!folder) folder = vdf_field(line, "installdir");
                if (!title) title = vdf_field(line, "name");
            }
            if (!folder || !title) continue;
            const auto name = path_of(*folder);
            if (name.empty() || name != name.filename() || name == L"." || name == L"..") continue;
            return std::pair{apps / L"common" / name, *title};
        } catch (const std::exception&) { /* 继续检查其它 Steam 库。 */ }
    }
    return {};
}
bool valid_executable(std::string_view name) {
    return name.size() > 4 && name.size() <= 150 && ascii_lower(std::string(name)).ends_with(".exe") &&
        std::none_of(name.begin(), name.end(), [](unsigned char ch) {
            return ch < 32 || ch == 127 || std::string_view("\\/:*?\"<>|").find(static_cast<char>(ch)) != std::string_view::npos;
        });
}
fs::path resolve_directory(const fs::path& path) {
    require(path.is_absolute() && !path.filename().empty() && path.filename() != L"." && path.filename() != L"..",
        "请选择游戏专用的绝对存档目录。");
    const auto parent = path.parent_path();
    plain_directory(parent);
    if (!saves::exists(path)) return extended_path(fs::canonical(extended_path(parent))) / path.filename();
    plain_directory(path);
    return extended_path(fs::canonical(extended_path(path)));
}
bool pbz_process(std::string_view name) {
    return name == "pbz.exe" || name == "projectpbz.exe" || name == "projectpbz-win64-shipping.exe" || name == "pbz-win64-shipping.exe";
}
}

std::pair<std::string, std::string> Settings::identity() const {
    if (custom) return {"steam-" + std::to_string(custom->app_id), "default"};
    const auto account = path_text(save_dir.filename());
    return {std::string(pbz_id), steam_profile(account) ? account : std::string{}};
}
std::string Settings::title() const { return custom ? custom->title : "影之刃零"; }
Json Settings::json() const {
    return {{"save_dir", path_text(save_dir)}, {"backup_dir", path_text(backup_dir)},
        {"interval_minutes", interval_minutes}, {"keep_auto", keep_auto}, {"auto_enabled", auto_enabled}, {"hotkey", hotkey}, {"dark", dark}};
}
Settings Settings::parse(const Json& json) {
    Settings result;
    result.save_dir = path_of(json.value("save_dir", std::string{}));
    result.backup_dir = path_of(json.value("backup_dir", std::string{}));
    result.interval_minutes = json.value("interval_minutes", result.interval_minutes);
    result.keep_auto = json.value("keep_auto", result.keep_auto);
    result.auto_enabled = json.value("auto_enabled", result.auto_enabled);
    result.hotkey = json.value("hotkey", result.hotkey);
    result.dark = json.value("dark", result.dark);
    return result;
}
Catalog Catalog::load(const fs::path& root) {
    const auto file = root / L"settings.json";
    const auto json = saves::exists(file) ? read_json(file) : Json::object();
    Catalog result;
    result.games.push_back(Settings::parse(json));
    if (result.games.front().backup_dir.empty()) result.games.front().backup_dir = root / L"Backups";
    std::set<uint32_t> ids;
    for (const auto& entry : json.value("additional", Json::array())) {
        const auto& identity = entry.at("game");
        const auto id = required_number(identity, "app_id");
        const auto name = ascii_lower(required_string(identity, "executable"));
        require(id > 0 && id <= UINT32_MAX && id != pbz_app_id && ids.insert(static_cast<uint32_t>(id)).second &&
            valid_executable(name) && !pbz_process(name), "已保存的 Steam 游戏身份无效。");
        auto settings = Settings::parse(entry.at("settings"));
        settings.custom = SteamGame{static_cast<uint32_t>(id), required_string(identity, "title"), name};
        settings.hotkey = result.games.front().hotkey;
        settings.dark = result.games.front().dark;
        result.games.push_back(std::move(settings));
    }
    result.selected = json.value("selected", size_t{});
    if (result.selected >= result.games.size()) result.selected = 0;
    result.enabled = json.value("enabled", false);
    return result;
}
void Catalog::save(const fs::path& root) const {
    auto json = games.front().json();
    Json additional = Json::array();
    for (size_t i = 1; i < games.size(); ++i) {
        const auto& game = *games[i].custom;
        additional.push_back({{"game", {{"app_id", game.app_id}, {"title", game.title}, {"executable", game.executable}}},
            {"settings", games[i].json()}});
    }
    json["additional"] = std::move(additional);
    json["selected"] = selected;
    json["enabled"] = enabled;
    write_json(root / L"settings.json", json);
}
void Catalog::add(uint32_t id, const std::string& exe, const fs::path& save_dir) {
    const auto name = ascii_lower(exe);
    require(id && id != pbz_app_id && games.size() <= 40 && valid_executable(name) && !pbz_process(name), "Steam App ID 或进程文件名无效。");
    for (const auto& settings : games) {
        if (settings.custom)
            require(settings.custom->app_id != id && !equal_name(wide(settings.custom->executable), wide(name)), "此 Steam 游戏或进程已添加。");
    }
    const auto game = manifest(id);
    require(game.has_value(), "请先安装指定 App ID 的 Steam 游戏。");
    Settings next;
    next.custom = SteamGame{id, game->second, name};
    next.save_dir = save_dir;
    next.hotkey = games.front().hotkey;
    next.dark = games.front().dark;
    next.save_dir = resolve_save(next);
    next.backup_dir = backup_location(next.save_dir, games.front().backup_dir);
    check_overlap(next.save_dir, next.backup_dir);
    games.push_back(std::move(next));
    selected = games.size() - 1;
}
void Catalog::check_overlap(const fs::path& save, const fs::path& backup, std::optional<size_t> exclude) const {
    for (size_t i = 0; i < games.size(); ++i) {
        if (i == exclude || games[i].save_dir.empty()) continue;
        fs::path other;
        try { other = resolve_save(games[i]); }
        catch (const std::exception&) { continue; }
        require(!overlaps(save, other) && !overlaps(backup / library_name, other), "存档或备份目录与另一款游戏重叠。");
        fs::path other_backup;
        try { other_backup = backup_location(other, games[i].backup_dir); }
        catch (const std::exception&) { continue; }
        require(!overlaps(save, other_backup / library_name), "存档位置与另一款游戏的备份库重叠。");
    }
}
std::vector<fs::path> discover_slots() {
    const auto game = manifest(pbz_app_id);
    if (!game) return {};
    const auto tree = game->first / LR"(ProjectPBZ\Saved\SaveGames\Steam)";
    std::error_code error;
    std::vector<fs::path> slots;
    for (const auto& entry : fs::directory_iterator(tree, error)) {
        try {
            if (!steam_profile(path_text(entry.path().filename()))) continue;
            const auto slot = resolve_directory(entry.path());
            regular_file(slot / main_save);
            if (fs::file_size(slot / main_save)) slots.push_back(slot);
        } catch (const std::exception&) { /* 展示可用账号。 */ }
    }
    std::sort(slots.begin(), slots.end());
    return slots;
}
fs::path resolve_save(const Settings& settings) {
    const auto resolved = resolve_directory(settings.save_dir);
    if (!settings.custom) {
        require(steam_profile(path_text(resolved.filename())), "请选择包含 PBZSaveGame.sav 的 17 位 Steam 账号目录。");
        return resolved;
    }
    auto protected_paths = steam_libraries();
    if (const auto installed = manifest(settings.custom->app_id)) protected_paths.insert(installed->first);
    for (const auto name : {L"USERPROFILE", L"SystemRoot", L"ProgramFiles", L"ProgramFiles(x86)", L"ProgramData", L"APPDATA", L"LOCALAPPDATA"}) {
        const auto path = environment(name);
        if (!path.empty()) protected_paths.insert(path);
    }
    require(resolved.has_relative_path(), "不能用磁盘根目录作存档目录。");
    for (const auto& path : protected_paths) {
        std::error_code error;
        const auto canonical = fs::canonical(path, error);
        require(error || !within(canonical, resolved), "请选择专用存档文件夹，不能使用系统、用户或 Steam 游戏库根目录。");
    }
    require(!saves::exists(resolved / path_of(settings.custom->executable)), "请选择存档文件夹，不能选择程序所在目录。");
    return resolved;
}
fs::path backup_location(const fs::path& save, const fs::path& requested) {
    require(requested.is_absolute(), "请选择绝对路径作为备份位置。");
    auto ancestor = requested;
    while (!saves::exists(ancestor)) {
        require(ancestor != ancestor.parent_path(), "找不到备份路径所在磁盘。");
        ancestor = ancestor.parent_path();
    }
    plain_directory(ancestor);
    auto resolved = extended_path(fs::canonical(extended_path(ancestor)));
    require(!within(resolved, save), "备份位置不能在存档文件夹内。");
    if (ancestor != requested) {
        for (const auto& part : requested.lexically_relative(ancestor)) {
            require(!part.empty() && part != L".." && part != L".", "备份位置包含无效路径。");
            resolved /= part;
        }
    }
    require(!overlaps(resolved / library_name, save), "存档位置和备份库不能重叠。");
    return resolved;
}
std::set<std::string> processes() {
    DWORD own{};
    require(ProcessIdToSessionId(GetCurrentProcessId(), &own) != FALSE, "无法核对当前会话。");
    UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    require(static_cast<bool>(snapshot), "无法查询游戏进程。");
    PROCESSENTRY32W process{sizeof(process)};
    std::set<std::string> names;
    auto next = Process32FirstW(snapshot.value, &process);
    while (next) {
        DWORD session{};
        if (ProcessIdToSessionId(process.th32ProcessID, &session) && session == own)
            names.insert(ascii_lower(utf8(process.szExeFile)));
        next = Process32NextW(snapshot.value, &process);
    }
    require(GetLastError() == ERROR_NO_MORE_FILES, "无法完成进程列表扫描。");
    return names;
}
bool running(const Settings& settings, const std::set<std::string>& names) {
    if (settings.custom) {
        const auto& name = settings.custom->executable;
        if (names.contains(name)) return true;
        if (std::all_of(name.begin(), name.end(), [](unsigned char ch) { return ch < 128; })) return false;
        return std::any_of(names.begin(), names.end(), [&](const auto& process) { return equal_name(wide(process), wide(name)); });
    }
    return std::any_of(names.begin(), names.end(), pbz_process);
}
}
