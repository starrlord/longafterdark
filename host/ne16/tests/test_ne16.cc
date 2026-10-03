// adw_lane_ne16 tests.
//
//   adw_ne16_tests                        control-record defaults (synthetic records); the
//                                         package rule, bridge choice and palette sources
//                                         (package.hh), After Dark 2.0's computed palettes
//                                         against the test's own reading of ABI.md §3.9; the
//                                         native AD3 bridge (bridge.hh) driving a
//                                         host-implemented AD_SND (AD 3.x's entry set, and
//                                         AD_SND 1.0's) and module; the host's own AD_SND
//                                         (win16/adsnd16.cc) under a made-up NE module that
//                                         imports it by name, directly and through the AD3
//                                         protocol for a package without an engine dir; the
//                                         protocol seam (protocol.hh): the lane driving a
//                                         scripted protocol — what it does by the
//                                         protocol's answers (KEY lines, carried overruns
//                                         and their bound, the pixel cost) and, through
//                                         run_host, the audio engine under a
//                                         timer-sequenced guest — and the AD3 protocol
//                                         through Protocol16 on the same host-implemented
//                                         modules; the module's kind, the windows dir and
//                                         the reader choice (synthetic NE images); the
//                                         Intermission readers (imreader.hh) and the IMX
//                                         protocol (imx_protocol.cc) on a host-implemented
//                                         module and reader, and an INTRMLIB of palettes
//   adw_ne16_tests --assets <adhostwin>   runs Classic modules headless through
//                                         adhostwin (exit 77 when the assets are absent):
//                                         each must load through OLDMOD16 and draw frames
//                                         without the host failing, and two runs must
//                                         produce the same FBHASH stream; the bridge
//                                         oracle (native vs oldmod16, ADMIPS=0) on a few;
//                                         sound captured headless (music gates and
//                                         MM_MCINOTIFY, MS-ADPCM, synchronous sounds)
//   adw_ne16_tests --pkg <adhostwin>      the package roots of PACKAGES.md §4.4 under
//                                         AD_NE16_PKGROOTS (exit 77 when unset/absent):
//                                         one module per package, standalone
//   adw_ne16_tests --interaction <adhostwin>  the games' status, DOS Shell's long run, the
//                                         module buttons in configure mode (Globe's map
//                                         picked through [-h-] in both releases, loaded by
//                                         a later run)
//   adw_ne16_tests --swse <adhostwin>     Star Wars Screen Entertainment's Intermission
//                                         modules from an imported package
//                                         (AD_NE16_SWSE_ROOT, else AD_ASSETS_DIR or the
//                                         installed assets; exit 77 without one): every
//                                         module twice, keys the saver does not wake on,
//                                         the per-pass modules' rate, the reader oracle, a
//                                         small screen, the desktop seed, streamed, sound
//                                         (the music at the saver's volume), a button
//   adw_ne16_tests --startrek <adhostwin> Star Trek: The Screen Saver's After Dark 2.0
//                                         modules from an imported package
//                                         (AD_NE16_STARTREK_ROOT, else AD_ASSETS_DIR or the
//                                         installed assets; exit 77 without one): every
//                                         module twice, sound captured twice, Scotty's
//                                         Files' blueprints, the two buttons (Sounder's
//                                         folder chosen through [-h-], played by a later
//                                         run), a scripted Final Exam (Num Lock, answers,
//                                         the mouse, the wake)
#include <windows.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "adw/core/audio.h"
#include "adw/core/clock.h"
#include "adw/core/env.h"
#include "adw/core/host.h"
#include "adw/core/protocol.h"
#include "adw/core/screen.h"
#include "test_paths.h"
#include "loader/ne.hh"
#include "ne16/bridge.hh"
#include "ne16/imreader.hh"
#include "ne16/lane.hh"
#include "ne16/package.hh"
#include "ne16/protocol.hh"
#include "win16/dos16.hh"
#include "win16/gdi16.hh"
#include "win16/input16.hh"
#include "win16/modules16.hh"
#include "win16/runtime16.hh"
#include "win16/shim_families16.hh"
#include "win16/sound16.hh"
#include "win32/ini_store.hh"
#include "win32/vfs.hh"

using namespace adw;

namespace {

int failures = 0, checks = 0;

#define CHECK(cond, ...)                          \
  do {                                            \
    checks++;                                     \
    if (!(cond)) {                                \
      printf("FAIL %s:%d: ", __FILE__, __LINE__); \
      printf(__VA_ARGS__);                        \
      printf("\n");                               \
      failures++;                                 \
    }                                             \
  } while (0)

void put16(std::string& s, size_t at, uint16_t v) {
  if (s.size() < at + 2) s.resize(at + 2, '\0');
  s[at] = char(v);
  s[at + 1] = char(v >> 8);
}

int run_unit() {
  // Kind 1: stops 25/50/75/100 (no 0 → one is prepended); default 60 → 50.
  std::string r1(0x20, '\0');
  put16(r1, 0, 1);
  put16(r1, 0x16, 4);
  put16(r1, 0x18, 60);
  r1.resize(0x20 + 4 * 16, '\0');
  for (int i = 0; i < 4; i++) put16(r1, 0x20 + 64 + 2 * i, uint16_t(25 * (i + 1)));
  CHECK(ne16::control_default16(r1) == 50, "string slider default stop (%d)", ne16::control_default16(r1));
  put16(r1, 0x18, 10);
  CHECK(ne16::control_default16(r1) == 0, "below the first stop: the prepended 0");
  // Kind 2: numeric 1..9, default 22 → 9 (Hard Rain's "# of Drops").
  std::string r2(0x34, '\0');
  put16(r2, 0, 2);
  put16(r2, 0x18, 22);
  put16(r2, 0x30, 1);
  put16(r2, 0x32, 9);
  CHECK(ne16::control_default16(r2) == 9, "numeric default clamped");
  // Kind 3: popup of 3, default 7 → 2; kind 5 checkbox.
  std::string r3(0x20, '\0');
  put16(r3, 0, 3);
  put16(r3, 0x16, 3);
  put16(r3, 0x18, 7);
  CHECK(ne16::control_default16(r3) == 2, "popup default clamped");
  std::string r5(0x20, '\0');
  put16(r5, 0, 5);
  put16(r5, 0x18, 9);
  CHECK(ne16::control_default16(r5) == 1, "checkbox default");
  // Small screens (lane.hh): the smallest whole factor that reaches 640x480.
  CHECK(ne16::Ne16Lane::auto_guest_scale(640, 480) == 1, "640x480: no guest scaling");
  CHECK(ne16::Ne16Lane::auto_guest_scale(856, 480) == 1, "856x480: no guest scaling");
  CHECK(ne16::Ne16Lane::auto_guest_scale(320, 240) == 2, "320x240: x2");
  CHECK(ne16::Ne16Lane::auto_guest_scale(640, 400) == 2, "640x400: x2 (height)");
  CHECK(ne16::Ne16Lane::auto_guest_scale(152, 112) == 5, "152x112: x5");
  CHECK(ne16::Ne16Lane::auto_guest_scale(1, 1) == 8, "1x1: at most x8");
  return 0;
}

// ---- the package rule (PACKAGES.md §7.1/§7.3) ---------------------------------------------------------------

void test_layout() {
  using ne16::BridgeKind;
  std::map<std::string, bool> files;
  auto exists = [&](const std::string& p) {
    std::string u = p;
    for (char& ch : u) ch = char(toupper(uint8_t(ch)));
    return files.count(u) != 0;
  };
  auto add = [&](const std::string& p) {
    std::string u = p;
    for (char& ch : u) ch = char(toupper(uint8_t(ch)));
    files[u] = true;
  };
  const std::string win = "C:\\A\\win";
  // A packaged module: its package root is the parent of its folder, whose parent is "packages".
  ne16::Ne16Layout p = ne16::resolve_layout(win + "\\packages\\ad32\\AD32\\GUTS.AD", win, exists);
  CHECK(p.packaged && p.package_id == "ad32", "packaged (%d, %s)", p.packaged, p.package_id.c_str());
  CHECK(p.engine_dir == win + "\\packages\\ad32\\ENGINE", "engine dir %s", p.engine_dir.c_str());
  CHECK(p.search_dirs.size() == 2 && p.search_dirs[0] == win + "\\packages\\ad32\\AD32" &&
            p.search_dirs[1] == p.engine_dir,
        "packaged search: module dir, engine dir");
  CHECK(ne16::resolve_layout("D:\\x\\PACKAGES\\tt\\TWISTED\\CHAM.AD", win, exists).packaged,
        "\"packages\" matches in any case, anywhere");
  // No OLDMOD16 in the engine dir: the native bridge; with it, OLDMOD16.
  CHECK(ne16::choose_bridge(p, exists) == BridgeKind::native, "ad32: native bridge");
  add(win + "\\packages\\ad10\\ENGINE\\OLDMOD16.DLL");
  CHECK(ne16::choose_bridge(ne16::resolve_layout(win + "\\packages\\ad10\\AD10TH\\CHAM.AD", win, exists), exists) ==
            BridgeKind::oldmod16,
        "ad10: OLDMOD16");
  // Legacy: Deluxe's FILES tree, exactly the lane's original rule.
  add(win + "\\FILES\\ENGINE\\OLDMOD16.DLL");
  ne16::Ne16Layout l = ne16::resolve_layout(win + "\\FILES\\CLASSIC\\TOAST3.AD", win, exists);
  CHECK(!l.packaged && l.engine_dir == win + "\\FILES\\ENGINE", "legacy engine dir %s", l.engine_dir.c_str());
  CHECK(l.search_dirs.size() == 3 && l.search_dirs[1] == win + "\\FILES\\CLASSIC" && l.search_dirs[2] == l.engine_dir,
        "legacy search: module dir, CLASSIC, ENGINE");
  CHECK(ne16::choose_bridge(l, exists) == BridgeKind::oldmod16, "Deluxe: OLDMOD16");
  // A lone module with OLDMOD16 beside it, and no Deluxe engine.
  files.clear();
  add("E:\\lone\\OLDMOD16.DLL");
  ne16::Ne16Layout lone = ne16::resolve_layout("E:\\lone\\X.AD", win, exists);
  CHECK(!lone.packaged && lone.engine_dir == "E:\\lone", "lone module: its own folder is the engine dir");
  // "packages" must be the grandparent of the module dir, not any ancestor.
  CHECK(!ne16::resolve_layout(win + "\\packages\\ad32\\AD32\\SUB\\X.AD", win, exists).packaged, "a deeper folder is legacy");
  // After Dark 2.0: a module folder holding AD_MOD.DLL, by file (any package, any case); one
  // in the engine dir or elsewhere in the package does not count.
  const std::string st = win + "\\packages\\startrek";
  ne16::Ne16Layout stl = ne16::resolve_layout(st + "\\AFTERDRK\\FINAL.AD", win, exists);
  CHECK(!ne16::after_dark2(stl, exists), "no AD_MOD.DLL: not After Dark 2.0");
  add(st + "\\ENGINE\\AD_MOD.DLL");
  add(st + "\\AD_MOD.DLL");
  CHECK(!ne16::after_dark2(stl, exists), "AD_MOD.DLL in the engine dir or the package root: not After Dark 2.0");
  add(st + "\\AFTERDRK\\ad_mod.dll");
  CHECK(ne16::after_dark2(stl, exists) && ne16::choose_bridge(stl, exists) == BridgeKind::native,
        "AD_MOD.DLL beside the module: After Dark 2.0, on the native bridge");
  CHECK(ne16::after_dark2(ne16::resolve_layout(st + "\\AFTERDRK\\SOUNDER.AD", win, exists), exists),
        "a module that does not import AD_MOD (Sounder) is After Dark 2.0's all the same");
  CHECK(!ne16::after_dark2(ne16::resolve_layout(win + "\\packages\\ad32\\AD32\\GUTS.AD", win, exists), exists),
        "an AD 3.2 module: not After Dark 2.0");
  // After Dark 3.x's host: an engine dir holding ADW30.EXE, by file (any package, any case);
  // one beside the module does not count.
  const std::string tw = win + "\\packages\\tt";
  ne16::Ne16Layout twl = ne16::resolve_layout(tw + "\\TWISTED\\CHAM.AD", win, exists);
  CHECK(!ne16::after_dark3_host(twl, exists), "no ADW30.EXE: no After Dark 3.x host");
  add(tw + "\\TWISTED\\ADW30.EXE");
  CHECK(!ne16::after_dark3_host(twl, exists), "ADW30.EXE beside the module: not the engine's");
  add(tw + "\\ENGINE\\adw30.exe");
  CHECK(ne16::after_dark3_host(twl, exists) && ne16::choose_bridge(twl, exists) == BridgeKind::native,
        "ADW30.EXE in the engine dir: After Dark 3.x's, on the native bridge");
  CHECK(!ne16::after_dark3_host(stl, exists) && !ne16::after_dark3_host(l, exists),
        "Star Trek's engine dir and Deluxe's: no ADW30.EXE");
  // The host's AD_SND: an engine dir without AD_SND.DLL (or none at all), by
  // file; one beside the module does not count.
  const std::string sn = win + "\\packages\\snoopy";
  ne16::Ne16Layout snl = ne16::resolve_layout(sn + "\\AFTERDRK\\IS_FLY.AD", win, exists);
  CHECK(ne16::host_ad_snd(snl, exists), "no ENGINE\\AD_SND.DLL: the host's AD_SND");
  add(sn + "\\AFTERDRK\\AD_SND.DLL");
  CHECK(ne16::host_ad_snd(snl, exists), "AD_SND.DLL beside the module: still the host's");
  add(sn + "\\ENGINE\\ad_snd.dll");
  CHECK(!ne16::host_ad_snd(snl, exists), "AD_SND.DLL in the engine dir, any case: that one");
  // ADNE16BRIDGE.
  bool is_auto = false;
  BridgeKind k = BridgeKind::native;
  CHECK(ne16::parse_bridge_choice("", &is_auto, &k) && is_auto, "empty = auto");
  CHECK(ne16::parse_bridge_choice("Native", &is_auto, &k) && !is_auto && k == BridgeKind::native, "native");
  CHECK(ne16::parse_bridge_choice("OLDMOD16", &is_auto, &k) && !is_auto && k == BridgeKind::oldmod16, "oldmod16");
  CHECK(!ne16::parse_bridge_choice("thunk", &is_auto, &k), "anything else is refused");
}

// A minimal NE image: no segments, the given resources, and exported names in
// the resident and non-resident name tables (the kind rule, package.hh: only
// the names count).
struct NeRes {
  uint16_t type, id;
  std::string data;
  std::string type_name, id_name;  // a string type or name instead of the number (INTRMLIB's "CLUT"/"CLUT")
};
struct NeNames {
  std::vector<std::pair<std::string, uint16_t>> resident, nonresident;  // name, ordinal
};
std::string ne_image(const std::string& module, const std::vector<NeRes>& res, const NeNames& names = {},
                     uint16_t flags = 0x8001) {
  std::string f(0x40, '\0');
  f[0] = 'M';
  f[1] = 'Z';
  put16(f, 0x3C, 0x40);
  std::string h(0x40, '\0');
  h[0] = 'N';
  h[1] = 'E';
  // Resource table: shift 4, one type block per resource (a type may repeat),
  // the end of types, then the string types' and names' Pascal strings (a
  // string id is its offset from the table's start).
  std::string rt;
  put16(rt, 0, 4);
  size_t data_at = 0;  // filled below
  std::vector<size_t> offs;
  const size_t strings_at = 2 + res.size() * (8 + 12) + 2;
  std::string strings;
  auto id_field = [&](uint16_t num, const std::string& name) {
    if (name.empty()) return uint16_t(0x8000 | num);
    uint16_t at = uint16_t(strings_at + strings.size());
    strings += char(name.size());
    strings += name;
    return at;
  };
  for (size_t i = 0; i < res.size(); i++) {
    size_t p = rt.size();
    rt.resize(p + 8 + 12, '\0');
    put16(rt, p, id_field(res[i].type, res[i].type_name));
    put16(rt, p + 2, 1);
    offs.push_back(p + 8);
    put16(rt, p + 8 + 4, 0x30);  // MOVEABLE|PURE
    put16(rt, p + 8 + 6, id_field(res[i].id, res[i].id_name));
  }
  rt.resize(rt.size() + 2, '\0');  // end of types
  if (!strings.empty()) rt += strings + '\0';
  // Name tables: entry 0 is the module name (resident) / description (non-resident).
  auto name_entry = [](std::string& t, const std::string& n, uint16_t ordinal) {
    t += char(n.size());
    t += n;
    t += char(ordinal & 0xFF);
    t += char(ordinal >> 8);
  };
  std::string resident;
  name_entry(resident, module, 0);
  for (const auto& [n, o] : names.resident) name_entry(resident, n, o);
  resident += '\0';
  std::string nonresident;
  if (!names.nonresident.empty()) {
    name_entry(nonresident, module + " (a test image)", 0);
    for (const auto& [n, o] : names.nonresident) name_entry(nonresident, n, o);
    nonresident += '\0';
  }
  std::string imp(1, '\0'), entry(2, '\0');
  uint16_t off = 0x40;
  uint16_t rt_off = off;
  off += uint16_t(rt.size());
  uint16_t resident_off = off;
  off += uint16_t(resident.size());
  uint16_t modref_off = off, imp_off = off;
  off += uint16_t(imp.size());
  uint16_t entry_off = off;
  off += uint16_t(entry.size());
  uint16_t nonresident_off = off;  // a file offset in the header: the NE header is at 0x40
  off += uint16_t(nonresident.size());
  if (!nonresident.empty()) {
    put16(h, 0x20, uint16_t(nonresident.size()));
    put16(h, 0x2C, uint16_t(0x40 + nonresident_off));
  }
  put16(h, 0x04, entry_off);
  put16(h, 0x06, uint16_t(entry.size()));
  put16(h, 0x0C, flags);  // LIBRARY | SINGLEDATA unless the caller says otherwise
  put16(h, 0x22, 0x40);  // segment table (empty)
  put16(h, 0x24, rt_off);
  put16(h, 0x26, resident_off);
  put16(h, 0x28, modref_off);
  put16(h, 0x2A, imp_off);
  put16(h, 0x32, 4);
  h[0x36] = 2;
  put16(h, 0x3E, 0x030A);
  std::string all = f + h + rt + resident + imp + entry + nonresident;
  data_at = (all.size() + 15) & ~size_t(15);
  for (size_t i = 0; i < res.size(); i++) {
    all.resize(data_at, '\0');
    size_t len = (res[i].data.size() + 15) & ~size_t(15);
    put16(all, 0x80 + offs[i], uint16_t(data_at >> 4));
    put16(all, 0x80 + offs[i] + 2, uint16_t(len >> 4));
    all += res[i].data;
    data_at += len;
  }
  all.resize(data_at, '\0');
  return all;
}

// A LOGPALETTE resource of n entries whose red channel is `tag`, green the index.
std::string logpal_res(uint8_t tag, uint16_t n) {
  std::string d;
  put16(d, 0, 0x300);
  put16(d, 2, n);
  for (uint16_t i = 0; i < n; i++) d += std::string{char(tag), char(i), 0, char(PC_RESERVED)};
  return d;
}

std::string temp_dir(const char* tag) {
  char base[MAX_PATH];
  GetTempPathA(MAX_PATH, base);
  std::string d = std::string(base) + "adw_ne16_" + tag + "_" + std::to_string(GetCurrentProcessId());
  CreateDirectoryA(d.c_str(), nullptr);
  return d;
}

void write_file(const std::string& path, const std::string& bytes) {
  FILE* fh = fopen(path.c_str(), "wb");
  fwrite(bytes.data(), 1, bytes.size(), fh);
  fclose(fh);
}

void test_palettes() {
  std::string dir = temp_dir("pal");
  std::string adtask = dir + "\\ADTASK.DLL";
  write_file(adtask, ne_image("ADTASK", {{5000, 1, logpal_res(1, 235)},
                                         {5000, 2, logpal_res(2, 235)},
                                         {5000, 3, logpal_res(3, 235)},
                                         {5000, 4, logpal_res(4, 235)},
                                         {5000, 5, logpal_res(5, 244)}}));
  ne16::AdPalettes p = ne16::palettes_from_adtask(adtask);
  CHECK(p.pal.size() == 4, "four palettes from ADTASK (%s)", p.error.c_str());
  if (p.pal.size() == 4) {
    // SETADPALETTE16 index i ← 5000/{3,1,4,2}[i], so request 10+k selects 5000/(k+1).
    CHECK(p.pal[0][0].peRed == 3 && p.pal[1][0].peRed == 1 && p.pal[2][0].peRed == 4 && p.pal[3][0].peRed == 2,
          "hpal[0..3] = 5000/3, 5000/1, 5000/4, 5000/2 (%u %u %u %u)", p.pal[0][0].peRed, p.pal[1][0].peRed,
          p.pal[2][0].peRed, p.pal[3][0].peRed);
    CHECK(p.pal[0].size() == 235 && p.pal[0][200].peGreen == 200 && p.pal[0][200].peFlags == PC_RESERVED,
          "235 PC_RESERVED entries, as the resource has them");
  }
  // The native bridge's source: ADTASK in the engine dir; else AFTERDAR.SCR.
  auto exists = [](const std::string& f) {
    DWORD a = GetFileAttributesA(f.c_str());
    return a != INVALID_FILE_ATTRIBUTES;
  };
  ne16::Ne16Layout l;
  l.engine_dir = dir;
  ne16::AdPalettes n = ne16::load_palettes(l, ne16::BridgeKind::native, exists);
  CHECK(n.pal.size() == 4 && n.source.find("ADTASK.DLL 5000/1..4") != std::string::npos && !n.computed, "native: %s",
        n.source.c_str());
  ne16::AdPalettes o = ne16::load_palettes(l, ne16::BridgeKind::oldmod16, exists);
  CHECK(o.pal.empty() && o.error.find("AFTERDAR.SCR") != std::string::npos, "OLDMOD16 wants AFTERDAR.SCR (%s)",
        o.error.c_str());
  // A broken ADTASK (4 missing) gives no palettes, and says so.
  write_file(adtask, ne_image("ADTASK", {{5000, 1, logpal_res(1, 235)}, {5000, 2, logpal_res(2, 235)}}));
  ne16::AdPalettes b = ne16::palettes_from_adtask(adtask);
  CHECK(b.pal.empty() && b.error.find("5000/") != std::string::npos, "missing resource: %s", b.error.c_str());
  DeleteFileA(adtask.c_str());
  RemoveDirectoryA(dir.c_str());
}

// After Dark 2.0's four palettes (package.hh palettes_after_dark2), checked
// against this test's own reading of ABI.md §3.9 — written apart from the
// host's code, and no byte from Berkeley's files: 235 PC_RESERVED entries
// each; request 10 a hue sweep (h = 0x217 + 0x11D·i, 16-bit, full
// saturation and value) through six sectors of the hue circle, each ending
// at 0x2AAA, 0x5555, 0x7FFF, 0xAAAA, 0xD554 and 0xFFFF and running its
// channel up or down over the sector's width (0x2AAA, or 0x2AAB for the
// falling ones); 11 the 6×6×6 cube of 255 − 51·k (blue fastest), then 19
// greys, 255 and then 12 + 13·k; 12 the grey ramp; 13 seven ramps of 34
// entries — white, red, orange, yellow, green, blue, magenta — each from its
// colour down by 245 / 33 = 7 in its channels (orange's green by 3), the
// last one cut to 31. SETADPALETTE16's order: hpal[0..3] = 12, 10, 13, 11.
void test_ad2_palettes() {
  using Pal = std::vector<PALETTEENTRY>;
  auto rgb = [](int r, int g, int b) { return PALETTEENTRY{BYTE(r), BYTE(g), BYTE(b), PC_RESERVED}; };
  Pal hue, cube, grey, ramps;
  struct Sector {
    uint32_t last, base, width;
    int rising;          // the channel that runs up (0 r, 1 g, 2 b), or -1
    int falling;         // … or down, or -1
    int full[2];         // the channels at 255 (-1: none)
  };
  const Sector sectors[6] = {{0x2AAA, 0x0000, 0x2AAA, 1, -1, {0, -1}}, {0x5555, 0x2AAA, 0x2AAB, -1, 0, {1, -1}},
                             {0x7FFF, 0x5555, 0x2AAA, 2, -1, {1, -1}}, {0xAAAA, 0x7FFF, 0x2AAB, -1, 1, {2, -1}},
                             {0xD554, 0xAAAA, 0x2AAA, 0, -1, {2, -1}}, {0xFFFF, 0xD554, 0x2AAB, -1, 2, {0, -1}}};
  for (uint32_t i = 0; i < 235; i++) {
    uint32_t h = (0x217 + 0x11D * i) % 0x10000;
    const Sector* s = sectors;
    while (h > s->last) s++;
    int c[3] = {0, 0, 0};
    for (int f : s->full) {
      if (f >= 0) c[f] = 255;
    }
    uint32_t ramp = (h - s->base) * 255 / s->width;
    if (s->rising >= 0) c[s->rising] = int(ramp);
    if (s->falling >= 0) c[s->falling] = int(255 - ramp);
    hue.push_back(rgb(c[0], c[1], c[2]));
  }
  for (int r = 0; r < 6; r++) {
    for (int g = 0; g < 6; g++) {
      for (int b = 0; b < 6; b++) cube.push_back(rgb(255 - 51 * r, 255 - 51 * g, 255 - 51 * b));
    }
  }
  cube.push_back(rgb(255, 255, 255));
  for (int k = 0; k < 18; k++) cube.push_back(rgb(12 + 13 * k, 12 + 13 * k, 12 + 13 * k));
  for (int i = 0; i < 235; i++) grey.push_back(rgb(i, i, i));
  const int starts[7][3] = {{255, 255, 255}, {255, 0, 0}, {255, 128, 0}, {255, 255, 0}, {0, 255, 0}, {0, 0, 255}, {255, 0, 255}};
  for (int k = 0; k < 7; k++) {
    for (int n = 0; n < 34 && ramps.size() < 235; n++) {
      int c[3];
      for (int ch = 0; ch < 3; ch++) c[ch] = starts[k][ch] ? starts[k][ch] - n * (k == 2 && ch == 1 ? 3 : 7) : 0;
      ramps.push_back(rgb(c[0], c[1], c[2]));
    }
  }
  auto same = [](const Pal& a, const Pal& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
      if (memcmp(&a[i], &b[i], sizeof(PALETTEENTRY)) != 0) return false;
    }
    return true;
  };
  ne16::AdPalettes p = ne16::palettes_after_dark2();
  CHECK(p.pal.size() == 4 && p.error.empty() && p.computed && p.source.find("computed") != std::string::npos,
        "four palettes, marked computed (%s)", p.source.c_str());
  if (p.pal.size() != 4) return;
  CHECK(hue.size() == 235 && cube.size() == 235 && ramps.size() == 235, "the test's own palettes: 235 entries each");
  CHECK(same(p.pal[0], grey), "hpal[0]: request 12, the grey ramp");
  CHECK(same(p.pal[1], hue), "hpal[1]: request 10, the hue sweep");
  CHECK(same(p.pal[2], ramps), "hpal[2]: request 13, the seven ramps");
  CHECK(same(p.pal[3], cube), "hpal[3]: request 11, the cube and its greys");
  // A few entries worked out by hand from the rule.
  const PALETTEENTRY& h0 = p.pal[1][0];  // h 0x217 in the first sector: green 0x217·255/0x2AAA = 12
  const PALETTEENTRY& m = p.pal[2][234];  // magenta's 31st: 255 − 30·7 = 45
  CHECK(h0.peRed == 255 && h0.peGreen == 12 && h0.peBlue == 0 && m.peRed == 45 && m.peGreen == 0 && m.peBlue == 45 &&
            p.pal[3][216].peRed == 255 && p.pal[3][217].peRed == 12 && p.pal[3][234].peRed == 233 &&
            p.pal[2][102].peRed == 255 && p.pal[2][102].peGreen == 255 && p.pal[2][102].peBlue == 0,
        "by hand: hue 0 (%u %u %u), magenta's last (%u %u %u), the greys, yellow's first", h0.peRed, h0.peGreen,
        h0.peBlue, m.peRed, m.peGreen, m.peBlue);
  // The native bridge's source: with neither ADTASK.DLL nor AFTERDAR.SCR in
  // the engine dir (or no engine dir), these; the real OLDMOD16 still wants
  // AFTERDAR.SCR, and an ADTASK.DLL still wins.
  auto none = [](const std::string&) { return false; };
  ne16::Ne16Layout l;
  l.engine_dir = "C:\\nowhere\\ENGINE";
  ne16::AdPalettes n = ne16::load_palettes(l, ne16::BridgeKind::native, none);
  CHECK(n.pal.size() == 4 && same(n.pal[0], grey) && same(n.pal[3], cube) && n.source == p.source && n.computed,
        "native, neither file: computed (%s)", n.source.c_str());
  ne16::AdPalettes o = ne16::load_palettes(l, ne16::BridgeKind::oldmod16, none);
  CHECK(o.pal.empty() && o.error.find("AFTERDAR.SCR") != std::string::npos, "OLDMOD16, neither file: none (%s)",
        o.error.c_str());
  auto adtask_only = [](const std::string& f) { return f.size() > 10 && f.substr(f.size() - 10) == "ADTASK.DLL"; };
  ne16::AdPalettes a = ne16::load_palettes(l, ne16::BridgeKind::native, adtask_only);
  CHECK(a.pal.empty() && a.source.empty() && a.error.find("ADTASK.DLL") != std::string::npos,
        "native, an ADTASK.DLL (unreadable here): that one, never the computed ones (%s)", a.error.c_str());
}

// ---- the native bridge ---------------------------------------------------------------------------------------
//
// AD_SND and the module are host-implemented system "modules" (shims), so a
// test sees every call the bridge makes and scripts every result.
struct BridgeRig {
  VirtualClock clock{VirtualClock::Mode::fixed_step, 16667};
  win16::Runtime16 rt{win16::Runtime16Options{}, clock};
  Screen screen{64, 48};
  std::vector<std::string> calls;
  std::map<uint16_t, std::vector<uint16_t>> results;  // MODULE(msg) results, in turn
  uint16_t hsys = 0;
  uint32_t module_text = 0;  // a string the module points AD_SYSTEM+0x22 at, when set
  bool want_snd = true;

  BridgeRig() {
    clock.set_read_step_us(5);
    win16::register_all16(rt);
    rt.attach_display(screen);
    using win16::Call16;
    using win16::Conv16;
    auto& r = rt.shims();
    auto snd = [&](const char* mod, uint16_t ord, const char* name, int bytes) {
      r.add(mod, ord, name, Conv16::pascal_, true, bytes, [this, name, bytes](Call16& c) {
        std::string s = name;
        if (bytes == 2) s += "(" + std::to_string(c.w()) + ")";
        if (s == "ADWGETSYSTEMVOLUMES") c.rt.wr16(c.ptr(), 0x1234);
        calls.push_back(s);
        c.ret(0);
      });
    };
    for (const char* m : {"FAKESND", "HALFSND"}) {
      snd(m, 11, "ADWSOUNDINIT", 6);
      snd(m, 9, "ADWSOUNDCLEANUP", 0);
      snd(m, 101, "ADWGETSYSTEMVOLUMES", 4);
      snd(m, 102, "ADWSETSYSTEMVOLUMES", 2);
      snd(m, 29, "ADWSETVOLUME", 2);
      snd(m, 8, "ADWSETSOUNDMUTE", 2);
    }
    snd("FAKESND", 26, "ADWSTOPSOUND", 0);  // HALFSND lacks it
    // FAKESND has After Dark 2.0's volume pair too: the bridge must keep to the first.
    snd("FAKESND", 20, "ADWSAVEPREVIOUSVOLUME", 0);
    snd("FAKESND", 10, "ADWRESTOREPREVIOUSVOLUME", 0);
    // AD_SND 1.0's entry set (After Dark 2.0): the five common entries and its
    // own volume pair, no adwGetSystemVolumes/adwSetSystemVolumes; SND10HALF
    // lacks adwRestorePreviousVolume.
    for (const char* m : {"SND10", "SND10HALF"}) {
      snd(m, 11, "ADWSOUNDINIT", 6);
      snd(m, 9, "ADWSOUNDCLEANUP", 0);
      snd(m, 29, "ADWSETVOLUME", 2);
      snd(m, 8, "ADWSETSOUNDMUTE", 2);
      snd(m, 26, "ADWSTOPSOUND", 0);
      snd(m, 20, "ADWSAVEPREVIOUSVOLUME", 0);
    }
    snd("SND10", 10, "ADWRESTOREPREVIOUSVOLUME", 0);
    // SND10NOSTOP: AD_SND 1.0's whole pair and every entry but adwStopSound.
    snd("SND10NOSTOP", 11, "ADWSOUNDINIT", 6);
    snd("SND10NOSTOP", 9, "ADWSOUNDCLEANUP", 0);
    snd("SND10NOSTOP", 29, "ADWSETVOLUME", 2);
    snd("SND10NOSTOP", 8, "ADWSETSOUNDMUTE", 2);
    snd("SND10NOSTOP", 20, "ADWSAVEPREVIOUSVOLUME", 0);
    snd("SND10NOSTOP", 10, "ADWRESTOREPREVIOUSVOLUME", 0);
    r.add("FAKEMOD", 1, "MODULE", Conv16::pascal_, true, 6, [this](Call16& c) {
      uint16_t msg = c.w();
      c.w();
      hsys = c.w();
      calls.push_back("MODULE(" + std::to_string(msg) + ")");
      uint32_t sys = c.rt.global().lock(hsys);
      if (msg == 5 && want_snd) c.rt.wr16(c.rt.global().lock(c.rt.rd16(sys + 0x20)) + 0x1E, 1);  // bWantSnd
      if (module_text) c.rt.wr32(sys + 0x22, module_text);
      uint16_t v = 0;
      auto& q = results[msg];
      if (!q.empty()) {
        v = q.front();
        q.erase(q.begin());
      }
      c.ret(v);
    });
  }
  std::string joined() {
    std::string s;
    for (auto& x : calls) s += (s.empty() ? "" : " ") + x;
    return s;
  }
};

void test_native_bridge() {
  BridgeRig g;
  win16::Runtime16& rt = g.rt;
  uint16_t hwnd = win16::user16_saver_window(rt);
  uint16_t hdc = win16::gdi16_screen_dc(rt, hwnd);
  std::string why;
  auto b = ne16::open_native_bridge(rt, "C:\\WINDOWS\\SYSTEM\\FAKESND.DLL", &why);
  CHECK(b != nullptr, "the bridge attaches (%s)", why.c_str());
  if (!b) return;
  // The lane's scratch block: ctrl4, errId, path, err.
  uint16_t hs = rt.global().alloc(win16::GlobalHeap16::kZeroInit, ne16::scratch::kSize);
  uint32_t s = uint32_t(hs) << 16;
  const int16_t ctrl[4] = {7, -1, 30, 1};
  for (uint32_t i = 0; i < 4; i++) rt.wr16(s + ne16::scratch::kCtrl + 2 * i, uint16_t(ctrl[i]));
  rt.write_str(s + ne16::scratch::kPath, "C:\\AFTERDRK\\FAKEMOD.AD", 260);
  // Four palettes: entry 0's red channel tells them apart.
  auto& gdi = rt.state<win16::Gdi16>();
  uint16_t hpal[4];
  for (uint16_t i = 0; i < 4; i++) {
    std::vector<PALETTEENTRY> pe(235, PALETTEENTRY{uint8_t(100 + i), 0, 0, PC_RESERVED});
    hpal[i] = gdi.create_palette(pe);
    b->set_palette(hpal[i], i);
  }
  // INITIALIZE asks for palette 12 (→ hpal[0]); BLANK succeeds.
  g.results[0] = {12};
  uint16_t ok = b->load(hwnd, hdc, s + ne16::scratch::kCtrl, 40, 1, s + ne16::scratch::kPath,
                        s + ne16::scratch::kError, ne16::scratch::kErrorSize, s + ne16::scratch::kErrId);
  CHECK(ok == 1, "LOADADMODULE16 -> %u", ok);
  CHECK(g.joined() ==
            "ADWSOUNDINIT ADWGETSYSTEMVOLUMES MODULE(5) ADWSETSOUNDMUTE(1) ADWSETVOLUME(40) MODULE(12) MODULE(0) "
            "MODULE(1)",
        "the OLDMOD16 load sequence: %s", g.joined().c_str());
  uint32_t sys = rt.global().lock(g.hsys);
  uint32_t mod = b->ad_module();
  CHECK(rt.rd16(sys) == 2 && rt.rd16(sys + 0x14) == 300 && rt.rd16(sys + 0x06) == 64 && rt.rd16(sys + 0x08) == 48,
        "AD_SYSTEM: 2, version 300, the screen size");
  CHECK(rt.rd16(sys + 0x02) == 4 && rt.rd16(sys + 0x04) == 1 && rt.rd16(sys + 0x0A) == 8 && rt.rd16(sys + 0x2A) == 1,
        "AD_SYSTEM: a 486 with an FPU, 8 bits per pixel, a palette device");
  std::string sig;
  for (uint32_t i = 0; i < 8; i++) sig += char(rt.rd16(sys + 0x2C + 2 * i));
  CHECK(sig == "BUTTHEAD", "AD_SYSTEM signature %s", sig.c_str());
  CHECK(rt.global().lock(rt.rd16(sys + 0x20)) == mod && rt.rd32(sys + 0x22) == s + ne16::scratch::kError,
        "AD_SYSTEM: hADModule, the error buffer");
  CHECK(int16_t(rt.rd16(mod + 6)) == 7 && int16_t(rt.rd16(mod + 8)) == -1 && rt.rd16(mod + 10) == 30,
        "AD_MODULE: the control values");
  CHECK(rt.rd16(mod + 0x0E) == 1 && rt.rd16(mod + 0x14) == 4 && rt.rd16(mod + 2) == 64 && rt.rd16(mod + 4) == 48,
        "AD_MODULE: control ids 1..4, the region size");
  CHECK(rt.rd16(mod + 0x16) != 0 && rt.rd16(mod) != 0, "AD_MODULE: hModule, hDrawRgn");
  CHECK(rt.rd16(mod + 0x18) == hpal[0], "palette request 12 selected hpal[0]");
  uint32_t lp = rt.rd32(mod + 0x1A);
  CHECK(rt.rd16(lp) == 0x300 && rt.rd16(lp + 2) == 256 && rt.rd8(lp + 4) == 100 && rt.rd8(lp + 4 + 4 * 234) == 100 &&
            rt.rd8(lp + 4 + 4 * 235) == 0,
        "lpLogPalette: {0x300, 256}, 235 entries copied, the rest zero");
  // DRAWFRAME answering RESTART: INITIALIZE + BLANK again.
  g.calls.clear();
  g.results[2] = {3, 13, 0};
  uint16_t r = b->message(2, s + ne16::scratch::kError, ne16::scratch::kErrorSize);
  CHECK(r == 0 && g.joined() == "MODULE(2) MODULE(0) MODULE(1)", "RESTART re-initializes: %u %s", r, g.joined().c_str());
  r = b->message(2, s + ne16::scratch::kError, ne16::scratch::kErrorSize);
  CHECK(r == 0 && rt.rd16(mod + 0x18) == hpal[2] && rt.rd8(rt.rd32(mod + 0x1A) + 4) == 102,
        "palette request 13 selected hpal[2]");
  // An error text the module points AD_SYSTEM+0x22 at is copied back.
  g.module_text = rt.static_bytes("test module text", "It broke.");
  g.results[2] = {9};
  r = b->message(2, s + ne16::scratch::kError, ne16::scratch::kErrorSize);
  CHECK(r == 9 && rt.read_str(s + ne16::scratch::kError) == "It broke.", "the module's error text: %u '%s'", r,
        rt.read_str(s + ne16::scratch::kError).c_str());
  g.module_text = 0;
  // Controls: the sound calls only when volume/mute change.
  g.calls.clear();
  rt.wr16(s + ne16::scratch::kCtrl, 99);
  b->set_controls(40, 1, s + ne16::scratch::kCtrl);
  CHECK(g.calls.empty() && rt.rd16(mod + 6) == 99, "same volume: no sound calls, controls copied");
  b->set_controls(70, 0, s + ne16::scratch::kCtrl);
  CHECK(g.joined() == "ADWSETSOUNDMUTE(0) ADWSETVOLUME(70)", "new volume: %s", g.joined().c_str());
  // Unload: CLOSE, then AD_SND restored and released.
  g.calls.clear();
  b->unload();
  CHECK(g.joined() == "MODULE(3) ADWSTOPSOUND ADWSETSYSTEMVOLUMES(4660) ADWSOUNDCLEANUP", "unload: %s",
        g.joined().c_str());
  b->close();
  CHECK(rt.modules().by_name("FAKEMOD") == nullptr || rt.modules().by_name("FAKEMOD")->system, "module freed");

  // Error ids: an AD_SND that is missing (1) or lacks an entry point (3).
  auto b2 = ne16::open_native_bridge(rt, "C:\\WINDOWS\\SYSTEM\\NOSUCH.DLL", &why);
  uint16_t ok2 = b2->load(hwnd, hdc, s + ne16::scratch::kCtrl, 40, 1, s + ne16::scratch::kPath,
                          s + ne16::scratch::kError, ne16::scratch::kErrorSize, s + ne16::scratch::kErrId);
  CHECK(ok2 == 0 && rt.rd16(s + ne16::scratch::kErrId) == 1, "no AD_SND: error id 1 (%u)",
        rt.rd16(s + ne16::scratch::kErrId));
  b2->close();
  auto b3 = ne16::open_native_bridge(rt, "C:\\WINDOWS\\SYSTEM\\HALFSND.DLL", &why);
  uint16_t ok3 = b3->load(hwnd, hdc, s + ne16::scratch::kCtrl, 40, 1, s + ne16::scratch::kPath,
                          s + ne16::scratch::kError, ne16::scratch::kErrorSize, s + ne16::scratch::kErrId);
  CHECK(ok3 == 0 && rt.rd16(s + ne16::scratch::kErrId) == 3, "AD_SND without adwStopSound: error id 3 (%u)",
        rt.rd16(s + ne16::scratch::kErrId));
  b3->close();
  // MODULESELECTED failing: the module is unloaded again, and AD_SND with it.
  auto b4 = ne16::open_native_bridge(rt, "C:\\WINDOWS\\SYSTEM\\FAKESND.DLL", &why);
  g.calls.clear();
  g.results[5] = {7};
  uint16_t ok4 = b4->load(hwnd, hdc, s + ne16::scratch::kCtrl, 40, 1, s + ne16::scratch::kPath,
                          s + ne16::scratch::kError, ne16::scratch::kErrorSize, s + ne16::scratch::kErrId);
  CHECK(ok4 == 0 && g.joined() == "ADWSOUNDINIT ADWGETSYSTEMVOLUMES MODULE(5) ADWSTOPSOUND ADWSETSYSTEMVOLUMES(4660) "
                                  "ADWSOUNDCLEANUP",
        "MODULESELECTED failing: %s", g.joined().c_str());
  b4->close();

  // BUTTONPUSHED16 (OLDMOD16 1:09e4): AD_SND, the module, AD_SYSTEM+0x26 =
  // the owner, SELECTED, the four values into AD_MODULE, MODULE(7 + slot),
  // the module's error text copied back, freed — no PREINITIALIZE, BLANK or
  // CLOSE; 1 unless SELECTED or the button answered 1 or 7.
  auto b5 = ne16::open_native_bridge(rt, "C:\\WINDOWS\\SYSTEM\\FAKESND.DLL", &why);
  g.calls.clear();
  g.results.clear();
  const int16_t bctrl[4] = {11, 12, -13, 14};
  for (uint32_t i = 0; i < 4; i++) rt.wr16(s + ne16::scratch::kCtrl + 2 * i, uint16_t(bctrl[i]));
  rt.write_str(s + ne16::scratch::kError, "stale", 16);
  uint16_t r5 = b5->button(s + ne16::scratch::kPath, 0xC004, 2, s + ne16::scratch::kCtrl, s + ne16::scratch::kError,
                           ne16::scratch::kErrorSize, s + ne16::scratch::kErrId);
  CHECK(r5 == 1 && g.joined() == "ADWSOUNDINIT ADWGETSYSTEMVOLUMES MODULE(5) MODULE(9) ADWSTOPSOUND "
                                 "ADWSETSYSTEMVOLUMES(4660) ADWSOUNDCLEANUP",
        "BUTTONPUSHED16 slot 2: %u %s", r5, g.joined().c_str());
  uint32_t sys5 = rt.global().lock(g.hsys);
  uint32_t mod5 = b5->ad_module();
  CHECK(rt.rd16(sys5 + 0x26) == 0xC004, "AD_SYSTEM+0x26 = the owner (%04X)", rt.rd16(sys5 + 0x26));
  CHECK(int16_t(rt.rd16(mod5 + 6)) == 11 && int16_t(rt.rd16(mod5 + 10)) == -13 && rt.rd16(mod5 + 12) == 14,
        "the four values reached AD_MODULE");
  CHECK(rt.read_str(s + ne16::scratch::kError).empty(), "the error buffer starts empty");
  // SELECTED answering 7: no button message, result 0; the button answering
  // with an error text: copied back.
  g.calls.clear();
  g.results[5] = {7};
  uint16_t r6 = b5->button(s + ne16::scratch::kPath, 0, 0, s + ne16::scratch::kCtrl, s + ne16::scratch::kError,
                           ne16::scratch::kErrorSize, s + ne16::scratch::kErrId);
  CHECK(r6 == 0 && g.joined().find("MODULE(7)") == std::string::npos, "SELECTED 7: no button (%u %s)", r6,
        g.joined().c_str());
  g.module_text = rt.static_bytes("test button text", "No pictures.");
  g.results[8] = {9};
  uint16_t r7 = b5->button(s + ne16::scratch::kPath, 0, 1, s + ne16::scratch::kCtrl, s + ne16::scratch::kError,
                           ne16::scratch::kErrorSize, s + ne16::scratch::kErrId);
  CHECK(r7 == 1 && rt.read_str(s + ne16::scratch::kError) == "No pictures.", "the button's error text: %u '%s'", r7,
        rt.read_str(s + ne16::scratch::kError).c_str());
  g.module_text = 0;
  b5->close();

  // Deferred palettes (Bridge16::defer_palettes): handed over at the first
  // palette request, once; a module that asks for none never gets them, and
  // the bridge makes none of their calls.
  auto b6 = ne16::open_native_bridge(rt, "C:\\WINDOWS\\SYSTEM\\FAKESND.DLL", &why);
  int supplied = 0;
  uint16_t dpal[4] = {0, 0, 0, 0};
  b6->defer_palettes([&] {
    supplied++;
    for (uint16_t i = 0; i < 4; i++) {
      std::vector<PALETTEENTRY> pe(235, PALETTEENTRY{uint8_t(110 + i), 0, 0, PC_RESERVED});
      dpal[i] = gdi.create_palette(pe);
      b6->set_palette(dpal[i], i);
    }
  });
  win16::Shim16Entry* gpe = rt.shims().find_name("GDI", "GetPaletteEntries");
  const uint64_t gpe0 = gpe->calls;
  g.results.clear();
  uint16_t ok6 = b6->load(hwnd, hdc, s + ne16::scratch::kCtrl, 40, 1, s + ne16::scratch::kPath,
                          s + ne16::scratch::kError, ne16::scratch::kErrorSize, s + ne16::scratch::kErrId);
  uint32_t mod6 = b6->ad_module();
  CHECK(ok6 == 1 && supplied == 0 && gpe->calls == gpe0 && rt.rd16(mod6 + 0x18) == 0,
        "deferred palettes: no request, not handed over (%d, %llu calls)", supplied,
        (unsigned long long)(gpe->calls - gpe0));
  g.results[2] = {13, 10};
  b6->message(2, s + ne16::scratch::kError, ne16::scratch::kErrorSize);
  CHECK(supplied == 1 && gpe->calls - gpe0 == 4 && rt.rd16(mod6 + 0x18) == dpal[2] &&
            rt.rd8(rt.rd32(mod6 + 0x1A) + 4) == 112,
        "deferred palettes: the first request (13) hands them over, and selects hpal[2]");
  b6->message(2, s + ne16::scratch::kError, ne16::scratch::kErrorSize);
  CHECK(supplied == 1 && gpe->calls - gpe0 == 4 && rt.rd16(mod6 + 0x18) == dpal[1],
        "deferred palettes: once only; request 10 selects hpal[1]");
  b6->unload();
  b6->close();
}

// AD_SND 1.0 (After Dark 2.0: Star Trek: The Screen Saver) has no
// adwGetSystemVolumes/adwSetSystemVolumes; the bridge takes its
// adwSavePreviousVolume()/adwRestorePreviousVolume() instead, called where the
// first pair is (as AD.EXE 2.0b called them), and looks them up only when the
// first pair is incomplete, so an AD 3.x/4.x AD_SND sees the calls it always did.
void test_native_bridge_ad_snd10() {
  BridgeRig g;
  win16::Runtime16& rt = g.rt;
  uint16_t hwnd = win16::user16_saver_window(rt);
  uint16_t hdc = win16::gdi16_screen_dc(rt, hwnd);
  uint16_t hs = rt.global().alloc(win16::GlobalHeap16::kZeroInit, ne16::scratch::kSize);
  uint32_t s = uint32_t(hs) << 16;
  rt.write_str(s + ne16::scratch::kPath, "C:\\AFTERDRK\\FAKEMOD.AD", 260);
  win16::Shim16Entry* gpa = rt.shims().find_name("KERNEL", "GetProcAddress");
  std::string why;
  auto load = [&](ne16::Bridge16& b) {
    return b.load(hwnd, hdc, s + ne16::scratch::kCtrl, 40, 1, s + ne16::scratch::kPath, s + ne16::scratch::kError,
                  ne16::scratch::kErrorSize, s + ne16::scratch::kErrId);
  };
  // AD_SND 1.0: its pair where OLDMOD16 calls the first; four lookups more.
  auto b = ne16::open_native_bridge(rt, "C:\\WINDOWS\\SYSTEM\\SND10.DLL", &why);
  CHECK(b != nullptr, "the bridge attaches (%s)", why.c_str());
  if (!b) return;
  uint64_t lookups = gpa->calls;
  CHECK(load(*b) == 1, "AD_SND 1.0: LOADADMODULE16 loads (error id %u)", rt.rd16(s + ne16::scratch::kErrId));
  CHECK(g.joined() == "ADWSOUNDINIT ADWSAVEPREVIOUSVOLUME MODULE(5) ADWSETSOUNDMUTE(1) ADWSETVOLUME(40) MODULE(12) "
                      "MODULE(0) MODULE(1)",
        "AD_SND 1.0: adwSavePreviousVolume where adwGetSystemVolumes was: %s", g.joined().c_str());
  CHECK(gpa->calls - lookups == 10, "AD_SND 1.0: seven lookups, its pair, MODULE (%llu)",
        (unsigned long long)(gpa->calls - lookups));
  g.calls.clear();
  b->unload();
  CHECK(g.joined() == "MODULE(3) ADWSTOPSOUND ADWRESTOREPREVIOUSVOLUME ADWSOUNDCLEANUP",
        "AD_SND 1.0 unload: adwRestorePreviousVolume where adwSetSystemVolumes was: %s", g.joined().c_str());
  // Its button (BUTTONPUSHED16) the same way.
  g.calls.clear();
  uint16_t r = b->button(s + ne16::scratch::kPath, 0, 2, s + ne16::scratch::kCtrl, s + ne16::scratch::kError,
                         ne16::scratch::kErrorSize, s + ne16::scratch::kErrId);
  CHECK(r == 1 && g.joined() == "ADWSOUNDINIT ADWSAVEPREVIOUSVOLUME MODULE(5) MODULE(9) ADWSTOPSOUND "
                                "ADWRESTOREPREVIOUSVOLUME ADWSOUNDCLEANUP",
        "AD_SND 1.0 button: %u %s", r, g.joined().c_str());
  b->close();
  // An AD_SND with both pairs (FAKESND): the first, as before; the second is never looked up.
  auto b2 = ne16::open_native_bridge(rt, "C:\\WINDOWS\\SYSTEM\\FAKESND.DLL", &why);
  g.calls.clear();
  lookups = gpa->calls;
  CHECK(load(*b2) == 1 && gpa->calls - lookups == 8 && g.joined().find("PREVIOUS") == std::string::npos &&
            g.joined().rfind("ADWSOUNDINIT ADWGETSYSTEMVOLUMES MODULE(5)", 0) == 0,
        "both pairs: adwGetSystemVolumes, and the seven lookups of before plus MODULE (%llu; %s)",
        (unsigned long long)(gpa->calls - lookups), g.joined().c_str());
  g.calls.clear();
  b2->unload();
  CHECK(g.joined() == "MODULE(3) ADWSTOPSOUND ADWSETSYSTEMVOLUMES(4660) ADWSOUNDCLEANUP", "both pairs, unload: %s",
        g.joined().c_str());
  b2->close();
  // Half of AD_SND 1.0's pair: an entry point is missing (error id 3); AD_SND
  // is released as for any missing entry, never initialized, no volume kept.
  auto b3 = ne16::open_native_bridge(rt, "C:\\WINDOWS\\SYSTEM\\SND10HALF.DLL", &why);
  g.calls.clear();
  CHECK(load(*b3) == 0 && rt.rd16(s + ne16::scratch::kErrId) == 3 && g.joined() == "ADWSTOPSOUND ADWSOUNDCLEANUP",
        "no adwRestorePreviousVolume and no first pair: error id 3 (%u; %s)", rt.rd16(s + ne16::scratch::kErrId),
        g.joined().c_str());
  b3->close();
  // AD_SND 1.0 with its whole pair but no adwStopSound: error id 3 too, and
  // as adwSavePreviousVolume never ran, adwRestorePreviousVolume is not
  // called either (as a 3.x AD_SND's adwSetSystemVolumes is not without a
  // saved volume: HALFSND above) — cleanup alone.
  auto b4 = ne16::open_native_bridge(rt, "C:\\WINDOWS\\SYSTEM\\SND10NOSTOP.DLL", &why);
  g.calls.clear();
  CHECK(load(*b4) == 0 && rt.rd16(s + ne16::scratch::kErrId) == 3 && g.joined() == "ADWSOUNDCLEANUP",
        "AD_SND 1.0 without adwStopSound: error id 3, nothing restored (%u; %s)", rt.rd16(s + ne16::scratch::kErrId),
        g.joined().c_str());
  g.calls.clear();
  uint16_t r4 = b4->button(s + ne16::scratch::kPath, 0, 2, s + ne16::scratch::kCtrl, s + ne16::scratch::kError,
                           ne16::scratch::kErrorSize, s + ne16::scratch::kErrId);
  CHECK(r4 == 0 && rt.rd16(s + ne16::scratch::kErrId) == 3 && g.joined() == "ADWSOUNDCLEANUP",
        "its button the same: no module, nothing restored (%u, error id %u; %s)", r4,
        rt.rd16(s + ne16::scratch::kErrId), g.joined().c_str());
  b4->close();
}

// ---- the host's AD_SND (win16/adsnd16.cc) --------------------------------------------------------------------
//
// A made-up NE module that imports AD_SND by name, in the case it pleases (the
// loader matches import names case-insensitively, as Windows did): two
// made-up sounds (type 3000: 1000, 11025 Hz, and "BARK", 22050 Hz), a button
// record (1000/1), a MODULE entry, and one export per AD_SND entry the tests
// reach, each a far jump through the module's own import of it — so a test
// calls AD_SND the way the module's code would. MODULE does what a Snoopy
// module does (IS_FLY 1:02a5..1:02e7): at INITIALIZE (0) adwOpenSound, then
// adwLoadSoundResource(its instance, 1000), adwSetSoundMode(h, 0x210: async,
// looping) and adwPlaySound(h); at CLOSE (3) adwFreeSound(h) and
// adwCloseSound(2); it answers 0 to every message.

// A made-up PCM WAV image: 8-bit mono at `rate` Hz, a sawtooth.
std::string made_up_wav(uint32_t rate, uint32_t samples) {
  std::string w = "RIFF";
  auto u32 = [&](uint32_t v) {
    for (int i = 0; i < 4; i++) w += char(v >> (8 * i));
  };
  u32(36 + samples);
  w += "WAVEfmt ";
  u32(16);
  w += std::string("\x01\x00\x01\x00", 4);  // PCM, mono
  u32(rate);
  u32(rate);
  w += std::string("\x01\x00\x08\x00", 4);  // block align 1, 8 bits
  w += "data";
  u32(samples);
  for (uint32_t i = 0; i < samples; i++) w += char(0x60 + (i * 5) % 0x40);
  return w;
}

// The AD_SND entries SNDMOD imports and exports (its ordinals 2.. in this order).
const char* const kSndEntries[] = {
    "adwSoundInit",         "adwSoundCleanup",       "adwOpenSound",         "adwCloseSound",
    "adwLoadSoundResource", "adwLoadSoundFile",      "adwCreateSound",       "adwSetSoundMode",
    "adwPlaySound",         "adwPlaySoundResource",  "adwPlaySoundFile",     "adwStopSound",
    "adwFreeSound",         "adwIsSoundDone",        "adwSoundAsyncCap",     "adwSoundLoopCap",
    "adwSoundVolumeCap",    "adwSetSoundMute",       "adwGetSoundMute",      "adwSetVolume",
    "adwGetVolume",         "adwGetSystemVolumes",   "adwSetSystemVolumes",  "adwSavePreviousVolume",
    "adwRestorePreviousVolume", "adwGetSoundInfo",   "adwSoundDllVer",       "VerStr",
    "adwQuerySfx",          "adwDoEffect",           "adwPauseSound",        "adwResumeSound"};

std::string sound_module_image() {
  // Segment 1: MODULE at 0 (its prolog is the one the loader patches to
  // mov ax, DGROUP), the jumps from 0x60, 8 bytes apart. FF FF 00 00 is an
  // import's place: the end of its fixup chain.
  std::vector<uint8_t> code = {
      0x1E, 0x58, 0x90,                    // 00 push ds; pop ax; nop (→ mov ax, DGROUP)
      0x45, 0x55, 0x8B, 0xEC, 0x1E,        // 03 inc bp; push bp; mov bp, sp; push ds
      0x8E, 0xD8,                          // 08 mov ds, ax
      0x83, 0x7E, 0x0A, 0x00,              // 0A cmp word [bp+0Ah], 0 (msg)
      0x75, 0x27,                          // 0E jne 37
      0x9A, 0xFF, 0xFF, 0x00, 0x00,        // 10 call far adwOpenSound
      0x1E, 0x6A, 0x00, 0x68, 0xE8, 0x03,  // 15 push ds (hInstance); push 0; push 1000
      0x9A, 0xFF, 0xFF, 0x00, 0x00,        // 1B call far adwLoadSoundResource
      0xA3, 0x00, 0x00,                    // 20 mov [0], ax
      0x50, 0x68, 0x10, 0x02,              // 23 push ax; push 0210h
      0x9A, 0xFF, 0xFF, 0x00, 0x00,        // 27 call far adwSetSoundMode
      0xFF, 0x36, 0x00, 0x00,              // 2C push word [0]
      0x9A, 0xFF, 0xFF, 0x00, 0x00,        // 30 call far adwPlaySound
      0xEB, 0x16,                          // 35 jmp 4D
      0x83, 0x7E, 0x0A, 0x03,              // 37 cmp word [bp+0Ah], 3
      0x75, 0x10,                          // 3B jne 4D
      0xFF, 0x36, 0x00, 0x00,              // 3D push word [0]
      0x9A, 0xFF, 0xFF, 0x00, 0x00,        // 41 call far adwFreeSound
      0x6A, 0x02,                          // 46 push 2
      0x9A, 0xFF, 0xFF, 0x00, 0x00,        // 48 call far adwCloseSound
      0x31, 0xC0, 0x1F, 0x5D, 0x4D,        // 4D xor ax, ax; pop ds; pop bp; dec bp
      0xCA, 0x06, 0x00};                   // 52 retf 6
  code.resize(0x60, 0x90);
  // The imported names: 0, AD_SND at 1 (the module reference), the entries.
  std::string imp(1, '\0');
  imp += char(6) + std::string("AD_SND");
  std::map<std::string, uint16_t> name_at;
  for (const char* n : kSndEntries) {
    name_at[n] = uint16_t(imp.size());
    imp += char(strlen(n)) + std::string(n);
  }
  struct Fixup {
    uint16_t at;
    const char* name;
  };
  std::vector<Fixup> fixups = {{0x11, "adwOpenSound"}, {0x1C, "adwLoadSoundResource"}, {0x28, "adwSetSoundMode"},
                               {0x31, "adwPlaySound"}, {0x42, "adwFreeSound"},         {0x49, "adwCloseSound"}};
  std::vector<uint16_t> jumps;
  for (const char* n : kSndEntries) {
    jumps.push_back(uint16_t(code.size()));
    fixups.push_back({uint16_t(code.size() + 1), n});
    code.insert(code.end(), {0xEA, 0xFF, 0xFF, 0x00, 0x00, 0x90, 0x90, 0x90});  // jmp far <import>
  }
  std::string rel;
  put16(rel, 0, uint16_t(fixups.size()));
  for (const Fixup& f : fixups) {
    size_t p = rel.size();
    rel.resize(p + 8, '\0');
    rel[p] = 3;      // a far pointer
    rel[p + 1] = 2;  // an import by name
    put16(rel, p + 2, f.at);
    put16(rel, p + 4, 1);
    put16(rel, p + 6, name_at[f.name]);
  }
  // Names: the module, MODULE (1), the jumps (2..); one bundle of moveable entries.
  auto name_entry = [](std::string& t, const std::string& n, uint16_t ordinal) {
    t += char(n.size()) + n + char(ordinal & 0xFF) + char(ordinal >> 8);
  };
  std::string resident;
  name_entry(resident, "SNDMOD", 0);
  name_entry(resident, "MODULE", 1);
  for (size_t i = 0; i < std::size(kSndEntries); i++) name_entry(resident, kSndEntries[i], uint16_t(i + 2));
  resident += '\0';
  std::string entry;
  entry += char(1 + jumps.size());
  entry += char(0xFF);
  auto moveable = [&](uint16_t off) { entry += std::string{char(0x03), char(0xCD), char(0x3F), char(1), char(off), char(off >> 8)}; };
  moveable(0);
  for (uint16_t j : jumps) moveable(j);
  entry += '\0';
  std::string modref;
  put16(modref, 0, 1);
  // Resources, one type block each (shift 4); "BARK" is a string name.
  std::string button_rec(0x20, '\0');
  put16(button_rec, 0, 4);
  struct Res {
    uint16_t type, id;
    std::string name, data;
  };
  const Res res[] = {{3000, 1000, "", made_up_wav(11025, 1100)}, {3000, 0, "BARK", made_up_wav(22050, 441)},
                     {1000, 1, "", button_rec}};
  std::string rt;
  put16(rt, 0, 4);
  std::vector<size_t> res_at;
  const size_t names_at = 2 + std::size(res) * 20 + 2;
  std::string names;
  for (const Res& r : res) {
    size_t p = rt.size();
    rt.resize(p + 20, '\0');
    put16(rt, p, uint16_t(0x8000 | r.type));
    put16(rt, p + 2, 1);
    res_at.push_back(p + 8);
    put16(rt, p + 8 + 4, 0x30);
    if (r.name.empty()) {
      put16(rt, p + 8 + 6, uint16_t(0x8000 | r.id));
    } else {
      put16(rt, p + 8 + 6, uint16_t(names_at + names.size()));
      names += char(r.name.size()) + r.name;
    }
  }
  rt.resize(rt.size() + 2, '\0');
  rt += names + '\0';
  // The header and the tables after it, then the code with its fixups, then the resources.
  std::string h(0x40, '\0');
  h[0] = 'N';
  h[1] = 'E';
  const uint16_t seg_off = 0x40, res_off = seg_off + 16, resident_off = uint16_t(res_off + rt.size());
  const uint16_t modref_off = uint16_t(resident_off + resident.size()), imp_off = uint16_t(modref_off + modref.size());
  const uint16_t entry_off = uint16_t(imp_off + imp.size()), tables_end = uint16_t(entry_off + entry.size());
  put16(h, 0x04, entry_off);
  put16(h, 0x06, uint16_t(entry.size()));
  put16(h, 0x0C, 0x8001);  // LIBRARY | SINGLEDATA
  put16(h, 0x0E, 2);       // DGROUP: segment 2
  put16(h, 0x1C, 2);
  put16(h, 0x1E, 1);
  put16(h, 0x22, seg_off);
  put16(h, 0x24, res_off);
  put16(h, 0x26, resident_off);
  put16(h, 0x28, modref_off);
  put16(h, 0x2A, imp_off);
  put16(h, 0x30, uint16_t(1 + jumps.size()));
  put16(h, 0x32, 4);
  h[0x36] = 2;
  put16(h, 0x3E, 0x030A);
  const size_t code_at = (0x40 + size_t(tables_end) + 15) & ~size_t(15);
  size_t data_at = (code_at + code.size() + rel.size() + 15) & ~size_t(15);
  for (size_t i = 0; i < std::size(res); i++) {
    put16(rt, res_at[i], uint16_t(data_at >> 4));
    put16(rt, res_at[i] + 2, uint16_t((res[i].data.size() + 15) >> 4));
    data_at += (res[i].data.size() + 15) & ~size_t(15);
  }
  std::string seg;
  put16(seg, 0, uint16_t(code_at >> 4));
  put16(seg, 2, uint16_t(code.size()));
  put16(seg, 4, 0x0110);  // RELOCINFO | MOVEABLE: the code
  put16(seg, 6, uint16_t(code.size()));
  put16(seg, 8, 0);       // DGROUP: no file data, 16 zero bytes
  put16(seg, 10, 0);
  put16(seg, 12, 0x0001);
  put16(seg, 14, 0x10);
  std::string f(0x40, '\0');
  f[0] = 'M';
  f[1] = 'Z';
  put16(f, 0x3C, 0x40);
  f += h + seg + rt + resident + modref + imp + entry;
  f.resize(code_at, '\0');
  f += std::string(code.begin(), code.end()) + rel;
  for (const Res& r : res) {
    f.resize((f.size() + 15) & ~size_t(15), '\0');
    f += r.data;
  }
  f.resize((f.size() + 15) & ~size_t(15), '\0');
  return f;
}

// SNDMOD loaded in a runtime of its own with the host's AD_SND, its MMSYSTEM
// calls recorded (and a MIDI device with a volume made up for them: without
// the audio engine there is none).
struct HostSndRig {
  VirtualClock clock{VirtualClock::Mode::fixed_step, 16667};
  win16::Runtime16 rt;
  win16::Module16* mod = nullptr;
  std::vector<std::string> mm;
  uint32_t buf = 0;  // 0x200 bytes of guest memory

  static win16::Runtime16Options options(bool device) {
    win16::Runtime16Options o;
    o.sound_device = device;  // ADSOUNDDEV=0 when false
    return o;
  }
  HostSndRig(const std::string& dir, bool device) : rt(options(device), clock) {
    clock.set_read_step_us(5);
    win16::register_all16(rt);
    rt.vfs().mount_overlay("C:\\AFTERDRK", dir, "");
    win16::register_host_ad_snd(rt);
    using win16::Call16;
    auto& r = rt.shims();
    auto wrap = [&](const char* name, std::function<std::string(Call16&)> what) {
      win16::Shim16Entry* e = r.find_name("MMSYSTEM", name);
      win16::Shim16Fn orig = e->fn;
      e->fn = [this, orig, what](Call16& c) {
        mm.push_back(what(c));
        c.rewind();
        orig(c);
      };
    };
    wrap("sndPlaySound", [](Call16& c) {
      uint32_t p = c.ptr();
      uint16_t flags = c.w();
      char b[64];
      std::string img = !p ? "NULL" : (flags & 4) ? c.rt.read_str(p, 4) + " " + std::to_string(c.rt.rd32(p + 24)) : "?";
      snprintf(b, sizeof(b), "snd(%s, %04X)", img.c_str(), flags);
      return std::string(b);
    });
    wrap("waveOutSetVolume", [](Call16& c) {
      uint16_t dev = c.w();
      char b[40];
      snprintf(b, sizeof(b), "wave(%u, %08X)", dev, c.l());
      return std::string(b);
    });
    wrap("waveOutOpen", [](Call16& c) {
      c.ptr();
      c.w();
      uint32_t fmt = c.ptr();
      c.l();
      c.l();
      uint32_t flags = c.l();
      return "query(" + std::to_string(c.rt.rd32(fmt + 4)) + ", " + std::to_string(flags) + ")";
    });
    r.find_name("MMSYSTEM", "midiOutGetNumDevs")->fn = [](Call16& c) { c.ret(1); };
    r.find_name("MMSYSTEM", "midiOutGetDevCaps")->fn = [](Call16& c) {
      c.w();
      uint32_t caps = c.ptr();
      c.rt.wr32(caps + 46, 1);  // MIDICAPS_VOLUME
      c.ret(0);
    };
    r.find_name("MMSYSTEM", "midiOutGetVolume")->fn = [](Call16& c) {
      c.w();
      c.rt.wr32(c.ptr(), 0x12341234);
      c.ret(0);
    };
    r.find_name("MMSYSTEM", "midiOutSetVolume")->fn = [this](Call16& c) {
      uint16_t dev = c.w();
      char b[40];
      snprintf(b, sizeof(b), "midi(%u, %08X)", dev, c.l());
      mm.push_back(b);
      c.ret(0);
    };
    uint16_t err = 0;
    mod = rt.modules().load_host(dir + "\\SNDMOD.AD", &err);
    uint16_t hb = rt.global().alloc(win16::GlobalHeap16::kZeroInit, 0x200);
    buf = uint32_t(hb) << 16;
  }
  // An AD_SND entry through SNDMOD's own import of it.
  uint32_t call(const char* name, std::initializer_list<win16::Arg16> args = {}) {
    uint32_t fp = mod ? rt.modules().proc_address(mod, name) : 0;
    if (!fp) throw std::runtime_error(std::string("SNDMOD has no ") + name);
    return rt.call_far(fp, args);
  }
  uint16_t call16(const char* name, std::initializer_list<win16::Arg16> args = {}) { return uint16_t(call(name, args)); }
  std::string seen() {
    std::string s;
    for (const std::string& x : mm) s += (s.empty() ? "" : " ") + x;
    mm.clear();
    return s;
  }
  uint64_t calls(const char* module, const char* name) {
    win16::Shim16Entry* e = rt.shims().find_name(module, name);
    return e ? e->calls : 0;
  }
};

void test_host_ad_snd() {
  using win16::l16;
  using win16::w16;
  std::string dir = temp_dir("hostsnd");
  write_file(dir + "\\SNDMOD.AD", sound_module_image());
  write_file(dir + "\\TONE.WAV", made_up_wav(11025, 300));
  {
    HostSndRig g(dir, /*device=*/true);
    win16::Runtime16& rt = g.rt;
    CHECK(g.mod && !g.mod->system && g.mod->name == "SNDMOD", "the made-up module loads, AD_SND imported by name");
    if (!g.mod) return;
    CHECK(rt.shims().has_module("AD_SND") && rt.modules().by_name("AD_SND") && rt.modules().by_name("AD_SND")->system,
          "AD_SND is the host's system module");
    const uint16_t hinst = g.mod->hinstance;
    const uint32_t bark = rt.static_bytes("test BARK", "BARK");
    // Before adwSoundInit nothing has the device; adwPlaySound answers 1 (its volume is 0).
    CHECK(g.call16("adwOpenSound") == 0 && g.call16("adwSoundAsyncCap") == 0 &&
              g.call16("adwLoadSoundResource", {w16(hinst), l16(1000)}) == 0 && g.call16("adwPlaySound", {w16(4)}) == 1 &&
              g.call16("adwStopSound") == 0 && g.call16("adwGetVolume") == 0,
          "before adwSoundInit: no device");
    // adwSoundInit: the probe, then the capabilities.
    g.seen();
    uint16_t init = g.call16("adwSoundInit", {w16(0), l16(g.buf)});
    std::string probe = g.seen();
    CHECK(init == 0 && rt.read_str(g.buf).empty() && probe == "query(11025, 1) query(22050, 1)",
          "adwSoundInit: 0, the 11 and 22 kHz queries (%u; %s)", init, probe.c_str());
    CHECK(g.call16("adwSoundAsyncCap") == 2 && g.call16("adwSoundLoopCap") == 4 && g.call16("adwSoundVolumeCap") == 1 &&
              g.call16("adwOpenSound") == 1 && g.call16("adwGetVolume") == 25,
          "with the device: async 2, loop 4, volume 1, open 1, level 25");
    // Loading: type 3000 from the module's own resources, by number or name.
    uint16_t h = g.call16("adwLoadSoundResource", {w16(hinst), l16(1000)});
    uint16_t h2 = g.call16("adwLoadSoundResource", {w16(hinst), l16(bark)});
    CHECK(h && h2 && h != h2 && g.call16("adwLoadSoundResource", {w16(hinst), l16(999)}) == 0 &&
              g.call16("adwLoadSoundResource", {w16(hinst), l16(0)}) == 0 && rt.global().find(h),
          "adwLoadSoundResource: 1000 and \"BARK\" load (%04X %04X), 999 and none do not", h, h2);
    // The play flags: async 0x07 (the default), async looping 0x0F, synchronous 0x06.
    g.seen();
    CHECK(g.call16("adwPlaySound", {w16(h)}) == 1 && g.seen() == "snd(RIFF 11025, 0007)", "the default mode: 0x07");
    CHECK(g.call16("adwSetSoundMode", {w16(h), w16(0x210)}) == 1 && g.call16("adwPlaySound", {w16(h)}) == 1 &&
              g.seen() == "snd(RIFF 11025, 000F)",
          "0x210, looping: 0x0F");
    CHECK(g.call16("adwSetSoundMode", {w16(h), w16(0x120)}) == 1 && g.call16("adwPlaySound", {w16(h)}) == 1 &&
              g.seen() == "snd(RIFF 11025, 0006)",
          "0x120, synchronous: 0x06");
    CHECK(g.call16("adwSetSoundMode", {w16(h), w16(0x300)}) == 0 && g.call16("adwSetSoundMode", {w16(h), w16(0x30)}) == 0 &&
              g.call16("adwSetSoundMode", {w16(h), w16(0x220)}) == 0 && g.call16("adwSetSoundMode", {w16(0), w16(0x10)}) == 0,
          "adwSetSoundMode refuses both of a pair, a synchronous loop, no sound");
    CHECK(g.call16("adwSetSoundMode", {w16(h), w16(0x200)}) == 1 && g.call16("adwPlaySound", {w16(h)}) == 0 &&
              g.seen().empty(),
          "a synchronous loop made in two calls plays nothing: 0");
    CHECK(g.call16("adwSetSoundMode", {w16(h), w16(0x110)}) == 1, "back to async, no loop");
    // The current sound.
    CHECK(g.call16("adwPlaySound", {w16(h2)}) == 1 && g.seen() == "snd(RIFF 22050, 0007)" &&
              g.call16("adwIsSoundDone", {w16(h2)}) == 1 && g.call16("adwIsSoundDone", {w16(h)}) == 0 &&
              g.call16("adwIsSoundDone", {w16(0)}) == 1,
          "adwIsSoundDone: 1 for the current sound (or 0), 0 for another");
    // Mute: stored, tested by adwPlaySound, which then answers 1 and plays nothing.
    CHECK(g.call16("adwSetSoundMute", {w16(1)}) == 1 && g.call16("adwGetSoundMute") == 1 &&
              g.call16("adwPlaySound", {w16(h2)}) == 1 && g.seen().empty(),
          "muted: adwPlaySound answers 1, plays nothing");
    g.call16("adwSetSoundMute", {w16(0)});
    // Volume: v × 0xFFFF / 100 on both channels, the MIDI devices first.
    CHECK(g.call16("adwSetVolume", {w16(50)}) == 1 && g.seen() == "midi(0, 7FFF7FFF) wave(0, 7FFF7FFF)" &&
              g.call16("adwGetVolume") == 50,
          "adwSetVolume(50): MIDI and wave 0x7FFF7FFF, level 50");
    CHECK(g.call16("adwSetVolume", {w16(50)}) == 1 && g.seen().empty(), "the same value again: 1, nothing set");
    CHECK(g.call16("adwSetVolume", {w16(101)}) == 0 && g.seen().empty() && g.call16("adwGetVolume") == 50,
          "101: 0, nothing set, the level kept");
    CHECK(g.call16("adwSetVolume", {w16(0)}) == 1 && g.seen() == "midi(0, 00000000) wave(0, 00000000)" &&
              g.call16("adwPlaySound", {w16(h2)}) == 1 && g.seen().empty(),
          "level 0: adwPlaySound answers 1, plays nothing");
    CHECK(g.call16("adwSetVolume", {w16(100)}) == 1 && g.seen() == "midi(0, FFFFFFFF) wave(0, FFFFFFFF)", "100: FFFF");
    g.call16("adwSetSoundMute", {w16(1)});
    CHECK(g.call16("adwSetVolume", {w16(70)}) == 1 && g.seen() == "midi(0, 00000000) wave(0, 00000000)",
          "muted, 70 counts as 0");
    g.call16("adwSetSoundMute", {w16(0)});
    CHECK(g.call16("adwSetVolume", {w16(70)}) == 1 && g.seen() == "midi(0, B332B332) wave(0, B332B332)", "70: 0xB332");
    // Stop and close.
    CHECK(g.call16("adwStopSound") == 1 && g.seen() == "snd(NULL, 0000)" && g.call16("adwIsSoundDone", {w16(h2)}) == 0,
          "adwStopSound: sndPlaySound(NULL, 0), no current sound");
    CHECK(g.call16("adwCloseSound", {w16(0)}) == 1 && g.seen().empty() && g.call16("adwCloseSound", {w16(2)}) == 1 &&
              g.seen() == "snd(NULL, 0000)",
          "adwCloseSound: 2 stops the sound");
    // Free: the current sound stopped first; the image (a resource) and the record freed.
    uint64_t freed = g.calls("KERNEL", "FreeResource");
    g.call16("adwPlaySound", {w16(h)});
    g.seen();
    CHECK(g.call16("adwFreeSound", {w16(h)}) == 1 && g.seen() == "snd(NULL, 0000)" && !rt.global().find(h) &&
              g.call16("adwPlaySound", {w16(h)}) == 0,
          "adwFreeSound of the current sound: stopped, freed, gone");
    CHECK(g.call16("adwFreeSound", {w16(h2)}) == 1 && g.seen().empty() && g.call16("adwFreeSound", {w16(0)}) == 0 &&
              g.calls("KERNEL", "FreeResource") - freed == 2,
          "adwFreeSound of another: no stop; both resources freed");
    // adwGetSoundInfo: the data's length, bytes and samples per second, channels.
    h = g.call16("adwLoadSoundResource", {w16(hinst), l16(1000)});
    uint32_t info = g.buf + 0x100;
    CHECK(g.call16("adwGetSoundInfo", {w16(h), l16(info)}) == 1 && rt.rd32(info) == 1100 && rt.rd32(info + 4) == 11025 &&
              rt.rd32(info + 8) == 11025 && rt.rd16(info + 12) == 1,
          "adwGetSoundInfo: %u bytes, %u B/s, %u Hz, %u channel(s)", rt.rd32(info), rt.rd32(info + 4), rt.rd32(info + 8),
          rt.rd16(info + 12));
    g.call16("adwFreeSound", {w16(h)});
    // adwPlaySoundResource: load, mode, play (the sound is never freed, as in the library).
    g.seen();
    CHECK(g.call16("adwPlaySoundResource", {w16(hinst), l16(bark), w16(0x10)}) == 1 && g.seen() == "snd(RIFF 22050, 0007)",
          "adwPlaySoundResource: played");
    // A file: adwLoadSoundFile reads it into memory, played as an image; a
    // named file sound is refused (so adwPlaySoundFile fails); a record with
    // no image plays nothing and answers 1.
    const uint32_t tone = rt.static_bytes("test TONE", "C:\\AFTERDRK\\TONE.WAV");
    uint16_t file = g.call16("adwLoadSoundFile", {l16(tone)});
    CHECK(file && g.call16("adwPlaySound", {w16(file)}) == 1 && g.seen() == "snd(RIFF 11025, 0007)",
          "adwLoadSoundFile: the file's image played (%04X)", file);
    CHECK(g.call16("adwPlaySoundFile", {l16(tone), w16(0x10)}) == 0 && g.seen().empty() &&
              g.call16("adwCreateSound", {l16(tone), w16(0x1010)}) == 0,
          "a named file sound: refused");
    uint16_t bare = g.call16("adwCreateSound", {l16(0), w16(0x10)});
    CHECK(bare && g.call16("adwPlaySound", {w16(bare)}) == 1 && g.seen().empty(), "a record with no image: 1, nothing");
    CHECK(g.call16("adwLoadSoundFile", {l16(rt.static_bytes("test NOSUCH", "C:\\AFTERDRK\\NOSUCH.WAV"))}) == 0,
          "adwLoadSoundFile of no file: 0");
    // The system volumes: every device with a volume saved, written back, the block freed.
    g.call16("adwSetVolume", {w16(30)});
    g.seen();
    uint32_t out = g.buf + 0x180;
    CHECK(g.call16("adwGetSystemVolumes", {l16(out)}) == 0 && rt.rd16(out) != 0, "adwGetSystemVolumes: a block");
    uint16_t sys = rt.rd16(out);
    g.call16("adwSetVolume", {w16(90)});
    g.seen();
    CHECK(g.call16("adwSetSystemVolumes", {w16(sys)}) == 0 && g.seen() == "wave(0, 4CCC4CCC) midi(0, 12341234)" &&
              !rt.global().find(sys),
          "adwSetSystemVolumes: the saved volumes written back, the block freed");
    uint16_t other = rt.global().alloc(win16::GlobalHeap16::kMoveable | win16::GlobalHeap16::kZeroInit, 0xB4);
    CHECK(g.call16("adwSetSystemVolumes", {w16(0)}) == 1 && g.call16("adwSetSystemVolumes", {w16(other)}) == 3 &&
              !rt.global().find(other),
          "adwSetSystemVolumes: 1 for no block, 3 (and freed) for another kind of block");
    // AD_SND 1.0's pair: the wave device's volume alone.
    g.call16("adwSetVolume", {w16(20)});
    CHECK(g.call16("adwSavePreviousVolume") == 1, "adwSavePreviousVolume");
    g.call16("adwSetVolume", {w16(80)});
    g.seen();
    CHECK(g.call16("adwRestorePreviousVolume") == 1 && g.seen() == "wave(0, 33333333)", "adwRestorePreviousVolume");
    // Versions, effects.
    uint32_t ver = g.call("adwSoundDllVer");
    CHECK(rt.read_str(ver) == "3.0.3" && g.call16("VerStr", {l16(g.buf), w16(64)}) == 303 &&
              rt.read_str(g.buf).rfind("AD_SND ver 303", 0) == 0,
          "adwSoundDllVer \"%s\", VerStr 303 \"%s\"", rt.read_str(ver).c_str(), rt.read_str(g.buf).c_str());
    CHECK(g.call16("adwQuerySfx", {w16(0)}) == 1 && g.call16("adwQuerySfx", {w16(1)}) == 0 &&
              g.call16("adwDoEffect", {w16(0), w16(0), w16(0), w16(0)}) == 1 && g.call16("adwPauseSound") == 1 &&
              g.call16("adwResumeSound") == 1,
          "no effects but 0; pause and resume answer 1");
    // Cleanup: no device any more, the mute cleared.
    g.call16("adwSetSoundMute", {w16(1)});
    CHECK(g.call16("adwSoundCleanup") == 1 && g.call16("adwOpenSound") == 0 && g.call16("adwSoundAsyncCap") == 0 &&
              g.call16("adwGetVolume") == 0 && g.call16("adwGetSoundMute") == 0 && g.call16("adwStopSound") == 0,
          "adwSoundCleanup: no device, the mute cleared");
    CHECK(g.calls("AD_SND", "ADWPLAYSOUND") > 0 && rt.shims().unimplemented_called().empty(),
          "every call implemented (census)");
  }
  {
    // No wave device (ADSOUNDDEV=0): adwSoundInit fails with why; every entry
    // that needs the device answers 0, adwPlaySound 1; nothing is played.
    HostSndRig g(dir, /*device=*/false);
    win16::Runtime16& rt = g.rt;
    CHECK(g.mod != nullptr, "the made-up module loads without a device");
    if (!g.mod) return;
    uint16_t init = g.call16("adwSoundInit", {w16(0), l16(g.buf)});
    CHECK(init == 1 && rt.read_str(g.buf) == "No wave output device is installed.", "no device: adwSoundInit 1, '%s'",
          rt.read_str(g.buf).c_str());
    CHECK(g.call16("adwSoundAsyncCap") == 0 && g.call16("adwSoundLoopCap") == 0 && g.call16("adwOpenSound") == 0 &&
              g.call16("adwLoadSoundResource", {w16(g.mod->hinstance), l16(1000)}) == 0 &&
              g.call16("adwPlaySound", {w16(0x1234)}) == 1 && g.call16("adwStopSound") == 0 &&
              g.call16("adwSetVolume", {w16(50)}) == 0 && g.call16("adwGetVolume") == 0,
          "no device: async cap 0, open 0, load 0, play 1, stop 0, volume 0");
    CHECK(g.seen().empty(), "no device: no sndPlaySound, no volume set");
  }
  DeleteFileA((dir + "\\SNDMOD.AD").c_str());
  DeleteFileA((dir + "\\TONE.WAV").c_str());
  RemoveDirectoryA(dir.c_str());
}

// ---- the protocol seam (protocol.hh) ------------------------------------------------------------------------
//
// The lane driving a scripted Protocol16: what it asks of a protocol and in
// which order, how it maps the calls' results, SET, frames that end inside a
// call and a call abandoned at close, and how configure mode reports a button
// — the contract a protocol relies on, AD3's or another.
struct Script {
  std::vector<ne16::Protocol16::Call> results;  // call() results, in turn (then ok)
  bool load_ok = true;
  size_t wait_call = 0;  // the call (1-based) that blocks the guest until virtual time has moved on ...
  uint64_t wait_us = 0;  // ... this far (Runtime16::wait_until_us)
  ne16::Protocol16::Button button;
  bool keys = true;    // takes_key_messages()
  bool carry = false;  // carries_overruns()
  // pixel_cost(): After Dark's (the base's) unless a knob is named.
  const char* pixel_knob = nullptr;
  uint32_t pixel_def = 0;
  const char* speed_knob = nullptr;  // speed_knob(): none (the base's) unless named
  bool quiet = false;  // call() and after_call() leave `seen` alone (long runs)
  // Guest work of the test's own, at load, in every call (with its 1-based
  // number) and at unload.
  std::function<void(win16::Runtime16&)> on_load, on_unload;
  std::function<void(win16::Runtime16&, size_t)> on_call;
  // Guest work in after_call (the protocol's message loop), which also runs
  // in frames that make no call.
  std::function<void(win16::Runtime16&)> on_after;
};

std::string joined(const std::vector<std::string>& v) {
  std::string s;
  for (const std::string& x : v) s += (s.empty() ? "" : ", ") + x;
  return s;
}

struct ScriptedProtocol : ne16::Protocol16 {
  ScriptedProtocol(std::vector<std::string>& seen_, const Script& script_) : seen(seen_), script(script_) {}
  const char* name() const override { return "scripted"; }
  void configure_runtime(win16::Runtime16Options& opts, LaneContext&) override {
    seen.push_back("configure_runtime");
    opts.guest_dir = "C:\\SAVER";
    opts.desktop_palette = true;
  }
  void mount(win16::Runtime16& rt, const Env&) override {
    const win16::Runtime16Options& o = rt.options();
    seen.push_back("mount " + o.guest_dir + (o.desktop_palette ? " desktop" : " boot"));
    rt.vfs().mount_overlay(o.windows_dir, "", "");
  }
  bool load(win16::Runtime16& rt, uint16_t hwnd, uint16_t hdc, LaneContext&) override {
    seen.push_back(hwnd && hdc ? "load" : "load without a window");
    rt_ = &rt;
    if (script.on_load) script.on_load(rt);
    return script.load_ok;
  }
  Call call() override {
    if (!script.quiet) seen.push_back("call");
    if (++calls == script.wait_call) {
      rt_->wait_until_us(rt_->peek_us() + script.wait_us);
      seen.push_back("waited");
    }
    if (script.on_call) script.on_call(*rt_, calls);
    Call c;
    if (!script.results.empty()) {
      c = script.results.front();
      script.results.erase(script.results.begin());
    }
    return c;
  }
  void after_call() override {
    afters++;
    if (!script.quiet) seen.push_back("after");
    if (script.on_after && rt_) script.on_after(*rt_);
  }
  bool takes_key_messages() const override { return script.keys; }
  bool carries_overruns() const override { return script.carry; }
  PixelCost pixel_cost() const override {
    return script.pixel_knob ? PixelCost{script.pixel_knob, script.pixel_def} : Protocol16::pixel_cost();
  }
  const char* speed_knob() const override { return script.speed_knob ? script.speed_knob : Protocol16::speed_knob(); }
  bool set_control(int index, int32_t value) override {
    seen.push_back("set " + std::to_string(index) + " " + std::to_string(value));
    return index == 1;  // this module has one control
  }
  void send_controls() override { seen.push_back("send"); }
  void unload() override {
    seen.push_back("unload");
    if (script.on_unload) script.on_unload(*rt_);
  }
  void close() override { seen.push_back("close"); }
  std::string error_text() const override { return "scripted error"; }
  bool check_button(int slot, std::string* why) override {
    seen.push_back("check_button " + std::to_string(slot));
    if (slot == 0) return true;
    *why = "control " + std::to_string(slot) + " is not a button";
    return false;
  }
  void configure_button_runtime(win16::Runtime16Options& opts, const Env&) override {
    seen.push_back("configure_button_runtime");
    opts.guest_dir = "C:\\SAVER";
  }
  Button button(win16::Runtime16& rt, int slot, uint16_t, LaneContext&) override {
    seen.push_back("button " + std::to_string(slot));
    uint32_t e = 0;
    if (script.button.ran) {
      rt.vfs().write_file("C:\\WINDOWS\\SCRIPTED.INI", "[Scripted]\r\n", &e);
      // A temporary file, written and deleted again (STRESS's GetTempFileName file).
      rt.vfs().write_file("C:\\WINDOWS\\~STR1234.TMP", "", &e);
      rt.vfs().remove("C:\\WINDOWS\\~STR1234.TMP", &e);
    }
    return script.button;
  }

  std::vector<std::string>& seen;
  Script script;
  win16::Runtime16* rt_ = nullptr;
  size_t calls = 0, afters = 0;
};

void test_protocol_seam() {
  using Call = ne16::Protocol16::Call;
  std::string dir = temp_dir("seam");
  const std::string module = dir + "\\SCRIPTED.AD";
  std::vector<std::string> seen;
  Script script;
  ne16::Ne16Lane::ProtocolFactory factory = [&](const ne16::Ne16Layout& layout, const Env&, std::string*) {
    seen.push_back("make " + layout.module_path.substr(layout.module_path.find_last_of('\\') + 1));
    return std::unique_ptr<ne16::Protocol16>(std::make_unique<ScriptedProtocol>(seen, script));
  };
  InputState input;
  Command set;
  set.kind = Command::Kind::set;

  // A run, one call per frame (ADMIPS=0): init's order, the results, SET, a stop, shutdown.
  {
    Env env = Env::parse({{"AD_ASSETS_DIR", dir}, {"ADMIPS", "0"}});
    VirtualClock clock(VirtualClock::Mode::fixed_step, 16667);
    Screen screen(640, 480);
    LaneContext ctx{env, screen, clock, input};
    script = Script{};
    script.results = {Call{Call::Kind::ok, 0}, Call{Call::Kind::toggle_events, 14}, Call{Call::Kind::cursor_on, 17},
                      Call{Call::Kind::cursor_off, 18}, Call{Call::Kind::stop, 9}};
    seen.clear();
    ne16::Ne16Lane lane(factory);
    bool ok = lane.init(module, ctx);
    CHECK(ok && joined(seen) == "make SCRIPTED.AD, configure_runtime, mount C:\\SAVER desktop, load",
          "init: the protocol's runtime options, then its disk, then the module: %s", joined(seen).c_str());
    if (!ok) return;
    CHECK(lane.runtime()->options().guest_dir == "C:\\SAVER" && lane.runtime()->options().desktop_palette &&
              std::string(lane.protocol()->name()) == "scripted",
          "the runtime has the protocol's options");
    auto step = [&] {
      clock.begin_frame();
      return lane.step();
    };
    seen.clear();
    CHECK(step() == StepResult::ok && joined(seen) == "call, after", "a frame: one call, then after_call: %s",
          joined(seen).c_str());
    CHECK(!lane.status().interactive && !lane.status().cursor, "ok: nothing changes");
    step();
    CHECK(lane.wants_events() && lane.status().interactive && lane.status().source == kStatusSourceAd3,
          "toggle_events: interactive (source %u)", lane.status().source);
    step();
    CHECK(lane.status().cursor, "cursor_on: the cursor shows");
    step();
    CHECK(!lane.status().cursor && lane.status().interactive, "cursor_off: hidden again, still interactive");
    // SET: the protocol says whether it concerns the module; the guest can be called, so at once.
    seen.clear();
    set.a = 1;
    set.b = 7;
    lane.on_command(set);
    set.a = 0;
    lane.on_command(set);
    CHECK(joined(seen) == "set 1 7, send, set 0 7", "SET: sent when the protocol takes it: %s", joined(seen).c_str());
    // A stop fails the step; shutdown still unloads and closes.
    seen.clear();
    CHECK(step() == StepResult::failed && joined(seen) == "call", "stop: the step fails, no after_call: %s",
          joined(seen).c_str());
    lane.shutdown();
    CHECK(joined(seen) == "call, unload, close", "shutdown: unload, then close: %s", joined(seen).c_str());
    CHECK(!lane.protocol() && !lane.runtime(), "shutdown releases the protocol, then the runtime");
  }
  // A wake (an After Dark 2.0 module's result 5): the frame's run ends there
  // (no after_call), the status says wake, and the module is called no more —
  // input and SET go nowhere; a headless run ends at the next step
  // (StepResult::finished: exit 0), a streamed one presents its last picture
  // until the front end ends it; shutdown unloads as usual.
  for (bool streamed : {false, true}) {
    Env env = Env::parse({{"AD_ASSETS_DIR", dir}, {"ADMAXDRAWS", "4"}, {"ADSTREAM", streamed ? "1" : "0"}});
    VirtualClock clock(VirtualClock::Mode::fixed_step, 16667);
    Screen screen(640, 480);
    InputState in;
    LaneContext ctx{env, screen, clock, in};
    script = Script{};
    script.results = {Call{Call::Kind::ok, 0}, Call{Call::Kind::ok, 0}, Call{Call::Kind::ok, 0},
                      Call{Call::Kind::ok, 0}, Call{Call::Kind::ok, 0}, Call{Call::Kind::wake, 5}};
    seen.clear();
    ne16::Ne16Lane lane(factory);
    bool ok = lane.init(module, ctx);
    CHECK(ok, "init for the wake (%s)", streamed ? "streamed" : "headless");
    if (!ok) continue;
    auto step = [&] {
      clock.begin_frame();
      return lane.step();
    };
    seen.clear();
    CHECK(step() == StepResult::ok && !lane.status().wake, "four calls, no wake yet");
    CHECK(step() == StepResult::ok && lane.status().wake && !lane.status().interactive,
          "%s: the call that woke ends the frame, which is presented with the wake in its status", streamed ? "streamed" : "headless");
    const std::string called = "call, after, call, after, call, after, call, after, call, after, call";
    CHECK(joined(seen) == called, "the second frame's run ends at the wake, without after_call: %s", joined(seen).c_str());
    Command k;
    k.kind = Command::Kind::key;
    k.a = 'A';
    k.b = 1;
    k.seq = 1;
    in.apply(k);
    lane.on_command(k);
    set.a = 1;
    set.b = 3;
    lane.on_command(set);
    CHECK(lane.status().unsettled == 0 && lane.status().eaten == 0, "input after the wake goes nowhere");
    if (!streamed) {
      CHECK(step() == StepResult::finished, "headless: the run ends at the next step (exit 0)");
    } else {
      const uint64_t f = lane.frames();
      for (int i = 0; i < 3; i++) CHECK(step() == StepResult::ok && lane.status().wake, "streamed: frame %d after the wake", i);
      CHECK(lane.frames() == f + 3, "streamed: the frames go on (%llu)", (unsigned long long)lane.frames());
    }
    CHECK(joined(seen) == called, "%s: no call, SET or send after the wake: %s", streamed ? "streamed" : "headless",
          joined(seen).c_str());
    lane.shutdown();
    CHECK(joined(seen) == called + ", unload, close", "shutdown unloads and closes: %s", joined(seen).c_str());
  }
  // ADDESKTOPPAL overrides the protocol's palette; a load that fails fails init.
  {
    Env env = Env::parse({{"AD_ASSETS_DIR", dir}, {"ADMIPS", "0"}, {"ADDESKTOPPAL", "0"}});
    VirtualClock clock(VirtualClock::Mode::fixed_step, 16667);
    Screen screen(640, 480);
    LaneContext ctx{env, screen, clock, input};
    script = Script{};
    script.load_ok = false;
    seen.clear();
    ne16::Ne16Lane lane(factory);
    CHECK(!lane.init(module, ctx) && joined(seen) == "make SCRIPTED.AD, configure_runtime, mount C:\\SAVER boot, load",
          "ADDESKTOPPAL=0 over the protocol's palette; the failed load fails init: %s", joined(seen).c_str());
    CHECK(!lane.protocol() && !lane.runtime(), "a failed init releases the protocol and the runtime");
  }
  // Long calls (ADMIPS on, at most two calls a frame): the second call blocks
  // for 40 ms of virtual time, so frames end inside it; a SET meanwhile waits
  // for the module's next call.
  {
    Env env = Env::parse({{"AD_ASSETS_DIR", dir}, {"ADMAXDRAWS", "2"}});
    VirtualClock clock(VirtualClock::Mode::fixed_step, 16667);
    Screen screen(640, 480);
    LaneContext ctx{env, screen, clock, input};
    script = Script{};
    script.wait_call = 2;
    script.wait_us = 40000;
    seen.clear();
    ne16::Ne16Lane lane(factory);
    bool ok = lane.init(module, ctx);
    CHECK(ok, "init with long calls");
    if (!ok) return;
    auto step = [&] {
      clock.begin_frame();
      return lane.step();
    };
    seen.clear();
    CHECK(step() == StepResult::ok && joined(seen) == "call, after, call", "two calls; the frame ends inside the second: %s",
          joined(seen).c_str());
    set.a = 1;
    set.b = 5;
    lane.on_command(set);
    CHECK(joined(seen) == "call, after, call, set 1 5", "SET during a suspended call: kept (%s)", joined(seen).c_str());
    int frames = 1;
    while (frames < 12 && std::find(seen.begin(), seen.end(), "waited") == seen.end()) {
      CHECK(step() == StepResult::ok, "a frame inside the call");
      frames++;
    }
    CHECK(frames >= 3 && frames < 12, "the call went on over %d frames", frames);
    CHECK(joined(seen) == "call, after, call, set 1 5, waited, after, send, call, after",
          "the call resumes and returns; the values reach the module before its next call: %s", joined(seen).c_str());
    lane.shutdown();
    CHECK(joined(seen) == "call, after, call, set 1 5, waited, after, send, call, after, unload, close",
          "shutdown: unload, then close: %s", joined(seen).c_str());
  }
  // At close, a call still suspended is abandoned (every level unwinds); then unload and close.
  {
    Env env = Env::parse({{"AD_ASSETS_DIR", dir}});
    VirtualClock clock(VirtualClock::Mode::fixed_step, 16667);
    Screen screen(640, 480);
    LaneContext ctx{env, screen, clock, input};
    script = Script{};
    script.wait_call = 1;
    script.wait_us = 10'000'000;  // longer than the run
    seen.clear();
    ne16::Ne16Lane lane(factory);
    bool ok = lane.init(module, ctx);
    CHECK(ok, "init with long calls");
    if (!ok) return;
    seen.clear();
    for (int i = 0; i < 3; i++) {
      clock.begin_frame();
      CHECK(lane.step() == StepResult::ok, "frame %d inside the call", i);
    }
    CHECK(joined(seen) == "call", "three frames, one call: %s", joined(seen).c_str());
    lane.shutdown();
    CHECK(joined(seen) == "call, unload, close", "the call abandoned, then unload and close: %s", joined(seen).c_str());
  }
  // A button: the protocol checks the slot before anything is loaded; the lane reports what the button left.
  {
    Env env = Env::parse({{"AD_ASSETS_DIR", dir}});
    VirtualClock clock(VirtualClock::Mode::realtime, 33333);
    Screen screen(640, 480);
    LaneContext ctx{env, screen, clock, input};
    auto configure = [&](int slot, std::string* json) {
      seen.clear();
      ne16::Ne16Lane lane(factory);
      ConfigureRequest req;
      req.slot = slot;
      return lane.configure(module, ctx, req, json);
    };
    std::string json;
    ConfigureResult r = configure(3, &json);
    CHECK(r == ConfigureResult::failed && json == configure_json(ConfigureResult::failed, 0, "control 3 is not a button", {}) &&
              joined(seen) == "make SCRIPTED.AD, check_button 3",
          "no such button: a failure, nothing loaded (%s; %s)", json.c_str(), joined(seen).c_str());
    script = Script{};
    script.button.message = "no reader";  // did not run
    r = configure(0, &json);
    CHECK(r == ConfigureResult::failed && json == configure_json(ConfigureResult::failed, 0, "no reader", {}) &&
              joined(seen) == "make SCRIPTED.AD, check_button 0, configure_button_runtime, mount C:\\SAVER boot, button 0",
          "a button that could not run: a failure, no close (%s; %s)", json.c_str(), joined(seen).c_str());
    script.button.ran = true;
    script.button.failure = "AD_SND.DLL lacks an entry point";
    script.button.message = "not this";
    r = configure(0, &json);
    CHECK(r == ConfigureResult::failed && json.find("\"message\":\"AD_SND.DLL lacks an entry point\"") != std::string::npos &&
              json.find("SCRIPTED.INI") != std::string::npos && joined(seen).find("button 0, close") != std::string::npos,
          "the protocol's failure, with what was written (%s; %s)", json.c_str(), joined(seen).c_str());
    script.button.failure.clear();
    script.button.message = "said so";
    r = configure(0, &json);
    CHECK(r == ConfigureResult::nothing && json.find("\"result\":\"nothing\",\"dialogs\":0,\"message\":\"said so\"") == 1 &&
              json.find("SCRIPTED.INI") != std::string::npos,
          "no dialog: nothing, with the module's message and what was written (%s)", json.c_str());
    CHECK(json.find("~STR1234.TMP") == std::string::npos, "a file written and deleted again is not listed as written (%s)",
          json.c_str());
  }
  RemoveDirectoryA(dir.c_str());
}

// Guest code in a code block of its own: its selector (the procedure is sel:0000).
uint16_t guest_code(win16::Runtime16& rt, const std::vector<uint8_t>& bytes) {
  win16::GlobalBlock* b = rt.global().alloc_block(uint32_t(bytes.size() + 16), true, 0, 0);
  rt.mem().memcpy(b->base, bytes.data(), bytes.size());
  return b->sel;
}

// A procedure that runs outer × (inner + 3) + 2 instructions and makes no API
// call: mov dx, outer; again: mov cx, inner; loop $; dec dx; jnz again; retf.
std::vector<uint8_t> busy_loop(uint16_t outer, uint16_t inner) {
  return {0xBA, uint8_t(outer), uint8_t(outer >> 8), 0xB9, uint8_t(inner), uint8_t(inner >> 8), 0xE2, 0xFE, 0x4A, 0x75, 0xF8,
          0xCB};
}

// A multimedia timer procedure, FAR PASCAL (wID, wMsg, dwUser, dw1, dw2) as
// MEMMIDI's MIDITIMERPROC: midiOutShortMsg(hmo, 0x00403C90), one note a period.
std::vector<uint8_t> note_timeproc(uint16_t hmo, uint32_t midi_out_short_msg) {
  const uint32_t f = midi_out_short_msg;
  return {0x68, uint8_t(hmo), uint8_t(hmo >> 8), 0x68, 0x40, 0x00, 0x68, 0x90, 0x3C, 0x9A, uint8_t(f), uint8_t(f >> 8),
          uint8_t(f >> 16), uint8_t(f >> 24), 0xCA, 0x10, 0x00};
}

uint32_t call_api(win16::Runtime16& rt, const char* module, const char* fn, std::initializer_list<win16::Arg16> args) {
  return rt.call_far(rt.thunk_far(*rt.shims().find_name(module, fn)), args);
}

// The events of a capture's .mid log (format 0, one tick a millisecond): (ms, message).
std::vector<std::pair<uint64_t, std::vector<uint8_t>>> mid_log(const std::string& path) {
  std::vector<std::pair<uint64_t, std::vector<uint8_t>>> out;
  std::vector<uint8_t> b;
  if (FILE* f = fopen(path.c_str(), "rb")) {
    for (int ch; (ch = fgetc(f)) != EOF;) b.push_back(uint8_t(ch));
    fclose(f);
  }
  size_t i = 22;  // MThd (14) + the MTrk header (8)
  uint64_t ms = 0;
  auto vlq = [&] {
    uint32_t v = 0;
    while (i < b.size()) {
      uint8_t x = b[i++];
      v = (v << 7) | (x & 0x7F);
      if (!(x & 0x80)) break;
    }
    return v;
  };
  while (i < b.size()) {
    ms += vlq();
    if (i >= b.size()) break;
    uint8_t st = b[i];
    if (st == 0xFF) {
      i += 2;
      i += vlq();
    } else if (st == 0xF0 || st == 0xF7) {
      i++;
      i += vlq();
    } else {
      size_t n = (st & 0xF0) == 0xC0 || (st & 0xF0) == 0xD0 ? 2 : 3;
      if (i + n > b.size()) break;
      out.push_back({ms, std::vector<uint8_t>(b.begin() + ptrdiff_t(i), b.begin() + ptrdiff_t(i + n))});
      i += n;
    }
  }
  return out;
}

// What the lane's machinery does by the protocol's answers, on a scripted
// protocol (lane.hh "Input and status", "Pacing", "Sound"): KEY lines kept
// from a protocol that takes no key messages, overruns carried (below
// kMaxOwedBudgets budgets) for one that carries them, the protocol's own
// pixel cost, the audio engine never taken past a timer period the guest has
// not had delivered, by the lane or by run_host, and frames that make no call
// still delivering timer periods and input.
void test_lane_machinery() {
  std::string dir = temp_dir("machinery");
  const std::string module = dir + "\\SCRIPTED.AD";
  std::vector<std::string> seen;
  Script script;
  ScriptedProtocol* proto = nullptr;
  ne16::Ne16Lane::ProtocolFactory factory = [&](const ne16::Ne16Layout&, const Env&, std::string*) {
    auto p = std::make_unique<ScriptedProtocol>(seen, script);
    proto = p.get();
    return std::unique_ptr<ne16::Protocol16>(std::move(p));
  };

  // KEY lines: a protocol that takes key messages finds them in the saver
  // window's queue (GetInputState), unsettled until the guest has had its
  // say; one that takes none (IMX) never has them queued, only in the key
  // state (GetAsyncKeyState).
  for (bool keys : {true, false}) {
    Env env = Env::parse({{"AD_ASSETS_DIR", dir}, {"ADMAXDRAWS", "1"}});
    VirtualClock clock(VirtualClock::Mode::fixed_step, 16667);
    Screen screen(640, 480);
    InputState in;
    LaneContext ctx{env, screen, clock, in};
    script = Script{};
    script.keys = keys;
    uint32_t queued = 99, shift = 0;
    script.on_call = [&](win16::Runtime16& rt, size_t) {
      queued = call_api(rt, "USER", "GetInputState", {}) & 0xFFFF;
      shift = call_api(rt, "USER", "GetAsyncKeyState", {win16::w16(VK_SHIFT)}) & 0xFFFF;
    };
    ne16::Ne16Lane lane(factory);
    if (!lane.init(module, ctx)) {
      CHECK(false, "init for the keys");
      continue;
    }
    Command k;
    k.kind = Command::Kind::key;
    k.a = VK_SHIFT;
    k.b = 1;
    k.seq = 7;
    in.apply(k);  // run_host updates the input state before the lane sees the line (lane.h)
    lane.on_command(k);
    const uint64_t unsettled = lane.status().unsettled;
    clock.begin_frame();
    lane.step();
    CHECK(keys ? queued == 1 && unsettled == 7 : queued == 0 && unsettled == 0,
          "%s key messages: GetInputState %u, unsettled %llu", keys ? "takes" : "takes no", queued,
          (unsigned long long)unsettled);
    CHECK(shift & 0x8000, "the key state has Shift down either way (%04X)", shift);
    lane.shutdown();
  }

  // Carried overruns: every call runs `outer` × (`inner` + 3) instructions
  // and no API call, against a budget of 25 MIPS × 16,667 µs = 416,675.
  struct Pace {
    size_t calls = 0, idle = 0, idle_run = 0, afters = 0;
    bool after_long = true;  // the frame after the one that finished a long call made a call
    size_t long_end_new = 0;  // the calls that frame started once the long call had returned
    std::vector<size_t> per_frame;  // the calls each frame made
    uint32_t speed = 0, max_draws = 0;  // the lane's machine speed (percent) and calls a frame at most
  };
  // The scripted protocol's speed knob (Protocol16::speed_knob) for the next
  // pace() runs: none unless a test names one.
  const char* pace_speed_knob = nullptr;
  // A long call first (first_wait_us of waiting): its loop after the wait as
  // every call's, or with wait_only its wait alone, or with work_first its
  // loop before the wait (a frame then ends inside it after the work).
  auto pace = [&](bool carry, const std::map<std::string, std::string>& extra, uint16_t outer, uint16_t inner, int frames,
                  uint64_t first_wait_us = 0, uint16_t after_outer = 0, uint16_t after_inner = 0, bool wait_only = false,
                  bool work_first = false) {
    Pace out;
    std::map<std::string, std::string> vars = extra;
    vars["AD_ASSETS_DIR"] = dir;
    Env env = Env::parse(vars);
    VirtualClock clock(VirtualClock::Mode::fixed_step, 16667);
    Screen screen(640, 480);
    InputState in;
    LaneContext ctx{env, screen, clock, in};
    script = Script{};
    script.carry = carry;
    script.speed_knob = pace_speed_knob;
    script.quiet = true;
    script.wait_call = first_wait_us && !work_first ? 1 : 0;  // a long call first: frames end inside it
    script.wait_us = first_wait_us;
    uint16_t loop = 0, after_loop = 0;
    script.on_load = [&](win16::Runtime16& rt) {
      loop = guest_code(rt, busy_loop(outer, inner));
      if (after_outer) after_loop = guest_code(rt, busy_loop(after_outer, after_inner));
    };
    script.on_call = [&](win16::Runtime16& rt, size_t n) {
      if (wait_only && n == 1) return;
      rt.call_far(uint32_t(loop) << 16, std::initializer_list<win16::Arg16>{});
      if (work_first && n == 1) {
        rt.wait_until_us(rt.peek_us() + first_wait_us);
        seen.push_back("waited");
      }
    };
    if (after_outer) {
      script.on_after = [&](win16::Runtime16& rt) {
        rt.call_far(uint32_t(after_loop) << 16, std::initializer_list<win16::Arg16>{});
      };
    }
    seen.clear();
    ne16::Ne16Lane lane(factory);
    if (!lane.init(module, ctx)) {
      CHECK(false, "init for the pacing");
      return out;
    }
    out.speed = lane.speed_percent();
    out.max_draws = lane.max_draws();
    size_t run = 0;
    bool finished_long = false;
    for (int f = 0; f < frames; f++) {
      const size_t before = proto->calls;
      const bool waited_before = std::find(seen.begin(), seen.end(), "waited") != seen.end();
      clock.begin_frame();
      if (lane.step() != StepResult::ok) {
        CHECK(false, "pacing: frame %d failed", f);
        break;
      }
      out.per_frame.push_back(proto->calls - before);
      const bool made_call = proto->calls != before;
      if (finished_long && !made_call) out.after_long = false;
      finished_long = !waited_before && std::find(seen.begin(), seen.end(), "waited") != seen.end();
      if (finished_long) out.long_end_new = proto->calls - before;  // the long call was counted as it began
      // A frame inside the long call, or the one it ended in, is no idle frame.
      if (made_call || (first_wait_us && !waited_before)) {
        run = 0;
      } else {
        out.idle++;
        out.idle_run = std::max(out.idle_run, ++run);
      }
    }
    out.calls = proto->calls;
    out.afters = proto->afters;
    lane.shutdown();
    return out;
  };
  // 11 × 64,003 + 2 = 704,035 (1.69 budgets, SWTEXT's pass at After Dark's
  // pixel cost): the 25-MIPS model's 35.5 calls in 60 frames, never two
  // frames in a row without one; the protocol's message loop runs in those
  // frames too.
  Pace p = pace(true, {}, 11, 64000, 60);
  CHECK(p.calls >= 35 && p.calls <= 36 && p.idle == 60 - p.calls && p.idle_run == 1 && p.afters == p.calls + p.idle,
        "1.69 budgets a call, carried: %zu calls in 60 frames, %zu without one (at most %zu in a row), after_call %zu",
        p.calls, p.idle, p.idle_run, p.afters);
  p = pace(true, {{"ADNE16IMXCARRY", "0"}}, 11, 64000, 60);
  CHECK(p.calls == 60 && p.idle == 0, "ADNE16IMXCARRY=0: a call every frame (%zu)", p.calls);
  p = pace(false, {}, 11, 64000, 60);
  CHECK(p.calls == 60 && p.idle == 0, "a protocol that carries nothing (AD3): a call every frame (%zu)", p.calls);
  p = pace(true, {{"ADMIPS", "0"}}, 11, 64000, 60);
  CHECK(p.calls == 60 && p.idle == 0, "ADMIPS=0: no budget, nothing carried (%zu)", p.calls);
  // 24 × 65,473 + 2 = 1,571,354 (3.77 budgets, TRENCH's pass at the
  // Intermission pixel cost): the model's 15.9 calls a second, three frames
  // in a row without one between them.
  p = pace(true, {}, 24, 65470, 60);
  CHECK(p.calls >= 15 && p.calls <= 16 && p.idle == 60 - p.calls && p.idle_run == 3 && p.afters == p.calls + p.idle,
        "3.77 budgets a call: %zu calls in 60 frames, at most %zu in a row without one, after_call %zu", p.calls, p.idle_run,
        p.afters);
  // 32 × 65,003 + 2 = 2,080,098 (4.99 budgets): below the bound, still the model's rate (12 a second).
  p = pace(true, {}, 32, 65000, 60);
  CHECK(p.calls >= 12 && p.calls <= 13 && p.idle_run == 4, "4.99 budgets a call: %zu calls in 60 frames, at most %zu idle in a row",
        p.calls, p.idle_run);
  // 64 × 65,003 + 2 = 4,160,194 (9.98 budgets): what is owed stays below
  // kMaxOwedBudgets budgets, so a call every kMaxOwedBudgets-th frame and
  // never more frames than one less in a row without one.
  constexpr uint64_t kBound = ne16::Ne16Lane::kMaxOwedBudgets;
  p = pace(true, {}, 64, 65000, 60);
  CHECK(p.calls == 60 / kBound && p.idle_run == kBound - 1,
        "9.98 budgets a call: capped at %llu budgets, %zu calls in 60 frames, at most %zu idle in a row",
        (unsigned long long)kBound, p.calls, p.idle_run);
  // Frames without a call still run the pumps (MEMMIDI's timer procedures,
  // the message loop), and what those cost is never owed again: with pumps
  // of 20,003 + 2 = 20,005 in every frame, the 9.98-budget call still comes
  // every kMaxOwedBudgets-th frame...
  p = pace(true, {}, 64, 65000, 60, 0, 1, 20000);
  CHECK(p.calls == 60 / kBound && p.idle_run == kBound - 1,
        "9.98 budgets a call, pumps of 20,005 in every frame: %zu calls in 60 frames, at most %zu idle in a row", p.calls,
        p.idle_run);
  // ...and with pumps of 8 × 62,503 + 2 = 500,026 (1.2 budgets, more than
  // such a frame pays back), the module is still called at least every
  // kMaxOwedBudgets-th frame, not starved for good.
  p = pace(true, {}, 24, 65470, 60, 0, 8, 62500);
  CHECK(p.calls >= 60 / kBound && p.idle_run <= kBound - 1,
        "pumps of 1.2 budgets in frames without a call: %zu calls in 60 frames, at most %zu idle in a row", p.calls,
        p.idle_run);
  // A long call first (40 ms of waiting: frames end inside it): nothing is
  // carried from it, so the frame after the one it ended in makes a call —
  // with 1.69 budgets of work after the wait, and with 3.77, where owing what
  // the frame it returned in did beyond its budget would leave more than a
  // budget owed and the next frame without a call.
  p = pace(true, {}, 11, 64000, 30, 40000);
  CHECK(p.after_long && p.idle_run == 1 && p.calls >= 14, "after a long call nothing is owed (%zu calls, %s)", p.calls,
        p.after_long ? "the next frame called" : "the next frame made NO call");
  p = pace(true, {}, 24, 65470, 30, 40000);
  CHECK(p.after_long && p.long_end_new == 0 && p.idle_run == 3,
        "after a long call with 3.77 budgets past its wait nothing is owed (%zu calls, %s, at most %zu in a row without one)",
        p.calls, p.after_long ? "the next frame called" : "the next frame made NO call", p.idle_run);
  // The passes the frame it returned in runs after it are carried as any
  // frame's: a long call that only waits, then calls of 3.77 budgets. The
  // first starts and completes in that frame, which owes 2.77 budgets of it,
  // so the next frame makes no call.
  p = pace(true, {}, 24, 65470, 30, 40000, 0, 0, true);
  CHECK(!p.after_long && p.long_end_new == 1 && p.idle_run == 3,
        "a pass after a long call returned, in its frame: carried (%zu started there, %s, at most %zu in a row without one)",
        p.long_end_new, p.after_long ? "the next frame made a call" : "the next frame made none", p.idle_run);
  // A frame that ends inside a call owes nothing either: a long call that
  // works first (3.77 budgets) and then waits, so a frame ends inside it
  // after the work. Owing that work would leave the frame it returns in no
  // budget for a new pass; owing nothing, that frame starts one, which it
  // carries as above.
  p = pace(true, {}, 24, 65470, 30, 1, 0, 0, false, true);
  CHECK(p.long_end_new == 1 && !p.after_long && p.idle_run == 3,
        "a frame that ended inside a call owes nothing (%zu started in the frame it returned in, %s, at most %zu in a "
        "row without one)",
        p.long_end_new, p.after_long ? "the next frame made a call" : "the next frame made none", p.idle_run);

  // The modeled machine's speed (lane.hh "Pacing", Speed): the protocol's
  // knob (Protocol16::speed_knob, the IMX protocol's ADNE16IMXSPEED), a
  // percent that scales each frame's budget and ADMAXDRAWS down and the bound
  // on what is owed up. Unset or 100, nothing changes, frame for frame; a
  // protocol without the knob ignores it.
  {
    const char* const kKnob = ne16::kImxSpeedKnob;
    const std::map<std::string, std::string> at100 = {{kKnob, "100"}}, at50 = {{kKnob, "50"}}, at25 = {{kKnob, "25"}},
                                             at6 = {{kKnob, "6"}};
    // Unset and 100: the frames' calls exactly as without the knob, for a
    // light call, the 1.69-, 3.77- and 9.98-budget ones (the last clamped).
    struct Work {
      uint16_t outer, inner;
    };
    for (const Work w : {Work{1, 20000}, Work{11, 64000}, Work{24, 65470}, Work{64, 65000}}) {
      pace_speed_knob = nullptr;
      const Pace none = pace(true, {}, w.outer, w.inner, 60);
      pace_speed_knob = kKnob;
      const Pace unset = pace(true, {}, w.outer, w.inner, 60);
      const Pace full = pace(true, at100, w.outer, w.inner, 60);
      CHECK(unset.per_frame == none.per_frame && full.per_frame == none.per_frame && none.calls > 0 &&
                unset.speed == 100 && full.speed == 100 && full.max_draws == 64 && none.idle_run == full.idle_run,
            "%u x (%u + 3): the knob unset and at 100 change nothing (%zu, %zu and %zu calls, speed %u/%u, %u a frame "
            "at most)",
            unsigned(w.outer), unsigned(w.inner), none.calls, unset.calls, full.calls, unset.speed, full.speed,
            full.max_draws);
    }
    pace_speed_knob = nullptr;
    p = pace(true, at25, 1, 20000, 60);
    CHECK(p.speed == 100 && p.max_draws == 64 && p.calls == 1250,
          "a protocol without the knob ignores it: speed %u, %u calls a frame at most, %zu calls (want 1250)", p.speed,
          p.max_draws, p.calls);
    pace_speed_knob = kKnob;
    // The budget: a call of 1 × 20,003 + 2 = 20,005, never capped (20.8 a
    // frame at 100, 1.25 at 6), makes ceil(60 frames × the budget / 20,005)
    // calls — the budget being 416,675 × percent / 100.
    for (const uint32_t percent : {100u, 50u, 25u, 12u, 6u}) {
      const uint64_t budget = 416675ull * percent / 100;
      const uint64_t want = (60 * budget + 20004) / 20005;
      p = pace(true, {{kKnob, std::to_string(percent)}}, 1, 20000, 60);
      CHECK(p.speed == percent && p.calls == want && p.idle == 0,
            "at %u%% the budget is %llu: %zu calls of 20,005 in 60 frames (want %llu), %zu without one", percent,
            (unsigned long long)budget, p.calls, (unsigned long long)want, p.idle);
    }
    // The cap: ADMAXDRAWS × percent / 100 to the nearest call, at least one —
    // with calls of 6 instructions, every frame makes that many.
    struct Cap {
      const char* max_draws;
      uint32_t percent, want;
    };
    for (const Cap c : {Cap{"64", 100, 64}, Cap{"64", 25, 16}, Cap{"64", 12, 8}, Cap{"64", 6, 4}, Cap{"10", 25, 3},
                        Cap{"10", 50, 5}, Cap{"10", 6, 1}, Cap{"1", 1, 1}}) {
      p = pace(true, {{"ADMAXDRAWS", c.max_draws}, {kKnob, std::to_string(c.percent)}}, 1, 1, 30);
      const bool every = std::all_of(p.per_frame.begin(), p.per_frame.end(), [&](size_t n) { return n == c.want; });
      CHECK(p.max_draws == c.want && every && p.calls == 30 * c.want,
            "ADMAXDRAWS=%s at %u%%: %u calls a frame at most (want %u), %zu calls in 30 frames", c.max_draws, c.percent,
            p.max_draws, c.want, p.calls);
    }
    // The bound on what is owed is the same work at every speed
    // (kMaxOwedBudgets × 100 / percent budgets), so a heavy call slows with the
    // machine instead of being clamped: the 4.99-budget call, unclamped at
    // 100 (13 calls in 60 frames), is 9.98 budgets at 50 (7) and 19.96 at 25
    // (4) — at a bound of six budgets both would be clamped to a call every
    // sixth frame (10), the same at 50 and at 25.
    struct Owed {
      uint32_t percent;
      size_t calls, idle_run;
    };
    for (const Owed o : {Owed{100, 13, 4}, Owed{50, 7, 9}, Owed{25, 4, 19}}) {
      p = pace(true, {{kKnob, std::to_string(o.percent)}}, 32, 65000, 60);
      CHECK(p.calls == o.calls && p.idle_run == o.idle_run,
            "4.99 budgets of the full machine at %u%%: %zu calls in 60 frames (want %zu), at most %zu in a row without "
            "one (want %zu)",
            o.percent, p.calls, o.calls, p.idle_run, o.idle_run);
    }
    // ...and the clamped 9.98-budget call comes every kMaxOwedBudgets × 100 /
    // percent frames: every 6th at 100, 12th at 50, 24th at 25.
    for (const uint32_t percent : {100u, 50u, 25u}) {
      const size_t every = size_t(kBound * 100 / percent);
      p = pace(true, {{kKnob, std::to_string(percent)}}, 64, 65000, 60);
      CHECK(p.calls == (60 + every - 1) / every && p.idle_run == every - 1,
            "9.98 budgets of the full machine at %u%%: clamped at %zu budgets, %zu calls in 60 frames, at most %zu in a "
            "row without one",
            percent, every, p.calls, p.idle_run);
    }
    // What the knob says: a whole percent, 1..100; outside it clamped, anything
    // else (logged) 100.
    struct Value {
      const char* text;
      uint32_t want;
    };
    for (const Value v : {Value{"", 100}, Value{"33", 33}, Value{"1", 1}, Value{"0", 1}, Value{"-5", 1},
                          Value{"250", 100}, Value{"12.5", 100}, Value{"fast", 100}}) {
      p = pace(true, {{kKnob, v.text}}, 1, 20000, 2);
      CHECK(p.speed == v.want, "%s='%s': %u%% (want %u)", kKnob, v.text, p.speed, v.want);
    }
    // No budget (ADMIPS=0): one call a frame, whatever the speed.
    p = pace(true, {{"ADMIPS", "0"}, {kKnob, "25"}}, 11, 64000, 60);
    CHECK(p.calls == 60 && p.idle == 0 && p.max_draws == 1, "ADMIPS=0 at 25%%: a call every frame (%zu)", p.calls);
    // A call heavier than the slowed budget: the 1.69-budget call is 3.38
    // budgets at 50 and 6.76 at 25 (below their bounds of 12 and 24), so it
    // comes half and a quarter as often (36 calls in 60 frames at 100), and
    // at 6 (28.2 budgets, below 100) every 28th frame.
    p = pace(true, at50, 11, 64000, 60);
    const Pace q = pace(true, at25, 11, 64000, 60);
    const Pace r = pace(true, at6, 11, 64000, 60);
    CHECK(p.calls == 18 && q.calls == 9 && r.calls == 3 && r.idle_run == 27,
          "the 1.69-budget call at 50%%, 25%% and 6%%: %zu, %zu and %zu calls in 60 frames (want 18, 9 and 3), at 6%% at "
          "most %zu in a row without one (want 27)",
          p.calls, q.calls, r.calls, r.idle_run);
    pace_speed_knob = nullptr;
  }

  // The pixel cost is the protocol's (Protocol16::pixel_cost): After Dark's
  // ADPIXCOST, 2, and an Intermission protocol's ADNE16IMXPIXCOST, 4, neither
  // knob changing the other's; and it is what a blit costs the budget — a
  // 640x480 PatBlt is one API call (500) and 307,200 pixels.
  {
    ne16::Ne16Layout layout;
    layout.module_path = module;
    const ne16::Protocol16::PixelCost imx = ne16::make_imx_protocol(layout)->pixel_cost();
    const ne16::Protocol16::PixelCost ad3 = ne16::make_ad3_protocol(layout)->pixel_cost();
    CHECK(std::string(imx.knob) == "ADNE16IMXPIXCOST" && imx.def == ne16::kImxPixelCost && ne16::kImxPixelCost == 4 &&
              std::string(ad3.knob) == "ADPIXCOST" && ad3.def == 2,
          "the protocols' pixel costs: IMX %s %u, AD3 %s %u", imx.knob, imx.def, ad3.knob, ad3.def);
    // The speed knob (lane.hh "Pacing", Speed) is the IMX protocol's alone.
    const char* imx_speed = ne16::make_imx_protocol(layout, ne16::ImxForm::asa)->speed_knob();
    CHECK(imx_speed && std::string(imx_speed) == "ADNE16IMXSPEED" && std::string(ne16::kImxSpeedKnob) == imx_speed &&
              !ne16::make_ad3_protocol(layout)->speed_knob() && !ne16::make_scr_protocol(layout)->speed_knob(),
          "the protocols' speed knobs: IMX %s, AD3 and scr none", imx_speed ? imx_speed : "(none)");
    struct Case {
      bool imx;
      std::map<std::string, std::string> vars;
      uint32_t want;
    };
    const Case cases[] = {{true, {}, 4},
                          {true, {{"ADPIXCOST", "9"}}, 4},
                          {true, {{"ADNE16IMXPIXCOST", "6"}, {"ADPIXCOST", "9"}}, 6},
                          {false, {}, 2},
                          {false, {{"ADPIXCOST", "3"}}, 3},
                          {false, {{"ADNE16IMXPIXCOST", "6"}}, 2}};
    for (const Case& k : cases) {
      std::map<std::string, std::string> vars = k.vars;
      vars["AD_ASSETS_DIR"] = dir;
      vars["ADMAXDRAWS"] = "1";
      Env env = Env::parse(vars);
      VirtualClock clock(VirtualClock::Mode::fixed_step, 16667);
      Screen screen(640, 480);
      InputState in;
      LaneContext ctx{env, screen, clock, in};
      script = Script{};
      script.quiet = true;
      if (k.imx) {
        script.pixel_knob = "ADNE16IMXPIXCOST";
        script.pixel_def = ne16::kImxPixelCost;
      }
      uint64_t blit = 0;
      script.on_call = [&](win16::Runtime16& rt, size_t) {
        const uint16_t hdc = win16::gdi16_screen_dc(rt, win16::user16_saver_window(rt));
        const uint64_t w0 = rt.work_insns();
        call_api(rt, "GDI", "PatBlt", {win16::w16(hdc), win16::w16(0), win16::w16(0), win16::w16(640), win16::w16(480),
                                       win16::l16(BLACKNESS)});
        blit = rt.work_insns() - w0;
      };
      ne16::Ne16Lane lane(factory);
      if (!lane.init(module, ctx)) {
        CHECK(false, "init for the pixel cost");
        continue;
      }
      clock.begin_frame();
      lane.step();
      const uint32_t cost = lane.runtime()->options().pixel_cost_insns;
      const uint64_t want_blit = 500 + 307200ull * k.want;
      std::string vars_text;
      for (const auto& [n, v] : k.vars) vars_text += " " + n + "=" + v;
      CHECK(cost == k.want && blit >= want_blit && blit < want_blit + 100,
            "%s protocol%s: a pixel costs %u (want %u), a 640x480 PatBlt %llu", k.imx ? "an Intermission" : "an After Dark",
            vars_text.c_str(), cost, k.want, (unsigned long long)blit);
      lane.shutdown();
    }
  }

  // The audio engine and a self-sequencing guest, through run_host: a 4 ms
  // timer procedure plays a note a period (MEMMIDI's shape). The lane
  // advances the engine at the end of each step, never past a period not yet
  // delivered to the guest, and run_host leaves a step's end to a lane that
  // advanced the engine itself (host.cc LaneEngine). Dated at their due times
  // and never behind the engine, the notes keep the 4 ms grid in the
  // capture's .mid log (a) when every frame's call is 10 ms of guest work
  // with no API call, so each frame ends with periods due that only the next
  // frame's pump delivers, and (b) when one call also blocks for 100 ms (a
  // synchronous sndPlaySound's wait): the frames resume inside it with no
  // delivery point at all, and an advance to the core clock after each of
  // them would render past their periods and bunch those notes.
  for (const bool blocks : {false, true}) {
    const std::string wav = dir + "\\timer.wav", mid = dir + "\\timer.mid";
    Env env = Env::parse({{"AD_ASSETS_DIR", dir}, {"ADMAXDRAWS", "1"}, {"ADFRAMES", "61"}, {"ADAUDIOOUT", wav}});
    script = Script{};
    script.quiet = true;
    if (blocks) {
      script.wait_call = 3;
      script.wait_us = 100000;
    }
    uint16_t loop = 0, hmo = 0, timer = 0;
    using win16::l16;
    using win16::w16;
    script.on_load = [&](win16::Runtime16& rt) {
      loop = guest_code(rt, busy_loop(16, 62500));  // 1,000,050 instructions: 10 ms
      const uint32_t lphmo = uint32_t(rt.global().alloc(win16::GlobalHeap16::kZeroInit, 0x10)) << 16;
      call_api(rt, "MMSYSTEM", "midiOutOpen", {l16(lphmo), w16(0xFFFF), l16(0), l16(0), l16(0)});
      hmo = rt.rd16(lphmo);
      const uint32_t short_msg = rt.thunk_far(*rt.shims().find_name("MMSYSTEM", "midiOutShortMsg"));
      const uint16_t proc = guest_code(rt, note_timeproc(hmo, short_msg));
      timer = uint16_t(call_api(rt, "MMSYSTEM", "timeSetEvent", {w16(4), w16(4), l16(uint32_t(proc) << 16), l16(0), w16(1)}));
    };
    size_t calls = 0;  // counted here: run_host's shutdown frees the protocol
    script.on_call = [&](win16::Runtime16& rt, size_t n) {
      calls = n;
      rt.call_far(uint32_t(loop) << 16, std::initializer_list<win16::Arg16>{});
    };
    script.on_unload = [&](win16::Runtime16& rt) {
      call_api(rt, "MMSYSTEM", "timeKillEvent", {win16::w16(timer)});
      call_api(rt, "MMSYSTEM", "midiOutClose", {win16::w16(hmo)});
    };
    seen.clear();
    ne16::Ne16Lane lane(factory);
    HostIo io;
    io.go_wait = false;
    const HostResult r = run_host(lane, module, env, io);  // one virtual second
    const bool waited = std::find(seen.begin(), seen.end(), "waited") != seen.end();
    std::vector<uint64_t> ons;
    for (const auto& [ms, m] : mid_log(mid)) {
      if (m.size() == 3 && m[0] == 0x90 && m[2] > 0) ons.push_back(ms);
    }
    size_t off_grid = 0;
    for (size_t k = 1; k < ons.size(); k++) off_grid += ons[k] - ons[k - 1] < 3 || ons[k] - ons[k - 1] > 5;
    // Blocking, six frames resumed inside the wait make no call.
    CHECK(r.exit_code == 0 && r.frames == 61 && hmo && timer && waited == blocks && (blocks ? calls <= 56 : calls == 61) &&
              ons.size() >= 240 && off_grid == 0,
          "%s: exit %d, %llu frames, %zu calls; %zu note-ons, %zu of them off the 4 ms grid",
          blocks ? "a call blocks 100 ms" : "10 ms of work a frame", r.exit_code, (unsigned long long)r.frames, calls,
          ons.size(), off_grid);
    DeleteFileA(wav.c_str());
    DeleteFileA(mid.c_str());
  }

  // Frames without a call (carried overruns) still do what goes on between
  // calls (lane.hh "Pacing"): the same note procedure every 4 ms, the carry
  // on, every call 3.77 budgets of work with no API call, so that three
  // frames in four make none. (a) Each of those frames delivers the timer
  // periods due by its pump, 4 or 5 of them (16.7 ms of 4 ms periods; there
  // is no other delivery point in it), and the notes keep the 4 ms grid
  // across them. The notes alone would not show a frame that delivered
  // nothing: a period delivered late still dates its note at its due time
  // (win16/sound16.hh), so the periods are counted per frame. (b) A MOUSE
  // line queued before one of them reaches the saver window's queue in that
  // step (the message loop's GetInputState sees its button) and is settled
  // after it. The lane is stepped here, so that each frame is known to have
  // made a call or none; the engine is the test's own, capturing as
  // run_host's would.
  {
    using win16::l16;
    using win16::w16;
    const std::string wav = dir + "\\idle.wav", mid = dir + "\\idle.mid";
    Env env = Env::parse({{"AD_ASSETS_DIR", dir}, {"ADSOUND", "1"}, {"ADAUDIOLIVE", "0"}, {"ADAUDIOOUT", wav}});
    std::unique_ptr<audio::Engine> engine = audio::make_engine(audio::Config::from_env(env));  // outlives the lane
    VirtualClock clock(VirtualClock::Mode::fixed_step, 16667);
    Screen screen(640, 480);
    InputState in;
    LaneContext ctx{env, screen, clock, in};
    ctx.audio = engine.get();
    script = Script{};
    script.carry = true;
    script.quiet = true;
    uint16_t loop = 0, hmo = 0, timer = 0;
    script.on_load = [&](win16::Runtime16& rt) {
      loop = guest_code(rt, busy_loop(24, 65470));  // 1,571,354 instructions: 3.77 budgets
      const uint32_t lphmo = uint32_t(rt.global().alloc(win16::GlobalHeap16::kZeroInit, 0x10)) << 16;
      call_api(rt, "MMSYSTEM", "midiOutOpen", {l16(lphmo), w16(0xFFFF), l16(0), l16(0), l16(0)});
      hmo = rt.rd16(lphmo);
      const uint32_t short_msg = rt.thunk_far(*rt.shims().find_name("MMSYSTEM", "midiOutShortMsg"));
      const uint16_t proc = guest_code(rt, note_timeproc(hmo, short_msg));
      timer = uint16_t(call_api(rt, "MMSYSTEM", "timeSetEvent", {w16(4), w16(4), l16(uint32_t(proc) << 16), l16(0), w16(1)}));
    };
    script.on_call = [&](win16::Runtime16& rt, size_t) {
      rt.call_far(uint32_t(loop) << 16, std::initializer_list<win16::Arg16>{});
    };
    // The message loop looks at the queue only in the frames a MOUSE line was
    // queued for: GetInputState, an API call, is a delivery point of its own.
    bool look = false;
    uint32_t button = 0;
    script.on_after = [&](win16::Runtime16& rt) {
      if (look) button = call_api(rt, "USER", "GetInputState", {}) & 0xFFFF;
    };
    script.on_unload = [&](win16::Runtime16& rt) {
      call_api(rt, "MMSYSTEM", "timeKillEvent", {w16(timer)});
      call_api(rt, "MMSYSTEM", "midiOutClose", {w16(hmo)});
    };
    ne16::Ne16Lane lane(factory);
    const bool ok = lane.init(module, ctx);
    CHECK(ok && hmo && timer, "init for the frames without a call (hmo %u, timer %u)", hmo, timer);
    size_t idle = 0, mice = 0, mice_in_step = 0;
    uint64_t fewest = UINT64_MAX;  // periods a frame without a call (and without GetInputState) delivered
    bool called = false;
    for (int f = 0; ok && f < 60; f++) {
      win16::Runtime16& rt = *lane.runtime();
      const size_t before = proto->calls;
      const uint64_t periods = win16::timer16_stats(rt).calls;
      // A frame after a call pays it back (3.77 budgets): it makes none.
      look = called;
      const uint64_t seq = 1000 + uint64_t(f);
      uint64_t unsettled = 0;
      if (look) {
        Command m;
        m.kind = Command::Kind::mouse;
        m.a = 100 + f;
        m.b = 50;
        m.c = mice % 2 ? 0 : int32_t(kMouseLeft);  // down, then up: a button message each time
        m.seq = seq;
        in.apply(m);  // run_host updates the input state before the lane sees the line (lane.h)
        lane.on_command(m);
        unsettled = lane.status().unsettled;
        button = 0;
        mice++;
      }
      clock.begin_frame();
      if (lane.step() != StepResult::ok) {
        CHECK(false, "frames without a call: frame %d failed", f);
        break;
      }
      called = proto->calls != before;
      if (look) mice_in_step += !called && unsettled == seq && lane.status().unsettled == 0 && button == 1;
      if (!called) {
        idle++;
        if (!look) fewest = std::min(fewest, win16::timer16_stats(rt).calls - periods);
      }
    }
    // As run_host ends: the captures finalized, then the lane lets go.
    engine->shutdown(clock.now_us());
    lane.shutdown();
    std::vector<uint64_t> ons;
    for (const auto& [ms, m] : mid_log(mid)) {
      if (m.size() == 3 && m[0] == 0x90 && m[2] > 0) ons.push_back(ms);
    }
    size_t off_grid = 0;
    for (size_t k = 1; k < ons.size(); k++) off_grid += ons[k] - ons[k - 1] < 3 || ons[k] - ons[k - 1] > 5;
    CHECK(idle >= 40 && fewest != UINT64_MAX && fewest >= 4 && ons.size() >= 240 && off_grid == 0,
          "carried: %zu of 60 frames without a call, each delivering at least %llu timer periods (want 4); %zu note-ons, "
          "%zu of them off the 4 ms grid",
          idle, (unsigned long long)(fewest == UINT64_MAX ? 0 : fewest), ons.size(), off_grid);
    CHECK(mice >= 10 && mice_in_step == mice,
          "MOUSE lines queued before a frame without a call: %zu of %zu reached the queue in that step and were settled",
          mice_in_step, mice);
    DeleteFileA(wav.c_str());
    DeleteFileA(mid.c_str());
  }
  RemoveDirectoryA(dir.c_str());
}

// The AD3 protocol through Protocol16 (ad3_protocol.cc), on BridgeRig's
// host-implemented module and an AD_SND under the name the protocol loads: the
// lane's AD3 code as it moved — the bridge and palette choices, the controls
// (the records' defaults, ADCVSET over them), LOADADMODULE16's sequence,
// DRAWFRAME's results mapped for the lane, SET, UNLOAD, and configure mode's
// BUTTONPUSHED16 with its outcomes.
struct Ad3Rig : BridgeRig {
  // An AD_SND of AD 3.x's entry set (without adwStopSound unless `complete`),
  // or of AD_SND 1.0's (`v10`: After Dark 2.0's volume pair instead).
  explicit Ad3Rig(bool complete = true, bool v10 = false) {
    using win16::Call16;
    using win16::Conv16;
    auto snd = [this](uint16_t ord, const char* name, int bytes) {
      rt.shims().add("AD_SND", ord, name, Conv16::pascal_, true, bytes, [this, name, bytes](Call16& c) {
        std::string s = name;
        if (bytes == 2) s += "(" + std::to_string(c.w()) + ")";
        if (s == "ADWGETSYSTEMVOLUMES") c.rt.wr16(c.ptr(), 0x1234);
        calls.push_back(s);
        c.ret(0);
      });
    };
    snd(11, "ADWSOUNDINIT", 6);
    snd(9, "ADWSOUNDCLEANUP", 0);
    if (v10) {
      snd(20, "ADWSAVEPREVIOUSVOLUME", 0);
      snd(10, "ADWRESTOREPREVIOUSVOLUME", 0);
    } else {
      snd(101, "ADWGETSYSTEMVOLUMES", 4);
      snd(102, "ADWSETSYSTEMVOLUMES", 2);
    }
    snd(29, "ADWSETVOLUME", 2);
    snd(8, "ADWSETSOUNDMUTE", 2);
    if (complete) snd(26, "ADWSTOPSOUND", 0);
  }
  // AD_MODULE, through AD_SYSTEM's hADModule.
  uint32_t ad_module() { return rt.global().lock(rt.rd16(rt.global().lock(hsys) + 0x20)); }
  std::string controls() {
    uint32_t m = ad_module();
    std::string s;
    for (uint32_t i = 0; i < 4; i++) s += (i ? " " : "") + std::to_string(int16_t(rt.rd16(m + 6 + 2 * i)));
    return s;
  }
};

void test_ad3_protocol() {
  using Kind = ne16::Protocol16::Call::Kind;
  auto exists = [](const std::string& f) { return GetFileAttributesA(f.c_str()) != INVALID_FILE_ATTRIBUTES; };
  // Two packages: "fake" ships no OLDMOD16 (the native bridge), "old" an OLDMOD16.DLL that is no NE image.
  // Each has an ENGINE\AD_SND.DLL (no NE image either): the rig's AD_SND, a
  // system module, answers for it by name — without one the host's own
  // AD_SND would (package.hh host_ad_snd; below).
  std::string root = temp_dir("ad3");
  std::string slider(0x34, '\0'), check(0x20, '\0'), button(0x20, '\0'), popup(0x20, '\0');
  put16(slider, 0, 2);  // numeric 1..9, default 22 → 9
  put16(slider, 0x18, 22);
  put16(slider, 0x30, 1);
  put16(slider, 0x32, 9);
  put16(check, 0, 5);  // checkbox, default 9 → 1
  put16(check, 0x18, 9);
  put16(button, 0, 4);  // a button
  put16(popup, 0, 3);   // popup of 3, default 1
  put16(popup, 0x16, 3);
  put16(popup, 0x18, 1);
  std::vector<std::string> files, dirs = {root + "\\packages"};
  CreateDirectoryA(dirs[0].c_str(), nullptr);
  auto package = [&](const char* id, bool oldmod16) {
    std::string pkg = root + "\\packages\\" + id;
    for (const std::string& d : {pkg, pkg + "\\MODS", pkg + "\\ENGINE"}) {
      CreateDirectoryA(d.c_str(), nullptr);
      dirs.push_back(d);
    }
    files.push_back(pkg + "\\MODS\\FAKEMOD.AD");
    write_file(files.back(), ne_image("FAKEMOD", {{1000, 1, slider}, {1000, 2, check}, {1000, 3, button}, {1000, 4, popup}}));
    files.push_back(pkg + "\\ENGINE\\ADTASK.DLL");
    write_file(files.back(), ne_image("ADTASK", {{5000, 1, logpal_res(1, 235)}, {5000, 2, logpal_res(2, 235)},
                                                 {5000, 3, logpal_res(3, 235)}, {5000, 4, logpal_res(4, 235)}}));
    files.push_back(pkg + "\\ENGINE\\AD_SND.DLL");
    write_file(files.back(), "stands for AD_SND: the rig's answers");
    if (oldmod16) {
      files.push_back(pkg + "\\ENGINE\\OLDMOD16.DLL");
      write_file(files.back(), "not an NE image");
    }
    return ne16::resolve_layout(pkg + "\\MODS\\FAKEMOD.AD", root, exists);
  };
  const ne16::Ne16Layout layout = package("fake", false), old = package("old", true);
  Env env = Env::parse({{"AD_ASSETS_DIR", root}, {"ADVOLUME", "30"}, {"ADSOUND", "1"}});
  InputState input;
  input.controls[3] = 2;  // ADCVSET 3=2, over the popup's default
  Screen screen(64, 48);
  CHECK(ne16::make_ad3_protocol(layout)->error_text() == "(no error text)", "no error text before a load");

  // A run.
  {
    Ad3Rig g;
    win16::Runtime16& rt = g.rt;
    LaneContext ctx{env, screen, g.clock, input};
    auto p = ne16::make_ad3_protocol(layout);
    win16::Runtime16Options opts;
    p->configure_runtime(opts, ctx);
    CHECK(std::string(p->name()) == "ad3/native" && opts.desktop_palette && opts.guest_dir == "C:\\AFTERDRK",
          "no OLDMOD16: the native bridge and the desktop palette (%s)", p->name());
    p->mount(rt, env);
    uint16_t hwnd = win16::user16_saver_window(rt);
    uint16_t hdc = win16::gdi16_screen_dc(rt, hwnd);
    bool loaded = p->load(rt, hwnd, hdc, ctx);
    CHECK(loaded && g.joined() ==
                        "ADWSOUNDINIT ADWGETSYSTEMVOLUMES MODULE(5) ADWSETSOUNDMUTE(0) ADWSETVOLUME(30) MODULE(12) MODULE(0) "
                        "MODULE(1)",
          "LOADADMODULE16 through the protocol, unmuted (ADSOUND) at ADVOLUME: %s", g.joined().c_str());
    if (!loaded) return;
    CHECK(g.controls() == "9 1 0 2", "controls: the records' defaults, ADCVSET over the fourth (%s)", g.controls().c_str());
    uint32_t mod = g.ad_module();
    CHECK(rt.rd16(mod + 0x18) == 0, "no palette request yet");
    auto call = [&](uint16_t result) {
      g.results[2] = {result};
      return p->call();
    };
    ne16::Protocol16::Call c = call(0);
    CHECK(c.kind == Kind::ok && c.code == 0, "DRAWFRAME 0: go on");
    c = call(0x0E);
    CHECK(c.kind == Kind::toggle_events && c.code == 0x0E, "0x0E: toggle events");
    c = call(0x11);
    CHECK(c.kind == Kind::cursor_on && c.code == 0x11, "0x11: cursor on");
    c = call(0x12);
    CHECK(c.kind == Kind::cursor_off && c.code == 0x12, "0x12: cursor off");
    c = call(0xFFFF);
    CHECK(c.kind == Kind::ok && c.code == -1, "below 0 counts as 0 (%d)", c.code);
    c = call(0x13);
    CHECK(c.kind == Kind::ok && c.code == 0x13, "above 0x12 counts as 0");
    g.module_text = rt.static_bytes("test protocol text", "It broke.");
    c = call(5);
    g.module_text = 0;
    CHECK(c.kind == Kind::stop && c.code == 5 && p->error_text() == "It broke.", "5: the module stopped, saying '%s'",
          p->error_text().c_str());
    CHECK(g.calls.back() == "MODULE(2)", "each call is one DRAWFRAME");
    CHECK(!p->set_control(-1, 5) && !p->set_control(4, 5), "SET outside 0..3 is not the module's");
    CHECK(p->set_control(1, -7), "SET 1 is");
    g.calls.clear();
    p->send_controls();
    CHECK(g.calls.empty() && g.controls() == "9 -7 0 2", "SETMODULECTRLVALUES16: the values, no sound calls (%s; %s)",
          g.controls().c_str(), g.joined().c_str());
    g.calls.clear();
    p->unload();
    CHECK(g.joined() == "MODULE(3) ADWSTOPSOUND ADWSETSYSTEMVOLUMES(4660) ADWSOUNDCLEANUP", "UNLOADADMODULE16: %s",
          g.joined().c_str());
    p->close();
  }
  // A button (configure mode): the slot check, then BUTTONPUSHED16.
  {
    auto p = ne16::make_ad3_protocol(layout);
    std::string why;
    CHECK(!p->check_button(-1, &why) && why == "control -1 is not a button", "slot -1: %s", why.c_str());
    CHECK(!p->check_button(0, &why) && why == "control 0 is not a button", "a slider: %s", why.c_str());
    CHECK(!p->check_button(4, &why) && why == "control 4 is not a button", "no record: %s", why.c_str());
    CHECK(p->check_button(2, &why), "control 2 is a button (kind 4)");
    Ad3Rig g;
    LaneContext ctx{env, screen, g.clock, input};
    win16::Runtime16Options opts;
    p->configure_button_runtime(opts, env);
    CHECK(std::string(p->name()) == "ad3/native" && opts.desktop_palette, "configure mode: the same bridge and palette");
    p->mount(g.rt, env);
    ne16::Protocol16::Button b = p->button(g.rt, 2, 0xC004, ctx);
    CHECK(b.ran && b.failure.empty() && b.message.empty() &&
              g.joined() ==
                  "ADWSOUNDINIT ADWGETSYSTEMVOLUMES MODULE(5) MODULE(9) ADWSTOPSOUND ADWSETSYSTEMVOLUMES(4660) ADWSOUNDCLEANUP",
          "BUTTONPUSHED16 slot 2 through the protocol: %s", g.joined().c_str());
    CHECK(g.rt.rd16(g.rt.global().lock(g.hsys) + 0x26) == 0xC004 && g.controls() == "9 1 0 2",
          "the owner in AD_SYSTEM, the controls in AD_MODULE (%s)", g.controls().c_str());
    p->close();
  }
  {
    // The module's error text is the button's message.
    auto p = ne16::make_ad3_protocol(layout);
    std::string why;
    p->check_button(2, &why);
    Ad3Rig g;
    LaneContext ctx{env, screen, g.clock, input};
    win16::Runtime16Options opts;
    p->configure_button_runtime(opts, env);
    p->mount(g.rt, env);
    g.module_text = g.rt.static_bytes("test protocol button text", "No pictures.");
    g.results[9] = {9};
    ne16::Protocol16::Button b = p->button(g.rt, 2, 0, ctx);
    CHECK(b.ran && b.failure.empty() && b.message == "No pictures.", "the module's error text: '%s'", b.message.c_str());
    p->close();
  }
  {
    // An AD_SND that lacks an entry point: it ran, and failed for AD_SND's reason.
    auto p = ne16::make_ad3_protocol(layout);
    std::string why;
    p->check_button(2, &why);
    Ad3Rig g(/*complete=*/false);
    LaneContext ctx{env, screen, g.clock, input};
    win16::Runtime16Options opts;
    p->configure_button_runtime(opts, env);
    p->mount(g.rt, env);
    ne16::Protocol16::Button b = p->button(g.rt, 2, 0, ctx);
    CHECK(b.ran && b.failure == "AD_SND.DLL lacks an entry point", "AD_SND without adwStopSound: '%s'", b.failure.c_str());
    p->close();
  }
  {
    // OLDMOD16 in the engine dir: the real bridge and the boot palette; one that
    // cannot load runs nothing (a button) and fails the load (a run).
    auto p = ne16::make_ad3_protocol(old);
    std::string why;
    CHECK(p->check_button(2, &why), "the old package's button");
    Ad3Rig g;
    LaneContext ctx{env, screen, g.clock, input};
    win16::Runtime16Options opts;
    p->configure_button_runtime(opts, env);
    CHECK(std::string(p->name()) == "ad3/oldmod16" && !opts.desktop_palette, "OLDMOD16: the real bridge, the boot palette");
    p->mount(g.rt, env);
    ne16::Protocol16::Button b = p->button(g.rt, 2, 0, ctx);
    CHECK(!b.ran && b.message.find("OLDMOD16.DLL") != std::string::npos && g.calls.empty(),
          "an OLDMOD16 that cannot load: nothing ran (%s)", b.message.c_str());
    auto q = ne16::make_ad3_protocol(old);
    win16::Runtime16Options o2;
    q->configure_runtime(o2, ctx);
    CHECK(!q->load(g.rt, 0, 0, ctx) && g.calls.empty(), "the run fails before LOADADMODULE16");
    // ADNE16BRIDGE=native forces the bridge; the palette still follows the engine dir.
    Env forced = Env::parse({{"AD_ASSETS_DIR", root}, {"ADNE16BRIDGE", "native"}});
    LaneContext fctx{forced, screen, g.clock, input};
    auto f = ne16::make_ad3_protocol(old);
    win16::Runtime16Options o3;
    f->configure_runtime(o3, fctx);
    CHECK(std::string(f->name()) == "ad3/native" && !o3.desktop_palette, "ADNE16BRIDGE=native: %s, %s palette", f->name(),
          o3.desktop_palette ? "desktop" : "boot");
  }
  // After Dark 2.0 (package.hh after_dark2: AD_MOD.DLL beside the module),
  // with no palette source in its engine dir and AD_SND 1.0: AD_PREFS.INI's
  // seeds, the load through AD_SND 1.0's volume pair, After Dark 2.0's
  // palettes computed (INITIALIZE asks for 12, the grey ramp), result 5 its
  // wake, every other result as before; a package without AD_MOD.DLL gets no
  // seeds.
  {
    const std::string pkg = root + "\\packages\\startrek";
    for (const std::string& d : {pkg, pkg + "\\AFTERDRK", pkg + "\\ENGINE"}) {
      CreateDirectoryA(d.c_str(), nullptr);
      dirs.push_back(d);
    }
    files.push_back(pkg + "\\AFTERDRK\\FAKEMOD.AD");
    write_file(files.back(), ne_image("FAKEMOD", {{1000, 1, slider}, {1000, 2, check}, {1000, 3, button}, {1000, 4, popup}}));
    files.push_back(pkg + "\\AFTERDRK\\AD_MOD.DLL");
    write_file(files.back(), "no NE image: nothing loads it here");
    files.push_back(pkg + "\\ENGINE\\AD_SND.DLL");
    write_file(files.back(), "stands for AD_SND 1.0: the rig's answers");
    const ne16::Ne16Layout st = ne16::resolve_layout(pkg + "\\AFTERDRK\\FAKEMOD.AD", root, exists);
    Ad3Rig g(/*complete=*/true, /*v10=*/true);
    win16::Runtime16& rt = g.rt;
    LaneContext ctx{env, screen, g.clock, input};
    auto p = ne16::make_ad3_protocol(st);
    win16::Runtime16Options opts;
    p->configure_runtime(opts, ctx);
    CHECK(std::string(p->name()) == "ad3/native" && opts.desktop_palette, "After Dark 2.0: the native bridge (%s)", p->name());
    p->mount(rt, env);
    win32::IniStore& ini = win16::profiles16(rt);
    const std::string prefs = "C:\\WINDOWS\\AD_PREFS.INI";
    CHECK(ini.get(prefs, "After Dark", "Path").value_or("") == "C:\\AFTERDRK\\" &&
              ini.get(prefs, "Sound", "SoundDriver").value_or("") == "AD_MME.DRV",
          "AD_PREFS.INI seeds: Path=%s, SoundDriver=%s", ini.get(prefs, "After Dark", "Path").value_or("(none)").c_str(),
          ini.get(prefs, "Sound", "SoundDriver").value_or("(none)").c_str());
    uint16_t hwnd = win16::user16_saver_window(rt);
    uint16_t hdc = win16::gdi16_screen_dc(rt, hwnd);
    g.results[0] = {12};
    const uint64_t gpe0 = rt.shims().find_name("GDI", "GetPaletteEntries")->calls;
    bool loaded = p->load(rt, hwnd, hdc, ctx);
    CHECK(loaded && g.joined() ==
                        "ADWSOUNDINIT ADWSAVEPREVIOUSVOLUME MODULE(5) ADWSETSOUNDMUTE(0) ADWSETVOLUME(30) MODULE(12) "
                        "MODULE(0) MODULE(1)",
          "After Dark 2.0: LOADADMODULE16 over AD_SND 1.0: %s", g.joined().c_str());
    if (loaded) {
      // Request 12 selected hpal[0]: the computed grey ramp, 235 entries (i, i, i).
      uint32_t mod = g.ad_module(), lp = rt.rd32(mod + 0x1A);
      CHECK(rt.rd16(mod + 0x18) != 0 && rt.rd16(lp + 2) == 256 && rt.rd8(lp + 4 + 4 * 100) == 100 &&
                rt.rd8(lp + 4 + 4 * 100 + 2) == 100 && rt.rd8(lp + 4 + 4 * 100 + 3) == PC_RESERVED &&
                rt.rd8(lp + 4 + 4 * 234) == 234 && rt.rd8(lp + 4 + 4 * 235 + 3) == 0 &&
                rt.shims().find_name("GDI", "GetPaletteEntries")->calls - gpe0 == 4,
            "After Dark 2.0's palettes, computed, handed over at the request: 12 is the grey ramp (entry 100: %u)",
            rt.rd8(lp + 4 + 4 * 100));
    }
    if (loaded) {
      auto call = [&](uint16_t result) {
        g.results[2] = {result};
        return p->call();
      };
      ne16::Protocol16::Call c = call(5);
      CHECK(c.kind == Kind::wake && c.code == 5, "After Dark 2.0's 5: its wake");
      c = call(0x0E);
      CHECK(c.kind == Kind::toggle_events, "0x0E: toggle events, as ever");
      c = call(4);
      CHECK(c.kind == Kind::stop && c.code == 4, "4: the module stopped, as ever");
      c = call(0);
      CHECK(c.kind == Kind::ok, "0: go on");
      g.calls.clear();
      p->unload();
      CHECK(g.joined() == "MODULE(3) ADWSTOPSOUND ADWRESTOREPREVIOUSVOLUME ADWSOUNDCLEANUP", "UNLOADADMODULE16: %s",
            g.joined().c_str());
    }
    p->close();
    Ad3Rig other;
    auto q = ne16::make_ad3_protocol(layout);
    q->mount(other.rt, env);
    CHECK(!win16::profiles16(other.rt).get(prefs, "After Dark", "Path") &&
              !win16::profiles16(other.rt).get(prefs, "Sound", "SoundDriver"),
          "no AD_MOD.DLL: no After Dark 2.0 seeds");
  }
  // After Dark 3.x (package.hh after_dark3_host: ADW30.EXE in the engine
  // dir): AD_PREFS.INI's seeds are the keys ADW30 wrote at every start, Path
  // without a backslash (ADXPL100 finds DIS_SND.DLL and MUSIC\ there); with
  // AD_MOD.DLL beside the module as well, After Dark 2.0's rule wins; a
  // package without ADW30.EXE (the "fake" one) gets neither.
  {
    const std::string pkg = root + "\\packages\\disney";
    for (const std::string& d : {pkg, pkg + "\\DISNEY", pkg + "\\ENGINE"}) {
      CreateDirectoryA(d.c_str(), nullptr);
      dirs.push_back(d);
    }
    files.push_back(pkg + "\\DISNEY\\FAKEMOD.AD");
    write_file(files.back(), ne_image("FAKEMOD", {{1000, 1, slider}}));
    files.push_back(pkg + "\\ENGINE\\ADW30.EXE");
    write_file(files.back(), "no NE image: nothing runs it here");
    const std::string prefs = "C:\\WINDOWS\\AD_PREFS.INI";
    auto seeds = [&](const ne16::Ne16Layout& l) {
      Ad3Rig g;
      auto p = ne16::make_ad3_protocol(l);
      p->mount(g.rt, env);
      win32::IniStore& ini = win16::profiles16(g.rt);
      return ini.get(prefs, "After Dark", "Path").value_or("(none)") + " " +
             ini.get(prefs, "Sound", "SoundDriver").value_or("(none)");
    };
    const ne16::Ne16Layout dl = ne16::resolve_layout(pkg + "\\DISNEY\\FAKEMOD.AD", root, exists);
    CHECK(ne16::after_dark3_host(dl, exists) && seeds(dl) == "C:\\AFTERDRK AD_MME.DRV",
          "ADW30.EXE in the engine dir: AD_PREFS.INI seeds %s", seeds(dl).c_str());
    CHECK(seeds(layout) == "(none) (none)", "no ADW30.EXE: no seeds (%s)", seeds(layout).c_str());
    files.push_back(pkg + "\\DISNEY\\AD_MOD.DLL");
    write_file(files.back(), "no NE image: nothing loads it here");
    CHECK(seeds(dl) == "C:\\AFTERDRK\\ AD_MME.DRV", "AD_MOD.DLL beside the module too: After Dark 2.0's seeds (%s)",
          seeds(dl).c_str());
  }
  // The host's AD_SND (package.hh host_ad_snd): a package with no engine dir
  // at all, as the importer installs Snoopy's Screen Savers, and the made-up
  // module importing AD_SND by name. The protocol registers the host's
  // AD_SND, the native bridge loads it by its path and the module by its
  // imports: LOADADMODULE16's sequence, the module's own sound at INITIALIZE
  // (looping: 0x0F), DRAWFRAME, then CLOSE (the module frees its sound and
  // closes, stopping it twice) and the bridge's adwStopSound,
  // adwSetSystemVolumes and adwSoundCleanup; its button the same way.
  {
    const std::string pkg = root + "\\packages\\snoopy";
    for (const std::string& d : {pkg, pkg + "\\AFTERDRK"}) {
      CreateDirectoryA(d.c_str(), nullptr);
      dirs.push_back(d);
    }
    files.push_back(pkg + "\\AFTERDRK\\SNDMOD.AD");
    write_file(files.back(), sound_module_image());
    const ne16::Ne16Layout sl = ne16::resolve_layout(pkg + "\\AFTERDRK\\SNDMOD.AD", root, exists);
    CHECK(ne16::host_ad_snd(sl, exists), "no engine dir: the host's AD_SND");
    auto run = [&](bool button) {
      BridgeRig g;
      win16::Runtime16& rt = g.rt;
      std::vector<std::string> played;
      win16::Shim16Entry* snd = rt.shims().find_name("MMSYSTEM", "sndPlaySound");
      win16::Shim16Fn orig = snd->fn;
      snd->fn = [&played, orig](win16::Call16& c) {
        uint32_t p = c.ptr();
        char b[32];
        snprintf(b, sizeof(b), "%s %04X", p ? c.rt.read_str(p, 4).c_str() : "NULL", c.w());
        played.push_back(b);
        c.rewind();
        orig(c);
      };
      auto calls = [&](const char* n) {
        win16::Shim16Entry* e = rt.shims().find_name("AD_SND", n);
        return e ? e->calls : 0;
      };
      auto p = ne16::make_ad3_protocol(sl);
      LaneContext ctx{env, screen, g.clock, input};
      std::string why;
      if (button) {
        CHECK(p->check_button(0, &why), "SNDMOD's button record (%s)", why.c_str());
        win16::Runtime16Options opts;
        p->configure_button_runtime(opts, env);
        p->mount(rt, env);
        ne16::Protocol16::Button b = p->button(rt, 0, 0xC004, ctx);
        CHECK(b.ran && b.failure.empty() && calls("ADWSOUNDINIT") == 1 && calls("ADWGETSYSTEMVOLUMES") == 1 &&
                  calls("ADWSTOPSOUND") == 1 && calls("ADWSETSYSTEMVOLUMES") == 1 && calls("ADWSOUNDCLEANUP") == 1,
              "the host's AD_SND: BUTTONPUSHED16 ran (%s)", b.failure.c_str());
        p->close();
        return;
      }
      win16::Runtime16Options opts;
      p->configure_runtime(opts, ctx);
      CHECK(std::string(p->name()) == "ad3/native" && opts.desktop_palette, "no engine dir: the native bridge (%s)",
            p->name());
      p->mount(rt, env);
      uint16_t hwnd = win16::user16_saver_window(rt);
      uint16_t hdc = win16::gdi16_screen_dc(rt, hwnd);
      bool loaded = p->load(rt, hwnd, hdc, ctx);
      win16::Module16* m = rt.modules().by_name("SNDMOD");
      std::string heard;
      for (const std::string& s : played) heard += (heard.empty() ? "" : ", ") + s;
      CHECK(loaded && m && !m->system && rt.modules().by_name("AD_SND")->system && calls("ADWSOUNDINIT") == 1 &&
                calls("ADWGETSYSTEMVOLUMES") == 1 && calls("adwOpenSound") == 1 && calls("adwLoadSoundResource") == 1 &&
                calls("adwSetSoundMode") == 1 && calls("adwPlaySound") == 1 && heard == "RIFF 000F",
            "the host's AD_SND: LOADADMODULE16, the module's own sound at INITIALIZE (%s)", heard.c_str());
      if (!loaded) return;
      CHECK(p->call().kind == Kind::ok, "DRAWFRAME: go on");
      // SNDMOD asks for no palette: After Dark 2.0's computed ones never reach the bridge.
      CHECK(rt.shims().find_name("GDI", "GetPaletteEntries")->calls == 0,
            "no palette request: the computed palettes are never handed over");
      played.clear();
      p->unload();
      heard.clear();
      for (const std::string& s : played) heard += (heard.empty() ? "" : ", ") + s;
      CHECK(calls("adwFreeSound") == 1 && calls("adwCloseSound") == 1 && calls("ADWSTOPSOUND") == 1 &&
                calls("ADWSETSYSTEMVOLUMES") == 1 && calls("ADWSOUNDCLEANUP") == 1 &&
                heard == "NULL 0000, NULL 0000, NULL 0000" && rt.shims().unimplemented_called().empty(),
            "the host's AD_SND: CLOSE, then AD_SND restored and released (%s)", heard.c_str());
      p->close();
    };
    run(false);
    run(true);
  }
  for (const std::string& f : files) DeleteFileA(f.c_str());
  for (auto d = dirs.rbegin(); d != dirs.rend(); ++d) RemoveDirectoryA(d->c_str());
  RemoveDirectoryA(root.c_str());
}

// ---- the module's kind, the windows dir, the IMX reader (package.hh) ------------------------------------------

void test_kinds() {
  using ne16::ModuleKind;
  auto kind_of = [](const NeNames& n, const std::string& file = "X.IMX") {
    loader::ne::Image img(ne_image("TESTMOD", {}, n));
    return ne16::detect_kind(img, file);
  };
  ne16::KindProbe k = kind_of({{{"MODULE", 1}}, {}});
  CHECK(k.ok && k.kind == ModuleKind::ad3, "MODULE (resident): an After Dark module");
  k = kind_of({{}, {{"SaverInit", 2}, {"saverDRAW", 3}, {"SAVERDLGPROC", 4}}});
  CHECK(k.ok && k.kind == ModuleKind::imx, "SAVERINIT + SAVERDRAW (non-resident, any case): an Intermission module");
  k = kind_of({{{"SAVERINIT", 2}}, {{"SAVERDRAW", 3}}});
  CHECK(k.ok && k.kind == ModuleKind::imx, "one in each table: still both");
  k = kind_of({{{"MODULE", 1}}, {{"SAVERINIT", 2}, {"SAVERDRAW", 3}}});
  CHECK(k.ok && k.kind == ModuleKind::ad3, "MODULE and SAVERINIT/SAVERDRAW: MODULE wins");
  k = kind_of({{}, {{"SAVERINIT", 2}}});
  CHECK(!k.ok && k.why.find("SAVERINIT without SAVERDRAW") != std::string::npos, "SAVERINIT alone: refused (%s)",
        k.why.c_str());
  CHECK(k.form == ne16::ImxForm::imx, "SAVERINIT + SAVERDRAW: the IMX form (read by IMIMXPLY.IMQ)");
  k = kind_of({{{"SAVERMAIN", 2}}, {}}, "PTERY.IMQ");
  CHECK(k.ok && k.kind == ModuleKind::imx && k.form == ne16::ImxForm::imq,
        "SAVERMAIN alone: an Intermission .IMQ, its own reader (its QUERY decides whether it is a saver)");
  k = kind_of({{}, {{"saverMain", 2}, {"SAVERDLGPROC", 3}, {"SAVERDLGPROC2", 4}}}, "DB-BEST.IMQ");
  CHECK(k.ok && k.form == ne16::ImxForm::imq, "SAVERMAIN in the non-resident table, any case: the same");
  CHECK(std::string(ne16::form_name(ne16::ImxForm::imx)) == "imx" &&
            std::string(ne16::form_name(ne16::ImxForm::asa)) == "asa" &&
            std::string(ne16::form_name(ne16::ImxForm::imq)) == "imq",
        "form names");
  CHECK(std::string(ne16::reader_file(ne16::ImxForm::imx)) == "IMIMXPLY.IMQ" &&
            std::string(ne16::reader_file(ne16::ImxForm::asa)) == "IMASAPLY.IMQ" &&
            !ne16::reader_file(ne16::ImxForm::imq),
        "each form's reader file: IMIMXPLY.IMQ, IMASAPLY.IMQ, none (an IMQ module is its own)");
  // Intermission's other data files: a form by type, read by IM<type>PLY.IMQ.
  {
    using ne16::ImxForm;
    const std::pair<const char*, ImxForm> kTypes[] = {
        {"FLI", ImxForm::fli}, {"flc", ImxForm::flc}, {"Mrf", ImxForm::mrf}, {"MSV", ImxForm::msv}};
    for (const auto& [type, want] : kTypes) {
      ImxForm f = ImxForm::imx;
      CHECK(ne16::form_for_type(type, &f) && f == want && ne16::data_form(f), "type %s: its form, a data form", type);
    }
    ImxForm f = ImxForm::imq;
    CHECK(!ne16::form_for_type("ASA", &f) && !ne16::form_for_type("IMX", &f) && !ne16::form_for_type(nullptr, &f) &&
              f == ImxForm::imq,
          "no other type has a form by extension (an ASA animation goes by its header)");
    CHECK(ne16::data_form(ImxForm::asa) && !ne16::data_form(ImxForm::imx) && !ne16::data_form(ImxForm::imq),
          "an ASA animation is data; IMX and IMQ modules are code");
    CHECK(std::string(ne16::form_name(ImxForm::fli)) == "fli" && std::string(ne16::form_name(ImxForm::flc)) == "flc" &&
              std::string(ne16::form_name(ImxForm::mrf)) == "mrf" &&
              std::string(ne16::form_name(ImxForm::msv)) == "msv",
          "the data forms' names");
    CHECK(std::string(ne16::reader_file(ImxForm::fli)) == "IMFLIPLY.IMQ" &&
              std::string(ne16::reader_file(ImxForm::flc)) == "IMFLCPLY.IMQ" &&
              std::string(ne16::reader_file(ImxForm::mrf)) == "IMMRFPLY.IMQ" &&
              std::string(ne16::reader_file(ImxForm::msv)) == "IMMSVPLY.IMQ",
          "the data forms' readers: IMFLIPLY, IMFLCPLY, IMMRFPLY, IMMSVPLY");
  }
  {
    const char asa_n[4] = {'A', 'n', 'i', 'N'}, asa_m[4] = {'A', 'n', 'i', 'M'}, other[4] = {'A', 'n', 'i', 'X'};
    CHECK(asa_header(asa_n) && asa_header(asa_m) && !asa_header(other) && !asa_header("MZ\x90\x00"),
          "an ASA animation's header: AniN or AniM");
  }
  k = kind_of({{}, {{"SAVERINIT", 2}, {"SAVERDRAW", 3}, {"setcurrsaver", 4}}});
  CHECK(!k.ok && k.why.find("SETCURRSAVER") != std::string::npos, "SETCURRSAVER: refused as the reader does (%s)",
        k.why.c_str());
  k = kind_of({{}, {{"SAVERINIT", 2}, {"SAVERDRAW", 3}}}, "imxx_one.imx");
  CHECK(!k.ok && k.why.find("IMXX_") != std::string::npos, "a file named IMXX_*: refused as the reader does (%s)",
        k.why.c_str());
  k = kind_of({{{"WEP", 1}}, {{"LIBMAIN", 2}}});
  CHECK(!k.ok && k.why.find("not an After Dark or Intermission module") != std::string::npos, "no such export: %s",
        k.why.c_str());
  loader::ne::Image named(ne_image("MODULE", {}, {{}, {{"SAVERINIT", 2}}}));
  k = ne16::detect_kind(named, "MODULE.DLL");
  CHECK(!k.ok, "entry 0 of a name table (the module name, the description) is no export");
  // A Windows 3.1 screen saver: an application (no LIBRARY bit) exporting SCREENSAVERPROC.
  auto app_of = [](const NeNames& n) {
    loader::ne::Image img(ne_image("SCRTEST", {}, n, 0x0002));
    return ne16::detect_kind(img, "SCRTEST.SCR");
  };
  k = app_of({{}, {{"ScreenSaverProc", 2}, {"SCREENSAVERCONFIGUREDIALOG", 3}}});
  CHECK(k.ok && k.kind == ModuleKind::scr, "an application exporting SCREENSAVERPROC (any case): a Windows 3.1 saver");
  k = app_of({{{"MODULE", 1}}, {{"SAVERINIT", 2}, {"SAVERDRAW", 3}}});
  CHECK(!k.ok && k.why.find("not a Windows 3.1 screen saver") != std::string::npos,
        "any other application is refused, whatever it exports (%s)", k.why.c_str());
  k = kind_of({{}, {{"SCREENSAVERPROC", 2}}});
  CHECK(!k.ok, "a library exporting SCREENSAVERPROC is no saver program");
  {
    ne16::Ne16Layout l;
    l.module_path = "C:\\A\\win\\packages\\castaway\\SCRANTIC\\scrantic.scr";
    l.module_dir = "C:\\A\\win\\packages\\castaway\\SCRANTIC";
    CHECK(ne16::scr_install_dir(l) == "C:\\SIERRA\\SCRANTIC", "SCRANTIC.SCR: its installer's directory (%s)",
          ne16::scr_install_dir(l).c_str());
    l.module_path = "C:\\A\\win\\packages\\other\\Savers\\OTHER.SCR";
    l.module_dir = "C:\\A\\win\\packages\\other\\Savers";
    CHECK(ne16::scr_install_dir(l) == "C:\\SAVERS", "another saver: C:\\ and its folder's name (%s)",
          ne16::scr_install_dir(l).c_str());
  }
  {
    ne16::Ne16Lane lane;
    std::vector<std::string> abis = lane.abis();
    CHECK(std::find(abis.begin(), abis.end(), "scrnsave") != abis.end() && abis.size() == 3,
          "the lane's ABIs: afterdark, intermission, scrnsave");
  }
  // ADNE16KIND, ADNE16READER.
  bool is_auto = false;
  ModuleKind mk = ModuleKind::ad3;
  CHECK(ne16::parse_kind_choice("", &is_auto, &mk) && is_auto, "ADNE16KIND empty = auto");
  CHECK(ne16::parse_kind_choice("Auto", &is_auto, &mk) && is_auto, "ADNE16KIND auto");
  CHECK(ne16::parse_kind_choice("IMX", &is_auto, &mk) && !is_auto && mk == ModuleKind::imx, "ADNE16KIND imx");
  CHECK(ne16::parse_kind_choice("ad3", &is_auto, &mk) && !is_auto && mk == ModuleKind::ad3, "ADNE16KIND ad3");
  CHECK(ne16::parse_kind_choice("SCR", &is_auto, &mk) && !is_auto && mk == ModuleKind::scr, "ADNE16KIND scr");
  CHECK(!ne16::parse_kind_choice("ad4", &is_auto, &mk), "ADNE16KIND: anything else is refused");
  CHECK(std::string(ne16::kind_name(ModuleKind::imx)) == "imx" && std::string(ne16::kind_name(ModuleKind::ad3)) == "ad3" &&
            std::string(ne16::kind_name(ModuleKind::scr)) == "scr",
        "kind names");
  ne16::ReaderKind rk = ne16::ReaderKind::imq;
  CHECK(ne16::parse_reader_choice("", &is_auto, &rk) && is_auto, "ADNE16READER empty = auto");
  CHECK(ne16::parse_reader_choice("NATIVE", &is_auto, &rk) && !is_auto && rk == ne16::ReaderKind::native, "native");
  CHECK(ne16::parse_reader_choice("imq", &is_auto, &rk) && !is_auto && rk == ne16::ReaderKind::imq, "imq");
  CHECK(!ne16::parse_reader_choice("oldmod16", &is_auto, &rk), "ADNE16READER: anything else is refused");

  // The windows dir and the reader's place.
  std::map<std::string, bool> files, dirs;
  auto up = [](std::string p) {
    for (char& ch : p) ch = char(toupper(uint8_t(ch)));
    return p;
  };
  auto exists = [&](const std::string& p) { return files.count(up(p)) != 0; };
  auto dir_exists = [&](const std::string& p) { return dirs.count(up(p)) != 0; };
  const std::string win = "C:\\A\\win", pkg = win + "\\packages\\swse";
  ne16::Ne16Layout l = ne16::resolve_layout(pkg + "\\SAVER\\VADER.IMX", win, exists, dir_exists);
  CHECK(l.packaged && l.engine_dir == pkg + "\\ENGINE" && l.windows_dir.empty(), "no WINDOWS folder: no windows dir");
  dirs[up(pkg + "\\WINDOWS")] = true;
  l = ne16::resolve_layout(pkg + "\\SAVER\\VADER.IMX", win, exists, dir_exists);
  CHECK(l.windows_dir == pkg + "\\WINDOWS", "the package's WINDOWS folder (%s)", l.windows_dir.c_str());
  CHECK(ne16::resolve_layout(pkg + "\\SAVER\\VADER.IMX", win, exists).windows_dir.empty(),
        "without a directory check: none");
  CHECK(ne16::resolve_layout(pkg + "\\WINDOWS\\X.IMX", win, exists, dir_exists).windows_dir.empty(),
        "the WINDOWS folder is never the module's own");
  dirs[up(win + "\\FILES\\WINDOWS")] = true;
  CHECK(ne16::resolve_layout(win + "\\FILES\\CLASSIC\\X.AD", win, exists, dir_exists).windows_dir.empty(),
        "legacy modules have no windows dir");
  CHECK(ne16::find_reader(l, exists).host.empty(), "no IMIMXPLY.IMQ anywhere: no reader file");
  files[up(pkg + "\\SAVER\\IMIMXPLY.IMQ")] = true;
  ne16::ReaderFile rf = ne16::find_reader(l, exists);
  CHECK(rf.host == pkg + "\\SAVER\\IMIMXPLY.IMQ" && !rf.in_engine_dir, "the module dir's reader (%s)", rf.host.c_str());
  files[up(pkg + "\\ENGINE\\IMIMXPLY.IMQ")] = true;
  rf = ne16::find_reader(l, exists);
  CHECK(rf.host == pkg + "\\ENGINE\\IMIMXPLY.IMQ" && rf.in_engine_dir, "the engine dir's first (%s)", rf.host.c_str());
  CHECK(ne16::find_reader(l, exists, ne16::kAsaReader).host.empty(), "no IMASAPLY.IMQ anywhere: no ASA reader");
  files[up(pkg + "\\SAVER\\IMASAPLY.IMQ")] = true;
  rf = ne16::find_reader(l, exists, ne16::kAsaReader);
  CHECK(rf.host == pkg + "\\SAVER\\IMASAPLY.IMQ" && !rf.in_engine_dir, "the module dir's ASA reader (%s)",
        rf.host.c_str());
  files[up(pkg + "\\ENGINE\\IMASAPLY.IMQ")] = true;
  rf = ne16::find_reader(l, exists, ne16::kAsaReader);
  CHECK(rf.host == pkg + "\\ENGINE\\IMASAPLY.IMQ" && rf.in_engine_dir, "the engine dir's ASA reader first (%s)",
        rf.host.c_str());

  // The lane's default factory, on files: the exports choose; ADNE16KIND forces.
  std::string root = temp_dir("kind");
  std::vector<std::string> made = {root + "\\packages", root + "\\packages\\k", root + "\\packages\\k\\MODS"};
  for (const std::string& d : made) CreateDirectoryA(d.c_str(), nullptr);
  const std::string mods = made.back();
  std::vector<std::string> written;
  auto put = [&](const char* name, const std::string& bytes) {
    written.push_back(mods + "\\" + name);
    write_file(written.back(), bytes);
    return ne16::resolve_layout(written.back(), root, [](const std::string&) { return false; });
  };
  const ne16::Ne16Layout ad3 = put("AD3.AD", ne_image("AD3", {}, {{{"MODULE", 1}}, {}}));
  const ne16::Ne16Layout imx = put("IMX.IMX", ne_image("IMX", {}, {{{"WEP", 1}}, {{"SAVERINIT", 2}, {"SAVERDRAW", 3}}}));
  const ne16::Ne16Layout reader = put("READER.IMQ", ne_image("READER", {}, {{}, {{"SAVERMAIN", 2}}}));
  const ne16::Ne16Layout junk = put("JUNK.AD", "not an NE image at all");
  const ne16::Ne16Layout anim = put("ANIM.ASA", std::string("AniN") + std::string(28, '\0'));
  const ne16::Ne16Layout anim_m = put("OLD.ASA", std::string("AniM") + std::string(28, '\0'));
  auto choose = [](const ne16::Ne16Layout& layout, const Env& env, std::string* why) {
    std::unique_ptr<ne16::Protocol16> p = ne16::Ne16Lane::choose_protocol(layout, env, why);
    return p ? std::string(p->name()).substr(0, 4) : std::string("refused");
  };
  Env none = Env::parse({}), force_imx = Env::parse({{"ADNE16KIND", "imx"}}), force_ad3 = Env::parse({{"ADNE16KIND", "ad3"}});
  Env bad = Env::parse({{"ADNE16KIND", "ad4"}});
  std::string why;
  CHECK(choose(ad3, none, &why) == "ad3/", "MODULE: the AD3 protocol");
  CHECK(choose(imx, none, &why) == "imx/", "SAVERINIT + SAVERDRAW: the IMX protocol");
  why.clear();
  CHECK(choose(reader, none, &why) == "imx/" && why.empty(),
        "an IMQ (SAVERMAIN): the IMX protocol, the IMQ its own reader (%s)", why.c_str());
  CHECK(choose(junk, none, &why) == "ad3/", "not an NE image: the AD3 protocol, which reports it as before");
  CHECK(choose(anim, none, &why) == "imx/" && choose(anim_m, none, &why) == "imx/",
        "an ASA animation (AniN, AniM): the IMX protocol, by its header");
  CHECK(choose(anim, force_ad3, &why) == "imx/", "an ASA animation whatever ADNE16KIND says");
  CHECK(choose(ad3, force_imx, &why) == "imx/" && choose(imx, force_ad3, &why) == "ad3/" &&
            choose(reader, force_imx, &why) == "imx/",
        "ADNE16KIND forces the protocol");
  CHECK(choose(imx, bad, &why) == "imx/", "a bad ADNE16KIND is auto");
  for (const std::string& f : written) DeleteFileA(f.c_str());
  for (auto d = made.rbegin(); d != made.rend(); ++d) RemoveDirectoryA(d->c_str());
  RemoveDirectoryA(root.c_str());
}

// ---- the Intermission readers (imreader.hh) --------------------------------------------------------------------
//
// The module (FAKEIMX, and HALFIMX/SETIMX that IMIMXPLY refuses) and a
// stand-in for Intermission's reader (under IMIMXPLY's own name, so the
// protocol's LoadLibrary of C:\WINDOWS\SYSTEM\IMIMXPLY.IMQ finds it) are
// host-implemented system modules: a test sees every call and scripts every
// answer. INTRMLIB, where the protocol looks for an engine palette, is a real
// NE file of resources in the package (test_imx_protocol). The runtime takes
// the options the protocol asked for (C:\SAVER).
struct ImRig {
  VirtualClock clock{VirtualClock::Mode::fixed_step, 16667};
  win16::Runtime16 rt;
  Screen screen{64, 48};
  std::vector<std::string> calls;
  // FAKEIMX: what SAVERINIT found in *w and writes there, its name, PALETTE's answer.
  uint16_t w_seen = 0xFFFF, w_answer = 0, palette_answer = 1;
  uint32_t name = 0;  // the module's own name buffer (the reader may cut it in place)
  struct Draw {
    uint16_t hwnd = 0, hdc = 0, hlib = 0, hpal = 0, code = 0;
  };
  std::vector<Draw> draws;
  // The IMIMXPLY stand-in: its answers by message (1 otherwise), what it saw.
  // IMASAPLY (the ASA reader), FAKEIMQ (an IMQ module, its own reader) and
  // the readers of the other data files (IMFLIPLY, IMFLCPLY, IMMRFPLY,
  // IMMSVPLY) are the same stand-in under their names.
  std::map<uint16_t, uint32_t> answers;
  uint8_t palette_type = 0;  // written at +0x53 by its QUERY
  uint32_t query_clears = 0; // flags its QUERY clears (0x1000: a reader's QUERY without a path)
  uint32_t query_sets = 0;   // flags its QUERY sets (0x0800: a reader, MultiSaver's or Morph's)
  bool post_task = false;    // its DRAW posts a message to the task (FORCETOWAKE)
  struct Seen {
    uint16_t msg = 0, hwnd = 0, hdc = 0, hpal = 0, reader = 0, index = 0xFFFF;
    uint32_t flags = 0, path_ptr = 0;
    std::string file, path, module;
  };
  std::vector<Seen> seen;
  std::function<void(uint16_t msg)> after_saver_main;  // runs as its SAVERMAIN returns
  uint16_t last_dc = 0;

  explicit ImRig(const win16::Runtime16Options& o = {}) : rt(o, clock) {
    clock.set_read_step_us(5);
    win16::register_all16(rt);
    rt.attach_display(screen);
    using win16::Call16;
    using win16::Conv16;
    auto& r = rt.shims();
    uint16_t hn = rt.global().alloc(win16::GlobalHeap16::kZeroInit, 0x100);
    name = uint32_t(hn) << 16;
    rt.write_str(name, "Fake Saver", 0x100);
    for (const char* m : {"FAKEIMX", "HALFIMX", "SETIMX"}) {
      r.add(m, 2, "SAVERINIT", Conv16::pascal_, false, 4, [this](Call16& c) {
        uint32_t pw = c.ptr();
        w_seen = c.rt.rd16(pw);
        c.rt.wr16(pw, w_answer);
        calls.push_back("saverinit(" + std::to_string(w_seen) + ")");
        c.ret(name);
      });
    }
    for (const char* m : {"FAKEIMX", "SETIMX"}) {
      r.add(m, 3, "SAVERDRAW", Conv16::pascal_, true, 10, [this](Call16& c) {
        Draw d;
        d.hwnd = c.w();
        d.hdc = c.w();
        d.hlib = c.w();
        d.hpal = c.w();
        d.code = c.w();
        draws.push_back(d);
        calls.push_back("saverdraw(" + std::to_string(d.code) + ")");
        c.ret(0);
      });
    }
    r.add("FAKEIMX", 4, "SAVERDLGPROC", Conv16::pascal_, true, 10, [](Call16& c) { c.ret(0); });
    r.add("FAKEIMX", 5, "SAVERDLGPROC2", Conv16::pascal_, true, 10, [](Call16& c) { c.ret(0); });
    r.add("FAKEIMX", 6, "PALETTE", Conv16::pascal_, true, 2, [this](Call16& c) {
      calls.push_back("palette(" + std::to_string(c.w()) + ")");
      c.ret(palette_answer);
    });
    r.add("SETIMX", 7, "SETCURRSAVER", Conv16::pascal_, true, 0, [](Call16& c) { c.ret(0); });
    // SAVERMAIN(LPVOID info, WORD msg): info pushed first, msg last.
    for (const char* reader : {"IMIMXPLY", "IMASAPLY", "FAKEIMQ", "IMFLIPLY", "IMFLCPLY", "IMMRFPLY", "IMMSVPLY"}) {
      std::string module = reader;
      r.add(reader, 2, "SAVERMAIN", Conv16::pascal_, false, 6, [this, module](Call16& c) {
        uint32_t info = c.ptr();
        uint16_t msg = c.w();
        Seen s;
        s.msg = msg;
        s.module = module;
        s.flags = c.rt.rd32(info);
        s.hwnd = c.rt.rd16(info + 4);
        s.hdc = c.rt.rd16(info + 6);
        s.hpal = c.rt.rd16(info + 8);
        s.reader = c.rt.rd16(info + 0x0A);
        s.index = c.rt.rd16(info + 0x59);
        s.file = c.rt.read_str(info + 0x44, 14);
        s.path_ptr = c.rt.rd32(info + 0x63);
        if (s.path_ptr) s.path = c.rt.read_str(s.path_ptr, 0x104);
        seen.push_back(s);
        calls.push_back("SAVERMAIN(" + std::to_string(msg) + ")");
        if (msg == 7) {
          c.rt.write_str(info + 0x14, "Fake Saver", 41);
          c.rt.wr8(info + 0x53, palette_type);
          c.rt.wr32(info, (c.rt.rd32(info) & ~query_clears) | query_sets);
        }
        if (msg == 0 && post_task) {
          call("USER", "PostAppMessage", {win16::w16(win16::kernel16_current_task(c.rt)), win16::w16(0x0200),
                                          win16::w16(0xFFFF), win16::l16(0)});
        }
        if (after_saver_main) after_saver_main(msg);
        auto a = answers.find(msg);
        c.ret(a == answers.end() ? 1 : a->second);
      });
    }
    // What a reader and the protocol ask of Windows.
    wrap("USER", "DialogBox", [this](Call16& c) {
      uint16_t h = c.w();
      std::string t = c.rt.read_str(c.ptr());
      uint16_t owner = c.w();
      uint32_t proc = c.ptr();
      calls.push_back("DialogBox(" + std::to_string(h) + "," + t + "," + std::to_string(owner) + "," +
                      std::to_string(proc == c.rt.modules().proc_address(c.rt.modules().by_name("FAKEIMX"), "SAVERDLGPROC")) +
                      ")");
      c.ret(1);
    });
    wrap("USER", "CreateDialog", [this](Call16& c) {
      c.w();
      std::string t = c.rt.read_str(c.ptr());
      uint16_t owner = c.w();
      calls.push_back("CreateDialog(" + t + "," + std::to_string(owner) + ")");
      c.ret(0x2345);
    });
    static const std::pair<const char*, const char*> kSeen[] = {
        {"USER", "GetDC"},          {"GDI", "SaveDC"},         {"GDI", "RestoreDC"},         {"USER", "ReleaseDC"},
        {"USER", "SelectPalette"},  {"USER", "RealizePalette"}, {"GDI", "DeleteObject"},     {"KERNEL", "LoadLibrary"},
        {"KERNEL", "FreeLibrary"},  {"KERNEL", "FindResource"}, {"KERNEL", "LoadResource"},  {"KERNEL", "LockResource"},
        {"KERNEL", "FreeResource"}, {"GDI", "CreatePalette"},   {"GDI", "GetDeviceCaps"}};
    for (const auto& [mod, f] : kSeen) {
      win16::Shim16Entry* e = r.find_name(mod, f);
      win16::Shim16Fn old = e->fn;
      std::string fname = f;
      e->fn = [this, old, fname](Call16& c) {
        std::string arg;
        if (fname == "LoadLibrary") arg = "(" + c.rt.read_str(c.ptr()) + ")";
        if (fname == "FindResource") {
          c.w();
          arg = "(" + c.rt.read_str(c.ptr()) + ")";
        }
        c.rewind();
        old(c);
        if (fname == "GetDC") last_dc = uint16_t(c.result());
        calls.push_back(fname + arg);
      };
    }
  }
  // INTRMLIB's module table (FINDALLMODULES, FREEMODINFO) as a stand-in:
  // an empty table of `count` records. Not in every rig: the palette tests
  // load a real INTRMLIB.DLL image of palettes.
  void intrmlib_modules(uint16_t count) {
    using win16::Call16;
    using win16::Conv16;
    rt.shims().add("INTRMLIB", 85, "FINDALLMODULES", Conv16::pascal_, false, 4, [this, count](Call16& c) {
      c.rt.wr16(c.ptr(), count);
      calls.push_back("FINDALLMODULES");
      c.ret32(0);
    });
    rt.shims().add("INTRMLIB", 88, "FREEMODINFO", Conv16::pascal_, true, 0, [this](Call16&) {
      calls.push_back("FREEMODINFO");
    });
  }
  void wrap(const char* mod, const char* name_, win16::Shim16Fn fn) { rt.shims().find_name(mod, name_)->fn = std::move(fn); }
  uint32_t call(const char* mod, const char* fn, std::initializer_list<win16::Arg16> args) {
    return rt.call_far(rt.thunk_far(*rt.shims().find_name(mod, fn)), args);
  }
  uint32_t str(const std::string& s) { return rt.static_bytes("imrig " + s, s); }
  std::string joined() {
    std::string s;
    for (auto& x : calls) s += (s.empty() ? "" : " ") + x;
    return s;
  }
  // Only the calls matching `keep` (prefixes), in order.
  std::string only(std::initializer_list<const char*> keep) {
    std::string s;
    for (auto& x : calls) {
      for (const char* k : keep) {
        if (x.rfind(k, 0) == 0) {
          s += (s.empty() ? "" : " ") + x;
          break;
        }
      }
    }
    return s;
  }
};

void test_native_reader() {
  ImRig g;
  win16::Runtime16& rt = g.rt;
  std::string why;
  auto rd = ne16::open_native_reader(rt, &why);
  CHECK(rd && std::string(rd->name()) == "native" && rd->instance() == 0, "the native reader opens (%s)", why.c_str());
  if (!rd) return;
  uint16_t hi = rt.global().alloc(win16::GlobalHeap16::kZeroInit, ne16::iminfo::kSize);
  const uint32_t info = uint32_t(hi) << 16;
  uint16_t hp = rt.global().alloc(win16::GlobalHeap16::kZeroInit, 0x104);
  const uint32_t path = uint32_t(hp) << 16;
  auto record = [&](const char* file) {
    for (uint32_t i = 0; i < ne16::iminfo::kSize; i++) rt.wr8(info + i, 0);
    rt.wr32(info, ne16::iminfo::kModuleFlags);
    rt.write_str(path, std::string("C:\\SAVER\\") + file, 0x104);
    rt.wr32(info + ne16::iminfo::kPath, path);
    rt.wr16(info + ne16::iminfo::kHwnd, 0x1111);
  };
  // LOAD (2:0372): the block, the module's six exports.
  record("FAKEIMX.IMX");
  g.calls.clear();
  uint32_t r = rd->saver_main(info, ne16::immsg::kLoad);
  win16::Module16* m = rt.modules().by_name("FAKEIMX");
  uint32_t b = rt.rd32(info + ne16::iminfo::kBlock);
  CHECK(r == 1 && m && b && g.only({"LoadLibrary"}) == "LoadLibrary(C:\\SAVER\\FAKEIMX.IMX)", "LOAD -> %u (%s)", r,
        g.joined().c_str());
  if (!m || !b) return;
  auto proc = [&](const char* n) { return rt.modules().proc_address(m, n); };
  CHECK(rt.rd16(b + ne16::imblock::kLib) != 0 && rt.rd32(b + ne16::imblock::kDraw) == proc("SAVERDRAW") &&
            rt.rd32(b + ne16::imblock::kInit) == proc("SAVERINIT") && rt.rd32(b + ne16::imblock::kPaletteFn) == proc("PALETTE") &&
            rt.rd32(b + ne16::imblock::kDlgProc) == proc("SAVERDLGPROC") &&
            rt.rd32(b + ne16::imblock::kDlgProc2) == proc("SAVERDLGPROC2"),
        "the block: +0 hLib, +2 saverdraw, +6 saverinit, +0xA palette, +0xE/+0x12 the dialog procs");
  const uint16_t hlib = rt.rd16(b + ne16::imblock::kLib);
  // QUERY (2:01d4): flags, palette(0) (0x100 → 0xFE), saverinit(&w), the name, w less 200/100.
  g.palette_answer = 0x100;
  g.w_answer = 250;
  rt.wr8(info, 0x08);  // bit 0x04 clear: QUERY sets it (the module has SAVERDLGPROC2)
  g.calls.clear();
  r = rd->saver_main(info, ne16::immsg::kQuery);
  CHECK(r == 1 && g.joined() == "palette(0) saverinit(254)", "QUERY: palette(0), then saverinit with its answer (%s)",
        g.joined().c_str());
  CHECK(rt.rd8(info) == 0x0C && rt.rd8(info + 1) == 0x12,
        "flags: SAVERDLGPROC2 (0x04 of byte 0), byte 1 (x & 0xF3) | 0x10 (%02X %02X)", rt.rd8(info), rt.rd8(info + 1));
  CHECK(rt.rd8(info + ne16::iminfo::kPaletteState) == 0xFE && rt.rd8(info + ne16::iminfo::kPaletteType) == 50 &&
            rt.read_str(info + ne16::iminfo::kName) == "Fake Saver",
        "+0x54 = 0xFE, +0x53 = 250 - 200 = 50, the name (%u, %u, %s)", rt.rd8(info + 0x54), rt.rd8(info + 0x53),
        rt.read_str(info + 0x14).c_str());
  const std::pair<uint16_t, uint8_t> ws[] = {{150, 50}, {305, 5}, {99, 99}, {0, 0}};
  for (auto [w, want] : ws) {
    g.w_answer = w;
    rd->saver_main(info, ne16::immsg::kQuery);
    CHECK(rt.rd8(info + ne16::iminfo::kPaletteType) == want, "w %u -> %u (%u)", w, want, rt.rd8(info + 0x53));
  }
  // A name longer than 40 characters is cut in the module's own buffer.
  rt.write_str(g.name, "An Extraordinarily Long Screen Saver Name Indeed", 0x100);
  rt.wr8(info + 1, 0x0E);
  rd->saver_main(info, ne16::immsg::kQuery);
  CHECK(rt.read_str(g.name).size() == 40 && rt.read_str(info + ne16::iminfo::kName) == rt.read_str(g.name) &&
            rt.rd8(info + 1) == 0x12,
        "cut at 40, in place: '%s'; byte 1 0x0E -> %02X", rt.read_str(info + 0x14).c_str(), rt.rd8(info + 1));
  rt.write_str(g.name, "Fake Saver", 0x100);
  // START/STOP/REPAINT, with and without the preview flag (+1 & 0x40).
  auto codes = [&](uint16_t msg) {
    g.draws.clear();
    rd->saver_main(info, msg);
    std::string s;
    for (auto& d : g.draws) s += std::to_string(d.code);
    return s;
  };
  rt.wr16(info + ne16::iminfo::kHdc, 0x2222);
  rt.wr16(info + ne16::iminfo::kPalette, 0x3333);
  CHECK(codes(ne16::immsg::kDraw) == "0" && g.draws[0].hwnd == 0x1111 && g.draws[0].hdc == 0x2222 &&
            g.draws[0].hlib == hlib && g.draws[0].hpal == 0x3333,
        "DRAW: saverdraw(+4, +6, hLib, +8, 0)");
  CHECK(codes(ne16::immsg::kStart) == "1" && codes(ne16::immsg::kStop) == "2" && codes(ne16::immsg::kRepaint) == "21",
        "START 1, STOP 2, REPAINT 2 then 1");
  rt.wr8(info + 1, uint8_t(rt.rd8(info + 1) | 0x40));
  CHECK(codes(ne16::immsg::kStart) == "31" && codes(ne16::immsg::kStop) == "24" && codes(ne16::immsg::kRepaint) == "21",
        "preview: START 3 then 1, STOP 2 then 4; REPAINT unchanged");
  rt.wr8(info + 1, uint8_t(rt.rd8(info + 1) & ~0x40));
  // PALETTE (6): palette(+0x54); 3, 4 and anything above 11 answer 1 and call nothing.
  rt.wr8(info + ne16::iminfo::kPaletteState, 7);
  g.calls.clear();
  CHECK(rd->saver_main(info, ne16::immsg::kPalette) == 1 && g.joined() == "palette(7)", "msg 6: palette(+0x54) (%s)",
        g.joined().c_str());
  g.calls.clear();
  CHECK(rd->saver_main(info, 3) == 1 && rd->saver_main(info, 4) == 1 && rd->saver_main(info, 12) == 1 &&
            rd->saver_main(info, 99) == 1 && g.calls.empty(),
        "3, 4, 12, 99: 1, nothing called (%s)", g.joined().c_str());
  // CONFIGURE (8): DialogBox(hLib, "DIALOGBOX", +4, saverdlgproc); PANEL (9): 999 first, then CreateDialog.
  g.calls.clear();
  r = rd->saver_main(info, ne16::immsg::kConfigure);
  CHECK(r == 1 && g.joined() == "DialogBox(" + std::to_string(hlib) + ",DIALOGBOX,4369,1)", "CONFIGURE: %u %s", r,
        g.joined().c_str());
  g.calls.clear();
  r = rd->saver_main(info, ne16::immsg::kPanel);
  CHECK(r == 0x2345 && g.w_seen == 999 && g.joined() == "saverinit(999) CreateDialog(DIALOGBOX,4369)",
        "PANEL: saverinit(&999), CreateDialog's HWND with DX 0 (%08X, %s)", r, g.joined().c_str());
  // FREE (11): FreeLibrary(hLib), the block's handle unlocked and freed.
  g.calls.clear();
  r = rd->saver_main(info, ne16::immsg::kFree);
  CHECK(r == 1 && g.only({"FreeLibrary"}) == "FreeLibrary" && rt.rd32(info + ne16::iminfo::kBlock) == b,
        "FREE: FreeLibrary; +0x55 is left as it was (%s)", g.joined().c_str());
  // What IMIMXPLY refuses: IMXX_* (before loading anything), SETCURRSAVER, no SAVERDRAW.
  record("IMXX_ONE.IMX");
  g.calls.clear();
  CHECK(rd->saver_main(info, ne16::immsg::kLoad) == 0 && g.only({"LoadLibrary"}).empty(), "IMXX_*: 0, nothing loaded");
  record("imxX_two.imx");
  CHECK(rd->saver_main(info, ne16::immsg::kLoad) == 0, "imxX_*: any case");
  record("IMX_ONE.IMX");
  g.calls.clear();
  rd->saver_main(info, ne16::immsg::kLoad);
  CHECK(g.only({"LoadLibrary"}) == "LoadLibrary(C:\\SAVER\\IMX_ONE.IMX)", "IMX_ is no IMXX_: loaded (%s)", g.joined().c_str());
  record("SETIMX.IMX");
  CHECK(rd->saver_main(info, ne16::immsg::kLoad) == 0, "SETCURRSAVER: 0");
  record("HALFIMX.IMX");
  CHECK(rd->saver_main(info, ne16::immsg::kLoad) == 0 && rt.rd32(rt.rd32(info + ne16::iminfo::kBlock) + ne16::imblock::kInit) != 0,
        "no SAVERDRAW: 0 (saverinit was found)");
  rd->saver_main(info, ne16::immsg::kFree);
  // No path: the reader describes itself (LOAD 1, QUERY "IMX").
  rt.wr32(info + ne16::iminfo::kPath, 0);
  rt.wr8(info + 1, 0x12);
  CHECK(rd->saver_main(info, ne16::immsg::kLoad) == 1 && rd->saver_main(info, ne16::immsg::kQuery) == 1 &&
            rt.read_str(info + ne16::iminfo::kType) == "IMX" && rt.read_str(info + ne16::iminfo::kName) == "IMX" &&
            rt.rd8(info + 1) == 0x0E,
        "no path: LOAD 1, QUERY 'IMX' at +0x5B and +0x14, byte 1 | 0x0C & ~0x10 (%02X)", rt.rd8(info + 1));
  // The real reader's open: a stand-in under IMIMXPLY's name answers GetProcAddress("saverMain").
  auto imq = ne16::open_imq_reader(rt, "C:\\WINDOWS\\SYSTEM\\IMIMXPLY.IMQ", &why);
  CHECK(imq && std::string(imq->name()) == "imq" && imq->instance() >= 32, "the IMQ reader opens (%s)", why.c_str());
  if (imq) {
    g.calls.clear();
    CHECK(imq->saver_main(info, 12) == 1 && g.joined() == "SAVERMAIN(12)" && g.seen.back().msg == 12 &&
              g.seen.back().hwnd == 0x1111,
          "SAVERMAIN(info, msg): info first, msg last (%s)", g.joined().c_str());
    imq->close();
    CHECK(g.only({"FreeLibrary"}) == "FreeLibrary" && imq->instance() == 0, "close: FreeLibrary");
  }
  auto none = ne16::open_imq_reader(rt, "C:\\WINDOWS\\SYSTEM\\NOSUCH.IMQ", &why);
  CHECK(!none && why.find("cannot load C:\\WINDOWS\\SYSTEM\\NOSUCH.IMQ") != std::string::npos, "no such reader: %s",
        why.c_str());
}

// The IMX protocol (imx_protocol.cc) on a package whose reader is the
// IMIMXPLY stand-in (or, ADNE16READER=native, the native reader over FAKEIMX).
void test_imx_protocol() {
  std::string root = temp_dir("imx");
  std::vector<std::string> dirs = {root + "\\packages", root + "\\packages\\fake", root + "\\packages\\fake\\SAVER",
                                   root + "\\packages\\fake\\ENGINE", root + "\\packages\\fake\\WINDOWS"};
  for (const std::string& d : dirs) CreateDirectoryA(d.c_str(), nullptr);
  const std::string pkg = dirs[1];
  std::vector<std::string> files;
  auto put = [&](const std::string& rel, const std::string& bytes) {
    files.push_back(pkg + "\\" + rel);
    write_file(files.back(), bytes);
  };
  put("SAVER\\FAKEIMX.IMX", ne_image("FAKEIMX", {}, {{}, {{"SAVERINIT", 2}, {"SAVERDRAW", 3}}}));
  put("SAVER\\SWSE.DLL", "stands for SWSE.DLL: the GDI seed's file rule");
  // INTRMLIB's palettes CLUT and HSV (type = name; red = 1 and 2, green the
  // index); no PRIM.
  put("SAVER\\INTRMLIB.DLL", ne_image("INTRMLIB", {{0, 0, logpal_res(1, 256), "CLUT", "CLUT"},
                                                   {0, 0, logpal_res(2, 256), "HSV", "HSV"}}));
  put("ENGINE\\IMIMXPLY.IMQ", "stands for the reader: the IMIMXPLY stand-in answers");
  put("WINDOWS\\SWSE.INI", "[Fake Saver]\r\nDelay=5\r\n");
  auto exists = [](const std::string& f) {
    DWORD a = GetFileAttributesA(f.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
  };
  auto is_dir = [](const std::string& f) {
    DWORD a = GetFileAttributesA(f.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
  };
  const ne16::Ne16Layout layout = ne16::resolve_layout(pkg + "\\SAVER\\FAKEIMX.IMX", root, exists, is_dir);
  CHECK(layout.windows_dir == pkg + "\\WINDOWS", "the package has a windows dir");
  InputState input;
  VirtualClock clock0(VirtualClock::Mode::fixed_step, 16667);
  Screen screen0(64, 48);
  auto profile_int = [](ImRig& g, const char* section, const char* key, const char* file) {
    return uint16_t(g.call("KERNEL", "GetPrivateProfileInt", {win16::l16(g.str(section)), win16::l16(g.str(key)),
                                                               win16::w16(999), win16::l16(g.str(file))}));
  };
  auto profile_str = [](ImRig& g, const char* section, const char* key, const char* file) {
    uint16_t hb = g.rt.global().alloc(win16::GlobalHeap16::kZeroInit, 0x100);
    uint32_t buf = uint32_t(hb) << 16;
    g.call("KERNEL", "GetPrivateProfileString", {win16::l16(g.str(section)), win16::l16(g.str(key)), win16::l16(g.str("")),
                                                  win16::l16(buf), win16::w16(0x100), win16::l16(g.str(file))});
    return g.rt.read_str(buf);
  };

  // A run: the reader's LOAD and QUERY, passes, the stop pass and FREE.
  {
    Env env = Env::parse({{"AD_ASSETS_DIR", root}});
    LaneContext ctx0{env, screen0, clock0, input};
    auto p = ne16::make_imx_protocol(layout);
    win16::Runtime16Options opts;
    p->configure_runtime(opts, ctx0);
    CHECK(std::string(p->name()) == "imx/imq" && opts.guest_dir == "C:\\SAVER" && opts.desktop_palette,
          "the IMQ reader (the engine dir has it), C:\\SAVER, the desktop palette (%s, %s)", p->name(), opts.guest_dir.c_str());
    ImRig g(opts);
    LaneContext ctx{env, g.screen, g.clock, input};
    p->mount(g.rt, env);
    std::string ini;
    CHECK(g.rt.vfs().read_file("C:\\WINDOWS\\SWSE.INI", &ini) && ini.find("[Fake Saver]") != std::string::npos,
          "C:\\WINDOWS over the package's WINDOWS folder");
    CHECK(g.rt.vfs().cwd() == "C:\\SAVER" && g.rt.vfs().exists("C:\\SAVER\\FAKEIMX.IMX") &&
              g.rt.vfs().exists("C:\\WINDOWS\\SYSTEM\\IMIMXPLY.IMQ"),
          "C:\\SAVER (the current directory), C:\\WINDOWS\\SYSTEM = the engine dir");
    CHECK(profile_int(g, "Intermission", "Volume", "ANTSW.INI") == 0 &&
              profile_str(g, "Intermission", "Saver Path", "ANTSW.INI") == "C:\\SAVER" &&
              profile_str(g, "technology", "DibBlit", "SWSE.INI") == "GDI" &&
              profile_str(g, "Fake Saver", "Delay", "SWSE.INI") == "5",
          "the seeds: Volume 0 (sound off), Saver Path, SWSE.INI [technology] GDI over the file's own sections");
    uint16_t hwnd = win16::user16_saver_window(g.rt);
    uint16_t hdc = win16::gdi16_screen_dc(g.rt, hwnd);
    g.calls.clear();
    bool ok = p->load(g.rt, hwnd, hdc, ctx);
    CHECK(ok && g.only({"LoadLibrary", "SAVERMAIN", "saver"}) ==
                    "LoadLibrary(C:\\WINDOWS\\SYSTEM\\IMIMXPLY.IMQ) SAVERMAIN(10) SAVERMAIN(7)",
          "load: the reader by its guest path, LOAD, QUERY (%s)", g.joined().c_str());
    if (!ok) return;
    const ImRig::Seen s10 = g.seen[0];
    CHECK(s10.msg == 10 && s10.flags == ne16::iminfo::kModuleFlags && s10.file == "FAKEIMX.IMX" &&
              s10.path == "C:\\SAVER\\FAKEIMX.IMX" && s10.hwnd == hwnd && s10.reader >= 32 && s10.index == 0 && s10.hdc == 0,
          "LOAD's record: flags %08X, file %s, path %s, hwnd %04X (%04X), reader %04X", s10.flags, s10.file.c_str(),
          s10.path.c_str(), s10.hwnd, hwnd, s10.reader);
    g.calls.clear();
    g.post_task = true;
    for (int i = 0; i < 3; i++) {
      ne16::Protocol16::Call c = p->call();
      CHECK(c.kind == ne16::Protocol16::Call::Kind::ok, "a pass never stops the run");
      p->after_call();
    }
    CHECK(g.only({"GetDC", "SaveDC", "RestoreDC", "ReleaseDC", "SAVERMAIN", "Select", "Realize"}) ==
              "GetDC SaveDC SAVERMAIN(1) RestoreDC ReleaseDC GetDC SaveDC SAVERMAIN(0) RestoreDC ReleaseDC "
              "GetDC SaveDC SAVERMAIN(0) RestoreDC ReleaseDC",
          "three passes: START, then DRAW, each in the DC bracket, no palette (%s)", g.joined().c_str());
    CHECK(g.seen.back().hdc == g.last_dc && g.last_dc != 0 && g.seen.back().hpal == 0 && g.seen.back().hwnd == hwnd,
          "+6 = the pass's DC, +8 = 0");
    win16::StepReport16 rep = win16::user16_end_step(g.rt, 0);
    CHECK(rep.task_posts == 2 && rep.last_task_msg == 0x0200 && !rep.wake,
          "the message loop took the two task posts (FORCETOWAKE's), not a wake (%u)", rep.task_posts);
    g.post_task = false;
    CHECK(!p->set_control(0, 5) && !p->set_control(1, 1), "SET is not the module's");
    g.calls.clear();
    p->unload();
    CHECK(g.only({"GetDC", "SaveDC", "RestoreDC", "ReleaseDC", "SAVERMAIN", "FreeLibrary", "Select"}) ==
              "GetDC SaveDC SAVERMAIN(2) RestoreDC ReleaseDC SAVERMAIN(11) FreeLibrary",
          "unload: the stop pass, FREE, the reader freed (%s)", g.joined().c_str());
    p->close();
  }
  // No START before unload: the stop pass sends nothing. An engine palette
  // (w = 2) is made from INTRMLIB's "HSV" resource, as CANISTART(1) made the
  // one IMCOPYPALETTE copied; it is selected in every pass but the stop pass,
  // and deleted.
  {
    Env env = Env::parse({{"AD_ASSETS_DIR", root}, {"ADSOUND", "1"}, {"ADVOLUME", "30"}});
    LaneContext ctx0{env, screen0, clock0, input};
    auto p = ne16::make_imx_protocol(layout);
    win16::Runtime16Options opts;
    p->configure_runtime(opts, ctx0);
    ImRig g(opts);
    LaneContext ctx{env, g.screen, g.clock, input};
    p->mount(g.rt, env);
    CHECK(profile_int(g, "Intermission", "Volume", "ANTSW.INI") == 30, "sound on: ANTSW.INI Volume = ADVOLUME (%u)",
          profile_int(g, "Intermission", "Volume", "ANTSW.INI"));
    g.palette_type = 2;
    uint16_t hwnd = win16::user16_saver_window(g.rt);
    g.calls.clear();
    bool ok = p->load(g.rt, hwnd, win16::gdi16_screen_dc(g.rt, hwnd), ctx);
    CHECK(ok && g.only({"LoadLibrary(INTRMLIB", "GetDeviceCaps", "FindResource", "LoadResource", "LockResource", "CreatePalette",
                        "FreeResource"}) ==
                    "LoadLibrary(INTRMLIB.DLL) GetDeviceCaps FindResource(HSV) LoadResource LockResource CreatePalette "
                    "FreeResource",
          "w = 2: INTRMLIB's HSV resource, as CANISTART made its palette (%s)", g.joined().c_str());
    g.calls.clear();
    p->call();
    const uint16_t hpal = g.seen.back().hpal;
    CHECK(g.only({"GetDC", "SaveDC", "RestoreDC", "ReleaseDC", "SAVERMAIN", "Select", "Realize"}) ==
              "GetDC SaveDC SelectPalette RealizePalette SAVERMAIN(1) SelectPalette RestoreDC ReleaseDC" &&
              hpal != 0,
          "a pass selects and realizes the engine palette, +8 = it (%s)", g.joined().c_str());
    uint16_t hb = g.rt.global().alloc(win16::GlobalHeap16::kZeroInit, 256 * 4);
    uint16_t n = uint16_t(g.call("GDI", "GetPaletteEntries", {win16::w16(hpal), win16::w16(0), win16::w16(256),
                                                              win16::l16(uint32_t(hb) << 16)}));
    const uint32_t e9 = g.rt.rd32((uint32_t(hb) << 16) + 9 * 4);
    CHECK(n == 256 && (e9 & 0xFFFF) == 0x0902, "the palette is the resource's: %u entries, entry 9 %08X", n, e9);
    g.calls.clear();
    auto q = ne16::make_imx_protocol(layout);  // a second saver, never started
    win16::Runtime16Options o2;
    q->configure_runtime(o2, ctx0);
    g.palette_type = 0;
    q->load(g.rt, hwnd, 0, ctx);
    g.calls.clear();
    q->unload();
    CHECK(g.only({"GetDC", "SaveDC", "RestoreDC", "ReleaseDC", "SAVERMAIN", "Select"}) ==
              "GetDC SaveDC RestoreDC ReleaseDC SAVERMAIN(11)",
          "never started: the stop pass sends no STOP (%s)", g.joined().c_str());
    g.calls.clear();
    p->unload();
    CHECK(g.only({"SAVERMAIN", "Select", "DeleteObject", "FreeLibrary"}) ==
              "SAVERMAIN(2) SAVERMAIN(11) FreeLibrary DeleteObject FreeLibrary",
          "the stop pass selects no palette; the palette deleted, INTRMLIB and the reader freed (%s)", g.joined().c_str());
  }
  // The other palette types: 1 and anything but 2 and 3 take CLUT; 3 takes
  // PRIM, which this INTRMLIB lacks, so the module runs without a palette.
  for (auto [w, want] : {std::pair<uint8_t, int>{1, 1}, {7, 1}, {3, 0}}) {
    Env env = Env::parse({{"AD_ASSETS_DIR", root}});
    LaneContext ctx0{env, screen0, clock0, input};
    auto p = ne16::make_imx_protocol(layout);
    win16::Runtime16Options opts;
    p->configure_runtime(opts, ctx0);
    ImRig g(opts);
    LaneContext ctx{env, g.screen, g.clock, input};
    p->mount(g.rt, env);
    g.palette_type = w;
    uint16_t hwnd = win16::user16_saver_window(g.rt);
    g.calls.clear();
    bool ok = p->load(g.rt, hwnd, 0, ctx);
    p->call();
    const uint16_t hpal = g.seen.back().hpal;
    int tag = 0;
    if (hpal) {
      uint16_t hb = g.rt.global().alloc(win16::GlobalHeap16::kZeroInit, 4);
      g.call("GDI", "GetPaletteEntries", {win16::w16(hpal), win16::w16(0), win16::w16(1), win16::l16(uint32_t(hb) << 16)});
      tag = g.rt.rd8(uint32_t(hb) << 16);
    }
    const std::string asked = g.only({"FindResource"});
    CHECK(ok && tag == want && (want || g.only({"Select"}).empty()),
          "w = %u: %s, the palette's tag %d (want %d)", unsigned(w), asked.c_str(), tag, want);
    p->unload();
  }
  // START returned, but its pass is abandoned in the bracket's RestoreDC (a
  // frame that ended there, then the run closed): the stop pass sends STOP.
  {
    Env env = Env::parse({{"AD_ASSETS_DIR", root}});
    LaneContext ctx0{env, screen0, clock0, input};
    auto p = ne16::make_imx_protocol(layout);
    win16::Runtime16Options opts;
    p->configure_runtime(opts, ctx0);
    ImRig g(opts);
    LaneContext ctx{env, g.screen, g.clock, input};
    p->mount(g.rt, env);
    uint16_t hwnd = win16::user16_saver_window(g.rt);
    bool ok = p->load(g.rt, hwnd, 0, ctx);
    struct Abandoned {};
    g.after_saver_main = [&](uint16_t msg) {
      if (msg == ne16::immsg::kStart) g.rt.set_deadline(0, [] { throw Abandoned{}; });  // at the next API call
    };
    g.calls.clear();
    bool abandoned = false;
    try {
      p->call();
    } catch (const Abandoned&) {
      abandoned = true;
    }
    g.rt.clear_deadline();
    g.after_saver_main = nullptr;
    CHECK(ok && abandoned && g.only({"SAVERMAIN", "RestoreDC", "ReleaseDC"}) == "SAVERMAIN(1)",
          "START returned, the pass abandoned before its RestoreDC (%s)", g.joined().c_str());
    g.calls.clear();
    p->unload();
    CHECK(g.only({"SAVERMAIN"}) == "SAVERMAIN(2) SAVERMAIN(11)", "an abandoned START pass still gets its STOP (%s)",
          g.joined().c_str());
  }
  // Sound on with the engine: ANTSW.INI Volume is the engine's volume, and the
  // MIDI bus follows it as After Dark's volume did (linear: 30 → 30%), so
  // MEMMIDI's music scales with it; without the engine the bus is left alone.
  {
    audio::Config cfg;
    cfg.guest_sound = true;
    cfg.volume = 30;
    std::unique_ptr<audio::Engine> engine = audio::make_engine(cfg);
    Env env = Env::parse({{"AD_ASSETS_DIR", root}, {"ADVOLUME", "80"}});
    LaneContext ctx0{env, screen0, clock0, input, engine.get()};
    auto p = ne16::make_imx_protocol(layout);
    win16::Runtime16Options opts;
    p->configure_runtime(opts, ctx0);
    ImRig g(opts);
    LaneContext ctx{env, g.screen, g.clock, input, engine.get()};
    p->mount(g.rt, env);
    const uint16_t volume = profile_int(g, "Intermission", "Volume", "ANTSW.INI");
    bool ok = p->load(g.rt, win16::user16_saver_window(g.rt), 0, ctx);
    const audio::Gain midi = engine->bus_gain(audio::Bus::midi), wave = engine->bus_gain(audio::Bus::wave);
    CHECK(ok && volume == 30 && midi == audio::gain_from_mm(0x4CCC4CCC) && midi.left >= 9829 && midi.left <= 9831 &&
              wave == audio::Gain{},
          "the engine's volume 30: ANTSW.INI Volume %u, the MIDI bus %04X/%04X (0.3 of unity), the wave bus untouched",
          volume, midi.left, midi.right);
    p->unload();
    engine->shutdown(g.rt.peek_us());
  }
  // LOAD refused: FREE follows, the load fails; ADNE16READER=imq without an
  // IMIMXPLY.IMQ fails; ADNE16READER=native runs the native reader.
  {
    Env env = Env::parse({{"AD_ASSETS_DIR", root}});
    LaneContext ctx0{env, screen0, clock0, input};
    auto p = ne16::make_imx_protocol(layout);
    win16::Runtime16Options opts;
    p->configure_runtime(opts, ctx0);
    ImRig g(opts);
    LaneContext ctx{env, g.screen, g.clock, input};
    p->mount(g.rt, env);
    g.answers[10] = 0;
    uint16_t hwnd = win16::user16_saver_window(g.rt);
    g.calls.clear();
    CHECK(!p->load(g.rt, hwnd, 0, ctx) && g.only({"SAVERMAIN", "FreeLibrary"}) == "SAVERMAIN(10) SAVERMAIN(11) FreeLibrary",
          "LOAD refused: FREE, the reader freed, the load fails (%s)", g.joined().c_str());
  }
  {
    ne16::Ne16Layout bare = layout;
    bare.engine_dir = pkg + "\\WINDOWS";  // no IMIMXPLY.IMQ there, nor in SAVER
    Env env = Env::parse({{"AD_ASSETS_DIR", root}, {"ADNE16READER", "imq"}});
    LaneContext ctx0{env, screen0, clock0, input};
    auto p = ne16::make_imx_protocol(bare);
    win16::Runtime16Options opts;
    p->configure_runtime(opts, ctx0);
    ImRig g(opts);
    LaneContext ctx{env, g.screen, g.clock, input};
    p->mount(g.rt, env);
    g.calls.clear();
    CHECK(std::string(p->name()) == "imx/imq" && !p->load(g.rt, win16::user16_saver_window(g.rt), 0, ctx) &&
              g.only({"LoadLibrary", "SAVERMAIN"}).empty(),
          "ADNE16READER=imq without IMIMXPLY.IMQ: nothing is loaded, the load fails (%s)", g.joined().c_str());
    Env auto_env = Env::parse({{"AD_ASSETS_DIR", root}});
    LaneContext actx{auto_env, screen0, clock0, input};
    auto a = ne16::make_imx_protocol(bare);
    a->configure_runtime(opts, actx);
    CHECK(std::string(a->name()) == "imx/native", "auto without IMIMXPLY.IMQ: the native reader");
  }
  {
    Env env = Env::parse({{"AD_ASSETS_DIR", root}, {"ADNE16READER", "native"}});
    LaneContext ctx0{env, screen0, clock0, input};
    auto p = ne16::make_imx_protocol(layout);
    win16::Runtime16Options opts;
    p->configure_runtime(opts, ctx0);
    CHECK(std::string(p->name()) == "imx/native", "ADNE16READER=native over an installed IMQ: %s", p->name());
    ImRig g(opts);
    LaneContext ctx{env, g.screen, g.clock, input};
    p->mount(g.rt, env);
    uint16_t hwnd = win16::user16_saver_window(g.rt);
    g.calls.clear();
    bool ok = p->load(g.rt, hwnd, 0, ctx);
    win16::Module16* fake = g.rt.modules().by_name("FAKEIMX");
    const uint16_t hinst = fake ? fake->hinstance : 0;
    for (int i = 0; i < 2 && ok; i++) {
      p->call();
      p->after_call();
    }
    if (ok) p->unload();
    CHECK(ok && g.only({"LoadLibrary", "saver", "palette", "SAVERMAIN", "FreeLibrary"}) ==
                    "LoadLibrary(C:\\SAVER\\FAKEIMX.IMX) palette(0) saverinit(1) saverdraw(1) saverdraw(0) saverdraw(2) "
                    "FreeLibrary",
          "the native reader drives FAKEIMX: LOAD, QUERY, START, DRAW, STOP, FREE (%s)", g.joined().c_str());
    CHECK(g.draws.size() == 3 && g.draws[0].hwnd == hwnd && g.draws[0].hdc != 0 && hinst != 0 &&
              g.draws[0].hlib == hinst && g.draws[0].hpal == 0,
          "saverdraw(the saver window, the pass's DC, the module's instance, no palette)");
  }
  // A button: slot 0 only; LOAD, QUERY, +4 = the owner, CONFIGURE, FREE.
  {
    Env env = Env::parse({{"AD_ASSETS_DIR", root}, {"ADSOUND", "1"}});
    auto p = ne16::make_imx_protocol(layout);
    std::string why;
    CHECK(!p->check_button(1, &why) && why == "control 1 is not a button" && !p->check_button(-1, &why) &&
              p->check_button(0, &why),
          "only button 0 (%s)", why.c_str());
    win16::Runtime16Options opts;
    p->configure_button_runtime(opts, env);
    CHECK(opts.guest_dir == "C:\\SAVER" && opts.desktop_palette, "configure mode: C:\\SAVER, the desktop palette");
    ImRig g(opts);
    LaneContext ctx{env, g.screen, g.clock, input};
    p->mount(g.rt, env);
    CHECK(profile_int(g, "Intermission", "Volume", "ANTSW.INI") == 0, "configure mode is silent: Volume 0");
    g.calls.clear();
    ne16::Protocol16::Button b = p->button(g.rt, 0, 0xC004, ctx);
    CHECK(b.ran && b.failure.empty() && b.message.empty() &&
              g.only({"SAVERMAIN", "GetDC"}) == "SAVERMAIN(10) SAVERMAIN(7) SAVERMAIN(8) SAVERMAIN(11)",
          "button 0: LOAD, QUERY, CONFIGURE, FREE; no pass (%s)", g.joined().c_str());
    CHECK(g.seen.size() == 4 && g.seen[0].hwnd == 0 && g.seen[1].hwnd == 0 && g.seen[2].hwnd == 0xC004,
          "+4 = 0 for LOAD and QUERY, the owner for CONFIGURE");
    p->close();
    g.answers[8] = 0;
    auto q = ne16::make_imx_protocol(layout);
    q->configure_button_runtime(opts, env);
    b = q->button(g.rt, 0, 0, ctx);
    CHECK(b.ran && b.failure.empty() && b.message.find("no Configure dialog") != std::string::npos,
          "CONFIGURE answering 0 (no SAVERDLGPROC): ran, nothing to show (%s)", b.message.c_str());
    g.answers[8] = 1;
    g.answers[10] = 0;
    auto f = ne16::make_imx_protocol(layout);
    f->configure_button_runtime(opts, env);
    b = f->button(g.rt, 0, 0, ctx);
    CHECK(b.ran && b.failure.find("refused") != std::string::npos, "LOAD refused: a failure (%s)", b.failure.c_str());
  }
  for (const std::string& f : files) DeleteFileA(f.c_str());
  for (auto d = dirs.rbegin(); d != dirs.rend(); ++d) RemoveDirectoryA(d->c_str());
  RemoveDirectoryA(root.c_str());
}

// The other two forms of an Intermission module (package.hh "Form"): an ASA
// animation, read by IMASAPLY.IMQ (from the engine dir, else the module
// dir; no native reader), and an IMQ module that is its own reader (its
// record a reader's: index -1, no path; refused when its QUERY clears the
// saver flag). The stand-ins are ImRig's (IMASAPLY, FAKEIMQ).
void test_imx_forms() {
  std::string root = temp_dir("imxforms");
  std::vector<std::string> dirs = {root + "\\packages", root + "\\packages\\forms", root + "\\packages\\forms\\SAVER",
                                   root + "\\packages\\forms\\ENGINE"};
  for (const std::string& d : dirs) CreateDirectoryA(d.c_str(), nullptr);
  const std::string pkg = dirs[1];
  std::vector<std::string> files;
  auto put = [&](const std::string& rel, const std::string& bytes) {
    files.push_back(pkg + "\\" + rel);
    write_file(files.back(), bytes);
  };
  auto drop = [&](const std::string& rel) { DeleteFileA((pkg + "\\" + rel).c_str()); };
  auto exists = [](const std::string& f) {
    DWORD a = GetFileAttributesA(f.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
  };
  put("SAVER\\ANIM.ASA", std::string("AniN") + std::string(60, '\0'));
  put("SAVER\\FAKEIMQ.IMQ", ne_image("FAKEIMQ", {}, {{}, {{"SAVERMAIN", 2}, {"SAVERDLGPROC", 3}}}));
  InputState input;
  VirtualClock clock0(VirtualClock::Mode::fixed_step, 16667);
  Screen screen0(64, 48);
  struct Ran {
    std::string name, calls;
    bool ok = false;
    std::vector<ImRig::Seen> seen;
  };
  // configure_runtime, mount, load, two passes, unload, close: what the stand-ins saw.
  using Vars = std::initializer_list<std::pair<std::string, std::string>>;
  auto run = [&](const std::string& module, ne16::ImxForm form, Vars kv, uint32_t query_clears = 0,
                 uint32_t query_sets = 0) {
    std::map<std::string, std::string> vars = {{"AD_ASSETS_DIR", root}};
    vars.insert(kv.begin(), kv.end());
    Env env = Env::parse(vars);
    LaneContext ctx0{env, screen0, clock0, input};
    const ne16::Ne16Layout layout = ne16::resolve_layout(pkg + "\\SAVER\\" + module, root, exists);
    auto p = ne16::make_imx_protocol(layout, form);
    win16::Runtime16Options opts;
    p->configure_runtime(opts, ctx0);
    ImRig g(opts);
    g.query_clears = query_clears;
    g.query_sets = query_sets;
    g.intrmlib_modules(60);
    LaneContext ctx{env, g.screen, g.clock, input};
    p->mount(g.rt, env);
    g.calls.clear();
    Ran r;
    r.name = p->name();
    r.ok = p->load(g.rt, win16::user16_saver_window(g.rt), 0, ctx);
    for (int i = 0; i < 2 && r.ok; i++) {
      p->call();
      p->after_call();
    }
    if (r.ok) p->unload();
    p->close();
    r.calls = g.only({"LoadLibrary", "SAVERMAIN", "FreeLibrary", "FINDALLMODULES", "FREEMODINFO"});
    r.seen = g.seen;
    return r;
  };

  // An ASA animation: IMASAPLY.IMQ from the engine dir, LOAD and QUERY with the file's path.
  put("ENGINE\\IMASAPLY.IMQ", "stands for the ASA reader: the IMASAPLY stand-in answers");
  Ran a = run("ANIM.ASA", ne16::ImxForm::asa, {});
  CHECK(a.ok && a.name == "imx/imq" &&
            a.calls == "LoadLibrary(C:\\WINDOWS\\SYSTEM\\IMASAPLY.IMQ) SAVERMAIN(10) SAVERMAIN(7) SAVERMAIN(1) "
                       "SAVERMAIN(0) SAVERMAIN(2) SAVERMAIN(11) FreeLibrary",
        "an ASA animation: IMASAPLY.IMQ from C:\\WINDOWS\\SYSTEM, LOAD, QUERY, START, DRAW, STOP, FREE (%s)",
        a.calls.c_str());
  CHECK(!a.seen.empty() && a.seen[0].module == "IMASAPLY" && a.seen[0].msg == 10 && a.seen[0].index == 0 &&
            a.seen[0].file == "ANIM.ASA" && a.seen[0].path == "C:\\SAVER\\ANIM.ASA" &&
            a.seen[0].flags == ne16::iminfo::kModuleFlags,
        "LOAD's record: the animation's path at +0x63, reader index 0, file ANIM.ASA (%s, %s)",
        a.seen.empty() ? "" : a.seen[0].path.c_str(), a.seen.empty() ? "" : a.seen[0].file.c_str());
  a = run("ANIM.ASA", ne16::ImxForm::asa, {{"ADNE16READER", "native"}});
  CHECK(a.ok && a.name == "imx/imq" && a.calls.rfind("LoadLibrary(C:\\WINDOWS\\SYSTEM\\IMASAPLY.IMQ)", 0) == 0,
        "ADNE16READER=native: ignored for an ASA animation (%s)", a.calls.c_str());
  drop("ENGINE\\IMASAPLY.IMQ");
  put("SAVER\\IMASAPLY.IMQ", "the ASA reader beside the modules, as Intermission's installer put it");
  a = run("ANIM.ASA", ne16::ImxForm::asa, {});
  CHECK(a.ok && a.calls.rfind("LoadLibrary(C:\\SAVER\\IMASAPLY.IMQ) SAVERMAIN(10)", 0) == 0,
        "IMASAPLY.IMQ from the module dir when the engine dir has none (%s)", a.calls.c_str());
  drop("SAVER\\IMASAPLY.IMQ");
  a = run("ANIM.ASA", ne16::ImxForm::asa, {});
  CHECK(!a.ok && a.name == "imx/imq" && a.calls.empty(), "no IMASAPLY.IMQ: nothing loaded, the load fails (%s)",
        a.calls.c_str());

  // An IMQ module: loaded by its own path, a reader's record (index -1, +0x63 = 0), QUERY without a path.
  Ran q = run("FAKEIMQ.IMQ", ne16::ImxForm::imq, {});
  CHECK(q.ok && q.name == "imx/imq" &&
            q.calls == "LoadLibrary(C:\\SAVER\\FAKEIMQ.IMQ) SAVERMAIN(10) SAVERMAIN(7) SAVERMAIN(1) SAVERMAIN(0) "
                       "SAVERMAIN(2) SAVERMAIN(11) FreeLibrary",
        "an IMQ module: itself as the reader, LOAD, QUERY, START, DRAW, STOP, FREE (%s)", q.calls.c_str());
  CHECK(q.seen.size() >= 2 && q.seen[0].module == "FAKEIMQ" && q.seen[0].index == 0xFFFF && q.seen[0].path_ptr == 0 &&
            q.seen[0].file == "FAKEIMQ.IMQ" && q.seen[1].msg == 7 && q.seen[1].path_ptr == 0 && q.seen[0].reader >= 32,
        "its record: index -1, no path for LOAD and QUERY, file FAKEIMQ.IMQ, its own instance at +0x0A");
  q = run("FAKEIMQ.IMQ", ne16::ImxForm::imq, {{"ADNE16READER", "native"}});
  CHECK(q.ok && q.calls.rfind("LoadLibrary(C:\\SAVER\\FAKEIMQ.IMQ)", 0) == 0,
        "ADNE16READER=native: ignored for an IMQ module (%s)", q.calls.c_str());
  q = run("FAKEIMQ.IMQ", ne16::ImxForm::imq, {}, ne16::iminfo::kSaver);
  CHECK(!q.ok && q.calls == "LoadLibrary(C:\\SAVER\\FAKEIMQ.IMQ) SAVERMAIN(10) SAVERMAIN(7) SAVERMAIN(11) FreeLibrary",
        "an IMQ whose QUERY clears 0x1000 is a reader: FREE, freed, refused (%s)", q.calls.c_str());
  // One that keeps 0x1000 beside 0x0800 is a reader too (INTERMIS 6:03d8):
  // the Morph, MultiSaver and Sequencer readers, listed for their editors.
  q = run("FAKEIMQ.IMQ", ne16::ImxForm::imq, {}, 0, ne16::iminfo::kIsReader);
  CHECK(!q.ok && q.calls == "LoadLibrary(C:\\SAVER\\FAKEIMQ.IMQ) SAVERMAIN(10) SAVERMAIN(7) SAVERMAIN(11) FreeLibrary",
        "an IMQ whose QUERY says saver and reader (0x1800) is refused (%s)", q.calls.c_str());

  // The other data files (package.hh "Form"): the reader of their type, from
  // the engine dir, else the module dir; LOAD and QUERY with the file's path.
  const std::pair<const char*, const char*> kReaders[] = {{"IMFLIPLY", "FLI"}, {"IMFLCPLY", "FLC"}, {"IMMRFPLY", "MRF"}};
  for (const auto& [reader, type] : kReaders) {
    const std::string file = std::string("DATA.") + type;
    ne16::ImxForm form = ne16::ImxForm::imx;
    ne16::form_for_type(type, &form);
    put("SAVER\\" + file, std::string("made-up ") + type + " bytes");
    Ran d = run(file, form, {});
    CHECK(!d.ok && d.calls.empty(), "%s without %s.IMQ: nothing loaded, the load fails (%s)", file.c_str(), reader,
          d.calls.c_str());
    put(std::string("SAVER\\") + reader + ".IMQ", std::string("stands for ") + reader);
    d = run(file, form, {{"ADNE16READER", "native"}});
    CHECK(d.ok && d.name == "imx/imq" &&
              d.calls == std::string("LoadLibrary(C:\\SAVER\\") + reader +
                             ".IMQ) SAVERMAIN(10) SAVERMAIN(7) SAVERMAIN(1) SAVERMAIN(0) SAVERMAIN(2) SAVERMAIN(11) "
                             "FreeLibrary",
          "%s: %s.IMQ beside it, LOAD, QUERY, START, DRAW, STOP, FREE; ADNE16READER=native ignored (%s)", file.c_str(),
          reader, d.calls.c_str());
    CHECK(!d.seen.empty() && d.seen[0].module == reader && d.seen[0].msg == 10 && d.seen[0].index == 0 &&
              d.seen[0].file == file && d.seen[0].path == "C:\\SAVER\\" + file,
          "%s: LOAD's record has the file's path and reader index 0 (%s)", file.c_str(),
          d.seen.empty() ? "" : d.seen[0].path.c_str());
    put(std::string("ENGINE\\") + reader + ".IMQ", "the engine dir's copy wins");
    d = run(file, form, {});
    CHECK(d.ok && d.calls.rfind(std::string("LoadLibrary(C:\\WINDOWS\\SYSTEM\\") + reader + ".IMQ)", 0) == 0,
          "%s: the engine dir's reader first (%s)", file.c_str(), d.calls.c_str());
    drop(std::string("ENGINE\\") + reader + ".IMQ");
  }
  // A MultiSaver group finds its modules in INTRMLIB's table, which INTERMIS
  // made at its start: FINDALLMODULES before the group's LOAD, FREEMODINFO
  // after its FREE (the stand-in's table is empty).
  put("SAVER\\GROUP.MSV", std::string(4, '\0') + "made-up group");
  put("SAVER\\IMMSVPLY.IMQ", "stands for the MultiSaver reader");
  Ran v = run("GROUP.MSV", ne16::ImxForm::msv, {});
  CHECK(v.ok && v.calls == "LoadLibrary(INTRMLIB.DLL) FINDALLMODULES LoadLibrary(C:\\SAVER\\IMMSVPLY.IMQ) SAVERMAIN(10) "
                           "SAVERMAIN(7) SAVERMAIN(1) SAVERMAIN(0) SAVERMAIN(2) SAVERMAIN(11) FreeLibrary FREEMODINFO "
                           "FreeLibrary",
        "an MSV group: INTRMLIB's module table around its whole run (%s)", v.calls.c_str());
  Ran f = run("DATA.FLI", ne16::ImxForm::fli, {});
  CHECK(f.ok && f.calls.find("FINDALLMODULES") == std::string::npos, "no other form makes the table (%s)",
        f.calls.c_str());

  // A button on an IMQ module: LOAD, QUERY, CONFIGURE (+4 = the owner), FREE,
  // through itself — with INTRMLIB, the control panel's (INTERMIS.EXE imports
  // it), loaded around it when the package has one.
  for (const bool intrmlib : {false, true}) {
    Env env = Env::parse({{"AD_ASSETS_DIR", root}});
    const ne16::Ne16Layout layout = ne16::resolve_layout(pkg + "\\SAVER\\FAKEIMQ.IMQ", root, exists);
    auto p = ne16::make_imx_protocol(layout, ne16::ImxForm::imq);
    win16::Runtime16Options opts;
    p->configure_button_runtime(opts, env);
    ImRig g(opts);
    if (intrmlib) g.intrmlib_modules(0);
    LaneContext ctx{env, g.screen, g.clock, input};
    p->mount(g.rt, env);
    g.calls.clear();
    ne16::Protocol16::Button b = p->button(g.rt, 0, 0xC004, ctx);
    p->close();
    CHECK(b.ran && b.failure.empty() &&
              g.only({"LoadLibrary", "SAVERMAIN", "FreeLibrary"}) ==
                  std::string("LoadLibrary(INTRMLIB.DLL) LoadLibrary(C:\\SAVER\\FAKEIMQ.IMQ) SAVERMAIN(10) SAVERMAIN(7) "
                              "SAVERMAIN(8) SAVERMAIN(11) FreeLibrary") +
                      (intrmlib ? " FreeLibrary" : "") &&
              g.seen.size() == 4 && g.seen[2].hwnd == 0xC004 && g.seen[2].path_ptr == 0,
          "an IMQ module's button: CONFIGURE through itself, owned by --owner; INTRMLIB %s (%s)",
          intrmlib ? "loaded and freed around it" : "looked for (none here)", g.joined().c_str());
  }
  for (const std::string& f : files) DeleteFileA(f.c_str());
  for (auto d = dirs.rbegin(); d != dirs.rend(); ++d) RemoveDirectoryA(d->c_str());
  RemoveDirectoryA(root.c_str());
}

int run_all_unit() {
  run_unit();
  test_layout();
  test_palettes();
  test_ad2_palettes();
  try {
    test_kinds();
  } catch (const std::exception& e) {
    CHECK(false, "kinds: exception %s", e.what());
  }
  try {
    test_native_reader();
  } catch (const std::exception& e) {
    CHECK(false, "native reader: exception %s", e.what());
  }
  try {
    test_imx_protocol();
  } catch (const std::exception& e) {
    CHECK(false, "IMX protocol: exception %s", e.what());
  }
  try {
    test_imx_forms();
  } catch (const std::exception& e) {
    CHECK(false, "IMX forms: exception %s", e.what());
  }
  try {
    test_native_bridge();
  } catch (const std::exception& e) {
    CHECK(false, "native bridge: exception %s", e.what());
  }
  try {
    test_native_bridge_ad_snd10();
  } catch (const std::exception& e) {
    CHECK(false, "native bridge, AD_SND 1.0: exception %s", e.what());
  }
  try {
    test_host_ad_snd();
  } catch (const std::exception& e) {
    CHECK(false, "the host's AD_SND: exception %s", e.what());
  }
  try {
    test_protocol_seam();
  } catch (const std::exception& e) {
    CHECK(false, "protocol seam: exception %s", e.what());
  }
  try {
    test_lane_machinery();
  } catch (const std::exception& e) {
    CHECK(false, "lane machinery: exception %s", e.what());
  }
  try {
    test_ad3_protocol();
  } catch (const std::exception& e) {
    CHECK(false, "AD3 protocol: exception %s", e.what());
  }
  printf("%d/%d checks passed\n", checks - failures, checks);
  return failures ? 1 : 0;
}

// The installed assets' win dir: AD_ASSETS_DIR, else the data folder's
// (read-only; core/tests/test_paths.h).
std::string assets_win() {
  const char* a = getenv("AD_ASSETS_DIR");
  std::string root;
  if (a && *a) {
    root = a;
  } else {
    root = adw_test::installed_assets_root();
    if (root.empty()) return {};
  }
  if (GetFileAttributesA((root + "\\win\\FILES").c_str()) != INVALID_FILE_ATTRIBUTES) return root + "\\win";
  if (GetFileAttributesA((root + "\\FILES").c_str()) != INVALID_FILE_ATTRIBUTES) return root;
  return {};
}

// Runs adhostwin headless on a module; returns its exit code and stderr.
int run_host(const std::string& exe, const std::string& module, int frames, std::string* err,
             const std::string& extra = "") {
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE rd = nullptr, wr = nullptr;
  CreatePipe(&rd, &wr, &sa, 0);
  SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
  HANDLE nul = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING,
                           0, nullptr);
  STARTUPINFOA si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = nul;
  si.hStdOutput = nul;
  si.hStdError = wr;
  std::string cmd = "\"" + exe + "\" " + module + " ADFRAMES=" + std::to_string(frames) + " ADGOWAITMS=0 ADFBHASH=1" + (extra.empty() ? "" : " " + extra);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    CloseHandle(rd);
    CloseHandle(wr);
    CloseHandle(nul);
    return -1;
  }
  CloseHandle(wr);
  CloseHandle(nul);
  std::string out;
  char buf[4096];
  DWORD got = 0;
  while (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got) out.append(buf, got);
  CloseHandle(rd);
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  if (err) *err = out;
  return int(code);
}

// Runs adhostwin on `module` for `frames` frames with its output discarded,
// sampling its GDI object count every 10 ms; returns the exit code and the
// highest count seen.
int run_host_gdi_peak(const std::string& exe, const std::string& module, int frames, DWORD* peak) {
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE nul = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING,
                           0, nullptr);
  STARTUPINFOA si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = si.hStdOutput = si.hStdError = nul;
  std::string cmd = "\"" + exe + "\" " + module + " ADFRAMES=" + std::to_string(frames) + " ADGOWAITMS=0";
  PROCESS_INFORMATION pi{};
  BOOL ok = CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  CloseHandle(nul);
  if (!ok) return -1;
  *peak = 0;
  do {
    *peak = std::max(*peak, GetGuiResources(pi.hProcess, GR_GDIOBJECTS));
  } while (WaitForSingleObject(pi.hProcess, 10) == WAIT_TIMEOUT);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  return int(code);
}

std::string hashes(const std::string& log) {
  std::string h;
  size_t p = 0;
  while ((p = log.find("FBHASH ", p)) != std::string::npos) {
    size_t e = log.find('\n', p);
    h += log.substr(p, e - p) + "\n";
    p = e;
  }
  return h;
}

// Sound (lane.hh "Sound", AUDIO.md §8, §10.3), captured headless — no device
// is ever opened. Rat Race's engine (ADXPL300) passes the music gates, plays
// its intro song, and the song's MM_MCINOTIFY, dispatched to its adwMidiCall
// window, chains the loop song; captures are byte-identical run to run, and
// ADSOUND=1 draws what the captured run drew. Bungee Jumping (Totally Twisted)
// plays its MS-ADPCM sounds through sndPlaySound; Nocturne's synchronous
// sounds hold its DRAWFRAME for their duration.
void test_sound_assets(const std::string& exe, const std::string& win) {
  std::string dir = temp_dir("sound");
  auto slurp = [](const std::string& p) {
    std::string s;
    if (FILE* f = fopen(p.c_str(), "rb")) {
      char buf[65536];
      size_t n;
      while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
      fclose(f);
    }
    return s;
  };
  // Note-ons (velocity > 0) in a format-0 SMF event log.
  auto note_ons = [](const std::string& smf) {
    size_t p = smf.find("MTrk");
    if (p == std::string::npos) return -1;
    p += 8;
    int n = 0;
    uint8_t status = 0;
    auto byte = [&]() -> int { return p < smf.size() ? uint8_t(smf[p++]) : -1; };
    auto vlq = [&]() {
      uint32_t v = 0;
      for (int i = 0; i < 4; i++) {
        int b = byte();
        if (b < 0) break;
        v = (v << 7) | uint32_t(b & 0x7F);
        if (!(b & 0x80)) break;
      }
      return v;
    };
    while (p < smf.size()) {
      vlq();
      int b = byte();
      if (b < 0) break;
      if (b == 0xFF) {
        byte();
        p += vlq();
        continue;
      }
      if (b == 0xF0 || b == 0xF7) {
        p += vlq();
        continue;
      }
      if (b & 0x80) status = uint8_t(b);
      else p--;
      int d1 = byte(), d2 = (status & 0xE0) == 0xC0 ? 0 : byte();
      (void)d1;
      if ((status & 0xF0) == 0x90 && d2 > 0) n++;
    }
    return n;
  };
  // The largest |sample| of a 16-bit WAV's data chunk.
  auto peak = [](const std::string& wav) {
    size_t p = wav.find("data");
    int m = 0;
    for (size_t i = p == std::string::npos ? wav.size() : p + 8; i + 1 < wav.size(); i += 2) {
      int v = int16_t(uint16_t(uint8_t(wav[i]) | (uint8_t(wav[i + 1]) << 8)));
      m = std::max(m, v < 0 ? -v : v);
    }
    return m;
  };
  {
    std::string e1, e2, es;
    std::string w1 = dir + "\\rat1.wav", w2 = dir + "\\rat2.wav";
    int c1 = run_host(exe, "FILES/CLASSIC/RATRACE.AD", 900, &e1, "ADTRACE=sound \"ADAUDIOOUT=" + w1 + "\"");
    int c2 = run_host(exe, "FILES/CLASSIC/RATRACE.AD", 900, &e2, "\"ADAUDIOOUT=" + w2 + "\"");
    int cs = run_host(exe, "FILES/CLASSIC/RATRACE.AD", 900, &es, "ADSOUND=1");
    std::string m1 = slurp(dir + "\\rat1.mid");
    CHECK(c1 == 0 && c2 == 0 && cs == 0, "RATRACE with sound: exit %d/%d/%d\n%s", c1, c2, cs, c1 ? e1.c_str() : "");
    CHECK(e1.find("open sequencer!C:\\AFTERDRK\\music\\tellintr.mid alias fred wait") != std::string::npos &&
              e1.find("notify SUCCESSFUL") != std::string::npos && e1.find("tellloop.mid") != std::string::npos,
          "RATRACE: the intro song plays and its MM_MCINOTIFY chains the loop song");
    CHECK(note_ons(m1) >= 100, "RATRACE: the MIDI log has the songs' notes (%d note-ons)", note_ons(m1));
    CHECK(!m1.empty() && slurp(w1).size() > 44 && slurp(w1) == slurp(w2) && m1 == slurp(dir + "\\rat2.mid") &&
              hashes(e1) == hashes(e2),
          "RATRACE: captures and frames identical run to run");
    CHECK(hashes(es) == hashes(e2) && std::count(e2.begin(), e2.end(), '\n') > 0,
          "RATRACE: ADSOUND=1 draws what the captured run drew");
  }
  // The bridge's sound settings (AUDIO.md §8.1): unmuted at the engine's
  // volume with sound on; muted at ADVOLUME without (the 1996 host's "sound
  // off"), through OLDMOD16 and the native bridge alike.
  for (const char* m : {"FILES/CLASSIC/DOMINOES.AD", "packages/simpsons/SIMPSONS/BURNS.AD"}) {
    if (GetFileAttributesA((win + "\\" + m).c_str()) == INVALID_FILE_ATTRIBUTES) continue;
    std::string on, off;
    int c1 = run_host(exe, m, 2, &on, "ADTRACE=lane ADSOUND=1 ADVOLUME=30");
    int c2 = run_host(exe, m, 2, &off, "ADTRACE=lane ADVOLUME=30");
    CHECK(c1 == 0 && c2 == 0 && on.find(", volume 30, mute 0) -> 1") != std::string::npos &&
              off.find(", volume 30, mute 1) -> 1") != std::string::npos,
          "%s: LOADADMODULE16 gets volume 30, unmuted only with sound on", m);
  }
  if (GetFileAttributesA((win + "\\packages\\tt\\TWISTED\\BUNGEE.AD").c_str()) != INVALID_FILE_ATTRIBUTES) {
    std::string e, w = dir + "\\bungee.wav";
    int c = run_host(exe, "packages/tt/TWISTED/BUNGEE.AD", 600, &e, "ADTRACE=sound \"ADAUDIOOUT=" + w + "\"");
    int pk = peak(slurp(w));
    CHECK(c == 0 && e.find(": voice ") != std::string::npos && e.find("tag 2,") != std::string::npos && pk > 0x1000,
          "BUNGEE: its MS-ADPCM sounds play through sndPlaySound (exit %d, peak %d)", c, pk);
  }
  {
    std::string e, w = dir + "\\nocturne.wav";
    int c = run_host(exe, "FILES/CLASSIC/NOCTURNE.AD", 600, &e, "ADTRACE=sound,pace \"ADAUDIOOUT=" + w + "\"");
    size_t sync = e.find(", synchronous until ");
    size_t held = sync == std::string::npos ? sync : e.find("0 DRAWFRAME(s), resumed, ended inside one", sync);
    CHECK(c == 0 && sync != std::string::npos && held != std::string::npos,
          "NOCTURNE: a synchronous sound holds its DRAWFRAME across frames (exit %d)", c);
  }
  for (const char* f : {"rat1.wav", "rat1.mid", "rat2.wav", "rat2.mid", "bungee.wav", "bungee.mid", "nocturne.wav",
                        "nocturne.mid"}) {
    DeleteFileA((dir + "\\" + f).c_str());
  }
  RemoveDirectoryA(dir.c_str());
}

int run_assets(const std::string& exe) {
  if (assets_win().empty()) {
    printf("assets not found: skipped\n");
    return 77;
  }
  // Every host below reads these assets and nothing under the real
  // %LOCALAPPDATA%.
  adw_test::sandbox_spawned_hosts(assets_win(), "ne16");
  // Modules that exercise the kinds of Classic module: the AD 3 engine
  // (TOAST3 on ADXPL300), AD_RSRC + ADTOOL (BUGS), a Borland-built one on
  // plain GDI (RAIN), MSVC plain GDI with palette animation (GEOBOUNC), a
  // Windows 3.0 build with a NAMETABLE and AD_SND sounds over AD_RSRC's
  // backward memmove (DOMINOES), and ADXPL300's (YBYH) — the last two need
  // the adw::cpu DF fix (win16.cpu_df_regression).
  for (const char* m : {"FILES/CLASSIC/TOAST3.AD", "FILES/CLASSIC/BUGS.AD", "FILES/CLASSIC/RAIN.AD",
                        "FILES/CLASSIC/GEOBOUNC.AD", "FILES/CLASSIC/DOMINOES.AD", "FILES/CLASSIC/YBYH.AD"}) {
    std::string e1, e2;
    int c1 = run_host(exe, m, 8, &e1);
    CHECK(c1 == 0, "%s: adhostwin exit %d\n%s", m, c1, e1.c_str());
    if (c1 != 0) continue;
    std::string h1 = hashes(e1);
    CHECK(std::count(h1.begin(), h1.end(), '\n') == 8, "%s: 8 frames hashed", m);
    int c2 = run_host(exe, m, 8, &e2);
    CHECK(c2 == 0 && hashes(e2) == h1, "%s: a second run hashes identically", m);
  }
  // Artist swaps two palettes ~90 times a frame (USER.SelectPalette in a
  // GetTickCount loop) with its stroke pen and brush selected, and each swap
  // re-keys them (win16/gdi16.hh extra_). Each key colour's real object is
  // made once: the host stays at a handful of GDI objects, where it used to
  // reach the 10,000-object quota every ~1.8 s.
  {
    DWORD peak = 0;
    int c = run_host_gdi_peak(exe, "FILES/CLASSIC/ARTIST.AD", 1800, &peak);
    CHECK(c == 0 && peak > 0 && peak < 500, "ARTIST over 1,800 frames: exit %d, peak GDI objects %lu", c, peak);
    printf("ARTIST: peak %lu GDI objects over 1,800 frames\n", peak);
  }
  // ZOT draws each lightning bolt, waits in a CPU delay loop and erases it
  // inside one DRAWFRAME: only the scanout rule (lane.cc on_scanout) lets a
  // frame show one, so its FBHASH stream must not be constant.
  {
    std::string e;
    int c = run_host(exe, "FILES/CLASSIC/ZOT.AD", 300, &e);
    std::string h = hashes(e);
    std::string first = h.substr(0, h.find('\n'));
    first = first.substr(first.rfind(' ') + 1);
    size_t lines = size_t(std::count(h.begin(), h.end(), '\n'));
    size_t same = 0;
    for (size_t p = 0; (p = h.find(first, p)) != std::string::npos; p += first.size()) same++;
    CHECK(c == 0 && lines == 300 && same < lines, "ZOT: a lightning bolt reaches some frame (%zu of %zu frames alike)",
          same, lines);
  }
  // Pacing (lane.hh): a module that reads no clock while drawing (Hard Rain)
  // gets a run of DRAWFRAMEs per presented frame, up to ADMAXDRAWS; ADMIPS=0
  // restores exactly one.
  {
    auto draws = [&](const std::string& extra) -> long {
      std::string e;
      int c = run_host(exe, "FILES/CLASSIC/RAIN.AD", 30, &e, "ADTRACE=lane " + extra);
      size_t p = e.find(" DRAWFRAME calls over 30 frames");
      if (c != 0 || p == std::string::npos) return -1;
      size_t s = e.rfind(' ', p - 1);
      return strtol(e.c_str() + s + 1, nullptr, 10);
    };
    long paced = draws(""), one = draws("ADMIPS=0"), capped = draws("ADMAXDRAWS=4");
    CHECK(paced > 30 * 8, "RAIN: a run of DRAWFRAMEs per frame (%ld over 30 frames)", paced);
    CHECK(one == 30, "RAIN with ADMIPS=0: one DRAWFRAME per frame (%ld)", one);
    CHECK(capped == 30 * 4, "RAIN with ADMAXDRAWS=4: four per frame (%ld)", capped);
  }
  // The desktop seed (ADSEEDIMG): Spotlight shows the screen it found through
  // its spots, so a seeded run differs from a black one; ADNOSEED ignores it.
  {
    std::string eb, es, en;
    int cb = run_host(exe, "FILES/CLASSIC/SPOT.AD", 20, &eb);
    int cs = run_host(exe, "FILES/CLASSIC/SPOT.AD", 20, &es, "ADSEEDIMG=:win95");
    int cn = run_host(exe, "FILES/CLASSIC/SPOT.AD", 20, &en, "ADSEEDIMG=:win95 ADNOSEED=1");
    CHECK(cb == 0 && cs == 0 && cn == 0 && hashes(es) != hashes(eb) && hashes(en) == hashes(eb),
          "SPOT: the seed reaches the frames, ADNOSEED drops it (exit %d/%d/%d)", cb, cs, cn);
  }
  // Small screens (lane.hh): Rat Race refuses a 320x240 display ("A larger
  // screen size is needed…"); the saver's /p preview asks for one, so the
  // guest gets 640x480 and the frames are averaged down — deterministically.
  {
    std::string e1, e2, e0;
    int c1 = run_host(exe, "FILES/CLASSIC/RATRACE.AD", 20, &e1, "ADSCREENW=320 ADSCREENH=240");
    int c2 = run_host(exe, "FILES/CLASSIC/RATRACE.AD", 20, &e2, "ADSCREENW=320 ADSCREENH=240");
    int c0 = run_host(exe, "FILES/CLASSIC/RATRACE.AD", 20, &e0, "ADSCREENW=320 ADSCREENH=240 ADNE16SCALE=1");
    std::string h1 = hashes(e1);
    CHECK(c1 == 0 && c2 == 0 && std::count(h1.begin(), h1.end(), '\n') == 20 && hashes(e2) == h1,
          "RATRACE at 320x240: runs on a 640x480 guest display, deterministic (exit %d/%d)\n%s", c1, c2, e1.c_str());
    CHECK(c0 != 0 && e0.find("larger screen") != std::string::npos,
          "RATRACE at 320x240 with ADNE16SCALE=1: the module refuses (exit %d)", c0);
    CHECK(e1.find("320x240") != std::string::npos, "the host still reports the 320x240 output");
  }
  // Long calls (lane.hh): SATORI's DRAWFRAME draws for about a second, waiting
  // on the tick count; frames end inside it, so its colours move every frame
  // (it was one frame per call: a 60x time-lapse headless, 1 fps streamed).
  {
    std::string e1, e2;
    int c1 = run_host(exe, "FILES/CLASSIC/SATORI.AD", 30, &e1, "ADTRACE=lane");
    int c2 = run_host(exe, "FILES/CLASSIC/SATORI.AD", 30, &e2);
    std::string h1 = hashes(e1);
    std::vector<std::string> v;
    for (size_t p = 0, q; (q = h1.find('\n', p)) != std::string::npos; p = q + 1) v.push_back(h1.substr(h1.rfind(' ', q) + 1, q - h1.rfind(' ', q) - 1));
    std::sort(v.begin(), v.end());
    size_t distinct = size_t(std::unique(v.begin(), v.end()) - v.begin());
    size_t at = e1.find(" ended inside one)");
    long inside = -1;
    if (at != std::string::npos) inside = strtol(e1.c_str() + e1.rfind('(', at) + 1, nullptr, 10);
    CHECK(c1 == 0 && c2 == 0 && distinct >= 25 && inside >= 25 && hashes(e2) == h1,
          "SATORI: frames end inside its long DRAWFRAME (exit %d/%d, %zu distinct of 30, %ld inside, %s)", c1, c2,
          distinct, inside, hashes(e2) == h1 ? "deterministic" : "NOT deterministic");
  }
  // Streamed, the realtime clock only runs from frame 0: EINSTEIN calibrates
  // its delays on the tick count while loading, which spun until the call
  // budget ran out (17 s, "lane init failed") before init time was modeled.
  // AD_RSRC's own load-time calibration (AD_RSRC 1:0110) did the same to the
  // 30 modules built on it (Bogglins, Boris, Mowin' Man, Aquatic Realm, …).
  for (const char* m : {"FILES/CLASSIC/EINSTEIN.AD", "FILES/CLASSIC/BOGGLINS.AD"}) {
    std::string e;
    DWORD t0 = GetTickCount();
    int c = run_host(exe, m, 5, &e, "ADSTREAM=1 ADSTREAMFORCE=1");
    DWORD ms = GetTickCount() - t0;
    CHECK(c == 0 && ms < 10000, "%s streamed: loads and draws (exit %d, %lu ms)\n%s", m, c, (unsigned long)ms,
          c ? e.c_str() : "");
  }
  // The bridge oracle (PACKAGES.md §9): the native AD3 bridge must drive a
  // Classic module exactly as OLDMOD16 does. With ADMIPS=0 neither bridge's
  // own instructions move virtual time, so the streams must be identical —
  // Hard Rain (RAIN) also checks AD_SYSTEM's version and "BUTTHEAD".
  for (const char* m : {"FILES/CLASSIC/TOAST3.AD", "FILES/CLASSIC/RAIN.AD", "FILES/CLASSIC/DOMINOES.AD",
                        "FILES/CLASSIC/GEOBOUNC.AD"}) {
    std::string eo, en;
    int co = run_host(exe, m, 30, &eo, "ADMIPS=0 ADNE16BRIDGE=oldmod16");
    int cn = run_host(exe, m, 30, &en, "ADMIPS=0 ADNE16BRIDGE=native");
    std::string ho = hashes(eo), hn = hashes(en);
    CHECK(co == 0 && cn == 0 && std::count(ho.begin(), ho.end(), '\n') == 30 && ho == hn,
          "%s: native bridge == OLDMOD16 (exit %d/%d)\n%s", m, co, cn, cn ? en.c_str() : "");
  }
  test_sound_assets(exe, assets_win());
  printf("%d/%d checks passed\n", checks - failures, checks);
  return failures ? 1 : 0;
}

// The PACKAGES.md §4.4 roots under AD_NE16_PKGROOTS (<root>\<id>\win\packages\<id>\...):
// one module per package runs standalone, twice, identically, with no
// unimplemented call.
int run_pkg(const std::string& exe) {
  const char* r = getenv("AD_NE16_PKGROOTS");
  if (!r || !*r) {
    printf("AD_NE16_PKGROOTS not set: skipped\n");
    return 77;
  }
  adw_test::sandbox_spawned_hosts("", "ne16pkg");  // each run names its AD_ASSETS_DIR
  struct Case {
    const char* id;
    const char* module;
  };
  int ran = 0;
  for (const Case& c : {Case{"ad10", "packages/ad10/AD10TH/CHAM.AD"}, Case{"ad32", "packages/ad32/AD32/GUTS.AD"},
                        Case{"tt", "packages/tt/TWISTED/CHAM.AD"},
                        Case{"simpsons", "packages/simpsons/SIMPSONS/HOMEREAT.AD"}}) {
    std::string root = std::string(r) + "\\" + c.id;
    if (GetFileAttributesA((root + "\\win\\" + c.module).c_str()) == INVALID_FILE_ATTRIBUTES) {
      printf("%s: absent, skipped\n", c.id);
      continue;
    }
    ran++;
    std::string e1, e2, arg = "\"AD_ASSETS_DIR=" + root + "\"";
    int c1 = run_host(exe, c.module, 30, &e1, arg);
    int c2 = run_host(exe, c.module, 30, &e2, arg);
    std::string h1 = hashes(e1);
    CHECK(c1 == 0 && c2 == 0 && std::count(h1.begin(), h1.end(), '\n') == 30 && hashes(e2) == h1,
          "%s %s: exit %d/%d, deterministic\n%s", c.id, c.module, c1, c2, e1.c_str());
    CHECK(e1.find(" 0 unimplemented") != std::string::npos, "%s: no unimplemented calls", c.module);
  }
  // Bring-up regressions on the package roots (each skipped when its root is absent):
  //  * LOGO counts DRAWFRAME calls (its picture moves every 100th at Medium):
  //    with the pacing run it moves; one call per frame left it frozen.
  //  * INS draws a Windows 3.1 window with LoadBitmap(NULL, OBM_*) about a
  //    minute in; with no system bitmaps it stopped with "Out of memory".
  struct Long {
    const char* id;
    const char* module;
    int frames;
    size_t min_distinct;
  };
  for (const Long& c : {Long{"ad32", "packages/ad32/AD32/LOGO.AD", 120, 20},
                        Long{"simpsons", "packages/simpsons/SIMPSONS/INS.AD", 3600, 100}}) {
    std::string root = std::string(r) + "\\" + c.id;
    if (GetFileAttributesA((root + "\\win\\" + c.module).c_str()) == INVALID_FILE_ATTRIBUTES) continue;
    std::string e, arg = "\"AD_ASSETS_DIR=" + root + "\"";
    int code = run_host(exe, c.module, c.frames, &e, arg);
    std::string h = hashes(e);
    std::vector<std::string> v;
    for (size_t p = 0, q; (q = h.find('\n', p)) != std::string::npos; p = q + 1) v.push_back(h.substr(h.rfind(' ', q) + 1, q - h.rfind(' ', q) - 1));
    std::sort(v.begin(), v.end());
    size_t distinct = size_t(std::unique(v.begin(), v.end()) - v.begin());
    CHECK(code == 0 && std::count(h.begin(), h.end(), '\n') == c.frames && distinct >= c.min_distinct,
          "%s: exit %d, %zu distinct frames of %d\n%s", c.module, code, distinct, c.frames, code ? e.c_str() : "");
  }
  if (!ran) {
    printf("no package roots found: skipped\n");
    return 77;
  }
  printf("%d/%d checks passed\n", checks - failures, checks);
  return failures ? 1 : 0;
}

// ---- interaction (INTERACTION.md §5.2, §6, §7, §9.1) ------------------------------------------------------------

struct Proc {
  int code = -1;
  std::string err, out;
};

// Runs `cmd` with stdin from `input` (NUL when empty); stdout and stderr
// captured, or stdout to NUL (`discard_stdout`: a streamed run's frames).
Proc run_cmd(const std::string& cmd, const std::string& input, bool discard_stdout = false) {
  Proc p;
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  char tmp[MAX_PATH], in_path[MAX_PATH];
  GetTempPathA(MAX_PATH, tmp);
  GetTempFileNameA(tmp, "adi", 0, in_path);
  if (!input.empty()) {
    FILE* f = fopen(in_path, "wb");
    fwrite(input.data(), 1, input.size(), f);
    fclose(f);
  }
  HANDLE in = CreateFileA(input.empty() ? "NUL" : in_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                          OPEN_EXISTING, 0, nullptr);
  HANDLE err_r, err_w, out_r, out_w;
  CreatePipe(&err_r, &err_w, &sa, 0);
  CreatePipe(&out_r, &out_w, &sa, 0);
  SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
  HANDLE nul = discard_stdout ? CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING,
                                            0, nullptr)
                              : nullptr;
  STARTUPINFOA si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = in;
  si.hStdOutput = discard_stdout ? nul : out_w;
  si.hStdError = err_w;
  PROCESS_INFORMATION pi{};
  std::string c = cmd;
  if (CreateProcessA(nullptr, c.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    CloseHandle(err_w);
    CloseHandle(out_w);
    CloseHandle(in);
    // stdout is one short JSON line (configure) or nothing: read stderr to the end, then stdout.
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(err_r, buf, sizeof(buf), &got, nullptr) && got) p.err.append(buf, got);
    while (ReadFile(out_r, buf, sizeof(buf), &got, nullptr) && got) p.out.append(buf, got);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    p.code = int(code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
  } else {
    CloseHandle(err_w);
    CloseHandle(out_w);
    CloseHandle(in);
  }
  if (nul) CloseHandle(nul);
  CloseHandle(err_r);
  CloseHandle(out_r);
  DeleteFileA(in_path);
  return p;
}

std::string gos(int n) {
  std::string s;
  for (int i = 0; i < n; i++) s += "GO\n";
  return s;
}

// STATUS <frame> flags=0x<hex> applied=<n> eaten=<n> src=<s>
struct Status {
  long frame = 0;
  unsigned flags = 0;
  unsigned long long applied = 0, eaten = 0;
};
std::vector<Status> statuses(const std::string& log) {
  std::vector<Status> v;
  size_t p = 0;
  while ((p = log.find("STATUS ", p)) != std::string::npos) {
    Status s;
    if (sscanf(log.c_str() + p, "STATUS %ld flags=0x%x applied=%llu eaten=%llu", &s.frame, &s.flags, &s.applied, &s.eaten) == 4)
      v.push_back(s);
    p += 7;
  }
  return v;
}

std::vector<std::string> hash_list(const std::string& log) {
  std::vector<std::string> v;
  size_t p = 0;
  while ((p = log.find("FBHASH ", p)) != std::string::npos) {
    size_t e = log.find('\n', p);
    std::string line = log.substr(p, e - p);
    v.push_back(line.substr(line.rfind(' ') + 1));
    p = e;
  }
  return v;
}

bool module_present(const std::string& win, const char* rel) {
  std::string p = win + "\\" + rel;
  for (char& ch : p)
    if (ch == '/') ch = '\\';
  return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

void remove_tree(const std::string& dir) {
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      std::string n = fd.cFileName;
      if (n == "." || n == "..") continue;
      std::string p = dir + "\\" + n;
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(p);
      else DeleteFileA(p.c_str());
    } while (FindNextFileA(h, &fd));
    FindClose(h);
  }
  RemoveDirectoryA(dir.c_str());
}

std::string read_file(const std::string& path) {
  std::string s;
  if (FILE* f = fopen(path.c_str(), "rb")) {
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    fclose(f);
  }
  return s;
}

// ---- a module's own folder dialog through [-h-] (win16/dos16.hh dos_chdir) ----

// A scratch folder in %TEMP% named by `tag` and this process's id in hex, a
// name that is its own 8.3 name: its H: path takes no alias that TEMP's
// other entries could shift.
std::string dos_temp_dir(char tag) {
  char base[MAX_PATH], name[16];
  GetTempPathA(MAX_PATH, base);
  snprintf(name, sizeof(name), "%c%07lX", tag, (unsigned long)(GetCurrentProcessId() & 0xFFFFFFF));
  std::string d = std::string(base) + name;
  CreateDirectoryA(d.c_str(), nullptr);
  return d;
}

// The guest path of a host folder, as a spawned host shows it under H: (8.3
// names: win32::Vfs::host_to_guest, the same code).
std::string host_guest(const std::string& host) {
  win32::Vfs v;
  v.mount_host_drives(/*short_names=*/true);
  return v.host_to_guest(host);
}

// A configure script that walks a DlgDirList folder list (`list`) through
// [-h-] down to `guest` (H:\…), a directory at a time: PICK, then OK.
std::string walk_script(const std::string& guest, int list) {
  std::string s = "PICK " + std::to_string(list) + " [-h-]\nCLICK 1\n";
  for (size_t i = 3; i < guest.size();) {
    size_t j = guest.find('\\', i);
    if (j == std::string::npos) j = guest.size();
    s += "PICK " + std::to_string(list) + " [" + guest.substr(i, j - i) + "]\nCLICK 1\n";
    i = j + 1;
  }
  return s;
}

// A key's value in a profile file's text ("" when absent).
std::string profile_value(const std::string& ini, const std::string& key) {
  size_t p = 0;
  while ((p = ini.find(key + "=", p)) != std::string::npos) {
    if (p == 0 || ini[p - 1] == '\n') {
      size_t b = p + key.size() + 1, e = ini.find_first_of("\r\n", b);
      return ini.substr(b, e == std::string::npos ? std::string::npos : e - b);
    }
    p++;
  }
  return {};
}

// Half a second of a 440 Hz tone: 8-bit mono PCM at 22050 Hz, the format of
// Sounder's own JIM.WAV.
std::string tone_wav() {
  std::string d;
  for (int i = 0; i < 11025; i++) d.push_back(char(uint8_t(128 + 100 * sin(2 * 3.14159265358979 * 440 * i / 22050))));
  std::string w = "RIFF";
  auto u32 = [&](uint32_t v) {
    for (int k = 0; k < 4; k++) w.push_back(char(v >> (8 * k)));
  };
  auto u16 = [&](uint16_t v) {
    w.push_back(char(v));
    w.push_back(char(v >> 8));
  };
  u32(uint32_t(36 + d.size()));
  w += "WAVEfmt ";
  u32(16), u16(1), u16(1), u32(22050), u32(22050), u16(1), u16(8);
  w += "data";
  u32(uint32_t(d.size()));
  return w + d;
}

// A made-up map for Globe: 322x156, 4 bits per pixel, the 16 VGA colours in
// bands — the size and depth of Globe's own maps.
std::string map_bmp() {
  const int w = 322, h = 156, stride = ((w * 4 + 31) / 32) * 4;
  static const uint8_t vga[16][3] = {{0, 0, 0},     {128, 0, 0},   {0, 128, 0},   {128, 128, 0},
                                     {0, 0, 128},   {128, 0, 128}, {0, 128, 128}, {192, 192, 192},
                                     {128, 128, 128}, {255, 0, 0}, {0, 255, 0},   {255, 255, 0},
                                     {0, 0, 255},   {255, 0, 255}, {0, 255, 255}, {255, 255, 255}};
  std::string b;
  auto u32 = [&](uint32_t v) {
    for (int k = 0; k < 4; k++) b.push_back(char(v >> (8 * k)));
  };
  auto u16 = [&](uint16_t v) {
    b.push_back(char(v));
    b.push_back(char(v >> 8));
  };
  const uint32_t off = 14 + 40 + 64, size = uint32_t(stride * h);
  b += "BM";
  u32(off + size), u16(0), u16(0), u32(off);
  u32(40), u32(uint32_t(w)), u32(uint32_t(h)), u16(1), u16(4), u32(0), u32(size), u32(0), u32(0), u32(16), u32(16);
  for (const auto& c : vga) {
    b.push_back(char(c[2]));
    b.push_back(char(c[1]));
    b.push_back(char(c[0]));
    b.push_back('\0');
  }
  for (int y = 0; y < h; y++) {
    std::string row(size_t(stride), '\0');
    for (int x = 0; x < w; x++) {
      uint8_t v = uint8_t((x / 20 + y / 12) % 16);
      row[size_t(x / 2)] = char(uint8_t(row[size_t(x / 2)]) | ((x & 1) ? v : uint8_t(v << 4)));
    }
    b += row;
  }
  return b;
}

int run_interaction(const std::string& exe) {
  std::string win = assets_win();
  if (win.empty()) {
    printf("assets not found: skipped\n");
    return 77;
  }
  adw_test::sandbox_spawned_hosts(win, "ne16int");
  const std::string host = "\"" + exe + "\" ";
  // B1: Caps Lock toggles the games in and out of interactive mode (0x0E);
  // HOW2DRAW shows the cursor (0x11); Caps-only modules never go interactive.
  const std::string caps = gos(900) + "KEY 20 1\nCAPS 1\n" + gos(6) + "KEY 20 0\n" + gos(300) + "KEY 20 1\nCAPS 0\n" +
                           gos(6) + "KEY 20 0\n" + gos(60) + "QUIT\n";
  struct Game {
    const char* module;
    bool game, cursor;
  };
  for (const Game& g : {Game{"FILES/CLASSIC/YBYH.AD", true, false}, Game{"packages/simpsons/SIMPSONS/SIMPTRIV.AD", true, false},
                        Game{"packages/tt/TWISTED/FRANKEN.AD", true, false}, Game{"packages/tt/TWISTED/MIMEHUNT.AD", true, false},
                        Game{"packages/simpsons/SIMPSONS/HOW2DRAW.AD", true, true}, Game{"FILES/CLASSIC/TUNNEL.AD", false, false},
                        Game{"FILES/CLASSIC/CONFETTI.AD", false, false}, Game{"packages/tt/TWISTED/TOILET.AD", false, false},
                        Game{"packages/tt/TWISTED/CHAM.AD", false, false}}) {
    if (!module_present(win, g.module)) {
      printf("%s: absent, skipped\n", g.module);
      continue;
    }
    Proc p = run_cmd(host + g.module + " ADSTATUSLOG=1 ADGOWAITMS=5000", caps);
    std::vector<Status> st = statuses(p.err);
    bool on = false, cursor = false, filter = false;
    for (const Status& s : st) {
      on |= (s.flags & 1) != 0;
      cursor |= (s.flags & 2) != 0;
      filter |= (s.flags & 8) != 0;
    }
    bool off_at_end = !st.empty() && !(st.back().flags & 1);
    CHECK(p.code == 0 && on == g.game && off_at_end && (!g.cursor || cursor), "%s: exit %d, interactive %s, off at the end %s%s",
          g.module, p.code, on ? "seen" : "never", off_at_end ? "yes" : "no", g.cursor ? (cursor ? ", cursor" : ", NO cursor") : "");
    printf("%s: key-filter %s\n", g.module, filter ? "raised (a WH_KEYBOARD hook while it plays)" : "never");
  }
  // B2: the Trivia answers reach the WH_KEYBOARD hook, are consumed while
  // interactive, and change the frames.
  for (const char* m : {"FILES/CLASSIC/YBYH.AD", "packages/simpsons/SIMPSONS/SIMPTRIV.AD"}) {
    if (!module_present(win, m)) continue;
    std::string a = gos(900) + "KEY 20 1\nCAPS 1\n" + gos(6) + "KEY 20 0\n", n = a;
    for (int i = 0; i < 25; i++) {
      int k = 49 + i % 3;
      a += gos(118) + "KEY " + std::to_string(k) + " 1\n" + gos(2) + "KEY " + std::to_string(k) + " 0\n";
      n += gos(120);
    }
    a += gos(60) + "QUIT\n";
    n += gos(60) + "QUIT\n";
    Proc pa = run_cmd(host + m + " ADSTATUSLOG=1 ADFBHASH=1 ADGOWAITMS=5000 ADTRACE=input16", a);
    Proc pn = run_cmd(host + m + " ADFBHASH=1 ADGOWAITMS=5000", n);
    std::vector<Status> st = statuses(pa.err);
    bool hooked = pa.err.find("WH_KEYBOARD") != std::string::npos && pa.err.find("vk 31") != std::string::npos;
    bool eaten = !st.empty() && st.back().eaten >= st.back().applied - 1 && st.back().eaten > 50;
    std::vector<std::string> ha = hash_list(pa.err), hn = hash_list(pn.err);
    size_t differ = 0;
    for (size_t i = 0; i < std::min(ha.size(), hn.size()); i++) differ += ha[i] != hn[i];
    CHECK(pa.code == 0 && pn.code == 0 && hooked && eaten && differ > 0 && ha.size() == hn.size(),
          "%s answers: hook %s, eaten %llu of %llu, %zu frames differ", m, hooked ? "yes" : "no",
          st.empty() ? 0ull : st.back().eaten, st.empty() ? 0ull : st.back().applied, differ);
  }
  // B3: Lunatic Fringe finds the blanker window ("Sleep"), reads its queue
  // every frame (key-filter before the first key), eats its keys, plays; it
  // reads LunData.dat from the package's Windows directory (GetWindowsDirectory),
  // seeded from the disc's LUNDATA.DAT as the installers did, so it never says
  // "Configuration File Not Accessible".
  for (const char* m : {"FILES/CLASSIC/LUNATIC.AD", "packages/ad10/AD10TH/LUNATIC.AD"}) {
    if (!module_present(win, m)) continue;
    std::string a = gos(300) + "KEY 20 1\nCAPS 1\n" + gos(6) + "KEY 20 0\n" + gos(120), n = a;
    for (int i = 0; i < 20; i++) {
      for (int k : {37, 38, 32, 75, 74, 32, 39}) {
        a += "KEY " + std::to_string(k) + " 1\n" + gos(10) + "KEY " + std::to_string(k) + " 0\n" + gos(5);
        n += gos(15);
      }
    }
    a += gos(60) + "QUIT\n";
    n += gos(60) + "QUIT\n";
    std::string state = temp_dir("lun");
    Proc pa = run_cmd(host + m + " ADSTATUSLOG=1 ADFBHASH=1 ADGOWAITMS=5000 ADTRACE=input16 \"ADSTATE=" + state + "\"", a);
    Proc pn = run_cmd(host + m + " ADFBHASH=1 ADGOWAITMS=5000", n);
    std::vector<Status> st = statuses(pa.err);
    bool filter_first = false;
    for (const Status& s : st) {
      if (s.applied == 0 && (s.flags & 8)) filter_first = true;
    }
    std::vector<std::string> ha = hash_list(pa.err), hn = hash_list(pn.err);
    size_t differ = 0;
    for (size_t i = 0; i < std::min(ha.size(), hn.size()); i++) differ += ha[i] != hn[i];
    bool found = pa.err.find("FindWindow(\"Sleep\", NULL) -> the saver window") != std::string::npos;
    bool eaten = !st.empty() && st.back().eaten == st.back().applied;
    bool data = pa.err.find("Configuration File Not Accessible") == std::string::npos &&
                pn.err.find("Configuration File Not Accessible") == std::string::npos;
    CHECK(pa.code == 0 && found && filter_first && eaten && differ > 0 && data,
          "%s: FindWindow %s, key-filter before the first key %s, all eaten %s, %zu frames differ, LunData.dat %s", m,
          found ? "yes" : "no", filter_first ? "yes" : "no", eaten ? "yes" : "no", differ, data ? "read" : "NOT ACCESSIBLE");
    remove_tree(state);
  }
  // B7 (INTERACTION.md §9.1): DOS Shell keeps going for five virtual minutes.
  // AD 3.2 ships no OLDMOD16: the native bridge only.
  struct Shell {
    const char* module;
    const char* bridge;
  };
  for (const Shell& s : {Shell{"FILES/CLASSIC/DOSSHELL.AD", "oldmod16"}, Shell{"FILES/CLASSIC/DOSSHELL.AD", "native"},
                         Shell{"packages/ad32/AD32/DOSSHELL.AD", "native"}}) {
    if (!module_present(win, s.module)) continue;
    Proc p = run_cmd(host + s.module + " ADFRAMES=18000 ADFBHASH=1 ADGOWAITMS=0 ADNE16BRIDGE=" + s.bridge, "");
    std::vector<std::string> h = hash_list(p.err);
    std::vector<std::string> late(h.begin() + std::min<size_t>(600, h.size()), h.end());
    std::sort(late.begin(), late.end());
    size_t distinct = size_t(std::unique(late.begin(), late.end()) - late.begin());
    CHECK(p.code == 0 && h.size() == 18000 && distinct > 1, "%s (%s): exit %d, %zu frames, %zu distinct after frame 600",
          s.module, s.bridge, p.code, h.size(), distinct);
  }
  // B5/B6: the module buttons in configure mode, hidden and scripted; the
  // state lands in the package's directories; message modules show their
  // text in a later headless run.
  std::string state = temp_dir("cfg"), empty = temp_dir("empty");
  auto script_file = [&](const char* name, const std::string& text) {
    std::string p = state + "_" + name + ".txt";
    FILE* f = fopen(p.c_str(), "wb");
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
    return p;
  };
  auto configure = [&](const char* m, int slot, const std::string& script, const std::string& st, const std::string& extra) {
    std::string cmd = host + "--configure " + m + " --button " + std::to_string(slot) + " ADCONFIGHIDDEN=1 ADCONFIGTIMEOUTMS=8000 \"ADSTATE=" +
                      st + "\"" + (script.empty() ? "" : " \"ADCONFIGSCRIPT=" + script + "\"") + (extra.empty() ? "" : " " + extra);
    return run_cmd(cmd, "");
  };
  std::string bmp = temp_dir("pics") + "\\A Picture With A Long Name.bmp";
  CopyFileA((win + "\\FILES\\CLASSIC\\BITMAPS\\TOASTVGA.BMP").c_str(), bmp.c_str(), FALSE);
  struct Button {
    const char* module;
    int slot;
    const char* name;
    std::string script;
    int want;             // exit code
    const char* written;  // a state file (relative to the state root), or ""
  };
  std::vector<Button> buttons = {
      {"FILES/CLASSIC/MESSAGE3.AD", 2, "message3", "TEXT 112 HELLO FROM TEST\nCLICK 113\n", 0, "deluxe\\CLASSIC\\MESG_AD3.DAT"},
      {"FILES/CLASSIC/NONSENSE.AD", 3, "nonsense", "", 0, "deluxe\\CLASSIC\\NONSENSE.TXT"},
      {"FILES/CLASSIC/FISHPRO.AD", 2, "fishpro", "CLICK 1001\nCLICK 1003\nCLICK 1\n", 0, "deluxe\\WINDOWS\\MODULES.INI"},
      {"FILES/CLASSIC/BUGS.AD", 1, "bugs", "CLICK 1001\nCLICK 1003\nCLICK 1\n", 0, "deluxe\\WINDOWS\\MODULES.INI"},
      {"packages/ad32/AD32/BUGS.AD", 1, "bugs32", "CLICK 1001\nCLICK 1003\nCLICK 1\n", 0, "ad32\\WINDOWS\\MODULES.INI"},
      {"FILES/CLASSIC/ARTIST.AD", 3, "artist", "FILE " + bmp + "\n", 0, "deluxe\\WINDOWS\\MODULES.INI"},
      {"FILES/CLASSIC/SLIDE.AD", 0, "slide", "CLICK 1\n", 0, "deluxe\\CLASSIC\\BITMAPS.ADC"},
      {"FILES/CLASSIC/GLOBE.AD", 2, "globe", "TEXT 201 EARTH2.BMP\nCLICK 1\n", 0, "deluxe\\WINDOWS\\AD_PREFS.INI"},
      {"packages/ad32/AD32/LOGO.AD", 2, "logo", "FILE " + bmp + "\n", 0, "ad32\\WINDOWS\\MODULES.INI"},
      {"FILES/CLASSIC/WMORPH.AD", 2, "wmorph", "CLICK 1\n", 0, "deluxe\\CLASSIC\\morphclk.dat"},
      {"FILES/CLASSIC/WMORPH.AD", 3, "wmorph_revert", "ANSWER IDYES\n", 0, ""},
      // Custom Configuration, then OK: the keys are written (copied up over the seed).
      {"FILES/CLASSIC/LUNATIC.AD", 1, "lunatic_keys", "CLICK 101\nCLICK 1\n", 0, "deluxe\\WINDOWS\\LunData.dat"},
      {"FILES/CLASSIC/LUNATIC.AD", 0, "lunatic_clear", "ANSWER IDYES\n", 0, ""},
      {"packages/tt/TWISTED/MESSYGES.AD", 3, "messyges", "TEXT 101 HELLO FROM TEST\nCLICK 1\n", 0, "tt\\WINDOWS\\MODULES.INI"},
      // HOW2DRAW's Help is a message box (not WinHelp).
      {"packages/simpsons/SIMPSONS/HOW2DRAW.AD", 2, "how2draw", "ANSWER IDOK\n", 0, ""},
  };
  for (const Button& b : buttons) {
    if (!module_present(win, b.module)) {
      printf("%s: absent, skipped\n", b.module);
      continue;
    }
    std::string sf = b.script.empty() ? std::string() : script_file(b.name, b.script);
    Proc p = configure(b.module, b.slot, sf, state, "");
    bool file = !*b.written || GetFileAttributesA((state + "\\" + b.written).c_str()) != INVALID_FILE_ATTRIBUTES;
    CHECK(p.code == b.want && file && p.out.find("\"result\"") != std::string::npos,
          "configure %s button %d: exit %d (want %d), %s %s\n%s%s", b.module, b.slot, p.code, b.want, b.written,
          file ? "written" : "MISSING", p.out.c_str(), p.code != b.want ? p.err.c_str() : "");
    if (!sf.empty()) DeleteFileA(sf.c_str());
  }
  // The persisted H: path is 8.3.
  std::string ini = read_file(state + "\\deluxe\\WINDOWS\\MODULES.INI");
  size_t at = ini.find("Image=H:\\");
  CHECK(at != std::string::npos && ini.find("~1.BMP", at) != std::string::npos, "ARTIST keeps a short H: path\n%s", ini.c_str());
  // NONSENSE's word list, as Notepad would have left it: names of our own
  // only (its first sentences have none; one appears within 30 s).
  std::string nonsense = state + "\\deluxe\\CLASSIC\\NONSENSE.TXT";
  if (FILE* f = fopen(nonsense.c_str(), "wb")) {
    fputs("Zyxwv Quibblethorpe\r\nQuibble Zyxwv\r\nXerxes Blorp\r\n", f);
    fclose(f);
  }
  struct Later {
    const char* module;
    const char* args;
    int frames;
  };
  for (const Later& l : {Later{"FILES/CLASSIC/MESSAGE3.AD", "", 600}, Later{"packages/tt/TWISTED/MESSYGES.AD", "ADCVSET=2=0", 600},
                         Later{"FILES/CLASSIC/NONSENSE.AD", "", 1800}}) {
    if (!module_present(win, l.module)) continue;
    std::string base = host + l.module + " ADFRAMES=" + std::to_string(l.frames) + " ADFBHASH=1 ADGOWAITMS=0 " + l.args;
    Proc with = run_cmd(base + " \"ADSTATE=" + state + "\"", "");
    Proc without = run_cmd(base + " \"ADSTATE=" + empty + "\"", "");
    std::vector<std::string> hw = hash_list(with.err), ho = hash_list(without.err);
    size_t differ = 0;
    for (size_t i = 0; i < std::min(hw.size(), ho.size()); i++) differ += hw[i] != ho[i];
    CHECK(with.code == 0 && without.code == 0 && hw.size() == size_t(l.frames) && differ > 0,
          "%s: a later run shows the saved text (%zu of %d frames differ)", l.module, differ, l.frames);
  }
  // Globe's "Map..." in both releases that have it: through [-h-] (its
  // folder list, 204) down to a folder of the host's own holding a made-up
  // map, the map picked (its file list, 202), OK. The guest's DOS is on H:
  // there (INT 21h AH=19h), so GlobeFile names the map drive and all (before,
  // C:\… — "Could not find selected bitmap" in every later run), and a later
  // run with that state loads it: the globe wears it (its frames differ from
  // a run without).
  {
    const std::string maps = dos_temp_dir('M');
    write_file(maps + "\\MAP.BMP", map_bmp());
    const std::string g = host_guest(maps);
    struct Globe {
      const char* module;
      const char* pkg;
    };
    for (const Globe& gl : {Globe{"FILES/CLASSIC/GLOBE.AD", "deluxe"}, Globe{"packages/ad32/AD32/GLOBE.AD", "ad32"}}) {
      if (!module_present(win, gl.module)) {
        printf("%s: absent, skipped\n", gl.module);
        continue;
      }
      if (g.rfind("H:\\", 0) != 0 || g.size() > win16::kMaxCurDir) {
        printf("%s through [-h-]: %s is not on H: within DOS's current directory: skipped\n", gl.module, g.c_str());
        continue;
      }
      const std::string gstate = temp_dir("globeh");
      const std::string sf = script_file("globeh", walk_script(g, 204) + "PICK 202 MAP.BMP\nCLICK 1\n");
      Proc p = configure(gl.module, 2, sf, gstate, "");
      DeleteFileA(sf.c_str());
      const std::string file = profile_value(read_file(gstate + "\\" + gl.pkg + "\\WINDOWS\\AD_PREFS.INI"), "GlobeFile");
      CHECK(p.code == 0 && file == g + "\\MAP.BMP", "%s: the map picked through [-h-] saved as %s (want %s\\MAP.BMP; exit %d)\n%s",
            gl.module, file.c_str(), g.c_str(), p.code, p.code ? p.err.c_str() : "");
      const std::string base = host + gl.module + " ADFRAMES=300 ADFBHASH=1 ADGOWAITMS=0";
      Proc with = run_cmd(base + " ADTRACE=file16 \"ADSTATE=" + gstate + "\"", "");
      Proc without = run_cmd(base + " \"ADSTATE=" + empty + "\"", "");
      std::vector<std::string> hw = hash_list(with.err), ho = hash_list(without.err);
      size_t differ = 0;
      for (size_t i = 0; i < std::min(hw.size(), ho.size()); i++) differ += hw[i] != ho[i];
      CHECK(with.code == 0 && without.code == 0 && hw.size() == 300 && differ > 0 &&
                with.err.find("open " + g + "\\MAP.BMP") != std::string::npos,
            "%s: a later run loads the map from H: (exit %d, %zu frames, %zu differ)\n%s", gl.module, with.code, hw.size(),
            differ, with.code ? with.err.c_str() : "");
      remove_tree(gstate);
    }
    remove_tree(maps);
  }
  // A headless run writes nothing into the state it reads.
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA((empty + "\\*").c_str(), &fd);
  int entries = 0;
  if (h != INVALID_HANDLE_VALUE) {
    do entries += std::string(fd.cFileName) != "." && std::string(fd.cFileName) != "..";
    while (FindNextFileA(h, &fd));
    FindClose(h);
  }
  CHECK(entries == 0, "the headless runs wrote nothing to their ADSTATE (%d entries)", entries);
  // B6: the native bridge's button path writes what OLDMOD16's does.
  if (module_present(win, "FILES/CLASSIC/MESSAGE3.AD")) {
    std::string so = temp_dir("oracle_o"), sn = temp_dir("oracle_n");
    std::string sf = script_file("oracle", "TEXT 112 HELLO FROM TEST\nCLICK 113\n");
    Proc po = configure("FILES/CLASSIC/MESSAGE3.AD", 2, sf, so, "ADNE16BRIDGE=oldmod16");
    Proc pn = configure("FILES/CLASSIC/MESSAGE3.AD", 2, sf, sn, "ADNE16BRIDGE=native");
    std::string fo = read_file(so + "\\deluxe\\CLASSIC\\MESG_AD3.DAT"), fn = read_file(sn + "\\deluxe\\CLASSIC\\MESG_AD3.DAT");
    CHECK(po.code == 0 && pn.code == 0 && !fo.empty() && fo == fn && fo.find("HELLO FROM TEST") != std::string::npos,
          "MESSAGE3's state: native == OLDMOD16 (%zu / %zu bytes)", fo.size(), fn.size());
    DeleteFileA(sf.c_str());
    remove_tree(so);
    remove_tree(sn);
  }
  remove_tree(state);
  remove_tree(empty);
  remove_tree(bmp.substr(0, bmp.find_last_of('\\')));
  printf("%d/%d checks passed\n", checks - failures, checks);
  return failures ? 1 : 0;
}

// ---- Star Wars Screen Entertainment (lane.hh "Intermission (IMX)") ---------------------------------------------
//
// The imported package: AD_NE16_SWSE_ROOT (an assets root, <root>\win\packages\swse\…), else AD_ASSETS_DIR or
// the installed assets; exit 77 when none holds it.
std::string swse_root() {
  std::vector<std::string> roots;
  if (const char* r = getenv("AD_NE16_SWSE_ROOT"); r && *r) roots.push_back(r);
  if (const char* a = getenv("AD_ASSETS_DIR"); a && *a) roots.push_back(a);
  if (roots.empty()) roots.push_back(adw_test::installed_assets_root());
  for (const std::string& r : roots) {
    if (!r.empty() && GetFileAttributesA((r + "\\win\\packages\\swse\\SAVER\\VADER.IMX").c_str()) != INVALID_FILE_ATTRIBUTES)
      return r;
  }
  return {};
}

size_t distinct_of(const std::vector<std::string>& h, size_t n) {
  std::vector<std::string> v(h.begin(), h.begin() + std::min(n, h.size()));
  std::sort(v.begin(), v.end());
  return size_t(std::unique(v.begin(), v.end()) - v.begin());
}

int run_swse(const std::string& exe) {
  const std::string root = swse_root();
  if (root.empty()) {
    printf("no imported Star Wars Screen Entertainment: skipped\n");
    return 77;
  }
  adw_test::sandbox_spawned_hosts(root, "ne16swse");
  const std::string host = "\"" + exe + "\" ";
  // Every module: 300 frames twice, identical, nothing unimplemented; the
  // animated ones move (past their title cards); VADER's frame 119 is his
  // face, not the title card.
  struct Mod {
    const char* name;
    size_t min_distinct;  // by frame 300
  };
  const Mod mods[] = {{"BATTLES", 10}, {"BIOS", 10},     {"BLUPRINT", 10}, {"CANTINA", 10}, {"HYPERSPC", 10},
                      {"ICLOCK", 10},  {"JAWAS", 10},    {"POSTERS", 2},   {"RCLOCK", 2},   {"SABRDUEL", 2},
                      {"STORYBRD", 10}, {"SWTEXT", 10},  {"TRENCH", 10},   {"VADER", 2}};
  for (const Mod& m : mods) {
    std::string path = std::string("packages/swse/SAVER/") + m.name + ".IMX";
    Proc a = run_cmd(host + path + " ADFRAMES=300 ADFBHASH=1 ADGOWAITMS=0", "");
    Proc b = run_cmd(host + path + " ADFRAMES=300 ADFBHASH=1 ADGOWAITMS=0", "");
    std::vector<std::string> ha = hash_list(a.err), hb = hash_list(b.err);
    size_t d = distinct_of(ha, 300);
    CHECK(a.code == 0 && b.code == 0 && ha.size() == 300 && ha == hb, "%s: exit %d/%d, %zu frames, %s\n%s", m.name, a.code,
          b.code, ha.size(), ha == hb ? "deterministic" : "NOT deterministic", a.code ? a.err.c_str() : "");
    CHECK(a.err.find(" 0 unimplemented") != std::string::npos, "%s: no unimplemented call", m.name);
    CHECK(d >= m.min_distinct, "%s: %zu distinct frames of 300 (at least %zu)", m.name, d, m.min_distinct);
    if (std::string(m.name) == "VADER") CHECK(ha.size() == 300 && ha[119] != ha[0], "VADER: frame 119 is not the title card");
  }
  // Keys the saver lets through without waking (Shift at frame 5; a key-up
  // alone at frame 1, the Enter of a Preview started from the keyboard) reach
  // an Intermission module's key state only: START's loading is not dropped,
  // and BATTLES and VADER play exactly as with no input (lane.hh
  // "Intermission (IMX)").
  {
    auto input = [](const std::vector<std::pair<int, std::string>>& events) {
      std::string s;
      for (int f = 0; f < 300; f++) {
        for (const auto& [at, line] : events) {
          if (at == f) s += line + "\n";
        }
        s += "GO\n";
      }
      return s + "QUIT\n";
    };
    for (const char* m : {"BATTLES", "VADER"}) {
      const std::string cmd = host + "packages/swse/SAVER/" + m + ".IMX ADFBHASH=1 ADGOWAITMS=0";
      std::vector<std::string> none = hash_list(run_cmd(cmd, input({})).err);
      std::vector<std::string> shift = hash_list(run_cmd(cmd, input({{5, "KEY 16 1"}, {6, "KEY 16 0"}})).err);
      std::vector<std::string> up = hash_list(run_cmd(cmd, input({{1, "KEY 13 0"}})).err);
      CHECK(none.size() == 300 && shift == none && up == none && none[150] != none[0],
            "%s: Shift at frame 5 %s, a lone key-up at frame 1 %s the stream without input (%zu frames)", m,
            shift == none ? "gives" : "does NOT give", up == none ? "gives" : "does NOT give", none.size());
    }
  }
  // The modules that step once per pass run at the 25-MIPS budget's rate,
  // their overruns carried, a pixel of their blits costing 4 (lane.hh
  // "Pacing"): SWTEXT about 19 passes a virtual second (3.13 budgets a pass),
  // TRENCH about 16 once past its START (3.77); ADNE16IMXPIXCOST=2 gives
  // After Dark's cost (SWTEXT about 35) whatever ADPIXCOST says, and
  // ADNE16IMXCARRY=0 the old 60.
  {
    auto rate = [&](const char* m, int frames, int from, const char* extra) {
      Proc p = run_cmd(host + "packages/swse/SAVER/" + m + ".IMX ADFRAMES=" + std::to_string(frames) +
                           " ADGOWAITMS=0 ADTRACE=pace " + extra,
                       "");
      long calls = 0, counted = 0;
      for (size_t at = 0; (at = p.err.find("[pace] frame ", at)) != std::string::npos; at++) {
        long f = 0, n = 0;
        if (sscanf(p.err.c_str() + at, "[pace] frame %ld: %ld DRAWFRAME", &f, &n) == 2 && f >= from) {
          calls += n;
          counted++;
        }
      }
      return p.code == 0 && counted ? double(calls) * 60.0 / double(counted) : 0.0;
    };
    const double swtext = rate("SWTEXT", 600, 120, ""), trench = rate("TRENCH", 700, 300, "");
    const double swtext_ad = rate("SWTEXT", 600, 120, "ADNE16IMXPIXCOST=2 ADPIXCOST=9");
    const double swtext_off = rate("SWTEXT", 300, 120, "ADNE16IMXCARRY=0");
    CHECK(swtext >= 18 && swtext <= 20.5 && trench >= 15 && trench <= 17 && swtext_ad >= 33 &&
              swtext_ad <= 37 && swtext_off == 60,
          "passes a virtual second: SWTEXT %.1f, TRENCH %.1f; SWTEXT at After Dark's pixel cost %.1f, without the carry %.1f",
          swtext, trench, swtext_ad, swtext_off);
  }
  // The reader oracle: IMIMXPLY.IMQ and the native reader drive a module
  // identically (ADMIPS=0: neither reader's own instructions move time).
  for (const char* m : {"BATTLES", "HYPERSPC", "SWTEXT", "VADER"}) {
    std::string path = std::string("packages/swse/SAVER/") + m + ".IMX";
    Proc q = run_cmd(host + path + " ADFRAMES=300 ADFBHASH=1 ADGOWAITMS=0 ADMIPS=0 ADNE16READER=imq ADTRACE=lane", "");
    Proc n = run_cmd(host + path + " ADFRAMES=300 ADFBHASH=1 ADGOWAITMS=0 ADMIPS=0 ADNE16READER=native ADTRACE=lane", "");
    std::vector<std::string> hq = hash_list(q.err), hn = hash_list(n.err);
    CHECK(q.code == 0 && n.code == 0 && hq.size() == 300 && hq == hn && q.err.find("reader imq (ADNE16READER)") != std::string::npos &&
              n.err.find("reader native (ADNE16READER)") != std::string::npos,
          "%s: native reader == IMIMXPLY.IMQ (exit %d/%d)", m, q.code, n.code);
  }
  // The reader itself is no module; a small screen runs on a 640x480 guest
  // display, deterministically; the desktop seed shows through SABRDUEL (its
  // duel plays over the desktop unless "Blank Background" is on).
  {
    Proc r = run_cmd(host + "packages/swse/ENGINE/IMIMXPLY.IMQ ADFRAMES=2 ADGOWAITMS=0", "");
    CHECK(r.code == 1 && r.err.find("an Intermission reader") != std::string::npos, "IMIMXPLY.IMQ as a module: refused (exit %d)",
          r.code);
    Proc s1 = run_cmd(host + "packages/swse/SAVER/BATTLES.IMX ADFRAMES=120 ADFBHASH=1 ADGOWAITMS=0 ADSCREENW=320 ADSCREENH=240", "");
    Proc s2 = run_cmd(host + "packages/swse/SAVER/BATTLES.IMX ADFRAMES=120 ADFBHASH=1 ADGOWAITMS=0 ADSCREENW=320 ADSCREENH=240", "");
    CHECK(s1.code == 0 && s2.code == 0 && hash_list(s1.err).size() == 120 && hash_list(s1.err) == hash_list(s2.err) &&
              distinct_of(hash_list(s1.err), 120) > 10,
          "BATTLES at 320x240: exit %d/%d, deterministic", s1.code, s2.code);
    Proc b = run_cmd(host + "packages/swse/SAVER/SABRDUEL.IMX ADFRAMES=120 ADFBHASH=1 ADGOWAITMS=0", "");
    Proc d = run_cmd(host + "packages/swse/SAVER/SABRDUEL.IMX ADFRAMES=120 ADFBHASH=1 ADGOWAITMS=0 ADSEEDIMG=:win95", "");
    std::vector<std::string> hb = hash_list(b.err), hd = hash_list(d.err);
    CHECK(b.code == 0 && d.code == 0 && hb.size() == 120 && hd.size() == 120 && hb.back() != hd.back(),
          "SABRDUEL: the desktop seed reaches its frames (exit %d/%d)", b.code, d.code);
  }
  // Streamed (frames to NUL): SWSE's loading and calibration end, frames come (under 10 s).
  {
    std::string e;
    DWORD t0 = GetTickCount();
    int c = run_host(exe, "packages/swse/SAVER/TRENCH.IMX", 5, &e, "ADSTREAM=1 ADSTREAMFORCE=1");
    DWORD ms = GetTickCount() - t0;
    CHECK(c == 0 && ms < 10000, "TRENCH streamed: exit %d, %lu ms\n%s", c, (unsigned long)ms, c ? e.c_str() : "");
  }
  // Sound: BATTLES' effects through sndPlaySound (SWSFX.DLL), at Intermission's
  // volume; nothing with sound off (ANTSW.INI Volume = 0).
  {
    std::string dir = temp_dir("swsesnd"), w1 = dir + "\\battles1.wav", w2 = dir + "\\battles2.wav";
    Proc c1 = run_cmd(host + "packages/swse/SAVER/BATTLES.IMX ADFRAMES=900 ADFBHASH=1 ADGOWAITMS=0 ADTRACE=sound,lane \"ADAUDIOOUT=" + w1 + "\"", "");
    Proc c2 = run_cmd(host + "packages/swse/SAVER/BATTLES.IMX ADFRAMES=900 ADFBHASH=1 ADGOWAITMS=0 \"ADAUDIOOUT=" + w2 + "\"", "");
    std::string a1 = read_file(w1), a2 = read_file(w2);
    size_t voices = 0;
    for (size_t p = 0; (p = c1.err.find(": voice ", p)) != std::string::npos; p++) voices++;
    CHECK(c1.code == 0 && c2.code == 0 && voices >= 5 && a1.size() > 44 && a1 == a2 &&
              c1.err.find("ANTSW.INI Volume=50") != std::string::npos && hash_list(c1.err) == hash_list(c2.err),
          "BATTLES with sound: %zu voices, captures identical, Volume=50 (exit %d/%d)", voices, c1.code, c2.code);
    Proc off = run_cmd(host + "packages/swse/SAVER/BATTLES.IMX ADFRAMES=300 ADGOWAITMS=0 ADTRACE=sound,lane", "");
    CHECK(off.code == 0 && off.err.find("ANTSW.INI Volume=0") != std::string::npos && off.err.find(": voice ") == std::string::npos,
          "BATTLES with sound off: Volume=0, no voice");
    // Its music (MEMMIDI's BATTLE.MID) at the saver's volume too: the MIDI bus
    // follows it, so every channel volume (CC7) at 50 is half of the song's own
    // at 100 (lane.hh "Sound"), and the notes are the same.
    const std::string w3 = dir + "\\battles3.wav";
    Proc c3 = run_cmd(host + "packages/swse/SAVER/BATTLES.IMX ADFRAMES=900 ADGOWAITMS=0 ADVOLUME=100 \"ADAUDIOOUT=" + w3 + "\"", "");
    auto music = [](const std::string& mid, std::vector<uint64_t>* ons) {
      int top = -1;
      for (const auto& [ms, m] : mid_log(mid)) {
        if (m.size() == 3 && (m[0] & 0xF0) == 0xB0 && m[1] == 7) top = std::max(top, int(m[2]));
        if (m.size() == 3 && (m[0] & 0xF0) == 0x90 && m[2]) ons->push_back((ms << 16) | (uint64_t(m[0]) << 8) | m[1]);
      }
      return top;
    };
    std::vector<uint64_t> ons50, ons100;
    const int top50 = music(dir + "\\battles1.mid", &ons50), top100 = music(dir + "\\battles3.mid", &ons100);
    CHECK(c3.code == 0 && top100 >= 100 && top50 == (top100 + 1) / 2 && ons50.size() > 100 && ons50 == ons100,
          "BATTLE.MID's channel volumes: at most %d at Volume 50, %d at 100; %zu/%zu note-ons alike", top50, top100,
          ons50.size(), ons100.size());
    for (const char* f : {"battles1.wav", "battles1.mid", "battles2.wav", "battles2.mid", "battles3.wav", "battles3.mid"})
      DeleteFileA((dir + "\\" + f).c_str());
    RemoveDirectoryA(dir.c_str());
  }
  // Configure: VADER's button 0 opens its dialog (hidden, OK clicked by the
  // script), which writes SWSE.INI into the package's state; button 1 fails.
  {
    std::string state = temp_dir("swsecfg"), script = state + "_ok.txt";
    write_file(script, "CLICK 1\n");
    Proc p = run_cmd(host + "--configure packages/swse/SAVER/VADER.IMX --button 0 ADCONFIGHIDDEN=1 ADCONFIGTIMEOUTMS=8000 \"ADSTATE=" +
                         state + "\" \"ADCONFIGSCRIPT=" + script + "\"",
                     "");
    std::string ini = read_file(state + "\\swse\\WINDOWS\\SWSE.INI");
    CHECK(p.code == 0 && p.out.find("\"result\":\"ok\"") != std::string::npos && p.out.find("C:\\\\WINDOWS\\\\SWSE.INI") != std::string::npos &&
              ini.find("[Darth Vader]") != std::string::npos && ini.find("[technology]") == std::string::npos,
          "VADER button 0: shown, SWSE.INI written without the seeds (exit %d)\n%s%s", p.code, p.out.c_str(), p.code ? p.err.c_str() : "");
    // STRESS's temporary file (GetTempFileName, deleted again) is not reported as written.
    CHECK(p.out.find("TEMP") == std::string::npos && p.out.find(".TMP") == std::string::npos,
          "VADER button 0: 'written' holds only what is still there\n%s", p.out.c_str());
    // Clicks where a user's land (PRESS): the dialog's ANT3DBOX frames lie
    // over its check boxes and answer WM_NCHITTEST with HTTRANSPARENT, so the
    // clicks reach Voice (100) and Breath (104) under them, and OK saves them
    // off. (The frames took every click before: SWSE.INI kept 1 and 1.)
    write_file(script, "PRESS 100\nPRESS 104\nCLICK 1\n");
    Proc pc = run_cmd(host + "--configure packages/swse/SAVER/VADER.IMX --button 0 ADCONFIGHIDDEN=1 ADCONFIGTIMEOUTMS=8000 \"ADSTATE=" +
                          state + "\" \"ADCONFIGSCRIPT=" + script + "\"",
                      "");
    std::string clicked = read_file(state + "\\swse\\WINDOWS\\SWSE.INI");
    CHECK(pc.code == 0 && profile_value(ini, "Voice") == "1" && profile_value(ini, "Breath") == "1" &&
              profile_value(clicked, "Voice") == "0" && profile_value(clicked, "Breath") == "0" &&
              pc.err.find("lands on") == std::string::npos,
          "VADER button 0, its check boxes clicked under their frames: Voice %s -> %s, Breath %s -> %s (exit %d)\n%s",
          profile_value(ini, "Voice").c_str(), profile_value(clicked, "Voice").c_str(), profile_value(ini, "Breath").c_str(),
          profile_value(clicked, "Breath").c_str(), pc.code, pc.err.c_str());
    Proc q = run_cmd(host + "--configure packages/swse/SAVER/VADER.IMX --button 1 ADCONFIGHIDDEN=1 \"ADSTATE=" + state + "\"", "");
    CHECK(q.code == 1 && q.out.find("control 1 is not a button") != std::string::npos, "VADER button 1: fails (exit %d)", q.code);
    DeleteFileA(script.c_str());
    remove_tree(state);
  }
  printf("%d/%d checks passed\n", checks - failures, checks);
  return failures ? 1 : 0;
}

// ---- Star Trek: The Screen Saver (After Dark 2.0; lane.hh "The AD3 protocol") ----------------------------------
//
// The imported package: AD_NE16_STARTREK_ROOT (an assets root, <root>\win\packages\startrek\…), else
// AD_ASSETS_DIR or the installed assets; exit 77 when none holds it.
std::string startrek_root() {
  std::vector<std::string> roots;
  if (const char* r = getenv("AD_NE16_STARTREK_ROOT"); r && *r) roots.push_back(r);
  if (const char* a = getenv("AD_ASSETS_DIR"); a && *a) roots.push_back(a);
  if (roots.empty()) roots.push_back(adw_test::installed_assets_root());
  for (const std::string& r : roots) {
    if (!r.empty() &&
        GetFileAttributesA((r + "\\win\\packages\\startrek\\AFTERDRK\\FINAL.AD").c_str()) != INVALID_FILE_ATTRIBUTES)
      return r;
  }
  return {};
}

// The highest magnitude of a 16-bit PCM capture's samples (0: silence, or no capture).
int wav_peak(const std::string& wav) {
  size_t d = wav.find("data");
  if (d == std::string::npos || d + 8 > wav.size()) return 0;
  const uint32_t n = uint32_t(uint8_t(wav[d + 4])) | (uint32_t(uint8_t(wav[d + 5])) << 8) |
                     (uint32_t(uint8_t(wav[d + 6])) << 16) | (uint32_t(uint8_t(wav[d + 7])) << 24);
  const size_t end = std::min(wav.size(), d + 8 + size_t(n));
  int peak = 0;
  for (size_t i = d + 8; i + 1 < end; i += 2) peak = std::max(peak, std::abs(int(int16_t(uint8_t(wav[i]) | (uint8_t(wav[i + 1]) << 8)))));
  return peak;
}

// The log's line that starts with `head`, or "".
std::string line_starting(const std::string& log, const std::string& head) {
  size_t p = log.find(head);
  if (p == std::string::npos) return {};
  return log.substr(p, log.find('\n', p) - p);
}

int run_startrek(const std::string& exe) {
  const std::string root = startrek_root();
  if (root.empty()) {
    printf("no imported Star Trek: The Screen Saver: skipped\n");
    return 77;
  }
  adw_test::sandbox_spawned_hosts(root, "ne16st");
  const std::string host = "\"" + exe + "\" ", dir = "packages/startrek/AFTERDRK/";
  // Every module: 900 frames twice, identical, nothing unimplemented — AD_SND
  // 1.0 through the native bridge, AD_PREFS.INI's seeds, the computed palettes —;
  // and sound captured twice (ADAUDIOOUT, no device), identical, heard from
  // every module that sounds within 15 s (Ion Storm has no sound, Space's one
  // comes after minutes). Sounder draws nothing: it plays JIM.WAV.
  struct Mod {
    const char* name;
    size_t min_distinct;  // of 900
    bool sounds;
  };
  const Mod mods[] = {{"BRAINCEL", 50, true}, {"COMMS", 40, true},    {"FINAL", 300, true},   {"FRONTIER", 300, true},
                      {"HORTA", 100, true},   {"IONSTORM", 300, false}, {"MISSION", 60, true}, {"PANELS", 200, true},
                      {"PLANETS", 100, true}, {"SCOTTYS", 40, true},  {"SICKBAY", 100, true}, {"SOUNDER", 1, true},
                      {"SPACE", 100, false},  {"SPOCK", 60, true},    {"THOLIAN", 100, true}, {"TRIBBLE", 40, true}};
  const std::string snd = temp_dir("stsnd");
  for (const Mod& m : mods) {
    const std::string cmd = host + dir + m.name + ".AD ADFRAMES=900 ADFBHASH=1 ADGOWAITMS=0";
    Proc a = run_cmd(cmd + " ADTRACE=lane", ""), b = run_cmd(cmd, "");
    std::vector<std::string> ha = hash_list(a.err), hb = hash_list(b.err);
    CHECK(a.code == 0 && b.code == 0 && ha.size() == 900 && ha == hb, "%s: exit %d/%d, %zu frames, %s\n%s", m.name, a.code,
          b.code, ha.size(), ha == hb ? "deterministic" : "NOT deterministic", a.code ? a.err.c_str() : "");
    CHECK(a.err.find(" 0 unimplemented") != std::string::npos, "%s: no unimplemented call", m.name);
    CHECK(distinct_of(ha, 900) >= m.min_distinct, "%s: %zu distinct frames of 900 (at least %zu)", m.name,
          distinct_of(ha, 900), m.min_distinct);
    CHECK(a.err.find("seeds: AD_PREFS.INI [After Dark] Path, [Sound] SoundDriver=AD_MME.DRV") != std::string::npos &&
              a.err.find("palettes After Dark 2.0's four, computed") != std::string::npos &&
              a.err.find("no AD palettes") == std::string::npos,
          "%s: After Dark 2.0's seeds and computed palettes", m.name);
    const std::string w1 = snd + "\\" + m.name + "1.wav", w2 = snd + "\\" + m.name + "2.wav";
    Proc s1 = run_cmd(cmd + " ADAUDIOLIVE=0 ADTRACE=sound \"ADAUDIOOUT=" + w1 + "\"", "");
    Proc s2 = run_cmd(cmd + " ADAUDIOLIVE=0 \"ADAUDIOOUT=" + w2 + "\"", "");
    const std::string c1 = read_file(w1), c2 = read_file(w2);
    size_t voices = 0;
    for (size_t p = 0; (p = s1.err.find(": voice ", p)) != std::string::npos; p++) voices++;
    const int peak = wav_peak(c1);
    std::vector<std::string> h1 = hash_list(s1.err), h2 = hash_list(s2.err);
    CHECK(s1.code == 0 && s2.code == 0 && c1.size() > 44 && c1 == c2 && h1.size() == 900 && h1 == h2,
          "%s with sound: exit %d/%d, captures %s (%zu bytes), streams %s", m.name, s1.code, s2.code,
          c1 == c2 ? "identical" : "DIFFER", c1.size(), h1 == h2 ? "identical" : "DIFFER");
    CHECK(!m.sounds || (voices > 0 && peak > 328), "%s: heard (%zu voices, peak %d, above -40 dBFS)", m.name, voices, peak);
    printf("%s: %zu distinct frames, %zu voices, peak %d\n", m.name, distinct_of(ha, 900), voices, peak);
    for (const std::string& w : {w1, w2}) {
      DeleteFileA(w.c_str());
      DeleteFileA((w.substr(0, w.size() - 4) + ".mid").c_str());
    }
  }
  RemoveDirectoryA(snd.c_str());
  // Scotty's Files draws its blueprints through masks it makes by stretching
  // 1-bpp DIBs, white on black, into monochrome bitmaps (win16/gdi16.cc): by
  // frame 300 the Klingon battle cruiser is on the screen, in a colour of its
  // own, beside the yellow title and the red labels (without the masks only
  // those three colours show). ADOUT writes one frame: the others' names are
  // taken by directories.
  {
    const std::string out = temp_dir("stscotty");
    for (int i = 0; i < 300; i++) {
      char n[32];
      snprintf(n, sizeof(n), "\\frame_%05d.ppm", i);
      CreateDirectoryA((out + n).c_str(), nullptr);
    }
    Proc p = run_cmd(host + dir + "SCOTTYS.AD ADFRAMES=301 ADGOWAITMS=0 \"ADOUT=" + out + "\"", "");
    const std::string ppm = read_file(out + "\\frame_00300.ppm");
    std::map<uint32_t, size_t> colours;
    size_t at = 0;
    for (int fields = 0; fields < 4 && at < ppm.size(); at++) {  // "P6", width, height, maxval
      if (isspace(uint8_t(ppm[at])) && (at == 0 || !isspace(uint8_t(ppm[at - 1])))) fields++;
    }
    for (size_t i = at; i + 2 < ppm.size(); i += 3)
      colours[(uint32_t(uint8_t(ppm[i])) << 16) | (uint32_t(uint8_t(ppm[i + 1])) << 8) | uint8_t(ppm[i + 2])]++;
    size_t other = 0;
    for (const auto& [rgb, n] : colours) {
      if (rgb != 0x000000 && rgb != 0xFFFF00 && rgb != 0xFF0000) other += n;
    }
    CHECK(p.code == 0 && ppm.size() > 640 * 480 * 3 && other > 1000,
          "SCOTTYS frame 300: %zu pixels besides black, yellow and red (the blueprint), %zu colours", other, colours.size());
    for (int i = 0; i < 300; i++) {
      char n[32];
      snprintf(n, sizeof(n), "\\frame_%05d.ppm", i);
      RemoveDirectoryA((out + n).c_str());
    }
    DeleteFileA((out + "\\frame_00300.ppm").c_str());
    RemoveDirectoryA(out.c_str());
  }
  // The buttons, in configure mode (hidden and scripted; the state in a temp
  // ADSTATE): Communications' "Edit Custom..." (3) saves the custom message,
  // without the seeds, and a later run with Message = Custom (ADCVSET=2=9)
  // types it; Sounder's "Sounds.." (2) saves its folder, and its Directories
  // list reaches the host's drives: [-h-] lists H:\.
  {
    const std::string state = temp_dir("stcfg"), empty = temp_dir("stempty"), hstate = temp_dir("stcfgh");
    auto configure = [&](const char* m, int slot, const std::string& script, const std::string& st) {
      const std::string sf = state + "_" + m + ".txt";
      write_file(sf, script);
      Proc p = run_cmd(host + "--configure " + dir + m + ".AD --button " + std::to_string(slot) +
                           " ADCONFIGHIDDEN=1 ADCONFIGTIMEOUTMS=8000 ADTRACE=dlg16 \"ADSTATE=" + st + "\" \"ADCONFIGSCRIPT=" +
                           sf + "\"",
                       "");
      DeleteFileA(sf.c_str());
      return p;
    };
    const std::string prefs = state + "\\startrek\\WINDOWS\\AD_PREFS.INI";
    Proc c = configure("COMMS", 3, "TEXT 103 Kirk to Enterprise.\nCLICK 1\n", state);
    std::string ini = read_file(prefs);
    CHECK(c.code == 0 && c.out.find("\"result\":\"ok\"") != std::string::npos &&
              c.out.find("C:\\\\WINDOWS\\\\AD_PREFS.INI") != std::string::npos &&
              ini.find("MessageText=Kirk to Enterprise.") != std::string::npos && ini.find("Path=") == std::string::npos &&
              ini.find("SoundDriver") == std::string::npos,
          "COMMS button 3: shown, the message saved without the seeds (exit %d)\n%s%s\n%s", c.code, c.out.c_str(),
          c.code ? c.err.c_str() : "", ini.c_str());
    const std::string later = host + dir + "COMMS.AD ADFRAMES=600 ADFBHASH=1 ADGOWAITMS=0 ADCVSET=2=9";
    std::vector<std::string> hw = hash_list(run_cmd(later + " \"ADSTATE=" + state + "\"", "").err);
    std::vector<std::string> ho = hash_list(run_cmd(later + " \"ADSTATE=" + empty + "\"", "").err);
    size_t differ = 0;
    for (size_t i = 0; i < std::min(hw.size(), ho.size()); i++) differ += hw[i] != ho[i];
    CHECK(hw.size() == 600 && ho.size() == 600 && differ > 0, "COMMS: a later run types the saved message (%zu of 600 frames differ)",
          differ);
    Proc s = configure("SOUNDER", 2, "CLICK 1\n", state);
    ini = read_file(prefs);
    const std::string listed = line_starting(s.err, "[dlg16] DlgDirList(C:\\AFTERDRK\\SOUNDS\\");
    CHECK(s.code == 0 && ini.find("SoundPath=C:\\AFTERDRK\\SOUNDS") != std::string::npos &&
              ini.find("MessageText=Kirk to Enterprise.") != std::string::npos &&
              line_starting(s.err, "[dlg16] DlgDirList(C:\\AFTERDRK\\SOUNDS\\*.WAV, C010) into 204: [..] [-c-] [-h-]") != "",
          "SOUNDER button 2: shown, its folder saved, [-c-] and [-h-] offered (exit %d)\n%s\n%s", s.code, listed.c_str(),
          ini.c_str());
    Proc h = configure("SOUNDER", 2, "SELECT 204 2\nCLICK 1\nCLICK 2\n", hstate);
    const std::string drives = line_starting(h.err, "[dlg16] DlgDirList(H:\\"), tail = "[-c-] [-h-]";
    CHECK(h.code == 0 && drives.find("into 204: [") != std::string::npos && drives.size() > tail.size() &&
              drives.compare(drives.size() - tail.size(), tail.size(), tail) == 0 &&
              h.out.find("\"written\":[]") != std::string::npos,
          "SOUNDER: [-h-] lists the host's drives, then Cancel writes nothing (exit %d)\n%s\n%s", h.code, drives.c_str(),
          h.out.c_str());
    // Through [-h-] down to a folder of the host's own holding a made-up
    // tone, OK: the guest's DOS is on H: there (INT 21h AH=19h), so Sounder
    // saves the folder drive and all (before, SoundPath=C:\C\… and every
    // later run stopped: "Can't find any .WAV files to play!"), and a later
    // run with that state plays the tone.
    const std::string wavs = dos_temp_dir('W'), wstate = temp_dir("stcfgw"), cap = temp_dir("stcap");
    write_file(wavs + "\\TONE.WAV", tone_wav());
    const std::string g = host_guest(wavs);
    if (g.rfind("H:\\", 0) != 0 || g.size() > win16::kMaxCurDir) {
      printf("SOUNDER through [-h-]: %s is not on H: within DOS's current directory: skipped\n", g.c_str());
    } else {
      Proc w = configure("SOUNDER", 2, walk_script(g, 204) + "CLICK 1\n", wstate);
      const std::string saved = profile_value(read_file(wstate + "\\startrek\\WINDOWS\\AD_PREFS.INI"), "SoundPath");
      CHECK(w.code == 0 && saved == g, "SOUNDER: the folder chosen through [-h-] saved as %s (want %s; exit %d)\n%s",
            saved.c_str(), g.c_str(), w.code, w.code ? w.err.c_str() : "");
      const std::string out = cap + "\\sounder.wav";
      Proc s = run_cmd(host + dir + "SOUNDER.AD ADFRAMES=900 ADFBHASH=1 ADGOWAITMS=0 ADAUDIOLIVE=0 ADTRACE=file16,sound \"ADSTATE=" +
                           wstate + "\" \"ADAUDIOOUT=" + out + "\"",
                       "");
      size_t voices = 0;
      for (size_t p = 0; (p = s.err.find(": voice ", p)) != std::string::npos; p++) voices++;
      const int peak = wav_peak(read_file(out));
      CHECK(s.code == 0 && voices > 0 && peak > 328 && s.err.find("open " + g + "\\TONE.WAV") != std::string::npos,
            "SOUNDER: a later run plays the tone from H: (exit %d, %zu voices, peak %d)\n%s", s.code, voices, peak,
            s.code ? s.err.c_str() : "");
    }
    remove_tree(state);
    remove_tree(empty);
    remove_tree(hstate);
    remove_tree(wavs);
    remove_tree(wstate);
    remove_tree(cap);
  }
  // Final Exam, scripted: Num Lock's toggle starts the exam (ADNUMLOCK at the
  // start, NUMLOCK when it changes), the answers (1, 2, keypad 4, 3) reach
  // its keyboard hook and are its own, and a mouse move ends the exam with
  // result 5, After Dark 2.0's wake: the status says wake, and the run ends,
  // exit 0. Without the toggle's change there is no exam. Streamed, the wake
  // is in the status and the frames go on unchanged until QUIT.
  {
    std::string exam = gos(300) + "KEY 144 1\nNUMLOCK 0\n" + gos(1) + "KEY 144 0\n" + gos(420);
    for (int k : {49, 50, 100, 51}) {
      exam += "KEY " + std::to_string(k) + " 1\n" + gos(1) + "KEY " + std::to_string(k) + " 0\n" + gos(420);
    }
    exam += "MOUSE 100 100 0\n" + gos(1) + "MOUSE 300 300 0\n" + gos(240) + "QUIT\n";
    size_t go_count = 0;
    for (size_t p = 0; (p = exam.find("GO\n", p)) != std::string::npos; p++) go_count++;
    std::string no_toggle = exam;
    no_toggle.erase(no_toggle.find("NUMLOCK 0\n"), 10);
    const std::string cmd = host + dir + "FINAL.AD ADNUMLOCK=1 ADSTATUSLOG=1 ADFBHASH=1 ADGOWAITMS=5000";
    auto seen = [](const std::vector<Status>& st, unsigned flag) {
      for (const Status& s : st)
        if (s.flags & flag) return true;
      return false;
    };
    Proc p = run_cmd(cmd + " ADTRACE=input16", exam);
    std::vector<Status> st = statuses(p.err);
    const bool hooked = p.err.find("WH_KEYBOARD hook") != std::string::npos && p.err.find("vk 31") != std::string::npos &&
                        p.err.find("vk 64") != std::string::npos;
    const Status last = st.empty() ? Status{} : st.back();
    CHECK(p.code == 0 && seen(st, 1) && hooked && (last.flags & 0x10) && !(last.flags & 1) && last.eaten == last.applied &&
              last.eaten >= 11,
          "FINAL's exam: exit %d, interactive %s, answers hooked %s, at the end flags 0x%x eaten %llu of %llu", p.code,
          seen(st, 1) ? "yes" : "no", hooked ? "yes" : "no", last.flags, last.eaten, last.applied);
    CHECK(p.err.find("FINAL.AD: frame ") != std::string::npos &&
              p.err.find(": the module woke the saver (result 5); the run ends") != std::string::npos &&
              p.err.find("(module finished") != std::string::npos && hash_list(p.err).size() < go_count &&
              size_t(last.frame) == hash_list(p.err).size(),
          "FINAL: the wake ends the headless run (%zu of %zu frames)\n%s", hash_list(p.err).size(), go_count,
          line_starting(p.err, "[adhostwin] exit").c_str());
    Proc n = run_cmd(cmd, no_toggle);
    std::vector<Status> sn = statuses(n.err);
    CHECK(n.code == 0 && !seen(sn, 1) && !seen(sn, 0x10) && hash_list(n.err).size() == go_count &&
              n.err.find("(QUIT") != std::string::npos,
          "FINAL without Num Lock's change: no exam, no wake, every frame (exit %d, %zu frames)", n.code,
          hash_list(n.err).size());
    Proc s = run_cmd(cmd + " ADSTREAM=1", exam, /*discard_stdout=*/true);
    std::vector<Status> ss = statuses(s.err);
    std::vector<std::string> hs = hash_list(s.err);
    long woke_at = 0;
    for (const Status& x : ss) {
      if ((x.flags & 0x10) && !woke_at) woke_at = x.frame;
    }
    bool still = woke_at > 0 && size_t(woke_at) <= hs.size();
    for (size_t i = size_t(std::max(woke_at, 1L)); still && i < hs.size(); i++) still = hs[i] == hs[size_t(woke_at) - 1];
    CHECK(s.code == 0 && woke_at > 0 && hs.size() == go_count && still &&
              s.err.find("the module woke the saver (result 5); it is called no more") != std::string::npos,
          "FINAL streamed: the wake at frame %ld, then its picture until QUIT (exit %d, %zu frames)", woke_at, s.code,
          hs.size());
  }
  printf("%d/%d checks passed\n", checks - failures, checks);
  return failures ? 1 : 0;
}

// Snoopy's Screen Savers (PACKAGES.md §7.4): eight After Dark modules made
// for the user's own After Dark 2.0 or 3.0, from an imported package, which
// has no engine dir at all — no AD_SND.DLL, no ADTASK.DLL, no AFTERDAR.SCR.
std::string snoopy_root() {
  std::vector<std::string> roots;
  if (const char* r = getenv("AD_NE16_SNOOPY_ROOT"); r && *r) roots.push_back(r);
  if (const char* a = getenv("AD_ASSETS_DIR"); a && *a) roots.push_back(a);
  if (roots.empty()) roots.push_back(adw_test::installed_assets_root());
  for (const std::string& r : roots) {
    if (!r.empty() &&
        GetFileAttributesA((r + "\\win\\packages\\snoopy\\AFTERDRK\\IS_FLY.AD").c_str()) != INVALID_FILE_ATTRIBUTES)
      return r;
  }
  return {};
}

int run_snoopy(const std::string& exe) {
  const std::string root = snoopy_root();
  if (root.empty()) {
    printf("no imported Snoopy's Screen Savers: skipped\n");
    return 77;
  }
  adw_test::sandbox_spawned_hosts(root, "ne16sn");
  const std::string host = "\"" + exe + "\" ", dir = "packages/snoopy/AFTERDRK/";
  // Every module: 900 frames twice, identical, nothing unimplemented, no
  // fault (Borland's far-heap free reloads the segment it has just freed at
  // CLOSE: the freed-selector rule), the host's AD_SND and After Dark 2.0's
  // computed palettes named by the lane (Collage fades its line art through
  // palette 12); sound captured twice (ADAUDIOOUT, no device), identical,
  // heard from the six modules that import AD_SND and from neither Collage
  // nor Spotlights, the three play flags between them (Therapy's synchronous
  // 0x06, 0x07, the loops' 0x0F); with no wave device (ADSOUNDDEV=0) all
  // eight still run, silent, AD_SND having said why.
  struct Mod {
    const char* name;
    size_t min_distinct;  // of 900
    bool sounds;
  };
  const Mod mods[] = {{"IS_COLAG", 40, false}, {"IS_DANCE", 3, true},  {"IS_FACES", 800, true}, {"IS_FLY", 150, true},
                      {"IS_LINUS", 60, true},  {"IS_LITRY", 40, true}, {"IS_SPTLT", 150, false}, {"IS_THRPY", 3, true}};
  const std::string snd = temp_dir("snsnd");
  std::set<std::string> flags;
  auto no_fault = [](const std::string& log) {
    return log.find("#GP") == std::string::npos && log.find("while closing") == std::string::npos;
  };
  for (const Mod& m : mods) {
    const std::string cmd = host + dir + m.name + ".AD ADFRAMES=900 ADFBHASH=1 ADGOWAITMS=0";
    Proc a = run_cmd(cmd + " ADTRACE=lane", ""), b = run_cmd(cmd, "");
    std::vector<std::string> ha = hash_list(a.err), hb = hash_list(b.err);
    CHECK(a.code == 0 && b.code == 0 && ha.size() == 900 && ha == hb, "%s: exit %d/%d, %zu frames, %s\n%s", m.name, a.code,
          b.code, ha.size(), ha == hb ? "deterministic" : "NOT deterministic", a.code ? a.err.c_str() : "");
    CHECK(a.err.find(" 0 unimplemented") != std::string::npos && no_fault(a.err) && no_fault(b.err),
          "%s: no unimplemented call, no fault", m.name);
    CHECK(distinct_of(ha, 900) >= m.min_distinct, "%s: %zu distinct frames of 900 (at least %zu)", m.name,
          distinct_of(ha, 900), m.min_distinct);
    CHECK(a.err.find("AD_SND the host's (no ") != std::string::npos &&
              a.err.find("palettes After Dark 2.0's four, computed") != std::string::npos,
          "%s: the host's AD_SND, the computed palettes", m.name);
    const std::string w1 = snd + "\\" + m.name + "1.wav", w2 = snd + "\\" + m.name + "2.wav";
    Proc s1 = run_cmd(cmd + " ADAUDIOLIVE=0 ADTRACE=sound \"ADAUDIOOUT=" + w1 + "\"", "");
    Proc s2 = run_cmd(cmd + " ADAUDIOLIVE=0 \"ADAUDIOOUT=" + w2 + "\"", "");
    const std::string c1 = read_file(w1), c2 = read_file(w2);
    size_t voices = 0;
    for (size_t p = 0; (p = s1.err.find(": voice ", p)) != std::string::npos; p++) voices++;
    for (size_t p = 0; (p = s1.err.find("sndPlaySound(", p)) != std::string::npos; p++) {
      size_t comma = s1.err.find(", ", p), close = s1.err.find(')', p);
      if (comma != std::string::npos && close == comma + 6 && s1.err.compare(p + 13, 4, "NULL") != 0 &&
          s1.err.compare(p + 13, 8, "00000000") != 0) {
        flags.insert(s1.err.substr(comma + 2, 4));
      }
    }
    const int peak = wav_peak(c1);
    std::vector<std::string> h1 = hash_list(s1.err), h2 = hash_list(s2.err);
    CHECK(s1.code == 0 && s2.code == 0 && c1.size() > 44 && c1 == c2 && h1.size() == 900 && h1 == h2 && no_fault(s1.err),
          "%s with sound: exit %d/%d, captures %s (%zu bytes), streams %s", m.name, s1.code, s2.code,
          c1 == c2 ? "identical" : "DIFFER", c1.size(), h1 == h2 ? "identical" : "DIFFER");
    CHECK(m.sounds ? voices > 0 && peak > 328 : voices == 0 && peak == 0, "%s: %s (%zu voices, peak %d)", m.name,
          m.sounds ? "heard, above -40 dBFS" : "silent", voices, peak);
    printf("%s: %zu distinct frames, %zu voices, peak %d\n", m.name, distinct_of(ha, 900), voices, peak);
    for (const std::string& w : {w1, w2}) {
      DeleteFileA(w.c_str());
      DeleteFileA((w.substr(0, w.size() - 4) + ".mid").c_str());
    }
    Proc n = run_cmd(host + dir + m.name + ".AD ADFRAMES=300 ADFBHASH=1 ADGOWAITMS=0 ADSOUND=1 ADSOUNDDEV=0 ADTRACE=sound", "");
    CHECK(n.code == 0 && hash_list(n.err).size() == 300 && no_fault(n.err) &&
              n.err.find("no sound: No wave output device is installed.") != std::string::npos &&
              n.err.find(": voice ") == std::string::npos,
          "%s, no wave device: runs silent (exit %d)", m.name, n.code);
  }
  RemoveDirectoryA(snd.c_str());
  std::string seen;
  for (const std::string& f : flags) seen += (seen.empty() ? "" : " ") + f;
  CHECK(seen == "0006 0007 000F", "the play flags: %s", seen.c_str());
  printf("%d/%d checks passed\n", checks - failures, checks);
  return failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 2 && std::string(argv[1]) == "--assets") return run_assets(argv[2]);
  if (argc > 2 && std::string(argv[1]) == "--pkg") return run_pkg(argv[2]);
  if (argc > 2 && std::string(argv[1]) == "--interaction") return run_interaction(argv[2]);
  if (argc > 2 && std::string(argv[1]) == "--swse") return run_swse(argv[2]);
  if (argc > 2 && std::string(argv[1]) == "--startrek") return run_startrek(argv[2]);
  if (argc > 2 && std::string(argv[1]) == "--snoopy") return run_snoopy(argv[2]);
  return run_all_unit();
}
