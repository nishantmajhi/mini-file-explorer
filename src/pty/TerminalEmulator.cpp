#include "TerminalEmulator.h"
#include <algorithm>
#include <cstdio>

namespace
{
// Standard xterm 16-color palette (indices 0-15).
constexpr uint8_t kAnsi16[16][3] = {
    {0x00,0x00,0x00}, {0xCD,0x00,0x00}, {0x00,0xCD,0x00}, {0xCD,0xCD,0x00},
    {0x00,0x00,0xEE}, {0xCD,0x00,0xCD}, {0x00,0xCD,0xCD}, {0xE5,0xE5,0xE5},
    {0x7F,0x7F,0x7F}, {0xFF,0x00,0x00}, {0x00,0xFF,0x00}, {0xFF,0xFF,0x00},
    {0x5C,0x5C,0xFF}, {0xFF,0x00,0xFF}, {0x00,0xFF,0xFF}, {0xFF,0xFF,0xFF},
};

TermColor Indexed256(int idx)
{
    TermColor c;
    c.isDefault = false;
    if (idx < 0) idx = 0;
    if (idx < 16)
    {
        c.r = kAnsi16[idx][0]; c.g = kAnsi16[idx][1]; c.b = kAnsi16[idx][2];
    }
    else if (idx < 232)
    {
        int i = idx - 16;
        int r = (i / 36) % 6, g = (i / 6) % 6, b = i % 6;
        auto level = [](int v) -> uint8_t { return v == 0 ? 0 : (uint8_t)(v * 40 + 55); };
        c.r = level(r); c.g = level(g); c.b = level(b);
    }
    else
    {
        int i = std::min(idx - 232, 23);
        uint8_t v = (uint8_t)(8 + i * 10);
        c.r = c.g = c.b = v;
    }
    return c;
}
} // namespace

TerminalEmulator::TerminalEmulator(int cols, int rows)
    : m_cols(std::max(1, cols)), m_rows(std::max(1, rows))
{
    m_primary.assign((size_t)m_cols * m_rows, TermCell());
    m_alt.assign((size_t)m_cols * m_rows, TermCell());
    m_scrollTop = 0;
    m_scrollBottom = m_rows - 1;
}

TermCell TerminalEmulator::BlankCell() const
{
    TermCell c;
    c.fg = m_curFg;
    c.bg = m_curBg;
    // Blank cells created by erase operations carry the *background* pen
    // color forward (so "clear to end of line" after setting a bg color
    // paints that color), but never bold/underline/reverse.
    return c;
}

void TerminalEmulator::Resize(int cols, int rows)
{
    cols = std::max(1, cols);
    rows = std::max(1, rows);
    if (cols == m_cols && rows == m_rows)
        return;

    auto reflowless = [&](std::vector<TermCell>& grid)
    {
        std::vector<TermCell> next((size_t)cols * rows, TermCell());
        int copyRows = std::min(rows, m_rows);
        int copyCols = std::min(cols, m_cols);
        for (int r = 0; r < copyRows; ++r)
            for (int c = 0; c < copyCols; ++c)
                next[(size_t)r * cols + c] = grid[(size_t)r * m_cols + c];
        grid = std::move(next);
    };
    reflowless(m_primary);
    reflowless(m_alt);

    m_cols = cols;
    m_rows = rows;
    m_scrollTop = 0;
    m_scrollBottom = m_rows - 1;
    ClampCursor();
    m_pendingWrap = false;
    ++m_generation;
}

void TerminalEmulator::ClampCursor()
{
    m_cursor.row = std::clamp(m_cursor.row, 0, m_rows - 1);
    m_cursor.col = std::clamp(m_cursor.col, 0, m_cols - 1);
}

const TermCell& TerminalEmulator::CellAt(int row, int col) const
{
    static TermCell blank;
    if (row < 0 || row >= m_rows || col < 0 || col >= m_cols)
        return blank;
    return Active()[(size_t)row * m_cols + col];
}

