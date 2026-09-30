#include "options.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include "log.h"

namespace lad {

namespace {

bool parse_long(const char* s, long lo, long hi, long* out) {
  if (!s || !*s) return false;
  char* end = nullptr;
  errno = 0;
  long v = strtol(s, &end, 10);
  if (errno || !end || *end || v < lo || v > hi) return false;
  *out = v;
  return true;
}

}  // namespace

int parse_options(int argc, char** argv, Options& o) {
  auto value = [&](int& i, const std::string& name) -> const char* {
    if (i + 1 >= argc) {
      err_line("%s needs a value (see --help)", name.c_str());
      return nullptr;
    }
    return argv[++i];
  };
  auto number = [&](int& i, const std::string& name, long lo, long hi, int* out) {
    const char* v = value(i, name);
    long n = 0;
    if (!v) return false;
    if (!parse_long(v, lo, hi, &n)) {
      err_line("%s takes a whole number from %ld to %ld, not \"%s\"", name.c_str(), lo, hi, v);
      return false;
    }
    *out = (int)n;
    return true;
  };
  // One module at a time, named by the argument or --module (the same one
  // named twice is one).
  auto set_module = [&](const char* m) {
    if (!o.module.empty() && o.module != m) {
      err_line("one module at a time (\"%s\" and \"%s\")", o.module.c_str(), m);
      return false;
    }
    o.module = m;
    return true;
  };
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "-h" || a == "--help") {
      o.help = true;
    } else if (a == "-v" || a == "--version") {
      o.version = true;
    } else if (a == "-w" || a == "--window") {
      o.want_window = true;
    } else if (a == "-f" || a == "--fullscreen") {
      o.want_fullscreen = true;
    } else if (a == "-root" || a == "--root") {
      o.want_root = true;
    } else if (a == "-window-id" || a == "--window-id") {
      const char* v = value(i, a);
      if (!v) return kExitUsage;
      unsigned long id = 0;
      if (!parse_window_id(v, &id)) {
        err_line("%s takes a window id (decimal or 0x hex), not \"%s\"", a.c_str(), v);
        return kExitUsage;
      }
      o.window_id = id;
    } else if (a == "-r" || a == "--random") {
      o.random = true;
    } else if (a == "--cycle") {
      if (!number(i, a, 0, 86400, &o.cycle_s)) return kExitUsage;
    } else if (a == "-l" || a == "--list") {
      o.list = true;
    } else if (a == "-s" || a == "--scale") {
      if (!number(i, a, 1, 8, &o.scale)) return kExitUsage;
    } else if (a == "--lines") {
      const char* v = value(i, a);
      if (!v) return kExitUsage;
      if (strcmp(v, "480") != 0 && strcmp(v, "720") != 0) {
        err_line("--lines takes 480 or 720, not \"%s\"", v);
        return kExitUsage;
      }
      o.lines = atoi(v);
    } else if (a == "--fps") {
      if (!number(i, a, 1, 240, &o.fps)) return kExitUsage;
    } else if (a == "--sound") {
      o.sound = true;
    } else if (a == "--no-sound") {
      o.sound = false;
    } else if (a == "--volume") {
      if (!number(i, a, 0, 100, &o.volume)) return kExitUsage;
    } else if (a == "--assets-dir") {
      const char* v = value(i, a);
      if (!v) return kExitUsage;
      o.assets_dir = v;
    } else if (a == "--module") {
      // The module as an option: XScreenSaver's settings keep an option
      // on a programs line they save (a <string> field), where they drop
      // a bare name. Empty names none, as leaving it out does.
      const char* v = value(i, a);
      if (!v) return kExitUsage;
      if (*v && !set_module(v)) return kExitUsage;
    } else if (a == "--test-pattern") {
      o.test_pattern = true;
    } else if (a == "--import") {
      o.import = true;
      for (int j = i + 1; j < argc; ++j) o.import_args.push_back(argv[j]);
      break;
    } else if (a == "--verbose") {
      o.verbose = true;
    } else if (a == "--stall-timeout") {
      if (!number(i, a, 500, 3600000, &o.stall_ms)) return kExitUsage;
    } else if (a == "--first-frame-timeout") {
      if (!number(i, a, 500, 3600000, &o.first_frame_ms)) return kExitUsage;
    } else if (a == "--test-display-off") {
      if (!number(i, a, 1, 600000, &o.test_display_off_ms)) return kExitUsage;
    } else if (!a.empty() && a[0] == '-') {
      err_line("unknown option %s (see --help)", a.c_str());
      return kExitUsage;
    } else if (!set_module(argv[i])) {
      return kExitUsage;
    }
  }
  // XScreenSaver names its window in $XSCREENSAVER_WINDOW for the hacks it
  // runs: with no mode given, that is -root's (a window of our own would
  // sit unseen beneath XScreenSaver's, playing its sound).
  const char* xss = getenv("XSCREENSAVER_WINDOW");
  o.root_from_env = !o.window_id && !o.want_root && !o.want_window && !o.want_fullscreen && xss && *xss;
  if (o.window_id) o.mode = WindowMode::embed;
  else if (o.want_root || o.root_from_env) o.mode = WindowMode::root;
  else if (o.want_window && !o.want_fullscreen) o.mode = WindowMode::window;
  else o.mode = WindowMode::fullscreen;
  return -1;
}

}  // namespace lad
