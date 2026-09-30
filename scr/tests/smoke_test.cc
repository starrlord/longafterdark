// End-to-end tests: the real LongAfterDark.scr against fakehost.exe and a
// fixture asset tree (placeholder module files — never After Dark bytes).
//
//   scr_smoke <test> --scr <LongAfterDark.scr> --fakehost <fakehost.exe>
//                    --fakeimport <fakeimport.exe> --fixtures <dir> --work <dir>
//
// Each test gets <work>/<test>/ with assets/, settings.ini, scr.log (the
// saver's AD_SCR_LOG) and fakehost.log (FAKEHOST_LOG: one "start" line per
// host with the argv/env the saver passed). The saver's test hook
// AD_SCR_TESTEXIT_AFTER_FRAMES=N makes it exit 0 once every host window has
// presented N frames; AD_SCR_TEST_IGNORE_INPUT keeps a stray mouse from
// ending a run early. GUI tests exit 77 (SKIP) without an input desktop or
// with AD_SCR_SKIP_GUI_TESTS=1. No process started here ever sees the user's
// data folder: AD_LOCALAPPDATA points every one of them at a scratch base.
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <uiautomation.h>
#include <wincodec.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <phosg/JSON.hh>

#include "adw/core/data_root.h"
#include "adw/ui/capture.h"
#include "catalog.h"
#include "dialog_support.h"
#include "geometry.h"
#include "host_process.h"
#include "log.h"
#include "paths.h"
#include "releases.h"
#include "resource.h"
#include "settings.h"
#include "sound.h"
#include "ui_model.h"

namespace fs = std::filesystem;
using namespace adw::scr;
using namespace std::chrono_literals;

namespace {

constexpr int kSkip = 77;

struct Opts {
  // scr: the program the tests run (LongAfterDark-test.scr, with the test
  // levers); shipped: the LongAfterDark.scr that is packaged (`resources`).
  std::wstring scr, fakehost, fakeimport, fixtures, work, shipped;
};

int g_failures = 0;

void failf(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "FAIL: ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
  ++g_failures;
}

#define CHECK(cond) \
  do { if (!(cond)) failf("%s:%d: %s", __FILE__, __LINE__, #cond); } while (0)

// ---- fixture tree ----------------------------------------------------------------

struct Work {
  fs::path dir, assets, settings, scr_log, host_log;
};

Work prepare(const Opts& o, const std::string& name, bool with_assets = true) {
  Work w;
  w.dir = fs::path(o.work) / name;
  std::error_code ec;
  fs::remove_all(w.dir, ec);
  fs::create_directories(w.dir);
  w.assets = w.dir / "assets";
  fs::create_directories(w.assets);
  if (with_assets) {
    fs::path win = w.assets / "win";
    fs::create_directories(win);
    fs::copy_file(fs::path(o.fixtures) / "catalog-win.json", win / "catalog-win.json");
    Catalog c;
    load_catalog((win / "catalog-win.json").wstring(), c, nullptr);
    for (const auto& m : c.modules) {
      if (m.id == "test.absent") continue;   // exercises "catalog lists it, file missing"
      fs::path p = resolve_module_path(win.wstring(), m.path);
      fs::create_directories(p.parent_path());
      write_file_atomic(p.wstring(), "placeholder module file for the LongAfterDark.scr smoke tests\n");
    }
  }
  w.settings = w.dir / "settings.ini";
  fs::copy_file(fs::path(o.fixtures) / "settings.ini", w.settings);
  w.scr_log = w.dir / "scr.log";
  w.host_log = w.dir / "fakehost.log";
  return w;
}

void edit_settings(const Work& w, const std::function<void(Settings&)>& f) {
  Settings s;
  load_settings(w.settings.wstring(), s);
  f(s);
  save_settings(w.settings.wstring(), s);
}

using EnvList = std::vector<std::pair<std::wstring, std::wstring>>;

EnvList base_env(const Opts& o, const Work& w) {
  return {
      {L"AD_HOST_EXE", o.fakehost},
      // Whatever falls back to the defaults finds a scratch %LOCALAPPDATA%.
      {adw::kDataRootBaseVar, (w.dir / "localappdata").wstring()},
      {L"AD_ASSETS_DIR", w.assets.wstring()},
      {L"AD_SETTINGS", w.settings.wstring()},
      {L"AD_SCR_LOG", w.scr_log.wstring()},
      {L"AD_SCR_HOSTLOG", (w.dir / "host-stderr.log").wstring()},
      {L"FAKEHOST_LOG", w.host_log.wstring()},
      {L"AD_SCR_TEST_IGNORE_INPUT", L"1"},
      // Never inherit these from whoever runs ctest.
      {L"FAKEHOST_EXIT_AFTER", L""}, {L"FAKEHOST_STALL_AFTER", L""}, {L"FAKEHOST_FORMAT", L""},
      {L"FAKEHOST_FAIL_START", L""}, {L"FAKEHOST_CORRUPT_EVERY", L""}, {L"FAKEHOST_IGNORE_QUIT", L""},
      {L"FAKEHOST_GARBAGE", L""}, {L"AD_SCR_TESTEXIT_AFTER_FRAMES", L""}, {L"AD_SCR_TEST_ROTATE_MS", L""},
      {L"AD_SCR_TEST_STALL_MS", L""}, {L"AD_SCR_TEST_DISPLAY_OFF_MS", L""}, {L"ADFRAMES", L""}, {L"ADCVSET", L""},
      {L"AD_SCR_TEST_MONITORS", L""}, {L"AD_SCR_TEST_SEED", L""}, {kPreviewSettingsEnv, L""}, {L"AD_IMPORT_EXE", o.fakeimport},
      {L"FAKEIMPORT_LOG", (w.dir / "fakeimport.log").wstring()}, {L"FAKEIMPORT_EXIT", L""},
      {L"FAKEIMPORT_CATALOG", L""}, {L"FAKEIMPORT_WAIT_MS", L""}, {L"FAKEHOST_LANES", L""},
      {L"FAKEHOST_ABIS", L""}, {L"FAKEHOST_EXIT3_MODULE", L""}, {L"FAKEHOST_CAPS_DELAY_MS", L""},
      {L"FAKEHOST_NUMLOCK", L""}, {L"ADNUMLOCK", L""},
      // Interaction (INTERACTION.md): fakehost's levers, and the saver's.
      {L"FAKEHOST_CONFIGURE", L""}, {L"FAKEHOST_CONFIGURE_MS", L""}, {L"FAKEHOST_CONFIGURE_EXIT", L""},
      {L"FAKEHOST_CONFIGURE_EXIT_FILE", L""}, {L"FAKEHOST_INTERACTIVE", L""}, {L"FAKEHOST_CURSOR", L""},
      {L"FAKEHOST_ROTATE_OK", L""}, {L"FAKEHOST_KEYFILTER", L""}, {L"FAKEHOST_EAT_VKS", L""},
      {L"FAKEHOST_WAKE_AFTER", L""}, {L"AD_SCR_TEST_INPUT", L""}, {L"AD_SCR_STATE", L""}, {L"AD_SCR_LASTLOG", L""},
      {L"ADSTATE", L""}, {L"ADCAPS", L""}, {L"ADSEEDIMG", L""}, {L"ADSTATUSHANDLE", L""},
      // The dialog's background thumbnails start hosts of their own; only
      // the tests about them (config-thumbs, config-classic) turn them on.
      {L"AD_SCR_THUMBGEN", L"0"}, {L"AD_SCR_THUMBS", L""},
      // Sound (AUDIO.md §9): the CTest environment's AD_SCR_SOUND=0 is kept
      // (the tests about sound take it out), and nothing else about sound is
      // inherited.
      {L"ADSOUND", L""}, {L"ADVOLUME", L""}, {L"ADAUDIOOUT", L""}, {L"ADAUDIOLIVE", L""},
      {L"FAKEHOST_QUIT_DELAY_MS", L""},
      // The dialog's credit link: the test build never opens it, only logs it.
      {L"AD_SCR_TEST_OPEN_LOG", L""},
  };
}

// ---- logs ------------------------------------------------------------------------

std::vector<std::string> lines_of(const fs::path& p) {
  std::string text;
  read_file(p.wstring(), text);
  std::vector<std::string> out;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t nl = text.find('\n', pos);
    std::string l = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    if (!l.empty() && l.back() == '\r') l.pop_back();
    out.push_back(l);
    if (nl == std::string::npos) break;
    pos = nl + 1;
  }
  return out;
}

// fakehost lines: "start\tkey=value\t..."
std::vector<std::map<std::string, std::string>> host_events(const Work& w, const char* kind) {
  std::vector<std::map<std::string, std::string>> r;
  for (const auto& l : lines_of(w.host_log)) {
    size_t tab = l.find('\t');
    if (l.substr(0, tab) != kind) continue;
    std::map<std::string, std::string> kv;
    while (tab != std::string::npos) {
      size_t next = l.find('\t', tab + 1);
      std::string f = l.substr(tab + 1, next == std::string::npos ? std::string::npos : next - tab - 1);
      size_t eq = f.find('=');
      if (eq != std::string::npos) kv[f.substr(0, eq)] = f.substr(eq + 1);
      tab = next;
    }
    r.push_back(kv);
  }
  return r;
}

// The hosts `parent` started (the settings dialog runs its own: the live
// preview and the lane probe), or with `exclude` every other process's.
std::vector<std::map<std::string, std::string>> hosts_of(const Work& w, DWORD parent, bool exclude = false) {
  std::vector<std::map<std::string, std::string>> r;
  for (auto& e : host_events(w, "start")) {
    bool mine = strtoul(e["ppid"].c_str(), nullptr, 10) == parent;
    if (mine != exclude) r.push_back(e);
  }
  return r;
}

int count_in_log(const fs::path& p, const std::string& needle) {
  int n = 0;
  for (const auto& l : lines_of(p)) if (l.find(needle) != std::string::npos) ++n;
  return n;
}

bool ends_with(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Every host the saver started must be gone once it has exited (the Job).
// A long test starts many processes, and Windows reuses a process id as soon
// as its last handle closes: a pid that now belongs to something other than a
// host is not a host that outlived the saver.
void check_hosts_gone(const Work& w) {
  for (auto& ev : host_events(w, "start")) {
    DWORD pid = (DWORD)strtoul(ev["pid"].c_str(), nullptr, 10);
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) continue;
    wchar_t image[MAX_PATH];
    DWORD len = MAX_PATH;
    if (QueryFullProcessImageNameW(h, 0, image, &len)) {
      std::wstring name = fs::path(std::wstring(image, len)).filename().wstring();
      if (_wcsicmp(name.c_str(), L"fakehost.exe") != 0 && _wcsicmp(name.c_str(), L"adhostwin.exe") != 0) {
        CloseHandle(h);
        continue;
      }
    }
    bool gone = WaitForSingleObject(h, 5000) == WAIT_OBJECT_0;
    CloseHandle(h);
    if (!gone) failf("host pid %lu outlived the saver", pid);
  }
}

void dump_logs(const Work& w) {
  for (const fs::path& p : {w.scr_log, w.host_log}) {
    fprintf(stderr, "---- %s\n", p.string().c_str());
    for (const auto& l : lines_of(p)) fprintf(stderr, "  %s\n", l.c_str());
  }
}

// C8 (INTERACTION.md §7.1): every host carried ADSTATE = <settings dir>\state.
void check_state_everywhere(const Work& w);

// ---- running the saver -----------------------------------------------------------

struct RunResult {
  bool started = false, timed_out = false;
  DWORD code = 0;
};

void pump() {
  MSG m;
  while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&m);
    DispatchMessageW(&m);
  }
}

// Runs LongAfterDark.scr and pumps our message queue while waiting: /p makes the
// saver's window a child of ours, and cross-process child creation sends
// messages to this thread.
RunResult run_scr(const Opts& o, const std::wstring& args, const EnvList& env, DWORD timeout_ms,
                  const std::function<void(DWORD pid)>& during = nullptr) {
  RunResult r;
  std::wstring cmd = quote_arg(o.scr) + L" " + args;
  std::wstring block = build_environment_block(env);
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(o.scr.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT, block.data(),
                      nullptr, &si, &pi)) {
    failf("CreateProcess(%ls) failed: %lu", o.scr.c_str(), GetLastError());
    return r;
  }
  r.started = true;
  CloseHandle(pi.hThread);
  ULONGLONG deadline = GetTickCount64() + timeout_ms;
  for (;;) {
    DWORD wr = MsgWaitForMultipleObjects(1, &pi.hProcess, FALSE, 50, QS_ALLINPUT);
    pump();
    if (wr == WAIT_OBJECT_0) break;
    if (during) during(pi.dwProcessId);
    if (GetTickCount64() > deadline) {
      r.timed_out = true;
      TerminateProcess(pi.hProcess, 99);
      WaitForSingleObject(pi.hProcess, 5000);
      break;
    }
  }
  GetExitCodeProcess(pi.hProcess, &r.code);
  CloseHandle(pi.hProcess);
  return r;
}

bool expect_exit(const Work& w, const RunResult& r, DWORD code) {
  if (!r.started) return false;
  if (r.timed_out) {
    failf("LongAfterDark.scr did not exit in time");
    dump_logs(w);
    return false;
  }
  if (r.code != code) {
    failf("LongAfterDark.scr exit code %lu, expected %lu", r.code, code);
    dump_logs(w);
    return false;
  }
  return true;
}

bool gui_available() {
  if (env_set(L"AD_SCR_SKIP_GUI_TESTS")) return false;
  // A disconnected session has no input desktop, yet its windows still work
  // for the tests that drive them by message (and the off-screen renders):
  // AD_SCR_FORCE_GUI_TESTS=1 runs them anyway, at the caller's risk.
  if (env_set(L"AD_SCR_FORCE_GUI_TESTS")) return true;
  HDESK d = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
  if (!d) return false;   // locked workstation / service session
  CloseDesktop(d);
  return true;
}

HWND make_parent(int cw, int ch) {
  static bool registered = false;
  if (!registered) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"AdScrSmokePreviewParent";
    RegisterClassW(&wc);
    registered = true;
  }
  RECT rc{0, 0, cw, ch};
  AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
  // Never shown: the test must not depend on (or disturb) what is on screen.
  return CreateWindowW(L"AdScrSmokePreviewParent", L"preview parent", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, 0, 0,
                       rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
}

std::wstring hwnd_arg(HWND h) { return std::to_wstring((unsigned long long)(uintptr_t)h); }

// ---- tests -------------------------------------------------------------------------

// HostProcess + Pacer + parser against fakehost, no windows involved.
int test_host_stream(const Opts& o) {
  Work w = prepare(o, "host-stream");
  HWND sink = CreateWindowW(L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
  Pacer pacer;
  pacer.start();
  auto spawn = [&](HostProcess& h, EnvList extra) {
    HostSpec spec;
    spec.exe = o.fakehost;
    spec.module_path = (w.dir / "fake module.AD").wstring();   // a space, to exercise quoting
    spec.env = {{L"ADSTREAM", L"1"}, {L"ADSCREENW", L"64"}, {L"ADSCREENH", L"48"},
                {L"FAKEHOST_LOG", w.host_log.wstring()}, {L"FAKEHOST_EXIT_AFTER", L""},
                {L"FAKEHOST_FORMAT", L""}, {L"FAKEHOST_GARBAGE", L""}, {L"FAKEHOST_STALL_AFTER", L""},
                {L"FAKEHOST_CORRUPT_EVERY", L""}};
    for (auto& e : extra) spec.env.push_back(e);
    std::wstring err;
    bool ok = h.start(spec, nullptr, &err);
    if (!ok) failf("start: %ls", err.c_str());
    return ok;
  };
  // Plays the UI's part: take (and recycle) frames as they arrive, since
  // the host only gets its next GO once the last frame has been taken.
  HostProcess* current = nullptr;
  std::unique_ptr<Frame> last;
  auto wait_for = [&](const std::function<bool()>& cond, int ms) {
    ULONGLONG end = GetTickCount64() + ms;
    while (!cond() && GetTickCount64() < end) {
      pump();
      if (current) {
        if (auto f = current->take_frame()) {
          current->recycle(std::move(last));
          last = std::move(f);
        }
      }
      Sleep(5);
    }
    return cond();
  };

  {  // P8 lockstep
    HostProcess h({sink, WM_APP + 1, WM_APP + 2, 1});
    current = &h;
    if (spawn(h, {})) {
      pacer.add(&h);
      CHECK(wait_for([&] { return h.frames() >= 30; }, 15000));
      auto f = std::move(last);
      CHECK(f && f->width == 64 && f->height == 48 && f->bpp == 8 && f->stride == 64 && f->bits.size() == 64 * 48);
      if (f) CHECK(f->palette[0].rgbRed == 0 && f->palette[0].rgbGreen == 0 && f->palette[0].rgbBlue == 0);
      // Lockstep: the fake only renders on GO, and we never have two in flight.
      CHECK(h.frames() <= h.gos_sent() + 1);
      CHECK(h.gos_sent() <= h.frames() + 1);
      h.send_line("SET 0 9");
      pacer.remove(&h);
      h.stop();
      auto exits = host_events(w, "exit");
      CHECK(!exits.empty() && exits.back()["reason"] == "quit");
      auto starts = host_events(w, "start");
      CHECK(!starts.empty() && ends_with(starts.back()["module"], "fake module.AD"));
    }
  }
  {  // P6 with junk between frames: the parser resynchronises
    HostProcess h({sink, WM_APP + 1, WM_APP + 2, 2});
    current = &h;
    last.reset();
    if (spawn(h, {{L"FAKEHOST_FORMAT", L"P6"}, {L"FAKEHOST_GARBAGE", L"1"}})) {
      pacer.add(&h);
      CHECK(wait_for([&] { return h.frames() >= 10; }, 15000));
      auto f = std::move(last);
      CHECK(f && f->bpp == 32 && f->stride == 256 && f->bits.size() == 64 * 48 * 4);
      CHECK(h.resyncs() >= 5);
      pacer.remove(&h);
      h.stop();
    }
  }
  {  // a frame lost whole to the parser: its GO is re-sent and the lockstep
     // carries on (the lost answer must not stay counted as outstanding)
    HostProcess h({sink, WM_APP + 1, WM_APP + 2, 5});
    current = &h;
    last.reset();
    if (spawn(h, {{L"FAKEHOST_CORRUPT_EVERY", L"4"}})) {
      pacer.add(&h);
      // Every 4th frame is spoiled; each costs one ~1 s GO retry, no more.
      // (Before the write-off the count stuck at 1 after the first loss and
      // the lockstep wedged until the stall watchdog.)
      CHECK(wait_for([&] { return h.frames() >= 10; }, 10000));
      CHECK(h.resyncs() >= 2);
      CHECK(h.gos_sent() <= h.frames() + 4);   // retries, not a GO storm
      pacer.remove(&h);
      h.stop();
      auto exits = host_events(w, "exit");
      CHECK(!exits.empty() && exits.back()["reason"] == "quit");
    }
  }
  {  // the sound host's longer grace (AUDIO.md §9): a host that takes 220 ms
     // after QUIT to silence its device ends on its own with ADSOUND=1, and is
     // terminated (no exit line) without
    for (bool sound : {true, false}) {
      HostProcess h({sink, WM_APP + 1, WM_APP + 2, sound ? 6u : 7u});
      current = &h;
      last.reset();
      const size_t exits_before = host_events(w, "exit").size();
      if (spawn(h, {{L"ADSOUND", sound ? L"1" : L""}, {L"FAKEHOST_QUIT_DELAY_MS", L"220"}})) {
        CHECK(h.sound() == sound);
        pacer.add(&h);
        CHECK(wait_for([&] { return h.frames() >= 5; }, 15000));
        pacer.remove(&h);
        const ULONGLONG t0 = GetTickCount64();
        h.stop_gracefully();
        const ULONGLONG took = GetTickCount64() - t0;
        auto exits = host_events(w, "exit");
        if (sound) {
          CHECK(exits.size() == exits_before + 1 && exits.back()["reason"] == "quit");
          CHECK(took >= 200);
        } else {
          CHECK(exits.size() == exits_before);   // terminated at 150 ms, before its exit line
          CHECK(took < 1000);
        }
        auto starts = host_events(w, "start");
        CHECK(!starts.empty() && starts.back()["ADSOUND"] == (sound ? "1" : "0"));
        if (!sound) CHECK(starts.back()["ADAUDIOOUT"].empty());
      }
    }
  }
  {  // backpressure: a frame nobody takes stops the GOs
    HostProcess h({sink, WM_APP + 1, WM_APP + 2, 4});
    current = nullptr;
    if (spawn(h, {})) {
      pacer.add(&h);
      CHECK(wait_for([&] { return h.frames() >= 1; }, 15000));
      Sleep(500);
      CHECK(h.frames() == 1 && h.gos_sent() == 1);
      h.take_frame();
      CHECK(wait_for([&] { return h.frames() >= 2; }, 5000));
      pacer.remove(&h);
      h.stop();
    }
  }
  {  // a host that dies: exited() and the exit notification
    HostProcess h({sink, WM_APP + 1, WM_APP + 2, 3});
    current = &h;
    last.reset();
    if (spawn(h, {{L"FAKEHOST_EXIT_AFTER", L"5"}})) {
      pacer.add(&h);
      CHECK(wait_for([&] { return h.exited(); }, 15000));
      CHECK(h.frames() == 5);
      pacer.remove(&h);
      h.stop();
      auto exits = host_events(w, "exit");
      CHECK(!exits.empty() && exits.back()["reason"] == "exit-after");
    }
  }
  current = nullptr;
  last.reset();
  pacer.stop();
  DestroyWindow(sink);
  check_hosts_gone(w);
  return 0;
}

std::vector<RECT> monitor_rects(bool primary_only) {
  std::vector<RECT> v;
  struct Ctx { std::vector<RECT>* v; bool primary_only; } ctx{&v, primary_only};
  EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT, LPARAM lp) -> BOOL {
    auto* c = reinterpret_cast<Ctx*>(lp);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(m, &mi) && (!c->primary_only || (mi.dwFlags & MONITORINFOF_PRIMARY))) c->v->push_back(mi.rcMonitor);
    return TRUE;
  }, reinterpret_cast<LPARAM>(&ctx));
  return v;
}

int test_run(const Opts& o) {
  Work w = prepare(o, "run");
  EnvList env = base_env(o, w);
  env.push_back({L"AD_SCR_TESTEXIT_AFTER_FRAMES", L"20"});
  RunResult r = run_scr(o, L"/s", env, 60000);
  if (!expect_exit(w, r, 0)) return 1;
  auto starts = host_events(w, "start");
  auto mons = monitor_rects(false);
  CHECK(starts.size() >= mons.size());
  std::multiset<int> want, got;
  for (const RECT& m : mons) {
    want.insert(emulated_screen_size((double)(m.right - m.left) / (m.bottom - m.top), 1.0).w);
  }
  for (auto& s : starts) {
    CHECK(ends_with(s["module"], "TESTRING.AD"));
    CHECK(s["ADSTREAM"] == "1");
    CHECK(s["ADSCREENH"] == "480");
    CHECK(s["ADCVSET"] == "0=75,1=0,2=2");
    CHECK(s["AD_ASSETS_DIR"] == w.assets.string());
    got.insert(atoi(s["ADSCREENW"].c_str()));
  }
  if (starts.size() == mons.size() && got != want) {
    failf("emulated widths don't match the monitors' aspect");
    for (int x : want) fprintf(stderr, "  want %d\n", x);
    for (int x : got) fprintf(stderr, "  got %d\n", x);
  }
  CHECK(count_in_log(w.scr_log, "test-exit after 20 frames") == 1);
  // /s draws through Direct2D unless told otherwise (present.h).
  CHECK(count_in_log(w.scr_log, "present window=0: direct2d") == 1);
  // The opening GO goes out with the spawn, well inside the host's 250 ms
  // ADGOWAITMS window (DESIGN.md §1), so frame 0 already answers a GO.
  auto exits = host_events(w, "exit");
  CHECK(!exits.empty());
  for (auto& e : exits) {
    long long ms = atoll(e["first_go_ms"].c_str());
    if (e["first_go_ms"].empty() || ms < 0 || ms >= 250) failf("first GO after %s ms", e["first_go_ms"].c_str());
  }
  check_state_everywhere(w);
  check_hosts_gone(w);
  if (g_failures) dump_logs(w);
  return 0;
}

int test_primary_only(const Opts& o) {
  Work w = prepare(o, "primary-only");
  edit_settings(w, [](Settings& s) { s.all_monitors = false; });
  EnvList env = base_env(o, w);
  env.push_back({L"AD_SCR_TESTEXIT_AFTER_FRAMES", L"10"});
  RunResult r = run_scr(o, L"-S", env, 60000);
  if (!expect_exit(w, r, 0)) return 1;
  auto starts = host_events(w, "start");
  CHECK(starts.size() == 1);
  if (!starts.empty()) {
    RECT m = monitor_rects(true).at(0);
    int want = emulated_screen_size((double)(m.right - m.left) / (m.bottom - m.top), 1.0).w;
    CHECK(atoi(starts[0]["ADSCREENW"].c_str()) == want);
  }
  check_hosts_gone(w);
  return 0;
}

int test_preview(const Opts& o) {
  Work w = prepare(o, "preview");
  edit_settings(w, [](Settings& s) { s.scale = 1.5; });   // previews ignore Scale: always 320x240
  HWND parent = make_parent(152, 112);
  EnvList env = base_env(o, w);
  env.push_back({L"AD_SCR_TESTEXIT_AFTER_FRAMES", L"10"});
  bool saw_child = false;
  RECT prc{};
  GetClientRect(parent, &prc);   // not exactly 152x112 once DPI scales the frame
  RunResult r = run_scr(o, L"/p " + hwnd_arg(parent), env, 60000, [&](DWORD) {
    HWND c = GetWindow(parent, GW_CHILD);
    if (c) {
      RECT rc{};
      GetClientRect(c, &rc);
      // The preview fills the parent's client area.
      if (rc.right == prc.right && rc.bottom == prc.bottom) saw_child = true;
    }
  });
  DestroyWindow(parent);
  if (!expect_exit(w, r, 0)) return 1;
  CHECK(saw_child);
  auto starts = host_events(w, "start");
  CHECK(starts.size() == 1);
  for (auto& s : starts) CHECK(s["ADSCREENW"] == "320" && s["ADSCREENH"] == "240");
  check_state_everywhere(w);
  check_hosts_gone(w);
  return 0;
}

int test_preview_parent_gone(const Opts& o) {
  Work w = prepare(o, "preview-parent-gone");
  // A DPI-unaware parent, like a legacy control panel, exercises the saver's
  // adopt-the-parent's-awareness path.
  DPI_AWARENESS_CONTEXT old = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_UNAWARE);
  HWND parent = make_parent(152, 112);
  SetThreadDpiAwarenessContext(old);
  EnvList env = base_env(o, w);
  // A host that won't take QUIT (a module mid-step) makes stopping it cost
  // the saver's full grace period; none of that may land on the parent.
  env.push_back({L"FAKEHOST_IGNORE_QUIT", L"1"});
  ULONGLONG child_at = 0;
  bool destroyed = false;
  double destroy_ms = -1;
  RunResult r = run_scr(o, L"/p:" + hwnd_arg(parent), env, 60000, [&](DWORD) {
    if (destroyed) return;
    if (!child_at && GetWindow(parent, GW_CHILD) && !host_events(w, "start").empty()) child_at = GetTickCount64();
    // Let it render for a moment, then close the "control panel".
    if (child_at && GetTickCount64() - child_at > 1000) {
      // DestroyWindow sends the saver's child its WM_DESTROY and waits for
      // it: the control panel is blocked for as long as the saver takes.
      LARGE_INTEGER f, t0, t1;
      QueryPerformanceFrequency(&f);
      QueryPerformanceCounter(&t0);
      DestroyWindow(parent);
      QueryPerformanceCounter(&t1);
      destroy_ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
      destroyed = true;
    }
  });
  if (!destroyed) DestroyWindow(parent);
  CHECK(destroyed);
  if (!expect_exit(w, r, 0)) return 1;
  // The saver's host grace alone is 150 ms; its WM_DESTROY must not wait on it.
  if (destroy_ms >= 100) failf("DestroyWindow(parent) blocked for %.0f ms", destroy_ms);
  check_hosts_gone(w);
  return 0;
}

int test_not_imported(const Opts& o) {
  Work w = prepare(o, "not-imported", false);
  EnvList env = base_env(o, w);
  env.push_back({L"AD_SCR_TESTEXIT_AFTER_FRAMES", L"5"});
  RunResult r = run_scr(o, L"/s", env, 30000);
  if (!expect_exit(w, r, 10)) return 1;
  CHECK(count_in_log(w.scr_log, "message shown") >= 1);
  CHECK(host_events(w, "start").empty());
  // The preview shows it too.
  HWND parent = make_parent(152, 112);
  r = run_scr(o, L"/p " + hwnd_arg(parent), env, 30000);
  DestroyWindow(parent);
  expect_exit(w, r, 10);
  return 0;
}

int test_host_missing(const Opts& o) {
  Work w = prepare(o, "host-missing");
  EnvList env = base_env(o, w);
  env.push_back({L"AD_HOST_EXE", (w.dir / "no-such-host.exe").wstring()});
  env.push_back({L"AD_SCR_TESTEXIT_AFTER_FRAMES", L"5"});
  RunResult r = run_scr(o, L"/s", env, 30000);
  expect_exit(w, r, 11);
  return 0;
}

int test_respawn(const Opts& o) {
  Work w = prepare(o, "respawn");
  edit_settings(w, [](Settings& s) { s.all_monitors = false; });
  EnvList env = base_env(o, w);
  env.push_back({L"FAKEHOST_EXIT_AFTER", L"4"});
  env.push_back({L"AD_SCR_TESTEXIT_AFTER_FRAMES", L"10"});
  RunResult r = run_scr(o, L"/s", env, 60000);
  if (!expect_exit(w, r, 0)) return 1;
  CHECK(host_events(w, "start").size() >= 3);
  CHECK(count_in_log(w.scr_log, "host-exit") >= 2);
  check_hosts_gone(w);
  return 0;
}

int test_stall(const Opts& o) {
  Work w = prepare(o, "stall");
  edit_settings(w, [](Settings& s) { s.all_monitors = false; });
  EnvList env = base_env(o, w);
  env.push_back({L"FAKEHOST_STALL_AFTER", L"3"});
  env.push_back({L"AD_SCR_TEST_STALL_MS", L"800"});
  env.push_back({L"AD_SCR_TESTEXIT_AFTER_FRAMES", L"7"});
  RunResult r = run_scr(o, L"/s", env, 60000);
  if (!expect_exit(w, r, 0)) return 1;
  CHECK(host_events(w, "start").size() >= 3);
  CHECK(count_in_log(w.scr_log, "host-stall") >= 2);
  check_hosts_gone(w);
  return 0;
}

int test_rotate(const Opts& o) {
  Work w = prepare(o, "rotate");
  edit_settings(w, [](Settings& s) {
    s.module = "random";
    s.randomize.clear();     // = every available module
    s.all_monitors = false;
  });
  EnvList env = base_env(o, w);
  // Long enough for a host to start and show frames even on a loaded
  // machine; the frame target spans a few rotations.
  env.push_back({L"AD_SCR_TEST_ROTATE_MS", L"700"});
  env.push_back({L"AD_SCR_TESTEXIT_AFTER_FRAMES", L"50"});
  RunResult r = run_scr(o, L"/s", env, 60000);
  if (!expect_exit(w, r, 0)) return 1;
  std::set<std::string> modules;
  for (auto& s : host_events(w, "start")) {
    const std::string& m = s["module"];
    modules.insert(m);
    CHECK(!ends_with(m, "ABSENT.AD"));   // catalogued but not on disk: never picked
    // Control values follow the module: only test.rings has stored ones.
    if (ends_with(m, "TESTRING.AD")) CHECK(s["ADCVSET"] == "0=75,1=0,2=2");
    else CHECK(s["ADCVSET"].empty());
  }
  CHECK(modules.size() >= 2);
  CHECK(count_in_log(w.scr_log, "rotate window=0") >= 1);
  check_hosts_gone(w);
  if (g_failures) dump_logs(w);
  return 0;
}

// Module=<id> next to a Randomize list: the named module plays first (once,
// since it isn't in the list), then the list rotates.
int test_rotate_lead(const Opts& o) {
  Work w = prepare(o, "rotate-lead");
  edit_settings(w, [](Settings& s) {
    s.module = "test.plain";
    s.randomize = {"test.rings", "test.stripes", "test.absent"};   // absent: skipped
    s.all_monitors = false;
  });
  EnvList env = base_env(o, w);
  env.push_back({L"AD_SCR_TEST_ROTATE_MS", L"400"});
  env.push_back({L"AD_SCR_TESTEXIT_AFTER_FRAMES", L"150"});
  RunResult r = run_scr(o, L"/s", env, 60000);
  if (!expect_exit(w, r, 0)) return 1;
  // What the saver started, from its own log: on a loaded machine a host
  // can be replaced at the next rotation before it has run far enough to
  // write its own "start" line (QA scr-flaky-smoke), so fakehost's log is
  // not a list of every spawn.
  std::vector<std::string> spawned;
  for (const auto& l : lines_of(w.scr_log)) {
    size_t at = l.find("spawn window=0 ");
    if (at == std::string::npos) continue;
    size_t m = l.find(" module=", at), end = m == std::string::npos ? m : l.find(' ', m + 8);
    if (m != std::string::npos) spawned.push_back(l.substr(m + 8, end == std::string::npos ? end : end - m - 8));
  }
  CHECK(spawned.size() >= 3);
  CHECK(count_in_log(w.scr_log, "respawn window=0") == 0);
  for (size_t i = 0; i < spawned.size(); ++i) {
    if (i == 0) {
      CHECK(spawned[i] == "test.plain");
    } else {
      if (spawned[i] != "test.rings" && spawned[i] != "test.stripes") failf("spawn %zu: %s", i, spawned[i].c_str());
      if (i > 1 && spawned[i] == spawned[i - 1]) failf("spawn %zu repeats %s", i, spawned[i].c_str());   // a two-module bag alternates
    }
  }
  // The hosts that did get going were started as the saver says.
  for (auto& s : host_events(w, "start")) {
    const std::string& m = s["module"];
    CHECK(ends_with(m, "TESTPLN.AD") || ends_with(m, "TESTRING.AD") || ends_with(m, "TESTSTRP.AD"));
  }
  check_hosts_gone(w);
  if (g_failures) dump_logs(w);
  return 0;
}

// A host that dies before its first frame, every time (the real adhostwin
// exits 3 for a lane it doesn't have yet): after three tries the window says
// so instead of staying black. The test hook turns that into exit code 12.
int test_start_failure(const Opts& o) {
  Work w = prepare(o, "start-failure");
  edit_settings(w, [](Settings& s) {
    s.module = "test.rings";
    s.randomize.clear();
    s.all_monitors = false;
  });
  EnvList env = base_env(o, w);
  env.push_back({L"FAKEHOST_FAIL_START", L"3"});
  env.push_back({L"AD_SCR_TESTEXIT_AFTER_FRAMES", L"5"});
  RunResult r = run_scr(o, L"/s", env, 60000);
  if (!expect_exit(w, r, 12)) return 1;
  CHECK(host_events(w, "start").size() == 3);
  CHECK(count_in_log(w.scr_log, "Test Rings\xE2\x80\x9D could not be started (host exit code 3)") == 1);
  CHECK(count_in_log(w.scr_log, "host-exit") == 3);
  check_hosts_gone(w);
  if (g_failures) dump_logs(w);
  return 0;
}

// The display powering off pauses the lockstep (no GOs: hosts idle, blocked
// on stdin) without the stall watchdog calling that a hang, and powering on
// resumes the same host.
int test_display_off(const Opts& o) {
  Work w = prepare(o, "display-off");
  edit_settings(w, [](Settings& s) { s.all_monitors = false; });
  EnvList env = base_env(o, w);
  env.push_back({L"AD_SCR_TEST_DISPLAY_OFF_MS", L"1500"});
  env.push_back({L"AD_SCR_TEST_STALL_MS", L"500"});
  env.push_back({L"AD_SCR_TESTEXIT_AFTER_FRAMES", L"30"});
  RunResult r = run_scr(o, L"/s", env, 60000);
  if (!expect_exit(w, r, 0)) return 1;
  CHECK(host_events(w, "start").size() == 1);   // no respawn: a pause is not a stall
  CHECK(count_in_log(w.scr_log, "host-stall") == 0);
  // Log lines are "[scr <pid> <tick>] ..."; the order and spacing tell the story.
  auto tick_of = [&](const std::string& needle) -> long long {
    for (const auto& l : lines_of(w.scr_log)) {
      if (l.find(needle) == std::string::npos) continue;
      size_t a = l.find(' ', 5), b = l.find(']');
      if (a != std::string::npos && b != std::string::npos) return atoll(l.substr(a + 1, b - a - 1).c_str());
    }
    return -1;
  };
  long long off = tick_of("display off"), on = tick_of("display on"), done = tick_of("test-exit after");
  CHECK(off > 0 && on > 0 && done > 0);
  CHECK(on - off >= 1400);
  CHECK(done > on);   // the 30 frames needed the display back
  check_hosts_gone(w);
  if (g_failures) dump_logs(w);
  return 0;
}

int test_p6_garbage(const Opts& o) {
  Work w = prepare(o, "p6-garbage");
  edit_settings(w, [](Settings& s) { s.all_monitors = false; });
  EnvList env = base_env(o, w);
  env.push_back({L"FAKEHOST_FORMAT", L"P6"});
  env.push_back({L"FAKEHOST_GARBAGE", L"1"});
  env.push_back({L"AD_SCR_TESTEXIT_AFTER_FRAMES", L"10"});
  RunResult r = run_scr(o, L"/s", env, 60000);
  if (!expect_exit(w, r, 0)) return 1;
  check_hosts_gone(w);
  return 0;
}

// The settings dialog of process `pid`, once it is up.
HWND find_dialog(DWORD pid) {
  struct Find { DWORD pid; HWND found; } f{pid, nullptr};
  EnumWindows([](HWND h, LPARAM lp) -> BOOL {
    auto* f = reinterpret_cast<Find*>(lp);
    DWORD p = 0;
    GetWindowThreadProcessId(h, &p);
    wchar_t cls[32] = {};
    GetClassNameW(h, cls, 32);
    if (p == f->pid && IsWindowVisible(h) && wcscmp(cls, L"#32770") == 0) {
      f->found = h;
      return FALSE;
    }
    return TRUE;
  }, reinterpret_cast<LPARAM>(&f));
  return f.found;
}

std::string window_text(HWND h) {
  wchar_t buf[256] = {};
  SendMessageW(h, WM_GETTEXT, 256, (LPARAM)buf);   // marshalled across processes
  return narrow(buf);
}

// Checked items in the module list (LVM_GETITEMSTATE carries no pointer, so
// it works across processes).
int checked_count(HWND list) {
  int n = (int)SendMessageW(list, LVM_GETITEMCOUNT, 0, 0), checked = 0;
  for (int i = 0; i < n; ++i) {
    UINT st = (UINT)SendMessageW(list, LVM_GETITEMSTATE, i, LVIS_STATEIMAGEMASK);
    if (((st & LVIS_STATEIMAGEMASK) >> 12) == 2) ++checked;
  }
  return checked;
}

// A button click as the dialog sees one, handled before this returns.
void click(HWND dlg, int id) {
  SendMessageW(dlg, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), (LPARAM)GetDlgItem(dlg, id));
}

// A message box the dialog put up (owned by it), or nullptr.
HWND owned_box(HWND dlg) {
  struct Find { HWND dlg, found; } f{dlg, nullptr};
  EnumWindows([](HWND h, LPARAM lp) -> BOOL {
    auto* f = reinterpret_cast<Find*>(lp);
    wchar_t cls[32] = {};
    GetClassNameW(h, cls, 32);
    if (GetWindow(h, GW_OWNER) == f->dlg && IsWindowVisible(h) && wcscmp(cls, L"#32770") == 0) {
      f->found = h;
      return FALSE;
    }
    return TRUE;
  }, reinterpret_cast<LPARAM>(&f));
  return f.found;
}

// Clicks a radio button the way the dialog sees a click.
void choose_radio(HWND dlg, int on, int off) {
  SendMessageW(GetDlgItem(dlg, off), BM_SETCHECK, BST_UNCHECKED, 0);
  SendMessageW(GetDlgItem(dlg, on), BM_SETCHECK, BST_CHECKED, 0);
  SendMessageW(dlg, WM_COMMAND, MAKEWPARAM(on, BN_CLICKED), (LPARAM)GetDlgItem(dlg, on));
}

// Drives the settings dialog from outside the process by control ID, then
// checks what OK wrote. Two sessions: the fixture's module (numeric slider,
// checkbox, popup; switched to a single module), then one shaped like the
// generated catalog (button, string slider, slider with a unit; switched to
// Random).
int test_config(const Opts& o) {
  Work w = prepare(o, "config");
  EnvList env = base_env(o, w);
  auto slot = [](HWND panel, int i, int part) {
    return GetDlgItem(panel, IDC_PANEL_BASE + i * IDC_PANEL_STRIDE + part);
  };

  // ---- session 1: Module=test.rings, Randomize=test.rings,test.stripes
  bool acted = false;
  int items = -1, checked = -1;
  LRESULT pos = -1, check = -1, sel = -1, random = -1;
  RunResult r = run_scr(o, L"/c", env, 60000, [&](DWORD pid) {
    if (acted) return;
    HWND dlg = find_dialog(pid);
    if (!dlg) return;
    HWND list = GetDlgItem(dlg, IDC_MODULE_LIST), panel = GetDlgItem(dlg, IDC_PANEL);
    HWND tb = slot(panel, 0, IDC_PART_INPUT), cb = slot(panel, 1, IDC_PART_INPUT), combo = slot(panel, 2, IDC_PART_INPUT);
    if (!list || !panel || !tb || !cb || !combo) return;   // not built yet
    items = (int)SendMessageW(list, LVM_GETITEMCOUNT, 0, 0);
    checked = checked_count(list);
    pos = SendMessageW(tb, TBM_GETPOS, 0, 0);
    check = SendMessageW(cb, BM_GETCHECK, 0, 0);
    sel = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    random = SendMessageW(GetDlgItem(dlg, IDC_MODE_RANDOM), BM_GETCHECK, 0, 0);
    // Move the slider and tick the box the way the controls report it.
    SendMessageW(tb, TBM_SETPOS, TRUE, 30);
    SendMessageW(panel, WM_HSCROLL, MAKEWPARAM(TB_ENDTRACK, 0), (LPARAM)tb);
    SendMessageW(cb, BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(panel, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(cb), BN_CLICKED), (LPARAM)cb);
    choose_radio(dlg, IDC_MODE_SINGLE, IDC_MODE_RANDOM);
    PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), (LPARAM)GetDlgItem(dlg, IDOK));
    acted = true;
  });
  CHECK(acted);
  if (!expect_exit(w, r, 0)) return 1;
  CHECK(items == 5);                // every catalogued module, including the missing one
  CHECK(checked == 2);              // Randomize=test.rings, test.stripes
  CHECK(pos == 75);                 // [Module.test.rings] 0=75
  CHECK(check == BST_UNCHECKED);    // 1=0
  CHECK(sel == 2);                  // 2=2
  CHECK(random == BST_CHECKED);     // a Randomize list makes the saver rotate: Random mode

  Settings s;
  CHECK(load_settings(w.settings.wstring(), s));
  CHECK(s.module == "test.rings");
  CHECK(s.randomize.empty());       // single module: no list, or the saver would rotate
  CHECK(!s.rotates());
  // ...but the checklist is kept for the next time Random is chosen.
  CHECK((s.randomize_saved == std::vector<std::string>{"test.rings", "test.stripes"}));
  CHECK(s.duration_min == 5 && s.scale == 1.0 && s.all_monitors);
  CHECK((s.controls["test.rings"] == std::map<int, int>{{0, 30}, {1, 1}, {2, 2}}));
  std::string text;
  read_file(w.settings.wstring(), text);
  IniFile ini;
  ini.parse(text);
  CHECK(ini.get("Saver", "FutureKey") && *ini.get("Saver", "FutureKey") == "keep me");
  CHECK(ini.get("Extra", "Unknown") && *ini.get("Extra", "Unknown") == "1");
  if (g_failures) fprintf(stderr, "---- settings.ini after session 1\n%s\n", text.c_str());

  // ---- session 2: Module=test.stops (single, the checklist kept aside)
  edit_settings(w, [](Settings& s) { s.module = "test.stops"; });
  // A host that can't open module windows: the button stays a read-only row
  // (config-buttons covers the live one).
  EnvList env2 = env;
  env2.push_back({L"FAKEHOST_CONFIGURE", L"none"});
  acted = false;
  random = -1;
  checked = -1;
  bool button_disabled = false;
  LRESULT stops = -1, stop_pos = -1, num_pos = -1, order = -1;
  std::string stop_label, stop_label_after, num_label;
  r = run_scr(o, L"/c", env2, 60000, [&](DWORD pid) {
    if (acted) return;
    HWND dlg = find_dialog(pid);
    if (!dlg) return;
    HWND panel = GetDlgItem(dlg, IDC_PANEL);
    HWND button = slot(panel, 0, IDC_PART_INPUT), ss = slot(panel, 1, IDC_PART_INPUT);
    HWND num = slot(panel, 2, IDC_PART_INPUT), combo = slot(panel, 3, IDC_PART_INPUT);
    if (!panel || !button || !ss || !num || !combo) return;
    random = SendMessageW(GetDlgItem(dlg, IDC_MODE_RANDOM), BM_GETCHECK, 0, 0);
    checked = checked_count(GetDlgItem(dlg, IDC_MODULE_LIST));
    button_disabled = !IsWindowEnabled(button);
    stops = SendMessageW(ss, TBM_GETRANGEMAX, 0, 0) + 1;
    stop_pos = SendMessageW(ss, TBM_GETPOS, 0, 0);
    stop_label = window_text(slot(panel, 1, IDC_PART_VALUE));
    num_pos = SendMessageW(num, TBM_GETPOS, 0, 0);
    num_label = window_text(slot(panel, 2, IDC_PART_VALUE));
    order = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    // "Always" -> "Rarely": the stored value is that stop's, 33.
    SendMessageW(ss, TBM_SETPOS, TRUE, 1);
    SendMessageW(panel, WM_HSCROLL, MAKEWPARAM(TB_THUMBPOSITION, 1), (LPARAM)ss);
    stop_label_after = window_text(slot(panel, 1, IDC_PART_VALUE));
    choose_radio(dlg, IDC_MODE_RANDOM, IDC_MODE_SINGLE);
    PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), (LPARAM)GetDlgItem(dlg, IDOK));
    acted = true;
  });
  CHECK(acted);
  if (!expect_exit(w, r, 0)) return 1;
  CHECK(random == BST_UNCHECKED);
  CHECK(checked == 2);              // session 1's checklist, back from RandomizeSaved
  CHECK(button_disabled);           // a host without configure: a read-only row
  CHECK(stops == 4);
  CHECK(stop_pos == 3);             // default 100 = "Always"
  CHECK(stop_label == "Always");
  CHECK(stop_label_after == "Rarely");
  CHECK(num_pos == 34);
  CHECK(num_label == "34%");
  CHECK(order == 1);

  CHECK(load_settings(w.settings.wstring(), s));
  // Random again: the kept checklist is the rotation list once more.
  CHECK(s.module == "random");
  CHECK((s.randomize == std::vector<std::string>{"test.rings", "test.stripes"}));
  CHECK(s.randomize_saved.empty());
  CHECK((s.controls["test.stops"] == std::map<int, int>{{1, 33}}));
  CHECK((s.controls["test.rings"] == std::map<int, int>{{0, 30}, {1, 1}, {2, 2}}));   // untouched
  read_file(w.settings.wstring(), text);
  if (g_failures) fprintf(stderr, "---- settings.ini after session 2\n%s\n", text.c_str());

  // ---- session 3: Random, Clear, then Single and OK: the checklist the user
  // left empty is kept as such (RandomizeSaved=-), not as "all" (WIP 9b).
  acted = false;
  checked = -1;
  r = run_scr(o, L"/c", env, 60000, [&](DWORD pid) {
    if (acted) return;
    HWND dlg = find_dialog(pid);
    HWND list = dlg ? GetDlgItem(dlg, IDC_MODULE_LIST) : nullptr;
    if (!list || SendMessageW(list, LVM_GETITEMCOUNT, 0, 0) == 0) return;
    click(dlg, IDC_CHECK_NONE);
    checked = checked_count(list);
    choose_radio(dlg, IDC_MODE_SINGLE, IDC_MODE_RANDOM);
    PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), (LPARAM)GetDlgItem(dlg, IDOK));
    acted = true;
  });
  CHECK(acted);
  if (!expect_exit(w, r, 0)) return 1;
  CHECK(checked == 0);
  read_file(w.settings.wstring(), text);
  CHECK(text.find("RandomizeSaved=-\r\n") != std::string::npos);
  CHECK(load_settings(w.settings.wstring(), s));
  CHECK(!s.rotates() && s.randomize_saved_none);

  // ---- session 4: the next dialog opens in Single, and Random shows none checked.
  acted = false;
  random = -1;
  int checked_single = -1, checked_random = -1;
  r = run_scr(o, L"/c", env, 60000, [&](DWORD pid) {
    if (acted) return;
    HWND dlg = find_dialog(pid);
    HWND list = dlg ? GetDlgItem(dlg, IDC_MODULE_LIST) : nullptr;
    if (!list || SendMessageW(list, LVM_GETITEMCOUNT, 0, 0) == 0) return;
    random = SendMessageW(GetDlgItem(dlg, IDC_MODE_RANDOM), BM_GETCHECK, 0, 0);
    checked_single = checked_count(list);
    choose_radio(dlg, IDC_MODE_RANDOM, IDC_MODE_SINGLE);
    checked_random = checked_count(list);
    PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), (LPARAM)GetDlgItem(dlg, IDCANCEL));
    acted = true;
  });
  CHECK(acted);
  if (!expect_exit(w, r, 0)) return 1;
  CHECK(random == BST_UNCHECKED);
  CHECK(checked_single == 0 && checked_random == 0);
  std::string after_cancel;
  read_file(w.settings.wstring(), after_cancel);
  CHECK(after_cancel == text);   // Cancel wrote nothing
  if (g_failures) fprintf(stderr, "---- settings.ini after session 3\n%s\n", text.c_str());
  return 0;
}

// A named Module leading a Randomize list (the fixture's shape: test.rings
// plays first, then test.rings/test.stripes rotate). The dialog shows it as
// Random, and saving in Random mode must keep the lead.
std::vector<std::string> list_item_names(HWND list);   // below

int test_config_lead(const Opts& o) {
  Work w = prepare(o, "config-lead");
  EnvList env = base_env(o, w);
  using Ids = std::vector<std::string>;

  // ---- session 1: OK without touching anything changes nothing.
  bool acted = false;
  LRESULT random = -1;
  int checked = -1;
  std::vector<std::string> rows;
  RunResult r = run_scr(o, L"/c", env, 60000, [&](DWORD pid) {
    if (acted) return;
    HWND dlg = find_dialog(pid);
    HWND list = dlg ? GetDlgItem(dlg, IDC_MODULE_LIST) : nullptr;
    if (!list || SendMessageW(list, LVM_GETITEMCOUNT, 0, 0) == 0) return;
    random = SendMessageW(GetDlgItem(dlg, IDC_MODE_RANDOM), BM_GETCHECK, 0, 0);
    checked = checked_count(list);
    rows = list_item_names(list);
    PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), (LPARAM)GetDlgItem(dlg, IDOK));
    acted = true;
  });
  CHECK(acted);
  if (!expect_exit(w, r, 0)) return 1;
  CHECK(random == BST_CHECKED);
  CHECK(checked == 2);
  // The lead's row says it plays first (WIP 9c: a badge, and this for screen readers); no other does.
  int leads = 0;
  for (const std::string& n : rows) {
    if (n.find("plays first") == std::string::npos) continue;
    ++leads;
    CHECK(n == "Test Rings, plays first");
  }
  CHECK(rows.size() == 5 && leads == 1);
  if (leads != 1) for (const std::string& n : rows) fprintf(stderr, "  row: %s\n", n.c_str());
  Settings s;
  CHECK(load_settings(w.settings.wstring(), s));
  CHECK(s.module == "test.rings");   // still leading (it used to become "random")
  CHECK((s.randomize == Ids{"test.rings", "test.stripes"}));
  CHECK(s.has_lead());

  // ---- session 2: "Check none" + OK is refused (Random needs something to
  // choose from; with a lead it would otherwise quietly save a single
  // module) and writes nothing; then "Check all" + OK keeps the lead with
  // the whole list written out.
  std::string before, after_refusal;
  read_file(w.settings.wstring(), before);
  HWND dlg = nullptr;
  int step = 0;
  bool box_seen = false;
  r = run_scr(o, L"/c", env, 60000, [&](DWORD pid) {
    if (!dlg) {
      // Found once: while the message box is up it is a #32770 of ours too.
      HWND d = find_dialog(pid);
      HWND list = d ? GetDlgItem(d, IDC_MODULE_LIST) : nullptr;
      if (!list || SendMessageW(list, LVM_GETITEMCOUNT, 0, 0) == 0) return;
      dlg = d;
    }
    if (step == 0) {
      click(dlg, IDC_CHECK_NONE);
      PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), (LPARAM)GetDlgItem(dlg, IDOK));
      step = 1;
    } else if (step == 1) {
      HWND box = owned_box(dlg);
      if (!box) return;
      box_seen = true;
      read_file(w.settings.wstring(), after_refusal);
      // Its OK button, pressed as a user would (a bare WM_COMMAND is ignored).
      if (HWND ok = FindWindowExW(box, nullptr, L"Button", nullptr)) PostMessageW(ok, BM_CLICK, 0, 0);
      else PostMessageW(box, WM_CLOSE, 0, 0);
      step = 2;
    } else if (step == 2) {
      if (owned_box(dlg)) return;
      click(dlg, IDC_CHECK_ALL);
      PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), (LPARAM)GetDlgItem(dlg, IDOK));
      step = 3;
    }
  });
  CHECK(step == 3);
  if (!expect_exit(w, r, 0)) return 1;
  CHECK(box_seen);
  CHECK(after_refusal == before);
  CHECK(load_settings(w.settings.wstring(), s));
  CHECK(s.module == "test.rings");
  // Every module, in list order: an empty list would make the lead a single module.
  CHECK((s.randomize == Ids{"test.absent", "test.plain", "test.rings", "test.stops", "test.stripes"}));
  CHECK(s.randomize_saved.empty());
  if (g_failures) fprintf(stderr, "---- settings.ini\n%s\n", before.c_str());
  return 0;
}

// The Import… button: adimport (fakeimport.exe here, a console program like
// it) starts without a console window, and its exit code is read as
// adw::import::Status — 5 cancelled and 1-4 failed change nothing, 0 brings
// in the new catalog. The first import, from the welcome, then asks the host
// what it runs.
int test_import(const Opts& o) {
  Work w = prepare(o, "import");
  fs::path catalog = w.assets / "win" / "catalog-win.json";
  std::string original;
  CHECK(read_file(catalog.wstring(), original));
  // What a finished import leaves behind: the fixture plus one module.
  std::string grown = original;
  const std::string anchor = "\"modules\": [";
  size_t at = grown.find(anchor);
  CHECK(at != std::string::npos);
  if (at == std::string::npos) return 1;
  grown.insert(at + anchor.size(), "\n    { \"id\": \"test.new\", \"displayName\": \"Test New\", \"lane\": \"pe32\", "
                                   "\"path\": \"FILES/AD40/TESTNEW.AD\", \"controls\": [] },");
  fs::path grown_path = w.dir / "catalog-grown.json";
  CHECK(write_file_atomic(grown_path.wstring(), grown));

  struct Case {
    const wchar_t* exit;
    int items;            // modules listed after the importer exits
    const char* status;   // the assets line then (a prefix)
  };
  // The failing importers overwrite the catalog anyway: a dialog that took
  // their exit for success would list the extra module.
  const Case cases[] = {{L"5", 5, "Import cancelled. Nothing was changed."},
                        {L"2", 5, "Import did not finish (adimport exit code 2). Nothing was changed."},
                        {L"0", 6, "6 modules imported"}};
  for (const Case& c : cases) {
    CHECK(write_file_atomic(catalog.wstring(), original));
    EnvList env = base_env(o, w);
    env.push_back({L"FAKEIMPORT_EXIT", c.exit});
    env.push_back({L"FAKEIMPORT_CATALOG", grown_path.wstring()});
    HWND dlg = nullptr;
    int step = 0, items_before = -1, items_after = -1;
    std::string status;
    RunResult r = run_scr(o, L"/c", env, 60000, [&](DWORD pid) {
      if (step == 2) return;
      if (!dlg) {
        HWND d = find_dialog(pid);
        HWND list = d ? GetDlgItem(d, IDC_MODULE_LIST) : nullptr;
        if (!list || SendMessageW(list, LVM_GETITEMCOUNT, 0, 0) == 0) return;
        dlg = d;
      }
      HWND list = GetDlgItem(dlg, IDC_MODULE_LIST);
      if (step == 0) {
        items_before = (int)SendMessageW(list, LVM_GETITEMCOUNT, 0, 0);
        click(dlg, IDC_IMPORT);   // disables the button until the importer has exited
        step = 1;
      } else if (IsWindowEnabled(GetDlgItem(dlg, IDC_IMPORT))) {
        items_after = (int)SendMessageW(list, LVM_GETITEMCOUNT, 0, 0);
        status = window_text(GetDlgItem(dlg, IDC_ASSETS_STATUS));
        PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
        step = 2;
      }
    });
    CHECK(step == 2);
    if (!expect_exit(w, r, 0)) return 1;
    CHECK(items_before == 5);
    CHECK(items_after == c.items);
    if (status.rfind(c.status, 0) != 0) failf("exit %ls: status \"%s\", expected \"%s…\"", c.exit, status.c_str(), c.status);
  }
  // Every run: no console window, and "--gui" (its own progress UI).
  auto runs = lines_of(w.dir / "fakeimport.log");
  CHECK(runs.size() == 3);
  for (const auto& l : runs) {
    if (l != "run\tconsole=0\targs=--gui") failf("importer run: %s", l.c_str());
  }

  // The first import, from the welcome (which asks the host nothing: it has
  // no module to ask about). A later catalog keeps the host's answer, but
  // this one has none yet: the host is asked after the import, before the
  // module shown starts, and the module's button comes alive.
  Work fw = prepare(o, "import-first");
  const fs::path imported = fw.dir / "catalog-imported.json";
  fs::copy_file(fw.assets / "win" / "catalog-win.json", imported);
  fs::remove(fw.assets / "win" / "catalog-win.json");   // the module files stay; the dialog opens on the welcome
  edit_settings(fw, [](Settings& s) {
    s.module = "test.stops";
    s.randomize.clear();
  });
  EnvList env = base_env(o, fw);
  env.push_back({L"FAKEIMPORT_EXIT", L"0"});
  env.push_back({L"FAKEIMPORT_CATALOG", imported.wstring()});
  HWND dlg = nullptr;
  int step = 0;
  bool welcome = false, live_button = false;
  ULONGLONG t0 = 0;
  RunResult r = run_scr(o, L"/c", env, 60000, [&](DWORD pid) {
    if (!dlg) {
      HWND d = find_dialog(pid);
      if (!d || !IsWindowVisible(GetDlgItem(d, IDC_WELCOME_IMPORT))) return;
      dlg = d;
      welcome = SendMessageW(GetDlgItem(d, IDC_MODULE_LIST), LVM_GETITEMCOUNT, 0, 0) == 0;
    }
    if (step == 0) {
      click(dlg, IDC_WELCOME_IMPORT);
      t0 = GetTickCount64();
      step = 1;
    } else if (step == 1) {
      // Until the button (test.stops' control 0) is a live one and the
      // module's live preview runs.
      HWND button = GetDlgItem(GetDlgItem(dlg, IDC_PANEL), IDC_PANEL_BASE + IDC_PART_INPUT);
      wchar_t cls[32] = {};
      if (button) GetClassNameW(button, cls, 32);
      live_button = button && _wcsicmp(cls, L"Button") == 0 && IsWindowEnabled(button);
      bool previewing = false;
      for (auto& e : hosts_of(fw, pid)) previewing |= ends_with(e["module"], "TESTSTOP.AD");
      if ((!live_button || !previewing) && GetTickCount64() - t0 < 10000) return;
      PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
      step = 2;
    }
  });
  CHECK(step == 2);
  if (!expect_exit(fw, r, 0)) return 1;
  CHECK(welcome && live_button);
  CHECK(count_in_log(fw.scr_log, "dialog: catalog reloaded") == 1);
  CHECK(count_in_log(fw.scr_log, "dialog: host capabilities") == 1);
  const auto lines = lines_of(fw.scr_log);
  auto line_of = [&](const char* needle) {
    for (size_t i = 0; i < lines.size(); ++i) {
      if (lines[i].find(needle) != std::string::npos) return (int)i;
    }
    return -1;
  };
  const int reloaded = line_of("dialog: catalog reloaded"), asked = line_of("dialog: host capabilities"),
            spawned = line_of("live preview: spawn");
  CHECK(reloaded >= 0 && asked > reloaded && spawned > asked);
  check_hosts_gone(fw);
  if (g_failures) dump_logs(fw);
  return 0;
}

// Preview runs "/s" from a throwaway copy of the dialog's settings in the
// temp directory. The Preview deletes it once read, so closing the dialog
// while a Preview runs leaves nothing behind; files of dialogs that died are
// swept when the next one opens, and nothing else is touched.
int test_preview_settings(const Opts& o) {
  Work w = prepare(o, "preview-settings");
  fs::path tmp = w.dir / "tmp";
  fs::create_directories(tmp);
  std::string live_name = "LongAfterDark-preview-" + std::to_string(GetCurrentProcessId()) + ".ini";
  fs::path dead = tmp / "LongAfterDark-preview-4294967292.ini", live = tmp / live_name, other = tmp / "unrelated.ini";
  for (const fs::path& p : {dead, live, other}) CHECK(write_file_atomic(p.wstring(), "[Saver]\r\nModule=test.plain\r\n"));
  EnvList env = base_env(o, w);
  env.push_back({L"TMP", tmp.wstring()});
  env.push_back({L"TEMP", tmp.wstring()});
  // The Preview inherits this: it ends itself after these frames.
  env.push_back({L"AD_SCR_TESTEXIT_AFTER_FRAMES", L"240"});
  HWND dlg = nullptr;
  int step = 0;
  DWORD dialog_pid = 0;
  bool swept = false, kept = false, gone_while_running = false, preview_busy = false;
  RunResult r = run_scr(o, L"/c", env, 60000, [&](DWORD pid) {
    dialog_pid = pid;
    if (step == 2) return;
    if (!dlg) {
      HWND d = find_dialog(pid);
      HWND list = d ? GetDlgItem(d, IDC_MODULE_LIST) : nullptr;
      if (!list || SendMessageW(list, LVM_GETITEMCOUNT, 0, 0) == 0) return;
      dlg = d;
    }
    if (step == 0) {
      swept = !fs::exists(dead);
      kept = fs::exists(live) && fs::exists(other);
      click(dlg, IDC_PREVIEW);
      step = 1;
    } else if (!hosts_of(w, pid, true).empty()) {
      // The Preview starts hosts only after it has read its settings (the
      // dialog's own live preview doesn't count).
      gone_while_running = !fs::exists(tmp / ("LongAfterDark-preview-" + std::to_string(pid) + ".ini"));
      preview_busy = !IsWindowEnabled(GetDlgItem(dlg, IDC_PREVIEW));
      PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);   // close it mid-Preview
      step = 2;
    }
  });
  CHECK(step == 2);
  if (!expect_exit(w, r, 0)) return 1;
  CHECK(swept);
  CHECK(kept);
  CHECK(gone_while_running);
  CHECK(preview_busy);
  ULONGLONG deadline = GetTickCount64() + 45000;
  while (count_in_log(w.scr_log, "test-exit after") == 0 && GetTickCount64() < deadline) Sleep(100);
  CHECK(count_in_log(w.scr_log, "test-exit after 240 frames") == 1);
  CHECK(count_in_log(w.scr_log, "settings: removed the temporary") == 1);
  // It ran what the dialog showed: the selected module and its values.
  auto starts = hosts_of(w, dialog_pid, true);
  CHECK(!starts.empty());
  for (auto& s : starts) {
    CHECK(ends_with(s["module"], "TESTRING.AD"));
    CHECK(s["ADCVSET"] == "0=75,1=0,2=2");
  }
  // The Preview's desktop capture (INTERACTION.md §8) goes with its last
  // handle, a moment after its log's last line.
  std::set<std::string> left;
  for (ULONGLONG until = GetTickCount64() + 10000;; Sleep(100)) {
    left.clear();
    for (auto& e : fs::directory_iterator(tmp)) left.insert(e.path().filename().string());
    if (left.size() <= 2 || GetTickCount64() > until) break;
  }
  CHECK((left == std::set<std::string>{live_name, "unrelated.ini"}));
  check_hosts_gone(w);
  if (g_failures) dump_logs(w);
  return 0;
}

// The dialog's live preview runs the selected module in its own host at a
// real screen's size with the dialog's (unsaved) values, restarts it when a
// value changes, and leaves no host behind.
int test_config_live(const Opts& o) {
  Work w = prepare(o, "config-live");
  EnvList env = base_env(o, w);
  DWORD dialog_pid = 0;
  int step = 0;
  RunResult r = run_scr(o, L"/c", env, 60000, [&](DWORD pid) {
    dialog_pid = pid;
    HWND dlg = find_dialog(pid);
    HWND panel = dlg ? GetDlgItem(dlg, IDC_PANEL) : nullptr;
    HWND cb = panel ? GetDlgItem(panel, IDC_PANEL_BASE + 1 * IDC_PANEL_STRIDE + IDC_PART_INPUT) : nullptr;
    if (!cb) return;
    auto rings = [&] {
      int n = 0;
      for (auto& e : hosts_of(w, pid)) n += ends_with(e["module"], "TESTRING.AD");
      return n;
    };
    if (step == 0 && rings() >= 1) {
      // Tick "Sound" (index 1): the preview restarts with it.
      SendMessageW(cb, BM_SETCHECK, BST_CHECKED, 0);
      SendMessageW(panel, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(cb), BN_CLICKED), (LPARAM)cb);
      step = 1;
    } else if (step == 1 && rings() >= 2) {
      PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
      step = 2;
    }
  });
  CHECK(step == 2);
  if (!expect_exit(w, r, 0)) return 1;
  std::vector<std::map<std::string, std::string>> live;
  for (auto& e : hosts_of(w, dialog_pid)) if (ends_with(e["module"], "TESTRING.AD")) live.push_back(e);
  CHECK(live.size() >= 2);
  if (live.size() >= 2) {
    CHECK(live[0]["ADSTREAM"] == "1");
    CHECK(live[0]["ADSCREENH"] == "480");   // a full-size screen, shown scaled down
    CHECK(live[0]["ADCVSET"] == "0=75,1=0,2=2");
    CHECK(live.back()["ADCVSET"] == "0=75,1=1,2=2");
  }
  check_state_everywhere(w);
  check_hosts_gone(w);
  // Cancel saved nothing.
  Settings s;
  CHECK(load_settings(w.settings.wstring(), s));
  CHECK((s.controls["test.rings"] == std::map<int, int>{{0, 75}, {1, 0}, {2, 2}}));
  if (g_failures) dump_logs(w);
  return 0;
}

// A host without the Classic lane: the dialog finds out (without running a
// module) and shows Classic modules as coming soon instead of starting them.
int test_config_classic(const Opts& o) {
  Work w = prepare(o, "config-classic");
  edit_settings(w, [](Settings& s) {
    s.module = "test.stripes";   // Classic
    s.randomize.clear();
  });
  EnvList env = base_env(o, w);
  env.push_back({L"FAKEHOST_LANES", L"pe32"});
  env.push_back({L"AD_SCR_THUMBGEN", L"1"});   // they must leave it alone too
  std::string badge;
  bool acted = false;
  RunResult r = run_scr(o, L"/c", env, 60000, [&](DWORD pid) {
    if (acted) return;
    HWND dlg = find_dialog(pid);
    HWND b = dlg ? GetDlgItem(dlg, IDC_MODULE_BADGE) : nullptr;
    if (!b) return;
    badge = window_text(b);
    if (badge.find("Coming soon") == std::string::npos) return;   // not answered yet
    PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
    acted = true;
  });
  CHECK(acted);
  if (!expect_exit(w, r, 0)) return 1;
  // The chip names the release (this catalog predates packages: "Other
  // modules"), never the lane (COVERS.md §1.7).
  CHECK(badge == "Other modules \xC2\xB7 Coming soon");
  // One probe (`--capabilities`: no module runs for it), and no live preview
  // (or thumbnail) of a Classic module.
  CHECK(host_events(w, "capabilities").size() == 1);
  for (auto& e : host_events(w, "start")) {
    if (e["module"].find("CLASSIC") != std::string::npos)
      failf("a Classic module was started: %s", e["module"].c_str());
  }
  check_hosts_gone(w);
  if (g_failures) dump_logs(w);
  return 0;
}

// Every module without an icon of its own gets a thumbnail taken in the
// background (thumbnails.h): each one it can run, one at a time, in a host
// of its own on the 640x480 screen the modules were made for, and none of a
// module whose file is missing. Closing the dialog leaves no host behind.
int test_config_thumbs(const Opts& o) {
  Work w = prepare(o, "config-thumbs");
  EnvList env = base_env(o, w);
  env.push_back({L"AD_SCR_THUMBGEN", L"1"});
  const fs::path thumbs = w.dir / "thumbs";
  const std::vector<std::string> want = {"test.plain", "test.rings", "test.stops", "test.stripes"};
  auto taken = [&] {
    int n = 0;
    for (const auto& id : want) n += fs::exists(thumbs / (id + ".v2.png"));
    return n;
  };
  DWORD dialog_pid = 0;
  bool done = false;
  RunResult r = run_scr(o, L"/c", env, 80000, [&](DWORD pid) {
    dialog_pid = pid;
    if (done) return;
    HWND dlg = find_dialog(pid);
    if (!dlg || taken() < (int)want.size()) return;
    PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
    done = true;
  });
  CHECK(done);
  if (!expect_exit(w, r, 0)) return 1;
  CHECK(taken() == (int)want.size());
  CHECK(!fs::exists(thumbs / "test.absent.v2.png"));   // its file is missing: nothing to run
  // Each taken by a host of its own at the modules' own screen size.
  std::set<std::string> ran;
  for (auto& e : hosts_of(w, dialog_pid)) {
    if (e["ADSCREENW"] == "640" && e["ADSCREENH"] == "480") ran.insert(e["module"]);
  }
  CHECK(ran.size() >= want.size());
  for (const auto& m : ran) CHECK(m.find("ABSENT") == std::string::npos);
  check_state_everywhere(w);
  check_hosts_gone(w);
  if (g_failures) dump_logs(w);
  return 0;
}

// ---- the list never opens on a cut row ------------------------------------------------

// A PNG as top-down 32-bit BGRA rows.
bool load_png(const fs::path& path, int* w, int* h, std::vector<uint8_t>* bgra) {
  HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  bool ok = false;
  IWICImagingFactory* f = nullptr;
  IWICBitmapDecoder* dec = nullptr;
  IWICBitmapFrameDecode* frame = nullptr;
  IWICFormatConverter* conv = nullptr;
  UINT uw = 0, uh = 0;
  if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) &&
      SUCCEEDED(f->CreateDecoderFromFilename(path.wstring().c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                             &dec)) &&
      SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(f->CreateFormatConverter(&conv)) &&
      SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                 WICBitmapPaletteTypeCustom)) &&
      SUCCEEDED(conv->GetSize(&uw, &uh))) {
    bgra->resize((size_t)uw * uh * 4);
    ok = SUCCEEDED(conv->CopyPixels(nullptr, uw * 4, (UINT)bgra->size(), bgra->data()));
    *w = (int)uw;
    *h = (int)uh;
  }
  for (IUnknown* u : {(IUnknown*)conv, (IUnknown*)frame, (IUnknown*)dec, (IUnknown*)f}) {
    if (u) u->Release();
  }
  if (SUCCEEDED(co)) CoUninitialize();
  return ok;
}

// The list opens with nothing cut at its top edge, whatever the DPI, the
// window's size, the mode and where the selected module sits (end of a
// group, start of the next, mid-list, last of all): rendered off-screen by
// the screenshot hook with a catalog shaped like the real one (23 + 61
// modules), and the list's first pixel rows checked to be the card's own
// colour.
// One render of the list-top sweep: the list's first pixel rows are the
// card's colour, nothing straddles its top edge, and (with releases) the
// strip is where and what the report says. Returns false when there was no picture.
bool list_top_shot(const Opts& o, const Work& w, const std::string& name, const std::string& state, int dpi,
                   const char* want_strip) {
  const fs::path png = w.dir / (name + ".png"), report = w.dir / (name + ".txt");
  EnvList env = base_env(o, w);
  env.push_back({L"AD_SCR_TEST_SCREENSHOT", png.wstring()});
  env.push_back({L"AD_SCR_TEST_SCREENSHOT_STATE", widen(state + ";dpi=" + std::to_string(dpi) + ";frames=1;wait=300;report=") +
                                                      report.wstring()});
  RunResult r = run_scr(o, L"/c", env, 30000);
  if (!expect_exit(w, r, 0)) return false;
  std::map<std::string, std::string> kv;
  for (const auto& l : lines_of(report)) {
    size_t eq = l.find('=');
    if (eq != std::string::npos) kv[l.substr(0, eq)] = l.substr(eq + 1);
  }
  int lx = 0, ly = 0, lw = 0, lh = 0, pw = 0, ph = 0;
  unsigned card = 0;
  std::vector<uint8_t> px;
  if (sscanf(kv["list"].c_str(), "%d,%d,%d,%d", &lx, &ly, &lw, &lh) != 4 || sscanf(kv["card"].c_str(), "%x", &card) != 1 ||
      !load_png(png, &pw, &ph, &px) || lx < 0 || ly < 0 || lx + lw > pw || ly + lh > ph) {
    failf("%s: no report or picture", name.c_str());
    return false;
  }
  if (kv["top"] != "clean") failf("%s: a row or header straddles the list's top edge", name.c_str());
  // Scrolled, the first 2 DIP of the list (above any row's highlight, tile
  // or text) are the card's colour across the rows' width. (At its start
  // the first group header is in its own place, and in Random its checkbox
  // comes near the edge.)
  const int rows = kv["scroll"] == "0" ? 0 : std::max(1, dpi * 2 / 96), inset = dpi * 16 / 96;
  int off = 0;
  for (int y = ly; y < ly + rows; ++y) {
    for (int x = lx + inset; x < lx + lw - inset; ++x) {
      const uint8_t* p = &px[((size_t)y * pw + x) * 4];
      const int r = (card >> 16) & 0xFF, g = (card >> 8) & 0xFF, b = card & 0xFF;
      off += std::abs(p[2] - r) > 3 || std::abs(p[1] - g) > 3 || std::abs(p[0] - b) > 3;
    }
  }
  if (off) failf("%s: %d pixels of a cut row at the list's top", name.c_str(), off);
  // The strip: its form, and the tiles area inside the picture, above the list.
  if (kv["strip_mode"] != want_strip) failf("%s: strip_mode=%s, expected %s", name.c_str(), kv["strip_mode"].c_str(), want_strip);
  int sx = 0, sy = 0, sw = 0, sh = 0;
  if (sscanf(kv["strip"].c_str(), "%d,%d,%d,%d", &sx, &sy, &sw, &sh) != 4) failf("%s: no strip in the report", name.c_str());
  if (std::string(want_strip) == "hidden") {
    if (sw || sh) failf("%s: a hidden strip at %d,%d %dx%d", name.c_str(), sx, sy, sw, sh);
  } else if (sw <= 0 || sh <= 0 || sx < 0 || sy < 0 || sx + sw > pw || sy + sh >= ly) {
    failf("%s: strip %d,%d %dx%d not in the picture above the list", name.c_str(), sx, sy, sw, sh);
  }
  return true;
}

// The catalog shaped like the real one for the list-top sweep: 23 AD4 and
// 61 Classic modules; with `releases`, in four releases (the strip shows),
// else from before packages (one "Other modules" group, no strip).
void write_list_top_catalog(const Work& w, bool releases) {
  fs::path win = w.assets / "win";
  std::string json = "{ \"version\": 1,";
  if (releases) {
    json += " \"packages\": [ {\"id\":\"deluxe\",\"title\":\"After Dark 4.0 Deluxe\",\"shortTitle\":\"Deluxe\",\"modules\":23},"
            " {\"id\":\"ad32\",\"title\":\"After Dark 3.2\",\"shortTitle\":\"After Dark 3.2\",\"modules\":30},"
            " {\"id\":\"tt\",\"title\":\"Totally Twisted After Dark\",\"shortTitle\":\"Totally Twisted\",\"modules\":20},"
            " {\"id\":\"simpsons\",\"title\":\"The Simpsons Screen Saver\",\"shortTitle\":\"Simpsons\",\"modules\":11} ],";
  }
  json += " \"modules\": [\n";
  auto add = [&](const std::string& id, const std::string& name, const char* lane, const std::string& rel, const char* pkg) {
    json += "  { \"id\": \"" + id + "\", \"displayName\": \"" + name + "\", \"lane\": \"" + lane + "\", \"path\": \"" + rel +
            "\", " + (releases ? std::string("\"package\": \"") + pkg + "\", " : std::string()) + "\"controls\": [] },\n";
    fs::path p = resolve_module_path(win.wstring(), rel);
    fs::create_directories(p.parent_path());
    write_file_atomic(p.wstring(), "placeholder module file for the LongAfterDark.scr smoke tests\n");
  };
  char buf[64];
  for (int i = 1; i <= 23; ++i) {
    snprintf(buf, sizeof(buf), "%02d", i);
    add(std::string("ad.m") + buf, std::string("Alpha ") + buf, "pe32", std::string("FILES/AD40/A") + buf + ".AD", "deluxe");
  }
  for (int i = 1; i <= 61; ++i) {
    snprintf(buf, sizeof(buf), "%02d", i);
    add(std::string("cl.m") + buf, std::string("Beta ") + buf, "ne16", std::string("FILES/CLASSIC/B") + buf + ".AD",
        i <= 30 ? "ad32" : i <= 50 ? "tt" : "simpsons");
  }
  json.erase(json.size() - 2);
  json += "\n] }\n";
  CHECK(write_file_atomic((win / "catalog-win.json").wstring(), json));
}

int test_list_top(const Opts& o) {
  Work w = prepare(o, "list-top");
  // 48 renders from a catalog before packages: no strip, as before.
  write_list_top_catalog(w, false);
  const char* modules[] = {"ad.m23", "cl.m01", "cl.m30", "cl.m61"};
  const char* sizes[] = {"900x600", "950x640", "1100x700"};
  const int dpis[] = {96, 120, 168, 192};
  int shots = 0, n = 0;
  for (int dpi : dpis) {
    for (const char* size : sizes) {
      for (const char* module : modules) {
        const bool random = (n++ % 2) == 1;
        const std::string name = std::string(module) + "_" + size + "_" + std::to_string(dpi) + (random ? "_r" : "_s");
        shots += list_top_shot(o, w, name,
                               "theme=light;module=" + std::string(module) + ";mode=" + (random ? "random" : "single") +
                                   ";size=" + size,
                               dpi, "hidden");
      }
    }
  }
  CHECK(shots == 48);
  // With releases (COVERS.md §1.12): the strip at each size, compact under
  // 760 DIP, in both modes and in light, dark and high contrast, grouped by
  // release (the modules at the ends of groups), and some under a filter.
  write_list_top_catalog(w, true);
  const char* strip_sizes[] = {"900x680", "950x740", "1100x800"};
  const char* themes[] = {"light", "dark", "hc"};
  const char* filters[] = {"", "ad32,simpsons", "tt"};
  shots = n = 0;
  for (int dpi : dpis) {
    for (const char* size : strip_sizes) {
      for (int k = 0; k < 2; ++k, ++n) {
        const char* module = modules[(n + k) % 4];
        const bool random = n % 2 == 1;
        const char* theme = themes[n % 3];
        const char* filter = filters[(n / 2) % 3];
        const std::string name = std::string("strip_") + module + "_" + size + "_" + std::to_string(dpi) + "_" + theme +
                                 (random ? "_r" : "_s") + (*filter ? "_f" : "");
        int h = 0;
        sscanf(size, "%*dx%d", &h);
        shots += list_top_shot(o, w, name,
                               std::string("theme=") + theme + ";module=" + module + ";mode=" + (random ? "random" : "single") +
                                   ";size=" + size + (*filter ? std::string(";collections=") + filter : std::string()),
                               dpi, h < 760 ? "compact" : "regular");
      }
    }
  }
  CHECK(shots == 24);
  check_hosts_gone(w);
  return 0;
}

// ---- the box-cover strip (COVERS.md §1) --------------------------------------------------

// A tile drawn at test time: a vertical gradient with a large letter, 640x800
// like the importer's tile.png. Never real art.
bool synth_tile(const fs::path& png, COLORREF top, COLORREF bottom, wchar_t letter) {
  const int w = 640, h = 800;
  BITMAPINFO bi{};
  bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB, 0, 0, 0, 0, 0};
  void* bits = nullptr;
  HDC dc = CreateCompatibleDC(nullptr);
  HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!bmp || !bits) {
    DeleteDC(dc);
    return false;
  }
  HGDIOBJ old = SelectObject(dc, bmp);
  auto* px = static_cast<uint8_t*>(bits);
  for (int y = 0; y < h; ++y) {
    const double t = (double)y / (h - 1);
    const uint8_t r = (uint8_t)(GetRValue(top) + (GetRValue(bottom) - GetRValue(top)) * t);
    const uint8_t g = (uint8_t)(GetGValue(top) + (GetGValue(bottom) - GetGValue(top)) * t);
    const uint8_t b = (uint8_t)(GetBValue(top) + (GetBValue(bottom) - GetBValue(top)) * t);
    for (int x = 0; x < w; ++x) {
      uint8_t* p = px + ((size_t)y * w + x) * 4;
      p[0] = b, p[1] = g, p[2] = r, p[3] = 255;
    }
  }
  HFONT font = CreateFontW(-560, 0, 0, 0, FW_BLACK, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                           ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
  HGDIOBJ of = SelectObject(dc, font);
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, RGB(255, 255, 255));
  RECT all{0, 0, w, h};
  DrawTextW(dc, &letter, 1, &all, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
  GdiFlush();
  std::vector<uint8_t> bgr((size_t)w * h * 3);
  for (size_t i = 0; i < (size_t)w * h; ++i) {
    bgr[i * 3] = px[i * 4], bgr[i * 3 + 1] = px[i * 4 + 1], bgr[i * 3 + 2] = px[i * 4 + 2];
  }
  SelectObject(dc, of);
  DeleteObject(font);
  SelectObject(dc, old);
  DeleteObject(bmp);
  DeleteDC(dc);
  fs::create_directories(png.parent_path());
  std::string err;
  if (!adw::ui::save_png_bgr(png.wstring(), w, h, bgr, &err)) {
    failf("cannot write %s: %s", png.string().c_str(), err.c_str());
    return false;
  }
  return true;
}

// A tree shaped like the real merged catalog (five releases, sameAs copies,
// two builds under one name, every cover origin; placeholder names), with
// placeholder module files and synthesized tiles.
Work prepare_releases(const Opts& o, const std::string& name) {
  Work w = prepare(o, name, false);
  fs::path win = w.assets / "win";
  fs::create_directories(win);
  fs::copy_file(fs::path(o.fixtures) / "catalog-releases.json", win / "catalog-win.json");
  Catalog c;
  load_catalog((win / "catalog-win.json").wstring(), c, nullptr);
  for (const auto& m : c.modules) {
    fs::path p = resolve_module_path(win.wstring(), m.path);
    fs::create_directories(p.parent_path());
    write_file_atomic(p.wstring(), "placeholder module file for the LongAfterDark.scr smoke tests\n");
  }
  static const COLORREF kTop[] = {RGB(200, 60, 40), RGB(40, 120, 200), RGB(60, 160, 80), RGB(180, 60, 180), RGB(230, 190, 40)};
  for (size_t i = 0; i < c.releases.size(); ++i) {
    const Cover& cv = c.releases[i].cover;
    if (cv.generated()) continue;
    synth_tile(resolve_module_path(win.wstring(), cv.tile), kTop[i % 5], RGB(20, 20, 40), (wchar_t)(L'A' + i));
  }
  edit_settings(w, [](Settings& s) {
    s.module = "random";
    s.randomize.clear();
    s.controls.clear();
  });
  return w;
}

HWND strip_of(HWND dlg) { return GetDlgItem(dlg, IDC_COVER_STRIP); }
HWND tile_of(HWND dlg, int i) { return GetDlgItem(strip_of(dlg), IDC_COVER_TILE_BASE + i); }

// A tile clicked as the strip sees a click: its check toggles, then BN_CLICKED.
void click_tile(HWND dlg, int i) {
  HWND t = tile_of(dlg, i);
  const bool on = SendMessageW(t, BM_GETCHECK, 0, 0) == BST_CHECKED;
  SendMessageW(t, BM_SETCHECK, on ? BST_UNCHECKED : BST_CHECKED, 0);
  SendMessageW(strip_of(dlg), WM_COMMAND, MAKEWPARAM(IDC_COVER_TILE_BASE + i, BN_CLICKED), (LPARAM)t);
}

std::string tiles_on(HWND dlg) {
  std::string s;
  for (int i = 0; i < 16; ++i) {
    HWND t = tile_of(dlg, i);
    if (!t) break;
    s += SendMessageW(t, BM_GETCHECK, 0, 0) == BST_CHECKED ? '1' : '0';
  }
  return s;
}

// What UI Automation (a screen reader) makes of a window: its control type,
// name and, when it has the Toggle pattern, its toggle state.
struct A11y {
  bool ok = false;
  int control_type = 0;        // UIA_*ControlTypeId
  std::string name;
  std::string help;            // HelpText (MSAA's description)
  int toggle = -1;             // ToggleState, -1 without the Toggle pattern
  int live = -1;               // LiveSetting (0 off, 1 polite, 2 assertive)
};
A11y a11y_of(HWND h) {
  // CUIAutomation, IUIAutomation, IUIAutomationTogglePattern.
  static const GUID kClsid = {0xff48dba4, 0x60ef, 0x4201, {0xaa, 0x87, 0x54, 0x10, 0x3e, 0xef, 0x59, 0x4e}};
  static const GUID kIid = {0x30cbe57d, 0xd9d0, 0x452a, {0xab, 0x13, 0x7a, 0xc5, 0xac, 0x48, 0x25, 0xee}};
  static const GUID kToggleIid = {0x94cf8058, 0x9b8d, 0x4ab9, {0x8b, 0xfd, 0x4c, 0xd0, 0xa3, 0x3c, 0x8c, 0x70}};
  A11y a;
  const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  IUIAutomation* uia = nullptr;
  if (SUCCEEDED(CoCreateInstance(kClsid, nullptr, CLSCTX_INPROC_SERVER, kIid, reinterpret_cast<void**>(&uia))) && uia) {
    IUIAutomationElement* e = nullptr;
    if (SUCCEEDED(uia->ElementFromHandle(h, &e)) && e) {
      CONTROLTYPEID type = 0;
      BSTR name = nullptr;
      a.ok = SUCCEEDED(e->get_CurrentControlType(&type)) && SUCCEEDED(e->get_CurrentName(&name));
      a.control_type = type;
      if (name) {
        a.name = narrow(std::wstring(name, SysStringLen(name)));
        SysFreeString(name);
      }
      VARIANT live;
      VariantInit(&live);
      if (SUCCEEDED(e->GetCurrentPropertyValue(UIA_LiveSettingPropertyId, &live)) && live.vt == VT_I4) a.live = live.lVal;
      VariantClear(&live);
      VARIANT help;
      VariantInit(&help);
      if (SUCCEEDED(e->GetCurrentPropertyValue(UIA_HelpTextPropertyId, &help)) && help.vt == VT_BSTR && help.bstrVal) {
        a.help = narrow(std::wstring(help.bstrVal, SysStringLen(help.bstrVal)));
      }
      VariantClear(&help);
      IUIAutomationTogglePattern* tp = nullptr;
      if (SUCCEEDED(e->GetCurrentPatternAs(UIA_TogglePatternId, kToggleIid, reinterpret_cast<void**>(&tp))) && tp) {
        ToggleState ts = ToggleState_Off;
        if (SUCCEEDED(tp->get_CurrentToggleState(&ts))) a.toggle = (int)ts;
        tp->Release();
      }
      e->Release();
    }
    uia->Release();
  }
  if (SUCCEEDED(co)) CoUninitialize();
  return a;
}

// What UI Automation calls each row of a list view (its items, in order).
std::vector<std::string> list_item_names(HWND list) {
  static const GUID kClsid = {0xff48dba4, 0x60ef, 0x4201, {0xaa, 0x87, 0x54, 0x10, 0x3e, 0xef, 0x59, 0x4e}};
  static const GUID kIid = {0x30cbe57d, 0xd9d0, 0x452a, {0xab, 0x13, 0x7a, 0xc5, 0xac, 0x48, 0x25, 0xee}};
  std::vector<std::string> names;
  const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  IUIAutomation* uia = nullptr;
  if (SUCCEEDED(CoCreateInstance(kClsid, nullptr, CLSCTX_INPROC_SERVER, kIid, reinterpret_cast<void**>(&uia))) && uia) {
    IUIAutomationElement* root = nullptr;
    IUIAutomationCondition* cond = nullptr;
    VARIANT type;
    VariantInit(&type);
    type.vt = VT_I4;
    type.lVal = UIA_ListItemControlTypeId;
    if (SUCCEEDED(uia->ElementFromHandle(list, &root)) && root &&
        SUCCEEDED(uia->CreatePropertyCondition(UIA_ControlTypePropertyId, type, &cond)) && cond) {
      IUIAutomationElementArray* all = nullptr;
      if (SUCCEEDED(root->FindAll(TreeScope_Descendants, cond, &all)) && all) {
        int n = 0;
        all->get_Length(&n);
        for (int i = 0; i < n; ++i) {
          IUIAutomationElement* e = nullptr;
          BSTR name = nullptr;
          if (SUCCEEDED(all->GetElement(i, &e)) && e && SUCCEEDED(e->get_CurrentName(&name)) && name) {
            names.push_back(narrow(std::wstring(name, SysStringLen(name))));
            SysFreeString(name);
          }
          if (e) e->Release();
        }
        all->Release();
      }
    }
    if (cond) cond->Release();
    if (root) root->Release();
    uia->Release();
  }
  if (SUCCEEDED(co)) CoUninitialize();
  return names;
}

int group_count(HWND list) { return (int)SendMessageW(list, LVM_GETGROUPCOUNT, 0, 0); }
int item_count(HWND list) { return (int)SendMessageW(list, LVM_GETITEMCOUNT, 0, 0); }

// Drives the strip by control ID (COVERS.md §1.12): tiles filter and regroup
// the list; Random's checks are global and written from all of them; a
// Random with nothing checked in the releases shown is refused; OK writes
// Collections and the next dialog restores it; a filter never changes the
// chosen single module; with one release the strip is hidden and the key
// left as it was.
int test_config_collections(const Opts& o) {
  Work w = prepare_releases(o, "config-collections");
  EnvList env = base_env(o, w);
  using Ids = std::vector<std::string>;

  // ---- session 1: the strip, the filter, the global checks.
  HWND dlg = nullptr;
  int step = 0;
  bool box_seen = false;
  struct Seen {
    bool visible = false;
    std::string name4, status0, status1, count1, tiles;
    int groups0 = -1, items0 = -1, groups1 = -1, items1 = -1, groups2 = -1, items2 = -1;
    bool link0 = true, link1 = false;
    A11y on, off, status;
  } v;
  std::string before_refusal, after_refusal;
  RunResult r = run_scr(o, L"/c", env, 60000, [&](DWORD pid) {
    if (!dlg) {
      HWND d = find_dialog(pid);
      HWND list = d ? GetDlgItem(d, IDC_MODULE_LIST) : nullptr;
      if (!list || item_count(list) == 0 || !tile_of(d, 4)) return;
      dlg = d;
    }
    HWND list = GetDlgItem(dlg, IDC_MODULE_LIST);
    if (step == 0) {
      v.visible = IsWindowVisible(strip_of(dlg)) != FALSE;
      v.name4 = window_text(tile_of(dlg, 4));
      v.status0 = window_text(GetDlgItem(dlg, IDC_STRIP_STATUS));
      v.link0 = IsWindowVisible(GetDlgItem(dlg, IDC_STRIP_SHOW_ALL)) != FALSE;
      v.groups0 = group_count(list);
      v.items0 = item_count(list);
      click(dlg, IDC_CHECK_NONE);   // every row shown: all 18
      click_tile(dlg, 3);           // Totally Twisted
      v.on = a11y_of(tile_of(dlg, 3));
      v.off = a11y_of(tile_of(dlg, 0));
      v.status = a11y_of(GetDlgItem(dlg, IDC_STRIP_STATUS));
      v.groups1 = group_count(list);
      v.items1 = item_count(list);
      v.count1 = window_text(GetDlgItem(dlg, IDC_MODULES_COUNT));
      v.status1 = window_text(GetDlgItem(dlg, IDC_STRIP_STATUS));
      v.link1 = IsWindowVisible(GetDlgItem(dlg, IDC_STRIP_SHOW_ALL)) != FALSE;
      read_file(w.settings.wstring(), before_refusal);
      // Random with nothing checked in the releases shown: refused.
      PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), (LPARAM)GetDlgItem(dlg, IDOK));
      step = 1;
    } else if (step == 1) {
      HWND box = owned_box(dlg);
      if (!box) return;
      box_seen = true;
      read_file(w.settings.wstring(), after_refusal);
      if (HWND ok = FindWindowExW(box, nullptr, L"Button", nullptr)) PostMessageW(ok, BM_CLICK, 0, 0);
      else PostMessageW(box, WM_CLOSE, 0, 0);
      step = 2;
    } else if (step == 2) {
      if (owned_box(dlg)) return;
      // Totally Twisted's two checked, then the Simpsons' three, then only
      // Totally Twisted shown again: the Simpsons' checks are kept.
      click(dlg, IDC_CHECK_ALL);
      click_tile(dlg, 3);
      click_tile(dlg, 4);
      click(dlg, IDC_CHECK_ALL);
      click_tile(dlg, 4);
      click_tile(dlg, 3);
      v.groups2 = group_count(list);
      v.items2 = item_count(list);
      v.tiles = tiles_on(dlg);
      PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), (LPARAM)GetDlgItem(dlg, IDOK));
      step = 3;
    }
  });
  CHECK(step == 3);
  if (!expect_exit(w, r, 0)) return 1;
  CHECK(v.visible);
  CHECK(v.name4 == "The Simpsons Screen Saver, 3 screen savers");
  CHECK(v.status0 == "Click covers to filter the list" && !v.link0);
  CHECK(v.groups0 == 5 && v.items0 == 18);
  CHECK(v.groups1 == 1 && v.items1 == 2 && v.count1 == "2 of 18");
  CHECK(v.status1 == "Showing 1 of 5 releases" && v.link1);
  // A screen reader meets a check box with the Toggle pattern (COVERS.md §1.5).
  printf("tile a11y: control type %d, \"%s\", toggle %d / %d\n", v.on.control_type, v.on.name.c_str(), v.on.toggle,
         v.off.toggle);
  CHECK(v.on.ok && v.on.control_type == UIA_CheckBoxControlTypeId);
  CHECK(v.on.name == "Totally Twisted After Dark, 2 screen savers");
  CHECK(v.on.toggle == ToggleState_On && v.off.toggle == ToggleState_Off);
  // The status line is a polite live region, read out as it changes.
  CHECK(v.status.ok && v.status.name == "Showing 1 of 5 releases" && v.status.live == 1);
  CHECK(box_seen && after_refusal == before_refusal);
  CHECK(v.groups2 == 1 && v.items2 == 2 && v.tiles == "00010");
  Settings s;
  CHECK(load_settings(w.settings.wstring(), s));
  CHECK((s.collections == Ids{"tt"}));
  CHECK(s.module == "random");
  // Written from every release's checks, in list order (releases in
  // packages[] order, rows by name).
  CHECK((s.randomize == Ids{"tt.beta", "tt.foxtrot", "simpsons.hotel", "simpsons.india", "simpsons.juliet"}));
  std::string text;
  read_file(w.settings.wstring(), text);
  if (g_failures) fprintf(stderr, "---- settings.ini after session 1\n%s\n", text.c_str());

  // ---- session 2: reopened, the filter is back; "Show all", then every
  // tile selected, saves "all".
  dlg = nullptr;
  step = 0;
  std::string tiles_open, tiles_after_all, status_all;
  int groups_open = -1, checked_open = -1, groups_all = -1;
  r = run_scr(o, L"/c", env, 60000, [&](DWORD pid) {
    if (step) return;
    HWND d = find_dialog(pid);
    HWND list = d ? GetDlgItem(d, IDC_MODULE_LIST) : nullptr;
    if (!list || item_count(list) == 0 || !tile_of(d, 4)) return;
    tiles_open = tiles_on(d);
    groups_open = group_count(list);
    checked_open = checked_count(list);
    click(d, IDC_STRIP_SHOW_ALL);
    groups_all = group_count(list);
    for (int i = 0; i < 5; ++i) click_tile(d, i);
    tiles_after_all = tiles_on(d);
    status_all = window_text(GetDlgItem(d, IDC_STRIP_STATUS));
    PostMessageW(d, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), (LPARAM)GetDlgItem(d, IDOK));
    step = 1;
  });
  CHECK(step == 1);
  if (!expect_exit(w, r, 0)) return 1;
  CHECK(tiles_open == "00010" && groups_open == 1 && checked_open == 2);
  CHECK(groups_all == 5 && tiles_after_all == "11111" && status_all == "Showing all 5 releases");
  CHECK(load_settings(w.settings.wstring(), s) && s.collections.empty());
  read_file(w.settings.wstring(), text);
  CHECK(text.find("Collections=\r\n") != std::string::npos);

  // ---- session 3: Single module with a filter hiding its release: no row
  // is selected, the details show the first listed module instead (never a
  // hidden one), and OK keeps the chosen module all the same.
  edit_settings(w, [](Settings& s) {
    s.module = "ad40.alpha";
    s.randomize.clear();
    s.collections = {"simpsons"};
  });
  step = 0;
  std::string title;
  LRESULT selected = -2, random = -1;
  int items3 = -1;
  r = run_scr(o, L"/c", env, 60000, [&](DWORD pid) {
    if (step) return;
    HWND d = find_dialog(pid);
    HWND list = d ? GetDlgItem(d, IDC_MODULE_LIST) : nullptr;
    if (!list || item_count(list) == 0) return;
    title = window_text(GetDlgItem(d, IDC_MODULE_TITLE));
    selected = SendMessageW(list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    random = SendMessageW(GetDlgItem(d, IDC_MODE_RANDOM), BM_GETCHECK, 0, 0);
    items3 = item_count(list);
    PostMessageW(d, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), (LPARAM)GetDlgItem(d, IDOK));
    step = 1;
  });
  CHECK(step == 1);
  if (!expect_exit(w, r, 0)) return 1;
  CHECK(title == "Hotel Donut" && selected == -1 && random == BST_UNCHECKED && items3 == 3);
  CHECK(load_settings(w.settings.wstring(), s) && s.module == "ad40.alpha" && (s.collections == Ids{"simpsons"}));

  // ---- session 4: one release installed: no strip, and OK leaves
  // Collections exactly as written.
  {
    std::string cat;
    read_file((w.assets / "win" / "catalog-win.json").wstring(), cat);
    phosg::JSON root = phosg::JSON::parse(cat);
    phosg::JSON one = phosg::JSON::dict();
    one.emplace("version", 1);
    one.emplace("packages", phosg::JSON::list({*root.at("packages").as_list()[0]}));
    phosg::JSON mods = phosg::JSON::list();
    for (const auto& m : root.at("modules").as_list()) {
      if (m->at("package").as_string() == "deluxe") mods.emplace_back(phosg::JSON(*m));
    }
    one.emplace("modules", std::move(mods));
    CHECK(write_file_atomic((w.assets / "win" / "catalog-win.json").wstring(), one.serialize()));
  }
  std::string one_before;
  CHECK(write_file_atomic(w.settings.wstring(), "[Saver]\r\nModule=random\r\nCollections=tt, bogus\r\n"));
  step = 0;
  bool strip_visible = true;
  int groups4 = -1;
  r = run_scr(o, L"/c", env, 60000, [&](DWORD pid) {
    if (step) return;
    HWND d = find_dialog(pid);
    HWND list = d ? GetDlgItem(d, IDC_MODULE_LIST) : nullptr;
    if (!list || item_count(list) == 0) return;
    strip_visible = IsWindowVisible(strip_of(d)) || IsWindowVisible(GetDlgItem(d, IDC_STRIP_STATUS));
    groups4 = group_count(list);
    PostMessageW(d, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), (LPARAM)GetDlgItem(d, IDOK));
    step = 1;
  });
  CHECK(step == 1);
  if (!expect_exit(w, r, 0)) return 1;
  CHECK(!strip_visible && groups4 == 1);
  read_file(w.settings.wstring(), text);
  CHECK(text.find("Collections=tt, bogus\r\n") != std::string::npos);
  if (g_failures) fprintf(stderr, "---- settings.ini at the end\n%s\n", text.c_str());
  return 0;
}

// The popup menu (#32768) the dialog's thread has open, or nullptr.
HWND open_menu(DWORD pid) {
  struct Find { DWORD pid; HWND found; } f{pid, nullptr};
  EnumWindows([](HWND h, LPARAM lp) -> BOOL {
    auto* f = reinterpret_cast<Find*>(lp);
    DWORD p = 0;
    GetWindowThreadProcessId(h, &p);
    wchar_t cls[16] = {};
    GetClassNameW(h, cls, 16);
    if (p == f->pid && IsWindowVisible(h) && wcscmp(cls, L"#32768") == 0) {
      f->found = h;
      return FALSE;
    }
    return TRUE;
  }, reinterpret_cast<LPARAM>(&f));
  return f.found;
}

// "Change cover…" from a tile's context menu (COVERS.md §1.11), against
// fakeimport.exe: it starts `--gui --change-cover <id>` with no console
// window; meanwhile Import… and the menu item are greyed; exit 0 reloads the
// catalog (keeping the filter), exit 5 changes nothing. A reload asks the
// host nothing again and leaves the live preview running.
int test_config_cover(const Opts& o) {
  Work w = prepare_releases(o, "config-cover");
  fs::path catalog = w.assets / "win" / "catalog-win.json";
  std::string original;
  CHECK(read_file(catalog.wstring(), original));
  // What a changed cover leaves: the Simpsons' tile from the user's picture.
  std::string changed = original;
  const std::string from = "\"tileMd5\": \"00000000000000000000000000000005\"";
  if (size_t at = changed.find(from); at != std::string::npos) {
    changed.replace(at, from.size(), "\"tileMd5\": \"000000000000000000000000000000ff\"");
  } else {
    failf("fixture: no Simpsons tileMd5");
  }
  fs::path changed_path = w.dir / "catalog-changed.json";
  CHECK(write_file_atomic(changed_path.wstring(), changed));

  struct Case {
    const wchar_t* exit;
    int reloads;
  };
  for (const Case& c : {Case{L"5", 0}, Case{L"0", 1}}) {
    CHECK(write_file_atomic(catalog.wstring(), original));
    std::error_code ec;
    fs::remove(w.scr_log, ec);
    EnvList env = base_env(o, w);
    env.push_back({L"FAKEIMPORT_EXIT", c.exit});
    env.push_back({L"FAKEIMPORT_CATALOG", changed_path.wstring()});
    env.push_back({L"FAKEIMPORT_WAIT_MS", L"1500"});
    HWND dlg = nullptr;
    int step = 0;
    ULONGLONG t0 = 0;
    bool import_greyed = false, item_greyed = false, item_enabled_before = false;
    std::string tiles_after;
    int groups_after = -1;
    RunResult r = run_scr(o, L"/c", env, 60000, [&](DWORD pid) {
      if (!dlg) {
        HWND d = find_dialog(pid);
        HWND list = d ? GetDlgItem(d, IDC_MODULE_LIST) : nullptr;
        if (!list || item_count(list) == 0 || !tile_of(d, 4)) return;
        dlg = d;
        click_tile(dlg, 3);   // a filter to keep
      }
      if (step == 0) {
        // The Simpsons tile's menu, from the keyboard (Shift+F10): "Change cover…" is its last item.
        PostMessageW(strip_of(dlg), WM_CONTEXTMENU, (WPARAM)tile_of(dlg, 4), (LPARAM)-1);
        t0 = GetTickCount64();
        step = 1;
      } else if (step == 1) {
        HWND menu = open_menu(pid);
        if (!menu) {
          if (GetTickCount64() - t0 > 5000) {
            failf("no context menu");
            step = 9;
            PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
          }
          return;
        }
        HMENU hm = (HMENU)SendMessageW(menu, MN_GETHMENU, 0, 0);
        item_enabled_before = hm && !(GetMenuState(hm, 3, MF_BYCOMMAND) & MF_GRAYED);
        PostMessageW(menu, WM_KEYDOWN, VK_UP, 0);
        PostMessageW(menu, WM_KEYDOWN, VK_RETURN, 0);
        t0 = GetTickCount64();
        step = 2;
      } else if (step == 2) {
        // Running: Import… is greyed, and so is "Change cover…".
        if (IsWindowEnabled(GetDlgItem(dlg, IDC_IMPORT))) {
          if (GetTickCount64() - t0 > 5000) {
            failf("Change cover did not start");
            step = 9;
            PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
          }
          return;
        }
        import_greyed = true;
        PostMessageW(strip_of(dlg), WM_CONTEXTMENU, (WPARAM)tile_of(dlg, 4), (LPARAM)-1);
        t0 = GetTickCount64();
        step = 3;
      } else if (step == 3) {
        HWND menu = open_menu(pid);
        if (!menu) return;
        HMENU hm = (HMENU)SendMessageW(menu, MN_GETHMENU, 0, 0);
        item_greyed = hm && (GetMenuState(hm, 3, MF_BYCOMMAND) & MF_GRAYED);
        PostMessageW(menu, WM_KEYDOWN, VK_ESCAPE, 0);
        step = 4;
      } else if (step == 4) {
        if (open_menu(pid) || !IsWindowEnabled(GetDlgItem(dlg, IDC_IMPORT))) return;   // until adimport has exited
        tiles_after = tiles_on(dlg);
        groups_after = group_count(GetDlgItem(dlg, IDC_MODULE_LIST));
        PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
        step = 5;
      }
    });
    CHECK(step == 5);
    if (!expect_exit(w, r, 0)) return 1;
    CHECK(item_enabled_before && import_greyed && item_greyed);
    CHECK(tiles_after == "00010" && groups_after == 1);   // the filter kept, reloaded or not
    CHECK(count_in_log(w.scr_log, "dialog: change cover simpsons exited with " + narrow(c.exit)) == 1);
    CHECK(count_in_log(w.scr_log, "dialog: catalog reloaded") == c.reloads);
    if (g_failures) dump_logs(w);
  }
  auto runs = lines_of(w.dir / "fakeimport.log");
  CHECK(runs.size() == 2);
  for (const auto& l : runs) {
    if (l != "run\tconsole=0\targs=--gui --change-cover simpsons") failf("importer run: %s", l.c_str());
  }

  // Across the reload the host is the same program: its answer stands (it
  // is asked once), and the module the details show (a pe32 one here) keeps
  // its live preview, one host from start to end, never stopped for a
  // question already answered.
  CHECK(write_file_atomic(catalog.wstring(), original));
  edit_settings(w, [](Settings& s) {
    s.module = "ad40.alpha";
    s.randomize.clear();
  });
  std::error_code ec;
  fs::remove(w.scr_log, ec);
  fs::remove(w.host_log, ec);
  EnvList env = base_env(o, w);
  env.push_back({L"FAKEIMPORT_EXIT", L"0"});
  env.push_back({L"FAKEIMPORT_CATALOG", changed_path.wstring()});
  env.push_back({L"FAKEIMPORT_WAIT_MS", L"300"});
  HWND dlg = nullptr;
  int step = 0;
  ULONGLONG t0 = 0;
  DWORD dialog_pid = 0;
  RunResult r = run_scr(o, L"/c", env, 60000, [&](DWORD pid) {
    dialog_pid = pid;
    if (!dlg) {
      HWND d = find_dialog(pid);
      HWND list = d ? GetDlgItem(d, IDC_MODULE_LIST) : nullptr;
      if (!list || item_count(list) == 0 || !tile_of(d, 4)) return;
      dlg = d;
    }
    auto give_up = [&](const char* why) {
      if (GetTickCount64() - t0 < 5000) return;
      failf("%s", why);
      step = 9;
      PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
    };
    if (step == 0) {
      // The preview has run a while: then "Change cover…" on the Simpsons' tile.
      if (hosts_of(w, pid).empty()) return;
      if (!t0) t0 = GetTickCount64();
      if (GetTickCount64() - t0 < 1000) return;
      PostMessageW(strip_of(dlg), WM_CONTEXTMENU, (WPARAM)tile_of(dlg, 4), (LPARAM)-1);
      t0 = GetTickCount64();
      step = 1;
    } else if (step == 1) {
      HWND menu = open_menu(pid);
      if (!menu) return give_up("no context menu");
      PostMessageW(menu, WM_KEYDOWN, VK_UP, 0);
      PostMessageW(menu, WM_KEYDOWN, VK_RETURN, 0);
      t0 = GetTickCount64();
      step = 2;
    } else if (step == 2) {
      if (IsWindowEnabled(GetDlgItem(dlg, IDC_IMPORT))) return give_up("Change cover did not start");
      step = 3;
    } else if (step == 3) {
      if (!IsWindowEnabled(GetDlgItem(dlg, IDC_IMPORT))) return;   // until adimport has exited
      t0 = GetTickCount64();
      step = 4;
    } else if (step == 4) {
      if (GetTickCount64() - t0 < 1500) return;   // time for a preview restarted after it to show
      PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
      step = 5;
    }
  });
  CHECK(step == 5);
  if (!expect_exit(w, r, 0)) return 1;
  CHECK(count_in_log(w.scr_log, "dialog: catalog reloaded") == 1);
  CHECK(count_in_log(w.scr_log, "dialog: host capabilities") == 1);
  CHECK(count_in_log(w.scr_log, "live preview: spawn") == 1);
  int alpha = 0;
  for (auto& e : hosts_of(w, dialog_pid)) alpha += ends_with(e["module"], "ALPHA.AD");
  CHECK(alpha == 1);
  if (g_failures) dump_logs(w);
  return 0;
}

// Random under Collections (COVERS.md §1.8): only the selected releases
// play, and byte-identical copies once per pass.
int test_rotate_collections(const Opts& o) {
  Work w = prepare_releases(o, "rotate-collections");
  edit_settings(w, [](Settings& s) {
    s.module = "random";
    s.randomize.clear();
    s.collections = {"simpsons"};
    s.all_monitors = false;
  });
  EnvList env = base_env(o, w);
  env.push_back({L"AD_SCR_TEST_ROTATE_MS", L"400"});
  env.push_back({L"AD_SCR_TESTEXIT_AFTER_FRAMES", L"120"});
  RunResult r = run_scr(o, L"/s", env, 60000);
  if (!expect_exit(w, r, 0)) return 1;
  std::set<std::string> modules;
  for (auto& s : host_events(w, "start")) {
    modules.insert(s["module"]);
    if (s["module"].find("SIMPSONS") == std::string::npos) failf("a module outside the filter played: %s", s["module"].c_str());
  }
  CHECK(modules.size() >= 2);
  CHECK(count_in_log(w.scr_log, "rotation: 3 module(s), collections=simpsons") == 1);
  check_hosts_gone(w);
  // Deluxe and 3.2 share Beta Fish's bytes: it is in the bag once.
  fs::remove(w.scr_log);
  fs::remove(w.host_log);
  edit_settings(w, [](Settings& s) { s.collections = {"ad32", "deluxe"}; });
  env = base_env(o, w);
  env.push_back({L"AD_SCR_TESTEXIT_AFTER_FRAMES", L"5"});
  r = run_scr(o, L"/s", env, 60000);
  if (!expect_exit(w, r, 0)) return 1;
  CHECK(count_in_log(w.scr_log, "rotation: 8 module(s), collections=deluxe,ad32") == 1);
  check_hosts_gone(w);
  if (g_failures) dump_logs(w);
  return 0;
}

// ---- six releases: another module ABI (Star Wars Screen Entertainment) --------------------

// The six-release tree (catalog-six.json): the five After Dark releases'
// entries and Star Wars Screen Entertainment's 14 Intermission modules (lane
// ne16, abi intermission, one Configure... button each), with placeholder
// module files (*.IMX for theirs) and synthesized cover tiles. With
// catalog-seven.json, Star Trek: The Screen Saver's four too (lane ne16,
// "screen": "640x480" each).
Work prepare_six(const Opts& o, const std::string& name, const char* fixture = "catalog-six.json") {
  Work w = prepare(o, name, false);
  fs::path win = w.assets / "win";
  fs::create_directories(win);
  fs::copy_file(fs::path(o.fixtures) / fixture, win / "catalog-win.json");
  Catalog c;
  load_catalog((win / "catalog-win.json").wstring(), c, nullptr);
  for (const auto& m : c.modules) {
    fs::path p = resolve_module_path(win.wstring(), m.path);
    fs::create_directories(p.parent_path());
    write_file_atomic(p.wstring(), "placeholder module file for the LongAfterDark.scr smoke tests\n");
  }
  static const COLORREF kTop[] = {RGB(230, 190, 40), RGB(90, 90, 110), RGB(60, 160, 80), RGB(180, 60, 180),
                                  RGB(200, 60, 40), RGB(40, 120, 200)};
  for (size_t i = 0; i < c.releases.size(); ++i) {
    const Cover& cv = c.releases[i].cover;
    if (cv.generated()) continue;
    synth_tile(resolve_module_path(win.wstring(), cv.tile), kTop[i % 6], RGB(20, 20, 40), (wchar_t)(L'A' + i));
  }
  edit_settings(w, [](Settings& s) {
    s.module = "random";
    s.randomize.clear();
    s.controls.clear();
  });
  return w;
}

// A picture already taken for every module of the tree but `except` (the
// dialog's thumbnails folder, `thumbs` next to settings.ini), so that its
// background queue has only those to take. Synthesized, never real art.
void seed_thumbs(const Work& w, const std::set<std::string>& except) {
  Catalog c;
  load_catalog((w.assets / "win" / "catalog-win.json").wstring(), c, nullptr);
  const int n = 96;
  std::vector<uint8_t> bgr((size_t)n * n * 3);
  for (int y = 0; y < n; ++y) {
    for (int x = 0; x < n; ++x) {
      uint8_t* p = &bgr[((size_t)y * n + x) * 3];
      p[0] = (uint8_t)(x * 2), p[1] = (uint8_t)(y * 2), p[2] = (uint8_t)((x + y) & 0xFF);
    }
  }
  fs::create_directories(w.dir / "thumbs");
  for (const auto& m : c.modules) {
    if (except.count(m.id)) continue;
    std::string err;
    if (!adw::ui::save_png_bgr((w.dir / "thumbs" / (m.id + ".v2.png")).wstring(), n, n, bgr, &err)) {
      failf("cannot write a thumbnail for %s: %s", m.id.c_str(), err.c_str());
    }
  }
}

// One off-screen render of the settings dialog (the screenshot hook: never
// on screen, never focused) with `state`, and its report.
std::map<std::string, std::string> dialog_report(const Opts& o, const Work& w, const std::string& state,
                                                 const EnvList& extra = {}) {
  const fs::path png = w.dir / "dialog.png", report = w.dir / "report.txt";
  EnvList env = base_env(o, w);
  env.insert(env.end(), extra.begin(), extra.end());
  env.push_back({L"AD_SCR_TEST_SCREENSHOT", png.wstring()});
  env.push_back({L"AD_SCR_TEST_SCREENSHOT_STATE", widen(state + ";report=") + report.wstring()});
  RunResult r = run_scr(o, L"/c", env, 90000);
  std::map<std::string, std::string> kv;
  if (!expect_exit(w, r, 0)) return kv;
  for (const auto& l : lines_of(report)) {
    size_t eq = l.find('=');
    if (eq != std::string::npos) kv[l.substr(0, eq)] = l.substr(eq + 1);
  }
  if (kv.empty()) failf("%s: no report", w.dir.string().c_str());
  return kv;
}

bool started_imx(const Work& w) {
  for (auto& e : host_events(w, "start")) {
    if (ends_with(e["module"], ".IMX")) return true;
  }
  return false;
}

// The settings dialog with a module ABI a host may lack (PLAN: catalog "abi",
// --capabilities "abis="), rendered off screen against fakehost:
//  * today's host (abis=afterdark,intermission): Star Wars Screen
//    Entertainment's modules run: previewed, a live Configure...;
//  * a host from before module ABIs (no abis=): they are "Coming soon",
//    dimmed, never started, not even for a thumbnail, their button read-only,
//    Preview greyed, the group's pill up; the rest run;
//  * one module's exit 3 (its preview, or its thumbnail) makes that module
//    alone "Coming soon", never every module of its lane or ABI;
//  * at the minimum window in Random, the long release title is ellipsized
//    so that its count shows.
int test_config_abi(const Opts& o) {
  const std::string swse_ids = "swse.battles,swse.bios,swse.bluprint,swse.cantina,swse.hyperspc,swse.iclock,swse.jawas,"
                               "swse.posters,swse.rclock,swse.sabrduel,swse.storybrd,swse.swtext,swse.trench,swse.vader";
  // Today's host.
  {
    Work w = prepare_six(o, "config-abi-host");
    auto kv = dialog_report(o, w, "theme=light;mode=single;module=swse.vader;size=1040x800;wait=6000;frames=3");
    CHECK(kv["caps"].find(" abis=afterdark,intermission ") != std::string::npos);
    CHECK(kv["details"] == "swse.vader" && kv["badge"] == "Star Wars" && kv["soon"].empty());
    CHECK(kv["button_live"] == "1" && kv["preview_enabled"] == "1");
    bool vader = false;
    for (auto& e : host_events(w, "start")) vader |= ends_with(e["module"], "VADER.IMX");
    CHECK(vader);   // the live preview ran it
    CHECK(host_events(w, "capabilities").size() == 1);
    check_hosts_gone(w);
    if (g_failures) dump_logs(w);
  }
  // A host from before module ABIs; the background thumbnails on (every
  // module but theirs already has a picture: the queue could take only
  // theirs, and must not).
  {
    Work w = prepare_six(o, "config-abi-old");
    std::set<std::string> theirs;
    for (size_t p = 0; p < swse_ids.size();) {
      size_t comma = swse_ids.find(',', p);
      theirs.insert(swse_ids.substr(p, comma == std::string::npos ? std::string::npos : comma - p));
      if (comma == std::string::npos) break;
      p = comma + 1;
    }
    seed_thumbs(w, theirs);
    auto kv = dialog_report(o, w, "theme=light;mode=random;module=swse.vader;size=1040x800;thumbgen=wait;wait=4000",
                            {{L"FAKEHOST_ABIS", L"none"}});
    CHECK(!kv["caps"].empty() && kv["caps"].find("abis=") == std::string::npos);
    CHECK(kv["soon"] == swse_ids);
    CHECK(kv["badge"] == "Star Wars \xC2\xB7 Coming soon" && kv["button_live"] == "0" && kv["preview_enabled"] == "0");
    CHECK(!started_imx(w));   // no preview, no thumbnail of theirs
    for (const auto& id : theirs) CHECK(!fs::exists(w.dir / "thumbs" / (id + ".v2.png")));
    check_hosts_gone(w);
    if (g_failures) dump_logs(w);
  }
  // One module's preview exits 3 (a host that lists everything): it alone
  // is "Coming soon"; the other 13, and every Classic module, still run.
  {
    Work w = prepare_six(o, "config-abi-exit3");
    auto kv = dialog_report(o, w, "theme=light;mode=single;module=swse.battles;size=1040x800;wait=5000;frames=1000",
                            {{L"FAKEHOST_EXIT3_MODULE", L"BATTLES.IMX"}});
    CHECK(kv["soon"] == "swse.battles");
    CHECK(kv["details"] == "swse.battles" && kv["badge"] == "Star Wars \xC2\xB7 Coming soon" && kv["button_live"] == "0");
    CHECK(count_in_log(w.scr_log, "swse.battles exited 3") == 1);
    check_hosts_gone(w);
    if (g_failures) dump_logs(w);
  }
  // ...and so does a module whose thumbnail's host exits 3 (the queue runs
  // the two without a picture: that one, and one that works).
  {
    Work w = prepare_six(o, "config-abi-thumb3");
    seed_thumbs(w, {"swse.jawas", "swse.cantina"});
    auto kv = dialog_report(o, w, "theme=light;mode=single;module=swse.vader;size=1040x800;thumbgen=wait;wait=30000",
                            {{L"FAKEHOST_EXIT3_MODULE", L"JAWAS.IMX"}});
    CHECK(kv["soon"] == "swse.jawas");
    CHECK(kv["details"] == "swse.vader" && kv["badge"] == "Star Wars" && kv["button_live"] == "1");
    CHECK(fs::exists(w.dir / "thumbs" / "swse.cantina.v2.png") && !fs::exists(w.dir / "thumbs" / "swse.jawas.v2.png"));
    check_hosts_gone(w);
    if (g_failures) dump_logs(w);
  }
  // The narrowest window in Random: "Star Wars Screen Entertainment" gives
  // way (its count shows), with or without its "Coming soon" pill; the After
  // Dark titles are drawn whole without one.
  for (bool old_host : {false, true}) {
    Work w = prepare_six(o, old_host ? "config-abi-min-old" : "config-abi-min");
    EnvList extra;
    if (old_host) extra.push_back({L"FAKEHOST_ABIS", L"none"});
    // The first row: the list opens at its top, the first two headers in view.
    auto kv = dialog_report(o, w, "theme=light;mode=random;module=simpsons.hotel;size=900x680;dpi=96;wait=2500;frames=2",
                            extra);
    const std::string title = "Star Wars Screen Entertainment", drawn = kv["drawn1"];
    CHECK(kv["group1"].rfind(title + ", ", 0) == 0);   // screen readers hear it whole
    CHECK(ends_with(drawn, "\xE2\x80\xA6") && drawn.size() > 3 && title.rfind(drawn.substr(0, drawn.size() - 3), 0) == 0);
    // Without the pill, the After Dark titles in view are drawn whole.
    CHECK(kv.count("drawn0") == 1);
    for (int g = 0; !old_host && g < 6; ++g) {
      const std::string k = std::to_string(g);
      if (g == 1 || !kv.count("drawn" + k)) continue;
      CHECK(kv["group" + k].rfind(kv["drawn" + k] + ", ", 0) == 0);
    }
    check_hosts_gone(w);
    if (g_failures) dump_logs(w);
  }
  return 0;
}

// The tree with Star Wars Screen Entertainment alone imported: the six
// releases' catalog cut down to its package and its 14 modules.
Work prepare_swse_only(const Opts& o, const std::string& name) {
  Work w = prepare_six(o, name);
  const fs::path catalog = w.assets / "win" / "catalog-win.json";
  std::string text;
  read_file(catalog.wstring(), text);
  phosg::JSON root = phosg::JSON::parse(text);
  phosg::JSON one = phosg::JSON::dict();
  one.emplace("version", 1);
  phosg::JSON packages = phosg::JSON::list(), modules = phosg::JSON::list();
  for (const auto& p : root.at("packages").as_list()) {
    if (p->at("id").as_string() == "swse") packages.emplace_back(phosg::JSON(*p));
  }
  for (const auto& m : root.at("modules").as_list()) {
    if (m->at("package").as_string() == "swse") modules.emplace_back(phosg::JSON(*m));
  }
  one.emplace("packages", std::move(packages));
  one.emplace("modules", std::move(modules));
  CHECK(write_file_atomic(catalog.wstring(), one.serialize()));
  return w;
}

// /s and /p with Star Wars Screen Entertainment imported: on a host from
// before module ABIs, Random leaves their 14 modules out (asking the host
// first: "rotation: left out 14 module(s)", counting only what the rotation
// would have held), and a list of only theirs falls back to what it can run;
// with nothing else imported, nothing plays and the window says why (never
// their modules into errors, one after another); on today's host they rotate
// like the rest. /s runs on one monitor staged off every real one (it never
// covers them).
int test_rotate_abi(const Opts& o) {
  Work w = prepare_six(o, "rotate-abi");
  edit_settings(w, [](Settings& s) { s.all_monitors = false; });
  auto run_s = [&](const Work& tree, const EnvList& extra, const char* frames, DWORD code = 0) {
    fs::remove(tree.scr_log);
    fs::remove(tree.host_log);
    EnvList env = base_env(o, tree);
    env.insert(env.end(), extra.begin(), extra.end());
    env.push_back({L"AD_SCR_TEST_MONITORS", L"-16000,0,856,480,p"});
    env.push_back({L"AD_SCR_TEST_ROTATE_MS", L"300"});
    env.push_back({L"AD_SCR_TESTEXIT_AFTER_FRAMES", widen(frames)});
    RunResult r = run_scr(o, L"/s", env, 60000);
    return expect_exit(tree, r, code);
  };
  auto run_p = [&](const Work& tree, const EnvList& extra, DWORD code = 0) {
    fs::remove(tree.scr_log);
    fs::remove(tree.host_log);
    HWND parent = make_parent(152, 112);
    EnvList env = base_env(o, tree);
    env.insert(env.end(), extra.begin(), extra.end());
    env.push_back({L"AD_SCR_TESTEXIT_AFTER_FRAMES", L"10"});
    RunResult r = run_scr(o, L"/p " + hwnd_arg(parent), env, 60000);
    DestroyWindow(parent);
    return expect_exit(tree, r, code);
  };
  const EnvList old_host = {{L"FAKEHOST_ABIS", L"none"}};
  // A host from before module ABIs: none of theirs is started.
  if (!run_s(w, old_host, "60")) return 1;
  CHECK(count_in_log(w.scr_log, "rotation: 13 module(s), collections=all") == 1);
  CHECK(count_in_log(w.scr_log, "rotation: left out 14 module(s) this host can't run") == 1);
  CHECK(!started_imx(w) && host_events(w, "start").size() >= 2);
  check_hosts_gone(w);
  // ...not even when the list names only theirs: every module it can run
  // (and "left out" counts the two the list held).
  edit_settings(w, [](Settings& s) { s.randomize = {"swse.vader", "swse.jawas"}; });
  if (!run_s(w, old_host, "30")) return 1;
  CHECK(count_in_log(w.scr_log, "rotation: 13 module(s), collections=all") == 1);
  CHECK(count_in_log(w.scr_log, "rotation: left out 2 module(s) this host can't run") == 1);
  CHECK(!started_imx(w));
  check_hosts_gone(w);
  // A list of one of theirs and one After Dark module: that one plays alone.
  edit_settings(w, [](Settings& s) { s.randomize = {"swse.vader", "ad40.alpha"}; });
  if (!run_s(w, old_host, "20")) return 1;
  CHECK(count_in_log(w.scr_log, "rotation: 1 module(s), collections=all") == 1);
  CHECK(count_in_log(w.scr_log, "rotation: left out 1 module(s) this host can't run") == 1);
  CHECK(!started_imx(w) && !host_events(w, "start").empty());
  for (auto& e : host_events(w, "start")) CHECK(ends_with(e["module"], "ALPHA.AD"));
  check_hosts_gone(w);
  edit_settings(w, [](Settings& s) { s.randomize = {"swse.vader", "swse.jawas"}; });
  // Today's host: they rotate, and nothing is left out.
  if (!run_s(w, {}, "90")) return 1;
  CHECK(count_in_log(w.scr_log, "rotation: 2 module(s), collections=all") == 1);
  CHECK(count_in_log(w.scr_log, "left out") == 0);
  std::set<std::string> played;
  for (auto& e : host_events(w, "start")) played.insert(e["module"]);
  CHECK(played.size() == 2);
  for (const auto& m : played) CHECK(ends_with(m, "VADER.IMX") || ends_with(m, "JAWAS.IMX"));
  check_hosts_gone(w);
  // The Control Panel's /p asks the host too, and leaves theirs out.
  edit_settings(w, [](Settings& s) { s.randomize.clear(); });
  if (!run_p(w, old_host)) return 1;
  CHECK(host_events(w, "capabilities").size() == 1 && !started_imx(w) && host_events(w, "start").size() == 1);
  CHECK(count_in_log(w.scr_log, "rotation: left out 14 module(s) this host can't run") == 1);
  check_hosts_gone(w);
  if (g_failures) dump_logs(w);

  // Theirs alone: a host from before module ABIs can run none of them. No
  // host starts, and the window says so (the test hook's exit 13).
  Work sw = prepare_swse_only(o, "rotate-abi-swse");
  edit_settings(sw, [](Settings& s) { s.all_monitors = false; });
  if (!run_s(sw, old_host, "30", 13)) return 1;
  CHECK(host_events(sw, "capabilities").size() == 1 && host_events(sw, "start").empty());
  CHECK(count_in_log(sw.scr_log, "rotation: 0 module(s), collections=all") == 1);
  CHECK(count_in_log(sw.scr_log, "rotation: left out 14 module(s) this host can't run") == 1);
  CHECK(count_in_log(sw.scr_log, "status window=0: None of the modules imported can run on this Long After Dark host "
                                 "(adhostwin.exe).") == 1);
  if (!run_p(sw, old_host, 13)) return 1;
  CHECK(host_events(sw, "capabilities").size() == 1 && host_events(sw, "start").empty());
  CHECK(count_in_log(sw.scr_log, "status window=0: No module can run on this host") == 1);
  // ...which today's host runs.
  if (!run_s(sw, {}, "30")) return 1;
  CHECK(started_imx(sw) && count_in_log(sw.scr_log, "left out") == 0 && count_in_log(sw.scr_log, "status window") == 0);
  check_hosts_gone(sw);
  if (g_failures) dump_logs(sw);
  return 0;
}

void post_display_change(DWORD pid, int times);   // below

// Posts WM_CLOSE to every saver window of `pid` (the saver exits 0).
void close_saver(DWORD pid) {
  EnumWindows([](HWND h, LPARAM lp) -> BOOL {
    DWORD p = 0;
    GetWindowThreadProcessId(h, &p);
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    if (p == (DWORD)lp && wcscmp(cls, L"LongAfterDarkSaver") == 0) PostMessageW(h, WM_CLOSE, 0, 0);
    return TRUE;
  }, (LPARAM)pid);
}

// When the saver logged the first line holding `needle` (its GetTickCount64
// stamp: "[scr <pid> <tick>] …"), or 0 when it didn't; `at` gets the line's index.
unsigned long long log_tick(const fs::path& log, const std::string& needle, int* at = nullptr) {
  const auto lines = lines_of(log);
  for (size_t i = 0; i < lines.size(); ++i) {
    if (lines[i].find(needle) == std::string::npos) continue;
    unsigned long pid = 0;
    unsigned long long tick = 0;
    if (sscanf(lines[i].c_str(), "[scr %lu %llu]", &pid, &tick) != 2) continue;
    if (at) *at = (int)i;
    return tick;
  }
  if (at) *at = -1;
  return 0;
}

// The saver's wait for the host's answer, when Random needs it (a rotation
// holding a module of another ABI): a host slow to answer (FAKEHOST_CAPS_DELAY_MS)
//  * past the wait (2 s): the first host starts once it runs out, the rotation
//    keeps every module, and the late answer is only logged;
//  * within it, while monitors change: a relayout retires the window that
//    was up when the saver asked, and the answer still reaches the window
//    that replaced it (Random leaves theirs out).
// On one monitor staged off every real one.
int test_rotate_abi_wait(const Opts& o) {
  Work w = prepare_six(o, "rotate-abi-wait");
  edit_settings(w, [](Settings& s) {
    s.all_monitors = false;
    s.randomize = {"swse.vader", "ad40.alpha"};
  });
  // Past the wait. Nothing ends the run on its own: the Intermission module
  // fails on this host (exit 1) and is skipped; it ends once the late answer
  // has come and theirs has been tried, or at the deadline.
  {
    EnvList env = base_env(o, w);
    env.push_back({L"FAKEHOST_ABIS", L"none"});
    env.push_back({L"FAKEHOST_CAPS_DELAY_MS", L"3000"});
    env.push_back({L"AD_SCR_TEST_MONITORS", L"-16000,0,856,480,p"});
    env.push_back({L"AD_SCR_TEST_ROTATE_MS", L"300"});
    const ULONGLONG t0 = GetTickCount64();
    bool closed = false;
    RunResult r = run_scr(o, L"/s", env, 60000, [&](DWORD pid) {
      const bool done = count_in_log(w.scr_log, "host capabilities: lanes=") > 0 && started_imx(w);
      if (!closed && (done || GetTickCount64() - t0 > 20000)) {
        close_saver(pid);
        closed = true;
      }
    });
    if (!expect_exit(w, r, 0)) return 1;
    CHECK(count_in_log(w.scr_log, "host capabilities: no answer within 2000 ms; Random keeps every module") == 1);
    CHECK(count_in_log(w.scr_log, "rotation: 2 module(s), collections=all") == 1);
    CHECK(count_in_log(w.scr_log, "left out") == 0);
    CHECK(started_imx(w));   // kept: this host never said it can't run it in time
    // The first host started when the wait ran out, not before, and the
    // answer came after it.
    int waited = -1, spawned = -1, answered = -1;
    const unsigned long long t_wait = log_tick(w.scr_log, "rotation: waiting for the host's capabilities", &waited);
    const unsigned long long t_spawn = log_tick(w.scr_log, "spawn window=0 ", &spawned);
    log_tick(w.scr_log, "host capabilities: lanes=", &answered);
    CHECK(waited >= 0 && spawned > waited && answered > spawned);
    if (t_wait && t_spawn && t_spawn - t_wait < 1900) failf("the first host started %llu ms into the wait", t_spawn - t_wait);
    check_hosts_gone(w);
    if (g_failures) dump_logs(w);
  }
  // Within the wait, while the monitor changes mode (16:9 to 4:3: a new
  // window, the first one retired) before the answer comes.
  {
    fs::remove(w.scr_log);
    fs::remove(w.host_log);
    edit_settings(w, [](Settings& s) { s.randomize.clear(); });
    EnvList env = base_env(o, w);
    env.push_back({L"FAKEHOST_ABIS", L"none"});
    env.push_back({L"FAKEHOST_CAPS_DELAY_MS", L"1200"});
    env.push_back({L"AD_SCR_TEST_MONITORS", L"-16000,0,856,480,p|-16000,0,640,480,p"});
    env.push_back({L"AD_SCR_TEST_ROTATE_MS", L"300"});
    env.push_back({L"AD_SCR_TESTEXIT_AFTER_FRAMES", L"30"});
    bool posted = false;
    RunResult r = run_scr(o, L"/s", env, 60000, [&](DWORD pid) {
      if (!posted && count_in_log(w.scr_log, "rotation: waiting for the host's capabilities") > 0) {
        post_display_change(pid, 1);
        posted = true;
      }
    });
    if (!expect_exit(w, r, 0)) return 1;
    CHECK(posted);
    CHECK(count_in_log(w.scr_log, "relayout monitors=1->1 kept=0 moved=0 created=1 retired=1") == 1);
    int relayout = -1, answered = -1;
    log_tick(w.scr_log, "relayout monitors=", &relayout);
    log_tick(w.scr_log, "host capabilities: lanes=", &answered);
    CHECK(relayout >= 0 && answered > relayout);   // the first window was gone when the answer came
    CHECK(count_in_log(w.scr_log, "no answer within") == 0);
    CHECK(count_in_log(w.scr_log, "rotation: 13 module(s), collections=all") == 1);
    CHECK(count_in_log(w.scr_log, "rotation: left out 14 module(s) this host can't run") == 1);
    CHECK(!started_imx(w) && !host_events(w, "start").empty());
    for (auto& e : host_events(w, "start")) CHECK(e["ADSCREENW"] == "640");   // the new window's hosts only
    check_hosts_gone(w);
    if (g_failures) dump_logs(w);
  }
  return 0;
}

int find_line(const std::vector<std::string>& lines, const std::string& needle, int from);   // below

// The modules the saver started, by its own spawn lines ("spawn window=<w>
// gen=<g> module=<id> …"), in log order: {window, module id}.
std::vector<std::pair<int, std::string>> spawned_modules(const Work& w) {
  std::vector<std::pair<int, std::string>> out;
  for (const auto& l : lines_of(w.scr_log)) {
    const size_t at = l.find("] spawn window=");
    if (at == std::string::npos) continue;
    const size_t m = l.find(" module=", at);
    if (m == std::string::npos) continue;
    const size_t end = l.find(' ', m + 8);
    out.push_back({atoi(l.c_str() + at + 15), l.substr(m + 8, end == std::string::npos ? end : end - m - 8)});
  }
  return out;
}

// Window `window`'s modules, in order (`spawned_modules`).
std::vector<std::string> modules_of(const std::vector<std::pair<int, std::string>>& spawns, int window) {
  std::vector<std::string> v;
  for (const auto& [win, m] : spawns) {
    if (win == window) v.push_back(m);
  }
  return v;
}

// The same without a module's repeated starts (a host that failed and was started again).
std::vector<std::string> collapsed(const std::vector<std::string>& v) {
  std::vector<std::string> out;
  for (const auto& m : v) {
    if (out.empty() || out.back() != m) out.push_back(m);
  }
  return out;
}

// The first `n` modules of a rotation over `ids` seeded `seed` (settings.h:
// Rotation): what one window plays, or every window following one.
std::vector<std::string> rotation_order(const std::vector<std::string>& ids, uint32_t seed, size_t n) {
  Rotation r(ids, seed);
  std::vector<std::string> v;
  for (size_t i = 0; i < n; ++i) v.push_back(i == 0 ? r.current() : r.next());
  return v;
}

bool starts_with(const std::vector<std::string>& v, const std::vector<std::string>& prefix) {
  return v.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), v.begin());
}

// Random on two (staged) monitors, off every real one, its order fixed by
// AD_SCR_TEST_SEED:
//  * by default every monitor plays the same module and they switch
//    together: both windows follow one shuffle bag (SharedRotation), a
//    Rotation over the rotation's modules with that seed, each switch one
//    pair of spawns, window 0's and window 1's;
//  * a module one monitor's host can't start (fakehost exits 3 for it) is
//    skipped on both, once, and both start the next one together;
//  * a monitor plugged in while it runs joins on the module the other plays,
//    and switches with it from then on;
//  * with DifferentPerMonitor=1 each has a rotation of its own, seeded apart
//    (the seed, and the seed + 7919): today's way.
// (The rotation waiting for the primary monitor's game, and then everyone
// switching, is SharedRotation's rule, in the unit tests: a game here would
// clip the cursor, as input-rotate does with one monitor.)
int test_rotate_monitors(const Opts& o) {
  Work w = prepare(o, "rotate-monitors");
  edit_settings(w, [](Settings& s) {
    s.module = "random";
    s.randomize.clear();
    s.all_monitors = true;
    s.different_per_monitor = false;
  });
  // The rotation's modules as the saver builds it (every module on disk, in catalog order).
  Catalog c;
  load_catalog((w.assets / "win" / "catalog-win.json").wstring(), c, nullptr);
  Settings loaded;
  load_settings(w.settings.wstring(), loaded);
  const std::wstring win = (w.assets / "win").wstring();
  const std::vector<std::string> ids =
      effective_rotation(loaded, c, [&](const std::string& id) {
        const Module* m = c.find(id);
        return m && file_exists(resolve_module_path(win, m->path));
      }).ids;
  CHECK(ids.size() == 4);
  // A seed whose two own rotations (DifferentPerMonitor) start on different modules.
  uint32_t seed = 1;
  while (seed < 100 && rotation_order(ids, seed, 1) == rotation_order(ids, seed + 7919u, 1)) ++seed;
  const std::wstring two = L"-16000,0,856,480,p;-15144,0,640,480";
  // One /s run; `done` (given the saver's pid) says when to close it. Each
  // run's log is kept as scr-<n>.log.
  int runs = 0;
  auto run = [&](const std::wstring& monitors, const EnvList& extra, const wchar_t* rotate_ms,
                 const std::function<bool(DWORD)>& done) {
    fs::remove(w.scr_log);
    fs::remove(w.host_log);
    EnvList env = base_env(o, w);
    env.insert(env.end(), extra.begin(), extra.end());
    env.push_back({L"AD_SCR_TEST_MONITORS", monitors});
    env.push_back({L"AD_SCR_TEST_ROTATE_MS", rotate_ms});
    env.push_back({L"AD_SCR_TEST_SEED", std::to_wstring(seed)});
    const ULONGLONG t0 = GetTickCount64();
    bool closed = false;
    RunResult r = run_scr(o, L"/s", env, 60000, [&](DWORD pid) {
      if (!closed && (done(pid) || GetTickCount64() - t0 > 40000)) {
        close_saver(pid);
        closed = true;
      }
    });
    std::error_code ec;
    fs::copy_file(w.scr_log, w.dir / ("scr-" + std::to_string(++runs) + ".log"), fs::copy_options::overwrite_existing, ec);
    return expect_exit(w, r, 0);
  };
  auto spawns_of = [&](int window) { return modules_of(spawned_modules(w), window); };

  // 1. The same module on both, switching together, in the bag's order.
  {
    if (!run(two, {}, L"1000", [&](DWORD) { return spawns_of(0).size() >= 6 && spawns_of(1).size() >= 6; })) return 1;
    const auto sp = spawned_modules(w);
    const auto w0 = modules_of(sp, 0), w1 = modules_of(sp, 1);
    CHECK(w0.size() >= 6 && w0 == w1);
    CHECK(w0 == rotation_order(ids, seed, w0.size()));
    CHECK(sp.size() % 2 == 0);
    for (size_t i = 0; i + 1 < sp.size(); i += 2) {
      if (sp[i].first != 0 || sp[i + 1].first != 1 || sp[i].second != sp[i + 1].second) {
        failf("spawns %zu and %zu: window %d %s, window %d %s (want a pair, window 0's and 1's)", i, i + 1, sp[i].first,
              sp[i].second.c_str(), sp[i + 1].first, sp[i + 1].second.c_str());
      }
    }
    CHECK(count_in_log(w.scr_log, "rotation: the same module on every monitor, switching together") == 1);
    CHECK(count_in_log(w.scr_log, "rotate window=0 ->") >= 5 &&
          count_in_log(w.scr_log, "rotate window=0 ->") == count_in_log(w.scr_log, "rotate window=1 ->"));
    check_hosts_gone(w);
    if (g_failures) {
      dump_logs(w);
      return 1;
    }
  }
  // 2. A module neither host can start is skipped on both, once, and the
  //    next one starts on both at once. (The rotation's clock, 5 s, leaves
  //    the three tries, 1.5 s apart at most, time to fail first.)
  {
    const std::vector<std::string> order = rotation_order(ids, seed, 4);
    const Module* bad = c.find(order[1]);
    CHECK(bad != nullptr);
    if (!bad) return 1;
    const std::string file = fs::path(bad->path).filename().string();
    auto has = [](const std::vector<std::string>& v, const std::string& m) { return std::find(v.begin(), v.end(), m) != v.end(); };
    if (!run(two, {{L"FAKEHOST_EXIT3_MODULE", widen(file)}}, L"5000",
             [&](DWORD) { return has(spawns_of(0), order[3]) && has(spawns_of(1), order[3]); })) {
      return 1;
    }
    const auto sp = spawned_modules(w);
    CHECK(starts_with(collapsed(modules_of(sp, 0)), order) && starts_with(collapsed(modules_of(sp, 1)), order));
    const std::string skipped = "module=" + order[1] + " after ";
    CHECK(count_in_log(w.scr_log, ", on every monitor") == 1 && count_in_log(w.scr_log, skipped) == 1);
    // Tried on both before it went; after it, not again, and the next one on
    // both at once: the first two spawns after the skip are that pair.
    int tried0 = 0, tried1 = 0;
    for (const auto& [win, m] : sp) (win == 0 ? tried0 : tried1) += m == order[1];
    CHECK(tried0 >= 1 && tried1 >= 1);
    const auto lines = lines_of(w.scr_log);
    const int skip = find_line(lines, skipped, 0);
    std::vector<std::pair<int, std::string>> after;
    for (int i = std::max(0, skip); skip >= 0 && i < (int)lines.size(); ++i) {
      const size_t at = lines[i].find("] spawn window=");
      if (at == std::string::npos) continue;
      const size_t m = lines[i].find(" module=", at), end = lines[i].find(' ', m + 8);
      after.push_back({atoi(lines[i].c_str() + at + 15), lines[i].substr(m + 8, end - m - 8)});
    }
    CHECK(after.size() >= 2);
    if (after.size() >= 2) {
      CHECK(after[0].first == 0 && after[1].first == 1 && after[0].second == order[2] && after[1].second == order[2]);
    }
    for (const auto& [win, m] : after) CHECK(m != order[1]);
    check_hosts_gone(w);
    if (g_failures) {
      dump_logs(w);
      return 1;
    }
  }
  // 3. A monitor plugged in joins on the module the other one plays, and
  //    switches with it.
  {
    bool posted = false;
    if (!run(L"-16000,0,856,480,p|" + two, {}, L"1500", [&](DWORD pid) {
          if (!posted && spawns_of(0).size() >= 2) {
            post_display_change(pid, 1);
            posted = true;
          }
          return posted && spawns_of(1).size() >= 3;
        })) {
      return 1;
    }
    CHECK(posted);
    CHECK(count_in_log(w.scr_log, "relayout monitors=1->2 kept=1 moved=0 created=1 retired=0") == 1);
    const auto sp = spawned_modules(w);
    // Window 0's in the bag's order all along; window 1's first host the
    // module window 0 played then, and every one after it a pair with window 0's.
    const auto w0 = modules_of(sp, 0);
    CHECK(w0 == rotation_order(ids, seed, w0.size()));
    size_t first1 = 0;
    while (first1 < sp.size() && sp[first1].first != 1) ++first1;
    CHECK(first1 > 0 && first1 < sp.size());
    if (first1 > 0 && first1 < sp.size()) {
      CHECK(sp[first1 - 1].first == 0 && sp[first1 - 1].second == sp[first1].second);
      CHECK((sp.size() - first1 - 1) % 2 == 0);
      for (size_t i = first1 + 1; i + 1 < sp.size(); i += 2) {
        CHECK(sp[i].first == 0 && sp[i + 1].first == 1 && sp[i].second == sp[i + 1].second);
      }
    }
    CHECK(modules_of(sp, 1).size() >= 3);
    check_hosts_gone(w);
    if (g_failures) {
      dump_logs(w);
      return 1;
    }
  }
  // 4. DifferentPerMonitor=1: a rotation of its own for each (today's way).
  {
    edit_settings(w, [](Settings& s) { s.different_per_monitor = true; });
    if (!run(two, {}, L"1000", [&](DWORD) { return spawns_of(0).size() >= 5 && spawns_of(1).size() >= 5; })) return 1;
    const auto sp = spawned_modules(w);
    const auto w0 = modules_of(sp, 0), w1 = modules_of(sp, 1);
    CHECK(w0.size() >= 5 && w1.size() >= 5);
    CHECK(w0 == rotation_order(ids, seed, w0.size()));
    CHECK(w1 == rotation_order(ids, seed + 7919u, w1.size()));
    CHECK(!w0.empty() && !w1.empty() && w0[0] != w1[0]);   // the seed was chosen so
    CHECK(count_in_log(w.scr_log, "rotation: a different module on each monitor (DifferentPerMonitor=1)") == 1);
    CHECK(count_in_log(w.scr_log, "rotation: the same module") == 0);
    check_hosts_gone(w);
    if (g_failures) dump_logs(w);
  }
  return 0;
}

// "x,y,w,h" from a screenshot report; false when it says "hidden" (or nothing).
bool report_rect(const std::string& v, RECT* r) {
  int x = 0, y = 0, w = 0, h = 0;
  if (sscanf(v.c_str(), "%d,%d,%d,%d", &x, &y, &w, &h) != 4) return false;
  *r = RECT{x, y, x + w, y + h};
  return true;
}

// The settings dialog's "A different module on each monitor"
// (DifferentPerMonitor; ui_model.h: per_monitor_choice), driven by control
// ID with monitors staged (AD_SCR_TEST_MONITORS, which the test build's
// dialog counts as the saver does):
//  * two monitors, a file without the key (the fixture: Random, a named
//    module leading its list): shown under "Change module every", enabled,
//    unchecked; checked, then "Primary monitor only": greyed, still checked;
//    "All monitors": enabled again; Single: hidden; Random: back; OK saves
//    DifferentPerMonitor=1 and keeps the rest;
//  * the next dialog shows it checked; unchecked, then Cancel: the file as it was;
//  * one monitor: not there, and OK keeps the file's value; a monitor
//    plugged in (WM_DISPLAYCHANGE: the staged layout after it) brings it,
//    checked;
// then off-screen renders, light and dark at 100% and 150%, greyed, focused,
// at the minimum size and with one monitor: under "Change module every",
// clear of it and of the list's card, its text whole beside its box; with one
// monitor the list keeps that room.
int test_config_monitors(const Opts& o) {
  Work w = prepare(o, "config-monitors");
  const std::wstring two = L"0,0,1280,720,p;1280,0,1024,768", one = L"0,0,1280,720,p";
  auto env_for = [&](const std::wstring& monitors) {
    EnvList env = base_env(o, w);
    env.push_back({L"AD_SCR_TEST_MONITORS", monitors});
    return env;
  };
  struct Seen {
    bool visible = false, enabled = false, checked = false;
  };
  auto look = [](HWND dlg) {
    HWND pm = GetDlgItem(dlg, IDC_PER_MONITOR);
    Seen s;
    s.visible = IsWindowVisible(pm) != FALSE;
    s.enabled = IsWindowEnabled(pm) != FALSE;
    s.checked = SendMessageW(pm, BM_GETCHECK, 0, 0) == BST_CHECKED;
    return s;
  };
  auto set_monitors = [](HWND dlg, int sel) {
    HWND mon = GetDlgItem(dlg, IDC_MONITORS);
    SendMessageW(mon, CB_SETCURSEL, sel, 0);
    SendMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDC_MONITORS, CBN_SELCHANGE), (LPARAM)mon);
  };
  auto set_check = [](HWND dlg, bool on) {
    HWND pm = GetDlgItem(dlg, IDC_PER_MONITOR);
    SendMessageW(pm, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDC_PER_MONITOR, BN_CLICKED), (LPARAM)pm);
  };
  auto ready = [](DWORD pid) -> HWND {
    HWND d = find_dialog(pid);
    HWND list = d ? GetDlgItem(d, IDC_MODULE_LIST) : nullptr;
    return list && SendMessageW(list, LVM_GETITEMCOUNT, 0, 0) > 0 ? d : nullptr;
  };
  std::string text, again;

  // ---- session 1: two monitors, a file without the key.
  Seen first, checked, primary, all, single, random;
  std::string label;
  bool acted = false;
  RunResult r = run_scr(o, L"/c", env_for(two), 60000, [&](DWORD pid) {
    if (acted) return;
    HWND dlg = ready(pid);
    if (!dlg) return;
    first = look(dlg);
    label = window_text(GetDlgItem(dlg, IDC_PER_MONITOR));
    set_check(dlg, true);
    checked = look(dlg);
    set_monitors(dlg, 1);
    primary = look(dlg);
    set_monitors(dlg, 0);
    all = look(dlg);
    choose_radio(dlg, IDC_MODE_SINGLE, IDC_MODE_RANDOM);
    single = look(dlg);
    choose_radio(dlg, IDC_MODE_RANDOM, IDC_MODE_SINGLE);
    random = look(dlg);
    PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), (LPARAM)GetDlgItem(dlg, IDOK));
    acted = true;
  });
  CHECK(acted);
  if (!expect_exit(w, r, 0)) return 1;
  CHECK(label == "A di&fferent module on each monitor");
  CHECK(first.visible && first.enabled && !first.checked);
  CHECK(checked.visible && checked.enabled && checked.checked);
  CHECK(primary.visible && !primary.enabled && primary.checked);   // greyed, keeping its check
  CHECK(all.visible && all.enabled && all.checked);
  CHECK(!single.visible && single.checked);
  CHECK(random.visible && random.enabled && random.checked);
  Settings s;
  CHECK(load_settings(w.settings.wstring(), s));
  CHECK(s.different_per_monitor && s.all_monitors && s.rotates() && s.module == "test.rings");
  read_file(w.settings.wstring(), text);
  CHECK(text.find("Monitors=all\r\n") != std::string::npos && text.find("DifferentPerMonitor=1\r\n") != std::string::npos);
  CHECK(text.find("FutureKey=keep me\r\n") != std::string::npos);

  // ---- session 2: shown checked; unchecked, then Cancel writes nothing.
  Seen reopened, unchecked;
  acted = false;
  r = run_scr(o, L"/c", env_for(two), 60000, [&](DWORD pid) {
    if (acted) return;
    HWND dlg = ready(pid);
    if (!dlg) return;
    reopened = look(dlg);
    set_check(dlg, false);
    unchecked = look(dlg);
    PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), (LPARAM)GetDlgItem(dlg, IDCANCEL));
    acted = true;
  });
  CHECK(acted);
  if (!expect_exit(w, r, 0)) return 1;
  CHECK(reopened.visible && reopened.enabled && reopened.checked && !unchecked.checked);
  read_file(w.settings.wstring(), again);
  CHECK(again == text);

  // ---- session 3: one monitor: not there, and OK keeps the file's value.
  Seen alone;
  acted = false;
  r = run_scr(o, L"/c", env_for(one), 60000, [&](DWORD pid) {
    if (acted) return;
    HWND dlg = ready(pid);
    if (!dlg) return;
    alone = look(dlg);
    PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), (LPARAM)GetDlgItem(dlg, IDOK));
    acted = true;
  });
  CHECK(acted);
  if (!expect_exit(w, r, 0)) return 1;
  CHECK(!alone.visible && alone.checked);
  CHECK(load_settings(w.settings.wstring(), s) && s.different_per_monitor);

  // ---- session 4: one monitor, then a second one plugged in.
  fs::remove(w.scr_log);
  Seen plugged_before, plugged_after;
  acted = false;
  r = run_scr(o, L"/c", env_for(one + L"|" + two), 60000, [&](DWORD pid) {
    if (acted) return;
    HWND dlg = ready(pid);
    if (!dlg) return;
    plugged_before = look(dlg);
    SendMessageW(dlg, WM_DISPLAYCHANGE, 32, MAKELPARAM(1280, 720));
    plugged_after = look(dlg);
    PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), (LPARAM)GetDlgItem(dlg, IDCANCEL));
    acted = true;
  });
  CHECK(acted);
  if (!expect_exit(w, r, 0)) return 1;
  CHECK(!plugged_before.visible && plugged_after.visible && plugged_after.enabled && plugged_after.checked);
  CHECK(count_in_log(w.scr_log, "dialog: 2 monitor(s)") == 1);
  check_hosts_gone(w);
  if (g_failures) {
    fprintf(stderr, "---- settings.ini\n%s\n", text.c_str());
    dump_logs(w);
    return 1;
  }

  // ---- renders (kept as per-monitor-<name>.png in the test's folder)
  struct Shot {
    const char* name;
    std::string state;
    bool two_monitors, enabled;
  };
  const std::string base = "mode=random;different=1;wait=5000;frames=3;size=";
  const std::vector<Shot> shots = {
      {"light-100", "theme=light;dpi=96;" + base + "1040x680", true, true},
      {"dark-100", "theme=dark;dpi=96;" + base + "1040x680", true, true},
      {"light-150", "theme=light;dpi=144;" + base + "1040x680", true, true},
      {"dark-150-focus", "theme=dark;dpi=144;focus=permonitor;" + base + "1040x680", true, true},
      {"light-100-greyed", "theme=light;dpi=96;monitors=primary;" + base + "1040x680", true, false},
      {"dark-150-min", "theme=dark;dpi=144;" + base + "900x600", true, true},
      {"light-100-one", "theme=light;dpi=96;" + base + "1040x680", false, false},
  };
  int card_h_two = 0, card_h_one = 0;
  for (const Shot& shot : shots) {
    auto kv = dialog_report(o, w, shot.state, {{L"AD_SCR_TEST_MONITORS", shot.two_monitors ? two : one}});
    const fs::path png = w.dir / (std::string("per-monitor-") + shot.name + ".png");
    std::error_code ec;
    fs::copy_file(w.dir / "dialog.png", png, fs::copy_options::overwrite_existing, ec);
    int pw = 0, ph = 0;
    std::vector<uint8_t> px;
    CHECK(load_png(png, &pw, &ph, &px));
    RECT pm{}, dur{}, card{};
    const bool shown = report_rect(kv["per_monitor"], &pm);
    CHECK(report_rect(kv["duration"], &dur) && report_rect(kv["list_card"], &card));
    if (std::string(shot.name) == "light-100") card_h_two = card.bottom - card.top;
    if (!shot.two_monitors) {
      CHECK(kv["monitors"] == "1" && !shown && kv["per_monitor"] == "hidden");
      card_h_one = card.bottom - card.top;
      continue;
    }
    CHECK(kv["monitors"] == "2" && shown);
    CHECK(kv["per_monitor_enabled"] == (shot.enabled ? "1" : "0") && kv["per_monitor_checked"] == "1");
    CHECK(kv["per_monitor_fits"] == "1");   // its text whole beside its box
    // In the picture, under "Change module every" and the list's card, across the card's width.
    CHECK(pm.left >= 0 && pm.top >= 0 && pm.right <= pw && pm.bottom <= ph);
    CHECK(pm.top >= dur.bottom && pm.top >= card.bottom);
    CHECK(std::abs(pm.left - card.left) <= 1 && std::abs(pm.right - card.right) <= 1);
    if (g_failures) {
      fprintf(stderr, "%s: per_monitor=%s duration=%s list_card=%s fits=%s picture %dx%d\n", shot.name,
              kv["per_monitor"].c_str(), kv["duration"].c_str(), kv["list_card"].c_str(), kv["per_monitor_fits"].c_str(),
              pw, ph);
      return 1;
    }
  }
  // With one monitor the list keeps the row's room (40 DIP at 100%).
  CHECK(card_h_one - card_h_two == 40);
  check_hosts_gone(w);
  return 0;
}

// The hosts' start lines for modules whose path ends in `suffix`, in order.
std::vector<std::map<std::string, std::string>> starts_of(const Work& w, const char* suffix) {
  std::vector<std::map<std::string, std::string>> v;
  for (auto& e : host_events(w, "start")) {
    if (ends_with(e["module"], suffix)) v.push_back(e);
  }
  return v;
}

bool screen_is(std::map<std::string, std::string>& e, const char* width, const char* height) {
  return e["ADSCREENW"] == width && e["ADSCREENH"] == height;
}

// Star Wars Screen Entertainment's modules fill the screen (PLAN §6a.1):
// whatever the Resolution setting (720 lines here), every host started for
// an Intermission module asks for its own 640x480 (geometry.h:
// module_screen), and its 4:3 frame is scaled to fit the monitor: on this
// 16:9 one its full height, side bars only. After Dark modules keep the
// setting.
//  * /s on a 16:9 monitor staged off every real one, rotating between an
//    Intermission and an After Dark module: each host its module's size,
//    640x480 and 1280x720, switch after switch; the first host starts on the
//    desktop captured at its own screen (where this desktop can be captured);
//  * that window shows the 640x480 frame full height, pillarboxed;
//  * a monitor change of aspect keeps an Intermission module's host (its
//    window moves) and replaces an After Dark module's;
//  * /p: 320x240, as for every module;
//  * the settings dialog: its live preview and thumbnails 640x480, and
//    Preview's /s too.
int test_screen_abi(const Opts& o) {
  Work w = prepare_six(o, "screen-abi");
  edit_settings(w, [](Settings& s) {
    s.scale = 1.5;
    s.all_monitors = false;
    s.module = "random";
    s.randomize = {"swse.vader", "ad40.alpha"};
  });
  const std::wstring wide = L"-16000,0,1280,720,p";
  // A rotation of both ABIs.
  {
    EnvList env = base_env(o, w);
    env.push_back({L"AD_SCR_TEST_MONITORS", wide});
    env.push_back({L"AD_SCR_TEST_ROTATE_MS", L"300"});
    bool closed = false;
    const ULONGLONG t0 = GetTickCount64();
    RunResult r = run_scr(o, L"/s", env, 60000, [&](DWORD pid) {
      if (!closed && (host_events(w, "start").size() >= 5 || GetTickCount64() - t0 > 20000)) {
        close_saver(pid);
        closed = true;
      }
    });
    if (!expect_exit(w, r, 0)) return 1;
    auto starts = host_events(w, "start");
    CHECK(starts.size() >= 4);
    int imx = 0, ad = 0;
    std::string last;
    for (auto& e : starts) {
      const bool is_imx = ends_with(e["module"], "VADER.IMX");
      CHECK(is_imx || ends_with(e["module"], "ALPHA.AD"));
      if (is_imx) {
        ++imx;
        CHECK(screen_is(e, "640", "480"));
      } else {
        ++ad;
        CHECK(screen_is(e, "1280", "720"));
      }
      CHECK(e["module"] != last);   // two modules: every switch a new host, of the other size
      last = e["module"];
    }
    CHECK(imx >= 2 && ad >= 2);
    CHECK(count_in_log(w.scr_log, "size=640x480 ") == imx && count_in_log(w.scr_log, "size=1280x720 ") == ad);
    // Both screens captured before the window appeared (either module may
    // come first), the first host started on its own one; the rest black.
    if (!starts.empty() && count_in_log(w.scr_log, "seed window=0 ") > 0) {
      CHECK(count_in_log(w.scr_log, "seed window=0 1280x720 from 1280x720 in ") == 1);
      CHECK(count_in_log(w.scr_log, "seed window=0 640x480 from 960x720 (the frame's part of the monitor) in ") == 1);
      CHECK(starts[0]["seed_check"] == "ok " + starts[0]["ADSCREENW"] + "x" + starts[0]["ADSCREENH"]);
      CHECK(ends_with(starts[0]["ADSEEDIMG"], starts[0]["ADSCREENW"] == "640" ? "-0-640x480.ppm" : "-0.ppm"));
    } else {
      fprintf(stderr, "screen-abi: no desktop capture here (a desktop that can't be read back); seeds not checked\n");
    }
    for (size_t i = 1; i < starts.size(); ++i) CHECK(starts[i]["ADSEEDIMG"].empty());
    check_hosts_gone(w);
    if (g_failures) dump_logs(w);
  }
  // What the window shows: the 640x480 frame at the monitor's full height,
  // black bars at the sides only (fit_rect: 960x720 at x=160).
  {
    fs::remove(w.scr_log);
    fs::remove(w.host_log);
    edit_settings(w, [](Settings& s) {
      s.module = "swse.vader";
      s.randomize.clear();
    });
    const fs::path caps = w.dir / "captures";
    fs::create_directories(caps);
    EnvList env = base_env(o, w);
    env.push_back({L"AD_SCR_TEST_MONITORS", wide});
    env.push_back({L"AD_SCR_TEST_CAPTURE", caps.wstring()});
    env.push_back({L"AD_SCR_TEST_CAPTURE_FRAMES", L"20"});
    env.push_back({L"AD_SCR_TESTEXIT_AFTER_FRAMES", L"30"});
    RunResult r = run_scr(o, L"/s", env, 60000);
    if (!expect_exit(w, r, 0)) return 1;
    auto starts = host_events(w, "start");
    CHECK(starts.size() == 1);
    for (auto& e : starts) CHECK(screen_is(e, "640", "480"));
    CHECK(count_in_log(w.scr_log, "capture window=0 frame=20 ok 1280x720 host=640x480 ") == 1);
    int sw = 0, sh = 0;
    std::vector<uint8_t> px;
    if (load_png(caps / "window0-frame20.png", &sw, &sh, &px) && sw == 1280 && sh == 720) {
      auto lit = [&](int x, int y) {
        const uint8_t* q = &px[((size_t)y * sw + x) * 4];
        return q[0] + q[1] + q[2] > 48;
      };
      const RectI fit = fit_rect(640, 480, 1280, 720);
      CHECK((fit == RectI{160, 0, 960, 720}));
      for (int y : {2, 180, 360, 540, 717}) {
        CHECK(!lit(80, y) && !lit(fit.x - 2, y) && !lit(fit.x + fit.w + 1, y) && !lit(1200, y));   // the side bars
      }
      // The frame reaches the top and the bottom rows: no bars there.
      int top = 0, bottom = 0, n = 0;
      for (int x = fit.x + 40; x < fit.x + fit.w - 40; x += 40, ++n) {
        top += lit(x, 1);
        bottom += lit(x, 718);
      }
      CHECK(top * 10 >= n * 9 && bottom * 10 >= n * 9);
    } else {
      failf("no 1280x720 capture of window 0 at frame 20");
    }
    check_hosts_gone(w);
    if (g_failures) dump_logs(w);
  }
  // A monitor change of aspect (16:9 to 4:3): an Intermission module's host
  // carries on in the moved window; an After Dark module's is replaced.
  for (bool imx : {true, false}) {
    fs::remove(w.scr_log);
    fs::remove(w.host_log);
    edit_settings(w, [&](Settings& s) {
      s.module = imx ? "swse.vader" : "ad40.alpha";
      s.randomize.clear();
    });
    EnvList env = base_env(o, w);
    env.push_back({L"AD_SCR_TEST_MONITORS", wide + L"|-16000,0,1024,768,p"});
    env.push_back({L"AD_SCR_TESTEXIT_AFTER_FRAMES", L"200"});
    bool posted = false;
    RunResult r = run_scr(o, L"/s", env, 60000, [&](DWORD pid) {
      if (!posted && count_in_log(w.scr_log, "spawn window=0 ") > 0) {
        post_display_change(pid, 1);
        posted = true;
      }
    });
    if (!expect_exit(w, r, 0)) return 1;
    CHECK(posted);
    auto starts = host_events(w, "start");
    if (imx) {
      CHECK(count_in_log(w.scr_log, "relayout monitors=1->1 kept=0 moved=1 created=0 retired=0") == 1);
      CHECK(starts.size() == 1);
      for (auto& e : starts) CHECK(screen_is(e, "640", "480"));
    } else {
      CHECK(count_in_log(w.scr_log, "relayout monitors=1->1 kept=0 moved=0 created=1 retired=1") == 1);
      CHECK(starts.size() == 2);
      if (starts.size() == 2) CHECK(screen_is(starts[0], "1280", "720") && screen_is(starts[1], "960", "720"));
    }
    check_hosts_gone(w);
    if (g_failures) dump_logs(w);
  }
  // /p: 320x240 for an Intermission module as for any (the host renders it
  // through a 640x480 guest display, which its scene fills).
  {
    fs::remove(w.scr_log);
    fs::remove(w.host_log);
    edit_settings(w, [](Settings& s) {
      s.module = "swse.vader";
      s.randomize.clear();
    });
    HWND parent = make_parent(152, 112);
    EnvList env = base_env(o, w);
    env.push_back({L"AD_SCR_TESTEXIT_AFTER_FRAMES", L"10"});
    RunResult r = run_scr(o, L"/p " + hwnd_arg(parent), env, 60000);
    DestroyWindow(parent);
    if (!expect_exit(w, r, 0)) return 1;
    auto starts = starts_of(w, "VADER.IMX");
    CHECK(starts.size() == 1);
    for (auto& e : starts) CHECK(screen_is(e, "320", "240"));
    check_hosts_gone(w);
    if (g_failures) dump_logs(w);
  }
  // The settings dialog: its live preview of an Intermission module, the
  // background thumbnails (one module of each ABI still without a picture),
  // then Preview, whose /s runs on the monitor staged off every real one and
  // ends itself.
  {
    fs::remove(w.scr_log);
    fs::remove(w.host_log);
    seed_thumbs(w, {"swse.jawas", "ad40.twin"});
    EnvList env = base_env(o, w);
    env.push_back({L"AD_SCR_THUMBGEN", L"1"});
    env.push_back({L"AD_SCR_TEST_MONITORS", wide});
    env.push_back({L"AD_SCR_TESTEXIT_AFTER_FRAMES", L"20"});
    DWORD dialog_pid = 0;
    int step = 0;
    RunResult r = run_scr(o, L"/c", env, 90000, [&](DWORD pid) {
      dialog_pid = pid;
      HWND dlg = find_dialog(pid);
      if (!dlg) return;
      if (step == 0) {
        std::set<std::string> ran;
        for (auto& e : hosts_of(w, pid)) ran.insert(fs::path(e["module"]).filename().string());
        if (!ran.count("VADER.IMX") || !ran.count("JAWAS.IMX") || !ran.count("TWIN.AD")) return;
        click(dlg, IDC_PREVIEW);
        step = 1;
      } else if (step == 1) {
        // The Preview has ended (its test exit) and the dialog took it back.
        if (!hosts_of(w, pid, true).empty() && IsWindowEnabled(GetDlgItem(dlg, IDC_PREVIEW))) {
          PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
          step = 2;
        }
      }
    });
    CHECK(step == 2);
    if (!expect_exit(w, r, 0)) return 1;
    bool live = false, thumb_imx = false, thumb_ad = false;
    for (auto& e : hosts_of(w, dialog_pid)) {
      const std::string file = fs::path(e["module"]).filename().string();
      if (file == "VADER.IMX") live |= screen_is(e, "640", "480");      // not the 16:9 preview's 856x480
      if (file == "JAWAS.IMX") thumb_imx |= screen_is(e, "640", "480");
      if (file == "TWIN.AD") thumb_ad |= screen_is(e, "640", "480");
      if (file == "VADER.IMX" || file == "JAWAS.IMX") CHECK(screen_is(e, "640", "480"));
    }
    CHECK(live && thumb_imx && thumb_ad);
    auto preview = hosts_of(w, dialog_pid, true);   // Preview's /s
    CHECK(!preview.empty());
    for (auto& e : preview) CHECK(ends_with(e["module"], "VADER.IMX") && screen_is(e, "640", "480"));
    check_hosts_gone(w);
    if (g_failures) dump_logs(w);
  }
  return 0;
}

// Star Trek: The Screen Saver's modules compose a fixed 640x480 scene (The
// Mission at the top left of a larger screen beside a grey band, Final Exam
// small in its middle), and the catalog gives each "screen": "640x480"
// (PLAN §2.4, catalog-seven.json): whatever the Resolution setting (720
// lines here), every host started for one asks for 640x480, exactly as an
// Intermission module does by its ABI (geometry.h: own_screen,
// module_screen), and its frame is scaled to fit the monitor.
//  * /s on a 16:9 monitor staged off every real one, rotating between a Star
//    Trek module and an After Dark one: 640x480 and 1280x720, switch after
//    switch, and a seed picture planned for each; beside a Star Wars module,
//    640x480 both;
//  * the window shows the 640x480 frame at the monitor's full height;
//  * a monitor change of aspect moves its window and keeps its host;
//  * /p: 320x240, as for every module;
//  * the settings dialog: its live preview and a thumbnail at 640x480, and
//    Preview's /s too;
//  * the live preview follows the catalog's "screen" alone: an import that
//    only adds it to the module shown replaces its host with one at 640x480.
int test_screen_field(const Opts& o) {
  Work w = prepare_six(o, "screen-field", "catalog-seven.json");
  edit_settings(w, [](Settings& s) {
    s.scale = 1.5;
    s.all_monitors = false;
    s.module = "random";
  });
  const std::wstring wide = L"-16000,0,1280,720,p";
  // Rotations: a Star Trek module and an After Dark one, then a Star Trek
  // module and a Star Wars one.
  for (bool with_swse : {false, true}) {
    fs::remove(w.scr_log);
    fs::remove(w.host_log);
    edit_settings(w, [&](Settings& s) { s.randomize = {"startrek.final", with_swse ? "swse.vader" : "ad40.alpha"}; });
    EnvList env = base_env(o, w);
    env.push_back({L"AD_SCR_TEST_MONITORS", wide});
    env.push_back({L"AD_SCR_TEST_ROTATE_MS", L"300"});
    bool closed = false;
    const ULONGLONG t0 = GetTickCount64();
    RunResult r = run_scr(o, L"/s", env, 60000, [&](DWORD pid) {
      if (!closed && (host_events(w, "start").size() >= 5 || GetTickCount64() - t0 > 20000)) {
        close_saver(pid);
        closed = true;
      }
    });
    if (!expect_exit(w, r, 0)) return 1;
    auto starts = host_events(w, "start");
    CHECK(starts.size() >= 4);
    int trek = 0, other = 0;
    std::string last;
    for (auto& e : starts) {
      const bool is_trek = ends_with(e["module"], "FINAL.AD");
      CHECK(is_trek || ends_with(e["module"], with_swse ? "VADER.IMX" : "ALPHA.AD"));
      if (is_trek) {
        ++trek;
        CHECK(screen_is(e, "640", "480"));
      } else {
        ++other;
        CHECK(with_swse ? screen_is(e, "640", "480") : screen_is(e, "1280", "720"));
      }
      CHECK(e["module"] != last);   // two modules: every switch a new host
      last = e["module"];
    }
    CHECK(trek >= 2 && other >= 2);
    CHECK(count_in_log(w.scr_log, "size=640x480 ") == (with_swse ? trek + other : trek));
    CHECK(count_in_log(w.scr_log, "size=1280x720 ") == (with_swse ? 0 : other));
    // One picture per screen its first host may be given, taken before the
    // window appeared (first_module_screens, plan_seed_shots): a Star Trek
    // module's 640x480 beside an After Dark module's 1280x720 is two (by the
    // ABIs alone it was one); with a Star Wars module in the rotation, what
    // Random plays waits on the host's answer, so every module available may
    // come first: two as well. Each picture planned logs a line, taken or
    // not (a desktop that can't be read back fails every capture).
    CHECK(count_in_log(w.scr_log, "seed window=0 ") + count_in_log(w.scr_log, "seed window=0: capture failed") == 2);
    CHECK(count_in_log(w.scr_log, "none taken at") == 0);
    if (!starts.empty() && count_in_log(w.scr_log, "seed window=0 ") > 0) {
      CHECK(count_in_log(w.scr_log, "seed window=0 640x480 from 960x720 (the frame's part of the monitor) in ") == 1);
      CHECK(count_in_log(w.scr_log, "seed window=0 1280x720 from 1280x720 in ") == 1);
      CHECK(starts[0]["seed_check"] == "ok " + starts[0]["ADSCREENW"] + "x" + starts[0]["ADSCREENH"]);
    } else {
      fprintf(stderr, "screen-field: no desktop capture here (a desktop that can't be read back); seeds not checked\n");
    }
    check_hosts_gone(w);
    if (g_failures) dump_logs(w);
  }
  // What the window shows of The Mission: the 640x480 frame at the
  // monitor's full height, black bars at the sides only (960x720 at x=160).
  {
    fs::remove(w.scr_log);
    fs::remove(w.host_log);
    edit_settings(w, [](Settings& s) {
      s.module = "startrek.mission";
      s.randomize.clear();
    });
    const fs::path caps = w.dir / "captures";
    fs::create_directories(caps);
    EnvList env = base_env(o, w);
    env.push_back({L"AD_SCR_TEST_MONITORS", wide});
    env.push_back({L"AD_SCR_TEST_CAPTURE", caps.wstring()});
    env.push_back({L"AD_SCR_TEST_CAPTURE_FRAMES", L"20"});
    env.push_back({L"AD_SCR_TESTEXIT_AFTER_FRAMES", L"30"});
    RunResult r = run_scr(o, L"/s", env, 60000);
    if (!expect_exit(w, r, 0)) return 1;
    auto starts = host_events(w, "start");
    CHECK(starts.size() == 1);
    for (auto& e : starts) CHECK(ends_with(e["module"], "MISSION.AD") && screen_is(e, "640", "480"));
    CHECK(count_in_log(w.scr_log, "capture window=0 frame=20 ok 1280x720 host=640x480 ") == 1);
    int sw = 0, sh = 0;
    std::vector<uint8_t> px;
    if (load_png(caps / "window0-frame20.png", &sw, &sh, &px) && sw == 1280 && sh == 720) {
      auto lit = [&](int x, int y) {
        const uint8_t* q = &px[((size_t)y * sw + x) * 4];
        return q[0] + q[1] + q[2] > 48;
      };
      const RectI fit = fit_rect(640, 480, 1280, 720);
      CHECK((fit == RectI{160, 0, 960, 720}));
      for (int y : {2, 180, 360, 540, 717}) {
        CHECK(!lit(80, y) && !lit(fit.x - 2, y) && !lit(fit.x + fit.w + 1, y) && !lit(1200, y));   // the side bars
      }
      int top = 0, bottom = 0, n = 0;
      for (int x = fit.x + 40; x < fit.x + fit.w - 40; x += 40, ++n) {
        top += lit(x, 1);
        bottom += lit(x, 718);
      }
      CHECK(top * 10 >= n * 9 && bottom * 10 >= n * 9);   // no bars above or below
    } else {
      failf("no 1280x720 capture of window 0 at frame 20");
    }
    check_hosts_gone(w);
    if (g_failures) dump_logs(w);
  }
  // A monitor change of aspect (16:9 to 4:3): the window moves, its host
  // carries on (an After Dark module's would be replaced: screen-abi).
  {
    fs::remove(w.scr_log);
    fs::remove(w.host_log);
    edit_settings(w, [](Settings& s) {
      s.module = "startrek.tribble";
      s.randomize.clear();
    });
    EnvList env = base_env(o, w);
    env.push_back({L"AD_SCR_TEST_MONITORS", wide + L"|-16000,0,1024,768,p"});
    env.push_back({L"AD_SCR_TESTEXIT_AFTER_FRAMES", L"200"});
    bool posted = false;
    RunResult r = run_scr(o, L"/s", env, 60000, [&](DWORD pid) {
      if (!posted && count_in_log(w.scr_log, "spawn window=0 ") > 0) {
        post_display_change(pid, 1);
        posted = true;
      }
    });
    if (!expect_exit(w, r, 0)) return 1;
    CHECK(posted);
    CHECK(count_in_log(w.scr_log, "relayout monitors=1->1 kept=0 moved=1 created=0 retired=0") == 1);
    auto starts = host_events(w, "start");
    CHECK(starts.size() == 1);
    for (auto& e : starts) CHECK(ends_with(e["module"], "TRIBBLE.AD") && screen_is(e, "640", "480"));
    check_hosts_gone(w);
    if (g_failures) dump_logs(w);
  }
  // /p: 320x240, as for every module.
  {
    fs::remove(w.scr_log);
    fs::remove(w.host_log);
    edit_settings(w, [](Settings& s) {
      s.module = "startrek.final";
      s.randomize.clear();
    });
    HWND parent = make_parent(152, 112);
    EnvList env = base_env(o, w);
    env.push_back({L"AD_SCR_TESTEXIT_AFTER_FRAMES", L"10"});
    RunResult r = run_scr(o, L"/p " + hwnd_arg(parent), env, 60000);
    DestroyWindow(parent);
    if (!expect_exit(w, r, 0)) return 1;
    auto starts = starts_of(w, "FINAL.AD");
    CHECK(starts.size() == 1);
    for (auto& e : starts) CHECK(screen_is(e, "320", "240"));
    check_hosts_gone(w);
    if (g_failures) dump_logs(w);
  }
  // The settings dialog: its live preview of Final Exam, the background
  // thumbnails (a Star Trek and an After Dark module still without a
  // picture), then Preview, whose /s runs on the monitor staged off every
  // real one and ends itself. In this copy of the catalog Tribbles' own
  // screen is 800x600, so its thumbnail shows the catalog's size is used
  // (a 4:3 capture at 480 lines would be 640x480 either way).
  {
    fs::remove(w.scr_log);
    fs::remove(w.host_log);
    {
      const fs::path catalog = w.assets / "win" / "catalog-win.json";
      std::string text;
      read_file(catalog.wstring(), text);
      phosg::JSON root = phosg::JSON::parse(text);
      int edited = 0;
      for (auto& m : root.at("modules").as_list()) {
        if (m->at("id").as_string() != "startrek.tribble") continue;
        m->at("screen") = phosg::JSON("800x600");
        ++edited;
      }
      CHECK(edited == 1 && write_file_atomic(catalog.wstring(), root.serialize()));
    }
    seed_thumbs(w, {"startrek.tribble", "ad40.twin"});
    EnvList env = base_env(o, w);
    env.push_back({L"AD_SCR_THUMBGEN", L"1"});
    env.push_back({L"AD_SCR_TEST_MONITORS", wide});
    env.push_back({L"AD_SCR_TESTEXIT_AFTER_FRAMES", L"20"});
    DWORD dialog_pid = 0;
    int step = 0;
    RunResult r = run_scr(o, L"/c", env, 90000, [&](DWORD pid) {
      dialog_pid = pid;
      HWND dlg = find_dialog(pid);
      if (!dlg) return;
      if (step == 0) {
        std::set<std::string> ran;
        for (auto& e : hosts_of(w, pid)) ran.insert(fs::path(e["module"]).filename().string());
        if (!ran.count("FINAL.AD") || !ran.count("TRIBBLE.AD") || !ran.count("TWIN.AD")) return;
        click(dlg, IDC_PREVIEW);
        step = 1;
      } else if (step == 1) {
        if (!hosts_of(w, pid, true).empty() && IsWindowEnabled(GetDlgItem(dlg, IDC_PREVIEW))) {
          PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
          step = 2;
        }
      }
    });
    CHECK(step == 2);
    if (!expect_exit(w, r, 0)) return 1;
    bool live = false, thumb_trek = false, thumb_ad = false;
    for (auto& e : hosts_of(w, dialog_pid)) {
      const std::string file = fs::path(e["module"]).filename().string();
      if (file == "FINAL.AD") live |= screen_is(e, "640", "480");   // not the 16:9 preview's 856x480
      if (file == "TRIBBLE.AD") thumb_trek |= screen_is(e, "800", "600");
      if (file == "TWIN.AD") thumb_ad |= screen_is(e, "640", "480");
      if (file == "FINAL.AD") CHECK(screen_is(e, "640", "480"));
      if (file == "TRIBBLE.AD") CHECK(screen_is(e, "800", "600"));
    }
    CHECK(live && thumb_trek && thumb_ad);
    CHECK(count_in_log(w.scr_log, "live preview: spawn ") > 0 &&
          count_in_log(w.scr_log, "FINAL.AD size=640x480 abi=afterdark screen=640x480 ") > 0);
    auto preview = hosts_of(w, dialog_pid, true);   // Preview's /s
    CHECK(!preview.empty());
    for (auto& e : preview) CHECK(ends_with(e["module"], "FINAL.AD") && screen_is(e, "640", "480"));
    check_hosts_gone(w);
    if (g_failures) dump_logs(w);
  }
  // The live preview follows the catalog's "screen" alone (live_preview.cc:
  // same_target). The dialog opens on Final Exam from a catalog without the
  // field (written by an adimport from before it), so its live preview runs
  // it as any After Dark module, at the preview box's 16:9 480 lines. An
  // import then adds "screen": "640x480" and changes nothing else about it
  // (id, ABI, path, settings): with the dialog still open, its host is
  // replaced by one at 640x480.
  {
    fs::remove(w.scr_log);
    fs::remove(w.host_log);
    const fs::path catalog = w.assets / "win" / "catalog-win.json";
    const fs::path imported = w.dir / "catalog-imported.json";
    std::string text;
    CHECK(read_file((fs::path(o.fixtures) / "catalog-seven.json").wstring(), text));
    CHECK(write_file_atomic(imported.wstring(), text));   // what the import leaves: the fixture as it is
    {
      phosg::JSON root = phosg::JSON::parse(text);
      size_t dropped = 0;
      for (auto& m : root.at("modules").as_list()) dropped += m->erase("screen");
      CHECK(dropped == 4 && write_file_atomic(catalog.wstring(), root.serialize()));
    }
    edit_settings(w, [](Settings& s) {
      s.module = "startrek.final";
      s.randomize.clear();
    });
    EnvList env = base_env(o, w);
    env.push_back({L"FAKEIMPORT_EXIT", L"0"});
    env.push_back({L"FAKEIMPORT_CATALOG", imported.wstring()});
    DWORD dialog_pid = 0;
    int step = 0;
    ULONGLONG clicked = 0;
    auto finals = [&](DWORD pid) {   // the dialog's hosts of Final Exam, in order
      std::vector<std::map<std::string, std::string>> v;
      for (auto& e : hosts_of(w, pid)) {
        if (ends_with(e["module"], "FINAL.AD")) v.push_back(e);
      }
      return v;
    };
    RunResult r = run_scr(o, L"/c", env, 60000, [&](DWORD pid) {
      dialog_pid = pid;
      if (step == 2) return;
      HWND dlg = find_dialog(pid);
      if (!dlg) return;
      if (step == 0) {
        if (finals(pid).empty()) return;   // its live preview runs
        click(dlg, IDC_IMPORT);             // disables the button until the importer has exited
        clicked = GetTickCount64();
        step = 1;
      } else if (step == 1) {
        // The import is in once the button is back (the catalog read again
        // in the same message): then a second host, or none within 10 s.
        if (!IsWindowEnabled(GetDlgItem(dlg, IDC_IMPORT))) return;
        if (finals(pid).size() < 2 && GetTickCount64() - clicked < 10000) return;
        PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
        step = 2;
      }
    });
    CHECK(step == 2);
    if (!expect_exit(w, r, 0)) return 1;
    CHECK(count_in_log(w.scr_log, "dialog: catalog reloaded") == 1);
    auto v = finals(dialog_pid);
    CHECK(v.size() == 2);
    if (v.size() == 2) {
      CHECK(v[0]["ADSCREENH"] == "480" && v[0]["ADSCREENW"] != "640");   // 856x480 (848x480 in a taller box)
      CHECK(screen_is(v[1], "640", "480"));
    }
    // In the log: the spawn without a screen of its own, the new catalog,
    // then the spawn with it, and no other.
    const auto lines = lines_of(w.scr_log);
    int without = -1, reloaded = -1, with = -1, spawns = 0;
    for (int i = 0; i < (int)lines.size(); ++i) {
      const std::string& l = lines[i];
      if (l.find("dialog: catalog reloaded") != std::string::npos) reloaded = i;
      if (l.find("live preview: spawn ") == std::string::npos) continue;
      ++spawns;
      if (l.find("FINAL.AD size=") == std::string::npos) continue;
      if (l.find(" abi=afterdark screen=0x0 ") != std::string::npos) without = i;
      if (l.find(" size=640x480 abi=afterdark screen=640x480 ") != std::string::npos) with = i;
    }
    CHECK(spawns == 2 && without >= 0 && reloaded > without && with > reloaded);
    check_hosts_gone(w);
    if (g_failures) dump_logs(w);
  }
  return 0;
}

// ---- twelve releases --------------------------------------------------------------------------

// The settings dialog with twelve releases (catalog-twelve.json: the seven,
// and Marvel Comics Screen Posters, Snoopy's Screen Savers, The Looney Tunes
// Screen Saver, ScreamSavers and The Disney Collection Screen Saver). Their
// regular covers never all fit side by side, nor do their compact ones in a
// window under 1120 DIP wide, so in these windows the strip scrolls:
//  * off screen at the first-open size, the smallest and one as narrow but
//    760 DIP tall, at 100% and 150%, light, dark and high contrast,
//    unscrolled, at a stop in the middle and at the last: as many tiles show
//    as the layout says, each wholly in the strip, clear of the chevrons (and
//    unscrolled, of the left one's place) and the status line, the others
//    outside the strip; in the picture the strip holds nothing but those
//    tiles and the chevrons on its base colour (a cover cut at an end would
//    show there); in Random, every module checked, screen readers call Star
//    Trek's group "…, all 4 in rotation" and Marvel's, of one module, "…, 1
//    in rotation" (never "all 1");
//  * driven by control ID, as many covers at a time as layout_window gives
//    the window's client: the last release's tile, not shown, takes the
//    focus and scrolls into view; the left chevron takes it away again, a
//    stop a click; Space on it (it keeps the focus) filters the list to its
//    release and brings it back into view; Show all shows every release; the
//    left chevron back to the first stop leaves its place empty (a click too
//    many lands on no cover); reopened with that filter saved, the row opens
//    with its tile showing; and from seven releases (their covers side by
//    side, no chevron) an import (fakeimport) brings twelve: the row
//    unscrolled, a right chevron;
//  * ScreamSavers' modules are After Dark modules with "screen": "640x480":
//    the live preview runs one at 640x480 where a Disney module runs at the
//    preview box's 16:9 480 lines, and /s at the 720-line setting on a 16:9
//    monitor, rotating between the two, asks for 640x480 and 1280x720, host
//    by host.
int test_config_twelve(const Opts& o) {
  Work w = prepare_six(o, "config-twelve", "catalog-twelve.json");
  auto rect_of = [](const std::string& v) {
    RectI r{};
    return sscanf(v.c_str(), "%d,%d,%d,%d", &r.x, &r.y, &r.w, &r.h) == 4 ? r : RectI{};
  };
  auto overlap = [](const RectI& a, const RectI& b) {
    return a.w > 0 && b.w > 0 && a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
  };
  auto within = [](const RectI& a, const RectI& b) {
    return a.x >= b.x && a.y >= b.y && a.x + a.w <= b.x + b.w && a.y + a.h <= b.y + b.h;
  };
  struct Shot {
    const char* state;
    const char* mode;
    int slots, first;
    const char* file;   // the module the details show, previewed
  };
  const Shot shots[] = {
      {"theme=light;dpi=96;size=1040x800;mode=single;module=screams.papa", "regular", 7, 0, "PAPA.AD"},
      {"theme=dark;dpi=144;size=1040x800;mode=random;module=disney.sierra", "regular", 7, 0, "SIERRA.AD"},
      {"theme=light;dpi=144;size=900x680;mode=random;module=marvel.kilo", "compact", 8, 0, "KILO.AD"},
      {"theme=dark;dpi=96;size=900x800;mode=single;module=screams.quebec", "regular", 5, 0, "QUEBEC.AD"},
      {"theme=hc;dpi=96;size=1040x800;mode=single;module=disney.tango;collections=disney;focus=strip", "regular", 7, 3,
       "TANGO.AD"},
      {"theme=light;dpi=144;size=900x680;mode=random;module=ad10.gamma;collections=ad10;focus=strip", "compact", 8, 4,
       "GAMMA.AD"},
      {"theme=dark;dpi=144;size=900x800;mode=single;module=screams.romeo;collections=looney,screams;focus=strip",
       "regular", 5, 1, "ROMEO.AD"},
  };
  for (const Shot& shot : shots) {
    fs::remove(w.host_log);
    auto kv = dialog_report(o, w, std::string(shot.state) + ";wait=2500;frames=2");
    const int dpi = std::max(96, atoi(kv["dpi"].c_str())), fm = std::max(3, MulDiv(3, dpi, 96));
    const RectI strip = rect_of(kv["strip"]), status = rect_of(kv["strip_status"]);
    const RectI box{strip.x - fm, strip.y - fm, strip.w + 2 * fm, strip.h + 2 * fm};   // the strip's window
    const RectI left = rect_of(kv["chevron_left"]), right = rect_of(kv["chevron_right"]);
    const int first = atoi(kv["strip_first"].c_str()), slots = atoi(kv["strip_slots"].c_str());
    CHECK(kv["strip_mode"] == shot.mode && slots == shot.slots && first == shot.first);
    CHECK(atoi(kv["strip_max_first"].c_str()) == 12 - shot.slots && strip.w > 0 && status.w > 0);
    CHECK((kv["chevron_left"] != "hidden") == (first > 0) && (kv["chevron_right"] != "hidden") == (first < 12 - slots));
    for (const RectI& c : {left, right}) CHECK(c.w == 0 || (within(c, box) && !overlap(c, status)));
    std::vector<RectI> shown;
    // The left chevron's place (the strip's left edge, 24 DIP), empty while
    // the row is unscrolled: every tile shown starts past it at every stop.
    const int left_end = strip.x + MulDiv(kStripChevronW, dpi, 96);
    for (int i = 0; i < 12; ++i) {
      const std::string v = kv["tile" + std::to_string(i)];
      const bool want = i >= shot.first && i < shot.first + shot.slots;
      if (want != (v != "hidden")) failf("config-twelve %s: tile %d is %s", shot.state, i, v.c_str());
      if (v == "hidden") continue;
      const RectI t = rect_of(v);
      shown.push_back(t);
      CHECK(within(t, box) && !overlap(t, left) && !overlap(t, right) && !overlap(t, status));
      if (t.x < left_end - 1) failf("config-twelve %s: tile %d in the left chevron's place (%d < %d)", shot.state, i, t.x, left_end);
    }
    // In the picture: nothing in the strip but the tiles shown and the
    // chevrons, on its base colour.
    int w_px = 0, h_px = 0, off = 0;
    unsigned base = 0;
    std::vector<uint8_t> px;
    if (sscanf(kv["base"].c_str(), "%x", &base) == 1 && load_png(w.dir / "dialog.png", &w_px, &h_px, &px) &&
        box.x >= 0 && box.y >= 0 && box.x + box.w <= w_px && box.y + box.h <= h_px) {
      for (int y = box.y; y < box.y + box.h; ++y) {
        for (int x = box.x; x < box.x + box.w; ++x) {
          bool owned = false;
          for (const RectI& t : shown) owned |= x >= t.x && x < t.x + t.w && y >= t.y && y < t.y + t.h;
          for (const RectI& c : {left, right}) owned |= c.w > 0 && x >= c.x && x < c.x + c.w && y >= c.y && y < c.y + c.h;
          if (owned) continue;
          const uint8_t* p = &px[((size_t)y * w_px + x) * 4];
          off += std::abs(p[2] - (int)((base >> 16) & 0xFF)) > 2 || std::abs(p[1] - (int)((base >> 8) & 0xFF)) > 2 ||
                 std::abs(p[0] - (int)(base & 0xFF)) > 2;
        }
      }
      if (off) failf("config-twelve %s: %d pixels drawn in the strip outside its tiles and chevrons", shot.state, off);
    } else {
      failf("config-twelve %s: no picture of the strip", shot.state);
    }
    // What screen readers call the groups in Random, every module checked
    // (no filter: Star Trek's group first, then Marvel's single module).
    const std::string state = shot.state;
    if (state.find("mode=random") != std::string::npos && state.find("collections=") == std::string::npos) {
      CHECK(kv["group0"] == "Star Trek: The Screen Saver, all 4 in rotation");
      CHECK(kv["group1"] == "Marvel Comics Screen Posters, 1 in rotation");
    }
    // The module the details show, previewed: ScreamSavers' and Marvel's at
    // their own 640x480, the rest at the box's 16:9 480 lines.
    int previews = 0;
    for (auto& e : host_events(w, "start")) {
      if (!ends_with(e["module"], shot.file)) continue;
      ++previews;
      const std::string file = shot.file;
      const bool own = file == "PAPA.AD" || file == "QUEBEC.AD" || file == "ROMEO.AD" || file == "KILO.AD";
      if (own) CHECK(screen_is(e, "640", "480"));
      else CHECK(e["ADSCREENH"] == "480" && e["ADSCREENW"] != "640");   // 856x480 (848x480 in a taller box)
    }
    CHECK(previews >= 1);
    if (g_failures) {
      fprintf(stderr, "config-twelve %s:\n", shot.state);
      for (auto& [k, v] : kv) fprintf(stderr, "  %s=%s\n", k.c_str(), v.c_str());
      dump_logs(w);
      return 1;
    }
  }

  // Driven by control ID, the window at its first-open size, clamped to this
  // monitor's work area: as many covers at a time as layout_window gives its
  // client (seven regular ones at 1040x800 DIP; ten compact ones where the
  // work area clamps the height under 760 DIP, as on 1920x1080 at 125% or
  // 150%, or 1366x768 at 100%).
  {
    fs::remove(w.scr_log);
    fs::remove(w.host_log);
    EnvList env = base_env(o, w);
    // The tile lies wholly in the strip (it shows), or wholly outside it.
    auto where = [](HWND dlg, int i) {
      RECT t{}, s{}, both{};
      GetWindowRect(tile_of(dlg, i), &t);
      GetWindowRect(strip_of(dlg), &s);
      return !IntersectRect(&both, &t, &s) ? 'o' : EqualRect(&both, &t) ? 's' : 'c';   // outside, shows, cut
    };
    auto row = [&](HWND dlg) {
      std::string r;
      for (int i = 0; i < 12; ++i) r += where(dlg, i);
      return r;
    };
    HWND dlg = nullptr;
    int step = 0, groups3 = -1, groups4 = -1, want = -1, client_dpi = 0;
    RECT client{};
    bool left_empty = false;   // back at the first stop, the left chevron's place holds no tile
    std::string row0, row1, row2, row3, row4, status3, status4;
    RunResult r = run_scr(o, L"/c", env, 60000, [&](DWORD pid) {
      if (!dlg) {
        HWND d = find_dialog(pid);
        HWND list = d ? GetDlgItem(d, IDC_MODULE_LIST) : nullptr;
        if (!list || item_count(list) == 0 || !tile_of(d, 11)) return;
        dlg = d;
      }
      HWND list = GetDlgItem(dlg, IDC_MODULE_LIST), strip = strip_of(dlg);
      if (step == 0) {
        // The tiles a stop shows in this client, as the dialog lays it out.
        GetClientRect(dlg, &client);
        client_dpi = (int)GetDpiForWindow(dlg);
        LayoutInput li{client.right, client.bottom, client_dpi, true};
        li.strip_tiles = 12;
        want = layout_window(li).tiles.slots;
        row0 = row(dlg);
        // The last release's tile takes the focus, as Tab or the arrow keys give it.
        SendMessageW(dlg, WM_NEXTDLGCTL, (WPARAM)tile_of(dlg, 11), TRUE);
        row1 = row(dlg);
        // The left chevron, twice: two stops back.
        HWND prev = GetDlgItem(strip, IDC_STRIP_PREV);
        for (int k = 0; k < 2; ++k) SendMessageW(strip, WM_COMMAND, MAKEWPARAM(IDC_STRIP_PREV, BN_CLICKED), (LPARAM)prev);
        row2 = row(dlg);
        // Space on it: it still has the focus.
        PostMessageW(tile_of(dlg, 11), WM_KEYDOWN, VK_SPACE, 0x00390001);
        PostMessageW(tile_of(dlg, 11), WM_KEYUP, VK_SPACE, 0xC0390001);
        step = 1;
      } else if (step == 1) {
        if (SendMessageW(tile_of(dlg, 11), BM_GETCHECK, 0, 0) != BST_CHECKED) return;
        row3 = row(dlg);
        groups3 = group_count(list);
        status3 = window_text(GetDlgItem(dlg, IDC_STRIP_STATUS));
        click(dlg, IDC_STRIP_SHOW_ALL);
        groups4 = group_count(list);
        status4 = window_text(GetDlgItem(dlg, IDC_STRIP_STATUS));
        // The left chevron back to the first stop, a click a stop and more:
        // where it showed (a pointer left there), no tile then.
        HWND prev = GetDlgItem(strip, IDC_STRIP_PREV);
        RECT place{};
        const bool showed = IsWindowVisible(prev) && GetWindowRect(prev, &place);
        for (int k = 0; k < 12; ++k) SendMessageW(strip, WM_COMMAND, MAKEWPARAM(IDC_STRIP_PREV, BN_CLICKED), (LPARAM)prev);
        row4 = row(dlg);
        left_empty = showed && !IsWindowVisible(prev);
        for (int i = 0; i < 12; ++i) {
          RECT t{}, both{};
          if (GetWindowRect(tile_of(dlg, i), &t) && IntersectRect(&both, &t, &place)) left_empty = false;
        }
        PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
        step = 2;
      }
    });
    CHECK(step == 2);
    if (!expect_exit(w, r, 0)) return 1;
    const int k = (int)std::count(row0.begin(), row0.end(), 's');   // tiles at a time
    CHECK(k == want && k >= 1 && k < 12);
    if (k >= 1 && k < 12) {
      const std::string s(k, 's'), rest(12 - k, 'o');
      const int back2 = std::max(0, 12 - k - 2);
      CHECK(row0 == s + rest);                                                          // unscrolled: the first k
      CHECK(row1 == rest + s);                                                          // the last stop: the last k
      CHECK(row2 == std::string(back2, 'o') + s + std::string(12 - k - back2, 'o'));   // two stops back
      CHECK(row3 == row1);                                                              // toggled: back in view
      CHECK(row4 == row0 && left_empty);                                                // unscrolled again
    }
    CHECK(groups3 == 1 && status3 == "Showing 1 of 12 releases");
    CHECK(groups4 == 12 && status4 == "Click covers to filter the list");
    check_hosts_gone(w);
    if (g_failures) {
      fprintf(stderr,
              "config-twelve driven: client %ldx%ld @%d, %d tiles at a time (the layout's %d), rows %s / %s / %s / %s / %s, "
              "left chevron's place %s, groups %d / %d, status \"%s\" / \"%s\"\n",
              client.right, client.bottom, client_dpi, k, want, row0.c_str(), row1.c_str(), row2.c_str(), row3.c_str(),
              row4.c_str(), left_empty ? "empty" : "not empty", groups3, groups4, status3.c_str(), status4.c_str());
      dump_logs(w);
      return 1;
    }
    // Reopened with the filter saved on the last release: its cover shows
    // (the row at its last stop), not only the status line.
    edit_settings(w, [](Settings& s) { s.collections = {"ad10"}; });
    dlg = nullptr;
    step = 0;
    std::string row_saved, status_saved;
    r = run_scr(o, L"/c", env, 60000, [&](DWORD pid) {
      if (step) return;
      HWND d = find_dialog(pid);
      HWND list = d ? GetDlgItem(d, IDC_MODULE_LIST) : nullptr;
      if (!list || item_count(list) == 0 || !tile_of(d, 11)) return;
      row_saved = row(d);
      status_saved = window_text(GetDlgItem(d, IDC_STRIP_STATUS));
      PostMessageW(d, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
      step = 1;
    });
    CHECK(step == 1);
    if (!expect_exit(w, r, 0)) return 1;
    if (k >= 1 && k < 12) CHECK(row_saved == std::string(12 - k, 'o') + std::string(k, 's'));
    CHECK(status_saved == "Showing 1 of 12 releases");
    edit_settings(w, [](Settings& s) { s.collections.clear(); });
    if (g_failures) {
      fprintf(stderr, "config-twelve reopened: row %s, status \"%s\"\n", row_saved.c_str(), status_saved.c_str());
      dump_logs(w);
      return 1;
    }
    // From seven releases to twelve through Import… (fakeimport leaves the
    // twelve-release catalog): the seven covers side by side, no chevron (as
    // the first-open window holds them on every monitor but a narrow one);
    // after the reload twelve, the row unscrolled with the right chevron.
    const fs::path catalog = w.assets / "win" / "catalog-win.json", imported = w.dir / "catalog-imported.json";
    std::string twelve, seven;
    CHECK(read_file(catalog.wstring(), twelve) && write_file_atomic(imported.wstring(), twelve));
    CHECK(read_file((fs::path(o.fixtures) / "catalog-seven.json").wstring(), seven) &&
          write_file_atomic(catalog.wstring(), seven));
    EnvList ienv = base_env(o, w);
    ienv.push_back({L"FAKEIMPORT_EXIT", L"0"});
    ienv.push_back({L"FAKEIMPORT_CATALOG", imported.wstring()});
    dlg = nullptr;
    step = 0;
    std::string row7, row12, assets7, assets12;
    bool chevron7 = true, right12 = false, left12 = true;
    int want7 = -1;   // the seven's tiles at a time in this client, as the dialog lays it out
    r = run_scr(o, L"/c", ienv, 60000, [&](DWORD pid) {
      if (step == 2) return;
      HWND d = find_dialog(pid);
      HWND list = d ? GetDlgItem(d, IDC_MODULE_LIST) : nullptr;
      if (!list || item_count(list) == 0 || !tile_of(d, 6)) return;
      HWND strip = strip_of(d);
      if (step == 0) {
        RECT cr{};
        GetClientRect(d, &cr);
        LayoutInput li{cr.right, cr.bottom, (int)GetDpiForWindow(d), true};
        li.strip_tiles = 7;
        want7 = layout_window(li).tiles.slots;
        for (int i = 0; i < 7; ++i) row7 += where(d, i);
        chevron7 = IsWindowVisible(GetDlgItem(strip, IDC_STRIP_PREV)) || IsWindowVisible(GetDlgItem(strip, IDC_STRIP_NEXT));
        assets7 = window_text(GetDlgItem(d, IDC_ASSETS_STATUS));
        click(d, IDC_IMPORT);   // Import… stays greyed until the importer has exited
        step = 1;
      } else if (step == 1) {
        if (!IsWindowEnabled(GetDlgItem(d, IDC_IMPORT)) || !tile_of(d, 11)) return;
        row12 = row(d);
        left12 = IsWindowVisible(GetDlgItem(strip, IDC_STRIP_PREV)) != FALSE;
        right12 = IsWindowVisible(GetDlgItem(strip, IDC_STRIP_NEXT)) != FALSE;
        assets12 = window_text(GetDlgItem(d, IDC_ASSETS_STATUS));
        PostMessageW(d, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
        step = 2;
      }
    });
    CHECK(step == 2);
    if (!expect_exit(w, r, 0)) return 1;
    CHECK(want7 >= 1 && want7 <= 7 && assets7 == "36 modules from 7 releases");
    if (want7 >= 1 && want7 <= 7) CHECK(row7 == std::string(want7, 's') + std::string(7 - want7, 'o') && chevron7 == (want7 < 7));
    if (k >= 1 && k < 12) CHECK(row12 == std::string(k, 's') + std::string(12 - k, 'o'));
    CHECK(!left12 && right12 && assets12 == "46 modules from 12 releases");
    CHECK(count_in_log(w.scr_log, "dialog: catalog reloaded") == 1);
    check_hosts_gone(w);
    if (g_failures) {
      fprintf(stderr, "config-twelve import: rows %s / %s (the layout's %d / %d), chevrons %d / %d,%d, assets \"%s\" / \"%s\"\n",
              row7.c_str(), row12.c_str(), want7, k, chevron7, left12, right12, assets7.c_str(), assets12.c_str());
      dump_logs(w);
      return 1;
    }
  }

  // /s at the 720-line setting on a 16:9 monitor staged off every real one,
  // rotating between a ScreamSavers and a Disney module: each host its
  // module's size, 640x480 and 1280x720, switch after switch.
  {
    fs::remove(w.scr_log);
    fs::remove(w.host_log);
    edit_settings(w, [](Settings& s) {
      s.scale = 1.5;
      s.all_monitors = false;
      s.module = "random";
      s.randomize = {"screams.papa", "disney.sierra"};
      s.collections.clear();
    });
    EnvList env = base_env(o, w);
    env.push_back({L"AD_SCR_TEST_MONITORS", L"-16000,0,1280,720,p"});
    env.push_back({L"AD_SCR_TEST_ROTATE_MS", L"300"});
    bool closed = false;
    const ULONGLONG t0 = GetTickCount64();
    RunResult r = run_scr(o, L"/s", env, 60000, [&](DWORD pid) {
      if (!closed && (host_events(w, "start").size() >= 4 || GetTickCount64() - t0 > 20000)) {
        close_saver(pid);
        closed = true;
      }
    });
    if (!expect_exit(w, r, 0)) return 1;
    auto starts = host_events(w, "start");
    CHECK(starts.size() >= 4);
    int screams = 0, disney = 0;
    std::string last;
    for (auto& e : starts) {
      if (ends_with(e["module"], "PAPA.AD")) {
        ++screams;
        CHECK(screen_is(e, "640", "480"));
      } else if (ends_with(e["module"], "SIERRA.AD")) {
        ++disney;
        CHECK(screen_is(e, "1280", "720"));
      } else {
        failf("config-twelve: a host for %s", e["module"].c_str());
      }
      CHECK(e["module"] != last);   // two modules: every switch a new host, of the other size
      last = e["module"];
    }
    CHECK(screams >= 2 && disney >= 2);
    CHECK(count_in_log(w.scr_log, "size=640x480 ") == screams && count_in_log(w.scr_log, "size=1280x720 ") == disney);
    check_hosts_gone(w);
    if (g_failures) dump_logs(w);
  }
  return 0;
}

// The tools of the tooltip windows `dlg` owns: each tool's window (a
// TTF_IDISHWND tool's uId; null for the others) and its text, read through
// memory in the dialog's process (TOOLINFO carries a pointer, which
// comctl32 doesn't marshal across processes).
std::vector<std::pair<HWND, std::string>> tooltip_tools(HWND dlg) {
  std::vector<HWND> tips;
  struct Find {
    HWND dlg;
    std::vector<HWND>* tips;
  } f{dlg, &tips};
  EnumWindows([](HWND h, LPARAM lp) -> BOOL {
    auto* f = reinterpret_cast<Find*>(lp);
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    if (GetWindow(h, GW_OWNER) == f->dlg && wcscmp(cls, TOOLTIPS_CLASSW) == 0) f->tips->push_back(h);
    return TRUE;
  }, reinterpret_cast<LPARAM>(&f));
  std::vector<std::pair<HWND, std::string>> out;
  DWORD pid = 0;
  GetWindowThreadProcessId(dlg, &pid);
  HANDLE p = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
  if (!p) return out;
  constexpr size_t kChars = 1024;   // longer than any tip the dialog sets
  if (void* mem = VirtualAllocEx(p, nullptr, sizeof(TOOLINFOW) + kChars * sizeof(wchar_t), MEM_COMMIT | MEM_RESERVE,
                                 PAGE_READWRITE)) {
    auto* remote_text = reinterpret_cast<wchar_t*>(static_cast<char*>(mem) + sizeof(TOOLINFOW));
    std::vector<wchar_t> text(kChars);
    for (HWND tip : tips) {
      const int n = (int)SendMessageW(tip, TTM_GETTOOLCOUNT, 0, 0);
      for (int i = 0; i < n; ++i) {
        TOOLINFOW ti{};
        ti.cbSize = sizeof(ti);
        ti.lpszText = remote_text;
        std::fill(text.begin(), text.end(), L'\0');
        if (!WriteProcessMemory(p, mem, &ti, sizeof(ti), nullptr) ||
            !WriteProcessMemory(p, remote_text, text.data(), kChars * sizeof(wchar_t), nullptr) ||
            !SendMessageW(tip, TTM_ENUMTOOLSW, (WPARAM)i, (LPARAM)mem)) {
          continue;
        }
        TOOLINFOW back{};
        ReadProcessMemory(p, mem, &back, sizeof(back), nullptr);
        ReadProcessMemory(p, remote_text, text.data(), kChars * sizeof(wchar_t), nullptr);
        text.back() = L'\0';
        out.push_back({(back.uFlags & TTF_IDISHWND) ? (HWND)back.uId : nullptr, narrow(text.data())});
      }
    }
    VirtualFreeEx(p, mem, 0, MEM_RELEASE);
  }
  CloseHandle(p);
  return out;
}

// The control with the keyboard focus in `dlg`'s thread (GetFocus() answers
// for the caller's own thread only).
HWND focus_in(HWND dlg) {
  GUITHREADINFO gi{};
  gi.cbSize = sizeof(gi);
  return GetGUIThreadInfo(GetWindowThreadProcessId(dlg, nullptr), &gi) ? gi.hwndFocus : nullptr;
}

// A push button with the default-button state (BS_DEFPUSHBUTTON): the one
// the dialog manager's Enter clicks while it has the focus.
bool default_button(HWND b) { return (GetWindowLongW(b, GWL_STYLE) & BS_TYPEMASK) == BS_DEFPUSHBUTTON; }

// The footer's credit (PLAN §6a.3): "Made With Love by StarrLord", a link
// between the assets line and Preview. In the dialog: shown, named so for
// screen readers (with where it goes), between Import and Preview in the tab
// order; a click, Enter and Space each ask for the project's page, which the
// test build records (AD_SCR_TEST_OPEN_LOG) and never opens. Hidden while it
// has the focus (the window narrowed under it), it hands the focus on to
// Preview with the default-button state, so Enter there runs Preview. Rendered
// off screen: clear of the assets line's text and of Preview by the footer's
// gap at the first-open size and the minimum, at 100% and 150%, in the
// welcome too; hidden, never clipped, when the assets line leaves it no room.
int test_config_credit(const Opts& o) {
  Work w = prepare_six(o, "config-credit");
  edit_settings(w, [](Settings& s) { s.module = "swse.vader"; });
  const fs::path open_log = w.dir / "open.log";
  const std::string url = "https://github.com/starrlord/longafterdark";
  EnvList env = base_env(o, w);
  env.push_back({L"AD_SCR_TEST_OPEN_LOG", open_log.wstring()});
  auto requests = [&] { return count_in_log(open_log, "open\t" + url); };
  bool visible = false, tip = false;
  std::string text;
  A11y a;
  HWND tab_after_import = nullptr, tab_after_credit = nullptr, credit_hwnd = nullptr, preview_hwnd = nullptr;
  int step = 0, after_click = -1, after_enter = -1, after_space = -1;
  RunResult r = run_scr(o, L"/c", env, 60000, [&](DWORD pid) {
    HWND dlg = find_dialog(pid);
    HWND list = dlg ? GetDlgItem(dlg, IDC_MODULE_LIST) : nullptr;
    if (!list || SendMessageW(list, LVM_GETITEMCOUNT, 0, 0) == 0) return;
    HWND link = GetDlgItem(dlg, IDC_FOOTER_CREDIT);
    if (step == 0) {
      credit_hwnd = link;
      preview_hwnd = GetDlgItem(dlg, IDC_PREVIEW);
      visible = link && IsWindowVisible(link);
      text = window_text(link);
      a = a11y_of(link);
      tab_after_import = GetNextDlgTabItem(dlg, GetDlgItem(dlg, IDC_IMPORT), FALSE);
      tab_after_credit = GetNextDlgTabItem(dlg, link, FALSE);
      for (const auto& [tool, tool_text] : tooltip_tools(dlg)) tip |= tool == link && tool_text == url;
      click(dlg, IDC_FOOTER_CREDIT);
      after_click = requests();
      // The keyboard: the link focused as Tab would, then Enter.
      SendMessageW(dlg, WM_NEXTDLGCTL, (WPARAM)link, TRUE);
      PostMessageW(link, WM_KEYDOWN, VK_RETURN, 0x001C0001);
      PostMessageW(link, WM_KEYUP, VK_RETURN, 0xC01C0001);
      step = 1;
    } else if (step == 1 && requests() >= 2) {
      after_enter = requests();
      PostMessageW(link, WM_KEYDOWN, VK_SPACE, 0x00390001);
      PostMessageW(link, WM_KEYUP, VK_SPACE, 0xC0390001);
      step = 2;
    } else if (step == 2 && requests() >= 3) {
      after_space = requests();
      PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
      step = 3;
    }
  });
  if (!expect_exit(w, r, 0)) return 1;
  CHECK(step == 3);
  // What screen readers hear is what the link draws, and it reads as asked.
  const std::string phrase = narrow(std::wstring(kFooterCreditLead) + L" " + kFooterCreditName);
  CHECK(phrase == "Made With Love by StarrLord");
  CHECK(visible && text == phrase);
  CHECK(tip);   // its tooltip is the address
  CHECK(a.ok && a.name == phrase && a.control_type == UIA_ButtonControlTypeId);
  CHECK(a.help == "Opens " + url + " in your browser");
  CHECK(credit_hwnd && tab_after_import == credit_hwnd && tab_after_credit == preview_hwnd);
  CHECK(after_click == 1 && after_enter == 2 && after_space == 3);
  CHECK(count_in_log(w.scr_log, "dialog: open " + url + " (the test build opens nothing)") == 3);
  check_hosts_gone(w);
  if (g_failures) {
    fprintf(stderr, "credit: visible=%d tip=%d text=\"%s\" name=\"%s\" type=%d help=\"%s\" requests=%d/%d/%d\n",
            visible, tip, text.c_str(), a.name.c_str(), a.control_type, a.help.c_str(), after_click, after_enter,
            after_space);
    dump_logs(w);
  }

  // The link hiding while it has the focus. With Star Wars Screen
  // Entertainment alone imported, its long title leaves the credit room only
  // in a wide window: the link is focused there as Tab would focus it (the
  // dialog manager makes it the default push button), then the window is
  // narrowed to its minimum. The link hides and the focus moves on to Preview
  // with the default-button state (Preview BS_DEFPUSHBUTTON, the link no
  // longer), so Enter there runs Preview's /s and the dialog stays open, having
  // saved nothing. Left with the hidden link, the default state sends Enter to
  // the dialog's default, OK, which saves and closes.
  {
    Work f = prepare_swse_only(o, "config-credit-focus");
    edit_settings(f, [](Settings& s) { s.module = "swse.vader"; });
    EnvList fenv = base_env(o, f);
    // Preview's /s inherits these: it runs on a monitor staged off every real
    // one and ends itself, its temporary files in the test's folder.
    const fs::path tmp = f.dir / "tmp";
    fs::create_directories(tmp);
    fenv.push_back({L"AD_SCR_TEST_MONITORS", L"-16000,0,1280,720,p"});
    fenv.push_back({L"AD_SCR_TESTEXIT_AFTER_FRAMES", L"20"});
    fenv.push_back({L"TMP", tmp.wstring()});
    fenv.push_back({L"TEMP", tmp.wstring()});
    bool wide = false, focused = false, link_default = false, hidden = false, on_preview = false,
         preview_default = false, link_plain = false, forced = false, reactivated_on_preview = false, still_open = false;
    int fstep = 0;
    DWORD dialog_pid = 0;
    RunResult fr = run_scr(o, L"/c", fenv, 60000, [&](DWORD pid) {
      dialog_pid = pid;
      HWND dlg = find_dialog(pid);
      HWND list = dlg ? GetDlgItem(dlg, IDC_MODULE_LIST) : nullptr;
      if (!list || SendMessageW(list, LVM_GETITEMCOUNT, 0, 0) == 0) return;
      HWND link = GetDlgItem(dlg, IDC_FOOTER_CREDIT), preview = GetDlgItem(dlg, IDC_PREVIEW);
      if (fstep == 0) {
        if (!IsWindowEnabled(preview)) return;   // not until the host's answer is in
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(MonitorFromWindow(dlg, MONITOR_DEFAULTTONEAREST), &mi);
        const RECT& wa = mi.rcWork;
        RECT wr{};
        GetWindowRect(dlg, &wr);
        const int height = std::min(wr.bottom - wr.top, wa.bottom - wa.top);
        // As wide as its monitor's work area (SetWindowPos returns once the
        // dialog has laid itself out again).
        SetWindowPos(dlg, nullptr, wa.left, wa.top, wa.right - wa.left, height, SWP_NOZORDER | SWP_NOACTIVATE);
        wide = IsWindowVisible(link) != FALSE;
        if (!wide) {
          // A monitor too narrow for the credit beside this assets line: no
          // step here (it is skipped below, not failed).
          PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
          fstep = 3;
          return;
        }
        SendMessageW(dlg, WM_NEXTDLGCTL, (WPARAM)link, TRUE);
        focused = focus_in(dlg) == link;
        link_default = default_button(link);
        // The narrowest it goes: SetWindowPos stops at the window's minimum size.
        SetWindowPos(dlg, nullptr, 0, 0, 1, height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        hidden = !IsWindowVisible(link);
        on_preview = focus_in(dlg) == preview;
        preview_default = default_button(preview);
        link_plain = !default_button(link);
        // The link hidden while the dialog was inactive: the dialog manager
        // saved the focus on it and gives it back on activation. The focus
        // put there as it would be, then a deactivation and an activation.
        SendMessageW(dlg, WM_NEXTDLGCTL, (WPARAM)link, TRUE);
        forced = focus_in(dlg) == link;
        SendMessageW(dlg, WM_ACTIVATE, MAKEWPARAM(WA_INACTIVE, 0), 0);
        SendMessageW(dlg, WM_ACTIVATE, MAKEWPARAM(WA_ACTIVE, 0), 0);
        fstep = 10;
      } else if (fstep == 10) {
        // The dialog's check after the restore has run (posted, before this poll).
        reactivated_on_preview = focus_in(dlg) == preview && default_button(preview) && !default_button(link);
        if (!reactivated_on_preview) {
          // Enter would go to the hidden link: stop here, the checks below say why.
          PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
          fstep = 3;
          return;
        }
        PostMessageW(preview, WM_KEYDOWN, VK_RETURN, 0x001C0001);
        PostMessageW(preview, WM_KEYUP, VK_RETURN, 0xC01C0001);
        fstep = 1;
      } else if (fstep == 1 && !hosts_of(f, pid, true).empty()) {
        // Preview's /s has started its host, and the dialog is still there.
        still_open = IsWindow(dlg) && count_in_log(f.scr_log, "dialog: saved") == 0;
        fstep = 2;
      } else if (fstep == 2 && IsWindowEnabled(preview)) {
        // The Preview has ended (its test exit) and the dialog took it back.
        PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
        fstep = 3;
      }
    });
    if (!expect_exit(f, fr, 0)) return 1;
    if (!wide) {
      // The credit needs about 1,030 DIP beside the Star Wars-only assets
      // line; a narrower work area never shows it, so there is nothing to hide.
      fprintf(stderr, "credit focus: skipped (the credit does not show at this monitor's work-area width)\n");
    } else {
      CHECK(focused && link_default);
      CHECK(hidden && on_preview && preview_default && link_plain);
      CHECK(forced && reactivated_on_preview);
      CHECK(fstep == 3 && still_open);
      CHECK(count_in_log(f.scr_log, "dialog: saved") == 0);
      auto preview_hosts = hosts_of(f, dialog_pid, true);
      CHECK(!preview_hosts.empty());
      for (auto& e : preview_hosts) CHECK(ends_with(e["module"], "VADER.IMX"));
    }
    check_hosts_gone(f);
    if (g_failures) {
      fprintf(stderr, "credit focus: step=%d wide=%d focused=%d link_default=%d hidden=%d on_preview=%d "
                      "preview_default=%d link_plain=%d forced=%d reactivated_on_preview=%d still_open=%d\n",
              fstep, wide, focused, link_default, hidden, on_preview, preview_default, link_plain, forced,
              reactivated_on_preview, still_open);
      dump_logs(f);
    }
  }

  // Off screen: where it shows, at 100% and 150%, light and dark.
  auto rect_of = [](const std::string& v) {
    RectI r{};
    return sscanf(v.c_str(), "%d,%d,%d,%d", &r.x, &r.y, &r.w, &r.h) == 4 ? r : RectI{};
  };
  struct Shot {
    const char* state;
    bool shown;
  };
  // (At 150% in the narrowest window it fits by a few pixels in Segoe UI
  // Variable, so there it is only held to the rules when it shows.)
  for (const Shot& shot : {Shot{"theme=light;dpi=96;size=1040x800", true}, Shot{"theme=dark;dpi=144;size=1040x800", true},
                           Shot{"theme=hc;dpi=144;size=1040x800", true}, Shot{"theme=light;dpi=96;size=900x680", true},
                           Shot{"theme=dark;dpi=144;size=900x680", false}}) {
    auto kv = dialog_report(o, w, std::string(shot.state) + ";mode=single;module=swse.vader;wait=2500;frames=2");
    if (!shot.shown && kv["credit"] == "hidden") continue;
    const RectI box = rect_of(kv["credit"]), lead = rect_of(kv["credit_lead"]), name = rect_of(kv["credit_name"]);
    const RectI assets = rect_of(kv["assets_text"]), preview = rect_of(kv["preview_button"]);
    const int dpi = atoi(kv["dpi"].c_str()), gap = dip(kCreditGapDip, std::max(96, dpi));
    CHECK(kv["credit"] != "hidden" && box.w > 0 && preview.w > 0 && assets.w > 0);
    CHECK(box.x >= assets.x + assets.w + gap && box.x + box.w <= preview.x - gap);
    CHECK(std::abs((box.x - (assets.x + assets.w)) - (preview.x - (box.x + box.w))) <= 1);   // centred between them
    CHECK(std::abs((box.y + box.h / 2) - (preview.y + preview.h / 2)) <= 1);               // on the buttons' line
    CHECK(lead.y == name.y && lead.x >= box.x && lead.x + lead.w < name.x && name.x + name.w <= box.x + box.w);
    CHECK(lead.y == assets.y);   // one line with the assets line's text
  }
  // The assets line at its longest (files missing) in the minimum window:
  // no room, so the credit hides.
  {
    Work m = prepare_six(o, "config-credit-missing");
    Catalog c;
    load_catalog((m.assets / "win" / "catalog-win.json").wstring(), c, nullptr);
    for (const char* id : {"ad40.alpha", "ad40.twin"}) {
      if (const Module* mod = c.find(id)) fs::remove(resolve_module_path((m.assets / "win").wstring(), mod->path));
    }
    auto kv = dialog_report(o, m, "theme=light;dpi=96;size=900x680;mode=single;module=swse.vader;wait=2500;frames=2");
    CHECK(kv["credit"] == "hidden" && kv["credit_lead"] == "hidden");
    if (g_failures) dump_logs(m);
  }
  // Nothing imported: the welcome's footer has room for it (no Import button).
  {
    Work e = prepare(o, "config-credit-welcome", false);
    auto kv = dialog_report(o, e, "theme=light;dpi=96;size=900x600;wait=1500");
    const RectI box = rect_of(kv["credit"]), assets = rect_of(kv["assets_text"]), preview = rect_of(kv["preview_button"]);
    CHECK(kv["credit"] != "hidden" && box.x > assets.x + assets.w && box.x + box.w < preview.x);
  }
  return 0;
}

// ---- the details card under the filter ---------------------------------------------------

// Selects row `item` of another process's list view as a click does (the
// dialog sees LVN_ITEMCHANGED): LVM_SETITEMSTATE's LVITEM is written into
// that process, since a pointer into ours would mean nothing there.
bool select_row(HWND list, int item) {
  DWORD pid = 0;
  GetWindowThreadProcessId(list, &pid);
  HANDLE p = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
  if (!p) return false;
  bool ok = false;
  if (void* mem = VirtualAllocEx(p, nullptr, sizeof(LVITEMW), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)) {
    LVITEMW it{};
    it.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
    it.state = LVIS_SELECTED | LVIS_FOCUSED;
    ok = WriteProcessMemory(p, mem, &it, sizeof(it), nullptr) &&
         SendMessageW(list, LVM_SETITEMSTATE, (WPARAM)item, (LPARAM)mem) != FALSE;
    VirtualFreeEx(p, mem, 0, MEM_RELEASE);
  }
  CloseHandle(p);
  return ok;
}

// A window's whole text (window_text() stops at 255 characters).
std::string long_text(HWND h) {
  const int n = (int)SendMessageW(h, WM_GETTEXTLENGTH, 0, 0);
  std::wstring buf((size_t)std::max(0, n) + 1, L'\0');
  SendMessageW(h, WM_GETTEXT, buf.size(), (LPARAM)buf.data());   // marshalled across processes
  buf.resize(wcslen(buf.c_str()));
  return narrow(buf);
}

// The details card follows the strip's filter and never shows a module the
// list hides, in Single and Random alike, yet showing is not choosing:
// Single's module changes only when a row is clicked. A filter that leaves
// no rows says so. The dialog's caption is "Long After Dark".
int test_config_details(const Opts& o) {
  Work w = prepare_releases(o, "config-details");
  EnvList env = base_env(o, w);
  using Ids = std::vector<std::string>;
  edit_settings(w, [](Settings& s) {
    s.module = "ad40.alpha";
    s.randomize.clear();
    s.collections.clear();
  });
  auto title_of = [](HWND d) { return window_text(GetDlgItem(d, IDC_MODULE_TITLE)); };
  auto selected_of = [](HWND d) {
    return (int)SendMessageW(GetDlgItem(d, IDC_MODULE_LIST), LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
  };
  auto ready = [](DWORD pid, int tiles) -> HWND {
    HWND d = find_dialog(pid);
    HWND list = d ? GetDlgItem(d, IDC_MODULE_LIST) : nullptr;
    if (!list || item_count(list) == 0 || !tile_of(d, tiles - 1)) return nullptr;
    return d;
  };

  // ---- session 1: Single module (Alpha Toasters, from Deluxe).
  struct Seen {
    std::string caption, t[8];
    int sel[8] = {-2, -2, -2, -2, -2, -2, -2, -2};
    int items1 = -1;
    bool picked = false;
  } v;
  int step = 0;
  RunResult r = run_scr(o, L"/c", env, 60000, [&](DWORD pid) {
    if (step) return;
    HWND d = ready(pid, 5);
    if (!d) return;
    HWND list = GetDlgItem(d, IDC_MODULE_LIST);
    v.caption = window_text(d);
    v.t[0] = title_of(d), v.sel[0] = selected_of(d);
    click_tile(d, 3);   // Totally Twisted
    click_tile(d, 4);   // ...and The Simpsons: Deluxe's module is hidden
    v.t[1] = title_of(d), v.sel[1] = selected_of(d), v.items1 = item_count(list);
    click_tile(d, 3);   // The Simpsons only: what showed is hidden now too
    v.t[2] = title_of(d), v.sel[2] = selected_of(d);
    click(d, IDC_STRIP_SHOW_ALL);   // the chosen module's row is back
    v.t[3] = title_of(d), v.sel[3] = selected_of(d);
    click_tile(d, 3);   // Totally Twisted only
    v.t[4] = title_of(d), v.sel[4] = selected_of(d);
    v.picked = select_row(list, 1);   // the user clicks "Foxtrot Twist"
    v.t[5] = title_of(d), v.sel[5] = selected_of(d);
    click_tile(d, 4);   // add The Simpsons: the chosen row stays
    v.t[6] = title_of(d), v.sel[6] = selected_of(d);
    PostMessageW(d, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), (LPARAM)GetDlgItem(d, IDOK));
    step = 1;
  });
  CHECK(step == 1);
  if (!expect_exit(w, r, 0)) return 1;
  printf("single: %s | %s | %s | %s | %s | %s | %s\n", v.t[0].c_str(), v.t[1].c_str(), v.t[2].c_str(), v.t[3].c_str(),
         v.t[4].c_str(), v.t[5].c_str(), v.t[6].c_str());
  CHECK(v.caption == "Long After Dark");
  CHECK(v.t[0] == "Alpha Toasters" && v.sel[0] >= 0);
  CHECK(v.t[1] == "Beta Fish" && v.sel[1] == -1 && v.items1 == 5);   // the first row listed, nothing selected
  CHECK(v.t[2] == "Hotel Donut" && v.sel[2] == -1);
  CHECK(v.t[3] == "Alpha Toasters" && v.sel[3] >= 0);
  CHECK(v.t[4] == "Beta Fish" && v.sel[4] == -1);
  CHECK(v.picked && v.t[5] == "Foxtrot Twist" && v.sel[5] == 1);
  CHECK(v.t[6] == "Foxtrot Twist" && v.sel[6] == 1);
  Settings s;
  CHECK(load_settings(w.settings.wstring(), s));
  CHECK(s.module == "tt.foxtrot");   // chosen by the click
  CHECK((s.collections == Ids{"tt", "simpsons"}));

  // ---- session 2: a filter alone never changes Single's module.
  edit_settings(w, [](Settings& s) {
    s.module = "ad40.alpha";
    s.collections.clear();
  });
  step = 0;
  std::string t_filtered;
  int sel_filtered = -2;
  r = run_scr(o, L"/c", env, 60000, [&](DWORD pid) {
    if (step) return;
    HWND d = ready(pid, 5);
    if (!d) return;
    click_tile(d, 4);
    t_filtered = title_of(d), sel_filtered = selected_of(d);
    PostMessageW(d, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), (LPARAM)GetDlgItem(d, IDOK));
    step = 1;
  });
  CHECK(step == 1);
  if (!expect_exit(w, r, 0)) return 1;
  CHECK(t_filtered == "Hotel Donut" && sel_filtered == -1);
  CHECK(load_settings(w.settings.wstring(), s) && s.module == "ad40.alpha" && (s.collections == Ids{"simpsons"}));

  // ---- session 3: Random follows the filter the same way.
  edit_settings(w, [](Settings& s) {
    s.module = "random";
    s.randomize.clear();
    s.collections.clear();
  });
  step = 0;
  std::string r0, r1, r2;
  int rs1 = -2;
  r = run_scr(o, L"/c", env, 60000, [&](DWORD pid) {
    if (step) return;
    HWND d = ready(pid, 5);
    if (!d) return;
    r0 = title_of(d);
    click_tile(d, 4);   // The Simpsons
    r1 = title_of(d), rs1 = selected_of(d);
    click_tile(d, 3);   // ...and Totally Twisted: what shows is still listed
    r2 = title_of(d);
    PostMessageW(d, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), (LPARAM)GetDlgItem(d, IDOK));
    step = 1;
  });
  CHECK(step == 1);
  if (!expect_exit(w, r, 0)) return 1;
  printf("random: %s | %s | %s\n", r0.c_str(), r1.c_str(), r2.c_str());
  CHECK(r0 == "Alpha Toasters");
  CHECK(r1 == "Hotel Donut" && rs1 == -1);
  CHECK(r2 == "Hotel Donut");
  CHECK(load_settings(w.settings.wstring(), s) && s.module == "random" && s.randomize.empty());

  // ---- session 4: a release with no modules listed: nothing to show, said so.
  {
    fs::path cat = w.assets / "win" / "catalog-win.json";
    std::string text;
    read_file(cat.wstring(), text);
    phosg::JSON root = phosg::JSON::parse(text);
    phosg::JSON empty = phosg::JSON::dict();
    empty.emplace("id", "empty");
    empty.emplace("title", "Empty Release");
    root.at("packages").emplace_back(std::move(empty));
    CHECK(write_file_atomic(cat.wstring(), root.serialize()));
  }
  edit_settings(w, [](Settings& s) {
    s.module = "ad40.alpha";
    s.collections.clear();
  });
  step = 0;
  std::string e_title, e_about, e_back;
  int e_items = -1;
  bool e_preview = true;
  r = run_scr(o, L"/c", env, 60000, [&](DWORD pid) {
    if (step) return;
    HWND d = ready(pid, 6);
    if (!d) return;
    click_tile(d, 5);   // only the release with nothing in it
    e_items = item_count(GetDlgItem(d, IDC_MODULE_LIST));
    e_title = title_of(d);
    e_about = long_text(GetDlgItem(d, IDC_ABOUT));
    e_preview = IsWindowEnabled(GetDlgItem(d, IDC_PREVIEW)) != FALSE;
    click(d, IDC_STRIP_SHOW_ALL);
    e_back = title_of(d);
    PostMessageW(d, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
    step = 1;
  });
  CHECK(step == 1);
  if (!expect_exit(w, r, 0)) return 1;
  printf("empty: items %d, \"%s\", preview %d, then \"%s\"\n", e_items, e_title.c_str(), (int)e_preview, e_back.c_str());
  CHECK(e_items == 0 && e_title == "No modules to show" && !e_preview);
  CHECK(e_about.find("have no modules") != std::string::npos);
  CHECK(e_back == "Alpha Toasters");
  check_hosts_gone(w);
  if (g_failures) dump_logs(w);
  return 0;
}

// ---- the data folder (paths.h, host/core's data_root.h) -------------------------------

// With nothing but the defaults (no AD_SETTINGS or AD_ASSETS_DIR) and a
// scratch AD_LOCALAPPDATA: /s runs from <base>\LongAfterDark (its hosts get
// assets and state under it, and its last-exit log is there), and the
// settings dialog finds everything there and saves back to it.
int test_data_root(const Opts& o) {
  Work w = prepare(o, "data-root");
  const fs::path base = w.dir / "lad", data = base / "LongAfterDark";
  std::error_code ec;
  fs::create_directories(data);
  fs::copy(w.assets, data / "assets", fs::copy_options::recursive, ec);
  if (ec) failf("cannot copy the fixture assets: %s", ec.message().c_str());
  fs::copy_file(w.settings, data / "settings.ini", ec);
  EnvList env = base_env(o, w);
  env.push_back({adw::kDataRootBaseVar, base.wstring()});
  env.push_back({L"AD_ASSETS_DIR", L""});
  env.push_back({L"AD_SETTINGS", L""});

  // ---- /s
  EnvList run_env = env;
  run_env.push_back({L"AD_SCR_TESTEXIT_AFTER_FRAMES", L"10"});
  RunResult r = run_scr(o, L"/s", run_env, 60000);
  if (!expect_exit(w, r, 0)) return 1;
  CHECK(count_in_log(data / "logs" / "saver-last.log", "start /s ") == 1);
  auto starts = host_events(w, "start");
  CHECK(!starts.empty());
  for (auto& s : starts) {
    CHECK(ends_with(s["module"], "TESTRING.AD"));   // the data folder's settings.ini's module
    CHECK(s["AD_ASSETS_DIR"] == (data / "assets").string());
    CHECK(s["ADSTATE"] == (data / "state").string());
  }
  check_hosts_gone(w);

  // ---- /c: everything is found there, and OK saves back to it.
  int items = -1, step = 0;
  std::string title;
  r = run_scr(o, L"/c", env, 60000, [&](DWORD pid) {
    if (step) return;
    HWND d = find_dialog(pid);
    HWND list = d ? GetDlgItem(d, IDC_MODULE_LIST) : nullptr;
    if (!list || item_count(list) == 0) return;
    items = item_count(list);
    title = window_text(d);
    PostMessageW(d, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), (LPARAM)GetDlgItem(d, IDOK));
    step = 1;
  });
  CHECK(step == 1);
  if (!expect_exit(w, r, 0)) return 1;
  CHECK(items == 5 && title == "Long After Dark");
  Settings s;
  CHECK(load_settings((data / "settings.ini").wstring(), s) && s.module == "test.rings");
  // Nothing but the data folder is made in the base.
  for (const auto& e : fs::directory_iterator(base, ec)) {
    if (e.path().filename() != "LongAfterDark") failf("made in the base: %s", e.path().string().c_str());
  }
  if (g_failures) dump_logs(w);
  return 0;
}

// What Windows reads from the file itself, no window needed: Screen Saver
// Settings lists string 1 (IDS_DESCRIPTION; without it, the file name),
// Explorer shows the version resource, and the manifest names the app.
// Whether the file holds `name` as the program would spell it: UTF-16 (an
// L"..." literal) or plain bytes.
bool binary_mentions(const std::string& bytes, const std::string& name) {
  if (bytes.find(name) != std::string::npos) return true;
  std::string wide;
  for (char c : name) {
    wide += c;
    wide += '\0';
  }
  return bytes.find(wide) != std::string::npos;
}

int test_resources(const Opts& o) {
  // The screen saver that ships is built without the test levers
  // (src/test_hooks.h; QA test-hooks-in-release): none of their names is in
  // it, while the test build the other smoke tests run has them all (so the
  // scan itself is known to work).
  if (o.shipped.empty()) {
    failf("--shipped <LongAfterDark.scr> missing");
    return 1;
  }
  std::string shipped_bytes, test_bytes;
  CHECK(read_file(o.shipped, shipped_bytes) && read_file(o.scr, test_bytes));
  for (const char* hook : {"AD_SCR_TEST_IGNORE_INPUT", "AD_SCR_TEST_INPUT", "AD_SCR_TEST_MONITORS", "AD_SCR_TEST_STALL_MS",
                           "AD_SCR_TEST_FIRSTFRAME_MS", "AD_SCR_TEST_DISPLAY_OFF_MS", "AD_SCR_TEST_DISPLAY_ON",
                           "AD_SCR_TEST_ROTATE_MS", "AD_SCR_TESTEXIT_AFTER_FRAMES", "AD_SCR_TEST_SCREENSHOT",
                           "AD_SCR_TEST_SCREENSHOT_STATE", "AD_SCR_TEST_CAPTURE", "AD_UI_TEST_HC_SCHEME",
                           "AD_SCR_TEST_OPEN_LOG", "AD_SCR_TEST_SEED"}) {
    if (binary_mentions(shipped_bytes, hook)) failf("the shipped LongAfterDark.scr reads %s", hook);
    if (!binary_mentions(test_bytes, hook)) failf("LongAfterDark-test.scr doesn't read %s", hook);
  }
  // The diagnostic overrides are in both.
  for (const char* knob : {"AD_SCR_LOG", "AD_SCR_STRETCH", "AD_SCR_PRESENT", "AD_SETTINGS"}) {
    CHECK(binary_mentions(shipped_bytes, knob));
  }
  CHECK(fs::path(o.shipped).filename().wstring() == L"LongAfterDark.scr");
  CHECK(fs::path(o.scr).filename().wstring() == L"LongAfterDark-test.scr");
  for (const std::wstring& file : {o.shipped, o.scr}) {
  HMODULE h = LoadLibraryExW(file.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
  CHECK(h != nullptr);
  if (!h) return 1;
  wchar_t buf[128] = {};
  const int n = LoadStringW(h, IDS_DESCRIPTION, buf, 128);
  CHECK(n > 0 && std::wstring(buf, n) == L"Long After Dark");
  if (HRSRC m = FindResourceW(h, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(24) /* RT_MANIFEST */)) {
    const char* p = static_cast<const char*>(LockResource(LoadResource(h, m)));
    const std::string manifest(p ? p : "", p ? SizeofResource(h, m) : 0);
    CHECK(manifest.find("name=\"LongAfterDark.Saver\"") != std::string::npos);
    CHECK(manifest.find("<description>Long After Dark screen saver</description>") != std::string::npos);
  } else {
    failf("no manifest resource");
  }
  FreeLibrary(h);
  DWORD handle = 0;
  const DWORD size = GetFileVersionInfoSizeW(file.c_str(), &handle);
  std::vector<uint8_t> vi(size);
  CHECK(size > 0 && GetFileVersionInfoW(file.c_str(), 0, size, vi.data()));
  auto value = [&](const wchar_t* key) {
    wchar_t* v = nullptr;
    UINT len = 0;
    std::wstring path = std::wstring(L"\\StringFileInfo\\040904B0\\") + key;
    return size && VerQueryValueW(vi.data(), path.c_str(), reinterpret_cast<void**>(&v), &len) && v ? std::wstring(v)
                                                                                                    : std::wstring();
  };
  CHECK(value(L"ProductName") == L"Long After Dark");
  CHECK(value(L"FileDescription") == L"Long After Dark screen saver");
  CHECK(value(L"OriginalFilename") == L"LongAfterDark.scr");
  CHECK(value(L"InternalName") == L"LongAfterDark");
  // From adw_version.h, as adhostwin's and adimport's: a.b.c.0 and LICENSE's line.
  CHECK(value(L"FileVersion").size() >= 7 && value(L"FileVersion") == value(L"ProductVersion"));
  CHECK(value(L"LegalCopyright").rfind(L"Copyright", 0) == 0);
  }
  return 0;
}

std::vector<DWORD> spawn_pids(const Work& w, int window);   // below

// Sends WM_DISPLAYCHANGE to every saver window of `pid`, as Windows does to
// every top-level window when the monitors change.
void post_display_change(DWORD pid, int times) {
  struct Ctx { DWORD pid; int times; } c{pid, times};
  EnumWindows([](HWND h, LPARAM lp) -> BOOL {
    auto* c = reinterpret_cast<Ctx*>(lp);
    DWORD p = 0;
    GetWindowThreadProcessId(h, &p);
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    if (p == c->pid && wcscmp(cls, L"LongAfterDarkSaver") == 0) {
      for (int i = 0; i < c->times; ++i) PostMessageW(h, WM_DISPLAYCHANGE, 32, MAKELPARAM(800, 600));
    }
    return TRUE;
  }, reinterpret_cast<LPARAM>(&c));
}

// Monitors change under a running /s (AD_SCR_TEST_MONITORS stages it): a
// 4:3 primary and a 16:9 secondary become an 800x600 primary (same aspect:
// the window moves, its host carries on) and a portrait monitor in place of
// the 16:9 one (new window and host; the old ones go at once). A burst of
// WM_DISPLAYCHANGE is one relayout; one that changes nothing does nothing.
int test_display_change(const Opts& o) {
  Work w = prepare(o, "display-change");
  edit_settings(w, [](Settings& s) {
    s.module = "test.rings";
    s.randomize.clear();
    s.all_monitors = true;
  });
  EnvList env = base_env(o, w);
  env.push_back({L"AD_SCR_TEST_MONITORS", L"0,0,640,480,p;640,0,854,480|0,0,800,600,p;800,0,480,640"});
  env.push_back({L"AD_SCR_TESTEXIT_AFTER_FRAMES", L"90"});
  int step = 0;
  RunResult r = run_scr(o, L"/s", env, 60000, [&](DWORD pid) {
    if (step == 0 && host_events(w, "start").size() >= 2) {
      post_display_change(pid, 3);
      step = 1;
    } else if (step == 1 && count_in_log(w.scr_log, "relayout monitors") >= 1) {
      post_display_change(pid, 1);   // the same monitors again
      step = 2;
    }
  });
  if (!expect_exit(w, r, 0)) return 1;
  CHECK(step == 2);
  auto starts = host_events(w, "start");
  CHECK(starts.size() == 3);
  // Each window's host by the saver's own spawn lines: the two first hosts
  // start together, and either may write its start line first.
  std::map<std::string, std::map<std::string, std::string>> by_pid;
  for (auto& e : starts) by_pid[e["pid"]] = e;
  const std::vector<DWORD> w0 = spawn_pids(w, 0), w1 = spawn_pids(w, 1), w2 = spawn_pids(w, 2);
  CHECK(w0.size() == 1 && w1.size() == 1 && w2.size() == 1);
  if (w0.size() == 1 && w1.size() == 1 && w2.size() == 1) {
    auto& s0 = by_pid[std::to_string(w0[0])];
    auto& s1 = by_pid[std::to_string(w1[0])];
    auto& s2 = by_pid[std::to_string(w2[0])];
    CHECK(s0["ADSCREENW"] == "640" && s1["ADSCREENW"] == "856");
    CHECK(s2["ADSCREENW"] == "640" && s2["ADSCREENH"] == "480");   // portrait: never narrower than 4:3
  }
  CHECK(count_in_log(w.scr_log, "relayout monitors=2->2 kept=0 moved=1 created=1 retired=1") == 1);
  CHECK(count_in_log(w.scr_log, "relayout monitors=2->2 kept=2 moved=0 created=0 retired=0") == 1);
  CHECK(count_in_log(w.scr_log, "relayout monitors") == 2);
  // The unplugged monitor's host stopped at the relayout, well before the
  // kept one (which ran on until the new window had its frames too).
  std::map<std::string, long long> frames;
  for (auto& e : host_events(w, "exit")) frames[e["pid"]] = atoll(e["frames"].c_str());
  if (w0.size() == 1 && w1.size() == 1) {
    const std::string kept = std::to_string(w0[0]), retired = std::to_string(w1[0]);
    CHECK(frames.count(kept) && frames.count(retired));
    CHECK(frames[retired] < frames[kept]);
  }
  CHECK(count_in_log(w.scr_log, "test-exit after 90 frames") == 1);
  check_hosts_gone(w);
  if (g_failures) dump_logs(w);
  return 0;
}

// ---- presentation (present.h) -------------------------------------------------------

int find_line(const std::vector<std::string>& lines, const std::string& needle, int from);   // below

// How much of `r` on the desktop (the composed screen, read back as a
// screenshot tool reads it) is not black, as a fraction of a sampled grid.
double screen_lit_fraction(const RECT& r) {
  const int w = r.right - r.left, h = r.bottom - r.top;
  if (w <= 0 || h <= 0) return -1;
  HDC screen = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(screen);
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  double lit = -1;
  if (dib && mem && bits) {
    HGDIOBJ old = SelectObject(mem, dib);
    if (BitBlt(mem, 0, 0, w, h, screen, r.left, r.top, SRCCOPY | CAPTUREBLT)) {
      GdiFlush();
      const uint8_t* px = static_cast<const uint8_t*>(bits);
      int n = 0, on = 0;
      for (int y = h / 8; y < h; y += h / 8) {
        for (int x = w / 16; x < w; x += w / 16) {
          const uint8_t* q = px + ((size_t)y * w + x) * 4;
          ++n;
          if (q[0] + q[1] + q[2] > 48) ++on;
        }
      }
      lit = n ? (double)on / n : -1;
    }
    SelectObject(mem, old);
  }
  if (dib) DeleteObject(dib);
  if (mem) DeleteDC(mem);
  ReleaseDC(nullptr, screen);
  return lit;
}

// A /s window shows the host's frames scaled into its monitor through
// Direct2D (QA present-4k-scaling), or through GDI when asked
// (AD_SCR_PRESENT=gdi): on the screen itself (the desktop read back while it
// runs), and in the test hook's captures (AD_SCR_TEST_CAPTURE, QA
// scr-e2e-coverage): what the window shows and the host's frame, whose
// every pixel is a crisp block of the picture (sampled at the blocks'
// centres), with black bars around.
int test_present(const Opts& o) {
  for (const char* way : {"d2d", "gdi"}) {
    const bool d2d = std::string(way) == "d2d";
    Work w = prepare(o, std::string("present-") + way);
    edit_settings(w, [](Settings& s) {
      s.module = "test.rings";
      s.randomize.clear();
      s.all_monitors = true;
    });
    const fs::path caps = w.dir / "captures";
    fs::create_directories(caps);
    EnvList env = base_env(o, w);
    // A 1920x1080 "monitor" on any machine: 856x480 scaled 2.24 times.
    env.push_back({L"AD_SCR_TEST_MONITORS", L"0,0,1920,1080,p"});
    env.push_back({L"AD_SCR_TEST_CAPTURE", caps.wstring()});
    env.push_back({L"AD_SCR_TEST_CAPTURE_FRAMES", L"20,40"});
    env.push_back({L"AD_SCR_TESTEXIT_AFTER_FRAMES", L"150"});
    // Each way asked for by name, so the auto policy can't switch it (or
    // GDI's HALFTONE to nearest) on a busy machine; `run` checks that auto
    // picks Direct2D.
    env.push_back({L"AD_SCR_PRESENT", d2d ? L"d2d" : L"gdi"});
    env.push_back({L"AD_SCR_STRETCH", d2d ? L"" : L"halftone"});
    double lit = -2;
    RunResult r = run_scr(o, L"/s", env, 60000, [&](DWORD) {
      if (lit > -2 || count_in_log(w.scr_log, "capture window=0 frame=40") == 0) return;
      lit = screen_lit_fraction(RECT{200, 150, 1720, 930});
    });
    if (!expect_exit(w, r, 0)) return 1;
    const std::vector<std::string> lines = lines_of(w.scr_log);
    CHECK((find_line(lines, "present window=0: direct2d", 0) >= 0) == d2d);
    CHECK(find_line(lines, "direct2d failed", 0) < 0);
    CHECK(count_in_log(w.scr_log, d2d ? "present=d2d" : "present=gdi") >= 1);
    // On screen: the rings, not a black (or stale desktop) window.
    if (lit > -2) {
      if (lit < 0.5) failf("%s: only %.0f%% of the window's middle is lit on the screen", way, lit * 100);
    } else {
      fprintf(stderr, "%s: the screen was not read back (no capture line seen in time)\n", way);
    }
    for (int k : {20, 40}) {
      CHECK(count_in_log(w.scr_log, "capture window=0 frame=" + std::to_string(k) + " ok 1920x1080 host=856x480 present=" +
                                        (d2d ? "d2d" : "gdi") + " filter=smooth") == 1);
      const fs::path shown = caps / ("window0-frame" + std::to_string(k) + ".png");
      const fs::path host = caps / ("window0-frame" + std::to_string(k) + "-host.png");
      int sw = 0, sh = 0, hw = 0, hh = 0;
      std::vector<uint8_t> sp, hp;
      CHECK(load_png(shown, &sw, &sh, &sp) && sw == 1920 && sh == 1080);
      CHECK(load_png(host, &hw, &hh, &hp) && hw == 856 && hh == 480);
      if (sw != 1920 || sh != 1080 || hw != 856 || hh != 480) continue;
      const RectI fit = fit_rect(856, 480, 1920, 1080);
      auto at = [](const std::vector<uint8_t>& px, int width, int x, int y) { return &px[((size_t)y * width + x) * 4]; };
      // The bars (the frame is a hair wider than 16:9) are black.
      for (int y = 0; y < fit.y; ++y) CHECK(at(sp, sw, 960, y)[0] == 0 && at(sp, sw, 960, y)[1] == 0);
      // Every host pixel sampled is its block's colour at the block's centre.
      int n = 0, same = 0, lit_host = 0;
      for (int hy = 8; hy < hh; hy += 37) {
        for (int hx = 8; hx < hw; hx += 41) {
          const int x = fit.x + (int)((hx + 0.5) * fit.w / hw), y = fit.y + (int)((hy + 0.5) * fit.h / hh);
          const uint8_t* a = at(hp, hw, hx, hy);
          const uint8_t* b = at(sp, sw, x, y);
          ++n;
          if (a[0] + a[1] + a[2] > 48) ++lit_host;
          if (std::abs(a[0] - b[0]) <= 8 && std::abs(a[1] - b[1]) <= 8 && std::abs(a[2] - b[2]) <= 8) ++same;
        }
      }
      if (same * 10 < n * 9) failf("%s frame %d: %d of %d sampled pixels match the host's frame", way, k, same, n);
      CHECK(lit_host * 2 > n);   // fakehost's rings, not a black frame
    }
    check_hosts_gone(w);
    if (g_failures) {
      dump_logs(w);
      return 0;
    }
  }
  return 0;
}

// ---- interaction (INTERACTION.md §4, §6.3, §7.1, §8, §9.1) ----------------------------

fs::path write_script(const Work& w, const char* name, const std::string& text) {
  fs::path p = w.dir / name;
  write_file_atomic(p.wstring(), text);
  return p;
}

// Index of the first log line at or after `from` containing `needle`, or -1.
int find_line(const std::vector<std::string>& lines, const std::string& needle, int from = 0) {
  for (int i = std::max(0, from); i < (int)lines.size(); ++i)
    if (lines[i].find(needle) != std::string::npos) return i;
  return -1;
}

// The pid a window's host got at its n-th spawn ("spawn window=<w> ... pid=<p>").
std::vector<DWORD> spawn_pids(const Work& w, int window) {
  std::vector<DWORD> out;
  const std::string key = "spawn window=" + std::to_string(window) + " ";
  for (const auto& l : lines_of(w.scr_log)) {
    size_t at = l.find(key);
    if (at == std::string::npos) continue;
    size_t p = l.find(" pid=", at);
    if (p != std::string::npos) out.push_back((DWORD)strtoul(l.c_str() + p + 5, nullptr, 10));
  }
  return out;
}

// C8: every host this test saw carried ADSTATE = <settings dir>\state.
void check_state_everywhere(const Work& w) {
  const std::string want = (w.dir / "state").string();
  int n = 0;
  for (const char* kind : {"start", "capabilities", "configure"}) {
    for (auto& e : host_events(w, kind)) {
      ++n;
      if (e["ADSTATE"] != want) failf("%s pid %s: ADSTATE=\"%s\" (want \"%s\")", kind, e["pid"].c_str(), e["ADSTATE"].c_str(), want.c_str());
    }
  }
  CHECK(n > 0);
}

// The cursor clip, as GetClipCursor reports it: false when it is the whole
// virtual screen (not clipped).
bool clipped(RECT* out = nullptr) {
  RECT r{}, virt{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0};
  virt.right = virt.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
  virt.bottom = virt.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
  GetClipCursor(&r);
  if (out) *out = r;
  return !EqualRect(&r, &virt);
}

EnvList input_env(const Opts& o, const Work& w, const fs::path& script) {
  EnvList env = base_env(o, w);
  env.push_back({L"AD_SCR_TEST_IGNORE_INPUT", L""});   // these tests are about input ending the run
  env.push_back({L"AD_SCR_TEST_INPUT", script.wstring()});
  return env;
}

// Caps Lock starts a game (fakehost: CAPS 1 = interactive); while it runs,
// keys, clicks, the wheel and moves are the module's; the cursor shows (the
// module asks) and is confined to the frame; Caps Lock again ends it, the
// threshold starts over where the cursor is, and the next key ends the saver.
int test_input_play(const Opts& o) {
  Work w = prepare(o, "input-play");
  edit_settings(w, [](Settings& s) {
    s.module = "test.rings";
    s.randomize.clear();
    s.all_monitors = false;
  });
  fs::path script = write_script(w, "input.txt",
                                 "FRAMES 10\n"
                                 "LOG step shift\n"
                                 "KEY 16 1\nKEY 16 0\nKEY 17 1\nKEY 17 0\nKEY 144 1\nKEY 144 0\n"
                                 "FRAMES 5\n"
                                 "LOG step caps-on\n"
                                 "KEY 20 1\nKEY 20 0\n"
                                 "FRAMES 10\n"
                                 "CLIPLOG\n"
                                 "LOG step arrows\n"
                                 "KEY 37 1\nKEY 37 0\nKEY 65 1\nKEY 65 0\nBUTTON 1 1\nBUTTON 1 0\nWHEEL\nMOVE 300 200\n"
                                 "FRAMES 10\n"
                                 "LOG step caps-off\n"
                                 "KEY 20 1\nKEY 20 0\n"
                                 "FRAMES 10\n"
                                 "CLIPLOG\n"
                                 "MOVE 4 3\n"
                                 "FRAMES 5\n"
                                 "LOG step exit-key\n"
                                 "KEY 66 1\n"
                                 "WAIT 5000\n"
                                 "LOG not-exited\n");
  EnvList env = input_env(o, w, script);
  env.push_back({L"FAKEHOST_CURSOR", L"1"});
  bool saw_clip = false;
  RECT clip_seen{};
  RunResult r = run_scr(o, L"/s", env, 60000, [&](DWORD) {
    RECT c;
    if (!saw_clip && clipped(&c)) {
      saw_clip = true;
      clip_seen = c;
    }
  });
  if (!expect_exit(w, r, 0)) return 1;
  auto lines = lines_of(w.scr_log);
  const int caps_on = find_line(lines, "test: step caps-on"), arrows = find_line(lines, "test: step arrows");
  const int caps_off = find_line(lines, "test: step caps-off"), exit_key = find_line(lines, "test: step exit-key");
  CHECK(caps_on > 0 && arrows > caps_on && caps_off > arrows && exit_key > caps_off);
  // No exit before the last key; that one ends it.
  const int exit_line = find_line(lines, "input: key vk=0x42");
  CHECK(exit_line > exit_key);
  CHECK(find_line(lines, "exit code=0 reason=key vk=0x42") > exit_key);
  CHECK(find_line(lines, "not-exited") < 0);
  for (int i = 0; i < exit_key && i < (int)lines.size(); ++i) {
    if (lines[i].find("] input: ") != std::string::npos && lines[i].find("input: caps") == std::string::npos &&
        lines[i].find("input: numlock") == std::string::npos && lines[i].find("input: hold") == std::string::npos &&
        lines[i].find("input: kept") == std::string::npos)
      failf("an input exit before the last key: %s", lines[i].c_str());
  }
  // Play started after Caps Lock and ended after Caps Lock again.
  const int starts = find_line(lines, "play starts window=0"), ends = find_line(lines, "play ends window=0");
  CHECK(starts > caps_on && starts < arrows);
  CHECK(ends > caps_off && ends < exit_key);
  // The cursor showed (the module asked) and hid again; the clip was set
  // while playing and released after (the saver's own view and ours).
  CHECK(find_line(lines, "cursor shown") > caps_on && find_line(lines, "cursor hidden") > caps_off);
  const int clip1 = find_line(lines, "test: clip="), clip2 = find_line(lines, "test: clip=", clip1 + 1);
  CHECK(clip1 > 0 && lines[clip1].find("clip=none") == std::string::npos);
  CHECK(clip2 > clip1 && lines[clip2].find("clip=none") != std::string::npos);
  CHECK(saw_clip);
  CHECK(!clipped());   // released after exit
  // The host got every input line, numbered, and the module had the keys.
  auto inputs = host_events(w, "input");
  int arrow_eaten = 0;
  for (size_t i = 0; i < inputs.size(); ++i) {
    CHECK(inputs[i]["seq"] == std::to_string(i + 1));
    if (inputs[i]["line"] == "KEY 37 1" && inputs[i]["interactive"] == "1") ++arrow_eaten;
  }
  CHECK(arrow_eaten == 1);
  int caps_lines = 0, numlock_lines = 0;
  for (auto& e : inputs) caps_lines += e["line"].rfind("CAPS ", 0) == 0;
  CHECK(caps_lines == 2);   // on, off: once each
  // Num Lock turned on (fakehost keeps the toggle, numlock=1): one NUMLOCK
  // line, numbered with the rest, after its key (right after it once the
  // host has answered --capabilities; test_numlock pins the order).
  bool numlock_key = false;
  for (size_t i = 0; i < inputs.size(); ++i) {
    numlock_key |= inputs[i]["line"] == "KEY 144 1";
    if (inputs[i]["line"].rfind("NUMLOCK ", 0) != 0) continue;
    ++numlock_lines;
    CHECK(inputs[i]["line"] == "NUMLOCK 1" && numlock_key);
  }
  CHECK(numlock_lines == 1);
  // ADCAPS and ADNUMLOCK at spawn (the synthetic toggles start off), and the state folder.
  for (auto& e : host_events(w, "start")) CHECK(e["ADCAPS"] == "0" && e["ADNUMLOCK"] == "0");
  check_state_everywhere(w);
  // The last-exit log, next to settings.ini (§9.1).
  std::string last;
  CHECK(read_file((w.dir / "logs" / "saver-last.log").wstring(), last));
  CHECK(last.find("start /s") != std::string::npos && last.find("input: key vk=0x42") != std::string::npos);
  CHECK(last.find("play starts") != std::string::npos && last.find("spawn window=0") != std::string::npos);
  check_hosts_gone(w);
  if (g_failures) dump_logs(w);
  return 0;
}

// Alt (WM_SYSKEYDOWN) ends the saver at once, even mid-game.
int test_input_alt(const Opts& o) {
  Work w = prepare(o, "input-alt");
  edit_settings(w, [](Settings& s) {
    s.module = "test.rings";
    s.randomize.clear();
    s.all_monitors = false;
  });
  fs::path script = write_script(w, "input.txt",
                                 "FRAMES 10\nKEY 20 1\nKEY 20 0\nFRAMES 10\nKEY 65 1\nKEY 65 0\nFRAMES 5\n"
                                 "LOG step alt\nSYSKEY 18 1\nWAIT 5000\nLOG not-exited\n");
  RunResult r = run_scr(o, L"/s", input_env(o, w, script), 60000);
  if (!expect_exit(w, r, 0)) return 1;
  auto lines = lines_of(w.scr_log);
  const int alt = find_line(lines, "test: step alt");
  CHECK(find_line(lines, "play starts window=0") >= 0 && find_line(lines, "play starts window=0") < alt);
  CHECK(find_line(lines, "input: syskey vk=0x12") > alt);
  CHECK(find_line(lines, "input: key vk=0x41") < 0);   // the module's while playing
  CHECK(find_line(lines, "not-exited") < 0);
  CHECK(!clipped());
  check_hosts_gone(w);
  if (g_failures) dump_logs(w);
  return 0;
}

// The host asks the saver window to close (ADWS_WAKE): that ends it.
int test_input_wake(const Opts& o) {
  Work w = prepare(o, "input-wake");
  edit_settings(w, [](Settings& s) {
    s.module = "test.rings";
    s.randomize.clear();
    s.all_monitors = false;
  });
  fs::path script = write_script(w, "input.txt", "WAIT 20000\nLOG not-exited\n");
  EnvList env = input_env(o, w, script);
  env.push_back({L"FAKEHOST_WAKE_AFTER", L"15"});
  RunResult r = run_scr(o, L"/s", env, 60000);
  if (!expect_exit(w, r, 0)) return 1;
  CHECK(count_in_log(w.scr_log, "input: wake") == 1);
  CHECK(count_in_log(w.scr_log, "not-exited") == 0);
  check_hosts_gone(w);
  if (g_failures) dump_logs(w);
  return 0;
}

// Rotation waits while the owner's module plays, and resumes after.
int test_input_rotate(const Opts& o) {
  Work w = prepare(o, "input-rotate");
  edit_settings(w, [](Settings& s) {
    s.module = "random";
    s.randomize.clear();
    s.all_monitors = false;
  });
  fs::path script = write_script(w, "input.txt",
                                 "FRAMES 5\nKEY 20 1\nKEY 20 0\nFRAMES 5\n"
                                 "LOG step playing\nWAIT 3000\n"
                                 "LOG step stop-playing\nKEY 20 1\nKEY 20 0\nWAIT 3000\n"
                                 "LOG step done\nSYSKEY 18 1\n");
  EnvList env = input_env(o, w, script);
  env.push_back({L"AD_SCR_TEST_ROTATE_MS", L"700"});
  RunResult r = run_scr(o, L"/s", env, 60000);
  if (!expect_exit(w, r, 0)) return 1;
  auto lines = lines_of(w.scr_log);
  const int playing = find_line(lines, "test: step playing"), stop = find_line(lines, "test: step stop-playing");
  const int done = find_line(lines, "test: step done"), started = find_line(lines, "play starts window=0");
  CHECK(started >= 0 && started < playing && playing < stop && stop < done);
  // Between the game starting and Caps Lock again: waits, no switch.
  CHECK(find_line(lines, "rotate-wait window=0", started) > started);
  const int switched = find_line(lines, "rotate window=0 ->", started);
  CHECK(switched > stop && switched < done);
  // Rotated hosts start black: no desktop seed after the first host.
  auto starts = host_events(w, "start");
  CHECK(starts.size() >= 2);
  for (size_t i = 1; i < starts.size(); ++i) CHECK(starts[i]["ADSEEDIMG"].empty());
  check_state_everywhere(w);
  check_hosts_gone(w);
  if (g_failures) dump_logs(w);
  return 0;
}

// Num Lock (INTERACTION.md §3.2, PLAN §2.4): the owner's host hears it only
// when its --capabilities says numlock=1 (fakehost says so, as today's host):
//  * every host starts with ADNUMLOCK, the (synthetic) toggle then;
//  * the key that flips it (KEY 144) sends NUMLOCK 1 right after that key's
//    KEY line, a change without a key (NUMLOCKSTATE) NUMLOCK 0 within
//    250 ms, each numbered as the host numbers its input lines;
//  * a host started while it is on gets ADNUMLOCK=1, and no line for it;
//  * Num Lock never ends the saver (Alt does, at the end).
// A host from before the toggle (FAKEHOST_NUMLOCK=0) hears no NUMLOCK line,
// which it would not number (every later line's number would be one off);
// hosts started after its answer get no ADNUMLOCK (the first, started
// before it, gets one, which such a host ignores).
int test_numlock(const Opts& o) {
  Work w = prepare(o, "numlock");
  auto run = [&](const EnvList& extra, bool rotate, const std::string& script_text) {
    fs::remove(w.scr_log);
    fs::remove(w.host_log);
    edit_settings(w, [&](Settings& s) {
      s.module = rotate ? "random" : "test.rings";
      s.randomize.clear();
      if (rotate) s.randomize = {"test.rings", "test.plain"};
      s.all_monitors = false;
    });
    EnvList env = input_env(o, w, write_script(w, "input.txt", script_text));
    env.insert(env.end(), extra.begin(), extra.end());
    if (rotate) env.push_back({L"AD_SCR_TEST_ROTATE_MS", L"700"});
    RunResult r = run_scr(o, L"/s", env, 60000);
    if (!expect_exit(w, r, 0)) return false;
    // Num Lock ended nothing: Alt did, at the end.
    auto lines = lines_of(w.scr_log);
    CHECK(find_line(lines, "exit code=0 reason=syskey vk=0x12") > find_line(lines, "test: step exit"));
    CHECK(count_in_log(w.scr_log, "input: key vk=") == 0 && count_in_log(w.scr_log, "not-exited") == 0);
    return true;
  };
  // The input lines each host got, in order ("pid" -> lines), their numbers
  // checked: 1, 2, 3 ... per host, whatever the saver sent.
  auto inputs_by_host = [&] {
    std::map<std::string, std::vector<std::string>> by;
    for (auto& e : host_events(w, "input")) {
      auto& v = by[e["pid"]];
      v.push_back(e["line"]);
      if (e["seq"] != std::to_string(v.size())) failf("pid %s: input line \"%s\" numbered %s, not %zu", e["pid"].c_str(),
                                                       e["line"].c_str(), e["seq"].c_str(), v.size());
    }
    return by;
  };
  using Lines = std::vector<std::string>;
  const std::string on_off = "WAIT 1000\nLOG step numlock-on\nKEY 144 1\nKEY 144 0\nKEY 16 1\nKEY 16 0\nFRAMES 5\n"
                             "LOG step numlock-off\nNUMLOCKSTATE 0\nWAIT 700\nFRAMES 3\n"
                             "LOG step exit\nSYSKEY 18 1\nWAIT 5000\nLOG not-exited\n";
  // A host that keeps the toggle: its lines, numbered with the others.
  {
    if (!run({}, false, on_off)) return 1;
    auto starts = host_events(w, "start");
    CHECK(starts.size() == 1);
    for (auto& e : starts) CHECK(e["ADNUMLOCK"] == "0");
    auto by = inputs_by_host();
    CHECK(by.size() == 1);
    for (auto& [pid, lines] : by) {
      CHECK((lines == Lines{"KEY 144 1", "NUMLOCK 1", "KEY 144 0", "KEY 16 1", "KEY 16 0", "NUMLOCK 0"}));
    }
    CHECK(count_in_log(w.scr_log, "input: numlock 1 -> owner (n=2)") == 1);
    CHECK(count_in_log(w.scr_log, "input: numlock 0 -> owner (n=6)") == 1);
    CHECK(host_events(w, "unknown").empty());
    auto exits = host_events(w, "exit");
    CHECK(exits.size() == 1 && exits[0]["numlock"] == "0" && exits[0]["input_lines"] == "6");
    check_hosts_gone(w);
    if (g_failures) dump_logs(w);
  }
  // Rotating while it is on: a host started then gets ADNUMLOCK=1 and no line.
  const std::string on_rotate = "WAIT 1000\nLOG step numlock-on\nKEY 144 1\nKEY 144 0\nWAIT 2200\n"
                                "LOG step exit\nSYSKEY 18 1\nWAIT 5000\nLOG not-exited\n";
  {
    if (!run({}, true, on_rotate)) return 1;
    const auto lines = lines_of(w.scr_log);
    const int sent = find_line(lines, "input: numlock 1 -> owner");
    CHECK(sent > 0);
    // Each spawn's ADNUMLOCK is the toggle then: 0 before the line, 1 after.
    std::map<std::string, std::string> want;   // pid -> ADNUMLOCK
    for (int i = 0; i < (int)lines.size(); ++i) {
      const size_t at = lines[i].find("spawn window=0 "), p = lines[i].find(" pid=");
      if (at == std::string::npos || p == std::string::npos) continue;
      const std::string pid = std::to_string(strtoul(lines[i].c_str() + p + 5, nullptr, 10));
      want[pid] = i < sent ? "0" : "1";
      CHECK(lines[i].find(i < sent ? " numlock=0 " : " numlock=1 ") != std::string::npos);
    }
    int after = 0;
    for (auto& e : host_events(w, "start")) {
      CHECK(want.count(e["pid"]) && e["ADNUMLOCK"] == want[e["pid"]]);
      after += e["ADNUMLOCK"] == "1";
    }
    CHECK(after >= 1);   // a host was started while it was on
    // The one line went to the host that was the owner's then, started with
    // it off; the ones started with it on heard none.
    int numlock_lines = 0;
    for (auto& [pid, got] : inputs_by_host()) {
      const int n = (int)std::count(got.begin(), got.end(), std::string("NUMLOCK 1"));
      numlock_lines += n;
      if (n) CHECK(want[pid] == "0");
      CHECK(std::none_of(got.begin(), got.end(), [](const std::string& l) { return l == "NUMLOCK 0"; }));
    }
    CHECK(numlock_lines == 1);
    CHECK(host_events(w, "unknown").empty());
    check_hosts_gone(w);
    if (g_failures) dump_logs(w);
  }
  // A host from before the toggle: no NUMLOCK line, ever; ADNUMLOCK only
  // for the host started before its answer.
  for (bool rotate : {false, true}) {
    if (!run({{L"FAKEHOST_NUMLOCK", L"0"}}, rotate, rotate ? on_rotate : on_off)) return 1;
    CHECK(count_in_log(w.scr_log, "input: numlock") == 0);
    CHECK(host_events(w, "unknown").empty());   // nothing it doesn't know was sent
    for (auto& [pid, got] : inputs_by_host()) {
      CHECK(std::none_of(got.begin(), got.end(), [](const std::string& l) { return l.rfind("NUMLOCK", 0) == 0; }));
    }
    const auto lines = lines_of(w.scr_log);
    const int answered = find_line(lines, "host capabilities: lanes=");
    CHECK(answered > 0 && lines[answered].find("numlock") == std::string::npos);
    std::map<std::string, bool> before;   // pid -> started before the answer
    for (int i = 0; i < (int)lines.size(); ++i) {
      const size_t at = lines[i].find("spawn window=0 "), p = lines[i].find(" pid=");
      if (at == std::string::npos || p == std::string::npos) continue;
      before[std::to_string(strtoul(lines[i].c_str() + p + 5, nullptr, 10))] = i < answered;
      CHECK(lines[i].find(i < answered ? " numlock=0 " : " numlock=-1 ") != std::string::npos);
    }
    auto starts = host_events(w, "start");
    CHECK(!starts.empty() && before[starts[0]["pid"]]);   // the first host starts before the answer comes
    int after = 0;
    for (auto& e : starts) {
      CHECK(e["ADNUMLOCK"] == (before[e["pid"]] ? "0" : ""));
      after += !before[e["pid"]];
    }
    if (rotate) CHECK(after >= 1);
    check_hosts_gone(w);
    if (g_failures) dump_logs(w);
  }
  return 0;
}

// A display change moves the cursor (Windows puts it back on a monitor that
// is still there, often hundreds of pixels away): until the relayout has
// re-planned the windows (500 ms after the last WM_DISPLAYCHANGE) a move is
// not the user coming back. Afterwards the threshold counts from where the
// cursor is, so a small nudge still doesn't end it and a real move does
// (QA scr-relayout-mouse-exit, WIP 9a).
int test_input_display_change(const Opts& o) {
  Work w = prepare(o, "input-display-change");
  edit_settings(w, [](Settings& s) {
    s.module = "test.rings";
    s.randomize.clear();
    s.all_monitors = true;
  });
  fs::path script = write_script(w, "input.txt",
                                 "FRAMES 10\n"
                                 "LOG step display-change\n"
                                 "DISPLAYCHANGE\n"
                                 "MOVE 400 300\n"     // Windows moving the cursor off the monitor that went
                                 "WAIT 100\n"
                                 "MOVE -300 200\n"
                                 "WAIT 1500\n"        // the relayout settles
                                 "LOG step settled\n"
                                 "MOVE 3 3\n"         // under the threshold, from the new origin
                                 "WAIT 200\n"
                                 "LOG step nudge\n"
                                 "MOVE 40 0\n"        // the user
                                 "WAIT 5000\n"
                                 "LOG not-exited\n");
  EnvList env = input_env(o, w, script);
  // Two monitors, then the second one gone (on any machine).
  env.push_back({L"AD_SCR_TEST_MONITORS", L"0,0,1280,720,p;1280,0,1024,768|0,0,1280,720,p"});
  RunResult r = run_scr(o, L"/s", env, 60000);
  if (!expect_exit(w, r, 0)) return 1;
  auto lines = lines_of(w.scr_log);
  const int change = find_line(lines, "test: step display-change"), settled = find_line(lines, "test: step settled");
  const int nudge = find_line(lines, "test: step nudge"), relayout = find_line(lines, "relayout monitors=2->1");
  const int exit_line = find_line(lines, "input: move dx=");
  CHECK(change >= 0 && settled > change && nudge > settled);
  CHECK(find_line(lines, "input: move while the display settles", change) > change);
  CHECK(relayout > change && relayout < settled);
  // Only the user's move ended it, after the nudge: the Windows moves didn't.
  CHECK(exit_line > nudge);
  if (exit_line >= 0) CHECK(lines[exit_line].find("input: move dx=43 dy=3") != std::string::npos);
  CHECK(count_in_log(w.scr_log, "input: move dx=") == 1);
  CHECK(count_in_log(w.scr_log, "display change: settling") == 1);
  CHECK(count_in_log(w.scr_log, "not-exited") == 0);
  check_hosts_gone(w);
  if (g_failures) dump_logs(w);
  return 0;
}

// Two (staged) monitors: only the primary window's host gets input lines,
// and each window's first host starts on its monitor's capture.
int test_input_monitors(const Opts& o) {
  Work w = prepare(o, "input-monitors");
  edit_settings(w, [](Settings& s) {
    s.module = "test.rings";
    s.randomize.clear();
    s.all_monitors = true;
  });
  fs::path script = write_script(w, "input.txt",
                                 "FRAMES 10\nMOVE 2 1\nKEY 16 1\nKEY 16 0\nFRAMES 5\nLOG step key\nKEY 65 1\n"
                                 "WAIT 5000\nLOG not-exited\n");
  EnvList env = input_env(o, w, script);
  env.push_back({L"AD_SCR_TEST_MONITORS", L"0,0,1280,720,p;1280,0,1024,768"});
  DWORD scr_pid = 0;
  RunResult r = run_scr(o, L"/s", env, 60000, [&](DWORD pid) { scr_pid = pid; });
  if (!expect_exit(w, r, 0)) return 1;
  CHECK(count_in_log(w.scr_log, "input: key vk=0x41") == 1);
  CHECK(count_in_log(w.scr_log, "not-exited") == 0);
  std::vector<DWORD> w0 = spawn_pids(w, 0), w1 = spawn_pids(w, 1);
  CHECK(w0.size() == 1 && w1.size() == 1);
  int to_owner = 0;
  for (auto& e : host_events(w, "input")) {
    DWORD pid = (DWORD)strtoul(e["pid"].c_str(), nullptr, 10);
    if (!w0.empty() && pid == w0[0]) ++to_owner;
    else failf("host pid %lu (not window 0's) got an input line: %s", pid, e["line"].c_str());
  }
  CHECK(to_owner >= 3);   // the move, Shift down/up, the key
  // Seeds (§8): each first host, its monitor at its emulated size.
  const SizeI e0 = emulated_screen_size(1280.0 / 720, 1.0), e1 = emulated_screen_size(1024.0 / 768, 1.0);
  for (auto& s : host_events(w, "start")) {
    const DWORD pid = (DWORD)strtoul(s["pid"].c_str(), nullptr, 10);
    const bool first0 = !w0.empty() && pid == w0[0], first1 = !w1.empty() && pid == w1[0];
    CHECK(first0 || first1);   // no respawns here: every host is a window's first
    const SizeI e = first0 ? e0 : e1;
    const std::string want = "ok " + std::to_string(e.w) + "x" + std::to_string(e.h);
    CHECK(s["seed_check"] == want);
    const std::string file = "LongAfterDark-seed-" + std::to_string(scr_pid) + "-" + (first0 ? "0" : "1") + ".ppm";
    CHECK(ends_with(s["ADSEEDIMG"], file));
  }
  CHECK(!clipped());
  check_state_everywhere(w);
  check_hosts_gone(w);
  if (g_failures) dump_logs(w);
  return 0;
}

// No desktop captures left in %TEMP% by saver `pid`.
int seed_files_left(DWORD pid) {
  int n = 0;
  WIN32_FIND_DATAW fd;
  std::wstring pattern = join_path(temp_dir(), L"LongAfterDark-seed-" + std::to_wstring(pid) + L"-*");
  HANDLE f = FindFirstFileW(pattern.c_str(), &fd);
  if (f == INVALID_HANDLE_VALUE) return 0;
  do ++n;
  while (FindNextFileW(f, &fd));
  FindClose(f);
  return n;
}

// §8: the first host of each window gets the desktop; respawned ones start
// black; nothing is left in %TEMP%; StartFromDesktop=0 turns it off.
int test_seed(const Opts& o) {
  Work w = prepare(o, "seed");
  edit_settings(w, [](Settings& s) {
    s.module = "test.rings";
    s.randomize.clear();
    s.all_monitors = true;
  });
  EnvList env = base_env(o, w);
  env.push_back({L"AD_SCR_TEST_MONITORS", L"0,0,1280,720,p;1280,0,1024,768"});
  env.push_back({L"FAKEHOST_EXIT_AFTER", L"6"});
  env.push_back({L"AD_SCR_TESTEXIT_AFTER_FRAMES", L"15"});
  DWORD scr_pid = 0;
  int during_files = 0;
  RunResult r = run_scr(o, L"/s", env, 60000, [&](DWORD pid) {
    scr_pid = pid;
    during_files = std::max(during_files, seed_files_left(pid));
  });
  if (!expect_exit(w, r, 0)) return 1;
  CHECK(during_files == 2);   // one per window while it runs
  std::map<std::string, int> seen;   // ADSCREENW -> starts
  for (auto& s : host_events(w, "start")) {
    const int nth = seen[s["ADSCREENW"]]++;
    if (nth == 0) {
      CHECK(s["seed_check"].rfind("ok ", 0) == 0);
    } else {
      CHECK(s["ADSEEDIMG"].empty() && s["seed_check"] == "none");
    }
  }
  CHECK(seen.size() == 2);
  for (auto& [k, n] : seen) CHECK(n >= 3);   // respawned at least twice
  CHECK(seed_files_left(scr_pid) == 0);

  // StartFromDesktop=0: no capture at all.
  Work w2 = prepare(o, "seed-off");
  edit_settings(w2, [](Settings& s) {
    s.module = "test.rings";
    s.randomize.clear();
    s.all_monitors = false;
    s.start_from_desktop = false;
  });
  EnvList env2 = base_env(o, w2);
  env2.push_back({L"AD_SCR_TESTEXIT_AFTER_FRAMES", L"5"});
  r = run_scr(o, L"/s", env2, 60000, [&](DWORD pid) { scr_pid = pid; });
  if (!expect_exit(w2, r, 0)) return 1;
  for (auto& s : host_events(w2, "start")) CHECK(s["ADSEEDIMG"].empty() && s["seed_check"] == "none");
  CHECK(count_in_log(w2.scr_log, "seed: off") == 1);
  CHECK(seed_files_left(scr_pid) == 0);
  // /p never captures.
  check_hosts_gone(w);
  check_hosts_gone(w2);
  if (g_failures) {
    dump_logs(w);
    dump_logs(w2);
  }
  return 0;
}

// §8 with a catalog whose modules each give a screen of their own (a
// hand-edited or tampered one: adimport writes "640x480" alone). A window
// that may start with any of them gets three pictures at most, taken before
// it appears (plan_seed_shots): After Dark's, 640x480 and the smallest of the
// other sizes, each logging a line, taken or not (a desktop that can't be
// read back fails every capture); a line counts the screens left out, and a
// first host whose screen is one of them starts on black.
int test_seed_screens(const Opts& o) {
  Work w = prepare(o, "seed-screens", false);
  const fs::path win = w.assets / "win";
  fs::create_directories(win);
  // id, file, screen ("": none, After Dark's that follows the display).
  const std::vector<std::array<std::string, 3>> mods = {
      {"many.plain", "PLAIN", ""},        {"many.vga", "VGA", "640x480"},    {"many.qvga", "QVGA", "320x240"},
      {"many.m400", "M400", "400x300"},   {"many.m512", "M512", "512x384"},  {"many.svga", "SVGA", "800x600"},
      {"many.xga", "XGA", "1024x768"},    {"many.m1152", "M1152", "1152x864"}, {"many.sxga", "SXGA", "1280x1024"},
      {"many.wide", "WIDE", "1024x640"},  {"many.hd", "HD", "1280x720"},      {"many.tall", "TALL", "480x640"}};
  std::string json = "{\"version\": 1, \"modules\": [";
  for (size_t i = 0; i < mods.size(); ++i) {
    const auto& [id, file, screen] = mods[i];
    json += std::string(i ? ",\n" : "\n") + "{\"id\": \"" + id + "\", \"displayName\": \"" + file +
            "\", \"lane\": \"pe32\", \"path\": \"FILES/MANY/" + file + ".AD\"" +
            (screen.empty() ? "" : ", \"screen\": \"" + screen + "\"") + "}";
  }
  json += "]}\n";
  CHECK(write_file_atomic((win / "catalog-win.json").wstring(), json));
  Catalog c;
  CHECK(load_catalog((win / "catalog-win.json").wstring(), c, nullptr) && c.modules.size() == mods.size());
  for (const auto& m : c.modules) {
    fs::path p = resolve_module_path(win.wstring(), m.path);
    fs::create_directories(p.parent_path());
    write_file_atomic(p.wstring(), "placeholder module file for the LongAfterDark.scr smoke tests\n");
  }
  // 1024x768, one of the screens left out, leads a list of all the others.
  edit_settings(w, [&](Settings& s) {
    s.module = "many.xga";
    s.randomize.clear();
    for (const auto& m : mods) {
      if (m[0] != s.module) s.randomize.push_back(m[0]);
    }
    s.scale = 1.0;
    s.all_monitors = false;
  });
  EnvList env = base_env(o, w);
  env.push_back({L"AD_SCR_TEST_MONITORS", L"-16000,0,1280,720,p"});
  env.push_back({L"AD_SCR_TESTEXIT_AFTER_FRAMES", L"5"});
  DWORD scr_pid = 0;
  RunResult r = run_scr(o, L"/s", env, 60000, [&](DWORD pid) { scr_pid = pid; });
  if (!expect_exit(w, r, 0)) return 1;
  // Twelve screens (After Dark's 856x480 on this 16:9 monitor, and eleven of
  // their own): three pictures, nine left out.
  const int taken = count_in_log(w.scr_log, "seed window=0 ");
  CHECK(taken + count_in_log(w.scr_log, "seed window=0: capture failed") == 3);
  CHECK(count_in_log(w.scr_log, "seed window=0: none taken at 9 more screens of modules' own (2 at most: 640x480, "
                                "then the smallest); a first host at one starts on black") == 1);
  auto starts = host_events(w, "start");
  CHECK(starts.size() == 1);
  if (!starts.empty()) {
    CHECK(ends_with(starts[0]["module"], "XGA.AD") && screen_is(starts[0], "1024", "768"));
    CHECK(starts[0]["ADSEEDIMG"].empty() && starts[0]["seed_check"] == "none");
  }
  if (taken > 0) {
    CHECK(count_in_log(w.scr_log, "seed window=0 856x480 from 1280x720 in ") == 1);
    CHECK(count_in_log(w.scr_log, "seed window=0 640x480 from 960x720 (the frame's part of the monitor) in ") == 1);
    CHECK(count_in_log(w.scr_log, "seed window=0 320x240 from 960x720 (the frame's part of the monitor) in ") == 1);
    CHECK(count_in_log(w.scr_log, "seed window=0: none taken at 1024x768; the module starts on black") == 1);
  } else {
    fprintf(stderr, "seed-screens: no desktop capture here (a desktop that can't be read back); pictures not checked\n");
  }
  CHECK(seed_files_left(scr_pid) == 0);
  check_hosts_gone(w);
  if (g_failures) dump_logs(w);
  return 0;
}

// §6.3: a module button runs `adhostwin --configure` owned by the dialog,
// with the dialog's unsaved values and the state folder; the dialog is
// disabled until the host exits (a crash included), then usable again, with
// the outcome under the row and the live preview restarted.
int test_config_buttons(const Opts& o) {
  Work w = prepare(o, "config-buttons");
  edit_settings(w, [](Settings& s) {
    s.module = "test.stops";
    s.randomize.clear();
    s.controls["test.stops"] = {{1, 33}};
  });
  const fs::path exit_file = w.dir / "configure-exit.txt";
  write_file_atomic(exit_file.wstring(), "0");
  EnvList env = base_env(o, w);
  env.push_back({L"FAKEHOST_CONFIGURE_MS", L"1500"});
  env.push_back({L"FAKEHOST_CONFIGURE_EXIT_FILE", exit_file.wstring()});
  auto slot = [](HWND panel, int i, int part) { return GetDlgItem(panel, IDC_PANEL_BASE + i * IDC_PANEL_STRIDE + part); };

  DWORD dialog_pid = 0;
  HWND dlg_seen = nullptr;
  int step = 0;
  bool live_button = false, enabled_while_running = false, disabled_seen = false;
  std::string note_crash, note_nothing, note_ok;
  size_t previews_before = 0;
  auto previews = [&](DWORD pid) {
    size_t n = 0;
    for (auto& e : hosts_of(w, pid)) n += ends_with(e["module"], "TESTSTOP.AD");
    return n;
  };
  auto ends = [&] { return host_events(w, "configure-end").size(); };
  RunResult r = run_scr(o, L"/c", env, 90000, [&](DWORD pid) {
    dialog_pid = pid;
    HWND dlg = find_dialog(pid);
    if (!dlg) return;
    dlg_seen = dlg;
    HWND panel = GetDlgItem(dlg, IDC_PANEL);
    HWND button = slot(panel, 0, IDC_PART_INPUT), note = slot(panel, 0, IDC_PART_VALUE);
    HWND combo = slot(panel, 3, IDC_PART_INPUT);
    wchar_t cls[32] = {};
    if (button) GetClassNameW(button, cls, 32);
    const bool is_button = button && _wcsicmp(cls, L"Button") == 0 && IsWindowEnabled(button);
    auto press = [&] {
      PostMessageW(panel, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(button), BN_CLICKED), (LPARAM)button);
    };
    const size_t configures = host_events(w, "configure").size();
    // While a run is on: the dialog stays disabled.
    if (configures > ends()) {
      if (IsWindowEnabled(dlg)) enabled_while_running = true;
      else disabled_seen = true;
      return;
    }
    switch (step) {
      case 0:
        // The probe has answered (the row is a live button) and the live
        // preview runs: change a value (unsaved), then press the button.
        if (!is_button || previews(pid) < 1 || !combo) return;
        live_button = true;
        SendMessageW(combo, CB_SETCURSEL, 0, 0);
        SendMessageW(panel, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(combo), CBN_SELCHANGE), (LPARAM)combo);
        previews_before = previews(pid);
        press();
        step = 1;
        break;
      case 1:   // ran (exit 0): usable again, the live preview started over
        if (ends() < 1 || !IsWindowEnabled(dlg) || previews(pid) <= previews_before) return;
        note_ok = window_text(note);
        write_file_atomic(exit_file.wstring(), "crash");
        previews_before = previews(pid);
        press();
        step = 2;
        break;
      case 2:   // crashed: usable again all the same, with the code shown, and the preview restarted
        if (ends() < 2 || !IsWindowEnabled(dlg) || previews(pid) <= previews_before) return;
        if (window_text(note).empty()) return;
        note_crash = window_text(note);
        write_file_atomic(exit_file.wstring(), "4");
        previews_before = previews(pid);
        press();
        step = 3;
        break;
      case 3:   // nothing to set
        if (ends() < 3 || !IsWindowEnabled(dlg) || previews(pid) <= previews_before) return;
        if (window_text(note).find("Nothing") == std::string::npos) return;
        note_nothing = window_text(note);
        PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
        step = 4;
        break;
    }
  });
  CHECK(step == 4);
  if (!expect_exit(w, r, 0)) return 1;
  CHECK(live_button);
  CHECK(disabled_seen && !enabled_while_running);
  CHECK(note_ok.empty());
  CHECK(note_crash == "Couldn\xE2\x80\x99t open this option (code 0xC0000005)");
  CHECK(note_nothing == "Nothing to set here");
  auto cfg = host_events(w, "configure");
  CHECK(cfg.size() == 3);
  const std::string owner = std::to_string((unsigned long long)(uintptr_t)dlg_seen);
  for (auto& e : cfg) {
    CHECK(ends_with(e["module"], "TESTSTOP.AD"));
    CHECK(e["button"] == "0");
    CHECK(e["owner"] == owner);
    CHECK(e["owner_enabled"] == "0");                    // disabled before the host even started
    CHECK(e["ADCVSET"] == "1=33,3=0");                    // the unsaved popup change included
    CHECK(e["AD_ASSETS_DIR"] == w.assets.string());
    CHECK(strtoul(e["ppid"].c_str(), nullptr, 10) == dialog_pid);
  }
  for (auto& e : host_events(w, "configure-end")) CHECK(e["owner_ever_enabled"] == "0");
  // The live preview restarted after each run: a new host each time.
  std::set<std::string> preview_pids;
  for (auto& e : hosts_of(w, dialog_pid))
    if (ends_with(e["module"], "TESTSTOP.AD")) preview_pids.insert(e["pid"]);
  CHECK(preview_pids.size() >= 4);   // the first, then one after each of the three runs
  CHECK(host_events(w, "capabilities").size() == 1);
  check_state_everywhere(w);
  check_hosts_gone(w);
  // Cancel saved nothing (the module's own state is its business).
  Settings s;
  CHECK(load_settings(w.settings.wstring(), s));
  CHECK((s.controls["test.stops"] == std::map<int, int>{{1, 33}}));
  if (g_failures) dump_logs(w);
  return 0;
}

// ---- opt-in, real modules (AD_E2E=1) --------------------------------------------------
// --realhost <adhostwin.exe> (a build with the lanes), AD_E2E_ASSETS=<assets root with
// the Deluxe package>. Skipped (77) otherwise.

// Ends a running /s as closing one of its windows does (WM_CLOSE).
void post_close(DWORD pid) {
  EnumWindows([](HWND h, LPARAM lp) -> BOOL {
    DWORD p = 0;
    GetWindowThreadProcessId(h, &p);
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    if (p == (DWORD)lp && wcscmp(cls, L"LongAfterDarkSaver") == 0) {
      PostMessageW(h, WM_CLOSE, 0, 0);
      return FALSE;
    }
    return TRUE;
  }, (LPARAM)pid);
}

// ---- sound (AUDIO.md §9) -------------------------------------------------------------
// fakehost makes no sound: these check what the saver tells each host. Each
// takes the CTest environment's AD_SCR_SOUND=0 out of the saver's.

// A spawn line's pid and sound ("spawn window=<w> … sound=<0|1> volume=<v> pid=<p>"), in log order.
struct SoundSpawn {
  int line = 0, window = -1, sound = -1, volume = -2;
  DWORD pid = 0;
};
std::vector<SoundSpawn> sound_spawns(const Work& w) {
  std::vector<SoundSpawn> out;
  auto lines = lines_of(w.scr_log);
  for (int i = 0; i < (int)lines.size(); ++i) {
    const std::string& l = lines[i];
    size_t at = l.find("spawn window=");
    if (at == std::string::npos) continue;
    SoundSpawn s;
    s.line = i;
    s.window = atoi(l.c_str() + at + 13);
    if (size_t p = l.find(" sound="); p != std::string::npos) s.sound = atoi(l.c_str() + p + 7);
    if (size_t p = l.find(" volume="); p != std::string::npos) s.volume = atoi(l.c_str() + p + 8);
    if (size_t p = l.find(" pid="); p != std::string::npos) s.pid = (DWORD)strtoul(l.c_str() + p + 5, nullptr, 10);
    out.push_back(s);
  }
  return out;
}

// Every host fakehost logged: ADSOUND=1 ADVOLUME=<volume> ADAUDIOOUT=<capture>
// for those in `sound_pids`, ADSOUND=0 and nothing else about sound for the rest.
void check_host_sound(const Work& w, const std::set<DWORD>& sound_pids, int volume, const std::string& capture) {
  int n = 0;
  for (const char* kind : {"start", "capabilities", "configure"}) {
    for (auto& e : host_events(w, kind)) {
      ++n;
      const DWORD pid = (DWORD)strtoul(e["pid"].c_str(), nullptr, 10);
      if (sound_pids.count(pid)) {
        if (e["ADSOUND"] != "1" || e["ADVOLUME"] != std::to_string(volume) || e["ADAUDIOOUT"] != capture)
          failf("%s pid %lu: ADSOUND=%s ADVOLUME=%s ADAUDIOOUT=%s (the sound host)", kind, pid, e["ADSOUND"].c_str(),
                e["ADVOLUME"].c_str(), e["ADAUDIOOUT"].c_str());
      } else if (e["ADSOUND"] != "0" || !e["ADVOLUME"].empty() || !e["ADAUDIOOUT"].empty()) {
        failf("%s pid %lu: ADSOUND=%s ADVOLUME=%s ADAUDIOOUT=%s (a silent host)", kind, pid, e["ADSOUND"].c_str(),
              e["ADVOLUME"].c_str(), e["ADAUDIOOUT"].c_str());
      }
    }
  }
  CHECK(n > 0);
}

// Never two sound hosts at once, by the saver's log: "spawn … sound=1" adds
// one, "sound host stops" takes it away.
void check_one_sound_host(const Work& w) {
  int live = 0;
  for (const auto& l : lines_of(w.scr_log)) {
    if (l.find("spawn window=") != std::string::npos && l.find(" sound=1 ") != std::string::npos) ++live;
    if (l.find("sound host stops window=") != std::string::npos) --live;
    if (live > 1) failf("two sound hosts at once, at: %s", l.c_str());
    if (live < 0) failf("a sound host stopped twice, at: %s", l.c_str());
  }
}

// Who plays in /s: only the primary monitor's window's host, with the
// settings' volume and the capture the saver was given; the other monitor's
// hosts are told ADSOUND=0 and lose ADAUDIOOUT. Rotation keeps it that way,
// and when the primary monitor changes the old owner's module starts again
// silent and the new owner's next host plays. Then Sound=0, AD_SCR_SOUND=0
// and /p: nobody plays.
int test_sound(const Opts& o) {
  {
    Work w = prepare(o, "sound");
    edit_settings(w, [](Settings& s) {
      s.module = "random";
      s.randomize.clear();
      s.all_monitors = true;
      s.volume = 35;   // Sound=1 is the default
    });
    const std::string capture = (w.dir / "inherited.wav").string();
    EnvList env = base_env(o, w);
    env.push_back({kSoundOverrideEnv, L""});
    env.push_back({L"ADAUDIOOUT", widen(capture)});   // the sound host keeps it; the others must not
    env.push_back({L"ADSOUND", L"1"});                // inherited: never trusted
    // The primary moves to the second monitor at the first topology change.
    env.push_back({L"AD_SCR_TEST_MONITORS", L"0,0,640,480,p;640,0,854,480|0,0,640,480;640,0,854,480,p"});
    env.push_back({L"AD_SCR_TEST_ROTATE_MS", L"700"});
    int step = 0;
    RunResult r = run_scr(o, L"/s", env, 60000, [&](DWORD pid) {
      auto spawns = sound_spawns(w);
      auto count = [&](int window, int sound, int after) {
        int n = 0;
        for (auto& s : spawns) n += s.window == window && s.sound == sound && s.line > after;
        return n;
      };
      auto lines = lines_of(w.scr_log);
      if (step == 0 && count(0, 1, -1) >= 2 && count(1, 0, -1) >= 2) {   // both rotated at least once
        post_display_change(pid, 1);
        step = 1;
      } else if (step == 1) {
        const int relayout = find_line(lines, "relayout monitors");
        if (relayout >= 0 && count(1, 1, relayout) >= 1 && count(0, 0, relayout) >= 2) {
          post_close(pid);
          step = 2;
        }
      }
    });
    if (!expect_exit(w, r, 0)) return 1;
    CHECK(step == 2);
    auto lines = lines_of(w.scr_log);
    const int relayout = find_line(lines, "relayout monitors=2->2 kept=2");
    const int dropped = find_line(lines, "sound: window=0 is no longer the primary monitor's");
    CHECK(relayout > 0 && dropped > relayout);
    CHECK(find_line(lines, "sound: primary monitor, volume 35") >= 0);
    std::set<DWORD> sound_pids;
    for (auto& s : sound_spawns(w)) {
      // Before the change window 0 is the primary's, after it window 1.
      const bool owner = s.line < relayout ? s.window == 0 : s.window == 1;
      if (s.sound != (owner ? 1 : 0)) failf("spawn window=%d at line %d: sound=%d", s.window, s.line, s.sound);
      if (s.sound == 1) {
        CHECK(s.volume == 35);
        sound_pids.insert(s.pid);
      }
    }
    CHECK(sound_pids.size() >= 3);
    check_host_sound(w, sound_pids, 35, capture);
    check_one_sound_host(w);
    // Window 0's module started again at once (its stop and stats lines
    // between, no rotation), silent.
    const int restart = find_line(lines, "spawn window=0", dropped);
    const int rotated = find_line(lines, "rotate window=0", dropped);
    CHECK(restart > dropped && restart <= dropped + 4 && (rotated < 0 || rotated > restart));
    if (restart > 0) CHECK(lines[restart].find(" sound=0 ") != std::string::npos);
    check_hosts_gone(w);
    if (g_failures) {
      dump_logs(w);
      return 0;
    }
  }
  // Sound=0; then AD_SCR_SOUND=0 over Sound=1; then /p: no host plays.
  for (int c = 0; c < 3; ++c) {
    const char* name = c == 0 ? "sound-off" : c == 1 ? "sound-override" : "sound-p";
    Work w = prepare(o, name);
    edit_settings(w, [&](Settings& s) {
      s.module = "test.rings";
      s.randomize.clear();
      s.all_monitors = false;
      s.sound = c != 0;
    });
    EnvList env = base_env(o, w);
    env.push_back({kSoundOverrideEnv, c == 1 ? L"0" : L""});
    env.push_back({L"ADAUDIOOUT", (w.dir / "never.wav").wstring()});
    env.push_back({L"AD_SCR_TESTEXIT_AFTER_FRAMES", L"10"});
    HWND parent = c == 2 ? make_parent(152, 112) : nullptr;
    RunResult r = run_scr(o, c == 2 ? L"/p " + hwnd_arg(parent) : std::wstring(L"/s"), env, 60000);
    if (parent) DestroyWindow(parent);
    if (!expect_exit(w, r, 0)) return 1;
    CHECK(!host_events(w, "start").empty());
    check_host_sound(w, {}, 0, "");
    for (auto& s : sound_spawns(w)) CHECK(s.sound == 0);
    if (c == 0) CHECK(count_in_log(w.scr_log, "sound: off (Sound=0)") == 1);
    if (c == 1) CHECK(count_in_log(w.scr_log, "sound: off (AD_SCR_SOUND=0)") == 1);
    check_hosts_gone(w);
    if (g_failures) dump_logs(w);
  }
  return 0;
}

// Waking (AUDIO.md §9): the sound host is sent QUIT before anything else and
// given its longer grace (400 ms) to silence its device: a host that takes
// 220 ms after QUIT ends on its own. The other monitor's hosts, rotated away
// with the ordinary 150 ms grace, are terminated first. Rotation stops each
// sound host the same way.
int test_sound_wake(const Opts& o) {
  Work w = prepare(o, "sound-wake");
  edit_settings(w, [](Settings& s) {
    s.module = "random";
    s.randomize.clear();
    s.all_monitors = true;
    s.volume = 64;
  });
  EnvList env = base_env(o, w);
  env.push_back({kSoundOverrideEnv, L""});
  env.push_back({L"AD_SCR_TEST_MONITORS", L"0,0,640,480,p;640,0,854,480"});
  // Long enough for each host to be up (a loaded machine can take a few
  // hundred ms to start one) before QUIT reaches it.
  env.push_back({L"AD_SCR_TEST_ROTATE_MS", L"1500"});
  env.push_back({L"FAKEHOST_QUIT_DELAY_MS", L"220"});
  bool closed = false;
  size_t seen = 0;
  ULONGLONG changed_at = GetTickCount64();
  RunResult r = run_scr(o, L"/s", env, 60000, [&](DWORD pid) {
    if (closed) return;
    int s0 = 0, s1 = 0;
    auto spawns = sound_spawns(w);
    for (auto& s : spawns) (s.window == 0 ? s0 : s1) += 1;
    if (spawns.size() != seen) {
      seen = spawns.size();
      changed_at = GetTickCount64();
    }
    // Closed once the latest hosts have been up a while.
    if (s0 >= 3 && s1 >= 3 && GetTickCount64() - changed_at >= 800) {
      post_close(pid);
      closed = true;
    }
  });
  if (!expect_exit(w, r, 0)) return 1;
  CHECK(closed);
  auto lines = lines_of(w.scr_log);
  const int exit_line = find_line(lines, "exit code=0 reason=WM_CLOSE");
  const int quit_first = find_line(lines, "wake: QUIT to the sound host window=0 first");
  CHECK(exit_line >= 0 && quit_first > exit_line);
  CHECK(find_line(lines, "sound host stops window=0", quit_first) > quit_first);
  std::map<DWORD, std::string> reason;
  for (auto& e : host_events(w, "exit")) reason[(DWORD)strtoul(e["pid"].c_str(), nullptr, 10)] = e["reason"];
  std::set<DWORD> sound_pids;
  int silent_rotated = 0;
  auto spawns = sound_spawns(w);
  for (size_t i = 0; i < spawns.size(); ++i) {
    const SoundSpawn& s = spawns[i];
    CHECK(s.sound == (s.window == 0 ? 1 : 0));
    if (s.sound == 1) {
      sound_pids.insert(s.pid);
      // Every sound host, rotated away or at the end, ended on its own.
      if (reason[s.pid] != "quit") failf("sound host pid %lu: exit reason \"%s\"", s.pid, reason[s.pid].c_str());
    } else {
      // A silent host rotated away (not the last of its window) had 150 ms: cut short.
      bool last = true;
      for (size_t j = i + 1; j < spawns.size(); ++j) last &= spawns[j].window != s.window;
      if (!last) {
        ++silent_rotated;
        if (reason.count(s.pid)) failf("silent host pid %lu outlasted its grace (%s)", s.pid, reason[s.pid].c_str());
      }
    }
  }
  CHECK(sound_pids.size() >= 3 && silent_rotated >= 2);
  check_host_sound(w, sound_pids, 64, "");
  check_one_sound_host(w);
  check_hosts_gone(w);
  if (g_failures) dump_logs(w);
  return 0;
}

// The settings dialog's Sound and Volume (AUDIO.md §9): shown from the file
// (defaults for a file without them), Volume a 0..100 slider with page 10,
// named "Volume" for screen readers, driven by the keys, greyed while Sound
// is Off. Preview plays the dialog's unsaved values (only the primary
// monitor's host of that "/s"); the dialog's own hosts (live preview,
// thumbnails, the capabilities probe) never do. OK writes Sound and Volume
// and keeps the rest; the next dialog shows them.
int test_config_sound(const Opts& o) {
  Work w = prepare(o, "config-sound");
  fs::path tmp = w.dir / "tmp";
  fs::create_directories(tmp);
  EnvList env = base_env(o, w);
  env.push_back({kSoundOverrideEnv, L""});
  env.push_back({L"TMP", tmp.wstring()});
  env.push_back({L"TEMP", tmp.wstring()});
  env.push_back({L"AD_SCR_THUMBGEN", L"1"});                 // its hosts must be silent too
  env.push_back({L"AD_SCR_TESTEXIT_AFTER_FRAMES", L"30"});   // the Preview's "/s" ends itself
  env.push_back({L"ADAUDIOOUT", (w.dir / "never.wav").wstring()});
  struct Seen {
    LRESULT sel = -1, pos = -1, mn = -1, mx = -1, page = -1;
    std::string value, label;
    bool enabled = false;
    A11y a;
  };
  auto look = [](HWND dlg) {
    Seen s;
    HWND vol = GetDlgItem(dlg, IDC_VOLUME);
    s.sel = SendMessageW(GetDlgItem(dlg, IDC_SOUND), CB_GETCURSEL, 0, 0);
    s.pos = SendMessageW(vol, TBM_GETPOS, 0, 0);
    s.mn = SendMessageW(vol, TBM_GETRANGEMIN, 0, 0);
    s.mx = SendMessageW(vol, TBM_GETRANGEMAX, 0, 0);
    s.page = SendMessageW(vol, TBM_GETPAGESIZE, 0, 0);
    s.value = window_text(GetDlgItem(dlg, IDC_VOLUME_VALUE));
    s.label = window_text(GetDlgItem(dlg, IDC_VOLUME_LABEL));
    s.enabled = IsWindowEnabled(vol) != FALSE;
    s.a = a11y_of(vol);
    return s;
  };
  auto key = [](HWND h, UINT vk) {
    SendMessageW(h, WM_KEYDOWN, vk, 1);
    SendMessageW(h, WM_KEYUP, vk, 0xC0000001);
  };
  Seen first, keyed, offed;
  std::string note, items;
  DWORD dialog_pid = 0, preview_pid = 0;
  int step = 0;
  RunResult r = run_scr(o, L"/c", env, 90000, [&](DWORD pid) {
    dialog_pid = pid;
    HWND dlg = find_dialog(pid);
    HWND list = dlg ? GetDlgItem(dlg, IDC_MODULE_LIST) : nullptr;
    if (!list || SendMessageW(list, LVM_GETITEMCOUNT, 0, 0) == 0) return;
    HWND vol = GetDlgItem(dlg, IDC_VOLUME), snd = GetDlgItem(dlg, IDC_SOUND);
    if (step == 0) {
      // The dialog's own hosts first: its live preview and a thumbnail (640x480).
      bool live = false, thumb = false;
      for (auto& e : hosts_of(w, pid)) (e["ADSCREENW"] == "640" && e["ADSCREENH"] == "480" ? thumb : live) = true;
      if (!live || !thumb) return;
      first = look(dlg);
      note = window_text(GetDlgItem(dlg, IDC_SOUND_NOTE));
      for (int i = 0, n = (int)SendMessageW(snd, CB_GETCOUNT, 0, 0); i < n; ++i) {
        wchar_t buf[64] = {};
        if (SendMessageW(snd, CB_GETLBTEXTLEN, i, 0) < 64) SendMessageW(snd, CB_GETLBTEXT, i, (LPARAM)buf);
        items += (items.empty() ? "" : "|") + narrow(buf);
      }
      // 50 → Page Up 60 → Up 61 → Right 62 → Left 61 → Page Down 51 → Page Up 61.
      for (UINT vk : {VK_PRIOR, VK_UP, VK_RIGHT, VK_LEFT, VK_NEXT, VK_PRIOR}) key(vol, vk);
      keyed = look(dlg);
      click(dlg, IDC_PREVIEW);   // the unsaved 61 plays
      step = 1;
    } else if (step == 1) {
      for (auto& e : hosts_of(w, pid, true)) preview_pid = (DWORD)strtoul(e["ppid"].c_str(), nullptr, 10);
      // The Preview has ended (its test exit) and the dialog took it back.
      if (preview_pid && IsWindowEnabled(GetDlgItem(dlg, IDC_PREVIEW))) step = 2;
    } else if (step == 2) {
      SendMessageW(snd, CB_SETCURSEL, 1, 0);
      SendMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDC_SOUND, CBN_SELCHANGE), (LPARAM)snd);
      offed = look(dlg);
      key(vol, VK_PRIOR);   // disabled: nothing moves
      PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), (LPARAM)GetDlgItem(dlg, IDOK));
      step = 3;
    }
  });
  CHECK(step == 3);
  if (!expect_exit(w, r, 0)) return 1;
  CHECK(first.sel == 0 && first.pos == 50 && first.mn == 0 && first.mx == 100 && first.page == 10);
  CHECK(first.value == "50" && first.label == "&Volume" && first.enabled);
  CHECK(first.a.ok && first.a.name == "Volume" && first.a.control_type == UIA_SliderControlTypeId);
  CHECK(items == "Primary monitor|Off");
  CHECK(note == "Sound plays from the primary monitor’s screen saver.");
  CHECK(keyed.pos == 61 && keyed.value == "61");
  CHECK(offed.sel == 1 && !offed.enabled && offed.label == "Volume" && offed.pos == 61);
  // The Preview's "/s": its primary monitor's host played the unsaved 61.
  std::set<DWORD> sound_pids;
  for (auto& s : sound_spawns(w)) {
    if (s.sound == 1) {
      CHECK(s.window == 0 && s.volume == 61);
      sound_pids.insert(s.pid);
    }
  }
  CHECK(sound_pids.size() == 1);
  // The dialog's own hosts (live preview, thumbnails) and its capabilities
  // probe: silent, the capture it was given taken away.
  CHECK(!hosts_of(w, dialog_pid).empty());
  check_host_sound(w, sound_pids, 61, (w.dir / "never.wav").string());
  Settings s;
  CHECK(load_settings(w.settings.wstring(), s));
  CHECK(!s.sound && s.volume == 61 && s.sound_monitor == "primary");
  std::string text;
  read_file(w.settings.wstring(), text);
  CHECK(text.find("Sound=0\r\nVolume=61\r\nSoundMonitor=primary\r\n") != std::string::npos);
  CHECK(text.find("FutureKey=keep me\r\n") != std::string::npos);

  // The next dialog shows them: Off, and Volume greyed at 61.
  Seen again;
  bool done = false;
  r = run_scr(o, L"/c", env, 60000, [&](DWORD pid) {
    if (done) return;
    HWND dlg = find_dialog(pid);
    HWND list = dlg ? GetDlgItem(dlg, IDC_MODULE_LIST) : nullptr;
    if (!list || SendMessageW(list, LVM_GETITEMCOUNT, 0, 0) == 0) return;
    again = look(dlg);
    PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
    done = true;
  });
  CHECK(done);
  if (!expect_exit(w, r, 0)) return 1;
  CHECK(again.sel == 1 && again.pos == 61 && again.value == "61" && !again.enabled && again.label == "Volume");
  check_hosts_gone(w);
  if (g_failures) {
    fprintf(stderr, "---- settings.ini\n%s\n", text.c_str());
    dump_logs(w);
  }
  return 0;
}

std::wstring g_realhost;

bool e2e_ready(std::wstring* assets) {
  if (env_w(L"AD_E2E") != L"1") {
    printf("SKIP: set AD_E2E=1 (and AD_E2E_ASSETS) to run the real-module tests\n");
    return false;
  }
  *assets = env_w(L"AD_E2E_ASSETS");
  if (g_realhost.empty() || !file_exists(g_realhost) || assets->empty() ||
      !file_exists(join_path(join_path(*assets, L"win"), L"catalog-win.json"))) {
    printf("SKIP: no real adhostwin (--realhost) or no assets (AD_E2E_ASSETS)\n");
    return false;
  }
  return true;
}

// Rodger Dodger /s: Caps Lock starts the game, arrows steer it, nothing exits;
// Caps Lock again ends it and the next key exits, with the reasons logged.
int test_e2e_rodger(const Opts& o) {
  std::wstring assets;
  if (!e2e_ready(&assets)) return kSkip;
  Work w = prepare(o, "e2e-rodger", false);
  edit_settings(w, [](Settings& s) {
    s.module = "ad40.rodger";
    s.randomize.clear();
    s.all_monitors = false;
  });
  fs::path script = write_script(w, "input.txt",
                                 "FRAMES 90\nLOG step caps-on\nKEY 20 1\nKEY 20 0\nFRAMES 30\nSTATUSLOG\n"
                                 "LOG step arrows\n"
                                 "KEY 37 1\nFRAMES 10\nKEY 37 0\nKEY 39 1\nFRAMES 10\nKEY 39 0\nKEY 38 1\nFRAMES 10\nKEY 38 0\n"
                                 "KEY 40 1\nFRAMES 10\nKEY 40 0\nFRAMES 30\nSTATUSLOG\n"
                                 "LOG step caps-off\nKEY 20 1\nKEY 20 0\nFRAMES 30\nSTATUSLOG\n"
                                 "LOG step exit-key\nKEY 65 1\nWAIT 5000\nLOG not-exited\n");
  EnvList env = input_env(o, w, script);
  env.push_back({L"AD_HOST_EXE", g_realhost});
  env.push_back({L"AD_ASSETS_DIR", assets});
  RunResult r = run_scr(o, L"/s", env, 180000);
  if (!expect_exit(w, r, 0)) return 1;
  auto lines = lines_of(w.scr_log);
  const int on = find_line(lines, "test: step caps-on"), arrows = find_line(lines, "test: step arrows");
  const int off = find_line(lines, "test: step caps-off"), key = find_line(lines, "test: step exit-key");
  CHECK(on > 0 && arrows > on && off > arrows && key > off);
  CHECK(find_line(lines, "play starts window=0") > on && find_line(lines, "play starts window=0") < arrows);
  CHECK(find_line(lines, "play ends window=0") > off && find_line(lines, "play ends window=0") < key);
  CHECK(find_line(lines, "input: key vk=0x41") > key);
  CHECK(find_line(lines, "not-exited") < 0);
  for (int i = 0; i < key && i < (int)lines.size(); ++i) {
    if (lines[i].find("] input: key") != std::string::npos || lines[i].find("] input: move") != std::string::npos)
      failf("an exit before the last key: %s", lines[i].c_str());
  }
  std::string last;
  CHECK(read_file((w.dir / "logs" / "saver-last.log").wstring(), last) && last.find("input: key vk=0x41") != std::string::npos);
  check_hosts_gone(w);
  if (g_failures) dump_logs(w);
  return 0;
}

// DOS Shell /S for 60 s, with the upper-case switch Windows itself passes
// (its .scr verb is `"%1" /S`): no input or host exit before the test's own
// exit, and the last-exit log lands next to AD_SETTINGS (§9.1). The saver's
// one monitor is staged off every real one, so the run never covers the
// screens of whoever is at the machine, and it is started with CreateProcess:
// a ShellExecute of a .scr can turn into the real full-screen saver.
int test_e2e_dosshell(const Opts& o) {
  std::wstring assets;
  if (!e2e_ready(&assets)) return kSkip;
  Work w = prepare(o, "e2e-dosshell", false);
  edit_settings(w, [](Settings& s) {
    s.module = "classic.dosshell";
    s.randomize.clear();
    s.all_monitors = false;
  });
  EnvList env = base_env(o, w);
  env.push_back({L"AD_SCR_TEST_IGNORE_INPUT", L""});
  env.push_back({L"AD_SCR_TEST_MONITORS", L"-16000,0,856,480,p"});
  env.push_back({L"AD_HOST_EXE", g_realhost});
  env.push_back({L"AD_ASSETS_DIR", assets});
  env.push_back({L"AD_SCR_TESTEXIT_AFTER_FRAMES", L"3600"});
  RunResult r = run_scr(o, L"/S", env, 180000);
  CHECK(r.started && !r.timed_out && r.code == 0);
  if (!r.started) return 1;
  auto lines = lines_of(w.scr_log);
  const int test_exit = find_line(lines, "test-exit after 3600 frames");
  CHECK(test_exit > 0);
  for (int i = 0; i < test_exit && i < (int)lines.size(); ++i) {
    if (lines[i].find("] input: ") != std::string::npos && lines[i].find("input: caps") == std::string::npos &&
        lines[i].find("input: hold") == std::string::npos && lines[i].find("input: kept") == std::string::npos)
      failf("input before test-exit: %s", lines[i].c_str());
    if (lines[i].find("] host-") != std::string::npos) failf("host line before test-exit: %s", lines[i].c_str());
  }
  std::string last;
  CHECK(read_file((w.dir / "logs" / "saver-last.log").wstring(), last));
  CHECK(last.find("start /s") != std::string::npos && last.find("test-exit after 3600 frames") != std::string::npos);
  check_hosts_gone(w);
  if (g_failures) dump_logs(w);
  return 0;
}

// Opt-in (AD_E2E=1 AD_E2E_ASSETS=<root> AD_SCR_SOUND_E2E=1): the real
// adhostwin under /s on two staged monitors, Sound on at volume 40, with a
// capture and ADAUDIOLIVE=0 (so nothing is ever played on the device): the
// primary monitor's host captures, the other's makes no sound at all, and
// the sound host ends on its QUIT with its capture closed. Skipped (77) when
// the host can't make sound yet (--capabilities without audio=1).
int test_e2e_sound(const Opts& o) {
  std::wstring assets;
  if (!e2e_ready(&assets)) return kSkip;
  if (env_w(L"AD_SCR_SOUND_E2E") != L"1") {
    printf("SKIP: set AD_SCR_SOUND_E2E=1 as well (a real host with sound on; captured, never played)\n");
    return kSkip;
  }
  const HostCapabilities caps = probe_capabilities(g_realhost, 10000);
  if (caps.line.find("audio=1") == std::string::npos) {
    printf("SKIP: this adhostwin has no audio (--capabilities: %s)\n", caps.line.c_str());
    return kSkip;
  }
  Work w = prepare(o, "e2e-sound", false);
  edit_settings(w, [](Settings& s) {
    s.module = "simpsons.burns";   // speech from SIMP_SND.DLL in its first seconds (AUDIO.md §10.3)
    s.randomize.clear();
    s.all_monitors = true;
    s.volume = 40;
  });
  const fs::path wav = w.dir / "e2e-sound.wav";
  EnvList env = base_env(o, w);
  env.push_back({kSoundOverrideEnv, L""});
  env.push_back({L"AD_HOST_EXE", g_realhost});
  env.push_back({L"AD_ASSETS_DIR", assets});
  env.push_back({L"ADAUDIOLIVE", L"0"});      // never the real device
  env.push_back({L"ADAUDIOOUT", wav.wstring()});
  env.push_back({L"AD_SCR_TEST_MONITORS", L"0,0,640,480,p;640,0,640,480"});
  // 20 s: Burns first speaks about 11 s in (AUDIO.md §10.5).
  env.push_back({L"AD_SCR_TESTEXIT_AFTER_FRAMES", L"1200"});
  RunResult r = run_scr(o, L"/s", env, 240000);
  if (!expect_exit(w, r, 0)) return 1;
  auto spawns = sound_spawns(w);
  int owner = 0, other = 0;
  for (auto& s : spawns) {
    if (s.window == 0) owner += s.sound == 1 && s.volume == 40;
    else other += s.sound == 0;
  }
  CHECK(owner == 1 && other == 1);
  CHECK(count_in_log(w.scr_log, "wake: QUIT to the sound host window=0 first") == 1);
  // One host ran with sound: one "[audio]" summary in the hosts' stderr.
  CHECK(count_in_log(w.dir / "host-stderr.log", "[audio]") == 1);
  std::string data;
  CHECK(read_file(wav.wstring(), data));
  CHECK(data.size() > 44 && data.compare(0, 4, "RIFF") == 0 && data.compare(8, 4, "WAVE") == 0);
  if (data.size() >= 8) {
    uint32_t riff = 0;
    memcpy(&riff, data.data() + 4, 4);
    CHECK(riff + 8 == data.size());   // the header was patched at shutdown: a clean end
  }
  // Burns spoke: some 100 ms window of the capture (16-bit stereo; the data
  // chunk follows the 44-byte header the engine writes) is above -40 dBFS.
  if (data.size() > 44) {
    uint32_t rate = 0;
    memcpy(&rate, data.data() + 24, 4);
    const size_t n = (data.size() - 44) / 2, win = size_t(rate ? rate : 44100) / 10 * 2;
    const int16_t* x = reinterpret_cast<const int16_t*>(data.data() + 44);
    double loudest = -200;
    for (size_t i = 0; i + win <= n; i += win) {
      double sq = 0;
      for (size_t k = i; k < i + win; ++k) sq += double(x[k]) * x[k];
      if (sq > 0) loudest = std::max(loudest, 10 * std::log10(sq / double(win)) - 20 * std::log10(32768.0));
    }
    printf("e2e-sound: loudest 100 ms window %.1f dBFS\n", loudest);
    CHECK(loudest > -40);
  }
  check_hosts_gone(w);
  if (g_failures) dump_logs(w);
  return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
  // Monitor rects in physical pixels, as the (per-monitor-v2) saver sees them.
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  if (argc < 2) {
    fprintf(stderr, "usage: scr_smoke <test> --scr <exe> --fakehost <exe> --fakeimport <exe> --fixtures <dir> "
                    "--work <dir>\n");
    return 2;
  }
  std::wstring test = argv[1];
  Opts o;
  for (int i = 2; i + 1 < argc; i += 2) {
    std::wstring k = argv[i], v = argv[i + 1];
    for (auto& c : v) if (c == L'/') c = L'\\';
    if (k == L"--scr") o.scr = v;
    else if (k == L"--fakehost") o.fakehost = v;
    else if (k == L"--fakeimport") o.fakeimport = v;
    else if (k == L"--fixtures") o.fixtures = v;
    else if (k == L"--work") o.work = v;
    else if (k == L"--realhost") g_realhost = v;
    else if (k == L"--shipped") o.shipped = v;
  }
  if (o.scr.empty() || o.fakehost.empty() || o.fakeimport.empty() || o.fixtures.empty() || o.work.empty()) {
    fprintf(stderr, "missing --scr/--fakehost/--fakeimport/--fixtures/--work\n");
    return 2;
  }
  std::map<std::wstring, int (*)(const Opts&)> tests = {
      {L"run", test_run},
      {L"primary-only", test_primary_only},
      {L"preview", test_preview},
      {L"preview-parent-gone", test_preview_parent_gone},
      {L"not-imported", test_not_imported},
      {L"host-missing", test_host_missing},
      {L"respawn", test_respawn},
      {L"stall", test_stall},
      {L"rotate", test_rotate},
      {L"rotate-lead", test_rotate_lead},
      {L"start-failure", test_start_failure},
      {L"display-off", test_display_off},
      {L"p6-garbage", test_p6_garbage},
      {L"config", test_config},
      {L"config-lead", test_config_lead},
      {L"config-live", test_config_live},
      {L"config-classic", test_config_classic},
      {L"config-thumbs", test_config_thumbs},
      {L"list-top", test_list_top},
      {L"import", test_import},
      {L"preview-settings", test_preview_settings},
      {L"display-change", test_display_change},
      {L"input-play", test_input_play},
      {L"input-alt", test_input_alt},
      {L"input-wake", test_input_wake},
      {L"input-rotate", test_input_rotate},
      {L"input-monitors", test_input_monitors},
      {L"input-display-change", test_input_display_change},
      {L"present", test_present},
      {L"seed", test_seed},
      {L"seed-screens", test_seed_screens},
      {L"config-buttons", test_config_buttons},
      {L"config-collections", test_config_collections},
      {L"config-cover", test_config_cover},
      {L"rotate-collections", test_rotate_collections},
      {L"config-abi", test_config_abi},
      {L"rotate-abi", test_rotate_abi},
      {L"rotate-abi-wait", test_rotate_abi_wait},
      {L"rotate-monitors", test_rotate_monitors},
      {L"config-monitors", test_config_monitors},
      {L"screen-abi", test_screen_abi},
      {L"screen-field", test_screen_field},
      {L"config-twelve", test_config_twelve},
      {L"numlock", test_numlock},
      {L"config-credit", test_config_credit},
      {L"config-details", test_config_details},
      {L"data-root", test_data_root},
      {L"e2e-rodger", test_e2e_rodger},
      {L"e2e-dosshell", test_e2e_dosshell},
      {L"e2e-sound", test_e2e_sound},
      {L"sound", test_sound},
      {L"sound-wake", test_sound_wake},
      {L"config-sound", test_config_sound},
  };
  // Nothing started from here may touch the user's data folder: whatever a
  // test leaves to the defaults lands in a scratch base.
  SetEnvironmentVariableW(adw::kDataRootBaseVar, (fs::path(o.work) / "localappdata").wstring().c_str());
  if (test == L"host-stream") {
    test_host_stream(o);
  } else if (test == L"resources") {
    test_resources(o);
  } else {
    auto it = tests.find(test);
    if (it == tests.end()) {
      fprintf(stderr, "unknown test %ls\n", test.c_str());
      return 2;
    }
    if (!gui_available()) {
      printf("SKIP: no interactive input desktop (or AD_SCR_SKIP_GUI_TESTS set)\n");
      return kSkip;
    }
    if (it->second(o) == kSkip) return kSkip;
  }
  printf("%ls: %s\n", test.c_str(), g_failures ? "FAILED" : "ok");
  return g_failures ? 1 : 0;
}
