#include "FileIconGrid.h"
#include "FileExplorerTab.h"
#include <wx/dcbuffer.h>
#include <wx/settings.h>
#include <algorithm>
#include <cstdlib>

wxBEGIN_EVENT_TABLE(FileIconGrid, wxScrolledWindow)
    EVT_PAINT(FileIconGrid::OnPaint)
    EVT_SIZE(FileIconGrid::OnSize)
    EVT_LEFT_DOWN(FileIconGrid::OnLeftDown)
    EVT_LEFT_UP(FileIconGrid::OnLeftUp)
    EVT_LEFT_DCLICK(FileIconGrid::OnLeftDClick)
    EVT_RIGHT_DOWN(FileIconGrid::OnRightDown)
    EVT_MOTION(FileIconGrid::OnMotion)
    EVT_KEY_DOWN(FileIconGrid::OnKeyDown)
    EVT_ERASE_BACKGROUND(FileIconGrid::OnEraseBackground)
wxEND_EVENT_TABLE()

FileIconGrid::FileIconGrid(wxWindow* parent, FileExplorerTab* owner)
    : wxScrolledWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL | wxBORDER_NONE | wxWANTS_CHARS)
    , m_owner(owner)
{
    SetScrollRate(0, 10);
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetBackgroundColour(*wxWHITE);
}

void FileIconGrid::SetEntries(std::vector<GridEntry> entries)
{
    m_entries = std::move(entries);
    m_selected.clear();
    m_focusIndex = -1;
    Relayout();
    Refresh();
}

void FileIconGrid::Relayout()
{
    auto computeAndApply = [&](int width)
    {
        m_cols = std::max(1, width / m_cellW);
        int rows = m_entries.empty() ? 0 : (((int)m_entries.size() + m_cols - 1) / m_cols);
        SetVirtualSize(width, rows * m_cellH);
    };

    int w = GetClientSize().GetWidth();
    computeAndApply(w);

    // Adding/removing the vertical scrollbar as a result of the virtual
    // size we just set can itself change the client width (the scrollbar
    // takes real estate) — redo once with the corrected width so the
    // column count that's actually visible matches what we just laid out.
    int w2 = GetClientSize().GetWidth();
    if (w2 != w)
        computeAndApply(w2);
}

int FileIconGrid::IndexAt(const wxPoint& pos) const
{
    if (pos.x < 0 || pos.y < 0 || m_cols <= 0)
        return -1;
    int col = pos.x / m_cellW;
    if (col >= m_cols)
        return -1; // clicked in the empty margin right of the last column
    int row = pos.y / m_cellH;
    int idx = row * m_cols + col;
    if (idx < 0 || idx >= (int)m_entries.size())
        return -1;
    return idx;
}

wxRect FileIconGrid::CellRect(int index) const
{
    if (index < 0 || index >= (int)m_entries.size() || m_cols <= 0)
        return wxRect();
    int col = index % m_cols;
    int row = index / m_cols;
    return wxRect(col * m_cellW, row * m_cellH, m_cellW, m_cellH);
}

void FileIconGrid::SelectOnly(int index)
{
    m_selected.clear();
    if (index >= 0)
        m_selected.insert(index);
}

void FileIconGrid::ToggleSelect(int index)
{
    if (index < 0)
        return;
    if (m_selected.count(index))
        m_selected.erase(index);
    else
        m_selected.insert(index);
}

void FileIconGrid::RangeSelectTo(int index)
{
    if (index < 0)
        return;
    if (m_focusIndex < 0)
    {
        SelectOnly(index);
        return;
    }
    int lo = std::min(m_focusIndex, index), hi = std::max(m_focusIndex, index);
    for (int i = lo; i <= hi; ++i)
        m_selected.insert(i);
}

std::vector<int> FileIconGrid::GetSelectedIndices() const
{
    return std::vector<int>(m_selected.begin(), m_selected.end());
}

void FileIconGrid::SetSelectedIndices(const std::vector<int>& indices)
{
    m_selected.clear();
    for (int i : indices)
        m_selected.insert(i);
    Refresh();
}

void FileIconGrid::ClearSelection()
{
    if (!m_selected.empty())
    {
        m_selected.clear();
        Refresh();
    }
}

