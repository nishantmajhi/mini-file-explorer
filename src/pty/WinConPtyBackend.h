#pragma once
#include "PtyBackend.h"

#ifdef __WXMSW__

// ConPTY (CreatePseudoConsole/ResizePseudoConsole/ClosePseudoConsole) is
// only declared by <windows.h> when the target version is at least
// Windows 10 (they were added in the RS5 / 1809 SDK). Define these before
// including windows.h in case the project's own build settings target an
// older baseline for compatibility elsewhere.
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000006 // NTDDI_WIN10_RS5
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00 // _WIN32_WINNT_WIN10
#endif
#include <windows.h>

// ConPTY-based backend for Windows. UNVERIFIED ON REAL HARDWARE: this
// project's only available build/test environment is Linux/GTK (see
// TRACKER.md), so this file compiles only under __WXMSW__ and has never
// actually been run — it follows Microsoft's documented ConPTY pattern
// (CreatePseudoConsole + two pipe pairs + EXTENDED_STARTUPINFO_PRESENT)
// as closely as possible, but treat it as needing a real Windows smoke
// test before being trusted.
//
// Requires Windows 10 1809+ (build 17763) for CreatePseudoConsole; older
// Windows will fail Spawn() with an error rather than crash.
class WinConPtyBackend : public IPtyBackend
{
public:
    ~WinConPtyBackend() override;

    bool Spawn(const wxString& programPath, const std::vector<wxString>& args,
               const wxString& cwd, int cols, int rows, wxString* errorOut) override;
    void Resize(int cols, int rows) override;
    void Write(const std::string& data) override;
    size_t ReadAvailable(std::string& out, size_t maxBytes = 65536) override;
    bool HasExited() const override;
    int ExitCode() const override { return m_exitCode; }
    long GetPid() const override { return (long)m_processId; }
    void Terminate() override;

private:
    HPCON m_hPC = nullptr;
    HANDLE m_inputWrite = nullptr;  // we write here; ConPTY reads it as the child's stdin
    HANDLE m_outputRead = nullptr;  // we read here; ConPTY writes the child's stdout/stderr here
    HANDLE m_process = nullptr;
    HANDLE m_thread = nullptr;
    DWORD m_processId = 0;
    LPPROC_THREAD_ATTRIBUTE_LIST m_attrList = nullptr;
    mutable bool m_exited = false;
    mutable int m_exitCode = 0;

    void ClosePipesAndHandles();
};

#endif // __WXMSW__
