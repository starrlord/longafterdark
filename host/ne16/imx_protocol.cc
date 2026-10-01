// The Intermission module protocol (protocol.hh, lane.hh "Intermission
// (IMX)"): what Delrina's INTERMIS.EXE and INTRMLIB's LOADSAVER/FREESAVER did
// around an Intermission module — the saver's IMINFO record, the reader's
// LOAD and QUERY, one pass of INTERMIS's idle loop per call (the DC bracket
// around START once, then DRAW), its message loop between passes, the stop
// pass and FREE, and CONFIGURE for the button — over a reader (imreader.hh):
// Intermission's IMIMXPLY.IMQ as real code, or the native reader; for an ASA
// animation IMASAPLY.IMQ, and for an IMQ module the module itself (package.hh
// "Form"). Addresses are INTERMIS.EXE's and INTRMLIB.DLL's
// (intermission_protocol.md §3–§8).
#include <windows.h>

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

#include "adw/core/audio.h"
#include "adw/core/log.h"
#include "adw/core/text.h"
#include "ne16/imreader.hh"
#include "ne16/protocol.hh"
#include "win16/dos16.hh"
#include "win16/modules16.hh"
#include "win16/runtime16.hh"
#include "win16/shim_families16.hh"
#include "win16/shims16.hh"
#include "win32/vfs.hh"

namespace adw::ne16 {

using win16::Arg16;
using win16::GuestError16;
using win16::l16;
using win16::Runtime16;
using win16::w16;

namespace {

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

std::string hex32(uint32_t v) {
  char b[16];
  snprintf(b, sizeof(b), "%08" PRIX32, v);
  return b;
}

}  // namespace

// The guest's disk (INTERACTION.md §7.2), one 1994 machine with the package
// installed:
//   C:\SAVER           the module's folder: Intermission's saver directory
//                      (the installer's DestDir, INTRMLIB's default
//                      "c:\saver") and the current directory — the modules,
//                      INTRMLIB, ANTSW, SWSE and the DLLs they load by name,
//                      the MIDI files, SWTEXT.TXT (SWSE.INI names
//                      C:\SAVER\SWTEXT.TXT) — under a copy-on-write upper
//                      layer <state>\<package>\<MODDIR>
//   C:\WINDOWS         over the package's windows dir (SWSE.INI, as the
//                      installer put it there) and the runtime's virtual seed
//                      files, under <state>\<package>\WINDOWS: what the
//                      modules' dialogs write (SWSE.INI's sections) lands there
//   C:\WINDOWS\TEMP    memory, as the runtime made it (STRESS's temp file)
//   C:\WINDOWS\SYSTEM  the engine dir (IMIMXPLY.IMQ), read-only
//   H:\<L>\…           the host's drives, read-only, 8.3 names (file dialogs)
// Without ADSTATE every upper layer is memory. The profile seeds, read as
// seed ⊕ file and never written out (win16/dos16.hh seed_intermission):
// SYSTEM.INI's display driver; SWSE.INI [technology] = GDI when the module
// dir holds SWSE.DLL (a file rule), so SWSE draws with GDI and its DIB
// driver at once and never tries WinG; ANTSW.INI [Intermission] Volume and
// Saver Path.
void mount_imx_disk(Runtime16& rt, const Env& env, const Ne16Layout& layout, int volume) {
  const win16::Runtime16Options& o = rt.options();
  win32::Vfs& vfs = rt.vfs();
  std::string pkg = package_state_dir(env, layout.module_path);
  std::string moddir = file_of(layout.module_dir);
  if (env.state_persistent()) vfs.set_state_root(env.state_root);
  vfs.mount_overlay(o.windows_dir, layout.windows_dir, pkg.empty() ? "" : pkg + "\\WINDOWS");
  vfs.mount_overlay(o.guest_dir, layout.module_dir, pkg.empty() ? "" : pkg + "\\" + moddir);
  vfs.mount(o.system_dir, layout.engine_dir, /*writable=*/false);
  vfs.mount_host_drives(/*short_names=*/true);
  vfs.set_cwd(o.guest_dir);
  for (const std::string& d : layout.search_dirs) rt.modules().add_search_dir(d);
  win16::IntermissionSeeds seeds;
  seeds.volume = volume;
  seeds.swse_gdi = file_exists(layout.module_dir + "\\SWSE.DLL");
  win16::seed_intermission(rt, seeds);
  trace("lane", "disk: %s and C:\\WINDOWS over %s%s; seeds: ANTSW.INI Volume=%d%s", o.guest_dir.c_str(),
        pkg.empty() ? "memory (no ADSTATE)" : (pkg + " (" + moddir + ")").c_str(),
        layout.windows_dir.empty() ? "" : (", C:\\WINDOWS over " + layout.windows_dir).c_str(), volume,
        seeds.swse_gdi ? ", SWSE.INI [technology] GDI" : "");
}

namespace {

class ImxProtocol : public Protocol16 {
 public:
  ImxProtocol(const Ne16Layout& layout, ImxForm form)
      : layout_(layout), form_(form), module_name_(file_of(layout.module_path)) {}

