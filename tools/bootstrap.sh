#!/usr/bin/env bash
# Bootstrap the build dependencies into third_party/ (gitignored).
#
#   bash tools/bootstrap.sh
#
# Produces:
#   third_party/toolchains/llvm-mingw-<ver>-ucrt-x86_64/   portable clang + lld + libc++ (no install)
#   third_party/toolchains/ninja/ninja.exe
#   third_party/win/zlib, third_party/win/phosg            sources (pinned)
#   third_party/win/local/{include,lib}                    static zlib + phosg
#
# The versions are pinned in tools/versions. Nothing is installed
# system-wide. Run from Git Bash. Set AD_SEED_DIR to a directory that already
# holds llvm-mingw-*.zip / ninja-win.zip to skip downloads.
#
# On Linux (a cross build for Windows; it also needs curl, git, cmake, unzip
# and xz) everything host-specific has a folder of its own, so a checkout
# shared with Windows (WSL, a dual boot, a container's bind mount) keeps both
# hosts' builds, and neither replaces the other's:
#   third_party/toolchains/llvm-mingw-<ver>-<LLVM_MINGW_LINUX_BUILD>/   the release's Linux build
#   third_party/toolchains/ninja-linux/ninja
#   third_party/win/local-linux/{include,lib}   the same static zlib + phosg,
#                                              built there (build-linux-*)
# (AD_SEED_DIR: its llvm-mingw-*.tar.xz / ninja-linux.zip); the zlib and
# phosg sources are shared.
#
# Safe to run again at any time, and it repairs what it finds:
#   - a zip is used only when its sha256 is the pinned one (a download cut
#     short, or any other file of that name, is fetched again); downloads go
#     to <zip>.part and are renamed only once they check out;
#   - a toolchain is unpacked into a temporary folder and renamed into place,
#     then marked complete (.bootstrap-complete, the zip's sha256), so an
#     interrupted unzip is redone rather than trusted; one unpacked before
#     these marks existed is compared with its zip (every file, by size) and
#     marked when it is whole, else unpacked again;
#   - zlib and phosg are rebuilt from scratch whenever their pinned revisions
#     or the toolchain differ from the ones recorded in
#     third_party/win/local/.bootstrap-deps (local-linux/ on Linux).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# NAME=value lines; tolerate a CRLF checkout.
eval "$(tr -d '\r' < "$ROOT/tools/versions" | grep -E '^[A-Z0-9_]+=[^ ]*$')"
die() { echo "bootstrap: $*" >&2; exit 1; }

TP="$ROOT/third_party"
TC="$TP/toolchains"
WD="$TP/win"

# Each host's own archives, unpacked into folders of their own: the
# release's archive and its folder share one name (the archive's top folder).
if [ "$(uname -s)" = "Linux" ]; then
  IS_LINUX=1
  for v in LLVM_MINGW_VER LLVM_MINGW_LINUX_BUILD LLVM_MINGW_LINUX_SHA256 NINJA_VER NINJA_LINUX_SHA256 ZLIB_REV PHOSG_REV; do
    [ -n "${!v:-}" ] || die "$v is not set in tools/versions"
  done
  for c in curl git cmake unzip tar xz sha256sum; do
    command -v "$c" >/dev/null 2>&1 ||
      die "$c is needed (Debian, Ubuntu: sudo apt install curl git cmake unzip xz-utils)"
  done
  LLVM_BUILD="$LLVM_MINGW_LINUX_BUILD"
  LLVM_ARCHIVE_EXT=".tar.xz"
  LLVM_ARCHIVE_SHA="$LLVM_MINGW_LINUX_SHA256"
  NINJA_ARCHIVE="ninja-linux.zip"
  NINJA_ARCHIVE_SHA="$NINJA_LINUX_SHA256"
  NINJA_DIR="$TC/ninja-linux"
  EXE_SUFFIX=""
