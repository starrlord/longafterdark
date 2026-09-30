// The desktop seed (INTERACTION.md §8): before any /s window appears, each
// monitor is captured and handed to that window's first host as ADSEEDIMG,
// so the module starts on the desktop it is saving, as the 1996 hosts did.
//
// A window gets one picture for each kind of module it may start with, three
// at most (geometry.h plan_seed_shots), made and written one at a time: each
// a binary P6 (maxval 255) at that module's emulated size, written to
// %TEMP%\LongAfterDark-seed-<pid>-<window index>.ppm (a module that follows
// the display: After Dark's) or ...-<window index>-640x480.ppm (a module's
// own screen, an Intermission, Star Trek, ScreamSavers or Marvel module's:
// paths.h seed_file_path), through a
// handle opened FILE_FLAG_DELETE_ON_CLOSE | FILE_ATTRIBUTE_TEMPORARY with
// share read + delete. The saver keeps that handle for its lifetime, so the
// file disappears when the saver ends, however it ends. A host opens it with
// FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE.
#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "geometry.h"

namespace adw::scr {

// A P6 image in memory: header + w*h*3 RGB bytes, from top-down BGRX rows.
std::vector<uint8_t> encode_p6(const uint8_t* bgrx, int w, int h, int stride);

// Creates the delete-on-close file at `path` and writes `p6` into it. Returns
// the open handle (keep it; closing it deletes the file) or
// INVALID_HANDLE_VALUE with *error set.
HANDLE write_seed_file(const std::wstring& path, const std::vector<uint8_t>& p6, std::wstring* error);

// Captures `monitor` (virtual-screen pixels) with BitBlt(SRCCOPY |
// CAPTUREBLT), shrinks or stretches it to `emu` with HALFTONE, and returns it
// as a P6 (empty on failure).
std::vector<uint8_t> capture_monitor_p6(const RECT& monitor, SizeI emu);

// One picture of a monitor: its part `src` (monitor pixels, seed_source) at
// the emulated size `emu`. A shot without a size is no picture
// (App::capture_seeds passes one for a picture that shares an earlier one's
// file).
struct SeedShot {
  RectI src;
  SizeI emu;
};
// Takes each shot's picture as soon as it is made, in order: `shot` its
// index, `p6` the P6 (empty when there is none: a shot without a size, a
// part that leaves the monitor, a capture that failed). The picture is
// dropped once it returns, and the next one is made only then, so however
// many shots there are, one picture is held at a time.
using SeedTake = std::function<void(size_t shot, const std::vector<uint8_t>& p6)>;

// One BitBlt of `monitor` for every shot, each its part shrunk or stretched
// on its own (shrink_parts); every shot is taken, an empty one for each
// when the capture fails.
void capture_monitor_shots(const RECT& monitor, const std::vector<SeedShot>& shots, const SeedTake& take);

// The shots' pictures from a monitor's picture already in `src` (a DC whose
// bitmap holds its mw x mh pixels): each part shrunk or stretched with
// HALFTONE, handed to `take` one by one, in order.
void shrink_parts(HDC src, int mw, int mh, const std::vector<SeedShot>& shots, const SeedTake& take);

}  // namespace adw::scr
