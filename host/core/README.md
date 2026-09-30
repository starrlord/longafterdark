# host/core — `adw_core` and `adhostwin.exe`

`adhostwin.exe` is the host process of **Long After Dark**. It runs one
module of any of the twelve releases the importer takes (284 modules): the
nine After Dark releases (After Dark 4.0 Deluxe, After Dark 3.2, Totally
Twisted, After Dark 10th Anniversary, The Simpsons Screen Saver, Star Trek:
The Screen Saver and Marvel Comics Screen Posters, the After Dark 2.0 ones,
The Looney Tunes Screen Saver and The Disney Collection Screen Saver), two
releases of other companies' modules for After Dark (Image Smith's Snoopy's
Screen Savers and Binary Software's ScreamSavers) and LucasArts' Star Wars
Screen Entertainment, whose modules run on Delrina's Intermission engine.
Modules built as
32-bit PE images go to the `pe32` lane, and 16-bit NE modules go to the
`ne16` (Classic) lane, which speaks both After Dark 2.x/3.x's module
protocol and Intermission's (`host/ne16/lane.hh`).

This component is the protocol half of that host (DESIGN.md §1 and §4): the
emulated 8-bit display, the frame stream, the stdin command channel, the
environment and the data folder, the virtual clock, pacing, and the
`adhostwin.exe` process that ties them to a **lane** (the thing that produces
frames). The lanes plug in through `adw::Lane`, and
`adhostwin --test-pattern` drives the whole protocol with a synthetic image,
so front-ends can be built and tested without any module.

```
bash tools/build.sh                                  # everything present
AD_BUILD_DIR=build/win-core AD_COMPONENTS="host/core" bash tools/build.sh
```

Tests: `core.unit` (in-process: encoding, hash, env and its win-dir rule,
commands, clock, pacer, pipes, the frame loop with fake I/O; and the
interaction contract of `docs/INTERACTION.md` §3: the `MOUSE`
bitmask, input-line numbers (`NUMLOCK` among them), held releases, `ADCAPS`
and `ADNUMLOCK` at init, the status
record's seqlock under a concurrent writer, `ADSTATUSHANDLE`/`STATUS` lines,
the `--configure` driver's exit codes and JSON, the `ADSTATE` resolution and
package names; and the data folder's path: the base rule, blanks, a trailing
separator, no base, and `Env` agreeing with `data_root.h`), `core.e2e`
(spawns `adhostwin.exe` over real pipes and parses frames with a strict reference frame reader; also `STATUS` lines, a `NUMLOCK` line and `ADNUMLOCK`, the status record through an
inherited handle, `--capabilities` and `--configure`'s usage/lane exit codes,
where a module run finds the default assets root, and that no run creates the
data folder; the test pattern's sound captured, and
sound on changing no frame), `core.lane_detect_assets` (probes every
real module; skips with 77 when the assets are absent), `core.audio` (the
audio engine, below: formats, the ADPCM decoders byte for byte against the
host's own `msacm32` codecs, gains, a golden mixer hash, timing, streams,
SMF, raw MIDI, captures, config) and `core.audio_assets` (real modules' sound, AUDIO.md
§10.3; opt-in, below). No test opens a sound device, and none touches the
real `%LOCALAPPDATA%`: every host the suites spawn gets a scratch
`AD_LOCALAPPDATA` (`tests/test_paths.h`), and the installed assets are found
read-only.

## Command line

```
adhostwin.exe <module> [NAME=VALUE ...]
adhostwin.exe --test-pattern [NAME=VALUE ...]
adhostwin.exe --capabilities
adhostwin.exe --configure <module> --button <slot> [--owner <hwnd>] [NAME=VALUE ...]
```

* `<module>` is a path. If it does not exist as given and is relative, it is
  also tried under the win dir (see `AD_ASSETS_DIR` below), so a catalog
  `path` such as `FILES/AD40/TOASTERS.AD` or
  `packages/ad10/AD10TH/HALLOFFA.AD` works directly.
* `NAME=VALUE` arguments (NAME an identifier) override the environment
  variable of the same name — `adhostwin x.AD ADFRAMES=10 ADFBHASH=1` equals
  setting those in the environment.
* The lane is chosen from the header: `MZ` + `PE\0\0` + i386 → `pe32`;
  `MZ` + `NE` → `ne16`. Anything else is refused. Inside `ne16` the
  module's exports choose the protocol: `MODULE` is an After Dark module,
  `SAVERINIT` + `SAVERDRAW` an Intermission `.IMX`; an NE file with neither
  fails the lane's init (exit 1).

Exit codes: **0** ADFRAMES reached / QUIT / stdin EOF after a GO / the reader
closed stdout / module finished (a headless run whose After Dark 2.0 module
woke the saver: `host/ne16/lane.hh`) · **1** lane init or step failure (including
an exception escaping the lane), stdout I/O error · **2** bad arguments (a
module together with `--test-pattern` included), unreadable or non-AD file,
refused stream target · **3** a valid module whose lane is not built into
this `adhostwin` yet.

