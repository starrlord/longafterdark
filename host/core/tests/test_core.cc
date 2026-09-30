// Unit tests for adw_core: P8/P6 encoding, FBHASH, env parsing, command
// parsing, the virtual clock, the pacer, the stdin reader / stdout writer on
// real anonymous pipes, lane probing, run_host driven in-process, and where
// the data folder is (data_root.h), which never touches the disk.
#include <windows.h>

#include <atomic>
#include <bitset>
#include <chrono>
#include <cstring>
#include <deque>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "adw/core/clock.h"
#include "adw/core/data_root.h"
#include "adw/core/env.h"
#include "adw/core/fnv.h"
#include "adw/core/host.h"
#include "adw/core/lane.h"
#include "adw/core/log.h"
#include "adw/core/pacer.h"
#include "adw/core/protocol.h"
#include "adw/core/rng.h"
#include "adw/core/screen.h"
#include "adw/core/status.h"
#include "adw/core/text.h"
#include "check.h"

using namespace adw;

namespace {

std::string temp_dir() {
  wchar_t buf[MAX_PATH];
  GetTempPathW(MAX_PATH, buf);
  return narrow(buf);
}

std::string write_temp(const char* name, const std::vector<uint8_t>& bytes) {
  std::string path = temp_dir() + "adw_core_test_" + std::to_string(GetCurrentProcessId()) + "_" + name;
  FILE* f = _wfopen(widen(path).c_str(), L"wb");
  if (f) {
    fwrite(bytes.data(), 1, bytes.size(), f);
    fclose(f);
  }
  return path;
}

std::vector<uint8_t> read_file(const std::string& path) {
  std::vector<uint8_t> out;
  FILE* f = _wfopen(widen(path).c_str(), L"rb");
  if (!f) return out;
  uint8_t buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
  fclose(f);
  return out;
}

Screen small_screen() {
  Screen s(4, 2);
  s.set_entry(0, 1, 2, 3);
  s.set_entry(1, 4, 5, 6);
  for (int x = 0; x < 4; x++) {
    s.row(0)[x] = uint8_t(x);
    s.row(1)[x] = uint8_t(4 + x);
  }
  return s;
}

}  // namespace

// ---- FNV / screen ------------------------------------------------------------

TEST(fnv_known_vectors) {
  CHECK_EQ(fnv1a64("", 0), 0xcbf29ce484222325ULL);
  CHECK_EQ(fnv1a64("a", 1), 0xaf63dc4c8601ec8cULL);
  CHECK_EQ(fnv1a64("foobar", 6), 0x85944171f73967e8ULL);
  // Chaining equals hashing the concatenation.
  CHECK_EQ(fnv1a64("bar", 3, fnv1a64("foo", 3)), fnv1a64("foobar", 6));
}

TEST(static_palette) {
  Screen s(2, 2);
  auto rgb = [&](int i) {
    const RGBQUAD& q = s.palette()[size_t(i)];
    return (uint32_t(q.rgbRed) << 16) | (uint32_t(q.rgbGreen) << 8) | q.rgbBlue;
  };
  CHECK_EQ(rgb(0), 0x000000u);
  CHECK_EQ(rgb(7), 0xC0C0C0u);
  CHECK_EQ(rgb(8), 0xC0DCC0u);
  CHECK_EQ(rgb(9), 0xA6CAF0u);
  CHECK_EQ(rgb(100), 0x000000u);
  CHECK_EQ(rgb(246), 0xFFFBF0u);
  CHECK_EQ(rgb(249), 0xFF0000u);
  CHECK_EQ(rgb(255), 0xFFFFFFu);
}

TEST(p8_encoding) {
  Screen s = small_screen();
  CHECK_EQ(p8_header(4, 2), std::string("P8\n4 2\n"));
  std::vector<uint8_t> out;
  s.encode_p8(out);
  CHECK_EQ(out.size(), size_t(7 + 768 + 8));
  CHECK_EQ(s.p8_size(), out.size());
  CHECK(memcmp(out.data(), "P8\n4 2\n", 7) == 0);
  const uint8_t* pal = out.data() + 7;
  CHECK(pal[0] == 1 && pal[1] == 2 && pal[2] == 3);        // entry 0 as R,G,B
  CHECK(pal[3] == 4 && pal[4] == 5 && pal[5] == 6);        // entry 1
  CHECK(pal[255 * 3] == 255 && pal[255 * 3 + 2] == 255);  // white stays
  const uint8_t* idx = out.data() + 7 + 768;
  for (int i = 0; i < 8; i++) CHECK_EQ(int(idx[i]), i);
  // Re-encoding reuses the buffer and yields identical bytes.
  std::vector<uint8_t> again = out;
  s.encode_p8(again);
  CHECK(again == out);
}

TEST(fbhash_is_fnv_of_p8_body) {
  Screen s = small_screen();
  std::vector<uint8_t> out;
  s.encode_p8(out);
  CHECK_EQ(s.fbhash(), fnv1a64(out.data() + 7, out.size() - 7));
  uint64_t before = s.fbhash();
  s.set_entry(200, 9, 9, 9);  // palette-only change must change the hash
  CHECK(s.fbhash() != before);
}

TEST(attach_bottom_up_and_padded) {
  // A bottom-up DIB with 8-byte padded rows: memory holds row 1 first.
  std::vector<uint8_t> mem(16, 0xEE);
  for (int x = 0; x < 4; x++) {
    mem[size_t(x)] = uint8_t(4 + x);  // bottom row (y=1) stored first
    mem[size_t(8 + x)] = uint8_t(x);  // top row (y=0) stored second
  }
  Screen s = small_screen();
  std::vector<uint8_t> owned;
  s.encode_p8(owned);
  s.attach(mem.data() + 8, -8);
  CHECK(s.attached());
  std::vector<uint8_t> aliased;
  s.encode_p8(aliased);
  CHECK(aliased == owned);
  s.row(0)[0] = 3;  // writes land in the external memory
  CHECK_EQ(int(mem[8]), 3);
  s.detach();
  CHECK(!s.attached());
  CHECK_EQ(int(s.row(0)[0]), 0);
}

TEST(p6_and_ppm) {
  Screen s = small_screen();
  std::vector<uint8_t> p6;
  s.encode_p6(p6);
  CHECK_EQ(p6.size(), size_t(11 + 4 * 2 * 3));
  CHECK(memcmp(p6.data(), "P6\n4 2\n255\n", 11) == 0);
  CHECK(p6[11] == 1 && p6[12] == 2 && p6[13] == 3);  // pixel (0,0) = entry 0
  CHECK(p6[14] == 4 && p6[15] == 5 && p6[16] == 6);  // pixel (1,0) = entry 1
  std::string path = temp_dir() + "adw_core_test_" + std::to_string(GetCurrentProcessId()) + ".ppm";
  CHECK(s.write_ppm(path));
  CHECK(read_file(path) == p6);
  DeleteFileW(widen(path).c_str());
}

TEST(screen_move_reseats_surface) {
  // Owned storage: the moved-to screen draws into its own (stolen) buffer,
  // and the moved-from one is an empty, detached 0x0 surface.
  Screen a = small_screen();
  std::vector<uint8_t> before;
  a.encode_p8(before);
  Screen b(std::move(a));
  CHECK(!b.attached());
  std::vector<uint8_t> after;
  b.encode_p8(after);
  CHECK(after == before);
  CHECK(!a.attached());
  CHECK_EQ(a.width(), 0);
  // Attached storage stays attached to the same external memory.
  std::vector<uint8_t> mem(8, 9);
  Screen c(4, 2);
  c.attach(mem.data(), 4);
  Screen d(1, 1);
  d = std::move(c);
  CHECK(d.attached());
  d.row(1)[3] = 5;
  CHECK_EQ(int(mem[7]), 5);
}

TEST(screen_dimensions_clamped) {
  Screen s(0, 99999);
  CHECK_EQ(s.width(), 1);
  CHECK_EQ(s.height(), Screen::kMaxDim);
}

// ---- env ---------------------------------------------------------------------

TEST(env_defaults) {
  Env e = Env::parse({});
  CHECK(!e.stream);
  CHECK_EQ(e.screen_w, 640);
  CHECK_EQ(e.screen_h, 480);
  CHECK_EQ(e.frames, uint64_t(0));
  CHECK(!e.fbhash);
  CHECK(e.out_dir.empty());
  CHECK(e.cvset.empty());
  CHECK_EQ(e.seed, uint64_t(1));
  CHECK(!e.no_pace);
  CHECK(e.trace.empty());
  CHECK_EQ(e.go_wait_ms, 250u);
  CHECK(e.warnings.empty());
  CHECK(e.assets_root.empty());
  CHECK(e.data_root.empty());
  Env l = Env::parse({{"LOCALAPPDATA", "C:\\Users\\x\\AppData\\Local"}});
  CHECK_EQ(l.data_root, std::string("C:\\Users\\x\\AppData\\Local\\LongAfterDark"));
  CHECK_EQ(l.assets_root, std::string("C:\\Users\\x\\AppData\\Local\\LongAfterDark\\assets"));
  CHECK_EQ(l.win_assets_dir(), std::string("C:\\Users\\x\\AppData\\Local\\LongAfterDark\\assets\\win"));
}

TEST(env_data_root) {
  // AD_LOCALAPPDATA (trimmed, not blank) stands in for LOCALAPPDATA; a
  // trailing separator joins cleanly.
  Env o = Env::parse({{"LOCALAPPDATA", "C:\\L"}, {"AD_LOCALAPPDATA", " D:\\scratch\\ "}});
  CHECK_EQ(o.data_root, std::string("D:\\scratch\\LongAfterDark"));
  CHECK_EQ(o.assets_root, std::string("D:\\scratch\\LongAfterDark\\assets"));
  Env b = Env::parse({{"LOCALAPPDATA", " C:\\L "}, {"AD_LOCALAPPDATA", "  "}});
  CHECK_EQ(b.data_root, std::string("C:\\L\\LongAfterDark"));
  // AD_ASSETS_DIR set: the data root is still known, but assets do not come from it.
  Env a = Env::parse({{"LOCALAPPDATA", "C:\\L"}, {"AD_ASSETS_DIR", "E:\\assets"}});
  CHECK_EQ(a.data_root, std::string("C:\\L\\LongAfterDark"));
  CHECK_EQ(a.assets_root, std::string("E:\\assets"));
  // A blank AD_ASSETS_DIR is unset.
  CHECK_EQ(Env::parse({{"LOCALAPPDATA", "C:\\L"}, {"AD_ASSETS_DIR", " "}}).assets_root,
           std::string("C:\\L\\LongAfterDark\\assets"));
  // No base at all: no data folder, and so no default assets root.
  Env n = Env::parse({{"LOCALAPPDATA", " "}, {"AD_LOCALAPPDATA", ""}});
  CHECK(n.data_root.empty());
  CHECK(n.assets_root.empty());
}

