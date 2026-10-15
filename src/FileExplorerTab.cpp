#include "FileExplorerTab.h"
#include "FileIconGrid.h"
#include "FileIconProvider.h"
#include "RecycleBin.h"
#include <wx/filename.h>
#include <wx/dir.h>
#include <wx/artprov.h>
#include <wx/mimetype.h>
#include <wx/file.h>
#include <wx/dnd.h>
#include <wx/log.h>
#include <wx/dirdlg.h>
#include <wx/tokenzr.h>
#include <wx/image.h>
#include <wx/imaglist.h>
#include <algorithm>
#include <cstring>
#include <map>
#ifdef __WXMSW__
#include <windows.h>
#include <shellapi.h>
#endif

wxBEGIN_EVENT_TABLE(FileExplorerTab, wxPanel)
    EVT_LIST_ITEM_ACTIVATED(wxID_ANY, FileExplorerTab::OnItemActivated)
    EVT_LIST_ITEM_RIGHT_CLICK(wxID_ANY, FileExplorerTab::OnItemRightClick)
    EVT_LIST_COL_CLICK(wxID_ANY, FileExplorerTab::OnColumnClick)
    EVT_LIST_BEGIN_DRAG(wxID_ANY, FileExplorerTab::OnBeginDrag)
    EVT_LIST_ITEM_SELECTED(wxID_ANY, FileExplorerTab::OnSelectionChanged)
    EVT_LIST_ITEM_DESELECTED(wxID_ANY, FileExplorerTab::OnSelectionChanged)
    EVT_MENU(ID_NewFolder, FileExplorerTab::OnContextMenu)
    EVT_MENU(ID_NewFile, FileExplorerTab::OnContextMenu)
    EVT_MENU(ID_Rename, FileExplorerTab::OnContextMenu)
    EVT_MENU(ID_Delete, FileExplorerTab::OnContextMenu)
    EVT_MENU(ID_Move, FileExplorerTab::OnContextMenu)
    EVT_MENU(ID_Copy, FileExplorerTab::OnContextMenu)
    EVT_MENU(ID_ShowInfo, FileExplorerTab::OnContextMenu)
    EVT_MENU(ID_Cut, FileExplorerTab::OnContextMenu)
    EVT_MENU(ID_CopyClip, FileExplorerTab::OnContextMenu)
    EVT_MENU(ID_PasteClip, FileExplorerTab::OnContextMenu)
wxEND_EVENT_TABLE()

// Shared across every tab/pane in the app — see the declaration comment in
// FileExplorerTab.h for why this is a static in-app clipboard rather than
// the real OS one.
std::vector<wxString> FileExplorerTab::s_clipboardPaths;
bool FileExplorerTab::s_clipboardIsCut = false;

namespace
{
    // Column indices for the details list.
    enum { COL_NAME = 0, COL_SIZE, COL_TYPE, COL_MODIFIED };

    wxString HumanReadableSize(wxULongLong bytes)
    {
        double b = bytes.ToDouble();
        const char* units[] = { "B", "KB", "MB", "GB", "TB" };
        int unit = 0;
        while (b >= 1024.0 && unit < 4) { b /= 1024.0; ++unit; }
        return wxString::Format("%.1f %s", b, units[unit]);
    }

    // Icon box sizes for the two parallel image lists (see the member
    // comment in FileExplorerTab.h). 16px matches the existing details-view
    // icons; 64px is a reasonable "medium" thumbnail size — big enough to
    // actually recognize an image at a glance, small enough that decoding
    // and scaling a folder's worth of them stays snappy.
    constexpr int kSmallIconPx = 16;
    constexpr int kNormalIconPx = 64;

    // Real image thumbnails are only attempted for files at or under this
    // size, and only for the first N per directory listing — without both
    // caps, a folder full of huge or numerous images would make every
    // ReloadDirectory() (which runs synchronously on the UI thread, same as
    // the rest of this file's directory scanning) noticeably freeze the
    // window. Anything skipped by either cap just falls back to the
    // generic file icon, same as any non-image file.
    const wxULongLong kMaxThumbnailFileSize = 15ull * 1024 * 1024;
    constexpr int kMaxThumbnailsPerListing = 200;

    bool IsImageExtension(const wxString& ext)
    {
        wxString e = ext.Lower();
        static const wxString kImageExts[] = {
            "png", "jpg", "jpeg", "bmp", "gif", "ico", "pcx", "tga", "tif", "tiff", "xpm"
        };
        for (const wxString& x : kImageExts)
            if (e == x)
                return true;
        return false;
    }

    // Scales src to fit within a boxPx x boxPx square (preserving aspect
    // ratio, letterboxed on transparent) rather than stretching it to fill
    // — a squashed/stretched thumbnail would misrepresent the image, and a
    // uniform box size is what wxImageList requires anyway.
    wxBitmap MakeThumbnailBitmap(const wxImage& srcIn, int boxPx)
    {
        int srcW = srcIn.GetWidth(), srcH = srcIn.GetHeight();
        if (srcW <= 0 || srcH <= 0)
            return wxBitmap(boxPx, boxPx);

        double scale = std::min((double)boxPx / srcW, (double)boxPx / srcH);
        int w = std::max(1, (int)(srcW * scale + 0.5));
        int h = std::max(1, (int)(srcH * scale + 0.5));
        wxImage scaled = srcIn.Scale(w, h, wxIMAGE_QUALITY_HIGH);
        if (!scaled.HasAlpha())
            scaled.InitAlpha();

        // Building the canvas as a wxImage (rather than drawing into a
        // wxMemoryDC) and zeroing its alpha buffer directly is what makes
        // the transparent margin around a non-square thumbnail reliably
        // transparent across platforms — a DC-based Clear(*wxTRANSPARENT_
        // BRUSH) doesn't consistently zero the alpha channel of a 32-bit
        // bitmap on every backend, but a wxImage's alpha buffer is just a
        // byte array we control directly.
        wxImage canvas(boxPx, boxPx);
        canvas.InitAlpha();
        memset(canvas.GetAlpha(), 0, (size_t)boxPx * boxPx);
        canvas.Paste(scaled, (boxPx - w) / 2, (boxPx - h) / 2);

        return wxBitmap(canvas);
    }

    // wxFileName(path) with no trailing separator treats the last path
    // component as a filename regardless of whether it's actually a file or
    // a directory on disk — which is exactly what we want here, since
    // GetFullPath()-built item paths (see m_contextTargetPaths) never have a
    // trailing separator either way.
    wxString ItemNameFromPath(const wxString& path)
    {
        wxFileName fn(path);
        return fn.GetFullName().IsEmpty() ? fn.GetDirs().Last() : fn.GetFullName();
    }

