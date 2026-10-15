#pragma once
#include <wx/wx.h>
#include <wx/timer.h>
#include <cstdint>
#include <memory>
#include "pty/PtyBackend.h"
#include "pty/TerminalEmulator.h"

// A custom-painted character-grid terminal, backed by a real pseudo-
// terminal (ConPTY on Windows, forkpty on POSIX — see src/pty/). The shell
// itself now controls the cursor, and full-screen curses apps (vim, top,
// htop, less, man) work because they get a real pty device: raw keystrokes
// go straight to the child, not "a full line collected locally and sent on
// Enter" like the previous wxProcess/pipe implementation.
//
// This replaces the old wxTextCtrl-based single-editable-control approach
// entirely — colors and cursor-addressed redraws need a cell grid we
// control the painting of, not a rich-text control's own text model.
class TerminalPanel : public wxWindow
{
public:
    explicit TerminalPanel(wxWindow* parent);
    ~TerminalPanel() override;

    // Starts the shell, seeded with startDir as its initial working
    // directory — but only the FIRST time this is called (see .cpp for
    // why: re-opening an already-running terminal shouldn't yank its cwd
    // or blow away its scrollback).
    void EnsureShellStarted(const wxString& startDir);

    // Writes cmd + Enter directly to the pty, as if the user had typed it
    // and pressed Enter — used for programmatic command injection.
    void RunCommand(const wxString& cmd);
    void FocusInput();

    // Ends the running shell session entirely and clears scrollback —
    // "close this terminal" (its own [x]) as opposed to just hiding the
    // panel (View > Toggle Terminal), which preserves the running session.
    void CloseSession();

private:
    std::unique_ptr<IPtyBackend> m_pty;
    std::unique_ptr<TerminalEmulator> m_emu;
    wxTimer m_pollTimer;
    wxTimer m_blinkTimer;
    wxFont m_font;
    int m_charW = 8, m_charH = 16;
    uint64_t m_lastPaintedGeneration = ~(uint64_t)0;
    bool m_cursorBlinkOn = true;
    bool m_hasFocus = false;
    bool m_exitNoticeShown = false;

    // Scrollback view state: 0 = live (following new output at the
    // bottom); >0 = viewing that many lines back into history. Only
    // meaningful on the primary screen — forced to 0 whenever the
    // alternate screen (vim/less/top) is active.
    int m_scrollOffset = 0;

    // Mouse-drag text selection, in *logical line* coordinates (see
    // LogicalLineCount()/LogicalLine() below) so a selection drawn while
    // scrolled back stays correct if new output arrives mid-drag.
    bool m_selecting = false;
    int m_selAnchorLine = 0, m_selAnchorCol = 0;
    int m_selActiveLine = 0, m_selActiveCol = 0;
    bool m_hasSelection = false;

    void StartShell(const wxString& startDir);
    void FeedInfoLine(const wxString& text); // feeds text+CRLF into the emulator (spawn/exit notices)
    void MeasureFont();
    wxSize GridSize() const; // (cols, rows) that currently fit the client area
    void PushSizeToBackendAndEmulator();
    void ScrollToBottom();
    void ClampScrollOffset();

    int LogicalLineCount() const; // scrollback + active screen rows
    const TermCell* LogicalLine(int index, int* outLen) const; // nullptr-safe blank row if out of range

    void SendUtf8(char32_t ch);
    void SendBytes(const std::string& bytes);
    bool EncodeSpecialKey(wxKeyEvent& event); // true if handled (and sent)
    void CopySelectionToClipboard();
    void PasteFromClipboard();
    void ClearSelection();
    wxPoint ClientPosToCell(const wxPoint& pos) const; // (col, logical line index)

    void OnPaint(wxPaintEvent& event);
    void OnSize(wxSizeEvent& event);
    void OnTimer(wxTimerEvent& event);
    void OnBlinkTimer(wxTimerEvent& event);
    void OnKeyDown(wxKeyEvent& event);
    void OnChar(wxKeyEvent& event);
    void OnMouseWheel(wxMouseEvent& event);
    void OnMouseDown(wxMouseEvent& event);
    void OnMouseMove(wxMouseEvent& event);
    void OnMouseUp(wxMouseEvent& event);
    void OnSetFocus(wxFocusEvent& event);
    void OnKillFocus(wxFocusEvent& event);
    void OnEraseBackground(wxEraseEvent& event); // no-op: we paint every pixel ourselves

    wxDECLARE_EVENT_TABLE();
};
