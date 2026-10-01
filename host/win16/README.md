# host/win16 — the Win16 guest runtime and API shims

`adw_win16` is one emulated Win16 "task" for Long After Dark's Classic lane
(`host/ne16`). That lane runs the 16-bit modules of all fifteen releases:
the After Dark 2.x/3.x modules in After Dark 4.0 Deluxe's `FILES\CLASSIC`,
After Dark 3.2, Totally Twisted, The Simpsons Screen Saver, the 16-bit
modules of After Dark 10th Anniversary, Star Trek: The Screen Saver's After
Dark 2.0 modules (below), Star Wars Screen Entertainment's Intermission
modules (below), those of the five later releases, Marvel Comics Screen
Posters, Snoopy's Screen Savers, The Looney Tunes Screen Saver,
ScreamSavers and The Disney Collection Screen Saver (below), Star Trek: The
Next Generation Screen Saver's (an After Dark 3.0 release, which runs as
the other AD 3.x releases do, with no addition to the runtime), and the
Intermission modules of The Far Side Screen Saver Collection and Scott
Adams' Dilbert Screen Saver Collection (below). The module,
its package's engine when it uses one (`ADXPL300.DLL`, `ADXPL320.DLL`,
`ADXPL40.DLL`, `ADXPL310.DLL`, `ADXPL41.DLL` or `ADXPL100.DLL`; After Dark 2.0's module
library `AD_MOD.DLL` with `AD_RSRC.DLL`; Marvel's decoder `DECO.DLL`),
`AD_SND.DLL` (and After Dark 2.0's sound driver `AD_MME.DRV`), the helper
DLLs and, where the package ships it (Deluxe, 10th Anniversary), the real
`OLDMOD16.DLL` — for an Intermission module, its helper DLLs and
Intermission's own reader, `IMIMXPLY.IMQ` (the ASA reader `IMASAPLY.IMQ` for
an ASA animation; an IMQ module is its own) — run as 16-bit protected-mode code
on `adw::cpu`, over a host-owned LDT, with KERNEL/USER/GDI/MMSYSTEM/…
supplied from here, and, for a package that ships no `AD_SND.DLL`
(Snoopy's), AD_SND too (`adsnd16.cc`, below). The packages without
`OLDMOD16.DLL` go through the lane's native AD3 bridge instead
(`ne16/bridge.hh`, PACKAGES.md §7.4). Design and contract:
`docs/DESIGN.md`, `docs/ABI.md` §3/§4/§8 (verified on the Deluxe binaries;
§3.8–§3.14 on the later releases'), `docs/API_SURFACE.md` §2,
`docs/PACKAGES.md` §7.3/§7.4/§7.5.

| File | What it owns |
|---|---|
| `layout16.hh` | The linear address space: BIOS data area (selector 0x40), system segment (environment, PSP, host strings), thunk segment, task stack, the global arena. |
| `ldt.hh/.cc` | `Ldt` (the CPU's `DescriptorProvider`): LDT selectors with RPL 3, handles = selector with bit 0 clear, huge blocks tiled `__AHINCR` (8) apart, the GDT selector 0x40; the runs of selectors KERNEL's selector calls made for the guest, marked in their entries (`mark_guest_run`, below). |
| `global_heap.hh/.cc` | `GlobalHeap16`: GlobalAlloc/ReAlloc/Free/Lock/Unlock/Size/Handle/Flags over `win32::GuestHeap` (one arena, so DIB sections can alias any block), module segments (`alloc_block`), DGROUPs reserved at 64 KiB. |
| `local_heap.hh/.cc` | `LocalHeaps16`: per-segment local heaps (LocalInit/Alloc/…), fixed pointers 4-aligned, moveable handles ≡ 2 mod 4 pointing at real handle-table entries, `DS:[6]` = pLocalHeap, growth to the segment end. |
| `shims16.hh/.cc` | Far thunks (`int 0xFE; dw id`), `Shim16Registry` keyed `MODULE.ordinal`, `Call16` (Pascal/cdecl argument readers, AX / DX:AX results), the unimplemented census. |
| `signatures16.cc` | **Generated** (`research/win/gen_sig16.py`): name, convention, return width and argument bytes of every entry of the emulated system DLLs, from the Win16 interface facts in `research/win/spec`. |
| `runtime16.hh/.cc` | `Runtime16`: the CPU in segmented mode, `call_far` (nested host→guest calls to a sentinel), thunk dispatch, faults (→ `GuestError16`), the freed-selector rule (`null_freed_segments`, below), VGA ports (0x3DA retrace from the clock, DAC 0x3C7–0x3C9 on the display palette), virtual time, debug knobs. |
| `modules16.hh/.cc` | `ModuleTable16`: NE loading via `adw::loader::ne` (place → selectors → dependencies → `load_segments` → prolog patching → LibEntry → DLLENTRYPOINT), LoadLibrary/FreeLibrary/GetModuleHandle/GetProcAddress (names case-insensitive, constant exports), resources incl. Win 3.0 `NAMETABLE`s, pseudo modules for the system DLLs. |
| `dos16.hh/.cc` | INT 21h (DOS 7.00: files as a handle table over `win32::Vfs::open`/`VfsFile`, directories, rename, FindFirst/FindNext on the merged listing, the current drive and each drive's current directory, `dos_chdir`), INT 1Ah/2Fh/25h/26h/10h/16h/31h, the guest disk's seeds (below: `C:\WINDOWS` and its `TEMP` as in-memory overlays until the lane mounts its own, `MODULES.INI`/`AD_PREFS.INI`/`AFTERDRK.INI` as empty virtual files with their settings as profile seeds, `seed_program_manager`'s `PROGMAN.INI` and `.GRP` files as virtual files, `seed_intermission`'s profile seeds for an Intermission module, `seed_after_dark2`'s for an After Dark 2.0 one, `seed_after_dark3`'s for an After Dark 3.x one), and `profiles16()`, the runtime's `win32::IniStore`. |
| `input16.hh`, `keyboard16.cc` | Saver-window input (below): the WH_KEYBOARD chain, input messages tagged with their input line, the per-step report (consumed, queue reads, wake); the fixed US keyboard (scan codes, `TranslateMessage`'s characters, `key_lparam`). |
| `dialogs16.hh/.cc` | Configure mode (below): the Win16 → Win32 dialog template converter, the message translation table, and the shims that make a module's dialogs, message boxes and file dialogs real. |
| `gdi16.hh`, `gdi16_objects.cc`, `gdi16.cc` | `Gdi16`: Win16 GDI objects on real GDI with the Win32 lane's key-table model (`win32/display.hh`); screen DCs are DIB sections over the display's bits; DIBs are translated to hardware indices through the DC's palette; the native DIB driver (`CreateDC("DIB")`, below). Also USER's SelectPalette/RealizePalette. |
| `kernel16.cc user16.cc system16.cc` | The API families (`register_<family>16(Runtime16&)`); `system16.cc` also has MMSYSTEM's clock, WIN87EM, COMMDLG, KEYBOARD, SHELL, TOOLHELP and `register_all16()`. `user16.cc` also holds the synthetic desktop and the icons (below), `SHELL.ExtractIcon`, the host-posted messages MMSYSTEM's callbacks use (`user16_post_host`/`user16_dispatch_host`), and the guest pump of the Intermission protocol (`user16_dispatch_guest`, below). |
| `sound16.hh/.cc` | MMSYSTEM's sound half (below, AUDIO.md §8) over the host audio engine (`adw/core/audio.h`): `sndPlaySound`, `waveOut*`, `midiOut*` (volumes, and the raw port a self-sequencing guest plays through)/`aux*` volumes, the mixer (none), `mciSendString`'s sequencer, the MCISEQ.DRV stub, and the delivery of `MM_WOM_*`/`MM_MOM_*`/`MM_MCINOTIFY` and of the multimedia timer events (`timeSetEvent`, registered in `system16.cc`). Without an enabled engine: the silent device, byte for byte. |
| `adsnd16.cc` | The host's own AD_SND (`register_host_ad_snd`, below; AUDIO.md §2.10): a system module named `AD_SND` with AD_SND 3.0.3's 36 entries, which the ne16 lane's AD3 protocol registers for a package without an `AD_SND.DLL`. Not one of `register_all16`'s families. |

## The synthetic desktop and icons (PACKAGES.md §7.3)

The desktop-icon gatherers of ADXPL40 (Totally Twisted: `CHAM`) and ADXPL310
(The Simpsons: `HOMEREAT`, `INS`) collect "desktop icons" from `EnumWindows`
(visible top-level windows with a class icon and a title) and Program
Manager's groups (`PROGMAN.INI [Groups]` → each `.GRP` file's name at the
`pName` offset 0x16). With none they `GlobalAlloc(…, 0)`, `GlobalLock`
fails and the module stops with "Out of memory". The first `EnumWindows`
brings a fixed desktop into being: a Windows 3.1 Program Manager window
(class `Progman`, title "Program Manager", HWND 0x0380 — below the window
counter, so no other window is renumbered — with a class icon drawn by the
host) under the saver window, and `C:\WINDOWS\PROGMAN.INI` naming five groups
(Main, Accessories, Games, StartUp, After Dark). A module that never
enumerates windows sees exactly the machine it always did: `BADDOG3` (Deluxe)
looks for `PROGMAN` with `FindWindow` and reads `PROGMAN.INI`, and its stream
must not change. `GetWindow`, `GetClassWord`, `GetWindowText`,
`GetClassName`, `IsWindowVisible` and `GetWindowPlacement` answer
consistently for the desktop's windows; `GetDCOrg` reports a DC's window
corner (0, 0 for the saver).

System bitmaps: `LoadBitmap(NULL, OBM_*)` — the system-menu box, the
minimize/maximize/restore buttons (and their pressed forms) and the four
scroll arrows — are drawn by the host in the Windows 3.1 look with the static
colours (`user16.cc` `system_bitmap`), SM_CXSIZE × SM_CYSIZE (18) or the
scroll-bar size (16). INS draws a window's chrome with them and stopped with
"Out of memory" when they were missing; OBJETS and Bad Dog load them too.
Other ids stay 0.

Icons (`LoadIcon`, `CopyIcon`, `CreateIcon`, `ExtractIcon`, `DrawIcon`,
`DestroyIcon`) are images in `UserState`, handles 0x8000–0xBFFC (multiples of
4: no selector, GDI object or window has one). Their pictures are the guest's
own `RT_GROUP_ICON`/`RT_ICON` resources (ADXPL40's and ADXPL310's group
icon 100), bits a module passes `CreateIcon`, or the Program Manager icon the
host draws — never the host's system icons, whose art differs between
Windows versions. `LoadIcon(NULL, IDI_*)` stays the image-less 0x0F04 it
always was. `DrawIcon` paints the opaque pixels through the DC's palette.

The display's starting palette (`Runtime16Options::desktop_palette`): the 20
statics with black between them, or with 236 distinct colours between them
(a 6×6×6 cube and 20 greys), as a Windows 256-colour desktop left the system
palette. ADXPL310 builds an "identity" palette from `GetSystemPaletteEntries`
at start-up and converts its full-screen canvas's colour table through it
with `GetNearestPaletteIndex`; with 236 identical black entries every
non-static index maps to 0 and `SIMPCLOK` draws its clocks in black. The
Classic lane turns it on for the AD 3 generation packages (`ne16/ad3_protocol.cc`)
and for every Intermission module (`ne16/imx_protocol.cc`: SWSE builds its
identity palettes from it too, `_CREATESYSTEMPALETTE`); `ADDESKTOPPAL`
overrides it (`ne16/lane.hh`).

## The DIB driver (`CreateDC("DIB")`)

`CreateDC("DIB", NULL, NULL, lpPackedDIB)` (or `"DIB.DRV"`) was Windows
3.1's DIB.DRV: a device DC that draws straight into a packed DIB's own bits,
which the program also reads and writes itself between GDI calls. Star Wars
Screen Entertainment's `SWSE.DLL` makes every "addressable canvas" this way
(`_GETCANVASDC`) and draws on it with GDI and with its own blitters. Here the
driver is native (DIB.DRV is never loaded), `Gdi16::create_dib_dc`: the DC's
surface is a DIB section aliasing the packed DIB's bits in guest memory, so
every GDI shim draws into them and blits read them like any 8-bit surface.
It takes what DIB.DRV's Enable took (verified in its code): a
`BITMAPINFOHEADER` (`biSize` 40, one plane, `BI_RGB`), the colour table
(`biClrUsed` entries, else 256), then the bits, bottom-up or top-down — 8
bits per pixel only (DIB.DRV also drew 1- and 4-bit DIBs: refused here,
logged). Anything else is 0. `DeleteDC` leaves the DIB as it was; no bitmap
selects into the DC.

Its pixel values are the DIB's own indices, and colours become them as
DIB.DRV's ColorInfo made them (`Gdi16::dib_index`):

* `DIBINDEX(n)` (0x10FFnnnn), `PALETTEINDEX(n)` and physical colours (the
  flag byte's top bit set) are pixel value n, the low byte, whatever palette
  is selected;
* any other colour is the nearest entry (squared RGB distance, the first of
  equals) of the DIB's colour table read as RGBQUADs — or, when the table's
  first `biClrImportant` (else all) WORDs are 0, 1, 2, … (an index table,
  what SWSE writes: `SETDIBUSAGEBI`), of the 16 VGA colours, pixel values
  0–15. On SWSE's canvases RGB white is 15 and black 0, whatever the
  canvas's palette holds there; HYPERSPC's `PALETTERGB` star streaks are 15,
  7 and 8.

The table is read from guest memory at every match, as the driver read it.
A memory DC made compatible with a DIB DC has the same colour semantics.
DIB bits drawn onto either (`StretchDIBits`, `SetDIBitsToDevice`) keep their
indices when the DC's table is an index table or the source's
`DIB_PAL_COLORS` table is the identity (DIB.DRV's translation between two
tables); otherwise each source colour goes to its nearest entry (a
`DIB_PAL_COLORS` source's colours are its entries in the DC's palette). A DIB
pattern brush (`CreateDIBPatternBrush`: a copy of the packed DIB, as Windows
kept one) paints its top-left 8×8 pixel values unchanged on a DIB DC, and
elsewhere its colours through a bitmap made for it, which `DeleteObject`
deletes with the brush (so does `CreateBrushIndirect`'s `BS_DIBPATTERN`).
`GetPixel`, `SetPixel`'s result and `GetNearestColor` answer the table's
colours (the VGA colours for an index table). The DC is no palette device
(no `RC_PALETTE`): `RealizePalette` maps nothing, and `GetDeviceCaps`
answers the driver's `GDIINFO`, sized by the DIB (256 colours, `RASTERCAPS`
0x2299, 96 dpi).

Real GDI batches drawing per thread, but the program reads the bits between
calls: an API call that touched a DIB DC (`Gdi16::host_dc`) ends with a
`GdiFlush` (`Runtime16::flush_gdi_after_call`).

## Intermission modules (Star Wars Screen Entertainment)

The ne16 lane runs Star Wars Screen Entertainment's IMX modules through
Intermission's own `IMIMXPLY.IMQ`, calling `SAVERMAIN` as INTERMIS did.
What the runtime has for it (the AD3 path uses none of the first two):

* **Profile seeds**, `seed_intermission(rt, IntermissionSeeds)` (`dos16.hh`):
  `SYSTEM.INI [boot] display.drv=pnpdrvr.drv` (Windows 95's driver name,
  which SWSE's blit-technology check compares with its own record); with
  `swse_gdi` (the module folder holds `SWSE.DLL`) `SWSE.INI [technology]
  display.drv=pnpdrvr.drv`, `WinGFound=1`, `DibBlit=GDI` — what SWSESET's
  "Use GDI Graphics" leaves, so SWSE draws with GDI and the DIB driver and
  never loads WinG; `ANTSW.INI [Intermission] Volume` (0–100; 0 is "Off":
  no sound at all) and `Saver Path` (default: the guest directory). Like
  every seed they are read under the files and never written out.
* **The guest pump**, `user16_dispatch_guest(rt, max)`: the message loop
  INTERMIS ran between two calls. The guest's own posted messages (not the
  host-posted ones, not the tagged input messages) go to their live
  windows' procedures in queue order — a posted `WM_TIMER` with a
  `TIMERPROC` to that procedure — then each due timer fires once, to its
  `TIMERPROC` or window (one that an earlier callback of the same pump set
  again waits for its new time, as `SetTimer` reset it on Windows). Messages
  posted to the task itself (hwnd 0: SWSE's `FORCETOWAKE` posts fake mouse
  and Shift-key input for INTERMIS with `PostAppMessage(GetCurrentTask(),
  …)`) are removed and counted in `StepReport16::task_posts` (a running
  count) and `last_task_msg`. At most `max` messages; returns how many were
  handled.
* **KERNEL**: resources are counted as Win16 counted them — `LoadResource`
  of a loaded resource returns the same block with one more use,
  `FreeResource` takes one away and frees the block at none, and the next
  `LoadResource` reads the image again (POSTERS edits its locked caption
  every frame). `AccessResource` is a read-only DOS handle at the
  resource's data in the module file (SWSE and READJPG read pictures,
  palettes, shapes and sounds through it). `GetTempFileName` with `uUnique`
  0 takes the first unused number from 1234h up and creates the empty file,
  in `C:\WINDOWS\TEMP` — with `TF_FORCEDRIVE`, in the current directory of
  the drive it names (the Windows 3.1 SDK's rule; that drive's own, below:
  on `C:` the module folder unless a chdir moved it, on `H:` its root until
  something went there, and `C:\WINDOWS\TEMP` for a drive the guest's disk
  does not have, as Wine does).
  `SetHandleCount` (grows only, at most 255), `GlobalWire`/`GlobalUnWire`
  (lock and unlock), `_hwrite`. `GetModuleHandle` finds the system modules
  Windows 95 always has loaded (KERNEL, USER, GDI, SYSTEM, KEYBOARD,
  DISPLAY, SOUND, MMSYSTEM) before anything imports them. The synthetic
  desktop's Program Manager belongs to a second task
  (`kernel16_shell_task`, which `IsTask` accepts and no task list shows);
  the runtime's own is `kernel16_current_task`.
* **USER**: `GetMenu`, `GetWindowTask`, `GetNextWindow` and
  `EnumChildWindows` answer from the synthetic desktop (JAWAS sizes up the
  desktop's windows; `EnumChildWindows(desktop)` calls back for its
  top-level windows and theirs). A `wsprintf`/`wvsprintf` `%s` whose far
  pointer reads nothing is "", logged (SWTEXT's configure dialog passes a
  near pointer).
* **GDI**: the DIB driver (above), `PaintRgn`, `CreateHatchBrush`,
  `CreateDIBPatternBrush`, `GetSystemPaletteUse`, and `MulDiv` with Win16's
  rounding (to nearest, halves away from zero; -32768 for a zero divisor or
  a result outside -32767..32767).
* **INT 2Fh 1684h** (a VxD's API entry point) answers that there is none
  (ES:DI = 0:0).
* **WING**: `WinGCreateHalftoneBrush` pops 8 argument bytes (WING.DLL
  returns with `retf 8`; the interface table says 6). Nothing of WinG is
  implemented: the seeds keep SWSE off it.

**The Far Side and Dilbert** (Delrina's own Intermission releases; the
lane's forms `asa` and `imq`, `ne16/package.hh` "Form", PACKAGES.md §7.5).
Their 25 ASA animations run through Intermission's ASA reader,
`IMASAPLY.IMQ`, and their five IMQ modules are their own readers; INTRMLIB,
ANTSW (its sprites and palettes), `DIBDLL.DLL`, Dilbert's `IM4_EXP.DLL` and
`MEMMIDI.DLL` run as real code. They get the same seeds (`seed_intermission`
without `swse_gdi`: the module folder holds no `SWSE.DLL`) and the same
guest pump. What the runtime added for them:

* **GDI**: `IntersectClipRect` (GDI.22), which IMASAPLY calls throughout
  every animation (96 to 274 times in 900 frames of each of Dilbert's): the
  DC's clip region cut to the rectangle (logical units). It answers the clip box's region type (`NULLREGION`,
  `SIMPLEREGION` or `COMPLEXREGION`), and `ERROR` (0) without a DC: the
  runtime's GDI answers `COMPLEXREGION` whatever is left, and Windows 3.1
  gave the new region's type. IMASAPLY ignores the answer.
* Nothing else: over 900 frames, each of the 30 modules makes no call the
  runtime lacks, and `DIBDLL.DLL`'s huge pointers (`KERNEL.__AHSHIFT`)
  resolve as SWSE's do (`ADTRACE=mod16`: 339 relocations, LibEntry 1). The
  Configure dialogs (IMASAPLY's "Animation Player Options" and the IMQ
  modules' own) use calls configure mode already had.

## After Dark 2.0 (Star Trek: The Screen Saver)

The ne16 lane runs Star Trek: The Screen Saver's 16 modules through its
native AD3 bridge (`ne16/bridge.hh`) over the package's own AD_SND 1.0;
every Win16 import of the modules, `AD_MOD.DLL`, `AD_RSRC.DLL` and
`AD_MME.DRV` already had a handler. What the runtime has for them:

* **Profile seeds**, `seed_after_dark2(rt)` (`dos16.hh`), which the lane's
  AD3 protocol applies when the module folder holds `AD_MOD.DLL`:
  `AD_PREFS.INI [After Dark] Path=C:\AFTERDRK\` — where AD_MOD opens
  `ST_RES\ST_RESDB.DLL` and `ST_SND.DLL`, AD_SND lists its `*.DRV` sound
  drivers and Sounder finds `SOUNDS\*.WAV` (without it every module but
  Sounder stops with "File not found.") — and `[Sound]
  SoundDriver=AD_MME.DRV`, AD_SND 1.0's driver for Windows' multimedia sound
  (the disk's default, the PC speaker's `AD_MPT.DRV`, has SPALETTE.DLL
  busy-wait on the timer chip's port 0x40, which the runtime has not). Read
  under the empty virtual file and never written out: AD_SND's `[Sound]
  Mute`, Communications' `[Communications] MessageText` and Sounder's
  `[Sounder] SoundPath` land in the upper layer.
* **Num Lock's toggle**: `GetKeyState(VK_NUMLOCK)` bit 0 from
  `InputState::numlock` (the `NUMLOCK` line, `ADNUMLOCK` at start), as
  `VK_CAPITAL`'s comes from `caps`. Final Exam latches it as it starts and
  begins its exam when it changes.
* **Sound**: AD_MME.DRV reaches MMSYSTEM with `GetModuleHandle("MMSystem")`
  and `GetProcAddress` by ordinal (`sndPlaySound` #2, `mmsystemGetVersion`
  #5, `waveOutGetNumDevs`, `waveOutGetDevCaps`, `waveOutOpen` with
  `WAVE_FORMAT_QUERY`, `waveOutGetVolume`/`waveOutSetVolume`): the modules'
  sounds are `sndPlaySound` images from `ST_SND.DLL` (8-bit mono PCM), their
  level `waveOutSetVolume` at After Dark's volume.
* **Configure mode**: Sounder's "Sounds.." dialog lists its folders with
  `DlgDirList(…, DDL_EXCLUSIVE | DDL_DRIVES | DDL_DIRECTORY)`: the drives are
  `[-c-]` and, the host's drives being mounted as `H:`, `[-h-]`, so the
  user reaches their own `.WAV` folders (below). The list takes the guest's
  DOS along (drive and directory, "Files" below), so on OK Sounder's
  `getcwd()` (INT 21h AH=19h, then AH=47h) names the folder drive and all:
  `SoundPath=H:\C\USERS\ME\MUSIC`, which every later run plays. Globe's
  "Map..." in After Dark 4.0 Deluxe and 3.2 saves `GlobeFile=H:\…` the same
  way. A folder whose 8.3 path is longer than DOS's current directory (66
  characters with `H:\`) is not entered: its `DlgDirList` fails. Sounder
  then lists the files of the drive's root (it asks for `\*.WAV`), and an
  OK there saves `SoundPath=H:\`, which plays nothing; Globe also lists the
  root, which holds no map, so its OK saves nothing.
* **Masks from DIBs** (`gdi16.cc`): DIB bits reach real GDI as hardware
  indices under the key table, except into a monochrome bitmap —
  `SetDIBits` and `CreateDIBitmap`, and `StretchDIBits` and
  `SetDIBitsToDevice` on a memory DC holding one —, where real GDI gets the
  DIB's own colour table (a `DIB_PAL_COLORS` one as the DC's palette maps
  it) and makes the bits of real GDI's rule: 1 only where a pixel is the
  table's entry nearest white (the first of equal ones), 0 for every other
  entry, however light (beside a white, yellow and light grey are 0). With
  the key table only index 255 would be 1, so a white the palette holds at
  any other slot made a black mask. Whether Windows 3.1 or 95 drew these
  masks by the same rule is not known here; a white-on-black 1-bpp mask
  comes out the same under any rule that makes white 1 and black 0.
  AD_MOD.DLL makes its masks so: Scotty's Files' blueprints (1-bpp DIBs,
  white on black; drawn through the mask in a colour it fades in with
  `AnimatePalette`), The Mission's console lights, a Final Frontier star.
  So does Mowin' Man (After Dark 4.0 Deluxe, 3.2, 10th Anniversary), whose
  mower was drawn inside a white box before. Everywhere else (a colour
  memory bitmap, a DIB DC, the screen, an RLE DIB) both calls keep the key
  table; `test_mono_dib_targets` checks both sides, and that the source
  origin and a band's start scan reach real GDI as given.

## The five later releases (Marvel, Snoopy, the Looney Tunes, ScreamSavers, Disney)

What the runtime has for them (PACKAGES.md §7.3; each change left the
frozen baselines at 0 differences, PACKAGES.md §9):

**GDI** (`gdi16.cc`):

* `GetMapMode` (GDI.81): real GDI's answer for the DC. Every ScreamSavers
  module gives its memory DC the screen DC's mode,
  `SetMapMode(mem, GetMapMode(screen))`: `MM_TEXT`.
* `FloodFill` (GDI.25, to the colour: `FLOODFILLBORDER`) and `ExtFloodFill`
  (GDI.372, border or `FLOODFILLSURFACE`): the colour is keyed as
  `SetPixel`'s, so the fill compares pixel indices, as a Windows 95 palette
  device compared physical colours; real GDI fills, from the logical point,
  4-connected, with the DC's brush and ROP2, within the surface and the clip
  region, and answers FALSE when the point is outside them or not in the
  area to fill. A monochrome DC compares bits against the colour's nearest
  of black and white; a DIB DC reads its pixels in its own row order,
  bottom-up DIBs included. The pixels a fill paints are charged as a fill's
  pixels are (Virtual time, below), counted by the host by real GDI's rule
  (`flood_pixels`, which `test_flood_fill` holds to what real GDI painted),
  a fill of nothing costing nothing. Snoopy's modules make their sprite
  masks so (ABI.md §3.10); without it every sprite was drawn in a white box.
* `GetDIBits`:
  * **4-bit rows** (the Disney Collection's Haunted captures the desktop a
    line at a time so): the
    colour table real GDI writes, measured on Windows 11 on an 8-bit key
    surface, the 16 VGA colours with dark grey `808080` at index 7 and light
    grey `C0C0C0` at 8 (DIB.DRV's table has them the other way round); each
    pixel becomes its nearest entry (squared distance, the first of equals,
    as real GDI chose for all 32 colours probed); `biClrUsed` 0.
  * **`DIB_PAL_COLORS`**: the bits stay hardware indices and the table
    describes them, entry h naming the DC palette's logical entry nearest
    hardware colour h (`GetNearestPaletteIndex`'s rule; `DEFAULT_PALETTE`
    when none is selected), so a `SetDIBits` through the same palette gives
    the colours back. The table used to be the identity: ADXPL41 (the Looney
    Tunes) draws its labels on a canvas it round-trips, `SetDIBits` → GDI →
    `GetDIBits` → `SetDIBits`, through a 255-entry palette holding the high
    statics at 245..254, so white (hardware 255) fell off the palette and
    246..254 moved one static on: Pepe's black label boxes, Sam's and Taz's
    broken folders. Windows NT and Wine return the logical indices
    themselves, which no module here needs (tried: the white labels turned
    cyan, and Simpsons Trivia moved).
  * **Monochrome bitmaps**: 1-bit rows are real GDI's, asked in a header of
    our own (40 bytes, the bitmap's own width and height, the guest's
    top-down sign kept; the guest's header keeps its `biSize`, and its
    2-entry table follows it); 4-, 8- and 24-bit rows show black and white
    as hardware indices 0 and 255, real GDI's values (0 and 15 in 4-bit
    rows, 0 and 255 in 8-bit ones, `000000` and `FFFFFF` in 24-bit ones);
    16- and 32-bit requests are refused, as for 8-bit bitmaps, and so are
    4-, 8- and 24-bit requests of the stock 1×1 bitmap. (Before, a
    monochrome bitmap's 8-, 16-, 24- and 32-bit requests crashed the host, a
    defect from 1.1.0, as did a 1-bit request with a header larger than 40
    bytes or wider than the bitmap, and its 4-bit rows were garbage; no
    module of the corpus makes such a request. `test_getdibits_mono` holds
    the fix.)
  * Rows are the bitmap's own width and height, whatever the header says;
    real GDI follows the header (measured). Haunted asks for 640 pixels of
    each 648-pixel line and gets 648 (the next line overwrites the extra
    bytes), and ADXPL40 and ADXPL41 ask for 24 rows of a 76-row bitmap with
    a 24-row header and get its bottom rows where real GDI gives its top
    ones: in Chameleon a stray icon then covers the "Accessories" label
    after about half a minute (a known gap, PACKAGES.md §12).
    `ADTRACE=dib16` logs each request.
* `CreateBitmapIndirect` (GDI.49): `CreateBitmap` of the structure's width,
  height, planes, bits per pixel and bits (the rows WORD-aligned, as
  `CreateBitmap` reads them). `CreatePatternBrush` (GDI.60): a brush of the
  bitmap's top-left 8×8 pixels (Windows 3.1 and 95 brushes were 8×8), in a
  bitmap of the brush's own that `DeleteObject` deletes with it, so the
  program may delete its bitmap once the brush is made, as Windows allowed;
  a monochrome pattern paints its 0 bits in the DC's text colour and its 1
  bits in its background colour. Little Mermaid's "Plain" sea fills each
  row with an 8×8 dither made so. (`CreateBrushIndirect`'s `BS_PATTERN`
  still paints the guest's bitmap itself, whole.)

**KERNEL and TOOLHELP** (`kernel16.cc`, `system16.cc`):

* `GetHeapSpaces` (KERNEL.138): a fixed, healthy local heap for any module
  handle, 57,600 of 64,000 bytes free (`0xFA00E100`: the 90%
  `GetFreeSystemResources` reports); 0 for a handle that is no module's, as
  Wine answers. `MARVEL.AD` divides by the size (ABI.md §3.11).
* `GetCodeHandle` (KERNEL.93): DX:AX = the selector and handle of the module
  segment `lpfn` points into, as `GlobalHandle` gives them; 0 when it is no
  module's.
* **The selector calls**, as Windows 3.1's KRNL386 made them (Pietrek's
  pseudocode, the 3.1 SDK, KB Q132005), for Marvel's `DECO.DLL`:
  `AllocSelector` (175) copies a selector's descriptor, one selector per 64
  KiB of its limit, tiled as a huge block's (for 0 or a bad selector, one
  uninitialized selector, not present until its rights are set; 0 when the
  LDT is full); `FreeSelector` (176) frees every tile of such a run (0; else
  the selector back); `AllocCStoDSAlias` and `AllocDStoCSAlias` (170, 171)
  make one selector over the descriptor with only its code bit changed;
  `GetSelectorBase` and `GetSelectorLimit` (186, 188) read any selector (0
  for a bad one); `SetSelectorBase` (187) returns the selector (0 when
  refused) and `SetSelectorLimit` (189) always 0, the limit taken as 20
  bits, byte-granular, and a segment register holding the selector sees the
  change at once. **Only their own**: the runs these calls make are marked
  in the LDT entries themselves (`Ldt::mark_guest_run`; freeing or
  reallocating an entry clears its mark, so a reused index is never taken
  for theirs), and `FreeSelector` and the Sets refuse any other selector (a
  global block's, a module segment's, the host's), where KRNL386 would have
  changed any LDT entry. `ADTRACE=mem16` logs each call that makes, frees
  or changes a selector, and each refusal.
* `GlobalFirst` and `GlobalNext` (TOOLHELP.51, 52): an empty walk, FALSE at
  once, the `GLOBALENTRY` untouched, as the unimplemented stubs answered.
  Their one caller, the Disney Collection's ADXPL100 with sound on, walks
  the heap for MCISEQ.DRV's blocks to page-lock them (AUDIO.md §2.9); on
  FALSE it notes "Error walking global list" and the song plays. MCISEQ.DRV
  is no NE module here (the MCI sequencer is the host's), so a real walk
  (every block in arena order with its address, size, handle, lock counts,
  owner and type: the `system16.cc` comment) would find none of its blocks
  and lock nothing either.

**The freed-selector rule** (`Runtime16::null_freed_segments`, one place):
as every API call returns (`dispatch_thunk`, whether or not the shim took
over CS:IP) and every software interrupt handler (`on_interrupt`: DPMI's
Free LDT Descriptor, INT 31h AX=0001h), any of DS, ES, FS and GS that holds
an LDT selector no longer in use becomes the null selector, whatever freed
it: `GlobalFree`, `FreeSelector`, `FreeResource`, `FreeLibrary`, a
`GlobalReAlloc` that gave a block new selectors, DPMI, or a callback the
call made. Null registers, the GDT's 0x40 and live selectors stay as they
are, and no load is fixed: a selector pushed before a call and popped after
it still faults. That is what Windows 3.1's `GlobalFree` did for the
caller's DS, what DPMI 1.0 does for any register, and what Wine's relay
does as every call returns; KERNEL fixed up no fault in an application's
own code (ABI.md §3.14; the evidence is
`research/win/pkg/more/l2/FREED_SELECTOR_RULE.md`, gitignored). It serves
`DECO.DLL`, which frees its work buffer with DS still holding it, and
Borland C++'s far-heap free in six of Snoopy's modules, which reloads the
ES it has just freed at CLOSE. It fires, invisibly, in 162 of the 202 After
Dark baseline modules too (ES after `GlobalFree` or `FreeResource`, FS four
times), every stream unchanged. `ADTRACE=mem16` logs each register it nulls:
"ES 00D7 was freed by KERNEL.17 GlobalFree: null on return".

**After Dark 3.x's `AD_PREFS.INI`**, `seed_after_dark3(rt)` (`dos16.hh`),
which the lane's AD3 protocol applies when the engine dir holds
`ADW30.EXE`: `[After Dark] Path=C:\AFTERDRK` (the guest directory, in
ADW30's own spelling, with no trailing backslash) and `[Sound]
SoundDriver=AD_MME.DRV`, the keys `ADW30.EXE` wrote at every start (ABI.md
§3.12), as profile seeds: read under the empty virtual file, never written
out. The Disney Collection's ADXPL100 finds `DIS_SND.DLL` and `MUSIC\` by
the first. ADW30's third key, `WIN.INI [Berkeley Systems] After Dark`,
`register_dos` seeds for every module. After Dark 2.0's seeds win where both
rules hold.

**The host's own AD_SND**, `register_host_ad_snd(rt)` (`adsnd16.cc`,
declared in `shim_families16.hh`): a system module named `AD_SND` with the
36 entries of AD_SND 3.0.3, their ordinals, names and argument sizes (ABI.md
§3.13). Once it is registered, `AD_SND` by name, an import or a
`LoadLibrary` whatever the path, is this module. It is no family of
`register_all16`: the ne16 lane's AD3 protocol registers it, for the native
bridge only, before the bridge opens, when the package's engine dir holds
no `AD_SND.DLL` (`ne16/package.hh` `host_ad_snd`; Snoopy's Screen Savers).
It keeps its state in the runtime (`HostAdSnd16`), makes every call through
the KERNEL and MMSYSTEM thunks as a real library's imports would, so the
census, the `api16` trace, virtual time and the audio engine see them, and
costs no instructions of its own. What each entry does is AUDIO.md §2.10.
`ADTRACE=sound` prints, at its init, "AD_SND (the host's): wave devices 0
(11 kHz) and 0 (22 kHz), capabilities 7", or "AD_SND (the host's): no sound:
…" with the reason.

Tests, all on made-up data: win16.unit's `test_map_mode`, `test_flood_fill`,
`test_getdibits_4bpp`, `test_dib_pal_colors`, `test_getdibits_mono`,
`test_pattern_brush`, `test_heap_spaces_code_handle`, `test_selector_calls`,
`test_freed_selector_rule`, `test_toolhelp_walk` and
`test_after_dark3_seeds`; ne16.unit's `test_host_ad_snd`, its After Dark
3.x seeds check and `test_ad2_palettes` (After Dark 2.0's palettes computed
in code, `ne16/package.hh` `palettes_after_dark2`).

## Sound (AUDIO.md §8)

`attach_audio16(rt, engine)` (the lane passes `LaneContext::audio`) decides
what MMSYSTEM is. Without an enabled engine (none, or `ADSOUND`/`ADAUDIOOUT`
unset) it is the silent device of before, answer for answer: one wave-out
device "Long After Dark (silent)" whose format queries all succeed and whose
`sndPlaySound` reports the sound played, no MIDI, aux or mixer devices, MCI
refused (`MCIERR_DEVICE_NOT_INSTALLED`); the calls it never answered
(`waveOutWrite` & co.) stay unimplemented, and `midiOutOpen` fails honestly
(`MMSYSERR_NODRIVER` for the mapper, `BADDEVICEID` otherwise, the handle
zeroed; a signature-only one used to return "success" without writing it),
with every midiOut handle invalid. AD_SND.DLL needs the wave device to
initialize at all (ABI.md §3.6). Timer events work either way (below).

With the engine on (every audio call carries `Runtime16::peek_us()`, or,
inside a callback procedure, its dated time — MCI commands excepted:
Callbacks, below):

* **`sndPlaySound`**: a `SND_MEMORY` image is copied at the call — its RIFF
  extent, bounded by the segment (AD_SND unlocks it right after) — then
  parsed and decoded (PCM, MS-ADPCM for the Totally Twisted banks, IMA-ADPCM)
  into one engine buffer on the wave bus. One voice per process: a new call
  replaces the sound, `SND_NOSTOP` while one plays is FALSE, NULL stops,
  `SND_LOOP` loops (and is always asynchronous). Without `SND_ASYNC`
  (NOCTURNE) the call lasts the sound's duration of virtual time
  (`wait_until_us`, below). A name is a file (as given, then `WINDOWS` and
  `SYSTEM`, `.WAV` assumed) or a `WIN.INI [sounds]` entry. Undecodable: FALSE.
* **`waveOut*`**: one device, "Long After Dark". `WAVE_FORMAT_QUERY` accepts
  PCM, and IMA-/MS-ADPCM on `WAVE_MAPPER` (decoded chunk by chunk when a
  stream is opened that way). Streams follow the Windows header model
  (`WHDR_PREPARED`/`INQUEUE`/`DONE`, `WAVERR_STILLPLAYING`/`UNPREPARED`),
  `GetPosition` in bytes/samples/ms, `Pause`/`Restart`/`Reset`, and
  `CALLBACK_NULL`/`WINDOW`/`TASK`/`FUNCTION` with `MM_WOM_OPEN`/`DONE`/
  `CLOSE`. Handles are 0xE000–0xFFFC (multiples of 4). `waveOutSetVolume` is
  the engine's wave bus.
* **`midiOut*`, `aux*`**: one MIDI device ("Long After Dark MIDI", mapper
  technology, `MIDICAPS_VOLUME|LRVOLUME`) — the engines' `IsMusicAvail` — and
  two aux devices (0 CD audio: stored; 1 = the MIDI bus). `midiOutSetVolume`
  and `auxSetVolume(1)` are one volume, the engine's MIDI bus. The device's
  raw port (AUDIO.md §8.3) is for a guest that sequences itself — Star Wars
  Screen Entertainment's MEMMIDI plays SWSE's songs through it from a 4 ms
  timer event: `midiOutOpen` (the mapper or device 0, one client at a time,
  `CALLBACK_*` with `MM_MOM_OPEN`/`DONE`/`CLOSE`), `midiOutShortMsg`/
  `LongMsg`/`Reset` into the engine's raw MIDI (`midi_short`/`long`/`reset`:
  the `.mid` log, the live synth), `MIDIHDR` flags (a long message is done
  when its call returns), patch caching `MMSYSERR_NOTSUPPORTED` (no
  `MIDICAPS_CACHE`), `midiOutGetID`. No mixer (`mixerGetNumDevs` 0), and
  with the engine on no mixer API by name either (`Shim16Entry::by_name`:
  `GetProcAddress` answers 0, ordinals resolve): AD_SND 3.2/TT picks its
  mixer path whenever `GetProcAddress` finds those names, and with no mixer
  device that path sets no volume at all; without them it sets the wave and
  MIDI volumes, so `ADVOLUME` reaches the AD 3.2 and Totally Twisted modules
  (AUDIO.md §2.10).
* **`mciSendString`**: the sequencer's commands (AUDIO.md §8.4): `open
  sequencer`, `open sequencer!<path> [alias a]`, `open <path> type sequencer`
  (or a `.mid`/`.rmi` path alone), `close <a>|all`, `play [from n] [to n]
  [notify]` (`play … wait` is `MCIERR_UNSUPPORTED_FUNCTION`), `stop`,
  `seek to start|end|n`, `status mode|length|position|ready|…`, `set time
  format ms`, `set port mapper`; `wait` is accepted and harmless elsewhere.
  Paths go through the VFS (relative to the current directory). `open`
  returns the device id (lowest free from 1); errors are the `MCIERR_*`
  Windows gives; the return buffer is always NUL-terminated. A play's
  `notify` sends `MM_MCINOTIFY(SUCCESSFUL, id)` to the callback window at the
  song's end (or its `to`); a later `notify` supersedes it (`SUPERSEDED`),
  `stop`/`seek`/`close` abort it (`ABORTED`), either before the new
  command's own notify.
* **The engines' music gates** (ADXPL300/310/40, AUDIO.md §2.9): a MIDI
  device; `LoadLibrary("TOOLHELP.DLL")` (a system module) and
  `LoadLibrary("MCISEQ.DRV")` (a stub system module registered only with the
  engine on) >= 32; `GetProcAddress(TOOLHELP, "GLOBALFIRST"/"GLOBALNEXT")`
  non-NULL. The engines use GlobalFirst/GlobalNext only on Windows 3.10
  exactly (to page-lock MCISEQ's segments, ADXPL310 4:f46f); on the 3.95 we
  report they never call them. (ADXPL41, the Looney Tunes', plays the same
  way; the Disney Collection's ADXPL100 walks the heap with them before a
  song on the 3.95 too and gets the empty walk, above.) They reload MCISEQ
  every 100 songs, and
  around every play set `system.ini [mciseq.drv] disablewarning=true` and
  write the old value back — the key is seeded `true` (a profile seed, sound
  on only), so no SYSTEM.INI is written to a persistent state directory.
  Their hidden `adwMidiCall` window's `MIDIWNDPROC` takes `MM_MCINOTIFY`:
  SUCCESSFUL replays (loop) or ends the song, SUPERSEDED and FAILURE end it,
  ABORTED is ignored.

**Callbacks** (AUDIO.md §8.6): the engine's events (`chunk_done` →
`MM_WOM_DONE` and the header's `WHDR_DONE`, `song_end` → `MM_MCINOTIFY`) and
the ones MMSYSTEM raises itself (`MM_WOM_OPEN`/`CLOSE`, `MM_MOM_OPEN`/`DONE`/
`CLOSE`, SUPERSEDED/ABORTED) queue with their virtual time and issue order,
with the periods of the multimedia timer events. They are delivered at the
first API call at or after that time (the runtime's audio hook, below) and
at the lane's pump before every DRAWFRAME (`audio16_pump`): window messages
are posted to the guest's queue (`user16_post_host`), where a guest that
pumps takes them, and the pump sends those still waiting to their window
procedures, as the 1996 host's message loop did between DRAWFRAMEs;
`CALLBACK_FUNCTION` procedures, and one timer procedure call per period, are
called as a nested `call_far` with their module's DS. Never while a delivery
runs (a callback's own API calls deliver nothing). Such a procedure ran at
interrupt time on Windows, at its event's time: the MMSYSTEM calls it makes
are dated from that time plus what it has run since, and neither a delivery
nor the lane's step end renders the engine past a due point still to be
delivered (a delivery takes it only as far as the events due, the step end
only up to `Runtime16::audio_due()`), so MEMMIDI's notes keep their 4 ms
grid when a frame ends after a DRAWFRAME that made no call, or inside a long
one; clocks the guest reads never go back. MCI commands a procedure sends
are dated at the delivery point instead (MCI was no interrupt-time API), so
a `play … to` stop is reckoned from the song's real start. What a procedure
causes — its device opened or closed, a long MIDI message sent, a WAVEHDR it
wrote that the stream finishes at once (an empty one) — waits for a later
delivery point, whatever its date: a procedure that answers each
notification with another request (`midiOutLongMsg` from `MM_MOM_DONE`, an
empty WAVEHDR from `MM_WOM_DONE`) takes one step per delivery point instead
of looping inside one while virtual time barely moves. Everything is a
function of the guest's calls and virtual time.

**Timer events** (`timeSetEvent`/`timeKillEvent`/`timeBeginPeriod`/
`timeEndPeriod`/`timeGetDevCaps` in `system16.cc`, delivered by
`sound16.cc`'s `timer16_set`/`timer16_kill`; AUDIO.md §8.6): with or without
the engine — the delivery hook goes in at the first event, so a module that
sets none runs exactly as before. 1–65535 ms, periodic or one-shot, 16
events at most, `TimeProc(wID, 0, dwUser, 0, 0)` FAR PASCAL per period, the
event rescheduled before the call; missed periods are caught up, at most the
last 250 ms of them (at least one) when a periodic event fell further behind
(an event a procedure sets starts no more than 250 ms back either).
`audio16_close` kills what the guest left set.

## The guest's disk (INTERACTION.md §7)

Files and profiles go through `win32::Vfs` and `win32::IniStore`, shared
with the pe32 lane. The Classic lane mounts, for an After Dark module,
`mount_disk` (`ne16/ad3_protocol.cc`) and, for an Intermission module,
`mount_imx_disk` (`ne16/imx_protocol.cc`), both declared in
`ne16/protocol.hh` (INTERACTION.md §7.2):

| Guest | Lower (read-only) | Upper |
|---|---|---|
| `C:\WINDOWS` | the package's `WINDOWS` folder when it has one (Star Wars Screen Entertainment's: `SWSE.INI`, as its installer put it there); virtual seed files (`MODULES.INI`, `AD_PREFS.INI`, `AFTERDRK.INI` empty; `PROGMAN.INI` and the `.GRP` files once the synthetic desktop exists; `LunData.dat`, the module dir's `LUNDATA.DAT`, where the installers copied it); for an Intermission module, the profile seeds of `seed_intermission` (above), for an After Dark 2.0 module those of `seed_after_dark2` (above), for an After Dark 3.x one those of `seed_after_dark3` (above) | `<ADSTATE>\<package>\WINDOWS`, or memory |
| `C:\WINDOWS\TEMP` | — | memory, always |
| `C:\WINDOWS\SYSTEM` | the engine dir (an Intermission module's: `IMIMXPLY.IMQ`, or The Far Side's and Dilbert's `IMASAPLY.IMQ`) | none (read-only) |
| `C:\AFTERDRK`, `C:\AFTERD~1` (After Dark) | the module dir | `<ADSTATE>\<package>\<MODDIR>` (one directory for both names; in memory mode each name has its own) |
| `C:\SAVER` (Intermission, instead of `C:\AFTERDRK`) | the module dir: the modules, their DLLs, the MIDI files, `SWTEXT.TXT` | `<ADSTATE>\<package>\SAVER`, or memory |
| `H:\<L>\…` | the host's drives, 8.3 names | none |

The guest directory (`C:\AFTERDRK` or `C:\SAVER`) is the current directory
when the module starts. The guest's DOS keeps a current drive and, on each
drive, a current directory of its own (`win32::Vfs`; a drive's root until
something goes there), as DOS did: INT 21h AH=0Eh selects a drive the
guest's disk has (`C:`, and `H:` with the host's drives mounted; it reports
8 drive letters, to `H:`), AH=19h reports the current drive, AH=3Bh sets the
directory of its path's drive and leaves the current drive, AH=47h reports
any drive's own directory (DL 0 the current one), whole, and `DlgDirList`
moves to the drive and directory it lists. `\name` resolves against the
current drive's root and `X:name` against drive X's own directory in every
file call; a bare `name` resolves against the current directory in
`_lopen` and INT 21h, `OpenFile` searches from there, and the profile
calls look for it in `C:\WINDOWS`, as Windows did. A current directory holds at most 66
characters with its drive (`kMaxCurDir`), as a DOS CDS did (AH=47h's
64-byte buffer holds the part after `C:\`): a chdir deeper than that fails
with error 3, and a folder list does not go there. (Before, AH=19h said
`C:` whatever the current directory's drive, AH=0Eh selected nothing and
reported 5 letters, AH=47h returned the current directory whatever DL
named, cut to 63 characters, and `X:name` meant `X:\name`, `c:` in a folder
list `C:\`.)

Opening a lower file for writing (`_lcreat`, `OpenFile(OF_CREATE|OF_WRITE…)`,
INT 21h 3Ch/3Dh/5Bh/6Ch) copies it up first; new files and directories go
to the upper layer; deleting or renaming a lower file fails (access denied).
Profiles read the seeds under the file (the file wins per key) and write the
upper file only. Without `ADSTATE` every upper layer is memory: a headless
run reads and writes nothing of the user's, and its frames are what they
always were. Where modules keep their state (verified in configure runs):
MESSAGE3 `MESG_AD3.DAT`, NONSENSE `NONSENSE.TXT`, SLIDE `BITMAPS.ADC`,
WMORPH `morph*.dat` (module dir); FISHPRO `[Fish]`, BUGS `[Bugs]`, ARTIST
`[The Artist] Image`, LOGO `[Logo Section] LogoFile`, Message Mayhem
`[Message Mayhem] CustomA` (`MODULES.INI`); GLOBE `AD_PREFS.INI`; LUNATIC
`LunData.dat` (in `GetWindowsDirectory()`, so `<package>\WINDOWS`; read from the seed until
Keys… or a high score writes it); Star Trek's Communications `[Communications]
MessageText`, Sounder `[Sounder] SoundPath` and AD_SND 1.0 `[Sound] Mute`
(`AD_PREFS.INI`); Marvel's Saver.. choices in `MRVLIMAG\MRVLIMAG.ADC` (module
dir: the whole catalog copied up at its first write, a Saver.. OK, a
Posters... Install or a wake with Create Poster On Wakeup, ABI.md §3.11)
and its Posters... Install
and Create Poster On Wakeup in `MRVLIMAG\MARVEL.BMP` and `WIN.INI [Desktop]`
(a wallpaper for the emulated PC only: `SystemParametersInfo` changes
nothing); the Looney Tunes' Messages `[Looney Messages] CustomA`
(`MODULES.INI`).

## Saver-window input (INTERACTION.md §5.2)

Modules mostly poll (`GetAsyncKeyState` & co. read the host's `InputState`;
`VK_RBUTTON`/`VK_MBUTTON` from the `MOUSE` bitmask; `GetKeyState`'s bit 0 is
Caps Lock's toggle for `VK_CAPITAL`, from the `CAPS` line, and Num Lock's for
`VK_NUMLOCK`, from the `NUMLOCK` line). For those that take
messages, the lane (`ne16/lane.cc`, "Input and status") hands every `KEY`
line to the WH_KEYBOARD chain (`SetWindowsHook`/`SetWindowsHookEx(2)`, most
recent first; `DefHookProc(…, &token)` and `CallNextHookEx(token)` call the
hook installed before; a non-zero result consumes the key) and otherwise
posts it to the saver window as `WM_KEYDOWN`/`WM_KEYUP` (Win16 `lParam`:
repeat 1, US scan code, extended bit, bit 30 previous state, bit 31
transition), `MOUSE` lines as `WM_MOUSEMOVE` and button messages, each
tagged with its input line. These wait in an input queue read after the
posted one, as Win16's system queue was; `PeekMessage`/`GetMessage` honour
their hwnd and range filters. A tagged message the guest removes and does
not `DispatchMessage` back to the saver window is consumed
(`user16_end_step`); one nobody took is dropped after its step, or kept up to
`keep_steps` more steps for a suspended DRAWFRAME that may still take it (the
lane passes 1, or 600 while that call reads the saver window's queue itself:
LUNATIC's game), and reported as `pending` meanwhile. Reading the
saver window's queue with removal for a range that includes keys counts as a
queue read (the lane's key-filter). `FindWindow("Sleep", NULL)` — the AD 2/3
blanker class LUNATIC looks for (LUNATIC `26:0089` pushes DS:0DE8 =
"Sleep") — answers the saver window. `PostMessage(saver, WM_CLOSE)` or
`SC_CLOSE` raises wake. `TranslateMessage` makes `WM_CHAR` from the US
layout; `KEYBOARD.MapVirtualKey`/`VkKeyScan` use the same tables (never the
host's layout: runs stay deterministic).

Measured (`ne16.interaction`): YBYH, SIMPTRIV, tt FRANKEN, tt MIMEHUNT and
HOW2DRAW install their WH_KEYBOARD hook when they enter the game (0x0E) and
remove it when they leave; the ADXPL40/ADXPL310 engines hook nothing at load.
Without input only LUNATIC raises key-filter (every frame, before the first
key). MIMEHUNT also asks for a WH_MOUSE hook (refused: other hook kinds
return 0 as before).

## Configure mode: real dialogs (INTERACTION.md §6.2 Win16)

`adhostwin --configure` runs a module's button through the bridge
(`BUTTONPUSHED16`); the lane calls `enable_real_dialogs16`, which replaces
or wraps the USER/COMMDLG/KERNEL shims below. Nothing of it is installed in
the saver.

* `DialogBox(Param)`, `DialogBoxIndirect(Param)`, `CreateDialog(Param)`,
  `CreateDialogIndirect`: the RT_DIALOG template goes through our converter
  (`convert_dialog_template16`: DWORD-aligned items, UTF-16 from code page
  1252, the menu and a custom dialog class dropped, `SS_ICON` ordinals
  emptied, the guest's extra item bytes dropped) into the real
  `DialogBoxIndirectParamW`, owned by `--owner`. The host dialog procedure
  forwards to the 16-bit `DLGPROC` through `call_far` (Pascal), from the
  dialog's first message on (`WM_MEASUREITEM` and `WM_SETFONT` come before
  `WM_INITDIALOG`, whose `lParam` is the guest's init param).
* Real windows are HWND16s from `0xC000`–`0xDFFC` (multiples of 4: no
  selector, GDI object, icon or emulated window has one), mapped both ways.
  The wrapped shims (`GetDlgItem`, `SendDlgItemMessage`, `Set/GetDlgItemText`,
  `…Int`, `CheckDlgButton`, `IsDlgButtonChecked`, `CheckRadioButton`,
  `DlgDirList`/`DlgDirSelect` (on the guest's disk; `DDL_DRIVES` lists
  `[-c-]` and, when the host's drives are mounted, `[-h-]`, which
  `DlgDirSelect` makes `h:`; `DlgDirList` moves the guest's DOS to the drive
  and directory it lists, each drive keeping its own, so `c:*.WAV` lists
  `C:` where the list left it; a directory deeper than a DOS current
  directory is refused, 0, the list, the spec and the current directory
  unchanged), `EndDialog`,
  `SendMessage`, `PostMessage`, `DefWindowProc`, `CallWindowProc`, window
  queries and moves, focus, capture, timers with guest `TIMERPROC`s, props,
  window words/longs incl. subclassing, `EnumChildWindows`, scroll bars,
  `CreateWindow(Ex)` on a real parent) act on the real window.
* Placement: the module keeps the screen coordinates of its emulated
  desktop (640×480: `GetDesktopWindow`'s rectangle,
  `GetSystemMetrics(SM_CXSCREEN)`), and that desktop lies over the settings
  window (`guest_screen_origin16`): its origin is the owner's centre less
  half the desktop, moved inside the owner monitor's work area (centred on
  the work area when that is the smaller), measured once in the dialog
  thread's 96-DPI coordinates. A top-level real window the module places
  (`MoveWindow`, `SetWindowPos` without `SWP_NOMOVE`, `CreateWindow(Ex)` of
  an owned popup) goes to origin + (x, y); a child's position, in its
  parent's client area, is left alone. `GetWindowRect` of a real window,
  `ClientToScreen`, `CB_GETDROPPEDCONTROLRECT`, a top-level window's
  `WM_MOVE`, `WM_NCHITTEST`'s point and `GetCursorPos` (the real cursor:
  ANTSW's slider drags its thumb to it and repeats an arrow while the cursor
  stays on it; on a desktop that is not the input desktop, where the real
  one cannot be read, the emulated one answers) subtract the origin and
  `ScreenToClient` adds it first, so a
  module that reads positions back stays consistent. The dialogs that place
  themselves on that screen (Marvel's Saver.. and Posters..., Lunatic
  Fringe's Keys..., Messages' Edit / Select, Globe's Map..., Slides...,
  DrawMorph's Edit..., Star Trek's Edit Custom... and Sounds.., the Star
  Wars modules' Configure...) so open over the settings window; before,
  they opened at the primary monitor's top left (Marvel's Saver.. and
  Slides... have no title bar to drag them by). A template's own position
  stays the real dialog manager's (relative to the owner; no module's
  template has `DS_ABSALIGN`). Without `--owner`, or hidden
  (`ADCONFIGHIDDEN`: the dialogs are parked off every monitor), the origin
  is (0, 0).
* Messages into the guest (Win32 → Win16): `WM_COMMAND` (`wParam` = id,
  `lParam` = MAKELONG(hwnd16, code)), `WM_CTLCOLOR*` → `WM_CTLCOLOR`
  (`wParam` = a DC wrapper, `lParam` = MAKELONG(hwnd16, CTLCOLOR_xxx); the
  colours it sets and the brush it returns go to the real DC), scroll
  messages (`lParam` = MAKELONG(pos, hwnd16)), the `*ITEM` structures as
  16-bit copies (`WM_DRAWITEM` with a DC wrapper), focus/activation (handles
  mapped), keys, mouse, `WM_TIMER`, `WM_PAINT`, and the dialog's and guest's
  own messages (DM_*, WM_USER + n). Guest classes named in a template or
  created on a real parent get a real class whose procedure forwards the
  same way (`WM_CREATE` with a 16-bit `CREATESTRUCT`); `SendMessage` to such
  a window calls the guest's procedure directly with the Win16 values.
  A guest's window procedure (a guest class's, or its subclass of a real
  control; never a `DLGPROC`, whose answer would be its `DWL_MSGRESULT`)
  also gets `WM_NCHITTEST`, its point on the guest's screen, and its answer
  is the real hit test's (an int: `HTTRANSPARENT` is AX = 0xFFFF, whatever
  DX holds). Intermission's frames (ANTSW's `ANT3DBOX`, `ANT3DGROUP`,
  `ASW3DBOX`, `ASW3DGROUP`) come first in every Configure template, so they
  lie above the check boxes, sliders and edits they frame, and answer
  `HTTRANSPARENT`: a click goes on to the control under them, as on Windows
  3.1. (Before, the real default procedure answered `HTCLIENT` for them: no
  option under a frame in a Far Side, Dilbert or Star Wars Configure dialog
  could be clicked, only reached with the keyboard.) A
  real `BM_GETCHECK`…`BM_SETSTYLE` to a guest class's window, or to a real
  button the guest subclassed, arrives as Win16's `WM_USER` + n, as Windows
  3.1's `CheckDlgButton`, `IsDlgButtonChecked` and `CheckRadioButton` sent
  it (ANTSW's check box takes `WM_USER` and `WM_USER + 1`), and a `GET`'s
  answer is the WORD in AX. `WM_SETFONT` brings a guest's control the
  dialog's font as a guest font object (`guest_font`), which ANTSW's check
  boxes, texts and frames draw their labels in (given 0, as a `DLGPROC`
  still is, they drew in the system font and their labels were cut short).
* Messages from the guest to a real control (Win16 → Win32): numbers by the
  target's class (`msg16_to_32`: EM 0x400+n ↔ 0xB0+n, BM 0x400+n ↔ 0xF0+n,
  LB 0x401+n ↔ 0x180+n, CB 0x400+n ↔ 0x140+n; the local-handle and
  word-break messages and `STM_*` have none), far pointers marshalled
  (strings, `LB_GETTEXT`/`CB_GETLBTEXT`, `EM_GETLINE`, tab stops,
  `LB_GETSELITEMS`, rectangles; owner-draw lists without `HASSTRINGS` pass
  item data), `EM_SETSEL`/`EM_LINESCROLL` repacked, `LB_DIR`/`CB_DIR` listed
  from the guest's disk (the drives as `DlgDirList` lists them).
* DC wrappers: a real HDC reaches the guest as a gdi16 DC for one message
  (or a `GetDC`/`BeginPaint` … `ReleaseDC`/`EndPaint`): an 8-bit key
  surface filled from the real pixels (nearest hardware colour), with the
  control's real font as a guest font object; the guest draws with the
  ordinary GDI shims (palettes and all), and the surface goes back to the
  real DC through the hardware palette — what a 256-colour display showed.
* `MessageBox` → a real `MessageBoxW`; `COMMDLG.GetOpenFileName`/
  `GetSaveFileName` → the real dialogs (hooks and templates ignored,
  logged), the chosen host path back as an 8.3 `H:\` path
  (`Vfs::host_to_guest`); `COMMDLG.ChooseFont` → the real font dialog
  (screen fonts; hooks, templates and printer fonts ignored, logged),
  started from the guest's `LOGFONT`, and on OK the `LOGFONT`, point size,
  font type and (`CF_EFFECTS`) colour written back — SWTEXT's Select Font;
  hidden (`ADCONFIGHIDDEN`), where no script line can pick a font, it is
  cancelled, logged; `WinHelp` → logged, 1; `WinExec("notepad
  <file>")` → the file copied into the upper layer and the real Notepad on
  that copy (not started when hidden or in memory); `EnumFonts` → the
  Windows 95 faces this host has.
* `ADCONFIGSCRIPT`/`ADCONFIGHIDDEN`/`ADCONFIGDUMP`/`ADCONFIGTIMEOUTMS`
  (`win32/config_script.hh`) attach once the guest has filled the dialog
  and it has been shown (a DialogBox shows when its queue first goes idle;
  MESSAGE3 builds its edit box on `WM_SHOWWINDOW`). The script's `PRESS`
  clicks where a user's click lands (through the frames above), and `CHECK`
  reaches a guest's check box (the real `BM_SETCHECK`, as `WM_USER + 1`).
* A guest failure inside a real callback is kept, every real dialog ends,
  and the failure is rethrown when the real call returns.

## Extension recipe: adding or finishing a shim

1. **Find the family file** for the module (`gdi16.cc` for `GDI.nnn`).
2. **Signature**: every entry of KERNEL/USER/GDI/MMSYSTEM/… has a row in
   `signatures16.cc`. `r.impl("GDI", "PatBlt", fn)` looks it up by name
   (case-insensitive). A row that is a `stub` (no argument list in the spec)
   is declared with `r.add(module, ordinal, name, Conv16::pascal_, ret16,
   arg_bytes, fn)`. Never hand-edit `signatures16.cc`: re-run the generator.
3. **Handler**: `[](Call16& c) { … c.ret(v); }`
   * arguments in **declaration order**: `c.w()`, `c.sw()`, `c.l()`,
     `c.sl()`, `c.ptr()` (a far pointer, `sel:off` packed);
   * guest memory through far pointers: `c.rt.rd16/wr16/rd32/wr32`,
     `read_str`, `write_str`, `read_bytes`, `write_bytes`, `read16<T>` /
     `write16<T>` (packed 16-bit structs in `gdi16.hh`); a bad pointer throws
     `GuestError16(fault)` — Win16 would have GP-faulted;
   * results: `c.ret(v)` (AX, or DX:AX for a function the table marks
     32-bit), `c.ret32(v)`;
   * the caller's DS (Local*): `caller_ds(c)`;
   * callbacks into the guest: `c.rt.call_far(proc, {w16(a), l16(b)})` —
     PASCAL order, nests freely, restores every register (also when a guest
     fault or hang throws out of it), so call `c.ret` **after** any nested
     call. A callback may `Throw` to a `Catch` made outside the shim: the
     host frames in between unwind (`GuestUnwind16`, not a `std::exception`)
     and the guest resumes at the `Catch` — let it pass through your handler;
   * a handler that sets CS:IP/SS:SP itself (Throw, SwitchStackTo) calls
     `c.take_over()`.
4. **State**: `struct MyState : RuntimeState16 {…}` + `c.rt.state<MyState>()`.
5. **GDI**: `c.rt.state<Gdi16>()` — `get`/`dc`/`host_dc`, `key(hdc, colorref)`
   (the only form a colour may reach real GDI in), `sync(hdc)` before real
   GDI draws, `dc_palette`, `dc_surface`, `display()`. On a DC with DIB
   colour semantics (`Dc16::dib_header`: a DIB driver DC or one compatible
   with it) `key` matches the DIB driver's way (`dib_index`); read pixels
   back with `surface_rgb`. Taking a DIB DC's `host_dc` schedules the
   `GdiFlush`.
6. **Time**: only `c.rt.tick_count()` (GetTickCount: 55 ms steps),
   `c.rt.time_ms()` (timeGetTime), `c.rt.local_filetime()`, `c.rt.clock_us()`.
   Every read advances virtual time (see below), so busy-waits end.
7. **Test**: `tests/test_win16.cc` builds guest code as bytes (see the
   `Machine` fixture); `adw_win16_tests --assets` loads OLDMOD16/AD_SND.
   `tests/test_sound16.cc` (`win16.sound`) tests MMSYSTEM's sound half
   against a scripted engine (exact times) and the real one.

## Virtual time

Headless runs are deterministic. Every clock read nudges virtual time by
`read_step_us`, plus one µs per `insns_per_us` instructions executed since
the previous read and `api_cost_insns` per API call (a notional 100-MIPS 1996
CPU on which a USER/GDI call costs 5 µs). Modules that draw until the tick
changes (SATORI) or calibrate on it (AD_RSRC, EINSTEIN) thus do the amount of
work they did on real hardware. That work runs *inside* the frame period: a
read sees `max(time already reached, frame × step)` plus its nudge, so a
frame that does less than a period's work costs no extra time (the lane
fills the period with DRAWFRAMEs, `ne16/lane.hh` "Pacing"), and only work
beyond the period pushes the clock past the frame grid. With `insns_per_us`
0 the nudges accumulate on the core clock instead (the lane's original
model, `ADMIPS=0`). `settle_time()` (the lane calls it as each frame ends)
charges the instructions run since the last read to that frame, so a module
that reads no clock while drawing does not carry a frame's work onto the next
frame's grid line. Streamed runs follow the wall clock (the instruction term
is off) from `start_frames()` on — the lane's first frame, when the realtime
core clock starts; before it (the module's load, where AD_RSRC, EINSTEIN,
GLOBE and Om Appliances calibrate on the tick count) reads are modeled as
headless, and the wall clock then continues from the time that took, so time
never goes back. `work_insns()` — instructions, API costs and
`pixel_cost_insns` per pixel a GDI blit or fill writes (`charge_pixels`; a
flood fill charges exactly the pixels it paints, `flood_pixels`, above) — is
what the lane's DRAWFRAME budget counts; pixels never move the clock.

`set_deadline(us, fn)` runs `fn` once, at the first API call (or retrace-port
read) at or after virtual time `us`, before the call does anything. The
Classic lane ends a frame there when a DRAWFRAME outlasts the period: `fn`
switches from the fiber the call runs on back to the host, which presents the
screen and resumes the call next frame (`ne16/lane.hh`, "Long calls").

`set_audio_hook(fn)` / `set_audio_due(us)`: `fn` runs at every API call at or
after `us` (after the scanout and deadline hooks, before the call); MMSYSTEM
keeps `us` at its next event (sound16.cc). `wait_until_us(us)` is a call that
blocked until virtual time `us` (a synchronous `sndPlaySound`): while the
yield hook (`set_yield_hook`, the Classic lane's long-call fiber) can end the
frame there, frames are presented and the call resumes on later ones until
guest time reaches `us` — the screen holds, as it did — otherwise the time is
charged at once (the modeled clock jumps; a realtime or read-step clock is
offset). Never a real wait.

The display is refreshed at a virtual 70 Hz, the timing the 0x3DA retrace
bit follows: `set_scanout_hook` runs at the first API call after each
refresh boundary (`peek_us()` includes instructions not yet charged), so a
lane can see what a monitor showed *during* a call — the Classic lane uses it
for content drawn and erased inside one DRAWFRAME (`ne16/lane.hh`, "Frames").

## Debugging

`ADTRACE` categories: `api16` (every call, raw argument words, result, caller),
`mod16` (loading, LibEntry, DllEntryPoint, GetProcAddress, the resident
system modules GetModuleHandle finds), `res16` (FindResource, LoadResource
and FreeResource with the use count, AccessResource), `file16` (files and profile strings), `dos` (INT 21h),
`throw16` (Throw with a BP-chain backtrace), `prof16` (host time per shim, at
exit), `pace` (the lane's DRAWFRAMEs, work and virtual time per frame, and frames that ended inside a call), `user16` (also host-posted messages, and what the guest pump dispatches), `sound` (every MMSYSTEM sound call
with its virtual time: voices, streams, MCI commands and their results,
timer events set and killed, notifies and callbacks as they are delivered —
but not the two that come hundreds of times a second: the timer periods and
the short MIDI messages), `timer16` (each multimedia timer procedure call:
the event, its procedure and `dwUser`, the period's due time and the virtual
time it was delivered at), `midi16` (each `midiOutShortMsg` with the time it
is dated at), `debug16` (`OutputDebugString`), `lane`, `bt16` (every call with the BP chain of
its callers: which module code reached a shim), `dib16` (per `StretchDIBits`
of an 8-bit DIB: the source rectangle's index histogram, what it became, the
DIB's colour table and the DC's palette; DIB driver DCs as they are made,
and each RGB colour matched on one with the pixel value it became; each
`GetDIBits` request, "GetDIBits(hdc, bitmap h WxH D-bit, scans S+L, usage
U): header N bytes, WxH, B bpp"), `mem16` (the selector calls that make,
free or change a selector, and their refusals; each segment register the freed-selector rule nulls:
"ES 00D7 was freed by KERNEL.17 GlobalFree: null on return"), `input16` (hooks installed and
called with their results, input posted/removed/dispatched/consumed,
`FindWindow("Sleep")`, wake), `dlg16` (configure mode: every message
forwarded to a guest dialog or window procedure, dialogs opened and ended,
classes made real, control messages without a Win32 form). Lane knobs `ADWATCH16=sel:off` (log every
change of that byte and the instruction that made it) and
`ADSTEP16=cs:lo-hi` (log every instruction executed in that range). Faults
report CS:IP as `MODULE seg:off` plus DS/ES/SS limits, and `log_state` prints
the registers, stack words and BP-chain frames where the failed call was
(captured before the unwinding restored the caller's registers);
`research/win/dis/<file>.asm` has the listings.

Build + test: `AD_BUILD_DIR=build/win-<key> AD_COMPONENTS="host/core;host/cpu;host/loader;host/win32;host/win16;host/ne16" bash tools/build.sh`
