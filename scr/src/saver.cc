#include "saver.h"

#include <windows.h>
#include <wtsapi32.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "adw/ui/capture.h"
#include "catalog.h"
#include "desktop_seed.h"
#include "dialog_support.h"
#include "geometry.h"
#include "host_process.h"
#include "input_rules.h"
#include "log.h"
#include "paths.h"
#include "present.h"
#include "releases.h"
#include "settings.h"
#include "sound.h"
#include "test_hooks.h"

namespace adw::scr {

namespace {

using namespace std::chrono_literals;
using std::chrono::milliseconds;

constexpr UINT WM_APP_FRAME = WM_APP + 1;
constexpr UINT WM_APP_HOSTEXIT = WM_APP + 2;
constexpr UINT WM_APP_TESTEXIT = WM_APP + 3;
// The host's --capabilities answer (lParam: HostCapabilities*), posted to the
// saver's thread rather than to a window: a relayout may retire any window,
// the first one included, and a message queued for it goes with it.
constexpr UINT WM_APP_CAPS = WM_APP + 4;
// The longest the first hosts wait for that answer, when Random needs it
// (App::caps_gate): past it, the rotation keeps every module.
constexpr UINT kCapsWaitMs = 2000;
constexpr UINT_PTR kTimerWatchdog = 1;
constexpr UINT_PTR kTimerRotate = 2;
constexpr UINT_PTR kTimerTestDisplay = 3;
constexpr UINT_PTR kTimerRotateRetry = 4;
// Windows sends WM_DISPLAYCHANGE in bursts while it reconfigures (to every
// top-level window, often for several intermediate modes); the windows are
// re-planned once it has been quiet this long.
constexpr UINT kRelayoutSettleMs = 500;
// Caps Lock (and Num Lock, for a host that keeps it) is re-checked this often
// (INTERACTION.md §4.1), and the input owner's status with it.
constexpr UINT kCapsCheckMs = 250;
// While a decision is held (§4.3) the owner's status is polled this often.
constexpr UINT kHoldPollMs = 16;
// A rotation that waits for a game to end re-checks this often (§4.2).
constexpr UINT kRotateRetryMs = 1000;
constexpr UINT kScriptTickMs = 10;
constexpr wchar_t kClassName[] = L"LongAfterDarkSaver";
// GUID_CONSOLE_DISPLAY_STATE (winnt.h), spelled out so no GUID library is
// needed: the console display's power state, 0 off / 1 on / 2 dimmed.
constexpr GUID kConsoleDisplayState = {0x6fe69556, 0x704a, 0x47a0, {0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47}};
// A host that ran this long with frames was healthy; its death resets backoff.
constexpr auto kHealthyRun = 5s;
// Presenting a frame (present.h). A /s window draws through Direct2D: the
// GPU scales with high-quality cubic filtering for well under a millisecond
// of CPU. GDI is the fallback, and /p's way (a small downscale): HALFTONE is
// its good stretch, but upscaling with it runs in software (~14 ms for
// 856x480 -> 2560x1440, ~27 ms to 3840x2160) where COLORONCOLOR is
// accelerated (<1 ms). "auto" keeps a window's way while it costs less than
// this per frame: Direct2D over budget (a software rasterizer, no GPU) gives
// way to GDI, and GDI's HALFTONE upscales to COLORONCOLOR. Downscales
// (previews) are cheap and always HALFTONE.
constexpr double kPresentBudgetMs = 8.0;
constexpr int kPresentWarmup = 2, kPresentSamples = 8;
// A device lost this many times in one window (a driver in trouble): GDI.
constexpr int kMaxDeviceLosses = 3;

enum class Stretch { automatic, halftone, nearest };
enum class PresentWay { automatic, d2d, gdi };

// Knobs: the smoke tests' (test_hooks.h: compiled into LongAfterDark-test.scr
// only), and the diagnostic ones anyone chasing a problem on a real machine
// may set (AD_SCR_STRETCH, AD_SCR_PRESENT). None is set in normal use.
struct Hooks {
  long long exit_after_frames = 0;   // AD_SCR_TESTEXIT_AFTER_FRAMES: exit 0 once every host window showed N
  bool ignore_input = false;         // AD_SCR_TEST_IGNORE_INPUT: a stray mouse can't end a test
  long long rotate_ms = 0;           // AD_SCR_TEST_ROTATE_MS: rotation interval instead of DurationMin
  milliseconds stall{20000};         // AD_SCR_TEST_STALL_MS: no frame for this long after the first = hung
  milliseconds first_frame{90000};   // AD_SCR_TEST_FIRSTFRAME_MS: grace for the first frame (slow module init)
  long long display_off_ms = 0;      // AD_SCR_TEST_DISPLAY_OFF_MS: after 5 frames, act as if the display slept this long
  // AD_SCR_TEST_DISPLAY_ON: the console display is taken to be on whatever
  // Windows reports, so the /s tests run on a machine whose monitors sleep
  // (the saver would pause every host); DISPLAY_OFF_MS still simulates one.
  bool display_on = false;
  std::wstring input_script;         // AD_SCR_TEST_INPUT: synthetic input instead of the real kind (input_rules.h)
  // AD_SCR_TEST_CAPTURE=<dir>: each window writes what it shows at the
  // presented frames AD_SCR_TEST_CAPTURE_FRAMES=<k>,... lists (default 30).
  std::wstring capture_dir;
  std::vector<uint64_t> capture_frames;
  Stretch stretch = Stretch::automatic;         // AD_SCR_STRETCH=halftone|nearest (default: auto)
  PresentWay present = PresentWay::automatic;   // AD_SCR_PRESENT=d2d|gdi (default: auto)
};

Hooks read_hooks() {
  Hooks h;
#if AD_SCR_TEST_HOOKS
  h.exit_after_frames = env_int(L"AD_SCR_TESTEXIT_AFTER_FRAMES", 0);
  h.ignore_input = env_set(L"AD_SCR_TEST_IGNORE_INPUT");
  h.rotate_ms = env_int(L"AD_SCR_TEST_ROTATE_MS", 0);
  h.stall = milliseconds(std::max<long long>(500, env_int(L"AD_SCR_TEST_STALL_MS", 20000)));
  h.first_frame = milliseconds(std::max<long long>(500, env_int(L"AD_SCR_TEST_FIRSTFRAME_MS", 90000)));
  h.display_off_ms = env_int(L"AD_SCR_TEST_DISPLAY_OFF_MS", 0);
  h.display_on = env_set(L"AD_SCR_TEST_DISPLAY_ON");
  h.input_script = env_w(L"AD_SCR_TEST_INPUT");
  h.capture_dir = env_w(L"AD_SCR_TEST_CAPTURE");
  if (!h.capture_dir.empty()) {
    const std::wstring list = env_w(L"AD_SCR_TEST_CAPTURE_FRAMES");
    for (size_t p = 0; p < list.size();) {
      const size_t comma = list.find(L',', p);
      const long long k = _wtoi64(list.substr(p, comma == std::wstring::npos ? std::wstring::npos : comma - p).c_str());
      if (k > 0) h.capture_frames.push_back((uint64_t)k);
      if (comma == std::wstring::npos) break;
      p = comma + 1;
    }
    if (h.capture_frames.empty()) h.capture_frames.push_back(30);
  }
#endif
  const std::wstring st = env_w(L"AD_SCR_STRETCH");
  if (st == L"nearest" || st == L"coloroncolor") h.stretch = Stretch::nearest;
  else if (st == L"halftone") h.stretch = Stretch::halftone;
  const std::wstring pw = env_w(L"AD_SCR_PRESENT");
  if (pw == L"gdi") h.present = PresentWay::gdi;
  else if (pw == L"d2d" || pw == L"direct2d") h.present = PresentWay::d2d;
  return h;
}

struct Monitor {
  RECT rc;
  bool primary;
};

BOOL CALLBACK enum_monitor(HMONITOR m, HDC, LPRECT, LPARAM lp) {
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  if (GetMonitorInfoW(m, &mi)) {
    reinterpret_cast<std::vector<Monitor>*>(lp)->push_back({mi.rcMonitor, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0});
  }
  return TRUE;
}

// AD_SCR_TEST_MONITORS="x,y,w,h[,p];…|…" (test hook): report these monitors
// instead of the real ones (",p" marks the primary; without one, the first
// is). Each '|' starts the layout reported from the next topology change
// on, so the smoke tests can stage monitors coming and going on any machine.
std::vector<Monitor> test_monitors(size_t layout) {
#if !AD_SCR_TEST_HOOKS
  (void)layout;
  return {};
#else
  std::wstring spec = env_w(L"AD_SCR_TEST_MONITORS");
  std::vector<std::wstring> layouts;
  for (size_t p = 0; !spec.empty();) {
    size_t bar = spec.find(L'|', p);
    layouts.push_back(spec.substr(p, bar == std::wstring::npos ? std::wstring::npos : bar - p));
    if (bar == std::wstring::npos) break;
    p = bar + 1;
  }
  std::vector<Monitor> v;
  if (layouts.empty()) return v;
  const std::wstring& l = layouts[std::min(layout, layouts.size() - 1)];
  bool have_primary = false;
  for (size_t p = 0; p < l.size();) {
    size_t semi = l.find(L';', p);
    std::wstring m = l.substr(p, semi == std::wstring::npos ? std::wstring::npos : semi - p);
    p = semi == std::wstring::npos ? l.size() : semi + 1;
    long x = 0, y = 0, w = 0, h = 0;
    wchar_t flag[8] = {};
    int n = swscanf(m.c_str(), L" %ld , %ld , %ld , %ld , %7ls", &x, &y, &w, &h, flag);
    if (n < 4 || w <= 0 || h <= 0) continue;
    bool primary = n == 5 && flag[0] == L'p' && !have_primary;
    have_primary |= primary;
    v.push_back({{x, y, x + w, y + h}, primary});
  }
  if (!v.empty() && !have_primary) v.front().primary = true;
  return v;
#endif
}

// The monitors in /s order: the primary first. `layout` counts topology
// changes (for the test hook).
std::vector<Monitor> monitors(size_t layout = 0) {
  std::vector<Monitor> v = test_monitors(layout);
  if (v.empty()) EnumDisplayMonitors(nullptr, nullptr, enum_monitor, reinterpret_cast<LPARAM>(&v));
  if (v.empty()) v.push_back({{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)}, true});
  std::stable_partition(v.begin(), v.end(), [](const Monitor& m) { return m.primary; });
  return v;
}

// The foreground window's program, for the "deactivated" exit reason.
std::string foreground_exe() {
  HWND fg = GetForegroundWindow();
  if (!fg) return "none";
  DWORD pid = 0;
  GetWindowThreadProcessId(fg, &pid);
  if (pid == GetCurrentProcessId()) return "self";
  HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!p) return "pid" + std::to_string(pid);
  wchar_t buf[MAX_PATH * 2];
  DWORD n = (DWORD)std::size(buf);
  std::string name = "pid" + std::to_string(pid);
  if (QueryFullProcessImageNameW(p, 0, buf, &n)) {
    std::wstring full(buf, n);
    name = narrow(full.substr(full.find_last_of(L"\\/") + 1));
  }
  CloseHandle(p);
  return name;
}

class App;

class SaverWindow {
 public:
  SaverWindow(App& app, int index, bool runs_host, std::wstring message)
      : app_(app), index_(index), runs_host_(runs_host), message_(std::move(message)) {}

