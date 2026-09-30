#include "geometry.h"

#include <algorithm>

namespace lad {

namespace {
// Truncate, then round to the nearest multiple of 8.
int snap8(double v) { return (((int)v + 4) / 8) * 8; }
}  // namespace

SizeI emulated_screen_size(double display_aspect, double scale, int base_w, int base_h) {
  if (!(scale > 0.0)) scale = 1.0;
  int h = snap8(base_h * scale);
  double a = std::max(display_aspect > 0.0 ? display_aspect : 0.0, (double)base_w / base_h);
  int w = std::min(snap8(h * a), snap8(base_w * scale) * 2);
  return {w, h};
}

RectI fit_rect(int src_w, int src_h, int dst_w, int dst_h) {
  if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) return {0, 0, std::max(dst_w, 0), std::max(dst_h, 0)};
  long long lhs = (long long)dst_w * src_h, rhs = (long long)dst_h * src_w;
  int w, h;
  if (lhs > rhs) {            // destination is wider: full height, pillarbox
    h = dst_h;
    w = (int)(((long long)dst_h * src_w + src_h / 2) / src_h);
  } else {                    // destination is taller (or equal): full width, letterbox
    w = dst_w;
    h = (int)(((long long)dst_w * src_h + src_w / 2) / src_w);
  }
  w = std::min(w, dst_w);
  h = std::min(h, dst_h);
  return {(dst_w - w) / 2, (dst_h - h) / 2, w, h};
}

SizeI own_screen(std::string_view abi, SizeI catalog_screen) {
  if (catalog_screen.w > 0 && catalog_screen.h > 0) return catalog_screen;
  if (abi == "intermission") return SizeI{640, 480};
  return SizeI{};
}

ModuleScreen module_screen(SizeI own, double display_aspect, double scale) {
  if (own.w > 0 && own.h > 0) return ModuleScreen{own, true};
  return ModuleScreen{emulated_screen_size(display_aspect, scale), false};
}

bool contains_centre(const RectI& outer, const RectI& inner) {
  if (outer.w <= 0 || outer.h <= 0 || inner.w <= 0 || inner.h <= 0) return false;
  // Twice every coordinate, so the centre is a whole number.
  const long long cx = 2LL * inner.x + inner.w, cy = 2LL * inner.y + inner.h;
  return cx >= 2LL * outer.x && cx < 2LL * ((long long)outer.x + outer.w) && cy >= 2LL * outer.y &&
         cy < 2LL * ((long long)outer.y + outer.h);
}

}  // namespace lad
