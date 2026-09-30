// Emulated-screen sizing and letterboxing. Pure arithmetic, so the unit tests
// can pin the exact numbers.
#pragma once

#include <compare>
#include <string_view>
#include <vector>

namespace adw::scr {

struct SizeI {
  int w = 0, h = 0;
  bool operator==(const SizeI&) const = default;
  auto operator<=>(const SizeI&) const = default;   // sets of screens (releases.h: first_module_screens)
};

struct RectI {
  int x = 0, y = 0, w = 0, h = 0;
  bool operator==(const RectI&) const = default;
};

// Base 640x480 (the Win95 display the modules were written for), scaled by
// `scale`, widened to `display_aspect` (never narrower than 4:3 — a portrait
// monitor letterboxes instead), both axes snapped to the nearest multiple of 8,
// width capped at twice the scaled 4:3 width so an ultrawide can't explode the
// framebuffer.
SizeI emulated_screen_size(double display_aspect, double scale, int base_w = 640, int base_h = 480);

// The largest rect with the source's aspect that fits in dst_w x dst_h,
// centred (the remainder is letterbox/pillarbox).
RectI fit_rect(int src_w, int src_h, int dst_w, int dst_h);

// ---- a module's emulated screen ----------------------------------------------------
// The one rule for the screen a module's host is given (ADSCREENW /
// ADSCREENH) by everything that starts one for it: the /s windows (a
// rotation's every host), the settings dialog's live preview, its
// thumbnails and its Preview button (a /s). It goes by the module's own
// screen (own_screen: its catalog entry's "screen", else its `abi`,
// catalog.h):
//  * a module without one (After Dark's): the Resolution setting (`scale`)
//    widened to the display, emulated_screen_size.
//  * a module with one: that screen, whatever the display or the Resolution
//    setting. Such a module may compose a fixed scene of that size and put it
//    in the middle (or at the top left) of a larger screen, so a bigger one
//    only adds bars (at 720 lines the scene sat small in the middle); at its
//    own size the presenter's letterbox (fit_rect) scales the frame to fit
//    the display instead, keeping its shape: a 4:3 one (640x480) at its full
//    height on a display at least 4:3 wide (bars at the sides on a
//    widescreen), at its full width on a narrower one (5:4, portrait: bars
//    above and below). Intermission modules (Star Wars Screen Entertainment,
//    "intermission") have 640x480 by their ABI; a catalog entry gives any
//    module one with "screen": "WxH" (Star Trek: The Screen Saver's,
//    ScreamSavers' and Marvel's modules, "640x480": several compose a fixed
//    640x480 scene).
// (/p stays 320x240 for every module: saver.cc.)
struct ModuleScreen {
  SizeI emu;
  // The module's own size (own_screen), which no display or setting
  // changes; false for one that follows the display (After Dark's).
  bool fixed = false;
  bool operator==(const ModuleScreen&) const = default;
};
// A module's own screen: the one its catalog entry gives (`catalog_screen`,
// catalog.h Module::screen, "screen": "WxH"), else the one its ABI has
// (Intermission's 640x480); {0, 0} for a module that follows the display
// (After Dark's, without "screen").
SizeI own_screen(std::string_view abi, SizeI catalog_screen = {});
// The screen of a module whose own screen is `own` (own_screen; {0, 0}: it
// has none and follows the display).
ModuleScreen module_screen(SizeI own, double display_aspect, double scale);
// The same for a catalog entry without "screen": its ABI's rule alone.
ModuleScreen module_screen(std::string_view abi, double display_aspect, double scale);

// The part of a display `w` x `h` px whose picture seeds a host with
// `screen` (the desktop capture, INTERACTION.md §8): all of it, shrunk to a
// screen that follows the display (After Dark's, as before); for a module's
// own size, the part its letterboxed frame covers (fit_rect), so the
// desktop shows where it was.
RectI seed_source(const ModuleScreen& screen, int w, int h);

// One picture of a monitor for a window's first host: the part `src`
// (seed_source) shrunk to `screen`'s emulated size. `same`: an earlier
// picture that is this one (the same part at the same size), whose file it
// shares; -1 for a file of its own.
struct SeedShotPlan {
  ModuleScreen screen;
  RectI src;
  int same = -1;
  bool operator==(const SeedShotPlan&) const = default;
};
// The most pictures of modules' own screens a window gets (plan_seed_shots),
// beside the one of the screen that follows the display.
inline constexpr size_t kMaxOwnSeedShots = 2;
// The pictures of a monitor `mw` x `mh` px for the screens a window's first
// host may be given (one per own screen its first module may have,
// releases.h first_module_screens, through module_screen;
// App::capture_seeds takes them all from one BitBlt), each screen once and
// at most three whatever the catalog holds (it may give every module a
// "screen" of its own, each a picture of up to 48 MB): the one that follows
// the display (After Dark's, the whole monitor: ...-<window>.ppm) first,
// then at most kMaxOwnSeedShots of modules' own sizes, 640x480 first
// (Intermission's, Star Trek's, ScreamSavers' and Marvel's, the frame's part:
// ...-<window>-640x480.ppm), then the smallest. A first host whose screen
// was left out starts on black ("none taken" in the saver's log);
// `left_out`, when given, is how many screens were. Where two are the same
// picture (a 4:3 monitor at 480 lines, the whole of it at 640x480 either
// way), the later one shares the first's file. No screens, no pictures.
std::vector<SeedShotPlan> plan_seed_shots(const std::vector<ModuleScreen>& screens, int mw, int mh,
                                          size_t* left_out = nullptr);

// ---- monitor topology changes ------------------------------------------------------
// /s puts one window on every monitor. When monitors come, go, change mode
// or move (WM_DISPLAYCHANGE: a dock, a DisplayPort monitor waking from deep
// sleep, a projector), the windows are re-planned against the new monitor
// list so every monitor is covered again and no window hangs off a monitor
// that is gone, while restarting as few modules as possible.

// One saver window's place and what it shows.
struct ScreenSlot {
  RectI rc;                 // its monitor, virtual-screen pixels
  bool runs_host = false;
  SizeI emu;                // emulated screen its host renders (only when runs_host)
  // ...which is its module's own (ModuleScreen::fixed: an Intermission, Star
  // Trek, ScreamSavers or Marvel module's 640x480), the same on any monitor.
  // A slot to be filled is described by the size its monitor gives (After
  // Dark's) and never fixed.
  bool fixed = false;
  bool message = false;     // carries the not-imported / host-missing text
  bool operator==(const ScreenSlot&) const = default;
};

struct RelayoutPlan {
  // Per new slot: the current window that takes it over, or -1 for a new one.
  std::vector<int> reuse;
  // Per current window: true when no new slot wants it (destroy it and its host).
  std::vector<bool> retire;
  int kept = 0, moved = 0, created = 0, retired = 0;
  bool unchanged() const { return moved == 0 && created == 0 && retired == 0; }
};

// A window keeps its host when it keeps its role (host or not, message or
// not) and its host's emulated size — an After Dark module's depends only on
// the monitor's aspect, so a mode change or a rearrangement moves the window
// (`moved`) without a restart; a module's own (`fixed`: an Intermission, Star
// Trek, ScreamSavers or Marvel module's) on nothing, so its window can move
// to any monitor. Exact matches (same rect) are taken first, then same-size
// ones in order, then fixed-size ones (so a window whose size matches is
// never left without a slot by one that fits anywhere).
// Anything else is a new window with a new host.
RelayoutPlan plan_relayout(const std::vector<ScreenSlot>& current, const std::vector<ScreenSlot>& next);

} // namespace adw::scr
