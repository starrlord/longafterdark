#!/usr/bin/env bash
# Configure, build, and test the tree with the bootstrapped llvm-mingw.
#   bash tools/build.sh [extra cmake --build args]
# Env: AD_BUILD_DIR (default build/win), AD_NO_TESTS=1 to skip ctest,
#      AD_COMPONENTS="host/cpu;host/loader" to build a subset,
#      AD_CTEST_ARGS extra ctest args (e.g. "-R cpu"): split at spaces, tabs
#      and line breaks and passed as they are, never read as shell syntax or
#      globbed, so a regex needs no quotes ("-R cpu|win16", "-E ^ui\.").
# On Linux it cross-compiles for Windows (cmake/llvm-mingw.cmake), and Wine
# runs the build's own Windows programs and the tests.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${AD_BUILD_DIR:-$ROOT/build/win}"
TC="$ROOT/third_party/toolchains"
if [ "$(uname -s)" = "Linux" ]; then
  export PATH="$TC/ninja-linux:$PATH"
else
  export PATH="$TC/ninja:/c/Program Files/CMake/bin:$PATH"
fi

cmake -S "$ROOT" -B "$BUILD" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/llvm-mingw.cmake" \
  -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}" \
  -DAD_COMPONENTS="${AD_COMPONENTS:-}" >/dev/null
cmake --build "$BUILD" "$@"
if [ -z "${AD_NO_TESTS:-}" ]; then
  # All of it, every line (LF or CRLF): -d '' reads up to a NUL, which no
  # value holds, so read takes the whole value and returns 1 at its end.
  IFS=$' \t\r\n' read -r -d '' -a ctest_args <<< "${AD_CTEST_ARGS:-}" || true
  (cd "$BUILD" && ctest --output-on-failure ${ctest_args[@]+"${ctest_args[@]}"})
fi
