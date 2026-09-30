#!/usr/bin/env bash
# Build the Linux player, longafterdark (scr/linux), with the system g++:
#   bash tools/build-player.sh [<output>]     (default build/linux/longafterdark)
#   bash tools/build-player.sh --tests [<output>]
#                             its unit tests, built and run (default
#                             build/linux/longafterdark-unit)
# The player is every scr/linux/*.cc (not tests/) and importer/minijson.cc,
# C++20, linked against X11, Xext, Xrandr and pthread and nothing else, with
# the flags and warnings below (not the environment's CXXFLAGS). libstdc++
# and libgcc are linked statically, so the program needs only the C library
# (glibc, the version it was built against or newer) and the X libraries of
# the machine it runs on: the release build is made on Ubuntu 22.04, so it
# runs on glibc 2.35 and newer. The unit tests are scr/linux/tests/unit.cc
# with the same sources but main.cc, built the same way; they need no X
# display and no Wine.
# Its version is adw_version.h's (ADW_VERSION_STRING), made beside the
# output from cmake/adw_version.h.in the way CMakeLists.txt makes it: the
# project() version and LICENSE's copyright line.
# Env: AD_CXX (default g++); AD_WERROR=1 makes warnings errors; AD_GLIBC_MAX
# (e.g. 2.35) fails the build when the player needs a newer glibc symbol, or
# needs libstdc++ or libgcc_s at all.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TESTS=0
if [ "${1:-}" = "--tests" ]; then TESTS=1; shift; fi
if [ "$TESTS" -eq 1 ]; then
  OUT="${1:-$ROOT/build/linux/longafterdark-unit}"
else
  OUT="${1:-$ROOT/build/linux/longafterdark}"
fi
CXX="${AD_CXX:-g++}"
die() { echo "build-player.sh: $*" >&2; exit 1; }

[ "$(uname -s)" = "Linux" ] || die "the player is a Linux program: build it on Linux"
command -v "$CXX" >/dev/null 2>&1 ||
  die "$CXX is needed to build the Linux player (Debian, Ubuntu: sudo apt install g++ libx11-dev libxext-dev libxrandr-dev)"

mkdir -p "$(dirname "$OUT")"
OUT="$(cd "$(dirname "$OUT")" && pwd)/$(basename "$OUT")"
GEN="$(dirname "$OUT")/generated"
mkdir -p "$GEN"

# adw_version.h, as configure_file(... @ONLY) makes it from the template.
read -r MAJOR MINOR PATCH < <(tr -d '\r' < "$ROOT/CMakeLists.txt" |
  sed -nE 's/^project\([^ ]+ VERSION ([0-9]+)\.([0-9]+)\.([0-9]+)[ )].*/\1 \2 \3/p') ||
  die "no project(... VERSION x.y.z ...) in CMakeLists.txt"
COPYRIGHT="$(tr -d '\r' < "$ROOT/LICENSE" | sed -n 's/^\(Copyright .*\)/\1/p')"
COPYRIGHT="${COPYRIGHT%%$'\n'*}"
[ -n "$COPYRIGHT" ] || die "no Copyright line in LICENSE"
COPYRIGHT="$(printf '%s' "$COPYRIGHT" | tr '"' "'")"   # as CMakeLists.txt does
esc() { local s="${1//\\/\\\\}"; s="${s//&/\\&}"; printf '%s' "${s//|/\\|}"; }
sed -e "s|@PROJECT_VERSION_MAJOR@|$MAJOR|g" -e "s|@PROJECT_VERSION_MINOR@|$MINOR|g" \
    -e "s|@PROJECT_VERSION_PATCH@|$PATCH|g" -e "s|@AD_COPYRIGHT@|$(esc "$COPYRIGHT")|g" \
    "$ROOT/cmake/adw_version.h.in" | tr -d '\r' > "$GEN/adw_version.h.tmp"
