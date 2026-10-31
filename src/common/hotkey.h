#pragma once
#include <cstdint>
#include <string>

namespace gauge {
struct NoticeHotkey {
    uint32_t modifiers{0x0006};
    uint32_t key{0x79};
    bool operator==(const NoticeHotkey&) const = default;
};
inline bool valid_notice_hotkey(NoticeHotkey hotkey) {
    if (!hotkey.key) return hotkey.modifiers == 0;
    if (!hotkey.modifiers || (hotkey.modifiers & ~0x0007u)) return false;
    return (hotkey.key >= 0x30 && hotkey.key <= 0x39) || (hotkey.key >= 0x41 && hotkey.key <= 0x5a) ||
        (hotkey.key >= 0x70 && hotkey.key <= 0x87 && hotkey.key != 0x7b);
}
inline std::wstring notice_hotkey_name(NoticeHotkey hotkey) {
    if (!hotkey.key) return L"已停用";
    std::wstring name;
    if (hotkey.modifiers & 0x0002) name += L"Ctrl+";
    if (hotkey.modifiers & 0x0001) name += L"Alt+";
    if (hotkey.modifiers & 0x0004) name += L"Shift+";
    if (hotkey.key >= 0x70 && hotkey.key <= 0x87) name += L"F" + std::to_wstring(hotkey.key - 0x6f);
    else name += static_cast<wchar_t>(hotkey.key);
    return name;
}
}
