# Long After Dark — design

**Long After Dark** runs the **original Windows After Dark modules**, and
the modules of Delrina's Intermission screen saver engine, LucasArts' Star
Wars Screen Entertainment's among them, on Windows 11 by executing their
x86 code under emulation: it loads the *real*
engine and the module into one emulated address space, traps every call
they make into the operating system, and supplies that OS surface from the
host. A Windows screen saver (`LongAfterDark.scr`) presents the frames.
Twenty releases are supported (§7): After Dark 4.0 Deluxe, After Dark 10th
Anniversary, After Dark 3.2, Totally Twisted After Dark, The Simpsons
Screen Saver, Star Trek: The Screen Saver (After Dark 2.0b), Star Trek: The
Next Generation Screen Saver, Marvel Comics Screen Posters, The Looney Tunes
Screen Saver, The Disney Collection Screen Saver, two other companies'
modules for After Dark (Binary Software's ScreamSavers and Image Smith's
Snoopy's Screen Savers), Star Wars Screen Entertainment, Intermission 4.0,
the Opus 'n Bill Screen Saver, Opus 'n Bill: On the Road Again!, The
Flintstones Screen Saver Collection, The Far Side Screen Saver Collection,
Scott Adams' Dilbert Screen Saver Collection and Sierra On-Line's Screen
Antics: Johnny Castaway, 429 modules in all. The seven from Star Wars
Screen Entertainment to Dilbert are not After Dark releases: their modules
were written for Delrina's Intermission screen saver engine (Intermission
4.0 is the engine's own release), and speak its own protocol (ABI.md
§3.8). Johnny Castaway is
no module of any engine: it is a Windows 3.1 screen-saver program, a `.SCR`
built on Microsoft's `SCRNSAVE.LIB` with its own message loop, which the
host runs whole, as Windows 3.1 ran it, through a third protocol
(`PACKAGES.md` §7.6).

Nothing here is a reimplementation of After Dark. The engine DLLs
(`ADXPL510.DLL`, `ADXPL300.DLL`) and Berkeley's own classic-module bridge
(`OLDMOD16.DLL`) run as real code; the host provides only what sits
*beneath* them — KERNEL/USER/GDI/MMSYSTEM. After Dark 2.0's module library
(`AD_MOD.DLL`) and sound library (`AD_SND.DLL` 1.0) run as real code too,
under a host that stands in for its `AD.EXE` (ABI.md §3.9). One package,
Snoopy's Screen Savers, was made to run in an After Dark the user already
had and ships no sound library: for it alone the host supplies AD_SND, our
own code (§7). The same goes
for Intermission: its IMX reader (`IMIMXPLY.IMQ`), its library
(`INTRMLIB.DLL`, `ANTSW.DLL`), the modules' framework (`SWSE.DLL`) and the
modules run as real code, and the host stands in only for Intermission's
engine application, `INTERMIS.EXE`, as it stands in for After Dark's
`AFTERDAR.SCR`.

## The corpus

The work started from one release; the other nineteen came later (§7,
`PACKAGES.md`), and the same host runs them all. The first corpus, the PC
side of a hybrid Mac/PC CD,
`After Dark 4.0 Deluxe (1996)(Berkeley Systems)[Mac-PC].iso`
(md5 `d875a60338b73f44b7befa06bdd33aeb`), holds the Windows 95 release as plain
ISO-9660 files — no installer archive, no resource forks:

| Folder | Contents |
|---|---|
| `/ADE/FILES/AD40/` | 22 After Dark 4 modules (`*.AD`) — **PE32 i386 DLLs**, Borland C++ (TLINK32 2.25), each exporting `Module` (ordinal 1); `ADXPL510.DLL` (the AD 4 engine, 32-bit). The 23rd AD4 module is `ENGINE/STARRYNI.AD` (MSVC build, exports `_Module@4`). `PSYCHO.AD` and `STARRYNI.AD` do not use the engine |
| `/ADE/FILES/CLASSIC/` | 61 "Classic" modules (`*.AD`) — **16-bit NE DLLs** (AD 2.x/3.x); `ADXPL300.DLL` (16-bit AD 3 engine), helper DLLs (`AD_RSRC`, `ADTOOL`, `AD30RSDB`, `READ*`, `DJPG`, `DTARGA`) |
| `/ADE/FILES/ENGINE/` | `AFTERDAR.SCR`/`AFTERDAR.EXE` (the original Win32 host), `OLDMOD32.DLL`/`OLDMOD16.DLL` (Win9x flat-thunk bridge that let the 32-bit host drive 16-bit modules), `AD_SND.DLL`, `ADPAGE.DLL`, `STARRYNI.AD` |

Both engines describe themselves as "AfterDark Cross Platform Library" and
export the same C++ classes (`XCanvas`, `Background`, `ArtSprite`, …).

Facts about the binaries below that came from third-party notes (the
`anne-skydancer/afterdark-win11` repository, which carries **no open-source
license**) are *leads to verify by our own disassembly*, never code to copy:
AD4 entry `int __stdcall Module(AD_MODULE32 *block)` with a 348-byte block;
AD3 entry `int FAR PASCAL Module(int msg, HDC hdc, HANDLE hADSystem)`.

## Architecture

```
LongAfterDark.scr (x64)     ── spawns ──►  adhostwin.exe (x64)   one process per running module
  /s /p /c, settings, picker                 │
  presents P8 frames      ◄── stdout P8 ──── │  adw::core   screen surface (8-bit + palette), protocol, pacing
  input/controls          ─── stdin lines ─► │  adw::win32  KERNEL32/USER32/GDI32/WINMM shims  ─┐
                                             │  adw::win16  KERNEL/USER/GDI/MMSYSTEM shims      ├─► real Win32 GDI on
                                             │  adw::loader PE32 + NE images                    ┘   DIB sections over
                                             │  adw::cpu    x86 interpreter (flat32 + seg16)        emulated memory
                                             ▼
                          emulated: module.AD + ADXPL510.DLL   (32-bit lane)
                                    module.AD + ADXPL300.DLL + OLDMOD16.DLL + helpers (16-bit lane)
                                    module.AD + AD_MOD.DLL + AD_RSRC.DLL + AD_SND.DLL 1.0 (16-bit lane, After Dark 2.0)
                                    module.IMX + SWSE.DLL + INTRMLIB.DLL + IMIMXPLY.IMQ + helpers (16-bit lane, Intermission)
                                    SCRANTIC.SCR, a whole program (16-bit lane, a Windows 3.1 screen saver)
```

* **One CPU core for both lanes.** `adw::cpu` is resource_dasm's `X86Emulator`
  (MIT, © Martin Michelsen) vendored and extended with 16-bit protected-mode
  segmentation. Flat 32-bit mode is the default and stays as fast as upstream.
* **Real GDI, emulated memory.** The host runs on Windows, so drawing is done by
  the real GDI onto DIB sections. Emulated memory arenas are backed by Win32
  file mappings, so a DIB section can alias emulated memory exactly: module code
  writing pixels and GDI drawing see the same bytes with no copies.
* **An 8-bit palettized display.** The host presents the authentic Win95 display
  the modules were written for: 8 bpp, `RC_PALETTE`, 256-entry system palette
  with 20 static colours. Palette animation (`AnimatePalette`,
  `SetDIBColorTable`) changes the hardware palette; frames go out as `P8`.
* **One host protocol** (§1), which every front-end speaks: the saver, its
  previews and thumbnails, and the tests.

## Layout

```
(repository root)
  README.md               what it is, for users
  CMakeLists.txt          top level; adds each component below if present
  cmake/llvm-mingw.cmake  toolchain file (third_party/toolchains/llvm-mingw-*)
  tools/bootstrap.sh      toolchain + zlib + phosg → third_party/ (gitignored)
  tools/build.sh          configure + build + ctest → build/win
  tools/package.sh        Release build, staged in build/dist/LongAfterDark (§6)
  docs/INSTALL.md         installing and using it (for users)
  docs/BUILDING.md        how to build, test and run (start here)
  docs/DESIGN.md          this file
  docs/ABI.md             module/engine ABI as we verify it (our own findings)
  docs/API_SURFACE.md     every function the Deluxe disc's binaries import, counted and classified
  docs/PACKAGES.md        the twenty releases: registry, import, catalog merge, lane rules (§7)
  docs/INTERACTION.md     input, module buttons, per-user state, desktop seed (§8)
  docs/COVERS.md          the box-cover strip, the cover pipeline, the shared UI library (§9)
  docs/AUDIO.md           sound: census, engine, lane mappings, saver settings (§10)
  common/ui/              adw_ui       namespace adw::ui   (Windows 11 theming shared by the .scr and adimport)
  host/cpu/               adw_cpu      namespace adw::cpu
  host/loader/            adw_loader + adwinspect.exe   namespace adw::loader
  host/core/              adw_core + adhostwin.exe   namespace adw (core; the audio engine is adw::audio)
  host/win32/             adw_win32    namespace adw::win32  (Win32 guest runtime + shims)
  host/pe32/              adw_lane_pe32   namespace adw::pe32  (32-bit lane)
  host/win16/             adw_win16    namespace adw::win16  (Win16 guest runtime + shims)
  host/ne16/              adw_lane_ne16   namespace adw::ne16  (16-bit lane)
  importer/               adw_import + adimport.exe  namespace adw::import
  importer/gui/           adw_import_gui             namespace adw::import::gui (adimport --gui)
  scr/                    adw_scr + LongAfterDark.scr   namespace adw::scr
```

