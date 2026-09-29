#pragma once
#include "config.h"
#include <array>
namespace gauge {
std::array<unsigned, 3> release_version(const std::string& value);
// 仅接受固定仓库的稳定版、指定命名的安装器及 GitHub 提供的摘要。
Json select_update(const Json& release, const std::string& repository, const std::string& current);
}
