#pragma once
#include "theme.h"
#include <algorithm>
#include <string>
#include <vector>

namespace gauge {
struct MenuOption {
    std::wstring title;
    bool checked{};
};

inline int show_options(HWND owner, float scale, D2D1_RECT_F anchor, const std::vector<MenuOption>& options) {
    if (options.empty()) return -1;
    auto menu = CreatePopupMenu();
    if (!menu) return -1;
    const auto background = CreateSolidBrush(theme::native_color(theme::sidebar));
    MENUINFO backdrop{sizeof(backdrop)};
    backdrop.fMask = MIM_BACKGROUND;
    backdrop.hbrBack = background;
    SetMenuInfo(menu, &backdrop);
    for (size_t i = 0; i < options.size(); ++i) {
        MENUITEMINFOW item{sizeof(item)};
        item.fMask = MIIM_FTYPE | MIIM_ID | MIIM_DATA | MIIM_STRING;
        item.fType = MFT_OWNERDRAW;
        item.wID = static_cast<UINT>(i + 1);
        item.dwItemData = reinterpret_cast<ULONG_PTR>(&options[i]);
        item.dwTypeData = const_cast<wchar_t*>(options[i].title.c_str());
        InsertMenuItemW(menu, static_cast<UINT>(i), TRUE, &item);
    }
    POINT point{static_cast<LONG>(anchor.left * scale), static_cast<LONG>(anchor.bottom * scale)};
    ClientToScreen(owner, &point);
    const auto result = TrackPopupMenu(menu, TPM_RETURNCMD, point.x, point.y, 0, owner, nullptr);
    DestroyMenu(menu);
    DeleteObject(background);
    return result ? static_cast<int>(result) - 1 : -1;
}

inline HFONT menu_font(UINT dpi) {
    return CreateFontW(-MulDiv(14, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH, L"Microsoft YaHei UI");
}
inline bool measure_menu(HWND owner, MEASUREITEMSTRUCT& item) {
    if (item.CtlType != ODT_MENU || !item.itemData) return false;
    const auto& option = *reinterpret_cast<const MenuOption*>(item.itemData);
    const UINT dpi = GetDpiForWindow(owner);
    const auto dc = GetDC(owner);
    const auto font = menu_font(dpi);
    const auto previous = SelectObject(dc, font);
    SIZE measured{};
    GetTextExtentPoint32W(dc, option.title.c_str(), static_cast<int>(option.title.size()), &measured);
    SelectObject(dc, previous); DeleteObject(font); ReleaseDC(owner, dc);
    item.itemWidth = std::clamp<LONG>(measured.cx + MulDiv(52, dpi, 96), MulDiv(166, dpi, 96), MulDiv(620, dpi, 96));
    item.itemHeight = MulDiv(34, dpi, 96);
    return true;
}
inline bool draw_menu(HWND owner, const DRAWITEMSTRUCT& item) {
    if (item.CtlType != ODT_MENU || !item.itemData) return false;
    const auto& option = *reinterpret_cast<const MenuOption*>(item.itemData);
    const UINT dpi = GetDpiForWindow(owner);
    const int saved = SaveDC(item.hDC);
    const bool selected = (item.itemState & ODS_SELECTED) != 0;
    const auto brush = CreateSolidBrush(theme::native_color(selected ? theme::panel_high : theme::sidebar));
    FillRect(item.hDC, &item.rcItem, brush);
    DeleteObject(brush);
    RECT label = item.rcItem;
    label.left += MulDiv(29, dpi, 96);
    label.right -= MulDiv(15, dpi, 96);
    const auto font = menu_font(dpi);
    SelectObject(item.hDC, font);
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, theme::native_color(option.checked ? theme::lilac : theme::white));
    DrawTextW(item.hDC, option.title.c_str(), static_cast<int>(option.title.size()), &label,
        DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (option.checked) {
        const int x = item.rcItem.left + MulDiv(10, dpi, 96), y = (item.rcItem.top + item.rcItem.bottom) / 2;
        const auto pen = CreatePen(PS_SOLID, std::max(1, MulDiv(2, dpi, 96)), theme::native_color(theme::lilac));
        const auto previous = SelectObject(item.hDC, pen);
        MoveToEx(item.hDC, x, y, nullptr);
        LineTo(item.hDC, x + MulDiv(4, dpi, 96), y + MulDiv(4, dpi, 96));
        LineTo(item.hDC, x + MulDiv(10, dpi, 96), y - MulDiv(4, dpi, 96));
        SelectObject(item.hDC, previous);
        DeleteObject(pen);
    }
    RestoreDC(item.hDC, saved);
    DeleteObject(font);
    return true;
}
}
