// scr_unit looks: the presenter (present_d3d.h): the sharp look against
// Direct2D's drawing, the resolve of 8- and 32-bit frames, the pass runner,
// the ambient bars, and present()'s refusals (looks_test.h). Small pictures
// throughout: WARP draws them, and it is slow at monitor sizes.
#include "looks_test.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "geometry.h"
#include "looks.h"
#include "present.h"
#include "present_d3d.h"
#include "shader_preset.h"

namespace adw::scr::looks_test {

namespace {

// LCHECK as an expression: counts a failure, and says whether it held.
bool check_ok(bool ok, const char* what, int line) {
  if (!ok) {
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, line, what);
    ++failures;
  }
  return ok;
}
#define LCHECK_OK(cond) check_ok((cond), #cond, __LINE__)

// A frame of `w` x `h` random pixels through a random 256-colour palette
// (every pixel an edge: the hardest case for two scalers to agree on).
Frame random_frame8(int w, int h, uint32_t seed) {
  uint32_t x = seed;
  auto next = [&x] {
    x = x * 1664525u + 1013904223u;
    return x >> 8;
  };
  std::vector<RGBQUAD> pal(256);
  for (RGBQUAD& q : pal) q = RGBQUAD{(BYTE)next(), (BYTE)next(), (BYTE)next(), 0};
  std::vector<uint8_t> idx((size_t)w * h);
  for (uint8_t& i : idx) i = (uint8_t)next();
  return make_frame8(w, h, [&](int px, int py) { return idx[(size_t)py * w + px]; }, pal);
}

// Mean and largest per-channel difference of two BGR pictures of one size.
struct Diff {
  double mean = 1e9;
  int max = 255;
};
Diff diff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
  Diff d;
  if (a.size() != b.size() || a.empty()) return d;
  long long sum = 0;
  d.max = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    const int e = std::abs((int)a[i] - (int)b[i]);
    sum += e;
    d.max = std::max(d.max, e);
  }
  d.mean = (double)sum / (double)a.size();
  return d;
}

bool draw(const Frame& f, int w, int h, const RectI& fit, const LookOptions& opts, std::vector<uint8_t>& bgr) {
  std::string err;
  const bool ok = render_frame_bgr_d3d(f, w, h, fit, opts, bgr, &err);
  if (!ok) fprintf(stderr, "looks core: %s %dx%d: %s\n", look_name(opts.look), w, h, err.c_str());
  return ok && bgr.size() == (size_t)w * h * 3;
}

bool draw_d2d(const Frame& f, int w, int h, const RectI& fit, std::vector<uint8_t>& bgr) {
  std::string err;
  const bool ok = render_frame_bgr(f, w, h, fit, true, Filter::smooth, bgr, &err);
  if (!ok) fprintf(stderr, "looks core: direct2d %dx%d: %s\n", w, h, err.c_str());
  return ok && bgr.size() == (size_t)w * h * 3;
}

