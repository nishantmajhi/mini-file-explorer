#include "FileIconProvider.h"
#include <wx/image.h>

// =======================================================================
// Linux/GTK: mime-type theme icons via GIO + GtkIconTheme.
//
// This is the one platform implementation in this file that's actually
// been run and verified (see tests/README.md's sibling note in
// TRACKER.md) — this project's only available build/test environment is
// Linux/GTK.
// =======================================================================
#ifdef __WXGTK__
#include <gio/gio.h>
#include <gtk/gtk.h>

namespace
{
wxImage PixbufToWxImage(GdkPixbuf* pix)
{
    if (!pix)
        return wxImage();

    int w = gdk_pixbuf_get_width(pix);
    int h = gdk_pixbuf_get_height(pix);
    if (w <= 0 || h <= 0)
        return wxImage();

    int channels = gdk_pixbuf_get_n_channels(pix);
    int rowstride = gdk_pixbuf_get_rowstride(pix);
    const guchar* pixels = gdk_pixbuf_get_pixels(pix);
    bool hasAlpha = gdk_pixbuf_get_has_alpha(pix) != FALSE;

    wxImage img(w, h);
    img.InitAlpha();
    unsigned char* rgb = img.GetData();
    unsigned char* alpha = img.GetAlpha();
    for (int y = 0; y < h; ++y)
    {
        const guchar* row = pixels + (size_t)y * rowstride;
        for (int x = 0; x < w; ++x)
        {
            const guchar* px = row + (size_t)x * channels;
            int idx = y * w + x;
            rgb[idx * 3 + 0] = px[0];
            rgb[idx * 3 + 1] = px[1];
            rgb[idx * 3 + 2] = px[2];
            alpha[idx] = hasAlpha ? px[3] : 255;
        }
    }
    return img;
}

bool LookupThemedBitmap(GIcon* gicon, int px, wxBitmap& out)
{
    if (!gicon)
        return false;

    GtkIconTheme* theme = gtk_icon_theme_get_default();
    GtkIconInfo* info = gtk_icon_theme_lookup_by_gicon(theme, gicon, px, GTK_ICON_LOOKUP_FORCE_SIZE);
    if (!info)
        return false;

    GdkPixbuf* pixbuf = gtk_icon_info_load_icon(info, nullptr);
    g_object_unref(info);
    if (!pixbuf)
        return false;

    wxImage img = PixbufToWxImage(pixbuf);
    g_object_unref(pixbuf);
    if (!img.IsOk())
        return false;

    // GTK_ICON_LOOKUP_FORCE_SIZE should already have delivered exactly
    // px*px, but re-scaling if not costs nothing when it's already right
    // and guarantees the caller's wxImageList (which requires uniform
    // dimensions) never gets a mismatched bitmap.
    if (img.GetWidth() != px || img.GetHeight() != px)
        img = img.Scale(px, px, wxIMAGE_QUALITY_HIGH);

    out = wxBitmap(img);
    return out.IsOk();
}
} // namespace

namespace FileIconProvider
{
bool GetTypeIcon(const wxString& fullPath, bool isDir, int smallPx, int largePx,
                  wxBitmap& outSmall, wxBitmap& outLarge)
{
    gchar* contentType = isDir
        ? g_content_type_from_mime_type("inode/directory")
        // data=nullptr guesses purely from the filename/extension,
        // never touching the file itself — the whole reason this is
        // cheap enough for a caller to do once per unique extension.
        : g_content_type_guess(fullPath.utf8_str(), nullptr, 0, nullptr);
    if (!contentType)
        return false;

    GIcon* gicon = g_content_type_get_icon(contentType);
    g_free(contentType);
    if (!gicon)
        return false;

    bool okSmall = LookupThemedBitmap(gicon, smallPx, outSmall);
    bool okLarge = LookupThemedBitmap(gicon, largePx, outLarge);
    g_object_unref(gicon);
    return okSmall && okLarge;
}

bool GetThumbnail(const wxString&, int, int, wxBitmap&, wxBitmap&)
{
    // See the header comment: no synchronous on-demand thumbnail API
    // exists here to call. FileExplorerTab.cpp's own image decode is the
    // real implementation on this platform.
    return false;
}
} // namespace FileIconProvider

