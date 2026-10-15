#include "MainFrame.h"
#include "TerminalPanel.h"
#include "ExplorerPane.h"
#include "FileExplorerTab.h"
#include "InfoBar.h"
#include <wx/filename.h>
#include <wx/fileconf.h>
#include <wx/stdpaths.h>

namespace
{
    // A dedicated wxFileConfig pointed at an explicit file under the user's
    // own config directory — not wxConfig::Get()'s platform default (the
    // Windows Registry), since the ask was specifically for a config
    // *file*, and a plain ini-style file is also just easier for a user to
    // find, back up, or delete by hand than a registry key would be.
    wxString GetSessionConfigPath()
    {
        wxString dir = wxStandardPaths::Get().GetUserConfigDir();
        wxFileName::Mkdir(dir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
        return wxFileName(dir, "mini-file-explorer.ini").GetFullPath();
    }
}

wxBEGIN_EVENT_TABLE(MainFrame, wxFrame)
    EVT_CLOSE(MainFrame::OnClose)
    EVT_MENU(MainFrame::ID_ToggleTerminal, MainFrame::OnToggleTerminal)
    EVT_MENU(MainFrame::ID_NewTab, MainFrame::OnNewTab)
    EVT_MENU(MainFrame::ID_CloseTab, MainFrame::OnCloseTab)
    EVT_MENU(MainFrame::ID_ToggleSplit, MainFrame::OnToggleSplit)
wxEND_EVENT_TABLE()

MainFrame::MainFrame()
    : wxFrame(nullptr, wxID_ANY, "Mini File Explorer", wxDefaultPosition, wxSize(1200, 780))
{
    m_auiMgr.SetManagedWindow(this);

    LoadSession(); // populates m_session; BuildLayout() below reads it
    BuildMenuBar();
    BuildLayout();

    m_auiMgr.Update();

    // Restoring "terminal was open" has to happen after the panes/AUI
    // manager above are fully set up (SetTerminalOpen ultimately touches
    // m_auiMgr and m_focusedPane), so it can't just be folded into
    // BuildLayout() alongside the tabs/split/sash restoration.
    if (m_session.terminalOpen)
        SetTerminalOpen(true);

    RefreshInfoBar(); // show the startup directory's item count immediately
}

MainFrame::~MainFrame()
{
    m_auiMgr.UnInit();
}

void MainFrame::BuildMenuBar()
{
    wxMenu* fileMenu = new wxMenu();
    fileMenu->Append(ID_NewTab, "New &Tab\tCtrl+T");
    fileMenu->Append(ID_CloseTab, "&Close Tab\tCtrl+W");
    fileMenu->AppendSeparator();
    fileMenu->Append(wxID_EXIT);

    wxMenu* viewMenu = new wxMenu();
    viewMenu->AppendCheckItem(ID_ToggleTerminal, "Toggle &Terminal\tCtrl+`");
    viewMenu->Check(ID_ToggleTerminal, false);
    viewMenu->AppendCheckItem(ID_ToggleSplit, "Toggle &Split View\tCtrl+\\");
    viewMenu->AppendSeparator();
    viewMenu->Append(wxID_ABOUT, "&About");

    wxMenuBar* menuBar = new wxMenuBar();
    menuBar->Append(fileMenu, "&File");
    menuBar->Append(viewMenu, "&View");
    SetMenuBar(menuBar);

    Bind(wxEVT_MENU, [this](wxCommandEvent&) { Close(); }, wxID_EXIT);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        wxMessageBox(
            "Manish Majhi liked how the terminal opens right inside VS Code, "
            "and wanted to build something with wxWidgets. So this app was born.\n\n"
            "It doesn't solve any real problem, and it isn't a special tool; it's just a human doing human things.",
            "About",
            wxOK | wxICON_INFORMATION, this);
    }, wxID_ABOUT);
}

