// How a saver window puts the host's frames on its monitor (scr/README.md,
// "Scaling"): the frame is letterboxed into the window (black bars) and
// scaled, by the GPU through Direct2D where it can be, else by GDI's
// StretchDIBits.
//
// GDI's good filter (HALFTONE) upscales in software: about 27 ms a paint for
// 856x480 -> 3840x2160, so on a 4K monitor the saver used to drop to
// nearest-neighbour COLORONCOLOR to keep up. Direct2D uploads the frame (an
// 8-bit frame through its palette) as a 32-bit bitmap and scales it on the
// GPU the way HALFTONE scales an upscale (crisp, evenly sized pixels, blended
// only where two meet), for under 2 ms a frame at 4K. GDI stays the
// fallback: for a window whose render target can't be made or keeps
// failing, and for /p.
#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "geometry.h"
#include "host_process.h"

namespace adw::scr {

// The scaling filter. smooth: GDI's HALFTONE; on Direct2D, an upscale is
// the frame repeated to the next whole multiple (nearest neighbour), then
// drawn linearly to the window, and a downscale is high-quality cubic
// (linear without ID2D1DeviceContext). nearest: nearest neighbour in both
// (GDI's COLORONCOLOR).
enum class Filter { smooth, nearest };

// The frame as 32-bit BGRX rows of width*4 bytes, top-down: an 8-bit frame
// through its palette, a 32-bit one as it is (stride padding dropped).
void frame_to_bgrx(const Frame& f, std::vector<uint32_t>& out);

// Extracts sub-rectangle `sub` from `src` into `dst` (preserving bpp and palette).
void crop_frame(const Frame& src, const RectI& sub, Frame& dst);

// GDI: `f` stretched into `fit` on `dc` with HALFTONE (smooth) or
// COLORONCOLOR (nearest). The bars around `fit` are the caller's.
void stretch_frame_gdi(HDC dc, const Frame& f, const RectI& fit, Filter filter);

// Direct2D presentation for one window. Device resources (the window's
// render target and the frame bitmap) are made on first use and remade
// after a device loss (a driver update or reset, a GPU removed).
class D2DPresenter {
 public:
  D2DPresenter();
  ~D2DPresenter();
  D2DPresenter(const D2DPresenter&) = delete;
  D2DPresenter& operator=(const D2DPresenter&) = delete;

  // Draws `f` into `fit` of `hwnd`'s client area, on black. false when it
  // couldn't (the caller draws this frame with GDI): `*error` says why, and
  // `*device_lost` whether the device went away (the next call starts
  // afresh; anything else is not worth retrying).
  bool present(HWND hwnd, const Frame& f, const RectI& fit, Filter filter, std::string* error = nullptr,
               bool* device_lost = nullptr);
  // Black over the whole window, while there is no frame (between modules).
  // false when there is no render target yet, or it failed (then released):
  // GDI paints it instead.
  bool clear(HWND hwnd);
  // A render target exists: the next present costs no device set-up.
  bool ready() const;
  // Lets go of the render target, so GDI owns the window again (a message
  // drawn over black).
  void release();
  // What `smooth` does here, for the log: "sharp upscale, high-quality
  // cubic downscale" ("linear downscale" without ID2D1DeviceContext; "" before
  // the first present).
  const char* smooth_name() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// What a window shows, rendered off screen: `f` scaled into `fit` on a
// black w x h picture, drawn as the D2DPresenter draws it (`d2d`, on a
// software render target) or as GDI's StretchDIBits does. BGR rows,
// top-down, w*3 bytes each (adw_ui's save_png_bgr). The test hook
// AD_SCR_TEST_CAPTURE writes these, and the unit tests check the filters.
bool render_frame_bgr(const Frame& f, int w, int h, const RectI& fit, bool d2d, Filter filter,
                      std::vector<uint8_t>& bgr, std::string* error = nullptr);

} // namespace adw::scr
