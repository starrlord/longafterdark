// adhostwin — the Windows host process: one per running After Dark module.
//
//   adhostwin.exe <module-path> [KEY=VALUE …]
//   adhostwin.exe --test-pattern [KEY=VALUE …]
//   adhostwin.exe --capabilities
//   adhostwin.exe --configure <module> --button <slot> [--owner <hwnd>] [KEY=VALUE …]
//
// Picks the lane from the module's header (PE32 -> the AD 4 lane, NE -> the
// Classic lane, which runs After Dark 2.x/3.x and Intermission modules), then
// hands everything to run_host(): frames out on stdout, commands in on stdin,
// per DESIGN.md §1. KEY=VALUE arguments override the environment
// (ADSTREAM=1, ADFRAMES=100, …). Diagnostics go to stderr only.
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstdlib>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "adw/core/env.h"
#include "adw/core/host.h"
#include "adw/core/lane.h"
#include "adw/core/log.h"
#include "adw/core/protocol.h"
#include "adw/core/status.h"
#include "adw/core/text.h"

using namespace adw;

namespace {

void usage() {
  log("usage: adhostwin.exe <module (.AD, .IMX, .SCR) | --test-pattern> [KEY=VALUE ...]");
  log("       adhostwin.exe --capabilities");
  log("       adhostwin.exe --configure <module> --button <slot> [--owner <hwnd>] [KEY=VALUE ...]");
  log("  env/KEY: ADSTREAM=1 ADSCREENW/ADSCREENH ADFRAMES=<n> ADFBHASH=1 ADOUT=<dir>");
  log("           ADCVSET=<i>=<v>,... ADSEED=<n>|random ADNOPACE=1 AD_ASSETS_DIR ADTRACE=<cats>");
  log("           ADCAPS=0|1 ADNUMLOCK=0|1 ADSTATE=<dir>|:memory: ADSTATUSHANDLE=<n> ADSTATUSLOG=1");
  log("           AD_LOCALAPPDATA=<dir> (instead of %%LOCALAPPDATA%% for the data folder)");
  log("  sound:   ADSOUND=1 ADAUDIOOUT=<file.wav> ADVOLUME=0..100 ADAUDIORATE=<hz> ADAUDIOLATENCYMS=<ms>");
  log("           ADAUDIOLIVE=0 ADMIDI=0 ADMIDIDEV=<n> ADMIDIBASE=1 (--test-pattern: ADTESTAUDIO=1)");
  log("  lanes:   ne16: ADNE16KIND=auto|ad3|imx ADNE16READER=auto|imq|native ADNE16BRIDGE=auto|oldmod16|native");
  log("                 ADNE16IMXSPEED=1..100 (an Intermission module's machine speed, percent) ...");
  log("           (the lane knobs: host/ne16/lane.hh, host/pe32/lane.hh)");
  log("  stdin:   GO | SET <i> <v> | KEY <vk> <0|1> | CAPS <0|1> | NUMLOCK <0|1> | MOUSE <x> <y> <buttons> | QUIT");
  log("  see host/core/README.md");
}

// The environment's warnings (malformed values that fell back to defaults),
// on stderr.
void log_env_warnings(const Env& env) {
  for (const std::string& w : env.warnings) log("%s", w.c_str());
}

// One line on stdout, outside any frame stream (--capabilities, --configure).
void print_stdout_line(const std::string& line) {
  std::string out = line + "\n";
  fwrite(out.data(), 1, out.size(), stdout);
  fflush(stdout);
}

// Decimal or 0x hex, the whole string.
bool parse_u64_arg(const std::string& s, uint64_t& out) {
  if (s.empty() || s[0] == '-') return false;
  int base = (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ? 16 : 10;
  char* end = nullptr;
  errno = 0;
  unsigned long long v = strtoull(s.c_str(), &end, base);
  if (errno || !end || *end) return false;
  out = v;
  return true;
}

struct LinkedLane {
  const char* name;
  std::unique_ptr<Lane> (*make)();
};

// The lanes this adhostwin was built with.
std::vector<LinkedLane> linked_lanes() {
  std::vector<LinkedLane> v;
#if ADW_HAVE_LANE_PE32
  v.push_back({"pe32", &make_pe32_lane});
#endif
#if ADW_HAVE_LANE_NE16
  v.push_back({"ne16", &make_ne16_lane});
#endif
  return v;
}

// "lanes=pe32,ne16 configure=pe32,ne16 abis=afterdark,intermission,scrnsave
// status=1 state=1 seed=1 audio=1 numlock=1" (§3.3; audio: AUDIO.md §4; numlock: the
// NUMLOCK line and ADNUMLOCK are understood): only what this build has.
// abis= is the union of the linked lanes' Lane::abis(), in lane order.
std::string capabilities_line() {
  std::string lanes, configure;
  std::vector<std::string> abis;
  for (const LinkedLane& l : linked_lanes()) {
    if (!lanes.empty()) lanes += ',';
    lanes += l.name;
    std::unique_ptr<Lane> lane = l.make();
    if (lane && lane->can_configure()) {
      if (!configure.empty()) configure += ',';
      configure += l.name;
    }
    if (lane) {
      for (const std::string& a : lane->abis()) {
        if (std::find(abis.begin(), abis.end(), a) == abis.end()) abis.push_back(a);
      }
    }
  }
  std::string abi_list;
  for (const std::string& a : abis) abi_list += (abi_list.empty() ? "" : ",") + a;
  return "lanes=" + lanes + " configure=" + configure + " abis=" + abi_list + " status=1 state=1 seed=1 audio=1 numlock=1";
}

bool file_exists(const std::string& p) {
  DWORD a = GetFileAttributesW(widen(p).c_str());
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// NAME=VALUE with an identifier-shaped NAME (so "C:\a=b\X.AD" stays a path).
bool is_override(const std::string& a) {
  size_t eq = a.find('=');
  if (eq == std::string::npos || eq == 0) return false;
  for (size_t i = 0; i < eq; i++) {
    char c = a[i];
    bool ok = c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (i > 0 && c >= '0' && c <= '9');
    if (!ok) return false;
  }
  return true;
}

bool is_absolute(const std::string& p) {
  return (p.size() >= 2 && p[1] == ':') || (!p.empty() && (p[0] == '\\' || p[0] == '/'));
}

// Picks the lane for a module path (resolving a catalog-relative path under
// the win dir first). Returns an exit code other than kExitOk when there is
// none; `module` is updated to the resolved path.
int pick_lane(const Env& env, std::string& module, std::unique_ptr<Lane>& lane, std::string* why) {
  // Catalog paths ("FILES/AD40/TOASTERS.AD") are relative to <assets>\win.
  if (!file_exists(module) && !is_absolute(module)) {
    std::string alt = env.win_assets_dir() + "\\" + module;
    if (file_exists(alt)) module = alt;
  }
  ModuleProbe probe = probe_module(module);
  switch (probe.kind) {
    case LaneKind::unreadable:
      *why = module + ": " + probe.detail;
      return kExitUsage;
    case LaneKind::unsupported:
      *why = module + ": not a module this host can run (" + probe.detail + ")";
      return kExitUsage;
    case LaneKind::pe32:
#if ADW_HAVE_LANE_PE32
      lane = make_pe32_lane();
#endif
      break;
    case LaneKind::ne16:
#if ADW_HAVE_LANE_NE16
      lane = make_ne16_lane();
#endif
      break;
  }
  if (!lane) {
    // Only a partial build (AD_COMPONENTS without host/pe32 or host/ne16)
    // lacks a lane; exit 3 tells the saver and thumbnails so.
    *why = module + ": " + probe.detail + " module - this adhostwin was built without the " +
           lane_kind_name(probe.kind) + " lane";
    return kExitLaneMissing;
  }
  return kExitOk;
}

// adhostwin --configure (INTERACTION.md §6.1): run the module's button
// handler once and print one JSON line. Exit 0 shown, 4 nothing, 5 no
// configure support in the lane, 1 error, 2 usage, 3 lane missing.
// The module's dialogs, message boxes and file dialogs are owned by the
// settings window, which lives in another process (INTERACTION.md §6.3).
// When such a window goes away while it is active, Windows does not hand the
// activation back across the process boundary: the settings window would
// drop behind whatever application was under it. So while this process is
// still the foreground one (its window is active as it starts to hide), it
// gives the foreground to the owner itself, enabled: the settings window
// re-enables itself when this process exits anyway.
HWND g_configure_owner = nullptr;
HHOOK g_configure_hook = nullptr;

LRESULT CALLBACK configure_owner_hook(int code, WPARAM wp, LPARAM lp) {
  if (code == HC_ACTION && g_configure_owner) {
    const auto* m = reinterpret_cast<const CWPSTRUCT*>(lp);
    if (m && m->message == WM_WINDOWPOSCHANGING) {
      const auto* pos = reinterpret_cast<const WINDOWPOS*>(m->lParam);
      if (pos && (pos->flags & SWP_HIDEWINDOW) && IsWindowVisible(m->hwnd) && GetForegroundWindow() == m->hwnd &&
          GetWindow(m->hwnd, GW_OWNER) == g_configure_owner && IsWindow(g_configure_owner)) {
        // A disabled window cannot take the activation (the settings window
        // disables itself for the run; the module may hide its dialog before
        // ending the modal loop that re-enables it).
        if (!IsWindowEnabled(g_configure_owner)) EnableWindow(g_configure_owner, TRUE);
        BOOL ok = SetForegroundWindow(g_configure_owner);
        trace("configure", "window %p goes: the foreground to the owner %p (%d)", (void*)m->hwnd,
              (void*)g_configure_owner, ok);
      }
    }
  }
  return CallNextHookEx(g_configure_hook, code, wp, lp);
}

struct ConfigureOwnerFocus {
  explicit ConfigureOwnerFocus(uint64_t owner) {
    HWND h = reinterpret_cast<HWND>(uintptr_t(owner));
    if (!h || !IsWindow(h)) return;
    g_configure_owner = h;
    g_configure_hook = SetWindowsHookExW(WH_CALLWNDPROC, configure_owner_hook, nullptr, GetCurrentThreadId());
  }
  ~ConfigureOwnerFocus() {
    if (g_configure_hook) UnhookWindowsHookEx(g_configure_hook);
    g_configure_hook = nullptr;
    g_configure_owner = nullptr;
  }
  ConfigureOwnerFocus(const ConfigureOwnerFocus&) = delete;
  ConfigureOwnerFocus& operator=(const ConfigureOwnerFocus&) = delete;
};

int run_configure(std::string module, const std::string& button_arg, const std::string& owner_arg,
                  const std::vector<std::pair<std::string, std::string>>& overrides) {
  auto fail = [](int code, const std::string& message) {
    log("configure: %s", message.c_str());
    print_stdout_line(configure_json(ConfigureResult::failed, 0, message, {}));
    return code;
  };
  uint64_t slot = 0, owner = 0;
  if (button_arg.empty() || !parse_u64_arg(button_arg, slot) || slot > 0xFFFF)
    return fail(kExitUsage, "--button needs a control index (0..65535), got '" + button_arg + "'");
  if (!owner_arg.empty() && !parse_u64_arg(owner_arg, owner))
    return fail(kExitUsage, "--owner needs a window handle (decimal or 0x hex), got '" + owner_arg + "'");

  Env env = Env::from_process(overrides);
  // The module's own settings persist in this mode (§6.1): an unset ADSTATE
  // means the user's state folder, not memory.
  env.use_configure_state_default();
  log_env_warnings(env);
  set_trace_categories(env.trace);

  std::unique_ptr<Lane> lane;
  std::string why;
  int code = pick_lane(env, module, lane, &why);
  if (code != kExitOk) return fail(code, why);

  ConfigureRequest req;
  req.slot = int(slot);
  req.owner = owner;
  log("configure %s button %d owner 0x%llx, lane %s, state %s", module.c_str(), req.slot,
      (unsigned long long)owner, lane->name(), env.state_persistent() ? env.state_root.c_str() : "(memory)");
  std::string json;
  int exit_code = 0;
  {
    ConfigureOwnerFocus focus(owner);
    exit_code = configure_module(*lane, module, env, req, &json);
  }
  print_stdout_line(json);
  log("configure exit %d", exit_code);
  return exit_code;
}

}  // namespace

int main() {
  // A host is a background worker of the screen saver: a fault must end the
  // process (the front-end sees EOF and moves on), never park a WER dialog on
  // the user's desktop.
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  _setmode(_fileno(stderr), _O_BINARY);  // "\n" line ends for machine-parsed FBHASH lines
  set_log_prefix("adhostwin");

  int wargc = 0;
  LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
  std::vector<std::string> args;
  for (int i = 1; wargv && i < wargc; i++) args.push_back(narrow(wargv[i]));
  if (wargv) LocalFree(wargv);

  bool test_pattern = false, capabilities = false, configure = false;
  bool have_button = false, have_owner = false;
  std::string module, button_arg, owner_arg;
  std::vector<std::pair<std::string, std::string>> overrides;
  for (size_t i = 0; i < args.size(); i++) {
    const std::string& a = args[i];
    if (a == "--test-pattern") {
      test_pattern = true;
    } else if (a == "--capabilities") {
      capabilities = true;
    } else if (a == "--configure") {
      configure = true;
    } else if (a == "--button" || a == "--owner") {
      if (i + 1 >= args.size()) {
        log("%s needs a value", a.c_str());
        if (configure) print_stdout_line(configure_json(ConfigureResult::failed, 0, a + " needs a value", {}));
        usage();
        return kExitUsage;
      }
      if (a == "--button") {
        have_button = true;
        button_arg = args[++i];
      } else {
        have_owner = true;
        owner_arg = args[++i];
      }
    } else if (a == "--help" || a == "-h" || a == "/?") {
      usage();
      return kExitOk;
    } else if (is_override(a)) {
      size_t eq = a.find('=');
      overrides.emplace_back(a.substr(0, eq), a.substr(eq + 1));
    } else if (module.empty() && !(a.size() > 2 && a[0] == '-' && a[1] == '-')) {
      module = a;
    } else {
      log("unexpected argument '%s'", a.c_str());
      if (configure) print_stdout_line(configure_json(ConfigureResult::failed, 0, "unexpected argument '" + a + "'", {}));
      usage();
      return kExitUsage;
    }
  }

  if (capabilities) {
    if (test_pattern || configure || have_button || have_owner || !module.empty()) {
      log("--capabilities takes no other argument");
      usage();
      return kExitUsage;
    }
    print_stdout_line(capabilities_line());
    return kExitOk;
  }
  if ((have_button || have_owner) && !configure) {
    log("--button/--owner only go with --configure");
    usage();
    return kExitUsage;
  }
  if (configure) {
    if (test_pattern || module.empty() || !have_button) {
      std::string msg = test_pattern     ? "--configure takes a module, not --test-pattern"
                        : module.empty() ? "--configure needs a module"
                                         : "--configure needs --button <slot>";
      log("%s", msg.c_str());
      print_stdout_line(configure_json(ConfigureResult::failed, 0, msg, {}));
      usage();
      return kExitUsage;
    }
    return run_configure(module, button_arg, owner_arg, overrides);
  }

  // Either order of "<module> --test-pattern" is a mistake: running the
  // pattern while the caller believes a module is loaded would mislead.
  if (test_pattern && !module.empty()) {
    log("--test-pattern takes no module (got '%s')", module.c_str());
    usage();
    return kExitUsage;
  }
  if (!test_pattern && module.empty()) {
    usage();
    return kExitUsage;
  }

  Env env = Env::from_process(overrides);
  log_env_warnings(env);
  set_trace_categories(env.trace);
  if (env.seed_random) log("ADSEED=random -> %" PRIu64 " (pass ADSEED=%" PRIu64 " to replay)", env.seed, env.seed);

  std::unique_ptr<Lane> lane;
  if (test_pattern) {
    module = "--test-pattern";
    lane = make_test_pattern_lane();
  } else {
    std::string why;
    int code = pick_lane(env, module, lane, &why);
    if (code != kExitOk) {
      log("%s", why.c_str());
      return code;
    }
  }

  StdoutSink sink;
  FrameSink* frames = nullptr;
  if (env.stream) {
    switch (sink.open(env.stream_force)) {
      case StdoutSink::Open::ok:
        frames = &sink;
        break;
      case StdoutSink::Open::refused_console:
        log("ADSTREAM refused: stdout is a console; pipe it to a front-end (frames are binary)");
        return kExitUsage;
      case StdoutSink::Open::refused_disk:
        log("ADSTREAM refused: stdout is a disk file, which has no backpressure (it would fill the "
            "disk at render speed). Pipe it to a consumer, or set ADSTREAMFORCE=1");
        return kExitUsage;
      case StdoutSink::Open::refused_shared_stderr:
        log("ADSTREAM refused: stdout and stderr are the same handle (or the same pipe, as with 2>&1), "
            "so log lines would corrupt the frame stream. Give stderr its own pipe (or NUL)");
        return kExitUsage;
      case StdoutSink::Open::no_stdout:
        log("ADSTREAM refused: this process has no stdout");
        return kExitUsage;
      case StdoutSink::Open::failed:
        log("ADSTREAM: could not take over stdout (error %lu)", GetLastError());
        return kExitError;
    }
  }

  StatusPublisher status;
  status.open(env);

  StdinReader reader;
  reader.start();
  HostIo io;
  io.commands = reader.kind() == StdinReader::Kind::none ? nullptr : &reader;
  io.frames = frames;
  io.status = status.active() ? &status : nullptr;
  // Only a pipe or a file can already hold a front-end's opening GO; waiting
  // on a console would just stall a person's interactive run.
  io.go_wait = reader.kind() == StdinReader::Kind::pipe || reader.kind() == StdinReader::Kind::file;

  log("lane %s, %dx%d, %s, stdin %s, seed %" PRIu64 "%s%s%s%s", lane->name(), env.screen_w, env.screen_h,
      env.stream ? (env.stream_p6 ? "streaming P6" : "streaming P8") : "headless",
      StdinReader::kind_name(reader.kind()), env.seed,
      env.frames ? (", " + std::to_string(env.frames) + " frames").c_str() : "",
      env.caps_at_start ? ", caps on" : "", env.numlock_at_start ? ", num lock on" : "",
      status.mapped() ? ", status record" : "");
  HostResult r;
  try {
    r = run_host(*lane, module, env, io);
  } catch (const std::exception& e) {
    // run_host contains the lane's exceptions itself; what reaches here is the
    // host's own setup failing, e.g. bad_alloc for a 16384x16384 screen.
    r.exit_code = kExitError;
    r.reason = "host setup failed";
    log("%s", e.what());
  }
  log("exit %d after %" PRIu64 " frames (%s%s)", r.exit_code, r.frames, r.reason,
      r.lockstep ? ", lockstep" : "");
  reader.stop();
  return r.exit_code;
}

