#!/usr/bin/env bash
set -euo pipefail

# ---------------------------------------------------------------------------
# build.sh – Build MiniFileExplorer, bundle DLLs, and launch the exe.
# Run from anywhere inside (or at the root of) the project tree.
# Requires: MSYS2 MINGW64 environment with wxWidgets installed.
# ---------------------------------------------------------------------------

# ── 1. Locate the project root (directory that contains build.sh itself) ────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$SCRIPT_DIR"
echo "[build] Project root : $PROJECT_DIR"

# ── 2. Resolve toolchain ────────────────────────────────────────────────────
CXX="${CXX:-g++}"
WX_CONFIG="${WX_CONFIG:-wx-config}"

if ! command -v "$CXX" &>/dev/null; then
    echo "[error] C++ compiler not found: $CXX"
    exit 1
fi
if ! command -v "$WX_CONFIG" &>/dev/null; then
    echo "[error] wx-config not found. Install wxWidgets: pacman -S mingw-w64-x86_64-wxwidgets3.2-msw"
    exit 1
fi

# ── 3. Collect source files ─────────────────────────────────────────────────
SRC_DIR="$PROJECT_DIR/src"
if [[ ! -d "$SRC_DIR" ]]; then
    echo "[warn] No src/ directory found – compiling from project root."
    SRC_DIR="$PROJECT_DIR"
fi

mapfile -t SOURCES < <(find "$SRC_DIR" -maxdepth 2 -name "*.cpp" | sort)
if [[ ${#SOURCES[@]} -eq 0 ]]; then
    echo "[error] No .cpp source files found under $SRC_DIR"
    exit 1
fi
echo "[build] Sources      : ${SOURCES[*]}"

# ── 4. Prepare build directory ──────────────────────────────────────────────
BUILD_DIR="$PROJECT_DIR/build"
mkdir -p "$BUILD_DIR"
EXE="$BUILD_DIR/MiniFileExplorer.exe"

# ── 5. Compile ──────────────────────────────────────────────────────────────
WX_CXXFLAGS="$("$WX_CONFIG" --cxxflags)"
WX_LIBS="$("$WX_CONFIG" --libs all)"

echo "[build] Compiling..."
# -lshell32 -lole32 -luuid: FileIconProvider.cpp's Windows implementation
# (colored per-file-type icons via SHGetFileInfo, real thumbnails via
# IShellItemImageFactory) calls directly into the Shell API and COM.
# wx-config's own libs may already pull these in transitively, but listing
# them explicitly here is what actually guarantees the link succeeds
# regardless of that — harmless if redundant.
# shellcheck disable=SC2086
"$CXX" \
    -O2 \
    -Wall \
    $WX_CXXFLAGS \
    "${SOURCES[@]}" \
    $WX_LIBS \
    -lshell32 -lole32 -luuid -lgdi32 \
    -o "$EXE"

echo "[build] Compiled     : $EXE"

# ── 6. Collect required DLLs ────────────────────────────────────────────────
echo "[build] Bundling DLLs..."

# ldd lists every dependency. Depending on the MSYS2/ldd version, resolved
# paths show up either POSIX-style (/mingw64/bin/foo.dll) or native
# Windows-style (D:/msys64/mingw64/bin/foo.dll) — matching only the former
# used to silently find nothing on setups that print the latter. Instead of
# guessing the path style, just exclude anything under the Windows system
# directories (kernel32.dll, ntdll.dll, etc., which are always on PATH
# already) and keep everything else ldd resolved to a real file.
mapfile -t DLL_PATHS < <(
    ldd "$EXE" 2>/dev/null \
        | awk '{ print $3 }' \
        | grep -v '^$' \
        | grep -vi '/windows/system32/\|/windows/syswow64/' \
        | sort -u
)

if [[ ${#DLL_PATHS[@]} -eq 0 ]]; then
    echo "[warn] ldd returned no non-system DLLs – is ldd available?"
else
    for dll in "${DLL_PATHS[@]}"; do
        # Normalize to a path bash's -f/cp can rely on either way.
        dll="$(cygpath -u "$dll" 2>/dev/null || echo "$dll")"
        if [[ -f "$dll" ]]; then
            cp -u "$dll" "$BUILD_DIR/"
            echo "  copied: $(basename "$dll")"
        fi
    done
fi

# ── 7. Copy any resource files the app might need ───────────────────────────
for dir in resources res assets icons; do
    if [[ -d "$PROJECT_DIR/$dir" ]]; then
        cp -ru "$PROJECT_DIR/$dir" "$BUILD_DIR/"
        echo "[build] Copied resource dir: $dir"
    fi
done

# ── 8. Launch ────────────────────────────────────────────────────────────────
echo "[build] Launching $EXE ..."
cd "$BUILD_DIR"
start "" "$(cygpath -w "$EXE")"
