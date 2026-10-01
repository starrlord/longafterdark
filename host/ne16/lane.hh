// adw_lane_ne16 — the Classic lane: a 16-bit NE module (an After Dark
// 2.x/3.x .AD, or an Intermission .IMX) run on the Win16 guest runtime
// (host/win16). Where the engine files come from follows the package rule
// (package.hh, PACKAGES.md §7.1/§7.3).
//
// Module protocols (protocol.hh): the lane (lane.cc) is the machinery every
// module needs — the runtime and its knobs, the display, the desktop seed and
// the small-screen guest display, the fiber that carries long calls, the draw
// budget, the scanout latch, input and status, the audio pump, the configure
// scaffolding — and it drives the module through a Protocol16, the host side
// of the protocol the module speaks: its choices and runtime options, the
// guest's disk, load, one call of each frame's run, SET, unload, and a button
// in configure mode. Below, "DRAWFRAME" in the lane's sections is that call.
// Two protocols exist, After Dark's own and Delrina Intermission's, and the
// module's exports choose (package.hh detect_kind; ADNE16KIND forces one):
// MODULE is an After Dark module, SAVERINIT with SAVERDRAW an Intermission
// one; an Intermission reader (SAVERMAIN alone), or an NE file with neither,
// is refused (exit 1). Both kinds are NE, so both are this lane's (catalog
// "lane":"ne16"; an Intermission entry adds "abi":"intermission"), and
// --capabilities lists both ABIs (Lane::abis: afterdark, intermission).
//
// The AD3 protocol (ad3_protocol.cc): an After Dark 2.x/3.x module driven the
// way AFTERDAR.SCR drove it through OLDMOD32's flat thunks (ABI.md §3.1–§3.3,
// §3.7). The host side of the AD3 module protocol is a bridge (bridge.hh):
// Berkeley's own OLDMOD16.DLL as real code, or — for the AD 3.x packages that
// ship none — the host-native AD3 bridge that does what OLDMOD16 does
// (PACKAGES.md §7.4).
//
//   init:  the bridge's SETADPALETTE16(hpal, i) for the four AD palettes
//          (AFTERDAR.SCR AD_PALETTE 101..104, or ADTASK.DLL 5000/1..4);
//          LOADADMODULE16(hwnd, hdc, ctrl4, volume, mute, path, err, 260,
//          &errId) — the bridge loads AD_SND.DLL and the module (which pulls
//          in ADXPL300/310/40, AD_RSRC, … by its imports) and sends
//          MODULESELECTED → PREINITIALIZE → INITIALIZE → BLANK itself.
//   step:  SetWindowOrg(hdc, 0, 0); MODULEMESSAGE16(2 /*DRAWFRAME*/, err, 260).
//          Result (AFTERDAR.SCR 0x401f6f): 0 ok; 0x0E toggles "wants events";
//          0x11/0x12 show/hide the cursor; below 0 or above 0x12 counts as 0;
//          anything else is the module's error (text in err) and ends the run
//          — but an After Dark 2.0 module's 5 is its wake (below).
//   SET:   SETMODULECTRLVALUES16(volume, mute, ctrl4) — four WORDs (ABI.md
//          §3.2); the 1996 host re-sent them every 2 s, we send on change.
//   close: UNLOADADMODULE16() (CLOSE, FreeLibrary, AD_SND unloaded), then
//          every module is freed — OLDMOD16's DLLENTRYPOINT(0) included.
//
// After Dark 2.0 (Star Trek: The Screen Saver; package.hh after_dark2: the
// module folder holds AD_MOD.DLL): AD.EXE 2.0b drove its modules with the
// same entry point, messages and blocks, and the native bridge drives them
// unchanged (the lane survey's oracle: byte for byte what the real OLDMOD16
// draws, both over AD_SND 4.0) over the package's own AD_SND 1.0, whose
// volume pair is adwSavePreviousVolume/adwRestorePreviousVolume
// (bridge.hh). What differs is the host's: its
// AD_PREFS.INI (mount_disk seeds [After Dark] Path, where AD_MOD.DLL finds
// ST_RES\, and [Sound] SoundDriver=AD_MME.DRV: win16/dos16.hh
// seed_after_dark2); its AD palettes, which AD.EXE built in code when a
// module asked for one, and which the lane computes the same way and hands to
// the bridge at the first request (package.hh palettes_after_dark2; no Star
// Trek module asks for one); and DRAWFRAME's result 5, which AD.EXE
// took as its wake (it posted itself its wake message, 0x7EE): the module's
// wake (Input and status). Final Exam ends its exam so, on a mouse move.
//
// A package that ships no AD_SND.DLL (package.hh host_ad_snd: Snoopy's Screen
// Savers, After Dark modules made to run in the user's own After Dark 2.0 or
// 3.0): the AD3 protocol registers the host's own AD_SND (win16/adsnd16.cc,
// AD_SND 3.0.3's entries) before the native bridge loads AD_SND, and the
// bridge and the modules' imports reach it by name; its engine dir holding
// neither ADTASK.DLL nor AFTERDAR.SCR, the bridge gets After Dark 2.0's
// computed palettes at the first request (Collage asks for 12, the grey
// ramp, at INITIALIZE).
//
// Intermission (IMX) (imx_protocol.cc, imreader.hh): Star Wars Screen
// Entertainment's 14 modules are Intermission .IMX DLLs (SAVERINIT,
// SAVERDRAW, SAVERDLGPROC, SAVERDLGPROC2), which Delrina's INTERMIS.EXE drove
// through a reader, IMIMXPLY.IMQ, with SAVERMAIN(info, msg) and an IMINFO
// record. INTERMIS.EXE is an NE application (the runtime runs libraries
// only), so the lane replaces it and INTRMLIB's LOADSAVER/FREESAVER, as it
// replaces AFTERDAR.SCR; the reader — the real IMIMXPLY.IMQ, or the native
// reader that does what it does (ADNE16READER) —, INTRMLIB, ANTSW, SWSE,
// READJPG, STRESS, SWSFX, MEMMIDI and the module run as real code. The Far
// Side Screen Saver Collection's and Dilbert's modules are the reader's two
// other forms (package.hh "Form"): .ASA animations, data that Intermission's
// ASA reader IMASAPLY.IMQ plays (with ANTSW's sprites and palettes; no
// native reader), and .IMQ modules that export SAVERMAIN themselves, each
// its own reader, loaded as INTRMLIB loaded a reader's record (index −1, no
// path for LOAD and QUERY) and refused when that QUERY leaves the saver flag
// 0x1000 clear (a pure reader such as IMIMXPLY.IMQ). Everything below holds
// for the three forms alike.
//
//   init:  the record (0x67 bytes, GMEM_MOVEABLE|GMEM_ZEROINIT, with
//          INTRMLIB's flags for an enabled module, 0x120C; +0x44 the file
//          name, +0x63 "C:\SAVER\<FILE>" (0, and +0x59 = −1, for an IMQ
//          module), +4 the saver window, +0x0A the reader); SAVERMAIN(10)
//          (LOAD: the reader loads the module and finds its exports, or
//          opens the animation), SAVERMAIN(7) (QUERY: its name, the palette
//          type w); when w != 0 (none of the 14) the engine palette:
//          INTERMIS took INTRMLIB's IMCOPYPALETTE(w), a copy of the palette
//          INTRMLIB's CANISTART(1) had made at start-up from its resource
//          "CLUT" (w 1, or anything but 2 and 3), "HSV" (2) or "PRIM" (3); the
//          lane runs no CANISTART (the rest of it is INTERMIS's: ANTSWINIT,
//          the wake hook), so it makes that palette itself, as CANISTART did
//          (FindResource … CreatePalette through the thunks).
//   step:  each call of the frame's run is one pass of INTERMIS's idle loop:
//          GetDC(saver window), SaveDC, [the engine palette selected], +6 =
//          the DC, +8 = the palette, SAVERMAIN(1) on the first pass (START:
//          the modules load their pictures and sounds there behind a title
//          card, a long call) and SAVERMAIN(0) (DRAW) on every other,
//          RestoreDC(-1), ReleaseDC — all through the thunks —; then
//          INTERMIS's message loop between passes (win16
//          user16_dispatch_guest: the guest's own posted messages and due
//          timers). INTERMIS called the saver back to back while idle, as
//          AFTERDAR.SCR did, so Pacing and Long calls apply, with a pixel
//          cost of Intermission's own and overruns carried (Pacing). How fast
//          a module goes is what it reads: BATTLES (a step every 54 ms at
//          most), JAWAS, SABRDUEL, VADER and HYPERSPC's scenes pace
//          themselves by GetTickCount; the text modules (BIOS, BLUPRINT,
//          CANTINA, POSTERS, STORYBRD) type and wipe for seconds inside one
//          call, busy-waiting on it (long calls); ICLOCK and RCLOCK redraw
//          from the DOS time; but SWTEXT's scroll, TRENCH and HYPERSPC's
//          stars step once per pass, so their speed is the lane's pass rate:
//          the 25-MIPS budget's at a pixel cost of 4, with the overruns
//          carried — SWTEXT's pass costs 3.13 frame budgets (19 passes a
//          virtual second), TRENCH's 3.77 (16), and HYPERSPC's star passes,
//          which make API calls rather than blits, about 240 a second. A pass
//          that draws the whole screen delays what a module times from its
//          end by the frames that pay it back (BLUPRINT's blueprints: 4.1
//          budgets, three frames without a call, about 1% of its cycle).
//   SET:   ignored (logged once): no controls; SWSE.INI holds the settings.
//   close: the stop pass (the bracket around SAVERMAIN(2), sent once START
//          has returned — even when its pass was abandoned in the bracket's
//          own RestoreDC, as INTERMIS cleared [0x2c0] before it), SAVERMAIN(11)
//          (FREE: the reader frees the module), the path block, the IMQ, the
//          engine palette, the record.
// Never interactive: INTERMIS gave input only to a saver flagged 0x2000,
// which IMIMXPLY never sets. Three IMQ modules set it in their QUERY (The Far
// Side's PTERY, Dilbert's DB-BEST and DIL-WHAK): for them INTERMIS captured
// the mouse and showed its own cursor (1:074d..1:076a), INTRMLIB's message
// hook re-posted input to the saver window instead of noting it as activity
// (SETEATMSGS(1), 6:0587), and a focus loss sent SAVERMAIN(12) rather than
// ending the blank. Here they run as screen savers under the saver's own wake
// rules like every Intermission module (the flag is traced, ADTRACE=lane):
// input ends them, and what they show is what they draw without it (DIL-WHAK,
// after its desk scene, a small figure walking on black). Star Wars' modules
// poll instead, to drop what they are loading when the user comes back:
// SWSE's USERABORT, under Intermission's
// default Wakeup Options "mbk", checks GetCursorPos against the position at
// START, the mouse buttons' GetAsyncKeyState, and GetInputState (any key or
// button message in the saver window's queue); SWSE's FORCETOWAKE then posts
// fake input to its own task (PostAppMessage): the pump takes and counts it
// (ADTRACE=lane), and it is not a wake — the saver's uniform wake rules
// decide (INTERACTION.md). Intermission woke on every key but Ctrl, so a key
// in the queue always came with the module's stop; the saver lets keys
// through that it does not wake on (Shift, Ctrl, Caps Lock, Num Lock, and
// every key-up), and one of them in the queue during START made USERABORT
// drop the loading with nothing stopping the module, which then sat on its
// title card until the saver ended. So the IMX protocol takes no key
// messages (Protocol16::takes_key_messages): a KEY line reaches the host's
// key state alone (GetAsyncKeyState, GetKeyState: VADER polls Shift), never
// the queue or a WH_KEYBOARD hook, and a key the saver does not wake on
// changes nothing. MOUSE lines go in as for AD3 (a button wakes the saver
// anyway). The guest's disk (protocol.hh mount_imx_disk):
// C:\SAVER (the module dir, the current directory), C:\WINDOWS over the
// package's WINDOWS folder (SWSE.INI), C:\WINDOWS\SYSTEM (the engine dir,
// IMIMXPLY.IMQ's), H:; the profile seeds (win16 seed_intermission): SWSE.INI
// [technology] GDI when the module dir holds SWSE.DLL (SWSE never tries
// WinG), ANTSW.INI [Intermission] Volume = the engine's volume with sound on
// and 0 ("Off": no effects, no music) without, Saver Path = C:\SAVER. The
// display starts with the desktop's palette (SWSE builds identity palettes
// from it). Configure: button 0 = SAVERMAIN(10), (7), +4 = the --owner,
// (8) — IMIMXPLY's DialogBox(hLib, "DIALOGBOX", owner, SAVERDLGPROC) —,
// (11): exit 0 when it showed, 4 for a module without SAVERDLGPROC; any
// other button fails. What the dialog writes lands in
// <state>\<package>\WINDOWS\SWSE.INI (Star Wars), or ANTSW.INI there, in a
// section named for the module (The Far Side's and Dilbert's: IMASAPLY's
// "Animation Player Options" for an ASA animation, an IMQ module's own
// dialog, such as PTERY's banner text).
//
// Input and status (INTERACTION.md §5.2): AFTERDAR.SCR forwarded no key or
// mouse message to a Classic module (ABI.md §3.1); modules poll
// GetAsyncKeyState/GetCursorPos (the host's input state, VK_RBUTTON/VK_MBUTTON
// from the MOUSE bitmask, the mouse scaled to a small screen's guest display),
// install a WH_KEYBOARD hook, or read the blanker window's queue. So each
// KEY line runs the guest's WH_KEYBOARD chain (a non-zero result consumes it)
// and is otherwise posted to the saver window as WM_KEYDOWN/WM_KEYUP — for a
// protocol that takes key messages (Protocol16::takes_key_messages: AD3's;
// the IMX protocol's KEY lines reach the key state alone, see Intermission
// (IMX)) —, MOUSE lines as WM_MOUSEMOVE and button messages, tagged with the
// line's number (win16/input16.hh). They go in at the next point the guest
// can be called: before the frame's first DRAWFRAME, or — the frame resuming
// a long call — inside the API call where it resumes (LUNATIC's game loop is
// one long DRAWFRAME that reads the queue every frame). After the step, what
// the guest removed and did not dispatch back is consumed; what nobody took
// is dropped (kept while a DRAWFRAME that may still take it is suspended:
// one more step, or until it does when that call reads the saver window's
// queue, at most kReaderKeepSteps; reported as unsettled meanwhile). CAPS and
// NUMLOCK lines are toggle states only (GetKeyState's bit 0 for VK_CAPITAL
// and VK_NUMLOCK); the KEY 20 / KEY 144 line before each is the key. status():
// interactive = the 0x0E toggle (source 2), cursor = 0x11 until 0x12 (the
// results of the protocol's calls, Protocol16::Call: AD3's),
// key-filter = a WH_KEYBOARD hook is in, or the guest read the saver
// window's queue with removal for keys within the last 120 steps, wake = the
// guest posted WM_CLOSE/SC_CLOSE to it, or the module woke the saver itself
// (Protocol16::Call::Kind::wake: an After Dark 2.0 module's 5), eaten = the
// highest line consumed (every line while interactive). A module that woke
// the saver is called no more: the frame of its wake is presented with the
// wake in its status (the saver then ends as when the user wakes it); a
// headless run ends at the next step (exit 0, "module finished", after the
// log line "the module woke the saver (result 5)"); streamed, the frames
// repeat its last picture, input and SET going nowhere, until the front end
// ends the run. Measured: the Caps Lock games (YBYH, SIMPTRIV, tt FRANKEN, tt
// MIMEHUNT, HOW2DRAW) hook while they play; the ADXPL40/ADXPL310 engines hook
// nothing at load; without input only LUNATIC raises key-filter. Final Exam
// (After Dark 2.0) latches Num Lock's toggle as it starts (ADNUMLOCK) and
// starts its exam when it changes (NUMLOCK): it hooks the keyboard for the
// answers (1-4, the keypad's too; its hook records them and passes them on),
// goes interactive (0x0E), and a mouse move ends the exam (0x0E again, then
// 5: the wake).
//
// Disk (the AD3 protocol's mount_disk, protocol.hh; INTERACTION.md §7):
// C:\WINDOWS and C:\AFTERDRK (+ C:\AFTERD~1) are copy-on-write overlays whose
// upper layers are the package's state directories under ADSTATE
// (<state>\<package>\WINDOWS and \<MODDIR>) or, without ADSTATE, memory:
// headless runs never read or write user state. C:\WINDOWS's lower layer is
// the package's WINDOWS folder when it has one (package.hh), for either
// protocol. C:\WINDOWS\SYSTEM is the engine dir; H: the host's drives (8.3).
// For After Dark 2.0, AD_PREFS.INI's profile seeds (After Dark 2.0 above),
// read under the file and never written out: the modules' own keys (AD_SND
// 1.0's [Sound] Mute, written at each adwSetSoundMute, which the bridge calls
// once, at load (a run's volume and mute are fixed at spawn by ADVOLUME and
// ADSOUND; SET lines carry only the four control values), for a module that
// wants sound — every one but Ion Storm —; Communications' [Communications]
// MessageText; Sounder's [Sounder] SoundPath) land in the upper layer. For
// After Dark 3.x (package.hh after_dark3_host: the engine dir holds
// ADW30.EXE), the keys the host the lane stands in for wrote into
// AD_PREFS.INI at every start, seeded the same way (win16/dos16.hh
// seed_after_dark3): [After Dark] Path=C:\AFTERDRK, where ADXPL100 (the
// Disney Collection) finds DIS_SND.DLL and MUSIC\, and [Sound]
// SoundDriver=AD_MME.DRV.
//
// Configure (INTERACTION.md §6.1, configure()): `adhostwin --configure`
// runs the protocol's button (Protocol16::button) with the module's dialogs,
// message boxes and file dialogs real (win16/dialogs16.hh), owned by --owner,
// and the ADCONFIG* hooks (win32/config_script.hh). AD3's loads the bridge
// but no module and runs BUTTONPUSHED16(path, owner16, slot, ctrl4, err, 260,
// &errId): the real OLDMOD16's, or the native bridge's same sequence
// (bridge.hh button()). The wall clock runs (timers tick), no call budget.
// Exit: 0 when something was shown, 4 when nothing was, 1 on a failure (a
// guest fault, a dialog that could not be shown, a scripted dialog that timed
// out, AD_SND missing).
//
// Timing: 60 frames per second of virtual time (ADPACEMS overrides); the
// Win16 GetTickCount advances in 55 ms steps as on Windows 95, and every clock
// read nudges virtual time by ADREADSTEPUS (and the instructions run since
// the last read by ADMIPS) so busy-waits and the AD_RSRC /
// EINSTEIN calibration loops end (ABI.md §4). Streamed, the clock follows the
// wall from frame 0; before it (LOADADMODULE16: EINSTEIN, GLOBE and Om
// Appliances calibrate there) time is modeled as headless, then the wall clock
// carries on from there (Runtime16::start_frames). A frame's work is charged
// inside its own period when it ends (Runtime16::settle_time).
//
// Pacing: AFTERDAR.SCR sent DRAWFRAME once per pass of its idle loop, as fast
// as the machine allowed, and modules were written for that: most pace
// themselves by the clock, but some count calls (ad32 LOGO moves its picture
// every 700th/300th/100th call at Slowest/Slow/Medium, every call at Fast; 14
// distinct Classic binaries read no clock while drawing). So one presented
// frame is a run of DRAWFRAMEs: as many as fit into ADDRAWMIPS (default 25)
// million instruction-equivalents per second of the frame period —
// instructions executed, ADAPICOST per API call and the protocol's pixel
// cost (below) per pixel a blit or fill writes, deterministic — at most
// ADMAXDRAWS. 25 is a 486-class machine, the one the AD 2/3 modules were
// written on, and what the emulator sustains in real time next to the API
// work with room to spare (~120 MIPS on 16-bit code since its fetch and data
// windows, cpu/README.md; ~70 before); a 100-MIPS budget kept busy would
// still cost about a frame of host time. That work happens inside the period (Runtime16's
// frame-bounded time, runtime16.hh): clock-paced modules keep real time, only
// a call that overruns the period pushes the clock on. ADMIPS=0 restores one
// DRAWFRAME per frame and the accumulating nudges.
//
// Pixel cost (Protocol16::pixel_cost): what a pixel a GDI blit or fill writes
// costs the budget — work, never time. An After Dark module's is ADPIXCOST,
// default 2 (12.5 Mpixel/s against the 25-MIPS budget); an Intermission
// module's is ADNE16IMXPIXCOST, default 4 (6.25 Mpixel/s). Each protocol reads
// its own knob alone: ADPIXCOST never changes an Intermission module's cost
// (the lane logs that it is ignored), nor ADNE16IMXPIXCOST an After Dark
// module's, so the AD 2/3 streams stay as they were. Why 4, a model choice
// rather than a measurement: SWSE draws its scenes as full-screen DIB
// stretches through GDI (StretchDIBits from DIB.DRV canvases; the host forces
// that technology, WinG not being emulated), the slow path WinG was made to
// avoid, which on the ISA and VL-bus 486s of 1994 ran several times slower
// than After Dark's figure; half of it puts the modules that step once per
// pass at 16 (TRENCH) and 19 (SWTEXT) passes a virtual second instead of 31
// and 35. (A second of 60 presented frames: a budget, and so the carry, is
// a frame's (ADDRAWMIPS × the frame period), and a streamed run is stepped
// once per GO, so passes follow the frames the front end presents — the
// saver's 30-fps preview shows about half as many a wall second, a display
// presenting faster more. So they did before the carry, at one a frame.)
//
// Carried overruns (a protocol that carries them, Protocol16::
// carries_overruns: IMX's; AD3 carries nothing, one DRAWFRAME at least per
// frame, and its streams stay as they were): a call that completes within
// its frame but does more work than the frame's budget had left took that
// long on the modeled machine, so the excess is owed — the next frames'
// budgets pay it back first, and a frame whose whole budget goes to it makes
// no call (it still delivers input, pumps audio and runs the protocol's
// message loop). A module that steps once per call and costs more than a
// budget otherwise ran at 60 steps a second whatever its cost (SWTEXT's
// crawl went by in 3.6 s); with the carry it runs at the model's rate. A
// call that runs past its frame's deadline is paced by the deadline instead
// (Long calls), as START's loading (every module's but SWTEXT's, whose START
// completes inside frame 0) and ICLOCK's multi-frame redraws are, and carries
// nothing: neither the frames that end inside it nor the one it returns in
// owe anything for it. The calls that frame makes after it returns are
// carried as any frame's (SWTEXT's second DRAW, in the frame where its long
// first one returns, owes 2.4 budgets). What is owed stays below six budgets
// (Ne16Lane::kMaxOwedBudgets), and a frame without a call pays a whole one
// back, so at most five frames in a row make none (83 ms): a pass of up to
// six budgets runs at the model's rate (TRENCH's 3.77: three frames without a
// call between its passes), and since the budget's work is not the clock the
// modules read (pixels cost work, not time), no single enormous call makes a
// longer pause, which clock-paced content would jump after. Six keeps
// TRENCH's passes and SWTEXT's scroll passes below the bound at every pixel
// cost from 3 to 6 (TRENCH's 2.8, 3.77, 4.7 and 5.6 budgets, SWTEXT's 2.4,
// 3.13, 3.8 and 4.6), so ADNE16IMXPIXCOST slows them over that whole range.
// Two of SWTEXT's passes can owe more: its START (5.7, 7.3, 8.9 and 10.6
// budgets) and its once-a-loop pass, where the crawl starts over (3.8, 4.9,
// 6.1 and 7.3). The bound drops what they owe beyond it, and that is all it
// costs: those passes are paced that much short of the model (START by 0.3
// budget at the default cost, 1.9 at 5 and 3.6 at 6, once; the loop pass,
// clamped at 6 only, by 0.84 budget the first loop and 0.55 each loop after,
// as much as the allowance left where it starts decides). Deterministic
// (work counts are).
// ADNE16IMXCARRY=0 turns it off.
//
// Long calls: some DRAWFRAMEs draw for much longer than a frame — SATORI
// about a second (it waits on the tick count, redrawing all the while),
// EINSTEIN, Fractal Forest, Om Appliances, Tunnel and Modern Art up to
// seconds, most modules' first call while it paints its scene. A 1996
// monitor showed that drawing as it happened. So the run of DRAWFRAMEs
// happens on a fiber of its own, and at the first API call (or retrace-port
// read) past the frame's deadline — the next line of the frame grid headless,
// 90% of a period of wall time streamed — it switches back: the frame is
// presented there, and the next step() resumes the call where it stopped.
// Deterministic (the deadline is virtual time); calls shorter than a frame
// are untouched. A SET that arrives mid-call reaches the module before its
// next DRAWFRAME; at close a suspended call is abandoned (the deadline hook
// throws and every call level restores the machine) before UNLOAD. Without it,
// SATORI was a 60x time-lapse headless and 1 fps streamed. ADNE16LONGCALLS=0
// (or ADMIPS=0) turns it off.
//
// Frames: the screen as the run of DRAWFRAMEs leaves it — except that a run
// which ends exactly where it began, but during which a (virtual 70 Hz) refresh
// showed something else, presents that refresh: content drawn, held by a CPU
// delay loop and erased inside one call (ZOT's lightning) is what a 1996
// monitor showed for a refresh, and would otherwise never reach a frame
// (lane.cc on_scanout).
//
// Small screens: the modules were written for a 640x480 (or larger) Win95
// display, and below it several refuse to load ("A larger screen size is
// needed…": Rat Race and You Bet Your Head below 512x384, Daredevil Dan at
// 320x240) or draw nothing or a corner of their picture (Lunatic Fringe,
// Lisa's Mood Swings, FrankenScreen). So an output smaller than 640x480 in
// either dimension (the saver's 320x240 /p preview) gets a guest display k
// times its size, the smallest whole k that reaches 640x480, and every
// presented frame is that display averaged down k×k → 1 and matched to the
// nearest hardware palette entry: a miniature of the full-screen saver. The
// mouse the guest sees is scaled up to match. Deterministic; outputs of at
// least 640x480 are untouched. ADNE16SCALE=<k> forces k (1 = off).
//
// Sound (AUDIO.md §8, win16/sound16.hh): with the host audio engine on
// (LaneContext::audio enabled: ADSOUND=1 or ADAUDIOOUT) the bridge loads the
// module unmuted at After Dark's volume slider (ADVOLUME through the engine's
// config), and MMSYSTEM plays: AD_SND's sndPlaySound images (PCM, MS-ADPCM)
// on the wave bus, the engines' MCI sequencer songs on the MIDI bus (After
// Dark 2.0's AD_SND 1.0 plays through the plug-in driver its AD_PREFS.INI
// names, the seeded AD_MME.DRV: sndPlaySound, and waveOutSetVolume for the
// volume; the host's own AD_SND, for a package without one, makes the calls
// AD_SND 3.x makes: sndPlaySound, waveOutSetVolume and midiOutSetVolume). The
// engines' music gates (§2.9) pass: one MIDI output device, TOOLHELP (a
// system module) and a stub MCISEQ.DRV; their hidden adwMidiCall window gets
// MM_MCINOTIFY at a song's end. Callbacks reach the guest at the first API
// call at or after their virtual time and at this lane's pump before every
// DRAWFRAME (the host's message loop ran between DRAWFRAMEs), which
// dispatches the notify messages the guest has not taken itself. A
// synchronous sndPlaySound (NOCTURNE) lasts its sound's duration of virtual
// time: with long calls on, the frames go on meanwhile as in a long call
// (the 1996 screen froze while the call blocked); otherwise the time is
// charged at once. Without the engine (or with it disabled) MMSYSTEM is the
// silent device of before, byte for byte, and FBHASH streams do not move.
// Every audio call carries Runtime16::peek_us() as its time — one that a
// CALLBACK_FUNCTION or timer procedure makes carries its event's due time
// instead (win16/sound16.hh: MEMMIDI's notes keep its 4 ms grid although the
// periods reach the guest at the next API call or pump) —, and after every
// step the lane advances the engine to peek_us(), but never past
// Runtime16::audio_due(): guest time runs ahead of the core clock run_host
// advances with (a frame's work headless, the modeled init time for good
// when streamed), so live output is rendered continuously; and a frame can
// end with timer periods due that only the next frame delivers (inside a long
// call, or after a call that made no API call), whose notes, dated before
// the engine's time, would otherwise sound at it, bunched. run_host leaves
// the end of such a step to the lane (host.cc LaneEngine: after a step in
// which the lane advanced the engine it advances nothing itself), so not even
// a frame with no delivery point at all — resumed inside a synchronous
// sndPlaySound, or in a retrace-port loop — renders past one. An Intermission
// module gets the same volume through ANTSW.INI (see Intermission (IMX)) for
// its effects, sndPlaySound images; its music is MEMMIDI's (midiOut*,
// timeSetEvent), whose level the IMX protocol sets on the engine's MIDI bus
// from that volume at load (linear, 50 = half amplitude, as After Dark's
// reaches the MIDI bus through AD_SND): Intermission set the effects' level
// alone, and the Windows mixer's synth line did the rest.
//
// Lane knobs (env, all optional):
//   ADNE16KIND=auto|ad3|imx  the module's protocol (default auto: its exports; see Module protocols)
//   ADNE16READER=auto|imq|native  the IMX reader (default auto: IMIMXPLY.IMQ from the engine dir, else from the
//                      module dir, else the native reader)
//   ADNE16BRIDGE=auto|oldmod16|native  the AD3 bridge (default auto: OLDMOD16 when the engine dir has it;
//                      ignored, and said so, for an IMX module)
//   ADNE16SCALE=auto|<k>  the guest display is k times the output (default auto; see Small screens)
//   ADNE16LONGCALLS=0  every DRAWFRAME ends its frame, however long (default on; see Long calls)
//   ADNE16IMXCARRY=0   an Intermission module's overruns are not carried: a call every frame (default on;
//                      see Pacing)
//   ADDESKTOPPAL=0|1   the display's starting palette, over the protocol's choice: 1 = a desktop's 236
//                      distinct colours between the statics (AD3's for the AD 3 packages, which the
//                      native bridge runs: ADXPL310's identity palette needs it), 0 = black between
//                      them (AD3's for the rest)
//   ADSOUND=1          sound on (with run_host's audio engine: see Sound); without an engine,
//                      unmuted on the silent device
//   ADSOUNDDEV=0       no wave device at all, sound on or off (AD_SND then refuses sounds; several
//                      modules stop); MIDI and aux devices are unaffected
//   ADVOLUME=<0..100>  the volume handed to the bridge (default 50, the registry default); with the
//                      engine on it comes from the engine's config (the same knob, clamped)
//   ADSEEDIMG=<spec>   what the screen holds before the module loads (win32/display.hh "desktop
//                      seed", as in the pe32 lane): ":win95" (teal) or a raw/P6/BMP file of any size.
//                      Unset = black. Modules with Clear Screen First off (the default for Bugs,
//                      Mowin' Man, Rebound, Mr. Burns, Objets B'art, …) and the screen transformers
//                      (Puzzle, Punch Out, Spotlight, Down the Drain) work on it. ADNOSEED=1 ignores it.
//   ADMAXDRAWS=<n>     DRAWFRAME calls per presented frame, at most (default 64; see Pacing)
//   ADDRAWMIPS=<n>     the DRAWFRAME budget, million instruction-equivalents per second (default 25,
//                      at most ADMIPS)
//   ADPIXCOST=<n>      instruction-equivalents per pixel a GDI blit/fill writes, for that budget
//                      only: an After Dark module's (default 2); an Intermission module ignores it
//   ADNE16IMXPIXCOST=<n>  the same for an Intermission module (default 4; see Pacing); an After Dark
//                      module ignores it
//   ADREADSTEPUS=<us>  virtual µs per clock read (default 5)
//   ADMIPS=<n>         the virtual CPU: instructions per virtual µs, for clock reads and the
//                      DRAWFRAME budget (default 100; 0 = one DRAWFRAME per frame, no instruction time)
//   ADAPICOST=<n>      instructions one API call counts as there (default 500 = 5 µs)
//   ADTICKMS=<ms>      Win16 GetTickCount granularity (default 55)
//   ADCALLBUDGET=<n>   instructions one call into the guest may run (default 1e9)
//   ADHEAPMB=<n>       the Win16 arena (default 64)
// ADTRACE=lane logs, at init, the package (or "legacy"), module dir, engine
// dir and, from the AD3 protocol, the bridge, AD_SND ("the host's (no …)"
// when the host's answers) and palette source ("After Dark 2.0's four,
// computed …" when there is no file to read them from);
// from the IMX protocol the windows dir, the reader, the module's name,
// palette type and flags, the disk and seeds, the pixel cost, whether
// overruns are carried (and the bound), and the passes at close.
// ADTRACE=pace logs each frame's DRAWFRAMEs and work (and, carrying
// overruns, what it paid back and what it owes).
#pragma once

