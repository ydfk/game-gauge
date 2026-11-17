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
#include <utility>
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
Json check_release(std::stop_token stop) {
    const std::string repository = GAMEGAUGE_REPOSITORY;
    if (repository.empty()) return {{"state", "unconfigured"}, {"message", "本地开发版本未配置发布仓库"}};
    std::string body;
    transfer("https://api.github.com/repos/" + repository + "/releases/latest", 2 * 1024 * 1024,
        [&](const char* bytes, DWORD count) { body.append(bytes, count); }, stop);
    return select_update(Json::parse(body), repository, GAMEGAUGE_VERSION);
}
std::filesystem::path update_path(const Json& release) {
    return data_dir() / L"updates" / wide(release.at("name").get<std::string>());
}
bool valid_package(const std::filesystem::path& path, const Json& release) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    return !error && size == release.at("size").get<uint64_t>() && sha256(path) == release.at("sha256").get<std::string>();
}
void download_release(const Json& release, std::stop_token stop) {
    const auto path = update_path(release);
    std::filesystem::create_directories(path.parent_path());
    auto temporary = path;
    temporary += L".partial";
    try {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("无法保存更新包");
        transfer(release.at("url").get<std::string>(), release.at("size").get<uint64_t>(), [&](const char* bytes, DWORD count) {
            output.write(bytes, count);
            if (!output) throw std::runtime_error("更新包写入失败");
        }, stop);
        output.close();
        if (!output || !valid_package(temporary, release)) throw std::runtime_error("更新包校验失败，未安装");
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("无法完成更新下载");
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}
void verify_release(const Json& release) {
    if (!valid_package(update_path(release), release)) throw std::runtime_error("更新包缺失或已变化，请重试更新");
}
HANDLE launch_installer(const Json& release) {
    const auto path = update_path(release);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    struct ComScope { HRESULT result; ~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); } } com_scope{com};
    SHELLEXECUTEINFOW launch{sizeof(launch)};
    launch.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    launch.lpVerb = L"runas";
    launch.lpFile = path.c_str();
    launch.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&launch)) throw std::runtime_error(GetLastError() == ERROR_CANCELLED ?
        "已取消安装，可点击一键更新重试" : "无法启动更新安装器，可点击一键更新重试");
    return launch.hProcess;
}
}
Updater::Updater() : Updater({check_release, download_release, verify_release, launch_installer}) {}
Updater::Updater(Operations operations) : operations_(std::move(operations)),
    status_({{"state", "idle"}, {"message", "尚未检查更新"}}), worker_([this](std::stop_token stop) { run(stop); }) {}
Updater::~Updater() { worker_.request_stop(); wake_.notify_all(); worker_.join(); if (installer_) CloseHandle(installer_); }
Json Updater::status() const {
    std::lock_guard lock(mutex_); auto result = status_;
    result["current"] = GAMEGAUGE_VERSION; result["repository"] = GAMEGAUGE_REPOSITORY; return result;
}
void Updater::configure(bool check, bool update) {
    std::lock_guard lock(mutex_);
    if (request_ == Request::none && ((check && !automatic_check_) || (update && !automatic_update_)))
        request_ = Request::check;
    if (update && !automatic_update_) deferred_version_.clear();
    automatic_check_ = check; automatic_update_ = update; wake_.notify_all();
}
void Updater::check() { request(Request::check); }
void Updater::download() { request(Request::download); }
void Updater::install() { request(Request::install); }
void Updater::update() { request(Request::update); }

void Updater::request(Request request) {
    std::lock_guard lock(mutex_);
    const auto state = status_.value("state", std::string{});
    if (state == "installing" || request_ == Request::update) return;
    if (request == Request::install && state != "ready" && state != "waiting") return;
    request_ = request;
    wake_.notify_one();
}

void Updater::idle(bool game_running) {
    std::lock_guard lock(mutex_);
    if (game_running_ == game_running) return;
    game_running_ = game_running;
    wake_.notify_one();
}

