// Unit tests for the window-free parts of LongAfterDark.scr.
//   scr_unit <suite>… [--fakeimport <exe>]
//     suites: args parser settings catalog geometry rotation convert env layout dialog ui input seed releases
//     paths sound
//     [--fakehost <exe>]  (ui: the lane probe runs against it)
#include <windows.h>
#include <shlobj.h>

#include <phosg/JSON.hh>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <random>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "args.h"
#include "catalog.h"
#include "desktop_seed.h"
#include "dialog_support.h"
#include "frame_parser.h"
#include "geometry.h"
#include "host_process.h"
#include "input_rules.h"
#include "log.h"
#include "paths.h"
#include "present.h"
#include "releases.h"
#include "settings.h"
#include "sound.h"
#include "ui_model.h"
#include "adw/core/data_root.h"
#include "adw/ui/theme.h"

using namespace adw::scr;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                          \
    }                                                                        \
  } while (0)

#define CHECK_EQ(a, b)                                                                      \
  do {                                                                                      \
    auto _a = (a);                                                                          \
    auto _b = (b);                                                                          \
    if (!(_a == _b)) {                                                                      \
      fprintf(stderr, "%s:%d: CHECK_EQ failed: %s != %s\n", __FILE__, __LINE__, #a, #b);    \
      ++g_failures;                                                                         \
    }                                                                                       \
  } while (0)

std::string fixture(const char* name) {
  std::string text;
  CHECK(read_file(widen(std::string(AD_SCR_FIXTURES) + "/" + name), text));
  return text;
}

// ---- args ------------------------------------------------------------------------

Args A(std::initializer_list<const wchar_t*> v) {
  std::vector<std::wstring> argv;
  for (auto* s : v) argv.emplace_back(s);
  return parse_args(argv);
}

void test_args() {
  Args a = A({});
  CHECK(a.mode == Mode::settings && !a.has_hwnd && a.valid);

  for (auto* s : {L"/s", L"/S", L"-s", L"-S"}) CHECK(A({s}).mode == Mode::run);

  a = A({L"/p", L"1234"});
  CHECK(a.mode == Mode::preview && a.has_hwnd && a.hwnd == 1234 && a.valid);
  for (auto* s : {L"/p:1234", L"/P1234", L"-p=1234", L"/p:1234 "}) {
    a = A({s});
    CHECK(a.mode == Mode::preview && a.hwnd == 1234 && a.valid);
  }
  a = A({L"/p"});
  CHECK(a.mode == Mode::preview && !a.valid);
  a = A({L"/p", L"notanumber"});
  CHECK(a.mode == Mode::preview && !a.valid);
  a = A({L"/p", L"-1234"});
  CHECK(a.hwnd == (uintptr_t)(intptr_t)-1234 && a.valid);
  a = A({L"/p", L"0x1A"});
  CHECK(a.hwnd == 26);

  a = A({L"/c:5678"});
  CHECK(a.mode == Mode::settings && a.has_hwnd && a.hwnd == 5678);
  a = A({L"/C", L"99"});
  CHECK(a.mode == Mode::settings && a.hwnd == 99);
  a = A({L"/c"});
  CHECK(a.mode == Mode::settings && !a.has_hwnd && a.valid);

  a = A({L"/a", L"12"});
  CHECK(a.mode == Mode::password && a.hwnd == 12);

  CHECK(A({L"/x"}).mode == Mode::settings);
  CHECK(A({L"junk", L"/s"}).mode == Mode::run);
  CHECK(A({L"/x", L"/s"}).mode == Mode::run);
  CHECK(A({L"/"}).mode == Mode::settings);
  // "/s 1234": a stray number after /s is harmless.
  a = A({L"/s", L"1234"});
  CHECK(a.mode == Mode::run && a.valid);
}

// ---- frame parser ------------------------------------------------------------------

std::string p8(int w, int h, uint8_t seed, const std::string& header = {}) {
  std::string s = header.empty() ? "P8\n" + std::to_string(w) + " " + std::to_string(h) + "\n" : header;
  for (int i = 0; i < 768; ++i) s.push_back((char)(uint8_t)(i * 7 + seed));
  for (int i = 0; i < w * h; ++i) s.push_back((char)(uint8_t)(i + seed));
  return s;
}

std::string p6(int w, int h, uint8_t seed) {
  std::string s = "P6\n" + std::to_string(w) + " " + std::to_string(h) + "\n255\n";
  for (int i = 0; i < w * h * 3; ++i) s.push_back((char)(uint8_t)(i * 3 + seed));
  return s;
}

bool frame_matches_p8(const RawFrame& f, int w, int h, uint8_t seed) {
  if (f.format != 8 || f.width != w || f.height != h || f.palette.size() != 768 || f.pixels.size() != (size_t)w * h)
    return false;
  for (int i = 0; i < 768; ++i) if (f.palette[i] != (uint8_t)(i * 7 + seed)) return false;
  for (int i = 0; i < w * h; ++i) if (f.pixels[i] != (uint8_t)(i + seed)) return false;
  return true;
}

void test_parser() {
  RawFrame f;
  {  // one whole frame
    FrameParser p;
    std::string s = p8(4, 2, 1);
    p.feed(s.data(), s.size());
    CHECK(p.take(f));
    CHECK(frame_matches_p8(f, 4, 2, 1));
    CHECK(!p.take(f));
    CHECK_EQ(p.buffered(), (size_t)0);
  }
  {  // byte at a time: the frame appears exactly when its last byte arrives
    FrameParser p;
    std::string s = p8(5, 3, 9);
    int got = 0;
    for (size_t i = 0; i < s.size(); ++i) {
      p.feed(&s[i], 1);
      bool t = p.take(f);
      if (t) {
        ++got;
        CHECK_EQ(i, s.size() - 1);
      }
    }
    CHECK_EQ(got, 1);
    CHECK(frame_matches_p8(f, 5, 3, 9));
  }
  {  // several concatenated frames, mixed formats, in one feed
    FrameParser p;
    std::string s = p8(4, 2, 1) + p6(3, 2, 5) + p8(8, 1, 2);
    p.feed(s.data(), s.size());
    CHECK(p.take(f) && frame_matches_p8(f, 4, 2, 1));
    CHECK(p.take(f) && f.format == 6 && f.width == 3 && f.height == 2 && f.pixels.size() == 18 && f.palette.empty());
    CHECK(f.pixels[0] == 5 && f.pixels[17] == (uint8_t)(17 * 3 + 5));
    CHECK(p.take(f) && frame_matches_p8(f, 8, 1, 2));
    CHECK(!p.take(f));
  }
  {  // '#' comments and extra whitespace between tokens (both allowed)
    FrameParser p;
    std::string s = p8(4, 2, 3, "P8 \t\n# made by a host\n  4\n# width above\n2\n");
    p.feed(s.data(), s.size());
    CHECK(p.take(f) && frame_matches_p8(f, 4, 2, 3));
  }
  {  // exactly ONE delimiter byte precedes the body: after "2\r" the '\n' is body
    FrameHeader hd;
    std::string s = "P8\r\n4 2\r\nXYZ";
    CHECK(parse_frame_header((const uint8_t*)s.data(), s.size(), hd) == HeaderResult::ok);
    CHECK_EQ(hd.body_start, (size_t)8);
    CHECK_EQ(s[hd.body_start], '\n');
  }
  {  // incomplete headers need more bytes, never fail
    FrameHeader hd;
    for (std::string s : {"", "P", "P8", "P8\n", "P8\n64", "P8\n64 48", "P6\n4 4\n25", "P8\n# comment without end"}) {
      CHECK(parse_frame_header((const uint8_t*)s.data(), s.size(), hd) == HeaderResult::need_more);
    }
  }
  {  // invalid headers
    FrameHeader hd;
    for (std::string s : {"P5\n4 4\n", "P8\n0 4\n", "P8\n4 -1\n", "P8\n20000 4\n", "P8\n4x 4\n", "P6\n4 4\n256\n",
                          "P6\n4 4\n0\n", "hello world\n"}) {
      CHECK(parse_frame_header((const uint8_t*)s.data(), s.size(), hd) == HeaderResult::invalid);
    }
  }
  {  // implausible sizes are rejected, and as soon as the height is in (a P6
     // header need not reach its maxval for that)
    FrameHeader hd;
    for (std::string s : {"P8\n8193 4\n", "P8\n4 8193\n", "P8\n8192 2049\n", "P8\n16000 16000\n", "P6\n4097 4097 ",
                          "P6\n9999 9999\n25"}) {
      CHECK(parse_frame_header((const uint8_t*)s.data(), s.size(), hd) == HeaderResult::invalid);
    }
    // The largest the saver ever asks for (Scale 4.0 on an ultrawide), and
    // the cap itself, are still frames.
    for (std::string s : {"P8\n5120 1920\n", "P8\n4096 4096\n", "P6\n8192 2048\n255\n", "P8\n1 8192\n"}) {
      CHECK(parse_frame_header((const uint8_t*)s.data(), s.size(), hd) == HeaderResult::ok);
    }
  }
  {  // a stray header that claims a gigantic frame costs one resync, not a
     // stall: the real frame right behind it comes out at once
    FrameParser p;
    std::string s = "P8\n16000 16000\n" + p8(4, 2, 1) + "P6 30000 2\n255\n" + p8(3, 3, 2);
    p.feed(s.data(), s.size());
    CHECK(p.take(f) && frame_matches_p8(f, 4, 2, 1));
    CHECK(p.take(f) && frame_matches_p8(f, 3, 3, 2));
    CHECK(!p.take(f));
    CHECK_EQ(p.buffered(), (size_t)0);
    CHECK(p.resyncs() >= 2);
  }
  {  // garbage before and between frames is skipped (a stray printf on stdout)
    FrameParser p;
    std::string s = "hello from a module\n" + p8(4, 2, 1) + "Pxx junk P" + p8(2, 2, 4);
    p.feed(s.data(), s.size());
    CHECK(p.take(f) && frame_matches_p8(f, 4, 2, 1));
    CHECK(p.take(f) && frame_matches_p8(f, 2, 2, 4));
    CHECK(p.resyncs() >= 2);
  }
  {  // a partial trailing 'P' is kept for the next feed
    FrameParser p;
    std::string s = "junk P";
    p.feed(s.data(), s.size());
    CHECK(!p.take(f));
    std::string rest = p8(3, 1, 7).substr(1);
    p.feed(rest.data(), rest.size());
    CHECK(p.take(f) && frame_matches_p8(f, 3, 1, 7));
  }
  {  // realistic size, random chunking
    FrameParser p;
    std::string s;
    for (int k = 0; k < 4; ++k) s += p8(856, 480, (uint8_t)k);
    std::mt19937 rng(42);
    size_t pos = 0;
    int got = 0;
    while (pos < s.size()) {
      size_t n = std::min<size_t>(s.size() - pos, 1 + rng() % 70000);
      p.feed(s.data() + pos, n);
      pos += n;
      while (p.take(f)) {
        CHECK(frame_matches_p8(f, 856, 480, (uint8_t)got));
        ++got;
      }
    }
    CHECK_EQ(got, 4);
    CHECK_EQ(p.resyncs(), (uint64_t)0);
  }
}

// ---- settings ----------------------------------------------------------------------

void test_settings() {
  Settings d = parse_settings("");
  CHECK(d == Settings{});
  CHECK(d.is_random() && d.duration_min == 5 && d.scale == 1.0 && d.all_monitors);

  Settings s = parse_settings(fixture("settings.ini"));
  CHECK_EQ(s.module, std::string("test.rings"));
  CHECK(!s.is_random());
  CHECK(s.rotates());   // a named module plus a Randomize list: it plays first, then the list
  CHECK((s.randomize == std::vector<std::string>{"test.rings", "test.stripes"}));
  CHECK_EQ(s.duration_min, 5);
  CHECK_EQ(s.scale, 1.0);
  CHECK(s.all_monitors);
  CHECK((s.controls["test.rings"] == std::map<int, int>{{0, 75}, {1, 0}, {2, 2}}));
  CHECK_EQ(s.controls.size(), (size_t)1);

  // Round trip: parse(serialize(x)) == x, from scratch and on top of the fixture.
  Settings x;
  x.module = "random";
  x.randomize = {"a.one", "b.two", "c.three"};
  x.duration_min = 0;
  x.scale = 1.5;
  x.all_monitors = false;
  x.controls["a.one"] = {{0, 5}, {3, -2}, {15, 100}};
  x.controls["weird.id.with.dots"] = {{1, 1}};
  CHECK(parse_settings(serialize_settings(x)) == x);
  std::string on_fixture = serialize_settings(x, fixture("settings.ini"));
  CHECK(parse_settings(on_fixture) == x);
  // ...and the fixture's unknown content survives the rewrite.
  IniFile ini;
  ini.parse(on_fixture);
  CHECK(ini.get("Saver", "FutureKey") && *ini.get("Saver", "FutureKey") == "keep me");
  CHECK(ini.get("Extra", "Unknown") && *ini.get("Extra", "Unknown") == "1");
  CHECK(on_fixture.find("; a section this version doesn't know about") != std::string::npos);
  CHECK(on_fixture.find("; Long After Dark settings (test fixture)") != std::string::npos);
  CHECK(!ini.get("Module.test.rings", "0"));   // the old control section is gone (x has none for it)
  CHECK(on_fixture.find("Module=random\r\n") != std::string::npos);
  CHECK(on_fixture.find("Scale=1.5\r\n") != std::string::npos);
  CHECK(on_fixture.find("Monitors=primary\r\n") != std::string::npos);
  CHECK(on_fixture.find("DurationMin=0\r\n") != std::string::npos);
  CHECK(on_fixture.find("[Module.a.one]\r\n0=5\r\n3=-2\r\n15=100\r\n") != std::string::npos);

  // Idempotent: serializing the parsed fixture onto itself changes nothing semantic.
  Settings fs = parse_settings(fixture("settings.ini"));
  CHECK(parse_settings(serialize_settings(fs, fixture("settings.ini"))) == fs);
  // A control section that stays is rewritten where it was, not moved to the end.
  fs.controls["test.rings"][0] = 30;
  std::string kept = serialize_settings(fs, fixture("settings.ini"));
  CHECK(kept.find("[Module.test.rings]\r\n0=30\r\n1=0\r\n2=2\r\n\r\n[Extra]") != std::string::npos);
  CHECK(parse_settings(kept) == fs);

  // Leniency.
  Settings l = parse_settings("\xEF\xBB\xBF[saver]\r\nmodule=\r\nSCALE = 1.5 \r\nmonitors=PRIMARY\r\nDurationMin=-3\r\n"
                              "[Module.x]\r\nfoo=1\r\n2=abc\r\n4=9\r\n[Module.]\r\n1=1\r\n");
  CHECK(l.is_random() && l.module == "random");
  CHECK_EQ(l.scale, 1.5);
  CHECK(!l.all_monitors);
  CHECK_EQ(l.duration_min, 0);
  CHECK((l.controls == std::map<std::string, std::map<int, int>>{{"x", {{4, 9}}}}));
  CHECK(parse_settings("[Saver]\nModule=RANDOM\n").is_random());
  CHECK(parse_settings("[Saver]\nModule=random\n").rotates());
  CHECK(!parse_settings("[Saver]\nModule=a.b\nRandomize=\n").rotates());
  CHECK(parse_settings("[Saver]\nModule=a.b\nRandomize=c.d\n").rotates());
  CHECK(parse_settings("").rotates());   // no file: Random over everything
  // A nameless "[]" section (as GetPrivateProfileString reads one) survives
  // a rewrite: what follows it stays out of [Saver].
  {
    std::string odd = "[Saver]\r\nModule=a.b\r\n[]\r\nDurationMin=9\r\n";
    Settings o = parse_settings(odd);
    CHECK_EQ(o.duration_min, 5);
    std::string again = serialize_settings(o, odd);
    CHECK(again.find("[]\r\n") != std::string::npos);
    CHECK_EQ(parse_settings(again).duration_min, 5);
  }
  CHECK(parse_settings("[Saver]\nScale=abc\n").scale == 1.0);
  CHECK(parse_settings("[Saver]\nScale=9\n").scale == 1.0);

  // StretchToFit: off unless it says 1 (a file without it keeps the bars);
  // written after DifferentPerMonitor, 1 or 0; round trips.
  CHECK(!d.stretch_to_fit && !s.stretch_to_fit);
  for (const char* on : {"1", "on", "YES", "true"}) {
    CHECK(parse_settings(std::string("[Saver]\nStretchToFit=") + on + "\n").stretch_to_fit);
  }
  for (const char* off : {"0", "off", "no", "", "maybe"}) {
    CHECK(!parse_settings(std::string("[Saver]\nStretchToFit=") + off + "\n").stretch_to_fit);
  }
  CHECK(parse_settings("[saver]\nstretchtofit=1\n").stretch_to_fit);
  {
    Settings p;
    p.stretch_to_fit = true;
    const std::string fresh = serialize_settings(p);
    CHECK(fresh.find("DifferentPerMonitor=0\r\nStretchToFit=1\r\n") != std::string::npos);
    CHECK(parse_settings(fresh) == p);
    p.stretch_to_fit = false;
    CHECK(serialize_settings(p).find("StretchToFit=0\r\n") != std::string::npos);
    CHECK(serialize_settings(p, "[Saver]\r\nStretchToFit=no\r\n").find("StretchToFit=no\r\n") != std::string::npos);
  }

  // DifferentPerMonitor: Random plays the same module on every monitor unless
  // it says 1 -- a file without it (the fixture, any file written before it)
  // included. Read as written or as a hand might write it, like Sound.
  CHECK(!d.different_per_monitor && !s.different_per_monitor);
  for (const char* on : {"1", "on", "YES", "true", " 1 ", "2"}) {
    CHECK(parse_settings(std::string("[Saver]\nDifferentPerMonitor=") + on + "\n").different_per_monitor);
  }
  for (const char* off : {"0", "off", "no", "False", "", "maybe", "-"}) {
    CHECK(!parse_settings(std::string("[Saver]\nDifferentPerMonitor=") + off + "\n").different_per_monitor);
  }
  CHECK(parse_settings("[saver]\ndifferentpermonitor=1\n").different_per_monitor);   // any case
  CHECK(!parse_settings("[Saver]\nDifferentPerMonitor=1\nDifferentPerMonitor=0\n").different_per_monitor);   // the last wins
  {
    // Written after Monitors in a new file, 1 or 0; round trips; the fixture
    // gains the key once it is saved.
    Settings p;
    p.different_per_monitor = true;
    const std::string fresh = serialize_settings(p);
    CHECK(fresh.find("Monitors=all\r\nDifferentPerMonitor=1\r\n") != std::string::npos);
    CHECK(parse_settings(fresh) == p);
    p.different_per_monitor = false;
    CHECK(serialize_settings(p).find("Monitors=all\r\nDifferentPerMonitor=0\r\n") != std::string::npos);
    const std::string saved = serialize_settings(parse_settings(fixture("settings.ini")), fixture("settings.ini"));
    CHECK(saved.find("DifferentPerMonitor=0\r\n") != std::string::npos && !parse_settings(saved).different_per_monitor);
    // A value that already says it is left as written; one that doesn't is replaced.
    const std::string yes = "[Saver]\r\nMonitors=all\r\nDifferentPerMonitor=yes\r\n";
    p.different_per_monitor = true;
    CHECK(serialize_settings(p, yes).find("DifferentPerMonitor=yes\r\n") != std::string::npos);
    p.different_per_monitor = false;
    const std::string now_off = serialize_settings(p, yes);
    CHECK(now_off.find("DifferentPerMonitor=0\r\n") != std::string::npos && now_off.find("=yes") == std::string::npos);
    CHECK(serialize_settings(p, "[Saver]\r\nDifferentPerMonitor=off\r\n").find("DifferentPerMonitor=off\r\n") !=
          std::string::npos);
    p.different_per_monitor = true;
    CHECK(serialize_settings(p, "[Saver]\r\nDifferentPerMonitor=junk\r\n").find("DifferentPerMonitor=1\r\n") !=
          std::string::npos);
    // The dialog's choices leave it as it was (it is its own checkbox).
    Settings lead_on = parse_settings("[Saver]\nModule=b\nRandomize=a,b\nDifferentPerMonitor=1\n");
    CHECK(apply_dialog_choice(lead_on, {false, {"a"}, 4, "c"}).different_per_monitor);
    CHECK(apply_dialog_choice(lead_on, {true, {"a", "b"}, 4, "c"}).different_per_monitor);
  }

  // ---- the settings dialog's two modes (apply_dialog_choice) ----
  using Ids = std::vector<std::string>;
  // A named Module leading a Randomize list keeps leading through Random...
  Settings lead = parse_settings("[Saver]\nModule=b\nRandomize=a,b,c\n");
  CHECK(lead.has_lead() && lead.rotates());
  CHECK((dialog_checklist(lead) == Ids{"a", "b", "c"}));
  Settings r = apply_dialog_choice(lead, {true, {"a", "c"}, 4, "d"});
  CHECK(r.module == "b" && (r.randomize == Ids{"a", "c"}) && r.randomize_saved.empty());
  CHECK(apply_dialog_choice(lead, {true, {"a", "b", "c"}, 4, "d"}) == lead);   // plain OK: nothing changes
  // ...and with everything checked its list is written out in full (an
  // empty one would make it a single module).
  r = apply_dialog_choice(lead, {true, {"a", "b", "c", "d"}, 4, "a"});
  CHECK(r.module == "b" && r.randomize.size() == 4 && r.has_lead());
  CHECK(parse_settings(serialize_settings(r)) == r);

  // Random without a lead: Module=random; all checked saves an empty list.
  Settings plain = parse_settings("[Saver]\nModule=random\nRandomize=a,b\n");
  r = apply_dialog_choice(plain, {true, {"a", "b", "c", "d"}, 4, "c"});
  CHECK(r.module == "random" && r.randomize.empty() && r.randomize_saved.empty());
  r = apply_dialog_choice(plain, {true, {"b"}, 4, "c"});
  CHECK(r.module == "random" && (r.randomize == Ids{"b"}));

  // Show the selected module: that Module, no Randomize (or the saver would
  // rotate), and the checklist kept aside rather than lost.
  r = apply_dialog_choice(plain, {false, {"a", "b"}, 4, "c"});
  CHECK(r.module == "c" && r.randomize.empty() && (r.randomize_saved == Ids{"a", "b"}) && !r.rotates());
  std::string single = serialize_settings(r);
  CHECK(single.find("Randomize=\r\n") != std::string::npos);
  CHECK(single.find("RandomizeSaved=a,b\r\n") != std::string::npos);
  Settings reopened = parse_settings(single);
  CHECK(reopened == r);
  CHECK(!reopened.rotates());   // the saver ignores RandomizeSaved
  CHECK((dialog_checklist(reopened) == Ids{"a", "b"}));   // the next dialog shows the same checks
  // Back to Random: the kept checklist is the rotation list again, and the
  // key leaves the file.
  r = apply_dialog_choice(reopened, {true, dialog_checklist(reopened), 4, "c"});
  CHECK(r.module == "random" && (r.randomize == Ids{"a", "b"}) && r.randomize_saved.empty());
  CHECK(serialize_settings(r, single).find("RandomizeSaved") == std::string::npos);
  // Single with everything checked keeps nothing (all = every module).
  CHECK(apply_dialog_choice(plain, {false, {"a", "b", "c", "d"}, 4, "c"}).randomize_saved.empty());
  // Single from a lead keeps the lead's list for later.
  r = apply_dialog_choice(lead, {false, {"a", "b", "c"}, 4, "b"});
  CHECK(r.module == "b" && r.randomize.empty() && (r.randomize_saved == Ids{"a", "b", "c"}));
  // Nothing selected: the Module stays; no catalog: nothing changes at all.
  CHECK(apply_dialog_choice(lead, {false, {"a"}, 4, ""}).module == "b");
  CHECK(apply_dialog_choice(lead, {false, {}, 0, ""}) == lead);
  CHECK(apply_dialog_choice(reopened, {true, {}, 0, ""}) == reopened);
  // Single with nothing checked (Random, Clear, then Single) keeps "none",
  // spelled "-": an empty checklist would read as "all" (WIP 9b).
  r = apply_dialog_choice(plain, {false, {}, 4, "c"});
  CHECK(r.module == "c" && r.randomize.empty() && r.randomize_saved.empty() && r.randomize_saved_none);
  CHECK(dialog_checklist_none(r) && dialog_checklist(r).empty());
  std::string none_text = serialize_settings(r, single);
  CHECK(none_text.find("RandomizeSaved=-\r\n") != std::string::npos);
  CHECK(none_text.find("Randomize=\r\n") != std::string::npos);
  Settings none_back = parse_settings(none_text);
  CHECK(none_back == r && dialog_checklist_none(none_back) && !none_back.rotates());
  CHECK(parse_settings(serialize_settings(none_back, none_text)) == none_back);   // a second OK keeps it
  // Back to Random from "none": nothing checked is refused there (random_allowed);
  // with something checked again the marker goes and the list is the rotation.
  CHECK(!random_allowed(DialogChoice{true, {}, 4, "c", false, {}, 0, 0}));
  r = apply_dialog_choice(none_back, {true, {"b"}, 4, "c"});
  CHECK(r.module == "random" && (r.randomize == Ids{"b"}) && !r.randomize_saved_none);
  CHECK(serialize_settings(r, none_text).find("RandomizeSaved") == std::string::npos);
  // Single again with checks: the marker gives way to the list, or to "all".
  CHECK(!apply_dialog_choice(none_back, {false, {"a"}, 4, "c"}).randomize_saved_none);
  CHECK(!apply_dialog_choice(none_back, {false, {"a", "b", "c", "d"}, 4, "c"}).randomize_saved_none);
  CHECK(serialize_settings(apply_dialog_choice(none_back, {false, {"a", "b", "c", "d"}, 4, "c"}), none_text)
            .find("RandomizeSaved") == std::string::npos);
  // "-" only means something while the saver doesn't rotate (it is the kept
  // checklist); a rotating file shows its Randomize.
  Settings odd_none = parse_settings("[Saver]\nModule=random\nRandomize=a\nRandomizeSaved= - \n");
  CHECK(odd_none.randomize_saved_none && !dialog_checklist_none(odd_none) && (dialog_checklist(odd_none) == Ids{"a"}));
  // A saved checklist is only a checklist: Module=x with it alone never rotates.
  CHECK(!parse_settings("[Saver]\nModule=x\nRandomize=\nRandomizeSaved=a,b\n").rotates());
  CHECK(!parse_settings("[Saver]\nModule=x\nRandomize=\nRandomizeSaved=-\n").rotates());
  CHECK((parse_settings("[Saver]\nModule=x\nRandomizeSaved= a, b ,a\n").randomize_saved == Ids{"a", "b"}));

  CHECK_EQ(format_cvset({}), std::string());
  CHECK_EQ(format_cvset({{2, 7}, {0, 50}, {1, -1}}), std::string("0=50,1=-1,2=7"));

  // Through the filesystem (atomic write + load).
  wchar_t tmp[MAX_PATH + 1];
  GetTempPathW(MAX_PATH, tmp);
  std::wstring path = join_path(tmp, L"adscr-unit-" + std::to_wstring(GetCurrentProcessId())) + L"\\sub\\settings.ini";
  CHECK(save_settings(path, x));
  Settings back;
  CHECK(load_settings(path, back));
  CHECK(back == x);
  x.controls.erase("a.one");
  CHECK(save_settings(path, x));
  CHECK(load_settings(path, back) && back == x);
  DeleteFileW(path.c_str());
  RemoveDirectoryW(dir_of(path).c_str());
  RemoveDirectoryW(dir_of(dir_of(path)).c_str());
  Settings none;
  CHECK(!load_settings(path, none) && none == Settings{});
}

// ---- catalog -----------------------------------------------------------------------

// The base the user's data folder lives in (their AD_LOCALAPPDATA, else
// %LOCALAPPDATA%), noted by main() before it points this process, and every
// process it starts, at a scratch one. Only ever read.
std::wstring g_real_base;

// The installed catalog, found without creating anything: AD_ASSETS_DIR
// when set, else under the user's data folder (adw::data_root_path).
std::wstring installed_catalog_path() {
  if (!env_w(L"AD_ASSETS_DIR").empty()) return catalog_path();
  const std::wstring data = adw::data_root_path(g_real_base);
  if (data.empty()) return {};
  const std::wstring root = join_path(data, L"assets"), win = join_path(root, L"win");
  const std::wstring nested = join_path(win, L"catalog-win.json");
  return file_exists(nested) ? nested : join_path(root, L"catalog-win.json");
}

void test_catalog() {
  Catalog c;
  std::string err;
  CHECK(parse_catalog(fixture("catalog-win.json"), c, &err));
  CHECK_EQ(c.version, 1);
  CHECK_EQ(c.modules.size(), (size_t)5);   // the id-less entry is skipped
  const Module* r = c.find("test.rings");
  CHECK(r != nullptr);
  if (r) {
    CHECK_EQ(r->display_name, std::string("Test Rings"));
    CHECK_EQ(r->lane, std::string("pe32"));
    CHECK_EQ(r->path, std::string("FILES/AD40/TESTRING.AD"));
    CHECK(r->about.find("synthetic") != std::string::npos);
    CHECK_EQ(r->controls.size(), (size_t)3);
    // Sorted by index regardless of file order.
    CHECK(r->controls[0].index == 0 && r->controls[0].type == ControlType::slider);
    CHECK(r->controls[1].index == 1 && r->controls[1].type == ControlType::checkbox && r->controls[1].def == 1);
    CHECK(r->controls[2].index == 2 && r->controls[2].type == ControlType::popup && r->controls[2].items.size() == 3);
    CHECK_EQ(r->controls[0].clamp(150), 100);
    CHECK_EQ(r->controls[0].clamp(-5), 0);
    CHECK_EQ(r->controls[1].clamp(7), 1);
    CHECK_EQ(r->controls[2].clamp(9), 2);
    CHECK(r->control(2) == &r->controls[2] && r->control(9) == nullptr);
    CHECK_EQ(r->abi, std::string("afterdark"));   // no "abi": After Dark's
  }
  const Module* s = c.find("test.stripes");
  CHECK(s && s->controls.size() == 2 && s->controls[1].type == ControlType::unknown &&
        s->controls[1].type_name == "dial" && s->controls[0].def == 5 && s->controls[0].min == 1);
  CHECK(s && !s->controls[1].settable() && s->controls[0].settable() && !s->controls[0].stepped());

  // The generator's shapes (ABI.md §2.10): button, string slider, numeric
  // slider with a unit, popup.
  const Module* t = c.find("test.stops");
  CHECK(t && t->controls.size() == 4);
  if (t && t->controls.size() == 4) {
    const Control& button = t->controls[0];
    CHECK(button.type == ControlType::button && !button.settable());
    const Control& ss = t->controls[1];
    CHECK(ss.type == ControlType::slider && ss.stepped() && ss.settable());
    CHECK_EQ(ss.stop_count(), 4);
    CHECK((ss.values == std::vector<int>{0, 33, 66, 100}));
    CHECK_EQ(ss.def, 100);
    CHECK(ss.min == 0 && ss.max == 100);
    // A value picks the last stop whose lower bound it reaches.
    CHECK_EQ(ss.stop_of(0), 0);
    CHECK_EQ(ss.stop_of(32), 0);
    CHECK_EQ(ss.stop_of(33), 1);
    CHECK_EQ(ss.stop_of(65), 1);
    CHECK_EQ(ss.stop_of(99), 2);
    CHECK_EQ(ss.stop_of(100), 3);
    CHECK_EQ(ss.stop_of(1000), 3);
    CHECK_EQ(ss.stop_of(-5), 0);
    CHECK_EQ(ss.value_of_stop(2), 66);
    CHECK_EQ(ss.value_of_stop(9), 100);
    CHECK_EQ(ss.clamp(50), 33);   // snaps to its stop
    CHECK_EQ(ss.value_label(70), std::string("Often"));
    const Control& num = t->controls[2];
    CHECK(num.type == ControlType::slider && !num.stepped() && num.min == 5 && num.max == 95 && num.def == 34);
    CHECK_EQ(num.value_label(34), std::string("34%"));
    CHECK_EQ(num.clamp(2), 5);
    const Control& pop = t->controls[3];
    CHECK(pop.type == ControlType::popup && pop.def == 1 && pop.clamp(5) == 1 && pop.value_label(1) == "1");
  }
  // String-slider edge cases: labels without values (value = stop index),
  // a table longer than its labels, an unordered table, a prefixed unit.
  CHECK(parse_catalog(R"({"modules":[{"id":"x","path":"p","controls":[
      {"index":0,"type":"slider","items":["a","b","c"],"default":1},
      {"index":1,"type":"slider","items":["lo","hi"],"values":[0,50,99],"default":60},
      {"index":2,"type":"slider","items":["hi","lo"],"values":[80,10],"defaultStop":1},
      {"index":3,"type":"slider","min":0,"max":9,"unit":"$","unitPos":"prefix","default":3},
      {"index":4,"type":"slider","min":0,"max":9,"unit":"%","unitPos":"none","default":3}]}]})", c, &err));
  if (c.modules.size() == 1 && c.modules[0].controls.size() == 5) {
    const auto& k = c.modules[0].controls;
    CHECK(k[0].stepped() && k[0].values.empty() && k[0].min == 0 && k[0].max == 2 && k[0].def == 1);
    CHECK(k[0].value_of_stop(2) == 2 && k[0].stop_of(7) == 2 && k[0].value_label(1) == "b");
    CHECK((k[1].values == std::vector<int>{0, 50}) && k[1].items.size() == 2 && k[1].def == 50);
    CHECK((k[2].values == std::vector<int>{10, 80}) && (k[2].items == std::vector<std::string>{"lo", "hi"}));
    CHECK_EQ(k[2].def, 10);           // defaultStop indexes the table as written ("lo")
    CHECK_EQ(k[2].stop_of(0), 0);     // below the first bound: the first stop
    CHECK_EQ(k[3].value_label(3), std::string("$3"));
    CHECK_EQ(k[4].value_label(3), std::string("3"));
  } else {
    CHECK(false);
  }
  CHECK(c.find("nope") == nullptr);
  CHECK(!parse_catalog("{ not json", c, &err) && !err.empty() && c.modules.empty());
  CHECK(!parse_catalog("[1,2]", c, &err));
  CHECK(!parse_catalog("{\"version\":1}", c, &err));
  CHECK(parse_catalog("{\"modules\":[]}", c, &err) && c.modules.empty());
  // Display name falls back to the id; control default is clamped.
  CHECK(parse_catalog(R"({"modules":[{"id":"x","path":"p","controls":[{"index":0,"type":"slider","min":10,"max":20,"default":99}]}]})", c, &err));
  CHECK(c.modules.size() == 1 && c.modules[0].display_name == "x" && c.modules[0].controls[0].def == 20);
  // The module ABI (PACKAGES.md §6): "abi" only when it is not After Dark's
  // (adimport writes it last); absent, empty or not a string is "afterdark".
  CHECK(parse_catalog(R"({"modules":[{"id":"a","path":"A.IMX","lane":"ne16","abi":"intermission"},
      {"id":"b","path":"B.AD","lane":"ne16"},{"id":"c","path":"C.AD","abi":""},{"id":"d","path":"D.AD","abi":7},
      {"id":"e","path":"E.AD","lane":"pe32","abi":"someday"}]})", c, &err));
  CHECK(c.modules.size() == 5);
  if (c.modules.size() == 5) {
    CHECK(c.modules[0].abi == "intermission" && c.modules[0].lane == "ne16");
    CHECK(c.modules[1].abi == kAfterDarkAbi && c.modules[2].abi == kAfterDarkAbi && c.modules[3].abi == kAfterDarkAbi);
    CHECK_EQ(c.modules[4].abi, std::string("someday"));   // kept as written: a host decides whether it runs it
  }
  // A module's own screen (PACKAGES.md §6): "screen": "WxH" for a module shown
  // at a fixed size (Star Trek: The Screen Saver's, "640x480"). Absent, not a string, not "<w>x<h>" with 1 to 5 decimal
  // digits either side, an axis outside 1..8192 or more than 4096x4096
  // pixels (no frame the saver reads back, frame_parser.h) is none: {0, 0},
  // and the ABI decides. Five digits hold every size allowed ("00640x0480"
  // is 640x480); an axis of six or more is none whatever its value
  // ("000640x480"), so no axis can overflow an int: 2^32 + 640 would wrap
  // to 640 ("4294967936x480", and 2^32 + 480 for the height).
  CHECK(parse_catalog(R"({"modules":[{"id":"a","path":"A.AD","lane":"ne16","screen":"640x480"},
      {"id":"b","path":"B.AD"},{"id":"c","path":"C.AD","screen":""},{"id":"d","path":"D.AD","screen":640},
      {"id":"e","path":"E.AD","screen":"800X600"},{"id":"f","path":"F.AD","screen":"8192x2048"},
      {"id":"g","path":"G.AD","screen":"4096x4097"},{"id":"h","path":"H.AD","screen":"8193x100"},
      {"id":"i","path":"I.AD","screen":"0x480"},{"id":"j","path":"J.AD","screen":"640x480 "},
      {"id":"k","path":"K.AD","screen":"640*480"},{"id":"l","path":"L.AD","screen":"-640x480"},
      {"id":"m","path":"M.AD","screen":"x480"},{"id":"n","path":"N.AD","screen":"640x"},
      {"id":"o","path":"O.AD","screen":"1x1"},{"id":"p","path":"P.AD","screen":"00640x0480"},
      {"id":"q","path":"Q.AD","screen":"123456x480"},{"id":"r","path":"R.IMX","lane":"ne16","abi":"intermission","screen":"800x600"},
      {"id":"s","path":"S.AD","screen":"640x480x2"},{"id":"t","path":"T.IMX","lane":"ne16","abi":"intermission"},
      {"id":"u","path":"U.AD","screen":"4294967936x480"},{"id":"v","path":"V.AD","screen":"640x4294967776"},
      {"id":"w","path":"W.AD","screen":"000640x480"},{"id":"x","path":"X.AD","screen":"640x000480"}]})",
                      c, &err));
  CHECK_EQ(c.modules.size(), (size_t)24);
  if (c.modules.size() == 24) {
    const std::map<std::string, SizeI> want = {
        {"a", {640, 480}}, {"b", {}},  {"c", {}},  {"d", {}},  {"e", {800, 600}}, {"f", {8192, 2048}}, {"g", {}},
        {"h", {}},         {"i", {}},  {"j", {}},  {"k", {}},  {"l", {}},         {"m", {}},           {"n", {}},
        {"o", {1, 1}},     {"p", {640, 480}},     {"q", {}},  {"r", {800, 600}}, {"s", {}},           {"t", {}},
        {"u", {}},         {"v", {}},  {"w", {}},  {"x", {}}};
    for (const Module& m : c.modules) {
      if (!want.count(m.id) || m.screen == want.at(m.id)) continue;
      fprintf(stderr, "catalog: \"screen\" of %s reads %dx%d\n", m.id.c_str(), m.screen.w, m.screen.h);
      CHECK(false);
    }
    CHECK(c.modules[0].abi == kAfterDarkAbi);   // a screen of its own changes nothing else about it
    // Its own screen: the catalog's word first, then the ABI's (Intermission's 640x480).
    CHECK((own_screen(c.modules[0].abi, c.modules[0].screen) == SizeI{640, 480}));
    CHECK((own_screen(c.modules[1].abi, c.modules[1].screen) == SizeI{}));
    CHECK((own_screen(c.modules[17].abi, c.modules[17].screen) == SizeI{800, 600}));
    CHECK((own_screen(c.modules[19].abi, c.modules[19].screen) == SizeI{640, 480}));
  }
  // The six-release fixture: Star Wars Screen Entertainment's 14 Intermission
  // modules (lane ne16, abi intermission), each with one Configure... button,
  // beside the five After Dark releases' entries.
  CHECK(parse_catalog(fixture("catalog-six.json"), c, &err));
  CHECK_EQ(c.modules.size(), (size_t)32);
  size_t imx = 0;
  for (const Module& m : c.modules) {
    if (m.package != "swse") {
      CHECK(m.abi == kAfterDarkAbi);
      continue;
    }
    ++imx;
    CHECK(m.abi == "intermission" && m.lane == "ne16" && m.about.empty() && m.credits.empty());
    CHECK(m.path.rfind("packages/swse/SAVER/", 0) == 0 && m.id.rfind("swse.", 0) == 0);
    CHECK(m.controls.size() == 1 && m.controls[0].index == 0 && m.controls[0].name == "Configure..." &&
          m.controls[0].type == ControlType::button && !m.controls[0].settable());
    CHECK(m.name == m.module_name && !m.name.empty());
  }
  CHECK_EQ(imx, (size_t)14);
  const Module* vader = c.find("swse.vader");
  CHECK(vader && vader->name == "Darth Vader" && vader->package_title == "Star Wars Screen Entertainment");
  for (const Module& m : c.modules) CHECK((m.screen == SizeI{}));   // no "screen" anywhere: the ABI decides
  // The seven-release fixture: Star Trek: The Screen Saver's modules (After
  // Dark 2.0b: lane ne16, After Dark's ABI) each with "screen": "640x480",
  // after the six releases' entries (registry order).
  CHECK(parse_catalog(fixture("catalog-seven.json"), c, &err));
  CHECK_EQ(c.modules.size(), (size_t)36);
  size_t trek = 0;
  for (const Module& m : c.modules) {
    if (m.package != "startrek") {
      CHECK((m.screen == SizeI{}));
      continue;
    }
    ++trek;
    CHECK((m.screen == SizeI{640, 480}) && m.abi == kAfterDarkAbi && m.lane == "ne16");
    CHECK(m.path.rfind("packages/startrek/AFTERDRK/", 0) == 0 && m.id.rfind("startrek.", 0) == 0);
    CHECK(m.name == m.module_name && m.package_title == "Star Trek: The Screen Saver");
  }
  CHECK_EQ(trek, (size_t)4);
  CHECK(c.modules.size() == 36 && c.modules[32].id == "startrek.comms");
  const Module* comms = c.find("startrek.comms");
  CHECK(comms && comms->controls.size() == 1 && comms->controls[0].type == ControlType::button &&
        comms->controls[0].index == 3);
  // The twelve-release fixture: ScreamSavers' and Marvel Comics Screen
  // Posters' modules (After Dark's ABI, lane ne16) each with "screen":
  // "640x480", as Star Trek's; Snoopy's, Looney Tunes' and Disney's without
  // one; after the seven releases' entries (registry order).
  CHECK(parse_catalog(fixture("catalog-twelve.json"), c, &err));
  CHECK_EQ(c.modules.size(), (size_t)46);
  size_t own = 0;
  for (const Module& m : c.modules) {
    if (m.package != "screams" && m.package != "marvel" && m.package != "startrek") {
      CHECK((m.screen == SizeI{}));
      continue;
    }
    ++own;
    CHECK((m.screen == SizeI{640, 480}) && m.abi == kAfterDarkAbi && m.lane == "ne16");
  }
  CHECK_EQ(own, (size_t)8);   // Star Trek's 4, Marvel's 1, ScreamSavers' 3
  CHECK(c.modules.size() == 46 && c.modules[36].id == "marvel.kilo" && c.modules[45].id == "disney.tango");

  CHECK(resolve_module_path(L"C:\\a\\win", "FILES/AD40/X.AD") == L"C:\\a\\win\\FILES\\AD40\\X.AD");
  CHECK(resolve_module_path(L"C:\\a\\win\\", "/FILES/X.AD") == L"C:\\a\\win\\FILES\\X.AD");
  CHECK(resolve_module_path(L"C:\\a\\win", "D:/abs/X.AD") == L"D:\\abs\\X.AD");

  // The catalog adimport generated on this machine, when there is one: every
  // module it lists loads, and every default it wrote is already a value the
  // control accepts (the .scr would otherwise silently move it). Found
  // read-only: nothing here may move (or create) the user's data folder.
  std::string real_text;
  const std::wstring installed = installed_catalog_path();
  if (installed.empty() || !read_file(installed, real_text)) {
    printf("catalog: no generated catalog at %s; that check is skipped\n", narrow(installed).c_str());
    return;
  }
  Catalog real;
  CHECK(parse_catalog(real_text, real, &err));
  phosg::JSON doc = phosg::JSON::parse(real_text);
  const auto& mods = doc.at("modules").as_list();
  CHECK_EQ(real.modules.size(), mods.size());
  size_t checked = 0;
  for (size_t i = 0; i < std::min(real.modules.size(), mods.size()); ++i) {
    const Module& m = real.modules[i];
    CHECK(m.lane == "pe32" || m.lane == "ne16");
    // After Dark's module ABI, or Intermission's (Star Wars Screen
    // Entertainment's IMX modules, on the 16-bit lane).
    CHECK(m.abi == kAfterDarkAbi || (m.abi == "intermission" && m.lane == "ne16"));
    CHECK(!m.display_name.empty() && !m.path.empty());
    const auto& ctls = mods[i]->at("controls").as_list();
    CHECK_EQ(m.controls.size(), ctls.size());
    for (size_t k = 0; k < std::min(m.controls.size(), ctls.size()); ++k) {
      const Control& c = m.controls[k];
      CHECK(c.type != ControlType::unknown);
      if (!c.settable()) continue;
      int raw = (int)ctls[k]->get_int("default");
      if (raw != c.def) {
        fprintf(stderr, "catalog: %s control %d \"%s\": default %d loads as %d\n", m.id.c_str(), c.index,
                c.name.c_str(), raw, c.def);
        ++g_failures;
      }
      ++checked;
    }
  }
  printf("catalog: generated catalog has %zu modules, %zu settable controls checked\n", real.modules.size(), checked);
}