else
  IS_LINUX=0
  for v in LLVM_MINGW_VER LLVM_MINGW_SHA256 NINJA_VER NINJA_SHA256 ZLIB_REV PHOSG_REV; do
    [ -n "${!v:-}" ] || die "$v is not set in tools/versions"
  done
  LLVM_BUILD="ucrt-x86_64"
  LLVM_ARCHIVE_EXT=".zip"
  LLVM_ARCHIVE_SHA="$LLVM_MINGW_SHA256"
  NINJA_ARCHIVE="ninja-win.zip"
  NINJA_ARCHIVE_SHA="$NINJA_SHA256"
  NINJA_DIR="$TC/ninja"
  EXE_SUFFIX=".exe"
fi
mkdir -p "$TC" "$WD"

LLVM_NAME="llvm-mingw-$LLVM_MINGW_VER-$LLVM_BUILD"
LLVM_ARCHIVE="$LLVM_NAME$LLVM_ARCHIVE_EXT"
LLVM_DIR="$TC/$LLVM_NAME"
sha256_of() { sha256sum "$1" | cut -d' ' -f1; }

fetch() {  # fetch <name> <url> <dest-dir> <sha256>: <dest-dir>/<name>, verified
  local name="$1" url="$2" dest="$3" want="$4"
  local f="$dest/$name"
  if [ -f "$f" ]; then
    [ "$(sha256_of "$f")" = "$want" ] && return 0
    echo "bootstrap: $name is not the pinned file (a cut-off download?); fetching it again" >&2
    rm -f "$f"
  fi
  rm -f "$f.part"
  if [ -n "${AD_SEED_DIR:-}" ] && [ -f "$AD_SEED_DIR/$name" ]; then
    if [ "$(sha256_of "$AD_SEED_DIR/$name")" = "$want" ]; then
      cp "$AD_SEED_DIR/$name" "$f.part"
      mv "$f.part" "$f"
      return 0
    fi
    echo "bootstrap: $AD_SEED_DIR/$name is not the pinned file; downloading instead" >&2
  fi
  curl -fL --retry 3 -o "$f.part" "$url" || { rm -f "$f.part"; die "cannot download $url"; }
  local got
  got="$(sha256_of "$f.part")"
  if [ "$got" != "$want" ]; then
    rm -f "$f.part"
    die "$url has sha256 $got, not the pinned $want (tools/versions)"
  fi
  mv "$f.part" "$f"
}

complete() { [ "$(cat "$1/.bootstrap-complete" 2>/dev/null)" = "$2" ]; }

# "<size> <path>" of every file in <zip> under <folder inside it>/, and of
# every file in <dir> (the mark aside), each sorted: <dir> holds all the zip
# unpacks to when every line of the first is in the second (files a tool
# made there since, such as Python's __pycache__, do not count).
zip_listing() {  # zip_listing <zip> [<folder inside the zip>]
  unzip -Z -l "$1" | awk -v p="${2:+$2/}" '
    NF >= 10 && $1 !~ /^d/ {
      name = $10
      for (i = 11; i <= NF; i++) name = name " " $i
      if (p == "" || index(name, p) == 1) print $4 " " substr(name, length(p) + 1)
    }' | LC_ALL=C sort
}
dir_listing() { (cd "$1" && find . -type f ! -name .bootstrap-complete -printf '%s %P\n') | LC_ALL=C sort; }

# A toolchain unpacked by an older bootstrap (no mark): whole, it is marked
# rather than unpacked again.
adopt_if_whole() {  # adopt_if_whole <zip> <sha256> <dir> [<folder inside the zip>]
  local zip="$1" sha="$2" dir="$3" inner="${4:-}"
  [ -d "$dir" ] && [ ! -e "$dir/.bootstrap-complete" ] && [ -f "$zip" ] || return 1
  [ "$(sha256_of "$zip")" = "$sha" ] || return 1
  if [[ "$zip" == *.zip ]]; then
    if [ -z "$(LC_ALL=C comm -23 <(zip_listing "$zip" "$inner") <(dir_listing "$dir") | head -1)" ]; then
      echo "$sha" > "$dir/.bootstrap-complete"
      echo "bootstrap: $(basename "$dir") is complete; marked it"
      return 0
    fi
  fi
  echo "bootstrap: $(basename "$dir") is incomplete; unpacking it again" >&2
  return 1
}

