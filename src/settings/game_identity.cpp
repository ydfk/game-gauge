#include "window.h"
#include "theme.h"
#include "common/game_identity.h"
#include <shellapi.h>
#include <wincodec.h>
#include <map>

namespace gauge {
void SettingsWindow::game_identity(const Json& row, float x, float y, float width) {
    using Microsoft::WRL::ComPtr;
    struct Identity { std::string name; ComPtr<IWICFormatConverter> icon; };
    static std::map<std::string, Identity> cache;
    const auto path = row.value("path", std::string{}), original = row.value("game", std::string{});
    const auto key = path + "\n" + original;
    if (cache.size() > 256) cache.clear();
    auto [it, added] = cache.try_emplace(key);
    auto& identity = it->second;
    if (added) {
        identity.name = game_display_name(path, original);
        SHFILEINFOW info{};
        if (!path.empty() && SHGetFileInfoW(wide(path).c_str(), 0, &info, sizeof(info), SHGFI_ICON | SHGFI_LARGEICON)) {
            ComPtr<IWICImagingFactory> factory;
            ComPtr<IWICBitmap> source;
            if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
                SUCCEEDED(factory->CreateBitmapFromHICON(info.hIcon, &source)) && SUCCEEDED(factory->CreateFormatConverter(&identity.icon))) {
                if (FAILED(identity.icon->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA,
                    WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) identity.icon.Reset();
            }
            DestroyIcon(info.hIcon);
        }
    }
    ComPtr<ID2D1Bitmap> bitmap;
    if (identity.icon && SUCCEEDED(target_->CreateBitmapFromWicBitmap(identity.icon.Get(), &bitmap)))
        target_->DrawBitmap(bitmap.Get(), D2D1::RectF(x, y + 2, x + 36, y + 38));
    else {
        fill(D2D1::RectF(x, y + 2, x + 36, y + 38), D2D1::ColorF(0x284758), 7);
        label(L"G", x + 9, y + 5, 24, 28, heading_.Get(), theme::mint);
    }
    auto name = row.value("display_name", identity.name);
    if (name.empty()) name = identity.name;
    label(wide(name), x + 48, y - 2, width - 48, 26, heading_.Get(), theme::white);
    label(wide(original), x + 48, y + 23, width - 48, 19, small_.Get(), theme::muted);
}
}
