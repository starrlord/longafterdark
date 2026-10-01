# host/win32 — the Win32 guest runtime and API shims

`adw_win32` is one emulated Win32 process for Long After Dark's 32-bit lane
(`host/pe32`, the AD4-generation lane). That lane runs the 32-bit modules of
After Dark 4.0 Deluxe (`FILES\AD40`, `FILES\ENGINE\STARRYNI.AD`) and of After
Dark 10th Anniversary (`packages\ad10`), each with its own package's
`ADXPL510.DLL` engine (5.1 and 5.2) when it uses one. The same component provides the host-level
display model, the file system (`Vfs`) and the profile store that the Win16
shims of the Classic lane reuse. Design and contract:
`docs/DESIGN.md`, `docs/ABI.md` §2/§4 (verified on the
Deluxe binaries), `docs/API_SURFACE.md` §1, and
`docs/PACKAGES.md` §7.2.

| File | What it owns |
|---|---|
| `layout.hh` | The address space: null guard, system area (PEB/TEB/strings), thunk area, 1 MB stack, the fake host EXE at 0x400000, images from 0x410000 (relocated), the heap arena at 0x20000000, shim-module handles at 0x7F000000. |
| `runtime.hh/.cc` | `Runtime`: CPU in flat mode, TEB/PEB (FS:[0] = -1, [4]/[8] stack, [0x18] self, [0x2C] TLS, [0x30] PEB), trap/fault routing, `call_guest`, `state<T>()`; the work counters the pe32 lane's DRAWFRAME budget reads (instructions, API calls, and the pixels a blit or fill wrote: `charge_pixels`), and the call hook its long calls suspend at (`set_call_hook`; `AbandonGuestCall` unwinds a suspended call, restoring registers and the SEH chain at every level). |
| `shims.hh/.cc` | Thunks (`int 0xFE` + id), `ShimRegistry`, `Call` argument/result helpers, the unimplemented census. |
| `signatures.cc` | Calling convention + stack bytes of **every** Win32-lane import (API_SURFACE.md §1, plus the 49 imports the After Dark 10th Anniversary's pe32 modules added: PACKAGES.md §10 B). |
| `seh.hh/.cc` | Win32 x86 SEH: CPU faults / `int n` / RaiseException through the FS:[0] chain, dispositions, nested exceptions, RtlUnwind, unhandled → `GuestError`. |
| `heap.hh/.cc` | The single guest allocator (Global/Local/Heap*/Virtual*), all in one file-mapped arena so DIB sections can alias guest memory. |
| `modules.hh/.cc` | PE image table: load + relocate via `adw::loader::pe`, bind imports (shim thunks or loaded-DLL exports), DllMain order (one PROCESS_ATTACH ever — ABI.md §8), resources. |
| `vfs.hh/.cc` | The guest's file system, shared by both lanes (INTERACTION.md §7, §10): plain mounts, **copy-on-write overlays** (a read-only lower host folder and/or virtual files under an upper layer that is the per-user state `ADSTATE\<pkg>\…` or memory), and `H:\<L>\…` for the host's drives (read-only, 8.3 names on request). `open()` → `VfsFile` (reads from the upper, virtual or lower layer; any write access copies up into a byte buffer committed on flush/close by temp file + `MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH)` under the state mutex `Local\LongAfterDark-state-<fnv64>`), `remove`/`rename`/`make_dir`/`remove_dir` (upper only; the lower's files refuse with `ERROR_ACCESS_DENIED`), a merged `list()` (upper wins, `.`/`..` first, sorted, 8.3 aliases), `stat`, `read_file`/`write_file`, `host_to_guest` (a mount form, else `H:`), `written()` (what the run wrote, for `--configure`'s JSON), and DOS's current directory: a current drive and one current directory per drive (`cwd`, `set_cwd` for Win32's `SetCurrentDirectory` and `DlgDirList`, `set_drive_cwd` for DOS's chdir, `drive_cwd`, `set_drive`), against which `name`, `\name` and `X:name` resolve (DOS's limits on top: win16's `dos16.hh`). A memory upper stamps a fixed 1996 clock, so headless runs stay deterministic and write nothing. |
| `ini_store.hh/.cc` | `IniStore`: the profile functions of both lanes over the Vfs. Reads = the lane's seeds ⊕ the file (the file wins per key); writes = read-modify-write of the upper file under the state mutex, line-preserving, CRLF; parses cached per file and re-validated by layer, size, write time and version. |
| `config_script.hh/.cc` | The configure-mode test hooks shared by both lanes (INTERACTION.md §6.6): `ADCONFIGSCRIPT` (TEXT/CHECK/SELECT/PICK/MULTI/CLICK on the real controls — PICK selects a list or combo item by its text, wherever a sorted list put it —, and PRESS, a click where a user's lands: the system's hit test from the dialog down, through any window answering `HTTRANSPARENT`, as `hit_window`; blocks split by NEXT; FILE and ANSWER queues), `ADCONFIGHIDDEN` (parked off-screen, cloaked, never activated; unanswered boxes cancel), `ADCONFIGDUMP`, `ADCONFIGTIMEOUTMS`. A lane calls `attach(dlg)` once a real dialog exists and asks `message_box()`/`file_dialog()` before showing one. |
| `realui.hh/.cc` | Real windows for `adhostwin --configure` (INTERACTION.md §6.2): the guest's RT_DIALOG templates become real dialogs (`CreateDialogIndirectParamW` + a DialogBox-style modal loop owned by `--owner`) whose host procedure forwards every message the guest understands to the guest DLGPROC through `call_guest`, marshalling handles, strings and item structs both ways; guest-registered classes get real classes; `WM_DRAWITEM`/`WM_CTLCOLOR*`/`BeginPaint`/`GetDC` give the guest an 8-bit DC wrapper converted through the hardware palette; MessageBoxA and GetOpen/SaveFileNameA become the real ones with guest ↔ host paths. Guest handles of real windows are `0xA000 + 4·i` (a zero high word, as Windows 95's HWNDs: FISH's and RAIN's dialog procedures read WM_COMMAND's notification from `HIWORD(lParam)`). The thread runs `DPI_AWARENESS_CONTEXT_UNAWARE_GDISCALED`. In configure mode a DirectSound that answers `0x8878000A` (`DSERR_ALLOCATED`, the value ADXPL510 tests) lets the Music Choice buttons open. |
| `display.hh/.cc` | Host-level 8-bit palettized screen: memory DC + DIB section over emulated memory attached to core's `Screen`; the system palette (20 statics), realize/animate, nearest-colour, GetDeviceCaps for an 8 bpp RC_PALETTE device (ABI.md §2.9). Also the **desktop seed** (`Display::seed`, `ADSEEDIMG`): before the module's first message a lane may fill the screen with `:win95` (solid teal) or a raw/P6/BMP file of any size, scaled and Floyd–Steinberg dithered onto the 20 statics (a P6 of exactly the screen size is used 1:1), so modules whose own blank keeps the screen (SHADOW with Clear Screen First off) draw over a desktop. The file is opened with share read + write + delete, so the `.scr`'s delete-on-close capture reads (INTERACTION.md §8). Shared with the Win16 lane. |
| `gdi_objects.hh/.cc` | `GdiTable` (a `RuntimeState`): guest handle ↔ real GDI object, DCs, bitmaps, emulated palettes. |
| `kernel32.cc user32.cc gdi32.cc winmm.cc msacm32.cc shell32.cc comdlg32.cc` | One API family per file, each with `register_<dll>(ShimRegistry&)` and its known gaps (deliberately left: no module of the 202 reaches them) listed at the top. |
| `audio.hh/.cc` | The host audio engine as the shims see it (`docs/AUDIO.md` §7): `attach_audio()` (the lane hands over `LaneContext::audio`, else `audio::null_engine()`), `audio_engine()`/`audio_enabled()`/`audio_now()` (the virtual clock *peeked*: an audio call never moves time), `audio_pump()` (applies finished waveOut chunks and song ends, runs due waveOut callbacks; every WINMM call and the lane before each `Module()`), guest `WAVEFORMATEX` helpers, and the Deluxe music renames (`Music\Flying Toasters.mid` → `TOASTERS.MID`, added as virtual files for a deluxe module). With the engine disabled every sound API answers as the silent host always did. |
| `dsound.cc` | DirectSound 3 over the engine, registered only while it is enabled (so `dsound.dll` is otherwise not found, and configure mode keeps RealUi's `DSERR_ALLOCATED`): objects `{vtbl, refcount, id}` on the guest heap with 11 `IDirectSound` and 21 `IDirectSoundBuffer` stdcall thunk slots; the guest backing store is the truth (Lock hands out pointers into it, Unlock copies exactly the given ranges into the engine buffer); duplicates share store and engine buffer; slots outside ADXPL510's verified set log once as "unverified DirectSound method" (`ADTRACE=sound`). |
| `winmm.cc` sound | Sound on: one waveOut device (engine streams; `WHDR_DONE` applied by `audio_pump`; window/function/event callbacks), the MCI sequencer over the engine's MIDI player (`mciSendCommandA`: open/set/play/stop/pause/resume/seek/status/getdevcaps/info/close, `MM_MCINOTIFY`, the real error texts), aux 0 (CD, stored) and 1 (the MIDI bus). `mciSendStringA` (CD audio), the mixer and waveIn stay absent. |
| `msacm32.cc` | ACM emulated with core's decoders (no host codec): the standard IMA/MS-ADPCM formats in the codecs' order, the enumeration callback into guest code, ADPCM → PCM16 streams. |
| `user32_real.cc` | Registered after the families: wraps the USER32/COMDLG32 handlers so a real-window guest handle acts on the real window (the emulated path is untouched), turns DialogBox*/MessageBoxA/GetOpenFileNameA real in configure mode, and declares the dialog entry points no module imports (DialogBoxParamW, DialogBoxIndirectParamA/W, CreateDialog(Indirect)ParamA, Get/SetDlgItemInt, GetDlgItemTextA, CheckRadioButton, MessageBeep, GetSaveFileNameA). |

## Extension recipe: adding or finishing a shim

1. **Find the family file** for the DLL (`gdi32.cc` for `GDI32.DLL!…`). A new DLL
   gets a new file `<dll>.cc` with `void register_<dll>(ShimRegistry& r)`; declare it
   in `shim_families.hh`, call it from `register_all_shims()`, and add the file to
   `CMakeLists.txt`.
2. **Signature**: every lane import already has a row in `signatures.cc`
   (`{"GDI32.DLL", "BitBlt", S, 36}` — S = stdcall, C = cdecl, V = varargs; bytes =
   the fixed arguments). A function reached only via `GetProcAddress` that has no
   row: add one (or use `r.add(dll, name, conv, bytes, fn)`); `r.impl()` of a name
   without a row throws `std::logic_error` at startup, deliberately. A module
   that imports a name with no row (a release the table has not seen yet)
   still loads: the binding is logged (`warning: X imports DLL!Name, which has
   no signature…`), the census names it (`N import(s) without a signature`),
   and a *call* to it throws `GuestError` naming it — returning would leave the
   caller's stack off by the unknown argument bytes (After Dark 10th
   Anniversary's `HALLOFFA.AD` crashed later, elsewhere, that way). Find such
   imports before a run with a PE import dump against this table.
3. **Handler**: `r.impl("GDI32.DLL", "PatBlt", [](Call& c) { … c.ret(v); });`
   * arguments: `c.arg(i)` (dword i above the return address), `c.iarg(i)`,
     `c.null(i)`, `c.str(i)` / `c.wstr(i)` (guest ANSI / UTF-16 strings),
     `c.pod<RECT>(i)` (struct through a pointer argument);
   * results: `c.ret(eax)`, `c.ret64(edx:eax)`, `c.ret_bool(b)`,
     `c.set_last_error(e)` (mirrored to TEB+0x34);
   * memory: `c.mem()` (`read_u32l`/`write_u32l`/`memcpy`), `read_cstr`/`write_cstr`/
     `read_wstr`/`write_wstr`/`read_pod`/`write_pod` in `guest.hh`;
   * struct marshalling is explicit: read the guest struct with `read_pod<T>` into
     the host type, translate every pointer/handle field by hand, write back with
     `write_pod` (`guest.hh` static_asserts the 32-bit layouts it relies on — add
     one for any new struct);
   * callbacks into the guest (WndProc, EnumFontsProc, timers):
     `c.rt.call_guest(addr, {args…}, Conv::stdcall_)` — nests freely;
   * a handler that transfers control itself (ExitProcess, RaiseException) sets
     EIP/ESP and calls `c.take_over()`.
4. **State**: keep per-family state in a `struct MyState : RuntimeState {…}` and
   reach it with `c.rt.state<MyState>()` — no edit to `runtime.hh` needed.
5. **Display / GDI**: `c.rt.display()` (null until the lane attaches the screen)
   gives the palette model and the screen DC; `c.rt.state<GdiTable>()` maps guest
   handles to real GDI objects (`get`, `host`, `add`, `wrap_host`, `screen_dc()`,
   `dc_palette`, `dc_surface`, `key_color`). Handles cross to the guest as 32-bit
   values from `GdiTable`, never raw host handles.
6. **Time**: only `c.rt.clock().read_us()` / `read_tick_count()` (each read
   advances the virtual clock, so busy-waits end deterministically). Audio
   engine calls are the exception: they take `audio_now(c.rt)`, a peek, and
   never a time later than now (audio.hh).
7. **Trace / census**: `ADTRACE=api` logs every call with its arguments;
   `trace("<cat>", …)` for family-specific categories (`file`: every open,
   `GetFileAttributes` and profile read with its host path and result; `user`:
   windows, classes, the message queue and timers; `sound`). An unimplemented
   call prints `[unimpl] DLL!Name` once and appears in the lane's `[census]` at
   exit.
8. **Test**: add a case to `tests/test_win32.cc` (hand-assembled guest code via
   `adw::cpu::X86Emulator::assemble`; see the fixture there) and re-run
   `research/win/pe32_census.sh` to check no module regressed.

Build + test: `AD_BUILD_DIR=build/win-<key> AD_COMPONENTS="host/core;host/cpu;host/loader;host/win32;host/pe32" bash tools/build.sh`

## Interaction and state (INTERACTION.md §5.1, §6, §7)

* **Input.** `GetAsyncKeyState` sets bit 0 when a key went down since the
  previous call for it; `VK_LBUTTON`/`VK_RBUTTON`/`VK_MBUTTON` come from the
  `MOUSE` bitmask; `GetKeyState(VK_CAPITAL)` bit 0 is the Caps Lock toggle
  (`ADCAPS` at start, `CAPS` lines after). `WM_CLOSE`/`SC_CLOSE` posted or
  sent to the saver window raises `saver_wake_requested()` (the lane's wake
  flag).
* **State.** Files and profiles go through the Vfs overlay: headless runs
  (no `ADSTATE`) keep writes in memory; with `ADSTATE` they land in
  `<state>\<pkg>\…`, atomically and under the state mutex, so several hosts
  (two monitors, the live preview, a configure run) never lose a profile
  update. `ADTRACE=file` logs every open, listing, profile read and commit.
* **Configure mode.** A lane enables `real_ui(rt)` with the `--owner` window
  and a `ConfigScript`, sends the module's button messages, and reads
  `shown()` / `timed_out()` / `vfs().written()` for the JSON line.
  `ADTRACE=realui` logs every message forwarded to a guest procedure.
