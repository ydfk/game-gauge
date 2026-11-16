#include "dav.h"
#include <wincred.h>
#include <wincrypt.h>
#include <objidl.h>
#include <xmllite.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <charconv>
#include <limits>
#include <utility>

namespace gauge::saves {
namespace {
constexpr auto max_manifest = 64ull * 1024;
constexpr auto max_index = 8ull * 1024 * 1024;
constexpr std::string_view propfind_body = R"(<?xml version="1.0"?><propfind xmlns="DAV:"><prop><resourcetype/></prop></propfind>)";
using Clock = std::chrono::steady_clock;

struct Endpoint {
    std::wstring url, origin, host, path;
    INTERNET_PORT port{};
    bool secure{}, loopback{};
};
Endpoint endpoint(std::string_view address) {
    require(address.size() <= 4096 && address.find_first_of("?#\\") == std::string_view::npos &&
        std::none_of(address.begin(), address.end(), [](unsigned char c) { return c < 32 || c == 127; }), "WebDAV URL 无效。");
    auto input = wide(address);
    URL_COMPONENTS fields{sizeof(fields)};
    fields.dwHostNameLength = fields.dwUrlPathLength = fields.dwUserNameLength = fields.dwPasswordLength = DWORD(-1);
    require(WinHttpCrackUrl(input.c_str(), static_cast<DWORD>(input.size()), 0, &fields) != FALSE &&
        fields.dwHostNameLength && !fields.dwUserNameLength && !fields.dwPasswordLength,
        "请输入有效的 WebDAV URL，地址中不能包含凭据。");
    auto host = wide(ascii_lower(utf8(std::wstring_view(fields.lpszHostName, fields.dwHostNameLength))));
    const bool secure = fields.nScheme == INTERNET_SCHEME_HTTPS;
    const bool loopback = host == L"localhost" || host == L"127.0.0.1" || host == L"[::1]" || host == L"::1";
    require(secure || (fields.nScheme == INTERNET_SCHEME_HTTP && loopback), "WebDAV 需要 HTTPS，仅本机回环可以用 HTTP。");
    const auto port = fields.nPort;
    const bool default_port = port == (secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT);
    auto origin = std::wstring(secure ? L"https://" : L"http://") + (host == L"::1" ? L"[::1]" : host);
    if (!default_port) origin += L":" + std::to_wstring(port);
    std::wstring path;
    if (fields.dwUrlPathLength) path.assign(fields.lpszUrlPath, fields.dwUrlPathLength);
    if (!path.ends_with(L'/')) path += L'/';
    return {origin + path, std::move(origin), std::move(host), std::move(path), port, secure, loopback};
}
void http_check(BOOL ok, const char* operation) {
    if (ok) return;
    const auto error = GetLastError();
    const auto reason = error == ERROR_WINHTTP_TIMEOUT ? "请求超时" :
        error == ERROR_WINHTTP_SECURE_FAILURE ? "证书验证失败" : "通信失败";
    throw std::runtime_error(std::string("WebDAV ") + operation + "：" + reason + " (" + std::to_string(error) + ")");
}
void accept(DWORD status, std::initializer_list<DWORD> expected, const char* method) {
    if (std::find(expected.begin(), expected.end(), status) == expected.end())
        throw std::runtime_error(std::string("WebDAV ") + method + " 返回 HTTP " + std::to_string(status));
}
struct Internet {
    HINTERNET handle{};
    explicit Internet(HINTERNET value = nullptr) : handle(value) {}
    ~Internet() { if (handle) WinHttpCloseHandle(handle); }
    Internet(Internet&& other) noexcept : handle(std::exchange(other.handle, nullptr)) {}
    Internet(const Internet&) = delete;
};
struct Sensitive {
    std::wstring value;
    ~Sensitive() { if (!value.empty()) SecureZeroMemory(value.data(), value.size() * sizeof(wchar_t)); }
};
void basic_auth(HINTERNET request, const DavConfig& config) {
    const auto target = wide(config.credential_target());
    if (target.empty()) return;
    PCREDENTIALW credential{};
    std::string input = config.user + ":";
    if (CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &credential)) {
        std::unique_ptr<CREDENTIALW, decltype(&CredFree)> stored(credential, CredFree);
        const auto size = stored->CredentialBlobSize;
        require(size <= CRED_MAX_CREDENTIAL_BLOB_SIZE && size % sizeof(wchar_t) == 0, "Windows WebDAV 凭据损坏。");
        if (size) {
            Sensitive secret;
            secret.value.resize(size / sizeof(wchar_t));
            memcpy(secret.value.data(), stored->CredentialBlob, size);
            SecureZeroMemory(stored->CredentialBlob, size);
            auto utf = utf8(secret.value);
            input += utf;
            if (!utf.empty()) SecureZeroMemory(utf.data(), utf.size());
        }
    } else require(GetLastError() == ERROR_NOT_FOUND, "无法读取 Windows WebDAV 凭据。");
    constexpr DWORD flags = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;
    DWORD size{};
    const auto* bytes = reinterpret_cast<const BYTE*>(input.data());
    const auto length = static_cast<DWORD>(input.size());
    const auto valid = CryptBinaryToStringW(bytes, length, flags, nullptr, &size);
    Sensitive encoded, header;
    encoded.value.resize(size);
    const auto encoded_ok = valid && size && CryptBinaryToStringW(bytes, length, flags, encoded.value.data(), &size);
    SecureZeroMemory(input.data(), input.size());
    require(encoded_ok, "无法编码 WebDAV 凭据。");
    header.value = L"Authorization: Basic ";
    header.value += encoded.value.c_str();
    http_check(WinHttpAddRequestHeaders(request, header.value.c_str(), DWORD(-1), WINHTTP_ADDREQ_FLAG_ADD), "身份验证");
}
uint64_t response_length(HINTERNET request) {
    wchar_t header[64]{};
    DWORD size = sizeof(header);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, header, &size, nullptr)) return 0;
    const auto text = utf8(header);
    uint64_t result{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
    return error == std::errc{} && end == text.data() + text.size() ? result : 0;
}
struct Response {
    Internet request;
    DWORD status{};
    Clock::time_point deadline;
    void read(uint64_t limit, const Sink& consumer) const {
        std::array<unsigned char, 65536> buffer{};
        uint64_t read{};
        for (DWORD count{};;) {
            require(Clock::now() < deadline, "WebDAV 请求超时。");
            http_check(WinHttpReadData(request.handle, buffer.data(), static_cast<DWORD>(buffer.size()), &count), "读取响应");
            if (!count) break;
            require(count <= limit - read, "WebDAV 响应超过大小限制。");
            read += count;
            consumer(std::span(buffer).first(count));
        }
    }
    std::string text(uint64_t limit) const {
        std::string result;
        read(limit, [&](auto chunk) { result.append(reinterpret_cast<const char*>(chunk.data()), chunk.size()); });
        return result;
    }
};
std::wstring remote_path(std::string_view game, std::string_view profile = {}, std::string_view name = {}) {
    auto path = L"SaveKeep/" + wide(game) + L"/";
    if (!profile.empty()) path += wide(profile) + L"/";
    path += wide(name);
    return path;
}
std::vector<std::string> href_names(const std::string& body) {
    Microsoft::WRL::ComPtr<IXmlReader> reader;
    require(SUCCEEDED(CreateXmlReader(__uuidof(IXmlReader), reinterpret_cast<void**>(reader.GetAddressOf()), nullptr)),
        "无法初始化 WebDAV XML 读取器。");
    require(SUCCEEDED(reader->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit)), "无法限制 WebDAV XML。");
    Microsoft::WRL::ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(body.data()), static_cast<UINT>(body.size())));
    require(stream && SUCCEEDED(reader->SetInput(stream.Get())), "无法读取 WebDAV XML。");
    std::vector<std::string> result;
    XmlNodeType node{};
    HRESULT status{};
    bool href{};
    std::wstring value;
    while ((status = reader->Read(&node)) == S_OK) {
        if (node == XmlNodeType_Element) {
            const wchar_t *name{}, *uri{};
            href = SUCCEEDED(reader->GetLocalName(&name, nullptr)) && SUCCEEDED(reader->GetNamespaceUri(&uri, nullptr)) &&
                std::wstring_view(name) == L"href" && std::wstring_view(uri) == L"DAV:" && !reader->IsEmptyElement();
            value.clear();
        } else if (href && node == XmlNodeType_EndElement) {
            while (value.ends_with(L'/')) value.pop_back();
            const auto last = value.find_last_of(L'/');
            result.push_back(utf8(value.substr(last == std::wstring::npos ? 0 : last + 1)));
            href = false;
        } else if (href && (node == XmlNodeType_Text || node == XmlNodeType_CDATA)) {
            const wchar_t* text{};
            UINT length{};
            if (SUCCEEDED(reader->GetValue(&text, &length))) value.append(text, length);
        }
    }
    require(status == S_FALSE, "WebDAV 目录响应不是有效的 XML。");
    return result;
}
}