  bool create_fullscreen(const RECT& rc);
  bool create_preview(HWND parent);
  void start();
  // Builds the rotation and starts the first host; start() does it at once
  // unless the App still waits for the host's capabilities (then App does).
  void start_rotation();
  bool rotation_started() const { return rotation_ != nullptr; }
  // Monitor topology changes (/s only): where the window is and what it
  // shows; moving it onto a (possibly resized) monitor keeps its host; a
  // retired window loses its host and its HWND without ending the saver.
  ScreenSlot slot() const;
  void place(const RECT& rc);
  void retire();
  void resumed() { resumed_at_ = Clock::now(); }   // the display came back on
  void request_quit() { if (host_) host_->request_quit(); }
  // Stops the host (with the sound host's longer grace when it is that one,
  // sound.h). `wait` = false hands the (up to ~150 ms, or 400 ms) teardown
  // to a background thread so a rotation or respawn never stalls the UI.
  void kill_host(bool wait = true);
  // Its host plays sound (AUDIO.md §9: the primary monitor's window's alone).
  bool host_sound() const { return host_ && host_->sound(); }
  // No longer the primary monitor's window while its host plays: the module
  // starts again without sound (the new owner's next host has it).
  void drop_sound();
  LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);

  // ---- input (INTERACTION.md §4; only the owner's host gets input lines) ----
  // The desktop captures for this window's first host (§8): one for each
  // screen it may be given, three at most (App::capture_seeds); it gets the
  // one of its own, if any.
  struct Seed {
    ModuleScreen screen;
    std::wstring path;
  };
  void set_seeds(std::vector<Seed> seeds) { seeds_ = std::move(seeds); }
  // Queue an input line for the host: its number, or 0 when there is no host.
  uint64_t send_input(const std::string& line);
  // The host's status, from the last consistent read.
  OwnerStatus status();
  HostProcess* host() const { return host_.get(); }
  // The cursor (screen px) as a point on the host's emulated screen.
  POINT map_cursor(POINT screen) const;
  // The letterboxed frame, in screen coordinates.
  RECT frame_screen() const;
  int caps_sent = -1;              // the Caps Lock toggle the host last heard (ADCAPS, then CAPS lines)
  // The Num Lock toggle it last heard (ADNUMLOCK, then NUMLOCK lines); -1
  // when it was started without one (a host that answered without numlock=1).
  int numlock_sent = -1;
  int index() const { return index_; }

  HWND hwnd = nullptr;
  uint64_t presented = 0;
  bool runs_host() const { return runs_host_; }

 private:
  // The screen a host of module `m` gets in this window (null: a module not
  // known yet): module_screen's rule on its monitor, or /p's 320x240.
  ModuleScreen screen_for(const Module* m) const;
  void spawn();
  void watchdog();
  void rotate();
  void present_latest();
  void render(HDC dc);
  bool paint_frame(HDC dc, const Frame& f, bool force_bars);
  // Direct2D (present.h): false when this frame is GDI's to draw.
  bool use_d2d() const;
  bool present_d2d(const Frame& f);
  // The filter a frame gets drawn one way or the other (AD_SCR_STRETCH, the auto policy).
  Filter filter_for(bool d2d, bool upscale) const;
  void maybe_capture();   // AD_SCR_TEST_CAPTURE
  void draw_message(HDC dc, const std::wstring& text);
  // What the window says on black instead of a frame ("" for nothing); the
  // test hook exits with `test_exit` when it appears.
  void set_status(std::wstring text, int test_exit = kExitStartFailed);

  App& app_;
  int index_;
  bool runs_host_;
  std::wstring message_;
  HWND parent_ = nullptr;          // preview only
  RECT rc_{};                      // full screen: the monitor it covers
  bool retiring_ = false;          // being destroyed by a relayout, not by an exit
  double aspect_ = 4.0 / 3.0;
  std::unique_ptr<Rotation> rotation_;
  bool rotating_ = false;
  UINT rotate_interval_ms_ = 0;
  bool rotate_waiting_ = false;    // the owner's rotation waits for a game to end
  std::unique_ptr<HostProcess> host_;
  uint64_t generation_ = 0;
  ModuleScreen screen_{};          // the current host's emulated screen (its module's: module_screen)
  std::vector<Seed> seeds_;        // consumed by the first spawn
  AdwHostStatusV1 status_cache_{};
  bool status_valid_ = false;
  std::unique_ptr<Frame> current_;
  RectI last_fit_{-1, -1, -1, -1};
  int failures_ = 0;               // consecutive failed runs of the current module
  size_t dead_modules_ = 0;        // modules skipped in a row without one frame
  std::wstring status_;            // "could not be started", until a frame arrives
  bool respawn_pending_ = false;
  Clock::time_point respawn_at_{};
  Clock::time_point resumed_at_{};   // stall clocks restart here after a pause
  bool message_painted_ = false;
  bool halftone_upscale_ = true;   // Stretch::automatic's current choice for upscales
  int halftone_samples_ = 0;
  double halftone_ms_ = 0;
  std::unique_ptr<D2DPresenter> d2d_;
  bool d2d_off_ = false;           // Direct2D gave way to GDI for good (failed, or over budget)
  bool d2d_logged_ = false;
  bool last_d2d_ = false;          // how the frame on screen was drawn
  bool d2d_fresh_ = false;         // ...with a render target made for it (not a sample of the cost)
  int d2d_losses_ = 0;
  int d2d_samples_ = 0;
  double d2d_ms_ = 0;
  // Presentation cost, logged when a host is retired (AD_SCR_LOG).
  uint64_t stat_presented_ = 0, stat_dups_ = 0;
  double stat_ms_total_ = 0, stat_ms_max_ = 0;
};

// Real hosts re-send the previous frame when a GO arrives before the
// module's next draw is due (most modules draw at ~10 fps); those cost a
// memcmp here instead of a stretch.
bool same_frame(const Frame& a, const Frame& b) {
  if (a.width != b.width || a.height != b.height || a.bpp != b.bpp || a.bits.size() != b.bits.size()) return false;
  if (a.bpp == 8 && memcmp(a.palette.data(), b.palette.data(), sizeof(RGBQUAD) * 256) != 0) return false;
  return memcmp(a.bits.data(), b.bits.data(), a.bits.size()) == 0;
}

// A decision held for the owner's verdict (INTERACTION.md §4.3).
struct PendingHold {
  InputEvent ev;
  HoldSeqs seqs;
  const HostProcess* host = nullptr;   // the host the line went to; another one means it is gone
  std::string reason;
};

class App {
 public:
  App(const Args& a, HINSTANCE hi) : args(a), hinst(hi), preview(a.mode == Mode::preview) {}
  int run();
  // Never blocks: it can run inside a message another thread is waiting on
  // (a /p child's WM_DESTROY is sent by the control panel's DestroyWindow,
  // WM_ACTIVATEAPP by the app being activated). The waiting part of the
  // shutdown is teardown(), after the message loop.
  void request_exit(int code, const char* why = "");
  void teardown();
  void input_exit(const std::string& what);
  void on_presented();
  void set_display_on(bool on);
  void display_changed();
  // After the timer: re-plans the windows against the monitors now present.
  void relayout();
  bool on_thread_message(const MSG& msg);
  ScreenSlot slot_for(const Monitor& m) const;
  std::unique_ptr<SaverWindow> make_window(const Monitor& m);

  // ---- input (INTERACTION.md §4) ----
  // The input owner: the primary monitor's window (the first in /s order).
  SaverWindow* owner() const { return preview || windows.empty() ? nullptr : windows.front().get(); }
  OwnerStatus owner_status() const;
  void on_key(int vk, bool down, bool sys);
  void on_button(uint32_t bit, bool down);
  void on_wheel();
  void on_move();
  void on_deactivate();
  void on_session_away(const char* what);
  void check_caps();
  // Num Lock, as Caps Lock (INTERACTION.md §3.2), for a host that said
  // numlock=1 (HostCapabilities::takes_numlock_lines): a NUMLOCK line when
  // the toggle changed since the owner's host last heard it. It never ends
  // the saver (input_rules.h: exempt_key).
  void check_numlock();
  // Status changed (or might have): wake, play starting/ending, held decisions.
  void poll_status();
  // The Caps Lock and Num Lock toggles, the cursor and the buttons: real, or
  // the test script's synthetic ones.
  int caps_toggle() const;
  int numlock_toggle() const;
  POINT cursor_pos() const;
  bool scripted() const { return !script.empty() || script_loaded; }
  // Called by a window whose host is being replaced.
  void owner_host_gone(const HostProcess* h);
  // Whether the owner's rotation must wait (it plays and may not be switched).
  bool owner_playing_no_rotate() const;
  void on_cursor(HWND h);   // WM_SETCURSOR

  Args args;
  HINSTANCE hinst;
  bool preview;
  Hooks hooks;
  Settings settings;
  Catalog catalog;
  std::wstring win_dir, host_exe;
  std::vector<std::string> available;      // ids whose module file exists
  bool is_available(const std::string& id) const {
    return std::find(available.begin(), available.end(), id) != available.end();
  }
  // What the host can run (--capabilities, dialog_support.h). When the
  // rotation holds a module of another ABI than After Dark's (caps_gate),
  // Random leaves out a module this host can't run (its lane or its module
  // ABI isn't listed): a host too old for Star Wars Screen Entertainment's
  // Intermission modules would run them into errors, a black screen three
  // times over each pass. Its first hosts wait for the answer, kCapsWaitMs
  // at most; any other run never waits and only logs it. The answer comes
  // to this thread (WM_APP_CAPS, on_thread_message), whatever windows a
  // relayout has made or retired meanwhile.
  HostCapabilities host_caps;
  bool caps_gate = false;                  // the rotation waits for, and applies, the answer
  bool caps_waiting = false;               // ...which hasn't come yet
  UINT_PTR caps_wait_timer = 0;            // thread timer: the end of that wait
  // The first rotation built says in the logs what it holds (the others hold
  // the same); a relayout during the wait may retire window 0 before it builds one.
  bool rotation_logged = false;
  // Random may play it: always without the gate; with it, when the host
  // lists its lane and ABI (or never answered).
  bool may_rotate(const std::string& id) const {
    const Module* m = catalog.find(id);
    return !caps_gate || !m || host_caps.runs(m->lane, m->abi);
  }
  void on_capabilities(const HostCapabilities& caps);   // the probe answered
  void caps_wait_over();                               // ...or the wait ran out
  std::wstring message;
  int message_code = kExitOk;
  HANDLE job = nullptr;
  Pacer pacer;
  bool sound_forced_off = false;           // AD_SCR_SOUND=0 (sound.h)
  POINT cursor_start{};
  bool first_move_seen = false;
  bool display_on = true;
  bool test_display_cycled = false;
  HPOWERNOTIFY power_notify = nullptr;
  bool exiting = false;
  int exit_code = kExitOk;
  uint32_t seed = 0;
  std::vector<std::unique_ptr<SaverWindow>> windows;
  int next_window_index = 0;               // log/seed identity; never reused
  UINT_PTR relayout_timer = 0;             // thread timer (no window: windows come and go)
  int settle_moves = 0;                    // moves while it runs (none of them ends the saver)
  size_t topology_changes = 0;             // relayouts so far (AD_SCR_TEST_MONITORS layout)
  bool relayouting = false;                // our own window shuffle is not the user leaving
  std::vector<HANDLE> seed_files;          // delete-on-close desktop captures, open until we exit
  // Window index -> its captures, one per screen its first host may be
  // given (first host only).
  std::map<int, std::vector<SaverWindow::Seed>> seed_paths;

  // Input state.
  UINT_PTR caps_timer = 0, hold_timer = 0, script_timer = 0;
  std::vector<PendingHold> holds;
  POINT baseline{};                        // the move threshold's origin (re-set when play ends)
  bool was_interactive = false;
  bool cursor_visible = false;
  bool clip_active = false;
  uint32_t buttons = 0;                    // mouse buttons down (1 L, 2 R, 4 M)
  // AD_SCR_TEST_INPUT.
  std::vector<TestStep> script;
  bool script_loaded = false;
  size_t script_pos = 0;
  bool script_waiting = false;
  ULONGLONG script_wait_until = 0;
  uint64_t script_frames_target = 0;
  int synthetic_caps = 0;
  int synthetic_numlock = 0;
  POINT synthetic_cursor{};