install_unpacked() {  # install_unpacked <archive> <sha256> <final dir> [<folder inside the archive>]
  local archive="$1" sha="$2" final="$3" inner="${4:-}"
  local tmp="$final.unpack-$$" old="$final.old-$$"
  rm -rf "$tmp"
  mkdir -p "$tmp"
  if [[ "$archive" == *.tar.xz ]]; then
    tar -xf "$archive" -C "$tmp" || { rm -rf "$tmp"; die "cannot unpack $archive"; }
  else
    unzip -q "$archive" -d "$tmp" || { rm -rf "$tmp"; die "cannot unpack $archive"; }
  fi
  local src="$tmp${inner:+/$inner}"
  [ -d "$src" ] || { rm -rf "$tmp"; die "$archive has no $inner folder"; }
  echo "$sha" > "$src/.bootstrap-complete"
  if [ -e "$final" ]; then
    mv "$final" "$old" || { rm -rf "$tmp"; die "cannot replace $final (is a build using it?)"; }
  fi
  mv "$src" "$final"
  rm -rf "$tmp" "$old"
}

if ! complete "$LLVM_DIR" "$LLVM_ARCHIVE_SHA" &&
   ! adopt_if_whole "$TC/$LLVM_ARCHIVE" "$LLVM_ARCHIVE_SHA" "$LLVM_DIR" "$LLVM_NAME"; then
  fetch "$LLVM_ARCHIVE" "https://github.com/mstorsjo/llvm-mingw/releases/download/$LLVM_MINGW_VER/$LLVM_ARCHIVE" "$TC" \
    "$LLVM_ARCHIVE_SHA"
  echo "bootstrap: unpacking $LLVM_ARCHIVE"
  install_unpacked "$TC/$LLVM_ARCHIVE" "$LLVM_ARCHIVE_SHA" "$LLVM_DIR" "$LLVM_NAME"
fi

if ! complete "$NINJA_DIR" "$NINJA_ARCHIVE_SHA" &&
   ! adopt_if_whole "$TC/$NINJA_ARCHIVE" "$NINJA_ARCHIVE_SHA" "$NINJA_DIR"; then
  # shellcheck disable=SC2153  # NINJA_VER is tools/versions' (the eval above)
  fetch "$NINJA_ARCHIVE" "https://github.com/ninja-build/ninja/releases/download/$NINJA_VER/$NINJA_ARCHIVE" "$TC" \
    "$NINJA_ARCHIVE_SHA"
  install_unpacked "$TC/$NINJA_ARCHIVE" "$NINJA_ARCHIVE_SHA" "$NINJA_DIR"
fi
# unzip keeps the mode the zip records; make sure of it.
if [ "$IS_LINUX" -eq 1 ]; then chmod +x "$NINJA_DIR/ninja"; fi

[ -x "$LLVM_DIR/bin/clang++$EXE_SUFFIX" ] || die "$LLVM_DIR/bin/clang++$EXE_SUFFIX is missing after unpacking"
[ -x "$NINJA_DIR/ninja$EXE_SUFFIX" ] || die "$NINJA_DIR/ninja$EXE_SUFFIX is missing after unpacking"
for other in "$TC"/llvm-mingw-*-"$LLVM_BUILD"; do
  if [ -d "$other" ] && [ "$other" != "$LLVM_DIR" ]; then
    echo "bootstrap: note: $(basename "$other") is no longer used (the build takes $LLVM_NAME); it can be deleted"
  fi
done

# zlib and phosg are built for Windows on either host, but a Linux host
# installs them (and builds them) in folders of its own: their CMake files
# record the host's absolute paths, so neither host may use, or replace, the
# other's. cmake/llvm-mingw.cmake looks in local-linux first on Linux.
if [ "$IS_LINUX" -eq 1 ]; then
  export PATH="$LLVM_DIR/bin:$NINJA_DIR:$PATH"
  LOCAL="$WD/local-linux"
  LOCAL_M="$LOCAL"
  DEPS_BUILD="$WD/build-linux"
