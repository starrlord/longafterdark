# Long After Dark: the x86 host for the original Windows After Dark modules,
# and the LongAfterDark.scr front-end. See docs/BUILDING.md and docs/DESIGN.md.
#
#   bash tools/bootstrap.sh     # once: toolchain + zlib + phosg into third_party/
#   bash tools/build.sh         # configure + build + ctest
cmake_minimum_required(VERSION 3.24)
# The version of every shipped program (adw_version.h below): set here only.
project(AfterDarkWindows VERSION 1.4.4 LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  # Release is load-bearing for an interpreter: unoptimized, the emulator is
  # far too slow to run the modules at their pace.
  set(CMAKE_BUILD_TYPE Release)
endif()

set(AD_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(AD_TP_LOCAL "${AD_REPO_ROOT}/third_party/win/local")
list(APPEND CMAKE_PREFIX_PATH "${AD_TP_LOCAL}")
find_package(phosg CONFIG REQUIRED)

add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
add_compile_options(-Wall -Wno-unused-function -Wno-invalid-constexpr)
# Self-contained binaries (no libc++.dll / libunwind.dll beside them), and the
# 8 MB stack resource_dasm-derived code needs (its tests segfault on 1 MB).
# No link timestamp in the PE header and debug directory: two builds of the
# same source are the same bytes (reproducible), not four bytes apart.
add_link_options(-static -Wl,--stack,8388608 -Wl,--no-insert-timestamp)

# adw_version.h: the version and copyright every program carries (its
# VERSIONINFO, --version). The copyright line is LICENSE's. A target that
# includes it adds ${AD_GENERATED_DIR} to its include directories. Editing
# LICENSE re-runs the configure, so the programs never carry a stale line.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${AD_REPO_ROOT}/LICENSE")
file(STRINGS "${AD_REPO_ROOT}/LICENSE" _ad_copyright REGEX "^Copyright " LIMIT_COUNT 1)
if(NOT _ad_copyright)
  message(FATAL_ERROR "no Copyright line in ${AD_REPO_ROOT}/LICENSE")
endif()
string(REPLACE "\"" "'" AD_COPYRIGHT "${_ad_copyright}")
set(AD_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/adw_version.h.in" "${AD_GENERATED_DIR}/adw_version.h" @ONLY)

enable_testing()

# Each component owns its own CMakeLists.txt; absent ones are skipped so the
# tree builds at every stage of bring-up. AD_COMPONENTS restricts the build to
# a subset (e.g. -DAD_COMPONENTS="host/cpu;host/loader") so components can be
# built and tested in isolation.
set(AD_COMPONENTS "" CACHE STRING "Subset of component dirs to build (empty = all present)")
foreach(_sub
    host/cpu        # adw_cpu      — x86 interpreter (flat 32-bit + 16-bit segmented)
    host/loader     # adw_loader   — PE32 / NE image parsing and loading
    common/ui       # adw_ui       — the Windows 11 look the .scr and adimport share (docs/COVERS.md §3)
    host/core       # adw_core     — screen surface, frame protocol, host main (adhostwin)
    host/win32      # adw_win32    — Win32 guest runtime + API shims (KERNEL32/USER32/GDI32/WINMM…)
    host/win16      # adw_win16    — Win16 guest runtime + API shims (KERNEL/USER/GDI/MMSYSTEM…)
    host/pe32       # adw_lane_pe32 — the AD4 lane: ADXPL510 + module, AD_MODULE32 driver
    host/ne16       # adw_lane_ne16 — the Classic lane: OLDMOD16 + ADXPL300 + module
    importer        # adw_import   — ISO-9660/Joliet reader, asset import (adimport.exe)
    scr             # LongAfterDark.scr — the screen saver front-end
    )
  if(AD_COMPONENTS AND NOT _sub IN_LIST AD_COMPONENTS)
    continue()
  endif()
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_sub}/CMakeLists.txt")
    add_subdirectory(${_sub})
  endif()
endforeach()

# adhostwin.exe's version information and manifest (cmake/adhostwin.rc), as
# LongAfterDark.scr and adimport.exe carry theirs; attached here, so the
# host/core component's own build is left as it is.
if(TARGET adhostwin)
  set(_ad_host_rc "${CMAKE_CURRENT_SOURCE_DIR}/cmake/adhostwin.rc")
  target_sources(adhostwin PRIVATE "${_ad_host_rc}")
  target_include_directories(adhostwin PRIVATE "${AD_GENERATED_DIR}")
  set_source_files_properties("${_ad_host_rc}" TARGET_DIRECTORY adhostwin PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/adhostwin.manifest;${AD_GENERATED_DIR}/adw_version.h")
endif()