void MainFrame::BuildLayout()
{
    // --- Center: one or two side-by-side ExplorerPanes ---
    m_splitter = new wxSplitterWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
        wxSP_LIVE_UPDATE | wxSP_3DSASH);
    m_splitter->SetMinimumPaneSize(220);

    wxString homeDir = wxFileName::GetHomeDir();
    m_paneLeft = new ExplorerPane(m_splitter, m_session.leftTabs, m_session.leftActiveIndex, homeDir);
    WirePane(m_paneLeft);
    m_focusedPane = m_paneLeft;

    if (m_session.splitActive && !m_session.rightTabs.empty())
    {
        m_paneRight = new ExplorerPane(m_splitter, m_session.rightTabs, m_session.rightActiveIndex, homeDir);
        WirePane(m_paneRight);
        if (m_session.sashPosition > 0)
            m_splitter->SplitVertically(m_paneLeft, m_paneRight, m_session.sashPosition);
        else
            m_splitter->SplitVertically(m_paneLeft, m_paneRight);
    }
    else
    {
        m_splitter->Initialize(m_paneLeft);
    }

    if (GetMenuBar())
        GetMenuBar()->Check(ID_ToggleSplit, m_paneRight != nullptr);

    m_auiMgr.AddPane(m_splitter, wxAuiPaneInfo().Name("panes").CenterPane());

    // --- Bottom: terminal (shared across both panes, like VS Code) ---
    m_terminal = new TerminalPanel(this);
    m_auiMgr.AddPane(m_terminal, wxAuiPaneInfo()
        .Name("terminal").Caption("Terminal")
        .Bottom().Layer(1).BestSize(-1, 260).MinSize(-1, 120)
        .CloseButton(true)
        .Hide()); // closed by default; opened on demand via View > Toggle Terminal

    // --- Also bottom, same slot: a thin info strip, shown whenever the
    // terminal isn't (an empty gap there at startup looked broken) ---
    m_infoBar = new InfoBar(this);
    m_auiMgr.AddPane(m_infoBar, wxAuiPaneInfo()
        .Name("infobar")
        .Bottom().Layer(1).BestSize(-1, 28).MinSize(-1, 28).MaxSize(-1, 28)
        .CaptionVisible(false).CloseButton(false).Floatable(false).Movable(false)
        .Resizable(false).Gripper(false));

    // The pane's own [x] closes it directly, bypassing OnToggleTerminal —
    // without this, the View menu checkbox would stay checked after that,
    // and the info bar wouldn't come back to fill the now-empty slot.
    Bind(wxEVT_AUI_PANE_CLOSE, [this](wxAuiManagerEvent& event) {
        if (event.GetPane() && event.GetPane()->window == m_terminal)
        {
            // The pane's own [x] means "close this terminal" (exit the
            // session), not just "hide it" — distinct from the View menu
            // toggle / Ctrl+`, which preserves the running session so
            // toggling it back shows you right where you left off.
            m_terminal->CloseSession();
            if (GetMenuBar())
                GetMenuBar()->Check(ID_ToggleTerminal, false);
            SetBottomSlotShowsTerminal(false);
        }
        event.Skip();
    });
}

void MainFrame::WirePane(ExplorerPane* pane)
{
    pane->SetExternalChangeHandler([this]() { RefreshAllPanes(); });
    pane->SetFocusHandler([this, pane]() { m_focusedPane = pane; RefreshInfoBar(); });
    pane->SetSelectionChangedHandler([this, pane]() {
        if (pane == m_focusedPane)
            RefreshInfoBar();
    });
}

void MainFrame::RefreshInfoBar()
{
    if (!m_infoBar)
        return;
    FileExplorerTab* tab = m_focusedPane ? m_focusedPane->CurrentTab() : nullptr;
    m_infoBar->SetText(tab ? tab->GetSelectionInfoText() : wxString());
}