const TermCell* TerminalEmulator::RowPtr(int row) const
{
    if (row < 0 || row >= m_rows) return nullptr;
    return &Active()[(size_t)row * m_cols];
}

const std::vector<TermCell>& TerminalEmulator::ScrollbackLine(int indexFromOldest) const
{
    static const std::vector<TermCell> empty;
    if (indexFromOldest < 0 || indexFromOldest >= (int)m_scrollback.size())
        return empty;
    return m_scrollback[(size_t)indexFromOldest];
}

std::string TerminalEmulator::TakeTitle()
{
    std::string t = std::move(m_pendingTitle);
    m_pendingTitle.clear();
    return t;
}

std::string TerminalEmulator::TakeResponses()
{
    std::string r = std::move(m_pendingResponses);
    m_pendingResponses.clear();
    return r;
}

void TerminalEmulator::Feed(const uint8_t* data, size_t len)
{
    for (size_t i = 0; i < len; ++i)
        FeedByte(data[i]);
}

// ---------------------------------------------------------------------
// UTF-8 decoding. Only used in Ground state for printable bytes >= 0x20;
// C0 control bytes are always single-byte and handled directly.
// ---------------------------------------------------------------------
void TerminalEmulator::DecodeAndPut(uint8_t b)
{
    if (m_utf8Remaining > 0)
    {
        if ((b & 0xC0) == 0x80)
        {
            m_utf8Codepoint = (m_utf8Codepoint << 6) | (b & 0x3F);
            if (--m_utf8Remaining == 0)
                PutChar(m_utf8Codepoint);
            return;
        }
        // Malformed continuation — abandon the partial sequence and
        // reprocess this byte as a fresh lead byte below.
        m_utf8Remaining = 0;
    }

    if (b < 0x80)
    {
        PutChar((char32_t)b);
    }
    else if ((b & 0xE0) == 0xC0)
    {
        m_utf8Codepoint = b & 0x1F;
        m_utf8Remaining = 1;
    }
    else if ((b & 0xF0) == 0xE0)
    {
        m_utf8Codepoint = b & 0x0F;
        m_utf8Remaining = 2;
    }
    else if ((b & 0xF8) == 0xF0)
    {
        m_utf8Codepoint = b & 0x07;
        m_utf8Remaining = 3;
    }
    else
    {
        PutChar(U'?'); // invalid lead byte
    }
}

void TerminalEmulator::FeedByte(uint8_t b)
{
    switch (m_state)
    {
    case ParseState::Ground:
        if (b == 0x1B) { m_state = ParseState::Escape; return; }
        if (b < 0x20 || b == 0x7F) { ExecuteControl(b); return; }
        DecodeAndPut(b);
        return;

    case ParseState::Escape:
        if (b == '[') { StartCsi(); return; }
        if (b == ']') { m_state = ParseState::OscString; m_oscBuf.clear(); return; }
        if (b == 'P' || b == '^' || b == '_' || b == 'X') { m_state = ParseState::DcsIgnore; return; }
        if (b == '(' || b == ')' || b == '*' || b == '+') { m_state = ParseState::IgnoreOne; return; }
        FinishEscape((char)b);
        m_state = ParseState::Ground;
        return;

    case ParseState::IgnoreOne:
        m_state = ParseState::Ground;
        return;

    case ParseState::CsiParam:
        if (b >= 0x40 && b <= 0x7E)
        {
            FinishCsi((char)b);
            m_state = ParseState::Ground;
        }
        else
        {
            m_paramBuf.push_back((char)b);
        }
        return;

    case ParseState::OscString:
        if (b == 0x07) { FinishOsc(); m_state = ParseState::Ground; return; }
        if (b == 0x1B) { m_state = ParseState::Escape; m_escSawBracket = false; return; } // expect '\' next (ST)
        m_oscBuf.push_back((char)b);
        return;

    case ParseState::DcsIgnore:
        if (b == 0x1B) { m_state = ParseState::Ground; } // approximate: bail on ESC (usually ESC \)
        return;
    }
}

