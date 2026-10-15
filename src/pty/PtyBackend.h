#pragma once
#include <wx/string.h>
#include <memory>
#include <string>
#include <vector>

// Spawns a shell attached to a real pseudo-terminal device (as opposed to
// TerminalPanel's previous wxProcess/pipe approach) so the shell itself
// believes it's talking to an interactive terminal: it allocates a window
// size, the kernel line-discipline handles job-control signals (Ctrl+C
// actually reaches the foreground process group), and full-screen curses
// programs (vim, top, less) can turn off canonical mode and read raw
// keystrokes.
//
// Two implementations: PosixPtyBackend (forkpty(), any non-Windows build)
// and WinConPtyBackend (Windows ConPTY, __WXMSW__ builds only). Callers go
// through CreatePtyBackend() and never touch either concrete class, so
// TerminalPanel itself has no #ifdef __WXMSW__ in it at all.
class IPtyBackend
{
public:
    virtual ~IPtyBackend() = default;

    // Spawns programPath (argv[0] implied; args are the rest of argv) with
    // cwd as its initial working directory and (cols, rows) as its initial
    // terminal size. Taking a program path + argument list rather than a
    // single shell-syntax string avoids re-parsing shell quoting/escaping
    // on either platform — POSIX execvp()'s and Windows CreateProcess()'s
    // argv both take it as already-split arguments. Returns false if the
    // spawn itself failed (backend-specific error already appended to
    // errorOut, if provided).
    virtual bool Spawn(const wxString& programPath, const std::vector<wxString>& args,
                       const wxString& cwd, int cols, int rows, wxString* errorOut) = 0;

    // Tells the pty device (and, on Windows, the ConPTY host) that the
    // window changed size, so the shell/curses apps re-query and redraw
    // rather than continuing to think the old size is still current.
    virtual void Resize(int cols, int rows) = 0;

    // Writes raw bytes to the pty's input side (i.e. what the shell reads
    // as its stdin) — normal keystrokes, control sequences for arrow
    // keys/Ctrl combos, and the emulator's own DA/DSR query responses all
    // go through here.
    virtual void Write(const std::string& data) = 0;

    // Non-blocking: appends whatever output is immediately available (up
    // to maxBytes) to out and returns the number of bytes appended. Never
    // blocks waiting for more data — callers poll this on a timer.
    virtual size_t ReadAvailable(std::string& out, size_t maxBytes = 65536) = 0;

    // True once the child has exited (detected the next time ReadAvailable
    // or a poll happens to notice) — NOT updated asynchronously the
    // instant the process dies.
    virtual bool HasExited() const = 0;
    virtual int ExitCode() const = 0;

    virtual long GetPid() const = 0;

    // Best-effort termination of the child process tree. Safe to call
    // multiple times and safe to call on an already-exited child.
    virtual void Terminate() = 0;
};

std::unique_ptr<IPtyBackend> CreatePtyBackend();
