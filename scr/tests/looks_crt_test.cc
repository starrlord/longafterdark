// scr_unit looks: the CRT look (looks_test.h, hlsl_crt.h): its scanlines
// come with room for them and go without, its light stays about the sharp
// look's, the curved glass blacks out the corners, a dot glows but stays
// put, and nothing lands outside the fit rectangle. Small made-up frames:
// WARP draws them.
#include "looks_test.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "hlsl_crt.h"
#include "looks.h"
#include "present_d3d.h"

namespace adw::scr::looks_test {

namespace {

const LookOptions kSharp{};
const LookOptions kFlat{Look::crt, false, {}};
const LookOptions kCurved{Look::crt_curved, false, {}};

// `f` drawn with `opts` into `fit` of a w x h picture.
bool draw(const Frame& f, int w, int h, const RectI& fit, const LookOptions& opts, std::vector<uint8_t>& bgr) {
  std::string err;
  const bool ok = render_frame_bgr_d3d(f, w, h, fit, opts, bgr, &err) && bgr.size() == (size_t)w * h * 3;
  if (!ok) fprintf(stderr, "looks crt: %s %dx%d: %s\n", look_name(opts.look), w, h, err.c_str());
  return ok;
}

// The mean luma of each row of a w x h picture.
std::vector<double> row_means(const std::vector<uint8_t>& bgr, int w, int h) {
  std::vector<double> rows(h);
  for (int y = 0; y < h; ++y) rows[y] = mean_luma(bgr, w, 0, y, w, y + 1);
  return rows;
}

// The brightest pixel's luma outside `keep` (pixels), in a w x h picture.
int max_luma_outside(const std::vector<uint8_t>& bgr, int w, int h, const RectI& keep) {
  int most = 0;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      if (x >= keep.x && x < keep.x + keep.w && y >= keep.y && y < keep.y + keep.h) continue;
      most = std::max(most, luma_at(bgr, w, x, y));
    }
  }
  return most;
}

} // namespace

