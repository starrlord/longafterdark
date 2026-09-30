#include "ne16/bridge.hh"

#include <initializer_list>
#include <stdexcept>

#include "adw/core/log.h"
#include "win16/modules16.hh"
#include "win16/runtime16.hh"
#include "win16/shims16.hh"

namespace adw::ne16 {

using win16::Arg16;
using win16::GuestError16;
using win16::l16;
using win16::Runtime16;
using win16::w16;

namespace {

// ---- the real OLDMOD16.DLL -------------------------------------------------------------------------------

class OldMod16Bridge : public Bridge16 {
 public:
  OldMod16Bridge(Runtime16& rt, win16::Module16* m) : rt_(rt), m_(m) {}

  bool resolve() {
    load_ = rt_.modules().proc_address(m_, "LOADADMODULE16");
    unload_ = rt_.modules().proc_address(m_, "UNLOADADMODULE16");
    message_ = rt_.modules().proc_address(m_, "MODULEMESSAGE16");
    controls_ = rt_.modules().proc_address(m_, "SETMODULECTRLVALUES16");
    palette_ = rt_.modules().proc_address(m_, "SETADPALETTE16");
    button_ = rt_.modules().proc_address(m_, "BUTTONPUSHED16");
    return load_ && unload_ && message_ && controls_ && palette_;
  }
  uint16_t button(uint32_t path, uint16_t owner, uint16_t slot, uint32_t ctrl4, uint32_t err, uint16_t err_len,
                  uint32_t err_id) override {
    if (!button_) throw GuestError16(GuestError16::Kind::fatal, "OLDMOD16.DLL lacks BUTTONPUSHED16");
    return uint16_t(rt_.call_far(button_, {l16(path), w16(owner), w16(slot), l16(ctrl4), l16(err), w16(err_len), l16(err_id)}));
  }

  const char* name() const override { return "oldmod16"; }
  uint16_t load(uint16_t hwnd, uint16_t hdc, uint32_t ctrl4, uint16_t volume, uint16_t mute, uint32_t path,
                uint32_t err, uint16_t err_len, uint32_t err_id) override {
    return uint16_t(rt_.call_far(load_, {w16(hwnd), w16(hdc), l16(ctrl4), w16(volume), w16(mute), l16(path), l16(err),
                                         w16(err_len), l16(err_id)}));
  }
  uint16_t message(uint16_t msg, uint32_t err, uint16_t err_len) override {
    return uint16_t(rt_.call_far(message_, {w16(msg), l16(err), w16(err_len)}));
  }
  void set_controls(uint16_t volume, uint16_t mute, uint32_t ctrl4) override {
    rt_.call_far(controls_, {w16(volume), w16(mute), l16(ctrl4)});
  }
  void set_palette(uint16_t hpal, uint16_t idx) override { rt_.call_far(palette_, {w16(hpal), w16(idx)}); }
  void unload() override { rt_.call_far(unload_, {}); }
  // OLDMOD16's DGROUP:11F0 holds AD_MODULE's far pointer (ABI.md §3.3).
  uint32_t ad_module() const override { return m_->dgroup ? rt_.rd32((uint32_t(m_->dgroup) << 16) | 0x11F0) : 0; }

 private:
  Runtime16& rt_;
  win16::Module16* m_;
  uint32_t load_ = 0, unload_ = 0, message_ = 0, controls_ = 0, palette_ = 0, button_ = 0;
};

// ---- the native bridge --------------------------------------------------------------------------------------
//
// Each member function names the OLDMOD16 routine it follows
// (research/win/dis/OLDMOD16.DLL.asm). Its globals become members; its DGROUP
// data the lane-visible parts of which are guest memory (the four
// LOGPALETTEs AD_MODULE points modules at, the buffers handed to AD_SND and
// the module) live in one guest block laid out like this:
namespace nb {
constexpr uint16_t kLogPal = 0x0000;     // 4 × LOGPALETTE {0x300, 256, entries} (OLDMOD16 DGROUP:01BE)
constexpr uint16_t kLogPalSize = 0x404;
constexpr uint16_t kPath = 0x1010;       // the module path (DGROUP:00BA, 0x104)
constexpr uint16_t kSndBuf = 0x1114;     // adwSoundInit's buffer (a stack buffer in OLDMOD16, 0x106)
constexpr uint16_t kSaved = 0x121A;      // adwGetSystemVolumes' WORD (DGROUP:11EA)
constexpr uint16_t kUnloadErr = 0x121C;  // CLOSE's error buffer (a stack buffer in OLDMOD16, 0x104)
constexpr uint16_t kRect = 0x1320;       // GetClientRect's RECT
constexpr uint16_t kCount = 0x1328;      // GetObject's palette entry count
constexpr uint16_t kSize = 0x1330;
}  // namespace nb

class NativeBridge : public Bridge16 {
 public:
  NativeBridge(Runtime16& rt, std::string ad_snd) : rt_(rt), ad_snd_(std::move(ad_snd)) {}

