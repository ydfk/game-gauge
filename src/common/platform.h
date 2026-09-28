#pragma once
#include <windows.h>
#include <filesystem>
#include <string>

namespace gauge {
std::string utf8(std::wstring_view value);
std::wstring wide(std::string_view value);
std::filesystem::path executable_dir();
std::filesystem::path data_dir();
void set_data_dir(std::filesystem::path path);
std::wstring user_token();
std::wstring pipe_name();
std::string error_text(DWORD code);
uint64_t file_ticks(const FILETIME& time);
struct UniqueHandle {
    HANDLE value{INVALID_HANDLE_VALUE};
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle) : value(handle) {}
    ~UniqueHandle() { reset(); }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept : value(other.value) { other.value = INVALID_HANDLE_VALUE; }
    explicit operator bool() const { return value && value != INVALID_HANDLE_VALUE; }
    void reset(HANDLE handle = INVALID_HANDLE_VALUE) {
        if (*this) CloseHandle(value);
        value = handle;
    }
};
}