TEST(env_values) {
  Env e = Env::parse({{"ADSTREAM", "1"},
                      {"ADSCREENW", "800"},
                      {"ADSCREENH", "0x258"},
                      {"ADFRAMES", "120"},
                      {"ADFBHASH", "yes"},
                      {"ADOUT", " C:\\frames "},
                      {"ADSEED", "42"},
                      {"ADNOPACE", "1"},
                      {"AD_ASSETS_DIR", "D:\\assets"},
                      {"ADTRACE", "Proto, gdi;kernel"},
                      {"ADPACEMS", "16.5"},
                      {"ADGOWAITMS", "0"},
                      {"ADSTREAMP6", "on"},
                      {"ADFOO", "bar"}});
  CHECK(e.stream);
  CHECK_EQ(e.screen_w, 800);
  CHECK_EQ(e.screen_h, 600);
  CHECK_EQ(e.frames, uint64_t(120));
  CHECK(e.fbhash);
  CHECK_EQ(e.out_dir, std::string("C:\\frames"));
  CHECK_EQ(e.seed, uint64_t(42));
  CHECK(e.no_pace);
  CHECK_EQ(e.assets_root, std::string("D:\\assets"));
  CHECK(e.trace == (std::set<std::string>{"proto", "gdi", "kernel"}));
  CHECK(e.traced("GDI"));
  CHECK(!e.traced("user"));
  CHECK(e.pace_ms == 16.5);
  CHECK_EQ(e.go_wait_ms, 0u);
  CHECK(e.stream_p6);
  CHECK(!e.stream_force);
  CHECK(e.get("ADFOO") && *e.get("ADFOO") == "bar");
  CHECK(e.get("adfoo") != nullptr);  // Windows env names are case-insensitive
  CHECK(e.get("ADNOPE") == nullptr);
  CHECK(e.warnings.empty());
}

TEST(env_flags_and_case) {
  CHECK(!Env::parse({{"ADSTREAM", "0"}}).stream);
  CHECK(!Env::parse({{"ADSTREAM", ""}}).stream);
  CHECK(!Env::parse({{"ADSTREAM", "off"}}).stream);
  CHECK(Env::parse({{"ADSTREAM", "true"}}).stream);
  CHECK(Env::parse({{"adstream", "1"}}).stream);
  Env all = Env::parse({{"ADTRACE", "all"}});
  CHECK(all.traced("anything"));
}

TEST(env_bad_values_fall_back) {
  Env e = Env::parse({{"ADSCREENW", "abc"},
                      {"ADSCREENH", "0"},
                      {"ADFRAMES", "-5"},
                      {"ADSEED", "x"},
                      {"ADPACEMS", "-1"},
                      {"ADGOWAITMS", "999999"}});
  CHECK_EQ(e.screen_w, 640);
  CHECK_EQ(e.screen_h, 480);
  CHECK_EQ(e.frames, uint64_t(0));
  CHECK_EQ(e.seed, uint64_t(1));
  CHECK(e.pace_ms == 0.0);
  CHECK_EQ(e.go_wait_ms, 250u);
  CHECK_EQ(e.warnings.size(), size_t(6));
  CHECK_EQ(Env::parse({{"ADSCREENW", "20000"}}).screen_w, 640);
}

TEST(env_cvset_and_seed) {
  Env e = Env::parse({{"ADCVSET", "0=5, 3:7,2=-1,,bad,9=x,70000=1"}});
  CHECK_EQ(e.cvset.size(), size_t(3));
  if (e.cvset.size() == 3) {
    CHECK(e.cvset[0] == std::make_pair(0, int32_t(5)));
    CHECK(e.cvset[1] == std::make_pair(3, int32_t(7)));
    CHECK(e.cvset[2] == std::make_pair(2, int32_t(-1)));
  }
  CHECK_EQ(e.warnings.size(), size_t(3));
  CHECK_EQ(Env::parse({{"ADSEED", "0x10"}}).seed, uint64_t(16));
  // Zero-padded values are decimal (as a front-end means them), not octal.
  Env z = Env::parse({{"ADSCREENW", "0640"}, {"ADFRAMES", "010"}, {"ADCVSET", "3=010,4=-0x10,5=08"}});
  CHECK_EQ(z.screen_w, 640);
  CHECK_EQ(z.frames, uint64_t(10));
  CHECK_EQ(z.cvset.size(), size_t(3));
  if (z.cvset.size() == 3) {
    CHECK(z.cvset[0] == std::make_pair(3, int32_t(10)));
    CHECK(z.cvset[1] == std::make_pair(4, int32_t(-16)));
    CHECK(z.cvset[2] == std::make_pair(5, int32_t(8)));
  }
  CHECK(z.warnings.empty());
  Env r = Env::parse({{"ADSEED", "random"}});
  CHECK(r.seed_random);
  CHECK(r.warnings.empty());
}

TEST(env_win_assets_dir_levels) {
  // AD_ASSETS_DIR may name the root (holding win\FILES) or win itself.
  std::string root = temp_dir() + "adw_core_assets_" + std::to_string(GetCurrentProcessId());
  CreateDirectoryW(widen(root).c_str(), nullptr);
  CreateDirectoryW(widen(root + "\\win").c_str(), nullptr);
  CreateDirectoryW(widen(root + "\\win\\FILES").c_str(), nullptr);
  CHECK_EQ(Env::parse({{"AD_ASSETS_DIR", root}}).win_assets_dir(), root + "\\win");
  CHECK_EQ(Env::parse({{"AD_ASSETS_DIR", root + "\\win"}}).win_assets_dir(), root + "\\win");
  RemoveDirectoryW(widen(root + "\\win\\FILES").c_str());
  RemoveDirectoryW(widen(root + "\\win").c_str());
  RemoveDirectoryW(widen(root).c_str());
}

TEST(env_win_assets_dir_markers) {
  // PACKAGES.md §5.2: FILES\, packages\ or catalog-win.json marks a win dir,
  // first at <root>\win, then at <root> itself; with none, <root>\win.
  std::string root = temp_dir() + "adw_core_markers_" + std::to_string(GetCurrentProcessId());
  auto mk = [](const std::string& d) { CreateDirectoryW(widen(d).c_str(), nullptr); };
  auto rm = [](const std::string& d) { RemoveDirectoryW(widen(d).c_str()); };
  auto touch = [](const std::string& f) {
    FILE* h = _wfopen(widen(f).c_str(), L"wb");
    if (h) fclose(h);
  };
  auto win_of = [](const std::string& r) { return Env::parse({{"AD_ASSETS_DIR", r}}).win_assets_dir(); };
  mk(root);
  CHECK_EQ(win_of(root), root + "\\win");  // nothing yet: a fresh install goes to <root>\win
  mk(root + "\\win");
  CHECK_EQ(win_of(root), root + "\\win");
  // Only non-Deluxe packages imported: no FILES\ anywhere.
  mk(root + "\\win\\packages");
  CHECK_EQ(win_of(root), root + "\\win");
  CHECK_EQ(win_of(root + "\\win"), root + "\\win");  // AD_ASSETS_DIR naming win itself
  rm(root + "\\win\\packages");
  // Only a catalog.
  touch(root + "\\win\\catalog-win.json");
  CHECK_EQ(win_of(root), root + "\\win");
  CHECK_EQ(win_of(root + "\\win"), root + "\\win");
  DeleteFileW(widen(root + "\\win\\catalog-win.json").c_str());
  // A directory named catalog-win.json is not a catalog; an empty win\ marks nothing.
  mk(root + "\\win\\catalog-win.json");
  CHECK_EQ(win_of(root + "\\win"), root + "\\win\\win");
  rm(root + "\\win\\catalog-win.json");
  // <root>\win wins over markers at <root>.
  mk(root + "\\packages");
  CHECK_EQ(win_of(root), root);  // win\ holds nothing, root holds packages\ .
  mk(root + "\\win\\FILES");
  CHECK_EQ(win_of(root), root + "\\win");
  rm(root + "\\win\\FILES");
  rm(root + "\\packages");
  rm(root + "\\win");
  rm(root);
}

// ---- the data folder (data_root.h) -------------------------------------------

TEST(data_root_path_from_base) {
  CHECK(data_root_path(L"C:\\L") == L"C:\\L\\LongAfterDark");
  // Blanks around the base and a trailing separator are fine.
  CHECK(data_root_path(L" C:\\L\\ ") == L"C:\\L\\LongAfterDark");
  CHECK(data_root_path(L"C:/L/") == L"C:/L/LongAfterDark");
  // No base, no data folder.
  CHECK(data_root_path(L"").empty());
  CHECK(data_root_path(L" \t ").empty());
}

TEST(data_root_base_from_environment) {
  auto get = [](const wchar_t* n) -> std::optional<std::wstring> {
    DWORD k = GetEnvironmentVariableW(n, nullptr, 0);
    if (k == 0) return std::nullopt;
    std::wstring v(k, L'\0');
    v.resize(GetEnvironmentVariableW(n, v.data(), k));
    return v;
  };
  auto put = [](const wchar_t* n, const std::optional<std::wstring>& v) {
    SetEnvironmentVariableW(n, v ? v->c_str() : nullptr);
  };
  auto la = get(L"LOCALAPPDATA"), over = get(L"AD_LOCALAPPDATA");
  put(L"LOCALAPPDATA", L" C:\\LA ");
  put(L"AD_LOCALAPPDATA", std::nullopt);
  CHECK(data_root_base() == L"C:\\LA");
  put(L"AD_LOCALAPPDATA", L"  ");  // blank: ignored
  CHECK(data_root_base() == L"C:\\LA");
  put(L"AD_LOCALAPPDATA", L"D:\\scratch");
  CHECK(data_root_base() == L"D:\\scratch");
  // adhostwin's Env finds the same folder in the process environment.
  CHECK_EQ(Env::from_process().data_root, narrow(data_root_path(data_root_base())));
  CHECK_EQ(Env::from_process().data_root, std::string("D:\\scratch\\LongAfterDark"));
  put(L"LOCALAPPDATA", std::nullopt);
  put(L"AD_LOCALAPPDATA", std::nullopt);
  CHECK(data_root_base().empty());
  CHECK(Env::from_process().data_root.empty());
  put(L"LOCALAPPDATA", la);
  put(L"AD_LOCALAPPDATA", over);
  CHECK(get(L"LOCALAPPDATA") == la);
}