 private:
  bool load();
  void capture_seeds(const std::vector<Monitor>& mons);
  void start_input();
  void evaluate(const InputEvent& ev, uint64_t n, long dx = 0, long dy = 0);
  void update_clip();
  void update_cursor(const OwnerStatus& st);
  void script_tick();
  void ensure_hold_timer();
};

LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    reinterpret_cast<SaverWindow*>(cs->lpCreateParams)->hwnd = hwnd;
  }
  auto* w = reinterpret_cast<SaverWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (msg == WM_NCDESTROY) {
    // The last message: after it the SaverWindow may be freed (a relayout
    // retires windows while the saver runs), so nothing may reach it.
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    return DefWindowProcW(hwnd, msg, wp, lp);
  }
  return w ? w->handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- SaverWindow ---------------------------------------------------------------

bool SaverWindow::create_fullscreen(const RECT& rc) {
  int w = rc.right - rc.left, h = rc.bottom - rc.top;
  rc_ = rc;
  if (w > 0 && h > 0) aspect_ = (double)w / h;
  // Topmost + tool window: above the taskbar, and no taskbar button.
  CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kClassName, L"Long After Dark", WS_POPUP, rc.left, rc.top, w, h,
                  nullptr, nullptr, app_.hinst, this);
  if (!hwnd) return false;
  // A lock (Win+L, Ctrl+Alt+Del > Lock, an idle-lock policy) or a session
  // switched away sends no WM_ACTIVATEAPP: without this the saver, a game in
  // play and its sound would run on behind the lock screen.
  WTSRegisterSessionNotification(hwnd, NOTIFY_FOR_THIS_SESSION);
  ShowWindow(hwnd, SW_SHOW);
  return true;
}

bool SaverWindow::create_preview(HWND parent) {
  parent_ = parent;
  // The Screen Saver Settings dialog is not per-monitor-v2 like us. Adopt the
  // parent's DPI awareness for this thread (it only ever serves this one
  // window) so the child lives in the parent's coordinate space and is
  // scaled with it. (In-process, Windows refuses mixed-awareness children
  // without opt-in; cross-process it currently allows them, so this is the
  // conservative choice rather than a hard requirement.)
  if (DPI_AWARENESS_CONTEXT ctx = GetWindowDpiAwarenessContext(parent)) SetThreadDpiAwarenessContext(ctx);
  RECT rc{};
  GetClientRect(parent, &rc);
  CreateWindowExW(0, kClassName, L"Long After Dark preview", WS_CHILD | WS_VISIBLE, 0, 0, rc.right, rc.bottom, parent,
                  nullptr, app_.hinst, this);
  return hwnd != nullptr;
}

void SaverWindow::start() {
  SetTimer(hwnd, kTimerWatchdog, 500, nullptr);
  if (!message_.empty()) {
    // Draw now rather than wait for WM_PAINT: a preview parent that isn't
    // visible yet never sends one.
    if (HDC dc = GetDC(hwnd)) {
      render(dc);
      ReleaseDC(hwnd, dc);
    }
  }
  if (!runs_host_) return;
  if (app_.caps_waiting) return;   // App::on_capabilities (or the end of the wait) starts it
  start_rotation();
}

void SaverWindow::start_rotation() {
  if (rotation_ || !runs_host_ || app_.exiting) return;
  std::vector<std::string> ids;
  const Settings& s = app_.settings;
  auto avail = [&](const std::string& id) { return app_.is_available(id); };
  std::string first;
  if (!s.is_random()) {
    if (avail(s.module)) first = s.module;
    else log_line("module %s unavailable; picking at random", s.module.c_str());
  }
  if (s.rotates()) {
    // Module=random, or a Randomize list: rotate through the list (a named
    // Module plays first), limited to the releases in Collections, with
    // byte-identical copies once per pass (COVERS.md §1.8). An empty or
    // all-stale list means every module. Random leaves out what this host
    // can't run (App::may_rotate): a list of only such modules means every
    // module it can run. A module chosen on its own is tried all the same,
    // and says why it can't start.
    const HostRotation r =
        rotation_for_host(s, app_.catalog, avail, [&](const std::string& id) { return app_.may_rotate(id); });
    const bool log_it = !app_.rotation_logged;
    app_.rotation_logged = true;
    if (r.plan.collections_ignored && log_it) {
      log_line("rotation: Collections ignored (nothing checked in them)");
      last_log("rotation: Collections ignored (nothing checked in them)");
    }
    ids = r.plan.ids;
    first = r.plan.lead;   // a named Module leads, unless this host can't run it
    // Nothing is left when the host can run none of the modules imported:
    // then nothing plays (below), rather than every one of them in turn into
    // errors, black and "could not be started" over and over.
    const bool none_runs = ids.empty() && r.left_out > 0;
    if (ids.empty() && !none_runs) ids = app_.available;
    if (log_it) {
      std::string f;
      for (const auto& id : effective_collections(s.collections, app_.catalog)) f += (f.empty() ? "" : ",") + id;
      log_line("rotation: %zu module(s), collections=%s", ids.size(), f.empty() ? "all" : f.c_str());
      if (r.left_out) last_log("rotation: left out %zu module(s) this host can't run", r.left_out);
    }
    rotating_ = true;
    if (none_runs) {
      // As the not-imported and host-missing messages do, this says what is
      // wrong: the settings dialog shows these modules "Coming soon".
      set_status(app_.preview ? L"No module can run on this host"
                              : L"None of the modules imported can run on this Long After Dark host (adhostwin.exe).",
                 kExitNoneRuns);
    }
  } else if (!first.empty()) {
    ids.push_back(first);
  } else {
    // The chosen module vanished (re-import, hand edit): show *something*.
    ids = app_.available;
  }
  rotation_ = std::make_unique<Rotation>(ids, app_.seed + (uint32_t)index_ * 7919u, first);
  if (rotation_->empty()) return;
  spawn();
  long long interval = app_.hooks.rotate_ms > 0 ? app_.hooks.rotate_ms : (long long)s.duration_min * 60000;
  if (rotating_ && rotation_->size() > 1 && interval > 0) {
    rotate_interval_ms_ = (UINT)std::min<long long>(interval, 0x7FFFFFFF);
    SetTimer(hwnd, kTimerRotate, rotate_interval_ms_, nullptr);
  }
}

ModuleScreen SaverWindow::screen_for(const Module* m) const {
  // /p: a thumbnail in someone else's window, 320x240 for every module. The
  // host renders an output that small through a guest display of at least
  // 640x480 (host/ne16 "Small screens"), so a 640x480 scene of a module's own
  // (an Intermission, Star Trek, ScreamSavers or Marvel module's) fills it
  // too, as it does a 4:3 monitor.
  if (app_.preview) return ModuleScreen{{320, 240}, true};
  // Its own screen when it has one (its catalog "screen", or its ABI's),
  // else the Resolution setting on this monitor.
  return module_screen(m ? own_screen(m->abi, m->screen) : SizeI{}, aspect_, app_.settings.scale);
}

ScreenSlot SaverWindow::slot() const {
  ScreenSlot s;
  s.rc = {(int)rc_.left, (int)rc_.top, (int)(rc_.right - rc_.left), (int)(rc_.bottom - rc_.top)};
  s.runs_host = runs_host_;
  s.message = !message_.empty();
  if (runs_host_) {
    // The screen its host renders; between two hosts (a respawn due), the one
    // its module's next host gets here; before its first (the rotation not
    // built yet), the monitor's, as an After Dark module's. A module with a
    // screen of its own (an Intermission, Star Trek, ScreamSavers or Marvel
    // module) keeps it on any monitor, so its window can move anywhere and
    // keep its host (plan_relayout).
    const ModuleScreen ms = host_ ? screen_ : screen_for(rotation_ ? app_.catalog.find(rotation_->current()) : nullptr);
    s.emu = ms.emu;
    s.fixed = ms.fixed;
  }
  return s;
}

