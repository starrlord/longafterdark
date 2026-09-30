#!/usr/bin/env bash
# Build the three programs (Release) and stage Long After Dark, ready to run:
#   build/dist/LongAfterDark/{LongAfterDark.scr, adhostwin.exe, adimport.exe,
#                             README.txt, LICENSE.txt, licenses/}
#   bash tools/package.sh
# Env: AD_BUILD_DIR (default build/win-release), AD_DIST_DIR (default
# build/dist/LongAfterDark; replaced whole on every run), AD_COMPONENTS.
# Only what ships is built (no test programs, no tests run): build.sh builds
# and tests the whole tree. The binaries carry no link timestamp, so the same
# source gives the same bytes. The new folder is staged beside the old one
# and swapped in only when complete; if the old one is in use (a running
# screen saver or settings window), nothing is replaced.
# No file of any of the releases is ever staged: the user imports their own
# copies with adimport.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${AD_BUILD_DIR:-$ROOT/build/win-release}"
DIST="${AD_DIST_DIR:-$ROOT/build/dist/LongAfterDark}"
# Both lanes: pe32 (the 32-bit modules) and ne16 (the 16-bit ones).
COMPONENTS="${AD_COMPONENTS:-host/core;host/cpu;host/loader;common/ui;host/win32;host/pe32;host/win16;host/ne16;importer;scr}"
eval "$(tr -d '\r' < "$ROOT/tools/versions" | grep -E '^[A-Z0-9_]+=[^ ]*$')"
LLVM_DIR="$ROOT/third_party/toolchains/llvm-mingw-$LLVM_MINGW_VER-ucrt-x86_64"

AD_BUILD_DIR="$BUILD" AD_COMPONENTS="$COMPONENTS" AD_NO_TESTS=1 \
  bash "$ROOT/tools/build.sh" --target adhostwin adimport LongAfterDark

# The saver: the build above makes LongAfterDark.scr (target LongAfterDark).
SCR="$BUILD/scr/LongAfterDark.scr"
for f in "$SCR" "$BUILD/host/core/adhostwin.exe" "$BUILD/importer/adimport.exe"; do
  [ -f "$f" ] || { echo "package.sh: missing $f" >&2; exit 1; }
done

FINAL="$DIST"
DIST="$FINAL.staging-$$"
rm -rf "$DIST"
trap 'rm -rf "$DIST"' EXIT
mkdir -p "$DIST/licenses"
cp "$SCR" "$DIST/LongAfterDark.scr"
cp "$BUILD/host/core/adhostwin.exe" "$BUILD/importer/adimport.exe" "$DIST/"

# Windows line endings for Notepad on older systems: every text file staged.
crlf() { sed -i 's/\r*$/\r/' "$@"; }

# The licences: this project's (LICENSE.txt), and in licenses\ the full text
# of everything built into the programs, with NOTICE.txt saying which file is
# whose (THIRD_PARTY_LICENSES.md is the repository's account of the same).
cp "$ROOT/LICENSE" "$DIST/LICENSE.txt"
L="$DIST/licenses"
cp "$ROOT/host/cpu/LICENSE.resource_dasm" "$L/resource_dasm.LICENSE.txt"
cp "$ROOT/third_party/win/phosg/src/LICENSE" "$L/phosg.LICENSE.txt"
cp "$ROOT/third_party/win/zlib/LICENSE" "$L/zlib.LICENSE.txt"
cp "$LLVM_DIR/LICENSE.TXT" "$L/LLVM.LICENSE.txt"
cp "$LLVM_DIR/x86_64-w64-mingw32/share/mingw32/COPYING.MinGW-w64-runtime.txt" "$L/mingw-w64-runtime.COPYING.txt"
cat > "$L/NOTICE.txt" <<EOF
Long After Dark: third-party software
=====================================

