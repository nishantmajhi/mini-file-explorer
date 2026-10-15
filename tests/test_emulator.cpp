// Standalone sanity test for TerminalEmulator — no wx dependency, so it
// compiles/runs fast and in isolation while iterating on the parser.
// Not part of the shipped app; not added to CMakeLists.txt or build.sh.
#include "TerminalEmulator.h"
#include <cassert>
#include <cstdio>
#include <cstring>

static std::string RowText(TerminalEmulator& e, int row)
{
    std::string s;
    for (int c = 0; c < e.Cols(); ++c)
    {
        char32_t ch = e.CellAt(row, c).ch;
        s.push_back(ch < 128 ? (char)ch : '?');
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

static void Feed(TerminalEmulator& e, const char* s)
{
    e.Feed(reinterpret_cast<const uint8_t*>(s), strlen(s));
}

int main()
{
    // Basic printing + wrap + newline.
    {
        TerminalEmulator e(10, 3);
        Feed(e, "hello");
        assert(RowText(e, 0) == "hello");
        assert(e.Cursor().row == 0 && e.Cursor().col == 5);
        printf("[ok] basic print\n");
    }

    // Cursor addressing (CUP).
    {
        TerminalEmulator e(10, 3);
        Feed(e, "\x1b[2;3Hx");
        assert(e.CellAt(1, 2).ch == U'x');
        printf("[ok] CUP\n");
    }

    // SGR colors.
    {
        TerminalEmulator e(10, 3);
        Feed(e, "\x1b[31mR\x1b[0mN");
        assert(e.CellAt(0,0).fg.isDefault == false);
        assert(e.CellAt(0,0).fg.r == 0xCD);
        assert(e.CellAt(0,1).fg.isDefault == true);
        printf("[ok] SGR basic color + reset\n");
    }

    // 256-color and truecolor.
    {
        TerminalEmulator e(10, 3);
        Feed(e, "\x1b[38;5;196mA\x1b[38;2;10;20;30mB");
        assert(e.CellAt(0,0).fg.r == 0xFF); // color 196 is a bright red in the cube
        assert(e.CellAt(0,1).fg.r == 10 && e.CellAt(0,1).fg.g == 20 && e.CellAt(0,1).fg.b == 30);
        printf("[ok] 256-color + truecolor SGR\n");
    }

    // Erase in line / display.
    {
        TerminalEmulator e(5, 2);
        Feed(e, "abcde\r\nfghij");
        Feed(e, "\x1b[1;1H\x1b[K"); // erase line 1 from cursor (col 0) to end
        assert(RowText(e, 0) == "");
        assert(RowText(e, 1) == "fghij");
        printf("[ok] EL\n");
    }

    // Scrolling via linefeed at bottom margin.
    {
        TerminalEmulator e(5, 2);
        Feed(e, "one\r\ntwo\r\nthree");
        assert(RowText(e, 0) == "two");
        assert(RowText(e, 1) == "three");
        printf("[ok] scroll-on-LF\n");
    }

    // Alternate screen buffer (vim/less/top style enter+exit).
    {
        TerminalEmulator e(5, 2);
        Feed(e, "main1");
        Feed(e, "\x1b[?1049h"); // enter alt screen
        assert(e.IsAlternateScreen());
        Feed(e, "\x1b[HALT");
        assert(e.CellAt(0,0).ch == U'A');
        Feed(e, "\x1b[?1049l"); // exit alt screen — should restore "main1"
        assert(!e.IsAlternateScreen());
        assert(RowText(e, 0) == "main1");
        printf("[ok] alt screen save/restore\n");
    }

    // Insert/Delete line and char.
    {
        TerminalEmulator e(5, 3);
        Feed(e, "aaa\r\nbbb\r\nccc");
        Feed(e, "\x1b[2;1H\x1b[1L"); // insert 1 line at row 2 (pushes bbb/ccc down)
        assert(RowText(e, 0) == "aaa");
        assert(RowText(e, 1) == "");
        assert(RowText(e, 2) == "bbb");
        printf("[ok] IL\n");
    }

    // Resize preserves in-bounds content.
    {
        TerminalEmulator e(5, 2);
        Feed(e, "abcde");
        e.Resize(3, 2);
        assert(RowText(e, 0) == "abc");
        e.Resize(8, 2);
        assert(RowText(e, 0) == "abc");
        printf("[ok] resize\n");
    }

    // Cursor position report (DSR 6) produces a response.
    {
        TerminalEmulator e(10, 5);
        Feed(e, "\x1b[3;4H\x1b[6n");
        std::string resp = e.TakeResponses();
        assert(resp == "\x1b[3;4R");
        printf("[ok] DSR cursor position report\n");
    }

    // Window title via OSC.
    {
        TerminalEmulator e(10, 2);
        Feed(e, "\x1b]0;hello world\x07");
        assert(e.TakeTitle() == "hello world");
        printf("[ok] OSC title\n");
    }

    // Split UTF-8 sequence across two Feed() calls.
    {
        TerminalEmulator e(10, 2);
        const char bytes[] = "\xE2\x9C\x93"; // U+2713 check mark, 3 bytes
        e.Feed(reinterpret_cast<const uint8_t*>(bytes), 1);
        e.Feed(reinterpret_cast<const uint8_t*>(bytes) + 1, 2);
        assert(e.CellAt(0,0).ch == 0x2713);
        printf("[ok] split UTF-8 across Feed() calls\n");
    }

    // Scrollback accumulates on a full-screen scroll and DECCKM tracks mode 1.
    {
        TerminalEmulator e(5, 2);
        Feed(e, "one\r\ntwo\r\nthree\r\nfour");
        assert(e.ScrollbackLineCount() >= 2);
        assert(RowText(e, 1) == "four");
        Feed(e, "\x1b[?1h");
        assert(e.ApplicationCursorKeys());
        Feed(e, "\x1b[?1l");
        assert(!e.ApplicationCursorKeys());
        printf("[ok] scrollback + DECCKM\n");
    }

    printf("\nAll tests passed.\n");
    return 0;
}
