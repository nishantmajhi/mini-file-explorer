#pragma once
#include <wx/wx.h>
#include <wx/scrolwin.h>
#include <wx/imaglist.h>
#include <set>
#include <vector>

class FileExplorerTab;

// One entry's worth of what the grid needs to paint itself — mirrors
// FileExplorerTab's own `entries` order 1:1 by index, which is also the
// same order/index m_list (kept populated but hidden while the grid is
// showing) uses, so an index here always means the same file/folder in
// both places.
struct GridEntry
{
    wxString label;
    int imageIndex;
    bool isDir;
};

// Why this exists instead of wxListCtrl's own wxLC_ICON mode: that mode's
// generic (non-native) implementation — which is what's used on every
// non-MSW platform this app targets, per the comment on the wxLC_ICON
// workaround this class replaces — lays items out column-major, wrapping
// into a new column purely based on the control's HEIGHT. Verified
// directly (both wxLIST_ALIGN_TOP and wxLIST_ALIGN_LEFT produce identical
// column-major layouts, and SetItemPosition() returns false/is a no-op on
// this implementation): there's no style flag or manual-positioning
// workaround that makes it wrap row-major based on WIDTH the way a real
// file manager's icon grid — or CSS flexbox with flex-wrap, which is
// exactly what this class imitates — actually behaves. Selection,
// keyboard nav, drag source, and context menu are all handled directly
// here (not delegated to a hidden m_list) since a hidden/zero-sized
// wxListCtrl can't reliably hold keyboard focus; FileExplorerTab's shared
// action methods (ActivateItem, BeginDragForItem, ShowContextMenuForItem,
// DoCut/DoCopy/DoPaste) are what keep this from duplicating logic that
// Details-mode already has via wxListCtrl's native behavior.
class FileIconGrid : public wxScrolledWindow
{
public:
    FileIconGrid(wxWindow* parent, FileExplorerTab* owner);

    void SetImageList(wxImageList* imageList) { m_imageList = imageList; }

    // Replaces the grid's contents and resets selection/focus — called
    // once per ReloadDirectory(), right after m_list itself is repopulated
    // with the same entries in the same order.
    void SetEntries(std::vector<GridEntry> entries);

    std::vector<int> GetSelectedIndices() const;
    void SetSelectedIndices(const std::vector<int>& indices);
    void ClearSelection();

    // For the drop target: translates a client-area point (already
    // ScreenToClient()'d against this window) into the entry index under
    // it, or -1 if none. Same index space as SetEntries()/m_list.
    int HitTestIndex(const wxPoint& clientPos) const { return IndexAt(CalcUnscrolledPosition(clientPos)); }

private:
    FileExplorerTab* m_owner;
    wxImageList* m_imageList = nullptr;
    std::vector<GridEntry> m_entries;
    std::set<int> m_selected;
    int m_focusIndex = -1;

    // Cell size is fixed (derived once from the icon box size + a
    // generous label allowance), so the only thing that changes on
    // resize is how many of them fit per row — exactly the "everything
    // else about a flex item stays the same size, only the wrap point
    // moves" behavior being asked for.
    int m_cellW = 96;
    int m_cellH = 100;
    int m_cols = 1;

    // Drag-start detection: a plain click shouldn't immediately count as
    // a drag, so OnLeftDown only records a candidate + position, and
    // OnMotion promotes it to a real drag once the mouse has moved past a
    // small threshold — mirrors standard Explorer/Finder click-vs-drag
    // feel, and matches how wxListCtrl's own EVT_LIST_BEGIN_DRAG behaves.
    long m_dragCandidateIdx = -1;
    wxPoint m_dragStartPos;
    bool m_dragging = false;

    void Relayout(); // recomputes m_cols + virtual size from the current client width
    int IndexAt(const wxPoint& posInVirtualCoords) const; // -1 if none
    wxRect CellRect(int index) const;
    void SelectOnly(int index);
    void ToggleSelect(int index);
    void RangeSelectTo(int index);
    void EnsureIndexVisible(int index);
    void NotifyOwnerSelectionChanged();

    void OnPaint(wxPaintEvent& event);
    void OnSize(wxSizeEvent& event);
    void OnLeftDown(wxMouseEvent& event);
    void OnLeftUp(wxMouseEvent& event);
    void OnLeftDClick(wxMouseEvent& event);
    void OnRightDown(wxMouseEvent& event);
    void OnMotion(wxMouseEvent& event);
    void OnKeyDown(wxKeyEvent& event);
    void OnEraseBackground(wxEraseEvent& event); // no-op: OnPaint covers the whole client area itself

    wxDECLARE_EVENT_TABLE();
};
