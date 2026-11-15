#include "saves_dialog.h"
#include <commdlg.h>
#include <shobjidl.h>
#include <wincred.h>
#include <wrl/client.h>
#include <array>
#include <stdexcept>

namespace gauge {
namespace {
constexpr int field_id = 100, browse_id = 200;
struct Form {
    const wchar_t* title;
    std::vector<SaveField> fields;
    int height{};
};

void pick_folder(HWND dialog, int id) {
    Microsoft::WRL::ComPtr<IFileOpenDialog> picker;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&picker)))) return;
    picker->SetOptions(FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    if (FAILED(picker->Show(dialog))) return;
    Microsoft::WRL::ComPtr<IShellItem> selected;
    PWSTR path{};
    if (SUCCEEDED(picker->GetResult(&selected)) && SUCCEEDED(selected->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        SetDlgItemTextW(dialog, id, path);
        CoTaskMemFree(path);
    }
}

INT_PTR CALLBACK procedure(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    auto form = reinterpret_cast<Form*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        form = reinterpret_cast<Form*>(lparam);
        SetWindowLongPtrW(dialog, DWLP_USER, lparam);
        SetWindowTextW(dialog, form->title);
        const auto font = SendMessageW(dialog, WM_GETFONT, 0, 0);
        const auto control = [&](const wchar_t* cls, const wchar_t* title, DWORD style, DWORD extra, int id, RECT rect) {
            MapDialogRect(dialog, &rect);
            auto window = CreateWindowExW(extra, cls, title, WS_CHILD | WS_VISIBLE | style, rect.left, rect.top,
                rect.right - rect.left, rect.bottom - rect.top, dialog, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                GetModuleHandleW(nullptr), nullptr);
            SendMessageW(window, WM_SETFONT, font, TRUE);
            return window;
        };
        int y = 10;
        for (size_t index = 0; index < form->fields.size(); ++index) {
            const auto& field = form->fields[index];
            const auto id = field_id + static_cast<int>(index);
            if (field.flag) {
                control(L"BUTTON", field.label.c_str(), WS_TABSTOP | BS_AUTOCHECKBOX, 0, id, {10, y, 300, y + 16});
                CheckDlgButton(dialog, id, field.value == L"1" ? BST_CHECKED : BST_UNCHECKED);
                y += 22;
                continue;
            }
            control(L"STATIC", field.label.c_str(), SS_LEFT, 0, -1, {10, y, 300, y + 12});
            y += 13;
            const int height = field.multiline ? 58 : 17;
            const DWORD edit_style = field.multiline ? ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL : ES_AUTOHSCROLL;
            const auto edit = control(L"EDIT", field.value.c_str(), WS_TABSTOP | edit_style, WS_EX_CLIENTEDGE, id,
                {10, y, field.folder ? 254 : 300, y + height});
            SendMessageW(edit, EM_SETLIMITTEXT, field.limit, 0);
            if (field.folder)
                control(L"BUTTON", L"浏览…", WS_TABSTOP | BS_PUSHBUTTON, 0, browse_id + static_cast<int>(index),
                    {259, y, 300, y + 17});
            y += height + 7;
        }
        control(L"BUTTON", L"确定", WS_TABSTOP | BS_DEFPUSHBUTTON, 0, IDOK, {188, y + 4, 242, y + 24});
        control(L"BUTTON", L"取消", WS_TABSTOP | BS_PUSHBUTTON, 0, IDCANCEL, {246, y + 4, 300, y + 24});
        SendMessageW(dialog, DM_SETDEFID, IDOK, 0);
        return TRUE;
    }
    if (message != WM_COMMAND || !form) return FALSE;
    const int id = LOWORD(wparam);
    if (id == IDCANCEL) { EndDialog(dialog, IDCANCEL); return TRUE; }
    if (id == IDOK) {
        for (size_t index = 0; index < form->fields.size(); ++index) {
            auto& field = form->fields[index];
            const auto edit = field_id + static_cast<int>(index);
            if (field.flag) {
                field.value = IsDlgButtonChecked(dialog, edit) == BST_CHECKED ? L"1" : L"0";
                continue;
            }
            field.value.resize(field.limit + 1);
            const int size = GetDlgItemTextW(dialog, edit, field.value.data(), static_cast<int>(field.value.size()));
            field.value.resize(size);
        }
        EndDialog(dialog, IDOK);
        return TRUE;
    }
    if (id >= browse_id && id < browse_id + static_cast<int>(form->fields.size())) {
        pick_folder(dialog, field_id + id - browse_id);
        return TRUE;
    }
    return FALSE;
}
}

