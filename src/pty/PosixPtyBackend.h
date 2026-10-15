#pragma once
#include "PtyBackend.h"

#ifndef __WXMSW__

class PosixPtyBackend : public IPtyBackend
{
public:
    ~PosixPtyBackend() override;

    bool Spawn(const wxString& programPath, const std::vector<wxString>& args,
               const wxString& cwd, int cols, int rows, wxString* errorOut) override;
    void Resize(int cols, int rows) override;
    void Write(const std::string& data) override;
    size_t ReadAvailable(std::string& out, size_t maxBytes = 65536) override;
    bool HasExited() const override { return m_exited; }
    int ExitCode() const override { return m_exitCode; }
    long GetPid() const override { return (long)m_childPid; }
    void Terminate() override;

private:
    int m_masterFd = -1;
    pid_t m_childPid = -1;
    bool m_exited = false;
    int m_exitCode = 0;

    void ReapIfExited();
};

#endif // !__WXMSW__
