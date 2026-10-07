// scr_unit looks: the Smooth look (hlsl_smooth.h), drawn next to the sharp
// look on small made-up frames (looks_test.h).
#include "looks_test.h"

#include <cmath>
#include <set>

#include "hlsl_smooth.h"
#include "looks.h"
#include "present_d3d.h"

namespace adw::scr::looks_test {

namespace {

const std::vector<RGBQUAD> kBlackWhite = {RGBQUAD{0, 0, 0, 0}, RGBQUAD{255, 255, 255, 0}};

// `f` drawn with `look` (bars black) into `fit` of a w x h picture; empty,
// with a failure counted, when it couldn't be.
std::vector<uint8_t> draw(const Frame& f, int w, int h, const RectI& fit, Look look) {
  LookOptions opts;
  opts.look = look;
  std::vector<uint8_t> bgr;
  std::string error;
  if (!render_frame_bgr_d3d(f, w, h, fit, opts, bgr, &error) || bgr.size() != (size_t)w * h * 3) {
    fprintf(stderr, "smooth: drawing %dx%d with %s failed: %s\n", w, h, look_name(look), error.c_str());
    ++failures;
    return {};
  }
  return bgr;
}

// How many grey levels strictly between black and white rows y0..y1-1 hold:
// none on a stepped edge, many on one drawn smooth.
int grey_levels(const std::vector<uint8_t>& bgr, int w, int y0, int y1) {
  std::set<int> levels;
  for (int y = y0; y < y1; ++y) {
    for (int x = 0; x < w; ++x) {
      const int l = luma_at(bgr, w, x, y);
      if (l > 4 && l < 251) levels.insert(l);
    }
  }
  return (int)levels.size();
}

// Where row y first goes from dark to light, in pixels (their centres
// whole), or -1.
double crossing(const std::vector<uint8_t>& bgr, int w, int y) {
  for (int x = 1; x < w; ++x) {
    const int a = luma_at(bgr, w, x - 1, y), b = luma_at(bgr, w, x, y);
    if (a < 128 && b >= 128) return x - 1 + (127.5 - a) / (b - a);
  }
  return -1;
}

// The picture's centre of light, in pixels (their centres whole).
void centroid(const std::vector<uint8_t>& bgr, int w, int h, double* cx, double* cy) {
  double sum = 0, sx = 0, sy = 0;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const int l = luma_at(bgr, w, x, y);
      sum += l;
      sx += (double)l * x;
      sy += (double)l * y;
    }
  }
  *cx = sum > 0 ? sx / sum : -1;
  *cy = sum > 0 ? sy / sum : -1;
}

} // namespace