// ---- geometry ----------------------------------------------------------------------

void test_geometry() {
  CHECK((emulated_screen_size(4.0 / 3.0, 1.0) == SizeI{640, 480}));
  CHECK((emulated_screen_size(1920.0 / 1080.0, 1.0) == SizeI{856, 480}));
  CHECK((emulated_screen_size(2560.0 / 1600.0, 1.0) == SizeI{768, 480}));
  CHECK((emulated_screen_size(3440.0 / 1440.0, 1.0) == SizeI{1144, 480}));
  CHECK((emulated_screen_size(5120.0 / 1440.0, 1.0) == SizeI{1280, 480}));   // capped at 2x 640
  CHECK((emulated_screen_size(1080.0 / 1920.0, 1.0) == SizeI{640, 480}));    // portrait: never narrower than 4:3
  CHECK((emulated_screen_size(1280.0 / 1024.0, 1.0) == SizeI{640, 480}));
  CHECK((emulated_screen_size(1920.0 / 1080.0, 1.5) == SizeI{1280, 720}));
  CHECK((emulated_screen_size(5120.0 / 1440.0, 1.5) == SizeI{1920, 720}));
  CHECK((emulated_screen_size(4.0 / 3.0, 1.5) == SizeI{960, 720}));
  CHECK((emulated_screen_size(0.0, 1.0) == SizeI{640, 480}));
  for (double a : {1.0, 1.25, 1.6, 1.777, 2.1, 2.37, 3.5}) {
    for (double sc : {1.0, 1.5}) {
      SizeI z = emulated_screen_size(a, sc);
      CHECK(z.w % 8 == 0 && z.h % 8 == 0);
    }
  }

  CHECK((fit_rect(640, 480, 1920, 1080) == RectI{240, 0, 1440, 1080}));
  CHECK((fit_rect(856, 480, 1920, 1080) == RectI{0, 1, 1920, 1077}));
  CHECK((fit_rect(320, 240, 152, 112) == RectI{1, 0, 149, 112}));
  CHECK((fit_rect(640, 480, 640, 480) == RectI{0, 0, 640, 480}));
  CHECK((fit_rect(640, 480, 1080, 1920) == RectI{0, 555, 1080, 810}));
  CHECK((fit_rect(0, 480, 100, 100) == RectI{0, 0, 100, 100}));

  // A module's emulated screen (module_screen), the one rule every host
  // started for it goes by: an After Dark module's is the Resolution setting
  // widened to the display, exactly emulated_screen_size's; an Intermission
  // module's is its own 640x480 on every display at every setting.
  for (double a : {4.0 / 3.0, 1920.0 / 1080.0, 2560.0 / 1600.0, 3440.0 / 1440.0, 5120.0 / 1440.0, 1080.0 / 1920.0,
                   1280.0 / 1024.0, 0.0}) {
    for (double sc : {1.0, 1.5, 2.0, 0.0}) {
      const ModuleScreen ad = module_screen(kAfterDarkAbi, a, sc), imx = module_screen(kIntermissionAbi, a, sc);
      CHECK(ad.emu == emulated_screen_size(a, sc) && !ad.fixed);
      CHECK((imx.emu == SizeI{640, 480}) && imx.fixed);
      // Only "intermission" has a size of its own: another ABI's modules follow the display.
      CHECK(module_screen("", a, sc) == ad && module_screen("someday", a, sc) == ad);
      // A catalog "screen" (own_screen) gives any module one: Star Trek: The
      // Screen Saver's After Dark 2.0b modules get their 640x480 exactly as
      // an Intermission module gets its own by its ABI, and any other size is
      // kept as given. Without one, the rule is the ABI's, as before.
      const ModuleScreen trek = module_screen(own_screen(kAfterDarkAbi, {640, 480}), a, sc);
      CHECK(trek == imx);
      const ModuleScreen other = module_screen(own_screen(kAfterDarkAbi, {1024, 640}), a, sc);
      CHECK((other.emu == SizeI{1024, 640}) && other.fixed);
      CHECK(module_screen(own_screen(kIntermissionAbi, {1024, 640}), a, sc) == other);   // the catalog's word first
      CHECK(module_screen(own_screen(kAfterDarkAbi), a, sc) == ad && module_screen(SizeI{}, a, sc) == ad);
      CHECK(module_screen(own_screen(kIntermissionAbi), a, sc) == imx);
    }
  }
  // own_screen alone: the catalog's size when it has both axes, else the ABI's.
  CHECK((own_screen(kAfterDarkAbi) == SizeI{}) && (own_screen(kIntermissionAbi) == SizeI{640, 480}));
  CHECK((own_screen("someday") == SizeI{}) && (own_screen("") == SizeI{}));
  CHECK((own_screen(kAfterDarkAbi, {640, 480}) == SizeI{640, 480}));
  CHECK((own_screen(kAfterDarkAbi, {0, 480}) == SizeI{}) && (own_screen(kIntermissionAbi, {640, 0}) == SizeI{640, 480}));
  // The user's case: the 720-line setting on a 1920x1080 monitor. An After
  // Dark module gets 1280x720; an Intermission module 640x480, whose 4:3
  // frame the letterbox scales to the monitor's full height, side bars only
  // (at 1280x720 its 640x480 scene sat in the middle, bars all round).
  CHECK((module_screen(kAfterDarkAbi, 1920.0 / 1080.0, 1.5).emu == SizeI{1280, 720}));
  const ModuleScreen imx = module_screen(kIntermissionAbi, 1920.0 / 1080.0, 1.5);
  CHECK((fit_rect(imx.emu.w, imx.emu.h, 1920, 1080) == RectI{240, 0, 1440, 1080}));
  CHECK((fit_rect(imx.emu.w, imx.emu.h, 1024, 768) == RectI{0, 0, 1024, 768}));    // a 4:3 monitor: no bars
  CHECK((fit_rect(imx.emu.w, imx.emu.h, 2560, 1080) == RectI{560, 0, 1440, 1080}));
  // Their desktop seeds: the whole monitor shrunk to an After Dark screen, as
  // before; the part an Intermission module's frame covers, so the desktop
  // shows where it was.
  CHECK((seed_source(module_screen(kAfterDarkAbi, 1920.0 / 1080.0, 1.0), 1920, 1080) == RectI{0, 0, 1920, 1080}));
  CHECK((seed_source(module_screen(kAfterDarkAbi, 1280.0 / 1024.0, 1.0), 1280, 1024) == RectI{0, 0, 1280, 1024}));
  CHECK((seed_source(imx, 1920, 1080) == RectI{240, 0, 1440, 1080}));
  CHECK((seed_source(imx, 1280, 1024) == RectI{0, 32, 1280, 960}));
  CHECK((seed_source(imx, 640, 480) == RectI{0, 0, 640, 480}));
  CHECK((seed_source(imx, 1080, 1920) == RectI{0, 555, 1080, 810}));
  // "Stretch to fit": a module's own screen drawn over the whole window (and
  // so its desktop seed is the whole monitor); a screen that follows the
  // display is the whole monitor either way.
  CHECK((frame_rect(imx.emu.w, imx.emu.h, 1920, 1080, true) == RectI{0, 0, 1920, 1080}));
  CHECK((frame_rect(imx.emu.w, imx.emu.h, 1920, 1080, false) == RectI{240, 0, 1440, 1080}));
  CHECK((frame_rect(imx.emu.w, imx.emu.h, 1080, 1920, true) == RectI{0, 0, 1080, 1920}));
  CHECK((seed_source(imx, 1920, 1080, true) == RectI{0, 0, 1920, 1080}));
  CHECK((seed_source(module_screen(kAfterDarkAbi, 1920.0 / 1080.0, 1.0), 1920, 1080, true) == RectI{0, 0, 1920, 1080}));
  {
    const std::vector<SeedShotPlan> plan = plan_seed_shots({imx}, 1920, 1080, nullptr, true);
    CHECK(plan.size() == 1 && plan[0].src == (RectI{0, 0, 1920, 1080}));
  }
  // A Star Trek module at the 720-line setting on a 16:9 monitor: its
  // 640x480 at the monitor's full height, bars at the sides only (at
  // 1280x720 The Mission's scene sat at the top left beside a grey band, and
  // Final Exam's small in the middle), seeded from that part of the monitor.
  const ModuleScreen trek = module_screen(own_screen(kAfterDarkAbi, {640, 480}), 1920.0 / 1080.0, 1.5);
  CHECK((trek.emu == SizeI{640, 480}) && trek.fixed);
  CHECK((fit_rect(trek.emu.w, trek.emu.h, 1920, 1080) == RectI{240, 0, 1440, 1080}));
  CHECK((seed_source(trek, 1920, 1080) == RectI{240, 0, 1440, 1080}));
  // A 16:10 screen of its own on the same monitor: its full height too.
  const ModuleScreen wide = module_screen(SizeI{1024, 640}, 1920.0 / 1080.0, 1.5);
  CHECK((fit_rect(wide.emu.w, wide.emu.h, 1920, 1080) == RectI{96, 0, 1728, 1080}));
  CHECK((seed_source(wide, 1920, 1080) == RectI{96, 0, 1728, 1080}));

  // A window's desktop seeds (plan_seed_shots): a picture for each screen its
  // first host may be given, After Dark's first, whatever order the screens
  // come in and however often; one the same as an earlier picture shares its
  // file (`same`, that picture's index).
  {
    using Plan = std::vector<SeedShotPlan>;
    auto screen_for = [](const char* abi, int w, int h, double scale) {
      return module_screen(abi, (double)w / h, scale);
    };
    // 16:9 at 720 lines, a window that may start with either kind: two
    // pictures, the whole monitor at 1280x720 and the frame's part at 640x480.
    const ModuleScreen ad_169 = screen_for(kAfterDarkAbi, 1920, 1080, 1.5);
    Plan p = plan_seed_shots({imx, ad_169, imx}, 1920, 1080);
    CHECK((p == Plan{{ad_169, {0, 0, 1920, 1080}, -1}, {imx, {240, 0, 1440, 1080}, -1}}));
    CHECK((ad_169.emu == SizeI{1280, 720}));
    // 4:3 at 480 lines: both pictures are the whole monitor at 640x480, so
    // the Intermission one is the After Dark one's file.
    const ModuleScreen ad_43 = screen_for(kAfterDarkAbi, 1024, 768, 1.0);
    p = plan_seed_shots({imx, ad_43}, 1024, 768);
    CHECK((p == Plan{{ad_43, {0, 0, 1024, 768}, -1}, {imx, {0, 0, 1024, 768}, 0}}));
    CHECK((ad_43.emu == SizeI{640, 480}) && !ad_43.fixed);
    // ...but at 720 lines the same part is two sizes: two files.
    const ModuleScreen ad_43_720 = screen_for(kAfterDarkAbi, 1024, 768, 1.5);
    p = plan_seed_shots({ad_43_720, imx}, 1024, 768);
    CHECK((p == Plan{{ad_43_720, {0, 0, 1024, 768}, -1}, {imx, {0, 0, 1024, 768}, -1}}));
    // An Intermission module alone (Module=<its id>, or Star Wars Screen
    // Entertainment the only release imported): the frame's part only, no
    // picture of the whole monitor.
    p = plan_seed_shots({imx}, 1920, 1080);
    CHECK((p == Plan{{imx, {240, 0, 1440, 1080}, -1}}));
    // After Dark modules alone: the whole monitor, as before.
    p = plan_seed_shots({ad_169, ad_169}, 1920, 1080);
    CHECK((p == Plan{{ad_169, {0, 0, 1920, 1080}, -1}}));
    // 5:4 and portrait: the same 640x480 either kind, but the frame covers
    // the full width only (bars above and below), so two pictures.
    const ModuleScreen ad_54 = screen_for(kAfterDarkAbi, 1280, 1024, 1.0);
    p = plan_seed_shots({imx, ad_54}, 1280, 1024);
    CHECK((p == Plan{{ad_54, {0, 0, 1280, 1024}, -1}, {imx, {0, 32, 1280, 960}, -1}}));
    CHECK((ad_54.emu == SizeI{640, 480}));
    const ModuleScreen ad_portrait = screen_for(kAfterDarkAbi, 1080, 1920, 1.0);
    p = plan_seed_shots({imx, ad_portrait}, 1080, 1920);
    CHECK((p == Plan{{ad_portrait, {0, 0, 1080, 1920}, -1}, {imx, {0, 555, 1080, 810}, -1}}));
    CHECK((ad_portrait.emu == SizeI{640, 480}));
    CHECK(plan_seed_shots({}, 1920, 1080).empty());
    // A Star Trek module (its catalog's 640x480) and an Intermission module
    // (its ABI's) have one screen: one picture of the frame's part, beside
    // After Dark's whole monitor, whichever of them a window may start with.
    CHECK(trek == imx);
    p = plan_seed_shots({trek, ad_169}, 1920, 1080);
    CHECK((p == Plan{{ad_169, {0, 0, 1920, 1080}, -1}, {trek, {240, 0, 1440, 1080}, -1}}));
    p = plan_seed_shots({trek, imx}, 1920, 1080);
    CHECK((p == Plan{{trek, {240, 0, 1440, 1080}, -1}}));
    // On a 4:3 monitor at 480 lines it is After Dark's own picture's file.
    const ModuleScreen trek_43 = module_screen(own_screen(kAfterDarkAbi, {640, 480}), 1024.0 / 768.0, 1.0);
    p = plan_seed_shots({trek_43, ad_43}, 1024, 768);
    CHECK((p == Plan{{ad_43, {0, 0, 1024, 768}, -1}, {trek_43, {0, 0, 1024, 768}, 0}}));
    // A screen of another size of its own is a picture of its own, after
    // After Dark's and 640x480, whatever order the screens came in.
    const Plan three{{ad_169, {0, 0, 1920, 1080}, -1}, {trek, {240, 0, 1440, 1080}, -1}, {wide, {96, 0, 1728, 1080}, -1}};
    size_t left = 99;
    p = plan_seed_shots({trek, wide, ad_169}, 1920, 1080, &left);
    CHECK(p == three && left == 0);
    CHECK(plan_seed_shots({wide, ad_169, trek}, 1920, 1080) == three);
    // However many screens of their own come (a catalog may give every
    // module one, each up to 4096x4096, a P6 of 48 MB), three pictures at
    // most: the one that follows the display, 640x480 (even beside smaller
    // ones), then the smallest of the others (the fewest bytes); `left_out`
    // counts the rest, whose first hosts start on black. In any order, and
    // each screen however often.
    const ModuleScreen qvga = module_screen(SizeI{320, 240}, 1920.0 / 1080.0, 1.5),
                       q400 = module_screen(SizeI{400, 300}, 1920.0 / 1080.0, 1.5);
    const Plan most{{ad_169, {0, 0, 1920, 1080}, -1}, {trek, {240, 0, 1440, 1080}, -1}, {qvga, {240, 0, 1440, 1080}, -1}};
    std::vector<ModuleScreen> many;
    for (int i = 0; i < 64; ++i) many.push_back(module_screen(SizeI{4096, 4096 - 8 * i}, 1920.0 / 1080.0, 1.5));
    many.insert(many.begin() + 20, {wide, ad_169, trek, wide});
    p = plan_seed_shots(many, 1920, 1080, &left);
    CHECK(p == three && left == 64);
    many.insert(many.begin() + 40, {q400, qvga, q400});
    p = plan_seed_shots(many, 1920, 1080, &left);
    CHECK(p == most && left == 66);
    std::reverse(many.begin(), many.end());
    CHECK(plan_seed_shots(many, 1920, 1080, &left) == most && left == 66);
    std::shuffle(many.begin(), many.end(), std::mt19937(7));
    CHECK(plan_seed_shots(many, 1920, 1080, &left) == most && left == 66);
    // Without 640x480 the two smallest (of one size, the narrower first);
    // without one that follows the display, those two alone.
    const ModuleScreen svga = module_screen(SizeI{800, 600}, 1920.0 / 1080.0, 1.5),
                       tall = module_screen(SizeI{600, 800}, 1920.0 / 1080.0, 1.5);
    many.erase(std::remove_if(many.begin(), many.end(),
                              [&](const ModuleScreen& ms) { return ms == trek || ms == wide || ms == qvga || ms == q400; }),
               many.end());
    many.push_back(svga);
    many.push_back(tall);
    p = plan_seed_shots(many, 1920, 1080, &left);
    CHECK(p.size() == 3 && left == 64);
    if (p.size() == 3) CHECK(p[0].screen == ad_169 && p[1].screen == tall && p[2].screen == svga);
    many.erase(std::remove(many.begin(), many.end(), ad_169), many.end());
    p = plan_seed_shots(many, 1920, 1080, &left);
    CHECK(p.size() == 2 && left == 64);
    if (p.size() == 2) CHECK(p[0].screen == tall && p[1].screen == svga && p[0].same < 0 && p[1].same < 0);
    // One that follows the display at most, the first given (a window has one).
    p = plan_seed_shots({ad_169, ad_43, trek}, 1920, 1080, &left);
    CHECK(p.size() == 2 && left == 1);
    if (p.size() == 2) CHECK(p[0].screen == ad_169 && p[1].screen == trek);
  }

  // The monitors the smoke tests stage (AD_SCR_TEST_MONITORS), as the saver
  // and the settings dialog read them: one layout per topology change, the
  // last one past the end, ",p" the primary (else the first), bad entries
  // and ones without area left out.
  {
    using M = std::vector<StagedMonitor>;
    CHECK(parse_staged_monitors(L"", 0).empty() && parse_staged_monitors(L"", 3).empty());
    const std::wstring two = L"0,0,1280,720,p;1280,0,1024,768|0,0,1280,720;1280,0,1024,768,p|-16000, 0, 856, 480";
    CHECK((parse_staged_monitors(two, 0) == M{{{0, 0, 1280, 720}, true}, {{1280, 0, 1024, 768}, false}}));
    CHECK((parse_staged_monitors(two, 1) == M{{{0, 0, 1280, 720}, false}, {{1280, 0, 1024, 768}, true}}));
    CHECK((parse_staged_monitors(two, 2) == M{{{-16000, 0, 856, 480}, true}}));
    CHECK(parse_staged_monitors(two, 9) == parse_staged_monitors(two, 2));
    // Without ",p" the first is the primary; a second ",p" is not one; bad entries go.
    CHECK((parse_staged_monitors(L"10,20,640,480;0,0,800,600", 0) == M{{{10, 20, 640, 480}, true}, {{0, 0, 800, 600}, false}}));
    CHECK((parse_staged_monitors(L"0,0,640,480;640,0,640,480,p;1280,0,640,480,p", 0) ==
           M{{{0, 0, 640, 480}, false}, {{640, 0, 640, 480}, true}, {{1280, 0, 640, 480}, false}}));
    CHECK((parse_staged_monitors(L"junk;0,0,0,480;1,2,3;0,0,640,480;", 0) == M{{{0, 0, 640, 480}, true}}));
    CHECK(parse_staged_monitors(L"x,y,w,h", 0).empty());
  }
}

// ---- monitor topology changes (plan_relayout) ---------------------------------------

void test_layout() {
  auto slot = [](int x, int y, int w, int h, bool runs = true, bool message = false) {
    ScreenSlot s;
    s.rc = {x, y, w, h};
    s.runs_host = runs;
    s.message = message;
    if (runs) s.emu = emulated_screen_size((double)w / h, 1.0);
    return s;
  };
  using Ints = std::vector<int>;
  using Bools = std::vector<bool>;
  const std::vector<ScreenSlot> two = {slot(0, 0, 1920, 1080), slot(1920, 0, 1280, 1024)};

  RelayoutPlan p = plan_relayout(two, two);   // a burst that changed nothing
  CHECK(p.unchanged() && p.kept == 2 && (p.reuse == Ints{0, 1}) && (p.retire == Bools{false, false}));
  // The second monitor went away (a DisplayPort monitor in deep sleep): its
  // window and host go, the other carries on.
  p = plan_relayout(two, {two[0]});
  CHECK(!p.unchanged() && p.kept == 1 && p.retired == 1 && (p.reuse == Ints{0}) && (p.retire == Bools{false, true}));
  // ...and came back: a new window for it.
  p = plan_relayout({two[0]}, two);
  CHECK(p.kept == 1 && p.created == 1 && p.retired == 0 && (p.reuse == Ints{0, -1}));
  // A mode change at the same aspect keeps the emulated size: the window
  // moves, its host keeps running.
  p = plan_relayout(two, {slot(0, 0, 2560, 1440), two[1]});
  CHECK(p.moved == 1 && p.kept == 1 && p.created == 0 && p.retired == 0 && (p.reuse == Ints{0, 1}));
  // Rearranged in the virtual screen: both move.
  p = plan_relayout(two, {slot(1280, 0, 1920, 1080), slot(0, 0, 1280, 1024)});
  CHECK(p.moved == 2 && p.created == 0 && (p.reuse == Ints{0, 1}));
  // A new aspect needs a new emulated screen: a new window and host.
  p = plan_relayout(two, {slot(0, 0, 1920, 1200), two[1]});
  CHECK(p.created == 1 && p.retired == 1 && p.kept == 1 && (p.reuse == Ints{-1, 1}) && (p.retire == Bools{true, false}));
  // Exact matches go first: of two same-size monitors the one still there
  // keeps its own window (its host was drawing for it already).
  const std::vector<ScreenSlot> pair = {slot(0, 0, 1920, 1080), slot(1920, 0, 1920, 1080)};
  p = plan_relayout(pair, {pair[1]});
  CHECK(p.kept == 1 && p.moved == 0 && (p.reuse == Ints{1}) && (p.retire == Bools{true, false}));
  // Roles: with nothing imported the message follows the primary; the
  // black window is not interchangeable with it.
  p = plan_relayout({slot(0, 0, 1920, 1080, false, true), slot(1920, 0, 1920, 1080, false, false)},
                    {slot(1920, 0, 1920, 1080, false, true), slot(0, 0, 1920, 1080, false, false)});
  CHECK(p.moved == 2 && p.kept == 0 && p.created == 0 && (p.reuse == Ints{0, 1}));
  // Monitors=primary and the primary changed: the host window moves over.
  p = plan_relayout({slot(0, 0, 1920, 1080, true), slot(1920, 0, 1920, 1080, false)},
                    {slot(1920, 0, 1920, 1080, true), slot(0, 0, 1920, 1080, false)});
  CHECK(p.moved == 2 && p.created == 0 && (p.reuse == Ints{0, 1}));
  // A host window and a black one never stand in for each other.
  p = plan_relayout({slot(0, 0, 1920, 1080, false)}, {slot(0, 0, 1920, 1080, true)});
  CHECK(p.created == 1 && p.retired == 1);
  // Degenerate lists.
  p = plan_relayout({}, {slot(0, 0, 800, 600)});
  CHECK(p.created == 1 && (p.reuse == Ints{-1}) && p.retire.empty());
  p = plan_relayout(two, {});
  CHECK(p.retired == 2 && p.reuse.empty() && (p.retire == Bools{true, true}));
  CHECK(plan_relayout({}, {}).unchanged());

  // A window whose host runs an Intermission module (its own 640x480,
  // `fixed`: module_screen) keeps that host wherever it goes: a new aspect
  // moves it, where an After Dark module's host is replaced (above).
  auto imx_slot = [&](int x, int y, int w, int h) {
    ScreenSlot s = slot(x, y, w, h);
    const ModuleScreen ms = module_screen(kIntermissionAbi, (double)w / h, 1.5);
    s.emu = ms.emu;
    s.fixed = ms.fixed;
    return s;
  };
  p = plan_relayout({imx_slot(0, 0, 1920, 1080)}, {slot(0, 0, 1920, 1200)});
  CHECK(p.moved == 1 && p.created == 0 && p.retired == 0 && (p.reuse == Ints{0}));
  p = plan_relayout({imx_slot(0, 0, 1920, 1080)}, {slot(0, 0, 1080, 1920)});   // turned portrait
  CHECK(p.moved == 1 && p.created == 0 && p.retired == 0);
  p = plan_relayout({imx_slot(0, 0, 1920, 1080)}, {slot(0, 0, 1920, 1080)});   // nothing changed
  CHECK(p.kept == 1 && p.unchanged());
  // A window of the slot's size is matched before one that fits anywhere, so
  // neither host restarts: the 16:9 slot takes the After Dark window, the 5:4
  // one the Intermission window (taken in order, the fixed window, first,
  // would have left the After Dark one without a slot of its size).
  p = plan_relayout({imx_slot(0, 0, 1920, 1080), slot(1920, 0, 1920, 1080)},
                    {slot(3840, 0, 1920, 1080), slot(0, 0, 1280, 1024)});
  CHECK(p.moved == 2 && p.created == 0 && p.retired == 0 && (p.reuse == Ints{1, 0}));
  // ...and an After Dark window of the right size is kept or moved as before
  // beside one that fits anywhere.
  p = plan_relayout({slot(0, 0, 1280, 1024), imx_slot(1280, 0, 1920, 1080)},
                    {slot(0, 0, 1280, 1024), slot(1280, 0, 2560, 1080)});
  CHECK(p.kept == 1 && p.moved == 1 && p.created == 0 && (p.reuse == Ints{0, 1}));
  // Roles still come first: its host never stands in for a black window.
  p = plan_relayout({imx_slot(0, 0, 1920, 1080)}, {slot(0, 0, 1920, 1080, false)});
  CHECK(p.created == 1 && p.retired == 1);
  // A window whose host runs a module with a screen of its own from its
  // catalog (a Star Trek module's "screen": "640x480", SaverWindow::slot)
  // is kept the same way: moved wherever the monitors go, never restarted,
  // and matched after the windows of a monitor's own size.
  auto trek_slot = [&](int x, int y, int w, int h) {
    ScreenSlot s = slot(x, y, w, h);
    const ModuleScreen ms = module_screen(own_screen(kAfterDarkAbi, {640, 480}), (double)w / h, 1.5);
    s.emu = ms.emu;
    s.fixed = ms.fixed;
    return s;
  };
  CHECK(trek_slot(0, 0, 1920, 1080).fixed && (trek_slot(0, 0, 1920, 1080).emu == SizeI{640, 480}));
  p = plan_relayout({trek_slot(0, 0, 1920, 1080)}, {slot(0, 0, 1920, 1200)});
  CHECK(p.moved == 1 && p.created == 0 && p.retired == 0 && (p.reuse == Ints{0}));
  p = plan_relayout({trek_slot(0, 0, 1920, 1080)}, {slot(0, 0, 1080, 1920)});   // turned portrait
  CHECK(p.moved == 1 && p.created == 0 && p.retired == 0);
  p = plan_relayout({trek_slot(0, 0, 1920, 1080)}, {slot(0, 0, 1920, 1080)});
  CHECK(p.kept == 1 && p.unchanged());
  p = plan_relayout({trek_slot(0, 0, 1920, 1080), slot(1920, 0, 1920, 1080)},
                    {slot(3840, 0, 1920, 1080), slot(0, 0, 1280, 1024)});
  CHECK(p.moved == 2 && p.created == 0 && p.retired == 0 && (p.reuse == Ints{1, 0}));
  // Beside an Intermission module's window, the two are alike: either takes either slot.
  p = plan_relayout({trek_slot(0, 0, 1920, 1080), imx_slot(1920, 0, 1920, 1080)},
                    {slot(0, 0, 1280, 1024), slot(1280, 0, 1024, 768)});
  CHECK(p.moved == 2 && p.created == 0 && p.retired == 0 && (p.reuse == Ints{0, 1}));
}

