#include "TerminalPanel.h"
#include <wx/clipbrd.h>
#include <wx/dcbuffer.h>
#include <wx/settings.h>
#include <algorithm>
#include <cstdlib>

namespace
{
enum { ID_POLL_TIMER = wxID_HIGHEST + 500, ID_BLINK_TIMER };

// Poll frequently enough that typing and command output both feel
// immediate (real terminals are effectively zero-latency), but not so
// often it burns CPU doing nothing between keystrokes.
constexpr int kPollIntervalMs = 33;
constexpr int kCursorBlinkMs = 500;
constexpr int kWheelLinesPerNotch = 3;

void AppendUtf8(std::string& out, char32_t ch)
{
    if (ch < 0x80)
    {
        out.push_back((char)ch);
    }
    else if (ch < 0x800)
    {
        out.push_back((char)(0xC0 | (ch >> 6)));
        out.push_back((char)(0x80 | (ch & 0x3F)));
    }
    else if (ch < 0x10000)
    {
        out.push_back((char)(0xE0 | (ch >> 12)));
        out.push_back((char)(0x80 | ((ch >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (ch & 0x3F)));
    }
    else
    {
        out.push_back((char)(0xF0 | (ch >> 18)));
        out.push_back((char)(0x80 | ((ch >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((ch >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (ch & 0x3F)));
    }
}
} // namespace

wxBEGIN_EVENT_TABLE(TerminalPanel, wxWindow)
    EVT_PAINT(TerminalPanel::OnPaint)
    EVT_SIZE(TerminalPanel::OnSize)
    EVT_TIMER(ID_POLL_TIMER, TerminalPanel::OnTimer)
    EVT_TIMER(ID_BLINK_TIMER, TerminalPanel::OnBlinkTimer)
    EVT_KEY_DOWN(TerminalPanel::OnKeyDown)
    EVT_CHAR(TerminalPanel::OnChar)
    EVT_MOUSEWHEEL(TerminalPanel::OnMouseWheel)
    EVT_LEFT_DOWN(TerminalPanel::OnMouseDown)
    EVT_MOTION(TerminalPanel::OnMouseMove)
    EVT_LEFT_UP(TerminalPanel::OnMouseUp)
    EVT_SET_FOCUS(TerminalPanel::OnSetFocus)
    EVT_KILL_FOCUS(TerminalPanel::OnKillFocus)
    EVT_ERASE_BACKGROUND(TerminalPanel::OnEraseBackground)
wxEND_EVENT_TABLE()

TerminalPanel::TerminalPanel(wxWindow* parent)
    : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxWANTS_CHARS | wxBORDER_NONE),
      m_pollTimer(this, ID_POLL_TIMER),
      m_blinkTimer(this, ID_BLINK_TIMER),
      m_font(10, wxFONTFAMILY_TELETYPE, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL)
{
    // We paint every pixel of the client area ourselves every time
    // (background fill included), so telling wx not to also erase the
    // background first avoids a flicker pass with no benefit.
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetBackgroundColour(*wxWHITE);

    MeasureFont();
    m_emu = std::make_unique<TerminalEmulator>(80, 24); // resized to fit as soon as we're laid out
}

TerminalPanel::~TerminalPanel()
{
    m_pollTimer.Stop();
    m_blinkTimer.Stop();
    if (m_pty)
        m_pty->Terminate();
}

void TerminalPanel::MeasureFont()
{
    // A wxClientDC on a window that isn't shown/laid out yet can report
    // unreliable metrics on some platforms; measuring against an
    // in-memory bitmap works consistently regardless of the window's
    // current state.
    wxBitmap dummy(1, 1);
    wxMemoryDC dc(dummy);
    dc.SetFont(m_font);
    wxSize extent = dc.GetTextExtent("M");
    m_charW = std::max(1, extent.GetWidth());
    m_charH = std::max(1, extent.GetHeight());
}

wxSize TerminalPanel::GridSize() const
{
    wxSize client = GetClientSize();
    int cols = std::max(1, client.GetWidth() / m_charW);
    int rows = std::max(1, client.GetHeight() / m_charH);
    return wxSize(cols, rows);
}

void TerminalPanel::PushSizeToBackendAndEmulator()
{
    wxSize g = GridSize();
    if (m_emu) m_emu->Resize(g.GetWidth(), g.GetHeight());
    if (m_pty) m_pty->Resize(g.GetWidth(), g.GetHeight());
    ClampScrollOffset();
}

void TerminalPanel::ScrollToBottom() { m_scrollOffset = 0; }

void TerminalPanel::ClampScrollOffset()
{
    int maxOffset = m_emu ? m_emu->ScrollbackLineCount() : 0;
    m_scrollOffset = std::clamp(m_scrollOffset, 0, maxOffset);
}

int TerminalPanel::LogicalLineCount() const
{
    return m_emu ? (m_emu->ScrollbackLineCount() + m_emu->Rows()) : 0;
}

const TermCell* TerminalPanel::LogicalLine(int index, int* outLen) const
{
    *outLen = 0;
    if (!m_emu || index < 0)
        return nullptr;

    int sb = m_emu->ScrollbackLineCount();
    if (index < sb)
    {
        const auto& line = m_emu->ScrollbackLine(index);
        *outLen = (int)line.size();
        return line.empty() ? nullptr : line.data();
    }

    int row = index - sb;
    if (row >= m_emu->Rows())
        return nullptr;
    *outLen = m_emu->Cols();
    return m_emu->RowPtr(row);
}

// ---------------------------------------------------------------------
// Session lifecycle
// ---------------------------------------------------------------------

void TerminalPanel::EnsureShellStarted(const wxString& startDir)
{
    if (m_pty && !m_pty->HasExited())
        return; // already running — leave its cwd and session alone

    if (m_pty)
        m_pty.reset(); // a previous session in this panel already ended; start fresh

    StartShell(startDir);
}

void TerminalPanel::StartShell(const wxString& startDir)
{
    m_pty = CreatePtyBackend();
    m_exitNoticeShown = false;
    wxSize g = GridSize();

    wxString programPath;
    std::vector<wxString> args;
#ifdef __WXMSW__
    // With a real ConPTY behind it, cmd.exe behaves exactly like it would
    // in a normal console window — no more of the old "/Q echo-off"
    // workaround the previous pipe-based implementation needed to stop it
    // double-echoing every command it read back from a redirected stdin
    // pipe (ConPTY gives it what looks like a real console, so it never
    // falls into that pipe-echo behavior in the first place).
    programPath = "cmd.exe";
#else
    programPath = wxGetenv("SHELL") ? wxGetenv("SHELL") : "/bin/bash";
    args = { "-i" }; // interactive, so prompt/aliases/history behave normally
#endif

    wxString err;
    if (!m_pty->Spawn(programPath, args, startDir, g.GetWidth(), g.GetHeight(), &err))
    {
        FeedInfoLine("[error] failed to start shell: " + err);
        m_pty.reset();
        return;
    }

    FeedInfoLine(wxString::Format("[started shell pid %ld]", m_pty->GetPid()));
    m_pollTimer.Start(kPollIntervalMs);
    m_blinkTimer.Start(kCursorBlinkMs);
}

void TerminalPanel::CloseSession()
{
    m_pollTimer.Stop();
    m_blinkTimer.Stop();
    if (m_pty)
    {
        m_pty->Terminate();
        m_pty.reset();
    }

    // A closed terminal reopening to a blank slate (not the previous
    // session's leftover scrollback) is the whole point of this being a
    // distinct action from just hiding the panel.
    wxSize g = GridSize();
    m_emu = std::make_unique<TerminalEmulator>(g.GetWidth(), g.GetHeight());
    m_scrollOffset = 0;
    ClearSelection();
    Refresh();
}

void TerminalPanel::RunCommand(const wxString& cmd)
{
    // Simulate the user having typed `cmd` at the current prompt and
    // pressed Enter — a real terminal sends CR for Enter, so \r here (not
    // \n) matches actually typing it.
    SendBytes((cmd + "\r").utf8_string());
}

void TerminalPanel::FocusInput()
{
    SetFocus();
}

void TerminalPanel::FeedInfoLine(const wxString& text)
{
    if (!m_emu) return;
    std::string s = (text + "\r\n").utf8_string();
    m_emu->Feed(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    Refresh();
}

// ---------------------------------------------------------------------
// Input -> pty
// ---------------------------------------------------------------------

void TerminalPanel::SendBytes(const std::string& bytes)
{
    if (!m_pty || bytes.empty())
        return;
    m_pty->Write(bytes);
    ScrollToBottom(); // typing always jumps back to the live view, like any real terminal
}

void TerminalPanel::SendUtf8(char32_t ch)
{
    std::string bytes;
    AppendUtf8(bytes, ch);
    SendBytes(bytes);
}

bool TerminalPanel::EncodeSpecialKey(wxKeyEvent& event)
{
    const int key = event.GetKeyCode();
    const bool ctrl = event.ControlDown();
    const bool shift = event.ShiftDown();

    // Copy/paste shortcuts take priority over everything else — Ctrl+C
    // alone must still reach the shell as an interrupt (0x03), which is
    // exactly why real terminals use Ctrl+Shift+C/V for clipboard instead.
    if (ctrl && shift && (key == 'C' || key == 'c')) { CopySelectionToClipboard(); return true; }
    if (ctrl && shift && (key == 'V' || key == 'v')) { PasteFromClipboard(); return true; }

    if (!m_pty)
        return false; // no shell running: let plain typing fall through harmlessly to OnChar (which also no-ops)

    const char* csi = (m_emu && m_emu->ApplicationCursorKeys()) ? "\x1bO" : "\x1b[";

    switch (key)
    {
    case WXK_RETURN: case WXK_NUMPAD_ENTER: SendBytes("\r"); return true;
    case WXK_BACK:   SendBytes("\x7f"); return true;
    case WXK_TAB:    SendBytes(shift ? "\x1b[Z" : "\t"); return true;
    case WXK_ESCAPE: SendBytes("\x1b"); return true;
    case WXK_UP:     SendBytes(std::string(csi) + "A"); return true;
    case WXK_DOWN:   SendBytes(std::string(csi) + "B"); return true;
    case WXK_RIGHT:  SendBytes(std::string(csi) + "C"); return true;
    case WXK_LEFT:   SendBytes(std::string(csi) + "D"); return true;
    case WXK_HOME:   SendBytes(std::string(csi) + "H"); return true;
    case WXK_END:    SendBytes(std::string(csi) + "F"); return true;
    case WXK_INSERT: SendBytes("\x1b[2~"); return true;
    case WXK_DELETE: SendBytes("\x1b[3~"); return true;
    case WXK_PAGEUP:
        if (shift && m_emu && !m_emu->IsAlternateScreen())
        {
            m_scrollOffset = std::min(m_scrollOffset + GridSize().GetHeight(), m_emu->ScrollbackLineCount());
            Refresh();
        }
        else SendBytes("\x1b[5~");
        return true;
    case WXK_PAGEDOWN:
        if (shift && m_emu && !m_emu->IsAlternateScreen())
        {
            m_scrollOffset = std::max(0, m_scrollOffset - GridSize().GetHeight());
            Refresh();
        }
        else SendBytes("\x1b[6~");
        return true;
    case WXK_F1:  SendBytes("\x1bOP"); return true;
    case WXK_F2:  SendBytes("\x1bOQ"); return true;
    case WXK_F3:  SendBytes("\x1bOR"); return true;
    case WXK_F4:  SendBytes("\x1bOS"); return true;
    case WXK_F5:  SendBytes("\x1b[15~"); return true;
    case WXK_F6:  SendBytes("\x1b[17~"); return true;
    case WXK_F7:  SendBytes("\x1b[18~"); return true;
    case WXK_F8:  SendBytes("\x1b[19~"); return true;
    case WXK_F9:  SendBytes("\x1b[20~"); return true;
    case WXK_F10: SendBytes("\x1b[21~"); return true;
    case WXK_F11: SendBytes("\x1b[23~"); return true;
    case WXK_F12: SendBytes("\x1b[24~"); return true;
    default: break;
    }

    if (ctrl && !shift && key >= 'A' && key <= 'Z')
    {
        // Ctrl+letter -> the corresponding C0 control code (Ctrl+C ->
        // 0x03 interrupt, Ctrl+D -> EOF, Ctrl+L -> clear/redraw, Ctrl+Z ->
        // suspend, Ctrl+R -> reverse-search, ...). This is what makes job
        // control and readline shortcuts work at all over a real pty.
        SendBytes(std::string(1, (char)(key - 'A' + 1)));
        return true;
    }
    if (ctrl && key == WXK_SPACE) { SendBytes(std::string(1, '\0')); return true; } // common readline binding
    if (ctrl && key == '[')  { SendBytes("\x1b"); return true; }
    if (ctrl && key == '\\') { SendBytes(std::string(1, '\x1c')); return true; }
    if (ctrl && key == ']')  { SendBytes(std::string(1, '\x1d')); return true; }

    return false;
}

void TerminalPanel::OnKeyDown(wxKeyEvent& event)
{
    if (EncodeSpecialKey(event))
        return; // handled + sent; don't let wxEVT_CHAR also fire for it
    event.Skip(); // let wxEVT_CHAR deliver the actual typed character
}

void TerminalPanel::OnChar(wxKeyEvent& event)
{
    if (!m_pty) return;

    wxChar uc = event.GetUnicodeKey();
    if (uc == (wxChar)WXK_NONE) { event.Skip(); return; }

    // Control characters and Enter/Tab/Backspace/Escape are already fully
    // handled in OnKeyDown; don't send them a second time here.
    if ((uint32_t)uc < 0x20 || uc == 0x7F)
        return;

    SendUtf8((char32_t)uc);
}

// ---------------------------------------------------------------------
// Clipboard
// ---------------------------------------------------------------------

void TerminalPanel::CopySelectionToClipboard()
{
    if (!m_hasSelection || !m_emu) return;

    int aLine = m_selAnchorLine, aCol = m_selAnchorCol;
    int bLine = m_selActiveLine, bCol = m_selActiveCol;
    if (aLine > bLine || (aLine == bLine && aCol > bCol))
    {
        std::swap(aLine, bLine);
        std::swap(aCol, bCol);
    }

    std::string text;
    for (int line = aLine; line <= bLine; ++line)
    {
        int len = 0;
        const TermCell* cells = LogicalLine(line, &len);
        int startCol = (line == aLine) ? aCol : 0;
        int endCol = (line == bLine) ? bCol : (len - 1);

        std::string rowText;
        for (int c = startCol; c <= endCol && c < len; ++c)
            AppendUtf8(rowText, cells ? cells[c].ch : U' ');
        while (!rowText.empty() && rowText.back() == ' ')
            rowText.pop_back(); // trailing padding on a line isn't meaningful content

        text += rowText;
        if (line != bLine)
            text += "\n";
    }

    if (text.empty()) return;
    if (wxTheClipboard->Open())
    {
        wxTheClipboard->SetData(new wxTextDataObject(wxString::FromUTF8(text)));
        wxTheClipboard->Close();
    }
}

void TerminalPanel::PasteFromClipboard()
{
    if (!m_pty) return;
    if (!wxTheClipboard->Open()) return;

    wxString text;
    if (wxTheClipboard->IsSupported(wxDF_TEXT) || wxTheClipboard->IsSupported(wxDF_UNICODETEXT))
    {
        wxTextDataObject data;
        wxTheClipboard->GetData(data);
        text = data.GetText();
    }
    wxTheClipboard->Close();
    if (text.IsEmpty()) return;

    // A real terminal sends \r for Enter regardless of platform, so a
    // pasted multi-line block needs its line endings normalized to that,
    // or the shell only ever sees the first line.
    text.Replace("\r\n", "\r");
    text.Replace("\n", "\r");

    std::string bytes = text.utf8_string();
    if (m_emu && m_emu->BracketedPaste())
    {
        // Tells the receiving app (typically a line editor like readline
        // or vim) "this whole chunk arrived from a paste, not typing" so
        // it can skip things like auto-indent-per-line that only make
        // sense for keystrokes.
        bytes = "\x1b[200~" + bytes + "\x1b[201~";
    }
    SendBytes(bytes);
}

void TerminalPanel::ClearSelection()
{
    if (m_hasSelection)
    {
        m_hasSelection = false;
        Refresh();
    }
}

// ---------------------------------------------------------------------
// Mouse
// ---------------------------------------------------------------------

wxPoint TerminalPanel::ClientPosToCell(const wxPoint& pos) const
{
    wxSize g = GridSize();
    int col = std::clamp(pos.x / m_charW, 0, std::max(0, g.GetWidth() - 1));
    int screenRow = std::clamp(pos.y / m_charH, 0, std::max(0, g.GetHeight() - 1));
    int logicalLine = LogicalLineCount() - g.GetHeight() - m_scrollOffset + screenRow;
    return wxPoint(col, logicalLine);
}

void TerminalPanel::OnMouseWheel(wxMouseEvent& event)
{
    int delta = event.GetWheelDelta();
    if (delta == 0 || !m_emu) { event.Skip(); return; }
    int notches = event.GetWheelRotation() / delta;
    if (notches == 0) { event.Skip(); return; }

    if (m_emu->IsAlternateScreen())
    {
        // Full-screen apps (vim/less/top/htop) manage their own paging;
        // there's no terminal-level scrollback to move through while one
        // has the alternate screen, so forward wheel motion as arrow-key
        // escapes instead — the same trick xterm's "alternateScroll"
        // option uses, and it makes the mouse wheel do something useful
        // in these apps rather than nothing at all.
        const char* csi = m_emu->ApplicationCursorKeys() ? "\x1bO" : "\x1b[";
        std::string seq = std::string(csi) + (notches > 0 ? "A" : "B");
        for (int i = 0; i < std::abs(notches) * kWheelLinesPerNotch; ++i)
            SendBytes(seq);
    }
    else
    {
        m_scrollOffset = std::clamp(m_scrollOffset - notches * kWheelLinesPerNotch, 0, m_emu->ScrollbackLineCount());
        Refresh();
    }
}

void TerminalPanel::OnMouseDown(wxMouseEvent& event)
{
    SetFocus();
    wxPoint cell = ClientPosToCell(event.GetPosition());
    m_selecting = true;
    m_selAnchorCol = cell.x; m_selAnchorLine = cell.y;
    m_selActiveCol = cell.x; m_selActiveLine = cell.y;
    m_hasSelection = false;
    if (!HasCapture())
        CaptureMouse();
    Refresh();
}

void TerminalPanel::OnMouseMove(wxMouseEvent& event)
{
    if (m_selecting && event.Dragging() && event.LeftIsDown())
    {
        wxPoint cell = ClientPosToCell(event.GetPosition());
        m_selActiveCol = cell.x; m_selActiveLine = cell.y;
        m_hasSelection = (m_selActiveLine != m_selAnchorLine) || (m_selActiveCol != m_selAnchorCol);
        Refresh();
    }
    event.Skip();
}

void TerminalPanel::OnMouseUp(wxMouseEvent& event)
{
    if (m_selecting)
    {
        m_selecting = false;
        if (HasCapture())
            ReleaseMouse();
        // Selecting text and releasing copies it immediately (as well as
        // Ctrl+Shift+C being available) — a plain click with no drag just
        // clears whatever was selected before, matching ordinary
        // click-to-deselect behavior.
        if (m_hasSelection)
            CopySelectionToClipboard();
        else
            ClearSelection();
    }
    event.Skip();
}

void TerminalPanel::OnSetFocus(wxFocusEvent& event)
{
    m_hasFocus = true;
    Refresh();
    event.Skip();
}

void TerminalPanel::OnKillFocus(wxFocusEvent& event)
{
    m_hasFocus = false;
    Refresh();
    event.Skip();
}

void TerminalPanel::OnEraseBackground(wxEraseEvent&)
{
    // Deliberately does nothing: OnPaint fills the entire client area
    // itself every time, so a separate erase pass would only add flicker.
}

// ---------------------------------------------------------------------
// Timers
// ---------------------------------------------------------------------

void TerminalPanel::OnTimer(wxTimerEvent&)
{
    if (!m_pty || !m_emu)
        return;

    std::string chunk;
    for (int i = 0; i < 8; ++i) // drain a few rounds without risking a long stall on a big burst
    {
        std::string piece;
        size_t n = m_pty->ReadAvailable(piece);
        if (n == 0) break;
        chunk += piece;
    }

    if (!chunk.empty())
    {
        int oldTotal = LogicalLineCount();
        m_emu->Feed(reinterpret_cast<const uint8_t*>(chunk.data()), chunk.size());
        int delta = LogicalLineCount() - oldTotal;

        // If the user had scrolled back into history, keep showing the
        // same lines rather than letting new output at the bottom shove
        // the viewport around; if they were already at the bottom, they
        // stay there and naturally see the new output (nothing to do).
        if (m_scrollOffset > 0)
            m_scrollOffset += delta;
        ClampScrollOffset();

        std::string resp = m_emu->TakeResponses();
        if (!resp.empty())
            m_pty->Write(resp);

        (void)m_emu->TakeTitle(); // consumed; nothing currently surfaces it in the UI

        Refresh();
    }

    if (m_pty->HasExited() && !m_exitNoticeShown)
    {
        m_exitNoticeShown = true;
        FeedInfoLine(wxString::Format("[process exited, code %d]", m_pty->ExitCode()));
        m_pollTimer.Stop();
    }
}

void TerminalPanel::OnBlinkTimer(wxTimerEvent&)
{
    m_cursorBlinkOn = !m_cursorBlinkOn;
    if (m_hasFocus) // no point repainting a blink nobody can see change
        Refresh();
}

void TerminalPanel::OnSize(wxSizeEvent& event)
{
    PushSizeToBackendAndEmulator();
    Refresh();
    event.Skip();
}

// ---------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------

void TerminalPanel::OnPaint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);

    // Plain light terminal (black text on white), consistent with the
    // rest of this app's native, no-custom-dark-theme look — this is now
    // a fully custom-painted canvas (not a native text control), but there
    // was never a design reason to make its *default* colors different
    // from what a native control would have shown; ANSI colors still
    // render as themselves regardless of this choice.
    const wxColour kDefaultBg(255, 255, 255);
    const wxColour kDefaultFg(0, 0, 0);
    const wxColour kSelBg = wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHT);
    const wxColour kSelFg = wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHTTEXT);

    dc.SetBackground(wxBrush(kDefaultBg));
    dc.Clear();

    if (!m_emu)
        return;

    dc.SetBackgroundMode(wxTRANSPARENT); // we already painted backgrounds ourselves; DrawText shouldn't repaint them

    wxSize g = GridSize();
    const int cols = g.GetWidth();
    const int rows = g.GetHeight();
    const int totalLines = LogicalLineCount();
    const int baseIndex = totalLines - rows - m_scrollOffset;

    int selALine = m_selAnchorLine, selACol = m_selAnchorCol;
    int selBLine = m_selActiveLine, selBCol = m_selActiveCol;
    if (m_hasSelection && (selALine > selBLine || (selALine == selBLine && selACol > selBCol)))
    {
        std::swap(selALine, selBLine);
        std::swap(selACol, selBCol);
    }
    auto isSelected = [&](int line, int col) {
        if (!m_hasSelection) return false;
        if (line < selALine || line > selBLine) return false;
        if (line == selALine && col < selACol) return false;
        if (line == selBLine && col > selBCol) return false;
        return true;
    };

    const bool cursorLive = (m_scrollOffset == 0) && m_emu->CursorVisible();
    const int cursorRow = m_emu->Cursor().row;
    const int cursorCol = m_emu->Cursor().col;

    std::vector<wxColour> fgArr(cols), bgArr(cols);
    std::vector<char32_t> chArr(cols);
    std::vector<bool> boldArr(cols);

    for (int screenRow = 0; screenRow < rows; ++screenRow)
    {
        int logicalIndex = baseIndex + screenRow;
        int len = 0;
        const TermCell* cells = (logicalIndex >= 0) ? LogicalLine(logicalIndex, &len) : nullptr;
        const int y = screenRow * m_charH;
        const bool cursorOnThisRow = cursorLive && (screenRow == cursorRow);

        for (int c = 0; c < cols; ++c)
        {
            TermCell cell;
            if (cells && c < len) cell = cells[c];

            wxColour fg = cell.fg.isDefault ? kDefaultFg : wxColour(cell.fg.r, cell.fg.g, cell.fg.b);
            wxColour bg = cell.bg.isDefault ? kDefaultBg : wxColour(cell.bg.r, cell.bg.g, cell.bg.b);
            if (cell.reverse)
                std::swap(fg, bg);

            if (isSelected(logicalIndex, c))
            {
                fg = kSelFg;
                bg = kSelBg;
            }

            if (cursorOnThisRow && c == cursorCol && m_hasFocus && m_cursorBlinkOn)
                std::swap(fg, bg); // solid block cursor while focused: invert fg/bg

            fgArr[c] = fg;
            bgArr[c] = bg;
            chArr[c] = cell.ch;
            boldArr[c] = cell.bold;
        }

        // Pass 1: background, run-length encoded so a mostly-uniform row
        // is a handful of fill rectangles rather than one per cell.
        dc.SetPen(*wxTRANSPARENT_PEN);
        for (int runStart = 0, c = 1; ; ++c)
        {
            if (c == cols || bgArr[c] != bgArr[runStart])
            {
                dc.SetBrush(wxBrush(bgArr[runStart]));
                dc.DrawRectangle(runStart * m_charW, y, (c - runStart) * m_charW, m_charH);
                if (c == cols) break;
                runStart = c;
            }
        }

        // Pass 2: text, run-length encoded by (fg, bold) so each
        // same-styled stretch is a single DrawText call.
        for (int runStart = 0, c = 1; ; ++c)
        {
            if (c == cols || fgArr[c] != fgArr[runStart] || boldArr[c] != boldArr[runStart])
            {
                wxString run;
                for (int k = runStart; k < c; ++k)
                    run += (wxUniChar)chArr[k];
                // Trailing blank runs of default-styled space are common
                // (most of any short line) and never visible either way;
                // skipping the DrawText call for them is a meaningful
                // paint-cost saving for typical terminal content.
                if (!run.IsEmpty() && run.find_first_not_of(' ') != wxString::npos)
                {
                    dc.SetFont(boldArr[runStart] ? m_font.Bold() : m_font);
                    dc.SetTextForeground(fgArr[runStart]);
                    dc.DrawText(run, runStart * m_charW, y);
                }
                if (c == cols) break;
                runStart = c;
            }
        }

        // Hollow cursor outline when the terminal doesn't have focus —
        // still shows where it is without a distracting blink.
        if (cursorOnThisRow && !m_hasFocus)
        {
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.SetPen(wxPen(kDefaultFg));
            dc.DrawRectangle(cursorCol * m_charW, y, m_charW, m_charH);
        }
    }
}