// ---- commands ----------------------------------------------------------------

TEST(parse_commands) {
  Command c;
  CHECK(parse_command("GO", c) && c.kind == Command::Kind::go);
  CHECK(parse_command("GO\r", c) && c.kind == Command::Kind::go);
  CHECK(parse_command("  go  ", c) && c.kind == Command::Kind::go);
  CHECK(parse_command("SET 3 -5", c) && c.kind == Command::Kind::set && c.a == 3 && c.b == -5);
  CHECK(parse_command("KEY 65 1", c) && c.kind == Command::Kind::key && c.a == 65 && c.b == 1);
  CHECK(parse_command("KEY 65 7", c) && c.b == 1);  // any nonzero = down
  CHECK(parse_command("CAPS 1", c) && c.kind == Command::Kind::caps && c.a == 1);
  CHECK(parse_command("NUMLOCK 1", c) && c.kind == Command::Kind::numlock && c.a == 1);
  CHECK(parse_command("numlock 7", c) && c.kind == Command::Kind::numlock && c.a == 1);  // any nonzero = on
  CHECK(parse_command("NUMLOCK 0", c) && c.kind == Command::Kind::numlock && c.a == 0);
  CHECK(std::string(command_name(Command::Kind::numlock)) == "NUMLOCK");
  CHECK(!parse_command("NUMLOCK", c) && c.kind == Command::Kind::unknown);
  CHECK(!parse_command("NUMLOCK 1 1", c));
  CHECK(!parse_command("NUMLOCK on", c));
  CHECK(parse_command("MOUSE 10 -20 1", c) && c.kind == Command::Kind::mouse && c.a == 10 &&
        c.b == -20 && c.c == 1);
  CHECK(parse_command("QUIT", c) && c.kind == Command::Kind::quit);
  CHECK(!parse_command("", c));
  CHECK(!parse_command("GO GO", c));
  CHECK(!parse_command("SET 1", c));
  CHECK(!parse_command("SET 1 2 3", c));
  CHECK(!parse_command("SET -1 2", c));
  CHECK(!parse_command("SET a 2", c));
  CHECK(!parse_command("KEY 256 1", c));
  CHECK(!parse_command("MOUSE 1 2", c));
  CHECK(!parse_command("SET 1 99999999999", c));
  CHECK(!parse_command("HELLO", c) && c.kind == Command::Kind::unknown && c.text == "HELLO");
}

TEST(input_state) {
  InputState in;
  Command c;
  parse_command("SET 2 40", c);
  in.apply(c);
  parse_command("KEY 37 1", c);
  in.apply(c);
  parse_command("CAPS 1", c);
  in.apply(c);
  CHECK(!in.numlock);
  parse_command("NUMLOCK 1", c);
  in.apply(c);
  parse_command("MOUSE 5 6 1", c);
  in.apply(c);
  CHECK_EQ(in.control(2, 0), 40);
  CHECK_EQ(in.control(3, -1), -1);
  CHECK(in.keys.test(37));
  CHECK_EQ(in.last_key, 37);
  CHECK(in.last_key_down);
  CHECK(in.caps);
  CHECK(in.numlock);
  CHECK(!in.keys.test(144));  // the toggle, not the key
  parse_command("NUMLOCK 0", c);
  in.apply(c);
  CHECK(!in.numlock && in.caps);
  CHECK(in.mouse_seen && in.mouse_x == 5 && in.mouse_y == 6 && in.mouse_button);
  parse_command("KEY 37 0", c);
  in.apply(c);
  CHECK(!in.keys.test(37));
  // A hand-built KEY outside 0..255 is ignored, not thrown out of bitset::set.
  Command wild;
  wild.kind = Command::Kind::key;
  wild.a = 999;
  wild.b = 1;
  in.apply(wild);
  wild.a = -1;
  in.apply(wild);
  CHECK_EQ(in.last_key, 37);
  CHECK(in.keys.none());
}

// ---- clock / pacer -----------------------------------------------------------

TEST(clock_fixed_step) {
  VirtualClock c(VirtualClock::Mode::fixed_step, 33333);
  c.begin_frame();
  CHECK_EQ(c.now_us(), uint64_t(0));
  CHECK_EQ(c.tick_count(), VirtualClock::kBootOffsetMs);
  c.begin_frame();
  c.begin_frame();
  CHECK_EQ(c.frame(), uint64_t(2));
  CHECK_EQ(c.now_us(), uint64_t(66666));
  CHECK_EQ(c.now_ms(), uint64_t(66));
  CHECK_EQ(c.read_us(), uint64_t(66666));  // no nudge by default
  c.set_read_step_us(10);
  CHECK_EQ(c.read_us(), uint64_t(66676));
  CHECK_EQ(c.read_us(), uint64_t(66686));
  c.begin_frame();
  CHECK_EQ(c.now_us(), uint64_t(99999 + 20));
}

TEST(clock_realtime_capped) {
  uint64_t wall = 1'000'000;
  VirtualClock c(VirtualClock::Mode::realtime, 33333, [&] { return wall; });
  c.begin_frame();
  CHECK_EQ(c.now_us(), uint64_t(0));
  wall += 5000;
  CHECK_EQ(c.now_us(), uint64_t(5000));  // intra-frame reads follow the wall
  wall += 5000;
  c.begin_frame();
  CHECK_EQ(c.now_us(), uint64_t(10000));
  wall += 10'000'000;  // a 10 s stall (occluded front-end)
  uint64_t during = c.now_us();
  CHECK_EQ(during, uint64_t(10000 + 250000));
  c.begin_frame();
  CHECK_EQ(c.now_us(), during);  // monotonic across the boundary
  c.set_scale(2.0);
  wall += 1000;
  CHECK_EQ(c.now_us(), during + 2000);
}

TEST(clock_realtime_busy_wait_progresses) {
  // A module that spins on GetTickCount inside ONE step for a whole second
  // must see that second pass: only gaps between observations are capped,
  // not the time since the frame began (which froze time 250 ms into a step).
  uint64_t wall = 5'000'000;
  VirtualClock c(VirtualClock::Mode::realtime, 33333, [&] { return wall; });
  c.begin_frame();
  uint32_t t0 = c.read_tick_count();
  uint32_t t = t0;
  for (int i = 0; i < 100000 && t - t0 < 1000; i++) {
    wall += 1000;
    t = c.read_tick_count();
  }
  CHECK_EQ(t - t0, 1000u);
  CHECK_EQ(c.now_us(), uint64_t(1'000'000));
  // At a fractional scale, reads a microsecond apart are not rounded away.
  c.set_scale(0.5);
  uint64_t r0 = c.read_us();
  CHECK_EQ(r0, uint64_t(1'000'000));
  for (int i = 0; i < 1000; i++) {
    wall += 1;
    c.read_us();
  }
  CHECK_EQ(c.read_us(), r0 + 500);
  // An idle gap first observed by a read (a lane's on_command, say) is capped
  // just like one first observed by begin_frame, and time stays monotonic.
  wall += 7'000'000;
  uint64_t after_idle = c.read_us();
  CHECK_EQ(after_idle, r0 + 500 + 125000);
  c.begin_frame();
  CHECK_EQ(c.now_us(), after_idle);
  // Lowering the cap applies from now on; nothing already elapsed is undone.
  wall += 100;
  uint64_t before_cap = c.now_us();
  c.set_max_gap_us(10);
  CHECK(c.now_us() >= before_cap);
}

TEST(pacer_grid) {
  uint64_t wall = 0;
  std::vector<uint64_t> slept;
  Pacer p(10000, true, [&] { return wall; }, [&](uint64_t us) {
    slept.push_back(us);
    wall += us;
  });
  CHECK_EQ(p.wait_for_next_frame(), uint64_t(10000));  // first frame: one period
  wall += 3000;                                         // a frame that took 3 ms
  CHECK_EQ(p.wait_for_next_frame(), uint64_t(7000));    // sleeps the rest of the period
  wall += 12000;                                        // overrun by 2 ms
  CHECK_EQ(p.wait_for_next_frame(), uint64_t(0));       // late: no sleep
  wall += 50000;                                        // way behind: grid restarts at now
  CHECK_EQ(p.wait_for_next_frame(), uint64_t(0));
  wall += 1000;
  CHECK_EQ(p.wait_for_next_frame(), uint64_t(9000));    // no burst to "catch up"
  p.enter_lockstep();
  CHECK_EQ(p.wait_for_next_frame(), uint64_t(0));
  Pacer headless(10000, false, [&] { return wall; }, [&](uint64_t) { CHECK(false); });
  CHECK_EQ(headless.wait_for_next_frame(), uint64_t(0));
}

TEST(trace_categories_case_insensitive) {
  // Env lower-cases ADTRACE, but set_trace_categories is public: whatever case
  // either side uses, tracing() must agree with Env::traced().
  set_trace_categories({"GDI", "proto"});
  CHECK(tracing("gdi"));
  CHECK(tracing("Proto"));
  CHECK(!tracing("user"));
  set_trace_categories({"All"});
  CHECK(tracing("anything"));
  set_trace_categories({});
  CHECK(!tracing("gdi"));
}

TEST(rng_deterministic) {
  Rng a(7), b(7), c(8);
  uint64_t x = a.next();
  CHECK_EQ(x, b.next());
  CHECK(x != c.next());
  Rng s1 = Rng::derive(1, 1), s2 = Rng::derive(1, 2);
  CHECK(s1.next() != s2.next());
  for (int i = 0; i < 1000; i++) CHECK(a.below(10) < 10);
}

