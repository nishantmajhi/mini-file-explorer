cmake_minimum_required(VERSION 3.16)
project(MiniFileExplorer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Requires: core, base, aui (docking panels)
find_package(wxWidgets REQUIRED COMPONENTS core base aui)
include(${wxWidgets_USE_FILE})

add_executable(MiniFileExplorer WIN32
    src/main.cpp
    src/MainFrame.cpp
    src/TerminalPanel.cpp
    src/FileExplorerTab.cpp
    src/FileIconGrid.cpp
    src/FileIconProvider.cpp
    src/ExplorerPane.cpp
    src/RecycleBin.cpp
    src/InfoBar.cpp
    src/pty/TerminalEmulator.cpp
    src/pty/PosixPtyBackend.cpp
    src/pty/WinConPtyBackend.cpp
    src/pty/PtyBackendFactory.cpp
)

target_include_directories(MiniFileExplorer PRIVATE src)
target_link_libraries(MiniFileExplorer ${wxWidgets_LIBRARIES})

if(UNIX AND NOT APPLE)
    # FileIconProvider's Linux implementation (colored per-file-type icons
    # via the desktop mime/icon theme) uses GTK3 + GIO directly — wx-config
    # doesn't expose their include paths/libs to consumers even though
    # wxGTK is itself built on top of them, so this needs its own
    # pkg-config lookup. Only relevant on Linux (this project's only real
    # build/test target besides Windows, which builds via build.sh/MSYS2
    # instead and never defines __WXGTK__, so FileIconProvider's Linux
    # branch — and this whole block — is simply compiled out there).
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GTK3 REQUIRED gtk+-3.0 gio-2.0)
    target_include_directories(MiniFileExplorer PRIVATE ${GTK3_INCLUDE_DIRS})
    target_link_libraries(MiniFileExplorer ${GTK3_LIBRARIES})
elseif(WIN32)
    # FileIconProvider's Windows implementation (colored per-file-type
    # icons via SHGetFileInfo/SHGetImageList, real thumbnails via
    # IShellItemImageFactory) calls directly into the Shell API, COM, and
    # GDI. wxWidgets' own libs may pull some of these in transitively, but
    # gdi32 specifically doesn't get dragged in that way and was missing
    # here for a while — same bug, same fix as build.sh's link line
    # (see the comment there): listing them explicitly is what actually
    # guarantees the link succeeds regardless of transitive linkage.
    target_link_libraries(MiniFileExplorer shell32 ole32 uuid gdi32)
endif()