std::optional<std::vector<std::wstring>> saves_form(HWND owner, const wchar_t* title, std::vector<SaveField> fields) {
    Form form{title, std::move(fields), 42};
    for (const auto& field : form.fields) form.height += field.flag ? 22 : field.multiline ? 78 : 37;
    alignas(DWORD) std::array<WORD, 64> bytes{};
    auto dialog = reinterpret_cast<DLGTEMPLATE*>(bytes.data());
    dialog->style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_CENTER | DS_SETFONT;
    dialog->cx = 310;
    dialog->cy = static_cast<short>(form.height);
    // 菜单、类和窗口标题为空，后续是字号和字体名称。
    auto font = reinterpret_cast<WORD*>(reinterpret_cast<BYTE*>(dialog) + 18) + 3;
    *font++ = 9;
    wcscpy_s(reinterpret_cast<wchar_t*>(font), 30, L"Microsoft YaHei UI");
    const auto result = DialogBoxIndirectParamW(GetModuleHandleW(nullptr), dialog, owner, procedure,
        reinterpret_cast<LPARAM>(&form));
    if (result == -1) throw std::runtime_error("无法打开存档设置对话框");
    if (result != IDOK) return std::nullopt;
    std::vector<std::wstring> values;
    for (auto& field : form.fields) values.push_back(std::move(field.value));
    return values;
}

bool save_webdav_password(HWND owner, const std::wstring& target, const std::wstring& username) {
    constexpr std::wstring_view prefix = L"GameGauge/Saves/WebDAV/";
    if (!target.starts_with(prefix) || target.size() != prefix.size() + 64 || username.empty())
        throw std::runtime_error("请先保存 WebDAV 地址和用户名");
    CREDUI_INFOW info{sizeof(info), owner, L"密码由 Windows 凭据管理器保存。", L"WebDAV 登录"};
    std::array<wchar_t, 513> user{};
    std::array<wchar_t, 257> password{};
    wcsncpy_s(user.data(), user.size(), username.c_str(), _TRUNCATE);
    const auto result = CredUIPromptForCredentialsW(&info, target.c_str(), nullptr, 0, user.data(),
        static_cast<ULONG>(user.size()), password.data(), static_cast<ULONG>(password.size()), nullptr,
        CREDUI_FLAGS_GENERIC_CREDENTIALS | CREDUI_FLAGS_ALWAYS_SHOW_UI |
        CREDUI_FLAGS_DO_NOT_PERSIST | CREDUI_FLAGS_KEEP_USERNAME);
    bool written{};
    if (result == NO_ERROR) {
        CREDENTIALW credential{};
        credential.Type = CRED_TYPE_GENERIC;
        credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
        credential.TargetName = const_cast<wchar_t*>(target.c_str());
        credential.UserName = const_cast<wchar_t*>(username.c_str());
        credential.CredentialBlob = reinterpret_cast<LPBYTE>(password.data());
        credential.CredentialBlobSize = static_cast<DWORD>(wcslen(password.data()) * sizeof(wchar_t));
        written = CredWriteW(&credential, 0) != FALSE;
    }
    SecureZeroMemory(password.data(), password.size() * sizeof(wchar_t));
    if (result == ERROR_CANCELLED) return false;
    if (!written) throw std::runtime_error("无法保存 Windows WebDAV 凭据");
    return true;
}
}