// ---- rotation ----------------------------------------------------------------------

void test_rotation() {
  Rotation r({"a", "b", "c", "b"}, 7);
  CHECK_EQ(r.size(), (size_t)3);
  std::string prev = r.current();
  std::map<std::string, int> seen;
  seen[prev]++;
  for (int i = 0; i < 2999; ++i) {
    std::string n = r.next();
    CHECK(n != prev);   // never the same module twice in a row
    seen[n]++;
    prev = n;
  }
  // A shuffle bag: every module exactly once per pass of three.
  CHECK(seen["a"] == 1000 && seen["b"] == 1000 && seen["c"] == 1000);

  Rotation one({"only"}, 1);
  CHECK(one.current() == "only" && one.next() == "only" && one.size() == 1);
  Rotation none({}, 1);
  CHECK(none.empty() && none.current().empty() && none.next().empty());

  // Module=<id> with a Randomize list: the named module plays first...
  for (uint32_t seed = 0; seed < 20; ++seed) {
    Rotation in({"a", "b", "c"}, seed, "c");
    CHECK_EQ(in.current(), std::string("c"));
    CHECK_EQ(in.size(), (size_t)3);
    std::set<std::string> pass{in.current()};
    pass.insert(in.next());
    pass.insert(in.next());
    CHECK_EQ(pass.size(), (size_t)3);   // ...still one shuffle-bag pass
  }
  // ...and when it is not in the list it plays once, ahead of it.
  Rotation lead({"a", "b"}, 3, "z");
  CHECK_EQ(lead.current(), std::string("z"));
  CHECK_EQ(lead.size(), (size_t)3);
  std::map<std::string, int> after;
  for (int i = 0; i < 100; ++i) after[lead.next()]++;
  CHECK(after.count("z") == 0 && after["a"] == 50 && after["b"] == 50);
  Rotation lead_only({}, 3, "z");
  CHECK(!lead_only.empty() && lead_only.current() == "z" && lead_only.next() == "z");

  // The rotation every monitor follows (SharedRotation): the order is its
  // bag's, a Rotation's with the same seed (so every monitor plays what one
  // would, and a monitor that joins plays current()), one step per move.
  {
    for (uint32_t seed : {1u, 7u, 12345u}) {
      SharedRotation sr({"a", "b", "c", "d"}, seed, "c");
      Rotation same({"a", "b", "c", "d"}, seed, "c");
      CHECK(sr.current() == "c" && sr.current() == same.current() && sr.size() == 4 && sr.step() == 0);
      for (int i = 1; i <= 40; ++i) {
        CHECK(sr.tick(false));
        CHECK(sr.current() == same.next() && sr.step() == (uint64_t)i && !sr.waiting());
      }
    }
    // The clock waits while the primary monitor's module plays a game it
    // may not be switched away from, as long as the game lasts, then moves
    // every monitor on once.
    SharedRotation sr({"a", "b", "c"}, 5);
    const std::string first = sr.current();
    CHECK(!sr.tick(true) && sr.waiting() && sr.current() == first && sr.step() == 0);
    CHECK(!sr.tick(true) && sr.waiting() && sr.step() == 0);
    CHECK(sr.tick(false) && !sr.waiting() && sr.current() != first && sr.step() == 1);
    // Nothing else to show: never moves, never waits.
    SharedRotation one({"only"}, 1);
    CHECK(!one.tick(false) && !one.tick(true) && !one.waiting() && one.current() == "only" && one.step() == 0);
    CHECK(!one.give_up(1, true, false) && one.step() == 0);
    SharedRotation none({}, 1);
    CHECK(none.empty() && !none.tick(false) && !none.give_up(0, true, false) && none.current().empty());
    // A module a monitor's host fails three times is skipped on every monitor,
    // the primary's or another's...
    SharedRotation skip({"a", "b", "c", "d"}, 9);
    Rotation skip_same({"a", "b", "c", "d"}, 9);
    CHECK(skip.give_up(1, false, false) && skip.current() == skip_same.next() && skip.step() == 1);
    CHECK(skip.give_up(1, true, false) && skip.current() == skip_same.next() && skip.step() == 2);
    // ...even while the primary monitor's module plays, when it is the one failing (it plays no game then)...
    CHECK(skip.give_up(2, true, true) && skip.current() == skip_same.next() && skip.step() == 3);
    // ...but not from under another monitor's game: that monitor retries meanwhile.
    const std::string kept = skip.current();
    CHECK(!skip.give_up(1, false, true) && skip.current() == kept && skip.step() == 3);
    // A monitor that has failed every module in turn is at fault, not the
    // module: it no longer moves the others on (it retries at a relaxed pace).
    CHECK(!skip.give_up(4, false, false) && !skip.give_up(5, true, false) && skip.current() == kept);
    CHECK(skip.give_up(3, false, false) && skip.step() == 4);
    // A skip ends a wait for a game (the clock then counts a full interval).
    SharedRotation wait({"a", "b", "c"}, 3);
    CHECK(!wait.tick(true) && wait.waiting());
    CHECK(wait.give_up(1, true, false) && !wait.waiting() && wait.step() == 1);
    // A named module leading the list plays first on every monitor, then the bag.
    SharedRotation lead_bag({"a", "b"}, 3, "z");
    CHECK(lead_bag.current() == "z" && lead_bag.size() == 3);
    std::map<std::string, int> after_lead;
    for (int i = 0; i < 100; ++i) {
      CHECK(lead_bag.tick(false));
      after_lead[lead_bag.current()]++;
    }
    CHECK(after_lead.count("z") == 0 && after_lead["a"] == 50 && after_lead["b"] == 50);
  }

  // A rotation switching between an After Dark module and one with a screen
  // of its own (an Intermission module's by its ABI, a Star Trek module's by
  // its catalog "screen") on a 16:9 monitor at 720 lines, host by host as
  // the saver does it (SaverWindow::spawn: each switch is a new host with its
  // module's screen, module_screen over own_screen; SaverWindow::slot: the
  // window's slot is its host's): the sizes alternate, 1280x720 and the
  // module's own 640x480. A monitor change of aspect meanwhile keeps the
  // host of the one with its own screen (its window moves) and replaces the
  // After Dark one, and the host after the move gets its module's size on
  // the new monitor.
  for (const char* own : {R"("abi":"intermission")", R"("screen":"640x480")"}) {
    Catalog c;
    CHECK(parse_catalog(std::string(R"({"modules":[{"id":"ad","path":"A.AD","lane":"pe32"},
                                       {"id":"imx","path":"B.IMX","lane":"ne16",)") + own + "}]}",
                        c, nullptr));
    Rotation mixed({"ad", "imx"}, 11);
    std::map<std::string, int> hosts;
    std::string last;
    for (int i = 0; i < 8; ++i) {
      const std::string id = i == 0 ? mixed.current() : mixed.next();
      const Module* m = c.find(id);
      CHECK(m != nullptr);
      if (!m) break;
      CHECK(id != last);   // two modules: every switch changes the module, and the size with it
      last = id;
      ++hosts[id];
      const ModuleScreen ms = module_screen(own_screen(m->abi, m->screen), 1920.0 / 1080.0, 1.5);
      if (id == "imx") CHECK((ms.emu == SizeI{640, 480}) && ms.fixed);
      else CHECK((ms.emu == SizeI{1280, 720}) && !ms.fixed);
      ScreenSlot now, next;
      now.rc = {0, 0, 1920, 1080};
      now.runs_host = true;
      now.emu = ms.emu;
      now.fixed = ms.fixed;
      next.rc = {0, 0, 1280, 1024};
      next.runs_host = true;
      next.emu = emulated_screen_size(1280.0 / 1024.0, 1.5);   // the new slot: its monitor's size
      const RelayoutPlan p = plan_relayout({now}, {next});
      if (ms.fixed) CHECK(p.moved == 1 && p.created == 0 && p.retired == 0);
      else CHECK(p.created == 1 && p.retired == 1);
      const ModuleScreen after = module_screen(own_screen(m->abi, m->screen), 1280.0 / 1024.0, 1.5);
      if (ms.fixed) CHECK(after == ms);
      else CHECK((after.emu == SizeI{960, 720}) && !after.fixed);
    }
    CHECK(hosts["ad"] == 4 && hosts["imx"] == 4);
  }
}

// ---- frame conversion ----------------------------------------------------------------

void test_convert() {
  RawFrame raw;
  raw.format = 8;
  raw.width = 3;
  raw.height = 2;
  raw.palette.resize(768);
  for (int i = 0; i < 768; ++i) raw.palette[i] = (uint8_t)i;
  raw.pixels = {1, 2, 3, 4, 5, 6};
  Frame f;
  convert_frame(raw, f);
  CHECK(f.bpp == 8 && f.stride == 4 && f.bits.size() == 8);
  CHECK((f.bits == std::vector<uint8_t>{1, 2, 3, 0, 4, 5, 6, 0}));
  // RGB triple -> RGBQUAD (B, G, R, 0)
  CHECK(f.palette[1].rgbRed == 3 && f.palette[1].rgbGreen == 4 && f.palette[1].rgbBlue == 5 && f.palette[1].rgbReserved == 0);

  raw.width = 4;
  raw.height = 1;
  raw.pixels = {9, 8, 7, 6};
  convert_frame(raw, f);
  CHECK(f.stride == 4 && (f.bits == std::vector<uint8_t>{9, 8, 7, 6}));

  raw.format = 6;
  raw.width = 2;
  raw.height = 1;
  raw.palette.clear();
  raw.pixels = {10, 20, 30, 40, 50, 60};
  convert_frame(raw, f);
  CHECK(f.bpp == 32 && f.stride == 8 && (f.bits == std::vector<uint8_t>{30, 20, 10, 0, 60, 50, 40, 0}));
}

// ---- environment block -----------------------------------------------------------------

std::map<std::wstring, std::wstring> parse_block(const std::wstring& b) {
  std::map<std::wstring, std::wstring> m;
  for (size_t i = 0; i < b.size() && b[i];) {
    std::wstring e(b.c_str() + i);
    size_t eq = e.find(L'=', 1);
    if (eq != std::wstring::npos) m[e.substr(0, eq)] = e.substr(eq + 1);
    i += e.size() + 1;
  }
  return m;
}

void test_env() {
  SetEnvironmentVariableW(L"AdScrUnitVar", L"old");
  SetEnvironmentVariableW(L"AdScrUnitGone", L"x");
  std::wstring b = build_environment_block({{L"ADSCRUNITVAR", L"new"}, {L"AdScrUnitGone", L""}, {L"AdScrUnitAdd", L"1"}});
  CHECK(b.size() >= 2 && b[b.size() - 1] == 0 && b[b.size() - 2] == 0);
  auto m = parse_block(b);
  int hits = 0;
  for (auto& [k, v] : m) {
    if (CompareStringOrdinal(k.c_str(), -1, L"AdScrUnitVar", -1, TRUE) == CSTR_EQUAL) { ++hits; CHECK(v == L"new"); }
    CHECK(CompareStringOrdinal(k.c_str(), -1, L"AdScrUnitGone", -1, TRUE) != CSTR_EQUAL);
  }
  CHECK_EQ(hits, 1);
  CHECK(m.count(L"AdScrUnitAdd") && m[L"AdScrUnitAdd"] == L"1");
  CHECK(!m[L"PATH"].empty() || !m[L"Path"].empty());   // the rest of our environment is inherited

  // ADNUMLOCK (INTERACTION.md §3.2; dialog_support.h numlock_env): the Num
  // Lock toggle a host starts with, for a host that keeps one (numlock=1), or
  // one that hasn't answered yet (the saver's first hosts start before the
  // answer unless the rotation waits for it, App::caps_gate, and a host
  // without the toggle ignores the variable); a host that answered without
  // numlock=1 gets none, and nothing inherited reaches it.
  const HostCapabilities none_yet, keeps = parse_capabilities("lanes=pe32,ne16 numlock=1"),
                                   lacks = parse_capabilities("lanes=pe32,ne16 abis=afterdark,intermission");
  CHECK(numlock_env(none_yet, true) == (std::pair<std::wstring, std::wstring>{L"ADNUMLOCK", L"1"}));
  CHECK(numlock_env(keeps, false) == (std::pair<std::wstring, std::wstring>{L"ADNUMLOCK", L"0"}));
  CHECK(numlock_env(keeps, true) == (std::pair<std::wstring, std::wstring>{L"ADNUMLOCK", L"1"}));
  CHECK(numlock_env(lacks, true) == (std::pair<std::wstring, std::wstring>{L"ADNUMLOCK", L""}));
  SetEnvironmentVariableW(L"ADNUMLOCK", L"1");   // hostile: inherited from whoever started the saver
  auto numlock_in = [&](const HostCapabilities& caps, bool on) {
    auto block = parse_block(build_environment_block({numlock_env(caps, on)}));
    for (auto& [k, v] : block) {
      if (CompareStringOrdinal(k.c_str(), -1, L"ADNUMLOCK", -1, TRUE) == CSTR_EQUAL) return v;
    }
    return std::wstring(L"(none)");
  };
  CHECK(numlock_in(keeps, false) == L"0" && numlock_in(none_yet, false) == L"0");
  CHECK(numlock_in(lacks, false) == L"(none)" && numlock_in(lacks, true) == L"(none)");
  SetEnvironmentVariableW(L"ADNUMLOCK", nullptr);

  CHECK(quote_arg(L"plain") == L"plain");
  CHECK(quote_arg(L"a b") == L"\"a b\"");
  CHECK(quote_arg(L"C:\\dir with space\\") == L"\"C:\\dir with space\\\\\"");
  CHECK(quote_arg(L"say \"hi\"") == L"\"say \\\"hi\\\"\"");
  CHECK(quote_arg(L"") == L"\"\"");
}

// ---- the settings dialog's helpers (dialog_support.h) --------------------------------

std::wstring g_fakeimport;   // scr_unit dialog --fakeimport <exe>

bool put_file(const std::wstring& path) { return write_file_atomic(path, "x"); }

void test_dialog() {
  CHECK(classify_import_exit(0) == ImportOutcome::imported);
  CHECK(classify_import_exit(5) == ImportOutcome::cancelled);
  for (DWORD c : {1ul, 2ul, 3ul, 4ul, 6ul, 0xC0000005ul}) CHECK(classify_import_exit(c) == ImportOutcome::failed);
  CHECK(import_outcome_note(0).empty());
  CHECK(import_outcome_note(5) == L"Import cancelled. Nothing was changed.");
  CHECK(import_outcome_note(3) == L"Import did not finish (adimport exit code 3). Nothing was changed.");
  CHECK(import_outcome_note(0xC0000005).find(L"exit code 0xC0000005)") != std::wstring::npos);

  // adimport is a console program: never a console window of its own.
  ChildLaunch c;
  c.exe = L"C:\\x\\adimport.exe";
  CHECK(child_creation_flags(c) == 0);
  c.console_program = true;
  CHECK(child_creation_flags(c) == CREATE_NO_WINDOW);
  c.env_block = build_environment_block({});
  CHECK(child_creation_flags(c) == (CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT));
  DWORD err = 0;
  c.exe = L"C:\\no\\such\\dir\\adimport.exe";
  CHECK(start_child(c, &err) == nullptr && err != 0);

  // Preview settings file names.
  DWORD pid = 0;
  CHECK(parse_preview_settings_name(L"LongAfterDark-preview-1234.ini", &pid) && pid == 1234);
  CHECK(parse_preview_settings_name(L"longafterdark-PREVIEW-8.INI", &pid) && pid == 8);
  CHECK(parse_preview_settings_name(L"LongAfterDark-preview-4294967295.ini", &pid) && pid == 4294967295u);
  for (const wchar_t* n : {L"settings.ini", L"LongAfterDark-preview-.ini", L"LongAfterDark-preview-12a.ini",
                           L"LongAfterDark-preview-4294967296.ini", L"LongAfterDark-preview-12.ini.tmp",
                           L"LongAfterDark-preview-12", L"xLongAfterDark-preview-12.ini", L""}) {
    CHECK(!parse_preview_settings_name(n));
  }
  CHECK(preview_settings_path(L"C:\\T", 42) == L"C:\\T\\LongAfterDark-preview-42.ini");
  CHECK(preview_settings_path(L"C:\\T\\", 42) == L"C:\\T\\LongAfterDark-preview-42.ini");
  std::wstring td = temp_dir();
  CHECK(!td.empty() && td.back() != L'\\');

  // The sweep: files of dialogs that are gone go (never ours, never a live
  // one's, never anything else).
  std::wstring dir = join_path(td, L"adscr-unit-sweep-" + std::to_wstring(GetCurrentProcessId()));
  CHECK(ensure_dir(dir));
  // A process that has exited while we still hold its handle (its pid is
  // not reused yet), and a live one (started suspended).
  std::wstring self = exe_path();
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION done{}, live{};
  std::wstring cmd = quote_arg(self) + L" no-such-suite";
  CHECK(CreateProcessW(self.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &done));
  WaitForSingleObject(done.hProcess, 10000);
  CHECK(CreateProcessW(self.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr,
                       nullptr, &si, &live));
  std::wstring dead_file = preview_settings_path(dir, 0xFFFFFFFCu);   // never a pid
  std::wstring exited_file = preview_settings_path(dir, done.dwProcessId);
  std::wstring live_file = preview_settings_path(dir, live.dwProcessId);
  std::wstring own_file = preview_settings_path(dir, GetCurrentProcessId());
  std::wstring other = join_path(dir, L"LongAfterDark-preview-x.ini"), unrelated = join_path(dir, L"settings.ini");
  for (const auto& f : {dead_file, exited_file, live_file, own_file, other, unrelated}) CHECK(put_file(f));
  CHECK_EQ(sweep_stale_preview_settings(dir), 2);
  CHECK(!file_exists(dead_file) && !file_exists(exited_file));
  CHECK(file_exists(live_file) && file_exists(own_file) && file_exists(other) && file_exists(unrelated));
  CHECK_EQ(sweep_stale_preview_settings(dir), 0);
  CHECK_EQ(sweep_stale_preview_settings(join_path(dir, L"missing")), 0);
  TerminateProcess(live.hProcess, 0);
  WaitForSingleObject(live.hProcess, 10000);
  for (HANDLE h : {done.hProcess, done.hThread, live.hProcess, live.hThread}) CloseHandle(h);

  // Started the way the dialog starts adimport: no console window, the
  // arguments and the exit code come through.
  if (g_fakeimport.empty()) {
    fprintf(stderr, "dialog: no --fakeimport given; the child-process check is skipped\n");
  } else {
    std::wstring log = join_path(dir, L"fakeimport.log");
    ChildLaunch imp;
    imp.exe = g_fakeimport;
    imp.args = L"--gui";
    imp.console_program = true;
    imp.env_block = build_environment_block(
        {{L"FAKEIMPORT_LOG", log}, {L"FAKEIMPORT_EXIT", L"5"}, {L"FAKEIMPORT_CATALOG", L""}});
    HANDLE h = start_child(imp, &err);
    CHECK(h != nullptr);
    if (h) {
      CHECK(WaitForSingleObject(h, 20000) == WAIT_OBJECT_0);
      DWORD code = 0;
      GetExitCodeProcess(h, &code);
      CloseHandle(h);
      CHECK_EQ(code, (DWORD)5);
      std::string text;
      CHECK(read_file(log, text));
      CHECK_EQ(text, std::string("run\tconsole=0\targs=--gui\r\n"));
    }
    DeleteFileW(log.c_str());
  }
  for (const auto& f : {live_file, own_file, other, unrelated}) DeleteFileW(f.c_str());
  RemoveDirectoryW(dir.c_str());
}

// ---- the settings dialog's presentation (ui_model.h) and the lane probe -------

std::wstring g_fakehost;

bool inside(const Rc& outer, const Rc& r) { return r.empty() || outer.contains(r); }

// test.stops' string slider ("Never / Rarely / Often / Always").
const Control* c_stops_control(const Module* stops) {
  static Control none;
  if (!stops) return &none;
  for (const Control& c : stops->controls) {
    if (c.stepped()) return &c;
  }
  return &none;
}

void check_layout(const WindowLayout& L, int cw, int ch, bool random) {
  const Rc client{0, 0, cw, ch};
  const std::vector<std::pair<const char*, Rc>> all = {
      {"header", L.header}, {"footer", L.footer}, {"list_card", L.list_card}, {"details_card", L.details_card},
      {"options_card", L.options_card}, {"mode", L.mode}, {"list", L.list}, {"preview", L.preview},
      {"about", L.about}, {"panel", L.panel}, {"scale", L.scale},
      {"monitors", L.monitors}, {"ok", L.ok}, {"cancel", L.cancel}, {"preview_button", L.preview_button},
      {"import", L.import}, {"assets", L.assets}, {"module_title", L.module_title},
      {"sound_label", L.sound_label}, {"sound", L.sound}, {"volume_label", L.volume_label},
      {"volume_value", L.volume_value}, {"volume", L.volume}, {"sound_note", L.sound_note}};
  for (const auto& [name, r] : all) {
    if (r.empty() || !client.contains(r)) {
      fprintf(stderr, "layout %dx%d @%d: %s %d,%d %dx%d not inside the client\n", cw, ch, L.dpi, name, r.x, r.y, r.w, r.h);
      ++g_failures;
    }
  }
  auto apart = [&](const char* what, const std::vector<Rc>& v) {
    for (size_t i = 0; i < v.size(); ++i) {
      for (size_t j = i + 1; j < v.size(); ++j) {
        if (!v[i].empty() && !v[j].empty() && v[i].overlaps(v[j])) {
          fprintf(stderr, "layout %dx%d @%d: %s #%zu overlaps #%zu\n", cw, ch, L.dpi, what, i, j);
          ++g_failures;
        }
      }
    }
  };
  apart("bands", {L.header, L.body, L.footer});
  apart("left and cards", {L.list_card, L.details_card, L.options_card, L.mode, L.modules_label, L.rotation_summary,
                           L.check_all, L.check_none, L.duration_label, L.duration, L.per_monitor});
  apart("details", {L.preview, L.about, L.module_icon, L.module_title, L.module_badge, L.panel});
  apart("details with credits", {L.preview, L.credits, L.module_icon, L.module_title, L.module_badge, L.panel});
  apart("controls column", {L.module_icon, L.panel, L.defaults});
  apart("options", {L.scale_label, L.scale, L.monitors_label, L.monitors, L.stretch, L.sound_label, L.sound,
                    L.volume_label, L.volume_value, L.volume, L.sound_note});
  apart("footer", {L.import, L.assets, L.preview_button, L.ok, L.cancel});
  CHECK(inside(L.list_card, L.list));
  for (const Rc& r : {L.preview, L.about, L.credits, L.controls, L.module_icon, L.module_title, L.module_badge, L.panel,
                      L.defaults}) {
    CHECK(inside(L.details_card, r));
  }
  // Caps (DIPs, a pixel of rounding allowed): the controls column, the
  // preview, and each option's dropdown.
  const int d = L.dpi;
  CHECK(L.controls.w <= dip(kControlsMaxW, d) + 1);
  CHECK(L.preview.w <= dip(kPreviewMaxW, d) + 1);
  for (const Rc& r : {L.duration, L.scale, L.monitors, L.sound, L.volume}) CHECK(r.w <= dip(kComboMaxW, d) + 1);
  // The options' dropdowns start their halves of the card: wide enough for
  // "Classic — 480 lines" even at the minimum size.
  CHECK(L.scale.w >= dip(200, d) && std::abs(L.monitors.w - L.scale.w) <= 1);
  // The controls' faces line up with the column: the panel is grown by the
  // focus margin, and "Restore defaults" starts its glyph on the edge.
  CHECK(L.panel.x < L.controls.x && L.panel.right() > L.controls.right());
  CHECK(std::abs(L.defaults.x + dip(kLinkPad, d) - L.controls.x) <= 1);
  // "Restore defaults" (where it is pinned) has its text on the credits' first line.
  CHECK(std::abs(L.defaults.y + L.defaults.h / 2 - (L.credits.y + dip(8, d))) <= 1);
  CHECK(L.panel.bottom() < L.defaults.y);
  for (const Rc& r : {L.scale_label, L.scale, L.monitors_label, L.monitors, L.stretch, L.sound_label, L.sound,
                      L.volume_label, L.volume_value, L.volume, L.sound_note}) {
    CHECK(inside(L.options_card, r));
  }
  // "Stretch to fit the screen": under Resolution and Monitors, above Sound,
  // starting where Resolution does, a control's height.
  CHECK(!L.stretch.empty() && L.stretch.y > L.scale.bottom() && L.stretch.bottom() < L.sound_label.y);
  CHECK(std::abs(L.stretch.x - L.scale.x) <= 1 && L.stretch.h >= dip(32, d) - 1);
  // Sound and Volume (AUDIO.md §9): a second row under Resolution and
  // Monitors, in the same columns and as wide; Volume's readout ends its
  // label row at the slider's end; the note under both, across the card.
  CHECK(L.sound.x == L.scale.x && L.volume.x == L.monitors.x && L.sound.w == L.scale.w && L.volume.w == L.monitors.w);
  CHECK(L.sound_label.y > L.scale.bottom() && L.sound.y > L.sound_label.y && L.volume.y == L.sound.y);
  CHECK(L.volume_label.y == L.sound_label.y && L.volume_value.y == L.volume_label.y);
  CHECK(L.volume_label.x == L.volume.x && L.volume_value.right() == L.volume.right() &&
        L.volume_label.right() < L.volume_value.x);
  CHECK(L.sound_note.y >= L.sound.bottom() && L.sound_note.y >= L.volume.bottom());
  CHECK(L.sound_note.x == L.scale_label.x && L.sound_note.right() >= L.volume.right());
  // Everything within the content column, which is centred and capped.
  CHECK(L.content.w <= dip(kContentMaxW, d) + 1);
  CHECK(std::abs(L.content.x - (cw - L.content.right())) <= dip(1, d) + 1);
  for (const Rc& r : {L.logo, L.mode, L.list_card, L.details_card, L.options_card, L.import, L.cancel}) {
    CHECK(r.x >= L.content.x && r.right() <= L.content.right());
  }
  CHECK(L.logo.x == L.content.x && L.list_card.x == L.content.x && L.import.x == L.content.x);
  CHECK(L.details_card.right() == L.content.right() && L.cancel.right() == L.content.right());
  for (const Rc& r : {L.import, L.assets, L.preview_button, L.ok, L.cancel}) CHECK(inside(L.footer, r));
  CHECK(random == !L.check_all.empty());
  CHECK(random == !L.rotation_summary.empty());
  // "Change module every" belongs to Random: under the rotation line, in the
  // left column, its dropdown ending on the card's edge.
  CHECK(random == !L.duration.empty() && random == !L.duration_label.empty());
  if (random) {
    CHECK(client.contains(L.duration) && L.duration.y >= L.check_all.bottom());
    CHECK(L.duration_label.x == L.list_card.x && L.duration.right() == L.list_card.right());
    CHECK(L.duration_label.right() < L.duration.x && L.duration.bottom() < L.options_card.bottom() + 1);
  }
  // "A different module on each monitor" (Random, several monitors): under
  // "Change module every", across the left column (its box on the card's
  // edge), ending level with the options card.
  CHECK(random || L.per_monitor.empty());
  if (!L.per_monitor.empty()) {
    CHECK(client.contains(L.per_monitor) && L.per_monitor.y >= L.duration.bottom() + dip(8, L.dpi) - 1);
    CHECK(L.per_monitor.x == L.list_card.x && L.per_monitor.right() == L.list_card.right());
    CHECK(std::abs(L.per_monitor.bottom() - L.options_card.bottom()) <= 1 && L.per_monitor.h >= dip(32, L.dpi) - 1);
  }
  if (random) {
    // "Clear"'s text (its box less the link padding) ends on the card's edge.
    CHECK(std::abs(L.check_none.right() - dip(kLinkPad, L.dpi) - L.list_card.right()) <= 1);
    CHECK(L.check_all.right() <= L.check_none.x);
    CHECK(L.rotation_summary.right() <= L.check_all.x + dip(kLinkPad, L.dpi));
  }
  // The live preview is 16:9.
  CHECK(std::abs(L.preview.w * 9 - L.preview.h * 16) <= 16 * 2);
}

