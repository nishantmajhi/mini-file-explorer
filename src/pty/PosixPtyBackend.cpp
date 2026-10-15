#include "PosixPtyBackend.h"

#ifndef __WXMSW__

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

PosixPtyBackend::~PosixPtyBackend()
{
    Terminate();
    if (m_masterFd >= 0)
        close(m_masterFd);
}

bool PosixPtyBackend::Spawn(const wxString& programPath, const std::vector<wxString>& args,
                            const wxString& cwd, int cols, int rows, wxString* errorOut)
{
    struct winsize ws {};
    ws.ws_col = (unsigned short)std::max(1, cols);
    ws.ws_row = (unsigned short)std::max(1, rows);

    pid_t pid = forkpty(&m_masterFd, nullptr, nullptr, &ws);
    if (pid < 0)
    {
        if (errorOut) *errorOut = wxString::Format("forkpty failed: %s", strerror(errno));
        return false;
    }

    if (pid == 0)
    {
        // Child: now running with the pty slave as its controlling
        // terminal and fds 0/1/2 already wired to it by forkpty().
        if (!cwd.IsEmpty())
        {
            if (chdir(cwd.utf8_string().c_str()) != 0)
            {
                // Not fatal — fall back to whatever cwd we inherited
                // (matches the old wxExecuteEnv::cwd behavior of just not
                // touching things further if the directory turned out to
                // be gone).
            }
        }
        setenv("TERM", "xterm-256color", 1);

        std::string progUtf8 = programPath.utf8_string();
        std::vector<std::string> argStorage;
        argStorage.push_back(progUtf8);
        for (auto& a : args) argStorage.push_back(a.utf8_string());

        std::vector<char*> argv;
        argv.reserve(argStorage.size() + 1);
        for (auto& s : argStorage) argv.push_back(const_cast<char*>(s.c_str()));
        argv.push_back(nullptr);

        execvp(progUtf8.c_str(), argv.data());
        // Only reached if execvp failed.
        fprintf(stderr, "[pty] failed to exec %s: %s\r\n", progUtf8.c_str(), strerror(errno));
        _exit(127);
    }

    // Parent.
    m_childPid = pid;
    int flags = fcntl(m_masterFd, F_GETFL, 0);
    fcntl(m_masterFd, F_SETFL, flags | O_NONBLOCK);
    return true;
}

void PosixPtyBackend::Resize(int cols, int rows)
{
    if (m_masterFd < 0) return;
    struct winsize ws {};
    ws.ws_col = (unsigned short)std::max(1, cols);
    ws.ws_row = (unsigned short)std::max(1, rows);
    ioctl(m_masterFd, TIOCSWINSZ, &ws);
}

void PosixPtyBackend::Write(const std::string& data)
{
    if (m_masterFd < 0 || data.empty()) return;

    size_t written = 0;
    while (written < data.size())
    {
        ssize_t n = write(m_masterFd, data.data() + written, data.size() - written);
        if (n > 0) { written += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN)
        {
            // Master-side write buffer is full (only realistic with a huge
            // paste while the child is stalled). Wait briefly for it to
            // drain rather than busy-spinning or dropping bytes.
            fd_set wfds; FD_ZERO(&wfds); FD_SET(m_masterFd, &wfds);
            struct timeval tv { 0, 50000 }; // 50ms
            int r = select(m_masterFd + 1, nullptr, &wfds, nullptr, &tv);
            if (r <= 0) break; // give up rather than hang the UI thread
            continue;
        }
        break; // fatal write error — drop the rest silently
    }
}

void PosixPtyBackend::ReapIfExited()
{
    if (m_exited || m_childPid <= 0) return;
    int status = 0;
    pid_t r = waitpid(m_childPid, &status, WNOHANG);
    if (r == m_childPid)
    {
        m_exited = true;
        m_exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
}

size_t PosixPtyBackend::ReadAvailable(std::string& out, size_t maxBytes)
{
    if (m_masterFd < 0) return 0;

    size_t total = 0;
    char buf[4096];
    while (total < maxBytes)
    {
        ssize_t n = read(m_masterFd, buf, sizeof(buf));
        if (n > 0)
        {
            out.append(buf, (size_t)n);
            total += (size_t)n;
            if ((size_t)n < sizeof(buf))
                break; // short read: almost certainly drained what's ready right now
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN) break; // nothing more ready right now — not an error
        // n == 0, or a hard error (commonly EIO once the slave side has no
        // more open fds): the child's side of the pty is gone.
        break;
    }
    ReapIfExited();
    return total;
}

void PosixPtyBackend::Terminate()
{
    if (m_childPid > 0)
    {
        // SIGHUP mirrors a real terminal hangup — well-behaved shells and
        // full-screen apps already handle it as "the terminal went away",
        // more gracefully than an immediate SIGKILL.
        kill(m_childPid, SIGHUP);
        kill(-m_childPid, SIGHUP); // also reach the whole process group, if any
        int status = 0;
        // Give it a brief moment, then escalate if it's still around.
        for (int i = 0; i < 20; ++i) // ~200ms
        {
            pid_t r = waitpid(m_childPid, &status, WNOHANG);
            if (r == m_childPid) { m_exited = true; m_childPid = -1; return; }
            usleep(10000);
        }
        kill(m_childPid, SIGKILL);
        waitpid(m_childPid, &status, 0);
        m_exited = true;
        m_childPid = -1;
    }
}

#endif // !__WXMSW__