#include <windows.h>

#include <array>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "adw/core/lane.h"
#include "ne16/package.hh"
#include "ne16/protocol.hh"

namespace adw::win16 {
class Runtime16;
struct Module16;
}  // namespace adw::win16

namespace adw::ne16 {

class Ne16Lane : public Lane {
 public:
  // Makes the module's protocol (protocol.hh), first thing once the layout is
  // known. Null refuses the module, *why saying why: the lane logs it and
  // fails (exit 1) before anything is loaded.
  using ProtocolFactory =
      std::function<std::unique_ptr<Protocol16>(const Ne16Layout& layout, const Env& env, std::string* why)>;
  Ne16Lane();  // choose_protocol: the protocol the module's exports (or ADNE16KIND) call for
  explicit Ne16Lane(ProtocolFactory make_protocol);  // tests: a protocol of their own
  ~Ne16Lane() override;

  // The default factory (Module protocols): ADNE16KIND when set (logged at
  // ADTRACE=lane), else detect_kind over the module's NE image — AD3 for
  // MODULE, IMX for SAVERINIT + SAVERDRAW, null (refused) for a reader or no
  // module at all; a file that is no readable NE image goes to the AD3
  // protocol, which reports it as it always did.
  static std::unique_ptr<Protocol16> choose_protocol(const Ne16Layout& layout, const Env& env, std::string* why);

