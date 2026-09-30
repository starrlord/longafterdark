// The monitors of an X screen, as RandR tells them, for what the player
// needs of them: which one is the primary (the Windows saver's sound goes
// with the primary monitor's window alone, AUDIO.md §9), where the
// full-screen window goes, and which others it leaves to black windows.
#pragma once

#include <X11/Xlib.h>

#include <string>
#include <vector>

#include "geometry.h"

namespace lad {

struct MonitorInfo {
  RectI rect;             // root-window coordinates
  std::string how;        // for the log: "RandR monitor DP-1 (primary)", "the whole screen (no RandR)"
  bool primary = false;   // (list_monitors) the one primary_monitor() picks
};

// The primary monitor of X screen `screen`: RandR 1.5's monitor list, its
// primary, else its first (the X server lists the primary first); else
// (a server before RandR 1.5) RandR 1.3's primary output's CRTC, else the
// first CRTC driving an output; else (no RandR) the whole screen.
MonitorInfo primary_monitor(Display* d, int screen);

// Every monitor of X screen `screen`, the primary among them flagged (as
// primary_monitor() picks it): RandR 1.5's monitor list; else RandR 1.3's
// CRTCs that drive an output; else the whole screen. A rectangle listed
// twice (a clone) is one monitor.
std::vector<MonitorInfo> list_monitors(Display* d, int screen);

// The monitors of `all` that a full-screen window over `main` leaves for
// black windows, as the Windows saver's "primary monitor only" leaves the
// other monitors black: those that don't overlap `main` (a clone of it, or
// a monitor sharing a part of it, shows that window already, and a window
// over it would hide the frames), each rectangle once. Pure.
std::vector<RectI> monitors_to_cover(const std::vector<MonitorInfo>& all, const RectI& main);

}  // namespace lad