// =======================================================================
// Windows: SHGetFileInfo for colored per-type icons, IShellItemImageFactory
// for real on-demand thumbnails.
//
// UNVERIFIED ON REAL HARDWARE — same caveat as src/pty/WinConPtyBackend.cpp
// and for the same reason: this project's only available build/test
// environment is Linux/GTK (see TRACKER.md). Written carefully against
// documented Win32/Shell APIs, but has never actually run.
// =======================================================================
#elif defined(__WXMSW__)
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <commoncontrols.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
// MinGW-w64's import libraries don't reliably export the IID_IImageList
// symbol that commoncontrols.h only declares (extern) rather than
// defines — linking against it produces "undefined reference to
// IID_IImageList" (verified: fails to link on x86_64-w64-mingw32 even
// though the header compiles cleanly). Defining it locally from its
// documented GUID value sidesteps the missing import-lib entry entirely.
static const GUID kIID_IImageList =
    { 0x46eb5926, 0x582e, 0x4017, { 0x9f, 0xdf, 0xe8, 0x99, 0x8d, 0xaa, 0x09, 0x50 } };
// Standard Win32 technique for reading a DIB section's pixels into our
// own buffer — used for both the HICON case (via GetIconInfo's mask/color
// bitmaps) and the HBITMAP case (IShellItemImageFactory::GetImage), so
// this doesn't depend on guessing at wx's own MSW-private bitmap-wrapping
// API surface, which isn't something we can verify from a Linux-only
// build environment.
wxImage HBitmapToWxImage(HBITMAP hBmp)
{
    BITMAP bmpInfo{};
    if (!hBmp || !GetObject(hBmp, sizeof(bmpInfo), &bmpInfo))
        return wxImage();

    int w = bmpInfo.bmWidth;
    int h = std::abs((long)bmpInfo.bmHeight);
    if (w <= 0 || h <= 0)
        return wxImage();

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h; // negative = top-down, matches the row order we write below
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    std::vector<unsigned char> buf((size_t)w * h * 4);
    HDC screenDC = GetDC(nullptr);
    int gotLines = GetDIBits(screenDC, hBmp, 0, (UINT)h, buf.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, screenDC);
    if (gotLines == 0)
        return wxImage();

    wxImage img(w, h);
    img.InitAlpha();
    unsigned char* rgb = img.GetData();
    unsigned char* alpha = img.GetAlpha();
    bool anyAlpha = false;
    for (int i = 0; i < w * h; ++i)
    {
        unsigned char b = buf[(size_t)i * 4 + 0];
        unsigned char g = buf[(size_t)i * 4 + 1];
        unsigned char r = buf[(size_t)i * 4 + 2];
        unsigned char a = buf[(size_t)i * 4 + 3];
        rgb[i * 3 + 0] = r;
        rgb[i * 3 + 1] = g;
        rgb[i * 3 + 2] = b;
        alpha[i] = a;
        if (a != 0)
            anyAlpha = true;
    }
    // Some icon/thumbnail sources return a 32bpp DIB with an all-zero
    // alpha channel, meaning "no real alpha data" rather than "fully
    // transparent" — taking that literally would render as a blank
    // square, so treat all-zero as fully opaque instead.
    if (!anyAlpha)
        memset(alpha, 255, (size_t)w * h);
    return img;
}