  const char* name() const override { return "ne16"; }
  std::vector<std::string> abis() const override { return {"afterdark", "intermission"}; }
  bool init(const std::string& module_path, LaneContext& ctx) override;
  uint32_t frame_interval_us() const override { return 16667; }
  void on_command(const Command& c) override;
  StepResult step() override;
  void shutdown() override;
  LaneStatus status() const override;
  bool can_configure() const override { return true; }
  ConfigureResult configure(const std::string& module_path, LaneContext& ctx, const ConfigureRequest& req,
                            std::string* json_out) override;

  // For tests and diagnostics.
  win16::Runtime16* runtime() { return rt_.get(); }
  uint64_t frames() const { return frames_; }
  bool wants_events() const { return wants_events_; }
  const Ne16Layout& layout() const { return layout_; }
  const Protocol16* protocol() const { return proto_.get(); }
  int guest_scale() const { return guest_scale_; }

  // The whole-number factor between the guest display and a w×h output (lane.hh
  // "Small screens"): the smallest k with k*w >= 640 and k*h >= 480, at most 8.
  static int auto_guest_scale(int w, int h);

  // Carried overruns (lane.hh "Pacing"): what is owed stays below this many
  // frame budgets, so at most this many minus one frames in a row make no call.
  static constexpr uint64_t kMaxOwedBudgets = 6;