void smooth_checks() {
  // Super-xBR (a luma pass and its three) twice, 4x the frame, then the fit
  // pass: the last, and the only one into the fit rectangle.
  const std::vector<PassSpec> passes = smooth_passes();
  LCHECK(passes.size() == 9);
  for (size_t i = 0; i < passes.size(); ++i) {
    LCHECK(!passes[i].name.empty() && !passes[i].hlsl.empty());
    LCHECK((passes[i].size == PassSize::fit) == (i + 1 == passes.size()));
  }

  // Diagonal staircases at 4x, one pixel across per row and two: the sharp
  // look draws the steps, the Smooth look a slope on the same line, within a
  // source pixel in every row and on average within a quarter of a screen
  // pixel (the fit pass half a texel of the 4x picture off would be half a
  // screen pixel; the quarter-pixel shift Super-xBR's second pass leaves for
  // its third to undo, a whole one). The 45 degree slope crosses every row
  // the same way, with a few greys; the shallower one crosses each row at a
  // different place, with many.
  struct Stair {
    int run;          // pixels across per row
    int y0, y1;       // the rows whose edge is clear of the frame's borders
  };
  for (const Stair st : {Stair{1, 16, 112}, Stair{2, 36, 92}}) {
    const int run = st.run;
    const Frame f = make_frame8(32, 32, [run](int x, int y) { return (uint8_t)(x > run * y - 16 * (run - 1) ? 1 : 0); },
                                kBlackWhite);
    const RectI fit{0, 0, 128, 128};
    const std::vector<uint8_t> sharp = draw(f, 128, 128, fit, Look::sharp);
    const std::vector<uint8_t> smooth = draw(f, 128, 128, fit, Look::smooth);
    if (sharp.empty() || smooth.empty()) continue;
    if (run == 2) {
      const int sharp_levels = grey_levels(sharp, 128, st.y0, st.y1);
      const int smooth_levels = grey_levels(smooth, 128, st.y0, st.y1);
      LCHECK(smooth_levels >= 24 && smooth_levels >= 4 * sharp_levels);
      if (smooth_levels < 24 || smooth_levels < 4 * sharp_levels) {
        fprintf(stderr, "smooth: staircase edge has %d grey levels, the sharp look's %d\n", smooth_levels,
                sharp_levels);
      }
    }
    double worst = 0, mean = 0;
    int rows = 0;
    for (int y = st.y0; y < st.y1; ++y) {
      const double a = crossing(sharp, 128, y), b = crossing(smooth, 128, y);
      LCHECK(a >= 0 && b >= 0);
      if (a < 0 || b < 0) break;
      worst = std::max(worst, std::fabs(b - a));
      mean += b - a;
      ++rows;
    }
    mean = rows ? mean / rows : 0;
    LCHECK(worst <= 4.0);
    LCHECK(std::fabs(mean) <= 0.25);
    if (worst > 4.0 || std::fabs(mean) > 0.25) {
      fprintf(stderr, "smooth: staircase (%d across a row) %.2f px off at worst, %.2f on average\n", run, worst,
              mean);
    }
  }

  // A flat colour stays flat, to the fit rectangle's edges: no ringing, no
  // dark border, growing or shrinking.
  {
    const Frame f = make_frame8(40, 30, [](int, int) { return (uint8_t)1; },
                                {RGBQUAD{0, 0, 0, 0}, RGBQUAD{40, 120, 200, 0}});
    for (const RectI fit : {RectI{0, 0, 160, 120}, RectI{0, 0, 90, 68}, RectI{0, 0, 33, 25}}) {
      const std::vector<uint8_t> smooth = draw(f, fit.w, fit.h, fit, Look::smooth);
      int worst = 0;
      for (size_t i = 0; i + 2 < smooth.size(); i += 3) {
        worst = std::max({worst, std::abs(smooth[i] - 40), std::abs(smooth[i + 1] - 120),
                          std::abs(smooth[i + 2] - 200)});
      }
      LCHECK(!smooth.empty() && worst <= 2);
      if (worst > 2) fprintf(stderr, "smooth: flat colour off by %d into %dx%d\n", worst, fit.w, fit.h);
    }
  }

  // No ringing at an edge either: a block of grey 220 on grey 60 (where an
  // overshoot can't hide in black or white) stays between the two, however
  // the picture is scaled.
  {
    const Frame f = make_frame8(40, 30, [](int x, int y) { return (uint8_t)(x >= 10 && x < 30 && y >= 8 && y < 22); },
                                {RGBQUAD{60, 60, 60, 0}, RGBQUAD{220, 220, 220, 0}});
    for (const RectI fit : {RectI{0, 0, 90, 68}, RectI{0, 0, 140, 105}, RectI{0, 0, 33, 25}}) {
      const std::vector<uint8_t> smooth = draw(f, fit.w, fit.h, fit, Look::smooth);
      int least = 255, most = 0;
      for (const uint8_t v : smooth) {
        least = std::min(least, (int)v);
        most = std::max(most, (int)v);
      }
      LCHECK(!smooth.empty() && least >= 58 && most <= 222);
      if (least < 58 || most > 222) {
        fprintf(stderr, "smooth: an edge rings into %dx%d: %d..%d\n", fit.w, fit.h, least, most);
      }
    }
  }

  // No shift: a single white pixel's centre of light lands where the sharp
  // look's does, within half a source pixel, at whole and fractional scales.
  {
    const Frame f = make_frame8(16, 16, [](int x, int y) { return (uint8_t)(x == 7 && y == 5 ? 1 : 0); },
                                kBlackWhite);
    for (const int size : {64, 48, 36}) {
      const RectI fit{0, 0, size, size};
      const std::vector<uint8_t> sharp = draw(f, size, size, fit, Look::sharp);
      const std::vector<uint8_t> smooth = draw(f, size, size, fit, Look::smooth);
      if (sharp.empty() || smooth.empty()) continue;
      double ax, ay, bx, by;
      centroid(sharp, size, size, &ax, &ay);
      centroid(smooth, size, size, &bx, &by);
      const double k = size / 16.0;
      LCHECK(ax >= 0 && bx >= 0);
      LCHECK(std::fabs(bx - ax) / k <= 0.5 && std::fabs(by - ay) / k <= 0.5);
      if (std::fabs(bx - ax) / k > 0.5 || std::fabs(by - ay) / k > 0.5) {
        fprintf(stderr, "smooth: at %.2fx the pixel's centre moved (%.3f, %.3f) source pixels\n", k, (bx - ax) / k,
                (by - ay) / k);
      }
    }
  }

  // Without AmbientBars the bars stay black, however bright the frame.
  {
    const Frame f = make_frame8(64, 48, [](int x, int y) { return (uint8_t)(((x ^ y) & 1) ? 1 : 2); },
                                {RGBQUAD{0, 0, 0, 0}, RGBQUAD{255, 255, 255, 0}, RGBQUAD{0, 255, 255, 0}});
    const int w = 384, h = 256;
    const RectI fit{64, 32, 256, 192};
    const std::vector<uint8_t> smooth = draw(f, w, h, fit, Look::smooth);
    if (!smooth.empty()) {
      int brightest = 0;
      for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
          if (x >= fit.x && x < fit.x + fit.w && y >= fit.y && y < fit.y + fit.h) continue;
          const uint8_t* p = &smooth[((size_t)y * w + x) * 3];
          brightest = std::max({brightest, (int)p[0], (int)p[1], (int)p[2]});
        }
      }
      LCHECK(brightest == 0);
      LCHECK(mean_luma(smooth, w, fit.x, fit.y, fit.x + fit.w, fit.y + fit.h) > 150);
    }
  }
}

} // namespace adw::scr::looks_test
