#include "WinConPtyBackend.h"

#ifdef __WXMSW__

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

namespace
{
// Builds a Win32 command line string from argv-style pieces, quoting each
// piece per the same rules CommandLineToArgvW expects on the way back in
// (doubling embedded quotes, escaping backslashes that immediately precede
// a quote). Plain arguments with no special characters are left unquoted
// for readability, matching how most Win32 tooling does it.
std::wstring QuoteArg(const std::wstring& arg)
{
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos)
        return arg;

    std::wstring out = L"\"";
    for (size_t i = 0; i < arg.size(); )
    {
        size_t backslashes = 0;
        while (i < arg.size() && arg[i] == L'\\') { ++backslashes; ++i; }

        if (i == arg.size())
        {
            out.append(backslashes * 2, L'\\'); // trailing backslashes before closing quote: double them
            break;
        }
        else if (arg[i] == L'"')
        {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
            ++i;
        }
        else
        {
            out.append(backslashes, L'\\');
            out.push_back(arg[i]);
            ++i;
        }
    }
    out.push_back(L'"');
    return out;
}

std::wstring BuildCommandLine(const wxString& programPath, const std::vector<wxString>& args)
{
    std::wstring cmd = QuoteArg(programPath.ToStdWstring());
    for (auto& a : args)
    {
        cmd.push_back(L' ');
        cmd += QuoteArg(a.ToStdWstring());
    }
    return cmd;
}
} // namespace

WinConPtyBackend::~WinConPtyBackend()
{
    Terminate();
    ClosePipesAndHandles();
}

void WinConPtyBackend::ClosePipesAndHandles()
{
    if (m_hPC) { ClosePseudoConsole(m_hPC); m_hPC = nullptr; }
    if (m_attrList) { DeleteProcThreadAttributeList(m_attrList); free(m_attrList); m_attrList = nullptr; }
    if (m_inputWrite) { CloseHandle(m_inputWrite); m_inputWrite = nullptr; }
    if (m_outputRead) { CloseHandle(m_outputRead); m_outputRead = nullptr; }
    if (m_thread) { CloseHandle(m_thread); m_thread = nullptr; }
    if (m_process) { CloseHandle(m_process); m_process = nullptr; }
}

bool WinConPtyBackend::Spawn(const wxString& programPath, const std::vector<wxString>& args,
                            const wxString& cwd, int cols, int rows, wxString* errorOut)
{
    HANDLE inputRead = nullptr, outputWrite = nullptr;

    // Two pipe pairs: ConPTY reads the child's "stdin" from inputRead (we
    // keep inputWrite to feed it); ConPTY writes the child's combined
    // stdout/stderr to outputWrite (we keep outputRead to consume it).
    if (!CreatePipe(&inputRead, &m_inputWrite, nullptr, 0) ||
        !CreatePipe(&m_outputRead, &outputWrite, nullptr, 0))
    {
        if (errorOut) *errorOut = "CreatePipe failed";
        return false;
    }

    COORD size { (SHORT)std::max(1, cols), (SHORT)std::max(1, rows) };
    HRESULT hr = CreatePseudoConsole(size, inputRead, outputWrite, 0, &m_hPC);
    // ConPTY duplicates the handles it needs internally; our copies of the
    // "other side" aren't needed once it's created.
    CloseHandle(inputRead);
    CloseHandle(outputWrite);

    if (FAILED(hr))
    {
        if (errorOut) *errorOut = wxString::Format("CreatePseudoConsole failed (0x%08lx) — Windows 10 1809+ is required", (unsigned long)hr);
        ClosePipesAndHandles();
        return false;
    }

    STARTUPINFOEX siEx {};
    siEx.StartupInfo.cb = sizeof(STARTUPINFOEX);

    SIZE_T attrListSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrListSize);
    m_attrList = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(attrListSize);
    if (!m_attrList || !InitializeProcThreadAttributeList(m_attrList, 1, 0, &attrListSize))
    {
        if (errorOut) *errorOut = "InitializeProcThreadAttributeList failed";
        ClosePipesAndHandles();
        return false;
    }
    if (!UpdateProcThreadAttribute(m_attrList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                    m_hPC, sizeof(HPCON), nullptr, nullptr))
    {
        if (errorOut) *errorOut = "UpdateProcThreadAttribute failed";
        ClosePipesAndHandles();
        return false;
    }
    siEx.lpAttributeList = m_attrList;

    std::wstring cmdLine = BuildCommandLine(programPath, args);
    // CreateProcessW is allowed to mutate its lpCommandLine buffer in place.
    std::vector<wchar_t> cmdLineBuf(cmdLine.begin(), cmdLine.end());
    cmdLineBuf.push_back(L'\0');

    std::wstring cwdW = cwd.ToStdWstring();

    PROCESS_INFORMATION pi {};
    BOOL ok = CreateProcessW(
        nullptr,
        cmdLineBuf.data(),
        nullptr, nullptr,
        FALSE, // don't inherit handles — ConPTY's own duplication is what matters
        EXTENDED_STARTUPINFO_PRESENT,
        nullptr,
        cwdW.empty() ? nullptr : cwdW.c_str(),
        &siEx.StartupInfo,
        &pi);

    if (!ok)
    {
        if (errorOut) *errorOut = wxString::Format("CreateProcess failed (error %lu)", GetLastError());
        ClosePipesAndHandles();
        return false;
    }

    m_process = pi.hProcess;
    m_thread = pi.hThread;
    m_processId = pi.dwProcessId;
    return true;
}