void FileIconGrid::EnsureIndexVisible(int index)
{
    wxRect r = CellRect(index);
    if (r.IsEmpty())
        return;

    int xu, yu;
    GetScrollPixelsPerUnit(&xu, &yu);
    if (yu <= 0)
        return;

    int viewTop = GetViewStart().y * yu;
    int viewHeight = GetClientSize().GetHeight();

    if (r.GetTop() < viewTop)
        Scroll(-1, r.GetTop() / yu);
    else if (r.GetBottom() > viewTop + viewHeight)
        Scroll(-1, (r.GetBottom() - viewHeight + yu) / yu);
}

void FileIconGrid::NotifyOwnerSelectionChanged()
{
    if (m_owner)
        m_owner->NotifySelectionChanged();
}

void FileIconGrid::OnPaint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);
    DoPrepareDC(dc); // shifts dc's origin by the current scroll offset, so the drawing below can use CellRect()'s virtual/unscrolled coordinates directly

    dc.SetBackground(wxBrush(GetBackgroundColour()));
    dc.Clear();

    if (m_entries.empty() || m_cols <= 0)
        return;

    // Skip rows that aren't actually in the update region — cheap and
    // meaningful for a directory with hundreds of entries, since only a
    // screenful is ever really being repainted at once.
    int xu, yu;
    GetScrollPixelsPerUnit(&xu, &yu);
    int scrollOffsetY = GetViewStart().y * (yu > 0 ? yu : 0);
    wxRect updateBox = GetUpdateRegion().GetBox();
    int firstRow = std::max(0, (updateBox.GetTop() + scrollOffsetY) / m_cellH);
    int totalRows = ((int)m_entries.size() + m_cols - 1) / m_cols;
    int lastRow = std::min(totalRows - 1, (updateBox.GetBottom() + scrollOffsetY) / m_cellH);

    wxColour selBg = wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHT);
    wxColour selFg = wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHTTEXT);

    int iconW = 0, iconH = 0;
    if (m_imageList)
        m_imageList->GetSize(0, iconW, iconH);

    for (int row = firstRow; row <= lastRow; ++row)
    {
        for (int col = 0; col < m_cols; ++col)
        {
            int idx = row * m_cols + col;
            if (idx >= (int)m_entries.size())
                break;

            const GridEntry& e = m_entries[idx];
            wxRect cell = CellRect(idx);
            bool selected = m_selected.count(idx) != 0;

            if (selected)
            {
                dc.SetBrush(wxBrush(selBg));
                dc.SetPen(*wxTRANSPARENT_PEN);
                dc.DrawRoundedRectangle(cell.Deflate(4, 2), 4);
            }
            else if (idx == m_focusIndex)
            {
                dc.SetBrush(*wxTRANSPARENT_BRUSH);
                dc.SetPen(wxPen(selBg, 1, wxPENSTYLE_DOT));
                dc.DrawRoundedRectangle(cell.Deflate(4, 2), 4);
            }

            if (m_imageList && e.imageIndex >= 0 && iconW > 0)
            {
                int iconX = cell.x + (cell.width - iconW) / 2;
                int iconY = cell.y + 6;
                m_imageList->Draw(e.imageIndex, dc, iconX, iconY, wxIMAGELIST_DRAW_TRANSPARENT);
            }

            dc.SetTextForeground(selected ? selFg : *wxBLACK);
            int maxTextWidth = cell.width - 6;
            wxString shown = wxControl::Ellipsize(e.label, dc, wxELLIPSIZE_END, maxTextWidth);
            wxSize textSz = dc.GetTextExtent(shown);
            int textX = cell.x + (cell.width - textSz.GetWidth()) / 2;
            int textY = cell.y + cell.height - textSz.GetHeight() - 6;
            dc.DrawText(shown, textX, textY);
        }
    }
}

void FileIconGrid::OnSize(wxSizeEvent& event)
{
    Relayout();
    Refresh();
    event.Skip();
}

void FileIconGrid::OnLeftDown(wxMouseEvent& event)
{
    SetFocus();
    m_dragCandidateIdx = IndexAt(CalcUnscrolledPosition(event.GetPosition()));
    m_dragStartPos = event.GetPosition();
    m_dragging = false;
    event.Skip();
}

