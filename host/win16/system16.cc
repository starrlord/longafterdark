// The small system DLLs: MMSYSTEM (the multimedia clock here; its sound half
// — sndPlaySound, waveOut, midiOut/aux, mixer, MCI — is sound16.cc), WIN87EM
// (the FP emulator's control entry — the CPU has an x87), COMMDLG / KEYBOARD
// / SHELL / TOOLHELP (API_SURFACE.md §2), WING's one corrected signature, and
// register_all16(). The common dialogs (GetOpenFileName, ChooseFont) answer
// "cancelled" in the saver; configure mode makes them real (dialogs16.cc).
//
// Sound (sound16.hh, AUDIO.md §8): without the host audio engine the lane has
// one wave-out device that plays nothing. AD_SND.DLL (ABI.md §3.6)
// initializes only when waveOutGetNumDevs() finds a device (AD_SND 1:1ea6),
// and without it adwLoadSoundResource returns 0 — which LUNATIC, BORIS,
// DOMINOES and others treat as out of memory — so, like the sound card every
// Win95 machine After Dark shipped for had, the device exists: format queries
// succeed and sndPlaySound reports the sound played. MIDI and aux devices:
// none. With the engine on (ADSOUND=1 / ADAUDIOOUT) the sounds play, and a
// MIDI device, two aux devices and the MCI sequencer exist. ADSOUNDDEV=0
// (the lane's knob) removes the wave device either way, for the no-sound-card
// path.
#include <windows.h>

#include <algorithm>
#include <cstring>

#include "adw/core/log.h"
#include "win16/dos16.hh"
#include "win16/input16.hh"
#include "win16/modules16.hh"
#include "win16/shim_families16.hh"
#include "win16/sound16.hh"

