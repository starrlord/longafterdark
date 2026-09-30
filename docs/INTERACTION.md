# Long After Dark — interaction

**Status: implemented and verified (2026-09-27).** Where the code settled
on something other than the plan below, the section says so; the component
READMEs (`scr/README.md`, `host/core/README.md`, `host/win32/README.md`,
`host/win16/README.md`) are authoritative for how it behaves.

How the saver decides whether a key, click or mouse move wakes it or goes to
the module; how module-owned configuration dialogs run; where the modules'
own settings and data persist; how the desktop reaches the first frame; and
the small fixes that ride along. `DESIGN.md` §8 summarizes this file; it
extends the host protocol of `DESIGN.md` §1 and the ABI of `ABI.md`
(§2.4 flags, §2.5 messages, §2.8 input, §2.10.4 buttons, §2.11 settings,
§3.1–§3.4 the Classic path).

Status marks as in `ABI.md`: **VERIFIED** (an address in our own disassembly
shows it), **EMPIRICAL** (observed by running the module in our host;
command and result given), **UNVERIFIED** (inference). Addresses: PE at the
preferred base, NE as `seg:off`. The AD 3.x host binaries (`ADW30.EXE`,
`ADTASK.DLL`, `ADHOOK.DLL` from `research/win/pkg/ad32/extracted/AFTERDRK`)
were disassembled with `research/win/nedis.py` into a scratch folder, not
into `research/win/dis`.

---

## 0. Decisions at a glance