    // wx has wxCopyFile for a single file, but nothing for a whole
    // directory tree — this walks it by hand. Returns false if anything
    // along the way failed (but still attempts everything else, so a
    // single locked/in-use file doesn't abort copying the rest).
    bool CopyDirectoryRecursive(const wxString& src, const wxString& dst)
    {
        if (!wxDirExists(dst) && !wxFileName::Mkdir(dst, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL))
            return false;

        wxDir dir(src);
        if (!dir.IsOpened())
            return false;

        wxLogNull noLog; // same access-denied-on-metadata rationale as ReloadDirectory
        wxString name;
        bool ok = true;

        bool cont = dir.GetFirst(&name, wxEmptyString, wxDIR_DIRS);
        while (cont)
        {
            wxString srcSub = wxFileName(src, "").GetPathWithSep() + name;
            wxString dstSub = wxFileName(dst, "").GetPathWithSep() + name;
            ok = CopyDirectoryRecursive(srcSub, dstSub) && ok;
            cont = dir.GetNext(&name);
        }
        cont = dir.GetFirst(&name, wxEmptyString, wxDIR_FILES);
        while (cont)
        {
            wxString srcFile = wxFileName(src, name).GetFullPath();
            wxString dstFile = wxFileName(dst, name).GetFullPath();
            ok = wxCopyFile(srcFile, dstFile) && ok;
            cont = dir.GetNext(&name);
        }
        return ok;
    }

    // One filesystem entry, carrying every field we might sort by, so a
    // single comparator can handle whichever column was clicked.
    struct DirEntry
    {
        wxString name;
        bool isDir;
        wxULongLong size;
        wxString typeLabel;
        wxDateTime modified;
        int imageIndex; // index into both m_imageListSmall and m_imageListNormal
    };

    // A drag payload is the source dir followed by one dragged item name
    // per line — enough for the drop target (in this pane or the other
    // split pane) to resolve full paths itself, for however many items are
    // being dragged.
    class ExplorerDropTarget : public wxTextDropTarget
    {
    public:
        // boundTo is whichever window this drop target is actually
        // attached to (m_list or m_iconGrid) — needed to convert the
        // coordinates OnDropText hands us (relative to that window) into
        // screen coordinates, since HandleDrop() has to work the same way
        // regardless of which of the two currently-visible widgets the
        // drop landed on.
        ExplorerDropTarget(FileExplorerTab* owner, wxWindow* boundTo) : m_owner(owner), m_boundTo(boundTo) {}
        bool OnDropText(wxCoord x, wxCoord y, const wxString& text) override
        {
            return m_owner->HandleDrop(m_boundTo->ClientToScreen(wxPoint(x, y)), text);
        }
    private:
        FileExplorerTab* m_owner;
        wxWindow* m_boundTo;
    };
}

FileExplorerTab::FileExplorerTab(wxWindow* parent, const wxString& startPath)
    : wxPanel(parent)
{
    // --- Toolbar row: back / forward / up / address bar ---
    wxPanel* toolbarPanel = new wxPanel(this);

    // Solid triangular arrowheads read far better at small toolbar sizes
    // than thin arrow glyphs (\u2190 etc.), which were nearly invisible.
    m_backBtn = MakeNavButton(toolbarPanel, wxT("\u2B9C"));
    m_forwardBtn = MakeNavButton(toolbarPanel, wxT("\u2B9E"));
    m_upBtn = MakeNavButton(toolbarPanel, wxT("\u2B9D"));

    m_addressBar = new wxTextCtrl(toolbarPanel, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
        wxTE_PROCESS_ENTER | wxBORDER_SIMPLE);

    // Shows the mode a click would switch TO (so the icon+label read
    // "[grid icon] Icons" while currently in Details view, and "[table
    // icon] List" in Icons view) — same idea as the terminal toggle's own
    // menu checkbox, just as a plain clickable icon+label pair to match
    // the nav buttons' flat, chromeless style rather than a boxed
    // toolbar/menu button. wxART_LIST_VIEW/wxART_REPORT_VIEW are wx's own
    // stock art for exactly this pairing, so this needs no custom-drawn
    // icon of our own.
    //
    // m_viewToggleIcon and m_viewToggleBtn live inside m_viewTogglePanel,
    // a dedicated container, rather than being added to the toolbar sizer
    // as two independent items, for two reasons:
    //   1. Two separate sizer items always leave the inter-item spacing
    //      between them un-owned by either control, so a click landing in
    //      that gap fell through to the toolbar panel and did nothing.
    //      Binding the click handler on the container as well as both
    //      children means the whole rect — icon, gap, and label — reads
    //      as one button.
    //   2. The label text alternates between "Icons" and "List", which
    //      have different natural widths; when the icon+label were two
    //      separate proportion-0 sizer items next to the address bar's
    //      proportion-1 expanding item, that width change visibly resized
    //      the address bar on every toggle. Giving the container a fixed
    //      min-width — sized to fit the wider of the two labels — keeps
    //      the toolbar's remaining space, and therefore the address bar,
    //      constant across the toggle.
    m_viewTogglePanel = new wxPanel(toolbarPanel);
    m_viewToggleIcon = new wxStaticBitmap(m_viewTogglePanel, wxID_ANY,
        wxArtProvider::GetBitmap(wxART_LIST_VIEW, wxART_BUTTON, wxSize(16, 16)));
    m_viewToggleBtn = new wxStaticText(m_viewTogglePanel, wxID_ANY, "Icons");

    {
        wxClientDC measureDC(m_viewToggleBtn);
        measureDC.SetFont(m_viewToggleBtn->GetFont());
        int wIcons = measureDC.GetTextExtent("Icons").GetWidth();
        int wList = measureDC.GetTextExtent("List").GetWidth();
        int maxLabelWidth = std::max(wIcons, wList);
        const int kIconLabelGap = 4;
        int fixedWidth = 16 /*icon*/ + kIconLabelGap + maxLabelWidth;
        m_viewTogglePanel->SetMinSize(wxSize(fixedWidth, -1));
    }

    wxBoxSizer* viewToggleSizer = new wxBoxSizer(wxHORIZONTAL);
    viewToggleSizer->Add(m_viewToggleIcon, 0, wxALIGN_CENTER_VERTICAL);
    viewToggleSizer->Add(m_viewToggleBtn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 4);
    m_viewTogglePanel->SetSizer(viewToggleSizer);

    m_viewTogglePanel->SetCursor(wxCursor(wxCURSOR_HAND));
    m_viewToggleIcon->SetCursor(wxCursor(wxCURSOR_HAND));
    m_viewToggleBtn->SetCursor(wxCursor(wxCURSOR_HAND));

    m_backBtn->Bind(wxEVT_LEFT_UP, &FileExplorerTab::OnBack, this);
    m_forwardBtn->Bind(wxEVT_LEFT_UP, &FileExplorerTab::OnForward, this);
    m_upBtn->Bind(wxEVT_LEFT_UP, &FileExplorerTab::OnUp, this);
    m_addressBar->Bind(wxEVT_TEXT_ENTER, &FileExplorerTab::OnAddressEnter, this);
    m_viewTogglePanel->Bind(wxEVT_LEFT_UP, &FileExplorerTab::OnViewToggleClicked, this);
    m_viewToggleIcon->Bind(wxEVT_LEFT_UP, &FileExplorerTab::OnViewToggleClicked, this);
    m_viewToggleBtn->Bind(wxEVT_LEFT_UP, &FileExplorerTab::OnViewToggleClicked, this);

    wxBoxSizer* toolbar = new wxBoxSizer(wxHORIZONTAL);
    toolbar->Add(m_backBtn, 0, wxALL | wxALIGN_CENTER_VERTICAL, 8);
    toolbar->Add(m_forwardBtn, 0, wxALL | wxALIGN_CENTER_VERTICAL, 8);
    toolbar->Add(m_upBtn, 0, wxALL | wxALIGN_CENTER_VERTICAL, 8);
    toolbar->Add(m_addressBar, 1, wxEXPAND | wxALL, 6);
    // Single sizer item now (m_viewTogglePanel groups the icon+label — see
    // the construction comment above), with a fixed min-size, so this
    // slot's width — and therefore m_addressBar's remaining expand space —
    // stays constant whether the label currently reads "Icons" or "List".
    toolbar->Add(m_viewTogglePanel, 0, wxALIGN_CENTER_VERTICAL | wxALL, 8);
    toolbarPanel->SetSizer(toolbar);

    // --- File list (starts in details view, like Explorer/Finder list
    // mode; ToggleViewMode() switches it to an icon/thumbnail grid) ---
    m_list = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
        wxLC_REPORT | wxBORDER_NONE);
    m_list->InsertColumn(COL_NAME, "Name", wxLIST_FORMAT_LEFT, 320);
    m_list->InsertColumn(COL_SIZE, "Size", wxLIST_FORMAT_RIGHT, 90);
    m_list->InsertColumn(COL_TYPE, "Type", wxLIST_FORMAT_LEFT, 110);
    m_list->InsertColumn(COL_MODIFIED, "Modified", wxLIST_FORMAT_LEFT, 160);
    m_list->SetDropTarget(new ExplorerDropTarget(this, m_list));
    // Bound directly on the list (not the event table) so Ctrl+X/C/V only
    // acts as a file operation while the list itself has keyboard focus —
    // the address bar's own text cut/copy/paste is untouched.
    m_list->Bind(wxEVT_KEY_DOWN, &FileExplorerTab::OnListKeyDown, this);

    // Both lists start with just the two generic icons; ReloadDirectory()
    // clears and rebuilds them from scratch every time (see there for why:
    // per-file thumbnails mean the contents genuinely differ per directory,
    // not just these two fixed entries).
    m_imageListSmall = new wxImageList(kSmallIconPx, kSmallIconPx);
    m_imageListNormal = new wxImageList(kNormalIconPx, kNormalIconPx);
    m_imageListSmall->Add(wxArtProvider::GetBitmap(wxART_FOLDER, wxART_LIST, wxSize(kSmallIconPx, kSmallIconPx)));
    m_imageListSmall->Add(
        wxArtProvider::GetBitmap(wxART_NORMAL_FILE, wxART_LIST, wxSize(kSmallIconPx, kSmallIconPx)));
    m_imageListNormal->Add(
        wxArtProvider::GetBitmap(wxART_FOLDER, wxART_OTHER, wxSize(kNormalIconPx, kNormalIconPx)));
    m_imageListNormal->Add(
        wxArtProvider::GetBitmap(wxART_NORMAL_FILE, wxART_OTHER, wxSize(kNormalIconPx, kNormalIconPx)));
    // AssignImageList() transfers ownership and only needs calling once —
    // RemoveAll()/Add() calls later just mutate these same two objects'
    // contents in place, which the already-assigned wxListCtrl picks up
    // without needing to be told again.
    m_list->AssignImageList(m_imageListSmall, wxIMAGE_LIST_SMALL);
    m_list->AssignImageList(m_imageListNormal, wxIMAGE_LIST_NORMAL);

    // --- Icon/thumbnail grid (see FileIconGrid.h for why this is a
    // custom widget rather than wxListCtrl's own wxLC_ICON mode) — occupies
    // the same sizer slot as m_list; ApplyViewMode() shows exactly one of
    // the two at a time. Starts hidden since m_viewMode defaults to
    // Details. ---
    m_iconGrid = new FileIconGrid(this, this);
    m_iconGrid->SetImageList(m_imageListNormal);
    m_iconGrid->SetDropTarget(new ExplorerDropTarget(this, m_iconGrid));
    m_iconGrid->Hide();

    wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(toolbarPanel, 0, wxEXPAND);
    sizer->Add(m_list, 1, wxEXPAND);
    sizer->Add(m_iconGrid, 1, wxEXPAND);
    SetSizer(sizer);

    NavigateTo(startPath);
}

