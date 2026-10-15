// End-to-end smoke test, POSIX only: spawns a real shell over a real pty,
// sends a command, and checks the emulator's grid ends up with the
// expected content. Not part of the shipped app.
#include "PosixPtyBackend.h"
#include "TerminalEmulator.h"
#include <cassert>
#include <cstdio>
#include <string>
#include <unistd.h>

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

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    PosixPtyBackend backend;
    TerminalEmulator emu(80, 24);

    wxString err;
    bool ok = backend.Spawn("/bin/bash", {"--noprofile", "--norc"}, "/tmp", 80, 24, &err);
    assert(ok);
    printf("[ok] spawned bash, pid=%ld\n", backend.GetPid());

    // Give bash a moment to finish its own startup (readline init, which
    // changes termios modes) before sending it anything — and send both
    // commands as one write rather than two separate ones, since two
    // separate Write() calls landing right as bash flips terminal modes
    // during its own startup was observed to occasionally stall bash's
    // input processing entirely (a test-timing hazard, not a pty/emulator
    // bug: a real user's keystrokes never arrive this fast after spawn).
    usleep(100000);
    backend.Write("PS1=''\nprintf '\\033[31mHELLO\\033[0m world\\n'\n");

    std::string collected;
    // Generous budget: this is a smoke test running in a possibly
    // CPU-contended sandbox, not a latency benchmark.
    for (int i = 0; i < 500 && collected.find("world") == std::string::npos; ++i)
    {
        std::string chunk;
        backend.ReadAvailable(chunk);
        if (!chunk.empty())
        {
            collected += chunk;
            emu.Feed(reinterpret_cast<const uint8_t*>(chunk.data()), chunk.size());
        }
        usleep(20000);
    }

    bool found = false;
    for (int r = 0; r < emu.Rows(); ++r)
    {
        if (RowText(emu, r).find("HELLO world") != std::string::npos)
        {
            found = true;
            // Confirm the "HELLO" portion actually carries a non-default
            // (red) foreground color from the escape sequence.
            int col = (int)RowText(emu, r).find("HELLO");
            assert(!emu.CellAt(r, col).fg.isDefault);
            assert(emu.CellAt(r, col).fg.r == 0xCD);
            printf("[ok] found colored output on row %d: \"%s\"\n", r, RowText(emu, r).c_str());
            break;
        }
    }
    if (!found)
    {
        printf("RAW BYTES: [%s]\n", collected.c_str());
        printf("backend.HasExited()=%d exitCode=%d\n", (int)backend.HasExited(), backend.ExitCode());
        for (int r = 0; r < emu.Rows(); ++r)
        {
            std::string t = RowText(emu, r);
            if (!t.empty()) printf("row %d: [%s]\n", r, t.c_str());
        }
    }
    assert(found);

    backend.Write("exit\n");
    for (int i = 0; i < 500 && !backend.HasExited(); ++i)
    {
        std::string chunk;
        backend.ReadAvailable(chunk);
        usleep(20000);
    }
    assert(backend.HasExited());
    printf("[ok] shell exited cleanly, code=%d\n", backend.ExitCode());

    printf("\nAll e2e tests passed.\n");
    return 0;
}