// The sharp look draws what Direct2D draws today (present.cc).
void sharp_checks() {
  const LookOptions sharp;
  // Black | white, 2x1, 4.5 times wider into 17x4 at x=4 (unit_tests.cc's
  // test_present): the one column where the blocks meet blends them half and
  // half, the rest is the blocks and the bars.
  const Frame bw = make_frame8(2, 1, [](int x, int) { return (uint8_t)x; }, {RGBQUAD{0, 0, 0, 0}, RGBQUAD{255, 255, 255, 0}});
  std::vector<uint8_t> a, b;
  const RectI fit{4, 0, 9, 4};
  if (LCHECK_OK(draw(bw, 17, 4, fit, sharp, a)) && LCHECK_OK(draw_d2d(bw, 17, 4, fit, b))) {
    for (int x = 0; x < 17; ++x) LCHECK(std::abs(luma_at(a, 17, x, 2) - luma_at(b, 17, x, 2)) <= 2);
    LCHECK(std::abs(luma_at(a, 17, 8, 2) - 128) <= 2);
    LCHECK(luma_at(a, 17, 7, 2) == 0 && luma_at(a, 17, 9, 2) == 255);
    LCHECK(luma_at(a, 17, 3, 2) == 0 && luma_at(a, 17, 13, 2) == 0);
  }
  // At an exact multiple there is nothing to blend; at 1:1, the frame itself.
  if (LCHECK_OK(draw(bw, 8, 2, RectI{0, 0, 8, 2}, sharp, a))) LCHECK(luma_at(a, 8, 3, 1) == 0 && luma_at(a, 8, 4, 1) == 255);
  const Frame rnd = random_frame8(107, 60, 7);
  if (LCHECK_OK(draw(rnd, 107, 60, RectI{0, 0, 107, 60}, sharp, a))) {
    const Frame f32 = to_frame32(rnd);
    bool same = true;
    for (int y = 0; y < 60 && same; ++y) {
      for (int x = 0; x < 107 && same; ++x) {
        same = memcmp(&a[((size_t)y * 107 + x) * 3], &f32.bits[(size_t)y * f32.stride + (size_t)x * 4], 3) == 0;
      }
    }
    LCHECK(same);
  }
  // Random pixels at the scales an 856x480 frame has on a 1080p and a 4K
  // monitor (2.24 and 4.49), small: the two agree within a level or so on
  // average, the odd blended pixel a few levels apart (measured: mean 0.21
  // and 0.27, max 4 and 7 at full size; research/looks/core/FACTS.md).
  for (const SizeI& out : {SizeI{240, 135}, SizeI{480, 270}}) {
    const RectI box = fit_rect(rnd.width, rnd.height, out.w, out.h);
    if (LCHECK_OK(draw(rnd, out.w, out.h, box, sharp, a)) && LCHECK_OK(draw_d2d(rnd, out.w, out.h, box, b))) {
      const Diff d = diff(a, b);
      if (d.mean > 0.6 || d.max > 12) fprintf(stderr, "looks core: sharp vs direct2d %dx%d: mean %.3f max %d\n", out.w, out.h, d.mean, d.max);
      LCHECK(d.mean <= 0.6 && d.max <= 12);
    }
  }
  // A downscale (a small /window) is Direct2D's high-quality cubic: random
  // pixels halved, and a picture of soft bands shrunk (measured at 428x240:
  // mean 0.21, max 8).
  {
    const Frame big = random_frame8(214, 120, 5);
    if (LCHECK_OK(draw(big, 107, 60, RectI{0, 0, 107, 60}, sharp, a)) &&
        LCHECK_OK(draw_d2d(big, 107, 60, RectI{0, 0, 107, 60}, b))) {
      const Diff d = diff(a, b);
      if (d.mean > 0.6 || d.max > 16) fprintf(stderr, "looks core: sharp halved vs direct2d: mean %.3f max %d\n", d.mean, d.max);
      LCHECK(d.mean <= 0.6 && d.max <= 16);
    }
  }
  const Frame soft = make_frame8(
      120, 80, [](int x, int y) { return (uint8_t)((x / 6 + y / 5) % 16); },
      [] {
        std::vector<RGBQUAD> pal(16);
        for (int i = 0; i < 16; ++i) pal[i] = RGBQUAD{(BYTE)(i * 16), (BYTE)(255 - i * 12), (BYTE)(i * 9), 0};
        return pal;
      }());
  if (LCHECK_OK(draw(soft, 60, 40, RectI{0, 0, 60, 40}, sharp, a)) && LCHECK_OK(draw_d2d(soft, 60, 40, RectI{0, 0, 60, 40}, b))) {
    const Diff d = diff(a, b);
    if (d.mean > 0.6 || d.max > 16) fprintf(stderr, "looks core: sharp downscale vs direct2d: mean %.3f max %d\n", d.mean, d.max);
    LCHECK(d.mean <= 0.6 && d.max <= 16);
  }
}