if grep -q '@[A-Za-z_][A-Za-z0-9_]*@' "$GEN/adw_version.h.tmp"; then
  die "cmake/adw_version.h.in has a variable this script does not fill in: $(grep -o '@[A-Za-z_][A-Za-z0-9_]*@' "$GEN/adw_version.h.tmp" | head -1)"
fi
mv "$GEN/adw_version.h.tmp" "$GEN/adw_version.h"

shopt -s nullglob
SRCS=()
for f in "$ROOT"/scr/linux/*.cc; do
  if [ "$TESTS" -eq 1 ] && [ "$(basename "$f")" = main.cc ]; then continue; fi
  SRCS+=("$f")
done
shopt -u nullglob
[ "${#SRCS[@]}" -gt 0 ] || die "no scr/linux/*.cc"
SRCS+=("$ROOT/importer/minijson.cc")
if [ "$TESTS" -eq 1 ]; then
  [ -f "$ROOT/scr/linux/tests/unit.cc" ] || die "no scr/linux/tests/unit.cc"
  SRCS=("$ROOT/scr/linux/tests/unit.cc" "${SRCS[@]}")
fi

FLAGS=(-std=c++20 -O2 -Wall -Wextra)
if [ -n "${AD_WERROR:-}" ]; then FLAGS+=(-Werror); fi
echo "build-player.sh: $($CXX --version | head -1)"
echo "build-player.sh: building $OUT (version $MAJOR.$MINOR.$PATCH)"
"$CXX" "${FLAGS[@]}" -I"$ROOT/importer" -I"$GEN" "${SRCS[@]}" \
  -static-libstdc++ -static-libgcc -lX11 -lXext -lXrandr -lpthread -o "$OUT.tmp" ||
  { rm -f "$OUT.tmp"
    die "the compile failed (its headers: sudo apt install libx11-dev libxext-dev libxrandr-dev)"; }
chmod 755 "$OUT.tmp"

if [ "$TESTS" -eq 1 ]; then
  mv "$OUT.tmp" "$OUT"
  echo "build-player.sh: running $OUT"
  "$OUT" || die "the player's unit tests failed"
  exit 0
fi

# What the program needs of the machine it runs on (readelf and objdump are
# binutils', which g++ depends on). A program that fails the check is removed.
reject() { rm -f "$OUT.tmp" "$OUT"; die "$@"; }
DYN="$(readelf -d "$OUT.tmp")"
echo "build-player.sh: needs $(sed -nE 's/.*\(NEEDED\).*\[(.*)\]/\1/p' <<< "$DYN" | tr '\n' ' ')"
if [ -n "${AD_GLIBC_MAX:-}" ]; then
  SYMS="$(objdump -T "$OUT.tmp")"
  NEWEST="$(grep -oE 'GLIBC_[0-9]+(\.[0-9]+)+' <<< "$SYMS" | sed 's/^GLIBC_//' | sort -uV | tail -n 1)"
  [ -n "$NEWEST" ] || reject "found no GLIBC_ symbol version in the player"
  if [ "$(printf '%s\n%s\n' "$NEWEST" "$AD_GLIBC_MAX" | sort -V | tail -n 1)" != "$AD_GLIBC_MAX" ]; then
    grep -F "GLIBC_$NEWEST" <<< "$SYMS" >&2 || true
    reject "the player needs glibc $NEWEST, newer than AD_GLIBC_MAX=$AD_GLIBC_MAX"
  fi
  if grep -qE 'GLIBCXX_|CXXABI_|GCC_[0-9]' <<< "$SYMS" || grep -qE '\[(libstdc\+\+|libgcc_s)\.so' <<< "$DYN"; then
    reject "the player needs libstdc++ or libgcc_s: they should be linked statically"
  fi
  echo "build-player.sh: needs glibc $NEWEST at most (AD_GLIBC_MAX=$AD_GLIBC_MAX)"
fi
mv "$OUT.tmp" "$OUT"
echo "build-player.sh: $OUT"
