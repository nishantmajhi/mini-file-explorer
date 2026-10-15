/**
 * Tests for src/RecycleBin.cpp.
 *
 * Deliberately standalone: no wxWidgets, no test framework. Build and run with
 *
 *     g++ -std=c++17 -municode -o tests/recyclebin_test.exe \
 *         tests/recyclebin_test.cpp src/RecycleBin.cpp -lole32 -lshell32 -luuid
 *     ./tests/recyclebin_test.exe
 *
 * The point of these is the one the old code got wrong: after a delete, the
 * file must be *recoverable*, not merely gone. So the recycle tests check both
 * that the path disappeared and that the Recycle Bin's item count went up.
 */

#include "../src/RecycleBin.h"

#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{

int checksRun = 0;
int checksFailed = 0;
std::string currentTest;

void startTest(const char* name)
{
    currentTest = name;
    std::printf("  %s\n", name);
}

void check(bool condition, const char* what)
{
    ++checksRun;

    if (!condition)
    {
        ++checksFailed;
        std::printf("    FAIL: %s\n", what);
    }
}

/** A directory under %TEMP% that this run owns. */
std::wstring makeScratchDirectory()
{
    wchar_t tempPath[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tempPath);

    std::wstring directory = std::wstring(tempPath) + L"mfe-recyclebin-test-"
                             + std::to_wstring(GetCurrentProcessId()) + L"-"
                             + std::to_wstring(GetTickCount());

    CreateDirectoryW(directory.c_str(), nullptr);

    return directory;
}

std::wstring writeFile(const std::wstring& directory, const wchar_t* name, const char* contents)
{
    std::wstring path = directory + L"\\" + name;

    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (handle != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        WriteFile(handle, contents, static_cast<DWORD>(std::strlen(contents)), &written, nullptr);
        CloseHandle(handle);
    }

    return path;
}