  // DLLENTRYPOINT(1) → 1:0110: both blocks GlobalAlloc'd (GMEM_MOVEABLE |
  // GMEM_DDESHARE) and locked; plus the data block OLDMOD16's DGROUP was.
  bool attach(std::string* why) {
    h_data_ = uint16_t(api("KERNEL", "GlobalAlloc", {w16(0x0040), l16(nb::kSize)}));
    data_ = h_data_ ? api("KERNEL", "GlobalLock", {w16(h_data_)}) : 0;
    if (!data_) {
      *why = "no guest memory for the bridge";
      return false;
    }
    for (int i = 0; i < 4; i++) {
      rt_.wr16(logpal(i), 0x0300);
      rt_.wr16(logpal(i) + 2, 0x0100);
    }
    h_system_ = uint16_t(api("KERNEL", "GlobalAlloc", {w16(0x2002), l16(0x3C)}));
    h_module_ = uint16_t(api("KERNEL", "GlobalAlloc", {w16(0x2002), l16(0x30)}));
    if (h_system_ && h_module_) {
      system_ = api("KERNEL", "GlobalLock", {w16(h_system_)});
      module_ = api("KERNEL", "GlobalLock", {w16(h_module_)});
    }
    if (!system_ || !module_) {
      *why = "no guest memory for AD_SYSTEM/AD_MODULE";
      return false;
    }
    return true;
  }

  const char* name() const override { return "native"; }

  // LOADADMODULE16 (1:0ec8).
  uint16_t load(uint16_t hwnd, uint16_t hdc, uint32_t ctrl4, uint16_t volume, uint16_t mute, uint32_t path,
                uint32_t err, uint16_t err_len, uint32_t err_id) override {
    if (!ctrl4 || !path || !err) return 0;
    uint16_t e = load_ad_snd();
    rt_.wr16(err_id, e);
    if (e) return 0;
    hdc_ = hdc;
    hwnd_ = hwnd;
    api("KERNEL", "lstrcpyn", {l16(data_ + nb::kPath), l16(path), w16(0x104)});
    if (!load_module(path)) return 0;
    saved_dc_ = uint16_t(api("GDI", "SaveDC", {w16(hdc_)}));
    fill_system(err, 0);
    fill_module(ctrl4, hmod_);
    api("USER", "GetClientRect", {w16(hwnd_), l16(data_ + nb::kRect)});
    int16_t left = int16_t(rt_.rd16(data_ + nb::kRect)), top = int16_t(rt_.rd16(data_ + nb::kRect + 2));
    int16_t right = int16_t(rt_.rd16(data_ + nb::kRect + 4)), bottom = int16_t(rt_.rd16(data_ + nb::kRect + 6));
    rt_.wr16(module_ + 2, uint16_t(right - left));
    rt_.wr16(module_ + 4, uint16_t(bottom - top));
    region_ = uint16_t(api("GDI", "CreateRectRgn", {w16(uint16_t(left)), w16(uint16_t(top)), w16(uint16_t(right)),
                                                    w16(uint16_t(bottom))}));
    rt_.wr16(module_ + 0, region_);
    uint16_t r = call_module(5);  // MODULESELECTED
    if (r == 0) {
      for (uint32_t i = 0; i < 4; i++) rt_.wr16(module_ + 6 + 2 * i, rt_.rd16(ctrl4 + 2 * i));
      if (rt_.rd16(module_ + 0x1E)) sound(volume, mute);
      want_snd_ = rt_.rd16(module_ + 0x1E);
      last_volume_ = volume;
      last_mute_ = mute;
      call_module(12);  // PREINITIALIZE (result ignored)
      r = init_blank();
    }
    if (r != 0) {
      unload();
    } else {
      uint32_t text = rt_.rd32(system_ + 0x22);
      if (text != err) api("KERNEL", "lstrcpyn", {l16(err), l16(text), w16(err_len)});
    }
    return r == 0 ? 1 : 0;
  }