// IShellItemImageFactory::GetImage fits the source into the requested
// SIZE while preserving aspect ratio — it does NOT pad/composite to an
// exact square the way the code that used to call this assumed. Forcing
// a non-square result through Scale(px, px, ...) is what stretched
// landscape/portrait photos to fill the whole square (visible as
// distorted-looking thumbnails). This mirrors FileExplorerTab.cpp's own
// MakeThumbnailBitmap: scale to fit within px*px preserving aspect ratio,
// then center on a transparent px*px canvas so the wxImageList still gets
// the uniform dimensions it requires.
wxImage LetterboxToSquare(const wxImage& srcIn, int px)
{
    int srcW = srcIn.GetWidth(), srcH = srcIn.GetHeight();
    if (srcW <= 0 || srcH <= 0)
        return wxImage(px, px);
    if (srcW == px && srcH == px)
        return srcIn;

    double scale = std::min((double)px / srcW, (double)px / srcH);
    int w = std::max(1, (int)(srcW * scale + 0.5));
    int h = std::max(1, (int)(srcH * scale + 0.5));
    wxImage scaled = srcIn.Scale(w, h, wxIMAGE_QUALITY_HIGH);
    if (!scaled.HasAlpha())
        scaled.InitAlpha();

    wxImage canvas(px, px);
    canvas.InitAlpha();
    memset(canvas.GetAlpha(), 0, (size_t)px * px);
    canvas.Paste(scaled, (px - w) / 2, (px - h) / 2);
    return canvas;
}

wxBitmap HIconToBitmap(HICON hIcon, int px)
{
    if (!hIcon)
        return wxBitmap();

    ICONINFO ii{};
    wxBitmap result;
    if (GetIconInfo(hIcon, &ii))
    {
        // hbmColor alone (without compositing hbmMask) is correct for the
        // common case of a modern 32bpp icon with its own alpha channel;
        // HBitmapToWxImage's own all-zero-alpha fallback above handles
        // the legacy non-alpha icon case reasonably (renders opaque
        // rather than invisible), which is an acceptable simplification
        // for file-type icons specifically — they're essentially always
        // modern 32bpp icons with real alpha in any current Windows theme.
        wxImage img = HBitmapToWxImage(ii.hbmColor);
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        if (ii.hbmMask) DeleteObject(ii.hbmMask);

        if (img.IsOk())
        {
            if (img.GetWidth() != px || img.GetHeight() != px)
                img = img.Scale(px, px, wxIMAGE_QUALITY_HIGH);
            result = wxBitmap(img);
        }
    }
    return result;
}

// SHGFI_LARGEICON only ever hands back the system's "large" icon size —
// 32x32 on a stock install, sometimes 48x48 depending on DPI/theme, but
// never anything close to this app's 64px grid cells. Upscaling that with
// HIconToBitmap's Scale() is exactly what produced the blurry/low-res
// icons: stretching a 32px source up to 64px. SHIL_JUMBO exposes the same
// per-file-type icons pre-rendered at 256x256, so downscaling from there
// to px stays crisp instead. Returns an invalid bitmap if anything along
// the way fails, letting the caller fall back to the SHGFI_LARGEICON path.
wxBitmap GetJumboIcon(const wchar_t* pathW, DWORD attrs, int px)
{
    SHFILEINFOW info{};
    if (!SHGetFileInfoW(pathW, attrs, &info, sizeof(info), SHGFI_SYSICONINDEX | SHGFI_USEFILEATTRIBUTES) || info.iIcon < 0)
        return wxBitmap();

    IImageList* imgList = nullptr;
    if (FAILED(SHGetImageList(SHIL_JUMBO, kIID_IImageList, (void**)&imgList)) || !imgList)
        return wxBitmap();

    HICON hIcon = nullptr;
    wxBitmap result;
    if (SUCCEEDED(imgList->GetIcon(info.iIcon, ILD_TRANSPARENT, &hIcon)) && hIcon)
    {
        result = HIconToBitmap(hIcon, px);
        DestroyIcon(hIcon);
    }
    imgList->Release();
    return result;
}
} // namespace

