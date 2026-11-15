#pragma once
#include <windows.h>
#include <optional>
#include <string>
#include <vector>

namespace gauge {
struct SaveField {
    std::wstring label, value;
    unsigned limit{260};
    bool folder{}, flag{}, multiline{};
};
std::optional<std::vector<std::wstring>> saves_form(HWND owner, const wchar_t* title, std::vector<SaveField> fields);
bool save_webdav_password(HWND owner, const std::wstring& target, const std::wstring& username);
}