  // MODULEMESSAGE16 (1:1176).
  uint16_t message(uint16_t msg, uint32_t err, uint16_t err_len) override {
    if (!err) return 1;
    return message_impl(msg, err, err_len);
  }

  // SETMODULECTRLVALUES16 (1:11b8).
  void set_controls(uint16_t volume, uint16_t mute, uint32_t ctrl4) override {
    if (want_snd_ && (volume != last_volume_ || mute != last_mute_)) {
      sound(volume, mute);
      last_volume_ = volume;
      last_mute_ = mute;
    }
    for (uint32_t i = 0; i < 4; i++) rt_.wr16(module_ + 6 + 2 * i, rt_.rd16(ctrl4 + 2 * i));
  }

  // SETADPALETTE16 (1:10f6).
  void set_palette(uint16_t hpal, uint16_t idx) override {
    if (!hpal || idx >= 4) return;
    hpal_[idx] = hpal;
    api("GDI", "GetObject", {w16(hpal), w16(2), l16(data_ + nb::kCount)});
    uint16_t n = rt_.rd16(data_ + nb::kCount);
    if (n > 0 && n <= 0x100) api("GDI", "GetPaletteEntries", {w16(hpal), w16(0), w16(n), l16(logpal(idx) + 4)});
  }

  // Bridge16::defer_palettes: kept until select_palette first runs.
  void defer_palettes(std::function<void()> supply) override { deferred_ = std::move(supply); }

  // UNLOADADMODULE16 (1:0e26).
  void unload() override {
    if (hmod_) {
      if (initialized_) {
        message_impl(3, data_ + nb::kUnloadErr, 0x104);  // CLOSE
        initialized_ = false;
      }
      api("GDI", "RestoreDC", {w16(hdc_), w16(saved_dc_)});
      api("KERNEL", "FreeLibrary", {w16(hmod_)});
      hmod_ = 0;
      entry_ = 0;
      if (region_) {
        api("GDI", "DeleteObject", {w16(region_)});
        region_ = 0;
      }
      rt_.wr8(data_ + nb::kPath, 0);
    }
    unload_ad_snd();
    want_snd_ = 0;
  }

  // BUTTONPUSHED16 (1:09e4), step for step; module handle and entry are
  // BUTTONPUSHED16's locals there, the members here (no module is loaded).
  uint16_t button(uint32_t path, uint16_t owner, uint16_t slot, uint32_t ctrl4, uint32_t err, uint16_t err_len,
                  uint32_t err_id) override {
    if (!path || !ctrl4 || !err) return 0;
    uint16_t e = load_ad_snd();
    rt_.wr16(err_id, e);
    if (e) return 0;
    rt_.wr8(err, 0);
    uint16_t r = 1;
    if (load_module(path)) {
      fill_system(err, owner);
      fill_module(ctrl4, hmod_);
      uint16_t dc = uint16_t(api("USER", "GetDC", {w16(0)}));
      r = uint16_t(rt_.call_far(entry_, {w16(5), w16(dc), w16(h_system_)}));  // MODULESELECTED
      if (r != 1 && r != 7) {
        for (uint32_t i = 0; i < 4; i++) rt_.wr16(module_ + 6 + 2 * i, rt_.rd16(ctrl4 + 2 * i));
        r = uint16_t(rt_.call_far(entry_, {w16(uint16_t(7 + slot)), w16(dc), w16(h_system_)}));
      }
      api("USER", "ReleaseDC", {w16(0), w16(dc)});
      uint32_t text = rt_.rd32(system_ + 0x22);
      if (text != err) api("KERNEL", "lstrcpyn", {l16(err), l16(text), w16(err_len)});
      api("KERNEL", "FreeLibrary", {w16(hmod_)});
      hmod_ = 0;
      entry_ = 0;
      if (r == 1) rt_.wr8(err, 0);
    }
    unload_ad_snd();
    return (r == 1 || r == 7) ? 0 : 1;
  }

  // DLLENTRYPOINT(0) → 1:01a0.
  void close() override {
    if (system_) api("KERNEL", "GlobalUnlock", {w16(h_system_)});
    if (h_system_) api("KERNEL", "GlobalFree", {w16(h_system_)});
    if (module_) api("KERNEL", "GlobalUnlock", {w16(h_module_)});
    if (h_module_) api("KERNEL", "GlobalFree", {w16(h_module_)});
    if (data_) api("KERNEL", "GlobalUnlock", {w16(h_data_)});
    if (h_data_) api("KERNEL", "GlobalFree", {w16(h_data_)});
    system_ = module_ = data_ = 0;
    h_system_ = h_module_ = h_data_ = 0;
  }