namespace adw::win16 {

void register_system16(Runtime16& rt) {
  Shim16Registry& r = rt.shims();

  // ---- MMSYSTEM ----
  const char* M = "MMSYSTEM";
  r.impl(M, "mmsystemGetVersion", [](Call16& c) { c.ret(0x030A); });
  r.impl(M, "timeGetTime", [](Call16& c) { c.ret32(c.rt.time_ms()); });
  // Multimedia timer events, engine or no engine (sound16.hh timer16_set):
  // Windows 95's timer services, 1 ms to 65535 ms (the 16-bit TIMECAPS; Wine's
  // winmm, MMSYSTIME_MININTERVAL/MAXINTERVAL), a callback per period at the
  // first delivery point at or after it. MEMMIDI sequences SWSE's songs from
  // one: timeBeginPeriod(4), timeSetEvent(4, 4, MIDITIMERPROC, song,
  // TIME_PERIODIC) (1:021a, 1:0236), timeKillEvent + timeEndPeriod at the end.
  constexpr uint16_t kTimerrNoCanDo = 97, kTimerrStruct = 129;  // TIMERR_NOCANDO, TIMERR_STRUCT
  constexpr uint16_t kTimePeriodic = 0x0001;                    // fuEvent: TIME_ONESHOT 0, TIME_PERIODIC 1
  // timeGetDevCaps(lpTimeCaps, wSize): TIMECAPS {wPeriodMin, wPeriodMax}.
  r.impl(M, "timeGetDevCaps", [](Call16& c) {
    uint32_t p = c.ptr();
    uint16_t size = c.w();
    if (!p || size < 4) return c.ret(kTimerrStruct);
    c.rt.wr16(p, 1);
    c.rt.wr16(p + 2, 0xFFFF);
    c.ret(0);
  });
  // The resolution only sets how closely a real timer kept to its periods; the
  // delivery points here are what they are. Out of range: TIMERR_NOCANDO.
  for (const char* n : {"timeBeginPeriod", "timeEndPeriod"}) {
    r.impl(M, n, [](Call16& c) { c.ret(c.w() == 0 ? kTimerrNoCanDo : 0); });
  }
  // timeSetEvent(wDelay, wResolution, lpFunction, dwUser, wFlags): the event's id, 0 on failure.
  r.impl(M, "timeSetEvent", [](Call16& c) {
    uint16_t delay = c.w();
    c.w();  // wResolution
    uint32_t proc = c.ptr(), user = c.l();
    uint16_t flags = c.w();
    Module16* m = c.rt.modules().containing(uint16_t(proc >> 16));
    uint16_t ds = m && m->dgroup ? m->dgroup : caller_ds(c);
    c.ret(timer16_set(c.rt, delay, proc, user, (flags & kTimePeriodic) != 0, ds));
  });
  r.impl(M, "timeKillEvent", [](Call16& c) { c.ret(timer16_kill(c.rt, c.w()) ? 0 : kTimerrNoCanDo); });
  // The sound half: sound16.cc (sndPlaySound, waveOut, midiOut, aux, mixer, MCI).
  register_sound16(rt);

  // ---- WIN87EM: __fpMath (BX = function) ----
  // With OSFIXUPs left as real x87 opcodes nothing calls the emulator's
  // arithmetic; the C runtimes still use this entry to set up and query it.
  r.impl("WIN87EM", "__fpMath", [](Call16& c) {
    auto& rr = c.rt.cpu().registers();
    auto& fpu = c.rt.cpu().fpu();
    switch (rr.r_bx()) {
      case 0:  // install
      case 1:  // initialize
        fpu.reset();
        rr.w_ax(0);
        break;
      case 2:  // deinstall
      case 3:  // set error handler (DX:AX)
      case 10:  // stack depth
        rr.w_ax(0);
        break;
      case 4:  // set control word
        fpu.cw = rr.r_ax();
        break;
      case 5:  // get control word
        rr.w_ax(fpu.cw);
        break;
      case 8: {  // get status word, clear exceptions
        rr.w_ax(fpu.status_word());
        fpu.sw &= uint16_t(~0x80FF);
        break;
      }
      case 11:  // installed?
        rr.w_dx(0);
        rr.w_ax(1);
        break;
      default:
        log("win16: WIN87EM.__fpMath function %u not supported", rr.r_bx());
        rr.w_ax(0);
        break;
    }
  });

  // ---- COMMDLG, KEYBOARD, SHELL ----
  // The common dialogs answer "cancelled" in the saver; configure mode makes
  // them real (dialogs16.cc).
  r.impl("COMMDLG", "GetOpenFileName", [](Call16& c) { c.ret(0); });
  r.impl("COMMDLG", "ChooseFont", [](Call16& c) {
    c.ptr();
    c.ret(0);
  });
  r.impl("COMMDLG", "CommDlgExtendedError", [](Call16& c) { c.ret32(0); });
  r.impl("COMMDLG", "GetFileTitle", [](Call16& c) {
    std::string path = c.rt.read_str(c.ptr());
    uint32_t buf = c.ptr();
    uint16_t n = c.w();
    size_t s = path.find_last_of("\\/:");
    std::string title = s == std::string::npos ? path : path.substr(s + 1);
    if (n <= title.size()) return c.ret(uint16_t(title.size() + 1));
    c.rt.write_str(buf, title, n);
    c.ret(0);
  });
  // The US keyboard (input16.hh), never the host's layout.
  r.impl("KEYBOARD", "MapVirtualKey", [](Call16& c) {
    uint16_t code = c.w(), type = c.w();
    switch (type) {
      case 0:  // virtual key → scan code
        return c.ret(vk_scan_code(uint8_t(code)));
      case 1:  // scan code → virtual key
        for (int vk = 1; vk < 256; vk++) {
          if (vk_scan_code(uint8_t(vk)) == code && !vk_extended(uint8_t(vk))) return c.ret(uint16_t(vk));
        }
        return c.ret(0);
      case 2: {  // virtual key → unshifted character
        int ch = vk_to_char(uint8_t(code), false, false);
        return c.ret(ch < 0 ? 0 : uint16_t(ch >= 'a' && ch <= 'z' ? ch - 32 : ch));
      }
      default:
        return c.ret(0);
    }
  });
  r.impl("KEYBOARD", "VkKeyScan", [](Call16& c) {
    uint8_t ch = uint8_t(c.w());
    for (int shift = 0; shift < 2; shift++) {
      for (int vk = 1; vk < 256; vk++) {
        if (vk >= VK_NUMPAD0 && vk <= VK_DIVIDE) continue;  // the main block's key first
        if (vk_to_char(uint8_t(vk), shift != 0, false) == ch) return c.ret(uint16_t(vk | (shift << 8)));
      }
    }
    c.ret(0xFFFF);
  });
  r.impl("KEYBOARD", "OemToAnsi", [](Call16& c) {
    uint32_t src = c.ptr(), dst = c.ptr();
    std::string s = c.rt.read_str(src);
    c.rt.write_bytes(dst, s.c_str(), s.size() + 1);
    c.ret(1);
  });
  // ---- TOOLHELP ----
  // GlobalEntryModule(lpGlobal, hModule, wSeg): ADTOOL's BADTOOLSTUFF /
  // BADTOOLUNSTUFF walk segments 1..99 of a module and save/restore the
  // GT_DATA ones (the module's non-automatic data segments) by
  // dwAddress/dwBlockSize/hBlock.
  r.impl("TOOLHELP", "GlobalEntryModule", [](Call16& c) {
    uint32_t ge = c.ptr();
    uint16_t hmod = c.w(), seg = c.w();
    Module16* m = c.rt.modules().by_handle(hmod);
    if (!ge || !m || m->system || c.rt.rd32(ge) < 36 || seg == 0 || seg > m->seg_sel.size()) return c.ret(0);
    GlobalBlock* b = c.rt.global().find(m->seg_sel[seg - 1]);
    if (!b) return c.ret(0);
    bool is_data = m->image->segment(seg).is_data();
    uint16_t type = m->seg_sel[seg - 1] == m->dgroup ? 1 /*GT_DGROUP*/ : is_data ? 2 /*GT_DATA*/ : 3 /*GT_CODE*/;
    c.rt.wr32(ge + 4, b->base);
    c.rt.wr32(ge + 8, b->size);
    c.rt.wr16(ge + 12, b->handle());
    c.rt.wr16(ge + 14, b->locks);
    c.rt.wr16(ge + 16, 0);
    c.rt.wr16(ge + 18, 0);
    c.rt.wr16(ge + 20, uint16_t(type == 1 && c.rt.local().has_heap(b->sel)));
    c.rt.wr16(ge + 22, m->hmodule);
    c.rt.wr16(ge + 24, type);
    c.rt.wr16(ge + 26, seg);
    c.rt.wr32(ge + 28, 0);
    c.rt.wr32(ge + 32, 0);
    c.ret(1);
  });
  // GlobalFirst/GlobalNext(lpGlobal, wFlags): an empty walk — FALSE at once,
  // lpGlobal untouched, what the unimplemented stubs answered. Their one
  // caller, ADXPL100's lock_sequencer_down_hard_now (the Disney Collection's
  // music modules, with sound on), walks the heap (GLOBAL_ALL, dwSize 0x24)
  // for MCISEQ.DRV's blocks to GlobalPageLock (7:09BF..7:0BB1): on FALSE it
  // notes "Error walking global list" and returns 1, and the song plays. A
  // real walk would have to list every block of the heap in arena order,
  // the burgermaster and the sentinel (GT_SENTINEL, which ends its loop),
  // each with its address, size, handle, lock and page-lock counts, owner
  // (a module, or the task's PDB), type (GT_CODE, GT_DGROUP, GT_DATA, …) and
  // segment number; MCISEQ is no NE module here (the MCI sequencer is the
  // host's), so no block would match and nothing would be locked either:
  // the walk would only add its own calls (it walks twice, one GlobalNext
  // per block each time).
  for (const char* n : {"GlobalFirst", "GlobalNext"}) {
    r.impl("TOOLHELP", n, [](Call16& c) {
      c.ptr();
      c.w();
      c.ret(0);
    });
  }

  r.impl("SHELL", "RegSetValue", [](Call16& c) { c.ret32(0); });
  r.impl("SHELL", "DragQueryFile", [](Call16& c) { c.ret(0); });
  r.impl("SHELL", "DragFinish", [](Call16&) {});

  // ---- WING ----
  // WinGCreateHalftoneBrush(HDC, COLORREF, WING_DITHER_TYPE) takes 8 argument
  // bytes, not the interface table's 6 (research/win/spec, Wine's wing.spec):
  // WING.DLL's entry reads [bp+6] (the dither type), [bp+8] (the COLORREF)
  // and [bp+0Ch] (the HDC) and returns with retf 8 (4:0A60..4:0C7F). Nothing of WinG is implemented —
  // the Star Wars seeds keep SWSE on its GDI path (dos16.hh
  // seed_intermission) — but a call must pop what the caller pushed.
  r.add("WING", 1008, "WinGCreateHalftoneBrush", Conv16::pascal_, true, 8);
}

void register_all16(Runtime16& rt) {
  register_dos(rt);
  register_kernel16(rt);
  register_user16(rt);
  register_gdi16(rt);
  register_system16(rt);
}

}  // namespace adw::win16