DavConfig DavConfig::make(std::string url, std::string user, bool automatic) {
    url = trim(std::move(url));
    if (url.empty()) return {};
    user = trim(std::move(user));
    require(user.size() <= 512 && std::none_of(user.begin(), user.end(), [](unsigned char c) {
        return c == ':' || c < 32 || c == 127;
    }), "WebDAV 用户名无效。");
    return {utf8(endpoint(url).url), std::move(user), automatic};
}
DavConfig DavConfig::parse(const Json& data) {
    return make(required_string(data, "url"), required_string(data, "user"), data.value("automatic", false));
}
std::string DavConfig::destination() const { return sha256(url + '\0' + user); }
std::string DavConfig::credential_target() const {
    return url.empty() || user.empty() ? std::string{} : "GameGauge/Saves/WebDAV/" + destination();
}
Json DavConfig::json() const { return {{"url", url}, {"user", user}, {"automatic", automatic}}; }

struct Remote::Connection {
    Endpoint address;
    DavConfig config;
    Internet session, server;
    explicit Connection(const DavConfig& cfg) : address(endpoint(cfg.url)), config(cfg),
        session(WinHttpOpen(L"GameGauge.Saves", address.loopback ? WINHTTP_ACCESS_TYPE_NO_PROXY : WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)) {
        require(session.handle != nullptr, "无法初始化 WebDAV 客户端。");
        http_check(WinHttpSetTimeouts(session.handle, 5000, 5000, 60000, 60000), "设置超时");
        server.handle = WinHttpConnect(session.handle, address.host.c_str(), address.port, 0);
        require(server.handle != nullptr, "无法打开 WebDAV 连接。");
    }
    Response send(const wchar_t* method, const std::wstring& resource, const std::wstring& headers = {},
        uint64_t size = 0, const Receive& body = {}) const {
        const auto deadline = Clock::now() + std::chrono::seconds(60);
        const auto path = address.path + resource;
        Internet request(WinHttpOpenRequest(server.handle, method, path.c_str(), nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES, address.secure ? WINHTTP_FLAG_SECURE : 0));
        require(request.handle != nullptr, "无法创建 WebDAV 请求。");
        DWORD disabled = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES;
        http_check(WinHttpSetOption(request.handle, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled)), "限制重定向");
        DWORD no_auto_logon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
        http_check(WinHttpSetOption(request.handle, WINHTTP_OPTION_AUTOLOGON_POLICY, &no_auto_logon, sizeof(no_auto_logon)), "限制自动登录");
        basic_auth(request.handle, config);
        auto request_headers = headers;
        if (size > MAXDWORD) request_headers += L"Content-Length: " + std::to_wstring(size) + L"\r\n";
        http_check(WinHttpSendRequest(request.handle, request_headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : request_headers.c_str(),
            request_headers.empty() ? 0 : DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0,
            size > MAXDWORD ? WINHTTP_IGNORE_REQUEST_TOTAL_LENGTH : static_cast<DWORD>(size), 0), "发出请求");
        uint64_t written{};
        if (body) body([&](auto chunk) {
            require(Clock::now() < deadline && chunk.size() <= size - written, "WebDAV 上传超时或大小不一致。");
            DWORD count{};
            http_check(WinHttpWriteData(request.handle, chunk.data(), static_cast<DWORD>(chunk.size()), &count), "上传数据");
            require(count == chunk.size(), "WebDAV 上传中断。");
            written += count;
        });
        require(written == size, "WebDAV 上传大小不一致。");
        http_check(WinHttpReceiveResponse(request.handle, nullptr), "接收响应");
        DWORD status{}, length = sizeof(status);
        http_check(WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &length, nullptr), "读取 HTTP 状态");
        return {std::move(request), status, deadline};
    }
    Response propfind(const std::wstring& resource, const wchar_t* depth) const {
        return send(L"PROPFIND", resource, std::wstring(L"Depth: ") + depth + L"\r\nContent-Type: application/xml; charset=utf-8\r\n",
            propfind_body.size(), [](const Sink& sink) {
                sink({reinterpret_cast<const unsigned char*>(propfind_body.data()), propfind_body.size()});
            });
    }
    void ensure_collection(const std::wstring& resource) const {
        const auto created = send(L"MKCOL", resource).status;
        if (created == 405) accept(propfind(resource, L"0").status, {207}, "PROPFIND");
        else accept(created, {201}, "MKCOL");
    }
    bool publish(const std::wstring& folder, const std::wstring& name, uint64_t size, const Receive& body, bool overwrite) const {
        const auto pending = folder + wide(new_request_id()) + L".part";
        auto cleanup = [&] {
            try { send(L"DELETE", pending); }
            catch (const std::exception&) { /* 仅尝试删除本次上传的临时对象。 */ }
        };
        try {
            accept(send(L"PUT", pending, {}, size, body).status, {200, 201, 204}, "PUT");
            const auto headers = L"Destination: " + address.url + folder + name +
                L"\r\nOverwrite: " + (overwrite ? L"T\r\n" : L"F\r\n");
            const auto moved = send(L"MOVE", pending, headers).status;
            if (!overwrite && moved == 412) { cleanup(); return false; }
            accept(moved, {200, 201, 204}, "MOVE");
            return true;
        } catch (...) { cleanup(); throw; }
    }
};