Updater::Step Updater::next_step() {
    if (installer_ && WaitForSingleObject(installer_, 0) == WAIT_OBJECT_0) {
        CloseHandle(installer_);
        installer_ = nullptr;
        status_["state"] = "ready";
        status_["message"] = "安装器已关闭，可点击一键更新重试";
        deferred_version_ = status_.value("version", std::string{});
    }
    const auto state = status_.value("state", std::string{});
    if (state == "installing") return Step::none;
    const bool ready = state == "ready" || state == "waiting";
    const bool manual = request_ == Request::update;
    const bool automatic_install = automatic_update_ && status_.value("version", std::string{}) != deferred_version_;
    if (ready && (manual || request_ == Request::install || automatic_install)) {
        if (game_running_) {
            if (request_ == Request::install) {
                request_ = Request::none;
                status_["message"] = "请退出游戏后安装更新";
            } else {
                status_["state"] = "waiting";
                status_["message"] = "已下载并校验，退出游戏后安装";
            }
            return Step::none;
        }
        status_["state"] = "installing";
        status_["message"] = "正在校验安装包";
        return Step::install;
    }
    if (state == "waiting") {
        status_["state"] = "ready";
        status_["message"] = "已下载并校验，点击一键更新安装";
    }
    const auto now = GetTickCount64();
    if (request_ == Request::check || (manual && state != "available") || (automatic_check_ && !ready && now >= next_check_)) {
        if (request_ == Request::check) request_ = Request::none;
        next_check_ = now + 6ull * 60 * 60 * 1000;
        status_ = {{"state", "checking"}, {"message", "正在检查更新"}};
        return Step::check;
    }
    if (state == "available" && (manual || request_ == Request::download || automatic_update_)) {
        if (request_ == Request::download) request_ = Request::none;
        status_["state"] = "downloading";
        status_["message"] = "正在下载更新";
        return Step::download;
    }
    request_ = Request::none;
    return Step::none;
}

void Updater::install_ready(const Json& release, std::stop_token stop) {
    operations_.verify(release);
    {
        std::lock_guard lock(mutex_);
        if (stop.stop_requested()) return;
        if (game_running_ || (!automatic_update_ && request_ != Request::install && request_ != Request::update)) {
            status_["state"] = "ready";
            status_["message"] = "已下载并校验，点击一键更新安装";
            return;
        }
        request_ = Request::none;
        status_["message"] = "请在 Windows 提示中确认安装";
    }
    try {
        const auto process = operations_.launch(release);
        if (!process) throw std::runtime_error("未能启动安装器，可点击一键更新重试");
        std::lock_guard lock(mutex_);
        installer_ = process;
        status_["message"] = "已启动安装器";
    } catch (const std::exception& error) {
        std::lock_guard lock(mutex_);
        status_["state"] = "ready";
        status_["message"] = error.what();
        deferred_version_ = release.value("version", std::string{});
    }
}
void Updater::run(std::stop_token stop) {
    while (!stop.stop_requested()) {
        std::unique_lock lock(mutex_);
        const auto step = next_step();
        if (step == Step::none) {
            wake_.wait_for(lock, std::chrono::seconds(1));
            continue;
        }
        auto release = status_;
        lock.unlock();
        try {
            if (step == Step::install) {
                install_ready(release, stop);
                continue;
            }
            if (step == Step::check) {
                release = operations_.check(stop);
            } else {
                operations_.download(release, stop);
                release["state"] = "ready";
                release["message"] = "已下载并校验，点击一键更新安装";
            }
        } catch (const std::exception& error) {
            release = {{"state", "error"}, {"message", error.what()}};
        }
        lock.lock();
        const auto result = release.value("state", std::string{});
        if (request_ == Request::update && result != "available" && result != "ready") request_ = Request::none;
        status_ = std::move(release);
    }
}
}
