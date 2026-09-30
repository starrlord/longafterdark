// The player's command line (longafterdark --help), parsed.
#pragma once

#include <string>
#include <vector>

#include "present.h"
#include "sound.h"

namespace lad {

inline constexpr int kExitOk = 0, kExitError = 1, kExitUsage = 2;

struct Options {
  WindowMode mode = WindowMode::fullscreen;
  bool want_window = false, want_fullscreen = false, want_root = false;
  bool root_from_env = false;   // -root because $XSCREENSAVER_WINDOW is set and no mode was given
  unsigned long window_id = 0;
  bool random = false;
  int cycle_s = 300;
  bool list = false;
  int scale = 0;
  int lines = 480;   // After Dark modules' screen: 480 or 720 lines (the Windows Resolution setting)
  int fps = 60;
  bool sound = true;
  int volume = kDefaultVolume;
  std::string assets_dir;
  std::string module;   // the module named (by the argument or --module), "" for none
  bool test_pattern = false;
  bool import = false;
  std::vector<std::string> import_args;
  bool verbose = false;
  int stall_ms = 20000;
  int first_frame_ms = 90000;
  int test_display_off_ms = 0;
  bool help = false, version = false;
};

// -1 when the options are good; else the exit code (kExitUsage), with the
// reason on stderr. The mode follows from the options and
// $XSCREENSAVER_WINDOW (-root when it is set and no mode is given).
int parse_options(int argc, char** argv, Options& o);

}  // namespace lad