void MainFrame::SetBottomSlotShowsTerminal(bool showTerminal)
{
    m_terminalVisible = showTerminal;

    // The terminal and info bar share one bottom AUI dock slot; only one
    // is ever shown. Naively doing that with Show()/Hide() looked right
    // but wasn't: once a wxAUI dock row's rendered height has been
    // established (e.g. stretched to the terminal's 260px BestSize while
    // it was visible), simply hiding that pane and showing a smaller one
    // in the same row does NOT shrink the row back down on its own — wxAUI
    // treats a dock row's height as sticky/persisted once set (by design,
    // so a manual sash-drag resize survives pane show/hide), and a plain
    // Hide()/Show() pair doesn't clear that. The visible effect was the
    // info bar's panel getting stuck at the terminal's last height, with
    // its text sitting in a huge, mostly-empty strip instead of a thin one.
    //
    // Fully detaching whichever pane is being hidden and re-adding the one
    // being shown (rather than merely toggling Show()) forces wxAUI to
    // treat the dock row as brand new and size it purely from whichever
    // pane is actually in it now.
    // Detach BOTH unconditionally first (DetachPane on a window that isn't
    // currently managed is a documented safe no-op) so exactly one of the
    // two is ever a registered AUI pane at a time. Detaching only the one
    // being hidden isn't enough: the one being shown might *already* still
    // be attached too (e.g. from the initial BuildLayout setup, or a prior
    // call to this very function), and calling AddPane on an already-
    // attached pane name hits a "pane with that name already exists"
    // assertion instead of updating it.
    m_auiMgr.DetachPane(m_terminal);
    m_auiMgr.DetachPane(m_infoBar);
    m_terminal->Hide();
    m_infoBar->Hide();

    // This Update() (yes, a second one is needed below) is what actually
    // fixes the dock-stuck-at-the-terminal's-height bug. wxAUI only clears
    // out an empty dock row's cached pixel height when LayoutAll() sees it
    // has zero panes in it at that moment, and only recomputes a dock's
    // height from scratch when it's a genuinely fresh dock (height == 0).
    // Detaching both panes and then immediately adding the next one back
    // — all before ever calling Update() — never gives LayoutAll() a
    // chance to observe the dock as empty even once, so its old (tall)
    // cached height from when the terminal was showing just carries
    // straight over to whatever gets shown next. Forcing a layout pass here,
    // with the dock actually empty, is what makes wxAUI drop that stale
    // dock entirely so the next AddPane below starts from a clean slate.
    m_auiMgr.Update();

    if (showTerminal)
    {
        m_auiMgr.AddPane(m_terminal, wxAuiPaneInfo()
            .Name("terminal").Caption("Terminal")
            .Bottom().Layer(1).BestSize(-1, 260).MinSize(-1, 120)
            .CloseButton(true));
    }
    else
    {
        m_auiMgr.AddPane(m_infoBar, wxAuiPaneInfo()
            .Name("infobar")
            .Bottom().Layer(1).BestSize(-1, 28).MinSize(-1, 28).MaxSize(-1, 28)
            .CaptionVisible(false).CloseButton(false).Floatable(false).Movable(false)
            .Resizable(false).Gripper(false));
    }
    m_auiMgr.Update();
}

void MainFrame::RefreshAllPanes()
{
    // A move/rename/delete in one pane might affect a folder the other
    // pane is also currently showing, so refresh both active tabs.
    if (m_paneLeft && m_paneLeft->CurrentTab())
        m_paneLeft->CurrentTab()->ReloadDirectory();
    if (m_paneRight && m_paneRight->CurrentTab())
        m_paneRight->CurrentTab()->ReloadDirectory();
}

void MainFrame::ToggleSplit()
{
    if (m_paneRight)
    {
        m_splitter->Unsplit(m_paneRight);
        m_paneRight->Destroy();
        m_paneRight = nullptr;
        m_focusedPane = m_paneLeft;
    }
    else
    {
        wxString path = m_paneLeft->CurrentPath();
        if (path.IsEmpty())
            path = wxFileName::GetHomeDir();
        m_paneRight = new ExplorerPane(m_splitter, path);
        WirePane(m_paneRight);
        m_splitter->SplitVertically(m_paneLeft, m_paneRight);
        m_focusedPane = m_paneRight;
    }
    if (GetMenuBar())
        GetMenuBar()->Check(ID_ToggleSplit, m_paneRight != nullptr);
    RefreshInfoBar();
}

void MainFrame::OnNewTab(wxCommandEvent&)
{
    if (m_focusedPane)
        m_focusedPane->NewTab();
}

void MainFrame::OnCloseTab(wxCommandEvent&)
{
    if (m_focusedPane)
        m_focusedPane->CloseActiveTab();
}

void MainFrame::OnToggleSplit(wxCommandEvent&)
{
    ToggleSplit();
}

