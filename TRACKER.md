# Project Tracker — Mini File Explorer

A snapshot of what's built, what's a deliberate limitation (not a bug),
and what's still on the roadmap. This is written for whoever — human or a
future Claude session — picks the project up next. It describes the
**current state of the code**, not a history of how it got here; treat it
as a living document and keep it that way (update it, don't just append to
it, when things change).

## ✅ Built and working

- Two-pane split view (`Ctrl+\` or View → Toggle Split View), each pane an
  independent tabbed notebook of folder locations with its own
  back/forward history (`Ctrl+T` new tab, `Ctrl+W` close tab)
- Details list (Name/Size/Type/Modified), sortable by clicking any column
  header — shows a ↑/↓ indicator on the active sort column. Folders always
  sort before files regardless of which column is active. Switching to a
  new column defaults to ascending (A-Z) for Name/Type, but *descending*
  (biggest/newest first) for Size/Modified — matches Explorer/Finder's own
  default direction for those two, since finding your largest files or
  most recent changes is the far more common reason to sort by either
- Drag a row from either pane's list onto a folder row (or empty list
  area) to move it there, including across panes in split view
- Multi-select in the file list (standard Ctrl/Shift-click, rubber-band
  select — the list no longer forces `wxLC_SINGLE_SEL`). The bottom info
  bar summarizes a multi-selection as an item/folder/file count rather than
  one item's Name/Type/Size/Modified. Drag-and-drop also drags the whole
  selection when the dragged row is part of it (dragging a row outside the
  current selection drags just that row instead, like Explorer/Finder)
- Clipboard-based Cut/Copy/Paste (`Ctrl+X`/`C`/`V`, scoped to whenever the
  file list itself has keyboard focus so the address bar's own text
  cut/copy/paste is untouched) as a faster alternative to the context
  menu's folder-picker-based Move To/Copy To, plus matching **Cut**/**Copy**/
  **Paste** context-menu items. This is an in-app clipboard (a static shared
  across every tab/pane, including the other split pane), not the real OS
  clipboard — same scope as drag-and-drop's own in-app-only payload, and it
  sidesteps needing Explorer's `CFSTR_PREFERREDDROPEFFECT` dance to make
  cut-vs-copy paste behave right against a real OS clipboard. A cut is
  one-shot like Explorer's own (pasting it clears the clipboard so a second
  `Ctrl+V` doesn't move the same items again); a copy can be pasted
  repeatedly
- Right-click context menu:
  - **New Folder** / **New File**
  - **Cut** / **Copy** / **Paste** — the clipboard actions above; Cut/Copy
    apply to the whole selection when right-clicked inside it (see below)
  - **Move To...** / **Copy To...** (folder-picker dialog) — on a specific
    item or the whole current selection, placed above Rename/Delete
  - **Rename** (single item only) / **Delete** (single item or bulk, with
    an item-count confirmation for multiple)
  - **Show More Info** (single item, or the current directory itself from
    a background right-click) — opens the real native OS Properties dialog
    on Windows (`ShellExecuteExW`, `properties` verb); falls back to a
    plain summary dialog on non-Windows builds
  - Right-clicking a row that's already part of a multi-selection keeps
    that whole selection as the menu's target (for bulk Cut/Copy/Move
    To/Copy To/Delete); right-clicking a row outside the current selection
    replaces it with just that row first, matching Explorer/Finder
- Bottom terminal (View → Toggle Terminal, or `Ctrl+backtick`) backed by a
  **real pseudo-terminal** — `forkpty` on POSIX, ConPTY on Windows (see
  `src/pty/`) — running the real OS shell (`cmd.exe` on Windows, `$SHELL`
  or `/bin/bash -i` otherwise). Since the shell gets an actual pty device
  instead of stdin/stdout/stderr piped through `wxProcess`, it controls
  its own cursor: ANSI colors (16/256/truecolor), and full-screen curses
  programs (`vim`, `top`, `htop`, `less`, `man`) all work, because
  `TerminalPanel` is a custom-painted character grid (`TerminalEmulator`,
  a VT100/xterm-subset parser) rather than a `wxTextCtrl`. `cls`/`clear`
  are no longer special-cased client-side — they're just forwarded to the
  shell and work because there's a real console/pty behind them now.
  Mouse-drag text selection (auto-copies on release) and Ctrl+Shift+C/V
  clipboard copy/paste replace what a native text control gave for free;
  scrollback (primary screen only, capped at 5000 lines) is reachable via
  the mouse wheel or Shift+PageUp/PageDown. **Hidden by default at
  startup** — the first time it's opened, it starts in whichever pane
  currently has keyboard focus (via the pty's own initial working
  directory, not by mutating the app's own). Closing it via the
  menu/shortcut preserves the running session (reopening drops you right
  back where you left off); closing it via the terminal pane's own `[x]`
  ends the session and clears the scrollback, so the next open starts
  fresh (`TerminalPanel::CloseSession`)
- Bottom info bar, occupying the same slot as the terminal — exactly one
  of the two is ever visible. Shows the currently open directory's own
  info (name, "Folder", modified date) when nothing is selected, the
  selected item's name/type/size/modified date when exactly one is, or an
  item/folder/file count summary when several are selected at once
- Details/Icons view toggle, per tab (an icon + "Icons"/"List" label next
  to the address bar — `wxART_LIST_VIEW`/`wxART_REPORT_VIEW` stock art, no
  custom-drawn icon of our own — showing whichever mode a click would
  switch TO; also reachable via `FileExplorerTab::ToggleViewMode()` for a
  future menu shortcut). The icon and label live inside `m_viewTogglePanel`,
  a small container that's a single toolbar sizer item rather than the two
  independent items it used to be — the click handler is bound on the
  container as well as both children, so the gap between the icon and the
  label is part of the clickable button instead of dead space that fell
  through to the toolbar background. `m_viewTogglePanel` also has a fixed
  min-width (measured once at construction as the wider of the two label
  strings, "Icons" and "List"), since the label's natural width changes
  between the two states and the toggle previously sat next to the
  address bar's `proportion=1` expanding sizer item — without the fixed
  width, every toggle visibly resized the address bar as the toolbar
  reflowed around the toggle's changing size. Details is the sortable
  Name/Size/Type/Modified report list (`wxListCtrl`, `wxLC_REPORT`); Icons
  is `FileIconGrid`
  (`src/FileIconGrid.h`), a **custom-painted widget**, not `wxListCtrl`'s
  own `wxLC_ICON` mode — that mode's generic (non-native) implementation on
  Linux/GTK (this project's only real build/test platform) lays items out
  column-major, wrapping into a new column based on the control's
  **height**, not its width, and completely ignores window resizes as a
  result (confirmed directly: `wxLIST_ALIGN_TOP` and `wxLIST_ALIGN_LEFT`
  produce identical layouts, and `SetItemPosition()` returns false/is a
  no-op on this implementation — there's no style flag or workaround that
  makes it reflow row-major by width the way a real file manager's icon
  grid, or CSS flexbox with `flex-wrap`, actually behaves). `FileIconGrid`
  implements that itself: fixed-size cells, column count recomputed from
  the current client width on every resize, verified empirically to
  reflow correctly on both grow *and* shrink (measured column counts of
  12/12/7/4 across 1200/1250/700/450px widths in the same live session,
  including growing back up after shrinking — ruling out a
  `wxScrolledWindow` virtual-size caching quirk hit and worked around
  during development, see the implementation gotchas below if
  reintroducing anything sizer-based here). `m_list` stays fully
  populated at all times regardless of which view is showing (it's the
  shared by-row-index data source both views key off), just hidden when
  Icons mode is active; selection, activation, drag-and-drop, and the
  context menu are implemented once (in `FileExplorerTab`, keyed by row
  index) and shared by both views rather than duplicated
- Colored, recognizable per-file-type icons (not one generic icon for
  every non-image file) via the OS's own facilities — `FileIconProvider`
  (`src/FileIconProvider.h`) tries the platform's real icon lookup first,
  falling back to the generic `wxART_NORMAL_FILE`/`wxART_FOLDER` icon only
  where that's unavailable or fails, per file *extension* (cached once per
  unique extension per directory listing, not once per file, since neither
  platform's lookup actually needs the real file to exist — see the header
  comment for why that's safe). Linux/GTK: GIO's mime-type content guess +
  `GtkIconTheme`, the same mechanism Nautilus/most Linux file managers use
  — verified directly (a probe script showed genuinely distinct colored
  icons for `.pdf`/`.zip`/`.cpp`/`.mp3`). Windows: `SHGetFileInfo` with
  `SHGFI_USEFILEATTRIBUTES` (registered file-type icon, no disk access) for
  the 16px list-view icon; the 64px grid icon instead comes from the
  `SHIL_JUMBO` (256x256) system image list via `SHGetImageList`, downscaled
  to size, falling back to upscaling `SHGFI_LARGEICON`'s own ~32px result
  only if that lookup fails — `SHGFI_LARGEICON` alone was tried first and
  produced visibly blurry icons once scaled up to fill a 64px grid cell,
  since it never returns anything above ~32-48px regardless of the size
  requested. **Unverified on real hardware**, same caveat as
  `WinConPtyBackend` and for the same reason (no Windows build/test machine
  available), written against the documented Shell API and syntax-/link-
  checked via a mingw-w64 cross-compile against real Win32 headers and
  import libraries — that link check is what caught a real MinGW gotcha:
  `commoncontrols.h` declares `IID_IImageList` but MinGW's import libraries
  don't export it, so linking against it as declared fails with
  `undefined reference to IID_IImageList`; `FileIconProvider.cpp` defines
  the GUID locally from its documented value instead of relying on the
  import lib.
  Image thumbnails (recognized extensions:
  `png/jpg/jpeg/bmp/gif/ico/pcx/tga/tif/tiff/xpm`, decoded via `wxImage`
  and letterboxed onto a transparent square so non-square images aren't
  stretched — `wxInitAllImageHandlers()` in `main.cpp` is what makes
  JPEG/GIF/TIFF/etc. actually decode, without it only BMP would work;
  capped at 200 real thumbnails per directory listing and 15MB per source
  file, both decoded synchronously on the UI thread during
  `ReloadDirectory()`, so an uncapped folder full of huge/numerous images
  would visibly freeze the window — anything over either cap falls back to
  the icon lookup below like any non-image file) still take priority over
  even a correct type icon when a real one can be decoded — a thumbnail is
  strictly more informative — but now fall back to the *colored* type icon
  rather than the flat generic one when they can't be. Real on-demand
  thumbnails additionally come from `IShellItemImageFactory` on Windows
  (same unverified-hardware caveat), also letterboxed onto a transparent
  square rather than stretched — `GetImage` fits the source into the
  requested box preserving aspect ratio, it does *not* pad non-square
  results to an exact square itself, so an earlier version of this code
  that force-scaled straight to `largePx x largePx` visibly stretched
  landscape/portrait photos; letterboxing (the same approach the manual
  `wxImage`-decode fallback above already used) fixed it — for whatever
  the shell's own thumbnail providers cover; deliberately NOT implemented
  via Linux's GIO `thumbnail::path` file attribute, since that only
  surfaces thumbnails a background thumbnailer service
  (tumbler/gnome-thumbnail-factory) has already generated and cached
  asynchronously — there's no synchronous "generate one now" call behind
  it, and this app has no business trying to talk to or launch that
  service itself
- Native OS theming throughout — no custom colors anywhere (see Known
  limitations for why)
- Session persistence: on close, the current tabs (per pane, in order,
  plus which one's active), whether split view was on, the splitter's sash
  position, and whether the terminal was open are all written to a config
  **file** — `wxFileConfig` pointed at an explicit path under
  `wxStandardPaths::GetUserConfigDir()`, not `wxConfig::Get()`'s Windows-
  Registry default, since the ask was specifically for a file (also just
  easier for a user to find/back up/delete by hand). Restored on next
  launch, before the window is even shown; a saved tab whose folder no
  longer exists (deleted, renamed, a removable drive that isn't plugged in
  right now, ...) is silently skipped rather than failing the whole
  restore, falling back further to a single home-directory tab if every
  saved tab in a pane turns out to be gone. First run (no config file yet)
  falls back to today's original defaults untouched: one pane, one tab at
  the home directory, no split, terminal closed
- View → About

## Known, accepted limitations (not bugs — by design for now)

- **No custom dark theme.** An earlier version of this app hand-painted
  everything (notebook tabs, list header, terminal, AUI chrome) in a
  custom dark palette. In practice this fought Windows' native control
  theming and lost in places that were hard to fully win: the AUI
  notebook's native tab art (`wxAuiMSWTabArt`) paints tabs via
  `DrawThemeBackground` and hardcodes label text to
  `wxSYS_COLOUR_BTNTEXT` regardless of what colors get set on it, and the
  list column header (a separate native Header control) kept its own
  glossy visual-styles gradient even after recoloring it — the net result
  was a visibly inconsistent mix of themed and stubbornly-native-looking
  chrome. Rather than continuing to fight native control theming
  control-by-control, the entire custom theming layer (`Theme.h`,
  `DarkMode.h`/`.cpp`) was removed outright. The app now renders with
  whatever the OS's native theme actually is — light by default on a
  stock Windows install — consistently, everywhere, for free. If dark
  mode comes back, it should be the OS's own native dark mode (Windows
  10 1809+ has real dark-mode-aware common controls), not a hand-rolled
  palette fighting individual native controls one at a time.
- **Terminal is a real PTY but not a complete xterm.** `TerminalEmulator`
  (`src/pty/`) covers what full-screen curses apps and colored CLI tools
  actually use — cursor addressing, SGR colors, erase/insert/delete
  line/char, scroll regions, the alternate screen buffer — but doesn't
  implement sixel/image protocols, mouse reporting escape sequences,
  double-width/double-height lines, or Unicode combining
  characters/East-Asian width. Scrollback is capped at 5000 lines and only
  accumulates on the primary screen (matches real terminals: the
  alternate screen used by `vim`/`less`/`top` never gets scrollback,
  since those apps page their own history).
- **`WinConPtyBackend` (`src/pty/WinConPtyBackend.cpp`) has never actually
  run.** Same root cause as the Linux/GTK-only limitation below — this
  project has no Windows machine to build/run on — so the ConPTY path was
  written carefully against Microsoft's documented pattern and syntax-
  checked with a mingw-w64 cross-compile against the real
  `<windows.h>`/ConPTY declarations, but needs a real smoke test on
  Windows before it can be trusted the way the POSIX (`forkpty`) backend
  already has been (spawned a real shell, ran colored output, launched and
  cleanly exited `vim`, resized mid-session, confirmed clean child-process
  teardown — see `tests/README.md`).
- **`FileIconGrid` covers the common interactions, not every one
  `wxListCtrl` gives Details mode for free.** Click/Ctrl-click/Shift-click
  select, double-click activates, right-click shows the context menu, drag
  starts past a small movement threshold, arrow keys/Home/End/Enter
  navigate, and Ctrl+A/X/C/V all work — but there's no rubber-band/marquee
  selection by dragging over empty space (a click on empty space just
  clears the selection instead), and arrow-key navigation always sets a
  single new selection rather than supporting Shift+Arrow to extend a
  range the way Details mode's native list does. Selection *does* carry
  over correctly when switching between Details and Icons mode either
  direction (`FileExplorerTab::ToggleViewMode()` reads it from whichever
  view is being left and reapplies it to whichever is being entered).
- Drag-and-drop moves (and the context menu's "Move To...") use
  `wxRenameFile`, so cross-drive moves (e.g. `C:` to a `D:` drive) fail
  with a warning dialog rather than silently doing a copy+delete — matches
  how a plain rename-based move behaves on any OS.
- **No "This PC"-style drive list.** `GoUp()` on a drive root (e.g. `D:\`)
  is a no-op — there's no virtual root above individual drives to go up
  *to*, so clicking Up while already at `D:\` does nothing rather than
  showing `C:\`, `D:\`, `E:\`, etc. the way Explorer's own Up arrow does
  from a drive root. Implementing that would mean a virtual pseudo-path
  (enumerated via `wxGetLogicalDriveNames`/`wxGetDriveLetters`-style
  Windows APIs, not a real `wxDir` listing), which `ReloadDirectory()`,
  `NavigateTo()`, and the up/back/forward state would all need to special-
  case — a real feature to add, not a one-line fix.
- The bottom info bar's "nothing selected" summary deliberately skips the
  Modified date for a bare drive root (e.g. `D:\`, 0 path components) —
  `wxFileName::GetModificationTime()` on exactly that shape of path hits a
  wxWidgets-internal contradiction on MSW: `GetPath(wxPATH_GET_VOLUME)`
  intentionally keeps the trailing backslash for a root (so `"D:"` isn't
  misread as "current directory on D:" instead of "root of D:"), but
  `msw/dir.cpp`'s `wxGetDirectoryTimes()` asserts its input must *not* end
  in one — those two wx-internal expectations directly contradict each
  other for this one case. Every other path shape (subfolders, files) is
  unaffected; this only ever came up because opening a drive root's own
  info (nothing selected) is exactly the case that asks for a root's own
  mtime.
- On Windows, the built exe is dynamically linked against wxWidgets/MinGW
  DLLs that live inside MSYS2's `mingw64/bin`, so running it from
  somewhere that folder isn't on `PATH` fails with a "DLL was not found"
  error. Not a bug — see
  [Running the built exe](README.md#running-the-built-exe) in the README
  for the fix. `build.sh` (see below) sidesteps this automatically by
  copying the required DLLs next to the exe as part of every build.

## Implementation gotchas (read before touching this code)

Things that look like they could be simplified but shouldn't be, because
the extra complexity is there to work around a real, previously-hit bug:

- **`MainFrame::SetBottomSlotShowsTerminal()`'s two `Update()` calls.**
  The terminal and info bar share one bottom AUI dock slot. The natural-
  looking implementation — `Hide()` the one being closed, `Show()` the one
  being opened, one `Update()` at the end — doesn't work: it leaves the
  slot stuck at whichever pane's height was largest the last time it was
  sized (so closing the terminal after it's been open leaves the info bar
  rendering as a huge, mostly-empty strip instead of shrinking back to its
  ~28px height). The root cause is that wxAUI caches a dock row's pixel
  height and only recomputes it from the currently-attached panes when
  `LayoutAll()` observes that row as having _zero_ panes at that exact
  moment — and `DetachPane()` doesn't even hide the underlying window on
  its own, either. The fix current code uses: detach both panes, hide both
  windows explicitly, call `Update()` while the dock is genuinely empty
  (this is the call that actually matters — it's what makes wxAUI drop the
  stale cached height), _then_ `AddPane()` the one that should be visible
  and call `Update()` again. If you see two `Update()` calls back to back
  and it looks like a mistake, it isn't — see the comment on that method
  for the full explanation, confirmed against a deterministic test that
  inspects `wxAuiPaneInfo::rect` directly before concluding this fix
  worked.
- **`TerminalEmulator::ScrollUp()` only pushes to scrollback on a genuine
  full-screen scroll** (`top == 0 && bottom == rows - 1`, primary screen
  only). It would be simpler to push scrollback on every scroll
  regardless of region, but a curses app scrolling just a sub-region (a
  status line, a split window) isn't linear history — capturing it would
  make scrollback show incoherent fragments of whatever the app was
  redrawing internally. Don't "simplify" this to an unconditional push.
- **Sending two separate `Write()` calls to a pty right after `Spawn()`
  is a real race, not just a theoretical one.** It bit `tests/
  test_pty_e2e.cpp` directly: bash's readline flips terminal modes
  during its own startup, and two rapid-fire writes landing right as that
  happens occasionally left bash never processing the second one for the
  rest of the test's timeout (reproduced ~1 in 5 runs before the fix). The
  fix was combining them into one `Write()` call plus a short settle delay
  after spawn — not a longer timeout, which doesn't help since the
  process doesn't recover, it just never gets unstuck. This is purely a
  "send input immediately after spawning" hazard, not a general PTY I/O
  bug — a real user's keystrokes always arrive well after the shell has
  finished initializing.
- **Don't reach for `wxListCtrl`'s `wxLC_ICON` mode on Linux/GTK expecting
  it to reflow like a normal icon grid.** Confirmed directly (a throwaway
  probe app, not guesswork): `Arrange(wxLIST_ALIGN_TOP)` and
  `Arrange(wxLIST_ALIGN_LEFT)` produce identical layouts, both column-
  major — items fill top-to-bottom until the control's *height* is
  exhausted, then start a new column to the right, entirely independent
  of the control's width. Widening the window never adds more items per
  row; narrowing it never removes any (until a column stops fitting at
  all). `SetItemPosition()` looks like the escape hatch but isn't: it
  returns `false` unconditionally on this generic (non-native)
  implementation. This is why `FileIconGrid` exists as a fully custom
  widget instead — see its class-level comment for the reflow approach
  that does work (manual per-resize column-count recompute, not a sizer).
- **If a `wxScrolledWindow`'s child content is laid out by a sizer
  (`wxWrapSizer` in particular) and you see it grow but never shrink back
  down on subsequent resizes, that's the scrolled window's own virtual-
  size caching, not the sizer.** `wxWrapSizer` itself reflows correctly on
  both grow and shrink when tested on a plain `wxPanel` (confirmed via
  probe) — the moment that same sizer/panel is moved inside a
  `wxScrolledWindow` and driven via `Layout()` + `FitInside()`, resizing
  down after having been wider "gets stuck" at the widest size ever seen.
  `FileIconGrid` sidesteps the entire issue by not using a sizer at all —
  manual `SetVirtualSize()` driven directly by `GetClientSize()` on every
  `EVT_SIZE` doesn't have this failure mode. If a future change reaches
  for `wxWrapSizer` here again (e.g. to avoid hand-rolled cell math),
  expect to need the wxScrolledWindow-has-no-sizer-of-its-own,
  manually-managed-inner-panel workaround pattern, not just `Layout()` +
  `FitInside()`.
- **`xdotool click --window WINDOW_ID` does not reliably deliver the
  click to a wx/GTK app in this project's headless Xvfb test setup —
  `xdotool click` with no `--window` (targeting whatever's at the current
  pointer position, set via a prior `xdotool mousemove --window ...`)
  does.** Cost real time to track down: menu-bar clicks worked with
  `--window`, but clicks on ordinary child widgets (a toolbar button, the
  file list) silently did nothing — no error, no crash, just zero effect,
  confirmed by diffing before/after screenshots pixel-for-pixel. Also
  worth knowing for the same reason: `xdotool search --name "..."` must
  be re-run fresh before every `windowfocus`/interaction sequence rather
  than reusing a window ID captured earlier in the conversation — Xvfb
  reassigns IDs across restarts, and a stale one fails with an X
  `BadWindow` error on `windowfocus` that's easy to miss in a wall of
  unrelated Xvfb keymap warnings, after which every subsequent click in
  that sequence silently goes nowhere.

## Windows build script

`build.sh`, run from an MSYS2 MinGW64 terminal, is the primary way this
project actually gets built day to day (the README's Build section also
documents a CMake path, which is the more portable/IDE-friendly option,
but `build.sh` is the fast one-shot loop). It compiles directly with
`g++`/`wx-config` — no CMake step — then copies every required
MinGW/wxWidgets DLL next to the resulting exe (so it's double-click
runnable from Explorer afterward, not just launchable from an MSYS2
terminal) and launches it. It's a full rebuild every time, and it globs
`src/*.cpp` up to two levels deep automatically, so new source files under
`src/` (including `src/pty/`) never need to be added to it by hand —
unlike `CMakeLists.txt`, which lists sources explicitly and *does* need
updating (already done for `src/pty/*.cpp`, `src/FileIconGrid.cpp`, and
`src/FileIconProvider.cpp`, the latter also needing its own GTK3/GIO
`pkg_check_modules` block on Linux since `wx-config` doesn't expose those
flags/libs to consumers even though wxGTK itself depends on them). It
explicitly links `-lshell32 -lole32 -luuid -lgdi32` on top of whatever
`wx-config --libs all` already pulls in, since `FileIconProvider.cpp`'s
Windows implementation calls directly into the Shell API, COM, and GDI
(`GetObjectW`/`GetDIBits`/`DeleteObject` for reading icon/thumbnail
bitmaps) — `gdi32` was missing for a while and only surfaced as a real
build failure (`undefined reference to __imp_GetDIBits` etc.) rather than
at review time, since none of those three GDI calls are reachable from any
other source file. This
is also why the
standalone tests under `tests/` (see `tests/README.md`) deliberately live
outside `src/` — each has its own `main()`, and `build.sh`'s glob would
otherwise sweep them into every real build and fail to link.

## 📋 Not started (roadmap)

Nothing currently queued — the last item here (colored file-type icons)
shipped this round; see the "Colored, recognizable per-file-type icons"
bullet under Built and working.