void test_ui() {
  // About: the name line goes, wrapped lines rejoin, meant breaks stay.
  CHECK_EQ(tidy_about("Flying Toasters!\n\nToaster meets toaster, toasters fall in love,\nToasters get married,\n"
                      "Baby Toasterettes!\n\nProgramming by Brock Wagenaar\nArt by Bob Ting",
                      "Flying Toasters!"),
           std::string("Toaster meets toaster, toasters fall in love,\nToasters get married,\nBaby Toasterettes!\n\n"
                       "Programming by Brock Wagenaar\nArt by Bob Ting"));
  CHECK_EQ(tidy_about("BORIS (tm)\r\n\r\nBoris will leap around, scratch the edges of\r\nyour screen, and preen.\r\n\r\n"
                      "Boris is a great display to use with\nMultiModule.  Try Boris and Meadow, for\nexample.\n\n"
                      "Windows Version by:\nMike Overlin\nSAPIEN Technologies, Inc.\nRichmond, CA",
                      "Boris"),
           std::string("Boris will leap around, scratch the edges of your screen, and preen.\n\n"
                       "Boris is a great display to use with MultiModule. Try Boris and Meadow, for example.\n\n"
                       "Windows Version by:\nMike Overlin\nSAPIEN Technologies, Inc.\nRichmond, CA"));
  CHECK_EQ(tidy_about("FLYING TOASTERS PRO\n\nText.", "Flying Toasters Pro"), std::string("Text."));
  // A long line stopping mid-phrase was wrapped (the catalog's own texts):
  // Art Critic, Rock Paper Scissors, Rainforest (twice) and Slide Show.
  CHECK_EQ(tidy_about("Images with a file size larger than your available memory\n(RAM) will not display.  Large "
                      "images may take seconds\nto display.",
                      "Art Critic"),
           std::string("Images with a file size larger than your available memory (RAM) will not display. Large images "
                       "may take seconds to display."));
  CHECK_EQ(tidy_about("Press \"Caps-lock\" to choose your fighter. Then,\nyou play the computer traditional Rock Paper\n"
                      "Scissors.",
                      "Rock Paper Scissors"),
           std::string("Press \"Caps-lock\" to choose your fighter. Then, you play the computer traditional Rock Paper "
                       "Scissors."));
  CHECK_EQ(tidy_about("This module features six creatures from Berkeley\nSystems' Living Puzzles\xE2\x84\xA2, Triazzle: "
                      "The Rainforest\nEdition.",
                      "Rainforest"),
           std::string("This module features six creatures from Berkeley Systems' Living Puzzles\xE2\x84\xA2, "
                       "Triazzle: The Rainforest Edition."));
  CHECK_EQ(tidy_about("The following file types are valid:\nJPEG, Windows BMP (including RLE), TARGA, PCX,\n"
                      "16-color GIF, and 256-color GIF.",
                      "Slide Show"),
           std::string("The following file types are valid:\nJPEG, Windows BMP (including RLE), TARGA, PCX, 16-color GIF, "
                       "and 256-color GIF."));
  // ...but credits, a list's lead-in and verse stay as they were.
  CHECK_EQ(tidy_about("Art by Craig Foster, Joe Kauffman, Jarir Maani, Laura Tullio\n3D art by Stephen Ekstrom\n"
                      "Concept and Windows Programming by Ben McCurtain\nMac Programming by Eric Pitcher",
                      "X"),
           std::string("Art by Craig Foster, Joe Kauffman, Jarir Maani, Laura Tullio\n3D art by Stephen Ekstrom\n"
                       "Concept and Windows Programming by Ben McCurtain\nMac Programming by Eric Pitcher"));
  CHECK_EQ(tidy_about("Cats above, cats below,\nCats pace to and fro,\nCats will pounce, cats will play,", "X"),
           std::string("Cats above, cats below,\nCats pace to and fro,\nCats will pounce, cats will play,"));
  CHECK_EQ(tidy_about("Hello there\n\nMore.", "Boris"), std::string("Hello there\n\nMore."));
  CHECK_EQ(tidy_about("", "X"), std::string(""));

  // "Change module every": presets, the file's own value kept, Never last.
  auto d = duration_choices(5);
  CHECK(d.size() == 12 && d.back().minutes == 0 && d.back().label == L"Never");
  CHECK(std::any_of(d.begin(), d.end(), [](const DurationChoice& c) { return c.minutes == 5 && c.label == L"5 minutes"; }));
  CHECK(std::any_of(d.begin(), d.end(), [](const DurationChoice& c) { return c.minutes == 60 && c.label == L"1 hour"; }));
  auto d7 = duration_choices(7);
  CHECK(d7.size() == 13);
  for (size_t i = 1; i + 1 < d7.size(); ++i) {
    if (d7[i].minutes == 7) CHECK(d7[i - 1].minutes == 5 && d7[i + 1].minutes == 10 && d7[i].label == L"7 minutes");
  }
  CHECK(duration_choices(0).size() == 12);
  CHECK(duration_choices(10080).size() == 13);
  CHECK(duration_label(1) == L"1 minute" && duration_label(120) == L"2 hours" && duration_label(90) == L"90 minutes");

  CHECK(rotation_summary(84, 84) == L"All 84 in rotation");
  CHECK(rotation_summary(12, 84) == L"12 of 84 in rotation");
  CHECK(rotation_summary(0, 84) == L"None in rotation");
  CHECK(rotation_summary(0, 0).empty());
  // Some checked modules can't run (no Classic lane in this host): said so.
  CHECK(rotation_summary(84, 84, 84) == L"All 84 in rotation");
  CHECK(rotation_summary(84, 84, 23) == L"All 84 selected \u00B7 23 can run now");
  CHECK(rotation_summary(12, 84, 5) == L"12 of 84 selected \u00B7 5 can run now");
  CHECK(rotation_summary(0, 84, 0) == L"None in rotation");
  // Byte-identical copies play once per pass: the line says how many
  // different modules rotate (QA rotation-count-vs-dedupe). What can't run
  // still comes first; no copies, no second half.
  CHECK(rotation_summary(202, 202, 202, 129) == L"All 202 selected · 129 distinct");
  CHECK(rotation_summary(40, 202, 40, 31) == L"40 of 202 selected · 31 distinct");
  CHECK(rotation_summary(202, 202, 202, 202) == L"All 202 in rotation");
  CHECK(rotation_summary(12, 84, -1, 12) == L"12 of 84 in rotation");
  CHECK(rotation_summary(202, 202, 150, 129) == L"All 202 selected · 150 can run now");
  CHECK(rotation_summary(0, 202, 0, 0) == L"None in rotation");
  // A list of one module (Marvel Comics Screen Posters, alone or filtered
  // to): "1", never "All 1"; two are "All 2" again.
  CHECK(rotation_summary(1, 1) == L"1 in rotation");
  CHECK(rotation_summary(1, 1, 1, 1) == L"1 in rotation");
  CHECK(rotation_summary(1, 1, 0) == L"1 selected · 0 can run now");
  CHECK(rotation_summary(1, 1, 0, 1) == L"1 selected · 0 can run now");
  CHECK(rotation_summary(0, 1) == L"None in rotation" && rotation_summary(0, 1, 0, 0) == L"None in rotation");
  CHECK(rotation_summary(2, 2) == L"All 2 in rotation" && rotation_summary(1, 2) == L"1 of 2 in rotation");
  CHECK(rotation_summary(2, 314, 2, 1) == L"2 of 314 selected · 1 distinct");
  // Its tooltip: the copies, and the module the file names to play first.
  CHECK(rotation_tip(202, 202, L"").empty() && rotation_tip(0, 0, L"").empty());
  CHECK(rotation_tip(202, 129, L"") ==
        L"A module that is on several of the releases checked plays once in each pass, so 129 different modules "
        L"take turns.");
  // Copies of one module alone (the same module checked on two releases):
  // nothing takes turns.
  CHECK(rotation_tip(2, 1, L"") ==
        L"A module that is on several of the releases checked plays once in each pass, so 1 module is in rotation.");
  CHECK(rotation_tip(3, 2, L"") ==
        L"A module that is on several of the releases checked plays once in each pass, so 2 different modules take "
        L"turns.");
  CHECK(rotation_tip(12, 12, L"Flying Toasters") ==
        L"Flying Toasters plays first (your settings name it), then the rotation.");
  CHECK(rotation_tip(202, 129, L"Fish") == rotation_tip(202, 129, L"") + L"\n\n" + rotation_tip(1, 1, L"Fish"));

  // Names the Windows 3.x control panel cut short are shown whole (ids, and
  // settings.ini, unchanged), keyed on the Classic module's own name so every
  // package's copy is fixed, keeping its " (<package>)" suffix
  // (INTERACTION.md §9.3); other names are left alone.
  {
    Catalog n;
    CHECK(parse_catalog(R"j({"modules":[
      {"id":"classic.strange","lane":"ne16","displayName":"Strange Attract","moduleName":"Strange Attract","path":"A.AD"},
      {"id":"classic.confetti","lane":"ne16","displayName":"ConfettiFactory","moduleName":"ConfettiFactory","path":"B.AD"},
      {"id":"classic.slide","lane":"ne16","displayName":"SlideShow","moduleName":"SlideShow","path":"C.AD"},
      {"id":"classic.om","lane":"ne16","displayName":"Om Appliances","moduleName":"Om Appliances","path":"D.AD"},
      {"id":"ad32.slides3","lane":"ne16","displayName":"SlideShow (After Dark 3.2)","moduleName":"SlideShow","path":"E.AD"},
      {"id":"ad10.slide","lane":"ne16","displayName":"SlideShow (10th Anniversary)","moduleName":"SlideShow","path":"F.AD"},
      {"id":"ad32.confetti","lane":"ne16","displayName":"ConfettiFactory (After Dark 3.2)","moduleName":"ConfettiFactory","path":"G.AD"},
      {"id":"ad32.mmas","lane":"ne16","displayName":"Om Appliances (After Dark 3.2)","moduleName":"Om Appliances","path":"H.AD"},
      {"id":"x.slideshow2","lane":"ne16","displayName":"SlideShow Deluxe","moduleName":"SlideShow Deluxe","path":"I.AD"},
      {"id":"ad40.slide","lane":"pe32","displayName":"SlideShow","moduleName":"SlideShow","path":"J.AD"},
      {"id":"old.slide","lane":"ne16","displayName":"SlideShow","path":"K.AD"},
      {"id":"old.slide2","lane":"ne16","displayName":"SlideShow (Other)","path":"L.AD"}]})j", n, nullptr));
    const std::vector<std::pair<const char*, const char*>> want = {
        {"classic.strange", "Strange Attractors"},
        {"classic.confetti", "Confetti Factory"},
        {"classic.slide", "Slide Show"},
        {"classic.om", "OM Appliances"},
        {"ad32.slides3", "Slide Show (After Dark 3.2)"},
        {"ad10.slide", "Slide Show (10th Anniversary)"},
        {"ad32.confetti", "Confetti Factory (After Dark 3.2)"},
        {"ad32.mmas", "OM Appliances (After Dark 3.2)"},
        {"x.slideshow2", "SlideShow Deluxe"},       // another module's name
        {"ad40.slide", "SlideShow"},                // AD4 names are never cut
        {"old.slide", "Slide Show"},                // no moduleName: displayName is the key
        {"old.slide2", "SlideShow (Other)"},        // ...and then it must match whole
    };
    CHECK(n.modules.size() == want.size());
    for (size_t i = 0; i < n.modules.size() && i < want.size(); ++i) {
      if (n.modules[i].id != want[i].first || n.modules[i].display_name != want[i].second) {
        fprintf(stderr, "  display name %s: \"%s\" (want %s \"%s\")\n", n.modules[i].id.c_str(),
                n.modules[i].display_name.c_str(), want[i].first, want[i].second);
        CHECK(false);
      }
    }
  }

  // Status text: no closing full stop (the releases' own line is checked in
  // the releases suite). Not every release is After Dark's: the words fit all fourteen.
  CHECK(assets_summary({}) == L"Nothing imported yet");
  // The not-imported welcome: what importing does.
  CHECK(welcome_text().find(L"The screen saver runs the original modules of After Dark and Star Wars Screen "
                            L"Entertainment from your own discs.\n\n"
                            L"Import them from any of your discs (fourteen releases are supported), a disc image, or "
                            L"the Internet Archive download.") == 0);
  CHECK(welcome_text().find(L"After Dark discs") == std::wstring::npos);
  Catalog c;
  CHECK(parse_catalog(fixture("catalog-win.json"), c, nullptr));
  AssetCounts a = count_assets(c, {true, true, true, true, false});
  CHECK(a.total == 5 && a.releases == 1 && a.missing == 1 && a.release.empty());
  CHECK(assets_summary(a) == L"5 modules imported \u00B7 1 missing \u2014 import again to restore");

  // The window at 100/125/150/200%: at the first-open size, the minimum and a
  // large one, in both modes.
  for (int dpi : {96, 120, 144, 192}) {
    for (auto [w, h] : {std::pair{kDesignClientW, kDesignClientH}, std::pair{kMinClientW, kMinClientH},
                        std::pair{1600, 1000}}) {
      for (bool random : {false, true}) {
        int cw = dip(w, dpi), ch = dip(h, dpi);
        check_layout(layout_window({cw, ch, dpi, random}), cw, ch, random);
      }
    }
  }
  // Designed on a 4-DIP grid: at 100% the main edges land on it.
  WindowLayout L96 = layout_window({kDesignClientW, kDesignClientH, 96, true});
  for (const Rc& r : {L96.mode, L96.list_card, L96.details_card, L96.options_card, L96.preview, L96.ok, L96.cancel,
                      L96.import, L96.controls, L96.sound, L96.volume, L96.sound_note}) {
    CHECK(r.x % 4 == 0 && r.y % 4 == 0 && r.w % 4 == 0 && r.h % 4 == 0);
  }
  // 200% is 100% doubled.
  WindowLayout L192 = layout_window({2 * kDesignClientW, 2 * kDesignClientH, 192, true});
  for (auto [a1, b1] : {std::pair{L96.list, L192.list}, std::pair{L96.preview, L192.preview}, std::pair{L96.ok, L192.ok},
                        std::pair{L96.panel, L192.panel}}) {
    CHECK(std::abs(b1.x - 2 * a1.x) <= 1 && std::abs(b1.y - 2 * a1.y) <= 1 && std::abs(b1.w - 2 * a1.w) <= 1 &&
          std::abs(b1.h - 2 * a1.h) <= 1);
  }
  // Single module: no rotation row, and the list takes its room.
  WindowLayout single = layout_window({kDesignClientW, kDesignClientH, 96, false});
  CHECK(single.check_all.empty() && single.list_card.h > L96.list_card.h);

  // "A different module on each monitor" (per_monitor_choice): shown in
  // Random with several monitors, enabled while every monitor plays; hidden
  // with one monitor and in Single.
  CHECK((per_monitor_choice(true, true, 2) == PerMonitorChoice{true, true}));
  CHECK((per_monitor_choice(true, true, 3) == PerMonitorChoice{true, true}));
  CHECK((per_monitor_choice(true, false, 2) == PerMonitorChoice{true, false}));   // Primary monitor only: greyed
  for (bool all : {false, true}) {
    CHECK((per_monitor_choice(true, all, 1) == PerMonitorChoice{}));
    CHECK((per_monitor_choice(true, all, 0) == PerMonitorChoice{}));
    for (int n : {1, 2, 4}) CHECK((per_monitor_choice(false, all, n) == PerMonitorChoice{}));
  }
  // Its row, under "Change module every" (which moves up by as much: 8 DIP
  // and its 32), takes the room from the list; at every scale, at the
  // first-open, the minimum and a large size, with and without the strip.
  // In Single it changes nothing.
  for (int dpi : {96, 120, 144, 168, 192}) {
    for (auto [w, h, tiles] : {std::tuple{kDesignClientW, kDesignClientH, 0}, std::tuple{kMinClientW, kMinClientH, 0},
                               std::tuple{kMinClientW, kMinClientHStrip, 7}, std::tuple{kDesignClientW, kDesignClientHStrip, 12},
                               std::tuple{1600, 1000, 12}}) {
      const int cw = dip(w, dpi), ch = dip(h, dpi);
      for (bool random : {false, true}) {
        LayoutInput without{cw, ch, dpi, random}, with = without;
        without.strip_tiles = with.strip_tiles = tiles;
        with.per_monitor = true;
        const WindowLayout a = layout_window(without), b = layout_window(with);
        check_layout(b, cw, ch, random);
        CHECK(a.per_monitor.empty() && random == !b.per_monitor.empty());
        if (!random) {
          CHECK(a.list_card == b.list_card && a.list == b.list && b.duration.empty());
          continue;
        }
        const int step = dip(40, dpi);
        CHECK(std::abs((a.list_card.h - b.list_card.h) - step) <= 1 && a.list_card.y == b.list_card.y);
        CHECK(std::abs((a.duration.y - b.duration.y) - step) <= 1 && std::abs((a.check_all.y - b.check_all.y) - step) <= 1);
        CHECK(std::abs(b.per_monitor.y - a.duration.y) <= 1);   // where "Change module every" was
        CHECK(a.details_card == b.details_card && a.options_card == b.options_card && a.preview == b.preview);
        // The list keeps room for a few rows (40 DIP each) even so.
        CHECK(b.list.h >= dip(4 * 40, dpi));
      }
    }
  }
  // A large window: the content stops growing at its cap and is centred;
  // the controls column and dropdowns stop at theirs, the preview takes the
  // room, and the two dropdowns each start their half of the options card.
  WindowLayout big = layout_window({1600, 1000, 96, true});
  CHECK(big.content.w == kContentMaxW && big.content.x == (1600 - kContentMaxW) / 2);
  CHECK(big.controls.w == kControlsMaxW && big.scale.w == kComboMaxW && big.monitors.w == kComboMaxW);
  CHECK(big.sound.w == kComboMaxW && big.volume.w == kComboMaxW);
  CHECK(big.preview.w > L96.preview.w);
  CHECK(std::abs((big.monitors.x - big.scale.x) - (big.options_card.right() - 20 - big.scale.x + 24) / 2) <= 1);
  // A module name too long for one line: two, the chips under them and the
  // settings moved down by as much.
  {
    LayoutInput two_in{kDesignClientW, kDesignClientH, 96, true};
    two_in.title_lines = 2;
    WindowLayout two = layout_window(two_in);
    CHECK(two.module_title.h == 44 && L96.module_title.h == 28);
    CHECK(two.module_badge.y >= two.module_title.bottom() && two.panel.y == L96.panel.y + 20);
    check_layout(two, kDesignClientW, kDesignClientH, true);
  }

  // The footer's credit (layout_footer_credit), measured in the real caption face,
  // at 100-250%, at the first-open, the minimum and a large size, beside the
  // assets line's texts (the welcome's starting where Import is): whenever it
  // shows, its box lies in the footer clear of the assets line's text and of
  // Preview by the footer's gap, as far from one as from the other, on the
  // buttons' centre line, holding the phrase on one line (the lead, a space,
  // the name) a link's padding in from its ends, over nothing else; when it
  // doesn't show, there was no room for it.
  {
    HDC dc = CreateCompatibleDC(nullptr);
    // The fourteen releases' line, "314 modules from 14 releases", is as
    // long as the twelve's ("284 modules from 12 releases"), a digit longer
    // than the seven's ("232 modules from 7 releases"), whose digits were
    // already wider in the caption face, Segoe UI Variable Small, than the
    // six's ("216 modules from 6 releases").
    const wchar_t* texts[] = {L"Nothing imported yet", L"314 modules from 14 releases",
                              L"84 modules from After Dark 4.0 Deluxe",
                              L"314 modules from 14 releases · 2 missing — import again to restore"};
    int shown = 0, hidden = 0, min_twelve = 0;
    std::string min_twelve_at;   // the scales it fits the narrowest window at
    for (int dpi = 96; dpi <= 240; dpi += 24) {
      adw::ui::Theme t;
      t.set_dpi(dpi);
      auto width = [&](const std::wstring& s) { return (int)adw::ui::measure_text(dc, s, t.fonts.caption).cx; };
      FooterCreditInput in;
      in.lead_w = width(kFooterCreditLead);
      in.space_w = width(L"a b") - width(L"ab");
      in.name_w = width(kFooterCreditName);
      in.line_h = (int)adw::ui::measure_text(dc, kFooterCreditName, t.fonts.caption).cy;
      CHECK(in.lead_w > 0 && in.space_w > 0 && in.name_w > 0 && in.line_h > 0);
      const int gap = dip(kCreditGapDip, dpi), pad = dip(kLinkPad, dpi);
      const int box_w = pad + in.lead_w + in.space_w + in.name_w + pad;
      struct Size {
        int w, h, tiles;
      };
      for (const Size& sz : {Size{kDesignClientW, kDesignClientHStrip, 12}, Size{kMinClientW, kMinClientHStrip, 12},
                             Size{kMinClientW, kMinClientH, 0}, Size{1600, 1000, 12}}) {
        LayoutInput li{dip(sz.w, dpi), dip(sz.h, dpi), dpi, true};
        li.strip_tiles = sz.tiles;
        const WindowLayout L = layout_window(li);
        for (const wchar_t* text : texts) {
          const bool welcome_line = std::wstring(text) == L"Nothing imported yet";
          // The assets line as the dialog sets it (place_assets_status):
          // wrapped in its box, which starts where Import is in the welcome.
          const int ax = welcome_line ? L.import.x : L.assets.x, aw = L.assets.right() - ax;
          RECT m{0, 0, aw, 0};
          HGDIOBJ old = SelectObject(dc, t.fonts.caption);
          DrawTextW(dc, text, -1, &m, DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
          SelectObject(dc, old);
          in.assets_right = ax + std::min<int>(aw, m.right - m.left);
          const FooterCreditLayout C = layout_footer_credit(L, in);
          const int room = L.preview_button.x - in.assets_right - 2 * gap;
          if (!C.shown) {
            ++hidden;
            if (room >= box_w) fprintf(stderr, "credit @%d %dx%d \"%ls\": hidden with %d px of room for %d\n", dpi, sz.w,
                                       sz.h, text, room, box_w);
            CHECK(room < box_w);
            continue;
          }
          ++shown;
          CHECK(room >= box_w && C.box.w == box_w && C.box.h == dip(kCreditLinkHDip, dpi));
          CHECK(L.footer.contains(C.box));
          CHECK(C.box.x >= in.assets_right + gap && C.box.right() <= L.preview_button.x - gap);
          CHECK(std::abs((C.box.x - in.assets_right) - (L.preview_button.x - C.box.right())) <= 1);
          const int cy = L.preview_button.y + L.preview_button.h / 2;
          CHECK(std::abs(C.box.y + C.box.h / 2 - cy) <= 1 && std::abs(C.lead.y + C.lead.h / 2 - cy) <= 1);
          CHECK(C.lead.y == C.name.y && C.lead.h == in.line_h && C.name.h == in.line_h);
          CHECK(C.lead.w == in.lead_w && C.name.w == in.name_w);
          CHECK(C.lead.x == C.box.x + pad && C.name.x == C.lead.right() + in.space_w && C.name.right() + pad == C.box.right());
          CHECK(C.box.y <= C.lead.y && C.lead.bottom() <= C.box.bottom());
          for (const Rc& r : {L.import, L.preview_button, L.ok, L.cancel}) CHECK(!C.box.overlaps(r));
          // The status line it sits beside is never under it.
          CHECK(C.box.x > in.assets_right);
        }
        // Where it matters: with fourteen releases it shows at the first-open
        // size (with room to spare) at every scale; one release's long title
        // and the assets line at its longest (files missing) leave it no room
        // in the minimum window. (In the minimum window beside the releases'
        // line it fits at some scales only: beside the seven's, in Segoe UI
        // Variable at 5 of 7 on Windows 11, not at 150% or 200%, where it fit
        // beside the six's at all 7. So that case is only reported: whether
        // it shows there depends on the face, and where it has no room it
        // hides, as it should, never clipped.)
        auto credit_for = [&](const wchar_t* text) {
          RECT m{0, 0, L.assets.w, 0};
          HGDIOBJ old = SelectObject(dc, t.fonts.caption);
          DrawTextW(dc, text, -1, &m, DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
          SelectObject(dc, old);
          FooterCreditInput i2 = in;
          i2.assets_right = L.assets.x + std::min<int>(L.assets.w, m.right - m.left);
          return layout_footer_credit(L, i2);
        };
        if (sz.w == kDesignClientW) CHECK(credit_for(texts[1]).shown);
        if (sz.w == kMinClientW) {
          CHECK(!credit_for(texts[2]).shown && !credit_for(texts[3]).shown);
          if (sz.h == kMinClientHStrip && credit_for(texts[1]).shown) {
            ++min_twelve;
            min_twelve_at += (min_twelve_at.empty() ? "" : ",") + std::to_string(dpi * 100 / 96) + "%";
          }
        }
      }
    }
    printf("ui: the credit fits beside \"314 modules from 14 releases\" in the narrowest window at %d of 7 scales (%s)\n",
           min_twelve, min_twelve_at.c_str());
    CHECK(shown > 0 && hidden > 0);
    DeleteDC(dc);
    // Degenerate input: nothing measured, no credit.
    CHECK(!layout_footer_credit(layout_window({kDesignClientW, kDesignClientH, 96, true}), FooterCreditInput{}).shown);
  }

  // The settings panel: rows top to bottom, then "Restore defaults".
  const Module* stops = c.find("test.stops");
  CHECK(stops != nullptr);
  if (stops) {
    for (int dpi : {96, 144, 192}) {
      const int w = dip(280, dpi);
      PanelLayout P = layout_panel(stops->controls, w, dpi);
      CHECK(P.rows.size() == 4 && P.empty_note.empty());
      int bottom = 0;
      for (const PanelRow& r : P.rows) {
        // Rows follow one another and hold their parts (they scroll whole).
        CHECK(r.top >= bottom && r.bottom > r.top);
        for (const Rc& part : {r.label, r.input, r.value}) {
          if (part.empty()) continue;
          CHECK(part.y >= r.top && part.bottom() <= r.bottom && part.x >= 0 && part.right() <= w);
        }
        bottom = r.bottom;
      }
      CHECK(P.content_h == bottom);
      // The module button is a read-only row: its name, and a note under it.
      CHECK(!P.rows[0].label.empty() && !P.rows[0].input.empty() && P.rows[0].input.y > P.rows[0].label.y);
    }
  }
  PanelLayout none = layout_panel({}, 280, 96);
  CHECK(none.rows.empty() && !none.empty_note.empty());
  {
    // A dropdown with a blank name has no label row.
    Control blank;
    blank.type = ControlType::popup;
    blank.name = " ";
    blank.items = {"Birds", "Gnats"};
    Control named = blank;
    named.name = "Kind:";
    PanelLayout B = layout_panel({blank, named}, 280, 96);
    CHECK(B.rows[0].label.empty() && B.rows[0].input.y == B.rows[0].top);
    CHECK(!B.rows[1].label.empty() && B.rows[1].input.y > B.rows[1].label.y);
    CHECK(blank_label(" ") && blank_label("") && blank_label(" : ") && !blank_label("Birds"));
  }

  // String sliders: runs of the same label are one stop, and a run stores
  // the default when it holds it, else the appended (bold) stop, else its last.
  {
    Catalog y;
    CHECK(parse_catalog(R"({"modules":[{"id":"c.ybyh","path":"Y.AD","controls":[
      {"index":0,"name":"Contestants:","type":"slider","items":["Text Only","1","2","3","3","3"],
       "values":[0,25,50,75,100,100],"default":100,"defaultStop":5,"boldStop":5},
      {"index":1,"name":"Timer:","type":"slider","items":["10 s","20 s","30 s","45 s","45 s","45 s"],
       "values":[0,25,50,75,100,100],"default":0,"defaultStop":0,"boldStop":5},
      {"index":2,"name":"Music:","type":"slider","items":["Never","Always","Always"],"values":[0,60,100],
       "default":0,"defaultStop":0}]}]})", y, nullptr));
    const Module* m = y.find("c.ybyh");
    CHECK(m && m->controls.size() == 3);
    if (m && m->controls.size() == 3) {
      const Control& contestants = m->controls[0];
      const Control& timer = m->controls[1];
      const Control& music = m->controls[2];
      CHECK(contestants.has_bold && contestants.bold_value == 100 && !music.has_bold);
      VisualStops a = visual_stops(contestants);
      CHECK((a.labels == std::vector<std::string>{"Text Only", "1", "2", "3"}));
      CHECK((a.value == std::vector<int>{0, 25, 50, 100}));   // "3" holds the default, 100
      CHECK(a.run_of_value(contestants, 75) == 3 && a.run_of_value(contestants, 100) == 3 && a.run_of_value(contestants, 0) == 0);
      VisualStops b = visual_stops(timer);
      CHECK(b.count() == 4 && b.value[3] == 100);            // not the default's run: the bold stop's value
      VisualStops c = visual_stops(music);
      CHECK(c.count() == 2 && c.value[1] == 100);            // neither: the run's last value
      VisualStops d = visual_stops(*c_stops_control(stops));
      CHECK(d.count() == 4 && (d.value == std::vector<int>{0, 33, 66, 100}));   // distinct labels: unchanged
    }
  }

  // Thumbnails: the busiest square of a frame, and none of a blank one.
  {
    const int fw = 856, fh = 480;
    auto blank = [](int, int) -> unsigned { return 0x000000; };
    CHECK(choose_thumb_crop(fw, fh, blank).busy == 0.0);
    // A 60x60 sprite near the bottom-right corner: all of it, filling a good part of the tile.
    auto sprite = [](int x, int y) -> unsigned { return x >= 760 && x < 820 && y >= 380 && y < 440 ? 0xE0C040u : 0u; };
    ThumbCrop c = choose_thumb_crop(fw, fh, sprite);
    CHECK(c.side >= fh / 4 && c.side <= fh / 2 && c.busy >= 0.04);
    CHECK(c.x <= 760 && c.x + c.side >= 820 && c.y <= 380 && c.y + c.side >= 440);
    CHECK(c.x >= 0 && c.y >= 0 && c.x + c.side <= fw && c.y + c.side <= fh);
    // A pattern over the whole screen: a third-height square of it (a tile shows detail, not a smudge).
    auto stripes = [](int x, int) -> unsigned { return (x / 8) % 2 ? 0xFFFFFFu : 0x2040A0u; };
    ThumbCrop f = choose_thumb_crop(fw, fh, stripes);
    CHECK(f.side == fh / 3 && f.busy > 0.3);
    // Worth keeping? Contrast, colours, and not mostly one flat tone.
    const int n = 96;
    auto judge = [&](auto px) {
      std::vector<unsigned char> bgr((size_t)n * n * 3);
      for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
          const unsigned v = px(x, y);
          unsigned char* d = &bgr[((size_t)y * n + x) * 3];
          d[0] = v & 0xFF;
          d[1] = (v >> 8) & 0xFF;
          d[2] = (v >> 16) & 0xFF;
        }
      }
      return judge_thumb(bgr.data(), n, n);
    };
    CHECK(!judge([](int, int) { return 0x000000u; }).good);                                  // black
    CHECK(!judge([](int, int) { return 0xE0E0E0u; }).good);                                  // one flat tone (Tunnel's white)
    CHECK(!judge([](int x, int y) { return x > 44 && x < 52 && y > 44 && y < 52 ? 0xFFFFFFu : 0u; }).good);   // a speck on black
    CHECK(!judge([](int x, int) { return x < 48 ? 0x000000u : 0xFFFFFFu; }).good);           // contrast, but two colours
    // A maze (white walls, a blue path, black): three colours, but a picture.
    CHECK(judge([](int x, int y) { return x % 12 == 0 || y % 12 == 0 ? 0xFFFFFFu : (x / 12 == 3 ? 0x2020E0u : 0u); }).good);
    // Sparse line art: a few thin lines on black read as specks in a tile.
    CHECK(!judge([](int x, int y) { return (x == 20 || x == 60 || y == 40) ? 0xE0E0E0u : 0u; }).good);
    auto rich = [](int x, int y) { return (unsigned)(((x * 5) & 0xFF) << 16 | ((y * 5) & 0xFF) << 8 | (((x + y) * 3) & 0xFF)); };
    ThumbQuality q = judge(rich);
    CHECK(q.good && q.score > 0 && q.stddev >= kThumbMinStddev && q.colours >= kThumbMinColours && q.flat <= kThumbMaxFlat);
    // A shaded sprite on black passes.
    auto shaded = [](int x, int y) {
      if (x < 24 || x >= 72 || y < 24 || y >= 72) return 0u;
      return (unsigned)((x * 3) << 16 | (y * 3) << 8 | (x + y));
    };
    CHECK(judge(shaded).good);
    // A small 16-colour sprite on black (a tenth of the tile) passes; so does
    // a soft, colourful pattern of little contrast; dim noise on black doesn't.
    auto sprite16 = [](int x, int y) {
      if (x < 34 || x >= 64 || y < 34 || y >= 64) return 0u;
      static const unsigned vga[] = {0xAA0000u, 0x00AA00u, 0xAAAA00u, 0x0000AAu, 0xAA00AAu, 0x00AAAAu, 0xAAAAAAu, 0xFFFF55u};
      return vga[((x / 4) + (y / 4)) % 8];
    };
    CHECK(judge(sprite16).good);
    auto soft = [](int x, int y) {
      return (unsigned)((0x50 + (x * 7 + y * 3) % 70) << 16 | (0x80 + (x * 3 + y * 5) % 70) << 8 | (0x50 + (x + y * 7) % 70));
    };
    CHECK(judge(soft).good);
    auto dim = [](int x, int y) { const unsigned v = (unsigned)((x * 13 + y * 7) % 18); return v << 16 | v << 8 | v; };
    CHECK(!judge(dim).good);
    // A bright sprite beside dim lines across the screen: the crop goes to the sprite.
    auto lines_and_sprite = [](int x, int y) -> unsigned {
      if (x >= 100 && x < 170 && y >= 200 && y < 300) return 0xE04080u;
      return (x % 60 == 0 || y % 45 == 0) ? 0x606060u : 0x000000u;
    };
    ThumbCrop ls = choose_thumb_crop(fw, fh, lines_and_sprite);
    CHECK(ls.x <= 100 && ls.x + ls.side >= 170 && ls.y <= 200 && ls.y + ls.side >= 300);
  }

  // What the host can do (`--capabilities`, INTERACTION.md §3.3), parsed,
  // then asked of fakehost standing in for adhostwin.
  {
    HostCapabilities c = parse_capabilities("lanes=pe32,ne16 configure=pe32 status=1 state=1 seed=1\r\n");
    CHECK(c.known && c.has_lane("pe32") && c.has_lane("ne16") && !c.has_lane("x"));
    CHECK(c.can_configure("pe32") && !c.can_configure("ne16"));
    CHECK(c.status && c.state && c.seed);
    CHECK(c.line == "lanes=pe32,ne16 configure=pe32 status=1 state=1 seed=1");
    HostCapabilities none = parse_capabilities("lanes= configure= status=1 state=1 seed=1");
    CHECK(none.known && none.lanes.empty() && none.configure.empty());
    CHECK(!parse_capabilities("").known);
    CHECK(!parse_capabilities("adhostwin: cannot open --capabilities").known);   // an older host
    CHECK(!probe_capabilities(L"C:\\no\\such\\adhostwin.exe").known);
    // The module ABIs it runs (PLAN: abis=). Without the key, a host from
    // before them: After Dark's alone, so an Intermission module doesn't run.
    CHECK((c.abis == std::vector<std::string>{"afterdark"}) && c.has_abi("afterdark") && !c.has_abi("intermission"));
    CHECK(c.runs("pe32", "afterdark") && c.runs("ne16", "") && !c.runs("ne16", "intermission") && !c.runs("x", "afterdark"));
    HostCapabilities imx =
        parse_capabilities("lanes=pe32,ne16 configure=pe32,ne16 abis=afterdark,intermission status=1 state=1 seed=1 audio=1");
    CHECK(imx.known && (imx.abis == std::vector<std::string>{"afterdark", "intermission"}));
    CHECK(imx.runs("ne16", "intermission") && imx.runs("pe32", "afterdark") && !imx.runs("pe32x", "intermission"));
    CHECK(!imx.runs("ne16", "someday"));
    // Listed as it stands: "abis=" alone runs no module ABI at all.
    HostCapabilities no_abi = parse_capabilities("lanes=pe32,ne16 abis=");
    CHECK(no_abi.known && no_abi.abis.empty() && !no_abi.runs("pe32", "afterdark"));
    // A host that didn't answer is taken to run everything (a module whose
    // lane it lacks exits 3 there, and the dialog marks that one).
    CHECK(HostCapabilities{}.runs("ne16", "intermission") && HostCapabilities{}.runs("anything", "someday"));
    // Num Lock (INTERACTION.md §3.2): numlock=1, and only that, is a host
    // that keeps the toggle. NUMLOCK lines go only to one that said so
    // (another doesn't number them); ADNUMLOCK to every host but one that
    // answered without it.
    HostCapabilities nl = parse_capabilities(
        "lanes=pe32,ne16 configure=pe32,ne16 abis=afterdark,intermission status=1 state=1 seed=1 audio=1 numlock=1");
    CHECK(nl.known && nl.numlock && nl.takes_numlock_lines() && nl.takes_numlock_env());
    CHECK(nl.runs("ne16", "intermission") && nl.status && nl.seed);   // the other keys as before
    for (const char* line : {"lanes=pe32,ne16 status=1 state=1 seed=1 audio=1", "lanes=pe32 numlock=0",
                             "lanes=pe32 numlock=", "lanes=pe32 numlock=yes", "lanes=pe32 NUMLOCK=1"}) {
      HostCapabilities no = parse_capabilities(line);
      CHECK(no.known && !no.numlock && !no.takes_numlock_lines() && !no.takes_numlock_env());
    }
    CHECK((parse_capabilities("numlock=1 lanes=pe32").numlock));   // wherever it is on the line
    CHECK(!HostCapabilities{}.takes_numlock_lines() && HostCapabilities{}.takes_numlock_env());   // no answer (yet)
    CHECK(!parse_capabilities("numlock=1").known);   // no lanes=: not an answer, whatever else it says
  }
  // What the dialog does with a module (module_run): it waits while the host
  // is asked; "Coming soon" when the host doesn't list its lane or its ABI,
  // or when a run of that very module exited 3 -- which never spreads to the
  // other modules of its lane or ABI (an exit 3 used to turn every Classic
  // module "Coming soon").
  {
    Catalog six;
    CHECK(parse_catalog(fixture("catalog-six.json"), six, nullptr));
    const Module* vader = six.find("swse.vader");
    const Module* beta = six.find("classic.beta");
    const Module* alpha = six.find("ad40.alpha");
    CHECK(vader && beta && alpha);
    if (vader && beta && alpha) {
      const HostCapabilities old_host = parse_capabilities("lanes=pe32,ne16 configure=pe32,ne16 status=1 state=1 seed=1");
      const HostCapabilities new_host =
          parse_capabilities("lanes=pe32,ne16 configure=pe32,ne16 abis=afterdark,intermission status=1");
      const HostCapabilities no_ne16 = parse_capabilities("lanes=pe32 configure=pe32 abis=afterdark,intermission");
      const HostCapabilities silent;   // no answer
      CHECK(module_run(*vader, old_host, true, false) == ModuleRun::waiting);
      CHECK(module_run(*alpha, new_host, true, false) == ModuleRun::waiting);   // every lane waits for the answer
      CHECK(module_run(*vader, old_host, false, false) == ModuleRun::coming_soon);
      CHECK(module_run(*beta, old_host, false, false) == ModuleRun::runs);
      CHECK(module_run(*vader, new_host, false, false) == ModuleRun::runs);
      CHECK(module_run(*vader, no_ne16, false, false) == ModuleRun::coming_soon);
      CHECK(module_run(*beta, no_ne16, false, false) == ModuleRun::coming_soon);
      CHECK(module_run(*alpha, no_ne16, false, false) == ModuleRun::runs);
      CHECK(module_run(*vader, silent, false, false) == ModuleRun::runs);
      // An exit 3 marks that module, even on a host that lists everything...
      CHECK(module_run(*vader, new_host, false, true) == ModuleRun::coming_soon);
      CHECK(module_run(*vader, silent, true, true) == ModuleRun::coming_soon);
      // ...and nothing else: the other Intermission and Classic modules run.
      for (const Module& m : six.modules) {
        if (&m != vader) CHECK(module_run(m, new_host, false, false) == ModuleRun::runs);
      }
    }
  }
  if (!g_fakehost.empty()) {
    SetEnvironmentVariableW(L"FAKEHOST_LOG", nullptr);
    SetEnvironmentVariableW(L"FAKEHOST_LANES", L"pe32");
    HostCapabilities c = probe_capabilities(g_fakehost);
    CHECK(c.known && c.has_lane("pe32") && !c.has_lane("ne16") && c.can_configure("pe32"));
    SetEnvironmentVariableW(L"FAKEHOST_LANES", nullptr);
    SetEnvironmentVariableW(L"FAKEHOST_CONFIGURE", L"ne16");
    c = probe_capabilities(g_fakehost);
    CHECK(c.known && c.has_lane("pe32") && c.has_lane("ne16") && !c.can_configure("pe32") && c.can_configure("ne16"));
    SetEnvironmentVariableW(L"FAKEHOST_CONFIGURE", nullptr);
    // fakehost's abis=: today's host's by default, or left out (an older host).
    CHECK(c.runs("ne16", "intermission") && c.line.find(" abis=afterdark,intermission ") != std::string::npos);
    SetEnvironmentVariableW(L"FAKEHOST_ABIS", L"none");
    c = probe_capabilities(g_fakehost);
    CHECK(c.known && c.line.find("abis=") == std::string::npos && !c.runs("ne16", "intermission") && c.runs("ne16", ""));
    SetEnvironmentVariableW(L"FAKEHOST_ABIS", L"afterdark");
    c = probe_capabilities(g_fakehost);
    CHECK(c.known && (c.abis == std::vector<std::string>{"afterdark"}) && !c.runs("ne16", "intermission"));
    SetEnvironmentVariableW(L"FAKEHOST_ABIS", nullptr);
    // fakehost's numlock=1: today's host's by default; FAKEHOST_NUMLOCK=0 a host from before it.
    c = probe_capabilities(g_fakehost);
    CHECK(c.known && c.numlock && c.takes_numlock_lines() && c.line.find(" numlock=1") != std::string::npos);
    SetEnvironmentVariableW(L"FAKEHOST_NUMLOCK", L"0");
    c = probe_capabilities(g_fakehost);
    CHECK(c.known && !c.numlock && !c.takes_numlock_env() && c.line.find("numlock") == std::string::npos);
    SetEnvironmentVariableW(L"FAKEHOST_NUMLOCK", nullptr);
  }

  // A group header in the module list (layout_group_header): the count and
  // the "Coming soon" pill always show whole; the title gives way, ellipsized.
  {
    auto eight = [](const std::wstring& s) { return (int)s.size() * 8; };   // 8 px a character
    CHECK(ellipsize(L"Short", 100, eight) == L"Short");
    // The longest start that fits with its ellipsis ("Star Wars" and the ellipsis: 80 px).
    CHECK(ellipsize(L"Star Wars Screen Entertainment", 80, eight) == std::wstring(L"Star Wars") + L'\u2026');
    CHECK(ellipsize(L"Star Wars Screen Entertainment", 95, eight) == std::wstring(L"Star Wars") + L'\u2026');
    CHECK(ellipsize(L"Star Wars Screen Entertainment", 104, eight) == std::wstring(L"Star Wars Sc") + L'\u2026');
    CHECK(ellipsize(L"Star Wars", 48, eight) == L"Star\u2026");     // the space before the ellipsis goes
    CHECK(ellipsize(L"Star Wars", 7, eight).empty());               // not even the ellipsis fits
    CHECK(ellipsize(L"", 0, eight).empty());
    GroupHeaderInput in;
    in.title = L"Star Wars Screen Entertainment";   // 240 px
    in.left = 48;
    in.right = 330;
    in.count_w = 16;
    in.gap = 8;
    GroupHeaderLayout L = layout_group_header(in, eight);
    CHECK(!L.ellipsized && L.title == in.title && L.title_w == 240 && L.count_x == 48 + 240 + 8);
    in.right = 300;   // 48 + 240 + 8 + 16 = 312: no longer fits
    L = layout_group_header(in, eight);
    CHECK(L.ellipsized && L.count_x + in.count_w <= in.right && L.count_x == in.left + L.title_w + in.gap);
    CHECK(L.title.back() == L'\u2026' && L.title_w <= 300 - 48 - 8 - 16);
    in.pill_w = 96;   // "Coming soon", at the right
    in.pill_right = 320;
    L = layout_group_header(in, eight);
    CHECK(L.pill_x == 320 - 96 && L.count_x + in.count_w + in.gap <= L.pill_x && L.ellipsized);
    in.title = L"Deluxe";
    L = layout_group_header(in, eight);
    CHECK(!L.ellipsized && L.title == L"Deluxe" && L.count_x == 48 + 48 + 8);
  }

  // A module button's outcome note (§6.3).
  CHECK(configure_outcome_note(0).empty());
  CHECK(configure_outcome_note(4) == L"Nothing to set here");
  CHECK(configure_outcome_note(5) == L"Couldn\u2019t open this option (code 5)");
  CHECK(configure_outcome_note(1) == L"Couldn\u2019t open this option (code 1)");
  CHECK(configure_outcome_note(0xC0000005) == L"Couldn\u2019t open this option (code 0xC0000005)");
  CHECK(configure_args(L"C:\\a b\\X.AD", 3, 0x1234) == L"--configure \"C:\\a b\\X.AD\" --button 3 --owner 4660");

  // Live module buttons: a push button with a note line under it.
  {
    Control b;
    b.type = ControlType::button;
    b.name = "Select Fish...";
    Control s;
    s.type = ControlType::slider;
    s.name = "Speed";
    std::vector<Control> cs = {b, s};
    std::vector<bool> live = {true, false};
    PanelLayout ro = layout_panel(cs, 300, 96, 16);
    PanelLayout lv = layout_panel(cs, 300, 96, 16, &live);
    CHECK(!ro.rows[0].label.empty() && lv.rows[0].label.empty());   // the button carries the name
    CHECK(!lv.rows[0].input.empty() && !lv.rows[0].value.empty());
    CHECK(lv.rows[0].value.y >= lv.rows[0].input.bottom());          // the note under the button
    CHECK(lv.rows[0].bottom >= lv.rows[0].value.bottom());
    CHECK(lv.rows[1].top > lv.rows[0].bottom);
    std::vector<Control> only = {b};
    std::vector<bool> one = {true};
    CHECK(layout_panel(only, 300, 96).empty_note.w > 0);             // read-only: "no settings"
    CHECK(layout_panel(only, 300, 96, 0, &one).empty_note.empty());  // a live button is something to set
  }
}

