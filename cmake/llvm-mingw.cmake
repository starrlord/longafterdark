# Toolchain file: the portable llvm-mingw that tools/bootstrap.sh
# unpacks into third_party/toolchains. On Windows a native x86_64 build (host
# == target); on Linux a cross build for Windows x86_64 with the release's
# Linux build of the toolchain, in a folder of its own
# (llvm-mingw-<ver>-<LLVM_MINGW_LINUX_BUILD>, beside the Windows one), and
# Wine running the build's own Windows programs and the tests.
#
# Exactly the pinned version (LLVM_MINGW_VER in tools/versions), never
# whichever llvm-mingw-* folder happens to be there: after a bump, with the
# old folder still present, the build must not quietly keep the old compiler
# (nor a build directory the one it was configured with: AD_LLVM_MINGW is
# computed on every run, never taken from the cache). -DAD_LLVM_MINGW_DIR=<dir>
# overrides it.
get_filename_component(_AD_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
if(CMAKE_HOST_WIN32)
  set(_EXE ".exe")
else()
  set(_EXE "")
endif()

if(AD_LLVM_MINGW_DIR)
  set(AD_LLVM_MINGW "${AD_LLVM_MINGW_DIR}")
else()
  # <name>=<value> from tools/versions (its first such line).
  function(_ad_pinned name out)
    file(STRINGS "${_AD_ROOT}/tools/versions" _line REGEX "^${name}=" LIMIT_COUNT 1)
    string(REGEX REPLACE "^${name}=([^ \t\r#]*).*$" "\\1" _value "${_line}")
    if(NOT _value)
      message(FATAL_ERROR "no ${name} in ${_AD_ROOT}/tools/versions")
    endif()
    set(${out} "${_value}" PARENT_SCOPE)
  endfunction()
  _ad_pinned(LLVM_MINGW_VER _AD_LLVM_VER)
  if(CMAKE_HOST_WIN32)
    set(_AD_LLVM_BUILD "ucrt-x86_64")
  else()
    _ad_pinned(LLVM_MINGW_LINUX_BUILD _AD_LLVM_BUILD)
  endif()
  set(AD_LLVM_MINGW "${_AD_ROOT}/third_party/toolchains/llvm-mingw-${_AD_LLVM_VER}-${_AD_LLVM_BUILD}")
  if(NOT EXISTS "${AD_LLVM_MINGW}/bin/x86_64-w64-mingw32-clang++${_EXE}")
    message(FATAL_ERROR "llvm-mingw ${_AD_LLVM_VER} (tools/versions) is not installed at "
                        "${AD_LLVM_MINGW}; run: bash tools/bootstrap.sh")
  endif()
endif()

if(NOT CMAKE_HOST_WIN32)
  set(CMAKE_SYSTEM_NAME Windows)
  set(CMAKE_SYSTEM_PROCESSOR x86_64)
  # The build runs two of its own Windows programs (the icon generators
  # scr_gen_icon and adimport_gen_icon), and ctest runs every test program:
  # on Linux, all of them through Wine. Without it the build would only fail
  # later, at the first icon, so it stops here.
  find_program(WINE_EXECUTABLE NAMES wine wine64 PATHS /usr/lib/wine)
  if(NOT WINE_EXECUTABLE)
    message(FATAL_ERROR "Building on Linux needs Wine: the build runs its own Windows programs "
                        "(the icon generators) and the tests under it. Install 64-bit Wine "
                        "(Debian, Ubuntu: sudo apt install wine wine64), or name one with "
                        "-DWINE_EXECUTABLE=<path>.")
  endif()
  set(CMAKE_CROSSCOMPILING_EMULATOR "${WINE_EXECUTABLE}")
  # zlib and phosg as bootstrap.sh builds them on Linux: in a folder of their
  # own (their CMake files record the host's absolute paths), found before
  # the Windows host's third_party/win/local that CMakeLists.txt adds.
  set(_AD_DEPS "${_AD_ROOT}/third_party/win/local-linux")
  if(NOT EXISTS "${_AD_DEPS}/.bootstrap-deps")
    message(FATAL_ERROR "zlib and phosg are not built at ${_AD_DEPS}; run: bash tools/bootstrap.sh")
  endif()
  if(NOT _AD_DEPS IN_LIST CMAKE_PREFIX_PATH)
    list(PREPEND CMAKE_PREFIX_PATH "${_AD_DEPS}")
  endif()
endif()

set(CMAKE_C_COMPILER   "${AD_LLVM_MINGW}/bin/x86_64-w64-mingw32-clang${_EXE}")
set(CMAKE_CXX_COMPILER "${AD_LLVM_MINGW}/bin/x86_64-w64-mingw32-clang++${_EXE}")
set(CMAKE_RC_COMPILER  "${AD_LLVM_MINGW}/bin/x86_64-w64-mingw32-windres${_EXE}")
# 32-bit compiler, used only by test oracles that execute x86 natively under
# WOW64. A plain variable, like the compilers: it follows the pinned version
# (a cache entry would keep an older one in an existing build directory).
set(AD_I686_CXX "${AD_LLVM_MINGW}/bin/i686-w64-mingw32-clang++${_EXE}")
