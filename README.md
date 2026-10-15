# Mini File Explorer

A minimal VS Code–shell-inspired file explorer built with wxWidgets:
browser-style tabs (each tab = a folder location, with its own back/forward
history), an optional split view for dragging files between two locations,
a bottom terminal panel that runs your real OS shell, and a bottom info bar
(sharing that same slot) summarizing the current directory or selection.
Renders with native OS theming throughout.

See [TRACKER.md](TRACKER.md) for what's built, what's a deliberate
limitation vs. a bug, and what's still on the roadmap.

```
┌────────────────────────────┬───────────────────────────┐
│ Documents | Downloads | +  │ Pictures | +              │  <- two independent panes
├────────────────────────────┼───────────────────────────┤
│ ⮜ ⮞ ⮝ [C:\...\Documents]  │ ⮜ ⮞ ⮝ [C:\...\Pictures] │  <- back/fwd/up + address bar
│ Name ↑    Size  Type       │ Name          Size  Type  │
│ Projects  -     Folder     │ vacation.jpg  4 MB  JPG   │  <- drag a row from one
│ notes.txt 2 KB  TXT        │ wallpaper.png 2 MB  PNG   │     pane onto the other
├────────────────────────────┴───────────────────────────┤
│  Documents   Folder   Modified 2026-07-19 11:34        │  <- info bar (nothing
└────────────────────────────────────────────────────────┘     selected) OR terminal —
                                                               the two share one slot
```

## Requirements

- CMake 3.16+ (only needed for the CMake build path — `build.sh` doesn't
  use it)
- A C++17 compiler (MinGW-w64/GCC via MSYS2, MSVC, or clang)
- wxWidgets 3.2+ built with the `aui` component

## Setup