// ---- releases: the strip, Collections, Random under it, the list by release --------
// (COVERS.md §1)

int index_of(const Catalog& c, const std::string& id) {
  for (size_t i = 0; i < c.modules.size(); ++i) {
    if (c.modules[i].id == id) return (int)i;
  }
  return -1;
}

std::vector<std::string> labels_of(const ListGroup& g) {
  std::vector<std::string> v;
  for (const ListRow& r : g.rows) v.push_back(r.label);
  return v;
}

void test_releases_catalog() {
  using Strs = std::vector<std::string>;
  Catalog c;
  std::string err;
  CHECK(parse_catalog(fixture("catalog-releases.json"), c, &err));
  CHECK(c.has_packages);
  CHECK_EQ(c.modules.size(), (size_t)18);
  CHECK_EQ(c.releases.size(), (size_t)5);
  if (c.releases.size() != 5) return;
  Strs ids, shorts;
  for (const Release& r : c.releases) {
    ids.push_back(r.id);
    shorts.push_back(r.short_title);
  }
  CHECK((ids == Strs{"deluxe", "ad10", "ad32", "tt", "simpsons"}));
  CHECK((shorts == Strs{"Deluxe", "10th Anniversary", "After Dark 3.2", "Totally Twisted", "Simpsons"}));
  CHECK(c.releases[4].title == "The Simpsons Screen Saver" && c.releases[4].modules == 3);
  // Every origin of packages[].cover (COVERS.md §2.7).
  const Cover& u = c.releases[0].cover;
  CHECK(u.origin == "user" && !u.generated() && u.tile == "covers/deluxe/tile.png" &&
        u.tile_md5 == "00000000000000000000000000000001" && u.image == "covers/deluxe/user.png" && u.width == 962 &&
        u.height == 1230 && u.original == "download" && u.art == "box");
  CHECK(c.releases[1].cover.origin == "download" && c.releases[1].cover.art == "disc" &&
        c.releases[1].cover.credit == "Internet Archive");
  CHECK(c.releases[2].cover.origin == "disc" && c.releases[2].cover.label == "Installer art");
  CHECK(c.releases[3].cover.generated() && c.releases[3].cover.tile.empty() && c.releases[3].cover.origin == "generated");
  CHECK(c.releases[4].cover.label == "Box front" && c.releases[4].cover.credit == "Wikisimpsons" &&
        c.releases[4].cover.width == 600 && c.releases[4].cover.height == 776);
  CHECK(c.release_index("tt") == 3 && c.release_index("nope") == -1);
  CHECK(c.modules_in(0) == 5 && c.modules_in(1) == 4 && c.modules_in(2) == 4 && c.modules_in(3) == 2 && c.modules_in(4) == 3);
  // The module fields.
  const Module* a10 = c.find("ad10.alpha");
  CHECK(a10 && a10->package == "ad10" && a10->release == 1 && a10->same_as == "ad40.alpha" &&
        a10->module_name == "Alpha Toasters" && a10->name == "Alpha Toasters" &&
        a10->display_name == "Alpha Toasters (10th Anniversary)" && a10->package_title == "After Dark 10th Anniversary");
  const Module* slide = c.find("classic.slide");
  CHECK(slide && slide->name == "Slide Show" && slide->release == 0 && slide->same_as.empty());   // shown whole
  // A cover with no tile, or an origin this version doesn't know, is generated.
  Catalog g;
  CHECK(parse_catalog(R"({"packages":[{"id":"a","title":"A","cover":{"origin":"download"}},
      {"id":"b","title":"B","shortTitle":"","cover":{"origin":"painted","tile":"x.png"}},
      {"id":"a","title":"again"},{"title":"no id"},{"id":"c"}],
      "modules":[{"id":"m1","path":"p1","package":"a"},{"id":"m2","path":"p2","package":"z","packageTitle":"Zed"},
                 {"id":"m3","path":"p3","package":"b","sameAs":"m3"}]})",
                      g, &err));
  CHECK_EQ(g.releases.size(), (size_t)4);   // a, b, c, then z made up for m2
  if (g.releases.size() == 4) {
    CHECK(g.releases[0].cover.generated() && g.releases[1].cover.generated());
    CHECK(g.releases[0].title == "A" && g.releases[1].short_title == "B" && g.releases[2].title == "c");
    CHECK(g.releases[3].id == "z" && g.releases[3].title == "Zed" && g.releases[3].short_title == "Zed");
    CHECK(g.modules[1].release == 3 && g.modules[0].release == 0);
    CHECK(g.modules[2].same_as.empty());   // itself: no copy
  }
  // A catalog from before packages (COVERS.md §1.10): one release per
  // module package, in order of first appearance, titled after its modules.
  Catalog old;
  CHECK(parse_catalog(fixture("catalog-win.json"), old, &err));
  CHECK(!old.has_packages && old.releases.size() == 1 && old.releases[0].id == "other" &&
        old.releases[0].title == "Other modules" && old.releases[0].short_title == "Other modules" &&
        old.releases[0].cover.generated());
  for (const Module& m : old.modules) CHECK(m.release == 0 && m.package == "other" && m.name == m.display_name);
  Catalog mixed;
  CHECK(parse_catalog(R"({"modules":[{"id":"ad40.x","path":"a"},{"id":"q.y","path":"b","package":"ad32",
      "packageTitle":"After Dark 3.2"},{"id":"classic.z","path":"c"},{"id":"w.w","path":"d"}]})",
                      mixed, &err));
  CHECK_EQ(mixed.releases.size(), (size_t)3);
  if (mixed.releases.size() == 3) {
    CHECK(mixed.releases[0].id == "deluxe" && mixed.releases[0].title == "After Dark 4.0 Deluxe");
    CHECK(mixed.releases[1].id == "ad32" && mixed.releases[1].title == "After Dark 3.2");
    CHECK(mixed.releases[2].id == "other" && mixed.releases[2].title == "Other modules");
    CHECK(mixed.modules[2].release == 0);
  }
  // The strip shows with two releases or more.
  CHECK(strip_shown(c) && strip_shown(mixed) && !strip_shown(old));
}

void test_releases_settings() {
  using Ids = std::vector<std::string>;
  // [Saver] Collections: parsed like the other lists, round-tripped.
  Settings s = parse_settings("[Saver]\nCollections= simpsons , tt,simpsons\n");
  CHECK((s.collections == Ids{"simpsons", "tt"}));
  CHECK(parse_settings(serialize_settings(s)) == s);
  CHECK(parse_settings("").collections.empty());
  // A list equal to the file's is left as written; an absent key stays absent.
  const std::string written = "[Saver]\r\nModule=random\r\nCollections=tt ,simpsons, bogus\r\n";
  Settings w = parse_settings(written);
  CHECK((w.collections == Ids{"tt", "simpsons", "bogus"}));   // unknown ids stay until the next OK
  CHECK(serialize_settings(w, written).find("Collections=tt ,simpsons, bogus\r\n") != std::string::npos);
  CHECK(serialize_settings(Settings{}, "[Saver]\r\nModule=random\r\n").find("Collections") == std::string::npos);
  Settings changed = w;
  changed.collections = {"ad32"};
  CHECK(serialize_settings(changed, written).find("Collections=ad32\r\n") != std::string::npos);
  changed.collections.clear();
  CHECK(serialize_settings(changed, written).find("Collections=\r\n") != std::string::npos);
  // Normalizing the strip's selection: no repeats; every release = all.
  CHECK((normalize_collections({"tt", "tt", "simpsons"}, 5) == Ids{"tt", "simpsons"}));
  CHECK(normalize_collections({"deluxe", "ad10", "ad32", "tt", "simpsons"}, 5).empty());
  CHECK(normalize_collections({}, 5).empty());
  // The dialog's choice: written while the strip shows, left alone otherwise.
  DialogChoice ch{true, {"a"}, 4, ""};
  CHECK((apply_dialog_choice(w, ch).collections == Ids{"tt", "simpsons", "bogus"}));
  CHECK(serialize_settings(apply_dialog_choice(w, ch), written).find("Collections=tt ,simpsons, bogus\r\n") != std::string::npos);
  ch.strip = true;
  ch.releases = 5;
  ch.collections = {"simpsons", "tt"};
  CHECK((apply_dialog_choice(w, ch).collections == Ids{"simpsons", "tt"}));
  ch.collections = {"deluxe", "ad10", "ad32", "tt", "simpsons"};
  CHECK(apply_dialog_choice(w, ch).collections.empty());
  CHECK(serialize_settings(apply_dialog_choice(w, ch), written).find("Collections=\r\n") != std::string::npos);
  ch.collections = {};
  CHECK(apply_dialog_choice(w, ch).collections.empty());
  // No catalog: nothing changes at all.
  ch.total = 0;
  CHECK(apply_dialog_choice(w, ch) == w);
  // Random needs a checked module among the rows shown.
  DialogChoice r{true, {"a"}, 4, ""};
  r.shown_checked = 0;
  CHECK(!random_allowed(r));
  r.shown_checked = 1;
  CHECK(random_allowed(r));
  r.shown_checked = 0;
  r.random = false;
  CHECK(random_allowed(r));
  r.random = true;
  r.total = 0;
  CHECK(random_allowed(r));
  CHECK(std::wstring(kRandomNeedsChecks) == L"Check at least one module in the releases shown");
  // Randomize is written from the global checks, whatever the filter.
  Settings plain = parse_settings("[Saver]\nModule=random\n");
  DialogChoice g{true, {"ad40.alpha", "tt.beta"}, 18, "tt.beta"};
  g.strip = true;
  g.releases = 5;
  g.collections = {"simpsons"};
  g.shown_checked = 0;
  CHECK(!random_allowed(g));
  Settings saved = apply_dialog_choice(plain, g);
  CHECK((saved.randomize == Ids{"ad40.alpha", "tt.beta"}) && (saved.collections == Ids{"simpsons"}));
}

void test_releases_rotation() {
  using Ids = std::vector<std::string>;
  Catalog c;
  CHECK(parse_catalog(fixture("catalog-releases.json"), c, nullptr));
  // Everything: one id per set of identical bytes, the first in catalog order.
  Settings s;
  RotationPlan p = effective_rotation(s, c);
  CHECK((p.ids == Ids{"ad40.alpha", "ad40.twin", "classic.beta", "classic.slide", "classic.twin3", "ad10.gamma",
                      "ad32.delta", "ad32.echo", "ad32.echo2", "tt.foxtrot", "simpsons.hotel", "simpsons.india",
                      "simpsons.juliet"}));
  CHECK(p.lead.empty() && !p.collections_ignored);
  // Collections: only those releases.
  s.collections = {"tt", "ad10"};
  p = effective_rotation(s, c);
  CHECK((p.ids == Ids{"ad10.alpha", "ad10.twin", "ad10.twin3", "ad10.gamma", "tt.beta", "tt.foxtrot"}));
  // A copy checked in two selected releases plays once, as the first in catalog order.
  s.collections = {"ad32", "deluxe"};
  p = effective_rotation(s, c);
  CHECK(std::count(p.ids.begin(), p.ids.end(), "classic.beta") == 1 &&
        std::count(p.ids.begin(), p.ids.end(), "ad32.beta") == 0);
  CHECK_EQ(p.ids.size(), (size_t)8);
  // Unknown ids and "every release" are no filter at all.
  s.collections = {"bogus"};
  CHECK_EQ(effective_rotation(s, c).ids.size(), (size_t)13);
  s.collections = {"deluxe", "ad10", "ad32", "tt", "simpsons"};
  CHECK_EQ(effective_rotation(s, c).ids.size(), (size_t)13);
  // Randomize under the filter.
  s.randomize = {"simpsons.hotel", "ad40.alpha"};
  s.collections = {"simpsons"};
  p = effective_rotation(s, c);
  CHECK((p.ids == Ids{"simpsons.hotel"}) && !p.collections_ignored);
  // Nothing checked in the selected releases (a hand-edited file): Randomize alone, said so.
  s.randomize = {"ad40.alpha", "ad10.alpha", "classic.beta"};
  p = effective_rotation(s, c);
  CHECK((p.ids == Ids{"ad40.alpha", "classic.beta"}) && p.collections_ignored);
  // A Randomize naming nothing the catalog has: every module (then the filter).
  s.randomize = {"gone.one"};
  s.collections = {"tt"};
  p = effective_rotation(s, c);
  CHECK((p.ids == Ids{"tt.beta", "tt.foxtrot"}));
  // The lead plays first whatever the filter...
  s.module = "simpsons.india";
  s.randomize = {"ad40.alpha", "simpsons.hotel"};
  s.collections = {"deluxe"};
  p = effective_rotation(s, c);
  CHECK((p.ids == Ids{"ad40.alpha"}) && p.lead == "simpsons.india");
  // ...and stands in for its own copy in the bag, so it plays once a pass.
  s.module = "ad10.alpha";
  s.randomize = {"ad40.alpha", "ad40.twin"};
  s.collections.clear();
  p = effective_rotation(s, c);
  CHECK((p.ids == Ids{"ad10.alpha", "ad40.twin"}) && p.lead == "ad10.alpha");
  Rotation rot(p.ids, 3, p.lead);
  CHECK(rot.current() == "ad10.alpha" && rot.size() == 2);
  // A single module (no list): nothing to rotate, no lead.
  s.randomize.clear();
  CHECK(effective_rotation(s, c).lead.empty());
  // A missing file: a present copy stands in for it.
  Settings all;
  auto have = [](const std::string& id) { return id != "ad40.alpha" && id != "classic.beta"; };
  p = effective_rotation(all, c, have);
  CHECK(std::count(p.ids.begin(), p.ids.end(), "ad10.alpha") == 1 && std::count(p.ids.begin(), p.ids.end(), "ad40.alpha") == 0);
  CHECK(std::count(p.ids.begin(), p.ids.end(), "ad32.beta") == 1);
  CHECK_EQ(p.ids.size(), (size_t)13);
}

void test_releases_list() {
  using Strs = std::vector<std::string>;
  Catalog c;
  CHECK(parse_catalog(fixture("catalog-releases.json"), c, nullptr));
  // Grouped by release in packages[] order; rows by name; the lane only
  // where two builds share a name.
  ListModel all = build_list(c, {});
  CHECK(all.shown == 18 && all.total == 18 && all.groups.size() == 5);
  if (all.groups.size() == 5) {
    for (int i = 0; i < 5; ++i) CHECK_EQ(all.groups[i].release, i);
    CHECK((labels_of(all.groups[0]) == Strs{"Alpha Toasters", "Beta Fish", "Slide Show", "Twin Dog!", "Twin Dog! (Classic)"}));
    CHECK((labels_of(all.groups[1]) == Strs{"Alpha Toasters", "Gamma Rays", "Twin Dog!", "Twin Dog! (Classic)"}));
    // Two Classic builds of one name: their file stems.
    CHECK((labels_of(all.groups[2]) == Strs{"Beta Fish", "Delta Globe", "Echo Echo (ECHO)", "Echo Echo (ECHO2)"}));
    CHECK((labels_of(all.groups[3]) == Strs{"Beta Fish", "Foxtrot Twist"}));
    CHECK((labels_of(all.groups[4]) == Strs{"Hotel Donut", "India Couch", "Juliet Stage"}));
    // The AD4 "Twin Dog!" is the plain one.
    CHECK(all.groups[0].rows[3].module == index_of(c, "ad40.twin") && all.groups[0].rows[4].module == index_of(c, "classic.twin3"));
  }
  CHECK_EQ(all.order().size(), (size_t)18);
  // The filter: only those releases' groups, the counts follow.
  ListModel f = build_list(c, {"tt", "simpsons"});
  CHECK(f.groups.size() == 2 && f.shown == 5 && f.total == 18);
  CHECK(f.shows(index_of(c, "tt.beta")) && !f.shows(index_of(c, "ad40.alpha")));
  CHECK(f.group(3) != nullptr && f.group(0) == nullptr);
  CHECK((f.order() == std::vector<int>{index_of(c, "tt.beta"), index_of(c, "tt.foxtrot"), index_of(c, "simpsons.hotel"),
                                       index_of(c, "simpsons.india"), index_of(c, "simpsons.juliet")}));
  CHECK(modules_count_label(f.shown, f.total) == L"5 of 18");
  // The details after a filter change never show a hidden module: the chosen
  // one while listed, else the one shown before while listed, else the first
  // row, else none (and showing a module never chooses it).
  {
    const int alpha = index_of(c, "ad40.alpha"), tt_beta = index_of(c, "tt.beta"), foxtrot = index_of(c, "tt.foxtrot"),
              hotel = index_of(c, "simpsons.hotel");
    CHECK_EQ(details_after_filter(all, alpha, foxtrot), alpha);
    CHECK_EQ(details_after_filter(f, alpha, alpha), tt_beta);    // Deluxe's module, with Totally Twisted + Simpsons shown
    CHECK_EQ(details_after_filter(f, alpha, hotel), hotel);      // what shows stays while it is listed
    CHECK_EQ(details_after_filter(f, foxtrot, hotel), foxtrot);  // the chosen row, when listed, wins
    CHECK_EQ(details_after_filter(f, -1, -1), tt_beta);
    const ListModel simpsons = build_list(c, {"simpsons"});
    CHECK_EQ(details_after_filter(simpsons, alpha, tt_beta), hotel);
    const ListModel none = build_list(c, {"nope"});
    CHECK(none.groups.empty() && none.shown == 0);
    CHECK_EQ(details_after_filter(none, alpha, alpha), -1);
    CHECK_EQ(details_after_filter(ListModel{}, -1, 3), -1);
  }
  CHECK(modules_count_label(all.shown, all.total) == L"18");
  CHECK(modules_count_label(15, 202) == L"15 of 202" && modules_count_label(0, 0).empty());
  // Names clash case-insensitively; two AD4 builds of one name get their stems.
  Catalog k;
  CHECK(parse_catalog(R"({"packages":[{"id":"p","title":"P"},{"id":"q","title":"Q"}],"modules":[
      {"id":"p.a","path":"X/ONE.AD","lane":"pe32","package":"p","moduleName":"Beta fish"},
      {"id":"p.b","path":"X/two.ad","lane":"ne16","package":"p","moduleName":"BETA FISH"},
      {"id":"q.a","path":"Y/THREE.AD","lane":"pe32","package":"q","moduleName":"Same"},
      {"id":"q.b","path":"Y/FOUR.AD","lane":"pe32","package":"q","moduleName":"Same"},
      {"id":"q.c","path":"Y/FIVE.AD","lane":"pe32","package":"q","moduleName":"Beta fish"}]})",
                      k, nullptr));
  ListModel kl = build_list(k, {});
  CHECK(kl.groups.size() == 2);
  if (kl.groups.size() == 2) {
    CHECK((labels_of(kl.groups[0]) == Strs{"Beta fish", "BETA FISH (Classic)"}));
    CHECK((labels_of(kl.groups[1]) == Strs{"Beta fish", "Same (FOUR)", "Same (THREE)"}));   // one name per group only
  }
  // "Also on": the other releases with the same bytes, sameAs either way.
  CHECK((also_on(c, index_of(c, "ad40.alpha")) == Strs{"After Dark 10th Anniversary"}));
  CHECK((also_on(c, index_of(c, "classic.beta")) == Strs{"After Dark 3.2", "Totally Twisted After Dark"}));
  CHECK((also_on(c, index_of(c, "tt.beta")) == Strs{"After Dark 4.0 Deluxe", "After Dark 3.2"}));
  CHECK(also_on(c, index_of(c, "ad10.gamma")).empty() && also_on(c, -1).empty());
  CHECK(also_on_tip({"A", "B"}) == L"Also on: A, B" && also_on_tip({}).empty());
  // The strip's words.
  CHECK(strip_status(0, 5) == L"Click covers to filter the list");
  CHECK(strip_status(1, 5) == L"Showing 1 of 5 releases");
  CHECK(strip_status(4, 5) == L"Showing 4 of 5 releases");
  CHECK(strip_status(5, 5) == L"Showing all 5 releases");
  CHECK(!filter_active(0, 5) && filter_active(1, 5) && filter_active(4, 5) && !filter_active(5, 5));
  CHECK(tile_name(c.releases[4], 15) == L"The Simpsons Screen Saver, 15 screen savers");
  Release amp;
  amp.title = "Tom & Jerry";
  CHECK(tile_name(amp, 1) == L"Tom && Jerry, 1 screen saver");
  CHECK(tile_tip(c.releases[4], 3) == L"The Simpsons Screen Saver — 3 screen savers\r\nClick to show only this "
                                      L"release’s screen savers, or several releases at once. Right-click to change "
                                      L"its cover.");
  CHECK(effective_collections({}, c).empty());
  CHECK((effective_collections({"simpsons", "nope", "tt"}, c) == Strs{"tt", "simpsons"}));
  CHECK(effective_collections({"nope"}, c).empty());
  CHECK(effective_collections({"deluxe", "ad10", "ad32", "tt", "simpsons"}, c).empty());
  // The assets line: modules from releases, or from the one release.
  AssetCounts a = count_assets(c, std::vector<bool>(18, true));
  CHECK(a.total == 18 && a.releases == 5 && a.missing == 0);
  CHECK(assets_summary(a) == L"18 modules from 5 releases");
  std::vector<bool> gone(18, true);
  gone[2] = gone[7] = false;
  CHECK(assets_summary(count_assets(c, gone)) == L"18 modules from 5 releases · 2 missing — import again to restore");
  CHECK(assets_summary({202, 5, 0, ""}) == L"202 modules from 5 releases");
  CHECK(assets_summary({84, 1, 0, "After Dark 4.0 Deluxe"}) == L"84 modules from After Dark 4.0 Deluxe");
  CHECK(assets_summary({1, 1, 0, ""}) == L"1 module imported");
  Catalog deluxe;
  CHECK(parse_catalog(R"({"modules":[{"id":"ad40.x","path":"a"},{"id":"classic.y","path":"b"}]})", deluxe, nullptr));
  CHECK(assets_summary(count_assets(deluxe, {true, true})) == L"2 modules from After Dark 4.0 Deluxe");
}

// A focusable control's focus margin, px: adw_ui's focus_margin (widgets.cc,
// which this program doesn't link: it needs comctl32 6, and scr_unit has no
// manifest asking for it).
int focus_margin_px(int dpi) { return std::max(3, MulDiv(3, dpi, 96)); }

// Everything about the strip that must hold at a scale and a count of releases.
void check_strip(const StripInput& in, const char* what) {
  const StripLayout S = layout_strip(in);
  const StripMetrics m = strip_metrics(in.compact);
  const int n = in.tiles, d = in.dpi;
  auto fail = [&](const char* why) {
    fprintf(stderr, "strip %s n=%d %s @%d first=%d: %s\n", what, n, in.compact ? "compact" : "regular", d, in.first, why);
    ++g_failures;
  };
  if ((int)S.cells.size() != n || (int)S.arts.size() != n || (int)S.whole.size() != n) return fail("tile count");
  if (S.mode != (in.compact ? StripMode::compact : StripMode::regular)) fail("mode");
  const bool overflow = n * m.pitch - (m.pitch - m.cell_w) > in.w;
  if (S.overflow != overflow || (S.max_first > 0) != overflow) fail("overflow");
  if (S.first != std::clamp(in.first, 0, S.max_first)) fail("scroll position not clamped to its stops");
  if (!S.area.contains(S.view)) fail("view outside the area");
  // Whole tiles: consecutive, starting with the first shown (never cut at the left).
  int whole = 0, first_whole = -1;
  for (int i = 0; i < n; ++i) {
    if (!S.whole[i]) continue;
    if (first_whole < 0) first_whole = i;
    else if (!S.whole[i - 1]) fail("whole tiles not consecutive");
    ++whole;
    if (!S.view.contains(S.cells[i])) fail("whole tile outside the view");
  }
  if (whole < 1 || first_whole != S.first) fail("the first tile shown is not whole");
  // As many at every stop: all of them, or overflowing, the slots between
  // the chevrons' zones (at least one, and fewer than all).
  if (S.slots != (overflow ? whole : n) || (overflow && (S.slots < 1 || S.slots >= n)) || S.max_first != n - S.slots)
    fail("tiles shown at a stop");
  // The chevrons (24 DIP) at the ends with more beyond them.
  const bool more_left = S.first > 0, more_right = !S.whole[n - 1];
  if (S.chevron_left.empty() == more_left) fail("left chevron");
  if (S.chevron_right.empty() == more_right) fail("right chevron");
  for (const Rc* c : {&S.chevron_left, &S.chevron_right}) {
    if (!c->empty() && (std::abs(c->w - dip(kStripChevronW, d)) > 1 || !S.area.contains(*c))) fail("chevron size");
  }
  // Beside the tiles: the left one at the area's edge, the first tile shown
  // just past its zone; the right one just past the last slot.
  const int chevron_gap = dip(kStripChevronGap, d);
  if (!S.chevron_left.empty() &&
      (S.chevron_left.x != S.area.x || std::abs(S.cells[S.first].x - S.chevron_left.right() - chevron_gap) > 1))
    fail("left chevron place");
  if (!S.chevron_right.empty() && std::abs(S.chevron_right.x - S.view.right() - chevron_gap) > 1) fail("right chevron place");
  // The first tile shown: overflowing, just past the left chevron's zone at
  // every stop (unscrolled too, the zone empty then); else at the area's edge.
  const int lead = overflow ? dip(kStripChevronW + kStripChevronGap, d) : 0;
  if (std::abs(S.cells[S.first].x - S.area.x - lead) > 1) fail("the first tile shown out of its place");
  if (S.first == S.max_first && more_right) fail("the last stop leaves tiles beyond");
  if (!overflow && (more_left || more_right)) fail("chevrons without overflow");
  // Nothing of a tile shown lies under a chevron, its focus ring included
  // (its window: the cell and a focus margin each side), nor outside the
  // strip's window (the area and a focus margin each side).
  const int fm = focus_margin_px(d);
  for (int i = 0; i < n; ++i) {
    if (!S.whole[i]) continue;
    const Rc win{S.cells[i].x - fm, S.cells[i].y - fm, S.cells[i].w + 2 * fm, S.cells[i].h + 2 * fm};
    for (const Rc* c : {&S.chevron_left, &S.chevron_right}) {
      if (!c->empty() && win.overlaps(*c)) fail("a tile shown under a chevron");
    }
    if (!Rc{S.area.x - fm, S.area.y - fm, S.area.w + 2 * fm, S.area.h + 2 * fm}.contains(win)) fail("a tile outside the strip");
  }
  // Tiles and their parts.
  for (int i = 0; i < n; ++i) {
    if (!S.cells[i].contains(S.arts[i])) fail("art outside its cell");
    if (std::abs(S.arts[i].w * 5 - S.arts[i].h * 4) > 5) fail("art not 4:5");
    if (in.compact != S.captions[i].empty()) fail("caption");
    if (!S.captions[i].empty() && (!S.cells[i].contains(S.captions[i]) || S.captions[i].y < S.arts[i].bottom())) fail("caption place");
  }
  // Scrolling to any tile shows it whole.
  for (int i = 0; i < n; ++i) {
    StripInput t = in;
    t.first = strip_first_showing(in, i);
    if (!layout_strip(t).whole[i]) fail("strip_first_showing");
  }
}