 private:
  bool init_impl(const std::string& module_path, LaneContext& ctx);
  void census();
  void on_scanout();
  void settle_screen();
  void sync_input();
  void present();
  bool draw_run();
  static void CALLBACK fiber_main(void* self);
  void fiber_body();
  void on_deadline();
  bool suspend_frame();
  void abandon_long_call();
  void free_fibers();
  void queue_input(const Command& c);
  void deliver_input();
  void end_step_input();

  // Input (lane.hh "Input and status"): the KEY/MOUSE lines not yet handed
  // to the guest, with what the lane knew before each (previous key state,
  // mouse position and buttons, in guest coordinates).
  struct PendingInput {
    Command::Kind kind = Command::Kind::key;
    uint8_t vk = 0;
    bool down = false, was_down = false, moved = false;
    int32_t x = 0, y = 0;
    uint32_t buttons = 0, prev_buttons = 0;
    uint64_t seq = 0;
  };
  std::vector<PendingInput> pending_input_;
  std::array<bool, 256> key_down_{};
  int32_t mouse_x_ = 0, mouse_y_ = 0;
  uint32_t mouse_buttons_ = 0;
  bool mouse_known_ = false;
  uint64_t eaten_ = 0, queue_reads_ = 0, last_read_frame_ = 0, queued_seq_ = 0;
  uint32_t task_posts_ = 0;  // messages the guest posted to its task (StepReport16), as last traced
  bool read_queue_ = false, hooked_ = false, wake_ = false;
  // The module woke the saver itself (Protocol16::Call::Kind::wake): it is called no more.
  bool woke_ = false;
  // A module that read the saver window's queue within this many steps is a
  // queue reader (key-filter); a suspended call of one keeps untaken input up
  // to kReaderKeepSteps steps (end_step_input).
  static constexpr uint64_t kReaderSteps = 120;
  static constexpr uint32_t kReaderKeepSteps = 600;