// ---- pipes -------------------------------------------------------------------

TEST(stdin_reader_pipe) {
  HANDLE r = nullptr, w = nullptr;
  CHECK(CreatePipe(&r, &w, nullptr, 0));
  StdinReader reader;
  reader.start(r);
  CHECK(reader.kind() == StdinReader::Kind::pipe);
  Command c;
  CHECK(!reader.poll(c));
  CHECK(!reader.wait_ready(30));  // nothing yet: times out
  const char* chunk1 = "SET 1 2\nGO\r\nbogus\n\n  \nQUI";
  const char* chunk2 = "T\n";
  DWORD n;
  WriteFile(w, chunk1, DWORD(strlen(chunk1)), &n, nullptr);
  Sleep(20);
  WriteFile(w, chunk2, DWORD(strlen(chunk2)), &n, nullptr);
  std::vector<Command::Kind> kinds;
  while (kinds.size() < 4 && reader.wait_ready(2000)) {
    while (reader.poll(c)) kinds.push_back(c.kind);
  }
  CHECK(kinds == (std::vector<Command::Kind>{Command::Kind::set, Command::Kind::go,
                                             Command::Kind::unknown, Command::Kind::quit}));
  CloseHandle(w);
  CHECK(reader.wait_ready(2000));
  CHECK(reader.poll(c) && c.kind == Command::Kind::eof);
  CHECK(!reader.poll(c));
  CHECK(!reader.wait_ready(INFINITE));  // ended: never blocks
  reader.stop();
  CloseHandle(r);
}

TEST(stdin_reader_final_line_without_newline) {
  HANDLE r = nullptr, w = nullptr;
  CHECK(CreatePipe(&r, &w, nullptr, 0));
  DWORD n;
  WriteFile(w, "GO", 2, &n, nullptr);
  CloseHandle(w);
  StdinReader reader;
  reader.start(r);
  Command c;
  std::vector<Command::Kind> kinds;
  while (reader.wait_ready(2000) && reader.poll(c)) kinds.push_back(c.kind);
  CHECK(kinds == (std::vector<Command::Kind>{Command::Kind::go, Command::Kind::eof}));
  reader.stop();
  CloseHandle(r);
}

TEST(stdin_reader_overlong_line_dropped_whole) {
  // 70 KB with no newline: reported once, and its tail (after the 64 KB cut)
  // must not surface as a second junk line before the next real command.
  HANDLE r = nullptr, w = nullptr;
  CHECK(CreatePipe(&r, &w, nullptr, 0));
  StdinReader reader;
  reader.start(r);
  std::thread writer([w] {
    std::string junk(70 * 1024, 'x');
    junk += "\nGO\n";
    DWORD n;
    WriteFile(w, junk.data(), DWORD(junk.size()), &n, nullptr);
    CloseHandle(w);
  });
  std::vector<Command> got;
  Command c;
  while (reader.wait_ready(3000) && reader.poll(c)) got.push_back(c);
  writer.join();
  CHECK_EQ(got.size(), size_t(3));
  if (got.size() == 3) {
    CHECK(got[0].kind == Command::Kind::unknown);
    CHECK_EQ(got[0].text, std::string("(overlong line discarded)"));
    CHECK(got[1].kind == Command::Kind::go);
    CHECK(got[2].kind == Command::Kind::eof);
  }
  reader.stop();
  CloseHandle(r);
}

TEST(stdin_reader_stop_unblocks) {
  HANDLE r = nullptr, w = nullptr;
  CHECK(CreatePipe(&r, &w, nullptr, 0));
  StdinReader reader;
  reader.start(r);
  Sleep(20);  // let the thread block in ReadFile
  DWORD t0 = GetTickCount();
  reader.stop();
  CHECK(GetTickCount() - t0 < 1000);
  CloseHandle(w);
  CloseHandle(r);
}

TEST(stdin_reader_stop_wakes_waiter) {
  // A frame loop blocked in wait_ready(INFINITE) must be released by stop()
  // from another thread, and stop() must also end the blocked ReadFile.
  HANDLE r = nullptr, w = nullptr;
  CHECK(CreatePipe(&r, &w, nullptr, 0));
  StdinReader reader;
  reader.start(r);
  HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  std::thread waiter([&] {
    reader.wait_ready(INFINITE);
    SetEvent(done);
  });
  Sleep(20);
  reader.stop();
  bool woke = WaitForSingleObject(done, 2000) == WAIT_OBJECT_0;
  CHECK(woke);
  if (!woke) {  // don't hang the suite on a failure: unblock it the hard way
    CloseHandle(w);
    w = nullptr;
  }
  waiter.join();
  CloseHandle(done);
  if (w) CloseHandle(w);
  CloseHandle(r);
}

TEST(stdin_reader_absent) {
  StdinReader reader;
  reader.start(nullptr);
  CHECK(reader.kind() == StdinReader::Kind::none);
  Command c;
  CHECK(!reader.poll(c));
  CHECK(!reader.wait_ready(INFINITE));
}

TEST(write_all_detects_closed_reader) {
  HANDLE r = nullptr, w = nullptr;
  CHECK(CreatePipe(&r, &w, nullptr, 1 << 16));
  uint8_t buf[100] = {};
  CHECK(write_all(w, buf, sizeof(buf)) == WriteStatus::ok);
  CloseHandle(r);
  CHECK(write_all(w, buf, sizeof(buf)) == WriteStatus::closed);
  CloseHandle(w);
}

// ---- module probe --------------------------------------------------------------

TEST(probe_module_kinds) {
  auto image = [](const char* sig, uint16_t machine) {
    std::vector<uint8_t> b(0x80, 0);
    b[0] = 'M';
    b[1] = 'Z';
    b[0x3C] = 0x40;
    memcpy(&b[0x40], sig, 4);
    b[0x44] = uint8_t(machine);
    b[0x45] = uint8_t(machine >> 8);
    return b;
  };
  std::string pe = write_temp("pe.ad", image("PE\0\0", 0x014C));
  std::string pe64 = write_temp("pe64.ad", image("PE\0\0", 0x8664));
  std::string ne = write_temp("ne.ad", image("NE\x05\x0a", 0));
  std::vector<uint8_t> dos_bytes(0x40, 0);  // MZ with e_lfanew = 0: plain DOS
  dos_bytes[0] = 'M';
  dos_bytes[1] = 'Z';
  std::string dos = write_temp("dos.exe", dos_bytes);
  std::string txt = write_temp("hello.txt", {'h', 'e', 'l', 'l', 'o'});
  CHECK(probe_module(pe).kind == LaneKind::pe32);
  CHECK(probe_module(pe64).kind == LaneKind::unsupported);
  CHECK(probe_module(ne).kind == LaneKind::ne16);
  CHECK(probe_module(dos).kind == LaneKind::unsupported);
  CHECK(probe_module(txt).kind == LaneKind::unsupported);
  CHECK(probe_module(temp_dir() + "adw_core_test_does_not_exist.ad").kind == LaneKind::unreadable);
  for (auto& p : {pe, pe64, ne, dos, txt}) DeleteFileW(widen(p).c_str());
}

// ---- run_host in-process -----------------------------------------------------

namespace {

struct FakeSource : CommandSource {
  std::deque<Command> q;
  void add(const char* line) {
    Command c;
    parse_command(line, c);
    q.push_back(c);
  }
  void add_eof() {
    Command c;
    c.kind = Command::Kind::eof;
    q.push_back(c);
  }
  bool poll(Command& out) override {
    if (q.empty()) return false;
    out = q.front();
    q.pop_front();
    return true;
  }
  // Scripted input never arrives later: an empty queue means "ended".
  bool wait_ready(uint32_t) override { return !q.empty(); }
};

struct FakeSink : FrameSink {
  std::vector<std::vector<uint8_t>> frames;
  size_t close_after = SIZE_MAX;
  WriteStatus write(const uint8_t* d, size_t n) override {
    if (frames.size() >= close_after) return WriteStatus::closed;
    frames.emplace_back(d, d + n);
    return WriteStatus::ok;
  }
};

struct Run {
  HostResult result;
  std::vector<uint64_t> hashes;
  std::vector<uint64_t> sleeps;
};

Run run(Lane& lane, const std::map<std::string, std::string>& vars, CommandSource* src,
        FrameSink* sink) {
  Run out;
  Env env = Env::parse(vars);
  uint64_t wall = 0;
  HostIo io;
  io.commands = src;
  io.frames = sink;
  io.wall_us = [&] { return wall; };
  io.sleep_us = [&](uint64_t us) {
    out.sleeps.push_back(us);
    wall += us;
  };
  io.on_frame = [&](uint64_t, uint64_t h) { out.hashes.push_back(h); };
  out.result = run_host(lane, "--test-pattern", env, io);
  return out;
}

Run run_pattern(const std::map<std::string, std::string>& vars, CommandSource* src = nullptr,
                FrameSink* sink = nullptr) {
  auto lane = make_test_pattern_lane();
  return run(*lane, vars, src, sink);
}

std::map<std::string, std::string> with_frames(std::map<std::string, std::string> v, int n) {
  v["ADFRAMES"] = std::to_string(n);
  return v;
}

std::map<std::string, std::string> with_stream(std::map<std::string, std::string> v) {
  v["ADSTREAM"] = "1";
  return v;
}

}  // namespace

TEST(host_headless_deterministic) {
  std::map<std::string, std::string> v = {{"ADFRAMES", "12"}, {"ADSCREENW", "96"}, {"ADSCREENH", "64"}};
  Run a = run_pattern(v), b = run_pattern(v);
  CHECK_EQ(a.result.exit_code, kExitOk);
  CHECK_EQ(a.result.frames, uint64_t(12));
  CHECK(a.hashes == b.hashes);
  CHECK(std::set<uint64_t>(a.hashes.begin(), a.hashes.end()).size() == 12);  // it animates
  CHECK(a.sleeps.empty());                                                    // headless never sleeps
  v["ADSEED"] = "2";
  Run c = run_pattern(v);
  CHECK(c.hashes[0] != a.hashes[0]);  // the seed reaches the pattern
}