// Across a row's scroll stops: each chevron at one place whenever it shows,
// and that place clear of every tile shown at the stop where it hides (the
// left one's unscrolled, the right one's at the last stop: a pointer left on
// it after a click too many lands on nothing), and each step moving the
// tiles by one pitch (every slot keeps its place across the stops).
void check_strip_stops(StripInput in, const char* what) {
  const StripMetrics m = strip_metrics(in.compact);
  const int d = in.dpi, fm = focus_margin_px(d);
  auto fail = [&](const char* why) {
    fprintf(stderr, "strip %s n=%d %s @%d w=%g first=%d: %s\n", what, in.tiles, in.compact ? "compact" : "regular", d, in.w,
            in.first, why);
    ++g_failures;
  };
  in.first = 0;
  const int stops = layout_strip(in).max_first;
  Rc left, right;
  std::vector<StripLayout> at;
  for (int f = 0; f <= stops; ++f) {
    in.first = f;
    at.push_back(layout_strip(in));
    const StripLayout& S = at.back();
    for (auto [c, seen] : {std::pair{S.chevron_left, &left}, std::pair{S.chevron_right, &right}}) {
      if (c.empty()) continue;
      if (seen->empty()) *seen = c;
      else if (!(c == *seen)) fail("a chevron moved between stops");
    }
    if (f > 0) {
      const int step = dip(m.pitch, d);
      for (size_t i = 0; i < S.cells.size(); ++i) {
        if (std::abs(at[f - 1].cells[i].x - S.cells[i].x - step) > 1) fail("a step is not one pitch");
      }
    }
  }
  auto clear_of = [&](const StripLayout& S, const Rc& chevron, const char* why) {
    for (size_t i = 0; i < S.cells.size() && !chevron.empty(); ++i) {
      const Rc win{S.cells[i].x - fm, S.cells[i].y - fm, S.cells[i].w + 2 * fm, S.cells[i].h + 2 * fm};
      if (S.whole[i] && win.overlaps(chevron)) fail(why);
    }
  };
  clear_of(at.front(), left, "a tile where the left chevron was, unscrolled");
  clear_of(at.back(), right, "a tile where the right chevron was, at the last stop");
}

void test_releases_layout() {
  // Metrics (DIPs) and the art at each scale (COVERS.md §1.2).
  const StripMetrics r = strip_metrics(false), k = strip_metrics(true);
  CHECK(r.art_w == 64 && r.art_h == 80 && r.cell_w == 96 && r.cell_h == 108 && r.pitch == 104 && r.band == 120);
  CHECK(r.art_x == 16 && r.art_y == 4 && r.caption_h == 16);
  CHECK(k.art_w == 48 && k.art_h == 60 && k.cell_w == 64 && k.cell_h == 68 && k.pitch == 72 && k.band == 80);
  CHECK(k.art_x == 8 && k.art_y == 4 && k.caption_h == 0);
  const std::vector<std::pair<int, int>> regular_art = {{96, 64}, {120, 80}, {144, 96}, {168, 112}, {192, 128}, {240, 160}};
  for (auto [dpi, w] : regular_art) {
    StripLayout S = layout_strip(StripInput{5, false, 24, 48, 776, 0, dpi});
    CHECK(S.arts[0].w == w && S.arts[0].h == w * 5 / 4);
  }
  for (auto [dpi, w] : std::vector<std::pair<int, int>>{{96, 48}, {192, 96}, {240, 120}}) {
    StripLayout S = layout_strip(StripInput{5, true, 24, 48, 776, 0, dpi});
    CHECK(S.arts[0].w == w && S.arts[0].h == w * 5 / 4);
  }
  // Room: at the minimum content width (852 DIP) the tiles area holds 6
  // regular or 8 compact tiles without scrolling.
  const double min_area = kMinClientW - 48 - kStripStatusW - kStripStatusGap;
  CHECK(!layout_strip(StripInput{6, false, 24, 48, min_area, 0, 96}).overflow);
  CHECK(layout_strip(StripInput{7, false, 24, 48, min_area, 0, 96}).overflow);
  CHECK(!layout_strip(StripInput{8, true, 24, 48, min_area, 0, 96}).overflow);
  CHECK(layout_strip(StripInput{9, true, 24, 48, min_area, 0, 96}).overflow);
  // Every scale 100-250%, 1 to 12 releases, both forms, several widths, every scroll position.
  for (int dpi = 96; dpi <= 240; dpi += 24) {
    for (int n = 1; n <= 12; ++n) {
      for (bool compact : {false, true}) {
        for (double w : {min_area, 776.0, 1024.0, 300.0, 850.0, 855.5, 856.0}) {
          for (int first = 0; first <= n; ++first) check_strip(StripInput{n, compact, 24, 48, w, first, dpi}, "sweep");
          check_strip_stops(StripInput{n, compact, 24, 48, w, 0, dpi}, "sweep");
        }
      }
    }
  }
  // On the 4-DIP grid at 100%, and 200% is 100% doubled.
  for (bool compact : {false, true}) {
    for (int first : {0, 1, 3, 9}) {
      const StripInput in{12, compact, 24, 48, min_area, first, 96};
      StripLayout a = layout_strip(in), b = layout_strip(StripInput{12, compact, 24, 48, min_area, first, 192});
      CHECK(a.first == b.first && a.max_first == b.max_first);
      std::vector<std::pair<Rc, Rc>> pairs = {{a.area, b.area}, {a.view, b.view}, {a.chevron_left, b.chevron_left},
                                              {a.chevron_right, b.chevron_right}};
      for (size_t i = 0; i < a.cells.size(); ++i) {
        pairs.push_back({a.cells[i], b.cells[i]});
        pairs.push_back({a.arts[i], b.arts[i]});
        pairs.push_back({a.captions[i], b.captions[i]});
      }
      for (const auto& [x, y] : pairs) {
        CHECK(x.x % 4 == 0 && x.y % 4 == 0 && x.w % 4 == 0 && x.h % 4 == 0);
        CHECK(y.x == 2 * x.x && y.y == 2 * x.y && y.w == 2 * x.w && y.h == 2 * x.h);
      }
    }
  }
  // Whole-cell stops: the last one shows the last tile, each step moves one pitch.
  {
    StripInput in{12, false, 24, 48, min_area, 0, 96};
    const StripLayout s0 = layout_strip(in);
    CHECK(s0.overflow && s0.max_first >= 1 && s0.chevron_left.empty() && !s0.chevron_right.empty());
    in.first = 2;
    const StripLayout s2 = layout_strip(in);
    in.first = 3;
    const StripLayout s3 = layout_strip(in);
    CHECK(s2.cells[5].x - s3.cells[5].x == r.pitch && !s3.chevron_left.empty());
    in.first = 99;
    const StripLayout end = layout_strip(in);
    CHECK(end.first == end.max_first && end.whole.back() && end.chevron_right.empty());
  }
  CHECK(layout_strip(StripInput{0, false, 24, 48, 600, 0, 96}).mode == StripMode::hidden);

  // The window with the strip (COVERS.md §1.2).
  // (836: 800 and the options card's "Stretch to fit" row.)
  CHECK(kDesignClientHStrip == 836 && kMinClientHStrip == 680 && kStripCompactBelow == 760);
  for (int dpi = 96; dpi <= 240; dpi += 24) {
    for (auto [w, h] : {std::pair{kDesignClientW, kDesignClientHStrip}, std::pair{kMinClientW, kMinClientHStrip},
                        std::pair{1600, 1000}, std::pair{1040, 759}, std::pair{1040, 760}}) {
      for (bool random : {false, true}) {
        const int cw = dip(w, dpi), ch = dip(h, dpi);
        LayoutInput in{cw, ch, dpi, random};
        in.strip_tiles = 5;
        WindowLayout L = layout_window(in);
        check_layout(L, cw, ch, random);
        const bool compact = h < kStripCompactBelow;
        CHECK(L.strip_mode == (compact ? StripMode::compact : StripMode::regular));
        CHECK(L.tiles.mode == L.strip_mode && L.tiles.cells.size() == 5 && !L.tiles.overflow);
        CHECK(L.content.contains(L.strip) && L.content.contains(L.strip_status));
        CHECK(std::abs(L.strip.y - L.header.bottom()) <= 1 && std::abs(L.strip_status.right() - L.content.right()) <= 1);
        CHECK(L.strip.x == L.content.x);   // left-aligned on the column
        // The status box, 200 DIP, beyond the tiles area; centred on the art.
        CHECK(std::abs(L.strip_status.w - dip(kStripStatusW, dpi)) <= 1);
        CHECK(L.strip.right() + dip(kStripStatusGap, dpi) - 1 <= L.strip_status.x);
        CHECK(std::abs((L.strip_status.y + L.strip_status.h / 2) - (L.tiles.arts[0].y + L.tiles.arts[0].h / 2)) <= 1);
        for (const Rc& c : L.tiles.cells) CHECK(!c.overlaps(L.strip_status) && L.strip.contains(c));
        // The columns start under the band.
        const int band = dip(compact ? 80 : 120, dpi);
        CHECK(std::abs(L.mode.y - (L.header.bottom() + band)) <= 1 && std::abs(L.details_card.y - L.mode.y) <= 1);
        CHECK(L.strip.bottom() <= L.mode.y && L.strip.bottom() <= L.details_card.y);
      }
    }
  }
  // The columns keep at least today's heights: at the strip's minimum and
  // first-open sizes they are exactly those without it.
  for (int dpi : {96, 144, 192}) {
    for (auto [with_h, without_h] : {std::pair{kMinClientHStrip, kMinClientH}, std::pair{kDesignClientHStrip, kDesignClientH}}) {
      for (bool random : {false, true}) {
        LayoutInput a{dip(kMinClientW, dpi), dip(with_h, dpi), dpi, random};
        a.strip_tiles = 5;
        LayoutInput b{dip(kMinClientW, dpi), dip(without_h, dpi), dpi, random};
        const WindowLayout A = layout_window(a), B = layout_window(b);
        CHECK(std::abs(A.details_card.h - B.details_card.h) <= 1 && std::abs(A.list_card.h - B.list_card.h) <= 1);
        CHECK(std::abs(A.preview.h - B.preview.h) <= 1 && std::abs(A.panel.h - B.panel.h) <= 1);
      }
    }
  }
  // One release, or none: no strip, and the window is laid out as before.
  for (int tiles : {0, 1}) {
    LayoutInput in{dip(kDesignClientW, 96), dip(kDesignClientH, 96), 96, true};
    in.strip_tiles = tiles;
    WindowLayout L = layout_window(in), before = layout_window({dip(kDesignClientW, 96), dip(kDesignClientH, 96), 96, true});
    CHECK(L.strip_mode == StripMode::hidden && L.strip.empty() && L.strip_status.empty() && L.tiles.cells.empty());
    CHECK(L.mode == before.mode && L.details_card == before.details_card && L.list == before.list);
  }
  // 200% is 100% doubled, the strip included.
  {
    LayoutInput a{kDesignClientW, kDesignClientHStrip, 96, true}, b{2 * kDesignClientW, 2 * kDesignClientHStrip, 192, true};
    a.strip_tiles = b.strip_tiles = 5;
    const WindowLayout A = layout_window(a), B = layout_window(b);
    for (auto [x, y] : {std::pair{A.strip, B.strip}, std::pair{A.strip_status, B.strip_status}, std::pair{A.list, B.list},
                        std::pair{A.tiles.cells[4], B.tiles.cells[4]}}) {
      CHECK(y.x == 2 * x.x && y.y == 2 * x.y && y.w == 2 * x.w && y.h == 2 * x.h);
    }
    for (const Rc& x : {A.strip, A.strip_status, A.tiles.arts[2], A.mode, A.list_card, A.details_card}) {
      CHECK(x.x % 4 == 0 && x.y % 4 == 0 && x.w % 4 == 0 && x.h % 4 == 0);
    }
  }
  // The regular captions: the shrink rule, then every short title the
  // importer's registry has (PACKAGES.md §2) measured in the real caption face
  // at 100-250%: each fits whole at 12 DIP ("10th Anniversary" was cut at 125%).
  {
    auto eight_per_dip = [](int size) { return size * 8; };
    CHECK(strip_caption_size(96, eight_per_dip) == 12);
    CHECK(strip_caption_size(95, eight_per_dip) == 11);
    CHECK(strip_caption_size(80, eight_per_dip) == 10);
    CHECK(strip_caption_size(10, eight_per_dip) == 10);   // nothing fits: the smallest, ellipsized
    CHECK(strip_caption_room(96, 96) == 102 && strip_caption_room(120, 120) == 128);
    const char* titles[] = {"Deluxe",    "10th Anniversary", "After Dark 3.2", "Totally Twisted",
                            "Simpsons",  "Star Wars",        "Star Trek",      "Marvel",
                            "Snoopy",    "Looney Tunes",     "ScreamSavers",   "Disney"};
    HDC dc = CreateCompatibleDC(nullptr);
    for (int dpi : {96, 120, 144, 168, 192, 216, 240}) {
      adw::ui::Theme t;
      t.set_dpi(dpi);
      const StripLayout S = layout_strip(StripInput{5, false, 24, 48, 776, 0, dpi});
      const int room = strip_caption_room(S.cells[0].w, dpi);
      for (const char* title : titles) {
        const int w = (int)adw::ui::measure_text(dc, widen(title), t.fonts.caption).cx;
        if (w > room) fprintf(stderr, "caption \"%s\" @%d: %d px in %d\n", title, dpi, w, room);
        CHECK(w <= room);
        CHECK(strip_caption_size(room, [&](int) { return w; }) == 12);
      }
    }
    DeleteDC(dc);
  }
}

// Six releases (catalog-six.json): the five After Dark ones and Star Wars
// Screen Entertainment, whose 14 Intermission modules (abi intermission) a
// host too old for them can't run. The list, what Random plays with and
// without them, when the saver has to ask the host first and what it then
// leaves out, and the list's group header for the longest title, measured in
// the real faces.
void test_releases_six() {
  using Strs = std::vector<std::string>;
  Catalog c;
  std::string err;
  CHECK(parse_catalog(fixture("catalog-six.json"), c, &err));
  CHECK_EQ(c.releases.size(), (size_t)6);
  if (c.releases.size() != 6) return;
  Strs ids, shorts;
  for (const Release& r : c.releases) {
    ids.push_back(r.id);
    shorts.push_back(r.short_title);
  }
  // Oldest first, as adimport orders them: the Star Wars release (1994-08)
  // after the Simpsons (1994-08) by registry order on the tie.
  CHECK((ids == Strs{"simpsons", "swse", "ad32", "tt", "deluxe", "ad10"}));
  CHECK((shorts == Strs{"Simpsons", "Star Wars", "After Dark 3.2", "Totally Twisted", "Deluxe", "10th Anniversary"}));
  CHECK(c.releases[1].title == "Star Wars Screen Entertainment" && c.releases[1].modules == 14 && c.releases[1].cover.generated());
  CHECK(c.modules_in(1) == 14 && strip_shown(c));
  ListModel all = build_list(c, {});
  CHECK(all.shown == 32 && all.groups.size() == 6);
  if (all.groups.size() == 6) {
    CHECK_EQ(all.groups[1].release, 1);
    CHECK((labels_of(all.groups[1]) ==
           Strs{"Blueprints", "Cantina", "Character Biographies", "Darth Vader", "Death Star Trench", "Hyperspace",
                "Imperial Clock", "Jawas", "Lightsaber Duel", "Poster Art", "Rebel Clock", "Scrolling Text", "Space Battles",
                "Storyboards"}));
  }
  CHECK(assets_summary(count_assets(c, std::vector<bool>(32, true))) == L"32 modules from 6 releases");
  CHECK(tile_name(c.releases[1], 14) == L"Star Wars Screen Entertainment, 14 screen savers");
  CHECK(strip_status(1, 6) == L"Showing 1 of 6 releases");

  // Random: every module, the 14 Intermission ones included, one per sameAs set.
  Settings s;
  RotationPlan p = effective_rotation(s, c);
  CHECK_EQ(p.ids.size(), (size_t)27);   // 13 After Dark (as with five releases) + 14
  CHECK(std::count(p.ids.begin(), p.ids.end(), "swse.vader") == 1);
  // What Random may play on a host too old for them (saver.cc: App::may_rotate
  // over its --capabilities): none of them, and a named lead of theirs goes too.
  const HostCapabilities old_host = parse_capabilities("lanes=pe32,ne16 configure=pe32,ne16 status=1 state=1 seed=1");
  auto playable = [&](const std::string& id) {
    const Module* m = c.find(id);
    return m && old_host.runs(m->lane, m->abi);
  };
  p = effective_rotation(s, c, playable);
  CHECK_EQ(p.ids.size(), (size_t)13);
  CHECK(std::none_of(p.ids.begin(), p.ids.end(), [](const std::string& id) { return id.rfind("swse.", 0) == 0; }));
  Settings lead;
  lead.module = "swse.vader";
  lead.randomize = {"swse.vader", "ad40.alpha", "tt.foxtrot"};
  CHECK(effective_rotation(lead, c).lead == "swse.vader");
  p = effective_rotation(lead, c, playable);
  CHECK(p.lead.empty() && (p.ids == Strs{"ad40.alpha", "tt.foxtrot"}));
  // A list of theirs alone falls back to every module the host can run.
  Settings only;
  only.randomize = {"swse.vader", "swse.jawas"};
  CHECK_EQ(effective_rotation(only, c, playable).ids.size(), (size_t)13);
  // When the saver must ask the host first: its rotation holds one of them.
  CHECK(rotation_needs_capabilities(s, c));
  CHECK(rotation_needs_capabilities(lead, c) && rotation_needs_capabilities(only, c));
  Settings simpsons;
  simpsons.collections = {"simpsons"};
  CHECK(!rotation_needs_capabilities(simpsons, c));
  simpsons.collections = {"simpsons", "swse"};
  CHECK(rotation_needs_capabilities(simpsons, c));
  Settings ad;
  ad.randomize = {"ad40.alpha", "tt.beta"};
  CHECK(!rotation_needs_capabilities(ad, c));
  ad.module = "swse.vader";   // a lead of theirs in front of an After Dark list
  CHECK(rotation_needs_capabilities(ad, c));
  Settings single;
  single.module = "swse.vader";   // one module, no rotation: it is simply tried
  CHECK(!single.rotates() && !rotation_needs_capabilities(single, c));
  // Their files missing: nothing of theirs can rotate, nothing to ask.
  CHECK(!rotation_needs_capabilities(s, c, [](const std::string& id) { return id.rfind("swse.", 0) != 0; }));
  Catalog five;
  CHECK(parse_catalog(fixture("catalog-releases.json"), five, nullptr));
  CHECK(!rotation_needs_capabilities(s, five));
  // What the saver plays with the host's answer (rotation_for_host), and how
  // many modules its log says it left out: those the rotation would have
  // held without the answer (a list of two loses at most two), and nothing
  // at all to play when the host can run none of the modules available.
  HostRotation hr = rotation_for_host(s, c, nullptr, playable);
  CHECK(hr.plan.ids.size() == 13 && hr.left_out == 14);
  hr = rotation_for_host(only, c, nullptr, playable);
  CHECK(hr.plan.ids.size() == 13 && hr.left_out == 2);   // the list's two; the rest plays instead
  Settings pair;
  pair.randomize = {"swse.vader", "ad40.alpha"};
  hr = rotation_for_host(pair, c, nullptr, playable);
  CHECK((hr.plan.ids == Strs{"ad40.alpha"}) && hr.left_out == 1);
  hr = rotation_for_host(lead, c, nullptr, playable);   // the lead, in its own list too: counted once
  CHECK(hr.plan.lead.empty() && (hr.plan.ids == Strs{"ad40.alpha", "tt.foxtrot"}) && hr.left_out == 1);
  hr = rotation_for_host(ad, c, nullptr, playable);     // a lead of theirs in front of an After Dark list
  CHECK(hr.plan.lead.empty() && (hr.plan.ids == Strs{"ad40.alpha", "tt.beta"}) && hr.left_out == 1);
  auto theirs = [](const std::string& id) { return id.rfind("swse.", 0) == 0; };
  hr = rotation_for_host(s, c, theirs, playable);       // theirs alone imported
  CHECK(hr.plan.ids.empty() && hr.plan.lead.empty() && hr.left_out == 14);
  hr = rotation_for_host(only, c, theirs, playable);
  CHECK(hr.plan.ids.empty() && hr.left_out == 2);
  // Today's host, or no answer asked for: the rotation as it stands.
  const HostCapabilities new_host = parse_capabilities("lanes=pe32,ne16 configure=pe32,ne16 abis=afterdark,intermission");
  auto runs_on = [&](const HostCapabilities& h) {
    return [&c, h](const std::string& id) {
      const Module* m = c.find(id);
      return m && h.runs(m->lane, m->abi);
    };
  };
  hr = rotation_for_host(s, c, nullptr, runs_on(new_host));
  CHECK(hr.plan.ids.size() == 27 && hr.left_out == 0);
  hr = rotation_for_host(s, c, theirs, nullptr);
  CHECK(hr.plan.ids.size() == 14 && hr.left_out == 0);
  // A host without the Classic lane: the pe32 modules alone, one per sameAs set.
  hr = rotation_for_host(s, c, nullptr, runs_on(parse_capabilities("lanes=pe32 configure=pe32 abis=afterdark,intermission")));
  CHECK((hr.plan.ids == Strs{"ad40.alpha", "ad40.twin", "ad10.gamma"}) && hr.left_out == 24);
  // A host that didn't answer is taken to run everything: nothing is left out.
  hr = rotation_for_host(s, c, nullptr, runs_on(HostCapabilities{}));
  CHECK(hr.plan.ids.size() == 27 && hr.left_out == 0);

  // The own screens a window's first module may have (first_module_screens:
  // {0, 0} for After Dark's, which follow the display; an Intermission
  // module's 640x480): the saver takes the desktop seed at each one's
  // screen, before it knows the module. (As the ABIs were, one for one: this
  // catalog gives no module a "screen".)
  {
    using Screens = std::set<SizeI>;
    const Screens both{SizeI{}, SizeI{640, 480}}, ad_only{SizeI{}}, imx_only{SizeI{640, 480}};
    Settings one;
    one.module = "swse.vader";   // alone: its own
    CHECK(first_module_screens(one, c) == imx_only);
    one.module = "ad40.alpha";
    CHECK(first_module_screens(one, c) == ad_only);
    one.module = "gone.module";   // gone: the saver shows any of those there are
    CHECK(first_module_screens(one, c) == both);
    CHECK(first_module_screens(one, c, [](const std::string& id) { return id.rfind("swse.", 0) != 0; }) == ad_only);
    CHECK(first_module_screens(s, c) == both);              // Random, every module
    CHECK(first_module_screens(ad, c) == both);             // a lead of theirs in front of an After Dark list
    ad.module = "random";
    CHECK(first_module_screens(ad, c) == ad_only);          // an After Dark list alone
    CHECK(first_module_screens(simpsons, c) == both);       // Simpsons and Star Wars selected
    simpsons.collections = {"simpsons"};
    CHECK(first_module_screens(simpsons, c) == ad_only);
    // A list of theirs alone: a host too old for them plays every module it
    // can run instead, so an After Dark module may come first too.
    CHECK(first_module_screens(only, c) == both);
    CHECK(first_module_screens(only, c, theirs) == imx_only);   // theirs alone imported
    CHECK(first_module_screens(s, c, [](const std::string&) { return false; }).empty());
  }

  // The group header of "Star Wars Screen Entertainment" at the minimum
  // window, in Random (the group checkbox takes room at the left), at
  // 100-250%: the count ("14") shows whole, and so does "Coming soon" when
  // the release can't run, the title ellipsized for them; the other titles
  // are ellipsized only to make room for the pill.
  HDC dc = CreateCompatibleDC(nullptr);
  for (int dpi = 96; dpi <= 240; dpi += 24) {
    adw::ui::Theme t;
    t.set_dpi(dpi);
    LayoutInput in{dip(kMinClientW, dpi), dip(kMinClientHStrip, dpi), dpi, true};
    in.strip_tiles = 6;
    const WindowLayout L = layout_window(in);
    CHECK(!L.tiles.overflow);   // six covers fit the narrowest window without scrolling
    auto title_w = [&](const std::wstring& s) { return (int)adw::ui::measure_text(dc, s, t.fonts.body_strong).cx; };
    const int count_w = (int)adw::ui::measure_text(dc, L"14", t.fonts.caption).cx;
    const int pill_w = (int)adw::ui::measure_text(dc, L"Coming soon", t.fonts.caption).cx + t.px(16);
    for (bool scrolls : {false, true}) {
      for (bool pill : {false, true}) {
        // The frame draw_group_headers uses, across the list's width.
        GroupHeaderInput h = group_header_frame(L.list.w, dpi, true, scrolls);
        CHECK(h.left == t.px(16) + t.px(kListBoxDip) + t.px(12) && h.right == L.list.w - t.px(16) && h.gap == t.px(8));
        h.count_w = count_w;
        h.pill_w = pill ? pill_w : 0;
        for (const Release& r : c.releases) {
          h.title = widen(r.title);
          const GroupHeaderLayout g = layout_group_header(h, title_w);
          const int count_end = g.count_x + count_w;
          if (count_end > h.right || (pill && count_end + h.gap > g.pill_x) || g.title.empty()) {
            fprintf(stderr, "group header \"%s\" @%d%s%s: count ends at %d, right %d, pill at %d, title \"%s\"\n",
                    r.title.c_str(), dpi, pill ? " (pill)" : "", scrolls ? " (scrolls)" : "", count_end, h.right, g.pill_x,
                    narrow(g.title).c_str());
            ++g_failures;
          }
          // Without a pill, only the Star Wars title is too long for the
          // narrowest list; every After Dark title keeps fitting whole.
          if (!pill && r.id != "swse" && g.ellipsized) {
            fprintf(stderr, "group header \"%s\" @%d ellipsized without a pill\n", r.title.c_str(), dpi);
            ++g_failures;
          }
          if (!pill && r.id == "swse" && dpi == 96) CHECK(g.ellipsized);   // the survey's render F: "…Entertainment 1"
        }
      }
    }
  }
  // At the first-open size the Star Wars title fits whole beside its count.
  {
    adw::ui::Theme t;
    t.set_dpi(96);
    LayoutInput in{kDesignClientW, kDesignClientHStrip, 96, true};
    in.strip_tiles = 6;
    const WindowLayout L = layout_window(in);
    GroupHeaderInput h = group_header_frame(L.list.w, 96, true, true);
    h.title = L"Star Wars Screen Entertainment";
    h.count_w = (int)adw::ui::measure_text(dc, L"14", t.fonts.caption).cx;
    CHECK(!layout_group_header(h, [&](const std::wstring& s) { return (int)adw::ui::measure_text(dc, s, t.fonts.body_strong).cx; })
               .ellipsized);
  }
  DeleteDC(dc);
}

// Seven releases (catalog-seven.json): Star Trek: The Screen Saver first
// (1992-11, the oldest), whose four modules (After Dark 2.0b: lane ne16,
// After Dark's ABI) each have "screen": "640x480", then the six. The list
// and its words, Random, what the saver seeds a window's first host with,
// and seven covers in the strip.
void test_releases_seven() {
  using Strs = std::vector<std::string>;
  Catalog c;
  std::string err;
  CHECK(parse_catalog(fixture("catalog-seven.json"), c, &err));
  CHECK_EQ(c.releases.size(), (size_t)7);
  if (c.releases.size() != 7) return;
  Strs ids, shorts;
  for (const Release& r : c.releases) {
    ids.push_back(r.id);
    shorts.push_back(r.short_title);
  }
  // Oldest first, as adimport orders them: the release of 1992 before the rest.
  CHECK((ids == Strs{"startrek", "simpsons", "swse", "ad32", "tt", "deluxe", "ad10"}));
  CHECK((shorts ==
         Strs{"Star Trek", "Simpsons", "Star Wars", "After Dark 3.2", "Totally Twisted", "Deluxe", "10th Anniversary"}));
  CHECK(c.releases[0].title == "Star Trek: The Screen Saver" && c.releases[0].modules == 4 &&
        c.releases[0].cover.generated());
  CHECK(c.modules_in(0) == 4 && strip_shown(c));
  ListModel all = build_list(c, {});
  CHECK(all.shown == 36 && all.groups.size() == 7);
  if (all.groups.size() == 7) {
    CHECK_EQ(all.groups[0].release, 0);
    CHECK((labels_of(all.groups[0]) == Strs{"Communications", "Final Exam", "The Mission", "Tribbles"}));
  }
  CHECK(assets_summary(count_assets(c, std::vector<bool>(36, true))) == L"36 modules from 7 releases");
  CHECK(tile_name(c.releases[0], 4) == L"Star Trek: The Screen Saver, 4 screen savers");
  CHECK(strip_status(1, 7) == L"Showing 1 of 7 releases" && strip_status(7, 7) == L"Showing all 7 releases");
  // Random: every module, theirs included; they are After Dark's ABI, so no
  // rotation waits for the host's answer on their account.
  Settings s;
  CHECK_EQ(effective_rotation(s, c).ids.size(), (size_t)31);   // 27 as with six releases, and their 4
  Settings trek;
  trek.collections = {"startrek"};
  CHECK(!rotation_needs_capabilities(trek, c) && effective_rotation(trek, c).ids.size() == 4);
  // Their screen: their own 640x480 on a 16:9 monitor at 720 lines, as the
  // Intermission modules' (module_screen over own_screen); the rest 1280x720.
  for (const Module& m : c.modules) {
    const ModuleScreen ms = module_screen(own_screen(m.abi, m.screen), 1920.0 / 1080.0, 1.5);
    if (m.package == "startrek" || m.package == "swse") CHECK((ms.emu == SizeI{640, 480}) && ms.fixed);
    else CHECK((ms.emu == SizeI{1280, 720}) && !ms.fixed);
  }
  // The screens a window's first host may be given (first_module_screens):
  // theirs and the Intermission modules' are one, 640x480, so a window that
  // may start with either gets one picture of the frame's part.
  {
    using Screens = std::set<SizeI>;
    const Screens both{SizeI{}, SizeI{640, 480}}, display{SizeI{}}, own{SizeI{640, 480}};
    Settings one;
    one.module = "startrek.final";
    CHECK(first_module_screens(one, c) == own);
    one.module = "ad40.alpha";
    CHECK(first_module_screens(one, c) == display);
    CHECK(first_module_screens(trek, c) == own);   // Star Trek alone selected
    trek.collections = {"startrek", "simpsons"};
    CHECK(first_module_screens(trek, c) == both);
    // With Star Wars selected, what Random plays waits on the host's answer
    // (rotation_needs_capabilities), so every module available may come
    // first; a Star Trek and a Star Wars module chosen alone share one screen.
    trek.collections = {"startrek", "swse"};
    CHECK(rotation_needs_capabilities(trek, c) && first_module_screens(trek, c) == both);
    one.module = "swse.vader";
    CHECK(first_module_screens(one, c) == own);
    Settings list;
    list.randomize = {"startrek.final", "startrek.tribble"};
    CHECK(first_module_screens(list, c) == own);
    list.randomize = {"startrek.final", "ad40.alpha"};
    CHECK(first_module_screens(list, c) == both);
    list.module = "startrek.comms";   // a lead of theirs in front of an After Dark list
    list.randomize = {"ad40.alpha", "tt.beta"};
    CHECK(first_module_screens(list, c) == both);
    CHECK(first_module_screens(s, c) == both);   // Random, every module
    auto theirs = [](const std::string& id) { return id.rfind("startrek.", 0) == 0; };
    CHECK(first_module_screens(s, c, theirs) == own);   // theirs alone imported
    // A monitor's pictures (plan_seed_shots): After Dark's whole monitor, and
    // one of the frame's part for theirs and Star Wars' alike.
    std::vector<ModuleScreen> screens;
    for (const SizeI& o : first_module_screens(s, c)) screens.push_back(module_screen(o, 1920.0 / 1080.0, 1.5));
    const std::vector<SeedShotPlan> plan = plan_seed_shots(screens, 1920, 1080);
    CHECK(plan.size() == 2);
    if (plan.size() == 2) {
      CHECK(!plan[0].screen.fixed && (plan[0].src == RectI{0, 0, 1920, 1080}));
      CHECK((plan[1].screen.emu == SizeI{640, 480}) && plan[1].screen.fixed && (plan[1].src == RectI{240, 0, 1440, 1080}));
    }
  }
  // Seven covers (COVERS.md §1.2): they fit the first-open window, and the
  // smallest one with its compact covers, at every scale without scrolling;
  // a window as narrow but 760 DIP or more tall has regular covers and shows
  // five at a time (only whole tiles show, clear of the chevrons): its row
  // scrolls, by two tiles at most, to the third cover.
  auto shown = [](const StripLayout& t) {
    return (int)std::count_if(t.arts.begin(), t.arts.end(), [&](const Rc& a) { return t.view.contains(a); });
  };
  auto whole = [](const StripLayout& t) { return (int)std::count(t.whole.begin(), t.whole.end(), true); };
  for (int dpi = 96; dpi <= 240; dpi += 24) {
    auto strip_at = [&](int w, int h, int scrolled = 0) {
      LayoutInput in{dip(w, dpi), dip(h, dpi), dpi, true};
      in.strip_tiles = 7;
      in.strip_first = scrolled;
      return layout_window(in);
    };
    const WindowLayout first = strip_at(kDesignClientW, kDesignClientHStrip);
    const WindowLayout small = strip_at(kMinClientW, kMinClientHStrip);
    const WindowLayout tall = strip_at(kMinClientW, kDesignClientHStrip);
    CHECK(first.strip_mode == StripMode::regular && first.tiles.cells.size() == 7 && !first.tiles.overflow);
    CHECK(small.strip_mode == StripMode::compact && small.tiles.cells.size() == 7 && !small.tiles.overflow);
    CHECK(tall.strip_mode == StripMode::regular && tall.tiles.overflow);
    CHECK(tall.tiles.max_first == 2 && shown(tall.tiles) == 5 && whole(tall.tiles) == 5 && tall.tiles.slots == 5);
    for (int stop : {1, 2}) {
      const StripLayout t = strip_at(kMinClientW, kDesignClientHStrip, stop).tiles;
      CHECK(t.first == stop && shown(t) == 5 && whole(t) == 5);
    }
    if (dpi == 96) {
      printf("releases: seven regular covers in a 900 DIP window: %d shown, scrolling by %d tiles at most (to cover %d)\n",
             shown(tall.tiles), tall.tiles.max_first, tall.tiles.max_first + 1);
    }
  }
}