| Topic | Decision |
|---|---|
| Who decides "wake or play" | The `.scr`, from a **status record** each host publishes in a shared-memory page (§3.4) plus **input sequence numbers** (§3.2). stdout still carries frames only. |
| Interactive | A module is interactive when the host says so: AD4 `WantEvents` (block `+0x08` bit 0), AD3 result `0x0E` toggle. Input a module *consumes* without being interactive (a WH_KEYBOARD hook that eats the key, a module that pulls the saver window's messages) is reported per input line, and the `.scr` waits for that verdict before exiting (§4.3). |
| Caps Lock | Never wakes the saver (as in AFTERDAR.SCR). The `.scr` sends `KEY 20 …` and `CAPS <state>`; the initial state goes in `ADCAPS` so modules latch it at start (§3.1). |
| Num Lock | Never wakes the saver either (as in AFTERDAR.SCR and After Dark 2.0). The `.scr` sends `KEY 144 …` as any key; since the seventh release also `NUMLOCK <state>`, to a host whose `--capabilities` says `numlock=1`, and the initial state in `ADNUMLOCK`: Star Trek: The Screen Saver's Final Exam begins its exam on a change of it, and ends it with a wake request (result 5) that ends the saver (§3, §4.1, §4.4, §5.2). |
| Exit gestures | Not playing: any key except Shift/Ctrl/Caps Lock/Num Lock, any click or wheel, a move past 10 px, switching away. Playing: Caps Lock toggles the game off (then any input exits), **Alt or F10 exits at once**, switching away exits. |
| Multi-monitor | One **input owner**: the primary monitor's window. Only its host gets `KEY`/`CAPS`/`NUMLOCK`/`MOUSE`; only its status is read. The cursor is confined to it while playing. |
| Module buttons | `adhostwin --configure <module> --button <slot> --owner <hwnd>` runs the module's own button handler; its `DialogBox*`/`MessageBox`/`GetOpenFileName` become **real** dialogs owned by the settings window, forwarding to the guest dialog procedure (§6). |
| Module state | A per-user, per-package **writable overlay** over the guest's `C:\WINDOWS` and `C:\AFTERDRK` (`C:\SAVER` for Star Wars Screen Entertainment's Intermission modules) in `ADSTATE`, which the `.scr` always passes (`state\` next to `settings.ini`); without it a host keeps the overlay in memory, so headless runs stay deterministic and write nothing (§7). |
| Desktop seed | The `.scr` captures each monitor before its windows appear, writes a delete-on-close P6 at the emulated size and passes `ADSEEDIMG` to that window's first host only (§8). |
| DOS Shell | Not reproducible on the current build (§9.1); ships with exit-reason logging and a 5-minute regression. |

---

## 1. What the 1996 hosts did

### 1.1 AFTERDAR.SCR (After Dark 4.0) — VERIFIED

Two windows take input. The **saver window** (SCRNSAVE-style,
`WindowsScreenSaverClass`) and a zero-size child **RunnerWindow** created by
the runner thread, which holds the focus (`ABI.md` §2.8; `WM_SETFOCUS` on the
saver window re-focuses the runner, `0x40150c`).

* **Wants-events flag.** A global `[0x4130b8]`, refreshed after *every*
  module call by `sub_401c73` (`0x401cb9..0x401cdc`): it calls the module
  class's vtable `+0x10` (wants events), `+0x14` (cursor visible →
  `[0x412cf8]`) and `+0x18` (may be switched by the randomizer →
  `[0x4130a8]`). For AD4 modules those read block `+0x08` bits 0/1/2
  (`ABI.md` §2.4); for Classic modules result `0x0E` toggles wants-events and
  `0x11`/`0x12` show/hide the cursor (`0x401f6f`, `ABI.md` §3.1). The flag is
  cleared when a module unloads (`0x401c44`) and at load (`0x401a1f`).
* **Keys** arrive at the runner window proc `0x40282c`:
  * `WM_KEYDOWN` (`0x4028c0`): wants events → module message 7 with
    `wParam` (every key, no exemptions). Otherwise VK 0x14 Caps Lock,
    0x90 Num Lock, 0x10 Shift and 0x11 Ctrl are dropped; any other key is
    re-posted to the saver window, whose `DefScreenSaverProc` closes.
  * `WM_KEYUP` (`0x402914`): wants events → message 8; otherwise re-posted
    (and ignored by `DefScreenSaverProc`).
  * `WM_SYSKEYDOWN/UP` (`0x402951`): re-posted only when the module does
    **not** want events (then `DefScreenSaverProc` closes on
    `WM_SYSKEYDOWN`); while it wants events they are **swallowed**.
* **Mouse** arrives at the saver window proc (`0x4010f5`):
  * `WM_MOUSEMOVE` (`0x4014a6`): handled (never closes) while the module
    wants events; otherwise `DefScreenSaverProc` (`0x406421`) closes when
    `|dx| + |dy| > 4` (`[0x4109f4]` = 4) against the position stored at
    `WM_CREATE` (`0x4065bf`), re-storing it after each close attempt
    (`0x406462`).
  * `WM_ACTIVATEAPP` with `wParam` 0 closes (`0x4063f1`), whatever the
    module wants.
  * `WM_LBUTTONDOWN..WM_RBUTTONDBLCLK` (`0x401472`): while it wants events,
    re-posted to the runner, which sends messages 10–15 (`0x4029d9`,
    `msg − 0x1F7`; the SDK dispatcher ignores them, `ABI.md` §2.5);
    otherwise `DefScreenSaverProc` closes (on L/R/M button down).
  * `WM_SETCURSOR` (`0x401501`) keeps the cursor only while `[0x412cf8]`.
* **Randomizer** switches a module only when its time is up **and** (it
  does not want events **or** bit 2 is set) (`0x40190b`).
* Consequence: the only ways out of a game were to let the module drop
  wants-events (Caps Lock again, for every SDK game) or to leave the desktop
  (Ctrl+Alt+Del). There was no host-level escape key.

### 1.2 The AD4 engine and modules — VERIFIED

* `PortableModule::WantEvents(bool)` (`ADXPL510 0x42bd17`) sets / clears
  block `+0x08` bit 0. `UserInput::SetMode(m)` (`0x413263`) calls
  `WantEvents(m != 0)`; `UserInput`'s destructor calls `WantEvents(0)`
  (`0x4131d3`).
* `PortableModule::GetCapsLockChange()` (`0x42bdb1`): `GetKeyState(0x14) & 1`
  (the **toggle** bit) compared with the value stored last time; returns 1
  on any change. The `PortableModule` constructor calls it once
  (`0x42b70d`), latching the state at PREINITIALIZE. So a module reacts to
  Caps Lock *changes*, whatever the state was when the saver started.
* The engine polls `GetAsyncKeyState(VK_LBUTTON/VK_RBUTTON)` and
  `GetCursorPos` (`UserInput::GetMousePoint 0x413502`, `CheckMouse
  0x41354b`) and `GetKeyState` for Shift/Ctrl/Alt (`KeyModifiers
  0x41334f`); keys from messages 7/8 go into a 32-entry buffer
  (`UserInput::GetNextKey`, `KeyDownNow`).
* Games: **RODGER** (`0x41588a`: `if (GetCapsLockChange() &&
  !IsDemoMode())` toggle the game and `SetMode(game)`), **MARBLES**
  (`0x412f62`, `0x413b88`), **RPS** (`0x41e51d`), **TURTLE** (`0x411240`,
  `0x4113bf`; the editor). These four call `SetMode`/`WantEvents`.
  Fourteen more AD4 modules import `GetCapsLockChange` without ever wanting
  events (FISH, SWIRLING, TIME, CRITIC, MESSAGES, TOASTERS, …; PSYCHO reads
  `GetKeyState` itself): Caps Lock does something (or nothing) and every
  other key still wakes the saver.
* In the `/p` preview AFTERDAR.SCR clears block bit 4, so `IsDemoMode()` is
  true and RODGER never goes interactive there.

### 1.3 Classic modules under AFTERDAR.SCR — VERIFIED

`ModuleMessage3216(2)` returns the module's DRAWFRAME result; `0x0E`
toggles wants-events, `0x11`/`0x12` show/hide the cursor (`0x401f6f`). Keys
and mouse are **never delivered** to a Classic module (`ABI.md` §3.1); it
polls `GetAsyncKeyState`/`GetKeyState`/`GetCursorPos`, installs a keyboard
hook, or reads the message queue itself (§1.5).

### 1.4 The AD 3.x host (ADW30.EXE + ADTASK.DLL + ADHOOK.DLL)

* **VERIFIED** `ADTASK` turns a module's result into a flag word through a
  table at `3:0000` (read by `3:025e..3:0260`): 1 → `0x0001`, 5 → `0x0010`,
  7 → `0x0002`, 8 → `0x0008`, **`0x0E` → `0x0020`**, `0x0F` → `0x0004`,
  `0x10` → `0x0040`, **`0x11` → `0x0100`, `0x12` → `0x0200`**; 3 takes the
  restart path (`3:0206`), 10–13 the palette path (`3:023c`). So the AD 3.x
  host saw the same `0x0E`/`0x11`/`0x12` protocol OLDMOD16 forwards.
* **VERIFIED** For a module's button in the AD 3.x control panel, `ADW30`
  disables its own window, forwards the command to the module through
  `ADTASK.DOMESSAGE` (ord 18), then re-enables and re-activates itself
  (`4:27cf` `EnableWindow(…, 0)`, `4:2813` `DOMESSAGE`, `4:2825`
  `EnableWindow(…, 1)`, `4:2830` `SetActiveWindow`). `ADTASK` gives
  selection/preinit/button messages (5, 12, 7–10) the desktop's DC
  (`3:004b..3:0071`), as OLDMOD16's `BUTTONPUSHED16` does.
* **VERIFIED** `ADHOOK.DLL` holds the idle/hot-key machinery:
  `KEYBOARD_PROC` (ord 101) matches the sleep/wake hot key with Ctrl/Shift
  state (`3:04ce..3:05e3`); `SYS_MSG_PROC` (ord 102) traps Alt+Tab and
  Alt+Esc (`WM_SYSKEYDOWN/UP` with VK 9 / 0x1B, `3:05e6..3:0681`) while
  app switching is trapped.
* **UNVERIFIED** How `ADW30`'s blanker window itself routed keys while a
  module was interactive (MFC, not traced). Our host follows AFTERDAR.SCR's
  rules (§1.1), which ran the same AD 3.x modules through OLDMOD16.

### 1.5 How AD 2/3 modules take input (corpus)

| Mechanism | Modules | Evidence |
|---|---|---|
| Return `0x0E` on a Caps Lock change (interactive on/off) | YBYH (You Bet Your Head), SIMPTRIV, `tt` FRANKEN, `tt` MIMEHUNT, HOW2DRAW (also `0x11`, cursor) | **EMPIRICAL**: `ADTRACE=lane` with a stdin script (900×`GO`, `KEY 20 1`, `CAPS 1`, 6×`GO`, `KEY 20 0`, …, `CAPS 0`) logs `MODULEMESSAGE16(DRAWFRAME) -> 14` once per toggle |
| `WH_KEYBOARD` hook (`SetWindowsHook(2, …)` / `SetWindowsHookEx(2, …)`) | YBYH `3:00cc`, HOW2DRAW `2:1444`, MIMEHUNT `5:00a8`, engines ADXPL40 `22:0b44` (Totally Twisted) and ADXPL310 `5:cdf6` (Simpsons); WMORPH `1:16d2`/`1:16f0` | **VERIFIED** call sites. Before this design `SetWindowsHook` returned 0 and these modules never received keys (the trivia answers 1/2/3 were lost); the ne16 lane now chains `WH_KEYBOARD` hooks (§5.2) |
| `FindWindow("After Dark", NULL)` then `PeekMessage(hwnd, 0, 0, PM_REMOVE)` loops, keeping `WM_KEYDOWN`/`WM_KEYUP` | LUNATIC (`26:008f`, `26:0187`, `26:01b0..26:01ed`) | **VERIFIED**. The AD 2/3 host's blanker window; the module takes the keys out of the host's queue so the host never sees them. Under AFTERDAR.SCR the class does not exist (inference: Lunatic was not playable under AD4) |
| `GetAsyncKeyState(vk) & 0x8001` polling | LUNATIC (`26:01f5..`), many others | **VERIFIED**: bit 0 ("pressed since the last call") matters |
| Caps Lock one-shot (`GetKeyState/GetAsyncKeyState(0x14)`), no `0x0E` | TUNNEL, CONFETTI, SATORI, NIRVANA, FISHPRO, MANDELBR, STRANGE, TOILET(S), CHAM, CS, ARTIST, RAY, TOAST3, BADDOG3, SIMPCLOK, GRAMPA, HOMEREAT, PHYSICS, SIMPFILE, SNOWBALL | **VERIFIED** call sites + **EMPIRICAL** (no `0x0E`) |
| Caps Lock as "next" (`GetKeyState(0x14) & 1`, called by the module itself, or through `AD_MOD.DLL` for MISSION and PANELS), no `0x0E` (Star Trek: The Screen Saver, since the seventh release) | FRONTIER (the next scene), HORTA (a new cavern), IONSTORM (the next colours), MISSION (the next scene), PANELS (the next panel), PLANETS (the next planet), SCOTTYS (the next schematic), SICKBAY (the next case) | **EMPIRICAL**: a `CAPS 1`/`CAPS 0` script against a run without it (the content survey) |
| Num Lock's toggle (`GetKeyState(0x90) & 1`) latched at start; a change starts an exam that hooks the keyboard (`WH_KEYBOARD`) and returns `0x0E`; a mouse move ends it with `0x0E`, then 5 (§1.7) | FINAL (Star Trek: The Screen Saver's Final Exam) | **VERIFIED** `FINAL.AD 3:732e`; **EMPIRICAL**: the host's scripted exam (`ne16.startrek`) |
| A Caps Lock game: Caps Lock on installs a `WH_KEYBOARD` hook and returns `0x0E` (status `0x29`: interactive, key filter, ready; source 2), the Wishing Star cursor appears and Pinocchio walks to the mouse; Caps Lock off removes the hook and returns `0x0E` (status `0x20`) (the Disney Collection, with the twelve releases) | PINOCCHI (Pinocchio), the only module of ADXPL100's that returns `0x0E` | **EMPIRICAL**: `KEY 20` + `CAPS 1` / `CAPS 0` in a lockstep script (the Disney survey, `research/win/pkg/disney/content/pinocchio/`) |
| Caps Lock as "next", no `0x0E` and no hook (status `0x20` throughout, nothing eaten) (the Looney Tunes and the Disney Collection, with the twelve releases) | Looney Tunes: ACMESHOP (the next product), CART101 (the next lesson), CONDUCKT (the next colour scheme), LTMESSGS (the next message), MARVIN and RABBITRN (the next scene), FROG, SAM and WOCKETS (they change too; PEPE, PUTTYTAT and TAZ do not react); Disney: BEAUTY, CHECAT, JUNGLE, MERMAID (they change from the next frames) | **EMPIRICAL**: `KEY 20`/`CAPS` lines at frames 300 and 700 with `ADSTATUSLOG=1` against a run without them (the surveys) |

The About texts announce the games: "Caps-lock gets you in and out of
interactive mode" (Rodger Dodger), "hit CAPS LOCK … hit 1, 2, or 3"
(You Bet Your Head), "Press 'Caps Lock' to start the interactive session
… pressing 1, 2, or 3" (Simpsons Trivia), "CapsLock: Take control of the
crosshairs — click the mouse" (Mime Hunt), "Press Caps-lock to display the
edit window" (Magic Turtle), "press Caps-lock and click and drag the pins"
(Marbles), "Press Caps-lock to choose your fighter" (RPS), "Depress the
Num Lock key to begin, and type the number of your answer. Move the mouse
to end the exam." (Final Exam), and "click the Caps Lock key … Pinocchio
will follow" (Pinocchio). The ScreamSavers, Snoopy's and Marvel's modules
read neither the keyboard nor the mouse (none imports `GetKeyState`,
`GetAsyncKeyState` or `GetCursorPos`), so none of them is interactive.

### 1.6 Module buttons — VERIFIED

* **AD4** (`ADPAGE 0x9001fbf`, `ABI.md` §2.10.4): a fresh zeroed block with
  `cbSize`, `+0x0C` = the property page's HWND (owner for dialogs), `+0x14` =
  `hModule`, `+0x3C` volume, `+0x40` values, `hDC` 0; `Module(0)`,
  `Module(6, slot)`, `Module(1)`. No PREINITIALIZE, BLANK or CLOSE.
* **Classic** (`OLDMOD16 BUTTONPUSHED16 1:09e4`, via `ButtonPushed3216`):
  load AD_SND and the module; `AD_SYSTEM+0x26` = owner HWND;
  `MODULE(5)` on `GetDC(0)`; unless it returned 1 or 7, copy the four
  values into `AD_MODULE+6` (`1:0acb..1:0afc`) and `MODULE(7 + slot)`
  (`1:0b01`); copy back a replaced error text; `FreeLibrary`; unload
  AD_SND. **No CLOSE, and the values are not copied back**: modules persist
  their button results themselves.
* **Intermission** (Star Wars Screen Entertainment; `ABI.md` §3.8.4): the
  control panel's **Confi&gure...** loads the module through the reader
  (`SAVERMAIN(10)`), sets the record's window to the panel's dialog, sends
  `SAVERMAIN(8)` — `IMIMXPLY.IMQ`'s `DialogBox(hLib, "DIALOGBOX", owner,
  SAVERDLGPROC)`, the module's own modal dialog (the reader answers 0 for a
  module without a `SAVERDLGPROC`) — and frees it (`SAVERMAIN(11)`); no
  QUERY, no START (`INTERMIS 2:1cb2..2:1d7d`). The dialog's OK writes the
  module's keys to `SWSE.INI` itself.
* What the buttons call and where they persist (static imports and
  strings):

| Module (lane) | Button(s) | Dialog APIs | Persists through |
|---|---|---|---|
| FISH, RAIN, MESSAGES (pe32) | Select Fish…, Creatures, Custom | `DialogBoxParamA`, `GetDlgItem`, `Get/SetWindowTextA`, `SendMessageA`, `EndDialog` | `WriteModulePrefs` → `WritePrivateProfileStringA` into `MODULES.INI` in the AD Ini dir (`ADXPL510 0x42c23a`, `ABI.md` §2.11) |
| POINTS, SLOWBURN, SWIRLING (pe32) | Music Choice… | `DialogBoxParamA`, `SendDlgItemMessageA`, `SetDlgItemTextA`, `GetOpenFileNameA` | `WriteModulePrefs` |
| CRITIC (pe32) | Pictures | `GetOpenFileNameA` ("double-click any file to select that folder"), `FindFirstFileA` | `WriteModulePrefs` |
| ARTIST, RAY (ne16) | Select Image, Shapes | `COMMDLG.GetOpenFileName`, `OpenFile` | `AFTERDRK.INI`/`MODULES.INI` via `WritePrivateProfileString` (ARTIST) |
| BUGS, FISHPRO (ne16) | Bug Type, Select Fish… | `DialogBoxParam` | engine/helper prefs (trace with `ADTRACE=file`) |
| MESSAGE3, NONSENSE, SLIDE, GLOBE, WMORPH, LUNATIC (ne16) | Edit / Select, Edit Names…, Slides…, Map…, Edit…/Revert, Clear Scores/Keys… | `DialogBox`, `EndDialog`, `MessageBox` (LUNATIC's "Do you really want to clear…") | `MESG_AD3.DAT`, `NONSENSE.TXT`, `LunData.dat` (`_lcreat`/`_lwrite`/`OpenFile`), `MODULES.INI`/`AFTERDRK.INI`/`AD_PREFS.INI`/`WriteProfileString` |
| `tt` MESSYGES, `ad32` LOGO, `ad32` BUGS, `simpsons` HOW2DRAW (ne16) | Edit Custom, Picture…, Bug Type, Help | `DialogBox` (+ engine prefs) | as above |
| the 14 `swse` modules (ne16, Intermission) | Configure... | `SAVERMAIN(8)` → `DialogBox` of `"DIALOGBOX"` (named through the module's `NAMETABLE`), with INTRMLIB's `ANT3DBOX`/`ANT3DCHECK`/`ANT3DSCROLL`/`ANT3DTEXT`/`ANT3DONEORMORE` controls and SWSE's animated credits box; Scrolling Text adds `GetOpenFileName`, `GetSaveFileName` and `ChooseFont` | `WritePrivateProfileString` into `SWSE.INI` in the Windows directory, one section per module (Scrolling Text also writes its edit box to `SWTXEDBX.TXT` there) |
| `startrek` COMMS, SOUNDER (ne16, After Dark 2.0) | Edit Custom... (Communications, MODULE 10), Sounds.. (Sounder, MODULE 9) | `DialogBox`: "Edit Message" (a multi-line edit, id 103); "Select Directory", whose folder list is `DlgDirList(…, DDL_EXCLUSIVE \| DDL_DRIVES \| DDL_DIRECTORY)` beside the folder's `*.WAV` (§6.2) | `WritePrivateProfileString` into `AD_PREFS.INI`: `[Communications] MessageText`, `[Sounder] SoundPath` (nothing for a folder without a `.WAV`) |
| `marvel` MARVEL (ne16, After Dark 2.0d; with the twelve releases) | Saver.. (index 0, MODULE 7), Posters... (index 1, MODULE 8) | `DialogBox`: Saver.. is the image selection, with the module's own "Images" window of nine owner-drawn thumbnails, All/None, Display In Order/Random, Show Captions and Create Poster On Wakeup; Posters... has Install, Uninstall, Info... (the poster's description) and Done | Saver.. OK rewrites the image catalog `C:\AFTERDRK\MRVLIMAG\MRVLIMAG.ADC` (the whole file is copied up into the state overlay; the display flags and each poster's selection word are written from the dialog, ABI.md §3.11). Posters... → Install decodes the poster into `C:\AFTERDRK\MRVLIMAG\MARVEL.BMP` and writes `WIN.INI [Desktop] TileWallPaper`; Create Poster On Wakeup writes `MARVEL.BMP` at every wake. Both land in the state overlay: `SystemParametersInfo` changes nothing in the runtime, so **the wallpaper features have no effect outside the emulator** |
| `looney` LTMESSGS (ne16, the Looney Tunes' Messages; with the twelve releases) | Edit Custom... (index 3, MODULE 10) | `DialogBox`: "Enter your custom message:" (an edit, id 101, "Your Message Here"; OK, Cancel) | `WritePrivateProfileString` into `MODULES.INI`: `[Looney Messages] CustomA`, which Foghorn, Elmer or Speedy then says when the message control picks the custom one |

The merged catalog of the six releases (216 modules) has 58 button
controls on 53 modules, from 35 distinct binaries: the 202 After Dark
modules have 44 on 39 modules (21 binaries; counted in the catalog when
Star Wars Screen Entertainment was added, where this line had said 39 and
22), and each of the 14 Intermission modules has one. With the seventh
release's two (232 modules) it has 60 on 55 modules, from 37 binaries, and
with the twelve releases (284 modules) 63 on 57 modules, from 39 binaries:
Marvel's two and the Looney Tunes' Messages' one; ScreamSavers, Snoopy's and
the Disney Collection's modules have none (`--configure --button 0` on one
of them exits 1, "control 0 is not a button"). Marvel's thumbnails are not
scriptable, in either dialog: their nine buttons (ids 1007–1015) are
children of the Images window (1006), and a configure script's `CLICK` goes
to the dialog (§6.6). Saver..'s All (1000) and None (1001) are the dialog's
own buttons, which a script can click.

### 1.7 After Dark 2.0 (`AD.EXE` 2.0b, Star Trek: The Screen Saver) — VERIFIED

Added with the seventh release (`ABI.md` §3.9; listings in
`research/win/pkg/startrek/lane/dis/` and `…/content/dis/`, gitignored):

* `AD.EXE` forwards no input to a module, as AFTERDAR.SCR forwards none to
  a Classic one. Its idle and wake detection is `AD_LIB.DLL`'s journal
  hook (`SAVERHOOK`, `2:0086..2:00b3`), which did not wake on Num Lock,
  Shift, Ctrl, Caps Lock or the four arrow keys (VK `0x25`–`0x28`), and
  passed every key while a module played (game mode, toggled by result
  `0x0E`). The saver's exemptions (§4.1) differ only in the arrows, which
  no Star Trek module uses outside Final Exam's exam, where every key is
  the game's.
* DRAWFRAME's result 5 posted `AD.EXE` its own wake message, 0x7EE: the
  module's wake request. Final Exam is the one module that returns it.
* **Final Exam** (`FINAL.AD`, the Starfleet Academy exam) latches
  `GetKeyState(VK_NUMLOCK) & 1` as it starts (`3:732e`) and cycles in review
  mode (a question, its timer, the answer; "PRESS NUM LOCK TO BEGIN EXAM").
  A change of the toggle restarts the module (result 3, handled inside its
  own `MODULE`), and the new instance hooks the keyboard
  (`SetWindowsHook(WH_KEYBOARD, …)`, a hook that records each key down and
  passes it on) and returns `0x0E`: the exam, interactive. Its answers are
  the keys 1–4, on the top row or the keypad (with Num Lock off the
  keypad's 1–4 are End, Down, PgDn and Left, which it takes too). Num Lock
  again removes the hook and returns `0x0E` (back to review). A mouse move
  (`GetCursorPos`) removes the hook, returns `0x0E`, and then 5: the wake.
  The exam is offered only while `AD_SYSTEM+0x1E` is not 1 and `+0x28`
  (the multi-module mode) is 0; the bridge writes 0 to both.

---

## 2. Model

* **Input owner.** In `/s`, the window on the primary monitor (the first in
  `/s` order). Only its host receives input lines and only its status is
  read. Other monitors' hosts run untouched (they are "other machines").
  If the primary window runs no host (a "not imported" message), no input is
  forwarded and every input exits, as today.
* **Input line.** `KEY`, `CAPS` and `MOUSE` are *input lines*, and since
  the seventh release `NUMLOCK` (§3.2). Each host
  numbers them 1, 2, 3 … in the order it reads them. The `.scr` keeps the
  same counter per host, so it knows every line's number without an
  acknowledgement.
* **Status.** After every completed step the host publishes: frames done,
  the number of the last input line applied before that step
  (`input_applied`), the highest input line the guest *consumed*
  (`input_eaten`), and flags (interactive, cursor, rotate-ok, key-filter,
  wake-request). §3.4.
* **Consumed** means the module took the input as its own: an AD4 message
  7/8/21/24/27/30 delivered while it wants events; any input while an AD3
  module is in `0x0E` mode; a key a `WH_KEYBOARD` hook returned non-zero
  for; a saver-window message the guest removed from its queue without
  dispatching it back to the saver window.

---

## 3. Protocol additions (host side, `DESIGN.md` §1)

### 3.1 Environment

| Variable | Meaning | Default |
|---|---|---|
| `ADCAPS=0\|1` | Caps Lock toggle state at start, applied to `InputState.caps` before `Lane::init` (like `ADCVSET`). Modules latch it at PREINITIALIZE (§1.2), so a first `CAPS` line must never be needed to establish it. | 0 |
| `ADNUMLOCK=0\|1` | Since the seventh release: Num Lock toggle state at start, applied to `InputState.numlock` before `Lane::init` (and in `--configure`), like `ADCAPS`. Final Exam latches it as it starts and begins its exam when it changes (§1.7), so a first `NUMLOCK` line must never be needed to establish it. The start log says `num lock on`. | 0 |
| `ADSTATUSHANDLE=<n>` | Decimal (or `0x` hex) value of an **inherited** handle to a pagefile-backed section of at least 4096 bytes. The host maps it read/write and publishes the status record (§3.4) there. A value that does not map is logged once and ignored. | none |
| `ADSTATUSLOG=1` | Also print `STATUS <frame> flags=0x<hex> applied=<n> eaten=<n> src=<s>` on stderr at frame 0 and whenever flags, `applied` or `eaten` change. For tests and `AD_SCR_HOSTLOG`. | off |
| `ADSTATE=<dir>\|:memory:` | The per-user state root (§7). Unset or `:memory:`: the overlay lives in memory for the process (headless runs, censuses, `FBHASH` never see or write user state). Exception: `--configure` with `ADSTATE` unset uses `%LOCALAPPDATA%\LongAfterDark\state`. The `.scr` passes it on every spawn (§7.1). | in memory |
| `ADSEEDIMG=<spec>` | Already implemented in both lanes (`win32/display.hh`, "desktop seed"). The `.scr` now passes it (§8). | unset = black |
| `ADCONFIG*` | Configure-mode test hooks (§6.6). | off |

### 3.2 Command changes

* `MOUSE <x> <y> <buttons>`: `buttons` becomes a **bitmask**, 1 left,
  2 right, 4 middle (0..7; the old 0/1 keep their meaning). The lanes answer
  `GetAsyncKeyState/GetKeyState(VK_LBUTTON/VK_RBUTTON/VK_MBUTTON)` from it.
* `Command` gains `uint64_t seq`: the input-line number (1-based) for
  `key`/`caps`/`mouse`, 0 for everything else. `InputState` gains
  `uint64_t input_seq` (the last one applied).
* **Every down is seen.** A `KEY <vk> 0` (or a `MOUSE` releasing a button)
  that arrives while the matching down has not yet been visible to a
  completed step is held and applied right after the next step. A
  quick tap batched into one `GO` (lockstep, slow step) therefore still
  shows `0x8000` to pollers for a frame. The held release keeps its own
  `seq`; `input_applied` counts it when it is applied.
* No new text commands: `KEY`, `CAPS`, `MOUSE`, `SET`, `GO`, `QUIT` cover
  everything.
* **`NUMLOCK <0|1>`** came later, with the seventh release: Num Lock's
  toggle, as `CAPS` is Caps Lock's (`Command::Kind::numlock`,
  `InputState::numlock`). It is an input line, numbered like `CAPS`, so
  `input_seq`, `input_applied` and `input_eaten` count it; the `KEY 144`
  line before it is the key itself. A host that does not know it ignores
  it without numbering it, as it does any line it does not know, and every
  later line would then be one off in the saver's count: so the saver sends
  it only to a host whose `--capabilities` says `numlock=1` (§3.3, §4.1).
  The Win16 runtime answers `GetKeyState(VK_NUMLOCK)`'s bit 0 from it; the
  pe32 lane ignores it.

### 3.3 New command-line forms

```
adhostwin.exe --capabilities
adhostwin.exe --configure <module> --button <slot> [--owner <hwnd>] [NAME=VALUE ...]
```

* `--capabilities` prints one line and exits 0:
  `lanes=pe32,ne16 configure=pe32,ne16 status=1 state=1 seed=1` (only what
  this build has). The settings dialog uses it instead of the exit-3/exit-2
  probe. Later additions: `audio=1` (AUDIO.md), and
  `abis=afterdark,intermission`, the module ABIs the build's lanes run
  (PACKAGES.md §7.5), and, since the seventh release, `numlock=1`: the host
  takes the `NUMLOCK` line and `ADNUMLOCK` (§3.1, §3.2). Today's line
  reads `lanes=pe32,ne16 configure=pe32,ne16 abis=afterdark,intermission
  status=1 state=1 seed=1 audio=1 numlock=1`. The saver takes `numlock=1`
  exactly: `numlock=0`, another spelling or nothing means no Num Lock
  toggle.
* `--configure`: §6.1.

### 3.4 The status record

A pagefile-backed section created by the front-end per host (unnamed,
inheritable handle, passed in `ADSTATUSHANDLE` and listed in the spawn's
`PROC_THREAD_ATTRIBUTE_HANDLE_LIST`). Little-endian, at offset 0:

```c
struct AdwHostStatusV1 {           // 64 bytes
  uint32_t magic;                  // 0x53574441 "ADWS"
  uint16_t version;                // 1
  uint16_t size;                   // 64
  volatile uint32_t gen;           // seqlock: odd while the host writes
  uint32_t flags;                  // below
  uint64_t frames;                 // steps completed
  uint64_t input_applied;          // seq of the last input line applied before the last completed step
  uint64_t input_eaten;            // highest input seq the guest consumed (§2)
  uint32_t source;                 // why interactive: 0 none, 1 AD4 WantEvents, 2 AD3 0x0E
  uint32_t lane;                   // 1 pe32, 2 ne16, 3 test pattern
  uint32_t reserved[4];            // 0
};
// flags
#define ADWS_INTERACTIVE   0x01    // the module takes keys, clicks and moves as its own
#define ADWS_CURSOR        0x02    // show a cursor (AD4 +0x08 bit 1; AD3 0x11 until 0x12)
#define ADWS_ROTATE_OK     0x04    // may be rotated away while interactive (AD4 +0x08 bit 2)
#define ADWS_KEY_FILTER    0x08    // the guest may consume input without being interactive:
                                   // a WH_KEYBOARD hook is installed, or within the last 120 steps it
                                   // read the saver window's queue with removal (PeekMessage PM_REMOVE
                                   // or GetMessage, hwnd = saver or NULL, a range that includes keys)
#define ADWS_WAKE          0x10    // the guest posted WM_CLOSE / SC_CLOSE to the saver window
                                   // (since the seventh release also: an After Dark 2.0 module
                                   // returned 5, its wake request, §5.2)
#define ADWS_READY         0x20    // lane init done
```

* **Writer** (core, `run_host`, after each completed step and once after
  `init`): `gen++` (odd), store fields, `MemoryBarrier()`, `gen++` (even).
  Values come from `Lane::status()` (§10) and the core's own counters.
* **`input_applied` is settled input.** It stays below
  `LaneStatus::unsettled`, the lowest line the guest may still take (posted to
  a queue the guest reads, or waiting to be posted, and neither taken nor
  dropped yet). So a key the Classic lane keeps in the saver window's queue
  for a suspended DRAWFRAME (§5.2) holds the `.scr`'s decision until the
  guest has taken it (eaten) or it was dropped, instead of reading "applied,
  not eaten" a step too early. Lanes that deliver input at once (pe32)
  report 0.
* **Reader** (`.scr`): read `gen`; if odd, retry; copy; re-read `gen`;
  equal → consistent. Give up after 4 tries and use the previous copy.
* **Why shared memory.** stdout is frames only. stderr is a log that may go
  to a file or `NUL`, and routing it through a pipe would make the `.scr`
  drain and forward all log output to keep hosts from blocking. A
  one-page section needs no thread, cannot block the host, always holds
  the latest state and dies with the processes. `ADSTATUSLOG` mirrors it
  on stderr for tests.

---

## 4. The saver's rules (`/s`; `/p` takes no input)

### 4.1 Caps Lock tracking

* At every spawn: `ADCAPS = GetKeyState(VK_CAPITAL) & 1` in the host's
  environment (every window's host, owner or not).
* On each `WM_KEYDOWN`/`WM_KEYUP` the owner receives `KEY <vk> <1|0>` first.
  Then the saver reads `GetKeyState(VK_CAPITAL) & 1` and, when it differs
  from the last value sent to the owner, sends `CAPS <state>`. (Checking on
  both down and up makes this independent of when Windows flips the toggle
  bit.) A 250 ms timer repeats the check.
* **Num Lock** (since the seventh release), the same way for a host that
  takes it (§3.3):
  * At every spawn: `ADNUMLOCK = GetKeyState(VK_NUMLOCK) & 1` (every
    window's host, owner or not, `/p` too), except for a host whose
    `--capabilities` answer came without `numlock=1`: then the variable is
    removed, so nothing inherited reaches it. Unless the rotation waits for
    the answer (it holds a module of another ABI: `App::caps_gate`, up to
    2 s), the saver's first hosts start before it, so they get `ADNUMLOCK`
    anyway: a host that does not know the variable ignores it, and without
    it Final Exam would latch a guessed state and take the first correcting
    `NUMLOCK` line for a toggle, starting its exam by itself.
  * After each `KEY` line to the owner (after the `CAPS` check), on the
    250 ms timer, and when the answer arrives: `NUMLOCK <state>` when it
    differs from the last value sent to the owner, but only once the
    owner's host has answered with `numlock=1`.
  * The saver logs `input: numlock N -> owner (n=K)`, and each spawn line
    says what the host started with (`numlock=0|1`, or `-1`: none).
* Caps Lock, Num Lock, Shift and Ctrl never wake the saver (AFTERDAR.SCR
  `0x4028db..0x4028f2`; After Dark 2.0's hook did not wake on them either,
  §1.7).

### 4.2 Event table

`I` = the owner's latest status has `ADWS_INTERACTIVE`. Every forwarded
event goes to the owner's host.

| Event | Not interactive | Interactive |
|---|---|---|
| `WM_KEYDOWN` Shift/Ctrl/Caps/NumLock | forward `KEY`; never exit | forward |
| `WM_KEYDOWN` any other key | forward `KEY`, then **decide** (§4.3) | forward (the module has it) |
| `WM_KEYUP` | forward `KEY` | forward |
| `WM_SYSKEYDOWN` (Alt, Alt+x, F10) | exit | **exit** (our escape; AFTERDAR swallowed these, so no module relied on them) |
| `WM_LBUTTON/RBUTTON/MBUTTONDOWN`, `WM_XBUTTONDOWN` | forward `MOUSE`, then decide | forward `MOUSE` |
| button up | forward `MOUSE` | forward `MOUSE` |
| `WM_MOUSEWHEEL/HWHEEL` | exit | ignore |
| `WM_MOUSEMOVE` | forward `MOUSE` (coalesced to one per `GO`); past the threshold: decide | forward `MOUSE`; never exits |
| `WM_ACTIVATEAPP(FALSE)` (Win key, Alt+Tab, Ctrl+Alt+Del) | exit | exit |
| `WM_WTSSESSION_CHANGE`: `WTS_SESSION_LOCK`, `WTS_CONSOLE_DISCONNECT`, `WTS_REMOTE_DISCONNECT` (a lock sends no deactivation) | exit | exit |
| status `ADWS_WAKE` on the owner | exit | exit |

* **MOUSE coordinates** are the cursor's position mapped into the owner's
  letterboxed frame rectangle, scaled to the host's emulated screen and
  clamped to it (that host's own: an Intermission module's 640×480, which
  its ABI gives it, or since the seventh release a Star Trek module's, and
  with the twelve releases a ScreamSavers or Marvel one's, which its
  catalog entry's `"screen": "640x480"` gives it, whatever the
  Resolution setting, DESIGN.md §6a; its frame pillarboxed on a widescreen;
  a game's cursor clip is that frame, and Final Exam's mouse move is mapped
  into it); `buttons` from `GetKeyState(VK_LBUTTON/RBUTTON/MBUTTON)`.
  Moves are coalesced: while a `MOUSE` line is queued and not yet written, a
  newer position replaces its text and keeps its number, so line numbers
  are assigned when a line is queued and never skipped.
* **Threshold**: 10 physical pixels (Euclidean) from the baseline, as today.
  When the owner goes from interactive to not, the baseline becomes the
  cursor's position at that moment (AFTERDAR.SCR kept the start position,
  so any movement after a mouse game woke it; we do not).
* **Cursor**: hidden, except while `I && ADWS_CURSOR` (arrow). While `I`,
  `ClipCursor` to the owner's frame rectangle; released when `I` drops,
  on exit and on deactivation. Non-owner windows answer `WM_MOUSEACTIVATE`
  with `MA_NOACTIVATE`, so the owner keeps the keyboard.
* **Rotation**: when the rotation timer fires while the owner is `I` and not
  `ADWS_ROTATE_OK`, the owner window's switch waits (re-checked every
  second) until it is not (AFTERDAR.SCR `0x40190b`). Other windows rotate on
  time.
* The exit reason is logged (`input: key vk=0x41`, `input: syskey`,
  `input: move dx=… dy=…`, `input: deactivated fg=<exe>`,
  `input: session locked`, `input: session disconnected`, `input: wake`),
  and kept in the last-exit log (§9.1).

### 4.3 Deciding

For an event that exits when not interactive (a non-exempt key down, a
button down, a move past the threshold), with `n` = the number of the input
line just sent for it:

1. If the owner's status is `ADWS_INTERACTIVE` → no exit.
2. Else if the status is **stale** (`input_applied < n − 1`: the host has
   not yet stepped with the input sent before this one, e.g. a Caps Lock
   press a moment ago) **or** `ADWS_KEY_FILTER` is set → **hold**: keep
   forwarding later input, and re-decide on every status change until
   `input_applied ≥ n`; then exit unless `input_eaten ≥ n` or
   `ADWS_INTERACTIVE`. The hold lasts at most **300 ms** (then decide on the
   status as it is) and ends at once when the owner's host is gone.
3. Else → exit.

Status is polled on every `WM_APP_FRAME` from the owner and on a 16 ms timer
while a hold is pending. The decision logic is a pure function
(`decide(event, status, seqs, now) → forward | exit | hold`) with unit tests.

### 4.4 What the user sees (the exit gesture, documented in `scr/README.md`)

* Not playing: any key except Shift/Ctrl/Caps Lock/Num Lock, any click, the
  wheel or a nudge of the mouse ends the saver. Caps Lock never does; in some
  modules it does something (scares the fish, changes the colours) or
  starts a game. Num Lock never does either; in Final Exam it starts the
  exam.
* Playing (after Caps Lock in Rodger Dodger, You Bet Your Head, Simpsons
  Trivia, Mime Hunt, Frankenscreen, Marbles, RPS, Magic Turtle's editor,
  How to Draw…, and the Disney Collection's Pinocchio, whose Wishing Star
  Pinocchio follows): keys, clicks and the mouse belong to the game. Press Caps
  Lock again to stop playing (the next key or move then ends the saver), or
  press **Alt** to end it at once.
* **Final Exam** (Star Trek: The Screen Saver, since the seventh release)
  is a Num Lock game: Num Lock starts the Starfleet Academy exam, whose
  answers are the number keys (the exam is interactive through the status
  record as any game is), and Num Lock again stops it. A move of the mouse
  ends the exam, and the module then asks its host to wake the saver
  (result 5, §5.2): the saver ends through the status's `ADWS_WAKE`
  (`input: wake`), as After Dark 2.0 ended. **Alt** ends it at once, as any
  game.
* Only the primary monitor plays; the others keep running on their own.

---

## 5. Lane obligations (input and status)

### 5.1 pe32

* `KEY` (down/up) while wants-events → messages 7/8 with `vk`, **consumed**
  (`input_eaten = seq`). `MOUSE` while wants-events → 30 (move, when the
  position changed), 21/27 (left down/up), 24 (left held, once per step while
  down), with `MAKELONG(x, y)`; consumed. The right button is not a module
  message (the SDK has none); pollers see it.
* `GetAsyncKeyState` implements bit 0 ("pressed since the last call"), per
  VK, as the Win16 shim already does; `VK_RBUTTON`/`VK_MBUTTON` from the
  bitmask.
* `status()`: interactive = block `+0x08` bit 0; cursor = bit 1;
  rotate-ok = bit 2; source 1; key-filter 0; wake if the guest posted
  `WM_CLOSE`/`SC_CLOSE` to `host_window()`.
* Scaled small screens (§9.2): mouse coordinates scale with the guest display.

### 5.2 ne16

* Interactive = the `0x0E` toggle (already tracked as `wants_events_`), source
  2; cursor = `0x11`/`0x12` (`cursor_`). While interactive every input line
  is consumed.
* **Saver-window queue.** Each `KEY` line posts `WM_KEYDOWN`/`WM_KEYUP` (and
  each `MOUSE` line `WM_MOUSEMOVE`/button messages) to the Win16 saver window
  in the guest queue, tagged with its `seq`, with Win16 `lParam` layout
  (repeat 1, scan code from `MapVirtualKey`, bit 30 previous state, bit 31
  transition). A tagged message the guest removes (`PeekMessage` with
  `PM_REMOVE`, `GetMessage`) and does not `DispatchMessage` back to the saver
  window is consumed. Untaken tagged messages are dropped after the step
  that followed them (the saver window "handled" them), unless a DRAWFRAME
  that may still take them is suspended (a long call): then they wait one
  more step, or, while that call itself reads the saver window's queue
  (key-filter from queue reads), until it takes them (at most 600 steps). The
  1996 host could not pump its queue before the module's call returned, so a
  key pressed while LUNATIC's game (one long DRAWFRAME polling the queue once
  per game tick, 2–3 frames apart under emulation) was busy was still the
  game's at its next poll; dropping it after one step woke the saver in the
  middle of play. Kept messages are reported as `unsettled` (§3.4). *Reading* the
  saver window's queue with removal (`PeekMessage(PM_REMOVE)` or
  `GetMessage` with `hwnd` = the saver window or NULL and a filter range that
  includes `WM_KEYDOWN`) sets key-filter for 120 steps, whether or not a
  message was there: LUNATIC polls every frame, so the flag is up before the
  first key arrives.
* **`FindWindow("After Dark", NULL)`** returns the saver window (the AD 2/3
  blanker class LUNATIC looks for, §1.5); every other lookup is unchanged.
* **Keyboard hooks.** `SetWindowsHook`/`SetWindowsHookEx` with `WH_KEYBOARD`
  (2) are accepted and chained (most recent first; `DefHookProc`,
  `CallNextHookEx`, `UnhookWindowsHook(Ex)`). For each `KEY` line, before it
  is posted, the chain is called with `HC_ACTION`, `wParam` = VK, `lParam`
  as above; a non-zero return consumes the key and it is not posted. Other
  hook types keep returning 0. While any `WH_KEYBOARD` hook is installed,
  key-filter is set.
* Hooks and posting run at the next point the guest can be called: the start
  of the step, or before the next DRAWFRAME of a run when a long call was
  suspended (like `controls_pending_`). A very long call can make the
  `.scr`'s 300 ms hold expire; that exits (acceptable).
* `TranslateMessage` produces `WM_CHAR` for tagged key downs with the US
  layout (`VkKeyScan` inverse, Shift state from `InputState`).
* Mouse coordinates are scaled for the small-screen guest display, as
  `sync_input()` already does.
* `CAPS` and `NUMLOCK` lines are toggle states only: `GetKeyState`'s bit 0
  for `VK_CAPITAL` and `VK_NUMLOCK` (since the seventh release for Num
  Lock). The `KEY 20` or `KEY 144` line before each is the key itself,
  which goes to the hook chain and the queue as any key does.
* **After Dark 2.0's wake** (Star Trek: The Screen Saver, since the seventh
  release; `PACKAGES.md` §7.3). An After Dark 2.0 module's DRAWFRAME
  result 5 is its wake request, as `AD.EXE` 2.0b took it (`ABI.md` §3.9).
  The status gets `ADWS_WAKE`, so the saver ends as when the user wakes it;
  the frame of the wake is presented, and the module is called no more.
  Headless, the run ends at the next step: exit 0, "module finished", after
  the log line `<module>: frame N: the module woke the saver (result 5);
  the run ends`. Streamed, the frames repeat the last picture, input and
  `SET` going nowhere, until the front end ends the run (`QUIT`, or stdin
  closing): a host that exited instead would race the saver, which ignores
  the status of a host whose stdout has closed. For every other module 5
  still ends the run as the module's error. Final Exam, the one module
  that returns it, does so when a mouse move ends its exam (§1.7), after a
  `0x0E` that ends its interactive state.
* **Intermission modules** (Star Wars Screen Entertainment,
  `PACKAGES.md` §7.5) are **never interactive**: Intermission gave its IMX
  modules no input at all (its reader never sets the record's input flag,
  `ABI.md` §3.8.3), so the status source stays 0 and the lane is still 2
  (ne16).
  **No `KEY` line becomes a key message for them**: the lane runs no
  keyboard hook chain and posts nothing to the saver window's queue; a key
  only changes the key state that `GetAsyncKeyState` and `GetKeyState`
  read. `MOUSE` lines are posted as above. The modules poll, through
  SWSE.DLL's `USERABORT`: `GetCursorPos`, `GetAsyncKeyState` for the
  buttons, and `GetInputState`, true while a key or button message waits
  in the queue. Under Intermission's default wake-up options ("mbk") every
  key but Ctrl ended the blank, so no module ever saw a key without that
  end. A posted key the saver does not wake on (Shift, Ctrl, Caps Lock, Num
  Lock, and any key-up, such as that of the key that started a preview)
  made `USERABORT` report input during a module's start instead, and the
  module gave its start up, as it did when Intermission was about to stop
  it, staying on its title card until the saver ended. So those keys change
  nothing for a Star Wars module, and a button that reaches `GetInputState`
  wakes the saver anyway. Key-filter and wake follow the same rules as for
  any ne16 module. When `USERABORT` sees input, SWSE's `FORCETOWAKE` posts
  fake mouse and Shift-key messages to its own task
  (`PostAppMessage(GetCurrentTask(), …)`, `ABI.md` §3.8.4) for Intermission
  to end the blank on. **That is not mapped to a wake**: the lane's pump
  removes and counts those posts (`user16_dispatch_guest`), and the saver's
  uniform rules (§4) stay in charge: a Star Wars module ends on the same
  input as any other module (a move past 10 pixels, not any move).

---

## 6. Module-owned configuration (buttons)

### 6.1 `adhostwin --configure`

```
adhostwin.exe --configure <module> --button <slot> [--owner <hwnd>] [NAME=VALUE ...]
```

* `<module>` resolves like the run form; `<slot>` is the catalog control
  `index` of a `button`; `--owner` is decimal or `0x` hex (0/absent = no
  owner, for the command line). The environment is parsed as usual
  (`AD_ASSETS_DIR`, `ADCVSET` = the dialog's current values, `ADSTATE`).
* The state overlay (§7) is **persistent** in this mode (`ADSTATE`, or its
  `--configure` default).
* The lane runs the original button sequence:
  * **pe32** (`ADPAGE 0x9001fbf`): load engine + module; zeroed block with
    `cbSize`, `+0x0C` = the owner as a guest handle (§6.2), `+0x14` =
    `hModule`, `+0x3C` = 50, `+0x40` = values, `+0x18` = 0; `Module(0)`,
    `Module(6, slot)`, `Module(1)`; unload.
  * **ne16** (`BUTTONPUSHED16`): with the real OLDMOD16, far-call
    `BUTTONPUSHED16(path, owner16, slot, ctrl4, err, 260, &errId)`; with the
    native AD3 bridge, the same sequence in C++ (`MODULE(5)` on the screen
    DC with `AD_SYSTEM+0x26` = owner16; unless 1 or 7: copy values,
    `MODULE(7 + slot)`; error copy-back; free).
  * **ne16, an Intermission module** (Star Wars Screen Entertainment; added
    later, `PACKAGES.md` §7.5): its only button is slot 0,
    **Configure...**. The lane sends `SAVERMAIN(10)`, loading the module
    through its reader, then `SAVERMAIN(7)`, the query INTRMLIB's
    enumeration had sent before the control panel offered the button (so
    the module's `palette(0)` and `saverinit` run first, and the record
    holds its name); it sets the record's window to owner16, sends
    `SAVERMAIN(8)` and frees the module with `SAVERMAIN(11)`.
    Intermission's control panel itself sent 10, 8 and 11, with no 7 (§1.6,
    `ABI.md` §3.8.4). The reader's `DialogBox` of the module's `DIALOGBOX`
    becomes a real modal dialog (§6.2), its ANT3D controls (classes
    INTRMLIB's LibMain registered in the guest) real windows forwarding to
    the guest. The reader answers 0 only for a module without a
    `SAVERDLGPROC` (none of the 14), which exits 4; any other slot is
    refused ("control N is not a button", exit 1). The module
    writes its settings to `C:\WINDOWS\SWSE.INI`, which lands in
    `<state>\swse\WINDOWS\SWSE.INI` (§7); the profile seeds of §7.2 are
    never written out.
* stdout (not streaming in this mode) gets one JSON line:
  `{"result":"ok"|"nothing"|"error","dialogs":<n>,"message":"…","written":["<guest path>",…]}`.
* Exit codes: **0** the button ran and showed at least one dialog or message
  box; **4** it ran and showed nothing; **5** the lane has no configure
  support; 1 error; 2 usage; 3 lane missing.
* No timeout: the user may keep the dialog open as long as they like.

### 6.2 Real dialogs from guest templates

Only in configure mode (in the saver, dialogs stay refused as today).

**Handles.** A real window is shown to the guest as a *guest handle*
from a range the emulated window table never uses (pe32: `0xA000 + 4·i`, at
most `0xFFFC`; ne16: a reserved HWND16 range), mapped both ways. The owner
passed with `--owner` gets one too. USER shims recognise these handles and
act on the real window. The pe32 handles keep a zero high word, as Windows
95's 16-bit HWNDs did: FISH's and RAIN's `BtnDlgProc` still decode
`WM_COMMAND` the Win16 way and read the notification code from
`HIWORD(lParam)`, which the plan's `0x00F00000 + 4·i` broke
(`host/win32/realui.hh`). They share numbers with GDI handles, as on
Windows 95; emulated windows start at `0x00010010`.

**Win32 (pe32).**

* `DialogBoxParamA/W`, `DialogBoxIndirectParamA/W`, `CreateDialogParamA`,
  `CreateDialogIndirectParamA`: find `RT_DIALOG` in the guest module
  (`hInstance` → loaded image), copy the `DLGTEMPLATE`/`DLGTEMPLATEEX`, and
  call the real `DialogBoxIndirectParamW` (or `CreateDialogIndirectParamW`)
  with the owner's real HWND and a **host dialog procedure** that, for every
  message, maps handles and pointer parameters into guest memory, calls the
  guest `DLGPROC` with `call_guest(proc, {hwnd_g, msg, wp, lp}, stdcall)`,
  and maps the result back. `WM_INITDIALOG` passes the guest's
  `dwInitParam`. `EndDialog` from the guest ends the real modal loop; the
  return value is the guest's.
* Custom classes named in a template that the guest registered with
  `RegisterClassA` get a real class whose window procedure forwards the same
  way.
* Marshalled messages (both directions, guest ↔ host memory): `WM_SETTEXT`,
  `WM_GETTEXT`, `WM_GETTEXTLENGTH`, `EM_*` with text (`EM_GETLINE`,
  `EM_REPLACESEL`, `EM_LIMITTEXT`, `EM_SETSEL`), `LB_*`/`CB_*` with strings
  (`ADDSTRING`, `INSERTSTRING`, `GETTEXT`, `GETLBTEXT`, `FINDSTRING`,
  `SELECTSTRING`, `DIR`, item data), `BM_*`, `STM_*`, `WM_COMMAND`,
  `WM_NOTIFY` (read-only copy), `WM_DRAWITEM`/`WM_MEASUREITEM`/
  `WM_COMPAREITEM`/`WM_DELETEITEM` (struct copies),
  `WM_CTLCOLOR*` (the `HDC` becomes a temporary guest DC wrapping the real
  one for the duration of the call; a returned guest brush becomes its real
  `HBRUSH`), `WM_PAINT` in custom controls (`BeginPaint` on a real window
  gives such a wrapped DC), `WM_TIMER`, scroll messages, `WM_SETFONT`.
  Messages outside this list pass numbers through and 0 pointers.
* The `Dlg*`/`*Dlg*` family (`GetDlgItem`, `SendDlgItemMessageA`,
  `SetDlgItemTextA`/`GetDlgItemTextA`, `SetDlgItemInt`/`GetDlgItemInt`,
  `CheckDlgButton`/`IsDlgButtonChecked`, `CheckRadioButton`,
  `EnableWindow`, `ShowWindow`, `SetFocus`, `GetWindowTextA`,
  `SetWindowTextA`, `SendMessageA`, `PostMessageA`, `InvalidateRect`,
  `GetClientRect`, `MessageBeep`, `SetTimer`/`KillTimer` on real windows)
  act on the real windows with the same marshalling.
* `MessageBoxA` becomes a real `MessageBoxW` owned by the dialog (or the
  `--owner`).
* `GetOpenFileNameA`/`GetSaveFileNameA` become the real `GetOpenFileNameW`
  with guest ↔ host path translation (§7.4): the initial directory from
  guest to host, the chosen path from host to guest (under `H:` unless it is
  inside a mount). Filters and flags pass through; hook procedures
  (`OFN_ENABLEHOOK`) are called through `call_guest`.
* GDI objects a dialog procedure creates (fonts, brushes, bitmaps) are the
  runtime's real GDI objects; a guest DC wrapper maps a real `HDC` to the
  guest for the duration of a message.
* DPI: the dialog thread uses
  `DPI_AWARENESS_CONTEXT_UNAWARE_GDISCALED`, so dialog-unit layouts designed
  for 96 DPI stay intact with crisp text.
* `DirectSoundCreate` answers `DSERR_ALLOCATED` (`0x8878000A`, the value
  ADXPL510 tests, AUDIO.md §2.1): DirectSound is present but its device is
  busy. The Music Choice buttons (POINTS, SLOWBURN, SWIRLING) look for
  `DirectSoundCreate` and, when `dsound.dll` is missing altogether, show an
  error instead of their dialog. Configure runs never enable the audio
  engine, so the real DirectSound of AUDIO.md §7 is never registered there.

**Win16 (ne16).** Same shape, plus 16-bit conversions:

* `DialogBox`, `DialogBoxParam`, `DialogBoxIndirect(Param)`,
  `CreateDialog(Param)`, `CreateDialogIndirect(Param)`: the Win16
  `DLGTEMPLATE` (DWORD style, BYTE item count, WORD x/y/cx/cy, then menu,
  class and caption as NUL-terminated ANSI strings (the menu may be `0xFF` +
  WORD ordinal), then with `DS_SETFONT` a WORD point size and face name;
  items with WORD x/y/cx/cy/id, DWORD style, class as BYTE `0x80` Button /
  `0x81` Edit / `0x82` Static / `0x83` ListBox / `0x84` ScrollBar / `0x85`
  ComboBox or a string, text as a string or `0xFF` + WORD, BYTE extra count
  + data) is converted to a Win32
  `DLGTEMPLATE` (DWORD-aligned items, UTF-16 strings from code page 1252).
  Our own converter; nothing is copied from Wine/WineVDM.
* The host dialog procedure calls the guest's 16-bit `DLGPROC` with
  `call_far(proc, {hwnd16, msg16, wParam16, lParam32})`, Pascal.
* **Message translation** (Win32 → Win16 into the guest; Win16 → Win32 for
  what the guest sends):
  * `WM_COMMAND`: Win32 `wParam = MAKELONG(id, code), lParam = hwnd` ↔
    Win16 `wParam = id, lParam = MAKELONG(hwnd16, code)`.
  * `WM_CTLCOLORxxx` (0x132–0x138) ↔ Win16 `WM_CTLCOLOR` (0x0019) with
    `wParam = hdc16`, `lParam = MAKELONG(hwnd16, CTLCOLOR_xxx)`; the returned
    `HBRUSH16` → real brush.
  * `WM_HSCROLL`/`WM_VSCROLL`: Win16 `wParam = code`, `lParam =
    MAKELONG(pos, hwnd16)`.
  * `WM_DRAWITEM`/`WM_MEASUREITEM`/`WM_COMPAREITEM`/`WM_DELETEITEM`: 16-bit
    struct copies in a scratch segment.
  * **Control messages are WM_USER-based in Win16 and depend on the target's
    class**, found with the real `GetClassNameW` of the target window:
    Edit `EM_*` 0x400+n ↔ 0x00B0+n; Button `BM_*` 0x400+n ↔ 0x00F0+n;
    ListBox `LB_*` 0x401+n ↔ 0x0180+n; ComboBox `CB_*` 0x400+n ↔ 0x0140+n
    (Win16 `LB_ADDSTRING` = `WM_USER+1`, `CB_ADDSTRING` = `WM_USER+3`,
    `BM_SETCHECK` = `WM_USER+1`, `EM_LIMITTEXT` = `WM_USER+21`). Far
    pointers in `lParam` (strings, `LB_GETTEXT` buffers, `LB_DIR` specs,
    `EM_GETLINE` buffers) are marshalled, and the few messages whose
    parameters moved are repacked (Win16 `EM_SETSEL` takes
    `lParam = MAKELONG(start, end)`, Win32 `wParam = start, lParam = end`;
    likewise `EM_LINESCROLL`). Static and scroll-bar class messages follow
    the same class-based rule.
  * `WM_GETTEXT`/`WM_SETTEXT`, `WM_INITDIALOG` (`lParam` = the guest's init
    param), `WM_CLOSE`, `WM_DESTROY`, `WM_TIMER`, `WM_PAINT`, `WM_SETFONT`
    (`hfont16`), `WM_SYSCOMMAND`: numbers equal, handles mapped.
* `COMMDLG.GetOpenFileName`/`GetSaveFileName` (16-bit `OPENFILENAME`):
  real dialog, **8.3 short paths** returned (`GetShortPathNameW`, under `H:`,
  §7.4), since Win16 modules expect them.
* `COMMDLG.ChooseFont` (added for Star Wars Screen Entertainment's
  Scrolling Text): the real font dialog, started from the guest's
  `LOGFONT`; on OK the `LOGFONT`, point size, font type and colour go back
  to the guest. Hidden (`ADCONFIGHIDDEN`), where no script line can pick a
  font, it is cancelled and logged (`host/win16/README.md`).
* `MessageBox` becomes a real `MessageBoxW` (today it logs and answers IDOK).
* `WinHelp`: logged, returns 1 (HOW2DRAW "Help" then shows nothing: exit 4).
* `DlgDirList`, `LB_DIR` and `CB_DIR` with `DDL_DRIVES` list `[-h-]` after
  `[-c-]` while the host's drives are mounted as `H:` (§7.4), and
  `DlgDirSelect` answers `h:` for it (since the seventh release: Star Trek's
  Sounder chooses its folder of `.WAV` files in such a list, so the user
  reaches their own folders; before, it listed `[-c-]` alone).
  `DlgDirList` takes the guest's DOS to the drive and directory it lists,
  as Windows 3.1's USER did through DOS (select disk, chdir), and each
  drive keeps its own current directory, so `[-c-]` returns to where `C:`
  was left. A module's `getcwd()` (INT 21h AH=19h, then AH=47h) therefore
  names the chosen folder with its drive: Sounder saves `[Sounder]
  SoundPath=H:\C\WINDOWS\MEDIA`, and Globe's "Map..." in After Dark 4.0
  Deluxe and 3.2 saves `[After Dark] GlobeFile=H:\…`, which every later run
  finds. A folder whose short path is longer than DOS's current directory
  (66 characters with the drive) is not entered: the list stays as it was.
* The configure script's `PICK <id> <text…>` selects the list-box or
  combo-box item whose text this is, ignoring case, wherever a sorted list
  holds it, then acts as `SELECT` (e.g. `PICK 204 [-h-]`).

### 6.3 The settings dialog

* A button control shows as a real button labelled with the control's name
  (e.g. "Select Fish…"). It is **enabled** when the host reports
  `configure=<lane>` for the module's lane (`--capabilities`) and the
  module file exists. Otherwise it is the current read-only row, with the
  tooltip saying why.
* Clicking runs `adhostwin --configure <path> --button <index> --owner
  <dialog hwnd>` with `CREATE_NO_WINDOW`, `AD_ASSETS_DIR`, `ADCVSET` = the
  dialog's current (unsaved) values for that module, and `ADSTATE` (§7.1).
  The module's dialog is owned by the settings window (cross-process owner),
  so it stays on top and the settings window is disabled by the modal loop,
  as ADW30 did (§1.4).
* **Activation.** The settings window disables itself for the run (from the
  click on). Each modal dialog, message box or file dialog the host shows
  finds its owner enabled again (the host lends it: `OwnerLend` in
  `win32/realui.cc`, `win16/dialogs16.cc`), so the modal loop disables and
  re-enables it as for any owner. When such a window goes while it is active,
  Windows does not hand the activation back across the process boundary (the
  settings window dropped behind whatever application was under it), so the
  host, still the foreground process at that moment, enables the owner and
  gives it the foreground itself (a `WH_CALLWNDPROC` hook on
  `WM_WINDOWPOSCHANGING` with `SWP_HIDEWINDOW`, `adhostwin_main.cc`). The
  settings window does not re-disable itself when re-enabled: a disabled
  window cannot take the activation.
* The settings dialog waits on a worker thread (the UI thread keeps pumping:
  the owner and the dialog share input once owned). When the process ends,
  whatever the exit code, it re-enables itself (`EnableWindow(TRUE)`: a
  crashed host must never leave it disabled), brings itself to the
  foreground, **restarts the live preview**, and drops that module's
  thumbnail so it is retaken.
* Exit 4 shows an inline note under the row ("Nothing to set here"); 1/3/5
  show "Couldn't open this option (code N)".
* Values the module keeps itself are saved by the module at once. The
  dialog's **Cancel does not undo them** (true of ADPAGE and the AD 3 control
  panel too); the dialog says so in the button's tooltip.
* Where the module has a popup whose items include "Custom" (Messages 4.0,
  Message Mayhem) and that popup is not on "Custom" after a successful run,
  the row shows a hint: "Choose “Custom” under Message: to show it". Nothing
  is switched automatically.
* Only one configure run at a time; the live preview is paused while it runs.

### 6.4 Persistence

Everything the module writes during the button run goes through the state
overlay (§7) and is persistent. The saver's hosts start fresh processes, so
they read it at their next spawn; the live preview is restarted after each
configure run. Nothing is copied into `settings.ini`.

### 6.5 End-to-end: message modules

| Module | Steps | Where the text lands |
|---|---|---|
| Messages 4.0 (`ad40.messages`, `ad10.messages`) | Message: = Custom; **Custom** → type → OK | `<state>\<pkg>\WINDOWS\MODULES.INI` (WriteModulePrefs) |
| Messages (`classic.message3`, `ad32.messages`) | **Edit / Select** → edit a message, select it → OK | `<state>\<pkg>\CLASSIC\MESG_AD3.DAT` (or `AD32\…`), plus `MODULES.INI` |
| Message Mayhem (`tt.messyges`) | Message = Custom; **Edit Custom** → type → OK | `<state>\tt\…` (trace with `ADTRACE=file`) |
| Nonsense (`classic.nonsense`, `ad10`/`ad32`) | **Edit Names…** | `NONSENSE.TXT` copied up into the module-dir overlay |

Verified by the acceptance tests in the briefs: type (scripted) → the state
file holds the text → a streamed/headless run with the same `ADSTATE` shows
it (frames differ from a run without, PNG checked by eye once).

### 6.6 Test hooks (configure mode)

* `ADCONFIGSCRIPT=<file>`: actions applied to the dialogs as they open,
  through the real controls (so the guest sees the same notifications as from
  a user):

  ```
  # comment
  TEXT <id> <text…>          WM_SETTEXT on an edit control (+ EN_CHANGE)
  CHECK <id> <0|1|2>         BM_SETCHECK + BN_CLICKED
  SELECT <id> <index>        LB/CB current selection + LBN_/CBN_SELCHANGE
  MULTI <id> <i,j,…>         multi-select list box selection
  CLICK <id>                 BN_CLICKED (1 = IDOK, 2 = IDCANCEL)
  FILE <host path>           answer the next file dialog without showing it
  ANSWER <IDOK|IDCANCEL|IDYES|IDNO|n>   answer the next message box without showing it
  NEXT                       the following lines apply to the next dialog opened
  ```
* `ADCONFIGHIDDEN=1`: dialogs never appear (parked off every monitor,
  cloaked with `DWMWA_CLOAK`, `WS_EX_NOACTIVATE`, never activated, as the
  settings dialog's screenshot hook does); message boxes and file dialogs are
  answered only by the script (unanswered: IDCANCEL / cancel, logged).
* With a script, a dialog still open when the script is exhausted is closed
  with IDCANCEL after `ADCONFIGTIMEOUTMS` (10000) and the host exits 1.
* `ADCONFIGDUMP=1`: logs each dialog's controls (id, class, style, text) on
  stderr, to write scripts.

---

## 7. The per-user state overlay

### 7.1 Layout

The `.scr` sets `ADSTATE=<folder of settings.ini>\state` on **every** host
it starts (saver, `/p`, the settings dialog's live preview, thumbnails,
`--configure`; `AD_SCR_STATE` overrides), so normally that is
`%LOCALAPPDATA%\LongAfterDark\state`, and a test that points `AD_SETTINGS`
at a scratch folder gets scratch state with it.

```
<state>\                                 ADSTATE
  <package>\                             deluxe (the FILES\… tree), ad10, ad32, tt, simpsons, swse, startrek,
                                         marvel, snoopy, looney, screams, disney,
                                         or legacy-<fnv32 of the module dir, 8 hex> for anything else
    WINDOWS\                             upper layer of the guest's C:\WINDOWS (both lanes of a package share it)
    <MODDIR>\                            upper layer of the guest's C:\AFTERDRK = the module dir
                                         (AD40, CLASSIC, AD10TH, AD32, TWISTED, SIMPSONS, …);
                                         for swse, SAVER: the upper layer of C:\SAVER
```

Star Wars Screen Entertainment's modules keep their settings in
`swse\WINDOWS\SWSE.INI` (one section per module, written by their
**Configure...** dialogs and by Storyboards, which remembers where it
stopped), and Scrolling Text its edit box in `swse\WINDOWS\SWTXEDBX.TXT`.
Deleting `<state>\swse` restores the disc's defaults, which stay in the
package's `WINDOWS\SWSE.INI` (§7.3).

Star Trek: The Screen Saver's modules keep what they write in
`startrek\WINDOWS\AD_PREFS.INI`: Communications' `[Communications]
MessageText` (its **Edit Custom...** button), Sounder's `[Sounder]
SoundPath` (its **Sounds..** button) and AD_SND 1.0's `[Sound] Mute`,
written at each load of a module that wants sound. The lane's profile
seeds for that file (`[After Dark] Path`, `[Sound] SoundDriver`) are never
written there (`PACKAGES.md` §7.3); deleting `<state>\startrek` restores
the defaults.

Of the twelve releases' later five, Marvel's module keeps its **Saver..**
choices in `marvel\AFTERDRK\MRVLIMAG\MRVLIMAG.ADC` (a copy of the package's
image catalog, made at its first write: a Saver.. OK, a Posters... →
Install or a wake with Create Poster On Wakeup, ABI.md §3.11), and
**Posters...** → Install and
Create Poster On Wakeup write `marvel\AFTERDRK\MRVLIMAG\MARVEL.BMP` and
`marvel\WINDOWS\WIN.INI` there: a wallpaper for the emulated PC only
(§1.6). The Looney Tunes' Messages keeps its custom message in
`looney\WINDOWS\MODULES.INI`. The After Dark 3.x seeds (`[After Dark]
Path`, `[Sound] SoundDriver` in `AD_PREFS.INI`) are never written there
either.

One package is one 1996 machine: Deluxe's AD4 and Classic modules share
`WIN.INI`, `MODULES.INI`, `AFTERDRK.INI` as they did on one Windows 95
install, while each package's modules never see another package's state
(`DESIGN.md` §7, "self-contained packages").

### 7.2 Mounts (both lanes)

| Guest | Lower (read-only) | Upper (writable) |
|---|---|---|
| `C:\WINDOWS` | the lane's synthetic files (WIN.INI `[Berkeley Systems]`, the ne16 `MODULES.INI` seeds, PROGMAN.INI and `.GRP` files; ne16 also `LunData.dat`, the module dir's `LUNDATA.DAT`, which the installers copied to WINDOWS: without it Lunatic Fringe says "Configuration File Not Accessible"); ne16, when the package has one, its `WINDOWS` folder (`swse`: `SWSE.INI`), and for an Intermission module the profile seeds of `SYSTEM.INI`, `SWSE.INI` and `ANTSW.INI` (`PACKAGES.md` §7.5), for an After Dark 2.0 module those of `AD_PREFS.INI` (`PACKAGES.md` §7.3) | `<state>\<pkg>\WINDOWS` |
| `C:\WINDOWS\SYSTEM` (ne16) | the engine dir | none (read-only, as today) |
| `C:\AFTERDRK`, `C:\AFTERD~1` (ne16) | the module dir | `<state>\<pkg>\<MODDIR>` (one upper for both names) |
| `C:\SAVER` (ne16, an Intermission module, instead of `C:\AFTERDRK`) | the module dir | `<state>\swse\SAVER` |
| `C:\PICTURES` (pe32) | module dir `PICTURES` | none |
| `H:\<L>\…` | the host's `<L>:\…`, read-only | none |

### 7.3 Semantics

* **Lookup**: upper first, then lower; directory listings (`FindFirstFile`,
  INT 21h 4Eh/4Fh) merge both, upper entries winning by name.
* **Copy-up**: opening a lower file for writing (any write access, or
  `CREATE_ALWAYS`/`TRUNCATE_EXISTING`, `_lcreat`, `OpenFile(OF_WRITE…)`,
  INT 21h 3Ch/3Dh with write access) first copies it to the upper layer
  (under the state mutex), then opens the upper copy. New files and
  directories are created in the upper layer. Deleting an upper-only file
  works; deleting a lower file fails with `ERROR_ACCESS_DENIED` (logged).
  Renames within the upper layer work.
* **Profiles** (`Get/WritePrivateProfileString(A)`, `Get/WriteProfileString`,
  `GetPrivateProfileInt`, Win16 and Win32 alike, one shared implementation):
  reading = the lane's **seed entries ⊕ the file** (the file wins per key;
  the file is the upper copy if any, else the lower). Writing =
  read-modify-write of the upper file only (copied up from the lower file
  first if there is one); seeds are never written out. Deleting a seed key is
  not persisted (logged).
* **Concurrency**: several hosts write at once (two monitors, the live
  preview, a configure run). Profile writes and copy-ups take a named mutex
  `Local\LongAfterDark-state-<fnv64 of the lower-cased state root, hex>`
  and write through a temp file + `MoveFileExW(REPLACE_EXISTING |
  WRITE_THROUGH)`; cached profile parses are re-validated by size + last
  write time. Data files opened by two hosts: last writer wins.
* **Modes**: persistent (`ADSTATE=<dir>`), or **in-memory** (`ADSTATE`
  unset or `:memory:`): the same semantics with the upper layer in memory for
  the life of the process (what the ne16 lane's `DosFiles` memory files do
  today, generalised). Headless runs, censuses and `FBHASH` are therefore
  unchanged by anything a user has saved, and never write.
* **Reset**: deleting `<state>\<pkg>` restores a package's defaults. The
  settings dialog may later offer this; not in this round.

### 7.4 Host paths (`H:`)

A path the user picks in a file dialog is outside the emulated disk. The
guest sees the host's drives under `H:`: host `D:\Photos\Cat.jpg` ↔ guest
`H:\D\PHOTOS\CAT.JPG`; host `C:\Users\me\Pictures` ↔ `H:\C\USERS\ME\PICTURES`.
Read-only. For ne16 the path handed to the guest is the 8.3 short form
(`GetShortPathNameW` per component), e.g. `H:\C\USERS\ME\PICTUR~1`. The
mapping is deterministic and reversible, so a folder chosen in configure
mode (and stored in the module's INI) resolves the same in every later run.
Guest paths already inside a mount keep their mount form (a picture under
`C:\PICTURES` stays there). A Win16 folder list reaches `H:` as its drive
`[-h-]` (§6.2): Star Trek's Sounder then plays the `.WAV` files of a folder
the user picked there, in every later run. Such a list reaches only folders
whose short path fits DOS's current directory (at most 63 characters after
`H:\`); file dialogs are not limited, since they hand over full paths.

---

## 8. Desktop seed

The hosts already paint `ADSEEDIMG` into the screen before the module's
first message (pe32 after the display attaches, ne16 before
`LOADADMODULE16`), dithered onto the 20 static colours: the only colours a
desktop keeps once a module realizes its own palette (`win32/display.hh`).
What remains:

* **Capture (`.scr`, `/s` only).** Before any saver window is created,
  for each monitor: `BitBlt(SRCCOPY | CAPTUREBLT)` of the monitor's rectangle
  from the screen DC into a 32-bpp DIB, then `StretchBlt` with `HALFTONE` to
  the emulated size (`ADSCREENW`×`ADSCREENH`) of the window's first host,
  then written as a binary P6 (maxval 255). That size depends on the first
  module, which is not known yet (a rotation's is drawn later), so the
  window gets one picture for each screen its first module may have
  (`releases.h` `first_module_screens`, since the seventh release; each
  kind of module, `first_module_abis`, before), all from the one `BitBlt`
  (`geometry.h` `plan_seed_shots`):
  * an After Dark module that follows the display: the whole monitor at
    the window's After Dark size, in
    `%TEMP%\LongAfterDark-seed-<pid>-<window index>.ppm`;
  * a module with a 640×480 screen of its own, an Intermission module by
    its ABI or a Star Trek, ScreamSavers or Marvel one by its catalog
    `screen` (that module alone,
    a rotation holding one, or Random while the host's answer may add
    them): the part of the monitor its 640×480 frame will cover
    (`geometry.h` `seed_source`: the letterboxed 4:3 frame), shrunk to
    640×480, in
    `%TEMP%\LongAfterDark-seed-<pid>-<window index>-640x480.ppm`, one
    picture for both kinds. A screen of another size would get its own,
    `…-<window index>-<W>x<H>.ppm`. Whatever the catalog holds (a
    hand-edited one may give every module a `screen` of its own, each a P6
    of up to 48 MB), a window gets three pictures at most (`geometry.h`
    `plan_seed_shots`): the one that follows the display, and two of
    modules' own screens, 640×480 first, then the smallest; a first module
    whose screen got none starts on black, and the pictures are written one
    at a time.

  A window that can only start with such a module (it alone, or Star Wars
  Screen Entertainment, Star Trek: The Screen Saver, ScreamSavers or Marvel
  Comics Screen Posters the only release imported) gets just the second. Where the two pictures are the same (a
  4:3 monitor at 480 lines, both kinds possible) the second is not written:
  the 640×480 module's entry uses the first file. Each file is created with
  `FILE_FLAG_DELETE_ON_CLOSE | FILE_ATTRIBUTE_TEMPORARY`, share
  read + delete, and **kept open** by the `.scr` for its lifetime, so the
  file disappears when the saver ends, however it ends.
* **Hand-over.** Only a window's **first** host of the session gets
  `ADSEEDIMG=<that file>`, the one taken at its emulated size. Respawns
  and rotations start black (the original randomizer blanked between
  modules, and a restarted module never saw the desktop again). A monitor
  added later by a relayout gets no seed.
* **Host.** `Display::seed` opens the file with
  `FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE` (required to open a
  delete-on-close file). A P6 of exactly the screen size is used 1:1 (no
  resampling); with a scaled guest display (§9.2) the lanes' resampler
  applies.
* **Off switch.** `settings.ini` `[Saver] StartFromDesktop=0` (default 1)
  skips the capture. `/p`, the live preview and thumbnails never capture
  (black, as headless).
* **Privacy.** The capture holds whatever was on screen. It lives only in
  that delete-on-close file and the hosts' memory.

---

## 9. Small items

### 9.1 DOS Shell ending after 3–5 s

**Reproduction attempts (this build, `build/dist/AfterDark`, scratch assets
with all five packages), all ran until stopped:**

* headless, `classic.dosshell` and `ad32.dosshell` (OLDMOD16 and native
  bridge), 1200 frames, and 3000 frames for every Color × Speed
  (`ADCVSET=1=0..4,2=0|100`), at 320×240 … 1712×960, and with `ADSOUND=1`:
  exit 0, `0 unimplemented`, no module stop;
* streamed free-running 12 s, and streamed in GO lockstep at 60 Hz for
  15 s (856×480): exit 0 on `QUIT`;
* the real `.scr /s` launched from Git Bash (single and both monitors,
  13 s), launched through `ShellExecute` (16 s), and through the settings
  dialog's **Preview** button (29 s): frames kept coming; the only exits
  were the test's own.

The frames show DOS Shell's boot (memory count to "640 KB OK" by ~4 s, BIOS
text, "Welcome to AD-DOS", then typed commands). The user's machine has no
`settings.ini` (so the saver ran **Random**) and the Deluxe package only.

**Conclusion.** Neither the module (no stop result) nor the host (no exit)
ends it on the current build. What remains is the `.scr`'s own exit paths
under real use: a key or nudge (today's rules exit on *any* key, including
Shift/Ctrl/Caps/NumLock), a deactivation, or a build whose `adhostwin` lacked
the Classic lane (as `package.sh` shipped before ne16 landed: three failed
starts show "“DOS Shell” could not be started" for about 3 s, then Random
moves on — the 3–5 s of the report). **Fix:**

1. The input rules of §4 (modifier keys and Caps Lock no longer exit).
2. An always-on **last-exit log**: `logs\saver-last.log` next to
   `settings.ini` (so `AD_SETTINGS` moves it; overwritten per `/s` run, at
   most 200 lines): start line (build,
   monitors, modules, host capabilities), every spawn/exit/respawn/rotation
   with the host's exit code, and the exit reason line of §4.2, so a report
   like this one comes with its cause.
3. Regressions: headless 18 000 frames (5 minutes of virtual time) of
   `classic.dosshell` and `ad32.dosshell` exit 0 with more than one distinct
   `FBHASH` after frame 600; and an opt-in real `/S` of 60 s
   (`scr_smoke_e2e-dosshell`: the upper-case switch Windows passes, started
   with `CreateProcess` on one monitor staged off every real one, so it never
   covers the screens) with `AD_SCR_TESTEXIT_AFTER_FRAMES=3600`, whose log
   has no `input:` or `host-` line before `test-exit`.

### 9.2 pe32 small screens

CYBER ("This Module needs a larger screen area to run!") and CRITIC ("requires
a minimum resolution of 510 x 342") exit 1 at the `/p` preview's 320×240
(**EMPIRICAL**). The pe32 lane adopts the ne16 lane's approach
(`ne16/lane.hh` "Small screens"): an output smaller than 640×480 in either
dimension gets a guest display k times its size (smallest whole k reaching
640×480), the block's `rcClient` is the guest size, every presented frame is
the guest display averaged k×k → 1 and matched to the nearest hardware
palette entry (6 bits per channel, lowest index on ties), mouse coordinates
scale up, `ADPE32SCALE=auto|<k>` overrides. Outputs of 640×480 and larger are
byte-for-byte unchanged.

### 9.3 Display names of package copies

`display_name_of` in `scr/src/catalog.cc` matched ids
(`classic.slide` …), so `ad10.slide` ("SlideShow (10th Anniversary)"),
`ad32.slides3` ("SlideShow (After Dark 3.2)"), `ad32.confetti`
("ConfettiFactory (After Dark 3.2)") and `ad32.mmas` ("Om Appliances (After
Dark 3.2)") kept the cut names. It now matches on the catalog's
`moduleName` (falling back to `displayName`) for `ne16` modules, and
replaces only that leading part, keeping the ` (<package>)` suffix: "Slide
Show (After Dark 3.2)".

---

## 10. Interfaces (exact)

Core (`host/core`, owned by the SCR work package; land these
**first**, before any other change, so the lanes can build against them):

```cpp
// protocol.h
struct Command {
  // … existing fields …
  uint64_t seq = 0;   // input-line number (key/caps/mouse), 1-based per process; 0 otherwise
};
struct InputState {
  // … existing fields …
  uint32_t mouse_buttons = 0;  // bitmask 1 L, 2 R, 4 M; mouse_button stays (= bit 0)
  uint64_t input_seq = 0;      // last input line applied
};

// lane.h
struct LaneStatus {
  bool interactive = false, cursor = false, rotate_ok = false, key_filter = false, wake = false;
  uint32_t source = 0;          // 1 AD4 WantEvents, 2 AD3 0x0E
  uint64_t eaten = 0;           // highest input seq consumed
  uint64_t unsettled = 0;       // lowest input seq the guest may still take (0 none); input_applied stays below it
};
struct ConfigureRequest {
  int slot = -1;
  uint64_t owner = 0;           // real HWND value of the owner, 0 = none
};
enum class ConfigureResult { shown, nothing, unsupported, failed };  // exit 0, 4, 5, 1
class Lane {
  // … existing members …
  virtual LaneStatus status() const { return {}; }
  virtual bool can_configure() const { return false; }   // for --capabilities (no module needed)
  virtual ConfigureResult configure(const std::string& module_path, LaneContext& ctx,
                                    const ConfigureRequest& req, std::string* json_out) {
    return ConfigureResult::unsupported;
  }
};

// env.h
struct Env {
  // … existing …
  std::string state_root;       // "" = in memory; ADSTATE (or the --configure default), §3.1
  bool state_persistent() const { return !state_root.empty(); }
  bool caps_at_start = false;   // ADCAPS
};
// §7.1: the package name ("deluxe", "ad10", …, "legacy-xxxxxxxx") of a module path,
// and <state_root>\<package> ("" in memory).
std::string package_state_name(const Env& env, const std::string& module_path);
std::string package_state_dir(const Env& env, const std::string& module_path);

// status.h (new): the §3.4 record, shared by adhostwin (writer) and the .scr (reader)
struct AdwHostStatusV1 { /* exactly as §3.4 */ };
class StatusPublisher { public: bool open(const Env&); void publish(const LaneStatus&, uint64_t frames,
                                          uint64_t input_applied, uint32_t lane); };
bool read_status(const void* view, AdwHostStatusV1* out);   // seqlock reader, used by the .scr
```

The `.scr` includes `status.h` from core (header-only reader) rather than
duplicating the layout.

Since the seventh release: `Command::Kind::numlock` (`NUMLOCK <0|1>`) is an
input line as `key`, `caps` and `mouse` are (`is_input_line`),
`InputState` has `bool numlock`, and `Env` has `numlock_at_start`
(`ADNUMLOCK`), applied before `Lane::init` as `caps_at_start` is.
`LaneStatus::wake` also carries an After Dark 2.0 module's result 5.

Shared VFS and profile store (`host/win32`, owned by L32; land early,
the ne16 lane uses them):

```cpp
// vfs.hh
class VfsFile;   // below
class Vfs {
 public:
  // … existing mount/to_host/… …
  // Upper-over-lower overlay (§7.3). lower_host may be "" (only virtual files);
  // upper_host "" = in-memory upper.
  void mount_overlay(std::string_view guest_dir, std::string_view lower_host, std::string_view upper_host);
  // Virtual lower-layer file (seeds that are real files, e.g. PROGMAN.INI, *.GRP).
  void add_virtual_file(std::string_view guest_path, std::vector<uint8_t> bytes);
  // H:\<L>\… ↔ <L>:\… (§7.4), read-only; short = 8.3 components for ne16.
  void mount_host_drives(bool short_names);
  std::string host_to_guest(std::string_view host_path) const;   // inside a mount → its form, else H:
  // One file API for both lanes (KERNEL32 CreateFileA/_lopen/_lcreat/OpenFile,
  // KERNEL/INT 21h 3Ch/3Dh/5Bh): reads come from the upper or lower layer; any
  // write access copies up first (a memory upper holds the bytes in memory).
  enum class Access { read, write, read_write };
  enum class Disposition { open_existing, create_always, create_new, open_always, truncate_existing };
  std::unique_ptr<VfsFile> open(std::string_view guest, Access, Disposition, uint32_t* win32_error);
  bool remove(std::string_view guest, uint32_t* win32_error);
  bool rename(std::string_view from, std::string_view to, uint32_t* win32_error);
  bool make_dir(std::string_view guest, uint32_t* win32_error);
  // Merged directory listing (upper wins by name).
  struct DirEntry { std::string name, short_name; uint32_t attributes; uint64_t size, write_time; };
  std::vector<DirEntry> list(std::string_view guest_dir, std::string_view pattern) const;
};
class VfsFile {
 public:
  virtual ~VfsFile() = default;
  virtual int64_t read(void* dst, uint32_t n) = 0;       // bytes read, -1 on error
  virtual int64_t write(const void* src, uint32_t n) = 0;
  virtual int64_t seek(int64_t offset, int whence) = 0;  // SEEK_SET/CUR/END → new position
  virtual uint64_t size() const = 0;
  virtual bool truncate() = 0;                           // at the current position
  virtual bool flush() = 0;                              // persistent upper: atomic replace on close
};

// ini_store.hh (new)
class IniStore {
 public:
  explicit IniStore(Vfs& vfs);
  void add_seed(std::string_view guest_path, std::string_view section, std::string_view key, std::string_view value);
  std::optional<std::string> get(std::string_view guest_path, std::string_view section, std::string_view key);
  std::vector<std::string> sections(std::string_view guest_path);
  std::vector<std::string> keys(std::string_view guest_path, std::string_view section);
  bool set(std::string_view guest_path, std::string_view section, std::optional<std::string_view> key,
           std::optional<std::string_view> value);   // key nullopt = delete section; value nullopt = delete key
};
```

---

## 11. Work packages

Three agents in parallel; file ownership does not overlap.

| Package | Owns | Depends on |
|---|---|---|
| **L32** | `host/win32/**`, `host/pe32/**` | core headers of §10 (SCR lands them first) |
| **L16** | `host/win16/**`, `host/ne16/**`, `host/cpu/**` (CPU bugs only) | core headers; `win32/vfs.hh` + `ini_store.hh` of §10 (L32 lands them early) |
| **SCR** | `host/core/**`, `scr/**` | nothing (lands §10 core first) |

The acceptance tests are in each package's brief (the report that
accompanies this document) and, in short:

* **L32**: `STATUS` toggles for RODGER/MARBLES/RPS/TURTLE with `KEY`
  lines consumed; SWIRLING's Caps Lock effect without interactivity;
  `--configure` of MESSAGES (scripted text persists and the next run shows
  it), FISH, RAIN, CRITIC (`FILE` answer → `H:` folder shown later),
  POINTS/SLOWBURN/SWIRLING; overlay semantics unit tests; CYBER and CRITIC
  at 320×240 exit 0 and deterministic; all 23 AD4 `FBHASH` streams at
  640×480 unchanged without the new variables.
* **L16**: `STATUS` toggles for YBYH/SIMPTRIV/FRANKEN/MIMEHUNT/HOW2DRAW;
  hook-delivered answers change SIMPTRIV/YBYH frames and are consumed;
  LUNATIC playable (its keys consumed, frames react, `LunData.dat` in the
  overlay); `--configure` of MESSAGE3, `tt` MESSYGES, NONSENSE, FISHPRO,
  BUGS, ARTIST, SLIDE, GLOBE, LOGO, WMORPH, LUNATIC with persistence; the
  native bridge's button path matches OLDMOD16's on Deluxe; every package's
  census `FBHASH` unchanged without input or state; DOS Shell 18 000 frames.
* **SCR**: core unit tests (seq, bitmask, held release, `ADCAPS`, status
  publishing, `--capabilities`, `--configure` exit codes, state
  resolution); the `decide()` table; fakehost smoke tests for interactive
  play, Alt exit, cursor/clip, rotation deferral, desktop seed hand-over and
  clean-up, the configure button flow; display names; the DOS Shell
  regression and the last-exit log.