// An 8-bit frame and the same picture in 32 bits draw the same, with every look.
void resolve_checks() {
  const Frame f8 = random_frame8(64, 48, 3);
  const Frame f32 = to_frame32(f8);
  for (Look l : {Look::sharp, Look::crt, Look::crt_curved, Look::smooth}) {
    if (look_passes(l).empty()) continue;
    for (bool ambient : {false, true}) {
      const LookOptions o{l, ambient, {}};
      std::vector<uint8_t> a, b;
      if (LCHECK_OK(draw(f8, 160, 90, fit_rect(64, 48, 160, 90), o, a)) &&
          LCHECK_OK(draw(f32, 160, 90, fit_rect(64, 48, 160, 90), o, b))) {
        LCHECK(a == b);
      }
    }
  }
  // A frame of another size after the first, and back: each as if drawn alone.
  const Frame other = random_frame8(40, 30, 11);
  std::vector<uint8_t> first, second, again;
  const LookOptions sharp;
  if (LCHECK_OK(draw(f8, 128, 72, fit_rect(64, 48, 128, 72), sharp, first)) &&
      LCHECK_OK(draw(other, 128, 72, fit_rect(40, 30, 128, 72), sharp, second)) &&
      LCHECK_OK(draw(f8, 128, 72, fit_rect(64, 48, 128, 72), sharp, again))) {
    LCHECK(first == again && first != second);
    std::vector<uint8_t> alone;
    if (LCHECK_OK(draw(other, 96, 96, fit_rect(40, 30, 96, 96), sharp, alone))) {
      LCHECK(LCHECK_OK(draw(other, 128, 72, fit_rect(40, 30, 128, 72), sharp, again)) && again == second);
    }
  }
}

// The bars: black without AmbientBars; with it, the frame's colours dimmed
// (a bright frame lights them), while the fit rectangle is the plain drawing.
void ambient_checks() {
  const Frame bright = grey_frame(64, 48, 220);
  const int W = 192, H = 108;
  const RectI fit = fit_rect(64, 48, W, H);   // 144x108 at x=24: bars at the sides
  std::vector<uint8_t> plain, lit;
  if (!LCHECK_OK(draw(bright, W, H, fit, LookOptions{}, plain)) ||
      !LCHECK_OK(draw(bright, W, H, fit, LookOptions{Look::sharp, true, {}}, lit)))
    return;
  LCHECK(mean_luma(plain, W, 0, 0, fit.x, H) == 0 && mean_luma(plain, W, fit.x + fit.w, 0, W, H) == 0);
  const double left = mean_luma(lit, W, 0, 0, fit.x, H), right = mean_luma(lit, W, fit.x + fit.w, 0, W, H);
  LCHECK(left > 50 && left < 110 && right > 50 && right < 110);   // 220 at 35%: 77
  bool same = true;
  for (int y = 0; y < H; ++y) {
    same = same && memcmp(&plain[((size_t)y * W + fit.x) * 3], &lit[((size_t)y * W + fit.x) * 3], (size_t)fit.w * 3) == 0;
  }
  LCHECK(same);
  // A black frame leaves them dark; a frame that fills the picture has none.
  std::vector<uint8_t> dark;
  if (LCHECK_OK(draw(grey_frame(64, 48, 0), W, H, fit, LookOptions{Look::sharp, true, {}}, dark)))
    LCHECK(mean_luma(dark, W, 0, 0, fit.x, H) < 2);
  if (LCHECK_OK(draw(bright, 128, 96, RectI{0, 0, 128, 96}, LookOptions{Look::sharp, true, {}}, lit)))
    LCHECK(mean_luma(lit, 128, 0, 0, 128, 96) > 215);
  // Bars above and below (a portrait picture) glow too, with the colour of
  // the frame's edge nearest them.
  const Frame halves = make_frame8(64, 48, [](int, int y) { return (uint8_t)(y < 24 ? 1 : 2); },
                                   {RGBQUAD{0, 0, 0, 0}, RGBQUAD{0, 0, 255, 0}, RGBQUAD{255, 0, 0, 0}});
  const RectI tall = fit_rect(64, 48, 90, 160);
  std::vector<uint8_t> t;
  if (LCHECK_OK(draw(halves, 90, 160, tall, LookOptions{Look::sharp, true, {}}, t))) {
    const uint8_t* top = &t[((size_t)4 * 90 + 45) * 3];
    const uint8_t* bottom = &t[((size_t)155 * 90 + 45) * 3];
    LCHECK(top[2] > 40 && top[0] < 20);      // red above
    LCHECK(bottom[0] > 40 && bottom[2] < 20); // blue below
  }
}