TEST(host_lockstep_one_frame_per_go) {
  std::map<std::string, std::string> v = {{"ADSTREAM", "1"}, {"ADSCREENW", "96"}, {"ADSCREENH", "64"}};
  Run ref = run_pattern({{"ADFRAMES", "6"}, {"ADSCREENW", "96"}, {"ADSCREENH", "64"}});
  FakeSource src;
  FakeSink sink;
  for (int i = 0; i < 3; i++) src.add("GO");
  src.add("SET 0 50");
  src.add("GO");
  src.add("MOUSE 10 10 1");
  src.add("GO");
  src.add("QUIT");
  src.add("GO");  // never reached
  Run r = run_pattern(v, &src, &sink);
  CHECK_EQ(r.result.exit_code, kExitOk);
  CHECK_EQ(r.result.frames, uint64_t(5));
  CHECK(r.result.lockstep);
  CHECK_EQ(std::string(r.result.reason), std::string("QUIT"));
  CHECK_EQ(sink.frames.size(), size_t(5));
  CHECK(r.sleeps.empty());  // lockstep never sleeps
  for (int i = 0; i < 3; i++) CHECK_EQ(r.hashes[size_t(i)], ref.hashes[size_t(i)]);
  CHECK(r.hashes[3] != ref.hashes[3]);  // SET is visible on the frame it precedes
  for (size_t i = 0; i < sink.frames.size(); i++) {
    const auto& f = sink.frames[i];
    std::string hdr = p8_header(96, 64);
    CHECK(f.size() == hdr.size() + 768 + 96 * 64);
    CHECK(memcmp(f.data(), hdr.data(), hdr.size()) == 0);
    CHECK_EQ(fnv1a64(f.data() + hdr.size(), f.size() - hdr.size()), r.hashes[i]);
  }
}

TEST(host_pattern_extreme_mouse) {
  // MOUSE takes any int32; the cursor must clip, not overflow (signed overflow
  // is UB), at both extremes. Two runs must also agree bit for bit.
  std::map<std::string, std::string> v = {{"ADSCREENW", "64"}, {"ADSCREENH", "48"}};
  auto hashes_for = [&](const char* mouse) {
    FakeSource src;
    src.add(mouse);
    src.add("GO");
    return run_pattern(v, &src).hashes;
  };
  for (const char* m : {"MOUSE 2147483647 2147483647 1", "MOUSE -2147483648 -2147483648 1"}) {
    auto a = hashes_for(m), b = hashes_for(m);
    CHECK_EQ(a.size(), size_t(1));
    CHECK(a == b);
  }
}

TEST(host_opening_lines_before_first_go) {
  // A front-end's opening "SET…, GO" is consumed before frame 0: frame 0 is
  // the reply to that GO (lockstep from the start) and already shows the SET.
  std::map<std::string, std::string> v = {{"ADSCREENW", "160"}, {"ADSCREENH", "120"}};
  Run ref = run_pattern(with_frames(v, 1));
  FakeSource src;
  FakeSink sink;
  src.add("SET 5 100");
  src.add("GO");
  src.add("GO");
  Run r = run_pattern(with_stream(v), &src, &sink);
  CHECK_EQ(sink.frames.size(), size_t(2));
  CHECK(r.result.lockstep);
  CHECK(!r.hashes.empty() && !ref.hashes.empty() && r.hashes[0] != ref.hashes[0]);
  FakeSource quit;
  quit.add("QUIT");
  Run q = run_pattern(with_stream(v), &quit, &sink);
  CHECK_EQ(q.result.frames, uint64_t(0));
  CHECK_EQ(std::string(q.result.reason), std::string("QUIT"));
}

TEST(host_eof_rules) {
  std::map<std::string, std::string> v = {{"ADSTREAM", "1"}, {"ADSCREENW", "32"}, {"ADSCREENH", "32"},
                                          {"ADNOPACE", "1"}};
  {  // EOF before any GO: keep free-running (until ADFRAMES here)
    FakeSource src;
    FakeSink sink;
    src.add_eof();
    auto vv = v;
    vv["ADFRAMES"] = "4";
    Run r = run_pattern(vv, &src, &sink);
    CHECK_EQ(r.result.frames, uint64_t(4));
    CHECK(!r.result.lockstep);
    CHECK_EQ(std::string(r.result.reason), std::string("ADFRAMES reached"));
  }
  {  // EOF after GO: exit cleanly
    FakeSource src;
    FakeSink sink;
    src.add("GO");
    src.add("GO");
    src.add_eof();
    Run r = run_pattern(v, &src, &sink);
    CHECK_EQ(r.result.exit_code, kExitOk);
    CHECK_EQ(r.result.frames, uint64_t(2));
    CHECK_EQ(std::string(r.result.reason), std::string("stdin closed after GO"));
  }
  {  // the reader closing stdout ends the host with success
    FakeSink sink;
    sink.close_after = 3;
    Run r = run_pattern(v, nullptr, &sink);
    CHECK_EQ(r.result.exit_code, kExitOk);
    CHECK_EQ(r.result.frames, uint64_t(3));
    CHECK_EQ(std::string(r.result.reason), std::string("stdout closed by the reader"));
  }
}

TEST(host_free_running_paces) {
  FakeSink sink;
  Run r = run_pattern({{"ADSTREAM", "1"}, {"ADFRAMES", "5"}, {"ADSCREENW", "32"}, {"ADSCREENH", "32"}},
                      nullptr, &sink);
  CHECK_EQ(sink.frames.size(), size_t(5));
  CHECK_EQ(r.sleeps.size(), size_t(5));
  for (uint64_t s : r.sleeps) CHECK_EQ(s, uint64_t(33333));
  FakeSink sink2;
  Run fast = run_pattern({{"ADSTREAM", "1"}, {"ADFRAMES", "5"}, {"ADSCREENW", "32"}, {"ADSCREENH", "32"},
                          {"ADPACEMS", "10"}},
                         nullptr, &sink2);
  for (uint64_t s : fast.sleeps) CHECK_EQ(s, uint64_t(10000));
  FakeSink sink3;
  Run nopace = run_pattern({{"ADSTREAM", "1"}, {"ADFRAMES", "5"}, {"ADSCREENW", "32"}, {"ADSCREENH", "32"},
                            {"ADNOPACE", "1"}},
                           nullptr, &sink3);
  CHECK(nopace.sleeps.empty());
}

TEST(host_cvset_reaches_lane_before_init) {
  struct Probe : Lane {
    int32_t seen = -1;
    const char* name() const override { return "probe"; }
    bool init(const std::string&, LaneContext& ctx) override {
      seen = ctx.input.control(4, -1);
      return true;
    }
    StepResult step() override { return StepResult::finished; }
  } lane;
  Run r = run(lane, {{"ADCVSET", "4=77"}}, nullptr, nullptr);
  CHECK_EQ(lane.seen, 77);
  CHECK_EQ(r.result.exit_code, kExitOk);
  CHECK_EQ(r.result.frames, uint64_t(0));
}

TEST(host_lane_failures) {
  struct BadInit : Lane {
    const char* name() const override { return "bad"; }
    bool init(const std::string&, LaneContext&) override { return false; }
    StepResult step() override { return StepResult::ok; }
  } bad;
  CHECK_EQ(run(bad, {}, nullptr, nullptr).result.exit_code, kExitError);
  struct BadStep : Lane {
    bool shut = false;
    const char* name() const override { return "badstep"; }
    bool init(const std::string&, LaneContext&) override { return true; }
    StepResult step() override { return StepResult::failed; }
    void shutdown() override { shut = true; }
  } bad_step;
  CHECK_EQ(run(bad_step, {}, nullptr, nullptr).result.exit_code, kExitError);
  CHECK(bad_step.shut);
}

TEST(host_clock_modes) {
  // What a lane's time reads see through the real loop: headless = frame x
  // the lane's own period (or ADPACEMS); ADSTREAM = the (here injected) wall
  // clock, which only the pacer's sleeps advance.
  struct ClockProbe : Lane {
    LaneContext* ctx = nullptr;
    VirtualClock::Mode mode = VirtualClock::Mode::fixed_step;
    std::vector<uint64_t> times;
    const char* name() const override { return "clock"; }
    bool init(const std::string&, LaneContext& c) override {
      ctx = &c;
      return true;
    }
    uint32_t frame_interval_us() const override { return 50000; }
    StepResult step() override {
      mode = ctx->clock.mode();
      times.push_back(ctx->clock.read_us());
      ctx->screen.mark_dirty();
      return StepResult::ok;
    }
  };
  ClockProbe headless, paced, streamed;
  run(headless, {{"ADFRAMES", "3"}}, nullptr, nullptr);
  CHECK(headless.mode == VirtualClock::Mode::fixed_step);
  CHECK(headless.times == (std::vector<uint64_t>{0, 50000, 100000}));
  run(paced, {{"ADFRAMES", "3"}, {"ADPACEMS", "20"}}, nullptr, nullptr);
  CHECK(paced.times == (std::vector<uint64_t>{0, 20000, 40000}));
  FakeSink sink;
  run(streamed, {{"ADFRAMES", "3"}, {"ADSTREAM", "1"}}, nullptr, &sink);
  CHECK(streamed.mode == VirtualClock::Mode::realtime);
  CHECK(streamed.times == (std::vector<uint64_t>{0, 50000, 100000}));
}