  const char* name() const override { return reader_kind_ == ReaderKind::imq ? "imx/imq" : "imx/native"; }
  void configure_runtime(win16::Runtime16Options& opts, LaneContext& ctx) override;
  void mount(Runtime16& rt, const Env& env) override { mount_imx_disk(rt, env, layout_, volume_); }
  bool load(Runtime16& rt, uint16_t hwnd, uint16_t hdc, LaneContext& ctx) override;
  Call call() override;
  void after_call() override;
  // A key in the saver window's queue makes SWSE's USERABORT drop what the
  // module is loading (lane.hh "Intermission (IMX)"); Intermission woke on
  // every key but Ctrl, so the module was stopped along with it. Here the
  // saver's wake rules decide, and the keys they let through only reach the
  // key state.
  bool takes_key_messages() const override { return false; }
  // SWTEXT, TRENCH and HYPERSPC step once per pass (lane.hh "Pacing").
  bool carries_overruns() const override { return true; }
  // SWSE draws its full-screen scenes with GDI (StretchDIBits of its DIB
  // canvases: the host forces its GDI technology), a path 1994's machines
  // ran several times slower than the After Dark figure (lane.hh "Pacing").
  PixelCost pixel_cost() const override { return {"ADNE16IMXPIXCOST", kImxPixelCost}; }
  bool set_control(int index, int32_t value) override;
  void unload() override;
  bool check_button(int slot, std::string* why) override;
  void configure_button_runtime(win16::Runtime16Options& opts, const Env& env) override;
  Button button(Runtime16& rt, int slot, uint16_t owner16, LaneContext& ctx) override;
  void close() override;

 private:
  void choose_reader(const Env& env, bool quiet);
  bool open_reader(std::string* why);
  bool start(uint16_t hwnd, std::string* failure);
  void engine_palette(uint8_t type);
  uint32_t pass(uint16_t msg, bool send, bool palette);
  void free_saver();
  void free_record();
  uint32_t saver_main(uint16_t msg) { return reader_->saver_main(info_, msg); }
  uint32_t str(const std::string& s) { return rt_->static_bytes("ne16 imx protocol: " + s, s); }
  // A Win16 API call through its thunk, as INTERMIS's and INTRMLIB's imports reached it.
  uint32_t api(const char* module, const char* name, std::initializer_list<Arg16> args) {
    win16::Shim16Entry* e = rt_->shims().find_name(module, name);
    if (!e) throw GuestError16(GuestError16::Kind::fatal, std::string("ne16 imx protocol: no shim ") + module + "." + name);
    return rt_->call_far(rt_->thunk_far(*e), args);
  }