Toolchain: llvm-mingw (clang 23, libc++, lld), C++20, static linking, 8 MB
stack, Release by default. Each component has a `CMakeLists.txt`, a static
library target, and CTest tests under `<component>/tests/` (register them with
`add_test`). Code style: 2-space indent, `snake_case` functions, comments that
explain *why*.

## Contracts

### 1. Host process protocol

`adhostwin.exe <module-path> [key=value …]`

* **stdout** (binary mode): concatenated frames. `P8` = ASCII header
  `"P8\n<w> <h>\n"`, then 768 bytes of RGB palette, then `w*h` index bytes
  (top-down rows, no padding). `P6` (`"P6\n<w> <h>\n255\n"` + `w*h*3` RGB) is
  allowed as a fallback. Nothing else may be written to stdout; logs go to
  stderr.
* **stdin** (text lines, may be absent):
  `GO` (advance one frame in lockstep mode) · `SET <idx> <val>` (module control
  value) · `KEY <vk> <0|1>` (**Windows virtual-key code**, down/up) ·
  `CAPS <0|1>` · `NUMLOCK <0|1>` · `MOUSE <x> <y> <btn>`
  (frame-local) · `QUIT`.
* **env**: `ADSTREAM=1` (stream frames to stdout; otherwise headless) ·
  `ADSCREENW`/`ADSCREENH` (default 640×480) · `ADFRAMES=<n>` (stop after n
  frames) · `ADFBHASH=1` (one `FBHASH <frame> <hex64>` line per frame on stderr —
  FNV-1a 64 over palette+indices) · `ADOUT=<dir>` (headless: write frames as
  `frame_NNNNN.ppm`) · `ADCVSET=<i>=<v>,…` (initial control values) ·
  `ADSEED=<n>` (seed for every randomness source we own) · `ADNOPACE=1`
  (never sleep) · `AD_ASSETS_DIR` (asset root override) · `ADTRACE=<cats>`
  (comma-separated log categories to stderr). Sound: `ADSOUND`,
  `ADAUDIOOUT`, `ADVOLUME` and the rest (§10, AUDIO.md §4); interaction:
  `ADCAPS`, `ADNUMLOCK`, `ADSTATE`, `ADSTATUSHANDLE`, `ADSEEDIMG` (§8); the
  data folder: `AD_LOCALAPPDATA` (§6); each lane's own knobs: §5a and its
  `lane.hh`. The full list is in `host/core/README.md`.
* **Pacing**: in `ADSTREAM` mode the host advances one frame per `GO` when the
  front-end sends them, else at the module's own rate against a virtual clock;
  headless runs never sleep and are deterministic (virtual time advances by a
  fixed tick per frame), so `FBHASH` streams are comparable run to run.
* **Lifetime**: the host exits on stdin EOF-after-GO-seen, on `QUIT`, or when a
  stdout write fails. The front-end also places hosts in a Job object with
  `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`.
* **As implemented** (`host/core/README.md` is authoritative for detail):
  GO lockstep also works headless — after the first `GO`, each frame consumes
  input lines up to exactly one `GO`, so scripted input is deterministic. Before
  frame 0 the host waits up to `ADGOWAITMS` (default 250) for the front-end's
  opening lines through its first `GO`; **front-ends should send their first
  `GO` immediately at spawn** (a headless harness with a silent stdin pipe sets
  `ADGOWAITMS=0`). Extra host env: `ADPACEMS`, `ADSTREAMFORCE` (allow streaming
  to a disk file), `ADSTREAMP6`, `ADGOWAITMS`. `ADSTREAM` to a console, a disk
  file, or a stdout that shares stderr's handle (a shell's `2>&1`, which
  would interleave log lines with frames) exits 2. Env numbers are decimal
  or `0x` hex; flags are false for empty/`0`/`false`/`no`/`off`. `ADSEED`
  defaults to 1 (`ADSEED=random` logs the drawn seed). Command keywords are
  case-insensitive; trailing junk is rejected. `AD_ASSETS_DIR` is the root that
  contains `win\` (pointing at `win\` itself also works); a relative module path
  is resolved under `<assets>\win`. Exit codes: 0 ok, 1 error, 2 usage, 3 lane
  missing (`--configure`: also 4, the button showed nothing, and 5, the
  lane has no configure support; INTERACTION.md §6.1). Front-ends spawning
  several hosts at once must not let them inherit each other's pipe ends (`PROC_THREAD_ATTRIBUTE_HANDLE_LIST`), and must close
  both pipes to stop a host. Stream mode uses the realtime clock, so only
  headless runs are `FBHASH`-deterministic.

### 2. CPU (`adw::cpu`)

Vendored from `swannman/resource_dasm` branch `afterdark-perf`
(`src/Emulators/X86Emulator.*`, `EmulatorBase.*`, `MemoryContext.*`, plus what
they include), moved to `namespace adw::cpu`, depending only on phosg.
Extensions:

* `MemoryContext` arenas on Windows are allocated with
  `CreateFileMapping(INVALID_HANDLE_VALUE, …)` + `MapViewOfFile`, and expose
  `std::pair<HANDLE, uint32_t> section_for(uint32_t addr)` (mapping handle +
  byte offset) so GDI can build DIB sections over emulated memory.
* Segmentation: `struct SegDesc { uint32_t base, limit; bool present, code,
  readable_or_writable, big /*D/B*/; uint8_t dpl; }` and an abstract
  `DescriptorProvider { virtual bool lookup(uint16_t sel, SegDesc& out) = 0; }`
  supplied by the host (the LDT is host-owned). Segment registers
  CS/DS/ES/SS/FS/GS with descriptor caches; all memory accesses go through the
  segment base (and limit check); default operand/address size from CS.D;
  16-bit ModRM addressing; SS.B selects SP vs ESP; far `call`/`jmp`/`retf`
  (incl. `retf imm16`), `iret`, `int n`, `into`, `lds/les/lfs/lgs/lss`,
  `push/pop sreg`, `mov sreg`, `enter/leave` in both sizes, string ops with
  16-bit addressing + segment overrides, `jcxz/loop*` on CX, `pusha/popa` 16,
  `lar/lsl/verr/verw`, `arpl`. Faults (#GP/#NP/#SS/#UD/#DE) are reported to a
  host callback with the faulting CS:EIP rather than thrown blindly.
* Flat mode (Win32 lane): CS/DS/ES/SS are base-0 4 GiB big segments; FS is a
  base-only segment pointing at the emulated TEB. Performance in flat mode must
  not regress versus upstream (segment-base adds are skipped when base is 0).
* Traps: `int n` calls the host handler `(Emulator&, uint8_t vector)`; the
  handler may change any register including CS:EIP and SS:ESP. Import thunks
  in both lanes use this (see §5).
* **As implemented — the fault contract** (`X86Emulator.hh`): every fault
  (#DE, #BR, #UD for an invalid *or unimplemented* opcode, #NP, #SS, #GP,
  #PF for emulated memory with no arena, #MF for a pending unmasked x87
  exception) reaches the host's fault handler as a `Fault{vector,
  error_code, cs, eip, address, …}` with EIP and ESP already rolled back to
  the faulting instruction. The handler may fix the cause and return (the
  instruction is retried), redirect CS:EIP, `request_stop()`, or throw.
  With no handler a fault throws `fault_error`. The Win32 runtime turns
  faults into SEH exceptions through the guest's `FS:[0]` chain (an
  unhandled one ends the run as a guest error); the Win16 runtime ends the
  run (`GuestError16`), since Win16 had no SEH and a GP fault killed the
  task. `in`/`out` without a port handler is #GP, as for
  ring-3 code; `#DE` records whether the divisor was zero, since Windows
  reports that differently from an overflowing quotient.