// The pass runner (present_d3d.h's contract): sizes, Source, Original,
// PassN, Params and the constants, a 16-bit float pass, and a pass that
// doesn't compile.
void runner_checks() {
  // A one-pixel checkerboard, 8x8.
  const Frame checker = make_frame8(8, 8, [](int x, int y) { return (uint8_t)((x + y) & 1); },
                                    {RGBQUAD{0, 0, 0, 0}, RGBQUAD{255, 255, 255, 0}});
  PassSpec invert;
  invert.name = "test-invert";
  invert.size = PassSize::original;
  invert.hlsl = R"hlsl(
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
  if (any(OutputSize.xy != OriginalSize.xy) || any(SourceSize.xy != OriginalSize.xy)) return float4(1, 0, 1, 1);
  return float4(1 - Original.Load(int3(pos.xy, 0)).rgb, 1);
}
)hlsl";
  PassSpec half;
  half.name = "test-half";
  half.size = PassSize::source;
  half.scale = 0.5f;
  half.format = PassFormat::rgba16f;
  half.hlsl = R"hlsl(
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
  if (any(OutputSize.xy * 2 != SourceSize.xy)) return float4(1, 0, 1, 1);
  int2 p = (int2)floor(pos.xy) * 2;
  float3 s = Source.Load(int3(p, 0)).rgb + Source.Load(int3(p + int2(1, 0), 0)).rgb +
             Source.Load(int3(p + int2(0, 1), 0)).rgb + Source.Load(int3(p + int2(1, 1), 0)).rgb;
  return float4(s / 4, 1);
}
)hlsl";
  PassSpec out;
  out.name = "test-out";
  out.size = PassSize::fit;
  out.hlsl = R"hlsl(
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
  if (any(OutputSize.xy != FitRect.zw) || any(SourceSize.xy * 2 != OriginalSize.xy) || FrameCount != 0)
    return float4(1, 0, 0, 1);
  if (WindowSize.x != 20 || WindowSize.y != 16) return float4(0, 0, 1, 1);
  float3 c = Pass0.Sample(PointClamp, uv).rgb * Params[0].x + Pass1.Sample(PointClamp, uv).rgb * Params[0].y +
             Source.Sample(PointClamp, uv).rgb * Params[0].z;
  return float4(c, 1);
}
)hlsl";
  const RectI fit{2, 0, 16, 16};
  std::vector<uint8_t> bgr;
  std::string err;
  // Pass0 (the inverted frame, nearest, twice the size) ...
  out.params = {1, 0, 0};
  if (LCHECK_OK(render_passes_bgr_d3d(checker, 20, 16, fit, {invert, half, out}, false, bgr, &err))) {
    bool ok = true;
    for (int y = 0; y < 16; ++y) {
      for (int x = 0; x < 16; ++x) ok = ok && luma_at(bgr, 20, fit.x + x, y) == (((x / 2 + y / 2) & 1) ? 0 : 255);
    }
    LCHECK(ok);
    LCHECK(luma_at(bgr, 20, 0, 5) == 0 && luma_at(bgr, 20, 19, 5) == 0);   // the bars
  } else {
    fprintf(stderr, "looks core: runner: %s\n", err.c_str());
  }
  // ... Pass1 and Source (the half-size average in 16-bit float: grey).
  for (int which : {1, 2}) {
    out.params = {0, which == 1 ? 1.0f : 0.0f, which == 2 ? 1.0f : 0.0f};
    if (LCHECK_OK(render_passes_bgr_d3d(checker, 20, 16, fit, {invert, half, out}, false, bgr, &err))) {
      const int v = luma_at(bgr, 20, 9, 7);
      LCHECK(v >= 127 && v <= 128);
      LCHECK(bgr[((size_t)7 * 20 + 9) * 3] == bgr[((size_t)7 * 20 + 9) * 3 + 2]);   // not a constants' colour
    }
  }
  // A pass that doesn't compile: refused, saying which and why.
  PassSpec broken = out;
  broken.name = "test-broken";
  broken.hlsl = "float4 main(float4 pos : SV_Position) : SV_Target { return nonsense; }";
  err.clear();
  LCHECK(!render_passes_bgr_d3d(checker, 20, 16, fit, {invert, broken}, false, bgr, &err));
  LCHECK(err.find("test-broken") != std::string::npos && err.find("compile") != std::string::npos);
  LCHECK(!render_passes_bgr_d3d(checker, 20, 16, fit, {}, false, bgr, &err));
  // And the runner is fine after it.
  out.params = {1, 0, 0};
  LCHECK(render_passes_bgr_d3d(checker, 20, 16, fit, {invert, half, out}, false, bgr, &err));
}