  // Long calls (lane.hh): the frame's DRAWFRAME run happens on guest_fiber_;
  // at the frame's deadline inside a call it switches back (suspended_), and
  // the next step() resumes it.
  enum class Run { none, frame_done, suspended, stopped, abandoned, error };
  bool long_calls_ = false;
  void* host_fiber_ = nullptr;
  void* guest_fiber_ = nullptr;
  bool converted_thread_ = false;
  bool mid_call_ = false, suspended_ = false, abandon_ = false, controls_pending_ = false;
  Run run_result_ = Run::none;
  std::exception_ptr fiber_error_;
  uint64_t frame_w0_ = 0, frame_budget_ = 0, frame_deadline_ = 0;
  uint32_t frame_draws_ = 0;
  uint64_t long_frames_ = 0;

  // Small screens: the guest's display (guest_scale_ times the output) when
  // guest_scale_ > 1, the input it sees, and the averaged-colour → hardware
  // index cache (6 bits per channel, valid for one palette).
  int guest_scale_ = 1;
  std::unique_ptr<Screen> guest_screen_;
  InputState guest_input_;
  std::array<RGBQUAD, 256> near_pal_{};
  std::vector<uint32_t> near_cache_;  // (generation << 8) | index
  uint32_t near_gen_ = 0;