  uint32_t ad_module() const override { return module_; }

 private:
  // A Win16 API call through its thunk, exactly as OLDMOD16's imports reach it.
  uint32_t api(const char* module, const char* name, std::initializer_list<Arg16> args) {
    win16::Shim16Entry* e = rt_.shims().find_name(module, name);
    if (!e) throw GuestError16(GuestError16::Kind::fatal, std::string("ne16 bridge: no shim ") + module + "." + name);
    return rt_.call_far(rt_.thunk_far(*e), args);
  }
  uint32_t str(const char* s) { return rt_.static_bytes(std::string("ne16 bridge: ") + s, s); }
  uint32_t logpal(int i) const { return data_ + nb::kLogPal + uint32_t(nb::kLogPalSize) * uint32_t(i); }
  uint32_t proc(uint16_t h, const char* name) { return api("KERNEL", "GetProcAddress", {w16(h), l16(str(name))}); }
  uint16_t call_module(uint16_t msg) {
    return uint16_t(rt_.call_far(entry_, {w16(msg), w16(hdc_), w16(h_system_)}));
  }

  // 1:0512: LoadLibrary(path) + GetProcAddress("MODULE").
  bool load_module(uint32_t path) {
    uint16_t h = uint16_t(api("KERNEL", "LoadLibrary", {l16(path)}));
    if (h < 32) {
      hmod_ = 0;
      return false;
    }
    hmod_ = h;
    entry_ = proc(h, "MODULE");
    if (!entry_) {
      api("KERNEL", "FreeLibrary", {w16(h)});
      hmod_ = 0;
      return false;
    }
    return true;
  }

  // 1:05c6 LoadAdSnd, without the VerStr gate (PACKAGES.md §7.4): 0 ok,
  // 1 AD_SND cannot be loaded, 3 a required entry point is missing — one of
  // the five (adwSoundInit, adwSoundCleanup, adwSetVolume, adwSetSoundMute,
  // adwStopSound) or a whole volume pair. The pairs: adwGetSystemVolumes
  // (LPWORD) + adwSetSystemVolumes(WORD), which OLDMOD16 uses (AD_SND 3.0.3
  // and later); else AD_SND 1.0's adwSavePreviousVolume() +
  // adwRestorePreviousVolume() (After Dark 2.0, which has no other: AD.EXE
  // 2.0b called them, with no arguments, where OLDMOD16 calls the first
  // pair). The second pair is looked up only when the first is incomplete,
  // so a 3.x/4.x AD_SND sees the same calls as ever.
  uint16_t load_ad_snd() {
    uint16_t h = uint16_t(api("KERNEL", "LoadLibrary", {l16(str(ad_snd_.c_str()))}));
    if (h < 32) {
      hsnd_ = 0;
      return 1;
    }
    hsnd_ = h;
    snd_init_ = proc(h, "adwSoundInit");
    snd_cleanup_ = proc(h, "adwSoundCleanup");
    snd_getsys_ = proc(h, "adwGetSystemVolumes");
    snd_setsys_ = proc(h, "adwSetSystemVolumes");
    snd_setvol_ = proc(h, "adwSetVolume");
    snd_setmute_ = proc(h, "adwSetSoundMute");
    snd_stop_ = proc(h, "adwStopSound");
    if (!snd_getsys_ || !snd_setsys_) {
      snd_getsys_ = snd_setsys_ = 0;
      snd_saveprev_ = proc(h, "adwSavePreviousVolume");
      snd_restoreprev_ = proc(h, "adwRestorePreviousVolume");
      if (!snd_saveprev_ || !snd_restoreprev_) snd_saveprev_ = snd_restoreprev_ = 0;
    }
    const bool volumes = snd_getsys_ || snd_saveprev_;
    if (!snd_init_ || !snd_cleanup_ || !volumes || !snd_setvol_ || !snd_setmute_ || !snd_stop_) {
      unload_ad_snd();
      return 3;
    }
    rt_.wr8(data_ + nb::kSndBuf, 0);
    rt_.call_far(snd_init_, {w16(0), l16(data_ + nb::kSndBuf)});
    if (snd_getsys_) {
      rt_.call_far(snd_getsys_, {l16(data_ + nb::kSaved)});
    } else {
      rt_.call_far(snd_saveprev_, {});
      snd_prev_saved_ = true;
    }
    return 0;
  }

