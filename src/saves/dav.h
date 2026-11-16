#pragma once
#include "archive.h"
#include <winhttp.h>
#include <chrono>
#include <map>
#include <memory>

namespace gauge::saves {
struct DavConfig {
    std::string url, user;
    bool automatic{};
    static DavConfig parse(const Json& data);
    static DavConfig make(std::string url, std::string user, bool automatic);
    std::string destination() const;
    std::string credential_target() const;
    Json json() const;
};

class Remote {
public:
    explicit Remote(const DavConfig& config);
    ~Remote();
    void test() const;
    std::vector<Json> list(std::string_view game, std::string_view profile) const;
    void upload(const Json& record, const fs::path& archive, bool metadata_only) const;
    void download(const Library& library, const Json& record) const;
private:
    struct Connection;
    std::unique_ptr<Connection> connection_;
    std::vector<std::string> children(const std::wstring& path) const;
    std::vector<Json> list_profile(std::string_view game, std::string_view profile) const;
};

class Cloud {
public:
    explicit Cloud(const fs::path& directory);
    DavConfig config;
    void configure(DavConfig next);
    void retry();
    bool may_prune(const Library& library, const Json& record) const;
    void upload(const Library& library, const Json& record) const;
    void record_receipt(const fs::path& root, const Json& record) const;
    std::string upload_next(const std::vector<Settings>& targets);
private:
    struct Retry {
        unsigned attempts{};
        std::chrono::steady_clock::time_point next{};
        std::string message;
    };
    fs::path path_;
    std::map<std::string, Retry> retries_;
    std::pair<bool, bool> receipt(const fs::path& root, const Json& record) const;
    void upload_at(const fs::path& root, const Json& record) const;
};
}