TEST(host_lane_exceptions_are_contained) {
  // The emulator reports faults as C++ exceptions. One escaping a lane must
  // end the run as exit 1 (with shutdown after a successful init), never
  // std::terminate the host.
  struct Thrower : Lane {
    int throw_in = 0;  // 1 = init, 2 = step #2, 3 = on_command
    int steps = 0;
    bool shut = false;
    const char* name() const override { return "thrower"; }
    bool init(const std::string&, LaneContext&) override {
      if (throw_in == 1) throw std::runtime_error("bad image");
      return true;
    }
    void on_command(const Command&) override {
      if (throw_in == 3) throw 42;  // not even a std::exception
    }
    StepResult step() override {
      if (throw_in == 2 && ++steps == 3) throw std::runtime_error("unmapped read at 0x0");
      return StepResult::ok;
    }
    void shutdown() override { shut = true; }
  };
  {
    Thrower t;
    t.throw_in = 1;
    Run r = run(t, {{"ADFRAMES", "5"}}, nullptr, nullptr);
    CHECK_EQ(r.result.exit_code, kExitError);
    CHECK_EQ(std::string(r.result.reason), std::string("lane init failed"));
    CHECK(!t.shut);  // init never succeeded
  }
  {
    Thrower t;
    t.throw_in = 2;
    Run r = run(t, {{"ADFRAMES", "5"}}, nullptr, nullptr);
    CHECK_EQ(r.result.exit_code, kExitError);
    CHECK_EQ(r.result.frames, uint64_t(2));
    CHECK(t.shut);
  }
  {
    Thrower t;
    t.throw_in = 3;
    FakeSource src;
    src.add("GO");
    src.add("SET 1 2");
    src.add("GO");
    Run r = run(t, {{"ADFRAMES", "5"}}, &src, nullptr);
    CHECK_EQ(r.result.exit_code, kExitError);
    CHECK_EQ(r.result.frames, uint64_t(1));
    CHECK(r.result.lockstep);
    CHECK(t.shut);
  }
}

TEST(host_unchanged_frame_resent) {
  // A lane that paints once then leaves the screen alone: the host re-sends
  // the identical cached frame, and the lane's commands arrive in order.
  struct Static : Lane {
    LaneContext* ctx = nullptr;
    int steps = 0;
    std::vector<std::string> got;
    const char* name() const override { return "static"; }
    bool init(const std::string&, LaneContext& c) override {
      ctx = &c;
      return true;
    }
    void on_command(const Command& c) override { got.push_back(c.text); }
    StepResult step() override {
      if (steps++ == 0) {
        ctx->screen.clear(7);
        ctx->screen.mark_dirty();
      }
      return StepResult::ok;
    }
  } lane;
  FakeSource src;
  FakeSink sink;
  src.add("KEY 13 1");
  src.add("GO");
  src.add("CAPS 1");
  src.add("GO");
  src.add("GO");
  Run r = run(lane, {{"ADSTREAM", "1"}, {"ADSCREENW", "8"}, {"ADSCREENH", "8"}}, &src, &sink);
  CHECK_EQ(sink.frames.size(), size_t(3));
  CHECK(sink.frames[0] == sink.frames[1] && sink.frames[1] == sink.frames[2]);
  CHECK(lane.got == (std::vector<std::string>{"KEY 13 1", "CAPS 1"}));
  CHECK_EQ(r.result.exit_code, kExitOk);
}

// ---- interaction (INTERACTION.md §3, §10) ------------------------------------

TEST(mouse_button_bitmask) {
  Command c;
  CHECK(parse_command("MOUSE 1 2 0", c) && c.c == 0);
  CHECK(parse_command("MOUSE 1 2 1", c) && c.c == 1);
  CHECK(parse_command("MOUSE 1 2 5", c) && c.c == 5);
  CHECK(parse_command("MOUSE 1 2 7", c) && c.c == 7);
  CHECK(!parse_command("MOUSE 1 2 8", c));
  CHECK(!parse_command("MOUSE 1 2 -1", c));
  CHECK_EQ(c.seq, uint64_t(0));  // the host numbers lines, never the parser
  InputState in;
  parse_command("MOUSE 3 4 2", c);
  in.apply(c);
  CHECK_EQ(in.mouse_buttons, uint32_t(kMouseRight));
  CHECK(!in.mouse_button);
  parse_command("MOUSE 3 4 5", c);
  in.apply(c);
  CHECK_EQ(in.mouse_buttons, uint32_t(kMouseLeft | kMouseMiddle));
  CHECK(in.mouse_button);
  parse_command("MOUSE 3 4 0", c);
  c.seq = 9;
  in.apply(c);
  CHECK_EQ(in.mouse_buttons, uint32_t(0));
  CHECK(!in.mouse_button);
  CHECK_EQ(in.input_seq, uint64_t(9));
  parse_command("SET 1 1", c);
  c.seq = 12;  // not an input line: never moves input_seq
  in.apply(c);
  CHECK_EQ(in.input_seq, uint64_t(9));
}

namespace {

// Records what each step saw, and every command with its seq.
struct InputProbe : Lane {
  LaneContext* ctx = nullptr;
  bool caps_at_init = false, numlock_at_init = false;
  std::vector<Command> commands;
  std::vector<std::bitset<256>> keys_at_step;
  std::vector<uint32_t> buttons_at_step;
  std::vector<uint64_t> seq_at_step;
  std::vector<bool> numlock_at_step;
  LaneStatus st;
  const char* name() const override { return "probe"; }
  bool init(const std::string&, LaneContext& c) override {
    ctx = &c;
    caps_at_init = c.input.caps;
    numlock_at_init = c.input.numlock;
    return true;
  }
  void on_command(const Command& c) override {
    commands.push_back(c);
    // A lane that eats every key while caps is on (a stand-in game).
    if (c.kind == Command::Kind::caps) st.interactive = c.a != 0;
    else if (st.interactive && c.seq) st.eaten = c.seq;
  }
  StepResult step() override {
    keys_at_step.push_back(ctx->input.keys);
    buttons_at_step.push_back(ctx->input.mouse_buttons);
    seq_at_step.push_back(ctx->input.input_seq);
    numlock_at_step.push_back(ctx->input.numlock);
    return StepResult::ok;
  }
  LaneStatus status() const override { return st; }
};

Run run_with_status(Lane& lane, const std::map<std::string, std::string>& vars, CommandSource* src,
                    std::vector<AdwHostStatusV1>* published) {
  Run out;
  Env env = Env::parse(vars);
  uint64_t wall = 0;
  HostIo io;
  io.commands = src;
  io.wall_us = [&] { return wall; };
  io.sleep_us = [&](uint64_t us) { wall += us; };
  io.on_status = [&](const AdwHostStatusV1& s) { published->push_back(s); };
  out.result = run_host(lane, "--probe", env, io);
  return out;
}

}  // namespace

TEST(input_lines_numbered) {
  InputProbe lane;
  FakeSource src;
  for (const char* l : {"SET 0 1", "KEY 65 1", "CAPS 1", "MOUSE 1 1 0", "SET 1 2", "KEY 66 1", "NUMLOCK 1", "GO", "QUIT"})
    src.add(l);
  std::vector<AdwHostStatusV1> pub;
  run_with_status(lane, {{"ADSTREAM", "1"}}, &src, &pub);
  std::vector<uint64_t> seqs;
  for (const Command& c : lane.commands) seqs.push_back(c.seq);
  CHECK((seqs == std::vector<uint64_t>{0, 1, 2, 3, 0, 4, 5}));  // NUMLOCK is an input line, as CAPS is
  CHECK_EQ(lane.seq_at_step.size(), size_t(1));
  CHECK_EQ(lane.seq_at_step[0], uint64_t(5));
  CHECK(lane.numlock_at_step.size() == 1 && lane.numlock_at_step[0]);
  // A NUMLOCK after a held release waits with it (order is kept).
  InputProbe held;
  FakeSource s2;
  for (const char* l : {"KEY 144 1", "KEY 144 0", "NUMLOCK 1", "GO", "GO", "QUIT"}) s2.add(l);
  pub.clear();
  run_with_status(held, {{"ADSTREAM", "1"}}, &s2, &pub);
  CHECK(held.numlock_at_step.size() == 2 && !held.numlock_at_step[0] && held.numlock_at_step[1]);
  CHECK(held.keys_at_step.size() == 2 && held.keys_at_step[0].test(144) && !held.keys_at_step[1].test(144));
}

TEST(held_release_seen_by_one_step) {
  // A tap (down + up) batched into one GO: exactly one step sees it down, the
  // next sees it up, and lines after the held release keep their order.
  InputProbe lane;
  FakeSource src;
  for (const char* l : {"KEY 65 1", "KEY 65 0", "KEY 66 1", "GO", "GO", "GO", "QUIT"}) src.add(l);
  std::vector<AdwHostStatusV1> pub;
  run_with_status(lane, {{"ADSTREAM", "1"}}, &src, &pub);
  CHECK_EQ(lane.keys_at_step.size(), size_t(3));
  int down_steps = 0;
  for (auto& k : lane.keys_at_step) down_steps += k.test(65) ? 1 : 0;
  CHECK_EQ(down_steps, 1);
  CHECK(lane.keys_at_step[0].test(65) && !lane.keys_at_step[0].test(66));
  CHECK(!lane.keys_at_step[1].test(65) && lane.keys_at_step[1].test(66));
  // input_applied never counts a line that has not been applied yet.
  CHECK_EQ(lane.seq_at_step[0], uint64_t(1));
  CHECK_EQ(lane.seq_at_step[1], uint64_t(3));
  CHECK(pub.size() >= 3);
  CHECK_EQ(pub[1].input_applied, uint64_t(1));
  CHECK_EQ(pub[2].input_applied, uint64_t(3));

  // A double tap in one GO: down, up (held), down (held), up (held again
  // until the second down has been seen).
  InputProbe dbl;
  FakeSource s2;
  for (const char* l : {"KEY 65 1", "KEY 65 0", "KEY 65 1", "KEY 65 0", "GO", "GO", "GO", "GO", "QUIT"}) s2.add(l);
  pub.clear();
  run_with_status(dbl, {{"ADSTREAM", "1"}}, &s2, &pub);
  std::string seen;
  for (auto& k : dbl.keys_at_step) seen += k.test(65) ? 'D' : 'u';
  CHECK_EQ(seen, std::string("DDuu"));

  // Mouse buttons too: a click in one GO shows the button for one step.
  InputProbe m;
  FakeSource s3;
  for (const char* l : {"MOUSE 5 5 1", "MOUSE 5 5 0", "GO", "GO", "QUIT"}) s3.add(l);
  pub.clear();
  run_with_status(m, {{"ADSTREAM", "1"}}, &s3, &pub);
  CHECK_EQ(m.buttons_at_step.size(), size_t(2));
  CHECK_EQ(m.buttons_at_step[0], uint32_t(kMouseLeft));
  CHECK_EQ(m.buttons_at_step[1], uint32_t(0));

  // A release after a step has seen the down is not held.
  InputProbe late;
  FakeSource s4;
  for (const char* l : {"KEY 65 1", "GO", "KEY 65 0", "GO", "QUIT"}) s4.add(l);
  pub.clear();
  run_with_status(late, {{"ADSTREAM", "1"}}, &s4, &pub);
  CHECK(late.keys_at_step[0].test(65) && !late.keys_at_step[1].test(65));
}

