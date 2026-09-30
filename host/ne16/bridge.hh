// The Classic lane's host side of the AD3 module protocol (ABI.md §3.2–§3.4,
// PACKAGES.md §7.4): one interface, two implementations, so the lane's frame
// loop is the same whichever runs.
//
//   OldMod16Bridge — Berkeley's OLDMOD16.DLL as real emulated code: the
//                    OLDMOD32 thunks become far calls to its five exports.
//   NativeBridge   — the same five entry points in C++, for the packages
//                    that ship no OLDMOD16 (AD 3.2, Totally Twisted, The
//                    Simpsons, and After Dark 2.0's Star Trek: The Screen
//                    Saver, whose modules AD.EXE 2.0b drove with the same
//                    messages and blocks): what OLDMOD16 does, step for step, as
//                    ABI.md §3.3 records it and research/win/dis/OLDMOD16.DLL.asm
//                    shows where §3.3 is silent. It fills AD_SYSTEM (version
//                    300, "BUTTHEAD") and AD_MODULE the same way, sends the
//                    same message sequence, makes the same Win16 API calls —
//                    through the same thunks, so the census, api16 traces,
//                    virtual time and the scanout hook see them as they see
//                    OLDMOD16's — and loads the engine dir's own AD_SND.DLL by
//                    full path without OLDMOD16's VerStr >= 400 gate (AD_SND
//                    3.0.3 and 3.2 export the seven entry points it uses with
//                    the same argument sizes). Of those, five are required
//                    with one volume pair: adwGetSystemVolumes +
//                    adwSetSystemVolumes, or else — After Dark 2.0's AD_SND
//                    1.0 (Star Trek: The Screen Saver), which has no other —
//                    adwSavePreviousVolume() + adwRestorePreviousVolume(),
//                    no arguments, called where the first pair is (as
//                    AD.EXE 2.0b called them; the restore only once the
//                    save has run). The error id: 1 when AD_SND cannot be
//                    loaded, 3 when a required entry is missing — one of the
//                    five, or a whole volume pair (neither pair complete).
//                    It replaces host code (as the lane already replaces
//                    OLDMOD32, AFTERDAR.SCR and, for After Dark 2.0, AD.EXE);
//                    every engine, sound DLL and module still runs as real
//                    code — but for a package that ships no AD_SND.DLL
//                    (Snoopy's Screen Savers; package.hh host_ad_snd), where
//                    the AD_SND it loads by that path is the host's own
//                    (win16/adsnd16.cc), registered by the AD3 protocol
//                    before the bridge opens.
//
// All pointers are guest far pointers (sel:off) into memory the lane owns;
// ctrl4 is four WORDs. Results are what OLDMOD16's exports return.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace adw::win16 {
class Runtime16;
struct Module16;
}  // namespace adw::win16

namespace adw::ne16 {

class Bridge16 {
 public:
  virtual ~Bridge16() = default;
  virtual const char* name() const = 0;
  // LOADADMODULE16(hwnd, hdc, ctrl4, volume, mute, path, err, errLen, errId): 1 = loaded.
  virtual uint16_t load(uint16_t hwnd, uint16_t hdc, uint32_t ctrl4, uint16_t volume, uint16_t mute, uint32_t path,
                        uint32_t err, uint16_t err_len, uint32_t err_id) = 0;
  // MODULEMESSAGE16(msg, err, errLen): the module's (mapped) result.
  virtual uint16_t message(uint16_t msg, uint32_t err, uint16_t err_len) = 0;
  // SETMODULECTRLVALUES16(volume, mute, ctrl4).
  virtual void set_controls(uint16_t volume, uint16_t mute, uint32_t ctrl4) = 0;
  // SETADPALETTE16(hpal, idx).
  virtual void set_palette(uint16_t hpal, uint16_t idx) = 0;
  // Palettes handed over at the first palette request instead of at load:
  // `supply` (which calls set_palette for each) runs once, when a module's
  // result first asks for a palette (10–13) — After Dark 2.0's computed
  // ones (package.hh AdPalettes::computed), as AD.EXE 2.0b built a palette
  // when a module asked for one. A module that asks for none never runs it,
  // so neither its API calls nor their virtual time happen. The real
  // OLDMOD16 maps the requests itself: supplied at once.
  virtual void defer_palettes(std::function<void()> supply) {
    if (supply) supply();
  }
  // UNLOADADMODULE16().
  virtual void unload() = 0;
  // BUTTONPUSHED16(path, owner, slot, ctrl4, err, errLen, errId) (OLDMOD16
  // 1:09e4, ABI.md §3.3, INTERACTION.md §1.6): with no module loaded, load
  // AD_SND and the module, AD_SYSTEM+0x26 = owner, MODULE(5 /*SELECTED*/) on
  // the screen DC; unless it returned 1 or 7, the four values into AD_MODULE
  // and MODULE(7 + slot); a replaced error text copied back; the module freed
  // (no CLOSE), AD_SND unloaded. 0 when SELECTED/the button returned 1 or 7
  // (or nothing could be loaded; errId says why), else 1.
  virtual uint16_t button(uint32_t path, uint16_t owner, uint16_t slot, uint32_t ctrl4, uint32_t err, uint16_t err_len,
                          uint32_t err_id) = 0;
  // After unload, before the modules are freed: what OLDMOD16's
  // DLLENTRYPOINT(0) did (the native bridge frees its blocks; the real one
  // runs that entry itself when it is freed).
  virtual void close() {}
  // AD_MODULE's far pointer (diagnostics), 0 when unknown.
  virtual uint32_t ad_module() const = 0;
};

// The real OLDMOD16.DLL at `host_path` (loaded here; its DLLENTRYPOINT(1)
// runs as part of the load). Null with *why set when it cannot be used.
std::unique_ptr<Bridge16> open_oldmod16_bridge(win16::Runtime16& rt, const std::string& host_path, std::string* why);

// The native bridge; `ad_snd_guest_path` is the engine dir's AD_SND.DLL as
// the guest sees it (C:\WINDOWS\SYSTEM\AD_SND.DLL): AD_SND 3.x/4.x, or 1.0
// (the volume pair above), or — a system module AD_SND registered first —
// the host's own. What DLLENTRYPOINT(1) did — AD_SYSTEM and
// AD_MODULE allocated and locked — happens here. Null with *why set when
// guest memory runs out.
std::unique_ptr<Bridge16> open_native_bridge(win16::Runtime16& rt, const std::string& ad_snd_guest_path,
                                             std::string* why);

}  // namespace adw::ne16