Remote::Remote(const DavConfig& config) : connection_(std::make_unique<Connection>(config)) {}
Remote::~Remote() = default;
void Remote::test() const { accept(connection_->propfind({}, L"0").status, {207}, "PROPFIND"); }
std::vector<std::string> Remote::children(const std::wstring& path) const {
    const auto response = connection_->propfind(path, L"1");
    if (response.status == 404) return {};
    accept(response.status, {207}, "PROPFIND");
    return href_names(response.text(max_index));
}
std::vector<Json> Remote::list_profile(std::string_view game, std::string_view profile) const {
    require(valid_identity(game, profile), "WebDAV 档案身份无效。");
    std::set<std::string> ids;
    const auto folder = remote_path(game, profile);
    for (const auto& name : children(folder)) {
        if (name.ends_with(".json") && uuid(std::string_view(name).substr(0, name.size() - 5))) ids.insert(name.substr(0, name.size() - 5));
    }
    std::vector<Json> result;
    result.reserve(ids.size());
    for (const auto& id : ids) {
        const auto response = connection_->send(L"GET", folder + wide(id) + L".json");
        accept(response.status, {200}, "GET");
        auto record = Json::parse(response.text(max_manifest));
        validate_record(record, game, profile, id);
        result.push_back(std::move(record));
    }
    return result;
}
std::vector<Json> Remote::list(std::string_view game, std::string_view profile) const {
    std::vector<Json> result;
    if (!profile.empty()) result = list_profile(game, profile);
    else {
        require(game == pbz_id, "WebDAV 游戏身份无效。");
        std::set<std::string> accounts;
        for (const auto& name : children(remote_path(game))) if (steam_profile(name)) accounts.insert(name);
        for (const auto& account : accounts) {
            auto rows = list_profile(game, account);
            result.insert(result.end(), std::make_move_iterator(rows.begin()), std::make_move_iterator(rows.end()));
        }
    }
    sort_records(result);
    return result;
}
void Remote::upload(const Json& record, const fs::path& archive, bool metadata_only) const {
    const auto game = required_string(record, "game_id"), profile = required_string(record, "profile"), id = required_string(record, "id");
    validate_record(record, game, profile, id);
    connection_->ensure_collection(L"SaveKeep/");
    connection_->ensure_collection(remote_path(game));
    const auto folder = remote_path(game, profile);
    connection_->ensure_collection(folder);
    const auto zip = wide(id) + L".zip";
    const auto bytes = required_number(record, "bytes");
    auto verify_remote = [&](const Response& remote) {
        accept(remote.status, {200}, "GET");
        Sha256 hash;
        uint64_t count{};
        remote.read(bytes, [&](auto chunk) { hash.add(chunk); count += chunk.size(); });
        require(count == bytes && hex(hash.finish()) == required_string(record, "sha256"), "云端同 ID 的压缩包与本地不同，已停止覆盖。");
    };
    const auto document = record.dump();
    {
        const auto remote = connection_->send(metadata_only ? L"HEAD" : L"GET", folder + zip);
        if (remote.status == 404) {
            verify_archive(archive, record);
            if (!connection_->publish(folder, zip, bytes, [&](const Sink& sink) {
                require(hex(read_file(archive, bytes, sink)) == required_string(record, "sha256"), "上传期间本地 ZIP 发生变化。");
            }, false))
                verify_remote(connection_->send(L"GET", folder + zip));
        } else if (metadata_only) {
            accept(remote.status, {200}, "HEAD");
            if (response_length(remote.request.handle) != bytes) verify_remote(connection_->send(L"GET", folder + zip));
        } else verify_remote(remote);
    }
    connection_->publish(folder, wide(id) + L".json", document.size(), [&](const Sink& sink) {
        sink({reinterpret_cast<const unsigned char*>(document.data()), document.size()});
    }, true);
}
void Remote::download(const Library& library, const Json& record) const {
    const auto id = required_string(record, "id");
    validate_record(record, library.game, library.profile, id);
    import_record(library.root, record, [&](const Sink& sink) {
        const auto response = connection_->send(L"GET", remote_path(library.game, library.profile, id + ".zip"));
        accept(response.status, {200}, "GET");
        response.read(required_number(record, "bytes"), sink);
    });
}
}
