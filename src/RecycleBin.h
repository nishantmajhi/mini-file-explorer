#pragma once

#include <string>
#include <vector>

/**
 * Deleting through the shell, so that deletions are recoverable.
 *
 * This exists because the delete action used to unlink directly:
 * wxRemoveFile for a file and wxFileName::Rmdir with wxPATH_RMDIR_RECURSIVE
 * for a directory. Neither goes anywhere near the Recycle Bin, so a mistaken
 * click on a folder destroyed it and everything under it with no way back.
 *
 * The application looks and behaves like Explorer, where Delete is recoverable
 * and Shift+Delete is the permanent one. Matching that expectation is the point
 * of this file.
 *
 * Deliberately free of any wxWidgets dependency, so it can be compiled and
 * tested on its own. See tests/recyclebin_test.cpp.
 */
namespace fileops
{

/** What happened to one path. */
struct DeleteOutcome
{
    /** The path as it was given. */
    std::wstring path;

    /** True if the item is gone from that path. */
    bool succeeded = false;

    /** Empty when succeeded; otherwise a description suitable for a user. */
    std::wstring error;
};

struct DeleteResult
{
    /** One entry per requested path, in the order given. */
    std::vector<DeleteOutcome> outcomes;

    /** True if the user cancelled at the shell's own confirmation prompt. */
    bool cancelled = false;

    /** Count of entries that succeeded. */
    std::size_t succeeded() const;

    /** True if every requested path was removed. */
    bool allSucceeded() const;
};

/**
 * Send paths to the Recycle Bin.
 *
 * Uses IFileOperation with FOFX_RECYCLEONDELETE, which is what Explorer itself
 * uses: the shell provides the progress dialog, the name-collision handling and
 * the undo entry. Falls back to SHFileOperationW with FOF_ALLOWUNDO on versions
 * where IFileOperation is unavailable.
 *
 * A path that cannot be recycled — an item larger than the bin, or one on a
 * volume with no bin, such as a network share or a USB stick formatted without
 * one — is reported as a failure rather than silently deleted permanently. The
 * caller decides whether to offer a permanent delete instead; that choice
 * belongs to the person, not to this function.
 *
 * @param paths Absolute paths. Relative paths are rejected, because the shell
 *              resolves them against a working directory this code does not
 *              control.
 */
DeleteResult MoveToRecycleBin(const std::vector<std::wstring>& paths);

/**
 * Delete paths outright, with no Recycle Bin.
 *
 * Provided so a caller can offer the Shift+Delete equivalent explicitly. It is
 * never the fallback for a failed recycle: silently turning "delete" into
 * "destroy" is exactly the behaviour this file exists to remove.
 */
DeleteResult DeletePermanently(const std::vector<std::wstring>& paths);

/** True if the path is absolute, which is what the shell functions require. */
bool IsAbsolutePath(const std::wstring& path);

/**
 * Pack paths into the double-null-terminated form SHFileOperationW expects.
 *
 * Exposed for testing: getting this wrong truncates the list silently, so the
 * shape is worth asserting on directly.
 */
std::wstring BuildDoubleNullTerminatedList(const std::vector<std::wstring>& paths);

} // namespace fileops
