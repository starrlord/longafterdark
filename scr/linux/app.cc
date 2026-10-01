// The Linux player: one module at a time in adhostwin.exe under Wine, its
// frames presented on X11, and the front-end rules the Windows saver keeps
// (scr/src/saver.cc, host_process.cc, input_rules.cc, sound.cc):
//
//  * one poll loop over the X connection, the hosts' pipes, a signal pipe
//    and the timers, so nothing a host does (exit, stall, crash, a flood of
//    garbage) can freeze the window or its input;
//  * GOs paced at --fps on the monotonic clock, one in flight, and none
//    while the display is powered off (DPMS), so the host idles; the poll
//    sleeps until the next deadline that something will act on, so no state
//    (a restart waiting for the display, say) turns into a busy loop;
//  * a watchdog: a host that exits, stalls for 20 s or sends no first frame
//    within 90 s is restarted with backoff, for as long as the player runs;
//    when three runs in a row failed and the last showed no frame, the
//    window says the module could not be started until a frame comes (the
//    Windows saver's rule, restart.h);
//  * wake-or-play decided from the host's own status record (ADSTATUSLOG),
//    by the same decide() as the Windows saver;
//  * the keyboard by the focus, never a grab, so a screen locker can still
//    lock (and its grab ends the saver, as a lock ends the Windows one); the
//    pointer confined to the frame only while a game plays;
//  * full screen, the primary monitor plays and the others are black, as
//    the Windows saver's "primary monitor only" keeps them: a move or a
//    click on any of them counts, and the pointer is also looked at every
//    250 ms, so a move past the threshold anywhere ends the saver;
//  * each host's screen by the Windows saver's rules (screen_for: After
//    Dark's at the window's shape, 480 or 720 lines; a module's own screen,
//    Intermission's, Star Trek's, ScreamSavers' and Marvel's 640x480, as it
//    is; a preview's 320x240 at 30 frames a second) and its frames
//    letterboxed into the window, the mouse mapped through the same
//    rectangle (present.h);
//  * XScreenSaver: -root draws in its window, its rotation follows the
//    clock, so the players on the other monitors show the same module and
//    change it at the same moment (rotation.h; --different-modules gives
//    each an order of its own), and only the player on the primary monitor
//    makes sound; a -window-id preview is silent, and its host and Wine run
//    at nice 10 (the Windows /p preview's host runs below normal priority);
//  * stopping: QUIT first, a grace for the sound shutdown, then SIGTERM and
//    SIGKILL; no child left behind.
#include "app.h"

#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/dpms.h>
#include <X11/keysym.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "adw_version.h"
#include "catalog.h"
#include "geometry.h"
#include "host.h"
#include "input_rules.h"
#include "keymap.h"
#include "log.h"
#include "options.h"
#include "present.h"
#include "restart.h"
#include "rotation.h"
#include "sound.h"
#include "status.h"
#include "wine.h"

