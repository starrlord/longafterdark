# Long After Dark — audio

The original modules make sound in five different ways; before this work the
host refused or silenced all of them. This document records how each one works,
from our own disassembly and from traced runs of every module of the five
releases (§1, §2). It then specifies the host audio engine that replaces the
silence (§3–§6), how each lane maps its guest APIs onto it (§7, §8), the
saver's sound settings (§9), and the tests (§10). The work is split between
four implementers (§11).

Status: **implemented and integrated** (2026-09-26). The engine (§5, §6),
both lanes (§7, §8) and the saver (§9) are in the tree, and the integration
census of §10.4 has run over all 202 modules (§10.5). The design census (§1)
was taken on the silent-stub host before that, and where the specification
below says "today" or "as today" it means that silent host, which is still
exactly what the lanes do with sound off. The releases added later are
outside that census and have sections of their own: Star Wars Screen
Entertainment's music (§10.6), Star Trek: The Screen Saver's AD_SND 1.0
(§2.12, §10.7), and the five of the twelve-release registry, with the
Looney Tunes' and the Disney Collection's music paths (§2.9), the host's
own AD_SND for Snoopy's Screen Savers (§2.10) and their results (§10.8).
Evidence is marked as in the
other documents: **VERIFIED** (our disassembly, with addresses), **EMPIRICAL**
(observed in runs or measured on the files) and **UNVERIFIED** (leads still
open).

Tooling (all under `research/win/audio/`, gitignored like the rest of
`research/`):

| File | What |
|---|---|
| `static_census.py` → `static.json` | imports, WAVE resources and their formats, named music/sound files resolved against each package |
| `dynamic_census.py` → `dynamic.json` | every module for 1800 frames (30 s of emulated time) with `ADSOUND=1 ADTRACE=api,api16,sound`; sound calls counted per function and caller |
| `census.py` → **`census.json`** | the merge: per module its mechanisms, data, what it is expected to play, what the stub host saw |
| `table.py` | the table of §1.3 |
| `verify.py` | the first cut of the verification census of §10.4 |
| `census_result.py` → **`census_result.json`**, `census_result.md` | the integration census (§10.5): every module 30 s with `ADAUDIOOUT`, classified against `census.json` and the implementers' findings, plus a 5-minute pass for the quiet ones |
| `spot_check.py` | the content checks of §10.5: a capture's note-ons against the song files, and its voices against the modules' own WAVE resources |
| `dis/` | listings made for this work: `HALLOFFA.AD`, `TOAST2K.AD`, `ADXPL310.DLL`, `ADXPL40.DLL`, the AD 3.2 and Simpsons `AD_SND.DLL` |

---

## 0. Decisions at a glance

* **One engine, in `adw_core`** (`adw/core/audio.h`, §5, frozen): a
  deterministic integer PCM mixer (voices over buffers, streams of chunks),
  a Standard MIDI File player, ADPCM decoders, and sinks: the real device
  (WASAPI shared mode, `waveOut` fallback) and a capture sink (a WAV file plus
  a MIDI event log). Lanes translate DirectSound, ACM, WINMM and MMSYSTEM onto
  it.
* **Guest-visible audio is a pure function of the guest's calls and virtual
  time.** Positions, status, completion times and callbacks come from the
  engine's model, never from a device, so the live device, the capture sink
  and no sink at all give the guest identical answers. The emulation never
  waits on a device; a late device underruns and a full one drops.
* **Sound off is today's behaviour, bit for bit.** Without `ADSOUND=1` or
  `ADAUDIOOUT`, both lanes behave exactly as now: `dsound.dll` refused, MCI
  and MIDI absent, the Classic lane's one silent wave device. The headless
  `FBHASH` baselines do not move.
* **Knobs** (§4): `ADSOUND=1` turns the guest's sound on and, in a streamed
  run, plays it. `ADAUDIOOUT=<file.wav>` turns it on and captures it (MIDI goes
  to `<file>.mid`). `ADVOLUME=0..100` is After Dark's own volume slider
  (default 50), handed to the modules.
* **pe32**: a DirectSound emulation with the methods ADXPL510 calls (§2.1,
  VERIFIED by disassembly), ACM emulated with our own IMA-ADPCM decoder,
  the MCI sequencer on the SMF player, two aux devices and no mixer (so the
  engine keeps per-buffer volume), `waveOut` for Hall of Fame. CD audio is
  still refused (no disc), and `waveIn` still has no device.
* **ne16**: `sndPlaySound` through the engine (PCM and MS-ADPCM, which the
  Totally Twisted modules use), one MIDI output device, and the MCI sequencer
  strings. Song ends reach the engines' `adwMidiCall` window as
  `MM_MCINOTIFY`. `MM_WOM_*` and `MM_MCINOTIFY` are delivered in a fixed
  order at defined points.
* **MIDI** plays live through the Windows MIDI mapper (the GS wavetable
  synth by default). Captures log MIDI events as a `.mid` file, since there is
  no deterministic synthesizer to render it. Songs authored for both MPC
  levels drop their base-level channels 13–16, as the mapper did (EMPIRICAL,
  §6.4).
* **The saver**: sound **on** by default, volume 50, and played only by the
  primary monitor's host. The settings dialog gets a Sound setting and a
  Volume slider. **Preview** plays sound; the live thumbnail, `/p` and the
  thumbnail generator never do (§9).

---

## 1. Census

### 1.1 Method

* **Static** (`static_census.py`): every `*.AD` and `*.DLL` under the
  assets (`build/win-pkg-setup/assets`, the five releases of the time
  imported). It
  records imports from WINMM, MMSYSTEM, MSACM32, AD_SND and the engines, and
  every resource that is a RIFF WAVE (type `3000` in the 16-bit releases,
  `WAV` in the AD4 ones) with its format and length. It also records the
  `.mid`, `.wav`, `.srf` and `*_snd.dll` names in the ASCII strings and
  string tables, resolved case-insensitively against the module folder and
  then the package's `ENGINE` folder.
* **Dynamic** (`dynamic_census.py`): each module ran for 1800 frames (30 s
  of emulated time at 60 fps), `ADSOUND=1`, `ADTRACE=api,api16,sound`,
  16 at a time, with a scratch `AD_LOCALAPPDATA`. All 202 runs exited 0. The
  run saw only what the stub host lets through: it refuses `dsound.dll`, so
  no AD4 effect or song is ever reached, and it reports no MIDI device, so no
  16-bit engine ever opens a song. What it does show is every
  `sndPlaySound`, with its flags and caller, and every probe.
* **Disassembly**: `research/win/dis/ADXPL510.DLL.asm` (existing) and the
  listings in `research/win/audio/dis/`. Addresses below are into those.

### 1.2 Totals

| | |
|---|---|
| Modules (catalog entries) | 202 (129 distinct files) |
| Make sound | **136** (35 pe32, 101 ne16; 89 distinct files) |
| Play music | **27** (and 2 name a song their release does not ship: Deluxe LIFE `music\nutcrack.mid`, Simpsons GRAMPA `music\grandpa.mid`) |
| Silent | 66 |
| DirectSound (ADXPL510 or its static copy) | 31 |
| … of which need IMA-ADPCM decoding through ACM | 29 |
| MCI sequencer, 32-bit (`mciSendCommandA`) | 8 |
| AD_SND → `sndPlaySound`, module's own imports | 37 |
| AD 3.x engine → AD_SND → `sndPlaySound` | 61 (ADXPL300 23, ADXPL310 15, ADXPL40 23) |
| MCI sequencer, 16-bit (`mciSendString` + `MM_MCINOTIFY`) | 21 (ADXPL300 9, ADXPL310 5, ADXPL40 7) |
| `waveOut` streaming (Hall of Fame) | 1 |
| `waveIn` (music-reactive mode) | 2 (POINTS, SWIRLING) |
| CD audio (Music Choice) | 3 (POINTS, SLOWBURN, SWIRLING) |

### 1.3 Module → mechanism → data

Byte-identical modules in several releases share a row. **Releases**: D =
After Dark 4.0 Deluxe, 10 = 10th Anniversary, 32 = After Dark 3.2, TT =
Totally Twisted, S = The Simpsons. **Engine**: X510 = ADXPL510 (X510* =
the engine linked statically into the module), X300/X310/X40 = the 16-bit
engines, — = none. **Mechanism**:

* **DS**: DirectSound through `XNoiseMaker` (DS* = the module's static copy).
* **ACM**: IMA-ADPCM decoded through MSACM32 (`XNoise::DecompressWave`).
* **SEQ32**: MCI sequencer through `WinMidiPlayer` (`mciSendCommandA`).
* **wIn**: `waveIn`.
* **DSprobe**: the module's own `DirectSoundCreate` probe.
* **CD**: CD audio through `XCdAudio`/`SoundHelp`.
* **WO**: `waveOut`.
* **SND**: the module calls AD_SND itself (→ `sndPlaySound`).
* **E300/E310/E40**: the engine's `XSoundDatabase` → AD_SND → `sndPlaySound`.
* **SEQ16**: the engine's `XSoundMusicPlayer` → `mciSendString` + `MM_MCINOTIFY`.

**Data**: "n WAV s" = n WAVE resources, s seconds in all, then format ×
count (P = PCM, IMA/MS = ADPCM, rate / bits). **Plays in 30 s**: the
`sndPlaySound` calls the stub host saw (ne16 only; the one
`sndPlaySound(NULL)` at unload is not counted). "—" marks pe32, where the
refusal hides them.

| Module | Releases | Engine | Mechanism | Data | Plays in 30 s |
|---|---|---|---|---|---|
| BADDOG | D,10 | X510 | DS ACM | 34 WAV 26s (IMA 22k/4 ×29, P 22k/16 ×5) | — |
| BADDOG3 | D,10 | X300 | E300 | 20 WAV 11s (P 11k/8 ×20) | 25 |
| BOGGLINS | D,10,32 | — | SND | 5 WAV 3s (P 22000/8 ×1, P 22k/8 ×4) | 81 |
| BORIS | D,10 | — | SND | 4 WAV 2s (P 11k/8 ×3, P 22k/8 ×1) | 20 |
| BUGS | D | X300 | E300 | 1 WAV 1s (P 11k/8 ×1) | 0 |
| CLOCKS3/CLOX3 | D,32 | X300 | E300 | 2 WAV 1s (P 11k/8 ×2) | 30 |
| CONFETTI | D,32 | — | SND | 3 WAV 1s (P 11k/8 ×3) | 1 |
| CRITIC | D | X510 | DS ACM | 19 WAV 42s (IMA 22k/4 ×19) | — |
| CYBER | D,10 | X510 | DS ACM | 21 WAV 12s (IMA 22k/4 ×14, P 11k/16 ×4, P 22k/16 ×2, P 22k/8 ×1) | — |
| CYCLE/DAREDEVI | D,10,32 | X300 | E300 | 14 WAV 8s (P 11k/8 ×13, P 5564/8 ×1) | 82 |
| DOMINOES | D | — | SND | 1 WAV 0s (P 22k/8 ×1) | 15 |
| DOSSHELL | D,32 | — | SND | 5 WAV 1s (P 11k/8 ×5) | 97 |
| EINSTEIN | D | — | SND | 8 WAV 3s (P 11000/8 ×3, P 11k/8 ×5) | 316 |
| FISH | D,10 | X510 | DS ACM | 11 WAV 60s (IMA 11k/4 ×11) | — |
| FISH3/FISHPRO | D,10,32 | X300 | E300 | 1 WAV 0s (P 5563/8 ×1) | 4 |
| FLOCKS | D | — | SND | 4 WAV 4s (P 11k/8 ×2, P 22k/8 ×2) | 8 |
| FRACTAL | D | — | SND | 8 WAV 2s (P 11k/8 ×8) | 8 |
| 3DBOUNCE/GEOBOUNC | D,32 | — | SND | 2 WAV 1s (P 22k/8 ×2) | 93 |
| GRAVITY | D | — | SND | 1 WAV 0s (P 22k/8 ×1) | 416 |
| GUERNSEY | D | X510 | DS ACM | 6 WAV 40s (IMA 22k/4 ×6) | — |
| HULA | D,10 | X510 | DS ACM | 22 WAV 24s (IMA 22k/4 ×2, P 11k/8 ×15, P 22k/16 ×1, P 22k/8 ×4) | — |
| LIFE | D | X510 | DS ACM SEQ32 | 40 WAV 54s (IMA 22k/4 ×39, P 11k/8 ×1); nutcrack.mid (absent) | — |
| LUNATIC | D,10 | — | SND | 21 WAV 6s (P 11k/8 ×13, P 22k/8 ×8) | 1 |
| MARBLES | D,10 | X510 | DS ACM | 32 WAV 13s (IMA 22k/4 ×29, P 22k/16 ×3) | — |
| MARBLES/MARBLES2 | D,32 | — | SND | 4 WAV 0s (P 11k/8 ×4) | 59 |
| MESSAGES | D,10 | X510 | DS ACM | 59 WAV 6s (IMA 22k/4 ×13, P 22k/16 ×46) | — |
| MOWIN | D,10,32 | — | SND | 1 WAV 0s (P 11k/8 ×1) | 26 |
| NOCTURNE | D,32 | — | SND | 2 WAV 3s (P 11k/8 ×1, P 22k/8 ×1) | 12 |
| MMAS/OM | D,32 | — | SND | 2 WAV 3s (P 11k/8 ×2) | 33 |
| OUT | D | X510 | DS ACM | 43 WAV 31s (IMA 22k/4 ×20, P 22k/16 ×23) | — |
| POINTS | D | X510 | SEQ32 wIn DSprobe CD | 3DMinor.MID→3DMINOR.MID | — |
| PUNCH | D,32 | — | SND | 1 WAV 0s (P 11k/8 ×1) | 1454 |
| PUZZLE | D,10,32 | — | SND | 1 WAV 0s (P 22k/8 ×1) | 79 |
| RAIN | D,10 | X510 | DS | 7 WAV 6s (P 11k/8 ×7) | — |
| RATRACE | D,10,32 | X300 | E300 SEQ16 | 2 WAV 1s (P 11k/8 ×2); tellend.mid; tellintr.mid; tellloop.mid | 2 |
| REBOUND | D,32 | X300 | E300 | 4 WAV 0s (P 11k/8 ×4) | 193 |
| RODGER | D,10 | X510 | DS ACM | 21 WAV 20s (IMA 22k/4 ×2, P 22k/16 ×19) | — |
| RPS | D | X510 | DS ACM | 37 WAV 19s (IMA 22k/4 ×36, IMA 44k/4 ×1) | — |
| SHADOW | D,10 | X510 | DS ACM | 6 WAV 34s (IMA 22k/4 ×6) | — |
| SLOWBURN | D | X510 | SEQ32 DSprobe CD | FIREBOMB.MID→FIREBOMB.MID | — |
| SUPERGUY | D,10 | X510 | DS ACM | 25 WAV 30s (IMA 22k/4 ×24, P 22k/8 ×1) | — |
| SWIRLING | D | X510 | SEQ32 wIn DSprobe CD | SEAPIXIE.mid→SEAPIXIE.MID | — |
| TIME | D,10 | X510 | DS ACM | 39 WAV 22s (IMA 22k/4 ×39) | — |
| TOAST3/TOASTER3 | D,10,32 | X300 | SEQ16 | omtw.mid; valkyrie.mid | 0 |
| TOASTERS | D,10 | X510 | DS ACM SEQ32 | 13 WAV 16s (IMA 22k/4 ×12, P 22k/8 ×1); Baby Toasters.mid→BABY.MID; Flying Toasters.mid→TOASTERS.MID | — |
| TOILET/TOILETS | D,32 | X300 | E300 | 6 WAV 6s (P 11127/8 ×6); TT_SND.DLL (absent) | 125 |
| WORMS | D | — | SND | 3 WAV 0s (P 22k/8 ×3) | 162 |
| YBYH | D,10,32 | X300 | E300 SEQ16 | 12 WAV 6s (P 11k/8 ×11, P 7418/8 ×1); ybyhclck.mid; ybyhthem.mid; ybyhwin.mid | 22 |
| AQUA | 10 | — | SND | 1 WAV 0s (P 22k/8 ×1) | 6 |
| BUNGEE | 10,TT | X40 | E40 | 8 WAV 6s (MS 11k/4 ×8) | 23 |
| CHAM | 10,TT | X40 | E40 | 8 WAV 4s (MS 11k/4 ×7, P 11127/8 ×1); TT_SND.DLL | 21 |
| FRANKEN | 10,TT | X40 | E40 SEQ16 | TT_SND.DLL; HORROR.MID | 5 |
| HALLOFFA | 10 | — | WO | hof.srf (sound chunks) | — |
| MBORIS | 10 | X40 | E40 SEQ16 | 5 WAV 2s (P 22k/8 ×5); Dawn.mid | 5 |
| MIKES | 10 | X40 | E40 | 16 WAV 11s (MS 11k/4 ×14, P 11k/8 ×2); TT_SND.DLL | 15 |
| MIMEHUNT | 10,TT | X40 | E40 SEQ16 | 3 WAV 3s (P 11k/8 ×1, P 11127/8 ×2); mimehunt.MID | 0 |
| PHLEGM_B | 10,TT | X40 | E40 | 7 WAV 3s (MS 11k/4 ×4, P 11k/8 ×3); TT_SND.DLL | 25 |
| TOAST2 | 10 | — | SND | 3 WAV 1s (P 22k/8 ×3) | 253 |
| TOAST2K | 10 | X510* | DS* ACM SEQ32 DSprobe | 13 WAV 16s (IMA 22k/4 ×12, P 22k/8 ×1); Baby Toasters.mid; Flying Toasters.mid | — |
| TOASTER2 | 10 | X510* | DS* ACM SEQ32 DSprobe | 13 WAV 16s (IMA 22k/4 ×12, P 22k/8 ×1); Baby Toasters.mid; Toasters2k.mid | — |
| TOILET | 10,TT | X40 | E40 | 4 WAV 2s (P 11127/8 ×4); TT_SND.DLL | 131 |
| TOXIC | 10 | X40 | E40 | 1 WAV 0s (P 5011/8 ×1) | 4 |
| VOYEUR | 10 | X40 | E40 | 7 WAV 7s (MS 11k/4 ×5, P 11127/8 ×2); TT_SND.DLL | 3 |
| BADDOG | 32 | X300 | E300 | 20 WAV 11s (P 11k/8 ×20) | 25 |
| BORIS | 32 | — | SND | 4 WAV 2s (P 11k/8 ×3, P 22k/8 ×1) | 13 |
| BUGS | 32 | X300 | E300 | 1 WAV 1s (P 11k/8 ×1) | 0 |
| RAINDROP | 32 | — | SND | 1 WAV 0s (P 11k/8 ×1) | 79 |
| CS | TT | X40 | E40 SEQ16 | 7 WAV 9s (MS 11k/4 ×7); TT_SND.DLL; FTTHEME.MID | 5 |
| MBORIS | TT | X40 | E40 SEQ16 | 5 WAV 2s (P 22k/8 ×5); Dawn.mid | 5 |
| MESSYGES | TT | X40 | E40 | 5 WAV 2s (MS 11k/4 ×5) | 11 |
| MIKES | TT | X40 | E40 | 16 WAV 11s (MS 11k/4 ×14, P 11k/8 ×2); TT_SND.DLL | 15 |
| TCLOCKS | TT | X40 | E40 | 12 WAV 6s (MS 11k/4 ×6, P 11k/8 ×6) | 89 |
| TOXIC | TT | X40 | E40 | 1 WAV 0s (P 5011/8 ×1) | 4 |
| VOYEUR | TT | X40 | E40 | 7 WAV 7s (MS 11k/4 ×5, P 11127/8 ×2); TT_SND.DLL | 3 |
| BURNS | S | X310 | E310 | SIMP_SND.DLL | 5 |
| CHALKBRD | S | X310 | E310 | SIMP_SND.DLL | 145 |
| GRAMPA | S | X310 | E310 SEQ16 | SIMP_SND.DLL; grandpa.mid (absent) | 8 |
| GRASSKRT | S | X310 | E310 | SIMP_SND.DLL | 42 |
| HOMEREAT | S | X310 | E310 | SIMP_SND.DLL | 16 |
| HOW2DRAW | S | X310 | E310 | SIMP_SND.DLL | 0 |
| INS | S | X310 | E310 SEQ16 | SIMP_SND.DLL; i&sshow.mid | 13 |
| KRUSTY | S | X310 | E310 | SIMP_SND.DLL | 18 |
| LISA | S | X310 | E310 SEQ16 | SIMP_SND.DLL; mlangry.mid; mlhappy.mid; mlsad.mid | 0 |
| OBJETS | S | X310 | E310 | SIMP_SND.DLL | 18 |
| PHYSICS | S | X310 | E310 | SIMP_SND.DLL | 3 |
| SIMPCLOK | S | X310 | E310 | SIMP_SND.DLL | 48 |
| SIMPFILE | S | X310 | E310 SEQ16 | SIMP_SND.DLL; simpsons.mid | 0 |
| SIMPTRIV | S | X310 | E310 | SIMP_SND.DLL | 103 |
| SNOWBALL | S | X310 | E310 SEQ16 | SIMP_SND.DLL; snowball.mid | 0 |

No sound (all 16-bit unless marked 32-bit):

* D, 10 and 32: ARTIST, NONSENSE, RAY, SLIDE/SLIDES3, WMORPH.
* D: DRAINO, MANDELBR, MEADOW, MODERN, PSYCHO (32-bit), RAIN (the Classic
  Hard Rain), SHAPES, SNAKE, SPIN, SPIRAL, STRANGE, STRING, SUNBURST,
  TUNNEL, VERTIGO, ZOT.
* D and 32: FROST, GLOBE, MESSAGE3/MESSAGES, MOUNTAIN, NIRVANA, PHOTON,
  ROSE, SATORI, SPHERES, SPLIGHT/SPOT, STAINED, WARP, ZOOM/ZOOOMMM.
* D and 10: STARRYNI (32-bit), TURTLE (32-bit).
* 10 and 32: SWANS.
* 32: GUTS, LOGO, TUNNEL.

Notes:

* A "0" in the last column does not mean silent. Those modules are
  music-only (TOAST3, LISA, SNOWBALL, SIMPFILE), or play nothing in their
  first 30 s (BUGS, HOW2DRAW, MIMEHUNT). PUNCH's 1454 are one short sound
  retriggered every frame.
* The last column counts every `sndPlaySound` call less one, and AD_SND stops
  the current sound with `sndPlaySound(NULL)` before each play, so the busy
  modules' counts are about twice their plays (TOAST2's 253 ≈ 126 plays), and
  the "1" of CONFETTI and LUNATIC is two stops and no play (L16; the
  integration census counts non-NULL calls, §10.5).