void SaverWindow::place(const RECT& rc) {
  int w = rc.right - rc.left, h = rc.bottom - rc.top;
  // Re-asserted even for an unchanged rect: while a monitor was gone,
  // Windows may have moved us onto another one.
  SetWindowPos(hwnd, HWND_TOPMOST, rc.left, rc.top, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
  if (EqualRect(&rc, &rc_)) return;
  rc_ = rc;
  if (w > 0 && h > 0) aspect_ = (double)w / h;
  // New letterbox, and a new upscale factor for the HALFTONE budget to judge.
  last_fit_ = {-1, -1, -1, -1};
  halftone_upscale_ = true;
  halftone_samples_ = 0;
  halftone_ms_ = 0;
  InvalidateRect(hwnd, nullptr, FALSE);
}

void SaverWindow::retire() {
  retiring_ = true;
  kill_host(false);
  if (hwnd) DestroyWindow(hwnd);   // WM_DESTROY stops the timers
}

void SaverWindow::spawn() {
  const Module* m = app_.catalog.find(rotation_->current());
  if (!m) return;
  // The GDI path's HALFTONE cost depends on the module's frames too: each new
  // host is judged afresh (a cheap first module must not keep an expensive
  // later one on HALFTONE).
  halftone_upscale_ = true;
  halftone_samples_ = 0;
  halftone_ms_ = 0;
  // Each host its own module's screen (module_screen): a rotation from an
  // After Dark module to one with a screen of its own (an Intermission, Star
  // Trek, ScreamSavers or Marvel module), or back, gets a host of the new
  // size, and the letterbox follows its frames.
  const ModuleScreen screen = screen_for(m);
  const SizeI emu = screen.emu;
  std::map<int, int> cv;
  if (auto it = app_.settings.controls.find(m->id); it != app_.settings.controls.end()) {
    // The catalog is authoritative about which slots exist and their ranges;
    // buttons and unknown kinds never carry a value.
    for (auto [idx, val] : it->second) {
      if (const Control* c = m->control(idx); c && c->settable()) cv[idx] = c->clamp(val);
    }
  }
  std::string cvset = format_cvset(cv);
  // Modules latch the Caps Lock toggle when they start (INTERACTION.md §3.1);
  // every window's host gets it, owner or not. The Num Lock toggle too, for
  // a host that keeps one (numlock=1; dialog_support.h: numlock_env): Final
  // Exam begins its exam on a change of it, so the host must start with the
  // real one. Before the host's answer every host gets it (one that doesn't
  // know ADNUMLOCK ignores it).
  const int caps = app_.caps_toggle();
  const int numlock = app_.numlock_toggle();
  const std::pair<std::wstring, std::wstring> numlock_var = numlock_env(app_.host_caps, numlock != 0);
  // Sound (AUDIO.md §9): the primary monitor's window's host alone, in /s,
  // with Sound=1; every other host is told ADSOUND=0.
  const SoundChoice sound = sound_for(app_.settings, app_.preview ? HostRole::control_panel : HostRole::saver,
                                      this == app_.owner(), app_.sound_forced_off);

  // Only a window's first host starts on the desktop (§8), from the capture
  // taken at its screen; without one (it failed, or its screen was not among
  // the three planned, plan_seed_shots) it starts on black.
  std::wstring seed;
  for (const Seed& s : seeds_) {
    if (s.screen == screen) seed = s.path;
  }
  if (!seeds_.empty() && seed.empty()) {
    last_log("seed window=%d: none taken at %dx%d; the module starts on black", index_, emu.w, emu.h);
  }
  seeds_.clear();

  HostSpec spec;
  spec.exe = app_.host_exe;
  spec.module_path = resolve_module_path(app_.win_dir, m->path);
  spec.working_dir = app_.win_dir;
  spec.env = {
      {L"ADSTREAM", L"1"},
      {L"ADSCREENW", std::to_wstring(emu.w)},
      {L"ADSCREENH", std::to_wstring(emu.h)},
      {L"ADCVSET", widen(cvset)},      // empty removes a stale inherited value
      {L"AD_ASSETS_DIR", assets_root()},
      {L"ADCAPS", caps ? L"1" : L"0"},
      numlock_var,
      {L"ADSTATE", state_dir()},
      // An empty value removes a stale inherited one.
      {L"ADSEEDIMG", seed},
  };
  add_sound_env(spec.env, sound);
  const bool seeded = !seed.empty();
  spec.stderr_path = env_w(L"AD_SCR_HOSTLOG");
  spec.priority_class = app_.preview ? BELOW_NORMAL_PRIORITY_CLASS : 0;

  if (host_ && this == app_.owner()) app_.owner_host_gone(host_.get());
  ++generation_;
  host_ = std::make_unique<HostProcess>(HostProcess::Notify{hwnd, WM_APP_FRAME, WM_APP_HOSTEXIT, (WPARAM)generation_});
  // Previews are thumbnails: 30 fps is plenty and keeps the control panel light.
  host_->set_min_go_interval(app_.preview ? 30ms : 12ms);
  status_valid_ = false;
  screen_ = screen;
  caps_sent = caps;
  numlock_sent = numlock_var.second.empty() ? -1 : numlock;
  std::wstring err;
  if (!host_->start(spec, app_.job, &err)) {
    last_log("spawn-failed window=%d module=%s: %s", index_, m->id.c_str(), narrow(err).c_str());
    host_.reset();
    failures_++;
    respawn_pending_ = true;
    respawn_at_ = Clock::now() + std::min<milliseconds>(30s, 1s * failures_);
    return;
  }
  app_.pacer.add(host_.get());
  last_log("spawn window=%d gen=%llu module=%s path=%s size=%dx%d cvset=%s caps=%d numlock=%d seed=%d sound=%d volume=%d "
           "pid=%lu",
           index_, (unsigned long long)generation_, m->id.c_str(), narrow(spec.module_path).c_str(), emu.w, emu.h,
           cvset.c_str(), caps, numlock_sent, seeded ? 1 : 0, sound.on ? 1 : 0, sound.on ? sound.volume : -1, host_->pid());
}

void SaverWindow::kill_host(bool wait) {
  if (!host_) return;
  if (this == app_.owner()) app_.owner_host_gone(host_.get());
  if (stat_presented_) {
    log_line("stats window=%d gen=%llu received=%llu presented=%llu duplicates=%llu present_ms_avg=%.2f "
             "present_ms_max=%.2f present=%s upscale=%s", index_, (unsigned long long)generation_,
             (unsigned long long)host_->frames(), (unsigned long long)stat_presented_, (unsigned long long)stat_dups_,
             stat_ms_total_ / stat_presented_, stat_ms_max_, last_d2d_ ? "d2d" : "gdi",
             last_d2d_ ? (app_.hooks.stretch == Stretch::nearest ? "nearest" : "sharp")
                       : (halftone_upscale_ ? "halftone" : "coloroncolor"));
  }
  stat_presented_ = stat_dups_ = 0;
  stat_ms_total_ = stat_ms_max_ = 0;
  app_.pacer.remove(host_.get());
  if (host_->sound()) last_log("sound host stops window=%d gen=%llu", index_, (unsigned long long)generation_);
  if (wait) {
    host_->stop_gracefully();
    host_.reset();
  } else {
    // Its reader may still post to us; the generation bump below makes those
    // posts stale. The Job still covers it if we exit first.
    std::thread([h = std::move(host_)] { h->stop_gracefully(); }).detach();
  }
  status_valid_ = false;
  ++generation_;   // anything the old reader already posted is now stale
}

uint64_t SaverWindow::send_input(const std::string& line) {
  if (!host_ || host_->exited()) return 0;
  return host_->send_input(line);
}

OwnerStatus SaverWindow::status() {
  OwnerStatus st;
  if (!host_ || host_->exited()) return st;
  st.running = true;
  AdwHostStatusV1 rec{};
  if (host_->read_status(&rec)) {
    status_cache_ = rec;
    status_valid_ = true;
  }
  // A read that raced the writer four times keeps the previous copy (§3.4).
  st.have = status_valid_;
  st.rec = status_cache_;
  return st;
}

// Both by the current host's screen: a module's own 640x480 (an Intermission,
// Star Trek, ScreamSavers or Marvel module's) is pillarboxed on a widescreen
// monitor, and its clicks and moves land on it (Final Exam's mouse move ends
// its exam).
POINT SaverWindow::map_cursor(POINT screen) const {
  RectI fit = fit_rect(screen_.emu.w, screen_.emu.h, rc_.right - rc_.left, rc_.bottom - rc_.top);
  return map_to_frame(screen, rc_, fit, screen_.emu);
}

RECT SaverWindow::frame_screen() const {
  RectI fit = fit_rect(screen_.emu.w, screen_.emu.h, rc_.right - rc_.left, rc_.bottom - rc_.top);
  return frame_screen_rect(rc_, fit);
}

void SaverWindow::watchdog() {
  auto now = Clock::now();
  if (parent_) {
    if (!IsWindow(parent_)) {
      app_.request_exit(kExitOk, "preview parent gone");
      return;
    }
    RECT pr{}, cr{};
    GetClientRect(parent_, &pr);
    GetClientRect(hwnd, &cr);
    if (pr.right != cr.right || pr.bottom != cr.bottom) {
      SetWindowPos(hwnd, nullptr, 0, 0, pr.right, pr.bottom, SWP_NOZORDER | SWP_NOACTIVATE);
      InvalidateRect(hwnd, nullptr, FALSE);
    }
  }
  if (!runs_host_ || app_.exiting || !rotation_ || rotation_->empty()) return;
  if (!host_) {
    if (respawn_pending_ && now >= respawn_at_ && app_.display_on) {
      respawn_pending_ = false;
      last_log("respawn window=%d module=%s", index_, rotation_->current().c_str());
      spawn();
    }
    return;
  }
  // A paused host is silent by design; its stall clocks restart on resume.
  const bool paused = !app_.display_on;
  const char* why = nullptr;
  if (host_->exited()) why = "exit";
  else if (paused) return;
  else if (host_->frames() > 0 && now - std::max(host_->last_frame_at(), resumed_at_) > app_.hooks.stall) why = "stall";
  else if (host_->frames() == 0 && now - std::max(host_->started_at(), resumed_at_) > app_.hooks.first_frame) {
    why = "no-first-frame";
  }
  if (!why) return;

  uint64_t frames = host_->frames();
  auto lived = now - host_->started_at();
  DWORD code = host_->exit_code();
  last_log("host-%s window=%d gen=%llu frames=%llu lived_ms=%lld code=%ld", why, index_,
           (unsigned long long)generation_, (unsigned long long)frames,
           (long long)std::chrono::duration_cast<milliseconds>(lived).count(), (long)code);
  kill_host(false);
  failures_ = (frames > 0 && lived >= kHealthyRun) ? 0 : failures_ + 1;
  if (frames > 0) dead_modules_ = 0;
  if (failures_ >= 3 && frames == 0) {
    // Say so rather than sit on a black screen: most likely this module's
    // lane is not in this adhostwin yet (exit 3) or its files are damaged.
    const Module* m = app_.catalog.find(rotation_->current());
    std::wstring name = m ? widen(m->display_name) : widen(rotation_->current());
    std::wstring text = L"“" + name + L"” could not be started";
    if (code != STILL_ACTIVE) {
      // A crash ends the host with an NTSTATUS (0xC0000005 …), recognisable
      // only in hex; the host's own exit codes (§1: 1, 2, 3) read as decimal.
      wchar_t num[16];
      swprintf(num, 16, code >= 0x10000 ? L"0x%08lX" : L"%lu", (unsigned long)code);
      text += L" (host exit code " + std::wstring(num) + L")";
    }
    set_status(text + L".");
  }
  // A module that can't stay up is skipped when there is anything else to show.
  bool skipped = false;
  if (failures_ >= 3 && rotating_ && rotation_->size() > 1) {
    last_log("skip window=%d module=%s after %d failures", index_, rotation_->current().c_str(), failures_);
    if (frames == 0) ++dead_modules_;
    rotation_->next();
    failures_ = 0;
    skipped = true;
  }
  milliseconds delay = failures_ == 0 ? milliseconds(250)
                                      : std::min<milliseconds>(30s, 500ms * (1 << std::min(failures_ - 1, 6)));
  // Every module in the rotation failed in turn: stop churning through
  // process launches and retry at a relaxed pace.
  if (skipped && dead_modules_ >= rotation_->size()) delay = 30s;
  respawn_pending_ = true;
  respawn_at_ = now + delay;
}

void SaverWindow::drop_sound() {
  if (!host_sound() || app_.exiting) return;
  last_log("sound: window=%d is no longer the primary monitor's; its module starts again without sound", index_);
  kill_host(false);
  failures_ = 0;
  respawn_pending_ = false;
  if (runs_host_ && rotation_ && !rotation_->empty()) spawn();
}

void SaverWindow::rotate() {
  if (!rotation_ || rotation_->size() < 2 || app_.exiting || !app_.display_on) return;
  // A game in progress on the input owner is not switched away (AFTERDAR.SCR
  // 0x40190b): the switch waits, re-checked every second, unless the module
  // says it may be rotated (INTERACTION.md §4.2). Other windows rotate on time.
  if (this == app_.owner() && app_.owner_playing_no_rotate()) {
    if (!rotate_waiting_) {
      rotate_waiting_ = true;
      last_log("rotate-wait window=%d: the module is interactive", index_);
    }
    SetTimer(hwnd, kTimerRotateRetry, kRotateRetryMs, nullptr);
    return;
  }
  KillTimer(hwnd, kTimerRotateRetry);
  if (rotate_waiting_) {
    rotate_waiting_ = false;
    // A full interval for the next module, counted from now.
    if (rotate_interval_ms_) SetTimer(hwnd, kTimerRotate, rotate_interval_ms_, nullptr);
  }
  const std::string& next = rotation_->next();
  last_log("rotate window=%d -> %s", index_, next.c_str());
  kill_host(false);
  // Blank between modules, as the original randomizer did.
  if (current_) current_.reset();
  InvalidateRect(hwnd, nullptr, FALSE);
  failures_ = 0;
  respawn_pending_ = false;
  spawn();
}

void SaverWindow::present_latest() {
  // Once exiting, leave the frame untaken: that also keeps the reader from
  // posting more WM_APP_FRAMEs ahead of the pending WM_QUIT.
  if (!host_ || app_.exiting) return;
  auto f = host_->take_frame();
  if (!f) return;
  if (current_ && same_frame(*current_, *f)) {
    host_->recycle(std::move(f));
    ++stat_dups_;
    ++presented;   // it *is* what's on screen
    maybe_capture();
    app_.on_presented();
    return;
  }
  auto t0 = Clock::now();
  bool halftone_up = false;
  const bool via_d2d = present_d2d(*f);
  if (!via_d2d) {
    if (HDC dc = GetDC(hwnd)) {
      halftone_up = paint_frame(dc, *f, false);
      ReleaseDC(hwnd, dc);
    }
  }
  last_d2d_ = via_d2d;
  double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
  ++stat_presented_;
  stat_ms_total_ += ms;
  stat_ms_max_ = std::max(stat_ms_max_, ms);
  if (via_d2d && !d2d_fresh_ && app_.hooks.stretch == Stretch::automatic &&
      app_.hooks.present == PresentWay::automatic && d2d_samples_ < kPresentWarmup + kPresentSamples) {
    // Direct2D on a machine without a usable GPU (its software rasterizer)
    // can cost more than GDI: it gives way when it is over budget.
    if (++d2d_samples_ > kPresentWarmup) d2d_ms_ += ms;
    if (d2d_samples_ == kPresentWarmup + kPresentSamples && d2d_ms_ / kPresentSamples > kPresentBudgetMs) {
      log_line("present window=%d direct2d %.1f ms/frame over budget -> gdi", index_, d2d_ms_ / kPresentSamples);
      d2d_off_ = true;
      d2d_.reset();
      last_fit_ = {-1, -1, -1, -1};
    }
  }
  if (halftone_up && app_.hooks.stretch == Stretch::automatic && halftone_samples_ < kPresentWarmup + kPresentSamples) {
    if (++halftone_samples_ > kPresentWarmup) halftone_ms_ += ms;
    if (halftone_samples_ == kPresentWarmup + kPresentSamples && halftone_ms_ / kPresentSamples > kPresentBudgetMs) {
      halftone_upscale_ = false;
      log_line("stretch window=%d halftone upscale %.1f ms/frame over budget -> coloroncolor", index_,
               halftone_ms_ / kPresentSamples);
    }
  }
  if (current_) host_->recycle(std::move(current_));
  current_ = std::move(f);
  ++presented;
  dead_modules_ = 0;
  if (!status_.empty()) set_status({});
  maybe_capture();
  app_.on_presented();
}

bool SaverWindow::use_d2d() const {
  if (d2d_off_ || !hwnd || !message_.empty() || !status_.empty()) return false;
  // /p is a small downscale in someone else's window: GDI, unless asked.
  if (app_.hooks.present == PresentWay::gdi) return false;
  return !app_.preview || app_.hooks.present == PresentWay::d2d;
}

Filter SaverWindow::filter_for(bool d2d, bool upscale) const {
  if (app_.hooks.stretch == Stretch::nearest) return Filter::nearest;
  if (app_.hooks.stretch == Stretch::halftone || d2d) return Filter::smooth;
  return !upscale || halftone_upscale_ ? Filter::smooth : Filter::nearest;
}

bool SaverWindow::present_d2d(const Frame& f) {
  if (!use_d2d()) return false;
  if (!d2d_) d2d_ = std::make_unique<D2DPresenter>();
  d2d_fresh_ = !d2d_->ready();
  RECT cr{};
  GetClientRect(hwnd, &cr);
  const RectI fit = fit_rect(f.width, f.height, cr.right, cr.bottom);
  std::string err;
  bool lost = false;
  if (d2d_->present(hwnd, f, fit, filter_for(true, true), &err, &lost)) {
    if (!d2d_logged_) {
      d2d_logged_ = true;
      log_line("present window=%d: direct2d, %s, %ldx%ld", index_,
               app_.hooks.stretch == Stretch::nearest ? "nearest neighbour" : d2d_->smooth_name(), cr.right, cr.bottom);
    }
    last_fit_ = {-1, -1, -1, -1};   // GDI, if it takes over, draws its bars afresh
    return true;
  }
  if (lost && ++d2d_losses_ <= kMaxDeviceLosses) {
    // The next frame makes a new device; this one is GDI's.
    log_line("present window=%d: direct2d %s; a new device next frame", index_, err.c_str());
    return false;
  }
  log_line("present window=%d: direct2d failed (%s) -> gdi", index_, err.c_str());
  d2d_off_ = true;
  d2d_.reset();
  return false;
}

// AD_SCR_TEST_CAPTURE: what the window shows at the presented frames listed,
// drawn off screen the way it was drawn on it (present.h), and the host's
// frame as it came.
void SaverWindow::maybe_capture() {
  const Hooks& h = app_.hooks;
  if (h.capture_dir.empty() || !current_ || !hwnd) return;
  if (std::find(h.capture_frames.begin(), h.capture_frames.end(), presented) == h.capture_frames.end()) return;
  RECT cr{};
  GetClientRect(hwnd, &cr);
  const Frame& f = *current_;
  const RectI fit = fit_rect(f.width, f.height, cr.right, cr.bottom);
  const bool upscale = fit.w > f.width || fit.h > f.height;
  const Filter filter = filter_for(last_d2d_, upscale);
  const std::wstring base = join_path(h.capture_dir, L"window" + std::to_wstring(index_) + L"-frame" +
                                                         std::to_wstring(presented));
  std::vector<uint8_t> bgr;
  std::string err;
  bool ok = render_frame_bgr(f, cr.right, cr.bottom, fit, last_d2d_, filter, bgr, &err) &&
            adw::ui::save_png_bgr(base + L".png", cr.right, cr.bottom, bgr, &err);
  ok = ok && render_frame_bgr(f, f.width, f.height, RectI{0, 0, f.width, f.height}, false, Filter::nearest, bgr, &err) &&
       adw::ui::save_png_bgr(base + L"-host.png", f.width, f.height, bgr, &err);
  log_line("capture window=%d frame=%llu %s %ldx%ld host=%dx%d present=%s filter=%s%s%s", index_,
           (unsigned long long)presented, ok ? "ok" : "failed", cr.right, cr.bottom, f.width, f.height,
           last_d2d_ ? "d2d" : "gdi", filter == Filter::nearest ? "nearest" : "smooth", ok ? "" : ": ", err.c_str());
}

void SaverWindow::set_status(std::wstring text, int test_exit) {
  if (status_ == text) return;
  status_ = std::move(text);
  if (!status_.empty()) {
    last_log("status window=%d: %s", index_, narrow(status_).c_str());
    current_.reset();   // the message goes on black, not over a stale frame
    if (d2d_) d2d_->release();   // GDI draws it: the window is GDI's again
    if (app_.hooks.exit_after_frames > 0) PostMessageW(hwnd, WM_APP_TESTEXIT, (WPARAM)test_exit, 0);
  }
  InvalidateRect(hwnd, nullptr, FALSE);
}

// Returns true when this was an upscale drawn with HALFTONE (what the auto
// policy measures).
bool SaverWindow::paint_frame(HDC dc, const Frame& f, bool force_bars) {
  RECT cr{};
  GetClientRect(hwnd, &cr);
  RectI r = fit_rect(f.width, f.height, cr.right, cr.bottom);
  if (force_bars || !(r == last_fit_)) {
    HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RECT bars[4] = {{0, 0, cr.right, r.y},
                    {0, r.y + r.h, cr.right, cr.bottom},
                    {0, r.y, r.x, r.y + r.h},
                    {r.x + r.w, r.y, cr.right, r.y + r.h}};
    for (auto& b : bars) if (b.right > b.left && b.bottom > b.top) FillRect(dc, &b, black);
    last_fit_ = r;
  }
  const bool upscale = r.w > f.width || r.h > f.height;
  const Filter filter = filter_for(false, upscale);
  stretch_frame_gdi(dc, f, r, filter);
  return filter == Filter::smooth && upscale;
}

// Everything a repaint shows: the last frame (with its bars) or black, and
// the message if there is one.
void SaverWindow::render(HDC dc) {
  if (current_) {
    paint_frame(dc, *current_, true);
  } else {
    RECT cr{};
    GetClientRect(hwnd, &cr);
    FillRect(dc, &cr, (HBRUSH)GetStockObject(BLACK_BRUSH));
  }
  if (message_.empty()) {
    if (!status_.empty()) draw_message(dc, status_);
    return;
  }
  draw_message(dc, message_);
  if (!message_painted_) {
    message_painted_ = true;
    last_log("message shown window=%d code=%d", index_, app_.message_code);
    if (app_.hooks.exit_after_frames > 0) PostMessageW(hwnd, WM_APP_TESTEXIT, app_.message_code, 0);
  }
}

void SaverWindow::draw_message(HDC dc, const std::wstring& text) {
  RECT cr{};
  GetClientRect(hwnd, &cr);
  int px = std::max(11, (int)(cr.bottom / (app_.preview ? 9 : 28)));
  HFONT font = CreateFontW(-px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
  HGDIOBJ old = SelectObject(dc, font);
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, RGB(200, 200, 200));
  RECT box{0, 0, std::max(1L, cr.right * 8 / 10), 0};
  const UINT fmt = DT_CENTER | DT_WORDBREAK | DT_NOPREFIX;
  DrawTextW(dc, text.c_str(), -1, &box, fmt | DT_CALCRECT);
  int bw = box.right - box.left, bh = box.bottom - box.top;
  RECT at{(cr.right - bw) / 2, (cr.bottom - bh) / 2, 0, 0};
  at.right = at.left + bw;
  at.bottom = at.top + bh;
  DrawTextW(dc, text.c_str(), -1, &at, fmt);
  SelectObject(dc, old);
  DeleteObject(font);
}

LRESULT SaverWindow::handle(UINT msg, WPARAM wp, LPARAM lp) {
  if (!app_.preview) {
    // Real input is ignored while a test script drives the rules.
    const bool real_input = !app_.scripted();
    switch (msg) {
      case WM_SETCURSOR:
        app_.on_cursor(hwnd);
        return TRUE;
      case WM_MOUSEACTIVATE:
        // Only the input owner takes the keyboard (INTERACTION.md §4.2).
        if (this != app_.owner()) return MA_NOACTIVATE;
        break;
      case WM_MOUSEMOVE:
        // Windows sends one WM_MOUSEMOVE when a window appears under the
        // cursor; that one is not the user.
        if (!app_.first_move_seen) {
          app_.first_move_seen = true;
          return 0;
        }
        if (real_input) app_.on_move();
        return 0;
      case WM_KEYDOWN:
      case WM_KEYUP:
      case WM_SYSKEYDOWN:
      case WM_SYSKEYUP:
        if (real_input)
          app_.on_key((int)wp, msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN, msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP);
        return 0;
      case WM_LBUTTONDOWN:
      case WM_LBUTTONUP:
        if (real_input) app_.on_button(1, msg == WM_LBUTTONDOWN);
        return 0;
      case WM_RBUTTONDOWN:
      case WM_RBUTTONUP:
        if (real_input) app_.on_button(2, msg == WM_RBUTTONDOWN);
        return 0;
      case WM_MBUTTONDOWN:
      case WM_MBUTTONUP:
        if (real_input) app_.on_button(4, msg == WM_MBUTTONDOWN);
        return 0;
      case WM_XBUTTONDOWN:
      case WM_XBUTTONUP:
        if (real_input) app_.on_button(0, msg == WM_XBUTTONDOWN);
        return TRUE;
      case WM_MOUSEWHEEL:
      case WM_MOUSEHWHEEL:
        if (real_input) app_.on_wheel();
        return 0;
      case WM_ACTIVATEAPP:
        if (!wp && !app_.relayouting && real_input) app_.on_deactivate();
        break;
      case WM_WTSSESSION_CHANGE:
        if (real_input && (wp == WTS_SESSION_LOCK || wp == WTS_CONSOLE_DISCONNECT || wp == WTS_REMOTE_DISCONNECT))
          app_.on_session_away(wp == WTS_SESSION_LOCK ? "session locked" : "session disconnected");
        return 0;
      case WM_DISPLAYCHANGE:
        app_.display_changed();
        return 0;
      case WM_SYSCOMMAND:
        // Already the screen saver. (SC_MONITORPOWER passes through, as in
        // scrnsave.lib: the display may still power down; see WM_POWERBROADCAST.)
        if ((wp & 0xFFF0) == SC_SCREENSAVE) return 0;
        break;
      case WM_POWERBROADCAST:
        if (wp == PBT_POWERSETTINGCHANGE) {
          auto* ps = reinterpret_cast<const POWERBROADCAST_SETTING*>(lp);
          if (ps && ps->PowerSetting == kConsoleDisplayState && ps->DataLength >= sizeof(DWORD)) {
            DWORD state = 0;
            memcpy(&state, ps->Data, sizeof(state));
#if AD_SCR_TEST_HOOKS
            if (app_.hooks.display_on && state == 0) {
              log_line("display state 0 taken as on (AD_SCR_TEST_DISPLAY_ON)");
              state = 1;
            }
#endif
            app_.set_display_on(state != 0);   // dimmed still shows us
          }
          return TRUE;
        }
        break;
      case WM_DPICHANGED:
        return 0;   // stay exactly on our monitor; never take the suggested rect
    }
  }
  switch (msg) {
    case WM_ERASEBKGND:
      return 1;   // WM_PAINT covers every pixel; erasing first would flicker
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hwnd, &ps);
      // Direct2D draws the frame, or the black between modules, while it
      // has the window; GDI draws the rest (messages, the very first paint).
      const bool d2d = current_ ? present_d2d(*current_) : use_d2d() && d2d_ && d2d_->clear(hwnd);
      if (!d2d) render(dc);
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_APP_FRAME:
      if (wp == (WPARAM)generation_) {
        present_latest();
        // The owner's status is read as each of its frames arrives: the host
        // publishes a step's record before writing its frame.
        if (this == app_.owner()) app_.poll_status();
      }
      return 0;
    case WM_APP_HOSTEXIT:
      if (wp == (WPARAM)generation_) {
        if (this == app_.owner()) app_.poll_status();   // a held decision ends at once
        watchdog();
      }
      return 0;
    case WM_APP_TESTEXIT:
      app_.request_exit((int)wp, "test hook");
      return 0;
    case WM_TIMER:
      if (wp == kTimerWatchdog) {
        watchdog();
      } else if (wp == kTimerRotate) {
        rotate();
      } else if (wp == kTimerRotateRetry) {
        rotate();
      } else if (wp == kTimerTestDisplay) {
        KillTimer(hwnd, kTimerTestDisplay);
        app_.set_display_on(true);
      }
      return 0;
    case WM_CLOSE:
      app_.request_exit(kExitOk, "WM_CLOSE");
      return 0;
    case WM_DESTROY:
      if (!parent_) WTSUnRegisterSessionNotification(hwnd);
      KillTimer(hwnd, kTimerWatchdog);
      KillTimer(hwnd, kTimerRotate);
      KillTimer(hwnd, kTimerRotateRetry);
      KillTimer(hwnd, kTimerTestDisplay);
      hwnd = nullptr;
      // A preview dies with the control panel's window; that ends us too.
      // A monitor that went away only ends its own window.
      if (!app_.exiting && !retiring_) app_.request_exit(kExitOk, "window destroyed");
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- App -----------------------------------------------------------------------

bool App::load() {
  load_settings(settings_path(), settings);   // absent file = defaults
  // The settings dialog's Preview runs us from a throwaway copy; it has
  // served its purpose once read, so it never outlives the preview (even when
  // the dialog closes first). Only a file named as the dialog names them is
  // ever deleted, whatever the environment says.
  if (env_set(kPreviewSettingsEnv)) {
    std::wstring path = settings_path();
    if (parse_preview_settings_name(path.substr(path.find_last_of(L"\\/") + 1)) && DeleteFileW(path.c_str())) {
      log_line("settings: removed the temporary %s", narrow(path).c_str());
    }
  }
  host_exe = host_exe_path();
  win_dir = win_assets_dir();
  std::string err;
  if (!load_catalog(catalog_path(), catalog, &err)) {
    log_line("catalog: %s", err.c_str());
  } else {
    for (const auto& m : catalog.modules) {
      if (file_exists(resolve_module_path(win_dir, m.path))) available.push_back(m.id);
    }
  }
  if (available.empty()) {
    // Any release will do (the eleven of After Dark modules, Star Wars Screen Entertainment).
    message = preview ? L"No modules imported" : L"No modules imported — open Screen Saver Settings…";
    message_code = kExitNotImported;
  } else if (!file_exists(host_exe)) {
    message = preview ? L"Long After Dark host missing"
                      : L"The Long After Dark host (adhostwin.exe) is missing — it belongs next to LongAfterDark.scr.";
    message_code = kExitHostMissing;
  }
  log_line("mode=%s catalog=%s modules=%zu available=%zu host=%s module=%s", preview ? "preview" : "run",
           narrow(catalog_path()).c_str(), catalog.modules.size(), available.size(), narrow(host_exe).c_str(),
           settings.module.c_str());
  if (!hooks.input_script.empty()) {
    std::string text, perr;
    if (!read_file(hooks.input_script, text) || !parse_test_script(text, script, &perr)) {
      log_line("test input: cannot use %s: %s", narrow(hooks.input_script).c_str(), perr.c_str());
    }
    script_loaded = true;   // even a broken script means "no real input"
  }
  return true;
}

// INTERACTION.md §8: each monitor as it is now, before any saver window
// covers it, at the emulated size of the window that will cover it: its
// first host's, which depends on that host's module (module_screen). The
// first module isn't known yet (a rotation's is drawn when it is built, after
// the host's answer when Random waits for it), so a window that may start
// with an After Dark module or one with a screen of its own (an Intermission,
// Star Trek, ScreamSavers or Marvel module) gets both captures: the whole
// monitor at its After Dark size, as ever, and the part that module's frame
// covers (640x480 of its own: seed_source). Its first host takes its own.
// Three at most (plan_seed_shots: a catalog may give every module a screen
// of its own); a first host whose screen has none starts on black
// (SaverWindow::spawn).
void App::capture_seeds(const std::vector<Monitor>& mons) {
  if (preview || !message.empty()) return;
  if (!settings.start_from_desktop) {
    last_log("seed: off (StartFromDesktop=0)");
    return;
  }
  const std::set<SizeI> owns =
      first_module_screens(settings, catalog, [this](const std::string& id) { return is_available(id); });
  int index = next_window_index;
  for (const Monitor& m : mons) {
    ScreenSlot s = slot_for(m);
    const int idx = index++;
    if (!s.runs_host) continue;
    const int mw = m.rc.right - m.rc.left, mh = m.rc.bottom - m.rc.top;
    std::vector<ModuleScreen> screens;
    for (const SizeI& own : owns) screens.push_back(module_screen(own, (double)mw / std::max(1, mh), settings.scale));
    // One BitBlt of the monitor for all of them, each picture its part
    // shrunk on its own and written before the next is made (one in memory
    // at a time), three at most (plan_seed_shots: After Dark's first, today's
    // file name, then 640x480 and the smallest of the other sizes of their
    // own); one the same as a picture before it, as on a 4:3 monitor at 480
    // lines, is that one's file, and no picture of its own (a shot without a
    // size).
    size_t left_out = 0;
    const std::vector<SeedShotPlan> plan = plan_seed_shots(screens, mw, mh, &left_out);
    std::vector<SeedShot> shots;
    for (const SeedShotPlan& p : plan) shots.push_back(p.same < 0 ? SeedShot{p.src, p.screen.emu} : SeedShot{});
    auto t0 = Clock::now();
    std::vector<std::wstring> written(plan.size());
    capture_monitor_shots(m.rc, shots, [&](size_t i, const std::vector<uint8_t>& p6) {
      const ModuleScreen& ms = plan[i].screen;
      const RectI& src = plan[i].src;
      if (const int same = plan[i].same; same >= 0) {
        if (written[same].empty()) return;   // that one failed: logged there
        written[i] = written[same];
        seed_paths[idx].push_back({ms, written[i]});
        last_log("seed window=%d %dx%d%s: the same picture as above", idx, ms.emu.w, ms.emu.h,
                 ms.fixed ? " (the frame's part of the monitor)" : "");
        return;
      }
      std::wstring path = seed_file_path(GetCurrentProcessId(), idx,
                                         ms.fixed ? std::to_wstring(ms.emu.w) + L"x" + std::to_wstring(ms.emu.h) : L""),
                   err;
      HANDLE h = write_seed_file(path, p6, &err);
      if (h == INVALID_HANDLE_VALUE) {
        last_log("seed window=%d: %s", idx, narrow(err).c_str());
        return;
      }
      seed_files.push_back(h);
      seed_paths[idx].push_back({ms, path});
      written[i] = path;
      // The first picture's time includes the monitor's capture.
      last_log("seed window=%d %dx%d from %dx%d%s in %lld ms", idx, ms.emu.w, ms.emu.h, src.w, src.h,
               ms.fixed ? " (the frame's part of the monitor)" : "",
               (long long)std::chrono::duration_cast<milliseconds>(Clock::now() - t0).count());
      t0 = Clock::now();
    });
    if (left_out > 0) {
      last_log("seed window=%d: none taken at %zu more screens of modules' own (%zu at most: 640x480, then the "
               "smallest); a first host at one starts on black", idx, left_out, kMaxOwnSeedShots);
    }
  }
}

int App::run() {
  hooks = read_hooks();
  sound_forced_off = adw::scr::sound_forced_off();
  load();
  seed = (uint32_t)GetTickCount64() ^ (GetCurrentProcessId() << 16);

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = wndproc;
  wc.hInstance = hinst;
  wc.hIcon = LoadIconW(hinst, MAKEINTRESOURCEW(100));
  wc.hCursor = preview ? LoadCursorW(nullptr, IDC_ARROW) : nullptr;
  wc.lpszClassName = kClassName;
  RegisterClassExW(&wc);

  job = create_kill_on_close_job();
  pacer.start();

  bool ok_host = message.empty();
  // Only a rotation holding a module of another ABI than After Dark's waits
  // for the host's answer (App::caps_gate), whatever the mode.
  caps_gate = ok_host && rotation_needs_capabilities(settings, catalog,
                                                     [this](const std::string& id) { return is_available(id); });
  if (preview) {
    HWND parent = reinterpret_cast<HWND>(args.hwnd);
    if (!IsWindow(parent)) return kExitBadArgs;
    auto w = std::make_unique<SaverWindow>(*this, 0, ok_host, message);
    if (!w->create_preview(parent)) return kExitBadArgs;
    windows.push_back(std::move(w));
  } else {
    // The last-exit log (INTERACTION.md §9.1): always on, one per /s run.
    last_log_open(last_exit_log_path());
    std::vector<Monitor> mons = monitors();
    last_log("start /s build=%s %s monitors=%zu module=%s rotates=%d available=%zu/%zu host=%s settings=%s state=%s%s",
             __DATE__, __TIME__, mons.size(), settings.module.c_str(), settings.rotates() ? 1 : 0, available.size(),
             catalog.modules.size(), narrow(host_exe).c_str(), narrow(settings_path()).c_str(),
             narrow(state_dir()).c_str(), scripted() ? " input=script" : "");
    if (sound_forced_off) last_log("sound: off (AD_SCR_SOUND=0)");
    else if (!settings.sound) last_log("sound: off (Sound=0)");
    else last_log("sound: primary monitor, volume %d%s", std::clamp(settings.volume, 0, 100),
                  iequals(settings.sound_monitor, "primary")
                      ? ""
                      : (" (SoundMonitor=" + settings.sound_monitor + " is taken as primary)").c_str());
    for (size_t i = 0; i < mons.size(); ++i) {
      const RECT& r = mons[i].rc;
      last_log("monitor %zu: %ld,%ld %ldx%ld%s", i, r.left, r.top, r.right - r.left, r.bottom - r.top,
               mons[i].primary ? " primary" : "");
    }
    // Before any window appears: what the desktop looks like now.
    capture_seeds(mons);
    for (const Monitor& m : mons) {
      auto w = make_window(m);
      if (auto it = seed_paths.find(w->index()); it != seed_paths.end()) w->set_seeds(it->second);
      if (w->create_fullscreen(m.rc)) windows.push_back(std::move(w));
    }
    if (windows.empty()) return kExitBadArgs;
    // Stop emulating while the display is off (the power manager turns it
    // off on its own timer while we run). Registration reports the current
    // state at once.
    power_notify = RegisterPowerSettingNotification(windows.front()->hwnd, &kConsoleDisplayState,
                                                    DEVICE_NOTIFY_WINDOW_HANDLE);
    GetCursorPos(&cursor_start);
    ShowCursor(FALSE);
    SetForegroundWindow(windows.front()->hwnd);
    start_input();
  }
  // What the host can do: for /s's last-exit log, and for what Random may
  // play when the rotation needs it (caps_gate: then, /p as well, the first
  // hosts wait for the answer, kCapsWaitMs at most). Otherwise it never
  // delays the first frame. The answer comes to this thread (WM_APP_CAPS):
  // the windows may be gone or new by then.
  if (message.empty() && (!preview || caps_gate)) {
    const DWORD to = GetCurrentThreadId();
    std::wstring exe = host_exe;
    std::thread([to, exe] {
      auto* caps = new HostCapabilities(probe_capabilities(exe, 5000));
      if (!PostThreadMessageW(to, WM_APP_CAPS, 0, reinterpret_cast<LPARAM>(caps))) delete caps;
    }).detach();
  }
  if (caps_gate) {
    caps_wait_timer = SetTimer(nullptr, 0, kCapsWaitMs, nullptr);
    caps_waiting = caps_wait_timer != 0;   // no timer, no wait: nothing could end it for sure
    log_line("rotation: waiting for the host's capabilities (a module of another ABI)");
  }
  for (auto& w : windows) w->start();

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (!msg.hwnd && on_thread_message(msg)) continue;
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  request_exit(exit_code);   // no-op unless the loop ended some other way
  teardown();
  if (!preview && !cursor_visible) ShowCursor(TRUE);
  return exit_code;
}

void App::request_exit(int code, const char* why) {
  if (exiting) return;
  exiting = true;
  exit_code = code;
  if (preview) log_line("exit code=%d%s%s", code, *why ? " reason=" : "", why);
  else last_log("exit code=%d%s%s", code, *why ? " reason=" : "", why);
  if (power_notify) {
    UnregisterPowerSettingNotification(power_notify);
    power_notify = nullptr;
  }
  if (clip_active) {
    ClipCursor(nullptr);
    clip_active = false;
    log_line("clip released (exit)");
  }
  for (UINT_PTR* t : {&caps_timer, &hold_timer, &script_timer, &caps_wait_timer}) {
    if (*t) KillTimer(nullptr, *t);
    *t = 0;
  }
  // The sound host hears QUIT before anything else happens (AUDIO.md §9),
  // so its audio stops while the windows go; teardown() gives it the longer
  // grace to silence its device and send MIDI all-notes-off.
  for (auto& w : windows) {
    if (!w->host_sound()) continue;
    w->request_quit();
    if (!preview) last_log("wake: QUIT to the sound host window=%d first", w->index());
  }
  // Give the desktop back first; tearing hosts down can take a moment.
  for (auto& w : windows) if (w->hwnd && !preview) ShowWindow(w->hwnd, SW_HIDE);
  for (auto& w : windows) w->request_quit();   // all hosts start quitting in parallel
  PostQuitMessage(code);
}

// The host's --capabilities answer (known or not): logged, kept for what
// Random may play (may_rotate) and whether Num Lock reaches the owner's host
// (check_numlock: a change made while it was awaited goes out now), and the
// end of the first hosts' wait.
void App::on_capabilities(const HostCapabilities& caps) {
  host_caps = caps;
  last_log("host capabilities: %s", caps.known ? caps.line.c_str() : "(no answer)");
  check_numlock();
  caps_wait_over();
}

// The first hosts start now: the host answered, or kCapsWaitMs ran out (the
// rotation then keeps every module, as with a host too old to answer).
void App::caps_wait_over() {
  if (!caps_waiting) return;
  caps_waiting = false;
  if (caps_wait_timer) KillTimer(nullptr, caps_wait_timer);
  caps_wait_timer = 0;
  if (exiting) return;
  for (auto& w : windows) {
    if (w->runs_host() && !w->rotation_started()) w->start_rotation();
  }
}

ScreenSlot App::slot_for(const Monitor& m) const {
  ScreenSlot s;
  s.rc = {(int)m.rc.left, (int)m.rc.top, (int)(m.rc.right - m.rc.left), (int)(m.rc.bottom - m.rc.top)};
  // Hosts on every monitor, or the primary only (the others stay black); the
  // not-imported / host-missing message goes on the primary alone.
  s.runs_host = message.empty() && (settings.all_monitors || m.primary);
  s.message = m.primary && !message.empty();
  if (s.runs_host && s.rc.w > 0 && s.rc.h > 0) s.emu = emulated_screen_size((double)s.rc.w / s.rc.h, settings.scale);
  return s;
}

std::unique_ptr<SaverWindow> App::make_window(const Monitor& m) {
  ScreenSlot s = slot_for(m);
  return std::make_unique<SaverWindow>(*this, next_window_index++, s.runs_host, s.message ? message : std::wstring());
}

void App::display_changed() {
  if (preview || exiting) return;
  if (!relayout_timer) last_log("display change: settling for %u ms", kRelayoutSettleMs);
  // Re-armed by every message of the burst: a thread timer, since the window
  // that took the message may be one the relayout destroys.
  relayout_timer = SetTimer(nullptr, relayout_timer, kRelayoutSettleMs, nullptr);
  // Windows moves the cursor off a monitor that goes away or changes mode,
  // often by far more than the move threshold, around this message (which
  // is sent, so it is handled before the WM_MOUSEMOVE that follows). Until
  // relayout() re-plans the windows, moves only re-set the threshold's
  // origin (on_move): they are not the user coming back.
  baseline = cursor_pos();
  settle_moves = 0;
}

bool App::on_thread_message(const MSG& msg) {
  if (msg.message == WM_APP_CAPS) {
    // The probe's answer (App::run): no window of ours is its target, so a
    // relayout can't lose it. (A modal loop would drop a thread message; the
    // saver runs none.)
    std::unique_ptr<HostCapabilities> caps(reinterpret_cast<HostCapabilities*>(msg.lParam));
    if (caps) on_capabilities(*caps);
    return true;
  }
  if (msg.message != WM_TIMER) return false;
  if (relayout_timer && msg.wParam == relayout_timer) {
    KillTimer(nullptr, relayout_timer);
    relayout_timer = 0;
    relayout();
    return true;
  }
  if (caps_timer && msg.wParam == caps_timer) {
    check_caps();
    check_numlock();
    poll_status();
    return true;
  }
  if (hold_timer && msg.wParam == hold_timer) {
    poll_status();
    return true;
  }
  if (caps_wait_timer && msg.wParam == caps_wait_timer) {
    last_log("host capabilities: no answer within %u ms; Random keeps every module", kCapsWaitMs);
    caps_wait_over();
    return true;
  }
  if (script_timer && msg.wParam == script_timer) {
    script_tick();
    return true;
  }
  return false;
}

void App::relayout() {
  if (preview || exiting || windows.empty()) return;
  std::vector<Monitor> mons = monitors(++topology_changes);
  std::vector<ScreenSlot> now, next;
  for (auto& w : windows) now.push_back(w->slot());
  for (auto& m : mons) next.push_back(slot_for(m));
  RelayoutPlan plan = plan_relayout(now, next);
  last_log("relayout monitors=%zu->%zu kept=%d moved=%d created=%d retired=%d", now.size(), next.size(), plan.kept,
           plan.moved, plan.created, plan.retired);

  relayouting = true;
  HWND old_front = windows.front()->hwnd;
  SaverWindow* old_owner = owner();
  std::vector<std::unique_ptr<SaverWindow>> fresh;
  std::vector<SaverWindow*> to_start;
  for (size_t i = 0; i < mons.size(); ++i) {
    if (plan.reuse[i] >= 0) {
      auto& w = windows[(size_t)plan.reuse[i]];
      w->place(mons[i].rc);
      fresh.push_back(std::move(w));
    } else {
      auto w = make_window(mons[i]);
      if (!w->create_fullscreen(mons[i].rc)) continue;
      last_log("relayout: window=%d on %ldx%ld at %ld,%ld", next_window_index - 1, mons[i].rc.right - mons[i].rc.left,
               mons[i].rc.bottom - mons[i].rc.top, mons[i].rc.left, mons[i].rc.top);
      to_start.push_back(w.get());
      fresh.push_back(std::move(w));
    }
  }
  if (fresh.empty()) {
    // Not one window could be made: keep what there is rather than nothing.
    for (auto& w : windows) if (w) fresh.push_back(std::move(w));
  }
  // Take the foreground before any window goes, so destroying the active
  // one hands activation to us and not to whatever is behind the saver.
  SetForegroundWindow(fresh.front()->hwnd);
  if (power_notify && fresh.front()->hwnd != old_front) {
    UnregisterPowerSettingNotification(power_notify);
    power_notify = nullptr;
  }
  // A new input owner: whatever was held for the old one is moot, and play
  // (clip, cursor) starts over from its status.
  if (fresh.front().get() != old_owner) {
    holds.clear();
    was_interactive = false;
  }
  for (auto& w : windows) if (w) w->retire();
  windows = std::move(fresh);   // the retired ones are freed here
  // Sound follows the primary monitor's window (AUDIO.md §9): one that lost
  // that place stops playing now; the new owner's next host plays.
  for (auto& w : windows) if (w.get() != owner()) w->drop_sound();
  if (!power_notify) {
    power_notify = RegisterPowerSettingNotification(windows.front()->hwnd, &kConsoleDisplayState,
                                                    DEVICE_NOTIFY_WINDOW_HANDLE);
  }
  for (SaverWindow* w : to_start) w->start();
  // Windows appearing under the cursor, or a cursor moved off a monitor that
  // went away, is not the user coming back.
  GetCursorPos(&cursor_start);
  baseline = cursor_pos();
  first_move_seen = false;
  relayouting = false;
  update_clip();
}

void App::teardown() {
  for (auto& w : windows) w->kill_host();      // QUIT is already on its way; this waits
  pacer.stop();
  for (auto& w : windows) if (w->hwnd) DestroyWindow(w->hwnd);
  if (job) {
    CloseHandle(job);   // KILL_ON_JOB_CLOSE: anything left dies here
    job = nullptr;
  }
  // Delete-on-close: the desktop captures go with their last handle.
  for (HANDLE h : seed_files) CloseHandle(h);
  seed_files.clear();
}

void App::set_display_on(bool on) {
  if (on == display_on || exiting) return;
  display_on = on;
  last_log("display %s: %s", on ? "on" : "off", on ? "resuming" : "pausing hosts");
  pacer.set_paused(!on);
  if (on) {
    for (auto& w : windows) w->resumed();
  }
}

void App::input_exit(const std::string& what) {
  if (exiting) return;
#if AD_SCR_TEST_HOOKS
  if (hooks.ignore_input) {
    log_line("input: %s (ignored: AD_SCR_TEST_IGNORE_INPUT)", what.c_str());
    return;
  }
#endif
  last_log("input: %s", what.c_str());
  request_exit(kExitOk, what.c_str());
}

void App::on_presented() {
  if (hooks.display_off_ms > 0 && !test_display_cycled && !windows.empty() && windows.front()->presented >= 5) {
    test_display_cycled = true;
    set_display_on(false);
    SetTimer(windows.front()->hwnd, kTimerTestDisplay, (UINT)std::min<long long>(hooks.display_off_ms, 600000), nullptr);
  }
  if (hooks.exit_after_frames <= 0 || exiting) return;
  for (auto& w : windows) {
    if (w->runs_host() && w->presented < (uint64_t)hooks.exit_after_frames) return;
  }
  last_log("test-exit after %lld frames", hooks.exit_after_frames);
  request_exit(kExitOk, "test-exit");
}

// ---- input (INTERACTION.md §4) --------------------------------------------------

void App::start_input() {
  baseline = cursor_start;
  if (scripted()) {
    // The synthetic cursor starts in the middle of the owner's window, so a
    // script's moves land on the owner's frame wherever the real one is.
    if (SaverWindow* o = owner()) {
      RECT r{};
      GetWindowRect(o->hwnd, &r);
      synthetic_cursor = {(r.left + r.right) / 2, (r.top + r.bottom) / 2};
    }
    baseline = synthetic_cursor;
    script_timer = SetTimer(nullptr, 0, kScriptTickMs, nullptr);
    log_line("test input: %zu step(s) from %s", script.size(), narrow(hooks.input_script).c_str());
  }
  caps_timer = SetTimer(nullptr, 0, kCapsCheckMs, nullptr);
}

int App::caps_toggle() const {
  if (scripted()) return synthetic_caps;
  return (GetKeyState(VK_CAPITAL) & 1) ? 1 : 0;
}

int App::numlock_toggle() const {
  if (scripted()) return synthetic_numlock;
  return (GetKeyState(VK_NUMLOCK) & 1) ? 1 : 0;
}

POINT App::cursor_pos() const {
  if (scripted()) return synthetic_cursor;
  POINT p{};
  GetCursorPos(&p);
  return p;
}

OwnerStatus App::owner_status() const {
  SaverWindow* o = owner();
  return o ? o->status() : OwnerStatus{};
}

bool App::owner_playing_no_rotate() const {
  OwnerStatus st = owner_status();
  return st.interactive() && !st.rotate_ok();
}

void App::owner_host_gone(const HostProcess* h) {
  // "The hold ... ends at once when the owner's host is gone" (§4.3).
  for (const PendingHold& p : holds) {
    if (p.host == h) {
      input_exit(p.reason + " (owner host gone while held)");
      return;
    }
  }
}

void App::ensure_hold_timer() {
  if (!hold_timer) hold_timer = SetTimer(nullptr, 0, kHoldPollMs, nullptr);
}

void App::evaluate(const InputEvent& ev, uint64_t n, long dx, long dy) {
  if (exiting) return;
  const auto now = InputClock::now();
  OwnerStatus st = owner_status();
  HoldSeqs hs{n, false, now};
  Verdict v = decide(ev, st, hs, now);
  std::string reason = exit_reason(ev, dx, dy);
  if (v == Verdict::exit) {
    input_exit(reason);
  } else if (v == Verdict::hold) {
    SaverWindow* o = owner();
    holds.push_back({ev, HoldSeqs{n, true, now}, o ? o->host() : nullptr, reason});
    log_line("input: hold %s n=%llu applied=%llu eaten=%llu flags=0x%x", reason.c_str(), (unsigned long long)n,
             (unsigned long long)st.rec.input_applied, (unsigned long long)st.rec.input_eaten, st.rec.flags);
    ensure_hold_timer();
  }
}

void App::check_caps() {
  if (exiting) return;
  SaverWindow* o = owner();
  if (!o || !o->host()) return;
  const int now = caps_toggle();
  if (o->caps_sent == now) return;
  uint64_t n = o->send_input(caps_line(now != 0));
  if (n) {
    o->caps_sent = now;
    log_line("input: caps %d -> owner (n=%llu)", now, (unsigned long long)n);
  }
}

void App::check_numlock() {
  // Only a host that numbers NUMLOCK lines hears one: another would leave the
  // input lines' numbers behind the saver's count (dialog_support.h).
  if (exiting || !host_caps.takes_numlock_lines()) return;
  SaverWindow* o = owner();
  if (!o || !o->host()) return;
  const int now = numlock_toggle();
  if (o->numlock_sent == now) return;
  uint64_t n = o->send_input(numlock_line(now != 0));
  if (n) {
    o->numlock_sent = now;
    log_line("input: numlock %d -> owner (n=%llu)", now, (unsigned long long)n);
  }
}

void App::on_key(int vk, bool down, bool sys) {
  if (exiting) return;
  InputEvent ev{sys ? (down ? InputKind::syskey_down : InputKind::syskey_up)
                    : (down ? InputKind::key_down : InputKind::key_up),
                vk};
  uint64_t n = 0;
  if (!sys) {
    // The owner hears the key first, then whether Caps Lock or Num Lock
    // changed (checked on both down and up: whenever Windows flips a toggle
    // bit).
    if (SaverWindow* o = owner()) n = o->send_input(key_line(vk, down));
    check_caps();
    check_numlock();
  }
  evaluate(ev, n);
}

void App::on_button(uint32_t bit, bool down) {
  if (exiting) return;
  if (bit) buttons = down ? (buttons | bit) : (buttons & ~bit);
  uint64_t n = 0;
  if (SaverWindow* o = owner()) {
    POINT f = o->map_cursor(cursor_pos());
    n = o->send_input(mouse_line(f.x, f.y, buttons));
  }
  evaluate(InputEvent{down ? InputKind::button_down : InputKind::button_up}, n);
}

void App::on_wheel() {
  if (exiting) return;
  evaluate(InputEvent{InputKind::wheel}, 0);
}

void App::on_move() {
  if (exiting) return;
  const POINT p = cursor_pos();
  uint64_t n = 0;
  if (SaverWindow* o = owner()) {
    POINT f = o->map_cursor(p);
    n = o->send_input(mouse_line(f.x, f.y, buttons));
  }
  if (relayout_timer) {
    // A display change is settling (display_changed): wherever Windows put
    // the cursor is the new origin, until relayout() sets it for good.
    baseline = p;
    if (settle_moves++ == 0) log_line("input: move while the display settles: not an exit");
    return;
  }
  const long dx = p.x - baseline.x, dy = p.y - baseline.y;
  // One move decision at a time: the held one covers this move too.
  for (const PendingHold& h : holds)
    if (h.ev.kind == InputKind::move) return;
  InputEvent ev{InputKind::move, 0, std::sqrt((double)dx * dx + (double)dy * dy)};
  evaluate(ev, n, dx, dy);
}

void App::on_deactivate() {
  if (exiting) return;
  if (clip_active) {
    ClipCursor(nullptr);
    clip_active = false;
  }
  input_exit("deactivated fg=" + foreground_exe());
}

void App::on_session_away(const char* what) {
  if (exiting) return;
  if (clip_active) {
    ClipCursor(nullptr);
    clip_active = false;
  }
  input_exit(what);
}

void App::update_clip() {
  SaverWindow* o = owner();
  if (was_interactive && o && !exiting) {
    RECT r = o->frame_screen();
    RECT cur{};
    GetClipCursor(&cur);
    if (!clip_active || !EqualRect(&cur, &r)) {
      ClipCursor(&r);
      if (!clip_active) log_line("clip set %ld,%ld,%ld,%ld", r.left, r.top, r.right, r.bottom);
      clip_active = true;
    }
  } else if (clip_active) {
    ClipCursor(nullptr);
    clip_active = false;
    log_line("clip released");
  }
}

void App::update_cursor(const OwnerStatus& st) {
  // Hidden, except while the owner plays and its module asks for one (§4.2).
  const bool want = was_interactive && st.cursor();
  if (want == cursor_visible) return;
  cursor_visible = want;
  ShowCursor(want ? TRUE : FALSE);
  SetCursor(want ? LoadCursorW(nullptr, IDC_ARROW) : nullptr);
  log_line("cursor %s", want ? "shown" : "hidden");
}

void App::on_cursor(HWND) { SetCursor(cursor_visible ? LoadCursorW(nullptr, IDC_ARROW) : nullptr); }

void App::poll_status() {
  if (preview || exiting) return;
  SaverWindow* o = owner();
  OwnerStatus st = owner_status();
  if (st.wake()) {
    input_exit("wake");
    return;
  }
  const bool now_interactive = st.interactive();
  if (now_interactive != was_interactive) {
    was_interactive = now_interactive;
    last_log("play %s window=%d source=%u", now_interactive ? "starts" : "ends", o ? o->index() : -1,
             (unsigned)st.rec.source);
    // After a game the move threshold counts from where the cursor is now
    // (AFTERDAR.SCR kept the start position; any move after a mouse game
    // would then end the saver).
    if (!now_interactive) baseline = cursor_pos();
  }
  update_clip();
  update_cursor(st);
  // Held decisions (§4.3).
  const auto now = InputClock::now();
  for (size_t i = 0; i < holds.size();) {
    PendingHold& h = holds[i];
    OwnerStatus hs = st;
    if (!o || o->host() != h.host) hs.running = false;   // its host is gone
    Verdict v = decide(h.ev, hs, h.seqs, now);
    if (v == Verdict::exit) {
      input_exit(h.reason + " (after hold " +
                 std::to_string(std::chrono::duration_cast<milliseconds>(now - h.seqs.since).count()) + " ms)");
      return;
    }
    if (v == Verdict::forward) {
      log_line("input: kept %s n=%llu applied=%llu eaten=%llu flags=0x%x", h.reason.c_str(),
               (unsigned long long)h.seqs.n, (unsigned long long)st.rec.input_applied,
               (unsigned long long)st.rec.input_eaten, st.rec.flags);
      // A move the module took resets the threshold, so the next nudge is
      // judged afresh.
      if (h.ev.kind == InputKind::move) baseline = cursor_pos();
      holds.erase(holds.begin() + (long)i);
      continue;
    }
    ++i;
  }
  if (holds.empty() && hold_timer) {
    KillTimer(nullptr, hold_timer);
    hold_timer = 0;
  }
}

void App::script_tick() {
  if (exiting) return;
  SaverWindow* o = owner();
  while (script_pos < script.size() && !exiting) {
    const TestStep& s = script[script_pos];
    if (s.op == TestStep::Op::wait) {
      if (!script_waiting) {
        script_waiting = true;
        script_wait_until = GetTickCount64() + (ULONGLONG)s.a;
      }
      if (GetTickCount64() < script_wait_until) return;
      script_waiting = false;
      ++script_pos;
      continue;
    }
    if (s.op == TestStep::Op::frames) {
      if (!script_waiting) {
        script_waiting = true;
        script_frames_target = (o ? o->presented : 0) + (uint64_t)s.a;
      }
      if (!o || o->presented < script_frames_target) return;
      script_waiting = false;
      ++script_pos;
      continue;
    }
    ++script_pos;
    switch (s.op) {
      case TestStep::Op::key:
        // Windows flips a toggle on the down.
        if (s.a == VK_CAPITAL && s.b) synthetic_caps ^= 1;
        if (s.a == VK_NUMLOCK && s.b) synthetic_numlock ^= 1;
        on_key(s.a, s.b != 0, false);
        break;
      case TestStep::Op::syskey:
        on_key(s.a, s.b != 0, true);
        break;
      case TestStep::Op::caps_state:
        synthetic_caps = s.a;
        break;
      case TestStep::Op::numlock_state:
        synthetic_numlock = s.a;
        break;
      case TestStep::Op::button:
        on_button((uint32_t)s.a, s.b != 0);
        break;
      case TestStep::Op::wheel:
        on_wheel();
        break;
      case TestStep::Op::move:
        synthetic_cursor.x += s.a;
        synthetic_cursor.y += s.b;
        on_move();
        break;
      case TestStep::Op::deactivate:
        on_deactivate();
        break;
      case TestStep::Op::display_change:
        // As Windows sends it: to the window, handled before any input.
        if (o && o->hwnd) SendMessageW(o->hwnd, WM_DISPLAYCHANGE, 32, 0);
        break;
      case TestStep::Op::clip_log: {
        RECT r{}, virt{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0};
        virt.right = virt.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
        virt.bottom = virt.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
        GetClipCursor(&r);
        if (EqualRect(&r, &virt)) log_line("test: clip=none");
        else log_line("test: clip=%ld,%ld,%ld,%ld", r.left, r.top, r.right, r.bottom);
        break;
      }
      case TestStep::Op::status_log: {
        OwnerStatus st = owner_status();
        log_line("test: status have=%d flags=0x%x frames=%llu applied=%llu eaten=%llu", st.have ? 1 : 0, st.rec.flags,
                 (unsigned long long)st.rec.frames, (unsigned long long)st.rec.input_applied,
                 (unsigned long long)st.rec.input_eaten);
        break;
      }
      case TestStep::Op::log:
        log_line("test: %s", s.text.c_str());
        break;
      default:
        break;
    }
  }
  if (script_pos >= script.size() && script_timer) {
    KillTimer(nullptr, script_timer);
    script_timer = 0;
    log_line("test input: done");
  }
}

} // namespace

int run_saver(const Args& args, void* hinstance) {
  if (args.mode == Mode::preview && !args.valid) return kExitBadArgs;
  App app(args, static_cast<HINSTANCE>(hinstance));
  return app.run();
}

} // namespace adw::scr