void MainFrame::SetTerminalOpen(bool open)
{
    SetBottomSlotShowsTerminal(open);
    if (GetMenuBar())
        GetMenuBar()->Check(ID_ToggleTerminal, open);

    if (open)
    {
        // Only takes effect the first time the terminal is ever opened —
        // EnsureShellStarted() is a no-op if a shell session is already
        // running, so re-opening the panel later doesn't yank an
        // already-in-use session over to wherever the explorer happens to
        // be pointed at that moment.
        wxString startDir = m_focusedPane ? m_focusedPane->CurrentPath() : wxString();
        if (startDir.IsEmpty())
            startDir = wxFileName::GetHomeDir();
        m_terminal->EnsureShellStarted(startDir);
        m_terminal->FocusInput();
    }
}

void MainFrame::OnToggleTerminal(wxCommandEvent& event)
{
    SetTerminalOpen(event.IsChecked());
}

void MainFrame::OnClose(wxCloseEvent& event)
{
    SaveSession();
    m_auiMgr.UnInit();
    event.Skip();
}

void MainFrame::LoadSession()
{
    wxString path = GetSessionConfigPath();
    if (!wxFileExists(path))
        return; // first run: m_session stays at its all-default state

    wxFileConfig cfg("", "", path, "", wxCONFIG_USE_LOCAL_FILE);
    if (!cfg.HasGroup("Session"))
        return;

    cfg.Read("Session/SplitActive", &m_session.splitActive, false);
    cfg.Read("Session/TerminalOpen", &m_session.terminalOpen, false);
    long sash = -1;
    cfg.Read("Session/SashPosition", &sash, -1L);
    m_session.sashPosition = (int)sash;

    // Reads one pane's "<group>/Count" + "<group>/PathN" keys into
    // outPaths, silently skipping this pane's data entirely if malformed
    // (missing Count) rather than partially restoring it — ExplorerPane's
    // own restore constructor already re-validates every path against disk
    // on top of this, so the only thing guarded here is the shape of the
    // config file itself (e.g. hand-edited or from a future/older version).
    auto readTabs = [&](const wxString& group, std::vector<wxString>& outPaths, int& outActive) {
        long count = 0;
        if (!cfg.Read(group + "/Count", &count) || count <= 0)
            return;
        for (long i = 0; i < count; ++i)
        {
            wxString p;
            if (cfg.Read(wxString::Format("%s/Path%ld", group.c_str(), i), &p) && !p.IsEmpty())
                outPaths.push_back(p);
        }
        long active = 0;
        cfg.Read(group + "/ActiveIndex", &active, 0L);
        outActive = (int)active;
    };
    readTabs("Session/LeftTabs", m_session.leftTabs, m_session.leftActiveIndex);
    if (m_session.splitActive)
        readTabs("Session/RightTabs", m_session.rightTabs, m_session.rightActiveIndex);
}

void MainFrame::SaveSession()
{
    wxFileConfig cfg("", "", GetSessionConfigPath(), "", wxCONFIG_USE_LOCAL_FILE);
    // Start each save from a clean slate: without this, a run with fewer
    // tabs than the previous one would leave that previous run's higher-
    // numbered PathN keys lingering (Count would shrink, but old entries
    // past the new Count aren't overwritten by the loop below).
    cfg.DeleteGroup("Session");

    cfg.Write("Session/SplitActive", m_paneRight != nullptr);
    cfg.Write("Session/TerminalOpen", m_terminalVisible);
    cfg.Write("Session/SashPosition", (long)m_splitter->GetSashPosition());

    auto writeTabs = [&](const wxString& group, ExplorerPane* pane) {
        if (!pane)
            return;
        std::vector<wxString> paths = pane->GetTabPaths();
        cfg.Write(group + "/Count", (long)paths.size());
        for (size_t i = 0; i < paths.size(); ++i)
            cfg.Write(wxString::Format("%s/Path%zu", group.c_str(), i), paths[i]);
        cfg.Write(group + "/ActiveIndex", (long)pane->GetActiveTabIndex());
    };
    writeTabs("Session/LeftTabs", m_paneLeft);
    writeTabs("Session/RightTabs", m_paneRight);

    cfg.Flush();
}