  // 1:07ac. What the load saved is restored, and only that:
  // adwSetSystemVolumes gets the WORD adwGetSystemVolumes saved (when not 0),
  // and AD_SND 1.0's adwRestorePreviousVolume runs only once
  // adwSavePreviousVolume has. A load refused for a missing entry (error id
  // 3) initialized and saved nothing, so its unload calls adwStopSound and
  // adwSoundCleanup alone (those that resolved).
  void unload_ad_snd() {
    if (hsnd_) {
      if (snd_stop_) rt_.call_far(snd_stop_, {});
      uint16_t saved = rt_.rd16(data_ + nb::kSaved);
      if (snd_setsys_ && saved) rt_.call_far(snd_setsys_, {w16(saved)});
      if (snd_restoreprev_ && snd_prev_saved_) rt_.call_far(snd_restoreprev_, {});
      if (snd_cleanup_) rt_.call_far(snd_cleanup_, {});
      api("KERNEL", "FreeLibrary", {w16(hsnd_)});
    }
    hsnd_ = 0;
    snd_init_ = snd_cleanup_ = snd_getsys_ = snd_setsys_ = snd_setvol_ = snd_setmute_ = snd_stop_ = 0;
    snd_saveprev_ = snd_restoreprev_ = 0;
    snd_prev_saved_ = false;
    rt_.wr16(data_ + nb::kSaved, 0);
  }

  // 1:0b98.
  void sound(uint16_t volume, uint16_t mute) {
    if (!snd_setmute_ || !snd_setvol_) return;
    rt_.call_far(snd_setmute_, {w16(mute)});
    rt_.call_far(snd_setvol_, {w16(volume)});
  }

  // 1:020e: AD_SYSTEM (ABI.md §3.3).
  void fill_system(uint32_t err, uint16_t owner) {
    uint32_t flags = api("KERNEL", "GetWinFlags", {});
    uint16_t ic = uint16_t(api("GDI", "CreateIC", {l16(str("DISPLAY")), l16(0), l16(0), l16(0)}));
    auto caps = [&](uint16_t i) { return uint16_t(api("GDI", "GetDeviceCaps", {w16(ic), w16(i)})); };
    uint32_t s = system_;
    rt_.wr16(s + 0x00, 2);
    rt_.wr16(s + 0x02, (flags & 0x0004) ? 3 : 4);  // WF_CPU386 ? 386 : 486
    rt_.wr16(s + 0x04, (flags & 0x0400) ? 1 : 0);  // WF_80x87
    rt_.wr16(s + 0x06, caps(8));                   // HORZRES
    rt_.wr16(s + 0x08, caps(10));                  // VERTRES
    uint16_t bits = caps(12);                      // BITSPIXEL
    uint16_t planes = caps(14);                    // PLANES
    rt_.wr16(s + 0x0A, uint16_t(bits * planes));
    rt_.wr16(s + 0x0C, caps(40));  // ASPECTX
    rt_.wr16(s + 0x0E, caps(42));  // ASPECTY
    rt_.wr16(s + 0x10, caps(88));  // LOGPIXELSX
    rt_.wr16(s + 0x12, caps(90));  // LOGPIXELSY
    rt_.wr16(s + 0x14, 300);       // AD version 3.00
    for (uint32_t o = 0x16; o <= 0x1E; o += 2) rt_.wr16(s + o, 0);
    rt_.wr16(s + 0x20, h_module_);
    rt_.wr32(s + 0x22, err);
    rt_.wr16(s + 0x26, owner);
    rt_.wr16(s + 0x28, 0);
    rt_.wr16(s + 0x2A, (caps(38) & 0x0100) ? 1 : 0);  // RASTERCAPS & RC_PALETTE
    const char* sig = "BUTTHEAD";
    for (uint32_t i = 0; i < 8; i++) rt_.wr16(s + 0x2C + 2 * i, uint8_t(sig[i]));
    api("GDI", "DeleteDC", {w16(ic)});
  }

  // 1:0412: AD_MODULE (ABI.md §3.3).
  void fill_module(uint32_t ctrl4, uint16_t hmodule) {
    uint32_t m = module_;
    for (uint32_t o = 0; o < 6; o += 2) rt_.wr16(m + o, 0);
    for (uint32_t i = 0; i < 4; i++) rt_.wr16(m + 6 + 2 * i, rt_.rd16(ctrl4 + 2 * i));
    for (uint32_t i = 0; i < 4; i++) rt_.wr16(m + 0x0E + 2 * i, uint16_t(i + 1));
    rt_.wr16(m + 0x16, hmodule);
    for (uint32_t o = 0x18; o < 0x30; o += 2) rt_.wr16(m + o, 0);
  }

