// Emulated-screen sizing and letterboxing: the Windows saver's pure rules
// (scr/src/geometry.*), so the Linux player gives a module the same screen
// and maps the mouse into the same frame rectangle.
#pragma once

#include <string_view>

namespace lad {

struct SizeI {
  int w = 0, h = 0;
  bool operator==(const SizeI&) const = default;
};

struct RectI {
  int x = 0, y = 0, w = 0, h = 0;
  bool operator==(const RectI&) const = default;
};

struct PointI {
  int x = 0, y = 0;
  bool operator==(const PointI&) const = default;
};

// Base 640x480 scaled by `scale`, widened to `display_aspect` (never
// narrower than 4:3), both axes snapped to the nearest multiple of 8, the
// width capped at twice the scaled 4:3 width.
SizeI emulated_screen_size(double display_aspect, double scale, int base_w = 640, int base_h = 480);

// The largest rect with the source's aspect that fits in dst_w x dst_h,
// centred (the remainder is letterbox/pillarbox).
RectI fit_rect(int src_w, int src_h, int dst_w, int dst_h);

// A module's own screen: its catalog "screen", else its ABI's
// (Intermission's 640x480); {0, 0} for a module that follows the display.
SizeI own_screen(std::string_view abi, SizeI catalog_screen = {});

struct ModuleScreen {
  SizeI emu;
  bool fixed = false;   // the module's own size, which no display changes
  bool operator==(const ModuleScreen&) const = default;
};
ModuleScreen module_screen(SizeI own, double display_aspect, double scale);

// The Windows /p preview's screen, every module's in a preview (-window-id):
// the host renders a screen this small through a guest display of at least
// 640x480 and shrinks it (host/ne16 and host/pe32, "Small screens").
inline constexpr SizeI kPreviewScreen{320, 240};

// True when `outer` holds the centre of `inner` (a window and a monitor:
// of the windows XScreenSaver puts one per monitor, exactly one holds the
// primary monitor's centre).
bool contains_centre(const RectI& outer, const RectI& inner);

}  // namespace lad
