cmake_minimum_required(VERSION 3.24)
project(rivet-linux-host LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Release artifacts must not embed developer-machine library search paths;
# every linked library (GTK4, GIO, WebKitGTK, json-glib, curses, LZ4, zlib)
# is a system dependency resolved by the normal loader search on the target
# machine. libpdfium.so rides beside the executable and is dlopen'd (see
# src/pdfium.cpp), so it needs no search path either.
set(CMAKE_SKIP_RPATH TRUE)

# RIVET_ROOT points at the installed Rivet package. `raco rivet build` sets
# it; manual builds may export it themselves.
if(NOT DEFINED ENV{RIVET_ROOT})
  message(FATAL_ERROR "RIVET_ROOT is not set. Export RIVET_ROOT to a Rivet checkout.")
endif()
set(RIVET_ROOT "$ENV{RIVET_ROOT}")

# The Rivet platform is consumed as a package library target (project
# constraint, rivet#238): hosts never compile Rivet sources into their own
# executable lists — rivet adding a translation unit needs no downstream
# change.
add_subdirectory("${RIVET_ROOT}/platform/linux"
                 "${CMAKE_BINARY_DIR}/rivet-platform-build")

find_package(PkgConfig REQUIRED)
pkg_check_modules(JSON_GLIB REQUIRED IMPORTED_TARGET json-glib-1.0)

# EPUB rendering rides WebKitGTK's GTK4 API (webkitgtk-6.0, WebKitGTK 2.40+).
# Ubuntu 24.04+: libwebkitgtk-6.0-dev. Disable with -DPDFGIST_ENABLE_EPUB=OFF
# for minimal builds; the host then reports EPUB support as unavailable.
option(PDFGIST_ENABLE_EPUB "Render EPUB through WebKitGTK" ON)
set(PDFGIST_WEBKIT_INCLUDES "")
set(PDFGIST_WEBKIT_LIBDIRS "")
set(PDFGIST_WEBKIT_LIBS "")
if(PDFGIST_ENABLE_EPUB)
  # Deliberately not IMPORTED_TARGET: the imported target's include dirs
  # did not survive to the compile line in practice, and epub.cpp failed
  # with "webkit/WebKit.h: No such file or directory". Explicit dirs cannot
  # silently drop.
  pkg_check_modules(WEBKIT REQUIRED webkitgtk-6.0)
  message(STATUS "webkitgtk-6.0 Cflags: ${WEBKIT_CFLAGS}")
  message(STATUS "webkitgtk-6.0 include dirs: ${WEBKIT_INCLUDE_DIRS}")
  if(NOT WEBKIT_INCLUDE_DIRS)
    # Some distro .pc files expose the headers only through their file list;
    # fall back to the standard Debian/Ubuntu include root.
    find_path(WEBKIT_HEADER_DIR webkit/WebKit.h
      PATHS /usr/include/webkitgtk-6.0 /usr/local/include/webkitgtk-6.0
      REQUIRED)
    set(PDFGIST_WEBKIT_INCLUDES ${WEBKIT_HEADER_DIR})
  else()
    set(PDFGIST_WEBKIT_INCLUDES ${WEBKIT_INCLUDE_DIRS})
  endif()
  set(PDFGIST_WEBKIT_LIBDIRS ${WEBKIT_LIBRARY_DIRS})
  set(PDFGIST_WEBKIT_LIBS ${WEBKIT_LIBRARIES})
endif()

# ---------------------------------------------------------------------------
# PDFium (bblanchon/pdfium-binaries) — rendered pages, text layer, outline.
#
# Fetched at build time from a pinned release with a pinned sha256; binaries
# never enter git. The tag is kept in lockstep with $PdfiumVersion in
# windows/fetch-pdfium.ps1 (same chromium revision on both hosts); the two
# digests must move together on a bump: the tag has to exist for every pinned
# architecture on both platforms.
# ---------------------------------------------------------------------------
set(PDFGIST_PDFIUM_TAG "chromium/8100")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
  set(PDFGIST_PDFIUM_ASSET "pdfium-linux-arm64.tgz")
  set(PDFGIST_PDFIUM_SHA256 "58c87a95a40a24c914a9c755fae798843f46f06adf9e5e9f2c893302d4bdb040")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
  set(PDFGIST_PDFIUM_ASSET "pdfium-linux-x64.tgz")
  set(PDFGIST_PDFIUM_SHA256 "1c1894a12d4d96ee01400c3874444126254180c5c7d2ab628576b67f8120320a")
else()
  message(FATAL_ERROR "no pdfium-binaries asset pinned for ${CMAKE_SYSTEM_PROCESSOR}")
endif()

include(FetchContent)
FetchContent_Declare(pdfium
  URL "https://github.com/bblanchon/pdfium-binaries/releases/download/${PDFGIST_PDFIUM_TAG}/${PDFGIST_PDFIUM_ASSET}"
  URL_HASH "SHA256=${PDFGIST_PDFIUM_SHA256}"
)
FetchContent_MakeAvailable(pdfium)

if(NOT EXISTS "${pdfium_SOURCE_DIR}/lib/libpdfium.so")
  message(FATAL_ERROR "the fetched pdfium archive has no lib/libpdfium.so")
endif()
# src/pdfium.cpp includes the vendored headers for TYPE DEFINITIONS only
# (FPDF_FORMFILLINFO / FPDF_FILEWRITE / FS_RECTF layout); every function is
# still dlsym'd, so nothing links against the archive.
if(NOT EXISTS "${pdfium_SOURCE_DIR}/include/fpdf_formfill.h")
  message(FATAL_ERROR "the fetched pdfium archive has no include/fpdf_formfill.h")
endif()

add_executable(RivetHost
  src/main.cpp
  src/host.cpp
  src/pdfium.cpp
  src/pdfview.cpp
  src/annotations.cpp
  src/panels.cpp
  src/epub.cpp
  src/tts.cpp
  src/printing.cpp
  src/forms.cpp
)

target_include_directories(RivetHost PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${pdfium_SOURCE_DIR}/include"
)

target_include_directories(RivetHost SYSTEM PRIVATE
  ${PDFGIST_WEBKIT_INCLUDES}
)

target_link_directories(RivetHost PRIVATE
  ${PDFGIST_WEBKIT_LIBDIRS}
)

target_compile_definitions(RivetHost PRIVATE
  $<$<BOOL:${PDFGIST_ENABLE_EPUB}>:PDFGIST_HAVE_WEBKIT=1>
)

target_link_libraries(RivetHost PRIVATE
  rivet::platform
  PkgConfig::JSON_GLIB
  ${PDFGIST_WEBKIT_LIBS}
)

target_compile_options(RivetHost PRIVATE -Wall -Wextra -Wpedantic)

# The dlopen'd library must sit beside the executable in every layout. Two
# copies: one beside the build-tree binary (manual `cmake --build` runs), one
# in the rivet stage (`RIVET_STAGE_DIR` when set — `raco rivet build` stages
# there and packages/AppImages bundle that directory wholesale). The stage
# copy runs through cmake/StagePdfium.cmake so a missing stage never fails
# the build (manual layouts stage nothing; RIVET_PDFIUM_PATH covers them).
add_custom_command(TARGET RivetHost POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${pdfium_SOURCE_DIR}/lib/libpdfium.so"
          "$<TARGET_FILE_DIR:RivetHost>/libpdfium.so"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${pdfium_SOURCE_DIR}/LICENSE"
          "$<TARGET_FILE_DIR:RivetHost>/libpdfium-LICENSE.txt"
  COMMAND ${CMAKE_COMMAND}
          -Dsource=${pdfium_SOURCE_DIR}/lib/libpdfium.so
          -Dbinary=$<TARGET_FILE:RivetHost>
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/StagePdfium.cmake
  VERBATIM
)