TEST(adcaps_visible_at_init) {
  InputProbe on, off;
  std::vector<AdwHostStatusV1> pub;
  run_with_status(on, {{"ADCAPS", "1"}, {"ADFRAMES", "1"}}, nullptr, &pub);
  run_with_status(off, {{"ADFRAMES", "1"}}, nullptr, &pub);
  CHECK(on.caps_at_init);
  CHECK(!off.caps_at_init);
  CHECK(Env::parse({{"ADCAPS", "1"}}).caps_at_start);
  CHECK(!Env::parse({{"ADCAPS", "0"}}).caps_at_start);
  CHECK(!on.numlock_at_init);  // ADCAPS is Caps Lock's alone
}

TEST(adnumlock_visible_at_init) {
  // Final Exam latches Num Lock's toggle as it starts, and a change starts its exam.
  InputProbe on, off;
  std::vector<AdwHostStatusV1> pub;
  run_with_status(on, {{"ADNUMLOCK", "1"}, {"ADFRAMES", "1"}}, nullptr, &pub);
  run_with_status(off, {{"ADFRAMES", "1"}}, nullptr, &pub);
  CHECK(on.numlock_at_init && !on.caps_at_init);
  CHECK(!off.numlock_at_init);
  CHECK(Env::parse({{"ADNUMLOCK", "1"}}).numlock_at_start);
  CHECK(!Env::parse({{"ADNUMLOCK", "0"}}).numlock_at_start);
  CHECK(!Env::parse({}).numlock_at_start);
}

TEST(status_published_after_init_and_each_step) {
  InputProbe lane;
  FakeSource src;
  for (const char* l : {"GO", "CAPS 1", "GO", "KEY 65 1", "GO", "CAPS 0", "KEY 66 1", "GO", "QUIT"}) src.add(l);
  std::vector<AdwHostStatusV1> pub;
  run_with_status(lane, {{"ADSTREAM", "1"}}, &src, &pub);
  CHECK_EQ(pub.size(), size_t(5));  // init + 4 steps
  if (pub.size() == 5) {
    CHECK_EQ(pub[0].frames, uint64_t(0));
    CHECK_EQ(pub[0].flags, ADWS_READY);
    CHECK_EQ(pub[0].lane, uint32_t(0));  // "probe" is no known lane
    CHECK_EQ(pub[1].frames, uint64_t(1));
    CHECK_EQ(pub[2].flags, ADWS_READY | ADWS_INTERACTIVE);
    CHECK_EQ(pub[2].input_applied, uint64_t(1));
    CHECK_EQ(pub[3].input_eaten, uint64_t(2));  // KEY 65 while interactive
    CHECK_EQ(pub[4].flags, ADWS_READY);
    CHECK_EQ(pub[4].input_applied, uint64_t(4));
    CHECK_EQ(pub[4].input_eaten, uint64_t(2));  // KEY 66 after CAPS 0 is not eaten
    for (auto& s : pub) CHECK(s.magic == kStatusMagic && s.version == 1 && s.size == 64);
  }
  // status_lane_id
  CHECK_EQ(status_lane_id(*make_test_pattern_lane()), kStatusLaneTest);
}

namespace {
// A lane whose guest takes a key from a queue one step after it arrives (the
// Classic lane's saver-window queue while a long DRAWFRAME is suspended):
// until then the key is unsettled.
struct QueueProbe : InputProbe {
  std::vector<uint64_t> fresh, waiting;
  void on_command(const Command& c) override {
    InputProbe::on_command(c);
    if (c.kind == Command::Kind::key && c.seq) fresh.push_back(c.seq);
    update();
  }
  StepResult step() override {
    InputProbe::step();
    for (uint64_t s : waiting) st.eaten = std::max(st.eaten, s);
    waiting = fresh;
    fresh.clear();
    update();
    return StepResult::ok;
  }
  void update() {
    st.unsettled = !waiting.empty() ? waiting.front() : !fresh.empty() ? fresh.front() : 0;
  }
};
}  // namespace

TEST(status_applied_stays_below_unsettled_input) {
  // Published input_applied never reaches a line the guest may still take, so
  // the front-end waits for its verdict instead of reading "applied, not eaten".
  QueueProbe lane;
  FakeSource src;
  for (const char* l : {"GO", "KEY 65 1", "GO", "GO", "KEY 65 0", "KEY 66 1", "GO", "GO", "QUIT"}) src.add(l);
  std::vector<AdwHostStatusV1> pub;
  run_with_status(lane, {{"ADSTREAM", "1"}}, &src, &pub);
  CHECK_EQ(pub.size(), size_t(6));  // init + 5 steps
  if (pub.size() == 6) {
    CHECK_EQ(pub[1].input_applied, uint64_t(0));
    CHECK_EQ(pub[2].input_applied, uint64_t(0));  // KEY 65 applied, still queued
    CHECK_EQ(pub[2].input_eaten, uint64_t(0));
    CHECK_EQ(pub[3].input_applied, uint64_t(1));  // taken a step later: settled and eaten
    CHECK_EQ(pub[3].input_eaten, uint64_t(1));
    CHECK_EQ(pub[4].input_applied, uint64_t(1));  // lines 2 and 3 queued
    CHECK_EQ(pub[5].input_applied, uint64_t(3));
    CHECK_EQ(pub[5].input_eaten, uint64_t(3));
    for (size_t i = 1; i < pub.size(); i++) CHECK(pub[i].input_applied >= pub[i - 1].input_applied);
  }
}

TEST(status_record_seqlock_round_trip) {
  // A pagefile section, as the front-end creates it; the writer hammers it
  // while the reader checks that every copy it accepts is consistent.
  HANDLE sec = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, kStatusSectionBytes, nullptr);
  CHECK(sec != nullptr);
  if (!sec) return;
  StatusPublisher pub;
  CHECK(pub.open_handle(sec));
  void* view = MapViewOfFile(sec, FILE_MAP_READ, 0, 0, sizeof(AdwHostStatusV1));
  CHECK(view != nullptr);
  AdwHostStatusV1 out{};
  CHECK(!read_status(view, &out));  // nothing published yet
  // The writer starts once the reader is in its loop and writes on (at least
  // 200000 records) until the reader has checked 1000 copies or 10 s pass: on
  // a busy machine it could otherwise finish before the reader ever looked.
  std::atomic<bool> done{false}, reading{false}, enough{false};
  std::atomic<uint64_t> written{0};
  std::thread writer([&] {
    while (!reading.load()) std::this_thread::yield();
    const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    uint64_t i = 0;
    for (;;) {
      i++;
      LaneStatus s;
      s.interactive = (i & 1) != 0;
      s.eaten = i;
      s.source = uint32_t(i & 0xFFFF);
      pub.publish(s, i, i, uint32_t(i & 0xFFFF));
      if (i >= 200000 && (enough.load() || std::chrono::steady_clock::now() > give_up)) break;
    }
    written = i;
    done = true;
  });
  uint64_t reads = 0, torn = 0, last = 0, backwards = 0;
  reading = true;
  while (!done.load()) {
    if (!read_status(view, &out)) continue;
    reads++;
    if (!(out.frames == out.input_applied && out.frames == out.input_eaten && out.source == (out.frames & 0xFFFF) &&
          out.lane == (out.frames & 0xFFFF) && ((out.flags & ADWS_INTERACTIVE) != 0) == ((out.frames & 1) != 0)))
      torn++;
    if (out.frames < last) backwards++;
    last = out.frames;
    if (reads >= 1000) enough = true;
  }
  writer.join();
  CHECK(read_status(view, &out));
  CHECK_EQ(out.frames, written.load());
  CHECK_EQ(torn, uint64_t(0));
  CHECK_EQ(backwards, uint64_t(0));
  CHECK(reads > 0);
  CHECK_EQ(out.gen % 2, uint32_t(0));
  UnmapViewOfFile(view);
  CloseHandle(sec);

  // A writer that died mid-write (gen odd) makes the reader give up rather
  // than return a torn copy.
  AdwHostStatusV1 fake{};
  fake.magic = kStatusMagic;
  fake.version = 1;
  fake.gen = 3;
  CHECK(!read_status(&fake, &out));
}

TEST(status_publisher_env) {
  // ADSTATUSHANDLE names an inherited handle; this process's own works too.
  HANDLE sec = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, kStatusSectionBytes, nullptr);
  CHECK(sec != nullptr);
  StatusPublisher a;
  CHECK(a.open(Env::parse({{"ADSTATUSHANDLE", std::to_string(uintptr_t(sec))}})));
  StatusPublisher b;
  char hex[32];
  snprintf(hex, sizeof(hex), "0x%llx", (unsigned long long)uintptr_t(sec));
  CHECK(b.open(Env::parse({{"ADSTATUSHANDLE", hex}})));
  StatusPublisher bad;
  CHECK(!bad.open(Env::parse({{"ADSTATUSHANDLE", "junk"}})));
  CHECK(!bad.open(Env::parse({{"ADSTATUSHANDLE", "0x7ffffff0"}})));  // not a handle of ours
  CHECK(!bad.active());
  StatusPublisher log_only;
  log_only.open(Env::parse({{"ADSTATUSLOG", "1"}}));
  CHECK(log_only.active() && !log_only.mapped());
  CloseHandle(sec);

  AdwHostStatusV1 s{};
  s.frames = 12;
  s.flags = ADWS_READY | ADWS_INTERACTIVE;
  s.input_applied = 7;
  s.input_eaten = 6;
  s.source = 1;
  CHECK_EQ(format_status_line(s), std::string("STATUS 12 flags=0x21 applied=7 eaten=6 src=1"));
  LaneStatus ls;
  ls.cursor = ls.rotate_ok = ls.key_filter = ls.wake = true;
  CHECK_EQ(status_flags(ls, false), ADWS_CURSOR | ADWS_ROTATE_OK | ADWS_KEY_FILTER | ADWS_WAKE);
}

