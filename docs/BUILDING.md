# Building Long After Dark

**Long After Dark** runs the original **Windows** After Dark modules on
64-bit Windows by emulating their x86 code. It loads Berkeley Systems' real
engines and modules into one emulated address space, traps every call they
make into the operating system, and supplies that OS surface from the host.
The screen saver presents the frames. It runs the modules of LucasArts'
Star Wars Screen Entertainment the same way: they were made for Delrina's
Intermission, whose reader, library and modules run as real code too.

It supports seven releases: After Dark 4.0 Deluxe, After Dark 10th
Anniversary, After Dark 3.2, Totally Twisted After Dark, The Simpsons
Screen Saver, Star Trek: The Screen Saver (After Dark 2.0b) and Star Wars
Screen Entertainment, 232 catalog entries in all
(`docs/PACKAGES.md`). Installing
and using it is covered in [INSTALL.md](INSTALL.md). This page is for
working on it: the architecture, the components, building, testing,
headless runs, where the assets live, and the conventions. Every command
below runs from the repository root, in Git Bash on Windows or bash on
Linux ([Building on Linux](#building-on-linux)).

**No After Dark or Star Wars Screen Entertainment files are shipped or
committed.** Nothing in the repository,
the build output or the test fixtures is a file of those releases; synthetic
fixtures are generated at test time. Users import their own copies with
`adimport.exe`, from their disc, an image or a folder, or from the Internet
Archive. Import manifests hold only path, size and MD5, and the AD 3.x
archive password is derived at import time, never stored. Extractions,
disassembly and other reverse-engineering output stay under `research/`
(gitignored), and local disc images under `source_iso/` (gitignored). Notes
from third parties with no open-source licence (the `anne-skydancer/afterdark-win11`
repository) are leads to verify by our own disassembly, never code to copy
(`docs/ABI.md`). Third-party code that is built in is listed in
[THIRD_PARTY_LICENSES.md](../THIRD_PARTY_LICENSES.md).

## Architecture

```
LongAfterDark.scr ──spawns──► adhostwin.exe  one process per monitor, preview, thumbnail or button run
  /s /p /c, settings dialog      │
  presents P8 frames  ◄─ stdout ─┤  adw::core    8-bit screen + palette, frame protocol, virtual clock, pacing,
  KEY/CAPS/MOUSE/SET/GO ─ stdin ►│               audio engine (mixer, MIDI player) ─► WASAPI / MIDI mapper, or a WAV capture
  reads the status record ◄ shm ─┤  adw::win32   Win32 guest runtime: KERNEL32/USER32/GDI32/WINMM/DSOUND… ─┐ real GDI on DIB
                                 │  adw::win16   Win16 guest runtime: KERNEL/USER/GDI/MMSYSTEM…           ─┘ sections over
                                 │  adw::loader  PE32 + NE images                                             emulated memory
  Import… ──► adimport.exe       │  adw::cpu     x86 interpreter: flat 32-bit, 16-bit protected mode, x87
                  │              ▼
                  │   emulated: pe32 lane  module + ADXPL510 engine
                  │             ne16 lane  module + ADXPL300/310/40 engine + helpers, under OLDMOD16 or the native AD3 bridge,
                  │                        or an Intermission module + SWSE.DLL + INTRMLIB, under Intermission's IMIMXPLY.IMQ
                  ▼
  %LOCALAPPDATA%\LongAfterDark\assets\win\{FILES, packages\<id>, catalog-win.json}   read by the host and the saver
```

* **Two lanes, chosen by the module file's header.** `pe32` runs 32-bit
  After Dark 4-generation modules (PE32 DLLs) with the real `ADXPL510.DLL`.
  `ne16` runs 16-bit modules (NE DLLs: the AD 2.x/3.x "Classic" modules and
  those of AD 3.2, Totally Twisted, the Simpsons, the 10th Anniversary and
  Star Trek: The Screen Saver) with their real engines. Where a disc ships
  Berkeley's `OLDMOD16.DLL` bridge, the lane runs it; otherwise a
  host-native AD3 bridge implements OLDMOD16's verified behaviour
  (`docs/PACKAGES.md` §7.4), standing in for Star Trek's After Dark 2.0
  host, `AD.EXE`, too (`docs/ABI.md` §3.9). Star Wars Screen
  Entertainment's modules are NE DLLs too: the ne16 lane tells them apart
  by their exports and drives them through Intermission's own reader,
  `IMIMXPLY.IMQ`, a second module protocol beside the After Dark one
  (`docs/PACKAGES.md` §7.5, `docs/ABI.md` §3.8).
* **Run the original code, replace only the OS and the host.** The engines,
  helper DLLs and modules all run as emulated code. The host stands in for
  Windows 95 underneath them, and for the original `AFTERDAR.SCR`,
  `OLDMOD32.DLL` and Intermission's `INTERMIS.EXE` above them.
* **An authentic display.** The guest sees an 8-bit `RC_PALETTE` screen with
  the 20 static colours. Drawing is done by the real GDI onto DIB sections
  whose bits alias emulated memory (arenas are file mappings), and palette
  animation reaches the frame as a new palette.
* **One protocol for every front-end.** `adhostwin` writes `P8` frames on
  stdout and reads text commands on stdin (`docs/DESIGN.md` §1). Interaction adds a shared-memory status record and numbered input
  lines (`docs/INTERACTION.md` §3).
* **Deterministic headless runs.** Every clock the guest reads comes from a
  virtual clock, and each run is seeded, so headless `FBHASH` streams
  compare run to run.
* **Packages are self-contained.** Each release is imported into its own
  folder and its modules never resolve a file from another release, so one
  disc is enough to run it (`docs/PACKAGES.md` §4, §7).
* **One sound engine for both lanes.** The lanes map the modules' sound
  APIs (DirectSound, ACM, the MCI sequencer and `waveOut` for the 32-bit
  modules; AD_SND's `sndPlaySound`, `waveOut`, `midiOut` and MCI strings for
  the 16-bit ones, and for Star Trek: The Screen Saver's AD_SND 1.0 its
  multimedia sound driver's `sndPlaySound` and `waveOut` volume; for Star
  Wars Screen Entertainment, `SWSE.DLL`'s own `sndPlaySound` for the
  effects and MEMMIDI's `midiOutShortMsg` stream, sequenced from a
  multimedia timer, for the music) onto one deterministic
  mixer, MIDI file player and raw MIDI port in `adw_core`, which plays
  through WASAPI and the Windows MIDI mapper or captures to a WAV. What the
  guest sees depends only on its own calls and virtual time. With sound off
  (`adhostwin`'s default) every sound API answers exactly as the silent
  host of before, so the headless `FBHASH` baselines hold
  (`docs/AUDIO.md`).

## Documents

| Document | What it covers |
|---|---|
| [INSTALL.md](INSTALL.md) | For users: importing the releases, covers, installing the screen saver, its settings, where the files are, updating |
| [LINUX.md](LINUX.md) | For users on Linux: what the Linux player needs, importing under Wine, its options and controls, XScreenSaver, where the files are |
| [DESIGN.md](DESIGN.md) | The architecture and every contract in brief: host protocol, CPU, loader, core, shims, pacing, catalog and settings, assets, packages, interaction, covers, audio |
| [ABI.md](ABI.md) | The host/engine/module ABI, recovered by our own disassembly and verified on the After Dark 4.0 Deluxe disc's binaries, Intermission's IMX protocol, verified on the Star Wars Screen Entertainment disc's (ABI.md §3.8), and After Dark 2.0's host, `AD.EXE`, verified on Star Trek: The Screen Saver's floppies (ABI.md §3.9) |
| [API_SURFACE.md](API_SURFACE.md) | Every function the Deluxe disc's modules and engines import, counted and classified (and where Star Wars Screen Entertainment's census is) |
| [PACKAGES.md](PACKAGES.md) | The seven releases: registry, identification, extraction formats (ISO-9660, FAT, PKZIP, ARJ, SZDD, KWAJ), layouts, the merged catalog, how the lanes find a package, the native AD3 bridge and After Dark 2.0, the Intermission protocol |
| [INTERACTION.md](INTERACTION.md) | Wake or play, the status record, module buttons (`--configure`), the per-user state overlay, the desktop seed |
| [COVERS.md](COVERS.md) | Releases in the settings dialog (the box-cover strip, filtering and grouping by release), the importer's cover pipeline, the shared UI library `adw_ui` and the importer's restyled windows (implemented; §9 records what was built) |
| [AUDIO.md](AUDIO.md) | Sound: how every module makes it (a census of all 202), the host audio engine (`adw/core/audio.h`), both lanes' DirectSound/ACM/WINMM/MMSYSTEM mappings, the saver's Sound and Volume settings, the tests |

Each component's README (below) is authoritative for how that component
actually behaves; the design documents record the plan and the evidence.

## Components

| Directory | Builds | Namespace | What it is | Read |
|---|---|---|---|---|
| `host/cpu/` | `adw_cpu` | `adw::cpu` | The x86 interpreter, vendored from resource_dasm's `X86Emulator` (MIT) and extended with 16-bit protected-mode segmentation, faults, x87 on the host FPU and file-mapped memory | DESIGN §2, `LICENSE.resource_dasm` |
| `host/loader/` | `adw_loader`, `adwinspect.exe` | `adw::loader` | PE32 and NE parsing, placement, relocation, imports, exports, resources; `adwinspect` dumps any PE/NE file | DESIGN §3 |
| `host/core/` | `adw_core`, **`adhostwin.exe`** | `adw` | The host process: screen, protocol, environment, virtual clock, pacing, the audio engine (mixer, MIDI player, WASAPI/`waveOut`/MIDI-mapper and capture sinks), status record, `--configure` driver, lane registration | [host/core/README.md](../host/core/README.md) |
| `host/win32/` | `adw_win32` | `adw::win32` | The Win32 guest runtime (TEB/PEB, thunks, SEH, heap, VFS and state overlay, 8-bit display) and its API shims, DirectSound included | [host/win32/README.md](../host/win32/README.md) |
| `host/pe32/` | `adw_lane_pe32` | `adw::pe32` | The 32-bit lane: `AD_MODULE32`, the DRAWFRAME budget loop, controls, package-aware DLL search | `pe32/lane.hh` |
| `host/win16/` | `adw_win16` | `adw::win16` | The Win16 guest runtime (LDT, global/local heaps, DOS/BIOS layer, synthetic desktop, the DIB driver) and its API shims, MMSYSTEM's sound included | [host/win16/README.md](../host/win16/README.md) |
| `host/ne16/` | `adw_lane_ne16` | `adw::ne16` | The 16-bit lane: its module protocols (After Dark: OLDMOD16 or the native AD3 bridge; Intermission: the IMX reader), palettes, small screens, long calls | `ne16/lane.hh` |
| `importer/` | `adw_import`, **`adimport.exe`** | `adw::import` | Package registry, ISO-9660/Joliet, FAT12/16, PKZIP, ARJ, SZDD and KWAJ readers, verification, atomic per-package import, Internet Archive downloads, covers, catalog generator | [importer/README.md](../importer/README.md) |
| `importer/gui/` | `adw_import_gui` | `adw::import::gui` | The importer's windows (`adimport --gui`): sources, downloads, progress, result, cover | [importer/gui/README.md](../importer/gui/README.md) |
| `scr/` | `adw_scr`, **`LongAfterDark.scr`** | `adw::scr` | The screen saver (`/s`, `/p`, `/c`), the settings dialog, input rules, which host plays sound, thumbnails, desktop capture | [scr/README.md](../scr/README.md) |
| `scr/linux/` | **`longafterdark`** (by `tools/build-player.sh`, not CMake) | `lad` | The Linux player: runs `adhostwin.exe` under Wine and presents its frames on X11 (full screen, a window, XScreenSaver's window or its preview) with the saver's input, sound and screen rules; unit and smoke tests in `tests/` | [LINUX.md](LINUX.md), INTERACTION §4.5 |
| `common/ui/` | `adw_ui` | `adw::ui` | The Windows 11 theming and widgets shared by the settings dialog and the importer's windows | [common/ui/README.md](../common/ui/README.md), COVERS §3 |
| `cmake/` | | | `llvm-mingw.cmake` (the toolchain file), `adw_version.h.in` (the one version), `adhostwin.rc` and its manifest | |
| `tools/` | | | `bootstrap.sh`, `build.sh`, `package.sh`, `build-player.sh` (the Linux player), `versions` (the pinned dependencies), `longafterdark.xml` (the player's entry for XScreenSaver's settings) | below |

Each lane's knobs (`ADMIPS`, `ADNE16BRIDGE`, `ADPE32SCALE`, …) are listed in
the header comment of its `lane.hh`.

## Build

You need Git for Windows (Git Bash, with `curl`, `unzip` and `git`) and
CMake 3.24 or later, on `PATH` or in `C:\Program Files\CMake`. The host
executes x87 code on the host FPU, so the target is x86-64 Windows only (on
Linux the same programs are cross-built for it: [Building on
Linux](#building-on-linux)).

```bash
# Git Bash, from the repository root
bash tools/bootstrap.sh     # once: llvm-mingw, ninja, zlib, phosg -> third_party/ (gitignored)
bash tools/build.sh         # configure + build + ctest -> build/win
bash tools/package.sh       # Release build in build/win-release, staged for install under build/dist/
```

* **`bootstrap.sh`** fetches a portable llvm-mingw (clang, lld, libc++) and
  ninja into `third_party/toolchains/`, and builds pinned zlib and phosg into
  `third_party/win/local/`. Nothing is installed system-wide.
  `AD_SEED_DIR=<dir>` supplies the toolchain zips offline. Every version and
  both zips' sha256 are pinned in `tools/versions` (the toolchain file uses
  exactly `LLVM_MINGW_VER` from there). It heals itself: a zip is checked
  after every fetch or reuse, downloads go to `.part` files, an unpacked
  toolchain is marked `.bootstrap-complete` only when whole, and
  `third_party/win/local/.bootstrap-deps` rebuilds zlib and phosg from
  scratch whenever a pin changes; rerunning it is a no-op otherwise.
* **`build.sh [cmake --build args]`** configures the repository root with
  Ninja and the llvm-mingw toolchain file (`cmake/llvm-mingw.cmake`), builds,
  and runs `ctest`. Environment:
  `AD_BUILD_DIR` (default `build/win`), `AD_COMPONENTS` (a `;`-separated
  subset of component directories), `AD_NO_TESTS=1`, `AD_CTEST_ARGS`
  (extra `ctest` arguments, e.g. `-R import` or `-LE gui -j8`: split at
  whitespace and passed as they are, never read as shell syntax or
  globbed, so a regex needs no quotes, as in `-R cpu|win16`, and can hold
  no space), `CMAKE_BUILD_TYPE` (default Release, which an interpreter
  needs).
* **`package.sh`** builds only the three shipped programs in Release (in
  `AD_BUILD_DIR`, default `build/win-release`; no test programs, no tests
  run) and stages `LongAfterDark.scr`, `adhostwin.exe`, `adimport.exe`, a
  `README.txt` for users, `LICENSE.txt` and, in `licenses\`, the licence
  texts of the code built into the programs (resource_dasm, phosg, zlib,
  LLVM, the mingw-w64 runtime) with a `NOTICE.txt` saying which is whose, in
  `build/dist/LongAfterDark/` (`AD_DIST_DIR` overrides it). The new folder is
  staged beside the old one and swapped in only when complete; while a
  program in the old one runs (a screen saver or settings window), nothing
  is replaced and it exits 1. The binaries carry no link timestamp, so the
  same source gives the same `adhostwin.exe` and `adimport.exe` byte for
  byte (`LongAfterDark.scr` differs only in the build date and time its
  last-exit log records, unless `SOURCE_DATE_EPOCH` sets them, as CI
  does), and all three carry a VERSIONINFO
  (`project(VERSION)` in `CMakeLists.txt`, with the copyright line taken
  from `LICENSE`).

Work on one component in its own build directory, so parallel work never
shares a tree:

```bash
AD_BUILD_DIR=build/win-host AD_COMPONENTS="host/core;host/cpu;host/loader;host/win32;host/pe32;host/win16;host/ne16" \
  bash tools/build.sh
AD_BUILD_DIR=build/win-import AD_COMPONENTS="host/loader;importer" bash tools/build.sh
AD_BUILD_DIR=build/win-scr AD_COMPONENTS="host/core;host/loader;scr" bash tools/build.sh
```

Outputs, inside a build directory: `host/core/adhostwin.exe`,
`importer/adimport.exe`, `scr/LongAfterDark.scr`, `host/loader/adwinspect.exe`,
and `scr/LongAfterDark-test.scr`, the saver with its test hooks
(`AD_SCR_TEST_*`, `AD_SCR_TESTEXIT_*`) compiled in: the smoke tests run it,
and so must anything else that sets those variables (with `AD_HOST_EXE`
naming the host); the packaged `LongAfterDark.scr` never reads them.
The binaries are linked statically (no DLLs beside them) with an 8 MB stack.

## Building on Linux

On Linux the same three programs are cross-built for Windows with the
pinned llvm-mingw's Linux build, and Wine runs the build's own Windows
programs (the icon generators) and the tests. The Linux player,
`longafterdark` (`scr/linux/`, [LINUX.md](LINUX.md)), is built beside them
with the system's g++ by `tools/build-player.sh`; CMake never compiles
`scr/linux/`. On Debian or Ubuntu (24.04 or later: 22.04's CMake, 3.22, is
older than the 3.24 the build needs, though the player alone builds there):

```bash
sudo apt install cmake git curl unzip xz-utils zip g++ libx11-dev libxext-dev libxrandr-dev wine wine64
sudo apt install xvfb xdotool python3   # an X display for the tests, and the player's smoke test
bash tools/bootstrap.sh                 # once: the toolchain's Linux build, ninja, zlib, phosg -> third_party/
bash tools/build-player.sh              # the player -> build/linux/longafterdark
bash tools/build-player.sh --tests      # its unit tests, built and run
bash tools/package.sh                   # the player, then build/dist/LongAfterDark and its zip
# the cross build and its tests under Wine -> build/win, without the tests Wine fails (below):
xvfb-run -a env AD_CTEST_ARGS='-LE gui -E ^(ui\.(theme|image|capture|slider)|core\.audio|win(16|32)\.unit|import\.(download|covers|gui_model)|scr_unit_(releases|present))$ -j4' bash tools/build.sh
```

* **`bootstrap.sh`** also needs `curl`, `git`, `cmake`, `unzip`, `tar` and
  `xz` on Linux, and names any that is missing. Everything specific to the
  host has a folder of its own, so a checkout shared with Windows (WSL, a
  dual boot, a container's bind mount) keeps both hosts' builds:
  `third_party/toolchains/llvm-mingw-<ver>-<LLVM_MINGW_LINUX_BUILD>/` (the
  release's Linux build; `LLVM_MINGW_LINUX_BUILD` is in `tools/versions`),
  `third_party/toolchains/ninja-linux/`, and zlib and phosg in
  `third_party/win/local-linux/` (built in `third_party/win/build-linux-*`);
  their sources are shared. The Linux archives have digests of their own
  (`LLVM_MINGW_LINUX_SHA256`, `NINJA_LINUX_SHA256`), so bumping
  `LLVM_MINGW_VER` or `NINJA_VER` needs both digests. `AD_SEED_DIR` supplies
  `llvm-mingw-*.tar.xz` and `ninja-linux.zip` offline.
* **`cmake/llvm-mingw.cmake`** makes it a cross build
  (`CMAKE_SYSTEM_NAME Windows`), runs the build's programs through Wine
  (`CMAKE_CROSSCOMPILING_EMULATOR`), and stops at configure with a message
  when there is no Wine (`-DWINE_EXECUTABLE=<path>` names one).
* **`build-player.sh [<output>]`** compiles every `scr/linux/*.cc` (not
  `tests/`) and `importer/minijson.cc` as C++20 with `-O2 -Wall -Wextra`
  (not the environment's `CXXFLAGS`), links libstdc++ and libgcc statically
  and `-lX11 -lXext -lXrandr -lpthread`, and takes the version from
  `adw_version.h`, which it makes beside the output from
  `cmake/adw_version.h.in` as CMake does. The player then needs only glibc
  (the version it was built against, or newer) and libX11, libXext and
  libXrandr. `AD_CXX` names the compiler, `AD_WERROR=1` makes warnings
  errors, and `AD_GLIBC_MAX=2.35` refuses (and deletes) a player that needs
  a newer glibc symbol, or libstdc++ or libgcc_s at all. `--tests [<output>]`
  builds `scr/linux/tests/unit.cc` with the same sources but `main.cc` and
  runs it; it needs no X display and no Wine.
* **`package.sh`** on Linux builds the player first (or stages
  `AD_PLAYER=<player>`), so a machine without g++ or `zip` fails before the
  long build, then builds the three programs as on Windows and stages
  `build/dist/LongAfterDark/` with `longafterdark` (mode 755),
  `longafterdark.xml`, a Linux `README.txt`, `LICENSE.txt` and `licenses/`,
  whose `NOTICE.txt` covers the player too; the text files keep LF endings.
  It zips the folder as `build/dist/LongAfterDark-linux-x64.zip` (`AD_ZIP`
  names another), since zip keeps the player's executable bit.
* **The tests under Wine** are the Windows ones, but for twelve that check
  what Wine does differently from Windows: the pixels of the Windows 11
  look, Windows' own ADPCM codec, 8.3 aliases, GDI object counts, WinHTTP
  on dropped and cancelled transfers, dropped files, Segoe UI's metrics and
  HALFTONE stretching. A plain `build.sh` runs them too: they fail, and
  `import.download` and `import.covers` hang until their timeouts. The
  `-E` list above leaves them out; the CI's, in
  `.github/workflows/build.yml`, is the same and says why for each, and the
  Windows job runs them all. Without 32-bit Wine, `cpu_x86_diff` and
  `cpu_x86_diff_seed2` skip (their oracle runs x86 code natively).
* **The player's smoke test**,
  `bash scr/linux/tests/smoke.sh <player> <adhostwin.exe> [<assets root>]`,
  runs it in private Xvfb servers and a private Wine prefix, as CI does
  with `build/win-release/host/core/adhostwin.exe`: the test pattern's
  frames (or a module's), a host killed mid-run (the player must recover
  or end, never hang), Shift and Caps Lock not ending it (its host hearing
  Caps Lock) and a key ending it, a 16-bit display without MIT-SHM, a host
  killed while the display is off (`--test-display-off`: the player must
  sleep, start no host until the display is back, then show frames again),
  XScreenSaver's protocol (`$XSCREENSAVER_WINDOW`, a resize, SIGTERM) and a
  preview whose window goes away (neither host hearing Caps Lock), and two
  RandR monitors (frames on the primary one, black on the other, and a
  move over it ends the player); these last three need python3, and are
  skipped without it. It exits 0 when every check passes,
  1 on a failure, 2 when it can't run.

## Test

Every component registers CTest tests under `<component>/tests/`; its README
lists them. Run them with `build.sh`, or `ctest --output-on-failure` in the
build directory. Left to their defaults the suites are offline, their
fixtures are synthetic, and every test that needs After Dark (or Star Wars
Screen Entertainment) files skips without them. The importer's readers are
tested on archives and images built at test time: `import.zip`,
`import.fat`, and, for Star Wars Screen Entertainment's installer,
`import.arj` (ARJ volumes, thirteen fixed LZH and method-4 vectors that the
Python reference and 7-Zip decode the same way, and streams crafted bit by
bit; no test compresses anything into ARJ,
[THIRD_PARTY_LICENSES.md](../THIRD_PARTY_LICENSES.md)) and `import.szdd`
(SZDD, with two fixed vectors that Windows' `EXPAND.EXE` expands the same
way); for Star Trek: The Screen Saver's, `import.kwaj` (KWAJ method 3:
eight fixed vectors of made-up data and six streams written token by
token, each expanded alike by the research reference decoder, libmspack and
Deark, then cut, bit-flipped and bounded; no test compresses anything),
and `import.fat` reads ZIPs of floppy images too; `import.packages` imports
all seven releases from made-up sources.

* **Tests that open real windows** are labelled `gui`: the screen saver's
  smoke tests (`scr_smoke_*`; `/s` briefly covers every monitor) and the
  importer's `import.gui_flow` (real importer windows, briefly). `ctest -LE
  gui` (with `build.sh`: `AD_CTEST_ARGS="-LE gui"`) skips every one of them;
  add `-j8` to run the rest in parallel. Since Star Trek: The Screen Saver
  they include `scr_smoke_screen-field` (a module whose catalog entry has
  `"screen": "640x480"`: its hosts, the seed pictures, a monitor changing,
  `/p`, the live preview and a thumbnail, with
  `scr/tests/fixtures/catalog-seven.json`) and `scr_smoke_numlock` (the
  `NUMLOCK` lines and `ADNUMLOCK`, against the test host `fakehost.exe`,
  which says `numlock=1` unless `FAKEHOST_NUMLOCK=0`), and
  `scr_smoke_seed-screens` (a catalog whose modules each give a screen of
  their own: three desktop-seed pictures at most, the screens left out
  counted in the log; it holds on a desktop that can't be read back too). While the monitors
  are asleep the saver pauses its hosts, and every smoke test that runs a
  `/s` times out: set `AD_SCR_TEST_DISPLAY_ON=1` for the run, a lever only
  the test build, `LongAfterDark-test.scr`, reads (`scr_resources` checks
  that the shipped `LongAfterDark.scr` does not hold it).
  Separately, `AD_SCR_SKIP_GUI_TESTS=1` makes the saver's report SKIP and
  `AD_IMPORT_SKIP_GUI_TESTS=1` the importer's. `import.gui_shots` renders
  every importer page in parked, cloaked windows that never show on the
  desktop, so it is not labelled `gui`; `import.cli` drives the importer's
  real windows only with `AD_GUI_TESTS=1`.
* **Tests never play sound.** Every scr test runs with `AD_SCR_SOUND=0`
  (no host the saver starts makes sound), and the tests about sound capture
  it instead: `ADAUDIOOUT=<file.wav>`, plus `ADAUDIOLIVE=0` in streamed
  runs, so nothing reaches the audio device. The one exception is opt-in
  (`AD_AUDIO_LIVE_TEST=1`, below).
* **Tests that read the imported releases** use `AD_ASSETS_DIR` when it is
  set, else the installed assets, read-only (`host/core/tests/test_paths.h`:
  they never move or write the data folder, and every host they start gets
  a scratch `AD_LOCALAPPDATA`). They skip (exit 77) when there are none:
  `core.lane_detect_assets` (which also probes Star Wars Screen
  Entertainment's `packages\swse\SAVER\*.IMX` when that release is there),
  `pe32.assets`, `pe32.packages` (the 10th Anniversary's pe32 modules;
  `AD_PE32_PKG_ROOT=<assets root>` names another root), `ne16.assets`,
  `ne16.interaction` (each absent module is skipped), `ne16.swse` (Star
  Wars Screen Entertainment's 14 Intermission modules: each twice, keys the
  saver does not wake on, the pace of the modules that step once per call,
  the reader oracle, a small screen, the desktop seed, a streamed start,
  sound and the music's volume, and Darth Vader's **Configure...** button;
  `AD_NE16_SWSE_ROOT=<assets root>` names another root, and it skips when
  its root holds no `swse`), `ne16.startrek` (Star Trek: The Screen Saver's
  16 After Dark 2.0 modules: each twice, over AD_SND 1.0 and the
  `AD_PREFS.INI` seeds, sound captured twice, Scotty's Files' blueprints,
  Communications' and Sounder's buttons in configure mode, hidden and
  scripted, Sounder's folder chosen through `[-h-]` (saved as `H:\…` and
  played by a later run; `ne16.interaction` does the same for Globe's map
  in both releases that have it), and a scripted Final Exam,
  Num Lock, answers, the mouse and the wake, headless and streamed;
  `AD_NE16_STARTREK_ROOT=<assets root>` names another root, and it skips
  when its root holds no `startrek`) and `import.catalog_real` (which also
  needs the prototype's `research/win/catalog-win.json`).
* **Opt-in tests** skip (77) unless their variable is set:

  | Variable | Runs | Needs | Network |
  |---|---|---|---|
  | `AD_E2E=1` | `import.e2e` (the Deluxe image, ~400 MB, imported and compared), `import.download_real` (every release from its Internet Archive copies; `AD_E2E_LOCAL_DIRS`, default the image folders below and the installed downloads, supplies verified local copies instead; `AD_E2E_PACKAGES=<id>[,<id>…]` limits it to those releases), `import.covers_real` (every registry cover) | disk space | **yes** |
  | `AD_E2E=1` + `AD_E2E_ASSETS=<assets root>` | the screen saver's real-module runs `scr_smoke_e2e-rodger` and `scr_smoke_e2e-dosshell` (real windows, label `gui`); note that `AD_E2E=1` also turns on the importer's downloads above | imported releases | no (the importer's: yes) |
  | … + `AD_SCR_SOUND_E2E=1` | `scr_smoke_e2e-sound`: the saver's sound on two staged monitors, captured to a WAV, never played | as above, and a host whose `--capabilities` says `audio=1` | no |
  | `AD_E2E_PKG=1` | `import.pkg_real` (the five real package images and Star Trek's two disk images, loose or in the ZIP they came in, found by MD5 in `AD_SOURCE_ISO_DIR`, a `;`-separated list of folders, default `source_iso/`, and in the folders directly in each; it also checks the Star Trek recipe against the disks' own `ST_NSTLL.INF`); with `AD_E2E=1`, `import.covers_real` also takes their disc art | the images | no |
  | `AD_AUDIO_ASSETS=1` (or a comma list of cases) | `core.audio_assets`: real modules' sound, captured and measured | imported releases (`AD_E2E_ASSETS`, else `AD_ASSETS_DIR`, else the installed ones) | no |
  | `AD_NE16_PKGROOTS=<dir>` | `ne16.pkg`: one module each of `ad10`, `ad32`, `tt` and `simpsons`, run standalone from one root per release (`<dir>\<id>\win\packages\<id>\…`, PACKAGES.md §4.4) | those roots | no |
  | `AD_GUI_TESTS=1` | `import.cli`'s runs of the importer's real windows | an interactive desktop | no |
  | `AD_AUDIO_LIVE_TEST=1` | the last check of `core.audio`: 3 s of test tone and a MIDI note **on the audio device**, for a person listening; never automated | speakers | no |

  `AD_E2E_KEEP=1` keeps the scratch folders of `import.download_real` and
  `import.covers_real`; `AD_E2E_NO_SEED=1` makes `import.download_real`
  download everything instead of reusing local copies.
* **Never use the real data folder.** Tests and experiments point
  `AD_ASSETS_DIR`, `AD_SETTINGS`, `adimport --dest` and `--download-dir` at
  scratch folders;
  nothing automated may write to `%LOCALAPPDATA%\LongAfterDark`.
  `AD_LOCALAPPDATA=<scratch>` stands in for `%LOCALAPPDATA%` wherever the
  data folder is derived through `adw/core/data_root.h`.
* **Determinism.** The Deluxe modules' headless `FBHASH` streams are the
  regression baseline for lane changes (`docs/PACKAGES.md` §9; for the
  sixth release, the streams of all 202 After Dark modules, three of them
  changed on purpose and accepted there, each with its reason, and for the
  seventh, three more, with the 14 Star Wars Screen Entertainment modules'
  streams unchanged). Sound off
  leaves them unchanged. A run with sound on differs from one without (the
  modules wait for their sounds and songs), but is deterministic too, and
  the same whether its sound is played, captured or dropped
  (`docs/AUDIO.md` §3).

## Run a module headless

`adhostwin.exe <module> [NAME=VALUE ...]` runs one module without a window.
A relative path is resolved under the assets' `win` folder, so a catalog
`path` works as is:

```bash
H=build/win/host/core/adhostwin.exe
# 300 frames, one FBHASH line per frame on stderr, every frame as a PPM
ADFRAMES=300 ADGOWAITMS=0 ADFBHASH=1 ADOUT=C:/Temp/frames $H FILES/AD40/TOASTERS.AD
# a 16-bit module from After Dark 3.2, with its Speed control (index 1) at 50, and traces
$H packages/ad32/AD32/GUTS.AD ADFRAMES=120 ADGOWAITMS=0 ADCVSET=1=50 ADTRACE=lane,api16
# 15 s of Flying Toasters with its sound captured, not played: toasters.wav + toasters.mid
ADFRAMES=900 ADGOWAITMS=0 ADAUDIOOUT=C:/Temp/toasters.wav $H FILES/AD40/TOASTERS.AD
# a scratch import instead of the user's assets (--download-dir: its cover
# downloads too, which would otherwise go to the data folder's downloads\covers)
build/win/importer/adimport.exe --image source_iso/<image> --dest C:/Temp/adroot --download-dir C:/Temp/addl
AD_ASSETS_DIR=C:/Temp/adroot ADFRAMES=120 ADGOWAITMS=0 $H packages/tt/TWISTED/<MODULE>.AD
```

* Left to its defaults (no `AD_ASSETS_DIR`), a module run reads the assets
  from `%LOCALAPPDATA%\LongAfterDark`, as every Long After Dark program
  does (below). Set `AD_ASSETS_DIR`, or `AD_LOCALAPPDATA=<scratch>`, to keep
  a run away from the real data folder; `--test-pattern` and
  `--capabilities` never touch it.
* stderr ends with a `[census]` line counting unimplemented APIs; a module
  that runs clean reports 0.
* `ADSCREENW`/`ADSCREENH` set the screen (default 640×480), `ADSEED` the
  randomness, `ADCVSET=<i>=<v>,…` the controls (catalog `index`).
* `ADSTREAM=1` streams `P8` frames on stdout instead, for a front-end.
* Sound is off unless asked for. `ADAUDIOOUT=C:/Temp/x.wav` turns the
  module's sound on and captures it (the mix as a WAV, the MIDI messages as
  `C:/Temp/x.mid`) without playing anything; `ADSOUND=1` turns it on and, in
  a streamed run, plays it on the real device (a headless run never opens
  one). `ADVOLUME` (0–100, default 50) is After Dark's volume slider, and
  for an Intermission module both Intermission's own Volume (the effects)
  and the music's level. At the end stderr gets one `[audio]` summary line
  (`docs/AUDIO.md` §4).
* `adhostwin --test-pattern` drives the whole protocol without a module;
  `--capabilities` prints what the build has; `--configure <module>
  --button <slot>` runs a module's own button (`docs/INTERACTION.md` §6.1).
* `adwinspect.exe <file>` dumps a PE or NE file's headers, imports, exports
  and resources.

The full environment is in [host/core/README.md](../host/core/README.md) and
each lane's `lane.hh`.

## Where assets and state live

Everything the programs keep is under **`%LOCALAPPDATA%\LongAfterDark`**
(`AD_LOCALAPPDATA` stands in for `%LOCALAPPDATA%`;
`host/core/include/adw/core/data_root.h`).

```
%LOCALAPPDATA%\LongAfterDark\
  assets\                          the assets root (AD_ASSETS_DIR, adimport --dest)
    win\
      FILES\{AD40,CLASSIC,ENGINE,AFI}\   After Dark 4.0 Deluxe, as on the disc; its import.json beside FILES
      packages\<id>\               every other release: module folder(s), ENGINE\, import.json (swse also WINDOWS\)
      catalog-win.json             merged over every installed release (DESIGN §6a, PACKAGES §6)
      covers\<id>\                 each release's box cover (COVERS §2.5)
      import.lock                  one import at a time
  downloads\                       Internet Archive downloads (adimport --download-dir)
  settings.ini                     the saver's settings (AD_SETTINGS)
  state\<package>\                 what the modules write: INI files, message texts, scores (ADSTATE, INTERACTION §7)
  thumbs\                          the settings dialog's module thumbnails
  logs\saver-last.log              how the last /s run ended
```

Package ids are `deluxe` (its files stay in `FILES\`), `ad10`, `ad32`, `tt`,
`simpsons`, `swse` and `startrek`. `ADSTATE` is unset in headless runs, so
the state overlay lives in memory and no user state is read or written.
Build trees go to `build/` (gitignored); on Linux the player goes to
`build/linux/`.

On Linux the importer and the hosts run under Wine, so the data folder is
the Wine prefix's `%LOCALAPPDATA%\LongAfterDark`, and the Linux player
keeps the modules' state in `$XDG_DATA_HOME/longafterdark/state` instead
([LINUX.md](LINUX.md#your-files)).

## Conventions

* C++20, clang with libc++ and lld, Release builds, static linking.
* Two-space indent, `snake_case` functions, and comments that explain
  *why*.
* A component is a directory with its own `CMakeLists.txt`: a static library
  target, its executables, and tests under `tests/` registered with
  `add_test`. The top-level `CMakeLists.txt` lists the components and skips
  any that is absent; a lane registers itself with core as
  [host/core/README.md](../host/core/README.md) ("Writing a lane")
  describes.
* Facts about the original binaries are recorded with their evidence (an
  address in our own disassembly, or an observed run) in the design
  documents, and marked VERIFIED, EMPIRICAL or UNVERIFIED.
* CI (`.github/workflows/build.yml`) runs the same steps on a Windows
  runner: `tools/bootstrap.sh`, `tools/build.sh` with `-LE gui` (tests
  that need imported assets skip themselves), then `tools/package.sh`,
  and uploads `build/dist/LongAfterDark` as an artifact. Its release job
  zips that onto a GitHub Release: every push to `main` replaces the
  `latest-main` pre-release, and a `v<version>` tag makes the release of
  that version (the tag must match `project(VERSION)` in `CMakeLists.txt`;
  a `-suffix`, as in `v1.1.0-rc1`, makes it a pre-release).
  A second job, `build-linux`, does the same on an Ubuntu 24.04 runner (the
  cross build and its tests under Wine and Xvfb, without the twelve above),
  builds the player in an Ubuntu 22.04 container (`AD_WERROR=1
  AD_GLIBC_MAX=2.35`, so it runs on glibc 2.35 and newer), runs its unit
  tests and its smoke test, and makes the Linux zip. Neither the Windows
  build nor its release waits for it, and a Linux failure neither fails
  the run nor marks the commit as failed: every step of the job is
  `continue-on-error` with a timeout of its own (a job that reaches its
  own timeout is cancelled, which `continue-on-error` doesn't cover), so
  the job succeeds, a warning on the run and the job's summary name the
  step that failed, and that run makes no Linux zip. When it has passed,
  `release-linux` adds its zip to the release that the release job
  published (`LongAfterDark-<version>-linux-x64.zip` for a tag,
  `LongAfterDark-linux-x64.zip` on `latest-main`, where the release job
  first removes an earlier commit's, and only while `latest-main` is still
  at its commit), with a paragraph for it in the notes. Both jobs build
  with `SOURCE_DATE_EPOCH` set to the commit's time, so both zips hold the
  same three Windows programs, byte for byte.