wxStaticText* FileExplorerTab::MakeNavButton(wxWindow* parent, const wxString& glyph)
{
    wxStaticText* label = new wxStaticText(parent, wxID_ANY, glyph, wxDefaultPosition, wxSize(28, -1),
        wxALIGN_CENTER_HORIZONTAL);
    label->SetFont(wxFont(wxFontInfo(12).Bold()));
    label->SetCursor(wxCursor(wxCURSOR_HAND));
    return label;
}

void FileExplorerTab::ApplyViewMode()
{
    bool showIcons = (m_viewMode == ViewMode::Icons);

    // m_list and m_iconGrid occupy the same sizer slot; whichever isn't
    // showing needs to actually be Hide()n (not just obscured) so it
    // doesn't keep keyboard focus or paint underneath the other one.
    m_list->Show(!showIcons);
    m_iconGrid->Show(showIcons);
    Layout();
    if (showIcons)
        m_iconGrid->SetFocus();
    else
        m_list->SetFocus();

    m_viewToggleBtn->SetLabel(showIcons ? "List" : "Icons");
    m_viewToggleIcon->SetBitmap(wxArtProvider::GetBitmap(
        showIcons ? wxART_REPORT_VIEW : wxART_LIST_VIEW, wxART_BUTTON, wxSize(16, 16)));
}

void FileExplorerTab::ToggleViewMode()
{
    // Carry the selection across the switch — grabbed against the OLD
    // mode before flipping m_viewMode, applied against the NEW one after,
    // since GetSelectedIndices()/SetSelectedIndices() both branch on
    // m_viewMode to know which of m_list/m_iconGrid is authoritative.
    std::vector<int> sel = GetSelectedIndices();
    m_viewMode = (m_viewMode == ViewMode::Details) ? ViewMode::Icons : ViewMode::Details;
    ApplyViewMode();
    SetSelectedIndices(sel);
}

void FileExplorerTab::OnViewToggleClicked(wxMouseEvent&)
{
    ToggleViewMode();
}

void FileExplorerTab::NavigateTo(const wxString& path, bool pushHistory)
{
    // Every path we navigate to is conceptually a directory, so check it
    // directly against the filesystem rather than going through
    // wxFileName(path).IsDir() — that ambiguous single-argument
    // constructor treats a trailing segment with no separator as a
    // *filename*, which silently strips it (e.g. "C:\Users\Admin" became
    // "C:\Users"), making every navigation land one level shallow than
    // requested.
    wxString cleanPath = path;
    if (cleanPath.Length() > 3 && (cleanPath.Last() == '\\' || cleanPath.Last() == '/'))
        cleanPath.RemoveLast();

    if (!wxDir::Exists(cleanPath))
        return;

    if (pushHistory)
    {
        // Drop any "forward" history beyond current position, then push.
        m_history.erase(m_history.begin() + (m_historyPos + 1), m_history.end());
        m_history.push_back(cleanPath);
        m_historyPos = (int)m_history.size() - 1;
    }

    ReloadDirectory();
}

