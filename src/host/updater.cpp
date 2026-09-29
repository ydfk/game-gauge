#include "updater.h"
#include "common/update_release.h"
#include "common/platform.h"
#include "version.h"
#include <winhttp.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <objbase.h>
#include <fstream>
#include <array>
#include <functional>
#include <iomanip>
#include <sstream>
namespace gauge {
namespace {
struct Internet { HINTERNET value{}; ~Internet() { if (value) WinHttpCloseHandle(value); } };
void network(bool result) { if (!result) throw std::runtime_error("更新网络请求失败：" + error_text(GetLastError())); }
void transfer(const std::string& address, uint64_t maximum, const std::function<void(const char*, DWORD)>& sink, std::stop_token stop) {
    auto url = wide(address); URL_COMPONENTS parts{sizeof(parts)};
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    network(WinHttpCrackUrl(url.c_str(), 0, 0, &parts));
    if (parts.nScheme != INTERNET_SCHEME_HTTPS) throw std::runtime_error("更新只允许 HTTPS");
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    const std::wstring path = std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength) + (parts.dwExtraInfoLength ? std::wstring(parts.lpszExtraInfo, parts.dwExtraInfoLength) : std::wstring{});
    Internet session{WinHttpOpen(L"GameGauge/" L"1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0)}; network(session.value != nullptr);
    WinHttpSetTimeouts(session.value, 5000, 5000, 15000, 15000);
    Internet connection{WinHttpConnect(session.value, host.c_str(), parts.nPort, 0)}; network(connection.value != nullptr);
    Internet request{WinHttpOpenRequest(connection.value, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)}; network(request.value != nullptr);
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    network(WinHttpSetOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect)));
    network(WinHttpSendRequest(request.value, L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n", static_cast<DWORD>(-1), nullptr, 0, 0, 0));
    network(WinHttpReceiveResponse(request.value, nullptr));
    DWORD status{}, bytes = sizeof(status);
    network(WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &bytes, nullptr));
    if (status != 200) throw std::runtime_error(status == 404 ? "尚无公开稳定版，或仓库尚未公开" : "GitHub 返回 HTTP " + std::to_string(status));
    std::array<char, 65536> buffer{}; uint64_t total{};
    for (;;) {
        if (stop.stop_requested()) throw std::runtime_error("更新已取消");
        DWORD read{}; network(WinHttpReadData(request.value, buffer.data(), static_cast<DWORD>(buffer.size()), &read));
        if (!read) break;
        total += read; if (total > maximum) throw std::runtime_error("更新响应超过大小限制");
        sink(buffer.data(), read);
    }
}
std::string sha256(const std::filesystem::path& path) {
    BCRYPT_ALG_HANDLE algorithm{}; BCRYPT_HASH_HANDLE hash{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) throw std::runtime_error("无法初始化校验");
    try {
        if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0) throw std::runtime_error("无法创建校验");
        std::ifstream input(path, std::ios::binary); if (!input) throw std::runtime_error("无法读取更新包");
        std::array<char, 65536> bytes{};
        while (input.read(bytes.data(), bytes.size()) || input.gcount())
            if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(input.gcount()), 0) < 0) throw std::runtime_error("更新校验失败");
        if (!input.eof()) throw std::runtime_error("更新包读取失败");
        std::array<unsigned char, 32> digest{};
        if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) throw std::runtime_error("更新校验失败");
        BCryptDestroyHash(hash); hash = nullptr; BCryptCloseAlgorithmProvider(algorithm, 0); algorithm = nullptr;
        std::ostringstream output; output << std::hex << std::setfill('0');
        for (auto byte : digest) output << std::setw(2) << static_cast<unsigned>(byte);
        return output.str();
    } catch (...) { if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); throw; }
}
}
Updater::Updater() : status_({{"state", "idle"}, {"message", "尚未检查更新"}}), worker_([this](std::stop_token stop) { run(stop); }) {}
Updater::~Updater() { worker_.request_stop(); wake_.notify_all(); worker_.join(); if (installer_) CloseHandle(installer_); }
Json Updater::status() const {
    std::lock_guard lock(mutex_); auto result = status_;
    result["current"] = GAMEGAUGE_VERSION; result["repository"] = GAMEGAUGE_REPOSITORY; return result;
}
void Updater::publish(Json value) { std::lock_guard lock(mutex_); status_ = std::move(value); }
void Updater::configure(bool check, bool update) {
    std::lock_guard lock(mutex_);
    if ((check && !automatic_check_) || (update && !automatic_update_)) check_ = true;
    if (update && !automatic_update_) deferred_version_.clear();
    automatic_check_ = check; automatic_update_ = update; wake_.notify_all();
}
void Updater::check() { std::lock_guard lock(mutex_); check_ = true; wake_.notify_all(); }
void Updater::download() { std::lock_guard lock(mutex_); download_ = true; wake_.notify_all(); }
void Updater::idle(bool game_running) { std::lock_guard lock(mutex_); game_running_ = game_running; }
void Updater::install() {
    auto release = status();
    if (release.value("state", "") != "ready") return;
    { std::lock_guard lock(mutex_); if (status_.value("state", "") != "ready") return;
      if (game_running_) { status_["message"] = "请退出游戏后安装更新"; return; } status_["state"] = "installing"; status_["message"] = "等待 Windows 安装确认"; }
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    struct ComScope { HRESULT result; ~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); } } com_scope{com};
    try {
        const auto path = data_dir() / L"updates" / wide(release.at("name").get<std::string>());
        if (sha256(path) != release.at("sha256").get<std::string>()) throw std::runtime_error("更新文件已变化，请重新下载");
        SHELLEXECUTEINFOW launch{sizeof(launch)}; launch.fMask = SEE_MASK_NOCLOSEPROCESS; launch.lpVerb = L"runas"; launch.lpFile = path.c_str(); launch.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&launch)) throw std::runtime_error(GetLastError() == ERROR_CANCELLED ? "已取消安装，可手动重试" : "无法启动更新安装器");
        std::lock_guard lock(mutex_); installer_ = launch.hProcess;
    } catch (const std::exception& error) { release["state"] = "ready"; release["message"] = error.what(); publish(release); std::lock_guard lock(mutex_); deferred_version_ = release.value("version", std::string{}); }
}
void Updater::run(std::stop_token stop) {
    while (!stop.stop_requested()) {
        bool check{}, download{}, automatic{}, install_ready{};
        { std::unique_lock lock(mutex_); wake_.wait_for(lock, std::chrono::seconds(1));
          if (stop.stop_requested()) break;
          if (installer_ && WaitForSingleObject(installer_, 0) == WAIT_OBJECT_0) {
              CloseHandle(installer_); installer_ = nullptr;
              status_["state"] = "ready"; status_["message"] = "安装器已关闭，可重新安装"; deferred_version_ = status_.value("version", std::string{});
          }
          const auto state = status_.value("state", "");
          check = check_ || (automatic_check_ && GetTickCount64() >= next_check_ && state != "ready" && state != "installing"); check_ = false;
          download = download_; download_ = false; automatic = automatic_update_;
          install_ready = automatic && !game_running_ && state == "ready" && status_.value("version", std::string{}) != deferred_version_;
          if (check) next_check_ = GetTickCount64() + 6ull * 60 * 60 * 1000;
        }
        if (install_ready) { install(); continue; }
        if (!check && !download) continue;
        try {
            if (std::string(GAMEGAUGE_REPOSITORY).empty()) { publish({{"state", "unconfigured"}, {"message", "本地开发版本未配置发布仓库"}}); continue; }
            Json release = status();
            if (check) {
                publish({{"state", "checking"}, {"message", "正在检查更新"}});
                std::string body;
                transfer("https://api.github.com/repos/" + std::string(GAMEGAUGE_REPOSITORY) + "/releases/latest", 2 * 1024 * 1024,
                    [&](const char* data, DWORD bytes) { body.append(data, bytes); }, stop);
                release = select_update(Json::parse(body), GAMEGAUGE_REPOSITORY, GAMEGAUGE_VERSION); publish(release);
            }
            if ((download || automatic) && release.value("state", "") == "available") {
                auto progress = release; progress["state"] = "downloading"; progress["message"] = "正在下载更新"; publish(progress);
                const auto directory = data_dir() / L"updates"; std::filesystem::create_directories(directory);
                const auto path = directory / wide(release.at("name").get<std::string>()); auto temporary = path; temporary += L".partial";
                try {
                    std::ofstream output(temporary, std::ios::binary | std::ios::trunc); if (!output) throw std::runtime_error("无法保存更新包");
                    transfer(release.at("url").get<std::string>(), release.at("size").get<uint64_t>(), [&](const char* data, DWORD bytes) { output.write(data, bytes); if (!output) throw std::runtime_error("更新包写入失败"); }, stop);
                    output.close();
                    if (std::filesystem::file_size(temporary) != release.at("size").get<uint64_t>() || sha256(temporary) != release.at("sha256").get<std::string>()) throw std::runtime_error("更新包校验失败，未安装");
                    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("无法完成更新下载");
                } catch (...) { std::error_code ignored; std::filesystem::remove(temporary, ignored); throw; }
                release["state"] = "ready"; release["message"] = "下载完成，游戏退出后可安装"; publish(release);
            }
        } catch (const std::exception& error) { publish({{"state", "error"}, {"message", error.what()}}); }
    }
}
}