void crt_checks() {
  // Four passes, the last (only) into the fit rectangle; the curved look
  // differs only in its last pass.
  {
    const std::vector<PassSpec> flat = crt_passes(false), curved = crt_passes(true);
    LCHECK(flat.size() == 4 && curved.size() == 4);
    for (size_t i = 0; i < flat.size() && i < curved.size(); ++i) {
      LCHECK((flat[i].size == PassSize::fit) == (i + 1 == flat.size()));
      LCHECK((flat[i].hlsl == curved[i].hlsl) && (i + 1 == flat.size() || flat[i].params == curved[i].params));
    }
    LCHECK(look_passes(Look::crt).size() == 4 && look_passes(Look::crt_curved).size() == 4);
  }

  const Frame grey = grey_frame(64, 48, 128);
  std::vector<uint8_t> a, b;

  // k = 4 (48 lines on 192 rows): scanlines, every fourth row. Rows 4n+1 and
  // 4n+2 lie on a line, 4n and 4n+3 between two; faint, never black bars.
  if (draw(grey, 256, 192, RectI{0, 0, 256, 192}, kFlat, a)) {
    const std::vector<double> rows = row_means(a, 256, 192);
    double on = 0, between = 0, wobble = 0;
    for (int y = 8; y < 184; y += 4) {
      on += (rows[y + 1] + rows[y + 2]) / 2;
      between += (rows[y] + rows[y + 3]) / 2;
    }
    on /= 44;
    between /= 44;
    for (int y = 8; y < 180; ++y) wobble = std::max(wobble, std::fabs(rows[y] - rows[y + 4]));
    LCHECK(on - between >= 6);
    LCHECK(between >= on * 0.6);
    LCHECK(wobble <= 1.5);
    // The light about the sharp look's.
    if (draw(grey, 256, 192, RectI{0, 0, 256, 192}, kSharp, b)) {
      const double crt = mean_luma(a, 256, 0, 0, 256, 192), sharp = mean_luma(b, 256, 0, 0, 256, 192);
      LCHECK(std::fabs(crt - sharp) <= 0.12 * sharp);
    }
  }
  // k = 2 (48 lines on 96 rows): no room for scanlines; the mask is upright
  // stripes, so every row is alike.
  if (draw(grey, 128, 96, RectI{0, 0, 128, 96}, kFlat, a)) {
    const std::vector<double> rows = row_means(a, 128, 96);
    const auto [lo, hi] = std::minmax_element(rows.begin(), rows.end());
    LCHECK(*hi - *lo <= 1.5);
    LCHECK(std::fabs(mean_luma(a, 128, 0, 0, 128, 96) - 128) <= 0.12 * 128);
  }

  // Curved: the fit rectangle's corners are outside the glass, its middle
  // lit; flat, the corners are lit too.
  for (const LookOptions& opts : {kFlat, kCurved}) {
    const bool curved = opts.look == Look::crt_curved;
    const RectI fit{8, 6, 256, 192};
    if (!draw(grey, 272, 204, fit, opts, a)) continue;
    LCHECK(luma_at(a, 272, 8 + 128, 6 + 96) >= 90);
    for (int corner = 0; corner < 4; ++corner) {
      const int x = corner & 1 ? fit.x + fit.w - 1 : fit.x, y = corner & 2 ? fit.y + fit.h - 1 : fit.y;
      LCHECK(curved ? luma_at(a, 272, x, y) <= 2 : luma_at(a, 272, x, y) >= 90);
    }
  }

  // A one-pixel white dot on black (k = 4: the dot is 128..131 x 96..99):
  // the beam and the glow spread it, a little, but nothing far from it.
  {
    const Frame dot = make_frame8(64, 48, [](int x, int y) { return (uint8_t)(x == 32 && y == 24); },
                                  {RGBQUAD{0, 0, 0, 0}, RGBQUAD{255, 255, 255, 0}});
    for (const LookOptions& opts : {kFlat, kCurved}) {
      if (!draw(dot, 256, 192, RectI{0, 0, 256, 192}, opts, a)) continue;
      LCHECK(luma_at(a, 256, 129, 97) >= 200);
      // Beside it, and a few pixels off (the glow).
      LCHECK(luma_at(a, 256, 126, 97) >= 16 && luma_at(a, 256, 133, 98) >= 16);
      LCHECK(luma_at(a, 256, 129 + 12, 97) >= 1 && luma_at(a, 256, 129, 97 - 12) >= 1);
      // Nothing beyond twelve frame pixels.
      LCHECK(max_luma_outside(a, 256, 192, RectI{128 - 48, 96 - 48, 4 + 96, 4 + 96}) == 0);
    }
  }

  // Nothing outside the fit rectangle: white into the middle of a bigger
  // picture leaves the bars black, glow and all.
  {
    const Frame white = grey_frame(64, 48, 255);
    const RectI fit{40, 30, 128, 96};
    for (const LookOptions& opts : {kFlat, kCurved}) {
      if (!draw(white, 208, 156, fit, opts, a)) continue;
      LCHECK(max_luma_outside(a, 208, 156, fit) == 0);
      LCHECK(luma_at(a, 208, 40 + 64, 30 + 48) >= 200);
    }
  }

  // Any size: from a window smaller than the frame (k = 0.5) to 8K's k = 9,
  // the grey comes out about grey, the middle lit (nothing undefined).
  for (const int eighths : {4, 8, 12, 18, 22, 26, 36, 72}) {
    const int w = 64 * eighths / 8, h = 48 * eighths / 8;
    for (const LookOptions& opts : {kFlat, kCurved}) {
      if (!draw(grey, w, h, RectI{0, 0, w, h}, opts, a)) continue;
      const double mid = mean_luma(a, w, w / 4, h / 4, w * 3 / 4, h * 3 / 4);
      if (std::fabs(mid - 128) > 0.12 * 128) {
        fprintf(stderr, "looks crt: %s at k = %.3f: the middle's mean luma %.1f\n", look_name(opts.look),
                eighths / 8.0, mid);
      }
      LCHECK(std::fabs(mid - 128) <= 0.12 * 128);
    }
  }
}

} // namespace adw::scr::looks_test
