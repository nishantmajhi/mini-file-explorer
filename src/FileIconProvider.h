#pragma once
#include <wx/bitmap.h>
#include <wx/string.h>

// Tries the operating system's own icon/thumbnail facilities first — the
// Windows Shell API's registered file-type icons and shell thumbnail
// cache, or GTK/GIO's mime-type theme icons on Linux — so files show the
// same colored, familiar icons the platform's own file manager would
// show, without this app needing to draw or maintain its own per-
// extension icon set. Returns false wherever a platform doesn't support
// a given lookup (or a specific call fails); FileExplorerTab.cpp falls
// back to a generic icon (colored type icon path) or its own manual
// image decode (thumbnail path) in that case — see the comments at each
// call site there for why those particular fallbacks are the right ones.
namespace FileIconProvider
{
    // Per-type icon: the folder icon when isDir is true, otherwise
    // whatever the OS associates with fullPath's extension. fullPath
    // doesn't need to actually exist on disk for this one — both
    // implementations key off the extension/attributes alone (confirmed:
    // Windows via SHGFI_USEFILEATTRIBUTES, Linux via g_content_type_guess
    // with no file data), which is what makes it reasonable for a caller
    // to look this up once per unique extension in a directory listing
    // rather than once per file.
    bool GetTypeIcon(const wxString& fullPath, bool isDir, int smallPx, int largePx,
                      wxBitmap& outSmall, wxBitmap& outLarge);

    // A real, decoded preview of the file's actual content — not just its
    // type icon. Only implemented where the OS exposes this synchronously
    // and on-demand (currently: Windows, via IShellItemImageFactory,
    // which covers images and anything else with an installed shell
    // thumbnail provider). Deliberately NOT implemented via Linux's GIO
    // "thumbnail::path" file attribute: that only surfaces thumbnails a
    // background thumbnailer service (tumbler/gnome-thumbnail-factory)
    // has already generated asynchronously and cached — there's no
    // synchronous "generate one now" call behind it, and this app has no
    // business trying to talk to or launch that service itself. fullPath
    // must be a real, readable file.
    bool GetThumbnail(const wxString& fullPath, int smallPx, int largePx,
                       wxBitmap& outSmall, wxBitmap& outLarge);
}