void WinConPtyBackend::Resize(int cols, int rows)
{
    if (!m_hPC) return;
    COORD size { (SHORT)std::max(1, cols), (SHORT)std::max(1, rows) };
    ResizePseudoConsole(m_hPC, size);
}

void WinConPtyBackend::Write(const std::string& data)
{
    if (!m_inputWrite || data.empty()) return;
    DWORD written = 0;
    // WriteFile on an anonymous pipe blocks only if the pipe's internal
    // buffer is full, which for interactive typing / small pastes in
    // practice never happens; a genuinely huge paste could stall the UI
    // thread briefly, same tradeoff the POSIX backend makes for its rare
    // EAGAIN-retry path.
    WriteFile(m_inputWrite, data.data(), (DWORD)data.size(), &written, nullptr);
}

size_t WinConPtyBackend::ReadAvailable(std::string& out, size_t maxBytes)
{
    if (!m_outputRead) return 0;

    DWORD available = 0;
    if (!PeekNamedPipe(m_outputRead, nullptr, 0, nullptr, &available, nullptr) || available == 0)
    {
        HasExited(); // opportunistically refresh exit state even when idle
        return 0;
    }

    size_t toRead = std::min((size_t)available, maxBytes);
    std::vector<char> buf(toRead);
    DWORD read = 0;
    if (!ReadFile(m_outputRead, buf.data(), (DWORD)toRead, &read, nullptr) || read == 0)
        return 0;

    out.append(buf.data(), read);
    return read;
}

bool WinConPtyBackend::HasExited() const
{
    if (m_exited || !m_process) return m_exited;
    DWORD code = 0;
    if (GetExitCodeProcess(m_process, &code) && code != STILL_ACTIVE)
    {
        m_exited = true;
        m_exitCode = (int)code;
    }
    return m_exited;
}

void WinConPtyBackend::Terminate()
{
    if (m_process && !HasExited())
    {
        // Closing the pseudo console first is ConPTY's documented way to
        // signal hangup to the child (analogous to SIGHUP on POSIX); fall
        // back to a hard TerminateProcess if it's still around afterward.
        if (m_hPC) { ClosePseudoConsole(m_hPC); m_hPC = nullptr; }
        DWORD wait = WaitForSingleObject(m_process, 200);
        if (wait != WAIT_OBJECT_0)
            TerminateProcess(m_process, 1);
    }
}

#endif // __WXMSW__
