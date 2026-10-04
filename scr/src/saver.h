// The running screen saver: /s (one borderless topmost window per monitor),
// /p (a child of the Display control panel's preview window) and /window (an
// ordinary window of its own, window_mode.h).
#pragma once

#include <string>

#include "args.h"

namespace adw::scr {

// Exit codes. 0 is the normal "user came back" exit; the others only matter
// to the test hooks, which assert which path the saver took.
enum : int {
  kExitOk = 0,
  kExitBadArgs = 1,          // also: a command line it can't run, or a /module it can't play (said so)
  kExitNotImported = 10,     // catalog/assets missing: the message was shown
  kExitHostMissing = 11,     // adhostwin.exe missing: the message was shown
  kExitStartFailed = 12,     // test hook only: a module "could not be started"
  kExitNoneRuns = 13,        // test hook only: the host can run none of the modules imported (said so)
};

int run_saver(const Args& args, void* hinstance);

// A message box: `problem` (a command line the saver can't run, Args::error,
// or a /module it can't play) and how the switches go (usage_message), or,
// with no problem, what every switch does (/help, usage_text).
void show_usage(const std::wstring& problem);

} // namespace adw::scr