namespace lad {

namespace {

using namespace std::chrono_literals;
using std::chrono::milliseconds;

constexpr auto kWatchdogTick = 500ms;
// Caps Lock and Num Lock are re-checked this often (INTERACTION.md §4.1).
constexpr auto kCapsCheck = 250ms;
// While a decision is held (§4.3) the status is looked at this often.
constexpr auto kHoldPoll = 16ms;
// A rotation that waits for a game to end re-checks this often (§4.2).
constexpr auto kRotateRetry = 1s;
// The clock's order changes this long after its slot ends (rotation.h).
constexpr auto kChangeMargin = 50ms;
// The longest the first host waits for the --capabilities answer when the
// rotation holds a module of another ABI than After Dark's.
constexpr auto kCapsGateWait = 2s;
// The --capabilities probe is given this long (Wine's first start in a new
// prefix makes the prefix first).
constexpr auto kProbeTimeout = 60s;
constexpr auto kStatsEvery = 5s;
// How long after the window appears a plain focus change is the window
// manager settling rather than the user switching away, and how often the
// focus is taken back meanwhile.
constexpr auto kFocusSettle = 3s;
constexpr int kFocusRetakes = 5;
// A window of the player's own is placed by the window manager as it maps
// (a full-screen one on its monitor): the first host waits this long at
// most for that, so its screen takes the window's real shape.
constexpr auto kMapGateWait = 1s;
// Previews are thumbnails: at most 30 frames a second (the Windows saver's
// 30 ms between a /p host's GOs).
constexpr auto kPreviewPeriod = std::chrono::microseconds(33333);
// ...and light: a preview's host, its --capabilities probe and every Wine
// process they start run at this nice value at least, as the Windows /p
// preview's host runs at BELOW_NORMAL_PRIORITY_CLASS.
constexpr int kPreviewNice = 10;

// ---- signals: handlers write the signal number into a pipe the loop polls
int g_sig_pipe[2] = {-1, -1};
void on_signal(int sig) {
  const int saved = errno;
  const unsigned char b = (unsigned char)sig;
  ssize_t w = write(g_sig_pipe[1], &b, 1);
  (void)w;
  errno = saved;
}

// The wall clock, in milliseconds since the Unix epoch: the clock's order
// (rotation.h) follows it.
int64_t wall_ms() {
  return std::chrono::duration_cast<milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// A wall-clock time as the local time of day, for the log.
std::string time_of_day(int64_t unix_ms) {
  const time_t t = (time_t)(unix_ms / 1000);
  tm local{};
  char text[16] = "?";
  if (localtime_r(&t, &local)) strftime(text, sizeof(text), "%H:%M:%S", &local);
  return text;
}

void print_help() {
  printf(
      "Long After Dark %s for Linux: the original After Dark screen savers, run under Wine.\n"
      "\n"
      "Usage:\n"
      "  longafterdark [options] [module]      run a module (by its id, name or catalog path),\n"
      "                                        or every imported module in turn when none is named\n"
      "  longafterdark --list                  list the imported modules\n"
      "  longafterdark --import <adimport options>   run adimport.exe under Wine (e.g. --download deluxe)\n"
      "\n"
      "Options:\n"
      "      --module <module>   the module to run, as [module] above (XScreenSaver's settings\n"
      "                          keep it this way)\n"
      "  -f, --fullscreen        full screen on the primary monitor, as a screen saver (the default;\n"
      "                          the other monitors go black)\n"
      "  -w, --window            in a window (640x480, times --scale)\n"
      "  -s, --scale <n>         window size factor, 1 to 8\n"
      "      --lines <480|720>   the screen After Dark modules get: 480 lines (the default) or 720,\n"
      "                          as wide as the display's shape; Star Wars, Star Trek, ScreamSavers\n"
      "                          and Marvel modules always get 640x480. Frames keep their shape,\n"
      "                          with black bars\n"
      "  -r, --random            rotate through every module (a named module plays first)\n"
      "      --cycle <seconds>   time per module when rotating (default 300; 0 never rotates)\n"
      "      --fps <n>           frames per second asked of the host (default 60)\n"
      "      --sound             sound on (the default)\n"
      "      --no-sound          sound off\n"
      "      --volume <0..100>   the modules' volume (default %d)\n"
      "      --test-pattern      the host's test pattern instead of a module\n"
      "      --assets-dir <dir>  the imported releases' folder (default: $AD_ASSETS_DIR, else\n"
      "                          the Wine prefix's %%LOCALAPPDATA%%\\LongAfterDark\\assets)\n"
      "  -root                   XScreenSaver's mode: draw in its window ($XSCREENSAVER_WINDOW),\n"
      "                          else the virtual root, else the root window. With a window per\n"
      "                          monitor, all show the same module and change it together, on\n"
      "                          the clock, and only the primary monitor's plays sound. The\n"
      "                          default when $XSCREENSAVER_WINDOW is set and no other mode is given\n"
      "      --different-modules\n"
      "                          with -root, a rotation in an order of its own: a different module\n"
      "                          on each monitor\n"
      "      --stretch           stretch modules with a 640x480 screen of their own (Star Wars,\n"
      "                          Star Trek, ScreamSavers, Marvel, The Far Side, Dilbert) to fill the\n"
      "                          window, instead of keeping their shape with black bars\n"
      "  -window-id <id>         draw in an existing window (xscreensaver-settings' preview, which\n"
      "                          passes --window-id): silent, a 320x240 screen, 30 frames a second\n"
      "      --verbose           log what the player and the host do on stderr\n"
      "  -v, --version           print the version\n"
      "  -h, --help              this text\n"
      "\n"
      "Ending the saver and playing (full screen; the rules of the Windows saver):\n"
      "  Any key but Shift, Ctrl, Caps Lock and Num Lock, a click, the wheel or a move of the\n"
      "  mouse ends it. Caps Lock starts the games that have one (Rodger Dodger, You Bet Your\n"
      "  Head, Simpsons Trivia, Marbles, ...) and Num Lock Final Exam's exam: while a game plays,\n"
      "  keys, clicks and the mouse are the game's; Caps Lock again stops it, and Alt or F10\n"
      "  ends the saver at once. In a window, Esc or q closes it when no game is playing.\n"
      "\n"
      "Testing:\n"
      "      --stall-timeout <ms>        restart a host that sends no frame this long (default 20000)\n"
      "      --first-frame-timeout <ms>  restart a host with no first frame this long (default 90000)\n"
      "      --test-display-off <ms>     after 5 frames, act as if the display powered off this long\n"
      "\n"
      "Environment: WINEPREFIX, AD_ASSETS_DIR, AD_HOST_EXE, AD_IMPORT_EXE, AD_WINE_BIN,\n"
      "AD_SCR_STATE (the modules' saved state; default $XDG_DATA_HOME/longafterdark/state),\n"
      "AD_SCR_SOUND=0, AD_SCR_LOG=<file>, AD_SCR_HOSTLOG=<file>.\n",
      ADW_VERSION_STRING, kDefaultVolume);
}

// Wine's own knobs for every Windows program the player runs: quiet unless
// the user asked for Wine's messages, and no Mono/Gecko installer dialogs if
// the run makes a new prefix (the modules need neither).
void add_wine_env(EnvChanges& env) {
  if (!getenv("WINEDEBUG")) env.emplace_back("WINEDEBUG", "-all");
  if (!getenv("WINEDLLOVERRIDES")) env.emplace_back("WINEDLLOVERRIDES", "mscoree=d;mshtml=d");
}

struct PendingHold {
  InputEvent ev;
  HoldSeqs seqs;
  uint64_t generation;   // the host it was sent to
  std::string reason;
};

enum class InputMode {
  saver,    // full screen: INTERACTION.md §4
  window,   // -w: every input goes to the module; Esc/q or the close button end it
  none,     // -root, -window-id: XScreenSaver has the input
};

class App {
 public:
  int run(int argc, char** argv);
  void emergency_stop();
  void on_x_error(const XErrorEvent& e);

 private:
  int run_import();
  int list_modules();
  bool setup_x(std::string* error);
  void setup_signals();
  void loop();
  void handle_signals();
  void handle_x_event(XEvent& ev);
  void on_frame();
  void log_stats(bool final);

  // hosts
  void try_start();
  void start_rotation();
  const Module* current_module() const;
  SizeI screen_for(const Module* m) const;
  void spawn();
  void kill_host();
  void watchdog();
  // A respawn is pending and will happen at respawn_at_: no host runs, and
  // the display is on (while it is off, the respawn waits for the watchdog
  // tick that sees it on again, and the loop sleeps meanwhile).
  bool respawn_armed() const { return respawn_pending_ && !host_ && display_on_ && !exiting_ && !gave_up_; }
  void show_start_failed(const std::string& text);
  void check_display();
  void rotate();
  int64_t slot_ms() const;
  void schedule_change();
  void start_probe();
  void poll_probe(const std::vector<pollfd>& fds, size_t idx);
  void finish_probe(bool answered);

  // input
  OwnerStatus owner_status() const;
  uint64_t send_input(const std::string& line);
  void on_key_event(XKeyEvent& xe, bool down);
  void on_key(int vk, bool down, bool sys);
  void on_button(unsigned xbutton, bool down, int wx, int wy);
  void on_motion(int rx, int ry, int wx, int wy);
  PointI event_point(Window w, int x, int y, int x_root, int y_root);
  void evaluate(const InputEvent& ev, uint64_t n, long dx = 0, long dy = 0);
  void poll_status();
  void check_caps();
  void check_numlock();
  void check_pointer();
  int caps_state();
  int numlock_state();
  PointI pointer_root();
  PointI mapped_pointer() const;
  void update_clip();
  void input_exit(const std::string& what);
  void request_exit(int code, const std::string& why);

  Options opt_;
  std::string wine_, prefix_, host_exe_;
  std::string state_dir_, state_win_;
  AssetsSearch assets_;
  std::string assets_root_win_, win_dir_;
  Catalog catalog_;
  bool have_catalog_ = false;
  bool test_pattern_ = false;
  Module file_module_;              // a module named by its file, outside the catalog
  bool use_file_module_ = false;
  const Module* named_ = nullptr;   // the module named on the command line
  bool rotating_ = false;
  std::unique_ptr<Rotation> rotation_;
  // rotation_, when it is the clock's order (-root, rotation.h); and the
  // wall clock as the player started, which its first slot follows
  // (XScreenSaver starts its players at once, but each may then wait a
  // different while for its host's capabilities).
  ClockRotation* clock_ = nullptr;
  int64_t started_ms_ = 0;

  Display* dpy_ = nullptr;
  Presenter pres_;
  InputMode input_mode_ = InputMode::saver;
  HostRole role_ = HostRole::saver;
  bool xkb_ = false;
  int xkb_event_ = 0;
  unsigned numlock_mask_ = 0;
  bool mapped_ = false;
  Clock::time_point mapped_at_{};
  bool had_focus_ = false;
  int focus_retakes_ = 0;
  // The pointer confined to the window while a game plays (the Windows
  // saver's ClipCursor, INTERACTION.md §4.2); released when play ends.
  bool clip_active_ = false;
  bool clip_failed_logged_ = false;
  // The display's power (DPMS): while it is off no GOs go out, so the host
  // sits idle on stdin (the Windows saver's Pacer::set_paused).
  bool dpms_ = false;
  bool display_on_ = true;
  Clock::time_point resumed_at_{};
  enum class TestDisplay { waiting, off, done } test_display_ = TestDisplay::waiting;
  Clock::time_point test_display_until_{};

  std::unique_ptr<HostProcess> host_;
  uint64_t generation_ = 0;
  Clock::duration period_{};
  SizeI host_screen_{};             // the current host's emulated screen (ADSCREENW/H)
  bool host_stretch_ = false;       // its frames fill the window (--stretch, a screen of its own)
  bool started_ = false;            // the first host has been asked for
  bool map_gate_ = false;           // ...but waits for the window to be placed
  Clock::time_point map_gate_until_{};
  RestartRule restart_;             // when the next host starts (the Windows saver's rule)
  bool respawn_pending_ = false;
  Clock::time_point respawn_at_{};
  bool gave_up_ = false;            // nothing to run: no host is ever started
  // "... could not be started" is on the window, until a frame comes (the
  // exit code is then 1).
  std::string start_failed_;
  std::string last_exit_text_ = "host ended";
  bool first_frame_logged_ = false;
  std::optional<Clock::time_point> rotate_at_;
  bool rotate_waiting_ = false;

  Child probe_;
  bool probing_ = false;
  std::string probe_out_;
  Clock::time_point probe_deadline_{};
  HostCapabilities caps_;
  bool caps_gate_ = false;
  Clock::time_point caps_gate_until_{};

  PointI baseline_{};
  PointI pointer_win_{};
  uint32_t buttons_ = 0;
  int caps_sent_ = -1, numlock_sent_ = -1;
  bool was_interactive_ = false;
  std::vector<PendingHold> holds_;
  Clock::time_point next_caps_check_{};
  Clock::time_point next_watchdog_{};

  bool exiting_ = false;
  int exit_code_ = kExitOk;
  bool target_gone_ = false;

  RawFrame frame_;
  uint64_t received_ = 0, presented_ = 0;
  uint64_t stats_received_ = 0, stats_presented_ = 0;
  Clock::time_point stats_at_{}, loop_started_{};
};

App* g_app = nullptr;

int x_error_handler(Display*, XErrorEvent* e) {
  if (g_app) g_app->on_x_error(*e);
  return 0;
}

int x_io_error_handler(Display*) {
  // The X server is gone: stop the host properly (its sound shutdown needs
  // no X), then leave; Xlib would exit anyway.
  if (g_app) g_app->emergency_stop();
  _exit(kExitError);
}

// ---- setup -------------------------------------------------------------------------

int App::run(int argc, char** argv) {
  started_ms_ = wall_ms();
  if (int r = parse_options(argc, argv, opt_); r >= 0) return r;
  if (opt_.help) {
    print_help();
    return kExitOk;
  }
  if (opt_.version) {
    printf("Long After Dark %s (Linux player)\n", ADW_VERSION_STRING);
    return kExitOk;
  }
  const char* log_path = getenv("AD_SCR_LOG");
  const char* hostlog_path = getenv("AD_SCR_HOSTLOG");
  log_open(log_path ? log_path : "", opt_.verbose);
  hostlog_open(hostlog_path ? hostlog_path : "", opt_.verbose);
  log_line("Long After Dark %s (Linux player), pid %d", ADW_VERSION_STRING, (int)getpid());

  wine_ = find_wine();
  prefix_ = wine_prefix();
  if (opt_.import) return run_import();

  assets_ = find_assets(opt_.assets_dir, prefix_);
  if (assets_.has_catalog) {
    win_dir_ = win_assets_dir(assets_.root);
    std::string err;
    have_catalog_ = load_catalog(win_dir_ + "/catalog-win.json", catalog_, &err);
    if (!have_catalog_) err_line("%s", err.c_str());
  }
  if (opt_.list) return list_modules();

  // What to run.
  test_pattern_ = opt_.test_pattern;
  if (!test_pattern_ && !opt_.module.empty()) {
    if (opt_.module.find('/') != std::string::npos && is_file(opt_.module)) {
      // A module file named by its path: run as it is.
      file_module_.id = opt_.module;
      file_module_.display_name = file_module_.name = opt_.module.substr(opt_.module.find_last_of('/') + 1);
      file_module_.path = opt_.module;
      use_file_module_ = true;
    } else if (!have_catalog_) {
      err_line("no module \"%s\": no releases are imported (looked in %s). Import one with: longafterdark "
               "--import --download <release>",
               opt_.module.c_str(), assets_.tried.empty() ? "?" : assets_.tried.front().c_str());
      return kExitUsage;
    } else {
      std::vector<const Module*> several;
      named_ = resolve_module(catalog_, opt_.module, &several);
      if (!named_) {
        if (!several.empty()) {
          std::string ids;
          for (const Module* m : several) ids += (ids.empty() ? "" : ", ") + m->id;
          err_line("\"%s\" names several modules (%s): name one by its id", opt_.module.c_str(), ids.c_str());
        } else {
          err_line("no module \"%s\" among the %zu imported (%s); --list shows them", opt_.module.c_str(),
                   catalog_.modules.size(), win_dir_.c_str());
        }
        return kExitUsage;
      }
    }
  }
  if (!test_pattern_ && !use_file_module_ && !named_ && (!have_catalog_ || catalog_.modules.empty())) {
    std::string where;
    for (const auto& t : assets_.tried) where += (where.empty() ? "" : ", ") + t;
    err_line("no modules imported (looked in %s); showing the test pattern. Import with: longafterdark --import "
             "--download <release>",
             where.empty() ? "?" : where.c_str());
    test_pattern_ = true;
  }

  if (wine_.empty()) {
    err_line("Wine is needed to run the modules and was not found (Debian, Ubuntu: sudo apt install wine64; or "
             "set AD_WINE_BIN)");
    return kExitError;
  }
  host_exe_ = find_program("AD_HOST_EXE", "adhostwin.exe",
                           {"build/dist/LongAfterDark/adhostwin.exe", "build/win/host/core/adhostwin.exe",
                            "build/win-release/host/core/adhostwin.exe"});
  if (host_exe_.empty()) {
    const char* e = getenv("AD_HOST_EXE");
    err_line("cannot find adhostwin.exe (%s)",
             e && *e ? (std::string("AD_HOST_EXE=") + e + " is not a file").c_str()
                     : ("it belongs next to this program, in " + self_dir()).c_str());
    return kExitError;
  }
  if (prefix_is_32bit(prefix_)) {
    err_line("the Wine prefix %s is 32-bit; adhostwin.exe needs a 64-bit prefix (a new WINEPREFIX made without "
             "WINEARCH=win32)",
             prefix_.c_str());
    return kExitError;
  }
  {
    std::string err;
    state_dir_ = state_dir(&err);
    if (state_dir_.empty()) err_line("%s; the modules' saved state stays in memory for this run", err.c_str());
    else state_win_ = to_windows_path(state_dir_, prefix_);
  }
  if (!assets_.root.empty()) assets_root_win_ = to_windows_path(assets_.root, prefix_);
  period_ = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / opt_.fps));
  log_line("wine=%s prefix=%s host=%s assets=%s (%s) state=%s fps=%d", wine_.c_str(), prefix_.c_str(),
           host_exe_.c_str(), assets_.root.empty() ? "-" : assets_.root.c_str(),
           assets_root_win_.empty() ? "-" : assets_root_win_.c_str(), state_win_.empty() ? "memory" : state_win_.c_str(),
           opt_.fps);