You need a C++ compiler and wxWidgets (plus CMake, if you're using that
build path). On Windows, **MSYS2** gives you all of that in one
lightweight install — a compiler toolchain plus a prebuilt wxWidgets
package, no compiling wxWidgets from source and no Visual Studio required.
If you already have a working toolchain, skip to
[Build](#build).

### Windows: MSYS2 (recommended — no Visual Studio needed)

1. Install [MSYS2](https://www.msys2.org/) (small installer, a few hundred
   MB installed — nowhere near a Visual Studio-sized footprint).
2. Open the **"MSYS2 MinGW64"** terminal specifically (not the plain MSYS2
   terminal — this distinction matters, since packages and PATH differ
   per subsystem).
3. Update packages (it may ask you to close and reopen the terminal once —
   do that, then rerun if prompted):
   ```
   pacman -Syu
   ```
4. Install the compiler toolchain, CMake, and wxWidgets together:
   ```
   pacman -S --needed mingw-w64-x86_64-toolchain mingw-w64-x86_64-cmake mingw-w64-x86_64-wxwidgets3.2-msw
   ```
   If that exact wxWidgets package name doesn't resolve, run
   `pacman -Ss wxwidgets` to see the current name in your repos and swap it
   in. (`mingw-w64-x86_64-cmake` is only needed if you're using the CMake
   build path — skip it if you'll only ever run `build.sh`.)

That's it — skip straight to [Build](#build). No separate
"install wxWidgets" step is needed: it's already installed system-wide
inside the MSYS2 MinGW64 environment.

**Heads up:** the built `.exe` depends on DLLs that live inside MSYS2's
`mingw64/bin` folder and won't run outside a shell that has that folder on
`PATH` — **unless you build with `build.sh`**, which copies the needed
DLLs next to the exe automatically. See
[Running the built exe](#running-the-built-exe) below for the CMake-build
version of this problem and its fixes.

### macOS

```
xcode-select --install
brew install cmake wxwidgets
```

### Linux (Debian/Ubuntu)

```
sudo apt install build-essential cmake libwxgtk3.2-dev
```

### Alternative on Windows: Visual Studio + vcpkg

If you'd rather use MSVC (e.g. you already have Visual Studio installed,
or need MSVC-specific tooling), that path still works:

1. Install [Visual Studio](https://visualstudio.microsoft.com/downloads/)
   (Community edition) with the **"Desktop development with C++"**
   workload, or just the standalone
   [Build Tools for Visual Studio](https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio)
   if you don't want the full IDE.
2. Install [CMake](https://cmake.org/download/), checking **"Add CMake to
   the system PATH"** during setup.
3. Install [vcpkg](https://github.com/microsoft/vcpkg):
   ```
   git clone https://github.com/microsoft/vcpkg C:\vcpkg
   C:\vcpkg\bootstrap-vcpkg.bat
   ```
   Then set a `VCPKG_ROOT` environment variable pointing to `C:\vcpkg`
   (Start menu → "Edit the system environment variables" → Environment
   Variables → add under "User variables"), and add `C:\vcpkg` to `Path`
   too. **Close and reopen your terminal** afterward — environment
   variable changes don't apply to terminals already open.
4. Install wxWidgets (compiles from source, 15–30+ minutes the first
   time):
   ```
   vcpkg install wxwidgets:x64-windows
   ```
5. Configure with the vcpkg toolchain file:
   ```
   cmake -B build -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake -A x64
   ```
   then build with `cmake --build build --config Release` as in the
   [Build](#build) section below (the binary lands in `build/Release/`
   with this generator, unlike the MSYS2 path).

## Build

**Windows (MSYS2 MinGW64 terminal) — `build.sh`, the fast one-shot path:**

```
./build.sh
```

This is the primary way this project gets built day to day. It compiles
directly with `g++`/`wx-config` (no CMake step, no separate configure
step — just re-run it after any source change), then automatically copies
every required MinGW/wxWidgets DLL next to the resulting exe and launches
it. The exe lands at `build/MiniFileExplorer.exe` and, thanks to the DLL
copying, is double-click runnable from Explorer afterward too — not just
launchable from an MSYS2 terminal. It globs `src/*.cpp`, so new source
files are picked up automatically without editing the script.

**Windows (MSYS2 MinGW64 terminal) — CMake path:**

```
cmake -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The binary lands at `build/MiniFileExplorer.exe` (MinGW Makefiles is a
single-config generator, so there's no `Release`/`Debug` subfolder like
with Visual Studio). Unlike `build.sh`, this does **not** copy DLLs next
to the exe — see [Running the built exe](#running-the-built-exe) below.

**Windows (Visual Studio + vcpkg alternative):** you already ran
`cmake -B build -DCMAKE_TOOLCHAIN_FILE=...` in the setup step, so just:

```
cmake --build build --config Release
```

Binary lands at `build/Release/MiniFileExplorer.exe`.

**macOS/Linux:**

```
cmake -B build
cmake --build build --config Release
```

Binary lands at `build/MiniFileExplorer`.

## Running the built exe

If you built with **`build.sh`**, the exe already has its DLLs copied
alongside it — just double-click it or run it from anywhere. Nothing
further to do; skip the rest of this section.

If you built with **CMake** on Windows (MSYS2/MinGW), `MiniFileExplorer.exe`
is dynamically linked against the wxWidgets runtime DLLs plus
`libstdc++-6.dll` and `libwinpthread-1.dll`. All of these live in your
MSYS2 installation's `mingw64/bin` folder, not next to the exe. If you
launch the exe somewhere that folder isn't on `PATH`, you'll see something
like:

```
MiniFileExplorer.exe - System Error
The code execution cannot proceed because wxbase32u_gcc_custom.dll was
not found. Reinstalling the program may fix this problem.
```

This is not a build error or missing dependency — the DLLs exist, Windows
just can't find them from wherever you launched the exe. The `_gcc_custom`
in the filename is simply how MSYS2 names its prebuilt wxWidgets shared
libraries; it doesn't indicate anything is misconfigured.

Pick whichever of these fits how you want to run the app:

**Option A — run it from the MSYS2 MinGW64 terminal (no setup needed)**

That terminal already has `mingw64/bin` on `PATH`, so this just works:

```
./build/MiniFileExplorer.exe
```

**Option B — make the exe double-clickable by copying its DLLs alongside it**

From the MSYS2 MinGW64 terminal, in your project's build output folder,
run `ldd` on the exe to get the exact list of required DLLs and where
each one currently resolves from:

```
ldd MiniFileExplorer.exe
```

Any line pointing into your MSYS2 install's `mingw64/bin` is one you need
to copy next to the exe. Typically that's the wxWidgets DLLs
(`wxbase32u_gcc_custom.dll`, `wxmsw32u_core_gcc_custom.dll`,
`wxmsw32u_aui_gcc_custom.dll`, etc. — exact names depend on your installed
version) plus `libstdc++-6.dll`, `libwinpthread-1.dll`, and
`libgcc_s_seh-1.dll`. Copy each of those files into the same folder as
`MiniFileExplorer.exe`. After that, double-clicking the exe from Explorer
(or launching it from a plain `cmd`/PowerShell window) works without
needing the MSYS2 terminal at all. (`build.sh` does exactly this step for
you automatically — see [Build](#build) above.)

**Option C — put `mingw64/bin` on your system `PATH`**

Add your MSYS2 installation's `mingw64/bin` folder to your Windows user
`PATH` (Environment Variables dialog → User variables → `Path` → New),
then open a fresh terminal. Every exe built against this MSYS2 toolchain
will find its DLLs from any terminal or launch method, not just this one
— convenient if you'll be building other MinGW projects too, though it's
a fairly heavy folder to add just for one app.

None of these are needed on macOS/Linux — those builds link against
system libraries already on the standard search path.

## Project layout

- `src/main.cpp` — app entry point (`wxIMPLEMENT_APP`)
- `src/MainFrame.*` — top-level window. Owns the `wxAuiManager`, a
  `wxSplitterWindow` holding one or two `ExplorerPane`s, the terminal, and
  the info bar. The terminal and info bar share one bottom AUI slot —
  `SetBottomSlotShowsTerminal()` switches which of the two occupies it.
  **This is not a simple `Show()`/`Hide()` pair** — see the big comment on
  that method before "simplifying" it. Short version: wxAUI caches a dock
  row's pixel height and only lets go of it when `LayoutAll()` observes
  that row as having zero panes in it at that exact moment; detaching one
  pane and adding another within a single `Update()` call never gives it
  that moment, so the row stays stuck at whatever size it last was (this
  is exactly the bug that made the info bar render as a huge mostly-empty
  strip after closing the terminal). The fix is to detach both, call
  `Update()` while the dock is genuinely empty, _then_ add the pane that
  should be shown. Also tracks which pane last had keyboard focus
  (`m_focusedPane`) so `Ctrl+T`/`Ctrl+W` and the terminal's startup
  directory apply to the right one
- `src/ExplorerPane.*` — one side of the (optional) split view: a tabbed
  notebook of `FileExplorerTab`s. `MainFrame` always has one; `Ctrl+\` (or
  View → Toggle Split View) adds/removes a second one for side-by-side
  browsing. Forwards each tab's selection/directory changes up to
  `MainFrame` so the info bar stays current
- `src/FileExplorerTab.*` — one tab = one folder location: toolbar
  (back/forward/up), editable address bar, and a details list of the
  folder's contents, sortable by clicking any column header (↑/↓
  indicator on the active one). Each tab keeps its own navigation history,
  like a browser tab. Rows are drag sources and drop targets — dragging a
  file onto a folder row (in this pane or the other split pane) moves it
  there via `wxRenameFile`. Right-click for New Folder/File, Move To/Copy
  To (folder-picker dialog), Rename, Delete, and Show More Info (native OS
  Properties dialog on Windows)
- `src/TerminalPanel.*` — spawns the OS shell via `wxProcess` + `wxExecute`,
  polls its stdout/stderr on a timer (bounded chunk reads — the stream-to-
  stream `Read()` overload blocks until EOF, which a live process never
  sends; stderr is polled before stdout since a shell always finishes
  writing a failed command's error before printing its next prompt). A
  single editable `wxTextCtrl` is both the scrollback and the live input
  line — you type directly where the shell's own prompt ends, with no
  separate input box. `m_editableFrom` tracks where the not-yet-sent line
  starts, and key handling blocks edits before that point so you can't
  type over old output. Up/Down recall command history. `cls` is
  special-cased client-side (see TRACKER.md) instead of being forwarded to
  the shell. The shell doesn't start at construction — `EnsureShellStarted`
  is called lazily by `MainFrame` the first time the terminal is opened,
  seeded with the focused pane's current directory, and is a no-op if a
  shell is already running (so re-opening the terminal doesn't disturb an
  existing session). `CloseSession` is the opposite: it kills the shell
  and clears the scrollback outright, used specifically for the terminal
  pane's own `[x]` (as opposed to just hiding the panel)
- `src/InfoBar.*` — the thin bottom status strip shown whenever the
  terminal isn't. Purely a display widget (`SetText`); `MainFrame` decides
  what text belongs there

## Behavior notes

- Double-clicking a **folder** navigates into it within the same tab.
- Double-clicking a **file** opens it with the OS default application
  (`wxLaunchDefaultApplication`) — same as Explorer/Finder.
- `Ctrl+T` opens a new tab at the current tab's location; `Ctrl+W` closes
  the active tab (at least one tab always stays open).
- `Ctrl+\` (or View → Toggle Split View) opens a second pane beside the
  first, starting at the same folder. Drag a file or folder row from either
  pane and drop it on a folder row (moves it _into_ that folder) or
  anywhere else in the list (moves it into whatever folder that pane is
  currently showing). Moves use `wxRenameFile`, so cross-drive moves aren't
  supported (matches how a plain rename-based move works on any OS).
- Click a column header to sort by that column; click again to reverse the
  direction. Folders always sort before files regardless of column.
- `Ctrl+backtick` (or View → Toggle Terminal) opens/closes the shared bottom
  terminal, hidden by default at startup. The info bar takes its place
  whenever it's closed. Closing the terminal this way (or via `Ctrl+backtick`again) **preserves the running shell session** — reopening it later
  drops you right back where you left off, scrollback and all. Clicking
  the terminal pane's own`[x]`close button is different: it actually
  ends the shell process and clears the scrollback, so the _next_ time
  the terminal is opened it starts a genuinely fresh session (see`TerminalPanel::CloseSession()`).
- The bottom info bar shows the currently open directory's own info (name,
  "Folder", modified date — the same shape a selected item shows) when
  nothing is selected, or the selected item's details (name, type, size,
  modified date) when one row is selected.
- View → About shows a short message about why this
  app exists.

## Known limitations

See
[TRACKER.md](TRACKER.md#known-accepted-limitations-not-bugs--by-design-for-now)
for the full list (no custom dark theme, terminal isn't a real PTY,
single-selection only, etc.) and for what's still on the roadmap.

## Licence

Free for personal, non-commercial use. You may use, study and modify this
software for yourself, but you may not use it commercially, and you may not
share or publish it, in its original form or modified. See [LICENSE](LICENSE)
for the full terms.