* **FS in flat mode** is a data segment based at the emulated TEB with limit
  `0xFFF` (one page, `set_flat_mode(fs_base, fs_limit)`), so a stray
  `FS:[n]` past the TEB faults instead of reading unrelated memory.

### 3. Loader (`adw::loader`)

Pure parsing and placement; no Windows API knowledge. Loads into an abstract
sink so it does not depend on `adw::cpu`:

```cpp
struct ImageSink {
  virtual ~ImageSink() = default;
  virtual uint32_t reserve(uint32_t preferred, uint32_t size) = 0;   // returns actual base (≠ preferred ⇒ relocate)
  virtual void write(uint32_t addr, const void* data, size_t size) = 0;
};
```

* **PE32**: headers, sections (placed at image base or relocated with
  `.reloc`), imports (`dll`, `name` or `ordinal`, IAT slot address) bound
  through a resolver callback `uint32_t(const Import&)`, exports (name→RVA,
  ordinal→RVA, forwarders), resource directory tree (type/name/language by id
  or string → data RVA+size, plus `RT_STRING` / `RT_VERSION` helpers), TLS
  directory (reported, not run).
* **NE**: header, segment table (file offset/length/min-alloc/flags, incl.
  `DATA`/`PRELOAD`/`MOVEABLE`/`RELOCINFO`), per-segment relocation records (all
  address types: LOBYTE, SELECTOR, POINTER32 (16:16), OFFSET, POINTER48
  (16:32), OFFSET32; all target kinds: INTERNALREF (fixed seg:off or moveable
  entry ordinal), IMPORTORDINAL, IMPORTNAME, OSFIXUP (FIARQQ…FJSRQQ floating
  point fixups); ADDITIVE; source chains), entry table (fixed/moveable
  bundles, exported/shared-data flags), resident/non-resident names, module
  reference + imported-names tables, resource table (integer and string
  type/name ids — AD uses custom types such as `RLEP`, `RDAT`, `PAL`,
  `STRINGLIST`, `1000`, `2000`, `3000`). Relocations are applied through a
  resolver that maps a target to `(selector, offset)`.
* **As implemented**: types live in `adw::loader::pe` / `adw::loader::ne`;
  include `"loader/pe.hh"`, `"loader/ne.hh"`. NE loading is two-step —
  `ne::place()` (one `reserve` per segment, so the host can allocate selectors
  in between) then `ne::load_segments()` with a `FarPtr(const Target&)`
  resolver (moveable entry ordinals arrive already translated to seg:off).
  OSFIXUP records never reach the resolver: `LoadOptions::os_fixups = leave`
  (default) keeps the real x87 opcodes — **the lane uses this; the CPU executes
  x87 directly** — while `emulate` rewrites them to INT 34h–3Dh emulator calls
  (Microsoft FIxRQQ constants). Sinks need not zero-fill. The automatic data
  segment's reservation includes the NE heap+stack sizes (capped at 64K).
  Hostile table sizes are rejected as `LoaderError(malformed)`.
  `adwinspect.exe` dumps any PE/NE file (`--spec-dir research/win/spec` for
  Win16 ordinal names).

### 4. Core (`adw::core`, `adhostwin.exe`)

`Screen`: 8-bit framebuffer `w×h` + `std::array<RGBQUAD,256>` hardware palette
+ dirty flag; emits `P8`. `Protocol`: stdout writer (binary mode, full-frame
writes), stdin reader thread (line queue, non-blocking poll from the frame
loop), env parsing (§1), `FBHASH`, `ADOUT`. `VirtualClock`: deterministic ms
tick source every time API reads (§5a). `adhostwin.exe --test-pattern`
drives the whole protocol with a synthetic animated, palette-cycling image
(with `ADTESTAUDIO=1`, sound too), so front-ends can be built and tested
without a module. `adhostwin.exe --capabilities` prints one line naming
what the build has (`lanes=pe32,ne16 configure=pe32,ne16
abis=afterdark,intermission,scrnsave status=1 state=1 seed=1 audio=1 numlock=1`,
where `abis` lists the module ABIs its lanes run and `numlock=1` says it
takes the `NUMLOCK` line and `ADNUMLOCK`), which the settings dialog reads
instead of probing a module.

As implemented: `Protocol` is split into `StdinReader` + `StdoutSink` +
`parse_command`, with `run_host()` owning the loop, FBHASH and ADOUT. A lane is
`class Lane { bool init(const std::string& module, LaneContext&);
uint32_t frame_interval_us(); void on_command(const Command&); StepResult
step(); void shutdown(); }` with `LaneContext { const Env&; Screen&;
VirtualClock&; const InputState&; audio::Engine* audio; }` (the audio engine
`run_host` creates from the environment, §10). A lane may also implement
`status()` (the status record's fields, INTERACTION.md §3), and
`can_configure()` / `configure()` (the `--configure` driver, INTERACTION.md
§6.1); all have defaults. A lane that fails returns `false` from `init` or
`StepResult::failed`; an exception that escapes it is still contained by
`run_host` (logged, exit 1). `Screen::attach(top_row, stride)` (negative
stride = bottom-up DIB) points the screen at external memory — the lane attaches
it to the DIB section it creates over emulated memory. Shims read time only via
`VirtualClock::read_us()` / `read_tick_count()`. Lane components register
themselves with CMake properties (`ADW_LANE_PE32_TARGET` / `…NE16…`) and core
links them into `adhostwin` (see `host/core/README.md`).

### 5. Shims

Import binding writes, for every imported function, a tiny thunk in a host
code area: `int 0xFE` followed by a 16-bit thunk id. The trap handler looks the
id up (`"KERNEL32.DLL!GetTickCount"`, `"GDI.23"` …), reads arguments from the
emulated stack per convention (stdcall / cdecl / Pascal far), runs the C++
implementation, sets EAX (DX:AX for 16-bit), pops the arguments and returns to
the caller (near for Win32, far for Win16). Unimplemented imports log once and
return 0 — census tooling counts them (the `[census]` line at the end of
every run, and `Runtime::api_calls()`). Callbacks into emulated code (window
procs, enum procs, timers, DLL entry points) run a nested emulation loop
until the callee returns to a sentinel address.

The Win16 lane has its own shims (`host/win16`: KERNEL, USER, GDI,
MMSYSTEM, KEYBOARD, SHELL, COMMDLG, TOOLHELP, WIN87EM and a DOS/BIOS layer)
on real GDI, with 16-bit handles mapped to host objects. The two runtimes
share the Win32 runtime's display model (`win32::Display` and its logical
palettes: the 8-bit screen, the system palette, the desktop seed), file
system (`Vfs`), profile store (`IniStore`), guest heap arena (`GuestHeap`)
and configure-mode test hooks (`ConfigScript`); neither is thunked onto the
other.

As implemented, the points that decide whether the original binaries run
(ABI.md has the evidence):

* **DLL initialization.** pe32 delivers `DLL_PROCESS_ATTACH` exactly once
  per mapped image, engine first: the Borland RTL's per-image counter fails
  a second attach (ABI.md §2.1), so a reload maps a fresh image. ne16 runs
  each NE DLL's LibEntry and then, for a DLL marked Windows 4.0 that exports
  `DLLENTRYPOINT` (on these discs only `OLDMOD16`, which has no LibEntry at
  all), that export with reason 1, as Windows 95's KERNEL did, and with
  reason 0 before unloading it; without it OLDMOD16's two blocks are never
  allocated (ABI.md §3.2).
* **Win16 names are case-insensitive.** Imports by name, `GetProcAddress`
  and `GetModuleHandle` compare without case: Einstein imports AD_SND's
  entries in mixed case, and OLDMOD16's own `LoadAdSnd` asks for them that
  way (ABI.md §3.6).
* **The guest's disk** is a virtual file system (`win32/vfs.hh`, both
  lanes): the module's own folder as `C:\AFTERDRK` (the current directory)
  and `C:\WINDOWS`, both copy-on-write overlays whose writes go to the
  per-user state or to memory (INTERACTION.md §7); the engine dir as
  `C:\WINDOWS\SYSTEM` (ne16); `C:\PICTURES`, the module dir's `PICTURES`
  folder read-only (pe32: Art Critic's sample pictures); and `H:\<drive>\…`,
  the host's drives read-only, for paths picked in configure mode. An
  Intermission module (ne16) finds its folder as `C:\SAVER` instead, where
  its installer put it, and its package's `WINDOWS` folder (what the
  installer put in `C:\WINDOWS`: `SWSE.INI`) is the read-only lower layer of
  the guest's `C:\WINDOWS`, under the state overlay (PACKAGES.md §7.5). An
  After Dark 2.0 module (ne16) finds its folder as `C:\AFTERDRK`, as any
  After Dark module does; its `AD_PREFS.INI` settings are profile seeds
  (PACKAGES.md §7.3), and so are the keys After Dark 3.x's host wrote there
  for an After Dark 3.x package.