The three programs (LongAfterDark.scr, adhostwin.exe, adimport.exe) are
linked statically, so each carries inside it the parts of the code below
that it uses. The full licence texts are in this folder (UNARJ's terms,
which have no file of their own, are quoted below).

  resource_dasm.LICENSE.txt       MIT
    The x86 emulator in adhostwin.exe is derived from resource_dasm
    (https://github.com/fuzziqersoftware/resource_dasm, through the fork
    https://github.com/swannman/resource_dasm, branch afterdark-perf,
    commit 02d8ea9a58eaf559a9194601b33d98723c9d4f60), modified.

  phosg.LICENSE.txt               MIT
    phosg (https://github.com/fuzziqersoftware/phosg), commit
    $PHOSG_REV,
    in adhostwin.exe and LongAfterDark.scr.

  zlib.LICENSE.txt                zlib
    zlib (https://zlib.net/), commit
    $ZLIB_REV,
    in adimport.exe (PKZIP archives: the After Dark 3.x installers' own
    and the ZIPs releases come in; and the CRC-32 of Star Wars Screen
    Entertainment's archives).

  (no file: the terms are here)   UNARJ's terms
    The ARJ decoder in adimport.exe, which reads Star Wars Screen
    Entertainment's archives, is a modified version of the decoder of
    UNARJ by Robert K. Jung (ARJ Software), DECODE.C with UNARJ.C's bit
    reader: ported to C++, with bounds checks added. UNARJ's LZH routines
    follow Haruhiko Okumura's AR (ar002). adimport.exe only extracts ARJ
    archives; it cannot make one. DECODE.C's header says (UNARJ.C's says
    the same, Copyright (c) 1991-93, without the request for a copy):
      Copyright (c) 1991 by Robert K Jung.  All rights reserved.
      This code may be freely used in programs that are NOT ARJ
      archivers (both compress and extract ARJ archives).
      If you wish to distribute a modified version of this program, you
      MUST indicate that it is a modified version both in the program
      and source code.
      If you modify this program, I would appreciate a copy of the new
      source code.  I am holding the copyright on the source code, so
      please do not delete my name from the program files or from the
      documentation.

  LLVM.LICENSE.txt                Apache-2.0 WITH LLVM-exception
    The LLVM runtimes (libc++, libc++abi, libunwind, compiler-rt) of
    llvm-mingw $LLVM_MINGW_VER (https://github.com/mstorsjo/llvm-mingw), in all
    three programs.

  mingw-w64-runtime.COPYING.txt   the mingw-w64 runtime licence
    The mingw-w64 runtime (start-up code and import libraries,
    https://www.mingw-w64.org/), as built by llvm-mingw $LLVM_MINGW_VER,
    in all three programs.

Apart from the code above and this project's own, everything the programs
use comes with Windows. No file of any of the releases the programs run
(After Dark and the others) is included.
EOF

cat > "$DIST/README.txt" <<'EOF'
Long After Dark
===============

Long After Dark is a screen saver for Windows that runs the original modules
of After Dark and of LucasArts' Star Wars Screen Entertainment, unchanged,
under x86 emulation. It knows twelve releases, 284 modules:

  id        Release                                    Internet Archive download
  deluxe    After Dark 4.0 Deluxe (1996)               CD image, 381.7 MB
  ad10      After Dark 10th Anniversary (1999)         CD image, 143.3 MB
  ad32      After Dark 3.2 (1995)                      CD image, 58.8 MB
  tt        Totally Twisted After Dark (1995)          CD image, 37.9 MB
  simpsons  The Simpsons Screen Saver (1994)           install files (ZIP), 2.6 MB
  swse      Star Wars Screen Entertainment (1994)      CD image, 6.9 MB
  startrek  Star Trek: The Screen Saver (1992)         two floppy images, 2.8 MB
  marvel    Marvel Comics Screen Posters (1993)        install files (ZIP), 1.9 MB
  snoopy    Snoopy's Screen Savers (1994)              install files (ZIP), 1.9 MB
  looney    The Looney Tunes Screen Saver (1995)       install files (ZIP), 2.8 MB
  screams   ScreamSavers (1995)                        install files (ZIP), 3.3 MB
  disney    The Disney Collection Screen Saver (1995)  install files (ZIP), 3.4 MB

Star Trek: The Screen Saver is After Dark 2.0 (version 2.0b) with 16 Star
Trek modules. Star Wars Screen Entertainment is not an After Dark release
(it is sometimes listed as "After Dark Star Wars"): its modules were made
for Delrina's Intermission screen saver engine. ScreamSavers (Binary
Software) and Snoopy's Screen Savers (Image Smith) are other companies'
modules for After Dark; Snoopy's were made to run in an After Dark already
installed, so Long After Dark supplies the sound library they found there.

No file of any of these releases is included: you import them from your
own copies (and are responsible for sourcing them legally). Requires 64-bit
Windows on an x64 PC.

This folder holds three programs. Keep them together: the screen saver looks
for the other two next to itself.

  LongAfterDark.scr   the screen saver and its settings window
  adhostwin.exe       the emulator; the screen saver starts one per monitor
  adimport.exe        copies the modules from your discs


1. Import your releases

   Double-click adimport.exe, or click Import... in the screen saver's
   settings. Then pick a source:
     - A disc or floppy image: .iso, .bin, .img, .ima, .vfd or .flp, or a
       .zip of the install files or of the floppy images. For a release on
       several floppies, select every image (the Simpsons' two, Star Trek's
       two), or the ZIP they came in. A .zip that keeps each disk's files
       in a folder of its own (DISK1, DISK2, ...) is read as all its disks
       together.
     - A drive or folder: the CD itself, or a folder copied from it (for
       floppies, one folder of every disk's files, or one holding a DISK1,
       DISK2, ... folder per disk).
     - A download from the Internet Archive: the twelve releases with their
       sizes, plus one entry that fetches every release not imported yet.
       Downloads resume if interrupted, and each one is checked against its
       published MD5 before it is used.
   The importer works out which release it was given, checks every file
   against that release's known MD5s and installs it beside the releases
   already imported. Import as many as you like.

   Star Wars Screen Entertainment verifies only as the build on its CD:
   the CD, its ISO or Redump BIN image, the ZIP of its files, or
   "adimport --download swse". The floppy sets found online (the US
   five-disk set and the German edition) are other builds: they fail
   verification (exit code 3) unless imported with "adimport --no-verify".

   Star Trek: The Screen Saver came on two floppies: import both images
   together, in either order, the ZIP they came in, a folder of both disks'
   files, or "adimport --download startrek". One disk alone is refused.

   Marvel Comics Screen Posters, Snoopy's Screen Savers, the Looney Tunes,
   ScreamSavers and the Disney Collection verify as the known ZIP of their
   install files (the one the download fetches), or file by file as a
   folder or ZIP of those files. Every install disk is needed. The Looney
   Tunes' August CD (LTW320CD, or LOONEY.zip) carries After Dark 3.2's
   engine files and fails verification unless imported with
   "adimport --no-verify".

   From a command prompt, with the ids above:
     adimport --image "C:\Images\After Dark 3.2.iso"
     adimport --image disk1.img --image disk2.img
     adimport --image afterdark-20b_startrek.zip
     adimport --image "After Dark - Scream Savers.zip"
     adimport --from E:\
     adimport --download ad10
     adimport --download swse
     adimport --download disney
     adimport --download all
     adimport --list-packages
     adimport --remove tt
   adimport --help lists every option.

2. Covers

   With two or more releases imported, the settings window shows their box
   covers above the module list; click covers to list only those releases.
   An import fetches the cover picture (checked against its published MD5)
   or uses the art on the disc. While a release still shows a plain
   generated cover, "Get the covers" (in the settings window or the
   importer) fetches the pictures. To use a picture of your own, choose
   "Change cover..." (right-click the cover in the settings window, or next
   to the release in the importer) and pick any picture Windows can read; it
   stays on this computer.

3. Install the screen saver

   For yourself: right-click LongAfterDark.scr -> Install. Windows makes it
   the current screen saver where it is and opens Screen Saver Settings, so
   leave this folder where it is.

   For every user: copy LongAfterDark.scr, adhostwin.exe and adimport.exe to
   C:\Windows\System32, then choose "Long After Dark" in Screen Saver
   Settings (Settings -> Personalization -> Lock screen -> Screen saver).

   Right-click -> Test (or double-click the .scr) runs it full screen.

4. Choose what it shows

   Settings... in Screen Saver Settings (or right-click LongAfterDark.scr ->
   Configure) picks one module or Random, the modules it rotates through,
   how often it changes, the resolution, the monitors to use and the sound,
   with a live preview of the selected module. Some modules have buttons of
   their own, such as Fish World's "Select Fish..." or each Star Wars
   module's "Configure...", which open the module's own settings window;
   what you set there is saved at once (Star Trek's Sounder finds your own
   drives under [-h-] in its "Sounds.." window, as After Dark's Globe does
   in its "Map..." window; pick a folder near a drive's root: as in DOS, its
   short path must fit in 63 characters). Marvel's module has "Saver.." to
   choose its posters and "Posters..." to make one a wallpaper, which stays
   inside the emulated PC: your own desktop never changes. The resolution
   applies to the other modules: the Star Wars, Star Trek, ScreamSavers and
   Marvel modules always draw at their original 640x480, scaled up to fit
   the screen.

   Sound is on by default. Only the primary monitor's screen saver plays
   it, at the default volume (50): the modules' wave effects, their MIDI
   music (through Windows' MIDI synthesizer) and the Simpsons' speech.
   In the settings window, Sound (Primary monitor / Off) and Volume (0-100)
   change that. For Star Wars Screen Entertainment, Volume sets the music
   too, as the Windows mixer's synthesizer slider did: Intermission itself
   set only the effects' volume. Preview plays sound with the values you
   have not saved yet; the small live preview never does.

   Any key except Shift, Ctrl, Caps Lock and Num Lock, a click, the mouse
   wheel or moving the mouse ends the screen saver. Caps Lock never does:
   in some modules it changes something or starts a game. While a game is
   playing, press Caps Lock again to stop playing, or Alt to end the screen
   saver at once. Num Lock starts Star Trek's Final Exam: type the number
   of your answer; moving the mouse ends the exam and the screen saver.
   Locking the computer (Win+L) always ends it.

Where your files are

   Everything is in %LOCALAPPDATA%\LongAfterDark (paste that into Explorer's
   address bar): the imported modules (assets\win), Internet Archive
   downloads (downloads), the screen saver's settings (settings.ini), what
   the modules save themselves (state), the settings window's module
   pictures (thumbs) and the last run's log (logs).

Updating

   Close the settings window, make sure the screen saver is not running, and
   replace the three programs with the new ones. Imported releases, downloads
   and settings are kept.

Status

   284 modules from the twelve releases, with their sound, their Caps Lock
   games, Final Exam's Num Lock exam and their own option buttons. Still
   being finished:
     - Speed: each module's pace follows a model of a mid-1990s PC; not
       every module has been compared with the original yet. Marvel's
       poster transitions show at once where the original swept them.
     - Chameleon (Totally Twisted, 10th Anniversary): after about half a
       minute a stray icon covers the "Accessories" label.
   There is no installer or code signing yet. "adimport --version" says
   which version you have (and so does each program's Properties -> Details
   in Explorer).

Licences

   LICENSE.txt is the project's licence. The licences of the code built
   into the programs are in licenses\ (NOTICE.txt there says which is
   whose, and quotes the terms of UNARJ, whose ARJ decoder adimport.exe
   uses in a modified version).
EOF
crlf "$DIST/README.txt" "$DIST/LICENSE.txt" "$L"/*.txt

# Swap it in: the old folder aside first (refused while a program in it
# runs, which leaves it as it was), then the new one in its place.
if [ -e "$FINAL" ]; then
  if ! mv "$FINAL" "$FINAL.old-$$" 2>/dev/null; then
    echo "package.sh: $FINAL is in use (close the screen saver and its settings window); nothing was replaced" >&2
    exit 1
  fi
fi
mv "$DIST" "$FINAL"
trap - EXIT
rm -rf "$FINAL.old-$$"
ls -la "$FINAL" "$FINAL/licenses"