  Ne16Layout layout_;
  ImxForm form_ = ImxForm::imx;
  std::string module_name_;
  Runtime16* rt_ = nullptr;  // load's or button's
  std::unique_ptr<ImReader> reader_;
  ReaderKind reader_kind_ = ReaderKind::native;
  bool reader_auto_ = true;  // no ADNE16READER (the init trace says when it chose)
  ReaderFile reader_file_;
  std::string reader_path_;  // the IMQ's guest path, "" for the native reader
  int volume_ = 0;           // ANTSW.INI [Intermission] Volume
  uint16_t hwnd_ = 0;        // the saver window
  uint16_t h_info_ = 0, h_path_ = 0;
  uint32_t info_ = 0, path_ = 0;  // the record and its path block
  uint16_t hpal_ = 0;             // the engine palette (INTERMIS's window word 4), 0 for none
  uint16_t intrmlib_ = 0;         // INTRMLIB, when the engine palette was looked for in it
  bool loaded_ = false;           // LOAD succeeded, FREE not yet sent
  bool started_ = false;          // START returned (INTERMIS clears [0x2c0], 1:08f6)
  bool set_logged_ = false;
  uint64_t passes_ = 0, dispatched_ = 0;
};

// The reader (package.hh): for an IMX module ADNE16READER, else IMIMXPLY.IMQ
// when the package has one, else the native reader; for an ASA animation
// IMASAPLY.IMQ, and for an IMQ module the module itself (neither has a
// native reader).
void ImxProtocol::choose_reader(const Env& env, bool quiet) {
  ReaderKind forced = ReaderKind::imq;
  reader_auto_ = true;
  if (const std::string* r = env.get("ADNE16READER")) {
    if (!parse_reader_choice(*r, &reader_auto_, &forced)) {
      if (!quiet) log("ADNE16READER='%s' is not auto, imq or native; using auto", r->c_str());
      reader_auto_ = true;
    }
  }
  if (form_ == ImxForm::imx) {
    reader_file_ = find_reader(layout_, file_exists);
    reader_kind_ = reader_auto_ ? (reader_file_.host.empty() ? ReaderKind::native : ReaderKind::imq) : forced;
  } else {
    if (!reader_auto_ && forced == ReaderKind::native && !quiet) {
      log("%s: ADNE16READER=native is ignored: %s has no native reader", module_name_.c_str(),
          form_ == ImxForm::asa ? "an ASA animation" : "an IMQ module, its own reader,");
    }
    reader_auto_ = reader_auto_ || forced == ReaderKind::native;
    reader_kind_ = ReaderKind::imq;
    if (form_ == ImxForm::asa) {
      reader_file_ = find_reader(layout_, file_exists, kAsaReader);
    } else {
      reader_file_ = ReaderFile{layout_.module_path, false};
    }
  }
  if (!quiet && env.get("ADNE16BRIDGE")) {
    log("%s: ADNE16BRIDGE is ignored: an Intermission module has no After Dark bridge", module_name_.c_str());
  }
}

// Ne16Lane::init, before the runtime exists: the reader, Intermission's
// volume, and the runtime's guest dir and palette.
void ImxProtocol::configure_runtime(win16::Runtime16Options& opts, LaneContext& ctx) {
  const Env& env = ctx.env;
  choose_reader(env, /*quiet=*/false);
  // ANTSW.INI [Intermission] Volume (lane.hh "Intermission (IMX)"): the
  // engine's volume with sound on (ADSOUND=1 alone: ADVOLUME), and 0 — which
  // SWSE's INITSOUND reads as Intermission's "Off": no effects, no music —
  // without; the AD3 protocol's volume and mute, in Intermission's terms.
  bool on = env.flag("ADSOUND");
  int volume = int(std::min<uint64_t>(env_u64(env, "ADVOLUME", 50), 100));
  if (ctx.audio && ctx.audio->enabled()) {
    on = true;
    volume = std::clamp(ctx.audio->config().volume, 0, 100);
  }
  volume_ = on ? volume : 0;
  // Intermission's saver directory (the installer's DestDir), the modules'
  // current directory; the desktop's palette, from which SWSE builds its
  // identity palettes (_CREATESYSTEMPALETTE). ADDESKTOPPAL overrides (the lane).
  opts.guest_dir = "C:\\SAVER";
  opts.desktop_palette = true;
}

bool ImxProtocol::open_reader(std::string* why) {
  reader_path_.clear();
  if (reader_kind_ == ReaderKind::native) {
    reader_ = open_native_reader(*rt_, why);
    return reader_ != nullptr;
  }
  if (reader_file_.host.empty()) {
    const std::string where = "neither " + layout_.engine_dir + " nor " + layout_.module_dir + " holds ";
    *why = form_ == ImxForm::asa ? where + kAsaReader + ", the reader of ASA animations (there is no native one)"
                                 : "ADNE16READER=imq, but " + where + kImxReader;
    return false;
  }
  // An IMQ module is loaded by its own path, as LOADSAVER loaded a reader's
  // record: <saver dir>\<+0x44> (1:1fe1..1:201b).
  const win16::Runtime16Options& o = rt_->options();
  reader_path_ =
      (reader_file_.in_engine_dir ? o.system_dir : o.guest_dir) + "\\" + win16::upper16(file_of(reader_file_.host));
  reader_ = open_imq_reader(*rt_, reader_path_, why);
  return reader_ != nullptr;
}

// INTRMLIB's LOADSAVER (1:1fc0..1:21a7) and INTERMIS's StartSaver (6:0396)
// up to QUERY. One record serves where INTRMLIB kept two (the reader's entry
// took LOAD, then its block moved to the module's): IMIMXPLY and IMASAPLY
// read neither entry's index or file name. An IMQ module's record is a
// reader's (index −1, no path block, +0x63 = 0): LOADSAVER loaded the IMQ as
// itself and sent it LOAD with no path (1:1fd5..1:2064), and the QUERY that
// follows has no path either.
bool ImxProtocol::start(uint16_t hwnd, std::string* failure) {
  Runtime16& rt = *rt_;
  // The record as FINDALLMODULES made it (1:19fc: zeroed, GMEM_MOVEABLE,
  // locked), with INTRMLIB's defaults for an enabled module (1:2274..1:22b5),
  // the file name and the index of the one reader.
  h_info_ = uint16_t(api("KERNEL", "GlobalAlloc", {w16(0x0042), l16(iminfo::kSize)}));
  info_ = h_info_ ? api("KERNEL", "GlobalLock", {w16(h_info_)}) : 0;
  if (!info_) {
    *failure = "no guest memory for the saver's record";
    return false;
  }
  const bool own = form_ == ImxForm::imq;
  const std::string file = win16::upper16(module_name_);
  rt.wr32(info_ + iminfo::kFlags, iminfo::kModuleFlags);
  rt.write_str(info_ + iminfo::kFile, file, iminfo::kFileSize);
  rt.wr16(info_ + iminfo::kReaderIndex, own ? iminfo::kOwnReader : 0);
  const std::string guest = rt.options().guest_dir + "\\" + file;
  if (!own) {
    // The path block (1:213d..1:2193): <Saver Path>\<file>, 260 bytes.
    h_path_ = uint16_t(api("KERNEL", "GlobalAlloc", {w16(0x0042), l16(0x104)}));
    path_ = h_path_ ? api("KERNEL", "GlobalLock", {w16(h_path_)}) : 0;
    if (!path_) {
      *failure = "no guest memory for the saver's path";
      return false;
    }
    rt.write_str(path_, guest, 0x104);
    rt.wr32(info_ + iminfo::kPath, path_);
  }
  rt.wr16(info_ + iminfo::kReader, reader_->instance());
  rt.wr16(info_ + iminfo::kHwnd, hwnd);  // INTERMIS 6:054b, before LOADSAVER
  uint32_t r = saver_main(immsg::kLoad);
  trace("lane", "SAVERMAIN(10 load %s) -> %" PRIu32, own ? "with no path: the IMQ is its own reader" : guest.c_str(),
        r);
  loaded_ = true;  // FREE follows even a failed LOAD (1:2072)
  if (!r) {
    *failure = "the reader refused the module (SAVERMAIN(10) -> 0)";
    free_saver();
    return false;
  }
  r = saver_main(immsg::kQuery);
  if (!r) {
    // INTERMIS frees a saver whose QUERY failed (6:06ec) and picks another.
    *failure = "the reader's query failed (SAVERMAIN(7) -> 0)";
    free_saver();
    return false;
  }
  // INTERMIS listed only runnable savers: an IMQ whose QUERY clears 0x1000 is a reader (package.hh "Form").
  if (own && !(rt.rd32(info_ + iminfo::kFlags) & iminfo::kSaver)) {
    *failure = "an Intermission reader (its query does not make it a saver: flags " +
               hex32(rt.rd32(info_ + iminfo::kFlags)) + "), not a module";
    free_saver();
    return false;
  }
  return true;
}

// The engine palette, for a palette type w != 0 (no Star Wars module asks:
// their SAVERINIT writes 0). INTERMIS (6:0692..6:06bc) took IMCOPYPALETTE(w,
// 0) for w 2 or 3 and IMCOPYPALETTE(1, 0) for any other: a copy of the
// palette INTRMLIB's CANISTART(1) had made at INTERMIS's start-up
// (1:07bb..1:091d) from its custom resource "CLUT" (1), "HSV" (2) or "PRIM"
// (3), type = name, on an RC_PALETTE display. The lane runs no CANISTART —
// the rest of it is INTERMIS's (ANTSWINIT, its WH_GETMESSAGE wake hook) — so
// INTRMLIB holds no palette, and IMCOPYPALETTE would answer 0 (1:06de): the
// engine palette is made here as CANISTART made that one, through the thunks,
// from the INTRMLIB.DLL the guest loads.
void ImxProtocol::engine_palette(uint8_t type) {
  static const char* const kResource[] = {"CLUT", "HSV", "PRIM"};
  const uint16_t which = (type == 2 || type == 3) ? type : 1;
  const char* name = kResource[which - 1];
  uint16_t h = uint16_t(api("KERNEL", "LoadLibrary", {l16(str("INTRMLIB.DLL"))}));
  if (h >= 32) {
    intrmlib_ = h;
    const uint16_t desktop = uint16_t(api("USER", "GetDesktopWindow", {}));
    const uint16_t dc = uint16_t(api("USER", "GetDC", {w16(desktop)}));
    const bool palette_device = (api("GDI", "GetDeviceCaps", {w16(dc), w16(RASTERCAPS)}) & RC_PALETTE) != 0;
    api("USER", "ReleaseDC", {w16(desktop), w16(dc)});
    const uint32_t id = str(name);  // the type is the name
    const uint16_t res = palette_device ? uint16_t(api("KERNEL", "FindResource", {w16(h), l16(id), l16(id)})) : 0;
    if (const uint16_t mem = res ? uint16_t(api("KERNEL", "LoadResource", {w16(h), w16(res)})) : 0) {
      if (const uint32_t p = api("KERNEL", "LockResource", {w16(mem)})) {
        hpal_ = uint16_t(api("GDI", "CreatePalette", {l16(p)}));
        api("KERNEL", "GlobalUnlock", {w16(mem)});
      }
      api("KERNEL", "FreeResource", {w16(mem)});
    }
  }
  if (!hpal_) {
    log("%s: palette type %u: no %s palette in INTRMLIB.DLL; the module runs without an engine palette",
        module_name_.c_str(), type, name);
  }
}

bool ImxProtocol::load(Runtime16& rt, uint16_t hwnd, uint16_t hdc, LaneContext& ctx) {
  (void)hdc;  // every pass gets its own DC (GetDC), as INTERMIS's did
  rt_ = &rt;
  hwnd_ = hwnd;
  // The music's level (lane.hh "Sound"): Intermission set the effects' alone
  // (SWSE's waveOutSetVolume from ANTSW.INI's Volume), and the Windows mixer's
  // synth line the rest. With the engine on, the saver's volume stands in for
  // that line on the MIDI bus as After Dark's reaches it (AD_SND's
  // midiOutSetVolume, linear: 50 is half amplitude); the engine scales
  // MEMMIDI's CC7 by it.
  if (ctx.audio && ctx.audio->enabled()) {
    const uint32_t v = uint32_t(std::clamp(volume_, 0, 100)) * 0xFFFF / 100;
    ctx.audio->set_bus_gain(audio::Bus::midi, audio::gain_from_mm(v | (v << 16)), rt.peek_us());
  }
  std::string why;
  if (!open_reader(&why) || !start(hwnd, &why)) {
    log("%s: %s", module_name_.c_str(), why.c_str());
    return false;
  }
  const uint8_t type = rt.rd8(info_ + iminfo::kPaletteType);
  if (type) engine_palette(type);
  rt.wr8(info_ + iminfo::kShown, 1);  // 6:06e4
  // An input-taking saver (lane.hh "Intermission (IMX)"): INTERMIS captured
  // the mouse for it and showed its cursor (1:074d..1:076a), INTRMLIB's hook
  // sent it the input (SETEATMSGS(1), 6:0587); here the saver's own wake
  // rules decide, as for every Intermission module.
  if (rt.rd32(info_ + iminfo::kFlags) & iminfo::kTakesInput) {
    trace("lane", "%s: its QUERY says it takes input (flag 0x2000); it runs as a screen saver here, input ending it",
          module_name_.c_str());
  }
  const win16::Runtime16Options& o = rt.options();
  trace("lane",
        "%s: package %s, module dir %s, engine dir %s, windows dir %s, kind imx, form %s, reader %s%s (%s), \"%s\", "
        "palette type %u (engine palette %04X), flags %08" PRIX32 ", %s display palette",
        module_name_.c_str(), layout_.packaged ? layout_.package_id.c_str() : "legacy", layout_.module_dir.c_str(),
        layout_.engine_dir.c_str(), layout_.windows_dir.empty() ? "none" : layout_.windows_dir.c_str(),
        form_name(form_), reader_name(reader_kind_), reader_auto_ ? "" : " (ADNE16READER)",
        reader_path_.empty() ? "IMIMXPLY's dispatch in C++" : reader_path_.c_str(),
        rt.read_str(info_ + iminfo::kName, iminfo::kNameSize).c_str(), type, hpal_, rt.rd32(info_ + iminfo::kFlags),
        o.desktop_palette ? "desktop" : "boot");
  return true;
}

// One call inside INTERMIS's DC bracket (the idle loop's 1:0787..1:09ca; the
// stop pass's 6:1815..6:18e4 selects no palette): by the record's DC mode
// (flags & 0x70; 0 for every IMX module) GetDC and SaveDC, the engine palette
// selected and realized, +6 = the DC, +8 = the palette, SAVERMAIN(msg) when
// `send`, the old palette back, RestoreDC(-1), ReleaseDC — every one through
// the thunks, so a module that changed the DC finds it fresh at its next call.
uint32_t ImxProtocol::pass(uint16_t msg, bool send, bool palette) {
  Runtime16& rt = *rt_;
  const uint8_t mode = rt.rd8(info_ + iminfo::kFlags) & 0x70;
  uint16_t dc = 0, old = 0;
  if (mode != 0x20) dc = uint16_t(api("USER", "GetDC", {w16(hwnd_)}));
  if (mode == 0) api("GDI", "SaveDC", {w16(dc)});
  if (mode != 0x20 && palette && hpal_) {
    old = uint16_t(api("USER", "SelectPalette", {w16(dc), w16(hpal_), w16(0)}));
    api("USER", "RealizePalette", {w16(dc)});
  }
  rt.wr16(info_ + iminfo::kHdc, dc);
  rt.wr16(info_ + iminfo::kPalette, hpal_);
  uint32_t r = send ? saver_main(msg) : 1;
  // START has returned: the stop pass sends STOP from now on, even when this
  // pass is abandoned in the bracket's own calls below (a frame that ended
  // there and a run that closed meanwhile). INTERMIS clears [0x2c0] right
  // after the call, before RestoreDC (1:08f2..1:08f6).
  if (send && msg == immsg::kStart) started_ = true;
  if (mode != 0x20 && old) api("USER", "SelectPalette", {w16(dc), w16(old), w16(0)});
  if (mode == 0) api("GDI", "RestoreDC", {w16(dc), w16(0xFFFF)});
  if (mode != 0x20) {
    if (uint16_t d = rt.rd16(info_ + iminfo::kHdc)) api("USER", "ReleaseDC", {w16(hwnd_), w16(d)});
  }
  return r;
}

// One pass of INTERMIS's idle loop (1:06ca..1:09e3): START on the first,
// DRAW on every other. SAVERMAIN's result is not looked at (INTERMIS never
// did): an Intermission module never stops the run, takes no input as its
// own and asks for no cursor.
Protocol16::Call ImxProtocol::call() {
  pass(started_ ? immsg::kDraw : immsg::kStart, /*send=*/true, /*palette=*/true);
  passes_++;
  return Call{};
}

// INTERMIS's message loop between passes (1:09da..1:0a64): the guest's own
// posted messages to their windows, the timers that are due; what the module
// posted to its task (SWSE's FORCETOWAKE) is taken and counted, never a wake
// (lane.hh "Intermission (IMX)").
void ImxProtocol::after_call() { dispatched_ += uint64_t(win16::user16_dispatch_guest(*rt_)); }

bool ImxProtocol::set_control(int index, int32_t value) {
  if (!set_logged_) {
    log("%s: SET %d %d is ignored: an Intermission module has no controls (its Configure dialog keeps its settings)",
        module_name_.c_str(), index, value);
    set_logged_ = true;
  }
  return false;
}

// INTRMLIB's FREESAVER (1:21b2..1:2220): FREE (11), the path block, the reader.
void ImxProtocol::free_saver() {
  if (!info_ || !reader_) return;
  Runtime16& rt = *rt_;
  if (loaded_) {
    loaded_ = false;
    saver_main(immsg::kFree);
  }
  if (h_path_) {
    api("KERNEL", "GlobalUnlock", {w16(h_path_)});
    api("KERNEL", "GlobalFree", {w16(h_path_)});
    h_path_ = 0;
    path_ = 0;
    rt.wr32(info_ + iminfo::kPath, 0);
  }
  reader_->close();
  rt.wr16(info_ + iminfo::kReader, 0);
}

void ImxProtocol::free_record() {
  if (!h_info_) return;
  api("KERNEL", "GlobalUnlock", {w16(h_info_)});
  api("KERNEL", "GlobalFree", {w16(h_info_)});
  h_info_ = 0;
  info_ = 0;
}

// At shutdown, with no call suspended: INTERMIS's WM_USER+2 (6:17c2) — the
// stop pass, STOP inside it only once START was sent ([0x2c0], 6:1871) —,
// FREESAVER, and its WM_DESTROY's DeleteObject of the engine palette
// (6:1044..6:105b).
void ImxProtocol::unload() {
  if (!rt_ || !info_) return;
  if (loaded_) pass(immsg::kStop, /*send=*/started_, /*palette=*/false);
  free_saver();
  if (hpal_) {
    api("GDI", "DeleteObject", {w16(hpal_)});
    hpal_ = 0;
  }
  if (intrmlib_) {
    api("KERNEL", "FreeLibrary", {w16(intrmlib_)});
    intrmlib_ = 0;
  }
  free_record();
  trace("lane", "%s: %" PRIu64 " passes; %" PRIu64 " guest messages dispatched between them", module_name_.c_str(),
        passes_, dispatched_);
}

void ImxProtocol::close() {
  if (reader_) reader_->close();
}

// Configure mode (lane.hh "Configure"): the one button, slot 0.
bool ImxProtocol::check_button(int slot, std::string* why) {
  if (slot == 0) return true;
  *why = "control " + std::to_string(slot) + " is not a button";
  return false;
}

void ImxProtocol::configure_button_runtime(win16::Runtime16Options& opts, const Env& env) {
  choose_reader(env, /*quiet=*/true);
  volume_ = 0;  // no sound in configure mode: Intermission's "Off"
  opts.guest_dir = "C:\\SAVER";
  opts.desktop_palette = true;
}

// INTERMIS's control panel Configure (2:1cb2..2:1d7d): LOADSAVER (10), +4 =
// the owner, CONFIGURE (8) — IMIMXPLY's DialogBox(hLib, "DIALOGBOX", owner,
// SAVERDLGPROC), which runs the whole dialog inside the call —, FREESAVER
// (11), +4 = 0. QUERY (7) follows the load, as INTRMLIB's enumeration had
// sent it (GETSAVERINFO: 10, 7, 11) before the control panel offered the
// button; nothing starts the module (no 1).
Protocol16::Button ImxProtocol::button(Runtime16& rt, int slot, uint16_t owner16, LaneContext& ctx) {
  (void)slot;
  (void)ctx;
  rt_ = &rt;
  Button out;
  std::string why;
  if (!open_reader(&why)) {
    out.message = why;
    return out;
  }
  out.ran = true;
  if (!start(0, &why)) {
    out.failure = why;
    free_record();
    return out;
  }
  rt.wr16(info_ + iminfo::kHwnd, owner16);
  uint32_t r = saver_main(immsg::kConfigure);
  trace("lane", "%s: \"%s\": SAVERMAIN(8 configure, owner %04X) through the %s reader -> %" PRIu32,
        module_name_.c_str(), rt.read_str(info_ + iminfo::kName, iminfo::kNameSize).c_str(), owner16,
        reader_name(reader_kind_), r);
  if (!r) out.message = "the module has no Configure dialog (no SAVERDLGPROC)";
  free_saver();
  rt.wr16(info_ + iminfo::kHwnd, 0);
  free_record();
  return out;
}

}  // namespace

std::unique_ptr<Protocol16> make_imx_protocol(const Ne16Layout& layout, ImxForm form) {
  return std::make_unique<ImxProtocol>(layout, form);
}

}  // namespace adw::ne16
