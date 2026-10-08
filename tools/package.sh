#!/usr/bin/env bash
# Build the three programs (Release) and stage Long After Dark, ready to run:
#   build/dist/LongAfterDark/{LongAfterDark.scr, LongAfterDark.exe, adhostwin.exe,
#                             adimport.exe, README.txt, LICENSE.txt, licenses/}
# LongAfterDark.exe is LongAfterDark.scr again under the name its window
# mode's switches reach it by (Windows starts a .scr with /S alone).
#   bash tools/package.sh
# On Linux (a cross build) the folder also holds the Linux player,
# longafterdark (tools/build-player.sh), and longafterdark.xml, its entry
# for XScreenSaver's settings, with a README.txt and NOTICE.txt for Linux,
# and is zipped as build/dist/LongAfterDark-linux-x64.zip: zip keeps the
# player's executable bit, and the release publishes that zip as it is.
# Env: AD_BUILD_DIR (default build/win-release), AD_DIST_DIR (default
# build/dist/LongAfterDark; replaced whole on every run), AD_COMPONENTS; on
# Linux AD_PLAYER, a player already built to stage instead of building one
# (CI's release build, made on Ubuntu 22.04), and AD_ZIP, the zip (default
# LongAfterDark-linux-x64.zip beside the dist folder).
# Only what ships is built (no test programs, no tests run): build.sh builds
# and tests the whole tree. The binaries carry no link timestamp, so the same
# source gives the same adhostwin.exe and adimport.exe. LongAfterDark.scr
# records its build date and time (__DATE__, __TIME__), so two builds of it
# match only when SOURCE_DATE_EPOCH sets those, as CI does (the commit's
# time); the Linux player is built by the host's g++. The new folder is
# staged beside the old one and swapped in only when complete; if the old
# one is in use (a running screen saver or settings window), nothing is
# replaced.
# No file of any of the releases is ever staged: the user imports their own
# copies with adimport.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${AD_BUILD_DIR:-$ROOT/build/win-release}"
DIST="${AD_DIST_DIR:-$ROOT/build/dist/LongAfterDark}"
# Both lanes: pe32 (the 32-bit modules) and ne16 (the 16-bit ones).
COMPONENTS="${AD_COMPONENTS:-host/core;host/cpu;host/loader;common/ui;host/win32;host/pe32;host/win16;host/ne16;importer;scr}"
eval "$(tr -d '\r' < "$ROOT/tools/versions" | grep -E '^[A-Z0-9_]+=[^ ]*$')"
if [ "$(uname -s)" = "Linux" ]; then
  LINUX=1
  LLVM_DIR="$ROOT/third_party/toolchains/llvm-mingw-$LLVM_MINGW_VER-$LLVM_MINGW_LINUX_BUILD"
else
  LINUX=0
  LLVM_DIR="$ROOT/third_party/toolchains/llvm-mingw-$LLVM_MINGW_VER-ucrt-x86_64"
fi

# On Linux the player first, so a machine without g++ (or zip) fails before
# the long build, never with a dist that lacks the player.
if [ "$LINUX" -eq 1 ]; then
  command -v zip >/dev/null 2>&1 || { echo "package.sh: zip is needed (Debian, Ubuntu: sudo apt install zip)" >&2; exit 1; }
  PLAYER="${AD_PLAYER:-}"
  if [ -z "$PLAYER" ]; then
    PLAYER="$ROOT/build/linux/longafterdark"
    bash "$ROOT/tools/build-player.sh" "$PLAYER"
  fi
  [ -f "$PLAYER" ] && [ -x "$PLAYER" ] || { echo "package.sh: no player at $PLAYER" >&2; exit 1; }
  ZIP="${AD_ZIP:-$(dirname "$DIST")/LongAfterDark-linux-x64.zip}"
  mkdir -p "$(dirname "$ZIP")"
  ZIP="$(cd "$(dirname "$ZIP")" && pwd)/$(basename "$ZIP")"
