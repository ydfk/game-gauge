#include "game_identity.h"
#include "platform.h"
#include <vector>
#include <algorithm>

namespace gauge {
namespace {
bool chinese(const std::wstring& text) {
    return std::any_of(text.begin(), text.end(), [](wchar_t c) { return c >= 0x3400 && c <= 0x9fff; });
}
}
std::string game_display_name(const std::string& path, const std::string& fallback, uintptr_t window) {
    // 已知名称只按完整文件名匹配，避免误把同目录工具当成游戏。
    const auto filename = std::filesystem::path(wide(path)).filename().wstring();
    if (_wcsicmp(filename.c_str(), L"CONTROLResonant.exe") == 0) return "控制：共振";
    wchar_t title[512]{};
    if (window) GetWindowTextW(reinterpret_cast<HWND>(window), title, 512);
    if (chinese(title)) return utf8(title);
    const auto executable = wide(path);
    DWORD unused{};
    const DWORD size = GetFileVersionInfoSizeW(executable.c_str(), &unused);
    std::wstring english;
    if (size && size <= 1024 * 1024) {
        std::vector<BYTE> data(size);
        if (GetFileVersionInfoW(executable.c_str(), 0, size, data.data())) {
            struct Translation { WORD language, codepage; };
            Translation* translations{}; UINT bytes{};
            if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translations), &bytes)) {
                for (UINT i = 0; i < bytes / sizeof(Translation); ++i) for (const auto field : {L"ProductName", L"FileDescription"}) {
                    wchar_t key[100]{};
                    swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\%s", translations[i].language, translations[i].codepage, field);
                    wchar_t* value{}; UINT length{};
                    if (!VerQueryValueW(data.data(), key, reinterpret_cast<void**>(&value), &length) || length <= 1) continue;
                    const std::wstring name(value, length - 1);
                    if (chinese(name)) return utf8(name);
                    if (english.empty() && name != L"Game" && name != L"game" && name != L"UE4Game" && name != L"UE5Game") english = name;
                }
            }
        }
    }
    if (!english.empty()) return utf8(english);
    if (*title) return utf8(title);
    return utf8(std::filesystem::path(wide(fallback)).stem().wstring());
}
}
