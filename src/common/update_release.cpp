#include "update_release.h"
#include <regex>
#include <stdexcept>
namespace gauge {
std::array<unsigned, 3> release_version(const std::string& value) {
    static const std::regex pattern(R"(^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$)");
    std::smatch match;
    if (!std::regex_match(value, match, pattern)) throw std::runtime_error("发布版本号格式不正确");
    std::array<unsigned, 3> version{};
    for (size_t i = 0; i < version.size(); ++i) {
        const auto part = std::stoull(match[i + 1]);
        if (part > 65535) throw std::runtime_error("发布版本号超出范围");
        version[i] = static_cast<unsigned>(part);
    }
    return version;
}
Json select_update(const Json& release, const std::string& repository, const std::string& current) {
    if (release.value("draft", true) || release.value("prerelease", true)) throw std::runtime_error("更新源不是已发布稳定版");
    const auto tag = release.at("tag_name").get<std::string>();
    if (!tag.starts_with('v')) throw std::runtime_error("发布标签必须以 v 开头");
    const auto version = tag.substr(1);
    if (release_version(version) <= release_version(current)) return {{"state", "current"}, {"message", "已是最新版本"}};
    const auto name = "GameGauge-" + version + "-Setup.exe";
    const auto url = "https://github.com/" + repository + "/releases/download/" + tag + "/" + name;
    for (const auto& asset : release.at("assets")) if (asset.value("name", std::string{}) == name) {
        const auto digest = asset.value("digest", std::string{});
        if (!std::regex_match(digest, std::regex("sha256:[0-9a-f]{64}")) || asset.value("browser_download_url", std::string{}) != url)
            throw std::runtime_error("更新包来源或 SHA-256 摘要无效");
        const auto size = asset.at("size").get<uint64_t>();
        if (!size || size > 512ull * 1024 * 1024) throw std::runtime_error("更新包大小异常");
        return {{"state", "available"}, {"message", "发现新版本 " + version}, {"version", version}, {"url", url},
            {"sha256", digest.substr(7)}, {"size", size}, {"name", name}};
    }
    throw std::runtime_error("该版本尚无完整安装包");
}
}
