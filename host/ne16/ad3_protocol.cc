// The AD3 module protocol (protocol.hh, lane.hh "The AD3 protocol"): an After
// Dark 2.x/3.x module driven the way AFTERDAR.SCR drove it through OLDMOD32's
// flat thunks, over a bridge (bridge.hh) — the lane's AD3 code, moved here
// unchanged behind Protocol16.
#include <windows.h>

#include <algorithm>
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "adw/core/audio.h"
#include "adw/core/log.h"
#include "adw/core/text.h"
#include "loader/ne.hh"
#include "ne16/bridge.hh"
#include "ne16/protocol.hh"
#include "win16/dos16.hh"
#include "win16/gdi16.hh"
#include "win16/modules16.hh"
#include "win16/runtime16.hh"
#include "win16/shim_families16.hh"
#include "win32/vfs.hh"

namespace adw::ne16 {

using win16::Runtime16;

namespace {

std::string full_path(const std::string& p) {
  std::wstring w = widen(p);
  DWORD n = GetFullPathNameW(w.c_str(), 0, nullptr, nullptr);
  if (!n) return p;
  std::wstring out(n, L'\0');
  n = GetFullPathNameW(w.c_str(), n, out.data(), nullptr);
  out.resize(n);
  return narrow(out);
}

std::string file_of(const std::string& p) {
  size_t s = p.find_last_of("\\/");
  return s == std::string::npos ? p : p.substr(s + 1);
}

uint64_t env_u64(const Env& env, const char* name, uint64_t def) {
  const std::string* v = env.get(name);
  if (!v || v->empty()) return def;
  char* end = nullptr;
  double d = strtod(v->c_str(), &end);  // accepts 1e9 as well as plain integers
  if (end == v->c_str() || d < 0) {
    log("%s='%s' is not a number; using %" PRIu64, name, v->c_str(), def);
    return def;
  }
  return uint64_t(d);
}

bool file_exists(const std::string& p) {
  DWORD a = GetFileAttributesW(widen(p).c_str());
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool read_host_file(const std::string& p, std::vector<uint8_t>* out) {
  HANDLE h = CreateFileW(widen(p).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER sz{};
  bool ok = GetFileSizeEx(h, &sz) && sz.QuadPart >= 0 && sz.QuadPart < (16 << 20);
  if (ok) {
    out->resize(size_t(sz.QuadPart));
    DWORD got = 0;
    ok = out->empty() || (ReadFile(h, out->data(), DWORD(out->size()), &got, nullptr) && got == out->size());
  }
  CloseHandle(h);
  return ok;
}

bool same_dir(const std::string& a, const std::string& b) {
  return CompareStringOrdinal(widen(full_path(a)).c_str(), -1, widen(full_path(b)).c_str(), -1, TRUE) == CSTR_EQUAL;
}

uint16_t u16(std::string_view s, size_t off) {
  if (off + 2 > s.size()) return 0;
  return uint16_t(uint8_t(s[off]) | (uint8_t(s[off + 1]) << 8));
}

}  // namespace

// The guest's disk (INTERACTION.md §7.2), one 1996 machine per package:
//   C:\AFTERDRK   the module's folder — the install directory, the AD Data
//                 Files directory, where the helper DLLs, sound databases and
//                 data files sit beside the modules — under a copy-on-write
//                 upper layer <state>\<package>\<MODDIR> (NONSENSE.TXT,
//                 MESG_AD3.DAT land there)
//   C:\AFTERD~1   the same folder under the short name of the original
//                 install directory ("C:\After Dark"), which data files
//                 carry baked in (BITMAPS.ADC lists C:\afterd~1\bitmaps); the
//                 same upper (a memory upper is one per mount). An alias: it
//                 resolves, but a listing of C:\ leaves it out (a 1996
//                 install had one of the two; DOS Shell's DIR shows C:\)
//   C:\WINDOWS    virtual seed files under <state>\<package>\WINDOWS
//                 (MODULES.INI, AFTERDRK.INI, WIN.INI …, and LunData.dat as
//                 the installers put it there), shared with the package's AD4
//                 modules; over the package's windows dir (package.hh) when
//                 it has one (none of the After Dark packages does); for
//                 After Dark 2.0 (package.hh after_dark2) AD_PREFS.INI's
//                 profile seeds (win16/dos16.hh seed_after_dark2), else for
//                 After Dark 3.x (package.hh after_dark3_host) the ones
//                 ADW30.EXE wrote (win16/dos16.hh seed_after_dark3)
//   C:\WINDOWS\SYSTEM  the engine dir (OLDMOD16, AD_SND), read-only
//   H:\<L>\…      the host's drives, read-only, 8.3 names (file dialogs)
// Without ADSTATE every upper layer is memory: nothing is read from or
// written to the user's state, so headless runs and FBHASH stay as they were.
void mount_disk(Runtime16& rt, const Env& env, const std::string& module_path, const Ne16Layout& layout) {
  const win16::Runtime16Options& o = rt.options();
  win32::Vfs& vfs = rt.vfs();
  std::string pkg = package_state_dir(env, module_path);
  std::string moddir = file_of(layout.module_dir);
  if (env.state_persistent()) vfs.set_state_root(env.state_root);
  vfs.mount_overlay(o.windows_dir, layout.windows_dir, pkg.empty() ? "" : pkg + "\\WINDOWS");
  vfs.mount_overlay(o.guest_dir, layout.module_dir, pkg.empty() ? "" : pkg + "\\" + moddir);
  vfs.mount_overlay("C:\\AFTERD~1", layout.module_dir, pkg.empty() ? "" : pkg + "\\" + moddir);
  vfs.hide_in_listing("C:\\AFTERD~1");
  vfs.mount(o.system_dir, layout.engine_dir, /*writable=*/false);
  vfs.mount_host_drives(/*short_names=*/true);
  vfs.set_cwd(o.guest_dir);
  for (const std::string& d : layout.search_dirs) rt.modules().add_search_dir(d);
  // What the installers copied from the module folder into WINDOWS (the ad10
  // install map has WINDOWS\LunData.dat, byte-identical to the disc's
  // LUNDATA.DAT beside LUNATIC.AD): Lunatic Fringe reads its keys and scores
  // from GetWindowsDirectory()\LunData.dat, and without it says
  // "Configuration File Not Accessible. Will Use Default Keys.". A lower-layer
  // seed: the module's own writes (Keys…, Clear Scores, high scores) go to the
  // upper layer as usual.
  for (const char* f : {"LunData.dat"}) {
    std::vector<uint8_t> bytes;
    if (read_host_file(layout.module_dir + "\\" + f, &bytes)) vfs.add_virtual_file(o.windows_dir + "\\" + f, std::move(bytes));
  }
  // MODULES.INI's per-install settings, now that the module dir is mounted.
  win16::seed_modules_ini(rt);
  // After Dark 2.0's AD_PREFS.INI (a rule by file: the module folder holds
  // AD_MOD.DLL): profile seeds under the empty virtual file, never the
  // disk's AD_PREFS.INI, whose PC-speaker driver would win over a seed.
  // Else After Dark 3.x's (the engine dir holds ADW30.EXE): the keys ADW30
  // wrote at every start, as seeds too.
  const bool ad2 = after_dark2(layout, file_exists);
  const bool ad3 = !ad2 && after_dark3_host(layout, file_exists);
  if (ad2) win16::seed_after_dark2(rt);
  if (ad3) win16::seed_after_dark3(rt);
  trace("lane", "disk: C:\\WINDOWS and %s over %s%s%s", o.guest_dir.c_str(),
        pkg.empty() ? "memory (no ADSTATE)" : (pkg + " (" + moddir + ")").c_str(),
        layout.windows_dir.empty() ? "" : (", C:\\WINDOWS over " + layout.windows_dir).c_str(),
        ad2   ? "; seeds: AD_PREFS.INI [After Dark] Path, [Sound] SoundDriver=AD_MME.DRV (After Dark 2.0)"
        : ad3 ? "; seeds: AD_PREFS.INI [After Dark] Path, [Sound] SoundDriver=AD_MME.DRV (After Dark 3.x's ADW30.EXE)"
              : "");
}

int16_t control_default16(std::string_view rec) {
  if (rec.size() < 0x1A) return 0;
  uint16_t kind = u16(rec, 0x00);
  int16_t def = int16_t(u16(rec, 0x18));
  switch (kind) {
    case 1: {  // string slider: `count` 16-byte labels at +0x20, then the stop values
      uint16_t count = std::min<uint16_t>(u16(rec, 0x16), 101);
      if (!count) return 0;
      size_t at = 0x20 + size_t(count) * 16;
      if (at + size_t(count) * 2 > rec.size()) return 0;
      std::vector<int32_t> stops;
      // A list that does not start at 0 gets a 0 stop in front (AFTERDAR.SCR 0x404f7b).
      if (u16(rec, at) != 0) stops.push_back(0);
      for (uint16_t i = 0; i < count; i++) stops.push_back(u16(rec, at + 2 * i));
      int32_t v = stops.front();
      for (int32_t s : stops)
        if (s <= def) v = s;
      return int16_t(v);
    }
    case 2: {  // numeric slider: clamped to [min, max] at +0x30/+0x32
      if (rec.size() < 0x34) return def;
      int16_t lo = int16_t(u16(rec, 0x30)), hi = int16_t(u16(rec, 0x32));
      if (lo > hi) std::swap(lo, hi);
      return std::clamp<int16_t>(def, lo, hi);
    }
    case 3: {  // popup: an item index
      uint16_t count = u16(rec, 0x16);
      return count ? std::clamp<int16_t>(def, 0, int16_t(count - 1)) : 0;
    }
    case 5:  // checkbox
      return def ? 1 : 0;
    default:  // none, button
      return 0;
  }
}

namespace {

class Ad3Protocol : public Protocol16 {
 public:
  explicit Ad3Protocol(const Ne16Layout& layout) : layout_(layout), module_name_(file_of(layout.module_path)) {}

  const char* name() const override { return bridge_kind_ == BridgeKind::oldmod16 ? "ad3/oldmod16" : "ad3/native"; }
  void configure_runtime(win16::Runtime16Options& opts, LaneContext& ctx) override;
  void mount(Runtime16& rt, const Env& env) override { mount_disk(rt, env, layout_.module_path, layout_); }
  bool load(Runtime16& rt, uint16_t hwnd, uint16_t hdc, LaneContext& ctx) override;
  Call call() override;
  bool set_control(int index, int32_t value) override;
  void send_controls() override;
  void unload() override {
    if (bridge_) bridge_->unload();
  }
  std::string error_text() const override;
  bool check_button(int slot, std::string* why) override;
  void configure_button_runtime(win16::Runtime16Options& opts, const Env& env) override;
  Button button(Runtime16& rt, int slot, uint16_t owner16, LaneContext& ctx) override;
  void close() override {
    if (bridge_) bridge_->close();
  }

 private:
  // The native bridge over its AD_SND: the engine dir's, or — none there
  // (package.hh host_ad_snd) — the host's own, registered first.
  std::unique_ptr<Bridge16> open_native(Runtime16& rt, std::string* why);

  Ne16Layout layout_;
  std::string module_name_;
  Runtime16* rt_ = nullptr;  // load's or button's
  std::unique_ptr<Bridge16> bridge_;
  BridgeKind bridge_kind_ = BridgeKind::oldmod16;
  bool bridge_auto_ = true;  // no ADNE16BRIDGE (the init trace says when it chose)
  bool host_ad_snd_ = false;  // the host's AD_SND answers (open_native)
  bool ad2_ = false;         // After Dark 2.0's (package.hh after_dark2): result 5 is its wake
  std::shared_ptr<loader::ne::Image> img_;  // configure mode: the image check_button read
  uint32_t scratch_ = 0;  // far pointer to the scratch block
  uint16_t hwnd_ = 0, hdc_ = 0;
  uint16_t volume_ = 50, mute_ = 1;
  int16_t ctrl_[4] = {0, 0, 0, 0};
};

// The native bridge loads C:\WINDOWS\SYSTEM\AD_SND.DLL, the engine dir's. A
// package that ships none (Snoopy's Screen Savers: modules for the user's
// own After Dark 2.0 or 3.0) gets the host's AD_SND instead, registered as
// the system module AD_SND before the bridge loads it — by that path, and
// the modules' imports by name, then reach it (win16/adsnd16.cc).
std::unique_ptr<Bridge16> Ad3Protocol::open_native(Runtime16& rt, std::string* why) {
  host_ad_snd_ = host_ad_snd(layout_, file_exists);
  if (host_ad_snd_) win16::register_host_ad_snd(rt);
  return open_native_bridge(rt, rt.options().system_dir + "\\" + kAdSndLibrary, why);
}

std::string Ad3Protocol::error_text() const {
  if (!rt_ || !scratch_) return "(no error text)";
  std::string s;
  try {
    s = rt_->read_str(scratch_ + scratch::kError, scratch::kErrorSize);
  } catch (const std::exception&) {
  }
  return s.empty() ? "(no error text)" : s;
}

// Ne16Lane::init, before the runtime exists: the bridge, the volume and mute,
// the display's starting palette.
void Ad3Protocol::configure_runtime(win16::Runtime16Options& opts, LaneContext& ctx) {
  const Env& env = ctx.env;
  BridgeKind forced = BridgeKind::oldmod16;
  if (const std::string* b = env.get("ADNE16BRIDGE")) {
    if (!parse_bridge_choice(*b, &bridge_auto_, &forced)) {
      log("ADNE16BRIDGE='%s' is not auto, oldmod16 or native; using auto", b->c_str());
      bridge_auto_ = true;
    }
  }
  bridge_kind_ = bridge_auto_ ? choose_bridge(layout_, file_exists) : forced;
  ad2_ = after_dark2(layout_, file_exists);
  volume_ = uint16_t(std::min<uint64_t>(env_u64(env, "ADVOLUME", 50), 100));
  mute_ = env.flag("ADSOUND") ? 0 : 1;
  // Sound (lane.hh "Sound", AUDIO.md §8.1): with the host audio engine on,
  // the module is unmuted at After Dark's volume slider (ADVOLUME, through
  // the engine's config). AD_SND may read AD_PREFS.INI [Sound] Mute at
  // adwSoundInit: in the AD 3.x/4.x packages nothing writes that key, so
  // AD_SND's default (not muted) holds; After Dark 2.0's AD_SND 1.0 writes it
  // at every adwSetSoundMute, so with ADSTATE it may read the Mute an earlier
  // run left, and the bridge's adwSetSoundMute (for a module that wants
  // sound, AD_MODULE+0x1E: every Star Trek module but Ion Storm) sets this
  // run's after MODULESELECTED, before the module initializes.
  const bool sound_on = ctx.audio && ctx.audio->enabled();
  if (sound_on) {
    volume_ = uint16_t(std::clamp(ctx.audio->config().volume, 0, 100));
    mute_ = 0;
  }
  // The display's starting palette (Runtime16Options::desktop_palette): a
  // desktop's distinct colours for the AD 3 generation packages (no OLDMOD16
  // in their engine dir: the native bridge stands in for ADW30/ADTASK, and
  // ADXPL310's identity palette needs them — SIMPCLOK); the lane's original
  // black-between-the-statics for everything OLDMOD16 runs, so Deluxe's and
  // ad10's streams stay as they were. ADDESKTOPPAL=0/1 overrides (the lane).
  opts.desktop_palette = layout_.packaged && choose_bridge(layout_, file_exists) == BridgeKind::native;
}

bool Ad3Protocol::load(Runtime16& rt, uint16_t hwnd, uint16_t hdc, LaneContext& ctx) {
  rt_ = &rt;
  hwnd_ = hwnd;
  hdc_ = hdc;
  const win16::Runtime16Options& opts = rt.options();
  const std::string& dir = layout_.module_dir;
  const std::string& engine = layout_.engine_dir;
  std::string ad_snd = engine + "\\AD_SND.DLL", why;
  if (bridge_kind_ == BridgeKind::oldmod16) {
    // The AD_SND guard (PACKAGES.md §7.3): an AD_SND.DLL beside the module
    // (older than OLDMOD16 accepts, in every layout seen) would be found
    // before the engine dir's. Load the engine's first; OLDMOD16's
    // LoadLibrary("ad_snd.dll") then finds it by module name.
    if (!same_dir(dir, engine) && file_exists(dir + "\\AD_SND.DLL") && file_exists(ad_snd)) {
      uint16_t e = 0;
      if (rt.modules().load_host(ad_snd, &e)) log("%s: %s\\AD_SND.DLL is ignored; OLDMOD16 gets %s", module_name_.c_str(), dir.c_str(), ad_snd.c_str());
    }
    bridge_ = open_oldmod16_bridge(rt, engine + "\\OLDMOD16.DLL", &why);
  } else {
    bridge_ = open_native(rt, &why);
  }
  if (!bridge_) {
    log("%s: %s", module_name_.c_str(), why.c_str());
    return false;
  }

  // The scratch block the far pointers point into.
  uint16_t hb = rt.global().alloc(win16::GlobalHeap16::kZeroInit, scratch::kSize);
  if (!hb) throw std::runtime_error("no guest memory for the scratch block");
  scratch_ = uint32_t(hb) << 16;

  // SetADPalette3216(hpal[i], i) for the four AD palettes (ABI.md §3.1).
  AdPalettes pals = load_palettes(layout_, bridge_kind_, file_exists);
  std::string pal_text = pals.pal.empty() ? "none: " + pals.error : pals.source;
  if (pals.computed) pal_text += ", handed over at the first palette request";
  std::string snd_text = host_ad_snd_ ? "the host's (no " + ad_snd + ")" : ad_snd;
  trace("lane", "%s: package %s, module dir %s, engine dir %s, bridge %s%s, AD_SND %s, palettes %s, %s display palette",
        module_name_.c_str(), layout_.packaged ? layout_.package_id.c_str() : "legacy", dir.c_str(), engine.c_str(),
        bridge_name(bridge_kind_), bridge_auto_ ? "" : " (ADNE16BRIDGE)", snd_text.c_str(), pal_text.c_str(),
        opts.desktop_palette ? "desktop" : "boot");
  if (pals.pal.empty()) {
    // No palette source (package.hh): an OLDMOD16 without AFTERDAR.SCR, or
    // an ADTASK.DLL without its palettes. (After Dark 2.0's are computed.)
    log("%s: no AD palettes (%s); palette requests will fail", module_name_.c_str(), pals.error.c_str());
  }
  // At load for the files' palettes, as AFTERDAR.SCR set them before loading
  // a module; at the first palette request for After Dark 2.0's computed
  // ones, as AD.EXE 2.0b built one when a module asked (AdPalettes::computed).
  auto supply = [&rt, bridge = bridge_.get(), pal = std::move(pals.pal)] {
    auto& gdi = rt.state<win16::Gdi16>();
    for (size_t i = 0; i < pal.size(); i++) bridge->set_palette(gdi.create_palette(pal[i]), uint16_t(i));
  };
  if (pals.computed) {
    bridge_->defer_palettes(std::move(supply));
  } else {
    supply();
  }

  // Controls: the record defaults, ADCVSET over them.
  std::shared_ptr<loader::ne::Image> img;
  try {
    img = std::make_shared<loader::ne::Image>(loader::ne::Image::from_file(layout_.module_path));
  } catch (const std::exception& e) {
    log("%s: not an NE module: %s", module_name_.c_str(), e.what());
    return false;
  }
  for (int i = 0; i < 4; i++) {
    int16_t def = 0;
    if (const auto* res = img->find_resource(loader::ResId::of(1000), loader::ResId::of(uint16_t(i + 1)))) {
      def = control_default16(img->resource_data(*res));
    }
    ctrl_[i] = int16_t(ctx.input.control(i, def));
    rt.wr16(scratch_ + scratch::kCtrl + 2u * uint32_t(i), uint16_t(ctrl_[i]));
    trace("lane", "control %d = %d%s", i, ctrl_[i], ctrl_[i] == def ? "" : " (ADCVSET)");
  }

  // LoadADModule3216 → LOADADMODULE16(hwnd, hdc, ctrl4, volume, mute, path, err, errLen, &errId).
  // The module as the install directory holds it (C:\AFTERD~1 is the same
  // folder under another name; the long form is the one AD's INI files held).
  std::string guest = opts.guest_dir + "\\" + win16::upper16(module_name_);
  rt.write_str(scratch_ + scratch::kPath, guest, 260);
  uint16_t r = bridge_->load(hwnd_, hdc_, scratch_ + scratch::kCtrl, volume_, mute_, scratch_ + scratch::kPath,
                             scratch_ + scratch::kError, scratch::kErrorSize, scratch_ + scratch::kErrId);
  trace("lane", "LOADADMODULE16(%s, volume %u, mute %u) -> %u", guest.c_str(), volume_, mute_, r);
  if (!r) {
    uint16_t id = rt.rd16(scratch_ + scratch::kErrId);
    // OLDMOD32 turned these ids into its own strings (ABI.md §3.7).
    std::string why_id = id == 1   ? "cannot load AD_SND.DLL (" + ad_snd + ")"
                         : id == 2 ? "AD_SND.DLL is too old"
                         : id == 3 ? "AD_SND.DLL lacks an entry point"
                                   : error_text();
    log("%s: the module did not load: %s", module_name_.c_str(), why_id.c_str());
    return false;
  }
  return true;
}

// One DRAWFRAME (lane.hh "The AD3 protocol").
Protocol16::Call Ad3Protocol::call() {
  // AFTERDAR.SCR 0x401f6f: SetWindowOrgEx(hdc, 0, 0) before every DRAWFRAME.
  if (HDC h = rt_->state<win16::Gdi16>().host_dc(hdc_)) SetWindowOrgEx(h, 0, 0, nullptr);
  uint16_t r = bridge_->message(2, scratch_ + scratch::kError, scratch::kErrorSize);
  int16_t v = int16_t(r);
  if (v != 0) trace("lane", "MODULEMESSAGE16(DRAWFRAME) -> %d", v);
  // 0 ok; 0x0E toggles "wants events"; 0x11/0x12 show/hide the cursor; below
  // 0 or above 0x12 counts as 0; anything else is the module's error (text
  // in err) and ends the run — but 5 from an After Dark 2.0 module is its
  // wake: AD.EXE 2.0b's result table (13:0c1f) posted itself its wake
  // message, 0x7EE, for it (Final Exam's mouse move ends its exam so).
  Call c;
  c.code = v;
  if (v == 0x0E) {
    c.kind = Call::Kind::toggle_events;
  } else if (v == 0x11 || v == 0x12) {
    c.kind = v == 0x11 ? Call::Kind::cursor_on : Call::Kind::cursor_off;
  } else if (v == 5 && ad2_) {
    c.kind = Call::Kind::wake;
  } else if (v > 0 && v <= 0x12) {
    c.kind = Call::Kind::stop;
  }
  return c;
}

bool Ad3Protocol::set_control(int index, int32_t value) {
  if (index < 0 || index >= 4) return false;
  ctrl_[index] = int16_t(value);
  return true;
}

void Ad3Protocol::send_controls() {
  for (int i = 0; i < 4; i++) rt_->wr16(scratch_ + scratch::kCtrl + 2u * uint32_t(i), uint16_t(ctrl_[i]));
  // SetModuleCtrlValues3216 → SETMODULECTRLVALUES16(volume, mute, ctrl4).
  bridge_->set_controls(volume_, mute_, scratch_ + scratch::kCtrl);
  // The bridge copied them into AD_MODULE.iControlValue (+6), where the module reads them.
  if (tracing("lane")) {
    uint32_t mod = bridge_->ad_module();
    if (mod) {
      trace("lane", "SETMODULECTRLVALUES16(%u, %u, {%d, %d, %d, %d}) -> AD_MODULE controls {%d, %d, %d, %d}", volume_,
            mute_, ctrl_[0], ctrl_[1], ctrl_[2], ctrl_[3], int16_t(rt_->rd16(mod + 6)), int16_t(rt_->rd16(mod + 8)),
            int16_t(rt_->rd16(mod + 10)), int16_t(rt_->rd16(mod + 12)));
    }
  }
}

// Configure mode (lane.hh "Configure"): the module's button handler, as
// AFTERDAR.SCR's property page ran it through OLDMOD32's ButtonPushed3216 —
// BUTTONPUSHED16 of the real OLDMOD16, or the native bridge's same sequence.
bool Ad3Protocol::check_button(int slot, std::string* why) {
  // The control record of the slot must be a button (kind 4, ABI.md §2.10.2).
  try {
    img_ = std::make_shared<loader::ne::Image>(loader::ne::Image::from_file(layout_.module_path));
  } catch (const std::exception& e) {
    *why = std::string("not an NE module: ") + e.what();
    return false;
  }
  const auto* rec = slot >= 0 ? img_->find_resource(loader::ResId::of(1000), loader::ResId::of(uint16_t(slot + 1)))
                              : nullptr;
  if (!rec || u16(img_->resource_data(*rec), 0) != 4) {
    *why = "control " + std::to_string(slot) + " is not a button";
    return false;
  }
  return true;
}

void Ad3Protocol::configure_button_runtime(win16::Runtime16Options& opts, const Env& env) {
  BridgeKind forced = BridgeKind::oldmod16;
  if (const std::string* b = env.get("ADNE16BRIDGE")) {
    if (!parse_bridge_choice(*b, &bridge_auto_, &forced)) bridge_auto_ = true;
  }
  bridge_kind_ = bridge_auto_ ? choose_bridge(layout_, file_exists) : forced;
  opts.desktop_palette = layout_.packaged && choose_bridge(layout_, file_exists) == BridgeKind::native;
}

Protocol16::Button Ad3Protocol::button(Runtime16& rt, int slot, uint16_t owner16, LaneContext& ctx) {
  rt_ = &rt;
  const win16::Runtime16Options& opts = rt.options();
  Button out;
  std::string ad_snd = layout_.engine_dir + "\\AD_SND.DLL", why;
  if (bridge_kind_ == BridgeKind::oldmod16) {
    if (!same_dir(layout_.module_dir, layout_.engine_dir) && file_exists(layout_.module_dir + "\\AD_SND.DLL") &&
        file_exists(ad_snd)) {
      uint16_t e = 0;
      rt.modules().load_host(ad_snd, &e);
    }
    bridge_ = open_oldmod16_bridge(rt, layout_.engine_dir + "\\OLDMOD16.DLL", &why);
  } else {
    bridge_ = open_native(rt, &why);
  }
  if (!bridge_) {
    out.message = why;
    return out;
  }
  uint16_t hb = rt.global().alloc(win16::GlobalHeap16::kZeroInit, scratch::kSize);
  if (!hb) throw std::runtime_error("no guest memory for the scratch block");
  scratch_ = uint32_t(hb) << 16;
  for (int i = 0; i < 4; i++) {
    int16_t def = 0;
    if (const auto* r = img_->find_resource(loader::ResId::of(1000), loader::ResId::of(uint16_t(i + 1)))) {
      def = control_default16(img_->resource_data(*r));
    }
    ctrl_[i] = int16_t(ctx.input.control(i, def));
    rt.wr16(scratch_ + scratch::kCtrl + 2u * uint32_t(i), uint16_t(ctrl_[i]));
  }
  std::string guest = opts.guest_dir + "\\" + win16::upper16(module_name_);
  rt.write_str(scratch_ + scratch::kPath, guest, 260);
  trace("lane", "%s: BUTTONPUSHED16(%s, owner %04X, %d) through the %s bridge%s", module_name_.c_str(), guest.c_str(),
        owner16, slot, bridge_name(bridge_kind_), host_ad_snd_ ? ", the host's AD_SND" : "");
  uint16_t r = bridge_->button(scratch_ + scratch::kPath, owner16, uint16_t(slot), scratch_ + scratch::kCtrl,
                               scratch_ + scratch::kError, scratch::kErrorSize, scratch_ + scratch::kErrId);
  uint16_t err_id = rt.rd16(scratch_ + scratch::kErrId);
  std::string err_text = rt.read_str(scratch_ + scratch::kError, scratch::kErrorSize);
  trace("lane", "BUTTONPUSHED16 -> %u (errId %u, error \"%s\")", r, err_id, err_text.c_str());
  out.ran = true;
  if (err_id) {
    out.failure =
        err_id == 1 ? "cannot load AD_SND.DLL" : err_id == 2 ? "AD_SND.DLL is too old" : "AD_SND.DLL lacks an entry point";
  }
  out.message = err_text;
  return out;
}

}  // namespace

std::unique_ptr<Protocol16> make_ad3_protocol(const Ne16Layout& layout) { return std::make_unique<Ad3Protocol>(layout); }

}  // namespace adw::ne16
