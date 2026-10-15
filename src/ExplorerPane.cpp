#include "ExplorerPane.h"
#include "FileExplorerTab.h"
#include <wx/filename.h>
#include <wx/dir.h>

ExplorerPane::ExplorerPane(wxWindow* parent, const wxString& startPath)
    : wxPanel(parent)
{
    m_notebook = new wxAuiNotebook(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
        wxAUI_NB_TOP | wxAUI_NB_TAB_MOVE | wxAUI_NB_CLOSE_ON_ACTIVE_TAB |
        wxAUI_NB_SCROLL_BUTTONS);

    wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(m_notebook, 1, wxEXPAND);
    SetSizer(sizer);

    BindNotebookEvents();
    AddTab(startPath);
}

ExplorerPane::ExplorerPane(wxWindow* parent, const std::vector<wxString>& tabPaths, int activeIndex,
    const wxString& fallbackStartPath)
    : wxPanel(parent)
{
    m_notebook = new wxAuiNotebook(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
        wxAUI_NB_TOP | wxAUI_NB_TAB_MOVE | wxAUI_NB_CLOSE_ON_ACTIVE_TAB |
        wxAUI_NB_SCROLL_BUTTONS);

    wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(m_notebook, 1, wxEXPAND);
    SetSizer(sizer);

    BindNotebookEvents();

    // Restoring a saved session: skip any path that's since vanished
    // (folder deleted/renamed, a removable drive that isn't plugged in
    // right now, ...) rather than letting one bad entry sink the whole
    // restore.
    int restoredCount = 0;
    for (const wxString& path : tabPaths)
    {
        if (wxDir::Exists(path))
        {
            AddTab(path);
            ++restoredCount;
        }
    }

    if (restoredCount == 0)
    {
        AddTab(fallbackStartPath);
    }
    else if (activeIndex >= 0 && activeIndex < (int)m_notebook->GetPageCount())
    {
        m_notebook->SetSelection(activeIndex);
    }
}

void ExplorerPane::BindNotebookEvents()
{
    // wxEVT_CHILD_FOCUS bubbles up from whichever descendant control (the
    // address bar, the file list, ...) actually received focus, so this
    // fires no matter how deep in the tab the click happened.
    Bind(wxEVT_CHILD_FOCUS, [this](wxChildFocusEvent& e) {
        if (m_onFocused) m_onFocused();
        e.Skip();
    });

    // Switching tabs changes what the "current tab" is, and therefore what
    // GetSelectionInfoText() on it would return, even though nothing about
    // the tab's own selection changed.
    m_notebook->Bind(wxEVT_AUINOTEBOOK_PAGE_CHANGED, [this](wxAuiNotebookEvent& e) {
        if (m_onSelectionChanged) m_onSelectionChanged();
        e.Skip();
    });
}

FileExplorerTab* ExplorerPane::AddTab(const wxString& path)
{
    FileExplorerTab* tab = new FileExplorerTab(m_notebook, path);

    tab->SetTabTitleUpdater([this, tab](const wxString& title) {
        int idx = m_notebook->GetPageIndex(tab);
        if (idx != wxNOT_FOUND)
            m_notebook->SetPageText(idx, title.IsEmpty() ? "/" : title);
    });
    // Forward through a lambda (not a direct copy of the std::function)
    // so tabs still work correctly if MainFrame sets the real handlers
    // after this pane/tab was already constructed.
    tab->SetExternalChangeHandler([this]() {
        if (m_onExternalChange) m_onExternalChange();
    });
    tab->SetSelectionChangedHandler([this]() {
        if (m_onSelectionChanged) m_onSelectionChanged();
    });

    wxFileName fn = wxFileName::DirName(path);
    wxString label = fn.GetDirs().empty() ? path : fn.GetDirs().Last();
    m_notebook->AddPage(tab, label, true);
    return tab;
}

FileExplorerTab* ExplorerPane::CurrentTab()
{
    int sel = m_notebook->GetSelection();
    if (sel == wxNOT_FOUND)
        return nullptr;
    return dynamic_cast<FileExplorerTab*>(m_notebook->GetPage(sel));
}

wxString ExplorerPane::CurrentPath()
{
    FileExplorerTab* tab = CurrentTab();
    return tab ? tab->GetCurrentPath() : wxString();
}

std::vector<wxString> ExplorerPane::GetTabPaths() const
{
    std::vector<wxString> paths;
    for (size_t i = 0; i < m_notebook->GetPageCount(); ++i)
    {
        if (auto* tab = dynamic_cast<FileExplorerTab*>(m_notebook->GetPage(i)))
            paths.push_back(tab->GetCurrentPath());
    }
    return paths;
}

int ExplorerPane::GetActiveTabIndex() const
{
    return m_notebook->GetSelection();
}

void ExplorerPane::NewTab()
{
    wxString path = CurrentPath();
    if (path.IsEmpty())
        path = wxFileName::GetHomeDir();
    AddTab(path);
}

void ExplorerPane::CloseActiveTab()
{
    int sel = m_notebook->GetSelection();
    if (sel != wxNOT_FOUND && m_notebook->GetPageCount() > 1)
        m_notebook->DeletePage(sel);
}