void TerminalEmulator::StartCsi()
{
    m_state = ParseState::CsiParam;
    m_paramBuf.clear();
}

std::vector<int> TerminalEmulator::ParseParams(const std::string& body, bool* priv)
{
    std::vector<int> params;
    size_t i = 0;
    if (priv) *priv = false;
    if (!body.empty() && (body[0] == '?' || body[0] == '>' || body[0] == '='))
    {
        if (priv) *priv = (body[0] == '?');
        i = 1;
    }
    int cur = -1; // -1 marks "not specified" -> caller applies its own default
    bool any = false;
    for (; i < body.size(); ++i)
    {
        char c = body[i];
        if (c >= '0' && c <= '9')
        {
            if (cur < 0) cur = 0;
            cur = cur * 10 + (c - '0');
            any = true;
        }
        else if (c == ';' || c == ':')
        {
            params.push_back(cur);
            cur = -1;
        }
        // other intermediate bytes (e.g. a trailing ' ' before the final
        // byte) are simply ignored — none of the sequences we act on use them.
    }
    if (any || !params.empty() || cur >= 0)
        params.push_back(cur);
    return params;
}

static int P(const std::vector<int>& params, size_t idx, int def)
{
    if (idx >= params.size() || params[idx] < 0) return def;
    return params[idx];
}

void TerminalEmulator::FinishCsi(char finalByte)
{
    bool priv = false;
    std::vector<int> params = ParseParams(m_paramBuf, &priv);

    switch (finalByte)
    {
    case 'A': m_cursor.row = std::max(0, m_cursor.row - std::max(1, P(params,0,1))); m_pendingWrap = false; break;
    case 'B': m_cursor.row = std::min(m_rows - 1, m_cursor.row + std::max(1, P(params,0,1))); m_pendingWrap = false; break;
    case 'C': m_cursor.col = std::min(m_cols - 1, m_cursor.col + std::max(1, P(params,0,1))); m_pendingWrap = false; break;
    case 'D': m_cursor.col = std::max(0, m_cursor.col - std::max(1, P(params,0,1))); m_pendingWrap = false; break;
    case 'E': m_cursor.row = std::min(m_rows - 1, m_cursor.row + std::max(1, P(params,0,1))); m_cursor.col = 0; m_pendingWrap = false; break;
    case 'F': m_cursor.row = std::max(0, m_cursor.row - std::max(1, P(params,0,1))); m_cursor.col = 0; m_pendingWrap = false; break;
    case 'G': m_cursor.col = std::clamp(P(params,0,1) - 1, 0, m_cols - 1); m_pendingWrap = false; break;
    case '`': m_cursor.col = std::clamp(P(params,0,1) - 1, 0, m_cols - 1); m_pendingWrap = false; break;
    case 'd': m_cursor.row = std::clamp(P(params,0,1) - 1, 0, m_rows - 1); m_pendingWrap = false; break;
    case 'H':
    case 'f':
        m_cursor.row = std::clamp(P(params,0,1) - 1, 0, m_rows - 1);
        m_cursor.col = std::clamp(P(params,1,1) - 1, 0, m_cols - 1);
        m_pendingWrap = false;
        break;
    case 'J': EraseInDisplay(P(params,0,0)); break;
    case 'K': EraseInLine(P(params,0,0)); break;
    case 'L': InsertLines(std::max(1, P(params,0,1))); break;
    case 'M': DeleteLines(std::max(1, P(params,0,1))); break;
    case 'P': DeleteChars(std::max(1, P(params,0,1))); break;
    case '@': InsertChars(std::max(1, P(params,0,1))); break;
    case 'X': EraseChars(std::max(1, P(params,0,1))); break;
    case 'S': ScrollUp(std::max(1, P(params,0,1))); break;
    case 'T': ScrollDown(std::max(1, P(params,0,1))); break;
    case 'b':
    {
        // REP: repeat the last printed character n times. We don't track
        // "last printed" separately from the grid, so just reuse whatever
        // is currently one column to the left of the cursor.
        int n = std::max(1, P(params,0,1));
        if (m_cursor.col > 0)
        {
            char32_t ch = Active()[(size_t)m_cursor.row * m_cols + (m_cursor.col - 1)].ch;
            for (int i = 0; i < n; ++i) PutChar(ch);
        }
        break;
    }
    case 'm': HandleSgr(params); break;
    case 'r':
    {
        int top = std::clamp(P(params,0,1) - 1, 0, m_rows - 1);
        int bottom = std::clamp(P(params,1,m_rows) - 1, 0, m_rows - 1);
        if (top < bottom) { m_scrollTop = top; m_scrollBottom = bottom; }
        else { m_scrollTop = 0; m_scrollBottom = m_rows - 1; }
        m_cursor.row = m_scrollTop;
        m_cursor.col = 0;
        m_pendingWrap = false;
        break;
    }
    case 'h': SetMode(params, priv, true); break;
    case 'l': SetMode(params, priv, false); break;
    case 's': SaveCursor(); break;
    case 'u': RestoreCursor(); break;
    case 'c':
        // Primary Device Attributes reply: "I am a VT102-ish terminal".
        m_pendingResponses += "\x1b[?1;2c";
        break;
    case 'n':
        if (P(params,0,0) == 6)
        {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "\x1b[%d;%dR", m_cursor.row + 1, m_cursor.col + 1);
            m_pendingResponses += buf;
        }
        else if (P(params,0,0) == 5)
        {
            m_pendingResponses += "\x1b[0n";
        }
        break;
    default:
        break; // unrecognized final byte: ignore the whole sequence
    }
    ++m_generation;
}