namespace FileIconProvider
{
bool GetTypeIcon(const wxString& fullPath, bool isDir, int smallPx, int largePx,
                  wxBitmap& outSmall, wxBitmap& outLarge)
{
    std::wstring pathW = fullPath.ToStdWstring();
    DWORD attrs = isDir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    // SHGFI_USEFILEATTRIBUTES: look the type up by the given attributes
    // and the extension in pathW alone, without touching the real file —
    // pathW doesn't even need to exist. This is what makes it reasonable
    // for FileExplorerTab.cpp to call this once per unique extension
    // rather than once per real file on disk.
    const UINT baseFlags = SHGFI_ICON | SHGFI_USEFILEATTRIBUTES;

    SHFILEINFOW infoSmall{};
    SHFILEINFOW infoLarge{};
    bool gotSmall = SHGetFileInfoW(pathW.c_str(), attrs, &infoSmall, sizeof(infoSmall), baseFlags | SHGFI_SMALLICON) != 0;
    bool gotLarge = SHGetFileInfoW(pathW.c_str(), attrs, &infoLarge, sizeof(infoLarge), baseFlags | SHGFI_LARGEICON) != 0;

    if (gotSmall && infoSmall.hIcon)
    {
        outSmall = HIconToBitmap(infoSmall.hIcon, smallPx);
        DestroyIcon(infoSmall.hIcon); // SHGetFileInfo's docs: caller owns and must destroy the returned handle
    }

    // Try the high-res jumbo list first for the large/grid icon; only
    // fall back to upscaling SHGFI_LARGEICON's ~32px result if the jumbo
    // lookup isn't available (e.g. a locked-down system image list).
    outLarge = GetJumboIcon(pathW.c_str(), attrs, largePx);
    if (!outLarge.IsOk() && gotLarge && infoLarge.hIcon)
        outLarge = HIconToBitmap(infoLarge.hIcon, largePx);
    if (gotLarge && infoLarge.hIcon)
        DestroyIcon(infoLarge.hIcon);

    return outSmall.IsOk() && outLarge.IsOk();
}

bool GetThumbnail(const wxString& fullPath, int smallPx, int largePx, wxBitmap& outSmall, wxBitmap& outLarge)
{
    HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool comAcquired = SUCCEEDED(coHr); // covers both S_OK (first init on this thread) and S_FALSE (already initialized elsewhere, e.g. by wx itself for drag & drop) — either way we now hold a reference to pair with CoUninitialize below

    bool ok = false;
    IShellItemImageFactory* factory = nullptr;
    std::wstring pathW = fullPath.ToStdWstring();
    HRESULT hr = SHCreateItemFromParsingName(pathW.c_str(), nullptr, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr) && factory)
    {
        HBITMAP hBmp = nullptr;
        // SIIGBF_BIGGERSIZEOK: accept a cached larger thumbnail and let us
        // downscale it ourselves rather than force a possibly-slower
        // regeneration at exactly largePx. GetImage fits the source into
        // this SIZE preserving aspect ratio — it does NOT pad the result
        // to an exact square for non-square sources, so the letterbox
        // step below is what keeps a landscape/portrait photo from being
        // stretched to fill the square.
        hr = factory->GetImage(SIZE{ largePx, largePx }, SIIGBF_BIGGERSIZEOK, &hBmp);
        if (SUCCEEDED(hr) && hBmp)
        {
            wxImage img = HBitmapToWxImage(hBmp);
            DeleteObject(hBmp);
            if (img.IsOk())
            {
                wxImage square = LetterboxToSquare(img, largePx);
                outLarge = wxBitmap(square);
                outSmall = wxBitmap(LetterboxToSquare(img, smallPx));
                ok = outLarge.IsOk() && outSmall.IsOk();
            }
        }
        factory->Release();
    }

    if (comAcquired)
        CoUninitialize();
    return ok;
}
} // namespace FileIconProvider

// =======================================================================
// Any other platform: no OS-provided lookup implemented. Callers already
// treat false as "fall back to the generic icon / manual decode", so this
// is a correct (if minimal) implementation, not a stub that needs finishing.
// =======================================================================
#else
namespace FileIconProvider
{
bool GetTypeIcon(const wxString&, bool, int, int, wxBitmap&, wxBitmap&) { return false; }
bool GetThumbnail(const wxString&, int, int, wxBitmap&, wxBitmap&) { return false; }
}
#endif