  std::unique_ptr<win16::Runtime16> rt_;
  ProtocolFactory make_protocol_;
  std::unique_ptr<Protocol16> proto_;  // the module's protocol (protocol.hh); released before rt_
  LaneContext* ctx_ = nullptr;
  std::string module_name_;
  Ne16Layout layout_;
  uint16_t hwnd_ = 0, hdc_ = 0;  // the saver window and its screen DC
  bool loaded_ = false, wants_events_ = false, cursor_ = false, census_done_ = false;
  uint64_t frames_ = 0;
  // Pacing: DRAWFRAMEs per presented frame until the work (instructions plus
  // ADAPICOST per API call, the protocol's pixel cost per pixel) reaches
  // ADDRAWMIPS (draw_mips_) × the frame period, at most max_draws_.
  uint64_t draw_mips_ = 0, draws_ = 0;
  uint32_t max_draws_ = 1;
  // A protocol that carries overruns (Protocol16::carries_overruns, lane.hh
  // "Pacing"): carry_ when it is on, owed_ the work the frames' completed
  // calls did beyond their budgets and not yet paid back, frame_allow_ the
  // work this frame's run may do (its budget less what it pays back).
  bool carry_ = false;
  uint64_t owed_ = 0, frame_allow_ = 0, idle_frames_ = 0;
  // Transient content (step()): the screen as the step began, the last
  // refresh during the step that showed something else, and whether the
  // presented frame is that refresh (the real screen is start_bits_ then).
  std::vector<uint8_t> start_bits_, latched_bits_;
  bool in_step_ = false, latched_ = false, showing_latched_ = false;
  uint64_t transient_frames_ = 0;
};

}  // namespace adw::ne16