void TerminalEmulator::FinishEscape(char c)
{
    switch (c)
    {
    case 'c': // RIS: full reset
        m_primary.assign((size_t)m_cols * m_rows, TermCell());
        m_alt.assign((size_t)m_cols * m_rows, TermCell());
        m_cursor = {};
        m_curFg = TermColor(); m_curBg = TermColor();
        m_curBold = m_curUnderline = m_curReverse = false;
        m_usingAlt = false;
        m_scrollTop = 0; m_scrollBottom = m_rows - 1;
        m_cursorVisible = true;
        m_pendingWrap = false;
        break;
    case '7': SaveCursor(); break;
    case '8': RestoreCursor(); break;
    case 'D': LineFeed(); break;         // IND
    case 'M': ReverseLineFeed(); break;  // RI
    case 'E':                            // NEL
        m_cursor.col = 0;
        LineFeed();
        break;
    default:
        break; // '=', '>', and anything else we don't model: no-op
    }
    ++m_generation;
}

void TerminalEmulator::FinishOsc()
{
    // Expect "N;text...". We only care about N==0 or N==2 (icon+title / title).
    size_t semi = m_oscBuf.find(';');
    if (semi != std::string::npos)
    {
        std::string numPart = m_oscBuf.substr(0, semi);
        if (numPart == "0" || numPart == "2")
            m_pendingTitle = m_oscBuf.substr(semi + 1);
    }
    m_oscBuf.clear();
}

void TerminalEmulator::ExecuteControl(uint8_t c)
{
    switch (c)
    {
    case '\a': break; // BEL: no audible/visual bell modeled
    case '\b':
        if (m_cursor.col > 0) m_cursor.col--;
        m_pendingWrap = false;
        break;
    case '\t':
    {
        int next = ((m_cursor.col / 8) + 1) * 8;
        m_cursor.col = std::min(next, m_cols - 1);
        break;
    }
    case '\n': case '\v': case '\f':
        LineFeed();
        break;
    case '\r':
        m_cursor.col = 0;
        m_pendingWrap = false;
        break;
    default:
        break; // SO/SI and other rarely-used C0 codes: ignore
    }
    ++m_generation;
}