// Every look's last pass, and only it, fills the fit; present() refuses what
// it can't draw without a crash or a device.
void contract_checks() {
  for (Look l : {Look::sharp, Look::crt, Look::crt_curved, Look::smooth}) {
    const std::vector<PassSpec> passes = look_passes(l);
    if (passes.empty()) continue;
    for (size_t i = 0; i < passes.size(); ++i) {
      LCHECK((passes[i].size == PassSize::fit) == (i + 1 == passes.size()));
      LCHECK(!passes[i].name.empty() && !passes[i].hlsl.empty());
    }
  }
  LCHECK(look_passes(Look::preset).empty());
  const Frame f = grey_frame(16, 12, 100);
  std::vector<uint8_t> bgr;
  std::string err;
  for (Look l : {Look::crt, Look::crt_curved, Look::smooth}) {
    if (!look_passes(l).empty()) continue;
    D3DPresenter p(LookOptions{l, false, {}});
    err.clear();
    LCHECK(!p.present(nullptr, f, RectI{0, 0, 16, 12}, &err));
    LCHECK(err.find("look not available") != std::string::npos);
    LCHECK(!render_frame_bgr_d3d(f, 16, 12, RectI{0, 0, 16, 12}, LookOptions{l, false, {}}, bgr, &err));
  }
  if (!shader_preset_library()) {
    D3DPresenter p(LookOptions{Look::preset, false, L"missing.slangp"});
    bool lost = true;
    LCHECK(!p.present(nullptr, f, RectI{0, 0, 16, 12}, &err, &lost) && !lost);
    LCHECK(err.find("shader preset") != std::string::npos);
  }
  D3DPresenter sharp(LookOptions{Look::sharp, true, {}});
  LCHECK(!sharp.present(nullptr, f, RectI{0, 0, 16, 12}, &err) && !sharp.ready() && sharp.gpu_ms() < 0);
  LCHECK(!sharp.clear(nullptr));
  sharp.release();
  LCHECK(sharp.describe().find("sharp, ambient bars") == 0);
  LCHECK(!render_frame_bgr_d3d(Frame{}, 16, 12, RectI{0, 0, 16, 12}, LookOptions{}, bgr, &err));
  LCHECK(!render_frame_bgr_d3d(f, 0, 12, RectI{0, 0, 16, 12}, LookOptions{}, bgr, &err) && bgr.empty());
}

} // namespace

void core_checks() {
  runner_checks();
  sharp_checks();
  resolve_checks();
  ambient_checks();
  contract_checks();
}

} // namespace adw::scr::looks_test