* Music controls: CS and MIMEHUNT default to "Never" (no song with default
  controls; `ADCVSET=3=20` plays CS's); RATRACE, TOAST3, INS, LISA, SIMPFILE
  and SNOWBALL to "Once"; FRANKEN to "Twice"; YBYH to "Always" (L16).
* `→` marks the **Deluxe music renames** (§7.7). The AD4 modules ask for
  `Music\<long name>.mid` (their string tables), but the Deluxe disc keeps the
  songs in `FILES\AD40` under 8.3 names. The 10th Anniversary's `SETUP.INF`
  (lines 292–293) copies `Toasters.mid` to `Music\Flying Toasters.mid` and
  `Baby.mid` to `Music\Baby Toasters.mid`. The Deluxe files have the same
  sizes (EMPIRICAL; the Deluxe `SETUP.INF` could not be re-read, since its
  image is not on this machine).
* `TT_SND.DLL (absent)`: Deluxe's `TOILETS.AD` and 3.2's `TOILET.AD` name the
  Totally Twisted bank, which their releases do not ship. The engine goes on
  without it (the runs play 125 sounds from the module's own resources).

### 1.4 Helper DLLs and sound banks

| Binary | Builds | Sound role |
|---|---|---|
| `AD_SND.DLL` | 3: Deluxe = 10th (`b3ab…`, "4.0.0 Sep 12 1996"), 3.2 = TT (`07a2…`), Simpsons (`08d3…`, "3.0.4 Jul 05 1994"); and since the seventh release Star Trek's 1.0 ("V1.0", §2.12); with the twelve releases, the Looney Tunes' and ScreamSavers' 3.1.4 (`b2d3…`), the Disney Collection's (3.2's, `07a2…`) and Marvel's (Star Trek's 1.0, byte for byte); Snoopy's Screen Savers ship none and get **the host's own** (§2.10) | the Classic sound library (§2.10); mandatory for OLDMOD16 (ABI.md §3.3) |
| `LT_SOUND.DLL` | the Looney Tunes | "Looney Tunes Shared Sounds" (NE module `LOON_SND`, KERNEL imports only): **103** type-3000 WAVE resources (ids 15000–16107, 1.25 MB); `ADXPL41` loads it by name (`FindResource(LOON_SND, id, 3000)`) and plays them through AD_SND |
| `DIS_SND.DLL` | the Disney Collection | 3 type-3000 WAVE resources (ids 1000–1002); `ADXPL100` loads it by name from `AD_PREFS.INI [After Dark] Path` (§2.9) |
| `SIMP_SND.DLL` | Simpsons | "Simpsons Shared Sounds": **143** type-3000 WAVE resources (all 8-bit 11025 Hz PCM mono, 109 s: the speech and effects), no code but a `WEP`; loaded by name by all 15 Simpsons modules and read through ADXPL310/AD_SND |
| `TT_SND.DLL` | TT and 10th (same file, also in the 10th's `MUSIC\`) | 12 type-3000 WAVE resources (8-bit PCM mono at 11025, 11127, 22254 and 22255 Hz; 8 s); loaded by name by 7 TT modules |
| `ST_SND.DLL` | Star Trek (since the seventh release) | 71 type-3000 WAVE resources (all 8-bit PCM mono at 11025 Hz, 94.8 s: the effects, Final Frontier's theme, McCoy's quotes); `AD_MOD.DLL` loads it by name from `<Path>ST_RES\` for 15 modules (§2.12) |
| `AD_MME.DRV`, `AD_MPT.DRV`, `AD_SB.DRV` | Star Trek | AD_SND 1.0's plug-in sound drivers (§2.12); only `AD_MME.DRV` is installed |
| `ADXPL300/310/40.DLL` | 16-bit engines | `XSoundDatabase` (→ AD_SND) and `XSoundMusicPlayer` (→ `mciSendString`); import MMSYSTEM `midiOutGetNumDevs`, `waveOutGetNumDevs/GetDevCaps`, `mciSendString` |
| `ADXPL41.DLL`, `ADXPL100.DLL` | the Looney Tunes' engine (a superset of ADXPL310's ordinals); the Disney Collection's library, of the After Dark 2.0 generation | the same two paths: effects through AD_SND, songs through the MCI sequencer strings (§2.9) |
| `ADXPL510.DLL` | Deluxe (`3fe1…`), 10th (`7976…`) | `XNoiseMaker`/`XNoise` (DirectSound, ACM), `WinMidiPlayer` (MCI), `XCdAudio`, `SoundHelp`; imports WINMM (aux, mixer, MCI) and MSACM32; `dsound.dll` dynamically |
| `ADTASK.DLL`, `ADW30.EXE` | 3.2, TT, Simpsons | the original AD 3 host. Not run: the native bridge replaces it (PACKAGES.md §7.4) |
| `AFTERDAR.SCR` | Deluxe, 10th | snapshots and restores every mixer control around a module (ABI.md §2.12). Not run: our host never touches the real mixer |

### 1.5 Music files

36 distinct `.mid` files. All are SMF format 0 (the five AD4 songs) or format
1 (up to 23 tracks), with PPQN division (192 or 480) and tempo meta events.
None has SMPTE timing or lyric events. The `TELL*.MID` files carry one SysEx
message each.

**EMPIRICAL: most 1994 songs are MPC dual-mode.** Their note counts on
channels 13–16 repeat those on 1–10: TELLLOOP 1:451 ↔ 15:451, 3:332 ↔ 13:332,
4:296 ↔ 14:296; HORROR 1:233 ↔ 13:233; SNOWBALL 1:503 ↔ 13:503. The 1995
editions of the same songs in 3.2 and TT drop channels 13–16 entirely. This
is the Multimedia PC authoring scheme, with an extended-level arrangement on
channels 1–10 and a base-level one on 13–16, of which the MIDI mapper played
one. Today's GS synth plays all 16, so the parts would sound twice. The
affected files are the Deluxe Classic, 10th and Simpsons songs; the AD4 songs
use 1–5 and 10 only (§6.4).

### 1.6 What the stub host saw

* **pe32**: every engine module calls `auxGetNumDevs` and
  `LoadLibraryExA("dsound.dll")`, which is refused. Those with IMA resources
  then call `acmMetrics` and `acmFormatEnumA` once per resource, which fail.
  No MCI call is ever made, because MIDI opens only after DirectSound does
  (§2.3). HALLOFFA calls `waveOutGetNumDevs`, gets 0 and stops.
* **ne16**: 162 modules init AD_SND. It probes `waveOutGetNumDevs/GetDevCaps`
  and two `waveOutOpen` format queries (11025 and 22050 Hz, 8-bit mono),
  saves the volumes, and at unload calls `sndPlaySound(NULL,0)`. The AD 3.2/TT
  build also calls `mixerGetNumDevs`. `sndPlaySound` flags seen: `0x0007`
  (`SND_ASYNC|SND_NODEFAULT|SND_MEMORY`, 439 samples), `0x0006`
  (**synchronous**, NOCTURNE only) and `0x0000` (stop). No `mciSendString`
  is ever reached, because the engines' `IsMusicAvail` needs a MIDI output
  device (§2.9).

---

## 2. How the originals make sound

### 2.1 AD4 effects: `XNoiseMaker` over DirectSound — VERIFIED

`XNoiseMaker::Open` (`0x42729c`):

1. Clamps its channel count (≤ 6). `SetNoiseVolume(block+0x3C)`.
2. `LoadLibraryExA("dsound.dll")`, `GetProcAddress("DirectSoundCreate")`,
   then `DirectSoundCreate(NULL, &ds /*maker+0x678*/, NULL)`.
3. `DSERR_ALLOCATED` (`0x8878000A`; earlier drafts called it
   `DSERR_NODRIVER`, which is `0x88780078`) sets a local flag (`0x42731d`).
4. On success, `ds->SetCooperativeLevel(block->hWnd, DSSCL_NORMAL)`, retried
   up the `GetParent` chain; if that fails too, `ds->Release()`.
5. **MIDI is opened only when the block's sound bit (`+0x04` bit 1) is set
   and DirectSound was created or answered `DSERR_ALLOCATED`** (`0x427425..
   0x427475`). With `dsound.dll` refused, as today, no AD4 module ever plays
   music.

Every virtual call the engine makes, from the listing
(`research/win/dis/ADXPL510.DLL.asm`), slot = offset in the COM vtable:

| Interface | Slot | Method | Callers |
|---|---|---|---|
| IDirectSound | `0x08` | Release | `Open` `0x42739f`, `Close` `0x4274d8` |
| | `0x0C` | CreateSoundBuffer | `XNoise::LoadByID` `0x428e52`, `LoadByName` (×3), `PlayNoiseList` `0x4280e1` (the streaming channel) |
| | `0x14` | DuplicateSoundBuffer | `PlayNoise` `0x427e95` (one duplicate per channel playing a noise) |
| | `0x18` | SetCooperativeLevel | `Open` `0x42734a`, `0x427374` |
| IDirectSoundBuffer | `0x08` | Release | `GiveTime`, `LoadByID` `0x428ee3` |
| | `0x10` | GetCurrentPosition | `GiveTime` `0x42762c` (streaming refill) |
| | `0x24` | GetStatus | `XNoiseMaker::GetStatus` `0x428c39` |
| | `0x2C` | Lock | `LoadByID` `0x428e81`; `GiveTime` ×5 (locks the **source** buffer to read from it, and the streaming buffer to write); `PlayNoiseList` ×4 |
| | `0x30` | Play | `DoPlaySound` `0x428bdd` (`DSBPLAY_LOOPING` when the repeat count is −1), `0x428bf0`; `PlayNoiseList` |
| | `0x3C` | SetVolume | `DoPlaySound` `0x428b9d`, `GiveTime` |
| | `0x40` | SetPan | `DoPlaySound` `0x428b86`, `PlayNoiseList` |
| | `0x48` | Stop | `FindFreeChannel`, `StopNoise`, `StopAllNoise`, `GiveTime` |
| | `0x4C` | Unlock | `LoadByID`, `GiveTime` ×5, `PlayNoiseList` ×4 |

No other slot is reached through the engine's objects. The calls through
`[eax+0x20]`/`[eax+0x24]` in `XNoise::CalcMemNeeded`/`LoadByID` are virtuals of
the C++ resource file (`theirResourceFile`), not of DirectSound.

* **Buffers**: `DSBUFFERDESC{dwSize 0x14, dwFlags 0xE8}` =
  `DSBCAPS_CTRLVOLUME|CTRLPAN|CTRLFREQUENCY|LOCSOFTWARE`, in both `LoadByID`
  (`0x428e29`) and `PlayNoiseList` (`0x42808d`). No primary buffer is
  created. A loaded noise is one buffer, filled once through Lock/memcpy/
  Unlock. Playing a noise duplicates it; the streaming channel is a separate
  buffer the engine refills from the source by play position in `GiveTime`.
* **Volume**: `SetNoiseVolume(v)` (`0x428adf`) stores (100 − v) × −20
  hundredths of a dB, so 50 gives −10 dB and 100 gives 0 dB. v = 0 or the
  sound bit clear gives −10000 plus a **timer-only mode** (`+0x680 = 1`) in
  which nothing is played. `GetStatus` (`0x428bfb`) then reports "playing"
  until the noise's duration has elapsed on `XTimer`. Pan: `DoCalcPosition`
  (`0x428b39`) maps 200 to centre and otherwise clamps ±100, ×100.
* **Durations** come from the decoded size (`LoadByID` `0x428de5`). While ACM
  fails, as today, every IMA noise has duration 0.

### 2.2 AD4 ADPCM: `XNoise::DecompressWave` through MSACM32 — VERIFIED

For tag `0x11` only (`LoadByID` `0x428daf`), at `0x429308`:

1. `acmMetrics(NULL, ACM_METRIC_MAX_SIZE_FORMAT)` sizes a format buffer,
   `wFormatTag = 0x11`.
2. `acmFormatEnumA(NULL, &afd, cb=0x429587, dwInstance=&srcwfx,
   ACM_FORMATENUMF_WFORMATTAG)` with `afd.cbStruct = 0x98`. **The callback is
   guest code**: it accepts the first format whose `dwFormatTag == 0x11` and
   `pwfx->nSamplesPerSec == src->nSamplesPerSec`, copies the source's
   `nChannels` into it, sets a flag and returns FALSE.
3. `acmFormatSuggest(NULL, found, &dst{wFormatTag=1}, 0x12,
   ACM_FORMATSUGGESTF_WFORMATTAG)`.
4. `acmStreamOpen(&has, NULL, found, &dst, NULL, 0, 0,
   ACM_STREAMOPENF_NONREALTIME)`, `acmStreamSize` (source), `HeapAlloc`,
   `acmStreamPrepareHeader`, `acmStreamConvert(…, ACM_STREAMCONVERTF_START)`,
   `acmStreamUnprepareHeader`, `acmStreamClose`.

The stream decodes with the **enumerated** format, so its block alignment is
the codec's standard one. The codec here (`imaadp32`, Windows 11) enumerates 8
formats in this order (EMPIRICAL, `acmFormatEnumA` on the host):

| Index | Rate | Ch | Block align | Samples per block |
|---:|---:|---:|---:|---:|
| 0 | 8000 | 1 | 256 | 505 |
| 1 | 8000 | 2 | 512 | 505 |
| 2 | 11025 | 1 | 256 | 505 |
| 3 | 11025 | 2 | 512 | 505 |
| 4 | 22050 | 1 | 512 | 1017 |
| 5 | 22050 | 2 | 1024 | 1017 |
| 6 | 44100 | 1 | 1024 | 2041 |
| 7 | 44100 | 2 | 2048 | 2041 |

These match every IMA resource in the corpus (22050 mono align 512, 11025 mono
256, RPS's one 44100 mono 1024). MS-ADPCM (`msadp32`) standard formats are the
same rates with samples per block 500/500/1012/2036 and a 32-byte `cbSize`
(7 coefficient pairs). `acmMetrics(ACM_METRIC_MAX_SIZE_FORMAT)` answers 50.

### 2.3 AD4 music: `WinMidiPlayer` over MCI — VERIFIED

`mciSendCommandA` only (`0x420220..0x42096e`), polled with no notify:

| Step | Command | Flags / fields |
|---|---|---|
| `Open` (test) | `MCI_OPEN` `0x803` | `MCI_OPEN_TYPE` (`0x2000`), type "sequencer"; then `auxGetVolume(1, &saved)`; `MCI_CLOSE` |
| `Load(path)` | `MCI_OPEN` | `MCI_OPEN_TYPE|MCI_OPEN_ELEMENT` (`0x2200`); path = `MakePathAbsoluteToAD` of a string-table name (`CreateFilePathFromID` `0x4208f9`, `LoadStringA`) |
| | `MCI_SET` `0x80D` | `MCI_SEQ_SET_PORT` (`0x20000`), port `0xFFFFFFFF` = `MIDI_MAPPER`; failure = "Midi: No MIDI Mapper." and close |
| | `SetVolumePercentage(block+0x3C)` | `auxSetVolume(1, v×655.35 in both words)` (`0x4208df`) |
| `Play` | `MCI_PLAY` `0x806` | flags 0 (no from/to, **no notify**) |
| `GetPlayState` | `MCI_STATUS` `0x814` | `MCI_STATUS_ITEM` (`0x100`), item 4 (`MCI_STATUS_MODE`): `0x20E` play / `0x20D` stop. When stopped, loops: `Stop` + `MCI_SEEK` `0x807` with `MCI_SEEK_TO_START` (`0x100`) + `Play` |
| `Stop` | `MCI_STOP` `0x808`, then `MCI_SEEK` to start | |
| `Close` | `auxSetVolume(1, saved)`, `Stop`, `MCI_CLOSE` `0x804` | |

Errors go through `mciGetErrorStringA`. The looping ("Looping not supported,
so I'll play it once…") is the engine's own poll above.

### 2.4 AD4 volume: aux and mixer — VERIFIED

`sub_426f0c` classifies each aux device (`i < auxGetNumDevs()`, at most 100)
with `mixerGetLineInfoA(i, &line, MIXER_OBJECTF_AUX|MIXER_GETLINEINFOF_…)`.
If that **fails**, the device is simply marked volume-controlled. If it
succeeds with component type 0x1008 (wave out), 0x1004 (synth) or 0x1005
(CD), the device is marked and a global bit set (`[0x451dd4]` |= 1/2/4).
While that global is non-zero, `DoPlaySound` and `GiveTime` **skip
`IDirectSoundBuffer::SetVolume`** and rely on the hardware line volume.
`sub_426fc1` then sets every marked aux device to the module volume each
`GiveTime`, and `0x427039`/`0x427060` save and restore them. Hence the
design: two aux devices and **no mixer**, so per-buffer volume stays in use
and aux 1 carries the MIDI volume (§7.6).

### 2.5 CD audio and Music Choice — VERIFIED

`XCdAudio` (`0x421aa0..`) registers the window class `blargCD` and drives
`mciSendStringA`: "open cdaudio alias qwanza wait", "status qwanza media
present wait", "set qwanza time format tmsf wait", "play qwanza from %d to
%d:%s notify"…. `SoundHelp` (`0x41b980`), used by POINTS, SLOWBURN and
SWIRLING for their Music Choice control, builds either an `XNoiseMaker` song
(MIDI) or an `XCdAudio` track player from the module's choice. Disc images
carry no CD-DA tracks, so CD audio stays refused ("no disc"). The MIDI choice
plays.

### 2.6 The static engine copies and the probes — VERIFIED (strings)

TOAST2K and TOASTER2 (10th Anniversary) are MSVC builds with the engine linked
in. They carry the same DirectSound and MIDI messages
(`TOAST2K.AD.asm` `0x10010025..0x100100d5`) and import WINMM aux/MCI and
MSACM32 themselves. The DirectSound emulation must therefore follow the COM
contract, not the ADXPL510 call list alone. POINTS (`0x4182be`), SLOWBURN and
SWIRLING also call `DirectSoundCreate`, `SetCooperativeLevel` and `Release`
themselves, as a probe.

### 2.7 Hall of Fame: `JSound`/`XMixer` over `waveOut` — VERIFIED

HALLOFFA (10th) mixes its own sound (from the `snd ` chunks of `HOF.SRF`) and
streams it:

* **Device choice** (`0x43520a`): tries `WAVE_MAPPER`, then 0…n−1. Each
  needs `waveOutGetDevCapsA` with the wanted `dwFormats` bit and no
  `WAVECAPS_SYNC`, a test `waveOutOpen` of 22050 Hz **16-bit mono** (or 8-bit
  stereo for the 2S08 bit) with `CALLBACK_NULL` (`0x4351f3`), a close, and a
  `waveOutGetVolume`/`SetVolume` round trip.
* **Streaming** (`0x4353ad..`): 64 `WAVEHDR`s of 1024 samples, all prepared
  up front, written as a ring (`0x434fd6`). Progress comes from
  `timeGetTime` and the headers; `waveOutPause`/`Restart`/`Reset` are used,
  plus a 16→8-bit stereo conversion for the 2S08 format.

### 2.8 AD 3.x effects: engines → AD_SND → `sndPlaySound` — VERIFIED / EMPIRICAL

The Classic modules either import AD_SND themselves (`adwOpenSound`,
`adwLoadSoundResource`, `adwPlaySound`, …) or go through their engine's
`XSoundDatabase`, which imports AD_SND by ordinal. The Simpsons and TT
engines also `LoadLibrary` the module's shared bank by name (`simp_snd.dll`,
`TT_SND.DLL`) and `FindResource(…, 3000)` in it. AD_SND then plays the
locked resource with `sndPlaySound` (below). Traced: `FindResource` of type
`0x0BB8` (3000) from AD_SND and ADXPL310, then `sndPlaySound` from AD_SND
(`dynamic.json`).

### 2.9 AD 3.x music: engines → `mciSendString` + `MM_MCINOTIFY` — VERIFIED (ADXPL310)

ADXPL310 (`research/win/audio/dis/ADXPL310.DLL.asm`); ADXPL300 and ADXPL40
carry the same strings and code shape (UNVERIFIED instruction by
instruction):

* `XSoundMusicPlayer::IsMusicAvail` (ord 406, `4:e7f4`) =
  `midiOutGetNumDevs() > 0`.
* Music init (`4:ea5b..4:ec98`):
  * Needs `GetVersion` ≥ 3.10 and `AD_MODULE+0x1E` (`bWantSnd`, via the
    module pointer the engine keeps) ≠ 0.
  * On Windows major version 3 (Win95 reports 3.95 to 16-bit code) it then
    needs `LoadLibrary("TOOLHELP.DLL")` ≥ 32 and `LoadLibrary("MCISEQ.DRV")`
    ≥ 32; otherwise it frees TOOLHELP and gives up.
  * It takes `GetProcAddress(TOOLHELP, "GLOBALFIRST"/"GLOBALNEXT")`. They
    are called only when Windows reports exactly 3.10 (`4:f46f`), to
    `GlobalPageLock` MCISEQ's data blocks; on the 3.95 we report they never
    are. The MCISEQ handle goes through `GetModuleHandle(MAKELONG(h, 0))`
    into player `+0x1C` for that lock, and the engine frees and reloads
    MCISEQ every 100 plays (player `+0x26`). (L16, from the listing.)
  * It registers the class `adwMidiCall` with `MIDIWNDPROC` (ord 428,
    `4:f2c8`) and creates a hidden window of it (`4:f32e`).
* Commands, `mciSendString` with the strings at DGROUP `0x449..0x4fd`:
  "open sequencer", "close sequencer", "close all wait", "open
  sequencer!%s alias fred wait", "play fred notify", "seek fred to start
  wait", "stop fred wait", "close fred". Around every play it sets `system.ini
  [mciseq.drv] disablewarning` to true and then writes the old value back
  (L16 seeds the key as true, so nothing is written).
* `MIDIWNDPROC` (`4:f377`): `WM_CREATE` keeps `lpCreateParams`, `WM_DESTROY`
  clears it, and **`MM_MCINOTIFY` (0x3B9)** calls the player's callback
  (`4:f173`) with `wParam`. Everything else goes to `DefWindowProc`. A song's
  end reaches the engine only as this message; that is how the engine loops
  or chains songs (RATRACE: intro, loop, end). The callback's jump table:
  `SUCCESSFUL` replays (`+0x20`) when the player loops (`+0x12`), else the
  song is done (`+0x1E`, "stop fred wait"); `SUPERSEDED` and `FAILURE` are
  done; `ABORTED` is ignored. ADXPL300 and ADXPL40 were observed doing the
  same in traces (RATRACE, TOAST3, YBYH): the same API sequence, and no
  `GlobalFirst`/`GlobalNext` calls.

**ADXPL41** (the Looney Tunes; EMPIRICAL, the survey's traces) plays the
same way: `open sequencer!C:\AFTERDRK\music\<song>.mid alias fred wait`,
then `play fred notify`, for all 13 of the release's General MIDI songs
(every open succeeds; Messages, Marvin and Taz open none), and its effects
come from `LT_SOUND.DLL` through AD_SND. **ADXPL100** (the Disney
Collection's library, of the After Dark 2.0 generation; VERIFIED in the
survey's listing, `research/win/pkg/disney/dis/`) reads `AD_PREFS.INI
[After Dark] Path` (the lane's seed of what `ADW30.EXE` wrote, PACKAGES.md
§7.3), loads `<Path>DIS_SND.DLL`, and opens its songs as
`sequencer!C:\AFTERDRK\music\<song>` with the alias `fred` (Beauty
`bandb`, Falling Flower `nutcrack`, Captain Hook `crocodil`, Little Mermaid
`undersea`, The Sorcerer `dukasexp` and `dukasint`), flipping `SYSTEM.INI
[mciseq.drv] disablewarning` around each play as ADXPL310 does. Its
`IsSoundLame()` takes the sound for "lame" only when `[Sound] SoundDriver`
is empty or `AD_MPT.DRV`, so the seed's `AD_MME.DRV`, like the default
"None", passes. Before a song its `lock_sequencer_down_hard_now` (7:09BF)
walks the global heap with TOOLHELP's `GlobalFirst`/`GlobalNext` (`GLOBAL_ALL`)
for MCISEQ.DRV's blocks to `GlobalPageLock`, on the 3.95 the runtime
reports too (ADXPL310 walks only on 3.10). The runtime answers an empty walk
(`GlobalFirst` FALSE at once; `host/win16/README.md`): the library notes
"Error walking global list", returns 1, and the song plays. A real walk
would lock nothing either (MCISEQ is the host's stub, with no blocks), and
only add its calls; before, the two were unimplemented and counted in the
census.

### 2.10 AD_SND — VERIFIED (`research/win/dis/AD_SND.DLL.asm`, Deluxe build)

* **Playback**: `sndPlaySound(lpRiff, flags)` from a `GlobalLock`ed
  (`1:234d`) or `LockResource`d (`1:2431`) image. `flags` =
  `SND_MEMORY|SND_NODEFAULT`, plus `SND_ASYNC` unless the sound mode is sync
  (NOCTURNE), plus `SND_LOOP` in loop mode (`1:2344`, `1:2428`). The memory is
  unlocked **right after** the call (`1:2392`, `1:2478`), so the host copies
  the image at the call. Stop = `sndPlaySound(NULL, 0)` (`1:24aa`). With no
  wave device (`[0x422]` = 0) AD_SND skips the call and reports success
  (`1:2368`).
* **Capability probes**: `waveOutGetNumDevs`, `waveOutGetDevCaps` and
  `waveOutOpen(…, WAVE_FORMAT_QUERY)` for 11025 and 22050 Hz 8-bit mono PCM
  (`1:26c2..1:285f`); `midiOutGetNumDevs/GetDevCaps`; `auxGetNumDevs/
  GetDevCaps`.
* **Volumes**: `adwGetSystemVolumes`/`adwSetSystemVolumes` save and restore
  the wave/MIDI/aux device volumes. `adwSetVolume(v)` sets
  `waveOutSetVolume(0, …)` (traced: 50 gives `0x7FFF7FFF`), and
  `adwSetSoundMute`.
* **INI**: `AD_PREFS.INI [Sound] Mute` (first letter `Y` mutes) and
  `system.ini [speaker.drv] Volume`.
* **Other builds**: the 3.2/TT build (`research/win/audio/dis/
  AD_SND_ad32.DLL.asm`) sets volumes through the mixer whenever
  `GetProcAddress(MMSYSTEM, …)` finds `mixerGetNumDevs`, `mixerGetLineInfo`,
  `mixerGetLineControls` and `mixerSetControlDetails` (`1:25d2`), whatever
  `mixerGetNumDevs` then answers: the synth (`0x1004`) and wave-out
  (`0x1008`) lines' volume (`1:2858`, `1:269e`), which with no mixer device
  sets nothing. Only without those exports does it take its Windows 3.1
  path (`1:2560`): `midiOutSetVolume` on each MIDI device with
  `MIDICAPS_VOLUME`, then `waveOutSetVolume` on the devices its format
  probes chose. (VERIFIED at integration; an earlier reading had 0 devices
  falling back to the wave volume, and the 3.2/TT modules then played at
  full volume whatever `ADVOLUME` said, §8.3.) The Simpsons build (3.0.4)
  looks for `MMMIXER` and, without it, uses the wave and aux volumes.

**The host's own AD_SND** (with the twelve releases; `host/win16/adsnd16.cc`;
PACKAGES.md §7.4). Snoopy's Screen Savers are eight modules made for an
After Dark already installed: they ship no AD_SND, six of them import it by
name, and the native bridge needs one before it loads any module. For a
package whose engine dir holds no `AD_SND.DLL` (a rule by file; Snoopy's is
the only one) the ne16 lane's AD3 protocol registers a Win16 system module
named `AD_SND` before the bridge opens, and the bridge's
`LoadLibrary(C:\WINDOWS\SYSTEM\AD_SND.DLL)` and the modules' imports reach
it. It is our own code, written from the Snoopy survey's measurements of
AD_SND 3.0.3 and 3.2 under the lane: no byte of Berkeley's. Everything it
does goes through the Win16 thunks (KERNEL, MMSYSTEM), as a real library's
imports would, so the census, the `api16` trace, virtual time and the audio
path see its calls alike; its own work costs no instructions.

* **Entries**: all 36 of AD_SND 3.0.3 (the Simpsons package's, version
  resource 3.0.3; 3.2 exports the same set), with their ordinals, names and
  argument sizes (ABI.md §3.13): the bridge's seven, AD_SND 1.0's volume
  pair, and every entry the Snoopy modules import among them.
* **Init.** `adwSoundInit(w, err)` asks `mmsystemGetVersion` and
  `waveOutGetNumDevs`, then, for 8-bit mono PCM at 11025 and at 22050 Hz,
  the first device whose `waveOutOpen(WAVE_FORMAT_QUERY)` takes it (a second
  pass adds `WAVE_ALLOWSYNC`). Found, it answers 0, leaves the level at 25
  until `adwSetVolume` sets one, and takes its capabilities from the
  devices' caps: volume (1) when both have `WAVECAPS_VOLUME`, async (2) and
  loop (4) unless the 11 kHz device has `WAVECAPS_SYNC`; the lane's device
  gives 7 (`adwSoundAsyncCap` answers 2). With no device (`ADSOUNDDEV=0`),
  or none that plays both rates, it answers 1 with the reason in `err` ("No
  wave output device is installed."); every entry that needs the device then
  answers 0 (`adwOpenSound`, `adwLoadSoundResource`, `adwSoundAsyncCap` and
  the other capabilities, `adwStopSound`), but `adwPlaySound` answers 1, so
  the modules run silent. Init and cleanup clear the mute and forget the
  last volume.
* **Sounds** are records the module keeps (`GlobalAlloc(GMEM_MOVEABLE |
  GMEM_DDESHARE)`, 0x42 bytes): the mode, a file name, the image's handle.
  `adwLoadSoundResource(hInstance, name)` is `FindResource(hInstance, name,
  3000)`, `LoadResource` and `LockResource`, the image staying locked;
  `adwLoadSoundFile` reads a file whole into a global block. The mode:
  0x200 loops, 0x100 does not (the default), 0x20 plays synchronously, 0x10
  asynchronously (the default); `adwSetSoundMode` refuses both of a pair and
  a synchronous loop, and changes only the pairs it names.
* **Playback.** `adwPlaySound(h)`: muted, or at level 0, nothing plays and
  it answers 1; else `sndPlaySound(image, SND_MEMORY | SND_NODEFAULT`, plus
  `SND_ASYNC` unless synchronous, plus `SND_LOOP` when looping`)`, flags
  0x07, 0x06 or 0x0F, the image locked for the call and unlocked right after
  it, and the sound becomes the current one. A synchronous loop plays
  nothing (0). `adwStopSound` is `sndPlaySound(NULL, 0)`; `adwCloseSound(2)`
  stops too; `adwFreeSound` stops the sound when it is the current one, then
  frees the image (a resource: `GlobalUnlock` and `FreeResource`) and the
  record.
* **Mute and volume.** `adwSetSoundMute` stores the value as it came
  (`adwPlaySound` tests it) and stops nothing. `adwSetVolume(v)`: muted, v
  counts as 0; the value it last took answers 1 at once; 0..100 sets the
  level and v × 0xFFFF / 100 on both channels, `midiOutSetVolume` on every
  MIDI device with `MIDICAPS_VOLUME`, then `waveOutSetVolume` on the wave
  devices: AD_SND 3.2's levels and order (50 gives `0x7FFF7FFF` on both
  buses, §6.7; 3.0.3's MIDI level followed a curve of its own, which no
  module of such a package hears, none playing MIDI); anything else answers
  0, and is remembered all the same.
* **System volumes.** `adwGetSystemVolumes(&h)` saves in a new global block
  (`GHND`, 0xB4 bytes, signature 0x6969) the volume of every wave, MIDI and
  aux device that has one; `adwSetSystemVolumes(h)` writes them back and
  frees the block (0; 1 for no block, 2 for one that does not lock, 3 for
  one that is not such a block). AD_SND 1.0's `adwSavePreviousVolume`/
  `adwRestorePreviousVolume` do the same for the wave devices alone. So a
  run sets `0x7FFF7FFF` at load and restores `0xFFFFFFFF` at unload, as the
  real library does.
* **The rest**: `adwOpenSound`, `adwPauseSound` and `adwResumeSound` answer
  1 with the device; `adwIsSoundDone` 1 for the current sound (or none) and
  0 for any other (the library cannot ask `sndPlaySound`); `adwGetSoundInfo`
  reads a PCM image's `fmt ` and `data` chunks; `adwCreateSound` makes a
  record, but none for a named file, so `adwPlaySoundFile` fails, as it does
  in 3.0.3 and 3.2; `adwPlaySoundResource` loads, sets the mode and plays
  (never freeing the sound: the library's own leak, kept); no sound effects
  (`adwQuerySfx` and `adwDoEffect` answer 1 for effect 0 alone) and no setup
  dialog; `adwSoundDllVer` "3.0.3", `VerStr` 303.
* **Not there**: a mixer path (`MMMIXER`, a Windows 3.1 sound card's DLL:
  the host has none), `AD_PREFS.INI` (3.2 reads `[Sound] Mute` there, which
  nothing in such a package writes; the bridge sets the mute anyway), and a
  `VerStr` gate (the native bridge has none; OLDMOD16, which wants 400, never
  gets this module).
* **Checked** (the Snoopy work; `research/win/pkg/more/sn/`, gitignored):
  with instruction timing off (`ADMIPS=0`) the eight modules' frames and
  captures are byte-identical to After Dark 3.2's real AD_SND and ADTASK
  borrowed into the package (a research stage only), and the MMSYSTEM trace
  equals the real 3.2's call for call (but one trace line of its own and the
  image's selector number). At the default timing the host's AD_SND makes
  other calls than a real one and costs no instructions of its own, so runs
  differ from a borrowed library's (PACKAGES.md §12). `ADTRACE=sound` says
  "AD_SND (the host's): wave devices 0 (11 kHz) and 0 (22 kHz), capabilities
  7", or "… no sound: No wave output device is installed.".
* **Chosen where the real ones differ**, never reached by a module of the
  corpus: `VerStr` 303 and "3.0.3" (the entry set's version resource; the
  3.0.3 file's internals say 304 and "3.0.4"); the MIDI level of 3.2; one
  `FreeResource` where the real library, when `LockResource` fails in
  `adwLoadSoundResource`, frees the resource twice.

### 2.11 Formats in the corpus

| Where | Formats |
|---|---|
| AD4 WAV resources (29 modules) | IMA-ADPCM 22050/11025/44100 mono; PCM 8- and 16-bit mono at 11025/22050 |
| AD 3.x type-3000 resources, SIMP_SND, TT_SND; since the seventh release ST_SND (all 11025 Hz); with the twelve releases LT_SOUND, DIS_SND, and the ScreamSavers and Snoopy modules' own | 8-bit PCM mono at 11025, 22050, 22000, 11000, 11127, 22254, 7418, 5564, 5563, 5011 Hz (ScreamSavers' Mac-derived 5563, 7418, 11127 and 22254 among them); **MS-ADPCM 11025 mono** in 8 ADXPL40 modules (BUNGEE, CHAM, CS, MESSYGES, MIKES, PHLEGM_B, TCLOCKS, VOYEUR), which on Win95 played through `sndPlaySound` via the wave mapper's ACM |
| HOF.SRF | its own chunks, mixed by HALLOFFA to 22050 16-bit mono PCM |
| Music | SMF 0/1, PPQN (§1.5) |

No stereo sound effect, no 44.1 kHz PCM, no MP3, no DirectSound 3D.

### 2.12 After Dark 2.0: AD_SND 1.0 and its sound drivers — VERIFIED / EMPIRICAL

Star Trek: The Screen Saver's sound library (since the seventh release;
`research/win/nedis.py` listings in `research/win/pkg/startrek/content/dis/`
and `…/lane/dis/`, and the content and lane surveys' runs in
`research/win/pkg/startrek/survey/`, all gitignored) is AD_SND 1.0
("AfterDark Sound DLL Module"; `adwSoundDllVer` answers "V1.0"). It plays
nothing itself: it hands every sound to a **plug-in sound driver**.

* **Init.** `adwSoundInit(WORD, LPSTR err)` returns 0, 1 (a device error,
  its text in `err`) or 2 ("Error reading the Sound preferences from
  AD_PREFS.INI"). It reads `AD_PREFS.INI` `[After Dark] Path` (adding a
  backslash) and `[Sound] Mute` (default "NO"; a first letter `Y` mutes),
  loads every `<Path>*.DRV` (up to 8) to list the devices by their
  `adwsdGetDeviceName`, then loads `<Path>` plus `[Sound] SoundDriver` and
  resolves its 18 `adwsd*` entries. An empty or unknown driver gives 1:
  "The Current Sound Device can not be found/loaded. Select another device
  in the Sound Setup Dialog."
* **The modules.** 15 go through `AD_MOD.DLL`'s sound classes, which import
  11 AD_SND entries by name (`adwOpenSound`, `adwCloseSound`,
  `adwCreateSound`, `adwFreeSound`, `adwGetSoundInfo`, `adwLoadSoundFile`,
  `adwLoadSoundResource`, `adwPlaySound`, `adwSetSoundMode`,
  `adwSoundAsyncCap`, `adwStopSound`): the bank is
  `LoadLibrary(<Path>ST_RES\ST_SND.DLL)`, and a sound is
  `adwLoadSoundResource(hInst, id)`, a type-3000 resource of it (71 of
  them, ids 1000–1913). Sounder imports 9 entries itself and plays `*.WAV`
  files with `adwLoadSoundFile`. `WantSound` sets `AD_MODULE+0x1E`
  (`bWantSnd`) in 15 modules, all but Ion Storm, so the bridge calls
  `adwSetSoundMute` and `adwSetVolume` for them (PACKAGES.md §7.4). Every
  play in every run had the flags `0x0007`
  (`SND_ASYNC|SND_NODEFAULT|SND_MEMORY`): no loops, no synchronous plays,
  and no MIDI or MCI (no module or engine DLL imports MMSYSTEM).
* **Mute.** `adwSetSoundMute` writes `AD_PREFS.INI [Sound] Mute=YES|NO`
  every time, so the value lands in the state overlay (INTERACTION.md
  §7.1), and `adwPlaySound` returns 1 without playing while muted or at
  volume 0. The bridge sets the mute at every load of a module that wants
  sound, so a value an earlier run left there never decides.
* **The drivers.**

  | Driver | Device | In the host |
  |---|---|---|
  | `AD_MME.DRV` | "Multimedia Windows Sound (Windows 3.1)" | plays: the driver the lane's profile seed names (PACKAGES.md §7.3) |
  | `AD_MPT.DRV` | "PC Internal Speaker", through M.P. Technologies' `SPALETTE.DLL`, which programs the timer chip (ports 0x43, 0x40, 0x42 and 0x61) and calls `AD_LIB.DLL`'s `GET_VXD_ENTRYPOINT` | hangs: the first sound waits on the timer's counter (`SPALETTE 3:090c`, `in al,0x40` in a loop), which the runtime does not provide, until the call's instruction budget fails the run ("did not return within 1000000000 instructions"); the disks' `AD_PREFS.INI` named it, and neither is installed |
  | `AD_SB.DRV` | "Sound Blaster Card (Windows 3.0)", through `SNDBLST.DLL` | refuses: init returns 1, "you are running Windows 3.0 Multimedia or Windows 3.1. Select the Multimedia Windows Sound Device …"; not installed |

  So the lane seeds `AD_PREFS.INI [Sound] SoundDriver=AD_MME.DRV`
  (PACKAGES.md §7.3), the one driver that plays in the host, where the
  disks' `AD_PREFS.INI` named `AD_MPT.DRV`; the recipe installs neither that
  file nor the PC-speaker path (PACKAGES.md §4.3). It is the choice the
  original's Sound Setup offered as "Multimedia Windows Sound (Windows
  3.1)", and its captures match the modules' own resources (§10.7).

* **`AD_MME.DRV`** imports only WIN87EM, KERNEL and USER, and reaches
  MMSYSTEM with `GetModuleHandle("MMSystem")` and `GetProcAddress` by
  ordinal: `sndPlaySound` (2), `mmsystemGetVersion` (5), `waveOutGetNumDevs`
  (401), `waveOutGetDevCaps` (402), `waveOutOpen` (404, `WAVE_FORMAT_QUERY`
  for 11025 and 22050 Hz 8-bit mono), `waveOutGetVolume` (415) and
  `waveOutSetVolume` (416), all of which the Win16 runtime has (§8.2,
  §8.3). A sound is `sndPlaySound(SND_MEMORY|SND_NODEFAULT|SND_ASYNC)`
  (with `SND_LOOP` on request) of the locked resource, unlocked right after
  the call. The volume v (0–100) is `waveOutSetVolume(v × 655.35)` on both
  channels (50 gives `0x7FFF7FFF`); at 0 nothing plays. With no wave device
  (`ADSOUNDDEV=0`) init returns 1, "there are no Multimedia Sound Devices
  installed", and every module runs silent.
* **The volume pair.** AD_SND 1.0 saves and restores the device volume with
  `adwSavePreviousVolume()` and `adwRestorePreviousVolume()` (ABI.md §3.9),
  which the bridge calls where it calls AD_SND 3.x's
  `adwGetSystemVolumes`/`adwSetSystemVolumes`. Its Sound Setup dialog
  (`adwSoundSetup`, where After Dark 2.0's user picked the driver) was
  reached only from `AD.EXE`'s "&Sound.." button, never by a module.

---

## 3. Architecture

```
adhostwin.exe
  run_host ─ creates audio::Engine from Config::from_env(env) ─► LaneContext::audio
             after every step: engine.advance(clock.now_us()), unless the lane
                               advanced it during the step (ne16: §8.6)
             at the end:       engine.shutdown(t), then lane.shutdown()

  pe32 lane (win32 shims)                 ne16 lane (win16 shims)
   DSOUND  IDirectSound/Buffer ─┐          MMSYSTEM sndPlaySound ────┐
   MSACM32 acm* (IMA decode) ───┤          waveOut* (+MM_WOM_*) ─────┤
   WINMM   waveOut*, mci*, aux* ┤          midiOut*/aux* volumes ────┤
                                │          mciSendString (+MM_MCINOTIFY)
                                ▼                                    ▼
                  adw::audio::Engine  (emulation thread; never blocks)
       buffers ─ voices ─┐                                 songs (SMF player,
       streams ─────────┼─► PCM mixer (int, Q15 gains) ──► MPC rule, CC7 scaling)
       bus gains ───────┘        │                              │
                                 ▼                              ▼
                  ┌─ capture: <file>.wav (44.1k s16 stereo)   <file>.mid (event log)
       sinks ─────┼─ live:    WASAPI shared (ring, own thread) midiOut (MIDI mapper, own thread)
                  └─ none
```

**Invariants.**

1. **Determinism.** Everything the guest can observe (return codes,
   cursors, positions, playing/stopped, `WHDR_DONE`, event times, the order of
   callbacks) is a function of the guest's calls and the virtual times passed
   with them. The mixer is integer-only, so a capture is bit-identical run to
   run and machine to machine.
2. **Sinks never influence the guest.** Live, capture and none give the same
   answers. For a headless run, `ADSOUND=1` and `ADAUDIOOUT=x` produce the
   same `FBHASH` stream.
3. **Nothing blocks.** The engine does not wait on the device, read its
   clock, or sleep. The live sinks run on their own threads behind lock-free
   queues; they underrun (silence) or drop (oldest data) instead.
4. **Sound off is unchanged.** With `enabled()` false each lane keeps its
   current sound-off behaviour byte for byte. Sound on legitimately changes
   what the guest does (noise durations, songs, the Classic engines' music
   windows), so its `FBHASH` streams differ from the sound-off ones and are
   deterministic among themselves.
5. **Never touch the real system volume or mixer.** Guest `*SetVolume`
   calls set emulated device gains only.

---

## 4. Policy and environment

| Variable | Default | Meaning |
|---|---|---|
| `ADSOUND=1` | off | **Guest sound on**: DirectSound/MCI/MIDI devices exist, the AD4 block's sound bit is set, and the Classic bridge unmutes. In a streamed run (`ADSTREAM=1`) it also plays on the real device. A headless run never opens the device. |
| `ADAUDIOOUT=<file.wav>` | — | **Capture**: guest sound on (as `ADSOUND=1`), PCM written to `<file.wav>` (the `ADAUDIORATE`, 16-bit stereo), MIDI events to `<file>.mid`. With `ADSOUND=1 ADSTREAM=1` too, both live and capture. |
| `ADVOLUME=0..100` | 50 | After Dark's volume slider, handed to the modules: AD4 block `+0x3C`, the Classic bridge's `adwSetVolume`. 50 is the registry default of the original. The host adds no gain of its own. |
| `ADAUDIORATE=<hz>` | 44100 | mixer and capture rate (8000..96000). Guest-visible state does not depend on it (§6.1). |
| `ADAUDIOLATENCYMS=<ms>` | 80 | live PCM ring target and MIDI delay (20..500) |
| `ADAUDIOLIVE=0` | on | with `ADSOUND=1 ADSTREAM=1`: do not open the device (tests of streamed runs) |
| `ADMIDI=0` | on | no live MIDI (still captured) |
| `ADMIDIDEV=<n>` | −1 | live `midiOut` device id; −1 = the MIDI mapper |
| `ADMIDIBASE=1` | off | keep channels 13–16 of MPC dual-mode songs (§6.4) |
| `ADSOUNDDEV=0` | (ne16) | unchanged: the Classic lane reports no wave device at all |
| `ADTRACE=audio` | | engine calls, song loads, sink state, underrun/drop counts |

The engine also answers `adhostwin --capabilities` with `audio=1`, and ends
a run with sound on with one stderr line: `[audio] voices=… chunks=…
songs=… midi_events=… underruns=… drops=… frames=…` (`frames` = PCM frames
rendered).

---

## 5. The frozen API: `host/core/include/adw/core/audio.h`

CORE implements this header exactly. L32 and L16 code against it in
parallel. A change needs all three to agree, and this section is updated.
`lane.h` gains one member, `audio::Engine* audio = nullptr;` at the end of
`LaneContext`. Existing `LaneContext{env, screen, clock, input}` aggregates
stay valid, and `run_host` sets it.

**Amendment (2026-09-28, Star Wars Screen Entertainment).** SWSE's music
is sequenced by the guest itself: MEMMIDI plays the song through
`midiOutShortMsg` from a multimedia timer (§8.3, §8.6). The engine gains a
raw MIDI port, `midi_short`/`midi_long`/`midi_reset` (below, after the
songs; semantics §6.4). They are virtual with bodies that do nothing, a
disabled engine's answer, so an `Engine` written before them (a lane's test
double) still builds; `make_engine`'s engines implement them. Nothing else
in the header changes but three comments: the one on `Time`, which names
the dated time of a call a Win16 callback or timer procedure makes (§8.6);
the one on `advance`, which says when `run_host` leaves a step's end to the
lane; and the note beside these three methods, which says a method added
this way must be forwarded by `LaneEngine`. `run_host`
hands a lane its engine wrapped in `LaneEngine` (`host/core/src/host.cc`),
which forwards every method and notes whether the lane called `advance`
during a step: if it did, `run_host` leaves that step's end to the lane
(§3). A method added with a body, as these three were, compiles without
the wrapper forwarding it, and the lanes' calls would reach the disabled
body: it must be added to `LaneEngine` too.

```cpp
// adw/core/audio.h — the host audio engine (docs/AUDIO.md). FROZEN:
// the lanes code against this header; a change goes through AUDIO.md §5.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace adw {
struct Env;
}

namespace adw::audio {

// Guest time: virtual microseconds on VirtualClock's scale, as a guest clock
// read would see it at the moment of the call (pe32: rt.clock().now_us();
// ne16: Runtime16::peek_us(), or for a call a callback or timer procedure
// makes, the procedure's due time plus what it has run, win16/sound16.hh).
// Never read_us() or any read that nudges time: audio calls must not move the
// clock. Times passed to one Engine should not go backwards: a value earlier
// than the latest seen is taken as the latest seen.
using Time = uint64_t;

// ---- configuration ----------------------------------------------------------
struct Config {
  bool guest_sound = false;    // ADSOUND=1 or ADAUDIOOUT set (AUDIO.md §4)
  bool live = false;           // guest_sound && ADSOUND=1 && ADSTREAM=1 && ADAUDIOLIVE!=0
  int volume = 50;             // ADVOLUME, clamped 0..100
  std::string capture_wav;     // ADAUDIOOUT (UTF-8); "" = no capture
  std::string capture_mid;     // capture_wav with its extension replaced by ".mid"
  uint32_t rate = 44100;       // ADAUDIORATE, 8000..96000
  uint32_t latency_ms = 80;    // ADAUDIOLATENCYMS, 20..500
  bool live_midi = true;       // ADMIDI (default on)
  int midi_device = -1;        // ADMIDIDEV; -1 = MIDI mapper
  bool mpc_base_channels = false;  // ADMIDIBASE=1

  // Malformed values fall back to the defaults, with a line in *warnings.
  static Config from_env(const Env& env, std::vector<std::string>* warnings = nullptr);
};

// ---- formats ------------------------------------------------------------------
constexpr uint16_t kTagPcm = 0x0001, kTagMsAdpcm = 0x0002, kTagImaAdpcm = 0x0011;

struct WaveFormat {
  uint16_t tag = kTagPcm;
  uint16_t channels = 1;            // 1 or 2
  uint32_t rate = 22050;            // frames per second
  uint32_t avg_bytes = 22050;       // nAvgBytesPerSec
  uint16_t block_align = 1;         // nBlockAlign
  uint16_t bits = 8;                // PCM: 8 or 16; ADPCM: 4
  uint16_t samples_per_block = 0;   // ADPCM only
  std::vector<std::array<int16_t, 2>> coefs;  // MS-ADPCM only: (iCoef1, iCoef2) pairs
  bool operator==(const WaveFormat&) const = default;
};

WaveFormat pcm_format(uint32_t rate, uint16_t channels, uint16_t bits);
// The standard formats of Windows' imaadp32/msadp32 codecs (AUDIO.md §2.2):
// block_align 256*ch at <= 11025 Hz, 512*ch at 22050, 1024*ch at 44100.
WaveFormat ima_adpcm_format(uint32_t rate, uint16_t channels);
WaveFormat ms_adpcm_format(uint32_t rate, uint16_t channels);
// PCMWAVEFORMAT (16 bytes) or WAVEFORMATEX (18 + cbSize) as held in guest
// memory; `bytes` is what is readable there. False = malformed or truncated.
bool parse_waveformat(std::span<const uint8_t> bytes, WaveFormat& out);
// WAVEFORMATEX bytes (18 + cbSize, ADPCM extras included) for the guest.
std::vector<uint8_t> waveformat_bytes(const WaveFormat& f);
// PCM, 8|16 bits, 1|2 channels, 1000..192000 Hz, block_align = channels*bits/8.
bool playable(const WaveFormat& f);
// playable(), or IMA-/MS-ADPCM, 4 bits, 1|2 channels, a block that holds its header.
bool decodable(const WaveFormat& f);

struct Wave {
  WaveFormat format;
  std::span<const uint8_t> data;   // the data chunk, clipped to the image
  uint32_t fact_samples = 0;       // the fact chunk, 0 when absent
};
// A RIFF WAVE image: fmt, optional fact, data; other chunks skipped; odd
// chunk sizes padded. `out.data` points into `riff`.
bool parse_wave(std::span<const uint8_t> riff, Wave& out);
// From the first 12 bytes: 8 + the RIFF size when they read "RIFF….WAVE", else 0
// (how much guest memory sndPlaySound(SND_MEMORY) must read).
size_t riff_extent(std::span<const uint8_t> first12);

// Decoding to PCM16 at the source's rate and channels (PCM input is returned
// unchanged). Bit-exact with Windows' imaadp32.acm / msadp32.acm, partial
// blocks included: imaadp32 drops a trailing partial block; msadp32 decodes
// one that holds its 7*channels-byte header.
WaveFormat decoded_format(const WaveFormat& src);
// acmStreamSize(ACM_STREAMSIZEF_SOURCE): whole blocks, rounded up; 0 below one block.
size_t decoded_size(const WaveFormat& src, size_t src_bytes);
size_t decode(const WaveFormat& src, std::span<const uint8_t> in, std::span<uint8_t> out);  // bytes written
std::vector<uint8_t> decode(const WaveFormat& src, std::span<const uint8_t> in);
// acmStreamSize(ACM_STREAMSIZEF_DESTINATION): the source bytes of the whole
// blocks whose output fits in pcm_bytes (rounded down; 0 when none does).
size_t encoded_size_for(const WaveFormat& src, size_t pcm_bytes);

// ---- gains --------------------------------------------------------------------
// Linear gain per channel, Q15: 0x8000 = unity (0 dB), 0 = silent.
struct Gain {
  uint16_t left = 0x8000, right = 0x8000;
  bool operator==(const Gain&) const = default;
};
// DirectSound: volume in hundredths of a dB (-10000..0; -10000 = silent),
// pan -10000 (left only) .. 10000 (right only); the other side is attenuated
// by |pan| hundredths of a dB. From a fixed table: identical everywhere.
Gain gain_from_ds(int32_t volume, int32_t pan);
// A WINMM/MMSYSTEM device volume DWORD: low word left, high word right,
// 0..0xFFFF linear (a mono device repeats the low word).
Gain gain_from_mm(uint32_t volume);
uint32_t mm_from_gain(Gain g);

// ---- the engine ----------------------------------------------------------------
enum class Bus : uint8_t { wave, midi };

using BufferId = uint32_t;   // 0 = none
using VoiceId = uint32_t;
using StreamId = uint32_t;
using SongId = uint32_t;

struct Event {
  enum class Kind : uint8_t {
    voice_end,    // a playing, non-looping voice reached the end of its buffer (id = voice)
    chunk_done,   // a stream finished a chunk, or reset/close released it (id = stream, cookie)
    song_end,     // a playing song reached its end (id = song)
  };
  Kind kind = Kind::voice_end;
  uint32_t id = 0;
  uint64_t cookie = 0;
  Time at = 0;
};

struct Stats {
  uint64_t voices_started = 0, chunks = 0, songs_started = 0, midi_events = 0;
  uint64_t rendered_frames = 0, underruns = 0, dropped_frames = 0;
};

// Called from the emulation thread only. No method blocks on a device. With
// enabled() false the lanes keep their sound-off behaviour and call nothing
// but config()/enabled(); a disabled engine answers 0 ids and neutral values.
class Engine {
 public:
  virtual ~Engine() = default;
  virtual const Config& config() const = 0;
  bool enabled() const { return config().guest_sound; }

  // Buffers: PCM sample memory (a DirectSound buffer's contents, a decoded sound).
  // Zero-filled. 0 when !playable(pcm), bytes == 0 or bytes > 256 MiB.
  virtual BufferId create_buffer(const WaveFormat& pcm, uint32_t bytes) = 0;
  // Copies into the buffer (clipped to it); audible from t on.
  virtual void write_buffer(BufferId b, uint32_t offset, std::span<const uint8_t> bytes, Time t) = 0;
  // The buffer lives on until its last voice is destroyed.
  virtual void release_buffer(BufferId b) = 0;
  virtual const WaveFormat* buffer_format(BufferId b) const = 0;
  virtual uint32_t buffer_size(BufferId b) const = 0;

  // Voices: one play cursor over one buffer; several may share a buffer
  // (DuplicateSoundBuffer). New: stopped, cursor 0, unity gain, the buffer's rate.
  virtual VoiceId create_voice(BufferId b, Bus bus) = 0;
  virtual void destroy_voice(VoiceId v, Time t) = 0;
  virtual void play(VoiceId v, bool loop, Time t) = 0;   // from the cursor; if already playing only `loop` changes
  virtual void stop(VoiceId v, Time t) = 0;              // the cursor stays where playback got to
  virtual void set_cursor(VoiceId v, uint32_t byte_offset, Time t) = 0;  // rounded down to a block, clamped
  virtual uint32_t cursor(VoiceId v, Time t) = 0;        // play cursor, bytes
  virtual bool playing(VoiceId v, Time t) = 0;
  virtual bool looping(VoiceId v, Time t) = 0;
  virtual Time end_time(VoiceId v, Time t) = 0;          // end of a playing non-looping voice, else 0
  virtual void set_gain(VoiceId v, Gain g, Time t) = 0;
  virtual void set_rate(VoiceId v, uint32_t hz, Time t) = 0;  // 0 = the buffer's own; clamped 100..200000
  virtual uint32_t rate(VoiceId v) const = 0;

  // Streams: PCM chunks played back to back (waveOut).
  virtual StreamId open_stream(const WaveFormat& pcm, Bus bus, Time t) = 0;  // 0 when !playable(pcm)
  // Copies the chunk; an empty chunk completes when playback reaches it.
  virtual void stream_write(StreamId s, std::span<const uint8_t> bytes, uint64_t cookie, Time t) = 0;
  virtual void stream_pause(StreamId s, Time t) = 0;
  virtual void stream_restart(StreamId s, Time t) = 0;
  // Every queued chunk is done at t (events in queue order), position back to 0, not paused.
  virtual void stream_reset(StreamId s, Time t) = 0;
  virtual uint64_t stream_position(StreamId s, Time t) = 0;  // bytes played since open or the last reset
  virtual bool stream_paused(StreamId s) const = 0;
  virtual void set_stream_gain(StreamId s, Gain g, Time t) = 0;
  virtual void close_stream(StreamId s, Time t) = 0;         // reset, then gone; its events are still polled

  // Songs: Standard MIDI Files (format 0/1, PPQN or SMPTE division) on the MIDI bus.
  virtual SongId load_song(std::span<const uint8_t> smf, std::string* error = nullptr) = 0;  // 0 = unplayable
  virtual void song_play(SongId s, Time t) = 0;          // from the position; at the end already: song_end at t
  virtual void song_stop(SongId s, Time t) = 0;          // sounding notes off; the position stays
  virtual void song_seek(SongId s, uint64_t position_us, Time t) = 0;  // stops first; clamped to the length
  virtual uint64_t song_position(SongId s, Time t) = 0;  // µs from the start
  virtual uint64_t song_length(SongId s) const = 0;      // µs
  virtual bool song_playing(SongId s, Time t) = 0;
  virtual void close_song(SongId s, Time t) = 0;         // stops; its pending events are dropped

  // Raw MIDI on the MIDI bus (amendment above): the guest's own messages,
  // stamped with t, into the .mid log and the live synth as song events are.
  virtual void midi_short(uint32_t msg, Time t) {}       // midiOutShortMsg's DWORD; a data byte first = running status
  virtual void midi_long(std::span<const uint8_t> bytes, Time t) {}  // midiOutLongMsg: SysEx, or any message stream
  virtual void midi_reset(Time t) {}                     // midiOutReset: note-offs, sustain off, all notes off

  // The guest's device volumes: wave = waveOutSetVolume; midi = midiOutSetVolume, auxSetVolume(1).
  virtual void set_bus_gain(Bus b, Gain g, Time t) = 0;
  virtual Gain bus_gain(Bus b) const = 0;

  // Events at or before t, in time order (equal times: in the order their
  // causes were issued), each returned once; appended to `out`.
  virtual void poll(Time t, std::vector<Event>& out) = 0;
  // The time of the earliest event not yet polled; 0 = none.
  virtual Time next_event_time() = 0;

  // Render up to t. After each step run_host calls it with the step's time,
  // unless the lane called it during that step (host.cc LaneEngine): a lane
  // that calls it owns its steps' ends.
  virtual void advance(Time t) = 0;
  // Stop audible output now (QUIT, stdin EOF, ADFRAMES, lane end): live PCM
  // stops, MIDI all-notes-off, captures are finalized. Later calls are accepted
  // and change nothing audible.
  virtual void shutdown(Time t) = 0;
  virtual Stats stats() const = 0;
};

std::unique_ptr<Engine> make_engine(const Config& config);
// A disabled engine for lanes run without one (LaneContext::audio == nullptr, unit tests).
Engine& null_engine();

}  // namespace adw::audio
```

---

## 6. Engine semantics (CORE)

### 6.1 Time

* Each voice, stream and song keeps its state in **continuous virtual time**.
  A voice started at `t0` at rate `r` from frame `f0` is at frame `f0 +
  floor((t − t0) · r / 10⁶)` at time `t`, wrapping when looping. Its end
  time is the first µs at which that reaches the buffer's frame count. The
  answers never depend on `ADAUDIORATE` or on how often `advance` is called.
* The renderer samples the same functions. Output frame `k` is at time `k ·
  10⁶ / R` (exact rational), and the source position is computed from it
  with 64-bit integer arithmetic plus the fractional part for interpolation.
  What is heard matches what the guest was told.
* A state-changing call at `t` first renders up to `t`, so the change starts
  at output frame `floor(t · R / 10⁶)`. `advance(t)` renders up to `t`, and
  capture sample `k` is guest time `k/R`: the file starts at virtual time 0.

### 6.2 Mixer

* Stereo `int16` output at `R`. Sources: 8-bit unsigned → `(x − 128) << 8`,
  16-bit signed as is, mono to both sides. Linear interpolation between
  source frames. Voice gain × bus gain (Q15 × Q15 → Q15) per side;
  accumulation in `int32`, saturated to `int16` once per output frame.
* No floating point anywhere in the render path. `gain_from_ds` uses a fixed
  table of `round(32768 · 10^(mB/2000))`, generated at build time or
  committed as source.
* Voices: at most 256 live at once (the corpus needs ≤ 8); past that,
  `create_voice` returns 0.

### 6.3 Voices and streams

* **Voices** follow DirectSound's buffer model: `play(loop)` from the
  cursor. A non-looping voice that reaches the end stops with its cursor
  back at 0, so a later `play` starts over, as DirectSound does with a
  secondary buffer at its end (UNVERIFIED; L32 confirms). ADXPL510 does not
  depend on it: it duplicates a fresh buffer for every play (§2.1).
  `write_buffer` while playing is audible from `t`, which is how the AD4
  streaming channel works.
* **Streams**: chunks play back to back with no gap. `stream_position` counts
  bytes consumed. `pause` holds the position, `restart` resumes, and `reset`
  completes every chunk at `t` in queue order. A `chunk_done` event carries
  the lane's cookie (the guest `WAVEHDR` address).

### 6.4 Songs: the SMF player

* **Parsing**: header and tracks (format 0, 1; format 2 is refused), running
  status, SysEx `F0`/`F7`, meta events (tempo `0x51` applied across tracks;
  end of track; others ignored), PPQN and SMPTE divisions. The tempo map
  turns every event into an absolute µs time; the length is the last
  end-of-track.
* **MPC dual-mode rule (EMPIRICAL §1.5; the mapper's exact Win95 behaviour is
  UNVERIFIED).** A song is dual-mode when it has note-ons on **channel 13**
  and on at least one of channels **1–10**. Every event on channels 13–16
  is then dropped. The rule catches every 1994 song listed in §1.5 and none
  of the AD4 or 1995 ones. `TOASTER1.MID`, which uses 15–16 without 13, is
  left alone. `ADMIDIBASE=1` disables the rule.
* **Volume**: the MIDI bus gain scales channel volume. The player tracks each
  channel's CC7 (default 100). At `song_play` it sends `CC7 = round(value ×
  gain)` for every channel the song uses, and it re-sends them when the bus
  gain changes. Gain here is the mean of the two sides in Q15.
* **Stop, seek and close**: note-off for every sounding note, then CC123
  (all notes off) on the channels used. `song_play` after a seek re-sends the
  program, controller and pitch-bend state in force at the new position
  (a chase), so a song resumed mid-way sounds right.
* **Delivery**: events whose time falls in the rendered span are sent to the
  MIDI sinks, together with the scaling and note-off messages above, each
  stamped with its guest time.
* **Raw MIDI** (`midi_short`/`midi_long`/`midi_reset`, the amendment of §5):
  messages a guest sequences itself (Win16 `midiOut*`, §8.3) share the bus
  with the songs. Each call first renders up to its time, so song events due
  by then reach the sinks first and the log stays in time order; the message
  then goes to the sinks stamped with that time. The engine has one raw port,
  as the guest has one MIDI device.
  * `midi_short`: the DWORD's low byte is the status, then the data bytes
    its type implies (only those are read: MEMMIDI leaves stack bytes in the
    rest). A data byte first is running status, as `midiOutShortMsg` allows;
    with no status to run on it is dropped, as a synth would. System common
    messages end running status; real-time bytes do not. `F0`/`F7` are
    long-message bytes and are dropped here.
  * `midi_long`: SysEx `F0 … F7` as given, or any stream of messages with
    running status inside it (a status byte other than real time where data
    belongs drops that message and starts the next; a message cut short at
    the end is dropped). Real-time bytes (`F8`–`FF`) may come anywhere, as
    MIDI 1.0 lets them: between a message's data bytes, or inside a SysEx,
    each goes out on its own as it comes (before the message it interrupted
    is complete), and the message goes on, running status and all — as a
    synth on the cable takes them.
  * System common and real-time messages have no SMF event of their own:
    they go to the sinks as an `F7` escape holding the raw bytes.
  * The MPC rule is the song player's (it chooses between an SMF's two
    arrangements); raw messages pass as the guest sends them.
  * **Bus gain, the songs' CC7 rule**: the guest's CC7 is sent as `round(value
    × gain)` and remembered per channel (default 100); a gain change re-sends
    the scaled CC7 of every channel the port has used; a channel's first
    message is preceded by its scaled default CC7 when the gain makes it
    differ from 100 (as `song_play` gives a song's channels theirs). At unity
    gain, the default, the port passes exactly what the guest sent.
  * `midi_reset` (what `midiOutReset` does): note-off for every note the port
    left sounding, then sustain off (CC64 0) and all notes off (CC123) on
    every channel it has used; running status ends. The channels keep their
    CC7 and stay used: the synth keeps them too.
  * `shutdown` silences the port like a playing song: note-offs for what it
    left sounding and CC123 on those channels.
  * A disabled engine does nothing with them.

### 6.5 Events

`voice_end`, `chunk_done` and `song_end` are queued with their exact times,
computed analytically as soon as they are known. `poll(t)` returns those
with `at ≤ t`, sorted by `(at, issue order)`. A cancelled cause (stopping a
voice before its end, closing a song) removes its pending event. A reset or
close stream turns its pending chunks into `chunk_done` at the reset time.

### 6.6 Sinks

* **Capture PCM** (`capture_wav`): a RIFF WAVE, PCM 16-bit stereo at `R`.
  The header is patched at `shutdown` and every 5 s of guest time, so a
  killed host leaves a readable file. It is created at engine start. When the
  file cannot be created, a warning is logged and the capture is off; the
  guest still hears nothing different.
* **Capture MIDI** (`capture_mid`): an SMF **event log**. Format 0, division
  500 PPQN at the default tempo, so 1 tick = 1 ms of guest time. It holds
  exactly the messages sent to the synth: song events after the MPC rule,
  the guest's raw messages (system ones as `F7` escapes), scaled CC7,
  note-offs, SysEx. The track length is patched at `shutdown`
  and every 5 s. **There is no deterministic General MIDI synthesizer in the
  host, so captures never render MIDI to PCM.** Tests assert on this log.
* **Live PCM** (`live`):
  * A dedicated thread runs WASAPI shared mode on the default render
    endpoint (`eConsole`), event-driven, `AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
    | SRC_DEFAULT_QUALITY`. Format 16-bit stereo at `R`; Windows converts to
    the mix format.
  * The session display name is "Long After Dark", so it appears in the
    Volume Mixer. MMCSS "Audio" is optional.
  * The feed is a single-producer/single-consumer ring. Playback starts once
    the ring holds `latency_ms`. Underrun → silence (counted). Fill above
    `latency_ms + 100 ms` → the oldest frames are dropped back to the target
    (counted); this absorbs clock drift and bursts.
  * Device loss (`AUDCLNT_E_DEVICE_INVALIDATED`) → reopen the new default at
    most once per wall-clock second, dropping meanwhile.
  * If WASAPI fails, `waveOut` on `WAVE_MAPPER` with 4 buffers of
    `latency/4`. If that fails as well, silence and one log line.
* **Live MIDI** (`live && live_midi`):
  * `midiOutOpen(midi_device)` on its own thread, which sends each event at
    `anchor_wall + (at − anchor_guest) + latency_ms`. The anchor is re-taken
    at every `advance`, so music and effects share the same constant delay.
  * SysEx goes through `midiOutLongMsg` with a prepared header.
  * On `shutdown`: note-offs, CC123 on all 16 channels, `midiOutReset`,
    `midiOutClose`.
  * When nothing is queued and no `advance` has come for `latency_ms` +
    500 ms (the saver sends no GO while the display is off; a stalled step),
    the notes still sounding get note-offs and CC123, so none drones on
    while the emulation waits. The song goes on with its next events once
    advances resume. Live only: the guest and the `.mid` never see it.
  * If the device cannot be opened, the music is silent live (logged once)
    and still captured.
* Headless runs never create a live sink, whatever `ADSOUND` says: virtual
  time outruns the wall clock there.

### 6.7 Volume mapping (who sets what)

| Guest action | Engine |
|---|---|
| AD4 `IDirectSoundBuffer::SetVolume/SetPan` (from block `+0x3C`, §2.1) | `set_gain(voice, gain_from_ds(vol, pan))` |
| AD4 `auxSetVolume(1, v)` (MIDI, §2.3/§2.4) | `set_bus_gain(midi, gain_from_mm(v))` |
| AD4 `auxSetVolume(0, v)` | stored (CD; no audio) |
| Win32 `waveOutSetVolume` (HALLOFFA) | `set_bus_gain(wave, …)` |
| Win16 `waveOutSetVolume` (AD_SND `adwSetVolume`) | `set_bus_gain(wave, …)` |
| Win16 `midiOutSetVolume`, `auxSetVolume(1)` | `set_bus_gain(midi, …)` |
| (host) an Intermission module's load, engine on: the saver's volume as the mixer's synth line (§10.6) | `set_bus_gain(midi, gain_from_mm(v))`, v = volume × 0xFFFF / 100 per channel |

`ADVOLUME` reaches the guest (AD4 block, Classic bridge) and comes back
through these calls, as it did on the original machine. At 50, the AD4
effects play at −10 dB and the MIDI at half amplitude; the Classic effects
play at half amplitude.

### 6.8 CORE also owns

* `Config::from_env`, the `README.md` table, `--capabilities audio=1`.
* `run_host` wiring (§3), the `[audio]` summary line, and `ADTRACE=audio`.
  The trace has one line per voice start/stop, chunk, song start/stop/end
  and bus-gain change, each with its virtual time in µs; tests key on these
  lines.
* `--test-pattern` with `ADTESTAUDIO=1`: a 100 ms 440 Hz PCM16 blip at every
  whole second of virtual time, and a MIDI note (C4, 200 ms) every second
  second. This drives the whole path without a module (§10.1).
* Linking: `ole32`, `winmm`, `uuid` (and `avrt` if MMCSS is used).

---

## 7. pe32 mapping (L32: `host/win32`, `host/pe32`)

### 7.1 Presence and the module block

* The lane stores `ctx.audio` (or `null_engine()`) where the shims reach it.
  Guest time for every audio call = `rt.clock().now_us()` (a peek).
* `enabled()` false → **everything exactly as today**. `LoadLibraryExA
  ("dsound.dll")` fails, MCI refused, `auxGetNumDevs` 0, ACM "no driver",
  `waveOutGetNumDevs` 0. `--configure`'s RealUi keeps its own
  `DirectSoundCreate` = `0x8878000A` (`DSERR_ALLOCATED`); configure runs never
  enable audio.
* `enabled()` true: the AD_MODULE32 block gets `+0x04 |= kSoundOn` and
  `+0x3C = config().volume`; today it is 50 hard-coded and the flag comes from
  `ADSOUND`. `DSOUND.DLL` is registered as a shim DLL, so
  `LoadLibraryExA("dsound.dll")` returns its handle and `GetProcAddress
  ("DirectSoundCreate")` its thunk.

### 7.2 DirectSound

* `DirectSoundCreate(lpGuid, LPDIRECTSOUND*, pUnkOuter)`: `lpGuid` NULL or
  `DSDEVID_DefaultPlayback`, `pUnkOuter` NULL → `DS_OK` and an object; else
  `DSERR_INVALIDPARAM` / `DSERR_NOAGGREGATION`.
* **Objects in guest memory**: `{vtbl, refcount, host id}` on the guest heap.
  Two static vtables of thunks: 11 slots for `IDirectSound`, 21 for
  `IDirectSoundBuffer`, stdcall, `this` first. Every slot is implemented. The
  slots in the §2.1 table are the verified contract. Any other slot logs once
  (`ADTRACE=sound`, "unverified DirectSound method") and follows the
  DirectSound 3 documentation.
* **IDirectSound**:
  * `QueryInterface` (`IID_IDirectSound` or `IID_IUnknown` → AddRef; else
    `E_NOINTERFACE`), `AddRef`, `Release` (freed at 0 with its buffers
    released).
  * `CreateSoundBuffer`: `dwSize` ≥ `0x14`, flags as given; `DSBCAPS_
    PRIMARYBUFFER` → a primary object with no engine buffer
    (`GetCurrentPosition` 0, `Play`/`SetVolume` accepted). Otherwise
    `lpwfxFormat` via `parse_waveformat` must be `playable()`, else
    `DSERR_BADFORMAT`, and `dwBufferBytes` must be 4..0x0FFFFFFF.
    Allocate the guest backing store and `engine.create_buffer` +
    `create_voice(wave)`.
  * `GetCaps` (`DSCAPS`, `dwSize` 96, `DSCAPS_EMULDRIVER|PRIMARY*|
    SECONDARY*|CONTINUOUSRATE`, rates 100..100000).
  * `DuplicateSoundBuffer`: a new object and voice over the same engine
    buffer and guest backing store; volume, pan and frequency copied; stopped
    at cursor 0.
  * `SetCooperativeLevel` → `DS_OK`; `Compact` → `DS_OK`;
    `GetSpeakerConfig` → `DSSPEAKER_STEREO`; `SetSpeakerConfig` → `DS_OK`;
    `Initialize` → `DSERR_ALREADYINITIALIZED`.
* **IDirectSoundBuffer**:
  * `GetCaps` (`dwSize` 20: flags, bytes, 0, 0).
  * `GetCurrentPosition(play, write)`: `play = cursor(v, now)`. While
    playing, `write = play` + 10 ms of bytes, block-aligned, modulo size;
    otherwise `write = play`. Either pointer may be NULL.
  * `GetFormat`, `GetVolume`, `GetPan`, `GetFrequency`: stored values.
  * `GetStatus`: `DSBSTATUS_PLAYING` 1 | `DSBSTATUS_LOOPING` 4.
  * `Initialize` → `DSERR_ALREADYINITIALIZED`.
  * `Lock(offset, bytes, &p1, &n1, &p2, &n2, flags)`: pointers into the
    guest backing store, split at the end of the buffer. Handles
    `DSBLOCK_FROMWRITECURSOR` and `DSBLOCK_ENTIREBUFFER`. The backing store
    is always current, so a read-only Lock (GiveTime reads its source this
    way) sees what was written.
  * `Play(0, 0, DSBPLAY_LOOPING?)`.
  * `SetCurrentPosition`.
  * `SetFormat` → `DSERR_INVALIDCALL` (secondary buffers).
  * `SetVolume` (−10000..0) / `SetPan` (±10000) → `set_gain(gain_from_ds)`.
  * `SetFrequency` (0 = original, 100..100000) → `set_rate`.
  * `Stop`.
  * `Unlock(p1, n1, p2, n2)`: copy exactly those guest ranges into the
    engine buffer with `write_buffer(…, now)`.
  * `Restore` → `DS_OK`.
* The engine's hardware-volume global `[0x451dd4]` (§2.4) is never set,
  because `mixerGetLineInfoA` fails. Its timer-only path (§2.1) is taken only
  at `ADVOLUME=0`, as it was on the original.

### 7.3 MSACM32 (emulated; no host codec)

* `acmMetrics(NULL, ACM_METRIC_MAX_SIZE_FORMAT)` → 50; `ACM_METRIC_COUNT_
  CODECS`/`DRIVERS` → 2. Others → `MMSYSERR_NOTSUPPORTED`.
* `acmFormatEnumA(had, pafd, fnCallback, dwInstance, fdwEnum)`:
  * With `ACM_FORMATENUMF_WFORMATTAG` and tag `0x11` or `2`, it walks the
    8 standard formats of §2.2 in order.
  * For each it fills `pafd` (`dwFormatIndex`, `dwFormatTag`,
    `fdwSupport = ACMDRIVERDETAILS_SUPPORTF_CODEC`, `*pwfx` up to `cbwfx`,
    and `szFormat` like "22.050 kHz, 4 Bit, Mono").
  * It then **calls the guest callback** `(hadid, pafd, dwInstance,
    fdwSupport)` through `rt.call_guest`, and stops when it returns FALSE.
  * Tag 1 (PCM) enumerates 8/16-bit mono/stereo at the four rates; other
    tags enumerate nothing. Returns 0.
* `acmFormatSuggest`: to PCM16 at the source rate and channels (for
  `ACM_FORMATSUGGESTF_WFORMATTAG` with tag 1, or none).
* `acmStreamOpen`: a `decodable()` ADPCM source to PCM16 with the same rate
  and channels. Anything else → `ACMERR_NOTPOSSIBLE`. `ACM_STREAMOPENF_QUERY`
  is honoured. The handle is a small guest-visible id.
* `acmStreamSize`: `SOURCE` → `decoded_size`, `DESTINATION` →
  `encoded_size_for`.
* `acmStreamPrepareHeader`/`Unprepare` set/clear
  `ACMSTREAMHEADER_STATUSF_PREPARED`.
* `acmStreamConvert` decodes whole blocks, and hands a trailing partial
  block to the decoder as the codecs do: imaadp32 yields nothing for it (so
  `cbSrcLengthUsed` counts whole blocks), msadp32 decodes it and it is
  consumed. It sets `cbSrcLengthUsed`, `cbDstLengthUsed` and
  `STATUSF_DONE`.
* `acmStreamClose`.

### 7.4 WINMM wave out (HALLOFFA)

* `waveOutGetNumDevs` → 1.
* `waveOutGetDevCapsA` (`WAVEOUTCAPSA`, 52 bytes): "Long After Dark",
  `dwFormats 0xFFF`, 2 channels, `dwSupport = VOLUME|LRVOLUME|SAMPLEACCURATE`,
  without `WAVECAPS_SYNC`. For `WAVE_MAPPER` and 0.
* `waveOutOpen`:
  * `WAVE_FORMAT_QUERY` answers from `playable()` (PCM only; ADPCM →
    `WAVERR_BADFORMAT`).
  * `CALLBACK_NULL` opens an engine stream.
  * `CALLBACK_WINDOW`/`CALLBACK_FUNCTION`/`CALLBACK_EVENT` work per §8.6's
    model on the Win32 side: messages posted to a guest window, functions
    called at the next safe point, and `WOM_OPEN`/`DONE`/`CLOSE` in order.
    The corpus uses none of them.
* `waveOutPrepareHeader`/`Unprepare` (`WHDR_PREPARED`). `waveOutWrite` sets
  `WHDR_INQUEUE`, clears `WHDR_DONE`, and calls `stream_write(bytes, cookie =
  header address)`. `WHDR_BEGINLOOP`/`ENDLOOP` are played once and logged.
* `waveOutPause`/`Restart`/`Reset`/`Close` (`WAVERR_STILLPLAYING` while
  chunks are queued, as Windows).
* `waveOutGetPosition` (`TIME_BYTES`, `TIME_SAMPLES`, `TIME_MS`; others →
  bytes).
* `waveOutGetVolume`/`SetVolume` (device id or handle) → wave bus gain.
* **`WAVEHDR` refresh**: `chunk_done` events are applied to the guest's
  headers (clear `INQUEUE`, set `DONE`) at every WINMM call, `timeGetTime`
  included, and before every `Module()` call. A poller therefore sees `DONE`
  exactly when virtual time passes the chunk's end.

### 7.5 MCI (`mciSendCommandA`, the sequencer)

* Device ids from 1.
* `MCI_OPEN`:
  * Supports `MCI_OPEN_TYPE` (type string "sequencer", case-insensitive, or
    `MCI_OPEN_TYPE_ID` with `MCI_DEVTYPE_SEQUENCER` 0x20B), `MCI_OPEN_ELEMENT`
    (a path resolved through the VFS, §7.7) and `MCI_OPEN_ALIAS`.
  * Without an element it opens the device alone (the engine's test open).
  * A missing file → `MCIERR_FILE_NOT_FOUND`; not an SMF → `MCIERR_INVALID_
    FILE`; any other type → `MCIERR_DEVICE_NOT_INSTALLED` (which keeps
    `cdaudio` refused).
* `MCI_SET`: `MCI_SEQ_SET_PORT` with `MIDI_MAPPER` or 0 → ok;
  `MCI_SET_TIME_FORMAT` milliseconds → ok.
* `MCI_PLAY` (`MCI_FROM`/`MCI_TO` in ms, optional `MCI_NOTIFY` → `MM_MCINOTIFY`
  posted to `dwCallback` on `song_end`). `MCI_STOP`. `MCI_SEEK`
  (`TO_START`/`TO_END`/`MCI_TO`).
  * As built: an `MCI_TO` end is noticed at the first pump point at or
    after it (at most one `Module()` call late, since the engine takes no
    future times), and the song is then left at exactly that position, as
    the Classic lane's `play … to n` is (§8.4).
  * A pending play notify ends `ABORTED` when a new `MCI_PLAY` or a
    stop, pause, seek or close interrupts it, and `SUPERSEDED` when another
    command asks for notification. The Classic lane answers `SUPERSEDED`
    to a play over a pending play (§8.4). The engines never issue one
    (§12), so the difference is not reachable.
* `MCI_STATUS` with `MCI_STATUS_ITEM`: MODE → `MCI_MODE_PLAY`/`STOP`;
  LENGTH/POSITION in ms; READY → TRUE; NUMBER_OF_TRACKS → 1.
* `MCI_CLOSE` → `close_song`.
* `MCI_WAIT` on non-play commands is harmless. `MCI_PLAY|MCI_WAIT` is not
  used by the corpus → `MCIERR_UNSUPPORTED_FUNCTION`, logged.
* `mciGetErrorStringA`: real texts.
* **`mciSendStringA`** stays as today (refused, `MCIERR_DEVICE_NOT_INSTALLED`).
  Only `XCdAudio` uses it.

### 7.6 aux, mixer, waveIn, CD

* `auxGetNumDevs` → **2** when enabled (0 CD audio, 1 MIDI synth).
* `auxGetVolume`/`SetVolume`: device 1 ↔ MIDI bus gain; device 0 stored
  (initially `0xFFFFFFFF`).
* `mixerGetLineInfoA` → `MMSYSERR_NODRIVER` (keeps per-buffer volume, §2.4).
* `waveIn*` stays at 0 devices: no microphone access, ever.
* CD audio stays refused.

### 7.7 Files

* MIDI paths come from the engine's `MakePathAbsoluteToAD` (`C:\AFTERDRK` =
  the module folder) and resolve through the existing VFS.
* **Deluxe renames**: in the `deluxe` package, a missing
  `C:\AFTERDRK\Music\<name>` falls back through this table (case-insensitive;
  EMPIRICAL, §1.3):

| Asked for | Deluxe file |
|---|---|
| `Music\Flying Toasters.mid` | `FILES\AD40\TOASTERS.MID` |
| `Music\Baby Toasters.mid` | `FILES\AD40\BABY.MID` |
| `Music\3DMinor.MID` | `FILES\AD40\3DMINOR.MID` |
| `Music\FIREBOMB.MID` | `FILES\AD40\FIREBOMB.MID` |
| `Music\SEAPIXIE.mid` | `FILES\AD40\SEAPIXIE.MID` |

  The 10th Anniversary package already holds `MUSIC\Flying Toasters.mid`
  and the rest (its importer applied `SETUP.INF`'s renames). LIFE's
  `music\nutcrack.mid` exists in no release (PREVIOUS.INF lists it for an
  older product), so LIFE plays no music, as on the Deluxe install.

---

## 8. ne16 mapping (L16: `host/win16`, `host/ne16`)

### 8.1 Presence and the bridge

* Guest time = `Runtime16::peek_us()`.
* `enabled()` false → **today's MMSYSTEM exactly**: the silent wave device
  (name included), `sndPlaySound` TRUE, no MIDI or aux devices, MCI refused.
* Enabled:
  * Mute = 0 and volume = `config().volume` for both OLDMOD16
    (`LoadADModule3216`'s arguments) and the native bridge (`adwSetSoundMute`,
    `adwSetVolume`). Today they come from `ADSOUND` and `ADVOLUME`.
  * `AD_PREFS.INI` stays absent, so AD_SND's own mute default is off.
    (After Dark 2.0, since the seventh release: the lane seeds its `Path`
    and `SoundDriver`, and AD_SND 1.0 reads a `[Sound] Mute` it wrote
    itself into the state overlay; the bridge's `adwSetSoundMute` sets the
    mute right after, for every module that wants sound, §2.12. After Dark
    3.x packages, with the twelve releases: the lane seeds the `Path` and
    `SoundDriver` `ADW30.EXE` wrote, which only the Disney Collection's
    library reads, §2.9.)
  * A package that ships no AD_SND (Snoopy's Screen Savers) gets the host's
    own (§2.10); the bridge sets its mute and volume as it sets any
    AD_SND's.
  * `ADSOUNDDEV=0` keeps meaning "no wave device at all".

### 8.2 `sndPlaySound` (MMSYSTEM.2)

* `SND_MEMORY`: read `riff_extent` bytes from the far pointer (bounded by the
  segment limit), copied at the call (§2.10). `parse_wave` + `decodable()` →
  `decode` → one engine buffer.
* A name without `SND_MEMORY`: a file through the DOS/VFS layer, else a
  `[sounds]` alias from the guest's WIN.INI. Not used by the corpus.
* **One sndPlaySound voice per process**:
  * A new call stops the current sound, unless `SND_NOSTOP` (0x10) while
    playing → FALSE.
  * `NULL` stops and returns TRUE.
  * `SND_LOOP` (with `SND_ASYNC`) loops.
  * `SND_ASYNC` returns TRUE at once.
  * **Synchronous (no `SND_ASYNC`, NOCTURNE)**: the voice starts at `t` and
    the call returns having consumed the sound's duration of virtual time, as
    the blocking call did on the original. The lane charges it to its time
    model (a `Runtime16` hook; deterministic). It is never a real wait.
  * An undecodable image → FALSE; `SND_NODEFAULT` has nothing to fall back
    to.

### 8.3 waveOut, midiOut, aux, mixer

* `waveOutGetNumDevs` 1.
* `waveOutGetDevCaps` "Long After Dark" (today's "(silent)" name only while
  disabled).
* `waveOutOpen(WAVE_FORMAT_QUERY)`: PCM `playable()` plus IMA/MS-ADPCM on
  `WAVE_MAPPER` (the Win95 mapper converted them).
* A real `waveOutOpen`: an engine stream (PCM), full header model as §7.4,
  `MM_WOM_OPEN`/`DONE`/`CLOSE` per §8.6. Not used by the corpus; implemented
  for completeness and tested with synthetic callers.
* `waveOutGet`/`SetVolume` → wave bus.
* `midiOutGetNumDevs` → **1** (the engines' `IsMusicAvail`, §2.9).
* `midiOutGetDevCaps`: "Long After Dark MIDI", `MOD_MAPPER`-like synth,
  16 channels, `MIDICAPS_VOLUME|LRVOLUME`.
* `midiOutGet`/`SetVolume` → MIDI bus.
* **The raw port** (for a guest that sequences itself: Star Wars Screen
  Entertainment's MEMMIDI, under SWSE; no After Dark binary imports
  `midiOutOpen` or `midiOutShortMsg`, statically or by name):
  * `midiOutOpen(MIDI_MAPPER or 0)`: a handle (`0xE000`… in fours, as
    `waveOut`'s), with `CALLBACK_NULL`/`WINDOW`/`TASK`/`FUNCTION` and
    `MM_MOM_OPEN`/`DONE`/`CLOSE` per §8.6; `*lphMidiOut` is zeroed first. One
    client at a time, as a Win16 MIDI output device (the mapper too) had:
    a second open → `MMSYSERR_ALLOCATED`. Another device id →
    `MMSYSERR_BADDEVICEID`; an unknown callback type → `MMSYSERR_INVALFLAG`.
    (Not coupled to the MCI sequencer's songs: no guest has used both.)
  * `midiOutShortMsg` → `midi_short` at the call's time (§8.6), `MMSYSERR_NOERROR`
    (never `MIDIERR_NOTREADY`, on which MEMMIDI would retry).
  * `midiOutPrepareHeader`/`Unprepare` (`MHDR_PREPARED`; a 28-byte MIDIHDR with
    data, else `MMSYSERR_INVALPARAM`; `MIDIERR_STILLPLAYING` while
    `MHDR_INQUEUE`). `midiOutLongMsg` needs a prepared header
    (`MIDIERR_UNPREPARED`); the buffer is copied to `midi_long` at the call,
    `MHDR_DONE` is set when the call returns (the synth takes it at once),
    and `MM_MOM_DONE(hmo, lpMidiHdr)` goes out at the next delivery point
    (sent from a procedure, a later one than the delivery running it,
    §8.6). Nothing is ever left queued, so `midiOutClose` never answers
    `MIDIERR_STILLPLAYING` and `midiOutReset` has no header to return.
  * `midiOutReset` → `midi_reset`. `midiOutClose` sends nothing to the synth.
  * `midiOutCachePatches`/`CacheDrumPatches` → `MMSYSERR_NOTSUPPORTED`: the
    caps carry no `MIDICAPS_CACHE` (MEMMIDI checks it and never asks,
    1:01bf), and a device without it answers so (the `MODM_CACHEPATCHES`
    contract; Wine's mapper, `dlls/midimap`, too). No document of the
    Windows 95 mapper's own answer was found (§10.6).
  * `midiOutGetID`: the id opened (`0xFFFF` for the mapper).
    `midiOutMessage` → `MMSYSERR_NOTSUPPORTED`.
  * Without an enabled engine there is no MIDI device: `midiOutOpen` zeroes
    the handle and answers `MMSYSERR_NODRIVER` (the mapper) or
    `MMSYSERR_BADDEVICEID`; every handle is `MMSYSERR_INVALHANDLE`. (The
    signature-only `midiOutOpen` of before returned 0, "success", without
    writing the handle; SWSE reads only the handle, 1:688e, so nothing it
    does changes.)
* `auxGetNumDevs` 2 and `aux*` as §7.6.
* `mixerGetNumDevs` stays 0, and with the engine on the mixer API is not
  exported **by name** (`GetProcAddress` answers 0; ordinals still resolve).
  AD_SND 3.2/TT then sets the wave and MIDI buses to the module volume
  through its `waveOutSetVolume`/`midiOutSetVolume` path (§2.10), as the
  other AD_SND builds do; with the names there, it would take a mixer path
  that sets nothing on a machine without a mixer device.

### 8.4 `mciSendString` (MMSYSTEM.702)

A small parser (case-insensitive, whitespace-separated, quoted paths)
over the engine's songs:

| Command | Behaviour |
|---|---|
| `open sequencer` / `open sequencer!<path> [alias <a>] [wait\|notify]` / `open <path> type sequencer [alias <a>]` | device-only or element open (the path through the DOS/VFS layer, relative to the guest's current directory); errors as §7.5; `sequencer` only, anything else `MCIERR_DEVICE_NOT_INSTALLED` |
| `close <a>\|all [wait]` | `close_song`; pending notifies of that device get `MCI_NOTIFY_ABORTED` |
| `play <a> [from n] [to n] [notify] [wait]` | `song_play`; `notify` → `MM_MCINOTIFY(MCI_NOTIFY_SUCCESSFUL, devId)` to `hwndCallback` at `song_end`; a new `notify` command on the device supersedes an older one (`MCI_NOTIFY_SUPERSEDED`); `play … wait` → `MCIERR_UNSUPPORTED_FUNCTION` (not in the corpus) |
| `stop <a> [wait]` | `song_stop`; a pending play notify → `MCI_NOTIFY_ABORTED` |
| `seek <a> to start\|end\|<n> [wait]` | `song_seek` |
| `status <a> mode\|length\|position\|ready` | "playing"/"stopped", ms, "true" in `lpstrReturn` |
| `set <a> time format milliseconds\|ms` | ok |
| anything else | `MCIERR_UNRECOGNIZED_COMMAND`, logged under `ADTRACE=sound` |

Return: 0 or an `MCIERR_*` DWORD in DX:AX; `lpstrReturn` always
NUL-terminated.

### 8.5 The engines' music gates (§2.9)

For AD 3.x music, L16 must make these hold and check them by tracing
RATRACE, TOAST3, YBYH, CS, FRANKEN, LISA, SNOWBALL:

* `midiOutGetNumDevs` ≥ 1.
* `GetVersion` as today.
* `LoadLibrary("TOOLHELP.DLL")` ≥ 32.
* `LoadLibrary("MCISEQ.DRV")` ≥ 32: a stub module (resident name `MCISEQ`,
  no exports needed unless tracing shows otherwise).
* `GetProcAddress(TOOLHELP, "GLOBALFIRST"/"GLOBALNEXT")` non-NULL. ADXPL300,
  310 and 40 call them only on an exact 3.10 (§2.9), so never here; the
  Disney Collection's ADXPL100 calls them before its songs, and gets an
  empty walk (§2.9).
* `RegisterClass`/`CreateWindowEx` of `adwMidiCall` with its guest
  `MIDIWNDPROC`.
* Delivery of `MM_MCINOTIFY` to that window (§8.6).
* The package's own AD_SND stays in charge of effects.

### 8.6 Callback delivery (both lanes' model)

* **Sources**: `engine.poll(now)` → `chunk_done` → `MM_WOM_DONE`;
  `song_end` → `MM_MCINOTIFY(SUCCESSFUL)`. Plus the synchronous ones the
  lane raises itself: `MM_WOM_OPEN` at open, `MM_WOM_CLOSE` after the last
  DONE, `MM_MOM_OPEN`/`DONE`/`CLOSE` (§8.3), and `SUPERSEDED`/`ABORTED` when
  the superseding or aborting command runs. And the periods of the
  **multimedia timer events** (below), which need no engine.
* **Order**: by event time, then by issue order; per device in the order
  above.
* **Window messages** (`CALLBACK_WINDOW`, MCI notify) are **posted** to the
  guest window's queue as a timer message would be. The lane's message pump
  dispatches them before the next DRAWFRAME, where the original host's
  message loop ran; a guest that pumps its own queue gets them earlier.
  `MM_MCINOTIFY`: `wParam` = notify code, `lParam` = device id (low word).
* **`CALLBACK_FUNCTION`** (Win16 far procedure `(HWAVEOUT, WORD msg, DWORD
  inst, DWORD p1, DWORD p2)`, PASCAL) is called from the host, never in the
  middle of a guest instruction. Delivery happens at the first safe point at
  or after the event's time: the next MMSYSTEM/KERNEL/USER API call (before
  the call runs) or the next DRAWFRAME boundary. Each call is a nested
  `call_far` with the callback's DS. A callback may call only the
  interrupt-safe APIs (`PostMessage`, `timeGetTime`, `waveOutWrite`…), as on
  Windows. Re-entrancy is guarded: no delivery while a delivery is running.
* **A procedure's calls are dated at its event's time.** On Windows a
  `CALLBACK_FUNCTION` or timer procedure ran at interrupt time, at its
  event's time; here it runs at the first safe point after it. The MMSYSTEM
  calls it makes are dated from its due time plus the virtual time it has
  run since, not from the safe point: the notes a timer sequences (MEMMIDI)
  reach the log and the synth at their own times, and a one-shot event set
  again from its procedure keeps its schedule, however late the safe points
  come (a module that makes no call between frames is delivered once a
  frame, and a frame that ends inside a long call leaves its periods to the
  next one). Neither a delivery nor the ne16 lane's step end renders the
  engine past a due point that has not been delivered: a delivery takes it
  only as far as the events due, the step end only up to the next due point
  (`Runtime16::audio_due()`), not to the guest's time. So those dates are
  not clamped to a later time the engine has already taken — but for the
  few µs a procedure runs, which the next one due at the same delivery
  point may find the engine past. Clocks the guest reads (`timeGetTime`,
  `GetTickCount`) still read the safe point's time: they never go back.
* **MCI commands are the exception**: a procedure that sends one (Win16
  forbade it: MCI was no interrupt-safe API; no guest does) has it dated at
  the safe point, which is never behind a time the engine has taken, so a
  song starts at the command's own time and a `play … to` stop, and its
  `MM_MCINOTIFY`, come when the song has really played to `to`.
* **What a procedure causes waits for a later delivery point.** A
  notification a procedure raises while a delivery runs it — its device
  opened or closed (`MM_WOM_OPEN`/`CLOSE`, `MM_MOM_OPEN`/`CLOSE`), a long
  MIDI message sent (`MM_MOM_DONE`), a WAVEHDR it wrote that the stream has
  done by the delivery point's time (an empty one) — is delivered at a later
  delivery point than the one running it, whatever its date, as
  `MM_MOM_DONE` after a guest's own `midiOutLongMsg` waits for the next
  delivery point. A procedure that answers each notification with another
  request (streaming SysEx by `midiOutLongMsg` from `MM_MOM_DONE`, an empty
  WAVEHDR written again from `MM_WOM_DONE`) thus takes one step per delivery
  point instead of looping inside one while its dates, and virtual time,
  barely move: with `ADMIPS=0`, or once the 1 s cap on the instructions no
  clock read has charged is reached, they do not move at all, and such a
  loop would never end. With the timer events' bound (below), every
  delivery ends.
* **Multimedia timer events** (`timeSetEvent`, `system16.cc`, engine or no
  engine; with sound off nothing but a timer event installs the delivery
  hook, so a module that sets none runs exactly as before):
  * `timeSetEvent(wDelay, wResolution, lpFunction, dwUser, fuEvent)`: 1 to
    65535 ms (the 16-bit `TIMECAPS`; Wine's winmm, `MMSYSTIME_MININTERVAL`/
    `MAXINTERVAL`); `TIME_PERIODIC` or one-shot; at most 16 events (Wine's
    table); ids from 1; 0 on failure. The first period is due `wDelay` ms
    after the call's time (after the procedure's due time when a procedure
    sets it).
  * Each period is one nested call of `TimeProc(wID, 0, dwUser, 0, 0)`, FAR
    PASCAL (MEMMIDI's MIDITIMERPROC reads `dwUser` at `[bp+0Eh]` and returns
    `retf 10h`, 1:1380), with its module's DS, at the first safe point at or
    after the period, in (time, issue) order with the other callbacks. The
    event is rescheduled before the call, so the procedure may kill it or set
    others. A one-shot event is gone once called (`timeKillEvent` then
    answers `TIMERR_NOCANDO`).
  * **Missed periods are caught up**: each gets its call, as Windows kept a
    periodic event on its schedule after a delay (Wine's winmm, time.c, on
    Windows' behaviour). Bounded: a periodic event more than 250 ms behind
    (a streamed run's host held up, frames not stepped) drops its oldest
    periods, keeping the last 250 ms of them (at least one) on its grid, and
    an event a procedure sets is dated no more than 250 ms back (so a
    one-shot chain catches up as little).
  * `timeKillEvent`: `TIMERR_NOERROR`, or `TIMERR_NOCANDO` for no such event.
    `timeBeginPeriod`/`timeEndPeriod`: `TIMERR_NOERROR` (0 ms:
    `TIMERR_NOCANDO`); the resolution changes nothing here.
    `timeGetDevCaps`: {1, 65535} (a missing or short `TIMECAPS`:
    `TIMERR_STRUCT`).
  * A procedure whose code segment was freed with its event still set kills
    the event (logged) instead of faulting.
  * Cost: MEMMIDI's procedure runs ~500 guest instructions a period; 250
    periods a virtual second cost about 2 ms of host time (§10.6).
  * Traces: `ADTRACE=sound` has `timeSetEvent`/`timeKillEvent` and the
    periods dropped behind the bound, but not the periods themselves nor the
    short messages, hundreds a virtual second: `ADTRACE=timer16` gives a
    line per procedure call (the event, its procedure and `dwUser`, the
    period's due time and the time it was delivered at), `ADTRACE=midi16`
    one per `midiOutShortMsg` with the time it is dated at.
* Deterministic: the delivery points are functions of the guest's own calls
  and virtual time.

---

## 9. The saver (SCR: `scr`, `common/ui`)

* **Settings** (`settings.ini [Saver]`, DESIGN §6a):
  * `Sound=1|0`, default **1**. The user asked for audio. A saver that makes
    sound is unusual, so the control is prominent and one click turns it off.
  * `Volume=0..100`, default 50 (After Dark's default; §6.7 for what it
    means).
  * `SoundMonitor=primary` is reserved: primary is the only value, written
    and kept for later.
* **Dialog**:
  * In the display-options panel (with Scale and Monitors) a **Sound** combo,
    "Primary monitor" / "Off", and a **Volume** slider 0–100 (page 10; accessible
    name "Volume"; disabled when Off).
  * A one-line note: "Sound plays from the primary monitor's screen saver."
  * Themed through `adw_ui`, which gains the slider's custom drawing for the
    light, dark and high-contrast palettes.
* **Who gets sound**: only the host of the **primary monitor's window**
  (`this == app_.owner()`, the input owner), and only when `Sound=1` and the
  run is `/s` (the Preview button's `/s` included). That host gets
  `ADSOUND=1 ADVOLUME=<Volume>`. Every other spawn sets `ADSOUND=0`
  explicitly and clears `ADAUDIOOUT` against inheritance:
  * the other monitors' hosts;
  * `/p` (the Control Panel thumbnail);
  * the dialog's live preview;
  * the thumbnail generator;
  * `--configure`.
* **Preview**: the dialog's Preview runs `/s` with the dialog's current
  (unsaved) settings, as today, so the Sound and Volume shown are what plays.
  The live thumbnail in the dialog stays silent.
* **Rotation**: the owner's next module gets a new host with sound; the old
  one is stopped. When the owner changes (a monitor disappears), the new
  owner's next spawn gets sound.
  As built, the window that was the primary's also restarts its module at
  once without sound when the primary monitor changes, so two hosts never
  play; with one module and no rotation, sound then stays off until the
  saver restarts.
* **Waking**: the audio host is sent `QUIT` first, before windows are torn
  down, and stopped with a grace of **400 ms** (`kSoundHostStopGraceMs`;
  150 ms for silent hosts), at a wake, a rotation and a monitor change, so
  it can silence the device and send MIDI all-notes-off. Its audio stops
  within the latency.
* **Display off**: the saver sends no GO while the display is powered off,
  so the sound host waits with its sinks open: live PCM underruns to
  silence, and live MIDI releases its sounding notes after latency + 500 ms
  without an `advance` (§6.6).
* **Tests**: `AD_SCR_SOUND=0` (also `off`, `no`, `false`) forces sound off
  whatever the settings say. The scr CTest suite sets it for every test (the
  GUI smoke tests run `/s`), except one opt-in test. The sound tests:
  `scr_unit_sound` (settings, the who-gets-sound rules, the spawn
  environment), `ui.slider` (adw_ui's `SliderSpec`/`init_slider`: keys, the
  accessible name, the three palettes), `scr_smoke_sound` (owner vs other
  monitor, rotation, a primary change: never two sound hosts),
  `scr_smoke_sound-wake` (QUIT to the sound host first; the graces),
  `scr_smoke_config-sound` (the dialog's controls; Preview plays the unsaved
  volume on its primary host only), and the opt-in `scr_smoke_e2e-sound`
  (`AD_E2E=1 AD_E2E_ASSETS=… AD_SCR_SOUND_E2E=1`: the real host on two staged
  monitors, captured with `ADAUDIOLIVE=0`, Burns' speech in the WAV).

---

## 10. Tests and verification

Nothing automated plays on the real device except the one opt-in check in
§10.3. Every assertion is on engine state, WAV captures or MIDI logs.

### 10.1 CORE (`core.unit`, `core.e2e`, a new `core.audio`)

* **Formats**: `parse_waveformat`/`parse_wave` (PCM 8/16 mono/stereo, fact,
  odd chunks, truncation, bogus sizes). `riff_extent`.
* **Decoders, bit-exact against the host's own ACM**: synthesize PCM (sweeps,
  noise, silence, clipping) at test time and encode it with `msacm32`
  (imaadp32, msadp32). Decode with both Windows' ACM and `audio::decode`,
  and compare byte for byte, partial last blocks included.
  `decoded_size`/`encoded_size_for` against `acmStreamSize`. No After Dark
  file is used.
* **Gains**: `gain_from_ds` table monotonic, −10000 → 0, 0 → 0x8000, pan
  laws; `gain_from_mm` ↔ `mm_from_gain` round trip.
* **Mixer determinism**: a scripted scenario (two buffers, looping, pan,
  rate change, `write_buffer` while playing, gain changes, a stream with
  pause/reset) rendered twice → identical bytes, plus a golden FNV-1a of the
  capture.
* **Timing**: `cursor`/`playing`/`end_time`/events against hand-computed
  values, e.g. a 11025-frame 22050 Hz voice started at 1000 µs ends at
  501000 µs. The rendered position matches `cursor()` (a ramp buffer); the
  answers are independent of `ADAUDIORATE` (22050 vs 44100 vs 48000);
  monotonic clamping works.
* **Streams**: chunk order, gapless joins, `chunk_done` times, pause/restart,
  reset semantics.
* **SMF**: synthetic files built by the test (format 0/1, running status,
  tempo changes mid-song, SysEx, SMPTE division). Checks cover event times,
  length, seek with chase, the MPC rule (dual-mode detection and the
  `ADMIDIBASE` override) and CC7 scaling on bus-gain changes.
* **Raw MIDI**: the `.mid` log of `midi_short`/`midi_long`/`midi_reset`
  calls interleaved with a song's events (time order), running status
  (kept across real-time bytes, ended by system common and by a reset),
  the unread high bytes of a short message, system messages as `F7`
  escapes, SysEx and message streams in a long message (malformed and cut
  short ones dropped), real-time bytes inside a long message's channel
  message, running-status message or SysEx (out on their own, the message
  kept), the CC7 rule (re-sent on a gain change, a first-use default when
  not at unity), the reset's note-offs/CC64/CC123, silencing at `shutdown`,
  and a disabled engine ignoring them.
* **Captures**: the WAV header (rate, 16/2, length = `floor(t_end·R/10⁶)`
  frames, patched on shutdown and on the 5 s grid); the `.mid` log re-parses
  with our own SMF parser, times in ms; a killed-process file is still
  readable.
* **Config**: every knob, clamping, warnings, `capture_mid` derivation.
* **e2e** (`core.e2e`):
  * `adhostwin --test-pattern ADTESTAUDIO=1 ADFRAMES=180 ADAUDIOOUT=<tmp>.wav`
    (6 s at the pattern's 30 fps) → RMS above −30 dBFS in each
    `[n, n+0.1 s)` window and below −80 dBFS elsewhere; `.mid` note-ons at
    0, 2 and 4 s.
  * The same without `ADAUDIOOUT` and `ADSOUND` → no files, `FBHASH`
    unchanged from today.
  * `ADSOUND=1` headless → identical `FBHASH` to the `ADAUDIOOUT` run.
  * `--capabilities` shows `audio=1`.
* **Live sink** (opt-in `AD_AUDIO_LIVE_TEST=1`, never in CI): §10.3.

### 10.2 Lanes (L32 `win32.*`/`pe32.*`, L16 `win16.*`/`ne16.*`)

Shims are tested through their registries with synthetic guest memory and
synthetic callers, as the existing suites do, against an engine made with
`make_engine(Config{guest_sound=true, capture_wav=<tmp>})`.

* **L32**:
  * DirectSound: vtable layout (every slot resolves to its thunk), refcounts,
    `CreateSoundBuffer` validation, Lock/Unlock wrap and copy, `Duplicate`
    sharing data, `GetCurrentPosition`/`GetStatus` against virtual time,
    `SetVolume`/`SetPan`/`SetFrequency`.
  * ACM: `acmFormatEnumA` calls a synthetic guest callback in index order and
    stops on FALSE; `acmStreamConvert` output equals `audio::decode`.
  * waveOut: `WHDR_DONE` appears exactly at the virtual end; pause/reset/
    close errors.
  * MCI: every command and flag of §7.5; the Deluxe alias table.
  * aux/mixer answers; `enabled()` false keeps today's answers (the existing
    tests unchanged).
* **L16**:
  * `sndPlaySound` flag matrix (ASYNC, sync with charged time, LOOP, NOSTOP,
    NULL, a non-memory name); MS-ADPCM images; copies made at the call.
  * waveOut/midiOut/aux counts and volumes, enabled and disabled.
  * The `mciSendString` grammar of §8.4.
  * `MM_MCINOTIFY` posted at the right virtual time, then dispatched to a
    synthetic window procedure before the next DRAWFRAME.
  * SUPERSEDED/ABORTED order.
  * `CALLBACK_FUNCTION` delivered at the next API call and never re-entrantly;
    a procedure answering each `MM_MOM_DONE` with `midiOutLongMsg`, or each
    `MM_WOM_DONE` with an empty WAVEHDR written again (the real engine),
    takes one step per delivery point, with `ADMIPS=0` and at 100 MIPS with
    a delivery point 1.1 s late.
  * The `MCISEQ.DRV`/`TOOLHELP` gates.
  * midiOut's raw port: open/short/long/reset/close, the one-client rule,
    MIDIHDR flags and errors, `MM_MOM_*` to a window and to a function,
    patch caching refused, the honest failures without an engine.
  * Timer events with synthetic `TimeProc`s: periodic and one-shot calls at
    their periods (counts, times, `dwUser`), missed periods caught up and
    the 250 ms bound, kill, the 16-event table, re-entrancy, a one-shot set
    again from its own procedure keeping its schedule, the `midiOutShortMsg`
    a procedure sends dated at its periods, timers without an engine,
    determinism; and with the real engine, a timer-driven sequencer's
    note-ons 4 ms apart in the `.mid` log though delivered once a frame
    (printing the cost of 250 callbacks a virtual second), also in the ne16
    lane's order — frames of 10 ms of guest work without a call, the engine
    rendered up to `Runtime16::audio_due()` at each frame's end — and a timer
    procedure's `play … to 800 notify`, after the engine was rendered past its
    due time, playing 800 ms from the song's real start before it stops and
    notifies.

### 10.3 Real modules (opt-in, skip 77 without assets; in `core/tests` as `core.audio_assets`)

Each case runs `adhostwin <module> ADFRAMES=1800 ADGOWAITMS=0
ADAUDIOOUT=<tmp>/<m>.wav` on the assets (`AD_E2E_ASSETS` or the installed
root, read-only) with a scratch `AD_LOCALAPPDATA`:

| Case | Asserts |
|---|---|
| `FILES/AD40/TOASTERS.AD` (Flying Toasters, AD4) | `.mid` has ≥ 100 note-ons (`Flying Toasters.mid` via the Deluxe rename); `[audio] voices` > 0 (the WAV is asserted non-silent once L32 has seen which noises it plays in 30 s) |
| `packages/ad10/AD10TH/TOASTER2.AD` | music from `MUSIC\Toasters2k.mid`, the static engine copy (music only: in attract mode it never requests an effect, §10.5) |
| `packages/simpsons/SIMPSONS/BURNS.AD` (speech from `SIMP_SND.DLL`) | ≥ 4 windows of 100 ms above −40 dBFS, each starting within 20 ms of a voice start logged by `ADTRACE=audio` |
| `FILES/AD40/FISH.AD` (IMA through ACM + DirectSound) | non-silent WAV; `[audio] voices` > 0 |
| `packages/tt/TWISTED/BUNGEE.AD` (MS-ADPCM through `sndPlaySound`) | non-silent WAV |
| `FILES/CLASSIC/TOAST3.AD` (16-bit music, `MM_MCINOTIFY`) | `.mid` note-ons, and more than one song start in 5 min (the notify loop), with its Music control at "Always" (`ADCVSET=2=80`; the default "Once" plays the song once) |
| `packages/ad10/AD10TH/HALLOFFA.AD` | non-silent WAV |
| any of the above twice | byte-identical WAV and `.mid`, identical `FBHASH` |

L32 or L16 fixes a case during implementation when a module turns out to
play nothing in its first 30 s. The census column "Plays in 30 s" is the
guide for ne16.

**The one live check** (`AD_AUDIO_LIVE_TEST=1`, run by a person, never by
CTest by default): the engine plays a 1 s 440 Hz tone at −30 dBFS on the
default device through WASAPI, then a 1 s MIDI note at velocity 40. It
asserts no underrun after prefill and a clean `shutdown`. Total ≤ 3 s.

### 10.4 The census (`research/win/audio/verify.py`)

After integration, `verify.py` runs all 202 modules for 30 s with
`ADAUDIOOUT` (12 at a time; no device, no user data). Per module it measures
peak, RMS, the count of 100 ms windows above −50 dBFS, and the note-ons of
the `.mid`, then classifies against `census.json`:

* `ok`;
* `silent-ok`;
* `MISSING-SFX` (expected effects, silent WAV; legitimate for the few that
  play nothing early);
* `MISSING-MUSIC` (a song the module names exists, but no note-ons);
* `UNEXPECTED`;
* `FAILED`.

Acceptance: no `FAILED`, no `UNEXPECTED`, no `MISSING-MUSIC`, and every
`MISSING-SFX` explained in `verify.json` (for example by a longer run).

### 10.5 Integration results (2026-09-26)

Run on the full tree (`build/win-au-integ`) against the five imported
releases (`build/win-pkg-setup/assets`), scratch data only.

* **CTest**: every suite passes (108 tests; the opt-in and absent-data ones
  skip). The opt-in `core.audio_assets` (all seven cases of §10.3 and the
  repeat) and `scr_smoke_e2e-sound` pass too.
* **Census** (`research/win/audio/census_result.py` → `census_result.json`,
  the table in `census_result.md`): every module for 1800 frames with
  `ADAUDIOOUT` at the default volume 50, then 5 minutes for any that stayed
  quiet. **127 ok, 66 silent-ok, 9 quiet-ok; no `MISSING-SFX`,
  `MISSING-MUSIC`, `UNEXPECTED`, `FAILED` or suspect (clipped or noise-like)
  capture**, and no unimplemented API called. It refines §10.4's
  expectations with what the lanes found:
  * ne16 effects are expected in 30 s only when the stub run saw a non-NULL
    `sndPlaySound` (§1.3's column counts the stops).
  * TOAST2K and TOASTER2 never request an effect in attract mode: the
    sprite sound-event count at `+0x13E2` is only ever zeroed (constructor
    `TOAST2K.AD` `0x1001d877`), so the lookup at `0x1001d938` finds nothing.
    They play their music only.
  * CS and MIMEHUNT: Music "Never" by default.
  * The 9 quiet ones: CONFETTI (×2, first effect at 34.9 s), BUGS (×2, 67 s
    and 279 s), LUNATIC (×2; a game whose sounds belong to its interactive
    mode), HOW2DRAW and MIMEHUNT (×2): nothing in 5 minutes of attract mode.
* **Levels** (peaks of the audible captures at volume 50): AD4 −8 dBFS
  median (DirectSound at −10 dB), every Classic release −6 dBFS (the wave
  device at half). `ADVOLUME` 20/50/80 gives −14/−6/−2 dBFS in Deluxe, 10th,
  AD 3.2 and Totally Twisted alike, and scales the MIDI (CC7 20/50/80).
  **Fixed at integration**: AD 3.2's and Totally Twisted's AD_SND took its
  mixer path because the mixer API resolved by name, and with no mixer device
  set no volume at all, so those 31 modules played at full scale whatever the
  Volume slider said (§2.10, §8.3). Their AD_SND now sets the wave and MIDI
  buses like the other builds.
* **Content** (`research/win/audio/spot_check.py`):
  * Songs, note-on by note-on (channel, key, velocity, time ±2 ms) against
    the files: Flying Toasters (Deluxe, through the rename) 3102/3102 of
    `TOASTERS.MID`, then its restart; TOASTER2 1054/1054 of `Toasters2k.mid`;
    RATRACE (10th) intro 234/234, loop 1377/1377 and again, end 48/48 (the
    `MM_MCINOTIFY` chain, MPC rule applied); FRANKEN (TT) `HORROR.MID`
    305/305 and its replay; MBORIS (TT) `DAWN.MID` 66/66; POINTS, SLOWBURN,
    SWIRLING, LISA, SNOWBALL, SIMPFILE and INS all match, with no capture
    note-on left unexplained.
  * Effects, voice by voice against the module's own resources (decoded by
    an independent Python decoder): BURNS' speech 4/4 voices = `SIMP_SND.DLL`
    resources 5001, 5008, 20017 (correlation 1.000, gain 0.500); CHAM (TT)
    9/9 audible voices = `TT_SND.DLL` and its own MS-ADPCM resources; BUNGEE
    (MS-ADPCM) 22/22; FISH (IMA-ADPCM through ACM and DirectSound, its voices
    always overlapping): every audible 0.5 s window is ≥ 99.97% explained by
    the mix of its resources.
* **Live** (the one real-device check): `LongAfterDark.scr /s` through
  `CreateProcess` on this machine's two monitors, `ad10.franken` at volume
  15, `AD_SCR_TESTEXIT_AFTER_FRAMES=180`: 3.5 s in all. The primary window's
  host alone got `ADSOUND=1` and printed the `[audio]` line; it played
  through WASAPI shared (MIDI through the mapper) with 0 underruns, and
  8444 frames (191 ms) dropped once at the start: the Classic lane's guest
  time runs ahead of the clock by its modelled init time, so the first
  render overfills the ring and the oldest frames go (§6.6). The Windows
  session meter saw exactly one process sending sound, `adhostwin.exe`,
  session "Long After Dark", peaking at −16.5 dBFS (the capture of the same
  run headless: −16.6). The sound host got `QUIT` first and ended cleanly.

### 10.6 Star Wars Screen Entertainment's music (2026-09-28)

SWSE.DLL opens the MIDI mapper and hands the song file to MEMMIDI (Sonic
Foundry), which plays it itself: `timeBeginPeriod(4)`, a 4 ms periodic
`timeSetEvent` whose procedure advances the song by one period and sends
the due events with `midiOutShortMsg` (§8.3, §8.6). Checked on the real
modules through WP W's Intermission harness (the real `IMIMXPLY.IMQ`,
SWSE, MEMMIDI; 1800 frames, 30 s of 60 Hz headless time, the engine
capturing), each capture's note-ons matched (channel, key, velocity, within
±8 ms of a least-squares fit) against the song files:

| Module | Song | Note-ons in 30 s | Matched | Largest deviation | Tempo (fitted) |
|---|---|---:|---:|---:|---:|
| VADER | `EMPIRE.MID` | 804 | 804 | 4.5 ms | 0.99960 |
| BATTLES | `BATTLE.MID` | 765 | 765 | 2.9 ms | 1.00001 |
| CANTINA | `CANTINA.MID` | 656 | 656 | 2.0 ms | 0.99998 |
| TRENCH (Music "Once"; the shipped `SWSE.INI` says "Never") | `BATTLE.MID` | 682 | 682 | 2.1 ms | 1.00001 |
| SWTEXT | `SWTHEME.MID` | 1000 | 1000 | 2.6 ms | 0.99997 |

* Channels 1–10, as in the files; the deviations are MEMMIDI's 4 ms grid.
  EMPIRE's 588 235 µs a quarter plays 0.04% fast, as 588 ms a quarter would
  (MEMMIDI's own arithmetic, EMPIRICAL): the file's tempo otherwise.
* 6 400–7 500 timer procedure calls a run (250 per virtual second from the
  song's start), none dropped; MEMMIDI's procedure runs about 500 guest
  instructions a call. Against the same frames without the event, the host
  spends about 2 ms more per virtual second, inside run-to-run noise (the
  isolated cost of a delivery with a small procedure and one
  `midiOutShortMsg`: 0.6 µs, `win16.sound`). A module that makes no call
  between frames is delivered once a frame, and a frame that ends inside a
  long call leaves the periods due to the next one, yet its notes keep the
  4 ms grid: they are dated at their periods, and the lane's step end never
  renders the engine past a period still to be delivered (§8.6).
* Two captures of each module are identical (WAV, `.mid`, `FBHASH`). With
  sound off the `FBHASH` streams equal those of the same build without
  midiOut and timer events — also when the guest wants music (Volume 50)
  and there is no engine: SWSE reads only the handle, which `midiOutOpen`
  now zeroes and answers `MMSYSERR_NODRIVER`.
* After Dark: all 202 modules, runs A, C and S, identical to the build
  without these changes, the C runs' WAV and `.mid` captures included; no
  difference from the accepted baselines.
* Open points:
  * The Windows 95 mapper's own answer to `midiOutCachePatches` was not
    found; the device reports no `MIDICAPS_CACHE`, so MEMMIDI never asks.
  * Volume (resolved since): Intermission sets only the wave volume
    (`ANTSW.INI [Intermission] Volume` × 595 through `waveOutSetVolume`);
    on the original the Windows mixer's synth line alone set the music's.
    The IMX protocol now stands in for that line: at load, with the engine
    on, it sets the MIDI bus from the saver's volume, as After Dark's
    reaches it (linear, 50 = half amplitude; `host/ne16/imx_protocol.cc`,
    §6.7), so the raw port scales MEMMIDI's CC7 (§6.4). The `.mid`
    captures at `ADVOLUME` 10 and 100 differ.
  * The raw port and the MCI sequencer are not one client between them
    (§8.3); no guest uses both.

### 10.7 Star Trek: The Screen Saver's sound (2026-09-29)

The 16 real modules over the package's AD_SND 1.0 and `AD_MME.DRV` (§2.12),
captured with `ADAUDIOOUT` and `ADAUDIOLIVE=0` for 900 frames (15 s of
60 Hz headless time), twice (`ne16.startrek`, and the host work's
`research/win/pkg/startrek/host/tools/sound_run.py`, gitignored):

* 14 modules are audible in 15 s. Sounds started: Brain Cells 19,
  Communications 14, Final Exam 23, Final Frontier 1 (its 14.5 s theme),
  Horta 14, The Mission 1, Ship Panels 14, Planetary Atlas 23, Scotty's
  Files 9, Sickbay 7, Sounder 7 (`JIM.WAV`), Spock 13, Tholian Web 1 and
  Tribbles 12, peaking between −18 and −6 dBFS at volume 50. Ion Storm has
  no sound, and Space's one sound comes minutes later (at 400 s in a
  12-minute run).
* Against the resources (the content survey): correlation 1.0000 for Final
  Frontier's theme (resource 1005), 0.9999 for a Tribbles sound (1601) and
  0.9981 for a Tholian Web one (1001), within 0–1 samples of the traced
  start, at a gain of 0.500 at volume 50 with both channels equal.
  `ADVOLUME` reaches `waveOutSetVolume` (20 gives `0x33333333`, 100
  `0xFFFFFFFF`).
* Two captures of each module are identical, and so are its frame streams.
  In the content survey's runs sound on and off gave identical frame
  streams for 13 of the 16: Final Frontier, Horta and Ship Panels differ,
  because the muted path skips the driver's API calls, which cost virtual
  time.
* Not exercised: live output (`ADAUDIOLIVE=1`, §12).

### 10.8 The five later releases' sound (2026-09-30)

The modules of Marvel Comics Screen Posters, the Looney Tunes, ScreamSavers,
the Disney Collection and Snoopy's Screen Savers, from packages their own
imports made, captured with `ADAUDIOOUT` for 900 frames, twice (the
twelve-release integration's and the Snoopy work's runs, and the surveys'
longer ones; gitignored under `research/win/pkg/`):

* **The Looney Tunes**: all 12 make sound: effects from `LT_SOUND.DLL`, 2
  to 20 ids per module in 30 s, and the 13 MIDI songs through the MCI
  sequencer (§2.9), all of them within 30 s (Conducktor's sound is its
  music).
* **ScreamSavers**: all 15 play their modules' own type-3000 WAVs through
  their AD_SND 3.1.4, 2 to 32 voices in 15 s, peaks near −6 dBFS;
  correlation 1.0000 against the resources for the top matches (Spin Out,
  Big Mess, Head Butt, Swamp Lunch, Bone Crunch). Sound on and off give
  identical frames for 12 modules; Swamp Lunch, Bug Out and Melt Down wait
  on their sounds.
* **The Disney Collection**: 10 modules play WAV effects (through AD_SND
  3.2's `sndPlaySound`; Cheshire Cat's capture against its resource
  3000/1100: correlation 0.9993 at lag 0, gain 0.499 at volume 50, both
  channels equal) and 5 play MIDI songs (Beauty, Falling Flower, Captain
  Hook, Little Mermaid, The Sorcerer); Scrooge is silent in 60 s. The
  music modules' `GlobalFirst` calls answer the empty walk, and the census
  counts no unimplemented call.
* **Marvel Comics Screen Posters** is silent by design: AD_SND 1.0
  initialises, and the module plays nothing (0 voices, 0 MIDI events, a
  peak of 0; the stream equals the one with sound off).
* **Snoopy's Screen Savers**, over the host's own AD_SND (§2.10): the six
  modules that import it are audible, Dance (one looping song), Faces,
  Flying Ace, Linus & Snoopy, Literary Ace and Therapy (its synchronous
  plays, flags 0x06), at about −6 dBFS; flags 0x07, 0x06 and 0x0F all
  occur; Collage and Spotlights import no AD_SND and are silent. With no
  wave device (`ADSOUNDDEV=0`) all eight run, silent, the host's AD_SND
  having said why.
* Every capture is identical run to run. The frozen baselines' C-run
  captures of the 202 After Dark modules stayed byte-identical to 1.1.0's
  (404 of 404), so did the Looney Tunes' and ScreamSavers' under the After
  Dark 3.x seeds, and so did Star Trek's 16 under the computed palettes
  (PACKAGES.md §7.4).
* Not exercised: live output (`ADAUDIOLIVE=1`, §12).

### 10.9 Johnny Castaway's sound (2026-10-02)

Screen Antics: Johnny Castaway's `SCRANTIC.SCR`, a Windows 3.1 screen-saver
program (ABI.md §3.15), plays its own 23 `WAVE` resources (`WAVESFX1`..`25`)
with MMSYSTEM's `sndPlaySound(lpRes, SND_MEMORY|SND_ASYNC|SND_NODEFAULT)`;
no AD_SND, no MIDI. Captured from the package its import made with
`ADAUDIOOUT` and `ADAUDIOLIVE=0` for 3,600 frames (60 s), three times:

* 13 sounds in the first minute, the first loud one at 22.3 s, peaking at
  −3.9 dBFS at volume 50; the program's story decides what plays, and when.
* The three captures are byte-identical, and the frames equal those of a
  run with sound off.
* Its one `mciSendCommand` call (`MCI_CLOSE` of a device ID nothing sets) is
  never reached.
* Its own **Setup...** has a "&Sounds" box (`Sounds=` in `SCRANTIC.INI`);
  the saver's own sound setting still applies on top of it.
* Not exercised: live output (`ADAUDIOLIVE=1`, §12), and a run with the
  program's Sounds box cleared.

---

## 11. Work split

Four implementers, strictly disjoint trees. The frozen interface is §5; each
brief is in the design report that accompanied this document and is
summarized here.

| Owner | Tree | Delivers |
|---|---|---|
| CORE | `host/core/**` | `audio.h` (§5) and its implementation (§6), `LaneContext::audio`, `run_host` wiring, env knobs, `--capabilities`, `ADTESTAUDIO`, README, tests §10.1 and the asset cases §10.3 |
| L32 | `host/win32/**`, `host/pe32/**` | §7 and the L32 tests of §10.2; `lane.hh` knob docs |
| L16 | `host/win16/**`, `host/ne16/**` | §8 and the L16 tests of §10.2; `lane.hh` knob docs |
| SCR | `scr/**`, `common/ui/**` | §9, its tests, `scr/README.md` |

Until CORE lands, L32 and L16 build against a header-only stub of §5.
`null_engine()` plus an inline test double is enough for their tests. They
must not add files under `host/core`. Integration then runs §10.3 and §10.4.

---

## 12. Open questions and risks

* **MPC rule**: which channels the Win95 mapper really played with a GM synth
  (UNVERIFIED). The rule matches the authoring scheme and the 1995 edits;
  `ADMIDIBASE=1` makes a quick A/B.
* **16-bit gate internals**: answered for ADXPL310 from its listing (§2.9);
  ADXPL300 and ADXPL40 are the same in every trace but were not read
  instruction by instruction.
* **A play over a pending play notify**: the Classic lane answers
  `SUPERSEDED` (§8.4), the AD4 lane `ABORTED` (§7.5). Microsoft's
  `MM_MCINOTIFY` documentation reads as `ABORTED` for an interrupting
  command that asks for notification. It matters to ADXPL310's
  `MIDIWNDPROC`, where `SUPERSEDED` stops the song and `ABORTED` is
  ignored. It is not reachable in the corpus: the engine's play entry
  (ADXPL310 ord 413, `4:ee48`) plays only while the player is idle
  (`+0x0E`, cleared by the play at `4:ef9e`). Align the lanes if a caller
  that does this ever turns up.
* **Synchronous `sndPlaySound` outside a DRAWFRAME in a streamed run**
  (§8.2): with no frame to yield, `Runtime16::wait_until_us` moves the
  realtime clock ahead by the sound's duration for good. The live ring
  then gets the whole sound at once and keeps only its last `latency_ms`.
  The corpus's only synchronous caller, NOCTURNE (flags `0x0006`),
  makes its call inside DRAWFRAME, which yields frames (`ne16.assets`).
  Input-driven sounds are the interaction stage's to check.
* **Deluxe renames** are inferred from the 10th's `SETUP.INF` (§1.3).
* **Latency**: the 80 ms delay puts sound behind the picture by that much
  (lip movement in the Simpsons modules). Tunable with `ADAUDIOLATENCYMS`.
* **GS synth**: Windows 11 still ships the Microsoft GS Wavetable Synth
  behind the MIDI mapper. If a future Windows drops it, live music falls
  silent (logged), and captures are unaffected.
* **ADPCM bit-exactness** against `imaadp32`/`msadp32` is asserted on this
  machine's codecs, which are the ones Windows has shipped since the 1990s.
* **Timing changes with sound on**: sound-on `FBHASH` streams differ from
  sound-off ones (noise durations, streaming work, song windows). New
  sound-on baselines are recorded after §10.4 passes.
* **Live PCM clock drift**: the ring is fed at the virtual clock's rate (the
  wall clock in a streamed run) and drained at the device's; nothing
  resamples between them, so over a long run the fill drifts until the ring
  drops 100 ms or underruns once and refills (both counted in the `[audio]`
  line). Only the one short live check has run (§10.5); a long listening run
  would show how often that happens on a given device.
* **Live start**: a streamed Classic module's first render overfills the
  ring (its guest time leads the clock by the modelled init time), so about
  the first 190 ms of its sound are dropped (§10.5).
* **Display off**: live MIDI releases its notes after latency + 500 ms
  without an `advance` (§6.6); exercised only by reasoning, since no test
  opens a MIDI device.
* **Sound-on `FBHASH` baselines** have not been recorded yet (the item
  above). Sound-on runs are deterministic (§10.3's repeat case), so they
  can be.
* **Star Trek: The Screen Saver's sound** has only been captured
  (§10.7), never played on a device.