  setup_signals();
  std::string err;
  if (!setup_x(&err)) {
    err_line("%s", err.c_str());
    return kExitError;
  }

  start_probe();
  // Random leaves out what this host can't run: when the rotation holds a
  // module of another ABI than After Dark's, the first host waits (2 s at
  // most) for the host's answer, as the Windows saver's does.
  if (!test_pattern_ && !use_file_module_ && (opt_.random || !named_)) {
    for (const Module& m : catalog_.modules) caps_gate_ = caps_gate_ || m.abi != "afterdark";
  }
  if (caps_gate_ && probing_) {
    caps_gate_until_ = Clock::now() + kCapsGateWait;
    log_line("rotation: waiting for the host's capabilities (a module of another ABI)");
  } else {
    caps_gate_ = false;
  }
  if (pres_.owns_window() && !mapped_) {
    map_gate_ = true;
    map_gate_until_ = Clock::now() + kMapGateWait;
  }
  try_start();
  loop();

  // Give the desktop back first (window gone, grabs released), while the
  // host, which heard QUIT when the exit was asked for, shuts down.
  log_stats(true);
  pres_.close();
  if (dpy_) XCloseDisplay(dpy_);
  dpy_ = nullptr;
  kill_host();
  if (probing_ || probe_.pid > 0) {
    if (probe_.pid > 0) {
      kill(probe_.pid, SIGKILL);
      int st;
      while (waitpid(probe_.pid, &st, 0) < 0 && errno == EINTR) {
      }
    }
    if (probe_.out >= 0) close(probe_.out);
    probe_ = Child{};
  }
  // Nothing could run, or the module never showed a frame since it was said
  // it could not be started: an error, however the player was ended.
  if ((gave_up_ || !start_failed_.empty()) && exit_code_ == kExitOk) exit_code_ = kExitError;
  log_line("exit code=%d", exit_code_);
  return exit_code_;
}

int App::run_import() {
  const std::string exe = find_program("AD_IMPORT_EXE", "adimport.exe",
                                       {"build/dist/LongAfterDark/adimport.exe", "build/win/importer/adimport.exe",
                                        "build/win-release/importer/adimport.exe"});
  if (exe.empty()) {
    err_line("cannot find adimport.exe (it belongs next to this program, in %s; or set AD_IMPORT_EXE)",
             self_dir().c_str());
    return kExitError;
  }
  if (wine_.empty()) {
    err_line("Wine is needed to run adimport.exe and was not found (Debian, Ubuntu: sudo apt install wine64)");
    return kExitError;
  }
  // Paths the importer takes, in the form a Windows program needs.
  std::vector<std::string> args = {wine_, exe};
  auto winpath = [&](const std::string& v) {
    return v.find('/') != std::string::npos || is_file(v) || is_dir(v) ? to_windows_path(v, prefix_) : v;
  };
  const auto& a = opt_.import_args;
  for (size_t i = 0; i < a.size(); ++i) {
    args.push_back(a[i]);
    const bool path_next = a[i] == "--image" || a[i] == "--iso" || a[i] == "--from" || a[i] == "--dest" ||
                           a[i] == "--download-dir";
    if (path_next && i + 1 < a.size()) {
      args.push_back(winpath(a[++i]));
    } else if (a[i] == "--set-cover" && i + 2 < a.size()) {
      args.push_back(a[++i]);
      args.push_back(winpath(a[++i]));
    }
  }
  std::string root = !opt_.assets_dir.empty() ? opt_.assets_dir : (getenv("AD_ASSETS_DIR") ? getenv("AD_ASSETS_DIR") : "");
  if (!root.empty()) {
    make_dirs(root, 0755);
    setenv("AD_ASSETS_DIR", to_windows_path(root, prefix_).c_str(), 1);
  }
  EnvChanges env;
  add_wine_env(env);
  for (const auto& [k, v] : env) setenv(k.c_str(), v.c_str(), 1);
  std::vector<char*> argv;
  for (auto& s : args) argv.push_back(s.data());
  argv.push_back(nullptr);
  execv(wine_.c_str(), argv.data());
  err_line("cannot run %s: %s", wine_.c_str(), strerror(errno));
  return kExitError;
}

int App::list_modules() {
  if (!have_catalog_ || catalog_.modules.empty()) {
    std::string where;
    for (const auto& t : assets_.tried) where += (where.empty() ? "" : ", ") + t;
    err_line("no modules imported (looked in %s). Import with: longafterdark --import --download <release> "
             "(deluxe, ad10, ad32, tt, simpsons, ...)",
             where.empty() ? "?" : where.c_str());
    return kExitError;
  }
  printf("%zu modules imported (%s):\n", catalog_.modules.size(), win_dir_.c_str());
  std::string last;
  for (const Module& m : catalog_.modules) {
    if (m.package != last) {
      printf("\n%s\n", m.package_title.c_str());
      last = m.package;
    }
    printf("  %-24s %s\n", m.id.c_str(), m.name.c_str());
  }
  return kExitOk;
}

void App::setup_signals() {
  if (pipe2(g_sig_pipe, O_CLOEXEC | O_NONBLOCK) != 0) g_sig_pipe[0] = g_sig_pipe[1] = -1;
  struct sigaction sa {};
  sa.sa_handler = on_signal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART;
  for (int s : {SIGTERM, SIGINT, SIGHUP}) sigaction(s, &sa, nullptr);
  sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
  sigaction(SIGCHLD, &sa, nullptr);
  signal(SIGPIPE, SIG_IGN);
}

bool App::setup_x(std::string* error) {
  dpy_ = XOpenDisplay(nullptr);
  if (!dpy_) {
    *error = "cannot open the X display (is DISPLAY set?)";
    return false;
  }
  XSetErrorHandler(x_error_handler);
  XSetIOErrorHandler(x_io_error_handler);
  int opcode = 0, error_base = 0, major = XkbMajorVersion, minor = XkbMinorVersion;
  xkb_ = XkbQueryExtension(dpy_, &opcode, &xkb_event_, &error_base, &major, &minor);
  if (xkb_) {
    // Held keys repeat as presses alone, as Windows repeats WM_KEYDOWN.
    Bool supported = False;
    XkbSetDetectableAutoRepeat(dpy_, True, &supported);
    XkbSelectEventDetails(dpy_, XkbUseCoreKbd, XkbStateNotify, XkbModifierLockMask, XkbModifierLockMask);
    numlock_mask_ = XkbKeysymToModifiers(dpy_, XK_Num_Lock);
  }
  int dpms_event = 0, dpms_error = 0;
  dpms_ = DPMSQueryExtension(dpy_, &dpms_event, &dpms_error) && DPMSCapable(dpy_);
  switch (opt_.mode) {
    case WindowMode::fullscreen: input_mode_ = InputMode::saver; break;
    case WindowMode::window: input_mode_ = InputMode::window; break;
    default: input_mode_ = InputMode::none; break;
  }
  role_ = opt_.mode == WindowMode::embed ? HostRole::preview : HostRole::saver;
  if (opt_.root_from_env) log_line("window: no mode given and $XSCREENSAVER_WINDOW is set: as -root");
  if (!pres_.open(dpy_, opt_.mode, opt_.window_id, opt_.scale, error)) return false;
  log_line("window: %s", pres_.describe().c_str());
  if (input_mode_ == InputMode::saver) pres_.set_cursor_visible(false);
  baseline_ = pointer_root();
  return true;
}

// ---- the loop --------------------------------------------------------------------

void App::loop() {
  const int xfd = ConnectionNumber(dpy_);
  loop_started_ = stats_at_ = Clock::now();
  next_caps_check_ = loop_started_ + kCapsCheck;
  next_watchdog_ = loop_started_ + kWatchdogTick;
  std::vector<pollfd> fds;
  while (!exiting_) {
    auto now = Clock::now();
    // The poll sleeps until the next deadline. Each one counted here is
    // acted on once it has passed, and that moves it on: one that is not
    // (a respawn while the display is off) would wake the poll at once,
    // again and again, a busy loop.
    Clock::time_point wake = std::min(next_watchdog_, next_caps_check_);
    if (host_ && display_on_) wake = std::min(wake, host_->next_go_at(now));
    if (!holds_.empty()) wake = std::min(wake, now + kHoldPoll);
    if (rotate_at_) wake = std::min(wake, *rotate_at_);
    if (respawn_armed()) wake = std::min(wake, respawn_at_);
    if (probing_) wake = std::min(wake, probe_deadline_);
    if (caps_gate_) wake = std::min(wake, caps_gate_until_);
    if (map_gate_) wake = std::min(wake, map_gate_until_);
    if (const auto t = pres_.next_tick()) wake = std::min(wake, *t);
    if (opt_.verbose || log_enabled()) wake = std::min(wake, stats_at_ + kStatsEvery);

    XFlush(dpy_);
    const bool queued = XEventsQueued(dpy_, QueuedAlready) > 0;
    fds.clear();
    fds.push_back({g_sig_pipe[0], POLLIN, 0});
    fds.push_back({xfd, POLLIN, 0});
    size_t host_base = 0, probe_idx = 0;
    if (host_) host_->add_pollfds(fds, &host_base);
    if (probing_ && probe_.out >= 0) {
      probe_idx = fds.size();
      fds.push_back({probe_.out, POLLIN, 0});
    }
    timespec ts{0, 0};
    if (!queued && wake > now) {
      const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(wake - now).count();
      ts.tv_sec = (time_t)(ns / 1000000000);
      ts.tv_nsec = (long)(ns % 1000000000);
    }
    int r = ppoll(fds.data(), fds.size(), &ts, nullptr);
    if (r < 0 && errno != EINTR) {
      err_line("poll: %s", strerror(errno));
      request_exit(kExitError, "poll failed");
      break;
    }
    if (r > 0 && fds[0].revents) handle_signals();
    if (host_ && r > 0) host_->on_poll(fds, host_base);
    if (probing_ && probe_idx && r > 0) poll_probe(fds, probe_idx);

    // X events (XPending also reads what the socket holds).
    while (!exiting_ && XPending(dpy_) > 0) {
      XEvent ev;
      XNextEvent(dpy_, &ev);
      handle_x_event(ev);
    }
    if (target_gone_ && !exiting_) request_exit(kExitOk, "window gone");
    if (exiting_) break;
    pres_.tick(Clock::now());

    if (host_ && host_->take_frame(frame_)) on_frame();
    if (host_) {
      host_->check_exit();
      if (host_->ended()) watchdog();   // at once, not at the next tick
    }

    now = Clock::now();
    if (now >= next_caps_check_) {
      next_caps_check_ = now + kCapsCheck;
      check_caps();
      check_numlock();
      poll_status();
      check_pointer();
    } else if (!holds_.empty()) {
      poll_status();
    }
    if (exiting_) break;
    if (now >= next_watchdog_) {
      next_watchdog_ = now + kWatchdogTick;
      check_display();
      watchdog();
    } else if (respawn_armed() && now >= respawn_at_) {
      watchdog();
    }
    if (rotate_at_ && now >= *rotate_at_) rotate();
    if (probing_ && now >= probe_deadline_) {
      log_line("host capabilities: no answer within %lld s", (long long)std::chrono::duration_cast<std::chrono::seconds>(kProbeTimeout).count());
      finish_probe(false);
    }
    if (caps_gate_ && now >= caps_gate_until_) {
      log_line("host capabilities: no answer within 2 s; the rotation keeps every module");
      caps_gate_ = false;
      try_start();
    }
    if (map_gate_ && now >= map_gate_until_) {
      log_line("window: not mapped within 1 s; the first host starts at %dx%d", pres_.width(), pres_.height());
      map_gate_ = false;
      try_start();
    }
    if ((opt_.verbose || log_enabled()) && now >= stats_at_ + kStatsEvery) log_stats(false);
    if (host_ && !exiting_ && display_on_) host_->maybe_send_go(Clock::now());
  }
}

void App::handle_signals() {
  unsigned char buf[64];
  ssize_t n;
  while ((n = read(g_sig_pipe[0], buf, sizeof(buf))) > 0) {
    for (ssize_t i = 0; i < n; ++i) {
      const int sig = buf[i];
      if (sig == SIGCHLD) {
        if (host_) host_->check_exit();
        // Reaped only: its answer may still be in the pipe, and the pipe's
        // end (or the deadline) finishes the probe.
        int st;
        if (probe_.pid > 0 && reap_child(probe_.pid, &st)) probe_.pid = -1;
      } else {
        log_line("signal %d", sig);
        request_exit(kExitOk, std::string("signal ") + strsignal(sig));
      }
    }
  }
}

void App::handle_x_event(XEvent& ev) {
  switch (ev.type) {
    case KeyPress:
    case KeyRelease:
      if (input_mode_ != InputMode::none) on_key_event(ev.xkey, ev.type == KeyPress);
      break;
    // The player's window, and the black windows over the other monitors
    // (full screen): a click or a move on those counts as on the Windows
    // saver's, and reaches the host as a point off its frame.
    case ButtonPress:
    case ButtonRelease:
      if (input_mode_ != InputMode::none) {
        const XButtonEvent& b = ev.xbutton;
        const PointI w = event_point(b.window, b.x, b.y, b.x_root, b.y_root);
        on_button(b.button, ev.type == ButtonPress, w.x, w.y);
      }
      break;
    case MotionNotify:
      if (input_mode_ != InputMode::none) {
        const XMotionEvent& m = ev.xmotion;
        const PointI w = event_point(m.window, m.x, m.y, m.x_root, m.y_root);
        on_motion(m.x_root, m.y_root, w.x, w.y);
      }
      break;
    case ConfigureNotify:
    case Expose:
    case DestroyNotify: {
      // The window's new size (a letterbox of its own, the last frame
      // redrawn in it), what needs drawing again, and its end: someone
      // else's window (XScreenSaver's, a preview's) going ends the player.
      bool gone = false;
      pres_.handle_event(ev, &gone);
      if (gone && !target_gone_) {
        log_line("the window was destroyed");
        target_gone_ = true;
      }
      break;
    }
    case MapNotify:
      if (ev.xmap.window == pres_.window() && !mapped_) {
        mapped_ = true;
        mapped_at_ = Clock::now();
        // The keyboard comes by the focus, not a grab: a screen locker must
        // still be able to grab it (and ends the saver when it does, below).
        if (input_mode_ != InputMode::none) XSetInputFocus(dpy_, pres_.window(), RevertToPointerRoot, CurrentTime);
        // Where the window manager put it: the black windows go over the
        // other monitors, wherever that is.
        pres_.update_covers();
        baseline_ = pointer_root();
        if (map_gate_) {
          map_gate_ = false;
          log_line("window: mapped at %dx%d", pres_.width(), pres_.height());
          try_start();
        }
      }
      break;
    case FocusIn:
      if (ev.xfocus.window == pres_.window()) had_focus_ = true;
      break;
    case FocusOut:
      // Switching away ends the saver (WM_ACTIVATEAPP(FALSE)), as a lock
      // does: another program grabbed the keyboard (a screen locker, a
      // window manager's shortcut), or the focus went to another window.
      // A window manager that keeps new windows from taking the focus may
      // hand it back to the previous window as ours appears: in the first
      // seconds that is taken back rather than taken as the user.
      if (input_mode_ == InputMode::saver && had_focus_ && ev.xfocus.window == pres_.window() &&
          (ev.xfocus.detail == NotifyAncestor || ev.xfocus.detail == NotifyVirtual ||
           ev.xfocus.detail == NotifyNonlinear || ev.xfocus.detail == NotifyNonlinearVirtual)) {
        if (ev.xfocus.mode == NotifyGrab) {
          evaluate(InputEvent{InputKind::deactivate}, 0);
        } else if (ev.xfocus.mode == NotifyNormal) {
          if (Clock::now() - mapped_at_ < kFocusSettle && focus_retakes_ < kFocusRetakes) {
            ++focus_retakes_;
            log_line("focus: taken back while the window settles (%d)", focus_retakes_);
            XSetInputFocus(dpy_, pres_.window(), RevertToPointerRoot, CurrentTime);
          } else {
            evaluate(InputEvent{InputKind::deactivate}, 0);
          }
        }
      }
      break;
    case LeaveNotify:
      // The pointer left the window (a window manager may keep a full-screen
      // window to one monitor): a move, judged from the baseline as any.
      // (Into the clip window over the frame, NotifyInferior, it has not.)
      if (input_mode_ != InputMode::none && ev.xcrossing.mode == NotifyNormal && ev.xcrossing.detail != NotifyInferior &&
          ev.xcrossing.window == pres_.window()) {
        on_motion(ev.xcrossing.x_root, ev.xcrossing.y_root, ev.xcrossing.x, ev.xcrossing.y);
      }
      break;
    case ClientMessage:
      if ((Atom)ev.xclient.data.l[0] == pres_.wm_delete()) request_exit(kExitOk, "window closed");
      break;
    default:
      if (xkb_ && ev.type == xkb_event_) {
        const XkbEvent* xe = reinterpret_cast<const XkbEvent*>(&ev);
        if (xe->any.xkb_type == XkbStateNotify) {
          check_caps();
          check_numlock();
        }
      } else {
        pres_.handle_event(ev, nullptr);   // the XShm completion
      }
      break;
  }
}

void App::on_frame() {
  ++received_;
  if (pres_.present(frame_)) ++presented_;
  // A frame ends the rotation's run of dead modules, and replaces the
  // "could not be started" message.
  restart_.frame_shown();
  if (!start_failed_.empty()) {
    log_line("status: a frame replaces the message: %s", start_failed_.c_str());
    start_failed_.clear();
  }
  if (!first_frame_logged_) {
    first_frame_logged_ = true;
    const RectI r = pres_.frame_rect();
    log_line("first frame %dx%d P%d after %lld ms, drawn at %d,%d %dx%d in %dx%d", frame_.width, frame_.height,
             frame_.format,
             (long long)std::chrono::duration_cast<milliseconds>(Clock::now() - host_->started_at()).count(), r.x, r.y,
             r.w, r.h, pres_.width(), pres_.height());
  }
  // The host publishes a step's record before its frame (status.h).
  poll_status();
}

void App::log_stats(bool final) {
  const auto now = Clock::now();
  if (final) {
    const double s = std::chrono::duration<double>(now - loop_started_).count();
    log_line("stats: %llu frames received, %llu drawn in %.1f s (%.1f fps received)", (unsigned long long)received_,
             (unsigned long long)presented_, s, s > 0 ? received_ / s : 0.0);
    return;
  }
  const double s = std::chrono::duration<double>(now - stats_at_).count();
  if (s <= 0) return;
  log_line("stats: %.1f fps received, %.1f drawn, gos=%llu", (received_ - stats_received_) / s,
           (presented_ - stats_presented_) / s, host_ ? (unsigned long long)host_->gos_sent() : 0ull);
  stats_received_ = received_;
  stats_presented_ = presented_;
  stats_at_ = now;
}

// ---- hosts -----------------------------------------------------------------------

const Module* App::current_module() const {
  if (use_file_module_) return &file_module_;
  if (!rotation_) return nullptr;
  return catalog_.find(rotation_->current());
}

// The emulated screen a module's host gets (ADSCREENW/H), the Windows
// saver's screen_for: a preview's 320x240 for every module (/p); else its
// own screen when it has one (its catalog "screen", or its ABI's:
// Intermission's 640x480), else --lines (480 or 720) widened to the
// window's shape (module_screen). The frames are letterboxed into the
// window, so a screen of its own keeps its shape.
SizeI App::screen_for(const Module* m) const {
  if (opt_.mode == WindowMode::embed) return kPreviewScreen;
  const double aspect = pres_.height() > 0 ? (double)pres_.width() / pres_.height() : 4.0 / 3.0;
  return module_screen(m ? own_screen(m->abi, m->screen) : SizeI{}, aspect, opt_.lines / 480.0).emu;
}

void App::try_start() {
  if (started_ || exiting_ || caps_gate_ || map_gate_) return;
  started_ = true;
  start_rotation();
}

void App::start_rotation() {
  if (rotation_ || exiting_) return;
  if (test_pattern_ || use_file_module_) {
    spawn();
    return;
  }
  std::vector<std::string> ids;
  size_t left_out = 0;
  rotating_ = opt_.random || !named_;
  // XScreenSaver's players, one on each monitor, follow the clock's order,
  // and so show the same module (rotation.h).
  const bool clocked = rotating_ && opt_.mode == WindowMode::root && !opt_.different_modules;
  if (rotating_) {
    for (const Module& m : catalog_.modules) {
      if (!m.same_as.empty()) continue;   // a byte-identical copy plays once per pass, as its first
      if (!caps_.runs(m.lane, m.abi)) {
        ++left_out;
        continue;
      }
      ids.push_back(m.id);
    }
    if (left_out) log_line("rotation: left out %zu module(s) this host can't run", left_out);
    log_line("rotation: %zu module(s), %d s each, %s", ids.size(), opt_.cycle_s,
             clocked ? "in the clock's order" : "in an order of its own");
  } else {
    ids.push_back(named_->id);
  }
  const std::string first = named_ ? named_->id : std::string();
  if (clocked) {
    // The first slot as the player started, unless it is over already.
    const int64_t slot = std::max(first_slot(started_ms_, slot_ms()), clock_slot(wall_ms(), slot_ms()));
    auto clock = std::make_unique<ClockRotation>(ids, slot, first);
    clock_ = clock.get();
    rotation_ = std::move(clock);
  } else {
    std::random_device rd;
    rotation_ = std::make_unique<ShuffleRotation>(ids, rd(), first);
  }
  if (rotation_->empty()) {
    const std::string text = "None of the modules imported can run on this Long After Dark host (adhostwin.exe).";
    err_line("%s", text.c_str());
    pres_.show_message(text);
    gave_up_ = true;
    return;
  }
  spawn();
  if (!rotating_ || rotation_->size() < 2 || opt_.cycle_s <= 0) return;
  if (clock_) schedule_change();
  else rotate_at_ = Clock::now() + std::chrono::seconds(opt_.cycle_s);
}

void App::spawn() {
  const Module* m = test_pattern_ ? nullptr : current_module();
  if (!test_pattern_ && !m) return;
  const SizeI emu = screen_for(m);
  const int caps = caps_state();
  const int numlock = numlock_state();
  const bool numlock_env = caps_.takes_numlock_env();
  // Sound from one host alone (AUDIO.md §9): XScreenSaver runs one player
  // per monitor, and the one whose window covers the primary monitor plays.
  bool owner = true;
  if (opt_.mode == WindowMode::root && role_ == HostRole::saver) {
    std::string detail;
    owner = pres_.covers_primary_monitor(&detail);
    log_line("sound owner: %s (%s)", owner ? "yes" : "no, another window covers the primary monitor", detail.c_str());
  }
  const SoundChoice sound = sound_for(opt_.sound, opt_.volume, role_, owner, sound_forced_off());

  // The Windows forms afresh: a prefix that didn't exist at the first spawn
  // (Wine makes it then) has its drives now.
  if (!state_dir_.empty()) state_win_ = to_windows_path(state_dir_, prefix_);
  if (!assets_.root.empty()) assets_root_win_ = to_windows_path(assets_.root, prefix_);
  HostProcess::Spec spec;
  spec.wine = wine_;
  spec.exe = host_exe_;
  std::string module_win;
  if (test_pattern_) {
    spec.args = {"--test-pattern"};
  } else {
    module_win = to_windows_path(use_file_module_ ? m->path : win_dir_ + "/" + m->path, prefix_);
    spec.args = {module_win};
  }
  spec.cwd = is_dir(win_dir_) ? win_dir_ : std::string();
  spec.env = {
      {"ADSTREAM", "1"},
      {"ADSCREENW", std::to_string(emu.w)},
      {"ADSCREENH", std::to_string(emu.h)},
      {"ADCVSET", ""},                // no saved control values here: a stale inherited one goes
      {"AD_ASSETS_DIR", assets_root_win_},
      {"ADCAPS", caps > 0 ? "1" : "0"},
      {"ADNUMLOCK", numlock_env ? (numlock > 0 ? "1" : "0") : ""},
      {"ADSTATE", state_win_},        // "" (a folder that couldn't be made): in memory
      {"ADSEEDIMG", ""},
      {"ADSTATUSLOG", "1"},           // the status record, on stderr (status.h)
      {"ADSTATUSHANDLE", ""},         // names a Windows handle; never ours
  };
  add_wine_env(spec.env);
  add_sound_env(spec.env, sound);
  spec.sound = sound.on;
  spec.nice = role_ == HostRole::preview ? kPreviewNice : 0;

  if (host_) kill_host();
  ++generation_;
  host_ = std::make_unique<HostProcess>();
  first_frame_logged_ = false;
  host_screen_ = emu;
  // --stretch: a module with a screen of its own fills the window (never a
  // preview, whose 320x240 is every module's).
  host_stretch_ = opt_.stretch && opt_.mode != WindowMode::embed && m && own_screen(m->abi, m->screen).w > 0;
  pres_.set_stretch(host_stretch_);
  pres_.set_screen(emu);   // where its frames will land, before the first of them
  caps_sent_ = caps > 0 ? 1 : 0;
  numlock_sent_ = numlock_env ? (numlock > 0 ? 1 : 0) : -1;
  std::string err;
  const std::string name = test_pattern_ ? std::string("test pattern") : m->id;
  const Clock::duration period = role_ == HostRole::preview ? std::max<Clock::duration>(period_, kPreviewPeriod) : period_;
  if (!host_->start(spec, period, &err)) {
    err_line("cannot start the host for %s: %s", name.c_str(), err.c_str());
    host_.reset();
    respawn_pending_ = true;
    respawn_at_ = Clock::now() + restart_.spawn_failed();
    return;
  }
  log_line("spawn gen=%llu module=%s path=%s size=%dx%d fps=%.0f caps=%d numlock=%d sound=%d volume=%d pid=%d%s",
           (unsigned long long)generation_, name.c_str(), test_pattern_ ? "--test-pattern" : module_win.c_str(), emu.w,
           emu.h, 1.0 / std::chrono::duration<double>(period).count(), caps_sent_, numlock_sent_, sound.on ? 1 : 0,
           sound.on ? sound.volume : -1, (int)host_->pid(), spec.nice ? (" nice>=" + std::to_string(spec.nice)).c_str() : "");
}

void App::kill_host() {
  if (!host_) return;
  // "The hold ... ends at once when the owner's host is gone" (§4.3).
  for (const PendingHold& h : holds_) {
    if (h.generation == generation_) {
      input_exit(h.reason + " (host gone while held)");
      break;
    }
  }
  holds_.clear();
  if (host_->sound()) log_line("sound host stops gen=%llu", (unsigned long long)generation_);
  host_->stop_gracefully();
  log_line("host gen=%llu ended: %s%s, %llu frames", (unsigned long long)generation_,
           describe_exit(host_->exit_status()).c_str(), host_->signalled() ? " (stopped by the player)" : "",
           (unsigned long long)host_->frames());
  // Its own exit, for the "could not be started" message: none when the
  // player had to stop it with a signal (the Windows saver's rule).
  last_exit_text_ = host_->signalled() ? std::string() : host_exit_text(host_->exit_status());
  host_.reset();
  was_interactive_ = false;   // no host, no game: the pointer goes free
  update_clip();
  ++generation_;
}

void App::watchdog() {
  const auto now = Clock::now();
  if (exiting_ || gave_up_) return;
  if (!host_) {
    if (respawn_armed() && now >= respawn_at_) {
      respawn_pending_ = false;
      const Module* m = current_module();
      log_line("respawn module=%s", test_pattern_ ? "test pattern" : m ? m->id.c_str() : "?");
      spawn();
    }
    return;
  }
  const char* why = nullptr;
  host_->check_exit();
  // A paused host (display off) is silent by design; its stall clocks
  // restart when the display comes back.
  const auto since_frame = now - std::max(host_->last_frame_at(), resumed_at_);
  const auto since_start = now - std::max(host_->started_at(), resumed_at_);
  if (host_->ended()) why = "exit";
  else if (!display_on_) return;
  else if (host_->frames() > 0 && since_frame > milliseconds(opt_.stall_ms)) why = "stall";
  else if (host_->frames() == 0 && since_start > milliseconds(opt_.first_frame_ms)) why = "no-first-frame";
  if (!why) return;

  const uint64_t frames = host_->frames();
  const auto lived = now - host_->started_at();
  const uint64_t gen = generation_;
  const bool exited = host_->ended();   // by itself, not stopped by the watchdog
  kill_host();   // reaps it (last_exit_text_)
  log_line("host-%s gen=%llu frames=%llu lived_ms=%lld", why, (unsigned long long)gen, (unsigned long long)frames,
           (long long)std::chrono::duration_cast<milliseconds>(lived).count());
  if (exiting_) return;
  // The Windows saver's rule (restart.h): the restarts go on, and the
  // window says so rather than sit on a black screen when the module keeps
  // failing without a frame (its files are damaged, its lane isn't in this
  // adhostwin, or Wine can't run it).
  const bool can_skip = rotating_ && rotation_ && rotation_->size() > 1;
  const RestartStep next = restart_.host_ended(frames, lived, can_skip, rotation_ ? rotation_->size() : 1);
  if (next.message) {
    const Module* m = current_module();
    const std::string name =
        test_pattern_ ? std::string("The test pattern") : "\"" + (m ? m->display_name : std::string("?")) + "\"";
    // The exit code only when the host ended by itself: a host the watchdog
    // stopped has only the signal it was given.
    show_start_failed(name + " could not be started" +
                      (exited && !last_exit_text_.empty() ? " (" + last_exit_text_ + ")" : "") + ".");
  }
  if (next.skip) {
    log_line("skip module=%s after %d failures", rotation_->current().c_str(), kFailedRunsBeforeMessage);
    rotation_->next();
    // Black until the next module's first frame, or the message until then.
    if (start_failed_.empty()) pres_.clear();
  }
  respawn_pending_ = true;
  respawn_at_ = now + next.delay;
  log_line("respawn in %lld ms%s", (long long)next.delay.count(), display_on_ ? "" : ", once the display is on");
}

// "... could not be started" on the window until a frame comes, said once
// while it stays the same (the Windows saver's set_status).
void App::show_start_failed(const std::string& text) {
  if (text == start_failed_) return;
  start_failed_ = text;
  err_line("%s", text.c_str());
  pres_.show_message(text);
}

void App::check_display() {
  bool on = display_on_;
  if (opt_.test_display_off_ms > 0 && test_display_ != TestDisplay::done) {
    // --test-display-off: a power-off simulated after 5 frames.
    const auto now = Clock::now();
    if (test_display_ == TestDisplay::waiting && presented_ >= 5) {
      test_display_ = TestDisplay::off;
      test_display_until_ = now + milliseconds(opt_.test_display_off_ms);
      on = false;
    } else if (test_display_ == TestDisplay::off && now >= test_display_until_) {
      test_display_ = TestDisplay::done;
      on = true;
    }
  } else {
    if (!dpms_ || !dpy_) return;
    CARD16 level = 0;
    BOOL enabled = False;
    if (!DPMSInfo(dpy_, &level, &enabled)) return;
    on = !enabled || level == DPMSModeOn;
  }
  if (on == display_on_) return;
  display_on_ = on;
  log_line("display %s: %s gos=%llu frames=%llu", on ? "on" : "off", on ? "resuming" : "pausing the host (no GOs)",
           host_ ? (unsigned long long)host_->gos_sent() : 0ull, host_ ? (unsigned long long)host_->frames() : 0ull);
  if (on) resumed_at_ = Clock::now();
}

void App::rotate() {
  rotate_at_.reset();
  if (!rotation_ || rotation_->size() < 2 || exiting_ || gave_up_) return;
  const auto now = Clock::now();
  if (!display_on_ && !clock_) {
    rotate_at_ = now + std::chrono::seconds(opt_.cycle_s);
    return;
  }
  // A game in progress is not switched away (AFTERDAR.SCR 0x40190b): the
  // switch waits, re-checked every second, unless the module says it may
  // be rotated (INTERACTION.md §4.2). The clock's order waits so while the
  // display is off too, and then goes to the clock's module, where the
  // other monitors' players are.
  const OwnerStatus st = owner_status();
  const bool game = st.interactive() && !st.rotate_ok();
  if (game || !display_on_) {
    if (!rotate_waiting_) log_line("rotate-wait: %s", game ? "the module is interactive" : "the display is off");
    rotate_waiting_ = true;
    rotate_at_ = now + kRotateRetry;
    return;
  }
  rotate_waiting_ = false;
  std::string next;
  if (clock_) {
    const int64_t slot = clock_slot(wall_ms(), slot_ms());
    if (slot == clock_->slot()) {   // not there yet on the wall clock
      schedule_change();
      return;
    }
    // A module the order reached by skipping plays on while it plays well.
    const std::string playing = clock_->current();
    next = clock_->go_to(slot, host_ && !host_->ended() && host_->frames() > 0);
    if (next == playing && (host_ || respawn_pending_)) {
      log_line("rotate: %s plays on", next.c_str());
      schedule_change();
      return;
    }
  } else {
    next = rotation_->next();
  }
  log_line("rotate -> %s", next.c_str());
  kill_host();
  // Black between modules, as the original randomizer did (a "could not be
  // started" stays until a frame replaces it, as the Windows saver's does).
  if (start_failed_.empty()) pres_.clear();
  restart_.new_module();
  respawn_pending_ = false;
  spawn();
  if (clock_) schedule_change();
  else rotate_at_ = now + std::chrono::seconds(opt_.cycle_s);
}

// The clock's slots: --cycle seconds, or the default's with --cycle 0 (the
// module is then chosen as with the default, and kept).
int64_t App::slot_ms() const { return (int64_t)(opt_.cycle_s > 0 ? opt_.cycle_s : kDefaultCycleS) * 1000; }

// The clock's next change: when its slot ends on the wall clock, timed on
// the loop's steady clock, and a moment later, so that the wall clock is in
// the next slot by then (rotate() waits for it if it isn't).
void App::schedule_change() {
  const int64_t end = slot_start(clock_->slot() + 1, slot_ms());
  rotate_at_ = Clock::now() + milliseconds(std::max<int64_t>(end - wall_ms(), 0)) + kChangeMargin;
  log_line("rotation: the clock's slot %lld, until %s", (long long)clock_->slot(), time_of_day(end).c_str());
}

void App::start_probe() {
  SpawnSpec s;
  s.program = wine_;
  s.argv = {wine_, host_exe_, "--capabilities"};
  s.pipe_stdin = false;
  s.pipe_stderr = false;
  add_wine_env(s.env);
  add_sound_env(s.env, sound_for(false, 0, HostRole::tool, false, true));
  // A preview's probe as its host: it may be the one that starts the Wine
  // server and Wine's own processes the host then uses.
  s.nice = role_ == HostRole::preview ? kPreviewNice : 0;
  std::string err;
  if (!spawn_child(s, probe_, &err)) {
    log_line("host capabilities: %s", err.c_str());
    return;
  }
  probing_ = true;
  probe_deadline_ = Clock::now() + kProbeTimeout;
}

void App::poll_probe(const std::vector<pollfd>& fds, size_t idx) {
  if (idx >= fds.size() || !fds[idx].revents || probe_.out < 0) return;
  char buf[4096];
  for (;;) {
    ssize_t n = read(probe_.out, buf, sizeof(buf));
    if (n > 0) {
      if (probe_out_.size() < 65536) probe_out_.append(buf, (size_t)n);
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
    close(probe_.out);
    probe_.out = -1;
    finish_probe(true);
    return;
  }
}

void App::finish_probe(bool answered) {
  if (!probing_) return;
  probing_ = false;
  if (probe_.out >= 0) {
    close(probe_.out);
    probe_.out = -1;
  }
  if (probe_.pid > 0) {
    int st;
    if (!reap_child(probe_.pid, &st)) {
      // It printed and closed stdout, or timed out: either way it is done.
      kill(probe_.pid, SIGKILL);
      while (waitpid(probe_.pid, &st, 0) < 0 && errno == EINTR) {
      }
    }
    probe_.pid = -1;
  }
  caps_ = answered ? parse_capabilities(probe_out_) : HostCapabilities{};
  log_line("host capabilities: %s", caps_.known ? caps_.line.c_str() : "(no answer)");
  // A Num Lock change made while it was awaited goes out now.
  check_numlock();
  if (caps_gate_) {
    caps_gate_ = false;
    try_start();
  }
}

// ---- input (INTERACTION.md §4) -----------------------------------------------------

OwnerStatus App::owner_status() const {
  OwnerStatus st;
  if (!host_ || host_->ended()) return st;
  st.running = true;
  st.have = host_->status(&st.rec);
  return st;
}

uint64_t App::send_input(const std::string& line) {
  if (!host_ || host_->ended()) return 0;
  return host_->send_input(line);
}

PointI App::pointer_root() {
  Window root_ret, child;
  int rx = 0, ry = 0, wx = 0, wy = 0;
  unsigned mask = 0;
  if (dpy_ && XQueryPointer(dpy_, DefaultRootWindow(dpy_), &root_ret, &child, &rx, &ry, &wx, &wy, &mask)) {
    return PointI{rx, ry};
  }
  return baseline_;
}

// The pointer in the host's screen: mapped into the letterbox of that
// host's emulated screen in the window as it is now (INTERACTION.md §4.2,
// "MOUSE coordinates"; the Windows saver's fit of screen_.emu).
PointI App::mapped_pointer() const {
  const SizeI emu = host_screen_.w > 0 && host_screen_.h > 0 ? host_screen_ : SizeI{640, 480};
  const RectI fit = host_stretch_ ? RectI{0, 0, pres_.width(), pres_.height()}
                                  : fit_rect(emu.w, emu.h, pres_.width(), pres_.height());
  return map_to_frame(pointer_win_, fit, emu);
}

int App::caps_state() {
  if (!xkb_ || !dpy_) return -1;
  XkbStateRec s;
  if (XkbGetState(dpy_, XkbUseCoreKbd, &s) != Success) return -1;
  return (s.locked_mods & LockMask) ? 1 : 0;
}

int App::numlock_state() {
  if (!xkb_ || !dpy_ || !numlock_mask_) return -1;
  XkbStateRec s;
  if (XkbGetState(dpy_, XkbUseCoreKbd, &s) != Success) return -1;
  return (s.locked_mods & numlock_mask_) ? 1 : 0;
}

// A lock change goes only to a host that takes input, as the Windows saver
// sends it only to its input owner: a preview (--window-id) and XScreenSaver's
// window (--root) take none (a /p saver has no owner), so their hosts hear no
// CAPS or NUMLOCK line, which would start a module's game there. They still
// get ADCAPS and ADNUMLOCK at spawn, as a /p host does.
void App::check_caps() {
  if (exiting_ || input_mode_ == InputMode::none || !host_ || host_->ended()) return;
  const int now = caps_state();
  if (now < 0 || caps_sent_ == now) return;
  if (uint64_t n = host_->send_input(caps_line(now != 0))) {
    caps_sent_ = now;
    log_line("input: caps %d -> host (n=%llu)", now, (unsigned long long)n);
  }
}

void App::check_numlock() {
  // Only a host that takes input (check_caps) and numbers NUMLOCK lines
  // hears one (status.h).
  if (exiting_ || input_mode_ == InputMode::none || !caps_.takes_numlock_lines() || !host_ || host_->ended()) return;
  const int now = numlock_state();
  if (now < 0 || numlock_sent_ == now) return;
  if (uint64_t n = host_->send_input(numlock_line(now != 0))) {
    numlock_sent_ = now;
    log_line("input: numlock %d -> host (n=%llu)", now, (unsigned long long)n);
  }
}

void App::on_key_event(XKeyEvent& xe, bool down) {
  const KeySym sym0 = xkb_ ? XkbKeycodeToKeysym(dpy_, (KeyCode)xe.keycode, XkbGroupForCoreState(xe.state), 0)
                           : XLookupKeysym(&xe, 0);
  on_key(vk_for_key(dpy_, &xe, xkb_), down, is_system_key(sym0, xe.state));
}

void App::on_key(int vk, bool down, bool sys) {
  if (exiting_) return;
  const InputEvent ev{sys ? (down ? InputKind::syskey_down : InputKind::syskey_up)
                          : (down ? InputKind::key_down : InputKind::key_up),
                      vk};
  uint64_t n = 0;
  if (!sys) {
    // The host hears the key first, then whether Caps Lock or Num Lock
    // changed (checked on both down and up: whenever X flips a lock).
    if (vk) n = send_input(key_line(vk, down));
    check_caps();
    check_numlock();
  }
  if (input_mode_ == InputMode::window) {
    if (down && !sys && (vk == VK_ESCAPE || vk == 'Q') && !owner_status().interactive()) {
      request_exit(kExitOk, exit_reason(ev));
    }
    return;
  }
  evaluate(ev, n);
}

void App::on_button(unsigned xbutton, bool down, int wx, int wy) {
  if (exiting_) return;
  if (xbutton >= 4 && xbutton <= 7) {
    // A wheel notch comes as a press and a release of buttons 4-7.
    if (down && input_mode_ == InputMode::saver) evaluate(InputEvent{InputKind::wheel}, 0);
    return;
  }
  // X: 1 left, 2 middle, 3 right; MOUSE: 1 left, 2 right, 4 middle (8, 9
  // and beyond: side buttons, which press nothing the modules read).
  const uint32_t bit = xbutton == 1 ? 1u : xbutton == 3 ? 2u : xbutton == 2 ? 4u : 0u;
  if (bit) buttons_ = down ? (buttons_ | bit) : (buttons_ & ~bit);
  pointer_win_ = {wx, wy};
  const PointI f = mapped_pointer();
  const uint64_t n = send_input(mouse_line(f.x, f.y, buttons_));
  if (input_mode_ == InputMode::window) return;
  evaluate(InputEvent{down ? InputKind::button_down : InputKind::button_up}, n);
}

void App::on_motion(int rx, int ry, int wx, int wy) {
  if (exiting_) return;
  pointer_win_ = {wx, wy};
  const PointI f = mapped_pointer();
  const uint64_t n = send_input(mouse_line(f.x, f.y, buttons_));
  if (input_mode_ == InputMode::window) return;
  // One move decision at a time: the held one covers this move too.
  for (const PendingHold& h : holds_) {
    if (h.ev.kind == InputKind::move) return;
  }
  const long dx = rx - baseline_.x, dy = ry - baseline_.y;
  evaluate(InputEvent{InputKind::move, 0, std::sqrt((double)dx * dx + (double)dy * dy)}, n, dx, dy);
}

// An event's pointer in the player's window: as the event gives it on the
// window itself, from its root position on the black windows over the
// other monitors.
PointI App::event_point(Window w, int x, int y, int x_root, int y_root) {
  if (w == pres_.window()) return PointI{x, y};
  return pres_.root_to_window(PointI{x_root, y_root});
}

// A move anywhere counts (§4.2: past the threshold from the baseline, the
// saver ends), also where no window of the player's hears it: over a window
// another program keeps above them (a panel), or a part of the screen no
// monitor's window covers. Looked at every 250 ms while no game plays (a
// game confines the pointer to its frame).
void App::check_pointer() {
  if (exiting_ || input_mode_ != InputMode::saver || !mapped_ || was_interactive_) return;
  for (const PendingHold& h : holds_) {
    if (h.ev.kind == InputKind::move) return;
  }
  const PointI p = pointer_root();
  const double dx = p.x - baseline_.x, dy = p.y - baseline_.y;
  if (std::sqrt(dx * dx + dy * dy) <= kMoveThreshold) return;
  log_line("input: the pointer is at %d,%d, %.0f px from %d,%d", p.x, p.y, std::sqrt(dx * dx + dy * dy), baseline_.x,
           baseline_.y);
  const PointI w = pres_.root_to_window(p);
  on_motion(p.x, p.y, w.x, w.y);
}

void App::evaluate(const InputEvent& ev, uint64_t n, long dx, long dy) {
  if (exiting_) return;
  const auto now = InputClock::now();
  const OwnerStatus st = owner_status();
  const Verdict v = decide(ev, st, HoldSeqs{n, false, now}, now);
  const std::string reason = exit_reason(ev, dx, dy);
  if (v == Verdict::exit) {
    input_exit(reason);
  } else if (v == Verdict::hold) {
    holds_.push_back({ev, HoldSeqs{n, true, now}, generation_, reason});
    log_line("input: hold %s n=%llu applied=%llu eaten=%llu flags=0x%x", reason.c_str(), (unsigned long long)n,
             (unsigned long long)st.rec.input_applied, (unsigned long long)st.rec.input_eaten, st.rec.flags);
  }
}

void App::poll_status() {
  // A preview takes no input and ends only when its window goes.
  if (exiting_ || opt_.mode == WindowMode::embed) return;
  const OwnerStatus st = owner_status();
  if (st.wake()) {
    input_exit("wake");
    return;
  }
  const bool now_interactive = st.interactive();
  if (now_interactive != was_interactive_) {
    was_interactive_ = now_interactive;
    log_line("play %s source=%u", now_interactive ? "starts" : "ends", (unsigned)st.rec.source);
    // After a game the move threshold counts from where the pointer is now.
    if (!now_interactive) baseline_ = pointer_root();
  }
  // The pointer: hidden, except while a game plays and its module asks for
  // one; confined to the window while a game plays.
  if (input_mode_ == InputMode::saver) pres_.set_cursor_visible(was_interactive_ && st.cursor());
  update_clip();
  const auto now = InputClock::now();
  for (size_t i = 0; i < holds_.size();) {
    PendingHold& h = holds_[i];
    OwnerStatus hs = st;
    if (h.generation != generation_) hs.running = false;   // its host is gone
    const Verdict v = decide(h.ev, hs, h.seqs, now);
    if (v == Verdict::exit) {
      input_exit(h.reason + " (after hold " +
                 std::to_string(std::chrono::duration_cast<milliseconds>(now - h.seqs.since).count()) + " ms)");
      return;
    }
    if (v == Verdict::forward) {
      log_line("input: kept %s n=%llu applied=%llu eaten=%llu flags=0x%x", h.reason.c_str(),
               (unsigned long long)h.seqs.n, (unsigned long long)st.rec.input_applied,
               (unsigned long long)st.rec.input_eaten, st.rec.flags);
      // A move the module took resets the threshold.
      if (h.ev.kind == InputKind::move) baseline_ = pointer_root();
      holds_.erase(holds_.begin() + (long)i);
      continue;
    }
    ++i;
  }
}

void App::update_clip() {
  if (input_mode_ != InputMode::saver || !dpy_ || !pres_.owns_window()) return;
  const bool want = was_interactive_ && !exiting_;
  if (want == clip_active_) return;
  if (want) {
    // Confined to the frame (the Windows saver's ClipCursor to the frame
    // rectangle): an InputOnly window over the letterbox's picture, which
    // follows it when the window's size changes.
    const Window w = pres_.window();
    const Window confine = pres_.clip_window();
    clip_active_ = XGrabPointer(dpy_, w, True, ButtonPressMask | ButtonReleaseMask | PointerMotionMask, GrabModeAsync,
                                GrabModeAsync, confine ? confine : w, None, CurrentTime) == GrabSuccess;
    if (clip_active_) {
      const RectI r = pres_.frame_rect();
      log_line("clip set: the pointer is confined to the frame %d,%d %dx%d", r.x, r.y, r.w, r.h);
      clip_failed_logged_ = false;
    } else {
      pres_.hide_clip_window();
      if (!clip_failed_logged_) {
        log_line("clip: the pointer could not be confined (another program holds it); trying again");
        clip_failed_logged_ = true;
      }
    }
  } else {
    XUngrabPointer(dpy_, CurrentTime);
    pres_.hide_clip_window();
    XFlush(dpy_);
    clip_active_ = false;
    log_line("clip released");
  }
}

void App::input_exit(const std::string& what) {
  if (exiting_) return;
  log_line("input: %s", what.c_str());
  request_exit(kExitOk, what);
}

void App::request_exit(int code, const std::string& why) {
  if (exiting_) return;
  exiting_ = true;
  exit_code_ = code;
  log_line("exit requested: %s", why.c_str());
  update_clip();   // the pointer goes free at once
  // The host hears QUIT before anything else happens (AUDIO.md §9), so its
  // sound stops while the window goes; kill_host() then gives it the grace.
  if (host_) host_->request_quit();
}

void App::emergency_stop() {
  log_line("the X connection was lost");
  if (host_) host_->stop_gracefully();
  host_.reset();
  if (probe_.pid > 0) {
    kill(probe_.pid, SIGKILL);
    int st;
    waitpid(probe_.pid, &st, 0);
  }
}

void App::on_x_error(const XErrorEvent& e) {
  char text[128] = "";
  if (dpy_) XGetErrorText(dpy_, e.error_code, text, sizeof(text));
  log_line("X error %d (%s), request %d, resource 0x%lx", e.error_code, text, e.request_code,
           (unsigned long)e.resourceid);
  // The window we draw in went away (XScreenSaver's, or the preview's).
  if ((e.error_code == BadWindow || e.error_code == BadDrawable) && e.resourceid == pres_.window()) target_gone_ = true;
}

}  // namespace

int player_main(int argc, char** argv) {
  App app;
  g_app = &app;
  const int r = app.run(argc, argv);
  g_app = nullptr;
  return r;
}

}  // namespace lad
