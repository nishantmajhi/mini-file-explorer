#pragma once
#include <wx/wx.h>
#include <wx/listctrl.h>
#include <vector>
#include <functional>

class FileIconGrid;

// One tab = one "location". Holds its own back/forward history, like a
// browser tab, and shows the contents of the current directory in a
// details list (name / size / type / date modified). Lives inside an
// ExplorerPane, which may be one of two side-by-side split panes.
class FileExplorerTab : public wxPanel
{
public:
    FileExplorerTab(wxWindow* parent, const wxString& startPath);

    wxString GetCurrentPath() const { return m_history[m_historyPos]; }
    void SetTabTitleUpdater(std::function<void(const wxString&)> cb) { m_onTitleChange = cb; }
    // Called after this tab changes something on disk that another pane's
    // tab might also be showing (a drag-and-drop move, rename, delete, new
    // file/folder) so the app can refresh other views too.
    void SetExternalChangeHandler(std::function<void()> cb) { m_onExternalChange = cb; }
    // Fired whenever the selected item (or lack thereof) changes, or the
    // directory is reloaded — MainFrame uses this to know when to re-pull
    // GetSelectionInfoText() for the bottom info bar. Just a "something
    // changed" ping, not a data callback, so it stays correct regardless of
    // *why* the selection changed.
    void SetSelectionChangedHandler(std::function<void()> cb) { m_onSelectionChanged = cb; }

    // Text for the bottom info bar: details about the selected item(s), or
    // (nothing selected) a summary of the current directory itself. The
    // list allows multi-selection, so more than one item selected shows an
    // aggregate count instead of one item's Name/Type/Size/Modified.
    wxString GetSelectionInfoText() const;

    // Full paths of every currently selected row, in list order. Empty if
    // nothing is selected.
    std::vector<wxString> GetSelectedPaths() const;

    void NavigateTo(const wxString& path, bool pushHistory = true);
    void GoUp();
    void GoBack();
    void GoForward();
    // Reload the current directory listing. Deliberately NOT named
    // Refresh() — wxWindow::Refresh(bool, const wxRect*) is virtual, and a
    // same-named non-virtual member here would hide the whole overload set
    // (triggers -Woverloaded-virtual) and shadow the real repaint-request
    // method for anyone calling it through a wxWindow*.
    void ReloadDirectory();

    // Used by the drop target to figure out where a dragged item landed.
    // screenPos (not client coordinates of any one widget) since the drop
    // can land on either m_list or m_iconGrid depending on view mode — see
    // ExplorerDropTarget's own comment for why.
    bool HandleDrop(const wxPoint& screenPos, const wxString& payload);

    // Switches between the details list and the icon/thumbnail grid.
    // Exposed publicly (rather than just a private button handler) so
    // MainFrame's View menu can toggle whichever pane/tab currently has
    // focus, the same way Ctrl+T/Ctrl+W already reach the focused pane.
    void ToggleViewMode();

    // The following are public because FileIconGrid (a separate class —
    // see FileIconGrid.h for why Icons mode needs one instead of just
    // using wxListCtrl's own wxLC_ICON style) calls them directly to reuse
    // this tab's existing activate/drag/context-menu/clipboard logic
    // rather than duplicating it. Not intended as a general-purpose public
    // API beyond that — idx is always an index into the same row order
    // m_list itself uses (m_list stays fully populated at all times
    // regardless of which view is visible; see the class-level comment
    // above m_list in the private section).
    void NotifySelectionChanged();
    void ActivateItem(long idx);
    void BeginDragForItem(long idx);
    void ShowContextMenuForItem(long idx); // idx == -1 means "blank space" (background menu)

    // Clipboard actions (Ctrl+X/C/V and the matching context menu items).
    // Cut/Copy stash the current selection's paths into the shared
    // clipboard below; Paste applies it against this tab's current
    // directory. Mirrors the same move/copy semantics as drag-and-drop and
    // Move To.../Copy To... (wxRenameFile for moves, recursive copy for
    // directories), just reached via keyboard/menu instead of a picker.
    void DoCut();
    void DoCopy();
    void DoPaste();

private:
    wxTextCtrl* m_addressBar;

    // Nav "buttons" are plain clickable labels, not wxButton. Native themed
    // wxButtons on Windows ignore custom text colors entirely (the OS theme
    // decides), which made disabled arrows nearly invisible on a dark
    // background. wxStaticText honors SetForegroundColour reliably, so we
    // draw enabled/disabled state ourselves instead of relying on the OS.
    wxStaticText* m_backBtn;
    wxStaticText* m_forwardBtn;
    wxStaticText* m_upBtn;
    bool m_canGoBack = false;
    bool m_canGoForward = false;
    bool m_canGoUp = false;

