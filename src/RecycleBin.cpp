// IFileOperation and the FOFX_* flags are Vista-era API. The MinGW headers
// hide them unless the target version is declared before windows.h is pulled
// in, and the symptom is confusing: IFileOperation is left as an incomplete
// forward declaration, so the errors talk about an incomplete type rather than
// about a missing version macro.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x06010000
#endif

#include "RecycleBin.h"

#include <windows.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <objbase.h>

#include <algorithm>

namespace fileops
{

std::size_t DeleteResult::succeeded() const
{
    return static_cast<std::size_t>(
        std::count_if(outcomes.begin(), outcomes.end(),
                      [](const DeleteOutcome& outcome) { return outcome.succeeded; }));
}

bool DeleteResult::allSucceeded() const
{
    return !outcomes.empty() && succeeded() == outcomes.size();
}

bool IsAbsolutePath(const std::wstring& path)
{
    if (path.size() >= 3 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/'))
    {
        const wchar_t drive = path[0];
        return (drive >= L'A' && drive <= L'Z') || (drive >= L'a' && drive <= L'z');
    }

    // A UNC path: \\server\share\...
    if (path.size() >= 2 && (path[0] == L'\\') && (path[1] == L'\\'))
        return true;

    return false;
}

std::wstring BuildDoubleNullTerminatedList(const std::vector<std::wstring>& paths)
{
    // SHFileOperationW takes pFrom as a sequence of null-terminated strings
    // ending in a second null. Building this with a plain wstring works only
    // because embedded nulls are preserved; using c_str() on a
    // newline-separated string, as is sometimes suggested, silently sends only
    // the first path.
    std::wstring packed;

    for (const std::wstring& path : paths)
    {
        packed.append(path);
        packed.push_back(L'\0');
    }

    packed.push_back(L'\0');

    return packed;
}

namespace
{

/** Whether a path currently exists on disk. */
bool PathExists(const std::wstring& path)
{
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

/** Result of asking the shell to do something, before per-path verification. */
struct ShellCallResult
{
    bool invoked = false;   // the API ran at all
    bool cancelled = false; // the user cancelled the shell's own prompt
};

/**
 * Ask the shell to delete, via IFileOperation where available.
 *
 * FOF_NO_UI keeps the operation silent: the application has already asked for
 * confirmation, and a second shell prompt would be a change to the interface.
 * FOFX_RECYCLEONDELETE is what puts items in the Recycle Bin.
 */
ShellCallResult RunFileOperation(const std::vector<std::wstring>& paths, bool recycle)
{
    ShellCallResult result;

    const HRESULT initialised = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool needsUninitialise = SUCCEEDED(initialised);

    // RPC_E_CHANGED_MODE means COM is already up in another mode, which is
    // usable; any other failure means it is not.
    if (FAILED(initialised) && initialised != RPC_E_CHANGED_MODE)
        return result;

    IFileOperation* operation = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&operation));

    if (SUCCEEDED(hr))
    {
        DWORD flags = FOF_NO_UI | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;

        if (recycle)
            flags |= FOFX_RECYCLEONDELETE | FOF_ALLOWUNDO;

        hr = operation->SetOperationFlags(flags);

        for (const std::wstring& path : paths)
        {
            if (FAILED(hr))
                break;

            IShellItem* item = nullptr;
            hr = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item));

            if (SUCCEEDED(hr))
            {
                hr = operation->DeleteItem(item, nullptr);
                item->Release();
            }
        }

        if (SUCCEEDED(hr))
        {
            hr = operation->PerformOperations();
            result.invoked = true;

            BOOL aborted = FALSE;
            if (SUCCEEDED(operation->GetAnyOperationsAborted(&aborted)))
                result.cancelled = (aborted == TRUE);
        }

        operation->Release();
    }

    if (needsUninitialise)
        CoUninitialize();

    if (result.invoked)
        return result;

    // Fall back to the older API. Same semantics, no progress reporting.
    SHFILEOPSTRUCTW operationStruct = {};
    std::wstring packed = BuildDoubleNullTerminatedList(paths);

    operationStruct.wFunc = FO_DELETE;
    operationStruct.pFrom = packed.c_str();
    operationStruct.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;

    if (recycle)
        operationStruct.fFlags |= FOF_ALLOWUNDO;

    const int code = SHFileOperationW(&operationStruct);

    result.invoked = true;
    result.cancelled = (operationStruct.fAnyOperationsAborted == TRUE);

    if (code != 0)
        result.invoked = true; // still ran; per-path checks below decide success

    return result;
}

DeleteResult PerformDelete(const std::vector<std::wstring>& paths, bool recycle)
{
    DeleteResult result;

    std::vector<std::wstring> usable;
    usable.reserve(paths.size());

    for (const std::wstring& path : paths)
    {
        DeleteOutcome outcome;
        outcome.path = path;

        if (path.empty())
        {
            outcome.error = L"Empty path.";
            result.outcomes.push_back(outcome);
            continue;
        }

        if (!IsAbsolutePath(path))
        {
            // The shell resolves a relative path against a working directory
            // this code does not control, so it could delete something other
            // than what the caller meant.
            outcome.error = L"Path is not absolute.";
            result.outcomes.push_back(outcome);
            continue;
        }

        if (!PathExists(path))
        {
            outcome.error = L"The item no longer exists.";
            result.outcomes.push_back(outcome);
            continue;
        }

        usable.push_back(path);
        result.outcomes.push_back(outcome);
    }

    if (usable.empty())
        return result;

    const ShellCallResult call = RunFileOperation(usable, recycle);
    result.cancelled = call.cancelled;

    // Success is decided by looking, not by the return code. The shell reports
    // partial results in ways that vary between versions, and what actually
    // matters to the caller is whether the item is still there.
    for (DeleteOutcome& outcome : result.outcomes)
    {
        if (!outcome.error.empty())
            continue;

        if (!PathExists(outcome.path))
        {
            outcome.succeeded = true;
            continue;
        }

        outcome.error = call.cancelled
            ? L"Cancelled."
            : (recycle
                   ? L"Could not move to the Recycle Bin. The item may be too "
                     L"large for it, or on a drive without one."
                   : L"Could not delete the item.");
    }

    return result;
}

} // namespace

DeleteResult MoveToRecycleBin(const std::vector<std::wstring>& paths)
{
    return PerformDelete(paths, /*recycle=*/true);
}

DeleteResult DeletePermanently(const std::vector<std::wstring>& paths)
{
    return PerformDelete(paths, /*recycle=*/false);
}

} // namespace fileops
