#pragma once
#include <cstdint>
#include <string>
#include <vector>

// A fixed-size character grid plus a parser for the subset of VT100/xterm
// control sequences real-world terminal programs actually rely on: SGR
// colors (16/256/truecolor), cursor addressing, erase-in-line/display,
// insert/delete line/char, scroll regions, and the alternate screen buffer
// switch that curses apps (vim, top, htop, less, man) use to take over the
// whole screen and hand it back cleanly on exit.
//
// This is deliberately not a complete xterm — no sixel/image protocols, no
// mouse reporting, no double-width/double-height lines, no true Unicode
// combining-character or East-Asian-width handling. What's here covers the
// programs that motivated this feature (colors + vim/top), without trying
// to reimplement the whole of xterm's terminfo entry.
//
// PTY BACKEND CONTRACT: whatever spawns the child process is responsible
// for feeding raw PTY output bytes into Feed() as they arrive, and for
// writing TakeResponses()'s bytes back to the PTY afterward (these are the
// terminal's own replies to cursor-position/device-attribute queries —
// without them, programs that ask "where is the cursor?" and block on an
// answer would hang).

struct TermColor
{
    bool isDefault = true; // "use whatever the renderer treats as normal fg/bg"
    uint8_t r = 0, g = 0, b = 0;

    bool operator==(const TermColor& o) const
    {
        return isDefault == o.isDefault && (isDefault || (r == o.r && g == o.g && b == o.b));
    }
    bool operator!=(const TermColor& o) const { return !(*this == o); }
};

struct TermCell
{
    char32_t ch = U' ';
    TermColor fg;
    TermColor bg;
    bool bold = false;
    bool underline = false;
    bool reverse = false;

    bool operator==(const TermCell& o) const
    {
        return ch == o.ch && fg == o.fg && bg == o.bg && bold == o.bold &&
               underline == o.underline && reverse == o.reverse;
    }
};

struct TermCursor
{
    int row = 0;
    int col = 0;
};

class TerminalEmulator
{
public:
    TerminalEmulator(int cols, int rows);

    // Reflows nothing (like a real terminal on SIGWINCH) — just changes the
    // grid dimensions, padding new cells as blank and clipping/truncating
    // rows or columns that no longer fit. Cursor is clamped into bounds.
    void Resize(int cols, int rows);

    // Feeds raw bytes (as read straight off the PTY) into the parser.
    void Feed(const uint8_t* data, size_t len);

    int Cols() const { return m_cols; }
    int Rows() const { return m_rows; }
    const TermCell& CellAt(int row, int col) const;
    const TermCursor& Cursor() const { return m_cursor; }
    bool CursorVisible() const { return m_cursorVisible; }
    bool IsAlternateScreen() const { return m_usingAlt; }
    bool ApplicationCursorKeys() const { return m_appCursorKeys; }
    bool BracketedPaste() const { return m_bracketedPaste; }

    // Direct pointer to a full active-screen row (Cols() cells), for
    // renderers that want to avoid a virtual call per cell. Row must be
    // in [0, Rows()); out-of-range returns nullptr.
    const TermCell* RowPtr(int row) const;

    // Scrollback (primary screen only, matching real terminals — the
    // alternate screen used by curses apps never accumulates scrollback,
    // since those apps manage their own history/paging with j/k, PgUp,
    // etc.). Line 0 is the oldest retained line.
    int ScrollbackLineCount() const { return (int)m_scrollback.size(); }
    const std::vector<TermCell>& ScrollbackLine(int indexFromOldest) const;

    // Bumped every time the visible grid changes, so a renderer can cheaply
    // skip repainting when nothing changed since it last checked.
    uint64_t Generation() const { return m_generation; }

    // Non-empty exactly when an OSC 0/2 (set window title) was seen since
    // the last call; consuming clears it.
    std::string TakeTitle();

    // Bytes the emulator itself wants written back to the PTY (DA/DSR
    // query replies). Consuming clears them. Empty almost all the time.
    std::string TakeResponses();

private:
    enum class ParseState
    {
        Ground,
        Escape,
        CsiParam,
        OscString,
        IgnoreOne,   // ESC ( / ESC ) charset designators: swallow exactly one more byte
        DcsIgnore,   // device control string — consumed and discarded until ST
    };

    int m_cols, m_rows;
    std::vector<TermCell> m_primary;
    std::vector<TermCell> m_alt;
    bool m_usingAlt = false;

    TermCursor m_cursor;
    TermCursor m_savedCursorPrimary;
    TermCursor m_savedCursorAlt;
    bool m_cursorVisible = true;
    int m_scrollTop = 0;
    int m_scrollBottom = 0; // inclusive, in rows of the *active* screen
    bool m_autoWrap = true;
    bool m_appCursorKeys = false; // DECCKM: arrow keys send SS3 (O) instead of CSI ([) when set
    bool m_bracketedPaste = false; // DEC 2004: wrap pasted text in ESC[200~ / ESC[201~ markers
    bool m_pendingWrap = false; // xterm's "deferred wrap": at right margin, wrap on the NEXT printable char

    static constexpr int kMaxScrollback = 5000;
    std::vector<std::vector<TermCell>> m_scrollback;

    // Current pen (SGR state), applied to every cell as it's written.
    TermColor m_curFg, m_curBg, m_savedFgPrimary, m_savedBgPrimary, m_savedFgAlt, m_savedBgAlt;
    bool m_curBold = false, m_curUnderline = false, m_curReverse = false;
    bool m_savedBoldPrimary = false, m_savedUnderlinePrimary = false, m_savedReversePrimary = false;
    bool m_savedBoldAlt = false, m_savedUnderlineAlt = false, m_savedReverseAlt = false;

    ParseState m_state = ParseState::Ground;
    std::string m_paramBuf; // raw text of the current CSI param/intermediate bytes
    std::string m_oscBuf;
    bool m_escSawBracket = false;

    // UTF-8 decode state, carried across Feed() calls in case a multi-byte
    // sequence is split across two PTY reads.
    char32_t m_utf8Codepoint = 0;
    int m_utf8Remaining = 0;

    std::string m_pendingTitle;
    std::string m_pendingResponses;
    uint64_t m_generation = 0;

    std::vector<TermCell>& Active() { return m_usingAlt ? m_alt : m_primary; }
    const std::vector<TermCell>& Active() const { return m_usingAlt ? m_alt : m_primary; }

    void FeedByte(uint8_t b);
    void DecodeAndPut(uint8_t b); // UTF-8 state machine feeding PutChar()

    void ExecuteControl(uint8_t c);
    void PutChar(char32_t ch);
    void StartCsi();
    void FinishCsi(char finalByte);
    void FinishEscape(char c);
    void FinishOsc();
    void HandleSgr(const std::vector<int>& params);

    void EraseInDisplay(int mode);
    void EraseInLine(int mode);
    void InsertLines(int n);
    void DeleteLines(int n);
    void InsertChars(int n);
    void DeleteChars(int n);
    void EraseChars(int n);
    void ScrollUp(int n);
    void ScrollDown(int n);
    void LineFeed();
    void ReverseLineFeed();
    void SetMode(const std::vector<int>& params, bool priv, bool enable);
    void EnterAltScreen();
    void ExitAltScreen();
    void SaveCursor();
    void RestoreCursor();
    void ClampCursor();
    TermCell BlankCell() const;

    static std::vector<int> ParseParams(const std::string& body, bool* priv);
};