**`--capabilities`** prints one line on stdout and exits 0: what this build
has, e.g. `lanes=pe32,ne16 configure=pe32,ne16 abis=afterdark,intermission
status=1 state=1 seed=1 audio=1 numlock=1` (`lanes=` lists the linked lanes,
`configure=` those whose `Lane::can_configure()` is true, `abis=` the union
of their `Lane::abis()` in lane order — the module ABIs this build runs:
`afterdark` (pe32 and ne16) and `intermission` (ne16), the catalog's
`"abi"`, absent meaning `afterdark`; `status`, `state`, `seed`, `audio` are
the core features below; `numlock=1`: the `NUMLOCK` line and `ADNUMLOCK`
are understood, so a front-end sends Num Lock's toggle as it sends Caps
Lock's). It takes no other argument, and readers ignore
keys they do not know. The settings dialog asks it once instead of probing a
module.

**`--configure <module> --button <slot> [--owner <hwnd>]`** runs a module's
own button handler once (INTERACTION.md §6.1): `<slot>` is the catalog index
of a `button` control, `--owner` the real HWND (decimal or `0x` hex) that
owns the module's dialogs (0/absent = none). `AD_ASSETS_DIR`, `ADCVSET` (the
dialog's current values), `ADCAPS`, `ADNUMLOCK` and `ADSTATE` are read as usual; with
`ADSTATE` unset the state is `%LOCALAPPDATA%\LongAfterDark\state` (not
memory; see [The data folder](#the-data-folder)), and `ADSTATE=:memory:`
keeps it in memory. Nothing is streamed;
stdout gets exactly one JSON line,
`{"result":"ok"|"nothing"|"error","dialogs":<n>,"message":"…","written":[…]}`,
usage errors included. Exit codes: **0** the button showed a dialog or
message box · **4** it ran and showed nothing · **5** the module's lane has
no configure support · **1** error · **2** usage (no module, no or a bad
`--button`, a bad `--owner`, `--button`/`--owner` without `--configure`) ·
**3** lane missing. No timeout: a user may keep a dialog open indefinitely.

`adhostwin` is a console-subsystem program: front-ends spawn it with
`CREATE_NO_WINDOW` (and `STARTF_USESTDHANDLES`), inside a
`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` job. It sets
`SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX` so a crash ends the process
(the front-end sees EOF) instead of raising a WER dialog.

## stdout — frames

Only whole frames, one `WriteFile` loop per frame, nothing else:

* **P8** (default): `"P8\n<w> <h>\n"`, 768 bytes of palette (256 × R,G,B),
  `w*h` index bytes, top-down rows, no padding.
* **P6** (`ADSTREAMP6=1`, to exercise a front-end's fallback path):
  `"P6\n<w> <h>\n255\n"` + `w*h*3` RGB.

On `ADSTREAM=1` the host puts fd 1 in binary mode (`_setmode`), duplicates the
stdout handle privately for frames, and re-points the CRT `stdout` and
`STD_OUTPUT_HANDLE` at `NUL`, so a stray `printf` anywhere in the process (shim
code, a lane's debugging) can never land inside a frame. A write failing with
`ERROR_NO_DATA`/`ERROR_BROKEN_PIPE` means the reader went away: the host exits
0. Streaming is refused (exit 2) when stdout is a console, when stdout and
stderr are the same handle or two handles to the same pipe/file — a shell's
`2>&1` duplicates the handle, which `CompareObjectHandles` (Windows 10 1607+)
still sees through; `>NUL 2>&1` is allowed, nothing can be corrupted there —
since log lines would land between frames, or when
stdout is a disk file (no backpressure: frames would be written at render
speed until the disk is full) unless `ADSTREAMFORCE=1`. Headless runs never
write stdout.

Std handles a front-end leaves NULL: under `CREATE_NO_WINDOW` Windows hands
the host its hidden console's handles instead (so a NULL stdout with
`ADSTREAM=1` is the console refusal above, and a NULL stdin is a console
that never speaks). Under `DETACHED_PROCESS` they stay absent: no stdin means
no commands and no EOF, and no stderr means logging is silently dropped.

## stdin — commands

Text lines, `\n`-terminated (`\r\n` accepted, keywords case-insensitive,
blank lines ignored; a final line without a newline still counts):

| Line | Meaning |
|---|---|
| `GO` | produce one frame (lockstep) |
| `SET <idx> <val>` | control value (`idx` 0..65535, `val` int32) — same slots as `ADCVSET` |
| `KEY <vk> <0\|1>` | **Windows virtual-key code** (0..255) down/up |
| `CAPS <0\|1>` | caps-lock toggle state |
| `NUMLOCK <0\|1>` | num-lock toggle state (`InputState::numlock`; the Classic lane's `GetKeyState(VK_NUMLOCK)` bit 0 — Final Exam starts its exam when it changes) |
| `MOUSE <x> <y> <buttons>` | frame-local pointer position; `buttons` a bitmask 0..7: 1 left, 2 right, 4 middle (0/1 keep their old meaning) |
| `QUIT` | exit 0 before the next frame |

**Input lines** (INTERACTION.md §3.2). `KEY`, `CAPS`, `NUMLOCK` and `MOUSE` are
numbered 1, 2, 3 … in the order the host reads them (`Command::seq`; 0 for
every other command); a front-end that counts the lines it sends knows every
line's number without an acknowledgement. `InputState::input_seq` is the
number of the last one applied, and `InputState::mouse_buttons` the bitmask
(`mouse_button` = its left bit).

**Every down is seen.** A `KEY <vk> 0` (or a `MOUSE` that releases a button)
whose down no completed step has seen yet is **held**, together with every
input line after it (so order is kept), and applied right after the next
step. A tap batched into one `GO` therefore shows "down" to pollers for
exactly one step; a held release keeps its number, and the published
`input_applied` never counts a line that has not been applied yet.

Malformed lines (wrong arity, trailing junk, out-of-range numbers) are logged
(first 20) and ignored. A reader thread turns stdin into a queue; the frame
loop never blocks on the pipe itself. stdin may be absent (no handle: no
commands, never EOF), `NUL`, a console (typing commands works — a debugging
aid), a file (a scripted run: `adhostwin x.AD < script.txt`), or a pipe.

**Pacing and lifetime.**

* Before frame 0, if stdin is a pipe or file, the host waits up to
  `ADGOWAITMS` (250 ms) for the first line, so a lockstep front-end's opening
  `GO` is seen before anything is emitted and every frame is an answer to a
  `GO`.
* **Free-running** (no `GO` yet): each frame first applies every queued line,
  then steps. In `ADSTREAM` mode frames are released on a fixed grid at the
  lane's rate (the test pattern: 30 fps; `ADPACEMS` overrides) with a
  high-resolution waitable timer; `ADNOPACE=1` never sleeps (pipe
  backpressure is the only brake). Headless never sleeps.
* **Lockstep** (from the first `GO` on, in either mode): each frame consumes
  lines in order up to and including exactly one `GO`, blocking for it. So a
  `SET`/`KEY`/`MOUSE` sent before a `GO` is applied to exactly the frame that
  `GO` produces — scripted input is deterministic.
* The host exits on `QUIT`, on stdin EOF **after** a `GO` has been seen (EOF
  before any `GO` just means nobody drives us by `GO`: keep running), when the
  stdout reader goes away, at `ADFRAMES`, or when the lane finishes/fails.

## Environment

Parsed once at startup (`adw::Env`), from every `AD*` variable (names are
case-insensitive, as Windows environment names are) plus command-line
overrides. Flags are true when set to anything but empty/`0`/`false`/`no`/
`off` (front-ends send `1`). Numbers are decimal (leading zeros are still
decimal) or `0x` hex. Malformed
values fall back to the default with a warning on stderr.

| Variable | Default | Meaning |
|---|---|---|
| `ADSTREAM=1` | headless | stream frames on stdout |
| `ADSCREENW` / `ADSCREENH` | 640 / 480 | screen size, 1..16384 |
| `ADFRAMES=<n>` | 0 = unbounded | stop after n frames |
| `ADFBHASH=1` | off | `FBHASH <frame> <hex64>` per frame on stderr |
| `ADOUT=<dir>` | — | headless only: `frame_NNNNN.ppm` (P6) per frame; dir is created |
| `ADCVSET=<i>=<v>,…` | — | initial control values, applied before lane init (`<i>:<v>` accepted too) |
| `ADSEED=<n>\|random` | 1 | seed of every randomness source the host owns; `random` draws one and logs it for replay |
| `ADNOPACE=1` | off | never sleep |
| `AD_ASSETS_DIR` | `%LOCALAPPDATA%\LongAfterDark\assets` | asset **root**; Windows files are under its win dir: `<root>\win` when that holds `FILES\` (Deluxe), `packages\` (every other release, PACKAGES.md §4) or `catalog-win.json`; else `<root>` itself when *it* holds one of them (the variable named the win dir); else `<root>\win` (`Env::win_assets_dir()`; the importer applies the same rule) |
| `AD_LOCALAPPDATA=<dir>` | `LOCALAPPDATA` | stands in for `LOCALAPPDATA` when the data folder `<dir>\LongAfterDark` is derived (blank = unset): for tests and for trying a build without touching the real folder. The rule is `data_root_base()`'s (below) |
| `ADTRACE=<cat>,…` | — | trace categories to stderr (`all`/`*` = every one; case-insensitive, and `tracing()` is one relaxed load when unset). Core uses `proto` (every applied command, lockstep transitions) and `audio` (the audio engine, below) |
| `ADPACEMS=<ms>` | lane's rate | *(host-local)* frame period for free-run pacing and the headless clock step (fractional; rounded to µs, at least 1 µs) |
| `ADSTREAMFORCE=1` | off | *(host-local)* stream even into a disk file |
| `ADSTREAMP6=1` | off | *(host-local)* P6 frames instead of P8 |
| `ADGOWAITMS=<ms>` | 250 | *(host-local)* pre-frame-0 wait for the first stdin line (0 disables) |
| `ADCAPS=0\|1` | 0 | Caps Lock toggle at start, in `InputState.caps` **before** `Lane::init` (modules latch it when they are created) |
| `ADNUMLOCK=0\|1` | 0 | Num Lock toggle at start, in `InputState.numlock` **before** `Lane::init` (Final Exam latches it as it starts, and a change starts its exam); logged as `num lock on` |
| `ADSTATE=<dir>\|:memory:` | in memory | per-user state root (`Env::state_root`, INTERACTION.md §7). Unset or `:memory:` = the lanes keep their writable overlay in memory, so headless runs never read or write user state; `--configure` with it unset uses `%LOCALAPPDATA%\LongAfterDark\state`. `package_state_name()` / `package_state_dir()` give a module's package: `deluxe` for `…\FILES\<dir>\<module>`, `<id>` for `…\packages\<id>\<dir>\<module>`, `legacy-<fnv32 of the lower-cased module dir, 8 hex>` otherwise |
| `ADSTATUSHANDLE=<n>` | — | decimal or `0x` value of an **inherited** handle to a pagefile section (≥ 4096 bytes): the status record is published there (below). A value that does not map is logged once and ignored |
| `ADSTATUSLOG=1` | off | also print `STATUS <frame> flags=0x<hex> applied=<n> eaten=<n> src=<s>` on stderr at frame 0 and whenever flags, `applied` or `eaten` change |
| `ADSOUND=1` | off | **guest sound on** (AUDIO.md §4): the lanes give the modules their sound devices. In a streamed run it also plays on the real device (WASAPI; MIDI through the MIDI mapper). A headless run never opens a device |
| `ADAUDIOOUT=<file.wav>` | — | sound on (as `ADSOUND=1`) and **captured**: the mix to `<file.wav>` (16-bit stereo at `ADAUDIORATE`), the MIDI messages to `<file>.mid` (the extension replaced; a name without one, or ending in `.mid`, gets `.mid` appended). Never plays by itself; with `ADSOUND=1 ADSTREAM=1` too, both |
| `ADVOLUME=0..100` | 50 | After Dark's volume slider, handed to the modules (AD4 block `+0x3C`, the Classic bridge; for an Intermission module, `ANTSW.INI [Intermission] Volume`, 0 while sound is off). The host adds no gain of its own. Out of range is clamped (warning) |
| `ADAUDIORATE=<hz>` | 44100 | mixer and capture rate, 8000..96000 (clamped). Nothing the guest sees depends on it |
| `ADAUDIOLATENCYMS=<ms>` | 80 | live PCM ring target and MIDI delay, 20..500 (clamped) |
| `ADAUDIOLIVE=0` | on | with `ADSOUND=1 ADSTREAM=1`: open no device (tests of streamed runs) |
| `ADMIDI=0` | on | no live MIDI (still captured) |
| `ADMIDIDEV=<n>` | −1 | live `midiOut` device id; −1 = the MIDI mapper |
| `ADMIDIBASE=1` | off | keep channels 13–16 of MPC dual-mode songs (AUDIO.md §6.4) |
| `ADTESTAUDIO=1` | off | `--test-pattern` with sound on: a 440 Hz blip every second, a MIDI note every second second (below) |

Lanes read their own knobs from the same snapshot: `env.get("ADFOO")`. They
are listed in the lanes' headers: `host/pe32/lane.hh` and `host/ne16/lane.hh`
(`ADNE16KIND=auto|ad3|imx` forces the Classic lane's module protocol,
`ADNE16READER=auto|imq|native` its Intermission reader, `ADNE16BRIDGE` its
After Dark bridge, …).

## The data folder

Long After Dark keeps everything per-user in one folder,
`%LOCALAPPDATA%\LongAfterDark`: `assets\` (what `adimport` installs),
`downloads\`, `state\` (the modules' own files, INTERACTION.md §7), and the
saver's `settings.ini` and caches. `adw/core/data_root.h` says where that
folder is, and it is the one implementation for every program: it is inline
and needs only kernel32, so the `.scr` and `adimport`, which do not link
`adw_core`, include it the way the `.scr` includes `status.h` (include dir
`host/core/include`). Every program that derives a default location
from the data folder takes it from there:

```cpp
#include "adw/core/data_root.h"

std::wstring base = adw::data_root_base();       // AD_LOCALAPPDATA, else LOCALAPPDATA; "" if neither
if (base.empty()) base = known_local_app_data();  // the caller's own SHGetKnownFolderPath fallback
std::wstring root = adw::data_root_path(base);    // <base>\LongAfterDark: assets\, downloads\, state\, settings.ini
```

`data_root_base()` trims each variable, and a blank `AD_LOCALAPPDATA` counts
as unset. `data_root_path()` trims the base and tolerates a trailing
separator; a blank base gives `""`. Neither looks at the disk or creates
anything.

**In adhostwin.** `Env::parse` applies `data_root_base()`'s rule to its
variable map (`from_process` snapshots `LOCALAPPDATA` along with the `AD*`
names), sets `Env::data_root` = `data_root_path(<base>)` and derives the
default `assets_root` from it, and `use_configure_state_default()` derives
`--configure`'s state root from it. None of them touch the disk. With
explicit `AD_ASSETS_DIR` and `ADSTATE`, which the `.scr` gives every host, no
location comes from the data folder.

## The status record

`status.h` (INTERACTION.md §3.4): a 64-byte `AdwHostStatusV1` ("ADWS",
version 1) at offset 0 of the section named by `ADSTATUSHANDLE`. `run_host`
publishes it once after `Lane::init` (frame 0) and after every completed step,
**before** that step's frame is written, so a front-end that reads it when a
frame arrives sees that step's verdict:

| Field | From |
|---|---|
| `flags` | `Lane::status()`: `ADWS_INTERACTIVE` 0x01, `ADWS_CURSOR` 0x02, `ADWS_ROTATE_OK` 0x04, `ADWS_KEY_FILTER` 0x08, `ADWS_WAKE` 0x10 (the module asked the saver to end: `WM_CLOSE`/`SC_CLOSE` to the saver window, or an After Dark 2.0 module's result 5); the host adds `ADWS_READY` 0x20 |
| `frames` | steps completed |
| `input_applied` | the last input line applied **before** the last completed step, kept below `LaneStatus::unsettled` (a line the guest may still take from a queue: the Classic lane's saver-window queue while a long DRAWFRAME is suspended) |
| `input_eaten` | `LaneStatus::eaten`: the highest input line the module consumed |
| `source` | 1 AD4 `WantEvents`, 2 AD3 `0x0E` |
| `lane` | 1 pe32, 2 ne16, 3 test pattern |

Writer (`StatusPublisher`, one per host): `gen` odd, fields, barrier, `gen`
even. Reader (`read_status`, header-only, used by the `.scr`): read `gen`,
retry while odd, copy, re-read `gen`; equal = consistent; it gives up after 4
tries.

## FBHASH

`FBHASH <frame> <hex64>` — frame is 0-based, the hash is 16 lower-case hex
digits of FNV-1a 64 (standard offset basis `0xcbf29ce484222325`, prime
`0x100000001b3`) over the P8 **body**: the 768 palette bytes, then the `w*h`
indices. So `FBHASH` equals the FNV-1a of the bytes after a P8 frame's header,
and a palette-only animation changes it.

## Time

`adw::VirtualClock` is the single time source every emulated time API reads.

* Headless: `fixed_step` — virtual time is `frame × step` (step = lane rate or
  `ADPACEMS`), so runs are bit-for-bit reproducible and never sleep.
* `ADSTREAM`: `realtime` — follows the wall clock. Each gap between two
  observations of the clock (a time read, or a frame start) is capped at
  250 ms, so a front-end that stops sending `GO` does not make the module leap
  ahead on resume, while a module that busy-waits inside one long step (a
  calibration loop, a `Delay` built on `GetTickCount`) still sees the wall
  clock run at full speed however long the step is.
* `read_us()` / `read_tick_count()` are what shims call; an optional per-read
  nudge (`set_read_step_us`) lets a module that busy-waits on the tick count
  inside one frame make progress, deterministically. **It defaults to 0**, and
  on the headless fixed-step clock nothing else moves time within a step, so
  a lane whose modules may spin on `GetTickCount`/`timeGetTime` must set it
  (a few µs) or such a loop never ends.
* `tick_count()` = `0x00100000` ms (~17 min of "uptime") + virtual ms.

## Audio

`adw/core/audio.h` is the host audio engine of `docs/AUDIO.md` (the
header is §5 there, **frozen**; the semantics are §6). The lanes translate
DirectSound, ACM, WINMM and MMSYSTEM onto it.

**Wiring.** `run_host` makes one engine per module or test-pattern run from
`audio::Config::from_env(env)` and puts it in `LaneContext::audio`: a
disabled engine when sound is off (no `ADSOUND=1`, no `ADAUDIOOUT`), and
`audio::null_engine()` for `--configure`. After every step it calls
`advance(clock.now_us())` — unless the lane advanced the engine during the
step: the lane sees its engine through `LaneEngine` (`src/host.cc`), which
forwards every method and notes that, and then the step's end is the
lane's (ne16's guest time runs ahead of the core clock, and it stops short
of a timer period it has not yet delivered: `host/ne16/lane.hh` "Sound";
a method `audio.h` gains must be forwarded there too); when the loop ends (QUIT, stdin EOF, `ADFRAMES`,
the lane's end or failure) it calls `shutdown(now)` **before**
`Lane::shutdown()`, then prints one line when sound is on:

```
[audio] voices=<n> chunks=<n> songs=<n> midi_events=<n> underruns=<n> drops=<n> frames=<n>
```

The engine stays alive until the process exits, so a lane's destructor may
still release its voices and songs; after `shutdown` every call is accepted
and nothing more is heard or written.

**For a lane.** Take `ctx.audio ? *ctx.audio : audio::null_engine()`. While
`enabled()` is false, call nothing but `config()`/`enabled()` and keep the
sound-off behaviour. Guest time for every call is a **peek** of the virtual
clock (pe32 `rt.clock().now_us()`, ne16 `Runtime16::peek_us()`), never a read
that nudges it; a time earlier than the latest the engine has seen is taken
as the latest. Every call with a time first renders up to it, so a change is
heard from output frame `floor(t·R/10⁶)`.

**Model.** State is kept in continuous virtual time, so everything the guest
can observe is a function of its calls and their times, independent of
`ADAUDIORATE`, of how often `advance` runs and of the sinks:

* A voice started at `t0` from frame `f0` at rate `r` is at frame
  `f0 + floor((t−t0)·r/10⁶)` (wrapping when looping); a non-looping one ends
  at `t0 + ceil((N−f0)·10⁶/r)` and stops there with its cursor back at 0.
  `write_buffer` while playing is heard from its time. At most 256 voices.
* Streams play their chunks back to back. A stream that has run dry resumes
  from the next write's time; `pause` holds the position, `restart` resumes,
  `reset`/`close` complete every queued chunk at that time, in order.
* Songs: SMF format 0/1 (and RIFF RMID), PPQN or SMPTE division, the tempo
  map across tracks; length = the last end of track. A song with note-ons on
  channel 13 and on any of 1–10 is MPC dual-mode and loses channels 13–16
  (`ADMIDIBASE=1` keeps them). The player tracks each channel's CC7 (default
  100) and sends `round(CC7 × the MIDI bus gain)` (the mean of its sides) at
  every play, for every song event, and again when the bus gain changes.
  Stop, seek and close send note-offs for what sounds, then CC123 on the
  channels used; a play after a seek first re-sends the program, controller
  and pitch-bend state in force there (bank select, program, controllers,
  bend). A natural end sends note-offs for anything still sounding.
* Raw MIDI (`midi_short`/`midi_long`/`midi_reset`, added after the freeze
  for a guest that sequences itself through Win16 `midiOut*`, AUDIO.md §5 and
  §6.4): the guest's messages on the same bus, stamped with their time
  (song events due by then go first). A short message is the DWORD
  `midiOutShortMsg` takes, running status included; a long one is SysEx or a
  message stream; system messages reach the log as `F7` escapes. The bus
  gain follows the songs' CC7 rule (the guest's CC7 scaled, re-sent on a gain
  change, a channel's scaled default first when not at unity). A reset sends
  note-offs for what the port left sounding, then CC64 0 and CC123 on every
  channel it used; `shutdown` silences it like a playing song. The bodies in
  the header do nothing (a disabled engine's answer), so older `Engine`
  implementations — the lanes' test doubles — still build.
* Events (`voice_end`, `chunk_done`, `song_end`) carry their exact times and
  are polled in (time, issue) order. A cancelled cause removes its future
  event (a stop before the end); `close_song` drops the song's unpolled
  events; a closed stream's are still polled.
* The mixer is integer only: 8-bit `(x−128)<<8`, 16-bit as is, mono to both
  sides, linear interpolation (16-bit fraction), voice gain × bus gain in Q15,
  `int32` accumulation, saturation once per output frame. Output frame `k` is
  virtual time `k·10⁶/R`; positions come from 64/128-bit integer arithmetic.
  With no PCM sink nothing is mixed at all.

**Decoders.** `decode` matches Windows' own codecs byte for byte (checked
against the host's `msacm32` by `core.audio`, partial blocks included):
imaadp32 decodes whole blocks only (a trailing partial block yields
nothing), msadp32 also decodes a trailing partial block that holds its
`7·channels`-byte header. `decoded_size` is `acmStreamSize(SOURCE)`: whole
blocks, rounded up, 0 below one block; `encoded_size_for` is
`acmStreamSize(DESTINATION)`: the whole blocks whose output fits, rounded
down, 0 when none does. PCM is returned unchanged. `gain_from_ds` uses a
committed table (`src/audio_db_table.inc`) of `round(32768·10^(mB/2000))`.

**Sinks.** None in a headless `ADSOUND=1` run. `ADAUDIOOUT` captures: the
WAV is created at engine start (a failure is logged and the run goes on
without it) and its header is patched every 5 s of guest time and at
`shutdown`, so a killed host leaves a readable file of
`floor(t_end·R/10⁶)` frames. The `.mid` log is format 0, 500 PPQN at the
default tempo (one tick = 1 ms of guest time), with explicit status bytes:
exactly the messages sent to the synth (song events after the MPC rule,
scaled CC7, note-offs, CC123, SysEx), its track length patched on the same
grid and its end of track written at `shutdown`. A hand-built `Config` with
`capture_wav` and no `capture_mid` gets the same `.mid` derivation. Live
output (`Config::live`: `ADSOUND=1` in a streamed run, `ADAUDIOLIVE` not 0)
runs on threads of its own that the engine never waits on:

* PCM: WASAPI shared mode on the default console endpoint, event-driven,
  `AUTOCONVERTPCM | SRC_DEFAULT_QUALITY`, 16-bit stereo at `R`, session name
  "Long After Dark". A single-producer/single-consumer ring: playback starts
  once it holds `ADAUDIOLATENCYMS`; an underrun plays silence (counted) and
  waits for the ring to fill again; above latency + 100 ms the oldest frames
  are dropped back to the latency (counted). A lost device is reopened at
  most once a second, dropping meanwhile. When WASAPI cannot start,
  `waveOut` on `WAVE_MAPPER` (4 buffers of latency/4); when that fails too,
  one log line and silence.
* MIDI (`ADMIDI` not 0): `midiOutOpen(ADMIDIDEV)` on its own thread, each
  message at `anchor_wall + (at − anchor_guest) + latency`, the anchor
  re-taken at every `advance`; SysEx through `midiOutLongMsg`. Closing sends
  note-offs, CC123 on all 16 channels, `midiOutReset`. When nothing is queued
  and no `advance` has come for the latency + 500 ms (the saver sends no GO
  while the display is off; a stalled step), the notes still sounding get
  note-offs and CC123, so none drones on while the emulation waits (live
  only: the guest and the `.mid` never see it). A device that will not open
  is logged once; the music is still captured.

`shutdown` also sends note-offs and CC123 for every playing song to the
sinks, without changing what the guest sees. Nothing ever touches the system
volume or mixer.

**`ADTRACE=audio`**: one line per engine call that changes something, each
with its virtual time: `[audio] @<µs> voice <id> start buf <b> frame <f> of
<n> rate <hz> loop <0|1> gain <l>/<r> bus <wave|midi>`, `voice <id>
stop|end|cursor|rate|gain|loop …` (a natural `end` carries its exact time),
`stream <id> open|chunk|pause|restart|reset|close|gain …`, `song <id>
load|start|stop|seek|end|close …`, `bus <wave|midi> gain <l>/<r>`, plus the
sinks' state (captures, live PCM and MIDI, underruns and drops at the end).

**Asset cases** (`core.audio_assets`, AUDIO.md §10.3): opt-in with
`AD_AUDIO_ASSETS=1` (every case) or a comma list of `toasters`, `toaster2`,
`burns`, `fish`, `bungee`, `toast3`, `halloffa`; the assets come from
`AD_E2E_ASSETS`, `AD_ASSETS_DIR` or the installed folder (read-only), every
host gets a scratch `AD_LOCALAPPDATA`, and a case whose lane is not linked is
skipped (77 when none can run). Each runs the module headless with
`ADAUDIOOUT` and `ADTRACE=audio` and checks its row of the table (note-ons
in the `.mid`, voices, a window above −40 dBFS, speech heard at the voice
starts, TOAST3's song loop over 5 minutes with its Music control at
"Always", `ADCVSET=2=80`: the default "Once" plays the song a single time);
then the first 1800-frame case
runs twice and must give byte-identical captures and `FBHASH` streams.

**The one live check**: `AD_AUDIO_LIVE_TEST=1 adw_core_audio_tests
live_device_check` (a person listening; never CI) plays 1 s of 440 Hz at
−30 dBFS through WASAPI and then a soft MIDI note, 3 s in all, and checks
that nothing underran after the prefill.

## Library surface (`#include "adw/core/…"`)

| Header | What |
|---|---|
| `screen.h` | `Screen` (8-bit fb + `std::array<RGBQUAD,256>` palette + dirty flag; `attach()` to external memory, negative stride = bottom-up DIB), `encode_p8/p6`, `fbhash`, `write_ppm`, `kStaticColors` (Win95 20 static colours) |
| `protocol.h` | `Command` + `parse_command`, `InputState`, `CommandSource`/`StdinReader`, `FrameSink`/`StdoutSink`, `write_all` |
| `env.h` | `Env` (typed §1 fields + `vars` snapshot + `get`/`flag`/`traced`/`win_assets_dir`; `state_root`, `caps_at_start`, `numlock_at_start`; `data_root`), `package_state_name`/`package_state_dir` |
| `data_root.h` | the data folder (above), header-only: `data_root_base`, `data_root_path`, `kDataDirName` / `kDataRootBaseVar` |
| `clock.h` | `VirtualClock` |
| `pacer.h` | `Pacer` |
| `lane.h` | `Lane`, `LaneContext` (`env`, `screen`, `clock`, `input`, `audio`), `StepResult`, `LaneStatus`, `ConfigureRequest`/`ConfigureResult`, `configure_json`, `probe_module`, lane factories |
| `audio.h` | the audio engine (above; AUDIO.md §5, **frozen**): `audio::Config` (`from_env`), `Engine` (buffers, voices, streams, songs, bus gains, events), `make_engine`, `null_engine`; formats (`WaveFormat`, `parse_waveformat`, `waveformat_bytes`, `parse_wave`, `riff_extent`, `pcm_format`/`ima_adpcm_format`/`ms_adpcm_format`, `playable`/`decodable`), the IMA/MS-ADPCM decoders (`decode`, `decoded_size`, `encoded_size_for`), gains (`gain_from_ds`, `gain_from_mm`, `mm_from_gain`). Links `ole32`, `winmm`, `uuid` |
| `status.h` | `AdwHostStatusV1` + `ADWS_*` flags, `write_status`/`read_status` (inline), `StatusPublisher` |
| `host.h` | `run_host` (the frame loop), `configure_module` (the `--configure` driver), `HostIo`, exit codes |
| `log.h` | `log`, `trace`/`tracing` (ADTRACE), `write_stderr` |
| `rng.h` | `Rng` (splitmix64; `Rng::derive(seed, stream)`) |
| `text.h` | `widen` / `narrow` (UTF-8 ↔ UTF-16) |
| `fnv.h` | `fnv1a64` |

### Writing a lane

```cpp
class Pe32Lane : public adw::Lane {
  const char* name() const override { return "pe32"; }
  bool init(const std::string& module, adw::LaneContext& ctx) override;  // ctx.input has ADCVSET
  uint32_t frame_interval_us() const override;   // the module's natural rate
  void on_command(const adw::Command& c) override; // SET/KEY/CAPS/NUMLOCK/MOUSE, in order, before the step
  adw::StepResult step() override;                 // draw into ctx.screen, mark_dirty() if changed
  void shutdown() override;
};
namespace adw { std::unique_ptr<Lane> make_pe32_lane() { return std::make_unique<Pe32Lane>(); } }
```

Interaction (INTERACTION.md §3, §5, §6): `on_command` gets each input line
with its `seq`; `status()` (called after init and after every step) reports
`LaneStatus{interactive, cursor, rotate_ok, key_filter, wake, source,
eaten}`, where `eaten` is the highest `seq` the module consumed as its own.
`can_configure()` says whether `configure(module, ctx, req, &json)` is
implemented; that entry point runs on a fresh lane object **instead of**
`init`/`step` (no `shutdown()` afterwards), returns `shown`/`nothing`/
`unsupported`/`failed` (exit 0/4/5/1) and may fill the JSON line with
`configure_json()`. `abis()` names the module ABIs the lane runs, for
`--capabilities` (pe32 `{"afterdark"}`, ne16 `{"afterdark", "intermission"}`).
All five have defaults (nothing to report, no configure, no ABI).

A lane should catch its emulator's exceptions and return `false` /
`StepResult::failed`; one that escapes `init`, `on_command` or `step` is
still contained by `run_host` (logged, exit 1, `shutdown()` run if `init`
had succeeded) rather than terminating the host.

The frame loop calls `ctx.clock.begin_frame()` before each `step()`. A lane
whose module draws through GDI onto a DIB section over emulated memory can
`ctx.screen.attach(top_row, stride)` once and just `mark_dirty()` per frame:
presenting reads the module's pixels in place. If `step()` leaves the screen
clean, the host re-sends the previous frame's cached encoding.

To have `adhostwin` link a lane, the lane's component defines the factory in a
library target and registers it in its `CMakeLists.txt`:

```cmake
set_property(GLOBAL PROPERTY ADW_LANE_PE32_TARGET adw_lane_pe32)   # or ADW_LANE_NE16_TARGET adw_lane_ne16
```

Core links it (deferred to the end of configuration, since core is configured
first) and compiles `adhostwin` — and `core.e2e`, whose exit-code
expectations follow — with `ADW_HAVE_LANE_PE32=1` / `ADW_HAVE_LANE_NE16=1`.
With a lane linked, `core.e2e` expects a header-only stub image of that kind
to fail the lane's `init` cleanly (exit 1, not a crash), and
`core.lane_detect_assets` expects real modules to be routed to the lane
(exit 0 or 1) instead of exit 3.

## The test pattern

`adhostwin --test-pattern` (30 fps): diagonal rainbow bands animated purely by
palette cycling (entries 10..137; control 0 = speed 1..16), a bouncing box and
circle, a scanning line, a seeded star field (`ADSEED`), a white border with
orientation markers (red top-left, green top-right, blue bottom-left, yellow
bottom-right), a `FRAME nnnnnn` counter, bars for controls 0..7 (`SET`,
`ADCVSET`; 0..100), the last `KEY`/`CAPS`, and a crosshair at the last
`MOUSE` position (yellow square while the left button is down; the readout
adds `R`/`M` for the other buttons). Every pixel is a
function of (frame, seed, input so far) — never of the clock — so the stream
is identical run to run, headless or streamed.

`ADTESTAUDIO=1`, with sound on (`ADSOUND=1` or `ADAUDIOOUT`), drives the
audio engine without a module: a 100 ms 440 Hz blip (22050 Hz PCM16 at
−6 dBFS, from an integer oscillator) at every whole second of virtual time,
and a one-note Standard MIDI File (C4, 200 ms) played from the start every
second second. Each is stamped with its exact whole-second time, so a capture
has them at `n` s whatever the frame rate; the pixels are unchanged.

`ADTESTINTERACTIVE=1` makes it a stand-in game for the interaction protocol:
`CAPS 1` sets interactive (`CAPS 0` clears it; the readout says
`INTERACTIVE`), and while interactive every `KEY`/`MOUSE` line is consumed
(`status().eaten`); `ADTESTINTERACTIVE=cursor` also raises the cursor flag.
Without it the pattern is never interactive and eats nothing.
