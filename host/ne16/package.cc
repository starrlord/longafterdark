#include "ne16/package.hh"

#include <cctype>
#include <cstring>
#include <utility>

#include "adw/core/log.h"
#include "loader/ne.hh"
#include "loader/pe.hh"

namespace adw::ne16 {

namespace {

std::string dir_of(const std::string& p) {
  size_t s = p.find_last_of("\\/");
  return s == std::string::npos ? std::string() : p.substr(0, s);
}

std::string file_of(const std::string& p) {
  size_t s = p.find_last_of("\\/");
  return s == std::string::npos ? p : p.substr(s + 1);
}

bool ieq(const std::string& a, const char* b) {
  size_t n = strlen(b);
  if (a.size() != n) return false;
  for (size_t i = 0; i < n; i++) {
    if (toupper(uint8_t(a[i])) != toupper(uint8_t(b[i]))) return false;
  }
  return true;
}

uint16_t u16(std::string_view s, size_t off) {
  if (off + 2 > s.size()) return 0;
  return uint16_t(uint8_t(s[off]) | (uint8_t(s[off + 1]) << 8));
}

// A LOGPALETTE resource: WORD version, WORD count, count PALETTEENTRYs.
bool parse_logpalette(std::string_view d, std::vector<PALETTEENTRY>* out) {
  uint16_t n = u16(d, 2);
  if (!n || n > 256 || d.size() < 4 + size_t(n) * 4) return false;
  out->resize(n);
  memcpy(out->data(), d.data() + 4, size_t(n) * 4);
  return true;
}

}  // namespace

const char* bridge_name(BridgeKind k) { return k == BridgeKind::oldmod16 ? "oldmod16" : "native"; }

Ne16Layout resolve_layout(const std::string& path, const std::string& win, const FileExists& exists,
                          const FileExists& dir_exists) {
  Ne16Layout l;
  l.module_path = path;
  l.module_dir = dir_of(path);
  if (l.module_dir.empty()) l.module_dir = ".";
  std::string root = dir_of(l.module_dir);
  std::string above = root.empty() ? std::string() : dir_of(root);
  if (!root.empty() && !above.empty() && ieq(file_of(above), "packages")) {
    l.packaged = true;
    l.package_root = root;
    l.package_id = file_of(root);
    l.engine_dir = root + "\\ENGINE";
    // What the installer put in C:\WINDOWS: never a module folder, even the module's own.
    std::string windows = root + "\\WINDOWS";
    if (dir_exists && !ieq(file_of(l.module_dir), "WINDOWS") && dir_exists(windows)) l.windows_dir = windows;
    l.search_dirs = {l.module_dir, l.engine_dir};
    return l;
  }
  // Legacy: exactly the lane's original rule.
  std::string engine = win + "\\FILES\\ENGINE", classic = win + "\\FILES\\CLASSIC";
  if (!exists(engine + "\\OLDMOD16.DLL") && exists(l.module_dir + "\\OLDMOD16.DLL")) engine = l.module_dir;
  l.engine_dir = engine;
  l.search_dirs = {l.module_dir, classic, engine};
  return l;
}

bool parse_bridge_choice(const std::string& v, bool* is_auto, BridgeKind* forced) {
  *is_auto = true;
  if (v.empty() || ieq(v, "auto")) return true;
  if (ieq(v, "oldmod16")) {
    *is_auto = false;
    *forced = BridgeKind::oldmod16;
    return true;
  }
  if (ieq(v, "native")) {
    *is_auto = false;
    *forced = BridgeKind::native;
    return true;
  }
  return false;
}

BridgeKind choose_bridge(const Ne16Layout& l, const FileExists& exists) {
  return exists(l.engine_dir + "\\OLDMOD16.DLL") ? BridgeKind::oldmod16 : BridgeKind::native;
}

bool after_dark2(const Ne16Layout& l, const FileExists& exists) {
  return exists(l.module_dir + "\\" + kAfterDark2Library);
}

bool after_dark3_host(const Ne16Layout& l, const FileExists& exists) {
  return exists(l.engine_dir + "\\" + kAfterDark3Host);
}

bool host_ad_snd(const Ne16Layout& l, const FileExists& exists) { return !exists(l.engine_dir + "\\" + kAdSndLibrary); }

const char* kind_name(ModuleKind k) { return k == ModuleKind::ad3 ? "ad3" : k == ModuleKind::imx ? "imx" : "scr"; }

const char* form_name(ImxForm f) {
  switch (f) {
    case ImxForm::imx: return "imx";
    case ImxForm::asa: return "asa";
    case ImxForm::imq: return "imq";
    case ImxForm::fli: return "fli";
    case ImxForm::flc: return "flc";
    case ImxForm::mrf: return "mrf";
    case ImxForm::msv: return "msv";
  }
  return "?";
}

bool data_form(ImxForm f) { return f != ImxForm::imx && f != ImxForm::imq; }

bool form_for_type(const char* type, ImxForm* form) {
  static const std::pair<const char*, ImxForm> kTypes[] = {
      {"FLI", ImxForm::fli}, {"FLC", ImxForm::flc}, {"MRF", ImxForm::mrf}, {"MSV", ImxForm::msv}};
  for (const auto& [t, f] : kTypes) {
    if (type && ieq(type, t)) {
      *form = f;
      return true;
    }
  }
  return false;
}

KindProbe detect_kind(const loader::ne::Image& img, const std::string& file_name) {
  auto exports = [&](const char* name) { return img.find_ordinal(name).has_value(); };
  KindProbe p;
  if (!img.header().is_dll()) {
    // An application: a Windows 3.1 screen saver (SCRNSAVE.LIB's export), or nothing the lane runs.
    if (exports("SCREENSAVERPROC")) {
      p.ok = true;
      p.kind = ModuleKind::scr;
    } else {
      p.why = "a Win16 application that is not a Windows 3.1 screen saver (no SCREENSAVERPROC export)";
    }
    return p;
  }
  if (exports("MODULE")) {
    p.ok = true;
    p.kind = ModuleKind::ad3;
    return p;
  }
  const bool init = exports("SAVERINIT"), draw = exports("SAVERDRAW");
  if (init && draw) {
    // IMIMXPLY's own refusals (2:03c7..2:0404, 2:044d..2:045c): a module
    // that exports SETCURRSAVER, and a file named IMXX_*.
    if (exports("SETCURRSAVER")) {
      p.why = "an Intermission module that exports SETCURRSAVER, which the IMX reader refuses";
    } else if (file_name.size() >= 5 && ieq(file_name.substr(0, 5), "IMXX_")) {
      p.why = "an Intermission module named IMXX_*, which the IMX reader refuses";
    } else {
      p.ok = true;
      p.kind = ModuleKind::imx;
    }
    return p;
  }
  if (exports("SAVERMAIN")) {
    // An .IMQ: its own reader; its QUERY says whether it is a saver (the protocol checks).
    p.ok = true;
    p.kind = ModuleKind::imx;
    p.form = ImxForm::imq;
  } else if (init || draw) {
    p.why = std::string("not an Intermission module: it exports ") + (init ? "SAVERINIT" : "SAVERDRAW") + " without " +
            (init ? "SAVERDRAW" : "SAVERINIT");
  } else {
    p.why = "not an After Dark or Intermission module (no MODULE, SAVERINIT or SAVERDRAW export)";
  }
  return p;
}

bool parse_kind_choice(const std::string& v, bool* is_auto, ModuleKind* forced) {
  *is_auto = true;
  if (v.empty() || ieq(v, "auto")) return true;
  if (ieq(v, "ad3") || ieq(v, "imx") || ieq(v, "scr")) {
    *is_auto = false;
    *forced = ieq(v, "ad3") ? ModuleKind::ad3 : ieq(v, "imx") ? ModuleKind::imx : ModuleKind::scr;
    return true;
  }
  return false;
}

const char* reader_name(ReaderKind k) { return k == ReaderKind::imq ? "imq" : "native"; }

bool parse_reader_choice(const std::string& v, bool* is_auto, ReaderKind* forced) {
  *is_auto = true;
  if (v.empty() || ieq(v, "auto")) return true;
  if (ieq(v, "imq") || ieq(v, "native")) {
    *is_auto = false;
    *forced = ieq(v, "imq") ? ReaderKind::imq : ReaderKind::native;
    return true;
  }
  return false;
}

const char* reader_file(ImxForm f) {
  switch (f) {
    case ImxForm::imx: return kImxReader;
    case ImxForm::asa: return kAsaReader;
    case ImxForm::imq: return nullptr;
    case ImxForm::fli: return kFliReader;
    case ImxForm::flc: return kFlcReader;
    case ImxForm::mrf: return kMrfReader;
    case ImxForm::msv: return kMsvReader;
  }
  return nullptr;
}

ReaderFile find_reader(const Ne16Layout& l, const FileExists& exists, const char* file) {
  ReaderFile r;
  if (exists(l.engine_dir + "\\" + file)) {
    r.host = l.engine_dir + "\\" + file;
    r.in_engine_dir = true;
  } else if (exists(l.module_dir + "\\" + file)) {
    r.host = l.module_dir + "\\" + file;
  }
  return r;
}

AdPalettes palettes_from_scr(const std::string& scr) {
  AdPalettes out;
  try {
    loader::pe::Image img = loader::pe::Image::from_file(scr);
    for (uint16_t id = 101; id <= 104; id++) {
      const loader::pe::Resource* r = img.find_resource(loader::ResId::of("AD_PALETTE"), loader::ResId::of(id));
      std::vector<PALETTEENTRY> e;
      if (!r || !parse_logpalette(img.resource_data(*r), &e)) {
        out.error = scr + ": no AD_PALETTE " + std::to_string(id);
        out.pal.clear();
        return out;
      }
      out.pal.push_back(std::move(e));
    }
  } catch (const std::exception& e) {
    out.error = scr + ": " + e.what();
    out.pal.clear();
    return out;
  }
  out.source = scr + " AD_PALETTE 101..104";
  return out;
}

AdPalettes palettes_from_adtask(const std::string& adtask) {
  AdPalettes out;
  try {
    loader::ne::Image img = loader::ne::Image::from_file(adtask);
    // SETADPALETTE16 index i takes 5000/kOrder[i]: OLDMOD16's palette request
    // 10+k selects hpal[1, 3, 0, 2][k], and the AD 3.2 survey matched
    // AFTERDAR.SCR's AD_PALETTE 102/104/101/103 to 5000/1..4 byte for byte.
    static constexpr uint16_t kOrder[4] = {3, 1, 4, 2};
    for (uint16_t id : kOrder) {
      const loader::ne::Resource* r = img.find_resource(loader::ResId::of(5000), loader::ResId::of(id));
      std::vector<PALETTEENTRY> e;
      if (!r || !parse_logpalette(img.resource_data(*r), &e)) {
        out.error = adtask + ": no palette resource 5000/" + std::to_string(id);
        out.pal.clear();
        return out;
      }
      out.pal.push_back(std::move(e));
    }
  } catch (const std::exception& e) {
    out.error = adtask + ": " + e.what();
    out.pal.clear();
    return out;
  }
  out.source = adtask + " 5000/1..4";
  return out;
}

// After Dark 2.0's palettes (package.hh; ABI.md §3.9). The algorithm, and
// only that, of the handlers AD.EXE 2.0b ran for the palette requests 10–13
// (13:2589): each builds a 235-entry LOGPALETTE of PC_RESERVED entries.
AdPalettes palettes_after_dark2() {
  constexpr int kEntries = 235;
  auto entry = [](int r, int g, int b) { return PALETTEENTRY{BYTE(r & 0xFF), BYTE(g & 0xFF), BYTE(b & 0xFF), PC_RESERVED}; };
  // 10: the hue sweep, through AD.EXE's HSV → RGB (13:1c1b): six sectors of
  // the 16-bit hue circle, 0x2AAA or 0x2AAB wide, between p = v·(1 − s) and
  // v; with full saturation and value, p = 0 and v = 255.
  auto hsv = [&](int h, int s, int v) {
    const int p = (v * (0x10000 - s)) >> 16, d = v - p;
    if (h <= 0x2AAA) return entry(v, h * d / 0x2AAA + p, p);
    if (h <= 0x5555) return entry(v - (h - 0x2AAA) * d / 0x2AAB, v, p);
    if (h <= 0x7FFF) return entry(p, v, (h - 0x5555) * d / 0x2AAA + p);
    if (h <= 0xAAAA) return entry(p, v - (h - 0x7FFF) * d / 0x2AAB, v);
    if (h <= 0xD554) return entry((h - 0xAAAA) * d / 0x2AAA + p, p, v);
    return entry(v, p, v - (h - 0xD554) * d / 0x2AAB);
  };
  std::vector<PALETTEENTRY> p10, p11, p12, p13;
  for (int i = 0, h = 0x217; i < kEntries; i++, h = (h + 0x11D) & 0xFFFF) p10.push_back(hsv(h, 0xFFFF, 0xFF));
  // 11: the cube, blue fastest, then the greys.
  const int levels[6] = {255, 204, 153, 102, 51, 0};
  for (int i = 0; i < 216; i++) p11.push_back(entry(levels[i / 36], levels[i / 6 % 6], levels[i % 6]));
  for (int g = 0xFF; p11.size() < size_t(kEntries); g = (g + 13) & 0xFF) p11.push_back(entry(g, g, g));
  // 12: the grey ramp (a step of 256 / 235 = 1).
  for (int i = 0; i < kEntries; i++) p12.push_back(entry(i, i, i));
  // 13: seven ramps, each from its colour down by 245 / 33 = 7 per entry in
  // the channels it has (orange's green by 3); a ramp ends after 34 entries.
  struct Ramp {
    int r, g, b, dr, dg, db;
  };
  const Ramp ramps[7] = {{255, 255, 255, 7, 7, 7}, {255, 0, 0, 7, 0, 0},   {255, 128, 0, 7, 3, 0}, {255, 255, 0, 7, 7, 0},
                         {0, 255, 0, 0, 7, 0},     {0, 0, 255, 0, 0, 7}, {255, 0, 255, 7, 0, 7}};
  for (int i = 0; i < kEntries; i++) {
    const Ramp& k = ramps[i / 34];
    const int n = i % 34;
    p13.push_back(entry(k.r - n * k.dr, k.g - n * k.dg, k.b - n * k.db));
  }
  AdPalettes out;
  out.pal = {std::move(p12), std::move(p10), std::move(p13), std::move(p11)};
  out.source = "After Dark 2.0's four, computed as its AD.EXE computed them";
  out.computed = true;
  return out;
}

AdPalettes load_palettes(const Ne16Layout& l, BridgeKind bridge, const FileExists& exists) {
  std::string scr = l.engine_dir + "\\AFTERDAR.SCR";
  if (bridge == BridgeKind::native) {
    std::string adtask = l.engine_dir + "\\ADTASK.DLL";
    if (exists(adtask)) {
      AdPalettes p = palettes_from_adtask(adtask);
      if (!p.pal.empty() || !exists(scr)) return p;
      log("ne16: %s; using %s", p.error.c_str(), scr.c_str());
    } else if (!exists(scr)) {
      // Neither file (package.hh): After Dark 2.0's four, as its host computed them.
      return palettes_after_dark2();
    }
  }
  if (!exists(scr)) {
    AdPalettes p;
    p.error = bridge == BridgeKind::native ? l.engine_dir + " holds neither ADTASK.DLL nor AFTERDAR.SCR"
                                           : scr + " is missing";
    return p;
  }
  return palettes_from_scr(scr);
}

}  // namespace adw::ne16