void FileExplorerTab::ReloadDirectory()
{
    if (m_historyPos < 0)
        return;
    wxString path = m_history[m_historyPos];

    m_list->DeleteAllItems();

    wxDir dir(path);
    if (!dir.IsOpened())
    {
        RefreshUI();
        return;
    }

    std::vector<DirEntry> entries;
    wxString name;

    // GetModificationTime()/GetSize() open the file to query its metadata,
    // which routinely fails with access-denied for OS-protected or
    // exclusively-locked entries (DumpStack.log, hiberfil.sys, pagefile.sys,
    // System Volume Information, ...) — completely normal when just
    // listing a directory like C:\, not something the user can fix. wx
    // reports that failure via wxLogSysError, which by default pops a modal
    // error dialog per file. wxLogNull suppresses that for this block only;
    // we still degrade gracefully below by leaving an invalid/zero value,
    // which the display code already renders as a blank field.
    wxLogNull noLog;

    // Rebuilt from scratch every reload: a directory's own set of image
    // thumbnails is entirely different from whatever the previously-shown
    // directory had. Index 0 = folder icon, index 1 = generic file icon in
    // both lists always (added in the same order below). Unrelated to the
    // 1=folder/0=file SetItemPtrData() markers used elsewhere in this file
    // (see the insertion loop below) — that's a separate encoding that just
    // happens to also be a small integer.
    m_imageListSmall->RemoveAll();
    m_imageListNormal->RemoveAll();
    m_imageListSmall->Add(wxArtProvider::GetBitmap(wxART_FOLDER, wxART_LIST, wxSize(kSmallIconPx, kSmallIconPx)));
    m_imageListSmall->Add(
        wxArtProvider::GetBitmap(wxART_NORMAL_FILE, wxART_LIST, wxSize(kSmallIconPx, kSmallIconPx)));
    m_imageListNormal->Add(
        wxArtProvider::GetBitmap(wxART_FOLDER, wxART_OTHER, wxSize(kNormalIconPx, kNormalIconPx)));
    m_imageListNormal->Add(
        wxArtProvider::GetBitmap(wxART_NORMAL_FILE, wxART_OTHER, wxSize(kNormalIconPx, kNormalIconPx)));
    constexpr int kFolderIcon = 0, kGenericFileIcon = 1;
    int thumbnailsBuilt = 0;

    // Prefer the OS's own colored folder icon (mime-theme icon on
    // Linux/GTK, Explorer's own folder icon on Windows) over the generic
    // wxART_FOLDER one just added above, if the platform can supply one —
    // see FileIconProvider.h for why one lookup is enough for every
    // folder in this listing rather than one per folder.
    {
        wxBitmap small, normal;
        if (FileIconProvider::GetTypeIcon(path, /*isDir=*/true, kSmallIconPx, kNormalIconPx, small, normal) &&
            small.IsOk() && normal.IsOk())
        {
            m_imageListSmall->Replace(kFolderIcon, small);
            m_imageListNormal->Replace(kFolderIcon, normal);
        }
    }

    // Colored per-extension icons (the OS's own file-type icon — e.g. a
    // recognizable PDF/ZIP/source-file icon instead of one plain generic
    // file icon for literally everything), cached per extension for this
    // listing: one OS lookup per unique extension, not per file, since
    // both platform implementations key off the extension alone (see
    // FileIconProvider.h). Falls back to kGenericFileIcon wherever the OS
    // can't supply one (including on any platform other than
    // Windows/Linux-GTK, where GetTypeIcon always returns false).
    std::map<wxString, int> extIconCache;
    auto getOrCreateTypeIcon = [&](const wxString& ext) -> int
    {
        wxString key = ext.Lower();
        auto it = extIconCache.find(key);
        if (it != extIconCache.end())
            return it->second;

        int idx = kGenericFileIcon;
        wxBitmap small, normal;
        wxString representative = ext.IsEmpty() ? wxString("file") : ("file." + ext);
        if (FileIconProvider::GetTypeIcon(representative, /*isDir=*/false, kSmallIconPx, kNormalIconPx, small, normal) &&
            small.IsOk() && normal.IsOk())
        {
            idx = m_imageListSmall->Add(small);
            m_imageListNormal->Add(normal);
        }
        extIconCache[key] = idx;
        return idx;
    };

    // Loads path as an image and, on success, adds a matching small+normal
    // pair of scaled-down thumbnails to the two image lists (in lockstep,
    // so they land at the same index in both — see the m_imageListSmall/
    // m_imageListNormal member comment in the header) and returns that
    // index. Tries the OS's own on-demand thumbnail facility first (see
    // FileIconProvider::GetThumbnail — currently only implemented on
    // Windows); everywhere else, and whenever that fails, falls back to
    // this app's own manual wxImage-based decode+scale. If neither can
    // produce a real thumbnail (corrupt file, an extension recognized as
    // an image type but not actually decodable by this wx build's
    // handlers, or either cap below already hit), falls back further to
    // this extension's colored type icon — a real thumbnail is strictly
    // more informative when available, but even a generic image-file
    // icon beats the same flat "unknown file" icon every other type uses.
    auto loadThumbnailOrFallback = [&](const wxString& fullPath, const wxString& ext, wxULongLong fileSize) -> int
    {
        if (thumbnailsBuilt >= kMaxThumbnailsPerListing || fileSize == wxULongLong(-1) ||
            fileSize > kMaxThumbnailFileSize)
            return getOrCreateTypeIcon(ext);

        wxBitmap small, normal;
        if (FileIconProvider::GetThumbnail(fullPath, kSmallIconPx, kNormalIconPx, small, normal) &&
            small.IsOk() && normal.IsOk())
        {
            int idx = m_imageListSmall->Add(small);
            m_imageListNormal->Add(normal);
            ++thumbnailsBuilt;
            return idx;
        }

        wxImage img;
        if (!img.LoadFile(fullPath, wxBITMAP_TYPE_ANY) || !img.IsOk())
            return getOrCreateTypeIcon(ext);

        m_imageListSmall->Add(MakeThumbnailBitmap(img, kSmallIconPx));
        int idx = m_imageListNormal->Add(MakeThumbnailBitmap(img, kNormalIconPx));
        ++thumbnailsBuilt;
        return idx;
    };

    bool cont = dir.GetFirst(&name, wxEmptyString, wxDIR_DIRS);
    while (cont)
    {
        wxFileName full(path, "");
        full.AppendDir(name);
        entries.push_back({ name, true, 0, "Folder", wxFileName(full).GetModificationTime(), kFolderIcon });
        cont = dir.GetNext(&name);
    }
    cont = dir.GetFirst(&name, wxEmptyString, wxDIR_FILES);
    while (cont)
    {
        wxFileName full(path, name);
        wxString ext = full.GetExt();
        wxULongLong size = full.GetSize();
        int imageIndex = IsImageExtension(ext) ? loadThumbnailOrFallback(full.GetFullPath(), ext, size)
                                                : getOrCreateTypeIcon(ext);
        entries.push_back({ name, false, size,
            ext.IsEmpty() ? "File" : ext.Upper() + " File", full.GetModificationTime(), imageIndex });
        cont = dir.GetNext(&name);
    }

    // Folders always sort before files, regardless of which column is
    // active — matches the mockup and standard Explorer/Finder behavior.
    std::sort(entries.begin(), entries.end(), [this](const DirEntry& a, const DirEntry& b) {
        if (a.isDir != b.isDir)
            return a.isDir;
        int cmp = 0;
        switch (m_sortCol)
        {
            case SortColumn::Name:     cmp = a.name.CmpNoCase(b.name); break;
            case SortColumn::Size:     cmp = (a.size < b.size) ? -1 : (a.size > b.size ? 1 : 0); break;
            case SortColumn::Type:     cmp = a.typeLabel.CmpNoCase(b.typeLabel); break;
            case SortColumn::Modified:
                cmp = a.modified.IsEarlierThan(b.modified) ? -1 : (a.modified.IsLaterThan(b.modified) ? 1 : 0);
                break;
        }
        return m_sortAsc ? cmp < 0 : cmp > 0;
    });

    long row = 0;
    for (auto& e : entries)
    {
        long idx = m_list->InsertItem(row++, e.name, e.imageIndex);
        // wxFileName::GetSize() returns this all-bits-set sentinel when it
        // couldn't read the size (e.g. access-denied on a protected/locked
        // system file) — show a blank dash rather than formatting it as a
        // nonsensical multi-million-terabyte value.
        if (!e.isDir)
            m_list->SetItem(idx, COL_SIZE, e.size == wxULongLong(-1) ? "—" : HumanReadableSize(e.size));
        m_list->SetItem(idx, COL_TYPE, e.typeLabel);
        m_list->SetItem(idx, COL_MODIFIED, e.modified.IsValid() ? e.modified.Format("%Y-%m-%d %H:%M") : "");
        m_list->SetItemPtrData(idx, e.isDir ? 1 : 0); // 1 = folder marker, 0 = file marker
    }

    UpdateColumnHeaders();

    // Mirrors m_list's rows 1:1 by index (same `entries` order, inserted
    // in the same loop above) — see the m_iconGrid member comment in the
    // header for why both stay populated regardless of which view is
    // currently visible.
    std::vector<GridEntry> gridEntries;
    gridEntries.reserve(entries.size());
    for (auto& e : entries)
        gridEntries.push_back({ e.name, e.imageIndex, e.isDir });
    m_iconGrid->SetEntries(std::move(gridEntries));

    RefreshUI();
}