// Twelve releases (catalog-twelve.json): the seven and, among them by date,
// Marvel Comics Screen Posters (1993-12), Snoopy's Screen Savers (1994-10),
// The Looney Tunes Screen Saver and ScreamSavers (1995-04, registry order on
// the tie) and The Disney Collection Screen Saver (1995-09). Their modules
// (placeholder names) are all After Dark's (lane ne16), ScreamSavers' and
// Marvel's each with "screen": "640x480". The list and its words, Random,
// their screens, twelve covers in the strip, and every release's title in
// the list's group headers, measured in the real faces.
void test_releases_twelve() {
  using Strs = std::vector<std::string>;
  Catalog c;
  std::string err;
  CHECK(parse_catalog(fixture("catalog-twelve.json"), c, &err));
  CHECK_EQ(c.releases.size(), (size_t)12);
  if (c.releases.size() != 12) return;
  Strs ids, shorts;
  for (const Release& r : c.releases) {
    ids.push_back(r.id);
    shorts.push_back(r.short_title);
  }
  // Oldest first, as adimport orders them.
  CHECK((ids ==
         Strs{"startrek", "marvel", "simpsons", "swse", "snoopy", "looney", "screams", "ad32", "tt", "disney", "deluxe", "ad10"}));
  CHECK((shorts == Strs{"Star Trek", "Marvel", "Simpsons", "Star Wars", "Snoopy", "Looney Tunes", "ScreamSavers",
                        "After Dark 3.2", "Totally Twisted", "Disney", "Deluxe", "10th Anniversary"}));
  CHECK(c.releases[1].title == "Marvel Comics Screen Posters" && c.releases[4].title == "Snoopy's Screen Savers" &&
        c.releases[5].title == "The Looney Tunes Screen Saver" && c.releases[6].title == "ScreamSavers" &&
        c.releases[9].title == "The Disney Collection Screen Saver");
  CHECK(c.releases[4].cover.generated() && !c.releases[6].cover.generated() && c.modules_in(9) == 2);
  ListModel all = build_list(c, {});
  CHECK(all.shown == 46 && all.groups.size() == 12);
  if (all.groups.size() == 12) {
    for (int g = 0; g < 12; ++g) CHECK_EQ(all.groups[g].release, g);
    CHECK((labels_of(all.groups[6]) == Strs{"Papa Ghoul", "Quebec Spin", "Romeo Grin"}));
    CHECK((labels_of(all.groups[9]) == Strs{"Sierra Clocks", "Tango Flower"}));
  }
  CHECK(assets_summary(count_assets(c, std::vector<bool>(46, true))) == L"46 modules from 12 releases");
  CHECK(tile_name(c.releases[4], 2) == L"Snoopy's Screen Savers, 2 screen savers");
  CHECK(strip_status(1, 12) == L"Showing 1 of 12 releases" && strip_status(11, 12) == L"Showing 11 of 12 releases" &&
        strip_status(12, 12) == L"Showing all 12 releases");
  // Random: every module, one per sameAs set (the new releases ship no
  // copies); theirs are After Dark's ABI, so no rotation waits for the
  // host's answer on their account.
  Settings s;
  CHECK_EQ(effective_rotation(s, c).ids.size(), (size_t)41);   // 31 as with seven releases, and their 10
  Settings fresh;
  fresh.collections = {"marvel", "snoopy", "looney", "screams", "disney"};
  CHECK(!rotation_needs_capabilities(fresh, c) && effective_rotation(fresh, c).ids.size() == 10);
  // Their screens (module_screen over own_screen): ScreamSavers' and
  // Marvel's catalog 640x480 whatever the display and the Resolution
  // setting, as Star Trek's and the Intermission modules'; Snoopy's, Looney
  // Tunes' and Disney's follow the display, as every other After Dark module.
  for (const Module& m : c.modules) {
    const bool own = m.package == "screams" || m.package == "marvel" || m.package == "startrek" || m.package == "swse";
    if (m.package == "screams" || m.package == "marvel") CHECK(m.abi == kAfterDarkAbi && (m.screen == SizeI{640, 480}));
    if (m.package == "snoopy" || m.package == "looney" || m.package == "disney") CHECK((m.screen == SizeI{}));
    for (auto [aspect, scale, display] : {std::tuple{1920.0 / 1080.0, 1.5, SizeI{1280, 720}},
                                          std::tuple{1024.0 / 768.0, 1.0, SizeI{640, 480}},
                                          std::tuple{2560.0 / 1080.0, 1.0, SizeI{1136, 480}}}) {
      const ModuleScreen ms = module_screen(own_screen(m.abi, m.screen), aspect, scale);
      if (own) CHECK((ms.emu == SizeI{640, 480}) && ms.fixed);
      else CHECK(ms.emu == display && !ms.fixed);
    }
  }
  // The screens a window's first host may be given (first_module_screens),
  // and a 16:9 monitor's pictures (plan_seed_shots).
  {
    using Screens = std::set<SizeI>;
    const Screens both{SizeI{}, SizeI{640, 480}}, display{SizeI{}}, own{SizeI{640, 480}};
    Settings one;
    one.module = "screams.papa";
    CHECK(first_module_screens(one, c) == own);
    one.module = "marvel.kilo";
    CHECK(first_module_screens(one, c) == own);
    one.module = "disney.sierra";
    CHECK(first_module_screens(one, c) == display);
    Settings sel;
    sel.collections = {"screams"};
    CHECK(first_module_screens(sel, c) == own);
    sel.collections = {"screams", "marvel", "startrek"};   // one screen for all of theirs
    CHECK(first_module_screens(sel, c) == own);
    sel.collections = {"snoopy", "looney", "disney"};
    CHECK(first_module_screens(sel, c) == display);
    sel.collections = {"screams", "disney"};
    CHECK(first_module_screens(sel, c) == both);
    std::vector<ModuleScreen> screens;
    for (const SizeI& o : first_module_screens(sel, c)) screens.push_back(module_screen(o, 1920.0 / 1080.0, 1.5));
    const std::vector<SeedShotPlan> plan = plan_seed_shots(screens, 1920, 1080);
    CHECK(plan.size() == 2);
    if (plan.size() == 2) {
      CHECK(!plan[0].screen.fixed && (plan[0].src == RectI{0, 0, 1920, 1080}));
      CHECK((plan[1].screen.emu == SizeI{640, 480}) && plan[1].screen.fixed && (plan[1].src == RectI{240, 0, 1440, 1080}));
    }
  }

  // Twelve covers (COVERS.md §1.2, §1.3), at every scale, only whole tiles
  // shown, as many at every stop, none of them under a chevron and none
  // reaching the status box. Regular ones (a client 760 DIP or more tall)
  // never all fit side by side (twelve need 1240 DIP; the tiles area stops
  // growing at 1024, the content column at 1240), so their row always
  // scrolls: the first-open window 7 at a time (6 stops), one as narrow 5 (8
  // stops), a large one 9 (4 stops). Compact ones (under 760 DIP tall)
  // scroll up to 1119 DIP wide: 8 at a time in the smallest window (5 stops),
  // 9 from 960 DIP wide, 10 from 1032 (so in a first-open window whose height
  // the monitor's work area clamps under 760 DIP: 1920x1080 at 125% or 150%,
  // 1366x768 at 100%), 11 from 1104; from 1120 DIP wide all twelve show side
  // by side, left-aligned, with no chevron.
  struct Want {
    int w, h;
    StripMode mode;
    int slots;
  };
  auto whole = [](const StripLayout& t) { return (int)std::count(t.whole.begin(), t.whole.end(), true); };
  for (int dpi = 96; dpi <= 240; dpi += 24) {
    for (const Want& want : {Want{kDesignClientW, kDesignClientHStrip, StripMode::regular, 7},
                             Want{kMinClientW, kMinClientHStrip, StripMode::compact, 8},
                             Want{kMinClientW, kDesignClientHStrip, StripMode::regular, 5},
                             Want{1600, 1000, StripMode::regular, 9},
                             Want{959, 700, StripMode::compact, 8},
                             Want{960, 700, StripMode::compact, 9},
                             Want{kDesignClientW, kMinClientHStrip, StripMode::compact, 10},
                             Want{kDesignClientW, 759, StripMode::compact, 10},
                             Want{1103, 700, StripMode::compact, 10},
                             Want{1104, 700, StripMode::compact, 11},
                             Want{1119, 740, StripMode::compact, 11},
                             Want{1120, 700, StripMode::compact, 12},
                             Want{1600, 759, StripMode::compact, 12}}) {
      StripInput stops{};
      const bool scrolls = want.slots < 12;
      for (int stop = 0; stop <= 12; ++stop) {
        LayoutInput in{dip(want.w, dpi), dip(want.h, dpi), dpi, true};
        in.strip_tiles = 12;
        in.strip_first = stop;
        const WindowLayout L = layout_window(in);
        const StripLayout& t = L.tiles;
        check_layout(L, in.client_w, in.client_h, true);
        check_strip(L.strip_in, "twelve");
        stops = L.strip_in;
        CHECK(L.strip_mode == want.mode && t.overflow == scrolls && t.slots == want.slots && t.max_first == 12 - want.slots);
        CHECK(t.first == std::min(stop, t.max_first) && whole(t) == want.slots);
        CHECK(t.chevron_left.empty() == (t.first == 0) && t.chevron_right.empty() == (t.first == t.max_first));
        if (!scrolls) CHECK(t.cells[0].x == L.strip.x);   // side by side: left-aligned on the column
        for (size_t i = 0; i < t.cells.size(); ++i) {
          if (t.whole[i]) CHECK(L.strip.contains(t.cells[i]) && !t.cells[i].overlaps(L.strip_status));
        }
        for (const Rc* ch : {&t.chevron_left, &t.chevron_right}) CHECK(ch->empty() || !ch->overlaps(L.strip_status));
      }
      check_strip_stops(stops, "twelve");
    }
  }
  {
    LayoutInput in{kDesignClientW, kDesignClientHStrip, 96, true};
    in.strip_tiles = 12;
    const WindowLayout L = layout_window(in);
    printf("releases: twelve covers in the first-open window: %d at a time, %d stops\n", L.tiles.slots, L.tiles.max_first + 1);
    in.client_h = kMinClientHStrip;
    const WindowLayout C = layout_window(in);
    printf("releases: twelve covers in a first-open window clamped to %d DIP tall: %d compact at a time, %d stops\n",
           kMinClientHStrip, C.tiles.slots, C.tiles.max_first + 1);
  }

  // Every release's title in the list's group header, in the narrowest and
  // the first-open list, in both modes (in Random the group checkbox takes
  // room at the left), at 100-250%: the count shows whole, and so does
  // "Coming soon" when the release can't run, the title ellipsized for them.
  // Without a pill only the long titles give way (Star Wars Screen
  // Entertainment's, The Disney Collection Screen Saver's, The Looney Tunes
  // Screen Saver's, Marvel Comics Screen Posters'), never the After Dark ones
  // or the short ones, and none in the first-open list in Single. Their
  // whole title is the header's tooltip and what screen readers hear. Each
  // count is the release's real one (the seven's 232 modules, the five's 52).
  HDC dc = CreateCompatibleDC(nullptr);
  const std::set<std::string> long_titles = {"swse", "disney", "looney", "marvel"};
  const std::map<std::string, int> real_count = {{"startrek", 16}, {"marvel", 1},  {"simpsons", 15}, {"swse", 14},
                                                 {"snoopy", 8},    {"looney", 12}, {"screams", 15},  {"ad32", 44},
                                                 {"tt", 13},       {"disney", 16}, {"deluxe", 84},   {"ad10", 46}};
  int modules = 0;
  for (const auto& [id, n] : real_count) modules += n;
  CHECK(modules == 284 && real_count.size() == c.releases.size());
  for (int dpi = 96; dpi <= 240; dpi += 24) {
    adw::ui::Theme t;
    t.set_dpi(dpi);
    auto title_w = [&](const std::wstring& s) { return (int)adw::ui::measure_text(dc, s, t.fonts.body_strong).cx; };
    const int pill_w = (int)adw::ui::measure_text(dc, L"Coming soon", t.fonts.caption).cx + t.px(16);
    for (const auto [w, h] : {std::pair{kMinClientW, kMinClientHStrip}, std::pair{kDesignClientW, kDesignClientHStrip}}) {
      for (bool random : {true, false}) {
        LayoutInput in{dip(w, dpi), dip(h, dpi), dpi, random};
        in.strip_tiles = 12;
        const WindowLayout L = layout_window(in);
        std::string cut;   // the titles ellipsized without a pill here
        for (bool scrolls : {false, true}) {
          for (bool pill : {false, true}) {
            GroupHeaderInput hd = group_header_frame(L.list.w, dpi, random, scrolls);
            hd.pill_w = pill ? pill_w : 0;
            for (size_t ri = 0; ri < c.releases.size(); ++ri) {
              const Release& r = c.releases[ri];
              hd.title = widen(r.title);
              const auto real = real_count.find(r.id);
              const int count = real != real_count.end() ? real->second : (int)c.modules_in((int)ri);
              hd.count_w = (int)adw::ui::measure_text(dc, std::to_wstring(count), t.fonts.caption).cx;
              const GroupHeaderLayout g = layout_group_header(hd, title_w);
              const int count_end = g.count_x + hd.count_w;
              if (count_end > hd.right || (pill && count_end + hd.gap > g.pill_x) || g.title.empty()) {
                fprintf(stderr, "group header \"%s\" @%d %dx%d%s%s%s: count ends at %d, right %d, pill at %d, title \"%s\"\n",
                        r.title.c_str(), dpi, w, h, random ? " random" : "", pill ? " (pill)" : "",
                        scrolls ? " (scrolls)" : "", count_end, hd.right, g.pill_x, narrow(g.title).c_str());
                ++g_failures;
              }
              if (!pill && g.ellipsized) {
                if (!long_titles.count(r.id)) {
                  fprintf(stderr, "group header \"%s\" @%d %dx%d ellipsized without a pill\n", r.title.c_str(), dpi, w, h);
                  ++g_failures;
                }
                if (!scrolls) cut += (cut.empty() ? "" : ", ") + r.id;
              }
            }
          }
        }
        // The first-open window in Single: every title whole beside its count.
        if (w == kDesignClientW && !random) CHECK(cut.empty());
        if (dpi == 96 || dpi == 144) {
          printf("releases: group headers ellipsized without a pill @%d%% %dx%d %s: %s\n", dpi * 100 / 96, w, h,
                 random ? "random" : "single", cut.empty() ? "none" : cut.c_str());
        }
      }
    }
  }
  DeleteDC(dc);
}

// A catalog whose modules each give a "screen" of their own (a hand-edited
// or tampered one: adimport writes "640x480" alone), all naming one module
// file: each is a screen a window's first module may have
// (first_module_screens), but the window's desktop seeds stay three pictures
// (plan_seed_shots): the one that follows the display, 640x480, and the
// smallest of the others. A first module whose screen got none starts on
// black.
void test_releases_many_screens() {
  std::string mods;
  auto add = [&](const std::string& id, const std::string& screen) {
    mods += std::string(mods.empty() ? "" : ",") + "{\"id\":\"x." + id +
            "\",\"path\":\"packages/x/ONE.AD\",\"lane\":\"ne16\",\"displayName\":\"" + id +
            "\",\"package\":\"x\",\"packageTitle\":\"X\",\"moduleName\":\"" + id + "\"" +
            (screen.empty() ? "" : ",\"screen\":\"" + screen + "\"") + "}";
  };
  for (int i = 0; i < 64; ++i) add("m" + std::to_string(i), "4096x" + std::to_string(4096 - 8 * i));
  add("vga", "640x480");
  add("plain", "");
  Catalog c;
  std::string err;
  CHECK(parse_catalog("{\"version\":1,\"packages\":[{\"id\":\"x\",\"title\":\"X\",\"shortTitle\":\"X\","
                      "\"modules\":66}],\"modules\":[" + mods + "]}",
                      c, &err));
  CHECK_EQ(c.modules.size(), (size_t)66);
  auto plan_of = [&](const Settings& s, size_t* left) {
    std::vector<ModuleScreen> screens;
    for (const SizeI& own : first_module_screens(s, c)) screens.push_back(module_screen(own, 1920.0 / 1080.0, s.scale));
    return plan_seed_shots(screens, 1920, 1080, left);
  };
  Settings s;   // Random, every module
  CHECK_EQ(first_module_screens(s, c).size(), (size_t)66);
  size_t left = 0;
  std::vector<SeedShotPlan> plan = plan_of(s, &left);
  CHECK(plan.size() == 3 && left == 63);
  if (plan.size() == 3) {
    CHECK(!plan[0].screen.fixed && (plan[0].screen.emu == SizeI{856, 480}) && (plan[0].src == RectI{0, 0, 1920, 1080}));
    CHECK(plan[1].screen.fixed && (plan[1].screen.emu == SizeI{640, 480}) && (plan[1].src == RectI{240, 0, 1440, 1080}));
    CHECK(plan[2].screen.fixed && (plan[2].screen.emu == SizeI{4096, 3592}));
  }
  // The largest leading a list of all the rest: its screen is one of those
  // left out, so its first host starts on black.
  Settings lead;
  lead.module = "x.m0";
  for (const Module& m : c.modules) {
    if (m.id != lead.module) lead.randomize.push_back(m.id);
  }
  CHECK(first_module_screens(lead, c).count(SizeI{4096, 4096}) == 1);
  plan = plan_of(lead, &left);
  CHECK(plan.size() == 3 && left == 63);
  for (const SeedShotPlan& p : plan) CHECK(!(p.screen.emu == SizeI{4096, 4096}));
  // Chosen alone, it has its own picture.
  Settings one;
  one.module = "x.m0";
  plan = plan_of(one, &left);
  CHECK(plan.size() == 1 && left == 0);
  if (plan.size() == 1) CHECK(plan[0].screen.fixed && (plan[0].screen.emu == SizeI{4096, 4096}));
}

void test_releases() {
  test_releases_catalog();
  test_releases_settings();
  test_releases_rotation();
  test_releases_list();
  test_releases_layout();
  test_releases_six();
  test_releases_seven();
  test_releases_twelve();
  test_releases_many_screens();
}

// ---- input rules (INTERACTION.md §4) ------------------------------------------------

OwnerStatus status_of(uint32_t flags, uint64_t applied, uint64_t eaten, bool running = true, bool have = true) {
  OwnerStatus st;
  st.running = running;
  st.have = have;
  st.rec.magic = adw::kStatusMagic;
  st.rec.version = 1;
  st.rec.flags = flags | adw::ADWS_READY;
  st.rec.input_applied = applied;
  st.rec.input_eaten = eaten;
  return st;
}

void test_input() {
  using std::chrono::milliseconds;
  const auto t0 = InputClock::now();
  auto key = [](int vk) { return InputEvent{InputKind::key_down, vk}; };
  const OwnerStatus idle = status_of(0, 10, 0), playing = status_of(adw::ADWS_INTERACTIVE, 10, 10);

  // Exempt keys never exit, whatever the status (even with no host).
  for (int vk : {VK_CAPITAL, VK_NUMLOCK, VK_SHIFT, VK_CONTROL, VK_LSHIFT, VK_RSHIFT, VK_LCONTROL, VK_RCONTROL}) {
    CHECK(exempt_key(vk));
    CHECK(decide(key(vk), idle, {11, false, t0}, t0) == Verdict::forward);
    CHECK(decide(key(vk), OwnerStatus{}, {0, false, t0}, t0) == Verdict::forward);
  }
  CHECK(!exempt_key('A') && !exempt_key(VK_MENU) && !exempt_key(VK_ESCAPE));

  // A fresh key, not interactive, status current: exit.
  CHECK(decide(key('A'), idle, {11, false, t0}, t0) == Verdict::exit);
  // Interactive: the module has it.
  CHECK(decide(key('A'), playing, {11, false, t0}, t0) == Verdict::forward);
  CHECK(decide(InputEvent{InputKind::button_down}, playing, {11, false, t0}, t0) == Verdict::forward);
  // Ups never decide anything.
  CHECK(decide(InputEvent{InputKind::key_up, 'A'}, idle, {11, false, t0}, t0) == Verdict::forward);
  CHECK(decide(InputEvent{InputKind::button_up}, idle, {11, false, t0}, t0) == Verdict::forward);

  // A key right after CAPS (lines 11 = KEY 20, 12 = CAPS 1, 13 = the key):
  // the host has applied only up to 10, so the status is stale: hold.
  CHECK(decide(key('A'), idle, {13, false, t0}, t0) == Verdict::hold);
  // ...and once the host reports interactive, no exit.
  CHECK(decide(key('A'), status_of(adw::ADWS_INTERACTIVE, 12, 12), {13, true, t0}, t0 + milliseconds(40)) ==
        Verdict::forward);
  // ...or once it has applied the key without taking it (a Caps-only module): exit.
  CHECK(decide(key('A'), status_of(0, 13, 0), {13, true, t0}, t0 + milliseconds(40)) == Verdict::exit);
  // ...still waiting for the host to step with it: keep holding.
  CHECK(decide(key('A'), status_of(0, 12, 0), {13, true, t0}, t0 + milliseconds(40)) == Verdict::hold);
  // A hold lasts at most 300 ms, then the status as it is decides.
  CHECK(decide(key('A'), idle, {13, true, t0}, t0 + milliseconds(299)) == Verdict::hold);
  CHECK(decide(key('A'), idle, {13, true, t0}, t0 + kHoldLimit) == Verdict::exit);
  CHECK(decide(key('A'), status_of(0, 10, 13), {13, true, t0}, t0 + kHoldLimit) == Verdict::forward);   // eaten meanwhile

  // key_filter: the guest may take keys without playing (a keyboard hook, a
  // module reading the saver window's queue): wait for the verdict.
  const OwnerStatus filter = status_of(adw::ADWS_KEY_FILTER, 10, 0);
  CHECK(decide(key('1'), filter, {11, false, t0}, t0) == Verdict::hold);
  CHECK(decide(key('1'), status_of(adw::ADWS_KEY_FILTER, 11, 11), {11, true, t0}, t0 + milliseconds(20)) ==
        Verdict::forward);   // eaten >= n: no exit
  CHECK(decide(key('1'), status_of(adw::ADWS_KEY_FILTER, 11, 10), {11, true, t0}, t0 + milliseconds(20)) ==
        Verdict::exit);      // applied, not eaten
  CHECK(decide(key('1'), status_of(0, 10, 0), {11, false, t0}, t0) == Verdict::exit);   // no filter: at once

  // Alt / F10 (WM_SYSKEYDOWN) always exit; switching away always exits.
  CHECK(decide(InputEvent{InputKind::syskey_down, VK_MENU}, playing, {0, false, t0}, t0) == Verdict::exit);
  CHECK(decide(InputEvent{InputKind::syskey_down, VK_F10}, idle, {0, false, t0}, t0) == Verdict::exit);
  CHECK(decide(InputEvent{InputKind::syskey_up, VK_MENU}, idle, {0, false, t0}, t0) == Verdict::forward);
  CHECK(decide(InputEvent{InputKind::deactivate}, playing, {0, false, t0}, t0) == Verdict::exit);
  // The wheel: exits unless playing.
  CHECK(decide(InputEvent{InputKind::wheel}, idle, {0, false, t0}, t0) == Verdict::exit);
  CHECK(decide(InputEvent{InputKind::wheel}, playing, {0, false, t0}, t0) == Verdict::forward);
  // Buttons like keys.
  CHECK(decide(InputEvent{InputKind::button_down}, idle, {11, false, t0}, t0) == Verdict::exit);
  CHECK(decide(InputEvent{InputKind::button_down}, idle, {13, false, t0}, t0) == Verdict::hold);

  // Moves: within 10 px never; past it, like a key; while playing, never.
  CHECK(decide(InputEvent{InputKind::move, 0, 9.9}, idle, {11, false, t0}, t0) == Verdict::forward);
  CHECK(decide(InputEvent{InputKind::move, 0, 10.0}, idle, {11, false, t0}, t0) == Verdict::forward);
  CHECK(decide(InputEvent{InputKind::move, 0, 10.5}, idle, {11, false, t0}, t0) == Verdict::exit);
  CHECK(decide(InputEvent{InputKind::move, 0, 500}, playing, {11, false, t0}, t0) == Verdict::forward);
  CHECK(decide(InputEvent{InputKind::move, 0, 50}, idle, {13, false, t0}, t0) == Verdict::hold);
  // Re-baselining is the caller's: after play ends, the distance counts from
  // where the cursor was then, so a small move is no longer past the threshold.
  CHECK(decide(InputEvent{InputKind::move, 0, 3}, idle, {14, false, t0}, t0) == Verdict::forward);

  // The owner's host dies during a hold: exit at once. No host at all: exit.
  CHECK(decide(key('A'), status_of(0, 10, 0, /*running=*/false), {13, true, t0}, t0 + milliseconds(5)) ==
        Verdict::exit);
  CHECK(decide(key('A'), OwnerStatus{}, {0, false, t0}, t0) == Verdict::exit);
  // A host that has not published yet (still starting) counts as stale.
  CHECK(decide(key('A'), status_of(0, 0, 0, true, /*have=*/false), {1, false, t0}, t0) == Verdict::hold);
  CHECK(decide(key('A'), status_of(0, 0, 0, true, false), {1, true, t0}, t0 + kHoldLimit) == Verdict::exit);

  // Exit reasons for the logs.
  CHECK(exit_reason(key('A')) == "key vk=0x41");
  CHECK(exit_reason(InputEvent{InputKind::syskey_down, VK_MENU}) == "syskey vk=0x12");
  CHECK(exit_reason(InputEvent{InputKind::move}, 12, -3) == "move dx=12 dy=-3");
  CHECK(exit_reason(InputEvent{InputKind::deactivate}) == "deactivated");

  // MOUSE lines: into the letterboxed frame, scaled, clamped.
  const RECT win{100, 50, 100 + 1920, 50 + 1080};
  const SizeI emu{856, 480};
  const RectI fit = fit_rect(emu.w, emu.h, 1920, 1080);
  POINT p = map_to_frame({100 + fit.x, 50 + fit.y}, win, fit, emu);
  CHECK(p.x == 0 && p.y == 0);
  p = map_to_frame({100 + fit.x + fit.w - 1, 50 + fit.y + fit.h - 1}, win, fit, emu);
  CHECK(p.x == emu.w - 1 && p.y == emu.h - 1);
  p = map_to_frame({100 + fit.x + fit.w / 2, 50 + fit.y + fit.h / 2}, win, fit, emu);
  CHECK(std::abs(p.x - emu.w / 2) <= 1 && std::abs(p.y - emu.h / 2) <= 1);
  p = map_to_frame({-5000, 9000}, win, fit, emu);   // another monitor: clamped
  CHECK(p.x == 0 && p.y == emu.h - 1);
  p = map_to_frame({100 + fit.x - 1, 50 + fit.y}, win, fit, emu);   // just left of the frame
  CHECK(p.x == 0);
  RECT fr = frame_screen_rect(win, fit);
  CHECK(fr.left == 100 + fit.x && fr.right == 100 + fit.x + fit.w && fr.top == 50 + fit.y);
  CHECK(key_line(65, true) == "KEY 65 1" && key_line(20, false) == "KEY 20 0");
  CHECK(caps_line(true) == "CAPS 1" && mouse_line(3, 4, 5) == "MOUSE 3 4 5" && mouse_line(3, 4, 9) == "MOUSE 3 4 1");
  // Num Lock (INTERACTION.md §3.2): its own line, as CAPS has, for a host
  // that keeps the toggle; its key is exempt above (it never wakes).
  CHECK(numlock_line(true) == "NUMLOCK 1" && numlock_line(false) == "NUMLOCK 0");
  CHECK(key_line(VK_NUMLOCK, true) == "KEY 144 1");

  // AD_SCR_TEST_INPUT scripts.
  std::vector<TestStep> steps;
  std::string err;
  CHECK(parse_test_script("# play a bit\nFRAMES 5\nKEY 20 1\nkey 20 0\nWAIT 500\nKEY 0x25 1 # left\n"
                          "SYSKEY 18 1\nCAPSSTATE 1\nBUTTON 2 1\nWHEEL\nMOVE -3 40\nDEACTIVATE\nCLIPLOG\n"
                          "STATUSLOG\nLOG hello there\n\n",
                          steps, &err));
  CHECK(steps.size() == 14);
  if (steps.size() == 14) {
    CHECK(steps[0].op == TestStep::Op::frames && steps[0].a == 5);
    CHECK(steps[1].op == TestStep::Op::key && steps[1].a == 20 && steps[1].b == 1);
    CHECK(steps[2].op == TestStep::Op::key && steps[2].b == 0);
    CHECK(steps[4].a == 0x25);
    CHECK(steps[5].op == TestStep::Op::syskey && steps[5].a == 18);
    CHECK(steps[6].op == TestStep::Op::caps_state && steps[6].a == 1);
    CHECK(steps[7].op == TestStep::Op::button && steps[7].a == 2 && steps[7].b == 1);
    CHECK(steps[9].op == TestStep::Op::move && steps[9].a == -3 && steps[9].b == 40);
    CHECK(steps[13].op == TestStep::Op::log && steps[13].text == "hello there");
  }
  CHECK(!parse_test_script("KEY 20\n", steps, &err) && err.find("line 1") != std::string::npos);
  CHECK(!parse_test_script("WAIT 1\nBUTTON 3 1\n", steps, &err) && err.find("line 2") != std::string::npos);
  CHECK(!parse_test_script("JUMP\n", steps, &err));
  CHECK(parse_test_script("DISPLAYCHANGE\nMOVE 400 300\n", steps, &err) && steps.size() == 2 &&
        steps[0].op == TestStep::Op::display_change);
  // The synthetic Num Lock: set without a key, or flipped by KEY 144 1 (saver.cc).
  CHECK(parse_test_script("NUMLOCKSTATE 1\nnumlockstate 0\nNUMLOCKSTATE 7\nKEY 144 1\n", steps, &err) && steps.size() == 4);
  if (steps.size() == 4) {
    CHECK(steps[0].op == TestStep::Op::numlock_state && steps[0].a == 1 && steps[1].a == 0 && steps[2].a == 1);
    CHECK(steps[3].op == TestStep::Op::key && steps[3].a == VK_NUMLOCK && steps[3].b == 1);
  }
  CHECK(!parse_test_script("NUMLOCKSTATE\n", steps, &err) && err.find("NUMLOCKSTATE <0|1>") != std::string::npos);
  CHECK(!parse_test_script("NUMLOCKSTATE 1 1\n", steps, &err));
}

// ---- desktop seed, last-exit log, input lines ----------------------------------------