else
  export PATH="$LLVM_DIR/bin:$NINJA_DIR:/c/Program Files/CMake/bin:$PATH"
  LOCAL="$WD/local"
  LOCAL_M="$(cygpath -m "$LOCAL")"
  DEPS_BUILD="$WD/build"
fi
DEPS_STAMP="$LOCAL/.bootstrap-deps"
DEPS_WANT="llvm-mingw=$LLVM_MINGW_VER zlib=$ZLIB_REV phosg=$PHOSG_REV"

clone_at() {  # clone_at <url> <dir> <rev>
  local url="$1" dir="$2" rev="$3"
  if [ ! -d "$dir/.git" ]; then rm -rf "$dir"; git clone -q "$url" "$dir"; fi
  git -C "$dir" fetch -q origin "$rev" 2>/dev/null || git -C "$dir" fetch -q --unshallow 2>/dev/null || true
  git -C "$dir" checkout -q --force "$rev"
  [ "$(git -C "$dir" rev-parse HEAD)" = "$rev" ] || die "$dir is not at $rev"
}

if [ "$(cat "$DEPS_STAMP" 2>/dev/null)" != "$DEPS_WANT" ] || [ ! -f "$LOCAL/lib/libz.a" ] ||
   [ ! -f "$LOCAL/lib/libphosg.a" ]; then
  echo "bootstrap: building zlib and phosg ($DEPS_WANT)"
  # From scratch: no stale objects, headers or libraries of other revisions.
  rm -rf "$DEPS_BUILD-zlib" "$DEPS_BUILD-phosg" "$LOCAL"

  clone_at https://github.com/madler/zlib "$WD/zlib" "$ZLIB_REV"
  cmake -S "$WD/zlib" -B "$DEPS_BUILD-zlib" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_SYSTEM_NAME=Windows \
    -DCMAKE_C_COMPILER="$LLVM_DIR/bin/x86_64-w64-mingw32-clang$EXE_SUFFIX" \
    -DCMAKE_RC_COMPILER="$LLVM_DIR/bin/x86_64-w64-mingw32-windres$EXE_SUFFIX" \
    -DCMAKE_INSTALL_PREFIX="$LOCAL_M" \
    -DZLIB_BUILD_TESTING=OFF -DZLIB_BUILD_SHARED=OFF >/dev/null
  cmake --build "$DEPS_BUILD-zlib" >/dev/null
  cmake --install "$DEPS_BUILD-zlib" >/dev/null
  cp "$LOCAL/lib/libzs.a" "$LOCAL/lib/libz.a"

  clone_at https://github.com/fuzziqersoftware/phosg "$WD/phosg" "$PHOSG_REV"
  cmake -S "$WD/phosg" -B "$DEPS_BUILD-phosg" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_SYSTEM_NAME=Windows \
    -DCMAKE_C_COMPILER="$LLVM_DIR/bin/x86_64-w64-mingw32-clang$EXE_SUFFIX" \
    -DCMAKE_CXX_COMPILER="$LLVM_DIR/bin/x86_64-w64-mingw32-clang++$EXE_SUFFIX" \
    -DCMAKE_RC_COMPILER="$LLVM_DIR/bin/x86_64-w64-mingw32-windres$EXE_SUFFIX" \
    -DCMAKE_INSTALL_PREFIX="$LOCAL_M" -DCMAKE_PREFIX_PATH="$LOCAL_M" >/dev/null
  # Build everything: phosg's install rules also copy its small CLI tools.
  cmake --build "$DEPS_BUILD-phosg" >/dev/null
  cmake --install "$DEPS_BUILD-phosg" >/dev/null

  echo "$DEPS_WANT" > "$DEPS_STAMP"
fi

echo "toolchain: $LLVM_DIR"
echo "deps:      $LOCAL ($DEPS_WANT)"
"$LLVM_DIR/bin/clang++$EXE_SUFFIX" --version | head -1
