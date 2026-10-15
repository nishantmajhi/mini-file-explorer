# Code Review — Mini File Explorer

Original review 2026-09-04. **Updated 2026-09-05 after the fix on branch
`cleanup/2026-09-05` (unpushed).**

## Status

| # | Finding | State |
|---|---|---|
| 1 | Delete was permanent and recursive, bypassing the Recycle Bin | **Fixed** — see caveat |
| 2 | Delete loop continued past failures with only a log warning | **Fixed** — each item reported |
| 3 | 20 build binaries committed; no `.gitignore` at all | **Fixed** |
| 4 | Cross-volume moves fail (`wxRenameFile` cannot cross filesystems) | **Open** |
| 5 | Copy has no progress or cancel, runs on the UI thread | **Open** |
| 6 | No conflict handling on paste | **Open** |
| 7 | ConPTY child may outlive its tab | **Open** |
| 8 | `FileExplorerTab.cpp` is 1,336 lines | **Open** |
| 9 | No `LICENSE` | **Open** |

## The caveat, and it matters

**The application was not rebuilt.** wxWidgets exists on this machine only as an
MSVC build (`D:\wxWidgets-3.3.1\lib\vc_x64_lib`) while this project uses gcc,
and building wxWidgets from source was not a good use of the night.

So the work splits in two:

- **`src/RecycleBin.{h,cpp}` is verified.** It compiles, and its 34 tests pass
  against real files. It has no wxWidgets dependency precisely so this was
  possible.
- **The call site in `src/FileExplorerTab.cpp` is not.** The change is
  mechanical — build a `std::vector<std::wstring>`, call `MoveToRecycleBin`,
  report each outcome — but it has never been through a compiler.

**Build it before you trust it.** If it does not compile, the likely spots are
the two string conversions (`ToStdWstring()` on the way in, `wxString` from
`const wchar_t*` on the way out).

## What the tests actually prove

```sh
g++ -std=c++17 -o tests/recyclebin_test.exe \
    tests/recyclebin_test.cpp src/RecycleBin.cpp -lole32 -lshell32 -luuid
./tests/recyclebin_test.exe
```

The assertions worth knowing about compare `SHQueryRecycleBinW`'s item count
before and after a delete. That is what separates "the file is gone" from "the
file can be got back" — the old implementation would have passed the first and
failed the second, which is exactly the bug. There are cases for a single file,
a folder with contents, a mixed batch where one item is missing, and a check
that `DeletePermanently` really does bypass the bin.

## A decision I made that you should agree with or reverse

**An item that cannot be recycled is reported and left alone**, not deleted
permanently instead. This applies to items too large for the bin and to volumes
that do not have one — network shares, and USB sticks formatted without one.

The alternative is to fall back to a permanent delete so the item goes away as
the user asked. I chose not to, because silently turning "delete" into "destroy"
is the behaviour this change exists to remove. `DeletePermanently()` is there so
you can offer it explicitly, which is the version where the person gets to
decide.

The consequence to be aware of: deleting from a network share now reports a
failure where it previously succeeded. That is the intended trade, but it will
look like a regression if you hit it without expecting it.

## Remaining work, in order

**Cross-volume moves.** `wxRenameFile(src, dst, false)` fails across
filesystems. Dragging from `C:` to `D:` silently fails with a log warning. Now
that `RecycleBin.cpp` exists as a home for shell operations, `IFileOperation`'s
`MoveItem` would fix this and give progress and conflict handling at the same
time — which also covers items 5 and 6 above.

**Get long operations off the UI thread.** Copying a large folder freezes the
window with no way to cancel.

**Kill the ConPTY child when its tab closes.** An orphaned `cmd.exe` holds a
directory handle, which then prevents that folder being deleted or renamed —
a confusing failure to debug from the symptom.

Read `TRACKER.md` before any of this. It distinguishes deliberate limitations
from missing features, and it is accurate.

## Licence, 2026-10-02

The author chose one licence for all of their projects: free for personal,
non-commercial use, modifiable for the user's own use, and not to be
distributed, original or modified. No standard licence says that, so `LICENSE`
is a short custom text, the same in every project apart from the year and what
it excludes. The README's licence section says the same. Anything above that
calls the licence an open question, or calls the project MIT, is superseded.

## CI, 2026-10-02

`.github/workflows/ci.yml` builds and runs the recycle-bin tests with the
runner's g++ on `windows-latest`. Nothing has been pushed, so it has not run
on GitHub yet. Every job was run before committing, from a fresh clone of this
branch, the way a runner would run it: Windows jobs here, in PowerShell 7 or
the shell the step names, with only the tools the job sets up on `PATH`. 34.