void FileExplorerTab::UpdateColumnHeaders()
{
    static const wxString labels[] = { "Name", "Size", "Type", "Modified" };
    for (int col = 0; col < 4; ++col)
    {
        wxString label = labels[col];
        if ((int)m_sortCol == col)
            label += m_sortAsc ? wxT(" \u2191") : wxT(" \u2193"); // ↑ ascending / ↓ descending
        wxListItem item;
        item.SetMask(wxLIST_MASK_TEXT);
        item.SetText(label);
        m_list->SetColumn(col, item);
    }
}

void FileExplorerTab::OnColumnClick(wxListEvent& event)
{
    SortColumn clicked = (SortColumn)event.GetColumn();
    if (clicked == m_sortCol)
        m_sortAsc = !m_sortAsc;
    else
    {
        m_sortCol = clicked;
        // Size and Modified read more naturally biggest/newest-first the
        // first time you click them (finding your largest files or most
        // recent changes is the far more common reason to sort by either
        // one) — matches Explorer/Finder's own default direction for
        // these two columns. Name/Type still default to A-Z.
        m_sortAsc = (clicked != SortColumn::Size && clicked != SortColumn::Modified);
    }
    ReloadDirectory();
}

void FileExplorerTab::RefreshUI()
{
    m_addressBar->ChangeValue(GetCurrentPath());

    m_canGoBack = m_historyPos > 0;
    m_canGoForward = m_historyPos < (int)m_history.size() - 1;
    m_canGoUp = wxFileName::DirName(GetCurrentPath()).GetDirCount() > 0;

    // Standard system "disabled text" gray for the dimmed state, so this
    // matches whatever the user's actual OS theme (light or dark, default
    // or custom accent) considers "disabled" rather than a value we made up.
    const wxColour kActive = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
    const wxColour kDimmed = wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT);

    m_backBtn->SetForegroundColour(m_canGoBack ? kActive : kDimmed);
    m_forwardBtn->SetForegroundColour(m_canGoForward ? kActive : kDimmed);
    m_upBtn->SetForegroundColour(m_canGoUp ? kActive : kDimmed);
    m_backBtn->Refresh();
    m_forwardBtn->Refresh();
    m_upBtn->Refresh();

    if (m_onTitleChange)
        m_onTitleChange(wxFileName::DirName(GetCurrentPath()).GetDirs().empty()
            ? GetCurrentPath() : wxFileName::DirName(GetCurrentPath()).GetDirs().Last());

    if (m_onSelectionChanged)
        m_onSelectionChanged();
}

void FileExplorerTab::OnSelectionChanged(wxListEvent&)
{
    NotifySelectionChanged();
}

void FileExplorerTab::NotifySelectionChanged()
{
    if (m_onSelectionChanged)
        m_onSelectionChanged();
}

wxString FileExplorerTab::GetSelectionInfoText() const
{
    std::vector<int> sel = GetSelectedIndices();
    long selCount = (long)sel.size();

    if (selCount == 0)
    {
        // Nothing selected: show the currently open directory's own info —
        // same Name/Type/Modified shape a selected folder row shows below —
        // rather than a bare item count, so this reads as "here's what
        // you're looking at" and not just a tally.
        wxString path = GetCurrentPath();
        wxFileName dirName = wxFileName::DirName(path);
        wxString name = dirName.GetDirs().IsEmpty() ? path : dirName.GetDirs().Last();

        wxString modified;
        // wxFileName::GetModificationTime() (which GetSelectionInfoText()
        // relies on below) hits a wxWidgets-internal debug assertion when
        // called on a bare drive root ("D:\", 0 directory components):
        // wxFileName::GetPath(wxPATH_GET_VOLUME) special-cases a root and
        // returns it WITH a trailing backslash (needed so "D:" — no
        // backslash — isn't misread as "current directory on D:" instead
        // of "root of D:"), but msw/dir.cpp's wxGetDirectoryTimes() asserts
        // that its input must NOT end in a backslash. Those two wx-internal
        // expectations directly contradict each other for this one case,
        // so we just skip querying it here rather than tripping over it —
        // a drive root's own "modified" date isn't especially meaningful
        // anyway.
        if (!dirName.GetDirs().IsEmpty())
        {
            wxLogNull noLog; // same access-denied-on-metadata rationale as ReloadDirectory
            wxDateTime mtime = dirName.GetModificationTime();
            if (mtime.IsValid())
                modified = mtime.Format("%Y-%m-%d %H:%M");
        }

        wxString info = name + "    Folder";
        if (!modified.IsEmpty())
            info += "    Modified " + modified;
        return info;
    }

    if (selCount == 1)
    {
        long selIdx = sel[0];
        wxString name = m_list->GetItemText(selIdx, COL_NAME);
        wxString type = m_list->GetItemText(selIdx, COL_TYPE);
        wxString size = m_list->GetItemText(selIdx, COL_SIZE);
        wxString modified = m_list->GetItemText(selIdx, COL_MODIFIED);

        wxString info = name;
        if (!size.IsEmpty()) // folders have no size entry
            info += "    " + size;
        
        info += "    " + type;
        
        if (!modified.IsEmpty())
            info += "    Modified " + modified;
        return info;
    }

    // Multiple items selected: a per-item Name/Type/Size/Modified line
    // doesn't generalize, so summarize instead — how many, and the
    // folder/file split (sizes aren't summed here since folder rows don't
    // carry one; re-deriving a directory's total size on the fly would mean
    // walking its tree just to render the info bar).
    int folders = 0, files = 0;
    for (int idx : sel)
    {
        if (m_list->GetItemData(idx) == 1) ++folders; else ++files;
    }

    wxString info = wxString::Format("%ld items selected", selCount);
    wxString breakdown;
    if (folders > 0)
        breakdown += wxString::Format("%d folder%s", folders, folders == 1 ? "" : "s");
    if (files > 0)
    {
        if (!breakdown.IsEmpty())
            breakdown += ", ";
        breakdown += wxString::Format("%d file%s", files, files == 1 ? "" : "s");
    }
    if (!breakdown.IsEmpty())
        info += "    (" + breakdown + ")";
    return info;
}

