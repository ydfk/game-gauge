#include "platform.h"
#include <sddl.h>
#include <shlobj.h>
#include <stdexcept>
#include <vector>

namespace gauge {
namespace { std::filesystem::path custom_data_dir; }
std::string utf8(std::wstring_view value) {
    if (value.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}
std::wstring wide(std::string_view value) {
    if (value.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (!size) return L"无效文本";
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}
std::filesystem::path executable_dir() {
    std::wstring path(32768, L'\0');
    auto count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(count);
    return std::filesystem::path(path).parent_path();
}
void set_data_dir(std::filesystem::path path) {
    auto value = path.wstring();
    const auto first = value.find_first_not_of(L" \t\r\n\"");
    const auto last = value.find_last_not_of(L" \t\r\n\"");
    custom_data_dir = first == std::wstring::npos ? std::filesystem::path{} :
        std::filesystem::path(value.substr(first, last - first + 1));
}
std::filesystem::path data_dir() {
    if (!custom_data_dir.empty()) return custom_data_dir;
    PWSTR local{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) return executable_dir() / L"data";
    std::filesystem::path path = std::filesystem::path(local) / L"GameGauge";
    CoTaskMemFree(local);
    return path;
}
std::wstring user_token() {
    UniqueHandle token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value)) throw std::runtime_error("读取当前用户失败");
    DWORD bytes{};
    GetTokenInformation(token.value, TokenUser, nullptr, 0, &bytes);
    std::vector<unsigned char> buffer(bytes);
    if (!GetTokenInformation(token.value, TokenUser, buffer.data(), bytes, &bytes)) throw std::runtime_error("读取用户 SID 失败");
    LPWSTR sid{};
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sid)) throw std::runtime_error("转换 SID 失败");
    std::wstring result(sid);
    LocalFree(sid);
    DWORD session{};
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    return result + L"-" + std::to_wstring(session);
}
std::wstring pipe_name() { return L"\\\\.\\pipe\\GameGauge-" + user_token(); }
std::string error_text(DWORD code) {
    LPWSTR message{};
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<LPWSTR>(&message), 0, nullptr);
    std::string result = message ? utf8(message) : "Windows error " + std::to_string(code);
    LocalFree(message);
    return result;
}
uint64_t file_ticks(const FILETIME& time) { return (uint64_t{time.dwHighDateTime} << 32) | time.dwLowDateTime; }
}