fi

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
# The same file again: "LongAfterDark.exe /window ..." (scr/src/args.h).
cp "$SCR" "$DIST/LongAfterDark.exe"
cp "$BUILD/host/core/adhostwin.exe" "$BUILD/importer/adimport.exe" "$DIST/"
if [ "$LINUX" -eq 1 ]; then
  cp "$PLAYER" "$DIST/longafterdark"
  chmod 755 "$DIST/longafterdark"
  cp "$ROOT/tools/longafterdark.xml" "$DIST/longafterdark.xml"
fi

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
{
cat <<EOF
Long After Dark: third-party software
=====================================

The three programs (LongAfterDark.scr, also there as LongAfterDark.exe,
adhostwin.exe and adimport.exe) are
linked statically, so each carries inside it the parts of the code below
that it uses. The full licence texts are in this folder (UNARJ's and
Super-xBR's terms, which have no file of their own, are quoted below).

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

  (no file: the terms are here)   MIT
    The Smooth look's shaders in LongAfterDark.scr are a modified version
    of Hyllian's Super-xBR, from libretro's slang-shaders
    (https://github.com/libretro/slang-shaders, commit
    1e0238f9fdd4668ce8212c31d80877af605d3b53,
    edge-smoothing/xbr/shaders/super-xbr/), ported to HLSL. Its notice:
      Copyright (c) 2015 Hyllian - sergiogdb@gmail.com
      Permission is hereby granted, free of charge, to any person
      obtaining a copy of this software and associated documentation
      files (the "Software"), to deal in the Software without
      restriction, including without limitation the rights to use,
      copy, modify, merge, publish, distribute, sublicense, and/or sell
      copies of the Software, and to permit persons to whom the
      Software is furnished to do so, subject to the following
      conditions:
      The above copyright notice and this permission notice shall be
      included in all copies or substantial portions of the Software.
      THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
      EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
      OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
      NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
      HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
      WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
      FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
      OTHER DEALINGS IN THE SOFTWARE.

  LLVM.LICENSE.txt                Apache-2.0 WITH LLVM-exception
    The LLVM runtimes (libc++, libc++abi, libunwind, compiler-rt) of
    llvm-mingw $LLVM_MINGW_VER (https://github.com/mstorsjo/llvm-mingw), in all
    three programs.

  mingw-w64-runtime.COPYING.txt   the mingw-w64 runtime licence
    The mingw-w64 runtime (start-up code and import libraries,
    https://www.mingw-w64.org/), as built by llvm-mingw $LLVM_MINGW_VER,
    in all three programs.

EOF
if [ "$LINUX" -eq 1 ]; then
cat <<'EOF'
The Linux player, longafterdark, is this project's own code (including the
JSON reader adimport.exe also uses), built with GCC. GCC's runtime
libraries, libstdc++ and libgcc, are linked into it statically. They are
under the GPL version 3 with the GCC Runtime Library Exception
(https://www.gnu.org/licenses/gcc-exception-3.1.html), which lets a program
built with GCC be distributed under its own terms. The player uses the C
library (glibc) and the X11 libraries (libX11, libXext and libXrandr) of
the system it runs on, and runs adhostwin.exe and adimport.exe with the
system's Wine; none of them is included.

Apart from the code above and this project's own, everything the three
programs use comes with Windows, or with Wine, which runs them on Linux.
No file of any of the releases the programs run (After Dark and the
others) is included.
EOF
else
cat <<'EOF'
Apart from the code above and this project's own, everything the programs
use comes with Windows (a shader preset's librashader.dll, which a user
may add, is theirs: it is not included). No file of any of the releases
the programs run (After Dark and the others) is included.
EOF
fi
} > "$L/NOTICE.txt"

if [ "$LINUX" -eq 1 ]; then
cat > "$DIST/README.txt" <<'EOF'
Long After Dark for Linux
=========================

Long After Dark is a screen saver that runs the original modules of After
Dark, of LucasArts' Star Wars Screen Entertainment, of Delrina's
Intermission and its Opus 'n Bill, Flintstones, Far Side and Dilbert
collections and of Sierra's Johnny Castaway, unchanged, under x86
emulation. It knows twenty releases, 429 modules:

id        Release                                      Internet Archive download
deluxe    After Dark 4.0 Deluxe (1996)                 CD image, 381.7 MB
ad10      After Dark 10th Anniversary (1999)           CD image, 143.3 MB
ad32      After Dark 3.2 (1995)                        CD image, 58.8 MB
tt        Totally Twisted After Dark (1995)            CD image, 37.9 MB
simpsons  The Simpsons Screen Saver (1994)             install files (ZIP), 2.6 MB
swse      Star Wars Screen Entertainment (1994)        CD image, 6.9 MB
startrek  Star Trek: The Screen Saver (1992)           two floppy images, 2.8 MB
marvel    Marvel Comics Screen Posters (1993)          install files (ZIP), 1.9 MB
snoopy    Snoopy's Screen Savers (1994)                install files (ZIP), 1.9 MB
looney    The Looney Tunes Screen Saver (1995)         install files (ZIP), 2.8 MB
screams   ScreamSavers (1995)                          install files (ZIP), 3.3 MB
disney    The Disney Collection Screen Saver (1995)    install files (ZIP), 3.4 MB
farside   The Far Side Screen Saver Collection (1994)  install files (5 ZIPs), 5.5 MB
dilbert   Scott Adams' Dilbert Screen Saver            install files (ZIP), 4.3 MB
            Collection (1994)
tng       Star Trek: The Next Generation Screen        CD image, 5.8 MB
            Saver (1994)
castaway  Screen Antics: Johnny Castaway (1992)        floppy image (ZIP), 1.3 MB
opus      Opus 'n Bill Screen Saver (1993)             install files (3 ZIPs), 2.8 MB
opusroad  Opus 'n Bill: On the Road Again! (1994)      install files (ZIP), 4.4 MB
flintstones
          The Flintstones Screen Saver                 install files (3 ZIPs in a tar), 129.3 MB
            Collection (1994)
intermission
          Intermission 4.0 (1993)                      three floppy images (ZIP), 3.3 MB

Star Trek: The Screen Saver is After Dark 2.0 (version 2.0b) with 16 Star
Trek modules, and Star Trek: The Next Generation Screen Saver After Dark
3.0 with 13 of the series. Star Wars Screen Entertainment is not an After Dark release
(it is sometimes listed as "After Dark Star Wars"): its modules were made
for Delrina's Intermission screen saver engine, as were those of
Intermission 4.0, Delrina's own release of the engine, and of Delrina's
collections for it: the Opus 'n Bill Screen Saver, Opus 'n Bill: On the
Road Again!, The Flintstones Screen Saver Collection, The Far Side Screen
Saver Collection and Scott Adams' Dilbert Screen Saver Collection.
Intermission 5.0 is not online as a product of its own: its engine came
with On the Road Again and Dilbert. ScreamSavers (Binary Software) and
Snoopy's Screen Savers (Image Smith) are other companies' modules for After
Dark; Snoopy's were made to run in an After Dark already installed, so Long
After Dark supplies the sound library they found there. Screen Antics:
Johnny Castaway (Sierra On-Line) is no module of either: it is a Windows 3.1
screen saver program of its own, which runs unchanged, as Windows 3.1 ran
it.

No file of any of these releases is included: you import them from your
own copies (and are responsible for sourcing them legally).

On Linux the emulator and the importer are the Windows programs, run under
Wine, and longafterdark, a Linux program, shows what the emulator draws:
full screen, in a window, or as one of XScreenSaver's display modes. This
folder holds:

  longafterdark       the Linux player and screen saver
  adhostwin.exe       the emulator; longafterdark runs it under Wine
  adimport.exe        copies the modules from your discs, under Wine
  longafterdark.xml   longafterdark's entry for XScreenSaver's settings
  LongAfterDark.scr   the Windows screen saver (for Windows only)
  LongAfterDark.exe   the same, for its window mode (for Windows only)

Keep longafterdark, adhostwin.exe and adimport.exe together: the player
looks for the other two in the folder it is really in.


What you need

  - A 64-bit PC (x86-64) running Linux with glibc 2.35 or newer (Ubuntu
    22.04, Debian 12, Fedora 36, Linux Mint 21 or later), and an X11
    desktop.
  - 64-bit Wine, the X11 libraries, and the CA certificates that the
    importer's downloads need. On Debian and Ubuntu:
      sudo apt install wine wine64 libx11-6 libxext6 libxrandr2 \
        ca-certificates
  - For the modules' text as it looked on Windows, Microsoft's core fonts
    (sudo apt install ttf-mscorefonts-installer); without them Wine draws
    the text in fonts of its own.
  - For the modules' MIDI music, a MIDI synthesizer on ALSA's sequencer,
    such as FluidSynth, started before Long After Dark:
      sudo apt install fluidsynth fluid-soundfont-gm
      systemctl --user enable --now fluidsynth
    Without one the music is silent; the sound effects still play.


1. Import your releases

   From this folder, with the ids above:
     ./longafterdark --import                 (the importer's window)
     ./longafterdark --import --download simpsons
     ./longafterdark --import --download all
     ./longafterdark --import --image ~/Downloads/afterdark-deluxe.iso
     ./longafterdark --import --image ~/Downloads/"After Dark - Snoopy.zip"
     ./longafterdark --import --from /media/$USER/AFTERDARK
     ./longafterdark --import --list-packages
   "./longafterdark --import" runs adimport.exe under Wine with the options
   that follow ("--import --help" lists them), and gives it your paths in
   the form a Windows program needs. It takes:
     - --image: a disc or floppy image (.iso, .bin, .img, .ima, .vfd or
       .flp), or a .zip or .7z of the install files or of the floppy
       images. For a release on several floppies, give every image (the
       Simpsons' two, Star Trek's two), each with its own --image, or the
       ZIP they came in. A .zip that keeps each disk's files in a folder of
       its own (DISK1, DISK2, ...) is read as all its disks together, and a
       .zip or .7z whose files all sit in one folder as that folder.
     - --from: a folder, such as a mounted CD or a copy of one (for
       floppies, one folder of every disk's files, or one holding nothing
       but DISK1, DISK2, ... folders).
     - --download: a release's Internet Archive copy, by its id; "all"
       imports all twenty, one after another (804 MB, those imported
       already too). Downloads resume if interrupted, and each one is
       checked against its published MD5 before it is used.
   The importer works out which release it was given, checks every file
   against that release's known MD5s and installs it in Wine's data folder
   (below), beside the releases already imported. The first time, Wine
   makes that folder, which takes a moment.

2. Run it

     ./longafterdark
   runs the screen saver full screen: every imported module in turn, five
   minutes each. "./longafterdark --help" lists every option; for example:
     ./longafterdark --list        the modules imported, with their ids
     ./longafterdark toasters      one module, by its id or name
     ./longafterdark -w            in a window
     ./longafterdark --no-sound    without sound

   Any key except Shift, Ctrl, Caps Lock and Num Lock, a click, the mouse
   wheel or moving the mouse ends it. Caps Lock never does: in some modules
   it changes something or starts a game. While a game is playing, press
   Caps Lock again to stop playing, or Alt to end the screen saver at once.
   Num Lock starts Star Trek's Final Exam: type the number of your answer;
   moving the mouse ends the exam and the screen saver. In a window, Esc or
   q closes it when no game is playing.

3. Use it as your screen saver (XScreenSaver)

   XScreenSaver needs an X11 session (at the login screen: GNOME on Xorg,
   Ubuntu on Xorg or Plasma (X11), not a Wayland session):
   a. Install it (Debian, Ubuntu: sudo apt install xscreensaver) and have
      it start when you log in. GNOME, KDE Plasma, Xfce, MATE, Cinnamon,
      LXQt and LXDE start what ~/.config/autostart lists:
        mkdir -p ~/.config/autostart
        printf '%s\n' '[Desktop Entry]' Type=Application Name=XScreenSaver \
          'Exec=xscreensaver -no-splash' \
          > ~/.config/autostart/xscreensaver.desktop
      It runs from your next login on; "xscreensaver -no-splash &" starts
      it now (one dash: XScreenSaver 5 refuses "--no-splash"). With a
      window manager alone, start "xscreensaver -no-splash" from its own
      startup file.
   b. Turn off the desktop's own screen blanking and lock, or they blank
      the screen before XScreenSaver does: in GNOME's Settings, under
      Power and Privacy; in KDE's System Settings, Screen Locking and Power
      Management; in Xfce, MATE or Cinnamon, their own screen saver.
      "man xscreensaver" says how for each desktop.
   c. Put longafterdark on your PATH with a symbolic link, so that it still
      finds the other two programs here:
        sudo ln -s "$PWD/longafterdark" /usr/local/bin/longafterdark
   d. So that XScreenSaver's settings show its options, copy its entry to
      XScreenSaver's configuration folder:
        sudo cp longafterdark.xml /usr/share/xscreensaver/config/
   e. Add it to the programs list in ~/.xscreensaver. That file holds all
      your XScreenSaver settings: edit it, and never copy anything over it.
      If you have none yet, change any setting in xscreensaver-settings
      (xscreensaver-demo in XScreenSaver 5; Mode, for one), which writes
      it. With that program closed, add this line right after the line
      "programs:":
        "Long After Dark"  longafterdark --root  \n\
      XScreenSaver notices the change by itself.
   f. In xscreensaver-settings, choose Long After Dark. Its Settings... has
      the module to show (Module: an id from "./longafterdark --list";
      empty, every module in turn), how often the module changes, whether
      640x480 modules stretch to fill the screen, the resolution, the sound
      and the volume; the preview there is always silent. XScreenSaver's own Cycle After setting also restarts it,
      with another module unless one is chosen (0 turns that off).
   XScreenSaver ends it on any key or mouse move, so the games are played
   in longafterdark's own full-screen mode.

What is different from Windows

   There is no settings window: you choose with longafterdark's options, or
   in XScreenSaver's settings. The modules' own options and buttons (such
   as Fish World's Select Fish... or the Configure... of a Star Wars, Far
   Side or Dilbert module) are not available, so each module runs with its
   defaults. On its own,
   longafterdark plays on the primary monitor and keeps the others black;
   XScreenSaver runs it on every monitor, and only the primary monitor's
   plays sound.

Where your files are

   In Wine's data folder, ~/.wine/drive_c/users/<you>/AppData/Local/
   LongAfterDark (with Wine 6, as on Ubuntu 22.04, .../users/<you>/Local
   Settings/Application Data/LongAfterDark), in $WINEPREFIX instead of
   ~/.wine if you set it: the imported modules (assets/win) and the
   Internet Archive downloads (downloads). What the modules save themselves
   (message texts, high scores, settings files) is in
   ~/.local/share/longafterdark/state.

Updating

   Make sure the screen saver is not running, and unzip the new release
   over this folder. Imported releases, downloads and saved state are kept.

Status

   The 429 modules of the twenty releases, with their sound, their Caps
   Lock games and Final Exam's Num Lock exam. Still being finished, as on
   Windows:
     - Speed: each module's pace follows a model of a mid-1990s PC; not
       every module has been compared with the original yet. Marvel's
       poster transitions show at once where the original swept them.
     - Chameleon (Totally Twisted, 10th Anniversary): after about half a
       minute a stray icon covers the "Accessories" label.
   The Linux player is newer still: docs/LINUX.md (Status) says what has
   not been tried with it yet. "./longafterdark --version" says which
   version you have.

More

   https://github.com/starrlord/longafterdark: docs/LINUX.md there has the
   rest: the player's options, XScreenSaver, troubleshooting and building
   from source.

Licences

   LICENSE.txt is the project's licence. The licences of the code built
   into the programs are in licenses/ (NOTICE.txt there says which is
   whose, and quotes the terms of UNARJ, whose ARJ decoder adimport.exe
   uses in a modified version, and of Super-xBR, which the Smooth look in
   LongAfterDark.scr uses in a modified version).
EOF
else
cat > "$DIST/README.txt" <<'EOF'
Long After Dark
===============

Long After Dark is a screen saver for Windows that runs the original modules
of After Dark, of LucasArts' Star Wars Screen Entertainment, of Delrina's
Intermission and its Opus 'n Bill, Flintstones, Far Side and Dilbert
collections and of Sierra's Johnny Castaway, unchanged, under x86
emulation. It knows twenty releases, 429 modules:

id        Release                                      Internet Archive download
deluxe    After Dark 4.0 Deluxe (1996)                 CD image, 381.7 MB
ad10      After Dark 10th Anniversary (1999)           CD image, 143.3 MB
ad32      After Dark 3.2 (1995)                        CD image, 58.8 MB
tt        Totally Twisted After Dark (1995)            CD image, 37.9 MB
simpsons  The Simpsons Screen Saver (1994)             install files (ZIP), 2.6 MB
swse      Star Wars Screen Entertainment (1994)        CD image, 6.9 MB
startrek  Star Trek: The Screen Saver (1992)           two floppy images, 2.8 MB
marvel    Marvel Comics Screen Posters (1993)          install files (ZIP), 1.9 MB
snoopy    Snoopy's Screen Savers (1994)                install files (ZIP), 1.9 MB
looney    The Looney Tunes Screen Saver (1995)         install files (ZIP), 2.8 MB
screams   ScreamSavers (1995)                          install files (ZIP), 3.3 MB
disney    The Disney Collection Screen Saver (1995)    install files (ZIP), 3.4 MB
farside   The Far Side Screen Saver Collection (1994)  install files (5 ZIPs), 5.5 MB
dilbert   Scott Adams' Dilbert Screen Saver            install files (ZIP), 4.3 MB
            Collection (1994)
tng       Star Trek: The Next Generation Screen        CD image, 5.8 MB
            Saver (1994)
castaway  Screen Antics: Johnny Castaway (1992)        floppy image (ZIP), 1.3 MB
opus      Opus 'n Bill Screen Saver (1993)             install files (3 ZIPs), 2.8 MB
opusroad  Opus 'n Bill: On the Road Again! (1994)      install files (ZIP), 4.4 MB
flintstones
          The Flintstones Screen Saver                 install files (3 ZIPs in a tar), 129.3 MB
            Collection (1994)
intermission
          Intermission 4.0 (1993)                      three floppy images (ZIP), 3.3 MB

Star Trek: The Screen Saver is After Dark 2.0 (version 2.0b) with 16 Star
Trek modules, and Star Trek: The Next Generation Screen Saver After Dark
3.0 with 13 of the series. Star Wars Screen Entertainment is not an After Dark release
(it is sometimes listed as "After Dark Star Wars"): its modules were made
for Delrina's Intermission screen saver engine, as were those of
Intermission 4.0, Delrina's own release of the engine, and of Delrina's
collections for it: the Opus 'n Bill Screen Saver, Opus 'n Bill: On the
Road Again!, The Flintstones Screen Saver Collection, The Far Side Screen
Saver Collection and Scott Adams' Dilbert Screen Saver Collection.
Intermission 5.0 is not online as a product of its own: its engine came
with On the Road Again and Dilbert. ScreamSavers (Binary Software) and
Snoopy's Screen Savers (Image Smith) are other companies' modules for After
Dark; Snoopy's were made to run in an After Dark already installed, so Long
After Dark supplies the sound library they found there. Screen Antics:
Johnny Castaway (Sierra On-Line) is no module of either: it is a Windows 3.1
screen saver program of its own, which runs unchanged, as Windows 3.1 ran
it.

No file of any of these releases is included: you import them from your
own copies (and are responsible for sourcing them legally). Requires 64-bit
Windows on an x64 PC.

This folder holds three programs in four files (the screen saver is there
twice). Keep them together: the screen saver, under either name, looks for
adhostwin.exe and adimport.exe next to itself.

  LongAfterDark.scr   the screen saver and its settings window
  LongAfterDark.exe   the same program, to run it in a window (see 5)
  adhostwin.exe       the emulator; the screen saver starts one per monitor
  adimport.exe        copies the modules from your discs


1. Import your releases

   Double-click adimport.exe, or click Import... in the screen saver's
   settings. Then pick a source:
     - A disc or floppy image: .iso, .bin, .img, .ima, .vfd or .flp, or a
       .zip or .7z of the install files or of the floppy images. For a
       release on several floppies, select every image (the Simpsons' two,
       Star Trek's two), or the ZIP they came in. A .zip that keeps each
       disk's files in a folder of its own (DISK1, DISK2, ...) is read as
       all its disks together, and a .zip or .7z whose files all sit in one
       folder as that folder.
     - A drive or folder: the CD itself, or a folder copied from it (for
       floppies, one folder of every disk's files, or one holding nothing
       but DISK1, DISK2, ... folders).
     - A download from the Internet Archive: the twenty releases with their
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

   The Far Side Screen Saver Collection's only intact copy online is a 1994
   bulletin-board copy of its five floppies, a ZIP of each disk's files
   (PNX-FSC1.ZIP to PNX-FSC5.ZIP; select all five), which the download
   fetches; the floppy images in the Internet Archive item named for it are
   damaged, and an import from them fails. Dilbert verifies as the Internet
   Archive's ZIP of its four floppies' files (DilbertS.zip), or as the same
   disks in four ZIPs, one per disk. A folder or ZIP of the same files
   verifies file by file. Every install disk is needed.

   The Opus 'n Bill Screen Saver verifies as a 1993 bulletin-board copy of
   its three floppies, a ZIP of each disk's files (OPUS1NTA.ZIP to
   OPUS3NTA.ZIP, which the download fetches, or two other copies; select
   all three), and so does its revised build of November 1993 (WC!OPUS1.ZIP
   to WC!OPUS3.ZIP), as that build. On the Road Again verifies as the
   Internet Archive's ZIP of its four floppies' files, all in one folder.
   The Flintstones verify as their June 1994 build, three ZIPs
   (FLINTST1.ZIP to FLINTST3.ZIP) that the download takes out of a 129.3 MB
   archive of a 1994 shareware collection, or as their May 1994 build
   (FLINT1.ZIP to FLINT3.ZIP), which imports without its Prehistoric
   Vehicles, damaged in every known copy. Intermission 4.0 verifies as its
   three floppy images, loose or in the Internet Archive's ZIP of them. The
   bulletin boards' notes and programs beside the files are never opened
   or run.

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
   "adimport --download all" imports all twenty, one after another (those
   imported already too). adimport --help lists every option.

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
   their own, such as Fish World's "Select Fish...", an Intermission
   module's "Configure..." (Star Wars, Opus 'n Bill, Flintstones, Far Side,
   Dilbert, Intermission 4.0) or Johnny Castaway's "Setup...", which open
   the module's own settings window;
   what you set there is saved at once (Star Trek's Sounder finds your own
   drives under [-h-] in its "Sounds.." window, as After Dark's Globe does
   in its "Map..." window; pick a folder near a drive's root: as in DOS, its
   short path must fit in 63 characters). Marvel's module has "Saver.." to
   choose its posters and "Posters..." to make one a wallpaper, which stays
   inside the emulated PC: your own desktop never changes. The resolution
   applies to the other modules: the Intermission modules, both Star Trek
   releases', ScreamSavers and Marvel modules, and Johnny Castaway, always
   get 640x480, scaled up to fit the screen, with bars at the sides on a
   widescreen monitor unless "Stretch to fit the screen" is checked.

   "Look: Sharp pixels", at the end of the "Stretch to fit the screen"
   line, opens the Look menu (Alt+K opens it too), which changes how the
   pictures are drawn. Its two groups, Look and Bars, start at "Sharp
   pixels" and "Black", the way the screen saver has always drawn them.
   Look: "CRT monitor" draws a 1990s VGA monitor (best on a 1440p or
   4K monitor; on a 1080p one its scanlines are left out), "Curved CRT
   monitor" the same behind curved glass, and "Smooth" smooths the edges of
   cartoon art (not photos or dithered pictures). Bars: "Ambient glow"
   fills the bars beside a picture that doesn't fill the screen with a
   blurred, dimmed copy of it. The live preview always shows the plain
   picture; Preview shows the look. They need Direct3D 11: without it, or
   with graphics too slow for the look, the screen saver draws as before.

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

5. Show it in a window (a "be right back" screen in OBS)

   LongAfterDark.exe, the same program as LongAfterDark.scr, shows the
   modules in an ordinary window titled "Long After Dark" instead of full
   screen, for OBS to capture. From PowerShell, in this folder:
     .\LongAfterDark.exe /window /size 1920x1080 /random
   /size is the size of the picture inside the window in pixels (1280x720
   unless you say). /module shows one module, by its id or by its name as
   the settings window lists it, in quotes if it has spaces:
   /module ad40.toasters, /module "Flying Toasters!" (a name several
   releases share needs the id; the program lists the ids). /random
   changes module as Random in the settings does, as often as it says
   (with /module, that module first). With neither, the window shows what
   the screen saver would. "LongAfterDark.exe /help" lists the switches.
   Windows starts a .scr with /S alone, whatever follows its name, so they
   work only with the .exe.

   Keys and the mouse never close the window, and Caps Lock starts no
   game; close it as any other window. While it is open Windows keeps the
   display on, which should also keep the screen saver from starting.
   Several can be open at once.

   In OBS, add a Window Capture source: Window "[LongAfterDark.exe]: Long
   After Dark", Capture Method "Windows 10 (1903 and up)", Window Match
   Priority "Match title, otherwise find window of same type" (the settings
   window and the program's messages are titled Long After Dark too, but
   only this window is of its type), and "Capture Cursor" unticked (the
   window never hides the pointer, so it would show on the stream). The
   window plays sound as the screen saver does (Sound and Volume in its
   settings); an Application Audio Capture source for the same window puts
   it in the stream.

Where your files are

   Everything is in %LOCALAPPDATA%\LongAfterDark (paste that into Explorer's
   address bar): the imported modules (assets\win), Internet Archive
   downloads (downloads), the screen saver's settings (settings.ini), what
   the modules save themselves (state), the settings window's module
   pictures (thumbs) and the last run's log (logs).

Updating

   Close the settings window, make sure the screen saver is not running, and
   replace the programs (LongAfterDark.scr and LongAfterDark.exe,
   adhostwin.exe, adimport.exe) with the new ones. Imported releases,
   downloads and settings are kept.

Status

   429 modules from the twenty releases, with their sound, their Caps Lock
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
   uses in a modified version, and of Super-xBR, which the Smooth look in
   LongAfterDark.scr uses in a modified version).
EOF
fi
# Linux keeps its own line endings.
if [ "$LINUX" -eq 0 ]; then crlf "$DIST/README.txt" "$DIST/LICENSE.txt" "$L"/*.txt; fi

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

# The Linux zip, made here where the player's mode is known (the release
# publishes it unchanged): zip on Linux records each file's mode.
if [ "$LINUX" -eq 1 ]; then
  rm -f "$ZIP.tmp"
  (cd "$(dirname "$FINAL")" && zip -qrX "$ZIP.tmp" "$(basename "$FINAL")")
  mv "$ZIP.tmp" "$ZIP"
  echo "package.sh: $ZIP"
fi