    wxListCtrl* m_list;
    // FileIconGrid is a separate custom-painted widget (see FileIconGrid.h
    // for why) that occupies the same sizer slot as m_list and is shown
    // instead of it in Icons mode. m_list itself stays wxLC_REPORT and
    // fully populated at ALL times regardless of which one is visible —
    // it's the single always-correct source of each row's name/size/type
    // /modified/isDir data by index, which both GetSelectedPaths() and
    // FileIconGrid (via ActivateItem/BeginDragForItem/etc., all keyed by
    // that same row index) rely on.
    FileIconGrid* m_iconGrid;
    // Two parallel image lists, kept in lockstep (every icon/thumbnail is
    // added to both, at the same index, in the same call) — wxListCtrl uses
    // one shared per-item image *index* regardless of which view is active,
    // so both the 16px list (report/details view) and the larger one (icon
    // view) need an entry at that same index or the "wrong" list would show
    // a stale/mismatched icon after a view switch.
    wxImageList* m_imageListSmall;
    wxImageList* m_imageListNormal;

    std::vector<wxString> m_history;
    int m_historyPos = -1;
    std::function<void(const wxString&)> m_onTitleChange;
    std::function<void()> m_onExternalChange;
    std::function<void()> m_onSelectionChanged;

    enum class SortColumn { Name = 0, Size = 1, Type = 2, Modified = 3 };
    SortColumn m_sortCol = SortColumn::Name;
    bool m_sortAsc = true;

    // Details (the sortable Name/Size/Type/Modified report list) or Icons
    // (a grid of larger folder/file icons and, for recognizable image
    // files, actual scaled-down thumbnails instead of a generic file icon).
    enum class ViewMode { Details, Icons };
    ViewMode m_viewMode = ViewMode::Details;
    wxStaticBitmap* m_viewToggleIcon; // shows the mode a click would switch TO, same as the label
    wxStaticText* m_viewToggleBtn; // shows the mode a click would switch TO
    // Groups m_viewToggleIcon + m_viewToggleBtn into a single sizer item
    // and a single click target — see the constructor comment at its
    // construction site for why this exists (two adjacent-but-separate
    // controls left a dead gap between them, and their combined width
    // changing with the label text was resizing the address bar).
    wxPanel* m_viewTogglePanel;

    // Applies m_viewMode: shows m_list and hides m_iconGrid, or vice versa
    // (both occupy the same sizer slot), updates the toggle button's
    // icon+label, and moves keyboard focus to whichever is now visible.
    // Called on toggle; NOT called from ReloadDirectory, since a runtime
    // view switch doesn't need re-fetching directory contents (both m_list
    // and m_iconGrid already hold everything either view needs).
    void ApplyViewMode();

    // Selection, as row indices into m_list's order (which m_iconGrid's
    // own entries also share — see the m_iconGrid member comment above).
    // Branches on m_viewMode since Details keeps selection in m_list's own
    // native state while Icons keeps it in m_iconGrid's — this is what
    // lets GetSelectedPaths(), GetSelectionInfoText(), and the shared
    // ActivateItem()/BeginDragForItem()/ShowContextMenuForItem() work
    // unchanged regardless of which view is currently active.
    std::vector<int> GetSelectedIndices() const;
    void SetSelectedIndices(const std::vector<int>& indices);

    void RefreshUI(); // update nav button states and address bar
    void UpdateColumnHeaders(); // show a \u2191/\u2193 indicator on the active sort column
    wxStaticText* MakeNavButton(wxWindow* parent, const wxString& glyph);

    void OnAddressEnter(wxCommandEvent& event);
    void OnBack(wxMouseEvent& event);
    void OnForward(wxMouseEvent& event);
    void OnUp(wxMouseEvent& event);
    void OnViewToggleClicked(wxMouseEvent& event);
    void OnItemActivated(wxListEvent& event);
    void OnItemRightClick(wxListEvent& event);
    void OnColumnClick(wxListEvent& event);
    void OnBeginDrag(wxListEvent& event);
    void OnContextMenu(wxCommandEvent& event);
    void OnSelectionChanged(wxListEvent& event);
    // Ctrl+X/C/V, scoped to whenever the file list itself has keyboard
    // focus (bound directly on m_list, not the event table), so it doesn't
    // steal ordinary text cut/copy/paste from the address bar.
    void OnListKeyDown(wxKeyEvent& event);

    enum {
        ID_NewFolder = wxID_HIGHEST + 100, ID_NewFile, ID_Rename, ID_Delete, ID_Move, ID_Copy, ID_ShowInfo,
        ID_Cut, ID_CopyClip, ID_PasteClip
    };

    // Paths the right-click context menu applies to — one or more, since
    // the list supports multi-selection. Populated from the current
    // selection when the menu is opened on a selected item (or set to just
    // the clicked item, replacing the selection, when right-clicking
    // outside it); empty when right-clicking blank list space.
    std::vector<wxString> m_contextTargetPaths;

    // In-app clipboard for Cut/Copy/Paste. Deliberately not the real OS
    // clipboard (no wxFileDataObject/CFSTR_PREFERREDDROPEFFECT dance for
    // Explorer-style cut-vs-copy interop) — same scope as drag-and-drop's
    // own in-app-only text payload. Static so every tab/pane in the app
    // (including the other split pane) shares one clipboard, the way a
    // single OS clipboard would.
    static std::vector<wxString> s_clipboardPaths;
    static bool s_clipboardIsCut;

    wxDECLARE_EVENT_TABLE();
};
