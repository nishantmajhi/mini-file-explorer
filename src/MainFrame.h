#pragma once
#include <wx/wx.h>
#include <wx/aui/aui.h>
#include <wx/splitter.h>
#include <vector>

class TerminalPanel;
class ExplorerPane;
class InfoBar;

class MainFrame : public wxFrame
{
public:
    MainFrame();
    ~MainFrame() override;

private:
    wxAuiManager      m_auiMgr;
    wxSplitterWindow* m_splitter = nullptr;
    ExplorerPane*     m_paneLeft = nullptr;
    ExplorerPane*     m_paneRight = nullptr; // nullptr when not split
    ExplorerPane*     m_focusedPane = nullptr;
    TerminalPanel*    m_terminal = nullptr;
    InfoBar*          m_infoBar = nullptr;
    // Tracks which of {terminal, info bar} currently occupies their shared
    // bottom AUI slot — see SetBottomSlotShowsTerminal() — so SaveSession()
    // has a plain flag to read instead of having to infer it from AUI pane
    // state.
    bool m_terminalVisible = false;

    // Everything restored from (or about to be written to) the on-disk
    // session config: which tabs each pane had open, whether split view
    // and the terminal were active, and the splitter's sash position.
    // Populated by LoadSession() before BuildLayout() runs, so BuildLayout
    // can construct the initial panes/split/terminal state directly from
    // this instead of always falling back to the single-tab/no-split/no-
    // terminal defaults.
    struct SessionData
    {
        std::vector<wxString> leftTabs;
        int leftActiveIndex = 0;
        bool splitActive = false;
        std::vector<wxString> rightTabs;
        int rightActiveIndex = 0;
        int sashPosition = -1; // -1 = no saved position; use the splitter's own default
        bool terminalOpen = false;
    };
    SessionData m_session;

    void BuildMenuBar();
    void BuildLayout();
    void WirePane(ExplorerPane* pane);
    void ToggleSplit();
    void RefreshAllPanes();
    // Pulls GetSelectionInfoText() from the focused pane's current tab and
    // pushes it into the info bar. Called any time something that text
    // could depend on changes (selection, directory reload, active tab,
    // focused pane) rather than threading the actual text through every
    // one of those callbacks individually.
    void RefreshInfoBar();
    // Switches which of {terminal, info bar} occupies their shared bottom
    // AUI slot, fully detaching the other rather than just hiding it — see
    // the .cpp for why a plain Show()/Hide() pair isn't enough here.
    void SetBottomSlotShowsTerminal(bool showTerminal);
    // Shared by the View menu toggle and session restore at startup: shows
    // or hides the terminal, keeps the View menu checkbox in sync, and (if
    // opening) starts its shell at the focused pane's current directory.
    void SetTerminalOpen(bool open);

    // Reads the on-disk session config (if any) into m_session. Called
    // from the constructor before BuildLayout(); does nothing (leaving
    // m_session at its all-default state) on first run or if the file is
    // missing/unreadable.
    void LoadSession();
    // Writes the current tabs/split/sash/terminal state to the same config
    // file, overwriting whatever was there. Called from OnClose(), while
    // everything is still alive and queryable.
    void SaveSession();

    void OnNewTab(wxCommandEvent& event);
    void OnCloseTab(wxCommandEvent& event);
    void OnToggleSplit(wxCommandEvent& event);
    void OnClose(wxCloseEvent& event);
    void OnToggleTerminal(wxCommandEvent& event);

    enum { ID_ToggleTerminal = wxID_HIGHEST + 1, ID_NewTab, ID_CloseTab, ID_ToggleSplit };

    wxDECLARE_EVENT_TABLE();
};