void TerminalEmulator::PutChar(char32_t ch)
{
    if (m_pendingWrap)
    {
        m_cursor.col = 0;
        LineFeed();
        m_pendingWrap = false;
    }

    if (m_cursor.row >= 0 && m_cursor.row < m_rows && m_cursor.col >= 0 && m_cursor.col < m_cols)
    {
        TermCell cell;
        cell.ch = ch;
        cell.fg = m_curFg;
        cell.bg = m_curBg;
        cell.bold = m_curBold;
        cell.underline = m_curUnderline;
        cell.reverse = m_curReverse;
        Active()[(size_t)m_cursor.row * m_cols + m_cursor.col] = cell;
    }

    if (m_cursor.col + 1 >= m_cols)
    {
        if (m_autoWrap)
            m_pendingWrap = true; // defer: don't wrap until the NEXT char actually arrives
    }
    else
    {
        m_cursor.col++;
    }
    ++m_generation;
}

void TerminalEmulator::LineFeed()
{
    if (m_cursor.row == m_scrollBottom)
        ScrollUp(1);
    else if (m_cursor.row < m_rows - 1)
        m_cursor.row++;
    m_pendingWrap = false;
}

void TerminalEmulator::ReverseLineFeed()
{
    if (m_cursor.row == m_scrollTop)
        ScrollDown(1);
    else if (m_cursor.row > 0)
        m_cursor.row--;
    m_pendingWrap = false;
}

void TerminalEmulator::ScrollUp(int n)
{
    auto& grid = Active();
    int top = m_scrollTop, bottom = m_scrollBottom;
    n = std::min(n, bottom - top + 1);

    // Only a genuine full-screen scroll on the primary buffer represents
    // real, linear history worth keeping — a scroll confined to a
    // sub-region (e.g. a curses app scrolling just its status area) isn't
    // "history" in any coherent sense, and the alternate screen never
    // gets scrollback at all (matches real terminals: vim/less/top manage
    // their own paging).
    if (!m_usingAlt && top == 0 && bottom == m_rows - 1)
    {
        for (int r = 0; r < n; ++r)
        {
            std::vector<TermCell> line(grid.begin() + (size_t)r * m_cols, grid.begin() + (size_t)(r + 1) * m_cols);
            m_scrollback.push_back(std::move(line));
        }
        if ((int)m_scrollback.size() > kMaxScrollback)
            m_scrollback.erase(m_scrollback.begin(), m_scrollback.begin() + ((int)m_scrollback.size() - kMaxScrollback));
    }

    for (int r = top; r + n <= bottom; ++r)
        for (int c = 0; c < m_cols; ++c)
            grid[(size_t)r * m_cols + c] = grid[(size_t)(r + n) * m_cols + c];
    for (int r = bottom - n + 1; r <= bottom; ++r)
        for (int c = 0; c < m_cols; ++c)
            grid[(size_t)r * m_cols + c] = BlankCell();
}

void TerminalEmulator::ScrollDown(int n)
{
    auto& grid = Active();
    int top = m_scrollTop, bottom = m_scrollBottom;
    n = std::min(n, bottom - top + 1);
    for (int r = bottom; r - n >= top; --r)
        for (int c = 0; c < m_cols; ++c)
            grid[(size_t)r * m_cols + c] = grid[(size_t)(r - n) * m_cols + c];
    for (int r = top; r < top + n; ++r)
        for (int c = 0; c < m_cols; ++c)
            grid[(size_t)r * m_cols + c] = BlankCell();
}

void TerminalEmulator::EraseInDisplay(int mode)
{
    auto& grid = Active();
    if (mode == 2 || mode == 3)
    {
        std::fill(grid.begin(), grid.end(), BlankCell());
    }
    else if (mode == 0) // cursor to end of screen
    {
        for (int c = m_cursor.col; c < m_cols; ++c) grid[(size_t)m_cursor.row * m_cols + c] = BlankCell();
        for (int r = m_cursor.row + 1; r < m_rows; ++r)
            for (int c = 0; c < m_cols; ++c) grid[(size_t)r * m_cols + c] = BlankCell();
    }
    else if (mode == 1) // start of screen to cursor
    {
        for (int r = 0; r < m_cursor.row; ++r)
            for (int c = 0; c < m_cols; ++c) grid[(size_t)r * m_cols + c] = BlankCell();
        for (int c = 0; c <= m_cursor.col && c < m_cols; ++c) grid[(size_t)m_cursor.row * m_cols + c] = BlankCell();
    }
}