void test_seed() {
  // P6 from BGRX rows (with stride padding).
  const int w = 3, h = 2, stride = 16;
  std::vector<uint8_t> bgrx(stride * h, 0xEE);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      uint8_t* p = &bgrx[y * stride + x * 4];
      p[0] = (uint8_t)(x + 1);        // B
      p[1] = (uint8_t)(10 * (y + 1)); // G
      p[2] = (uint8_t)(100 + x);      // R
    }
  std::vector<uint8_t> p6 = encode_p6(bgrx.data(), w, h, stride);
  const std::string head = "P6\n3 2\n255\n";
  CHECK(p6.size() == head.size() + w * h * 3);
  CHECK(memcmp(p6.data(), head.data(), head.size()) == 0);
  CHECK(p6[head.size()] == 100 && p6[head.size() + 1] == 10 && p6[head.size() + 2] == 1);
  CHECK(p6[head.size() + 3 * 4] == 101 && p6[head.size() + 3 * 4 + 1] == 20);

  // The seed file: readable by another opener that shares delete (as a host
  // opens it), gone when the handle closes, whatever happens.
  std::wstring path = seed_file_path(GetCurrentProcessId(), 7);
  CHECK(path.find(L"LongAfterDark-seed-" + std::to_wstring(GetCurrentProcessId()) + L"-7.ppm") != std::wstring::npos);
  // The capture for an Intermission module's own screen, beside it.
  const std::wstring sized = seed_file_path(GetCurrentProcessId(), 7, L"640x480");
  CHECK(sized.size() > path.size() && sized.substr(0, path.size() - 4) == path.substr(0, path.size() - 4) &&
        sized.compare(sized.size() - 12, 12, L"-640x480.ppm") == 0);
  std::wstring err;
  HANDLE f = write_seed_file(path, p6, &err);
  CHECK(f != INVALID_HANDLE_VALUE);
  HANDLE r = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, 0, nullptr);
  CHECK(r != INVALID_HANDLE_VALUE);
  if (r != INVALID_HANDLE_VALUE) {
    std::vector<uint8_t> back(p6.size() + 8);
    DWORD got = 0;
    ReadFile(r, back.data(), (DWORD)back.size(), &got, nullptr);
    CHECK(got == p6.size() && memcmp(back.data(), p6.data(), got) == 0);
    CloseHandle(r);
  }
  // Without FILE_SHARE_DELETE a delete-on-close file cannot be opened.
  HANDLE strict = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
  CHECK(strict == INVALID_HANDLE_VALUE);
  if (strict != INVALID_HANDLE_VALUE) CloseHandle(strict);
  if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
  CHECK(!file_exists(path));
  CHECK(write_seed_file(path, {}, &err) == INVALID_HANDLE_VALUE && !file_exists(path));

  // A capture of the primary monitor at a small emulated size.
  RECT mon{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
  std::vector<uint8_t> shot = capture_monitor_p6(mon, {64, 48});
  if (!shot.empty()) {   // (no desktop in some CI sessions)
    const std::string sh = "P6\n64 48\n255\n";
    CHECK(shot.size() == sh.size() + 64 * 48 * 3 && memcmp(shot.data(), sh.data(), sh.size()) == 0);
  }
  // Every shot's picture, taken as it comes: in order, one call each.
  std::vector<size_t> order;
  auto collect = [&order](std::vector<std::vector<uint8_t>>& into) {
    order.clear();
    into.clear();
    return [&order, &into](size_t shot, const std::vector<uint8_t>& p6) {
      order.push_back(shot);
      into.push_back(p6);
    };
  };
  // The parts, from a picture of known colours (no desktop needed): 64x32,
  // its left half red and its right half blue. The whole at 8x4 keeps red on
  // the left and blue on the right; the right half alone is all blue; a part
  // that leaves the picture, and a shot without a size, get none.
  {
    HDC mem = CreateCompatibleDC(nullptr);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = 64;
    bi.bmiHeader.biHeight = -32;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = mem ? CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    CHECK(bmp != nullptr);
    if (bmp) {
      auto paint = [&](uint32_t left, uint32_t right) {   // BGRX
        GdiFlush();
        auto* px = static_cast<uint32_t*>(bits);
        for (int y = 0; y < 32; ++y)
          for (int x = 0; x < 64; ++x) px[y * 64 + x] = x < 32 ? left : right;
      };
      const uint32_t kRed = 0x00FF0000u, kBlue = 0x000000FFu, kGreen = 0x0000FF00u;
      paint(kRed, kBlue);
      HGDIOBJ old = SelectObject(mem, bmp);
      std::vector<std::vector<uint8_t>> parts;
      shrink_parts(mem, 64, 32,
                   {{{0, 0, 64, 32}, {8, 4}}, {{32, 0, 32, 32}, {4, 4}}, {{40, 0, 32, 32}, {4, 4}}, {{0, 0, 64, 32}, {}}},
                   collect(parts));
      CHECK((order == std::vector<size_t>{0, 1, 2, 3}) && parts.size() == 4 && parts[2].empty() && parts[3].empty());
      const std::string h0 = "P6\n8 4\n255\n", h1 = "P6\n4 4\n255\n";
      auto rgb = [](const std::vector<uint8_t>& p6, size_t head, int w, int x, int y) {
        const uint8_t* c = p6.data() + head + (size_t(y) * w + x) * 3;
        return std::array<int, 3>{c[0], c[1], c[2]};
      };
      auto red = [](std::array<int, 3> c) { return c[0] > 200 && c[1] < 50 && c[2] < 50; };
      auto blue = [](std::array<int, 3> c) { return c[0] < 50 && c[1] < 50 && c[2] > 200; };
      auto green = [](std::array<int, 3> c) { return c[0] < 50 && c[1] > 200 && c[2] < 50; };
      if (parts.size() == 4 && parts[0].size() == h0.size() + 8 * 4 * 3 && parts[1].size() == h1.size() + 4 * 4 * 3) {
        CHECK(memcmp(parts[0].data(), h0.data(), h0.size()) == 0 && memcmp(parts[1].data(), h1.data(), h1.size()) == 0);
        CHECK(red(rgb(parts[0], h0.size(), 8, 0, 0)) && red(rgb(parts[0], h0.size(), 8, 2, 3)));
        CHECK(blue(rgb(parts[0], h0.size(), 8, 7, 0)) && blue(rgb(parts[0], h0.size(), 8, 5, 3)));
        bool all_blue = true;
        for (int y = 0; y < 4; ++y)
          for (int x = 0; x < 4; ++x) all_blue &= blue(rgb(parts[1], h1.size(), 4, x, y));
        CHECK(all_blue);
      } else {
        CHECK(false);   // the sizes are wrong
      }
      // One picture at a time (App::capture_seeds writes each before the
      // next is made): the next is made only once the one before is taken,
      // so a picture turned green while the first is taken makes the second
      // green, where pictures made together would both be red and blue.
      std::vector<std::vector<uint8_t>> seq;
      shrink_parts(mem, 64, 32, {{{0, 0, 64, 32}, {8, 4}}, {{0, 0, 64, 32}, {8, 4}}},
                   [&](size_t shot, const std::vector<uint8_t>& p6) {
                     seq.push_back(p6);
                     if (shot == 0) paint(kGreen, kGreen);
                   });
      CHECK(seq.size() == 2);
      if (seq.size() == 2 && seq[0].size() == h0.size() + 8 * 4 * 3 && seq[1].size() == seq[0].size()) {
        CHECK(red(rgb(seq[0], h0.size(), 8, 0, 0)) && blue(rgb(seq[0], h0.size(), 8, 7, 3)));
        CHECK(green(rgb(seq[1], h0.size(), 8, 0, 0)) && green(rgb(seq[1], h0.size(), 8, 7, 3)));
      } else {
        CHECK(false);   // the sizes are wrong
      }
      SelectObject(mem, old);
      DeleteObject(bmp);
    }
    if (mem) DeleteDC(mem);
  }
  // One capture, several pictures: each its part of the monitor at its own
  // size (an Intermission frame's part beside the whole), and a part that
  // leaves the monitor none; a capture that can't be made takes every shot,
  // each without a picture (the saver logs each).
  {
    const int cw = mon.right, ch = mon.bottom;
    std::vector<std::vector<uint8_t>> shots;
    capture_monitor_shots(
        mon, {{{0, 0, cw, ch}, {64, 48}}, {{cw / 4, 0, cw / 2, ch}, {32, 48}}, {{cw / 2, 0, cw, ch}, {16, 16}}},
        collect(shots));
    CHECK((order == std::vector<size_t>{0, 1, 2}) && shots.size() == 3 && shots[2].empty());
    if (shots.size() == 3 && !shots[0].empty()) {
      const std::string h0 = "P6\n64 48\n255\n", h1 = "P6\n32 48\n255\n";
      CHECK(shots[0].size() == h0.size() + 64 * 48 * 3 && memcmp(shots[0].data(), h0.data(), h0.size()) == 0);
      CHECK(shots[1].size() == h1.size() + 32 * 48 * 3 && memcmp(shots[1].data(), h1.data(), h1.size()) == 0);
    }
    capture_monitor_shots(RECT{0, 0, 0, 0}, {{{0, 0, 1, 1}, {1, 1}}, {{0, 0, 1, 1}, {1, 1}}}, collect(shots));
    CHECK((order == std::vector<size_t>{0, 1}) && shots.size() == 2 && shots[0].empty() && shots[1].empty());
  }

  // The last-exit log: rewritten per run, capped, first and last lines kept.
  std::wstring dir = join_path(temp_dir(), L"adw-scr-unit-lastlog-" + std::to_wstring(GetCurrentProcessId()));
  std::wstring log = join_path(join_path(dir, L"logs"), L"saver-last.log");
  last_log_open(log);
  CHECK(file_exists(log));
  for (int i = 0; i < 500; ++i) last_log("line %d", i);
  CHECK(last_log_lines() == kLastLogMaxLines);
  std::string text;
  CHECK(read_file(log, text));
  CHECK(text.find(" line 0\r\n") != std::string::npos && text.find(" line 499\r\n") != std::string::npos);
  CHECK(text.find(" line 250\r\n") == std::string::npos && text.find("skipped") != std::string::npos);
  size_t lines = std::count(text.begin(), text.end(), '\n');
  CHECK(lines == kLastLogMaxLines);
  last_log_open(log);   // the next run starts over
  last_log("again");
  CHECK(read_file(log, text) && text.find("line 0") == std::string::npos && text.find("again") != std::string::npos);
  last_log_open(L"");   // closed: nothing more is written there
  DeleteFileW(log.c_str());
  RemoveDirectoryW(join_path(dir, L"logs").c_str());
  RemoveDirectoryW(dir.c_str());

  // Paths: state and logs follow settings.ini unless overridden.
  SetEnvironmentVariableW(L"AD_SETTINGS", L"C:\\scratch\\x\\settings.ini");
  SetEnvironmentVariableW(L"AD_SCR_STATE", nullptr);
  SetEnvironmentVariableW(L"AD_SCR_LASTLOG", nullptr);
  CHECK(state_dir() == L"C:\\scratch\\x\\state");
  CHECK(last_exit_log_path() == L"C:\\scratch\\x\\logs\\saver-last.log");
  SetEnvironmentVariableW(L"AD_SCR_STATE", L"D:\\st");
  CHECK(state_dir() == L"D:\\st");
  SetEnvironmentVariableW(L"AD_SCR_STATE", nullptr);
  SetEnvironmentVariableW(L"AD_SETTINGS", nullptr);

  // Settings: StartFromDesktop (no UI; default on, kept when written back).
  CHECK(parse_settings("").start_from_desktop);
  CHECK(!parse_settings("[Saver]\nStartFromDesktop=0\n").start_from_desktop);
  CHECK(parse_settings("[Saver]\nStartFromDesktop=1\n").start_from_desktop);
  Settings off;
  off.start_from_desktop = false;
  CHECK(!parse_settings(serialize_settings(off)).start_from_desktop);
  CHECK(serialize_settings(Settings{}).find("StartFromDesktop") == std::string::npos);
}

// ---- the data folder (paths.h app_data_root, host/core's data_root.h) -------------------
// Each case gives AD_LOCALAPPDATA a scratch base of its own and leaves every
// other location to the defaults (unless it says otherwise).

namespace fs = std::filesystem;

std::wstring lad_dir(const std::wstring& base) { return join_path(base, L"LongAfterDark"); }

// A data folder as an import and a save leave it: settings.ini choosing
// `module`, and an imported catalog.
void make_data_folder(const std::wstring& dir, const std::string& module) {
  CHECK(write_file_atomic(join_path(dir, L"settings.ini"), "[Saver]\r\nModule=" + module + "\r\n"));
  CHECK(write_file_atomic(join_path(join_path(join_path(dir, L"assets"), L"win"), L"catalog-win.json"),
                          R"({"version":1,"modules":[]})"));
}

void test_paths() {
  // What the cases change, put back at the end: the suites share this process.
  const wchar_t* const vars[] = {adw::kDataRootBaseVar, L"LOCALAPPDATA",  L"AD_SETTINGS",   L"AD_ASSETS_DIR",
                                 L"AD_SCR_THUMBS",      L"AD_SCR_STATE", L"AD_SCR_LASTLOG"};
  std::vector<std::pair<bool, std::wstring>> saved;
  for (const wchar_t* v : vars) saved.push_back({env_set(v), env_w(v)});
  const std::wstring root = join_path(temp_dir(), L"adw-scr-unit-paths-" + std::to_wstring(GetCurrentProcessId()));
  std::error_code ec;
  fs::remove_all(root, ec);
  auto base_for = [&](const char* name) {
    std::wstring b = join_path(join_path(root, widen(name)), L"lad");
    CHECK(ensure_dir(b));
    SetEnvironmentVariableW(adw::kDataRootBaseVar, b.c_str());
    for (const wchar_t* v : {L"AD_SETTINGS", L"AD_ASSETS_DIR", L"AD_SCR_THUMBS", L"AD_SCR_STATE", L"AD_SCR_LASTLOG"}) {
      SetEnvironmentVariableW(v, nullptr);
    }
    return b;
  };

  // A data folder there: every default location is in it, and what it holds is found.
  std::wstring data = lad_dir(base_for("found"));
  make_data_folder(data, "test.rings");
  CHECK(app_data_root() == data);
  CHECK(settings_path() == join_path(data, L"settings.ini"));
  Settings s;
  CHECK(load_settings(settings_path(), s) && s.module == "test.rings");
  CHECK(assets_root() == join_path(data, L"assets"));
  CHECK(catalog_path() == join_path(data, L"assets\\win\\catalog-win.json") && file_exists(catalog_path()));
  CHECK(state_dir() == join_path(data, L"state") && thumbs_dir() == join_path(data, L"thumbs"));
  CHECK(last_exit_log_path() == join_path(data, L"logs\\saver-last.log"));

  // Nothing there yet: the folder is named, and only made by a first save.
  data = lad_dir(base_for("fresh"));
  CHECK(settings_path() == join_path(data, L"settings.ini"));
  CHECK(!dir_exists(data));
  CHECK(save_settings(settings_path(), Settings{}));
  CHECK(dir_exists(data));

  // AD_SETTINGS and AD_ASSETS_DIR set (as every smoke test runs): nothing is
  // made in the data folder, whatever is used.
  data = lad_dir(base_for("lazy"));
  const std::wstring elsewhere = join_path(join_path(root, L"lazy"), L"elsewhere");
  SetEnvironmentVariableW(L"AD_SETTINGS", join_path(elsewhere, L"settings.ini").c_str());
  SetEnvironmentVariableW(L"AD_ASSETS_DIR", join_path(elsewhere, L"assets").c_str());
  CHECK(settings_path() == env_w(L"AD_SETTINGS"));
  CHECK(assets_root() == env_w(L"AD_ASSETS_DIR"));
  (void)win_assets_dir();
  (void)catalog_path();
  CHECK(write_file_atomic(settings_path(), "[Saver]\r\nModule=x\r\n"));
  CHECK(ensure_dir(thumbs_dir()) && ensure_dir(state_dir()));
  last_log_open(last_exit_log_path());
  last_log("hello");
  last_log_open(L"");
  CHECK(file_exists(join_path(elsewhere, L"logs\\saver-last.log")));
  CHECK(!dir_exists(data));

  // The base (names only; nothing is made): trimmed, a trailing separator
  // tolerated; a blank AD_LOCALAPPDATA gives way to LOCALAPPDATA, and with
  // neither set (the secure desktop's thin environment) the shell's known
  // folder answers.
  const std::wstring scratch = join_path(root, L"base");
  SetEnvironmentVariableW(adw::kDataRootBaseVar, (L" " + scratch + L"\\ ").c_str());
  CHECK(app_data_root() == lad_dir(scratch));
  SetEnvironmentVariableW(adw::kDataRootBaseVar, L" ");
  SetEnvironmentVariableW(L"LOCALAPPDATA", scratch.c_str());
  CHECK(app_data_root() == lad_dir(scratch));
  SetEnvironmentVariableW(adw::kDataRootBaseVar, nullptr);
  SetEnvironmentVariableW(L"LOCALAPPDATA", nullptr);
  PWSTR known = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &known))) {
    CHECK(app_data_root() == lad_dir(known));
    CoTaskMemFree(known);
  }

  for (size_t i = 0; i < saved.size(); ++i) {
    SetEnvironmentVariableW(vars[i], saved[i].first ? saved[i].second.c_str() : nullptr);
  }
  // This process (and so every test) runs on a scratch base, never the user's.
  CHECK(!env_w(L"AD_LOCALAPPDATA").empty() && env_w(L"AD_LOCALAPPDATA") != g_real_base);
  fs::remove_all(root, ec);
}

// ---- sound (AUDIO.md §9) --------------------------------------------------------------

// The environment as a map (case-insensitive names), for the spawn checks.
std::map<std::wstring, std::wstring> env_map(const EnvChanges& env) {
  std::map<std::wstring, std::wstring> m;
  for (const auto& [k, v] : env) {
    std::wstring key = k;
    for (auto& c : key) if (c >= L'a' && c <= L'z') c = (wchar_t)(c - 32);
    m[key] = v;   // the last one wins, as build_environment_block applies them
  }
  return m;
}

void test_sound() {
  // ---- settings: [Saver] Sound, Volume, SoundMonitor ----
  Settings d = parse_settings("");
  CHECK(d.sound && d.volume == 50 && d.sound_monitor == "primary");
  CHECK(kDefaultVolume == 50);
  // The fixture predates sound: the defaults, and OK writes them out.
  Settings f = parse_settings(fixture("settings.ini"));
  CHECK(f.sound && f.volume == 50 && f.sound_monitor == "primary");
  {
    std::string text = serialize_settings(f, fixture("settings.ini"));
    CHECK(text.find("Sound=1\r\n") != std::string::npos);
    CHECK(text.find("Volume=50\r\n") != std::string::npos);
    CHECK(text.find("SoundMonitor=primary\r\n") != std::string::npos);
    CHECK(text.find("FutureKey=keep me\r\n") != std::string::npos);   // unknown keys stay
    CHECK(text.find("[Extra]\r\n") != std::string::npos);
    CHECK(parse_settings(text) == f);
  }
  // Values as written or as a hand might write them.
  struct Case { const char* text; bool sound; int volume; const char* monitor; };
  const Case cases[] = {
      {"[Saver]\nSound=0\n", false, 50, "primary"},
      {"[Saver]\nSound=1\nVolume=0\n", true, 0, "primary"},
      {"[saver]\nsound = OFF \nvolume= 75 \n", false, 75, "primary"},
      {"[Saver]\nSound=on\nVolume=100\n", true, 100, "primary"},
      {"[Saver]\nSound=no\n", false, 50, "primary"},
      {"[Saver]\nSound=Yes\n", true, 50, "primary"},
      {"[Saver]\nSound=false\n", false, 50, "primary"},
      {"[Saver]\nSound=maybe\nVolume=loud\n", true, 50, "primary"},   // not answers: the defaults
      {"[Saver]\nSound=\nVolume=\nSoundMonitor=\n", true, 50, "primary"},
      {"[Saver]\nSound=2\nVolume=-5\n", true, 0, "primary"},             // any number but 0 is on; clamped
      {"[Saver]\nVolume=150\n", true, 100, "primary"},
      {"[Saver]\nVolume=075\n", true, 75, "primary"},
      {"[Saver]\nSoundMonitor=secondary\n", true, 50, "secondary"},      // reserved: kept as written
      {"[Saver]\nSound=0\nSound=1\n", true, 50, "primary"},             // the last one wins
  };
  for (const Case& c : cases) {
    Settings s = parse_settings(c.text);
    if (s.sound != c.sound || s.volume != c.volume || s.sound_monitor != c.monitor) {
      fprintf(stderr, "sound: \"%s\" gave Sound=%d Volume=%d SoundMonitor=%s\n", c.text, s.sound ? 1 : 0, s.volume,
              s.sound_monitor.c_str());
      CHECK(false);
    }
  }
  // Round trips, from scratch and on top of a file.
  for (bool on : {false, true}) {
    for (int v : {0, 1, 35, 50, 99, 100}) {
      Settings x;
      x.sound = on;
      x.volume = v;
      CHECK(parse_settings(serialize_settings(x)) == x);
      CHECK(parse_settings(serialize_settings(x, fixture("settings.ini"))) == x);
      std::string text = serialize_settings(x);
      CHECK(text.find(std::string("Sound=") + (on ? "1" : "0") + "\r\n") != std::string::npos);
      CHECK(text.find("Volume=" + std::to_string(v) + "\r\n") != std::string::npos);
    }
  }
  // A value that already says this is left as written; one that doesn't is replaced.
  {
    const std::string hand = "[Saver]\r\nSound=on\r\nVolume=075\r\nSoundMonitor=secondary\r\nOther=1\r\n";
    Settings h = parse_settings(hand);
    std::string same = serialize_settings(h, hand);
    CHECK(same.find("Sound=on\r\n") != std::string::npos && same.find("Volume=075\r\n") != std::string::npos);
    CHECK(same.find("SoundMonitor=secondary\r\n") != std::string::npos && same.find("Other=1\r\n") != std::string::npos);
    h.sound = false;
    h.volume = 20;
    std::string changed = serialize_settings(h, hand);
    CHECK(changed.find("Sound=0\r\n") != std::string::npos && changed.find("Sound=on") == std::string::npos);
    CHECK(changed.find("Volume=20\r\n") != std::string::npos && changed.find("Volume=075") == std::string::npos);
    CHECK(changed.find("SoundMonitor=secondary\r\n") != std::string::npos);
    CHECK(parse_settings(changed) == h);
    // Out of range in memory: written clamped.
    Settings loud;
    loud.volume = 250;
    CHECK(parse_settings(serialize_settings(loud)).volume == 100);
  }
  // The dialog's choices never touch sound (gather() sets it from its own controls).
  {
    Settings s = parse_settings("[Saver]\nModule=random\nSound=0\nVolume=12\nSoundMonitor=secondary\n");
    Settings r = apply_dialog_choice(s, {false, {"a"}, 4, "b"});
    CHECK(!r.sound && r.volume == 12 && r.sound_monitor == "secondary");
  }

  // ---- who gets sound ----
  Settings on;
  on.volume = 35;
  Settings off = on;
  off.sound = false;
  auto is_on = [](const SoundChoice& c, int volume) { return c.on && c.volume == volume; };
  // The primary monitor's window's host in /s: the only one.
  CHECK(is_on(sound_for(on, HostRole::saver, true, false), 35));
  // Everyone else is silent: the other monitors' hosts, /p, the dialog's
  // live preview and thumbnails, --configure / --capabilities.
  CHECK(!sound_for(on, HostRole::saver, false, false).on);
  for (HostRole r : {HostRole::control_panel, HostRole::live_preview, HostRole::thumbnail, HostRole::tool}) {
    for (bool owner : {false, true}) {
      if (sound_for(on, r, owner, false).on) {
        fprintf(stderr, "sound: role %s owner=%d plays\n", host_role_name(r), owner ? 1 : 0);
        CHECK(false);
      }
    }
  }
  // Sound=0, and AD_SCR_SOUND=0 whatever the settings say.
  CHECK(!sound_for(off, HostRole::saver, true, false).on);
  CHECK(!sound_for(on, HostRole::saver, true, true).on);
  // Volume is carried as set, clamped; SoundMonitor (reserved) is primary whatever it says.
  on.volume = 0;
  CHECK(is_on(sound_for(on, HostRole::saver, true, false), 0));
  on.volume = 180;
  CHECK(is_on(sound_for(on, HostRole::saver, true, false), 100));
  on.volume = 35;
  on.sound_monitor = "secondary";
  CHECK(is_on(sound_for(on, HostRole::saver, true, false), 35));
  CHECK(!sound_for(on, HostRole::saver, false, false).on);

  // ---- the spawn environment ----
  {
    auto m = env_map(sound_env(SoundChoice{true, 35}));
    CHECK(m[L"ADSOUND"] == L"1" && m[L"ADVOLUME"] == L"35");
    CHECK(!m.count(L"ADAUDIOOUT"));   // a capture the saver was given reaches the sound host
    m = env_map(sound_env(SoundChoice{}));
    CHECK(m.count(L"ADSOUND") && m[L"ADSOUND"] == L"0");
    // Removed (empty value), so nothing inherited turns sound on.
    CHECK(m.count(L"ADAUDIOOUT") && m[L"ADAUDIOOUT"].empty());
    CHECK(m.count(L"ADVOLUME") && m[L"ADVOLUME"].empty());
    // What the spec said about them before is replaced, not added to.
    EnvChanges env = {{L"ADSTREAM", L"1"}, {L"adsound", L"1"}, {L"ADAudioOut", L"x.wav"}};
    add_sound_env(env, SoundChoice{});
    int sounds = 0;
    for (auto& [k, v] : env) sounds += _wcsicmp(k.c_str(), L"ADSOUND") == 0;
    CHECK(sounds == 1);
    m = env_map(env);
    CHECK(m[L"ADSOUND"] == L"0" && m[L"ADAUDIOOUT"].empty() && m[L"ADSTREAM"] == L"1");
    // Every host is silent unless its spawn says ADSOUND (host_process.h).
    EnvChanges plain = {{L"ADSTREAM", L"1"}};
    add_host_defaults(plain);
    m = env_map(plain);
    CHECK(m[L"ADSOUND"] == L"0" && m.count(L"ADAUDIOOUT") && m[L"ADAUDIOOUT"].empty());
    EnvChanges owner = sound_env(SoundChoice{true, 80});
    add_host_defaults(owner);
    m = env_map(owner);
    CHECK(m[L"ADSOUND"] == L"1" && m[L"ADVOLUME"] == L"80" && !m.count(L"ADAUDIOOUT"));
    // Through the real block: an inherited capture or ADSOUND=1 never reaches a silent host.
    SetEnvironmentVariableW(L"ADAUDIOOUT", L"C:\\inherited.wav");
    SetEnvironmentVariableW(L"ADSOUND", L"1");
    SetEnvironmentVariableW(L"ADVOLUME", L"99");
    EnvChanges silent;
    add_host_defaults(silent);
    auto block = parse_block(build_environment_block(silent));
    bool out = false, snd = false, vol = false;
    for (auto& [k, v] : block) {
      if (_wcsicmp(k.c_str(), L"ADAUDIOOUT") == 0) out = true;
      if (_wcsicmp(k.c_str(), L"ADVOLUME") == 0) vol = true;
      if (_wcsicmp(k.c_str(), L"ADSOUND") == 0) snd = v == L"0";
    }
    CHECK(!out && !vol && snd);
    // ...while the sound host keeps the capture it was given.
    block = parse_block(build_environment_block(sound_env(SoundChoice{true, 40})));
    out = false;
    for (auto& [k, v] : block) {
      if (_wcsicmp(k.c_str(), L"ADAUDIOOUT") == 0) out = v == L"C:\\inherited.wav";
      if (_wcsicmp(k.c_str(), L"ADVOLUME") == 0) CHECK(v == L"40");
    }
    CHECK(out);
    SetEnvironmentVariableW(L"ADAUDIOOUT", nullptr);
    SetEnvironmentVariableW(L"ADSOUND", nullptr);
    SetEnvironmentVariableW(L"ADVOLUME", nullptr);
  }

  // ---- AD_SCR_SOUND ----
  {
    const std::wstring saved = env_w(kSoundOverrideEnv);   // the scr tests run with it set
    const bool had = env_set(kSoundOverrideEnv);
    struct { const wchar_t* v; bool off; } overrides[] = {
        {L"0", true}, {L" 0 ", true}, {L"off", true}, {L"No", true}, {L"FALSE", true},
        {L"1", false}, {L"on", false}, {L"", false}, {nullptr, false},
    };
    for (auto& c : overrides) {
      SetEnvironmentVariableW(kSoundOverrideEnv, c.v);
      if (sound_forced_off() != c.off) {
        fprintf(stderr, "sound: AD_SCR_SOUND=%ls\n", c.v ? c.v : L"(unset)");
        CHECK(false);
      }
    }
    SetEnvironmentVariableW(kSoundOverrideEnv, had ? saved.c_str() : nullptr);
  }
  // The stop graces (AUDIO.md §9 "Waking": at least 200 ms for the sound host).
  CHECK(kSoundHostStopGraceMs >= 200 && kSoundHostStopGraceMs > kHostStopGraceMs);
}

} // namespace

// ---- present (present.h) ------------------------------------------------------------------
// How a window draws a frame: the palette expansion, and the scaling filters
// of both ways (Direct2D on its software rasterizer, as off-screen captures
// draw; GDI's StretchDIBits), with the letterbox bars black.

// Grey level at (x, y) of a BGR picture w pixels wide (the blue byte).
int grey_at(const std::vector<uint8_t>& bgr, int w, int x, int y) { return bgr[((size_t)y * w + x) * 3]; }

void test_present() {
  // frame_to_bgrx: 8-bit through its palette (rows re-packed), 32-bit as it is.
  Frame f8;
  f8.width = 3;
  f8.height = 2;
  f8.bpp = 8;
  f8.stride = 4;
  f8.bits = {0, 1, 2, 9, 2, 1, 0, 9};
  f8.palette[0] = RGBQUAD{1, 2, 3, 0};
  f8.palette[1] = RGBQUAD{10, 20, 30, 0};
  f8.palette[2] = RGBQUAD{100, 110, 120, 0};
  std::vector<uint32_t> px;
  frame_to_bgrx(f8, px);
  CHECK(px.size() == 6);
  if (px.size() == 6) {
    CHECK(px[0] == 0xFF030201u && px[1] == 0xFF1E140Au && px[2] == 0xFF786E64u);
    CHECK(px[3] == px[2] && px[5] == px[0]);
  }
  Frame f32;
  f32.width = 2;
  f32.height = 2;
  f32.bpp = 32;
  f32.stride = 8;
  f32.bits = {1, 2, 3, 0, 4, 5, 6, 0, 7, 8, 9, 0, 10, 11, 12, 0};
  frame_to_bgrx(f32, px);
  CHECK(px.size() == 4 && px[0] == 0x00030201u && px[3] == 0x000C0B0Au);

  // Black | white, 2x1, drawn 4.5 times wider into a 17x4 picture at x=4
  // (a 856-wide frame on a 3840-wide monitor scales 4.5 times): pillar bars
  // at 0-3 and 13-16. Each pixel is a crisp block either way; the smooth
  // filter (HALFTONE's look for an upscale, done on the GPU) blends only the
  // one column where the blocks meet, 4.5 columns in, by how much of each it
  // covers, where nearest makes one block 4 columns and the other 5.
  Frame bw;
  bw.width = 2;
  bw.height = 1;
  bw.bpp = 8;
  bw.stride = 4;
  bw.bits = {0, 1, 0, 0};
  bw.palette[0] = RGBQUAD{0, 0, 0, 0};
  bw.palette[1] = RGBQUAD{255, 255, 255, 0};
  const int W = 17, H = 4;
  const RectI fit{4, 0, 9, 4};
  for (bool d2d : {true, false}) {
    for (Filter filter : {Filter::nearest, Filter::smooth}) {
      std::vector<uint8_t> bgr;
      std::string err;
      const char* name = d2d ? (filter == Filter::smooth ? "d2d smooth" : "d2d nearest")
                             : (filter == Filter::smooth ? "gdi smooth" : "gdi nearest");
      const bool ok = render_frame_bgr(bw, W, H, fit, d2d, filter, bgr, &err);
      if (!ok) fprintf(stderr, "present: %s: %s\n", name, err.c_str());
      CHECK(ok && bgr.size() == (size_t)W * H * 3);
      if (!ok || bgr.size() != (size_t)W * H * 3) continue;
      const int y = H / 2;
      std::string row;
      int between = 0;
      for (int x = 0; x < W; ++x) {
        const int v = grey_at(bgr, W, x, y);
        row += " " + std::to_string(v);
        if (x >= 4 && x < 13 && v > 16 && v < 239) ++between;
      }
      for (int x : {0, 3, 13, 16}) CHECK(grey_at(bgr, W, x, y) == 0);   // the bars
      for (int x : {4, 5, 6, 7}) CHECK(grey_at(bgr, W, x, y) == 0);      // black block
      for (int x : {9, 10, 11, 12}) CHECK(grey_at(bgr, W, x, y) == 255); // white block
      if (filter == Filter::nearest) {
        CHECK(between == 0);
      } else if (d2d) {
        CHECK(between == 1 && std::abs(grey_at(bgr, W, 8, y) - 128) <= 40);   // half and half
      } else {
        CHECK(between <= 1);
      }
      if (g_failures) fprintf(stderr, "present: %s row:%s\n", name, row.c_str());
    }
  }
  // At an exact multiple there is nothing to blend: smooth is nearest.
  for (bool d2d : {true, false}) {
    std::vector<uint8_t> bgr;
    CHECK(render_frame_bgr(bw, 8, 2, RectI{0, 0, 8, 2}, d2d, Filter::smooth, bgr));
    if (bgr.size() == 8 * 2 * 3) CHECK(grey_at(bgr, 8, 3, 1) == 0 && grey_at(bgr, 8, 4, 1) == 255);
  }
  // At 1:1 (the capture's host picture) both ways give the exact colours.
  for (bool d2d : {true, false}) {
    std::vector<uint8_t> bgr;
    CHECK(render_frame_bgr(f8, 3, 2, RectI{0, 0, 3, 2}, d2d, Filter::nearest, bgr));
    if (bgr.size() == 18) {
      CHECK(bgr[0] == 1 && bgr[1] == 2 && bgr[2] == 3);
      CHECK(bgr[6] == 100 && bgr[7] == 110 && bgr[8] == 120);
      CHECK(bgr[15] == 1 && bgr[16] == 2 && bgr[17] == 3);
    }
  }
  // Letterbox top and bottom: a 4:3 frame in a 16:9 picture.
  Frame solid;
  solid.width = 4;
  solid.height = 3;
  solid.bpp = 8;
  solid.stride = 4;
  solid.bits.assign(12, 7);
  solid.palette[7] = RGBQUAD{200, 100, 50, 0};
  const RectI box = fit_rect(4, 3, 64, 27);   // pillarboxed: 36x27 at x=14
  std::vector<uint8_t> bgr;
  CHECK(render_frame_bgr(solid, 64, 27, box, true, Filter::smooth, bgr));
  if (bgr.size() == (size_t)64 * 27 * 3) {
    const uint8_t* mid = &bgr[((size_t)13 * 64 + 32) * 3];
    CHECK(mid[0] == 200 && mid[1] == 100 && mid[2] == 50);
    CHECK(bgr[((size_t)13 * 64 + 2) * 3] == 0 && bgr[((size_t)13 * 64 + 61) * 3 + 2] == 0);
  }
  // Nothing to draw: refused, not a crash.
  CHECK(!render_frame_bgr(Frame{}, 8, 8, RectI{0, 0, 8, 8}, true, Filter::smooth, bgr));
  CHECK(!render_frame_bgr(solid, 0, 8, RectI{0, 0, 8, 8}, false, Filter::smooth, bgr));
}

int main(int argc, char** argv) {
  // Never the user's data folder: this process and every one it starts
  // resolve %LOCALAPPDATA% to a scratch base (data_root.h AD_LOCALAPPDATA),
  // noting the real one for the read-only look at the installed catalog.
  g_real_base = adw::data_root_base();
  SetEnvironmentVariableW(adw::kDataRootBaseVar,
                          join_path(temp_dir(), L"adw-scr-unit-lad-" + std::to_wstring(GetCurrentProcessId())).c_str());
  std::map<std::string, std::function<void()>> suites = {
      {"args", test_args},         {"parser", test_parser},   {"settings", test_settings},
      {"catalog", test_catalog},   {"geometry", test_geometry}, {"rotation", test_rotation},
      {"convert", test_convert},   {"env", test_env},         {"layout", test_layout},
      {"dialog", test_dialog},     {"ui", test_ui},           {"input", test_input},
      {"seed", test_seed},         {"releases", test_releases}, {"paths", test_paths},
      {"sound", test_sound},       {"present", test_present},
  };
  std::vector<std::string> run;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--fakeimport") == 0 && i + 1 < argc) g_fakeimport = widen(argv[++i]);
    else if (strcmp(argv[i], "--fakehost") == 0 && i + 1 < argc) g_fakehost = widen(argv[++i]);
    else run.push_back(argv[i]);
  }
  if (run.empty()) for (auto& [k, v] : suites) run.push_back(k);
  for (const auto& name : run) {
    auto it = suites.find(name);
    if (it == suites.end()) {
      fprintf(stderr, "unknown suite %s\n", name.c_str());
      return 2;
    }
    int before = g_failures;
    it->second();
    printf("%s: %s\n", name.c_str(), g_failures == before ? "ok" : "FAILED");
  }
  return g_failures ? 1 : 0;
}