void FileIconGrid::OnMotion(wxMouseEvent& event)
{
    if (event.Dragging() && event.LeftIsDown() && m_dragCandidateIdx >= 0 && !m_dragging)
    {
        wxPoint delta = event.GetPosition() - m_dragStartPos;
        if (std::abs(delta.x) > 4 || std::abs(delta.y) > 4)
        {
            long idx = m_dragCandidateIdx;
            m_dragCandidateIdx = -1;
            m_dragging = true;
            if (m_owner)
                m_owner->BeginDragForItem(idx); // blocks until the OS drag-drop loop finishes
            m_dragging = false;
        }
    }
    event.Skip();
}

void FileIconGrid::OnLeftUp(wxMouseEvent& event)
{
    m_dragCandidateIdx = -1;
    if (m_dragging)
    {
        m_dragging = false;
        return; // the drag (started from OnMotion) already fully handled this interaction
    }

    int idx = IndexAt(CalcUnscrolledPosition(event.GetPosition()));
    if (idx < 0)
    {
        ClearSelection();
        m_focusIndex = -1;
    }
    else if (event.ShiftDown())
    {
        RangeSelectTo(idx);
        m_focusIndex = idx;
    }
    else if (event.ControlDown())
    {
        ToggleSelect(idx);
        m_focusIndex = idx;
    }
    else
    {
        SelectOnly(idx);
        m_focusIndex = idx;
    }
    Refresh();
    NotifyOwnerSelectionChanged();
}

void FileIconGrid::OnLeftDClick(wxMouseEvent& event)
{
    int idx = IndexAt(CalcUnscrolledPosition(event.GetPosition()));
    if (idx >= 0 && m_owner)
        m_owner->ActivateItem(idx);
}

void FileIconGrid::OnRightDown(wxMouseEvent& event)
{
    SetFocus();
    int idx = IndexAt(CalcUnscrolledPosition(event.GetPosition()));
    if (m_owner)
        m_owner->ShowContextMenuForItem(idx); // -1 means background (blank grid space)
}

void FileIconGrid::OnKeyDown(wxKeyEvent& event)
{
    if (event.ControlDown() && !event.AltDown() && !event.ShiftDown())
    {
        switch (event.GetKeyCode())
        {
        case 'X': if (m_owner) m_owner->DoCut();   return;
        case 'C': if (m_owner) m_owner->DoCopy();  return;
        case 'V': if (m_owner) m_owner->DoPaste(); return;
        case 'A':
            m_selected.clear();
            for (size_t i = 0; i < m_entries.size(); ++i)
                m_selected.insert((int)i);
            Refresh();
            NotifyOwnerSelectionChanged();
            return;
        default: break;
        }
    }

    if (m_entries.empty())
    {
        event.Skip();
        return;
    }

    int key = event.GetKeyCode();
    int newIndex = m_focusIndex;
    switch (key)
    {
    case WXK_RIGHT: newIndex = (m_focusIndex < 0) ? 0 : std::min((int)m_entries.size() - 1, m_focusIndex + 1); break;
    case WXK_LEFT:  newIndex = (m_focusIndex < 0) ? 0 : std::max(0, m_focusIndex - 1); break;
    case WXK_DOWN:  newIndex = (m_focusIndex < 0) ? 0 : std::min((int)m_entries.size() - 1, m_focusIndex + m_cols); break;
    case WXK_UP:    newIndex = (m_focusIndex < 0) ? 0 : std::max(0, m_focusIndex - m_cols); break;
    case WXK_HOME:  newIndex = 0; break;
    case WXK_END:   newIndex = (int)m_entries.size() - 1; break;
    case WXK_RETURN:
    case WXK_NUMPAD_ENTER:
        if (m_focusIndex >= 0 && m_owner)
            m_owner->ActivateItem(m_focusIndex);
        return;
    default:
        event.Skip();
        return;
    }

    if (newIndex != m_focusIndex)
    {
        m_focusIndex = newIndex;
        SelectOnly(newIndex);
        EnsureIndexVisible(newIndex);
        Refresh();
        NotifyOwnerSelectionChanged();
    }
}

void FileIconGrid::OnEraseBackground(wxEraseEvent&)
{
    // Deliberately does nothing: OnPaint fills the entire client area
    // itself every time, so a separate erase pass would only add flicker.
}