std::vector<wxString> FileExplorerTab::GetSelectedPaths() const
{
    std::vector<wxString> paths;
    for (int idx : GetSelectedIndices())
    {
        wxString name = m_list->GetItemText(idx);
        paths.push_back(wxFileName(GetCurrentPath(), name).GetFullPath());
    }
    return paths;
}

std::vector<int> FileExplorerTab::GetSelectedIndices() const
{
    if (m_viewMode == ViewMode::Icons)
        return m_iconGrid->GetSelectedIndices();

    std::vector<int> result;
    long idx = m_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    while (idx != wxNOT_FOUND)
    {
        result.push_back((int)idx);
        idx = m_list->GetNextItem(idx, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    }
    return result;
}

void FileExplorerTab::SetSelectedIndices(const std::vector<int>& indices)
{
    if (m_viewMode == ViewMode::Icons)
    {
        m_iconGrid->SetSelectedIndices(indices);
        return;
    }

    long sel = m_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    while (sel != wxNOT_FOUND)
    {
        m_list->SetItemState(sel, 0, wxLIST_STATE_SELECTED);
        sel = m_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    }
    for (int i : indices)
        m_list->SetItemState(i, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
}

void FileExplorerTab::GoUp()
{
    wxFileName fn = wxFileName::DirName(GetCurrentPath());
    if (fn.GetDirCount() > 0)
    {
        fn.RemoveLastDir();
        NavigateTo(fn.GetPath());
    }
}

void FileExplorerTab::GoBack()
{
    if (m_historyPos > 0)
    {
        --m_historyPos;
        ReloadDirectory();
    }
}

void FileExplorerTab::GoForward()
{
    if (m_historyPos < (int)m_history.size() - 1)
    {
        ++m_historyPos;
        ReloadDirectory();
    }
}

void FileExplorerTab::OnAddressEnter(wxCommandEvent&)
{
    NavigateTo(m_addressBar->GetValue());
}

void FileExplorerTab::OnBack(wxMouseEvent&) { if (m_canGoBack) GoBack(); }
void FileExplorerTab::OnForward(wxMouseEvent&) { if (m_canGoForward) GoForward(); }
void FileExplorerTab::OnUp(wxMouseEvent&) { if (m_canGoUp) GoUp(); }

void FileExplorerTab::OnItemActivated(wxListEvent& event)
{
    ActivateItem(event.GetIndex());
}

void FileExplorerTab::ActivateItem(long idx)
{
    wxString name = m_list->GetItemText(idx);
    bool isFolder = m_list->GetItemData(idx) == 1;
    wxString fullPath = wxFileName(GetCurrentPath(), name).GetFullPath();

    if (isFolder)
    {
        wxFileName dirPath(GetCurrentPath(), "");
        dirPath.AppendDir(name);
        NavigateTo(dirPath.GetPath());
    }
    else
    {
        // Open with the OS default application, same as double-clicking
        // in Explorer/Finder.
        if (!wxLaunchDefaultApplication(fullPath))
            wxLogWarning("Could not open '%s' with the default application.", fullPath);
    }
}

void FileExplorerTab::OnBeginDrag(wxListEvent& event)
{
    BeginDragForItem(event.GetIndex());
}

void FileExplorerTab::BeginDragForItem(long idx)
{
    if (idx < 0)
        return;

    // Dragging a row that's part of the current multi-selection drags the
    // whole selection; dragging a row that isn't (a drag started without
    // first selecting it) drags just that one, matching standard
    // Explorer/Finder behavior.
    std::vector<int> sel = GetSelectedIndices();
    std::vector<wxString> names;
    if (std::find(sel.begin(), sel.end(), (int)idx) != sel.end())
    {
        for (int i : sel)
            names.push_back(m_list->GetItemText(i));
    }
    else
    {
        names.push_back(m_list->GetItemText(idx));
    }

    // Payload: source dir on the first line, one dragged item name per line
    // after that. Newlines are safe as a delimiter since filenames can't
    // contain them on any of the platforms this app targets.
    wxString payload = GetCurrentPath();
    for (auto& n : names)
        payload += "\n" + n;

    wxTextDataObject data(payload);
    wxDropSource source(data, this);
    source.DoDragDrop(wxDrag_DefaultMove);
}


bool FileExplorerTab::HandleDrop(const wxPoint& screenPos, const wxString& payload)
{
    wxArrayString lines = wxStringTokenize(payload, "\n", wxTOKEN_RET_EMPTY_ALL);
    if (lines.GetCount() < 2)
        return false;
    wxString srcDir = lines[0];

    // Drop onto a folder item moves the item(s) into that folder; drop
    // anywhere else moves them into the current directory. Hit-tests
    // against whichever of m_list/m_iconGrid is actually visible, since
    // the drop could have landed on either depending on view mode — both
    // share the same row-index space, so m_list is still the right place
    // to ask "is this index a folder, and what's its name" regardless of
    // which one was actually hit-tested.
    wxString destDir = GetCurrentPath();
    int hitIdx = wxNOT_FOUND;
    if (m_viewMode == ViewMode::Details)
    {
        int flags = 0;
        hitIdx = (int)m_list->HitTest(m_list->ScreenToClient(screenPos), flags);
    }
    else
    {
        hitIdx = m_iconGrid->HitTestIndex(m_iconGrid->ScreenToClient(screenPos));
    }
    if (hitIdx != wxNOT_FOUND && hitIdx >= 0 && m_list->GetItemData(hitIdx) == 1)
    {
        wxString hitName = m_list->GetItemText(hitIdx);
        destDir = wxFileName(GetCurrentPath(), "").GetPathWithSep() + hitName;
    }

    if (srcDir == destDir)
        return false; // dropped back where it started

    bool anyMoved = false;
    for (size_t i = 1; i < lines.GetCount(); ++i)
    {
        const wxString& name = lines[i];
        wxString srcPath = wxFileName(srcDir, name).GetFullPath();
        wxString destPath = wxFileName(destDir, name).GetFullPath();

        if (wxFileExists(destPath) || wxDirExists(destPath))
        {
            wxLogWarning("Couldn't move \"%s\" \u2014 something with that name already exists there.", name);
            continue;
        }
        if (!wxRenameFile(srcPath, destPath, false))
        {
            wxLogWarning("Couldn't move \"%s\".", name);
            continue;
        }
        anyMoved = true;
    }

    if (!anyMoved)
        return false;

    ReloadDirectory();
    if (m_onExternalChange)
        m_onExternalChange();
    return true;
}

void FileExplorerTab::OnListKeyDown(wxKeyEvent& event)
{
    if (event.ControlDown() && !event.AltDown() && !event.ShiftDown())
    {
        switch (event.GetKeyCode())
        {
            case 'X': DoCut();   return;
            case 'C': DoCopy();  return;
            case 'V': DoPaste(); return;
        }
    }
    event.Skip();
}

void FileExplorerTab::DoCut()
{
    std::vector<wxString> paths = GetSelectedPaths();
    if (paths.empty())
        return;
    s_clipboardPaths = paths;
    s_clipboardIsCut = true;
}

void FileExplorerTab::DoCopy()
{
    std::vector<wxString> paths = GetSelectedPaths();
    if (paths.empty())
        return;
    s_clipboardPaths = paths;
    s_clipboardIsCut = false;
}

void FileExplorerTab::DoPaste()
{
    if (s_clipboardPaths.empty())
        return;

    wxString destDir = GetCurrentPath();
    bool anyChanged = false;

    for (const wxString& srcPath : s_clipboardPaths)
    {
        wxString itemName = ItemNameFromPath(srcPath);
        wxString destPath = wxFileName(destDir, itemName).GetFullPath();

        if (destPath == srcPath)
            continue; // no-op: pasted into the folder it's already in
        if (wxFileExists(destPath) || wxDirExists(destPath))
        {
            wxLogWarning("Couldn't paste \"%s\" \u2014 something with that name already exists there.", itemName);
            continue;
        }

        bool ok;
        if (s_clipboardIsCut)
        {
            ok = wxRenameFile(srcPath, destPath, false);
            if (!ok)
                wxLogWarning("Couldn't move \"%s\".", itemName);
        }
        else if (wxDirExists(srcPath))
        {
            ok = CopyDirectoryRecursive(srcPath, destPath);
            if (!ok)
                wxLogWarning("Couldn't fully copy \"%s\" \u2014 some items may be missing.", itemName);
        }
        else
        {
            ok = wxCopyFile(srcPath, destPath);
            if (!ok)
                wxLogWarning("Couldn't copy \"%s\".", itemName);
        }
        if (ok)
            anyChanged = true;
    }

    // A cut is a one-shot, like Explorer's: pasting it consumes the
    // clipboard so a second Ctrl+V doesn't move the same (now already-
    // moved) items again. A copy stays put so it can be pasted repeatedly.
    if (s_clipboardIsCut)
        s_clipboardPaths.clear();

    ReloadDirectory();
    if (anyChanged && m_onExternalChange)
        m_onExternalChange();
}

void FileExplorerTab::OnItemRightClick(wxListEvent& event)
{
    ShowContextMenuForItem(event.GetIndex());
}

void FileExplorerTab::ShowContextMenuForItem(long idx)
{
    if (idx < 0)
    {
        // Right-clicked blank space: this is a "background" context menu
        // (New Folder/New File/Paste only), not one that acts on any item,
        // regardless of what's currently selected.
        m_contextTargetPaths.clear();
    }
    else
    {
        // Right-clicking a row that isn't part of the current selection
        // replaces the selection with just that row, matching standard
        // Explorer/Finder behavior. Right-clicking within an existing
        // multi-selection leaves it intact so bulk actions apply to all of
        // it.
        std::vector<int> sel = GetSelectedIndices();
        if (std::find(sel.begin(), sel.end(), (int)idx) == sel.end())
            SetSelectedIndices({ (int)idx });
        m_contextTargetPaths = GetSelectedPaths();
    }

    size_t n = m_contextTargetPaths.size();

    wxMenu menu;
    menu.Append(ID_NewFolder, "New Folder");
    menu.Append(ID_NewFile, "New File");
    if (n > 0)
    {
        menu.AppendSeparator();
        menu.Append(ID_Cut, n == 1 ? "Cut" : wxString::Format("Cut (%zu items)", n));
        menu.Append(ID_CopyClip, n == 1 ? "Copy" : wxString::Format("Copy (%zu items)", n));
    }
    if (!s_clipboardPaths.empty())
        menu.Append(ID_PasteClip, wxString::Format("Paste (%zu items)", s_clipboardPaths.size()));
    if (n > 0)
    {
        menu.AppendSeparator();
        menu.Append(ID_Move, n == 1 ? "Move To..." : wxString::Format("Move %zu Items To...", n));
        menu.Append(ID_Copy, n == 1 ? "Copy To..." : wxString::Format("Copy %zu Items To...", n));
        menu.AppendSeparator();
        if (n == 1)
            menu.Append(ID_Rename, "Rename");
        menu.Append(ID_Delete, n == 1 ? "Delete" : wxString::Format("Delete %zu Items", n));
    }
    if (n <= 1)
    {
        menu.AppendSeparator();
        menu.Append(ID_ShowInfo, "Show More Info");
    }
    PopupMenu(&menu);
}

void FileExplorerTab::OnContextMenu(wxCommandEvent& event)
{
    switch (event.GetId())
    {
        case ID_NewFolder:
        {
            wxString name = wxGetTextFromUser("Folder name:", "New Folder", "New Folder", this);
            if (!name.IsEmpty())
            {
                wxFileName::Mkdir(wxFileName(GetCurrentPath(), "").GetPathWithSep() + name);
                if (m_onExternalChange) m_onExternalChange();
            }
            ReloadDirectory();
            break;
        }
        case ID_NewFile:
        {
            wxString name = wxGetTextFromUser("File name:", "New File", "new_file.txt", this);
            if (!name.IsEmpty())
            {
                wxFile file;
                file.Create(wxFileName(GetCurrentPath(), name).GetFullPath());
                if (m_onExternalChange) m_onExternalChange();
            }
            ReloadDirectory();
            break;
        }
        case ID_Rename:
        {
            if (m_contextTargetPaths.size() != 1)
                break; // menu only offers Rename for a single selected item
            const wxString& targetPath = m_contextTargetPaths[0];
            wxString oldName = ItemNameFromPath(targetPath);
            wxString newName = wxGetTextFromUser("New name:", "Rename", oldName, this);
            if (!newName.IsEmpty() && newName != oldName)
            {
                wxFileName newPath(GetCurrentPath(), newName);
                wxRenameFile(targetPath, newPath.GetFullPath());
                if (m_onExternalChange) m_onExternalChange();
            }
            ReloadDirectory();
            break;
        }
        case ID_Delete:
        {
            if (m_contextTargetPaths.empty())
                break;
            wxString msg = m_contextTargetPaths.size() == 1
                ? "Delete '" + m_contextTargetPaths[0] + "'?"
                : wxString::Format("Delete these %zu items?", m_contextTargetPaths.size());
            if (wxMessageBox(msg, "Confirm Delete", wxYES_NO | wxICON_WARNING, this) == wxYES)
            {
                // Deleting goes through the shell, so items land in the Recycle
                // Bin and can be restored. This previously called wxRemoveFile
                // and wxFileName::Rmdir(wxPATH_RMDIR_RECURSIVE), which unlink
                // directly: a mistaken click on a folder destroyed it and
                // everything under it with nothing to undo. The application
                // looks like Explorer, where Delete is recoverable.
                std::vector<std::wstring> targets;
                targets.reserve(m_contextTargetPaths.size());

                for (const wxString& targetPath : m_contextTargetPaths)
                    targets.push_back(targetPath.ToStdWstring());

                const fileops::DeleteResult result = fileops::MoveToRecycleBin(targets);

                bool anyChanged = false;

                for (const fileops::DeleteOutcome& outcome : result.outcomes)
                {
                    if (outcome.succeeded)
                    {
                        anyChanged = true;
                        continue;
                    }

                    // An item that could not be recycled is reported and left
                    // alone. It is never deleted permanently as a fallback:
                    // silently turning "delete" into "destroy" is the behaviour
                    // this change exists to remove. Offering that choice is the
                    // person's to make.
                    wxLogWarning("Couldn't delete \"%s\": %s",
                                 ItemNameFromPath(wxString(outcome.path.c_str())),
                                 wxString(outcome.error.c_str()));
                }

                if (anyChanged && m_onExternalChange) m_onExternalChange();
            }
            ReloadDirectory();
            break;
        }
        case ID_Move:
        {
            if (m_contextTargetPaths.empty())
                break;
            size_t n = m_contextTargetPaths.size();
            wxString title = n == 1
                ? "Move \"" + ItemNameFromPath(m_contextTargetPaths[0]) + "\" to..."
                : wxString::Format("Move %zu Items To...", n);
            wxDirDialog dlg(this, title, GetCurrentPath());
            if (dlg.ShowModal() == wxID_OK)
            {
                bool anyChanged = false;
                for (const wxString& srcPath : m_contextTargetPaths)
                {
                    wxString itemName = ItemNameFromPath(srcPath);
                    wxString destPath = wxFileName(dlg.GetPath(), itemName).GetFullPath();
                    if (destPath == srcPath)
                    {
                        // no-op: moved to the folder it's already in
                    }
                    else if (wxFileExists(destPath) || wxDirExists(destPath))
                    {
                        wxLogWarning("Couldn't move \"%s\" \u2014 something with that name already exists there.",
                            itemName);
                    }
                    else if (!wxRenameFile(srcPath, destPath, false))
                    {
                        wxLogWarning("Couldn't move \"%s\".", itemName);
                    }
                    else
                    {
                        anyChanged = true;
                    }
                }
                if (anyChanged && m_onExternalChange)
                    m_onExternalChange();
            }
            ReloadDirectory();
            break;
        }
        case ID_Copy:
        {
            if (m_contextTargetPaths.empty())
                break;
            size_t n = m_contextTargetPaths.size();
            wxString title = n == 1
                ? "Copy \"" + ItemNameFromPath(m_contextTargetPaths[0]) + "\" to..."
                : wxString::Format("Copy %zu Items To...", n);
            wxDirDialog dlg(this, title, GetCurrentPath());
            if (dlg.ShowModal() == wxID_OK)
            {
                bool anyChanged = false;
                for (const wxString& srcPath : m_contextTargetPaths)
                {
                    wxString itemName = ItemNameFromPath(srcPath);
                    wxString destPath = wxFileName(dlg.GetPath(), itemName).GetFullPath();
                    bool ok;
                    if (destPath == srcPath)
                    {
                        ok = false; // no-op: copied to the folder it's already in
                    }
                    else if (wxFileExists(destPath) || wxDirExists(destPath))
                    {
                        wxLogWarning("Couldn't copy \"%s\" \u2014 something with that name already exists there.",
                            itemName);
                        ok = false;
                    }
                    else if (wxDirExists(srcPath))
                    {
                        ok = CopyDirectoryRecursive(srcPath, destPath);
                        if (!ok)
                            wxLogWarning("Couldn't fully copy \"%s\" \u2014 some items may be missing.", itemName);
                    }
                    else
                    {
                        ok = wxCopyFile(srcPath, destPath);
                        if (!ok)
                            wxLogWarning("Couldn't copy \"%s\".", itemName);
                    }
                    if (ok)
                        anyChanged = true;
                }
                if (anyChanged && m_onExternalChange)
                    m_onExternalChange();
            }
            ReloadDirectory();
            break;
        }
        case ID_Cut:
            DoCut();
            break;
        case ID_CopyClip:
            DoCopy();
            break;
        case ID_PasteClip:
            DoPaste();
            break;
        case ID_ShowInfo:
        {
            // Background right-click (nothing selected) shows info for the
            // current directory itself; otherwise the single selected item
            // this menu item is only offered for (see OnItemRightClick).
            wxString targetPath = m_contextTargetPaths.empty() ? GetCurrentPath() : m_contextTargetPaths[0];
#ifdef __WXMSW__
            // Opens the exact same native "Properties" dialog Windows
            // Explorer's own right-click menu shows — same tabs (General,
            // Details, Security, ...), same everything, since it's the
            // real OS dialog and not something we're reimplementing.
            SHELLEXECUTEINFOW info = {};
            info.cbSize = sizeof(info);
            info.fMask = SEE_MASK_INVOKEIDLIST;
            info.lpVerb = L"properties";
            info.lpFile = targetPath.wc_str();
            info.nShow = SW_SHOW;
            if (!ShellExecuteExW(&info))
                wxLogWarning("Couldn't open Properties for \"%s\".", ItemNameFromPath(targetPath));
#else
            // No native Properties dialog outside Windows (the actual
            // target platform for this app) — fall back to a plain summary
            // so the feature still does something on other build hosts.
            wxString itemName = ItemNameFromPath(targetPath);
            bool isDir = wxDirExists(targetPath);
            wxFileName fn(targetPath);
            wxString summary = "Name: " + itemName + "\n";
            summary += "Location: " + fn.GetPath() + "\n";
            summary += isDir ? "Type: Folder\n" : "Type: File\n";
            if (!isDir)
                summary += "Size: " + HumanReadableSize(fn.GetSize()) + "\n";
            summary += "Modified: " + fn.GetModificationTime().FormatISOCombined(' ');
            wxMessageBox(summary, itemName + " Properties", wxOK | wxICON_INFORMATION, this);
#endif
            break;
        }
    }
}