  // 1:0be0: a palette request. Deferred palettes (defer_palettes) are
  // handed over first, at the first request.
  uint16_t select_palette(int idx) {
    if (deferred_) {
      std::function<void()> supply = std::move(deferred_);
      deferred_ = nullptr;
      supply();
    }
    if (!hdc_ || !hpal_[idx]) return 7;
    rt_.wr16(module_ + 0x18, hpal_[idx]);
    rt_.wr32(module_ + 0x1A, logpal(idx));
    api("USER", "SelectPalette", {w16(hdc_), w16(hpal_[idx]), w16(0)});
    api("USER", "RealizePalette", {w16(hdc_)});
    return 0;
  }

  // 1:0c6c: palette requests 10/11/12/13 select hpal[1]/[3]/[0]/[2].
  uint16_t map_result(uint16_t r) {
    switch (r) {
      case 10:
        return select_palette(1);
      case 11:
        return select_palette(3);
      case 12:
        return select_palette(0);
      case 13:
        return select_palette(2);
      default:
        return r;
    }
  }

  // 1:0cdc: INITIALIZE, then BLANK if it succeeded (0x0E counts as success).
  uint16_t init_blank() {
    if (!entry_) return 1;
    initialized_ = false;
    uint16_t r = map_result(call_module(0));
    if (r == 0x0E) r = 0;
    if (r == 0) {
      initialized_ = true;
      r = map_result(call_module(1));
    }
    return r;
  }

  // 1:0d68.
  uint16_t message_impl(uint16_t msg, uint32_t err, uint16_t err_len) {
    if (!entry_) return 1;
    rt_.wr32(system_ + 0x22, err);
    uint16_t r = map_result(call_module(msg));
    if (r == 3) r = init_blank();  // RESTART
    uint32_t text = rt_.rd32(system_ + 0x22);
    if (text != err) api("KERNEL", "lstrcpyn", {l16(err), l16(text), w16(err_len)});
    if (r == 1) rt_.wr8(err, 0);
    return r;
  }

  Runtime16& rt_;
  std::string ad_snd_;
  uint16_t h_data_ = 0, h_system_ = 0, h_module_ = 0;
  uint32_t data_ = 0, system_ = 0, module_ = 0;
  uint16_t hsnd_ = 0;
  uint32_t snd_init_ = 0, snd_cleanup_ = 0, snd_getsys_ = 0, snd_setsys_ = 0, snd_setvol_ = 0, snd_setmute_ = 0,
           snd_stop_ = 0;
  uint32_t snd_saveprev_ = 0, snd_restoreprev_ = 0;  // AD_SND 1.0's volume pair (load_ad_snd)
  bool snd_prev_saved_ = false;                      // adwSavePreviousVolume ran (unload_ad_snd restores only then)
  uint16_t hmod_ = 0;
  uint32_t entry_ = 0;
  uint16_t hdc_ = 0, hwnd_ = 0, saved_dc_ = 0, region_ = 0;
  uint16_t hpal_[4] = {0, 0, 0, 0};
  std::function<void()> deferred_;  // defer_palettes' supply, until the first palette request
  bool initialized_ = false;
  uint16_t want_snd_ = 0, last_volume_ = 0, last_mute_ = 0;
};

}  // namespace

std::unique_ptr<Bridge16> open_oldmod16_bridge(Runtime16& rt, const std::string& host_path, std::string* why) {
  uint16_t err = 0;
  win16::Module16* m = rt.modules().load_host(host_path, &err);
  if (!m) {
    *why = "cannot load " + host_path + " (error " + std::to_string(err) + ")";
    return nullptr;
  }
  auto b = std::make_unique<OldMod16Bridge>(rt, m);
  if (!b->resolve()) {
    *why = "OLDMOD16.DLL lacks its exports";
    return nullptr;
  }
  return b;
}

std::unique_ptr<Bridge16> open_native_bridge(Runtime16& rt, const std::string& ad_snd_guest_path, std::string* why) {
  auto b = std::make_unique<NativeBridge>(rt, ad_snd_guest_path);
  if (!b->attach(why)) return nullptr;
  return b;
}

}  // namespace adw::ne16
