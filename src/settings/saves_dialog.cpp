#include "saves_dialog.h"
#include "theme.h"
#include <commdlg.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shobjidl.h>
#include <wincred.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <stdexcept>

namespace gauge {
namespace {
constexpr int field_id = 100, browse_id = 200;
struct FormColors {
    HBRUSH background = CreateSolidBrush(theme::native_color(theme::sidebar));
    HBRUSH input = CreateSolidBrush(theme::native_color(theme::panel_high));
    ~FormColors() { DeleteObject(background); DeleteObject(input); }
};
struct Form {
    const wchar_t* title;
    std::vector<SaveField> fields;
    int height{};
    FormColors colors;
    std::vector<std::pair<HWND, RECT>> edits;
};

void paint_check(HWND dialog, const NMCUSTOMDRAW& item, HBRUSH background) {
    const UINT dpi = GetDpiForWindow(dialog);
    const int size = MulDiv(16, dpi, 96), inset = MulDiv(6, dpi, 96);
    const int x = item.rc.left + 1, y = (item.rc.top + item.rc.bottom - size) / 2;
    const bool checked = SendMessageW(item.hdr.hwndFrom, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool focused = GetFocus() == item.hdr.hwndFrom;
    const int saved = SaveDC(item.hdc);
    FillRect(item.hdc, &item.rc, background);
    const auto brush = CreateSolidBrush(theme::native_color(checked ? theme::accent : theme::panel));
    const auto frame = CreatePen(PS_SOLID, 1, theme::native_color(focused ? theme::lilac : checked ? theme::accent : theme::edge));
    SelectObject(item.hdc, brush); SelectObject(item.hdc, frame);
    RoundRect(item.hdc, x, y, x + size, y + size, inset, inset);
    const auto ink = CreatePen(PS_SOLID, std::max(1, MulDiv(2, dpi, 96)), theme::native_color(theme::white));
    if (checked) {
        SelectObject(item.hdc, ink);
        MoveToEx(item.hdc, x + size / 4, y + size / 2, nullptr);
        LineTo(item.hdc, x + size * 7 / 16, y + size * 11 / 16);
        LineTo(item.hdc, x + size * 3 / 4, y + size / 3);
    }
    auto text = item.rc;
    text.left = x + size + inset * 2;
    wchar_t caption[120]{};
    GetWindowTextW(item.hdr.hwndFrom, caption, ARRAYSIZE(caption));
    SelectObject(item.hdc, reinterpret_cast<HFONT>(SendMessageW(dialog, WM_GETFONT, 0, 0)));
    SetTextColor(item.hdc, theme::native_color(focused ? theme::lilac : theme::white));
    SetBkMode(item.hdc, TRANSPARENT);
    DrawTextW(item.hdc, caption, -1, &text, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    RestoreDC(item.hdc, saved);
    DeleteObject(ink); DeleteObject(frame); DeleteObject(brush);
}

void paint_button(HWND dialog, const NMCUSTOMDRAW& item) {
    const bool primary = item.hdr.idFrom == IDOK;
    const bool selected = (item.uItemState & CDIS_SELECTED) != 0;
    const bool active = (item.uItemState & (CDIS_SELECTED | CDIS_HOT)) != 0;
    const bool focus = GetFocus() == item.hdr.hwndFrom;
    const auto brush = CreateSolidBrush(theme::native_color(primary ? (active ? theme::accent : theme::edge)
        : active ? theme::panel_high : theme::panel));
    const auto pen = CreatePen(PS_SOLID, 1, theme::native_color(focus ? theme::lilac : primary ? theme::accent : theme::edge));
    const int saved = SaveDC(item.hdc);
    SelectObject(item.hdc, brush); SelectObject(item.hdc, pen);
    const auto rect = item.rc;
    const int radius = MulDiv(static_cast<int>(theme::control_radius), GetDpiForWindow(dialog), 96);
    RoundRect(item.hdc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    wchar_t caption[96]{};
    GetWindowTextW(item.hdr.hwndFrom, caption, ARRAYSIZE(caption));
    auto text = rect;
    if (selected) ++text.top;
    SelectObject(item.hdc, reinterpret_cast<HFONT>(SendMessageW(dialog, WM_GETFONT, 0, 0)));
    SetTextColor(item.hdc, theme::native_color(theme::white));
    SetBkMode(item.hdc, TRANSPARENT);
    DrawTextW(item.hdc, caption, -1, &text, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    RestoreDC(item.hdc, saved); DeleteObject(pen); DeleteObject(brush);
}

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
        const BOOL dark = TRUE;
        const COLORREF border = theme::native_color(theme::edge), caption = theme::native_color(theme::sidebar);
        DwmSetWindowAttribute(dialog, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        DwmSetWindowAttribute(dialog, DWMWA_BORDER_COLOR, &border, sizeof(border));
        DwmSetWindowAttribute(dialog, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
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
            const int right = field.folder ? 254 : 300;
            const auto edit = control(L"EDIT", field.value.c_str(), WS_TABSTOP | edit_style, 0, id,
                {12, y + 4, right - 2, y + height - 2});
            RECT frame{10, y, right, y + height};
            MapDialogRect(dialog, &frame);
            form->edits.emplace_back(edit, frame);
            SendMessageW(edit, EM_SETLIMITTEXT, field.limit, 0);
            const DWORD inset = MulDiv(8, GetDpiForWindow(dialog), 96);
            SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(inset, inset));
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
    if (!form) return FALSE;
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        const auto dc = BeginPaint(dialog, &paint);
        const int saved = SaveDC(dc);
        SelectObject(dc, form->colors.input);
        const int radius = MulDiv(static_cast<int>(theme::control_radius), GetDpiForWindow(dialog), 96);
        for (const auto& [edit, rect] : form->edits) {
            const auto pen = CreatePen(PS_SOLID, 1, theme::native_color(GetFocus() == edit ? theme::accent : theme::edge));
            const auto previous = SelectObject(dc, pen);
            RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
            SelectObject(dc, previous); DeleteObject(pen);
        }
        RestoreDC(dc, saved);
        EndPaint(dialog, &paint);
        return TRUE;
    }
    if (message == WM_CTLCOLORDLG) return reinterpret_cast<INT_PTR>(form->colors.background);
    if (message == WM_CTLCOLORSTATIC || message == WM_CTLCOLORBTN || message == WM_CTLCOLOREDIT) {
        const auto dc = reinterpret_cast<HDC>(wparam);
        const bool edit = message == WM_CTLCOLOREDIT;
        SetTextColor(dc, theme::native_color(edit ? theme::white : theme::muted));
        SetBkColor(dc, theme::native_color(edit ? theme::panel_high : theme::sidebar));
        return reinterpret_cast<INT_PTR>(edit ? form->colors.input : form->colors.background);
    }
    if (message == WM_NOTIFY) {
        const auto& header = *reinterpret_cast<const NMHDR*>(lparam);
        if (header.code == NM_CUSTOMDRAW) {
            const auto& item = *reinterpret_cast<const NMCUSTOMDRAW*>(lparam);
            const bool check = (GetWindowLongPtrW(header.hwndFrom, GWL_STYLE) & BS_TYPEMASK) == BS_AUTOCHECKBOX;
            const bool push = header.idFrom == IDOK || header.idFrom == IDCANCEL || header.idFrom >= browse_id;
            if (item.dwDrawStage == CDDS_PREPAINT && (check || push)) {
                if (check) paint_check(dialog, item, form->colors.background);
                else paint_button(dialog, item);
                SetWindowLongPtrW(dialog, DWLP_MSGRESULT, CDRF_SKIPDEFAULT);
                return TRUE;
            }
        }
    }
    if (message != WM_COMMAND) return FALSE;
    const int id = LOWORD(wparam);
    if (id >= field_id && id < browse_id && (HIWORD(wparam) == EN_SETFOCUS || HIWORD(wparam) == EN_KILLFOCUS))
        InvalidateRect(dialog, nullptr, FALSE);
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
    dialog->style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN | DS_MODALFRAME | DS_CENTER | DS_SETFONT;
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