void TerminalEmulator::EraseInLine(int mode)
{
    auto& grid = Active();
    int row = m_cursor.row;
    if (mode == 0)
        for (int c = m_cursor.col; c < m_cols; ++c) grid[(size_t)row * m_cols + c] = BlankCell();
    else if (mode == 1)
        for (int c = 0; c <= m_cursor.col && c < m_cols; ++c) grid[(size_t)row * m_cols + c] = BlankCell();
    else if (mode == 2)
        for (int c = 0; c < m_cols; ++c) grid[(size_t)row * m_cols + c] = BlankCell();
}

void TerminalEmulator::InsertLines(int n)
{
    if (m_cursor.row < m_scrollTop || m_cursor.row > m_scrollBottom) return;
    int savedTop = m_scrollTop;
    m_scrollTop = m_cursor.row;
    ScrollDown(n);
    m_scrollTop = savedTop;
}

void TerminalEmulator::DeleteLines(int n)
{
    if (m_cursor.row < m_scrollTop || m_cursor.row > m_scrollBottom) return;
    int savedTop = m_scrollTop;
    m_scrollTop = m_cursor.row;
    ScrollUp(n);
    m_scrollTop = savedTop;
}

void TerminalEmulator::InsertChars(int n)
{
    auto& grid = Active();
    int row = m_cursor.row;
    n = std::min(n, m_cols - m_cursor.col);
    for (int c = m_cols - 1; c >= m_cursor.col + n; --c)
        grid[(size_t)row * m_cols + c] = grid[(size_t)row * m_cols + (c - n)];
    for (int c = m_cursor.col; c < m_cursor.col + n && c < m_cols; ++c)
        grid[(size_t)row * m_cols + c] = BlankCell();
}

void TerminalEmulator::DeleteChars(int n)
{
    auto& grid = Active();
    int row = m_cursor.row;
    n = std::min(n, m_cols - m_cursor.col);
    for (int c = m_cursor.col; c < m_cols - n; ++c)
        grid[(size_t)row * m_cols + c] = grid[(size_t)row * m_cols + (c + n)];
    for (int c = m_cols - n; c < m_cols; ++c)
        grid[(size_t)row * m_cols + c] = BlankCell();
}

void TerminalEmulator::EraseChars(int n)
{
    auto& grid = Active();
    int row = m_cursor.row;
    for (int c = m_cursor.col; c < std::min(m_cols, m_cursor.col + n); ++c)
        grid[(size_t)row * m_cols + c] = BlankCell();
}

