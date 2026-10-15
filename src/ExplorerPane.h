#pragma once
#include <wx/wx.h>
#include <wx/aui/aui.h>
#include <functional>
#include <vector>

class FileExplorerTab;

// One side of the (optional) split view. Owns a tabbed notebook of
// FileExplorerTabs. MainFrame creates one of these always, and a second
// one when the user toggles split view — dragging a file from one pane's
// list onto the other's is what makes the split useful.
class ExplorerPane : public wxPanel
{
public:
    explicit ExplorerPane(wxWindow* parent, const wxString& startPath);
    // Restores a previously-saved set of tabs (see MainFrame's session
    // save/load) instead of starting with just one. Any path that no
    // longer exists (deleted folder, now-absent removable drive, ...) is
    // silently skipped rather than failing the whole restore; if that
    // leaves nothing valid, falls back to a single tab at startPath, same
    // as the single-path constructor above.
    ExplorerPane(wxWindow* parent, const std::vector<wxString>& tabPaths, int activeIndex,
        const wxString& fallbackStartPath);

    FileExplorerTab* AddTab(const wxString& path);
    FileExplorerTab* CurrentTab();
    wxString CurrentPath();

    void NewTab();
    void CloseActiveTab();

    // Every open tab's current path, in tab order, and which one is
    // active — everything needed to restore this pane's tabs verbatim on
    // next launch (see MainFrame's session save).
    std::vector<wxString> GetTabPaths() const;
    int GetActiveTabIndex() const;

    void SetExternalChangeHandler(std::function<void()> cb) { m_onExternalChange = cb; }
    // Fired whenever a descendant control (address bar, file list, ...)
    // gets keyboard focus, so MainFrame can track which pane Ctrl+T/Ctrl+W
    // should apply to.
    void SetFocusHandler(std::function<void()> cb) { m_onFocused = cb; }
    // Fired on the active tab's selection changing, its directory
    // reloading, OR the active tab itself switching — anything that could
    // change what GetSelectionInfoText() on the (new) current tab returns.
    void SetSelectionChangedHandler(std::function<void()> cb) { m_onSelectionChanged = cb; }

private:
    wxAuiNotebook* m_notebook;
    std::function<void()> m_onExternalChange;
    std::function<void()> m_onFocused;
    std::function<void()> m_onSelectionChanged;

    void BindNotebookEvents();
};