TEST(test_pattern_interactive) {
  // ADTESTINTERACTIVE=1: CAPS 1 sets interactive, keys are eaten while it is.
  auto lane = make_test_pattern_lane();
  FakeSource src;
  for (const char* l : {"KEY 65 1", "GO", "CAPS 1", "GO", "KEY 66 1", "GO", "CAPS 0", "KEY 67 1", "GO", "QUIT"})
    src.add(l);
  std::vector<AdwHostStatusV1> pub;
  run_with_status(*lane, {{"ADSTREAM", "1"}, {"ADTESTINTERACTIVE", "1"}, {"ADSCREENW", "64"}, {"ADSCREENH", "48"}},
                  &src, &pub);
  CHECK_EQ(pub.size(), size_t(5));
  if (pub.size() == 5) {
    CHECK(!(pub[1].flags & ADWS_INTERACTIVE));
    CHECK((pub[2].flags & ADWS_INTERACTIVE) && pub[2].source == kStatusSourceAd4);
    CHECK_EQ(pub[3].input_eaten, uint64_t(3));
    CHECK(!(pub[4].flags & ADWS_INTERACTIVE));
    CHECK_EQ(pub[4].input_eaten, uint64_t(3));
    CHECK_EQ(pub[4].lane, kStatusLaneTest);
  }
  // Without the knob it never goes interactive.
  auto plain = make_test_pattern_lane();
  FakeSource s2;
  for (const char* l : {"CAPS 1", "GO", "KEY 66 1", "GO", "QUIT"}) s2.add(l);
  pub.clear();
  run_with_status(*plain, {{"ADSTREAM", "1"}, {"ADSCREENW", "64"}, {"ADSCREENH", "48"}}, &s2, &pub);
  for (auto& s : pub) CHECK(!(s.flags & ADWS_INTERACTIVE) && s.input_eaten == 0);
}

TEST(configure_driver_exit_codes) {
  struct Cfg : Lane {
    bool supported = true;
    ConfigureResult result = ConfigureResult::shown;
    bool throws = false;
    std::string json;
    ConfigureRequest seen_req;
    int32_t seen_cv = -1;
    bool seen_caps = false, seen_numlock = false;
    std::string seen_state;
    const char* name() const override { return "cfg"; }
    bool init(const std::string&, LaneContext&) override { return true; }
    StepResult step() override { return StepResult::finished; }
    bool can_configure() const override { return supported; }
    ConfigureResult configure(const std::string&, LaneContext& ctx, const ConfigureRequest& req,
                              std::string* out) override {
      seen_req = req;
      seen_cv = ctx.input.control(2, -1);
      seen_caps = ctx.input.caps;
      seen_numlock = ctx.input.numlock;
      seen_state = ctx.env.state_root;
      if (throws) throw std::runtime_error("boom");
      if (!json.empty()) *out = json;
      return result;
    }
  };
  Env env = Env::parse({{"ADCVSET", "2=40"}, {"ADCAPS", "1"}, {"ADNUMLOCK", "1"}, {"ADSTATE", "C:\\st"}});
  ConfigureRequest req;
  req.slot = 3;
  req.owner = 0x1234;
  std::string line;
  Cfg shown;
  CHECK_EQ(configure_module(shown, "m.ad", env, req, &line), 0);
  CHECK_EQ(shown.seen_req.slot, 3);
  CHECK_EQ(shown.seen_req.owner, uint64_t(0x1234));
  CHECK_EQ(shown.seen_cv, 40);
  CHECK(shown.seen_caps);
  CHECK(shown.seen_numlock);
  CHECK_EQ(shown.seen_state, std::string("C:\\st"));
  CHECK(line.find("\"result\":\"ok\"") != std::string::npos);
  Cfg nothing;
  nothing.result = ConfigureResult::nothing;
  CHECK_EQ(configure_module(nothing, "m.ad", env, req, &line), 4);
  CHECK(line.find("\"result\":\"nothing\"") != std::string::npos);
  Cfg unsupported;
  unsupported.supported = false;
  CHECK_EQ(configure_module(unsupported, "m.ad", env, req, &line), 5);
  CHECK(unsupported.seen_req.slot == -1);  // never called
  Cfg failed;
  failed.throws = true;
  CHECK_EQ(configure_module(failed, "m.ad", env, req, &line), 1);
  CHECK(line.find("\"result\":\"error\"") != std::string::npos);
  Cfg own;
  own.json = "{\"result\":\"ok\",\n\"dialogs\":2}";
  CHECK_EQ(configure_module(own, "m.ad", env, req, &line), 0);
  CHECK_EQ(line, std::string("{\"result\":\"ok\", \"dialogs\":2}"));  // one line

  CHECK_EQ(configure_json(ConfigureResult::shown, 2, "a \"q\"\\\n", {"C:\\WINDOWS\\MODULES.INI"}),
           std::string("{\"result\":\"ok\",\"dialogs\":2,\"message\":\"a \\\"q\\\"\\\\\\n\","
                       "\"written\":[\"C:\\\\WINDOWS\\\\MODULES.INI\"]}"));
  CHECK_EQ(configure_exit_code(ConfigureResult::nothing), 4);
  CHECK_EQ(configure_exit_code(ConfigureResult::unsupported), 5);
  CHECK_EQ(configure_exit_code(ConfigureResult::failed), 1);
}

TEST(state_root_resolution) {
  // ADSTATE unset / :memory: = in memory; a directory = persistent;
  // --configure's default only replaces "unset".
  CHECK_EQ(Env::parse({}).state_root, std::string());
  CHECK(!Env::parse({}).state_persistent());
  CHECK_EQ(Env::parse({{"ADSTATE", ":memory:"}}).state_root, std::string());
  CHECK_EQ(Env::parse({{"ADSTATE", " D:\\st\\ "}}).state_root, std::string("D:\\st"));
  CHECK_EQ(Env::parse({{"ADSTATE", "D:\\st"}}).state_root, std::string("D:\\st"));
  Env e = Env::parse({{"LOCALAPPDATA", "C:\\Users\\me\\AppData\\Local"}});
  e.use_configure_state_default();
  CHECK_EQ(e.state_root, std::string("C:\\Users\\me\\AppData\\Local\\LongAfterDark\\state"));
  Env m = Env::parse({{"LOCALAPPDATA", "C:\\L"}, {"ADSTATE", ":memory:"}});
  m.use_configure_state_default();
  CHECK_EQ(m.state_root, std::string());
  Env d = Env::parse({{"LOCALAPPDATA", "C:\\L"}, {"ADSTATE", "E:\\x"}});
  d.use_configure_state_default();
  CHECK_EQ(d.state_root, std::string("E:\\x"));
  Env n = Env::parse({});
  n.use_configure_state_default();
  CHECK_EQ(n.state_root, std::string());  // no LOCALAPPDATA: memory, as before

  // Package names (§7.1).
  Env s = Env::parse({{"ADSTATE", "S:\\state"}});
  CHECK_EQ(package_state_name(s, "C:\\a\\win\\FILES\\AD40\\TOASTERS.AD"), std::string("deluxe"));
  CHECK_EQ(package_state_name(s, "C:/a/win/files/classic/TUNNEL.AD"), std::string("deluxe"));
  CHECK_EQ(package_state_name(s, "FILES/AD40/TOASTERS.AD"), std::string("deluxe"));
  CHECK_EQ(package_state_name(s, "C:\\a\\win\\packages\\ad10\\AD10TH\\X.AD"), std::string("ad10"));
  CHECK_EQ(package_state_name(s, "C:\\a\\win\\Packages\\TT\\TWISTED\\X.AD"), std::string("tt"));
  CHECK_EQ(package_state_name(s, "packages/simpsons/SIMPSONS/X.AD"), std::string("simpsons"));
  std::string legacy = package_state_name(s, "C:\\Some\\Where\\X.AD");
  CHECK(legacy.size() == 15 && legacy.compare(0, 7, "legacy-") == 0);
  CHECK_EQ(package_state_name(s, "c:\\some\\where\\Y.AD"), legacy);  // per dir, case-insensitive
  CHECK(package_state_name(s, "C:\\Some\\Else\\X.AD") != legacy);
  CHECK_EQ(package_state_dir(s, "C:\\a\\win\\FILES\\AD40\\X.AD"), std::string("S:\\state\\deluxe"));
  CHECK_EQ(package_state_dir(Env::parse({}), "C:\\a\\win\\FILES\\AD40\\X.AD"), std::string());
  // A package component that is not an id never names a state folder: ".."
  // would put the module's state beside the state root (in the data folder).
  std::string dotdot = package_state_name(s, "C:\\x\\packages\\..\\AD32\\GUTS.AD");
  CHECK(dotdot.compare(0, 7, "legacy-") == 0);
  CHECK(package_state_dir(s, "C:\\x\\packages\\..\\AD32\\GUTS.AD").find("..") == std::string::npos);
  CHECK(package_state_name(s, "C:\\x\\packages\\.\\AD32\\GUTS.AD").compare(0, 7, "legacy-") == 0);
  CHECK(package_state_name(s, "C:\\x\\packages\\my pack\\AD32\\GUTS.AD").compare(0, 7, "legacy-") == 0);
  CHECK(package_state_name(s, "C:\\x\\packages\\a..b\\AD32\\GUTS.AD").compare(0, 7, "legacy-") == 0);
  CHECK(package_state_name(s, "../AD32/GUTS.AD").compare(0, 7, "legacy-") == 0);
  // "." and ".." elsewhere in the path resolve before the layout is read.
  CHECK_EQ(package_state_name(s, "C:\\a\\win\\packages\\ad32\\.\\AD32\\X.AD"), std::string("ad32"));
  CHECK_EQ(package_state_name(s, "C:\\a\\win\\packages\\ad32\\AD32\\sub\\..\\X.AD"), std::string("ad32"));
  CHECK_EQ(package_state_name(s, "C:\\a\\win\\FILES\\x\\..\\AD40\\X.AD"), std::string("deluxe"));
  CHECK_EQ(package_state_name(s, "packages/ad-1_b/DIR/X.AD"), std::string("ad-1_b"));
}

int main(int argc, char** argv) { return adw_test::run_all(argc > 1 ? argv[1] : nullptr); }