void TerminalEmulator::HandleSgr(const std::vector<int>& params)
{
    if (params.empty())
    {
        m_curFg = TermColor(); m_curBg = TermColor();
        m_curBold = m_curUnderline = m_curReverse = false;
        return;
    }

    for (size_t i = 0; i < params.size(); ++i)
    {
        int p = params[i] < 0 ? 0 : params[i];
        if (p == 0) { m_curFg = TermColor(); m_curBg = TermColor(); m_curBold = m_curUnderline = m_curReverse = false; }
        else if (p == 1) m_curBold = true;
        else if (p == 4) m_curUnderline = true;
        else if (p == 7) m_curReverse = true;
        else if (p == 21 || p == 22) m_curBold = false;
        else if (p == 24) m_curUnderline = false;
        else if (p == 27) m_curReverse = false;
        else if (p >= 30 && p <= 37) m_curFg = Indexed256(p - 30);
        else if (p == 39) m_curFg = TermColor();
        else if (p >= 40 && p <= 47) m_curBg = Indexed256(p - 40);
        else if (p == 49) m_curBg = TermColor();
        else if (p >= 90 && p <= 97) m_curFg = Indexed256(8 + (p - 90));
        else if (p >= 100 && p <= 107) m_curBg = Indexed256(8 + (p - 100));
        else if (p == 38 || p == 48)
        {
            bool isFg = (p == 38);
            if (i + 1 < params.size() && params[i + 1] == 5 && i + 2 < params.size())
            {
                TermColor c = Indexed256(params[i + 2]);
                if (isFg) m_curFg = c; else m_curBg = c;
                i += 2;
            }
            else if (i + 1 < params.size() && params[i + 1] == 2 && i + 4 < params.size())
            {
                TermColor c; c.isDefault = false;
                c.r = (uint8_t)std::clamp(params[i + 2], 0, 255);
                c.g = (uint8_t)std::clamp(params[i + 3], 0, 255);
                c.b = (uint8_t)std::clamp(params[i + 4], 0, 255);
                if (isFg) m_curFg = c; else m_curBg = c;
                i += 4;
            }
        }
        // Other SGR codes (italic, blink, strikethrough, font selection...)
        // are accepted-and-ignored: no visual effect, but they don't
        // desync parsing of whatever follows.
    }
}

void TerminalEmulator::SetMode(const std::vector<int>& params, bool priv, bool enable)
{
    for (int p : params)
    {
        if (p < 0) continue;
        if (priv)
        {
            switch (p)
            {
            case 25: m_cursorVisible = enable; break;
            case 7:  m_autoWrap = enable; break;
            case 1:  m_appCursorKeys = enable; break;
            case 2004: m_bracketedPaste = enable; break;
            case 47: case 1047: enable ? EnterAltScreen() : ExitAltScreen(); break;
            case 1049:
                if (enable) { SaveCursor(); EnterAltScreen(); }
                else { ExitAltScreen(); RestoreCursor(); }
                break;
            default: break; // mouse tracking, bracketed paste, etc.: not modeled, safely ignored
            }
        }
        // Non-private modes (IRM insert mode, LNM, etc.) aren't modeled.
    }
}

void TerminalEmulator::EnterAltScreen()
{
    if (m_usingAlt) return;
    m_usingAlt = true;
    std::fill(m_alt.begin(), m_alt.end(), TermCell());
    ++m_generation;
}

void TerminalEmulator::ExitAltScreen()
{
    if (!m_usingAlt) return;
    m_usingAlt = false;
    ++m_generation;
}

void TerminalEmulator::SaveCursor()
{
    if (m_usingAlt)
    {
        m_savedCursorAlt = m_cursor;
        m_savedFgAlt = m_curFg; m_savedBgAlt = m_curBg;
        m_savedBoldAlt = m_curBold; m_savedUnderlineAlt = m_curUnderline; m_savedReverseAlt = m_curReverse;
    }
    else
    {
        m_savedCursorPrimary = m_cursor;
        m_savedFgPrimary = m_curFg; m_savedBgPrimary = m_curBg;
        m_savedBoldPrimary = m_curBold; m_savedUnderlinePrimary = m_curUnderline; m_savedReversePrimary = m_curReverse;
    }
}

void TerminalEmulator::RestoreCursor()
{
    if (m_usingAlt)
    {
        m_cursor = m_savedCursorAlt;
        m_curFg = m_savedFgAlt; m_curBg = m_savedBgAlt;
        m_curBold = m_savedBoldAlt; m_curUnderline = m_savedUnderlineAlt; m_curReverse = m_savedReverseAlt;
    }
    else
    {
        m_cursor = m_savedCursorPrimary;
        m_curFg = m_savedFgPrimary; m_curBg = m_savedBgPrimary;
        m_curBold = m_savedBoldPrimary; m_curUnderline = m_savedUnderlinePrimary; m_curReverse = m_savedReversePrimary;
    }
    ClampCursor();
    m_pendingWrap = false;
}
