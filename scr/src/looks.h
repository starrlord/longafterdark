// The looks (scr/README.md, "Looks"): how a /s or /window window may draw a
// module's frames beyond the sharp upscale it has always drawn (present.h).
// Each is a setting the user turns on. With the defaults (Look=sharp,
// AmbientBars=0) nothing here runs: the saver makes no Direct3D device and
// draws through Direct2D (or GDI) exactly as before.
//
//   [Saver] Look=sharp|crt|crt-curved|smooth|preset
//     sharp       every pixel of the frame a crisp block: today's way, the default
//     crt         a 1990s VGA monitor: a soft beam, faint scanlines, a fine
//                 mask and a little glow (hlsl_crt.h)
//     crt-curved  the same behind curved glass
//     smooth      an edge-directed upscale for flat cartoon art (hlsl_smooth.h)
//     preset      the RetroArch shader preset ShaderPreset names, run by
//                 librashader, which the user supplies (shader_preset.h)
//   [Saver] AmbientBars=1|0   the bars around a frame that doesn't fill the
//                             monitor show a blurred, dimmed copy of it
//                             instead of black
//   [Saver] ShaderPreset=<path of a .slangp>   (Look=preset)
// A Look this version doesn't know is kept in the file as written and drawn
// as sharp. /p and the settings window's live preview always draw as before.
#pragma once

#include <string>
#include <string_view>

namespace adw::scr {

enum class Look { sharp, crt, crt_curved, smooth, preset };

// The settings.ini spelling of each look ("sharp", "crt", "crt-curved",
// "smooth", "preset"), and back: case-insensitive, surrounding blanks
// ignored; false for anything else (*out untouched).
const char* look_name(Look look);
bool parse_look(std::string_view text, Look* out);

// What a window draws with.
struct LookOptions {
  Look look = Look::sharp;
  bool ambient = false;          // AmbientBars=1
  std::wstring preset;           // Look::preset: the .slangp, as the settings file names it
  // Anything beyond today's drawing: the window draws through Direct3D 11
  // (present_d3d.h) instead of Direct2D.
  bool any() const { return look != Look::sharp || ambient; }
  bool operator==(const LookOptions&) const = default;
};

// How strongly the CRT look draws its scanlines for `k` screen pixels per
// source line: not at all below 2.5, where they would beat against the
// screen's own pixel grid (480 lines on a 1080p monitor is k = 2.25; in the
// default 1280x720 /window, 1.5), fully from 3, linearly in between. The
// CRT's shader computes the same from its pass constants (hlsl_crt.h).
double crt_scanline_strength(double k);

} // namespace adw::scr