* **A selector freed under its caller.** A call (or software interrupt)
  that frees a selector the caller still holds in DS, ES, FS or GS returns
  with that register null, as Windows 3.1's `GlobalFree` did for DS and
  DPMI 1.0 and Wine's relay do for any of them; a later reload of the
  register from the stack then loads null instead of faulting. It fixes no
  load, and it is one check as every API call and interrupt returns
  (`host/win16/README.md`, ABI.md §3.14): Marvel's decoder and Snoopy's
  modules need it.

### 5a. Time and pacing

The originals ran on a 1990s PC, as fast as it allowed, and paced
themselves in three ways: by the clock (`timeGetTime`, `GetTickCount`), by
counting their own calls, or by drawing for as long as a call took. The
host reproduces all three against virtual time, deterministically headless
(each lane's `lane.hh` has the details and every knob):

* **One clock.** `VirtualClock` is the only time source the shims read.
  Headless it is fixed-step (frame × period), so `FBHASH` streams repeat;
  streamed it follows the wall clock, each gap between two observations
  capped at 250 ms. Each read may nudge it by a small step, which the lanes
  set (`ADREADSTEPUS`, default 5 µs), so a busy-wait on the clock inside one
  call ends deterministically. Win16's `GetTickCount` advances in 55 ms
  steps as on Windows 95 (`ADTICKMS`).
* **Both lanes present 60 frames per second** of virtual time
  (`ADPACEMS` overrides).
* **A presented frame is a run of DRAWFRAME calls**, because some modules
  count calls instead of reading the clock (Psycho Deli builds its next
  pattern 80 `SetPixel`s per call; ad32's Logo moves every n-th call). The
  lane calls DRAWFRAME back to back until a virtual machine has used up one
  frame period, at most `ADMAXDRAWS` (64) times. pe32 models a 100-MIPS
  P5 (`ADMIPS`), charging instructions, `ADAPICOST` (500) per API call and
  `ADPIXCOST` (4, about 25 Mpixel/s, a 1996 PCI card) per pixel a blit or
  fill writes, counted at 480 lines so the saver's Scale changes sharpness,
  not speed; work a call does beyond its frame's share is carried into the
  next frames (at most 30 frames' worth). ne16 models a 486-class 25-MIPS
  machine (`ADDRAWMIPS`), charging instructions, `ADAPICOST` per call and
  per pixel `ADPIXCOST` (2) for an After Dark module, `ADNE16IMXPIXCOST` (4)
  for an Intermission one (SWSE's GDI DIB stretches, the slow path of
  1994); an After Dark module makes at least one call a frame, whatever it
  costs. An Intermission module's completed calls carry what they did
  beyond their frame's share into the next frames instead, a frame whose
  whole share goes to it making no call (at most five in a row), so a
  module that steps once per call keeps the model's pace — Death Star
  Trench 16 passes per 60 frames, not 60 (`ADNE16IMXCARRY=0` turns that
  off). `ADMIPS=0` restores one DRAWFRAME per frame; `ADTRACE=pace` logs
  each frame's calls, work and account.
* **Long calls (both lanes).** Some DRAWFRAMEs draw for seconds (Satori,
  Einstein, Tunnel, Psycho Deli's first pattern, most modules' first call).
  They run on a fiber: at the first API call past the frame's share (and,
  streamed, past 90% of the frame's wall time in pe32) the frame is
  presented, and the next step resumes the call where it stopped, so the
  drawing shows as it happened on a 1996 monitor. Queued input reaches the
  module between calls, never inside a suspended one.
  `ADNE16LONGCALLS=0` / `ADPE32LONGCALLS=0` turn this off.
* **Hung calls.** One call into the guest may run `ADCALLBUDGET`
  instructions (default 10⁹) before the lane counts it as hung and fails
  the run.

### 6a. Catalog and settings

`<assets>\win\catalog-win.json` — generated from the module binaries by
`adimport` without running them: rewritten after every import and removal,
and by `adimport --catalog-only` alone (`importer/README.md`,
"Catalog"). Front-ends read only this. One entry, abridged (the real
Flying Toasters! of the Deluxe disc):

```json
{ "version": 1, "generator": "adimport 1.3",
  "modules": [
    { "id": "ad40.toasters", "displayName": "Flying Toasters!", "lane": "pe32",
      "path": "FILES/AD40/TOASTERS.AD", "entry": "Module", "needs": ["ADXPL510.DLL"],
      "controls": [
        { "index": 0, "name": "Objects:", "kind": "stringslider", "type": "slider",
          "items": ["Flight","Squadron","Air Wing","Swarm"], "values": [0,25,50,75],
          "default": 25, "defaultStop": 1 },
        { "index": 1, "name": "Toasters:", "kind": "popup", "type": "popup",
          "items": ["Adults","Babies","Random"], "default": 0 },
        { "index": 2, "name": "Music:", "kind": "stringslider", "type": "slider",
          "items": ["Never","1 Min.","2 Mins.","5 Mins.","30 Mins.","Always"],
          "values": [0,16,32,48,64,80], "default": 80, "defaultStop": 5 },
        { "index": 3, "name": "Display Karaoke", "kind": "checkbox", "type": "checkbox", "default": 1 } ],
      "about": "Flying Toasters!\n\nToaster meets toaster, …",
      "package": "deluxe", "packageTitle": "After Dark 4.0 Deluxe",
      "moduleName": "Flying Toasters!", "md5": "…" } ],
  "packages": [ … ] }
```

`lane` is `pe32` or `ne16`; `path` is relative to `<assets>\win`; control
`index` is what `SET <idx> <val>` / `ADCVSET` address. A control's `type`
(`slider`, `popup`, `checkbox` or `button`) is what a front-end draws;
`kind` refines it (`numslider` or `stringslider` for a slider). A string
slider's value is the chosen stop's entry in `values`, and `defaultStop` is
the stop of `default`. Buttons carry no value: they run the module's own
dialog (§8). A control with `host` (since the twenty releases, Intermission
4.0's **Speed:**) is the front-end's, not the module's: its value goes to
the host in that environment variable (`ADNE16IMXSPEED`, the emulated PC's
pace in percent), never as `SET` or `ADCVSET` (INTERACTION.md §6.7).

`abi` (optional, since `adimport 1.3`) names a module ABI other than After
Dark's: `"intermission"` for Star Wars Screen Entertainment's IMX modules,
written last and only on their entries. Absent, the entry is an After Dark
module, so catalogs of the five After Dark releases are laid out as before.
An Intermission entry is `ne16` (the file is NE) with `entry` `SAVERDRAW`,
an empty `about`, and one control, `{"index": 0, "name": "Configure...",
"kind": "button", "type": "button"}`: the module's own settings dialog.
Its `moduleName` comes from the importer's registry, since no resource in
the file holds it (PACKAGES.md §6). An ASA animation and an IMQ module
(The Far Side's, Dilbert's and the other Delrina releases'), and since the
twenty releases an FLI animation, a morph (`.MRF`) and a MultiSaver group
(`.MSV`), Intermission 4.0's, are Intermission entries too, lane `ne16`
(the probe sends them there by their header or extension), with `entry`
`SAVERMAIN`, their reader's, and the same one button, which opens their
reader's dialog or the IMQ module's own. Since the seventh round `abi` may also
be `"scrnsave"`: a Windows 3.1 screen-saver program (Johnny Castaway's
`SCRANTIC.SCR`), lane `ne16`, `entry` `SCREENSAVERPROC` (the export that
makes it one), `screen` `"640x480"`, and one button, **Setup...**, the
program's own settings dialog (PACKAGES.md §6).

`screen` (optional, since the seventh release, under the same `adimport
1.3`) is a fixed screen, `"WxH"`, that the module gets whatever the display:
`"640x480"` on every Star Trek: The Screen Saver entry, written last
(several of its modules compose a fixed 640×480 scene, and all 16 get that
screen so the release looks as it did at 640×480), and with the twelve
releases on every ScreamSavers entry and on Marvel Comics Screen Posters'
one (five ScreamSavers modules compose a fixed 640×480 scene, and all 15
get it; Marvel's posters are 640×480 pictures); on no other. A front-end gives such a module that screen whatever the
Resolution setting, scaled to fit, as it gives an Intermission module its
640×480 by its ABI; the catalog's screen comes first. Absent, or not of
that form, the module has no screen of its own (PACKAGES.md §6).

The catalog is **merged over every imported package** (§7). Ids are stable
keys for `settings.ini`: the Deluxe disc keeps `ad40.<base>` / `classic.<base>`
(unchanged), and every other package uses `<package>.<base>` (`ad32.toilet`,
`tt.toilet`, `ad10.toasters`), where `<base>` is the lower-case file stem. The
lane comes from the file header, never from the folder: one folder can hold
both kinds (`packages/ad10/AD10TH`). The same module in several packages is
**listed once per package**; `displayName` stays unique within a lane by
appending ` (<package short title>)` to every later duplicate name, so a
front-end that groups by `lane` and shows `displayName` still works.
Optional fields that front-ends may use and must otherwise ignore:
`package`, `packageTitle`, `moduleName` (the name before disambiguation),
`md5`, `sameAs` (the id of the first listed module with identical bytes),
and a top-level `packages` array. Rules: `PACKAGES.md` §6. The settings
dialog groups its list by `package` and shows `moduleName` (COVERS.md
§1.7).

Each `packages[]` entry also carries `cover` (COVERS.md §2.7):

```json
"cover": { "origin": "download", "tile": "covers/simpsons/tile.png", "tileMd5": "…",
           "image": "covers/simpsons/original.png", "width": 600, "height": 776,
           "art": "box", "label": "Box front", "credit": "Wikisimpsons", "original": "download" }
```

* `origin` is `user`, `download`, `disc` or `generated`. A generated cover
  is just `{ "origin": "generated" }`, which front-ends draw themselves.
* `tile` is a 640×800 PNG, relative to `<assets>\win`.
* `tileMd5` changes whenever the tile does.
* The catalog `version` stays 1. The generator is `adimport 1.3` since the
  sixth release (`abi`, and with the seventh `screen`), `adimport 1.2`
  before it (the covers).

`%LOCALAPPDATA%\LongAfterDark\settings.ini` — front-end settings (UTF-8 INI):
`[Saver] Module=<id>|random`, `Randomize=<id>,<id>,…`,
`RandomizeSaved=<id>,…|-` (the Random checklist kept while a single module is
chosen, `-` = nothing checked; the saver ignores it), `DurationMin=<n>|0` (0 = forever),
`Scale=1.0|1.5` (the Resolution setting, 480 or 720 lines, for the other
modules; an Intermission module, and one whose catalog entry gives a
`screen` — Star Trek, ScreamSavers, Marvel, Johnny Castaway —, always gets 640×480, scaled
to fit the monitor in its 4:3 shape: `scr/README.md`), `Monitors=all|primary`,
`DifferentPerMonitor=1|0` (default 0: a rotation plays the same module on
every monitor and switches them together; 1: each monitor has a rotation of
its own, the settings window's "A different module on each monitor"),
`StretchToFit=1|0` (default 0: a 640×480 module keeps its 4:3 shape with
bars; 1: its frame fills each monitor, the settings window's "Stretch to fit
the screen"; never in `/p`), `StartFromDesktop=1|0` (no UI;
INTERACTION.md §8), `Collections=<package id>,…` (the box-strip filter;
empty or missing = every release), `Sound=1|0` (default 1), `Volume=0..100`
(default 50), `SoundMonitor=primary` (reserved; §10, AUDIO.md §9);
`[Module.<id>] <index>=<value>` per control. Saving edits the file in
place, keeping keys and sections it does not know.

The saver rotates when `Module=random` (or there is no `Module`) or when
`Randomize` lists modules; a named `Module` in front of a list plays first,
then the list rotates. In Random mode the saver plays `Randomize` (or every
module), limited to the releases in `Collections`, with byte-identical
copies (`sameAs`) played once per pass (COVERS.md §1.8).

Each `/s` window presents the host's frames through Direct2D: a sharp GPU
upscale (nearest neighbour to the next whole multiple, then linear), about
1.7 ms a frame at 4K. GDI is the fallback (no render target, repeated
device loss, or Direct2D averaging over 8 ms) and the `/p` preview's path:
it stretches with `HALFTONE` and switches to `COLORONCOLOR` when a host's
`HALFTONE` upscales average more than 8 ms (judged afresh for each host).
`AD_SCR_PRESENT=gdi|d2d` and `AD_SCR_STRETCH=halftone|nearest` force one
(`scr/README.md`).

### 6. The data folder and the assets

Everything per user lives in one folder, `%LOCALAPPDATA%\LongAfterDark`:
`assets\` (below), `downloads\`, `state\` (INTERACTION.md §7), `settings.ini`
(§6a), `thumbs\` and `logs\`. The three programs (adhostwin,
`LongAfterDark.scr` and `adimport.exe`) derive it with the same header-only
helper (`host/core/include/adw/core/data_root.h`) from the same base
(`AD_LOCALAPPDATA`, else `LOCALAPPDATA`, else the known folder), so they
always agree on it. A run with explicit locations (`AD_ASSETS_DIR`,
`AD_SETTINGS`, `--dest`, …) never touches the data folder. The details are
in `host/core/README.md`, "The data folder".

The assets are `%LOCALAPPDATA%\LongAfterDark\assets\win\` (override:
`AD_ASSETS_DIR`, the root that contains `win\`):

```
FILES\{AD40,CLASSIC,ENGINE,AFI}\…   After Dark 4.0 Deluxe: layout unchanged, 8.3 upper-case names
import.json                          Deluxe's import record (version 1, unchanged)
packages\<id>\<MODDIR>\…              every other package (§7): its modules and what sits beside them
packages\<id>\ENGINE\…                that package's engine support files (never a module folder)
packages\<id>\WINDOWS\…               optional: what its installer put in C:\WINDOWS (Star Wars Screen Entertainment's SWSE.INI)
                                      (Snoopy's Screen Savers has no ENGINE: it shipped only modules)
packages\<id>\import.json             that package's import record (version 2)
covers\<id>\{original,user,tile}.png  each release's cover (§9, COVERS.md §2.5); cover.json beside them
catalog-win.json                      merged over every installed package (§6a)
import.lock                           one import (or catalog rewrite) at a time
```

Produced by `adimport.exe` from any known package source: an ISO image, a
floppy image (or several: every disk of a set), a ZIP of the install
files, a mounted disc or folder, or a download of the release's Internet
Archive copy, and a ZIP of floppy images (as the Internet Archive serves
a release's disks together), or a ZIP or folder that keeps a release's
disks in `DISK<n>` folders (read as their union). Inside a source, the
installers' own archives are read too: encrypted PKZIP (After Dark 3.x),
multi-volume ARJ and COMPRESS'd SZDD files (Star Wars Screen
Entertainment), Microsoft Setup's KWAJ-compressed files (Star Trek: The
Screen Saver), and InstallShield 2's compressed libraries (Marvel Comics
Screen Posters, Snoopy's Screen Savers). Each package is verified against
its own manifest and image md5s (for a release on several floppies, every
disk's; for a release known by the ZIP of its install files, that ZIP's).
The host and the importer take `<root>\win` when
it holds `FILES`, `packages` or `catalog-win.json`; otherwise they take
`<root>` itself when that holds one of them; otherwise `<root>\win`. The
front-end's check, which looks only at `catalog-win.json`, agrees. So an
install that holds only non-Deluxe packages (no `FILES`) still resolves.
**No file of any supported release is ever committed**;
reverse-engineering dumps go under `research/` (gitignored).

Each `import.json` records the source (kind, path or URL, image size and
md5), how the package was verified (`image`, `files`, `partial` or `none`)
and every file's path, size and md5, never file contents and never the
AD 3.x archive password (PACKAGES.md §5.3, `importer/README.md`).

**The programs.** `tools/package.sh` builds every component in
Release and stages `build/dist/LongAfterDark/` (`AD_DIST_DIR` overrides it;
the folder is replaced whole, so the saver must not be running):

```
LongAfterDark.scr   the screen saver and its settings window; finds the other two beside itself
LongAfterDark.exe   the same file, the name its window mode's switches reach it under (scr/README.md)
adhostwin.exe       the host (one per monitor, preview, thumbnail or module button)
adimport.exe        the importer (command line and --gui)
README.txt          for users: import, covers, install, settings, where the files are
LICENSE.txt         this project's licence
licenses\           NOTICE.txt (which is whose) and the licence texts of resource_dasm, phosg, zlib, LLVM and the mingw-w64 runtime
```

All three programs are static x64 executables with no DLLs beside them.
Installing is Windows' own: right-click → Install uses the `.scr` where it
is, and for every user the three programs are copied together to
`C:\Windows\System32` (`INSTALL.md`).

### 7. Packages

The host runs modules from twenty releases: ten of Berkeley Systems'
After Dark, two of other companies' modules for After Dark (ScreamSavers,
Snoopy's Screen Savers), seven for Delrina's Intermission (Intermission
4.0 itself, LucasArts' Star Wars Screen Entertainment, and Delrina's own
Opus 'n Bill Screen Saver, Opus 'n Bill: On the Road Again!, The
Flintstones Screen Saver Collection, The Far Side Screen Saver Collection
and Scott Adams' Dilbert Screen Saver Collection), and Sierra On-Line's
Screen Antics: Johnny Castaway, a Windows 3.1 screen-saver program.
The full specification is `docs/PACKAGES.md`: registry,
identification, extraction formats, per-package layouts, the catalog merge,
the lane contract, and the three work packages that implement it. The
contract in brief:

* **A package** is one release in the importer's built-in registry: an id
  (`deluxe`, `ad10`, `ad32`, `tt`, `simpsons`, `swse`, `startrek`,
  `marvel`, `snoopy`, `looney`, `screams`, `disney`), a title,
  identification data (image md5s, volume id, file fingerprints) and an
  extraction recipe. Only the Windows half of each disc is read; the Mac
  half of a hybrid disc is skipped (`PACKAGES.md` §12).

  | id | Release | Medium | Recipe | Modules |
  |---|---|---|---|---|
  | `deluxe` | After Dark 4.0 Deluxe (1996) | hybrid CD, plain ISO-9660 files | `tree` → `FILES\` | 84 (23 pe32 + 61 ne16) |
  | `ad10` | After Dark 10th Anniversary (1999) | hybrid CD, ISO-9660 + Joliet, plain files | `tree` → `packages\ad10\` | 46 (17 pe32 + 29 ne16) |
  | `ad32` | After Dark 3.2 for Windows (1995) | hybrid CD, InstallShield 3 + encrypted PKZIP | `ad3zip` → `packages\ad32\` | 44 ne16 |
  | `tt` | Totally Twisted After Dark (1995) | hybrid CD, InstallShield 3 + encrypted PKZIP | `ad3zip` → `packages\tt\` | 13 ne16 |
  | `simpsons` | The Simpsons Screen Saver (1994) | two floppies merged into one FAT12 image, InstallShield 2 + encrypted PKZIP | `ad3zip` → `packages\simpsons\` | 15 ne16 |
  | `swse` | Star Wars Screen Entertainment (LucasArts, 1994; not After Dark) | plain ISO-9660 CD copy of five install floppies, Presage installer: multi-volume ARJ + SZDD | `intermission` → `packages\swse\` | 14 ne16 (Intermission IMX) |
  | `startrek` | Star Trek: The Screen Saver (1992; After Dark 2.0b) | two 1.44 MB floppies (FAT12), Microsoft Setup 2.0: KWAJ-compressed files | `ad2kwaj` → `packages\startrek\` | 16 ne16 |
  | `marvel` | Marvel Comics Screen Posters (1993; After Dark 2.0d) | two floppies, InstallShield 2: compressed libraries, one split over both (known by the ZIP of their files) | `islib` → `packages\marvel\` | 1 ne16 |
  | `snoopy` | Snoopy's Screen Savers (Image Smith, 1994; modules for an installed After Dark) | two floppies, InstallShield 2: one library split over both (known by the ZIP of their files) | `islib` → `packages\snoopy\` | 8 ne16 |
  | `looney` | The Looney Tunes Screen Saver (1995) | two floppies or their CD copy, InstallShield 3 + encrypted PKZIP (known by the ZIP of their files, and the CD) | `ad3zip` → `packages\looney\` | 12 ne16 |
  | `screams` | ScreamSavers (Binary Software, 1995; on After Dark 3.0.6) | three floppies, InstallShield 3 + encrypted PKZIP (known by the ZIP of their files, in `DISK1`–`DISK3` folders) | `ad3zip` → `packages\screams\` | 15 ne16 |
  | `disney` | The Disney Collection Screen Saver (1995) | three floppies, InstallShield 3 + encrypted PKZIP (known by the ZIP of their files) | `ad3zip` → `packages\disney\` | 16 ne16 |

* **Every package owns exactly one directory**, its *package root*: `FILES`
  for Deluxe and `packages\<id>` for the others. Importing a package stages
  that directory and swaps only it, so importing or re-importing one package
  never touches another. The catalog is rewritten over all installed
  packages after every import.
* **Self-contained packages.** Each package root holds everything its
  modules need: module folders with the helper DLLs and data files beside the
  modules, plus an `ENGINE` folder with the AD_SND build the package shipped
  and its host-side files (for Star Wars Screen Entertainment, Intermission's
  reader), and, where the installer put files in `C:\WINDOWS`, a `WINDOWS`
  folder. (Snoopy's Screen Savers shipped no engine and has no `ENGINE`:
  the host supplies its AD_SND, below.) **A module inside a package never resolves a file
  from another package**, so a module's output does not depend on which other
  discs are imported, and a user who owns only one disc can run it.
* **How a lane finds a module's package** (no environment variable, no
  descriptor file): module dir = the folder of the module file; package root
  = the parent of the module dir; engine dir = `<package root>\ENGINE`. The
  module is *packaged* when the package root's parent directory is named
  `packages`. Packaged modules search the module dir, then the engine dir,
  and nothing else. Anything else (Deluxe's `FILES\…`, or a lone module
  outside the assets tree) keeps exactly today's search order.
* **16-bit host for AD 3.x packages.** OLDMOD16 was Berkeley's own bridge by
  which AD 4 ran AD 3.x modules, so running AD 3.x modules under it is
  faithful. But AD 3.2, Totally Twisted, The Simpsons and the later AD 3.x
  collections (the Looney Tunes, ScreamSavers, the Disney Collection) ship
  no OLDMOD16: their host was `ADW30.EXE` + `ADTASK.DLL`. The ne16 lane therefore picks
  its bridge per package. It runs the real `OLDMOD16.DLL` when the engine dir
  holds one (Deluxe, `ad10`). Otherwise it uses a **host-native AD3 bridge**:
  a C++ implementation of the OLDMOD16 behaviour verified in ABI.md §3.3/§3.4.
  It builds `AD_SYSTEM` (version 300, "BUTTHEAD") and `AD_MODULE` exactly as
  OLDMOD16 does, sends the same message sequence, and uses the package's own
  `AD_SND.DLL` without OLDMOD16's `VerStr >= 400` gate. It takes the four AD
  palettes from the package's `ADTASK.DLL` (resources 5000/1..4). These are
  byte-identical in all three AD 3.x packages to `AFTERDAR.SCR`'s
  `AD_PALETTE` 102/104/101/103. This bridge is the one exception to "run
  Berkeley's code". It sits on the host side of the module protocol, the side
  our host already replaces (as it replaces `AFTERDAR.SCR` and `OLDMOD32`).
  Every engine (`ADXPL300/310/40`) and module still runs as real code. It was
  chosen over driving the real `ADTASK.DLL` because ADTASK's exported ABI is
  unverified and MFC-heavy, while OLDMOD16's side of the protocol is fully
  verified. In the surveys every AD 3.2, Totally Twisted and Simpsons module
  loaded under OLDMOD16's `AD_SYSTEM`. The modules that failed did so for
  unrelated reasons (INI seeds, desktop icons).
* **Intermission modules (Star Wars Screen Entertainment).** Its 14
  modules are 16-bit NE DLLs too, so they go to the ne16 lane, which tells
  them from After Dark modules by their exports (`SAVERINIT` and
  `SAVERDRAW`, where an After Dark module exports `MODULE`) and drives them
  with a second module protocol beside the After Dark one. It does what
  Intermission's engine `INTERMIS.EXE` did (an NE application, with its own
  message loop and control panel, which the Win16 runtime cannot run and the
  host replaces): it calls Intermission's own IMX reader, `IMIMXPLY.IMQ`,
  as real code, the way it calls OLDMOD16, and the reader loads and drives
  the module. A native reader, IMIMXPLY's dispatch in C++, is the oracle and
  the fallback (`ADNE16READER`). The lane never branches on the package id:
  the kind comes from the exports, the `WINDOWS` lower layer of `C:\WINDOWS`
  from that folder existing, and the profile seeds that steer `SWSE.DLL` to
  its GDI drawing path from that DLL sitting beside the module
  (`PACKAGES.md` §7.5; the protocol is ABI.md §3.8).
* **After Dark 2.0 (Star Trek: The Screen Saver).** Its 16 modules are
  After Dark modules (they export `MODULE`), which After Dark 2.0's
  `AD.EXE` drove with the same messages and blocks as OLDMOD16 (ABI.md
  §3.9); the disks ship no OLDMOD16, so the native AD3 bridge runs them,
  over the package's own AD_SND 1.0, whose volume pair it also takes. The
  lane's other rules for them are rules by file, keyed on `AD_MOD.DLL` in
  the module folder: `AD_PREFS.INI` profile seeds (the After Dark
  directory, and Windows' multimedia sound driver in place of the PC
  speaker's, which would hang the emulator) and DRAWFRAME's result 5 taken
  as the module's wake (how Final Exam ends). Its four AD palettes, which
  `AD.EXE` built in code when a module asked, are computed the same way
  (none of its modules asks). Every catalog entry carries `"screen":
  "640x480"` (§6a). (`PACKAGES.md` §7.3, §7.4.)
* **The five later releases.** Marvel Comics Screen Posters and Snoopy's
  Screen Savers came as InstallShield 2 libraries, which the importer reads
  with a strict reader of its own and places by a table baked from their
  installers' scripts (the `islib` recipe); the Looney Tunes, ScreamSavers
  and the Disney Collection use the AD 3.x installer (`ad3zip`), whose
  identification now wants each package's own folder file too, since
  ScreamSavers ships 3.2's engine DLL. All run through the native bridge,
  with rules by file again: the keys After Dark 3.x's `ADW30.EXE` wrote into
  `AD_PREFS.INI` at every start are profile seeds for a package whose
  engine dir holds it (the Disney Collection's library needs them); a
  package whose engine dir holds no `AD_SND.DLL` (Snoopy's, modules made for
  an After Dark already installed) gets the host's own AD_SND, our code
  with AD_SND 3.0.3's entries, byte-identical in frames and sound to After
  Dark 3.2's real one with instruction timing off; and one whose engine dir
  holds neither `ADTASK.DLL` nor `AFTERDAR.SCR` gets After Dark 2.0's four
  palettes, generated in code as `AD.EXE` 2.0 generated them, never copied
  from Berkeley's files, handed over at the first palette request (Snoopy's
  Collage asks for one). A package that ships its own files runs exactly as
  before. (`PACKAGES.md` §3, §4.3, §7.3, §7.4.)
* **Delrina's own releases (The Far Side, Dilbert, the two Opus 'n Bill
  releases, the Flintstones, Intermission 4.0).** Their floppies were
  installed by Delrina's own installer, which copied files by name, most
  of them SZDD-compressed; the importer does the same from a baked table
  (the `intermission` recipe's second installer), naming each release by
  file names alone. Their modules come in more forms than Star Wars': ASA
  animations (data, played by Intermission's ASA reader, `IMASAPLY.IMQ`),
  IMQ modules (each its own reader) and IMX modules, and Intermission
  4.0's FLI animations, morph and MultiSaver group, each played by its own
  Intermission reader. Every reader runs as real code from the package's
  `ENGINE`, the lane picking it by the module's form. Two releases came in
  two builds (the Opus 'n Bill Screen Saver's of September and November
  1993, the Flintstones' of June and May 1994), each with its own known
  images, table and manifest; the Flintstones' June build is online only
  inside a tar, from which the download takes the three ZIPs of its disks.
  (`PACKAGES.md` §2, §3, §4.3, §5.2, §6, §7.5.)
* **A Windows 3.1 screen-saver program (Johnny Castaway).** Sierra
  On-Line's Screen Antics: Johnny Castaway came on one floppy, its program
  and data compressed file by file by InstallShield 1, which the importer
  expands with a strict reader of its own (the `is1` recipe). The program,
  `SCRANTIC.SCR`, is no module: it is an NE application built on
  Microsoft's `SCRNSAVE.LIB`, with its own `WinMain`, window and message
  loop. The ne16 lane tells it apart by its export `SCREENSAVERPROC` and
  runs it whole, from its own entry point with `/s`, as Windows 3.1 did,
  through a third protocol beside After Dark's and Intermission's; its
  **Setup...** button starts it with `/c`, its own settings dialog. It
  paints a fixed 640×480 scene, so its catalog entry carries that `screen`.
  (`PACKAGES.md` §3, §4.3, §7.6, §8.11.)
* **Status: implemented.** All twenty releases import (from a disc, an
  image, a ZIP or a 7z, a folder or the Internet Archive) into
  self-contained package roots (429 catalog modules, 73 of them `sameAs` an
  earlier one). The 283 modules of the twelve After Dark releases run
  headless and deterministically, each release on its own; the 145 of the
  seven Intermission releases run through the Intermission protocol above,
  and Johnny Castaway through the screen-saver protocol. The
  survey-time numbers (before this work, with every AD 3.x module leaning on
  Deluxe's `ENGINE` files, and before each later release) are in
  `PACKAGES.md` §1.

### 8. Interaction

The full contract is `docs/INTERACTION.md`. It is derived from the
original hosts (AFTERDAR.SCR's runner-window and saver-window procedures,
the AD4 engine's `WantEvents`/`GetCapsLockChange`, OLDMOD16's and ADTASK's
result codes, ADPAGE's and OLDMOD16's button paths) and from running the
modules in our host. In brief:

* **Wake or play.** As in AFTERDAR.SCR, a module is *interactive* while it
  wants events: AD4 block `+0x08` bit 0 (set by `UserInput::SetMode` when a
  game starts on a Caps Lock change), AD3 result `0x0E` (a toggle). While
  interactive, keys, clicks and moves belong to the module. Otherwise any
  key except Shift/Ctrl/Caps Lock/Num Lock, any click, the wheel or a move
  past 10 px wakes the saver. Caps Lock never wakes it, nor does Num Lock.
  Alt/F10 always does (our escape; the 1996 host swallowed those while a
  game ran), and so does switching away. Star Trek: The Screen Saver's Final
  Exam is a Num Lock game: Num Lock starts its exam, and a mouse move ends
  it, the module then asking to wake the saver (its result 5, reported as
  the status's wake).
* **Host → saver status.** stdout stays frames only. Each host publishes a
  64-byte status record (interactive, cursor, rotate-ok, key-filter, wake,
  frames, and input sequence numbers) in a shared-memory section the `.scr`
  creates and passes as an inherited handle (`ADSTATUSHANDLE`);
  `ADSTATUSLOG=1` mirrors it on stderr. Input lines (`KEY`, `CAPS`,
  `NUMLOCK`, `MOUSE`) are numbered in arrival order, so the `.scr` can wait
  (at most 300 ms) for the host's verdict on a key a module might consume
  without being interactive: a `WH_KEYBOARD` hook (the AD 3.x engines
  ADXPL40 and ADXPL310, You Bet Your Head, Mime Hunt), or Lunatic Fringe
  taking messages out of the blanker window's queue. `ADCAPS` gives the
  Caps Lock state at start, and `ADNUMLOCK` the Num Lock state (sent, with
  `NUMLOCK` lines, to a host whose `--capabilities` says `numlock=1`);
  `MOUSE` buttons become a bitmask.
* **Multi-monitor.** Only the primary monitor's host gets input (the input
  owner); the cursor is shown only when the module asks and is confined to
  that monitor while playing; rotation waits while it plays.
* **Module buttons.** `adhostwin --configure <module> --button <slot>
  --owner <hwnd>` runs the original button sequence (ADPAGE's
  `Module(0)`/`Module(6, slot)`/`Module(1)`; OLDMOD16's `BUTTONPUSHED16`;
  for an Intermission module's one **Configure...** button, `SAVERMAIN`
  10, 7, 8 and 11, load, query, configure and free, of which 8 opens the
  module's own `DIALOGBOX`; Intermission's control panel sent no 7) and
  turns the guest's `DialogBox*`/`CreateDialog*`, `MessageBox` and
  common file dialogs (Win32 and Win16 templates) into real modal dialogs
  owned by the settings window, forwarding every message to the guest
  dialog procedure. The settings dialog's button rows launch it.
  Scripted test hooks (`ADCONFIGSCRIPT`, `ADCONFIGHIDDEN`) make the
  message modules verifiable end to end.
* **Module state.** The modules' own INI and data files (MODULES.INI,
  AFTERDRK.INI, MESG_AD3.DAT, LunData.dat, Star Wars Screen
  Entertainment's SWSE.INI, Star Trek: The Screen Saver's AD_PREFS.INI, …)
  persist in a per-user,
  per-package copy-on-write overlay over the guest's `C:\WINDOWS` and
  `C:\AFTERDRK` (`C:\SAVER` for an Intermission module) under `ADSTATE`, which the `.scr` passes on every spawn
  (`state\` next to `settings.ini`). Without it the overlay is in memory,
  so headless runs and `FBHASH` never see user state. Host paths picked in
  file dialogs appear read-only under a guest drive `H:`.
* **Desktop seed.** The `.scr` captures each monitor before its windows
  appear and hands a delete-on-close P6 at the emulated size to that
  window's first host (`ADSEEDIMG`, already honoured by both lanes, dithered
  onto the static colours). A window gets a picture for each kind of
  module it may start with, three at most whatever the catalog says (a
  module whose screen got none starts on black): the whole monitor for an After Dark module,
  the part its 640×480 frame covers for one with a screen of its own, an
  Intermission module or one whose catalog entry gives a screen (Star
  Trek, ScreamSavers, Marvel, Johnny Castaway) (INTERACTION.md §8). Respawns,
  rotations, `/p` and headless runs start black.
* **Also.** DOS Shell's early end does not reproduce on the current build;
  the saver has an always-on last-exit log (`logs\saver-last.log`) and a
  5-minute regression. The pe32 lane renders screens under 640×480 on a k×
  guest display, like ne16, so CYBER and CRITIC run in the `/p` preview. The
  dialog's display-name fixes cover the package copies too.

### 9. Collections and covers

The full specification is `docs/COVERS.md`: the settings dialog's
box-cover strip, the importer's cover pipeline, the shared UI library, the
importer GUI restyle, and the work split. The contract in brief:

* **The strip.** When two or more releases (packages) are installed, the
  settings dialog shows one 4:5 box-cover tile per release across the top
  of the window, oldest release first.
  * Tile art is 64×80 DIP, or 48×60 DIP when the client is under 760 DIP
    tall.
  * Clicking a tile toggles it. Several tiles can be selected at once.
  * **Nothing selected means every release.** The module list shows only
    the modules of the selected releases.
  * Each tile is a real toggle button: its accessible name is "<title>, N
    screen savers", it reports a checked state, it can be reached with the
    arrow keys, and it has a context menu with "Show only …", "Show all
    releases", "Change cover…" and "Remove …" (adimport's window asks
    first, then removes the release: `--gui --remove <id>`).
  * Every cover shows: covers that don't fit on one row wrap onto more
    (regular, else compact covers); only a window too short for those rows
    scrolls one compact row between chevrons.
  * With one release installed, the strip is hidden.
* **The list, by release.** The list is grouped by release (title, oldest
  release first), not by lane.
  * Rows show `moduleName`. A release's copy of a module shared with other
    releases is listed under each release it belongs to.
  * The lane is invisible, except that " (Classic)" tells apart two builds
    of one name inside a release ("Bad Dog!").
* **Random.** Each group header's checkbox checks the whole release.
  Select all and Clear act on the rows shown. The saver plays the checked
  modules of the selected releases (`[Saver] Collections`, §6a), and a copy
  with the same bytes (`sameAs`) plays once per pass.
* **Covers.** The importer captures a default cover for each package when
  it imports it. The registry lists each package's sources in that
  package's own preference order. There are two kinds:
  * downloads, checked by md5 and size, with an optional crop;
  * on-disc art (a bitmap file or an NE/PE bitmap resource on the import
    source), with an optional crop.

  3.2 and Totally Twisted, for example, prefer their disc art. A generated
  cover, which the front-ends draw themselves, comes last.

  `--set-cover <id> <picture>` (and "Change cover…" in the GUI) stores the
  user's own picture, which wins over everything. `--clear-cover` goes
  back to the original, and `--refresh-covers` upgrades a fallback once the
  network is back.
* **Storage.** Covers are PNGs under `<assets>\win\covers\<id>\`, never
  inside a package root and never in the repository. The catalog describes
  them in `packages[].cover`.
* **Failures.** A cover failure never fails an import. Nothing fetches
  covers in the background.
* **One look.** `common/ui` (`adw_ui`) holds the settings dialog's
  Windows 11 theming: palettes for light, dark and high contrast, the
  accent, the Segoe UI Variable ramp, the custom-drawn stock controls,
  the header band, cover drawing and off-screen capture.
  * `LongAfterDark.scr` and `adimport.exe` both link it.
  * Every importer window (sources, downloads, progress, result, cover)
    follows the app mode and high contrast live.
  * Surfaces are solid; neither app uses Mica.

### 10. Audio

The full specification is `docs/AUDIO.md`: the census of how all 202
modules make sound, the verified engine paths, the engine's frozen API
(`adw/core/audio.h`), both lanes' mappings, the saver's settings, the tests
and the work split. The contract in brief:

* **What the originals do** (census, `research/win/audio/census.json`):
  * 136 of the 202 modules make sound and 27 play music.
  * AD4: effects through **DirectSound**, which ADXPL510 loads dynamically,
    with IMA-ADPCM decoded through **MSACM32**; music through the **MCI
    sequencer** (`mciSendCommandA`, polled).
  * Hall of Fame streams through **`waveOut`**.
  * AD 3.x (Classic, 3.2, Totally Twisted, Simpsons): effects through
    **AD_SND → `sndPlaySound(SND_MEMORY)`**, from the modules' own resources
    or the shared banks `SIMP_SND.DLL`/`TT_SND.DLL`; PCM, plus MS-ADPCM in
    eight Totally Twisted modules. Music through **`mciSendString`**
    ("open sequencer!…", "play fred notify"), with song ends delivered as
    `MM_MCINOTIFY` to the engines' `adwMidiCall` window.
  * Star Wars Screen Entertainment, outside that census (ABI.md §3.8.8):
    effects through `SWSE.DLL`'s own `sndPlaySound(SND_MEMORY)` of SWSFX's
    resources; music through MEMMIDI, which sequences the General MIDI
    songs itself, `midiOutShortMsg` from a 4 ms multimedia timer.
  * Star Trek: The Screen Saver (After Dark 2.0), outside it too: effects
    through AD_SND 1.0, which plays through a plug-in sound driver; with the
    lane's seed that is Windows' multimedia driver, `AD_MME.DRV`, and so
    `sndPlaySound(SND_MEMORY)` of `ST_SND.DLL`'s resources (AUDIO.md
    §2.12). No MIDI: Final Frontier's theme is one of those recordings.
  * The five later releases, outside it too (AUDIO.md §10.8): the Looney
    Tunes' and the Disney Collection's effects through their AD_SND and
    their songs through the MCI sequencer strings, as the AD 3.x engines
    do; ScreamSavers' effects through its AD_SND 3.1.4; Snoopy's through
    the host's own AD_SND (AUDIO.md §2.10); Marvel's module is silent.
  * CD audio and `waveIn` stay without a device.
* **One engine in `adw_core`**: a deterministic integer PCM mixer (voices
  over buffers, streams of chunks), a Standard MIDI File player, and the
  ADPCM decoders. Sinks: the real device (WASAPI shared mode, `waveOut`
  fallback; MIDI through the Windows MIDI mapper), or a capture (a WAV file
  plus a `.mid` event log, since no deterministic synthesizer renders MIDI).
* **Guest-visible audio is a function of the guest's calls and virtual time
  only.** Cursors, status, completion times and callbacks never come from a
  device, the emulation never waits on one, and live, capture and no sink
  give the guest identical answers.
* **Sound off is unchanged.** Without `ADSOUND=1` or `ADAUDIOOUT`, both lanes
  behave exactly as before, so the headless `FBHASH` baselines hold.
  * `ADSOUND=1` turns guest sound on, and in a streamed run plays it.
  * `ADAUDIOOUT=<file.wav>` turns it on and captures it.
  * `ADVOLUME` (default 50) is After Dark's volume slider, handed to the
    modules. An Intermission module gets it as Intermission's own Volume,
    which sets its effects, and as the MIDI bus gain for its music, in
    place of the Windows mixer's synthesizer slider (Intermission itself
    set only the effects' volume).
* **pe32** emulates DirectSound (COM objects in guest memory, the vtable
  slots ADXPL510 calls verified by disassembly), ACM for IMA-ADPCM, the MCI
  sequencer, `waveOut`, and two aux devices with no mixer. **ne16** plays
  `sndPlaySound` through the engine, reports one MIDI device, implements the
  MCI sequencer strings, and delivers `MM_MCINOTIFY`/`MM_WOM_*` in a fixed
  order at defined points; for Star Wars Screen Entertainment's music it
  adds the raw `midiOut` port (MEMMIDI's `midiOutShortMsg` stream) and the
  multimedia timers that sequence it.
* **The saver** has Sound (default on) and Volume settings. Only the primary
  monitor's host plays sound; the Preview button plays it, and the live
  thumbnail, `/p` and the thumbnail generator never do.