bool exists(const std::wstring& path)
{
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

/** How many items the Recycle Bin currently holds, or -1 if it cannot be read. */
long long recycleBinItemCount()
{
    SHQUERYRBINFO info = {};
    info.cbSize = sizeof(info);

    if (FAILED(SHQueryRecycleBinW(nullptr, &info)))
        return -1;

    return static_cast<long long>(info.i64NumItems);
}

void removeTree(const std::wstring& directory)
{
    std::wstring packed = directory;
    packed.push_back(L'\0');
    packed.push_back(L'\0');

    SHFILEOPSTRUCTW operation = {};
    operation.wFunc = FO_DELETE;
    operation.pFrom = packed.c_str();
    operation.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    SHFileOperationW(&operation);
}

// ---------------------------------------------------------------------------

void testAbsolutePathDetection()
{
    startTest("absolute paths are recognised, relative ones refused");

    check(fileops::IsAbsolutePath(L"C:\\Users\\test\\file.txt"), "drive path");
    check(fileops::IsAbsolutePath(L"C:/Users/test/file.txt"), "forward slashes");
    check(fileops::IsAbsolutePath(L"\\\\server\\share\\file.txt"), "unc path");

    check(!fileops::IsAbsolutePath(L"file.txt"), "bare filename");
    check(!fileops::IsAbsolutePath(L"..\\file.txt"), "relative with traversal");
    check(!fileops::IsAbsolutePath(L"\\file.txt"), "rooted but no drive");
    check(!fileops::IsAbsolutePath(L""), "empty");
}

void testDoubleNullList()
{
    startTest("paths are packed with the double null the shell requires");

    std::wstring packed = fileops::BuildDoubleNullTerminatedList({L"A", L"BB"});

    // "A\0BB\0\0"
    check(packed.size() == 6, "total length includes every terminator");
    check(packed[0] == L'A', "first path");
    check(packed[1] == L'\0', "separator after the first");
    check(packed[2] == L'B' && packed[3] == L'B', "second path");
    check(packed[4] == L'\0' && packed[5] == L'\0', "list ends with two nulls");

    std::wstring empty = fileops::BuildDoubleNullTerminatedList({});
    check(empty.size() == 1 && empty[0] == L'\0', "an empty list is just the terminator");
}

void testRelativePathsAreRefused()
{
    startTest("a relative path is refused rather than resolved");

    fileops::DeleteResult result = fileops::MoveToRecycleBin({L"some-relative-file.txt"});

    check(result.outcomes.size() == 1, "one outcome per requested path");
    check(!result.outcomes[0].succeeded, "not reported as deleted");
    check(!result.outcomes[0].error.empty(), "a reason is given");
}

void testMissingPathIsReported()
{
    startTest("a path that does not exist is reported, not silently ignored");

    std::wstring scratch = makeScratchDirectory();
    std::wstring absent = scratch + L"\\never-created.txt";

    fileops::DeleteResult result = fileops::MoveToRecycleBin({absent});

    check(result.outcomes.size() == 1, "one outcome");
    check(!result.outcomes[0].succeeded, "not reported as deleted");

    removeTree(scratch);
}

void testFileGoesToTheRecycleBin()
{
    startTest("a deleted file is recoverable, not destroyed");

    std::wstring scratch = makeScratchDirectory();
    std::wstring file = writeFile(scratch, L"document.txt", "important");

    check(exists(file), "the file was created");

    const long long before = recycleBinItemCount();

    fileops::DeleteResult result = fileops::MoveToRecycleBin({file});

    check(result.allSucceeded(), "the delete reported success");
    check(!exists(file), "the file is gone from its original path");

    const long long after = recycleBinItemCount();

    if (before >= 0 && after >= 0)
    {
        // This is the assertion that separates a recycle from a destroy, and
        // the one the previous implementation would have failed.
        check(after > before, "the item count in the Recycle Bin went up");
    }
    else
    {
        std::printf("    (note: Recycle Bin count unavailable; skipped that check)\n");
    }

    removeTree(scratch);
}

void testDirectoryGoesToTheRecycleBin()
{
    startTest("a deleted folder and its contents are recoverable");

    std::wstring scratch = makeScratchDirectory();
    std::wstring folder = scratch + L"\\project";
    CreateDirectoryW(folder.c_str(), nullptr);

    writeFile(folder, L"a.txt", "one");
    writeFile(folder, L"b.txt", "two");

    const long long before = recycleBinItemCount();

    fileops::DeleteResult result = fileops::MoveToRecycleBin({folder});

    check(result.allSucceeded(), "the delete reported success");
    check(!exists(folder), "the folder is gone");

    const long long after = recycleBinItemCount();

    if (before >= 0 && after >= 0)
        check(after > before, "the folder is in the Recycle Bin");

    removeTree(scratch);
}

void testMultiplePaths()
{
    startTest("several items are reported one outcome each");

    std::wstring scratch = makeScratchDirectory();
    std::wstring first = writeFile(scratch, L"one.txt", "1");
    std::wstring second = writeFile(scratch, L"two.txt", "2");
    std::wstring absent = scratch + L"\\three.txt";

    fileops::DeleteResult result = fileops::MoveToRecycleBin({first, second, absent});

    check(result.outcomes.size() == 3, "one outcome per requested path");
    check(result.succeeded() == 2, "the two real files were deleted");
    check(!result.allSucceeded(), "the missing one keeps allSucceeded false");
    check(!exists(first) && !exists(second), "both real files are gone");

    // Order is preserved, so a caller can match outcomes to what it asked for.
    check(result.outcomes[0].path == first, "first outcome matches first request");
    check(result.outcomes[2].path == absent, "third outcome matches third request");

    removeTree(scratch);
}

void testPermanentDeleteDoesNotRecycle()
{
    startTest("permanent delete really is permanent");

    std::wstring scratch = makeScratchDirectory();
    std::wstring file = writeFile(scratch, L"scratch.txt", "temporary");

    const long long before = recycleBinItemCount();

    fileops::DeleteResult result = fileops::DeletePermanently({file});

    check(result.allSucceeded(), "the delete reported success");
    check(!exists(file), "the file is gone");

    const long long after = recycleBinItemCount();

    if (before >= 0 && after >= 0)
        check(after == before, "nothing was added to the Recycle Bin");

    removeTree(scratch);
}

} // namespace

int main()
{
    std::printf("RecycleBin tests\n\n");

    testAbsolutePathDetection();
    testDoubleNullList();
    testRelativePathsAreRefused();
    testMissingPathIsReported();
    testFileGoesToTheRecycleBin();
    testDirectoryGoesToTheRecycleBin();
    testMultiplePaths();
    testPermanentDeleteDoesNotRecycle();

    std::printf("\n%d checks, %d failures\n", checksRun, checksFailed);

    return checksFailed == 0 ? 0 : 1;
}
