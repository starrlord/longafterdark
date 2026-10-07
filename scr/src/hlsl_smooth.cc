// The Smooth look (hlsl_smooth.h): Hyllian's Super-xBR twice over, which
// draws the frame at four times its size with its edges followed instead of
// stepped, then a Catmull-Rom resample of that into the fit rectangle.
//
// The Super-xBR passes are derived from Hyllian's Super-xBR, ported to HLSL
// (shader model 4.0) from libretro's slang-shaders
// (https://github.com/libretro/slang-shaders), files
// edge-smoothing/xbr/shaders/super-xbr/super-xbr-pass0.slang, -pass1.slang
// and -pass2.slang at commit 1e0238f9fdd4668ce8212c31d80877af605d3b53 (last
// changed upstream in 1f70a4365030706cb74999cd9415848c650b5c68), under this
// notice:
//
//   Copyright (c) 2015 Hyllian - sergiogdb@gmail.com
//
//   Permission is hereby granted, free of charge, to any person obtaining a copy
//   of this software and associated documentation files (the "Software"), to deal
//   in the Software without restriction, including without limitation the rights
//   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//   copies of the Software, and to permit persons to whom the Software is
//   furnished to do so, subject to the following conditions:
//
//   The above copyright notice and this permission notice shall be included in
//   all copies or substantial portions of the Software.
//
//   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
//   THE SOFTWARE.
//
// The arithmetic is upstream's, and so are the numbers it works on. As in
// upstream's preset, a pass before each Super-xBR stores every pixel's luma
// in its alpha, in 8 bits (support/luma.slang, two lines, rewritten here),
// and the passes read luma from alpha and keep their own colours' there.
// Working luma out where it is read instead differs in the last bit now and
// then, and in a dither, where Super-xBR's edge measures tie, that is enough
// to break a tie the other way. So the 4x picture matches upstream's chain,
// run by librashader on the same GPU with 8-bit intermediate textures, to
// the bit (upstream's preset asks for sRGB ones, which round a level or two
// differently). What the port changes around the arithmetic:
// - Upstream nudges its texture coordinates by 1.0001 to keep its taps off
//   the texels' edges. Here they are snapped to the output's texel centres
//   instead, which keeps every tap a quarter or half a texel inside its texel
//   at any size (the nudge, a fraction of the coordinate, would reach half a
//   texel on the 4x picture of a wide frame).
// - The three passes' common half (the edge measures, the filters along the
//   two strongest directions, their blend and the anti-ringing clamp) is
//   written once, in xbr().
// - The parameters are upstream's defaults as its super-xbr.slangp runs them
//   (smooth_passes()).
// Upstream's preset ends with a Jinc2 resample and a deblur pass, both under
// the GPL; neither is used here. The resample into the fit rectangle
// (kFitPass) is our own.
#include "hlsl_smooth.h"

namespace adw::scr {

namespace {

// What the three Super-xBR passes share. Each pass's Params[0] is (MODE,
// edge strength, XBR_WEIGHT, -) and Params[1] (XBR_EDGE_SHP,
// XBR_TEXTURE_SHP, -, -), upstream's parameters.
constexpr char kXbrCommon[] = R"hlsl(
#define MODE Params[0].x
#define XBR_EDGE_STR Params[0].y
#define XBR_WEIGHT Params[0].z
#define XBR_EDGE_SHP Params[1].x
#define XBR_TEXTURE_SHP Params[1].y

static const float3 Y = float3(0.2126, 0.7152, 0.0722);

float RGBtoYUV(float3 color) { return dot(color, Y); }

float df(float A, float B) { return abs(A - B); }

/*
                              P1
     |P0|B |C |P1|         C     F4          |a0|b1|c2|d3|
     |D |E |F |F4|      B     F     I4       |b0|c1|d2|e3|   |e1|i1|i2|e2|
     |G |H |I |I4|   P0    E  A  I     P3    |c0|d1|e2|f3|   |e3|i3|i4|e4|
     |P2|H5|I5|P3|      D     H     I5       |d0|e1|f2|g3|
                           G     H5
                              P2
*/

float d_wd(float wp1, float wp2, float wp3, float wp4, float wp5, float wp6, float b0, float b1, float c0, float c1,
           float c2, float d0, float d1, float d2, float d3, float e1, float e2, float e3, float f2, float f3) {
  return (wp1 * (df(c1, c2) + df(c1, c0) + df(e2, e1) + df(e2, e3)) + wp2 * (df(d2, d3) + df(d0, d1)) +
          wp3 * (df(d1, d3) + df(d0, d2)) + wp4 * df(d1, d2) + wp5 * (df(c0, c2) + df(e1, e3)) +
          wp6 * (df(b0, b1) + df(f2, f3)));
}

float hv_wd(float wp1, float wp2, float wp3, float wp4, float wp5, float wp6, float i1, float i2, float i3, float i4,
            float e1, float e2, float e3, float e4) {
  return (wp4 * (df(i1, i2) + df(i3, i4)) + wp1 * (df(i1, e1) + df(i2, e2) + df(i3, e3) + df(i4, e4)) +
          wp3 * (df(i1, e2) + df(i3, e4) + df(e1, i2) + df(e3, i4)));
}

float3 min4(float3 a, float3 b, float3 c, float3 d) { return min(a, min(b, min(c, d))); }

float3 max4(float3 a, float3 b, float3 c, float3 d) { return max(a, max(b, max(c, d))); }

float max4float(float a, float b, float c, float d) { return max(a, max(b, max(c, d))); }

// The centre of the output texel `uv` falls in: every tap then sits a
// quarter or half a texel inside the texel it reads.
float2 texel_centre(float2 uv) { return (floor(uv * OutputSize.xy) + 0.5) * OutputSize.zw; }

// A texel of a pass before this one: its colour, and its luma in alpha.
float4 tap(Texture2D t, float2 c) { return t.SampleLevel(PointClamp, c, 0); }

// The 4x4 neighbourhood a pass reads, named as in the diagram: colour and
// luma.
struct Taps {
  float4 P0, B, C, P1, D, E, F, F4, G, H, I, I4, P2, H5, I5, P3;
};

// The new colour between E, F, H and I.
float3 xbr(Taps t, float wp1, float wp2, float wp3, float wp4, float wp5, float wp6, float weight1, float weight2) {
  float3 P0 = t.P0.rgb, B = t.B.rgb, C = t.C.rgb, P1 = t.P1.rgb;
  float3 D = t.D.rgb, E = t.E.rgb, F = t.F.rgb, F4 = t.F4.rgb;
  float3 G = t.G.rgb, H = t.H.rgb, I = t.I.rgb, I4 = t.I4.rgb;
  float3 P2 = t.P2.rgb, H5 = t.H5.rgb, I5 = t.I5.rgb, P3 = t.P3.rgb;
  float p0 = t.P0.a, b = t.B.a, c = t.C.a, p1 = t.P1.a, d = t.D.a, e = t.E.a, f = t.F.a, f4 = t.F4.a;
  float g = t.G.a, h = t.H.a, i = t.I.a, i4 = t.I4.a, p2 = t.P2.a, h5 = t.H5.a, i5 = t.I5.a, p3 = t.P3.a;

  /* Calc edgeness in diagonal directions. */
  float d_edge = (d_wd(wp1, wp2, wp3, wp4, wp5, wp6, d, b, g, e, c, p2, h, f, p1, h5, i, f4, i5, i4) -
                  d_wd(wp1, wp2, wp3, wp4, wp5, wp6, c, f4, b, f, i4, p0, e, i, p3, d, h, i5, g, h5));

  /* Calc edgeness in horizontal/vertical directions. */
  float hv_edge = (hv_wd(wp1, wp2, wp3, wp4, wp5, wp6, f, i, e, h, c, i5, b, h5) -
                   hv_wd(wp1, wp2, wp3, wp4, wp5, wp6, e, f, h, i, d, f4, g, i4));

  float limits = XBR_EDGE_STR + 0.000001;
  float edge_strength = smoothstep(0.0, limits, abs(d_edge));

  float4 w1, w2;
  float3 c3, c4;
  if (MODE == 2.0) {
    float contrast = max(max4float(df(e, f), df(e, i), df(e, h), df(f, h)), max(df(f, i), df(h, i))) / (e + 0.001);

    float wgt1 = weight1 * (smoothstep(0.0, 0.6, contrast) * XBR_EDGE_SHP + XBR_TEXTURE_SHP);
    float wgt2 = weight2 * (smoothstep(0.0, 0.6, contrast) * XBR_EDGE_SHP + XBR_TEXTURE_SHP);

    /* Filter weights. Two taps only. */
    w1 = float4(-wgt1, wgt1 + 0.5, wgt1 + 0.5, -wgt1);
    w2 = float4(-wgt2, wgt2 + 0.25, wgt2 + 0.25, -wgt2);
    c3 = mul(w2, float4x3(P0 + 2.0 * (D + G) + P2, B + 2.0 * (E + H) + H5, C + 2.0 * (F + I) + I5,
                          P1 + 2.0 * (F4 + I4) + P3)) / 3.0;
    c4 = mul(w2, float4x3(P0 + 2.0 * (C + B) + P1, D + 2.0 * (F + E) + F4, G + 2.0 * (I + H) + I4,
                          P2 + 2.0 * (I5 + H5) + P3)) / 3.0;
  } else {
    /* Filter weights. Two taps only. */
    w1 = float4(-weight1, weight1 + 0.5, weight1 + 0.5, -weight1);
    w2 = float4(-weight2, weight2 + 0.25, weight2 + 0.25, -weight2);
    c3 = mul(w2, float4x3(D + G, E + H, F + I, F4 + I4));
    c4 = mul(w2, float4x3(C + B, F + E, I + H, I5 + H5));
  }

  /* Filtering and normalization in four direction generating four colors. */
  float3 c1 = mul(w1, float4x3(P2, H, F, P1));
  float3 c2 = mul(w1, float4x3(P0, E, I, P3));

  /* Smoothly blends the two strongest directions (one in diagonal and the other in vert/horiz direction). */
  float3 color = lerp(lerp(c1, c2, step(0.0, d_edge)), lerp(c3, c4, step(0.0, hv_edge)), 1.0 - edge_strength);

  /* Anti-ringing code. */
  float3 min_sample = min4(E, F, H, I);
  float3 max_sample = max4(E, F, H, I);
  return clamp(color, min_sample, max_sample);
}
)hlsl";

// Upstream's luma pass (support/luma.slang), at the size of its input: the
// picture as it is, its luma in alpha for the Super-xBR passes to read.
constexpr char kLumaPass[] = R"hlsl(
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
  float3 color = tap(Source, texel_centre(uv)).rgb;
  return float4(color, RGBtoYUV(color));
}
)hlsl";

// Upstream's pass0, at the size of its input: the colour at the corner
// between each pixel and the ones right, below and below right of it.
constexpr char kXbrPass0[] = R"hlsl(
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
  // settings //
  float wp1 = 2.0, wp2 = 1.0, wp3 = -1.0, wp4 = 4.0, wp5 = -1.0, wp6 = 1.0;
  float weight1 = (XBR_WEIGHT * 1.29633 / 10.0);
  float weight2 = (XBR_WEIGHT * 1.75068 / 10.0 / 2.0);
  // end settings //

  float2 tc = texel_centre(uv);
  float dx = SourceSize.z, dy = SourceSize.w;
  Taps t;
  t.P0 = tap(Source, tc + float2(-dx, -dy));
  t.P1 = tap(Source, tc + float2(2.0 * dx, -dy));
  t.P2 = tap(Source, tc + float2(-dx, 2.0 * dy));
  t.P3 = tap(Source, tc + float2(2.0 * dx, 2.0 * dy));
  t.B = tap(Source, tc + float2(0.0, -dy));
  t.C = tap(Source, tc + float2(dx, -dy));
  t.H5 = tap(Source, tc + float2(0.0, 2.0 * dy));
  t.I5 = tap(Source, tc + float2(dx, 2.0 * dy));
  t.D = tap(Source, tc + float2(-dx, 0.0));
  t.F4 = tap(Source, tc + float2(2.0 * dx, 0.0));
  t.G = tap(Source, tc + float2(-dx, dy));
  t.I4 = tap(Source, tc + float2(2.0 * dx, dy));
  t.E = tap(Source, tc);
  t.F = tap(Source, tc + float2(dx, 0.0));
  t.H = tap(Source, tc + float2(0.0, dy));
  t.I = tap(Source, tc + float2(dx, dy));
  float3 color = xbr(t, wp1, wp2, wp3, wp4, wp5, wp6, weight1, weight2);
  return float4(color, RGBtoYUV(color));
}
)hlsl";

// Upstream's pass1, at twice the size: the luma pass's picture (XbrSource,
// defined in front of this) and pass0's corners laid out as a checkerboard,
// the other half of the pixels found along the strongest directions of that
// grid turned 45 degrees. Its picture sits a quarter of an input pixel up and
// left of the input's; pass2 puts it back.
constexpr char kXbrPass1[] = R"hlsl(
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
  // settings //
  float wp1, wp2, wp3, wp4, wp5, wp6, weight1, weight2;
  if (MODE == 1.0) {
    wp1 = 1.0;
    wp2 = 0.0;
    wp3 = 0.0;
    wp4 = 0.0;
    wp5 = 0.0;
    wp6 = 0.0;

    weight1 = (XBR_WEIGHT * 1.29633 / 10.0);
    weight2 = (XBR_WEIGHT * 1.75068 / 10.0 / 2.0);
  } else if (MODE == 2.0) {
    wp1 = 8.0;
    wp2 = 0.0;
    wp3 = 0.0;
    wp4 = 0.0;
    wp5 = 0.0;
    wp6 = 0.0;

    weight1 = (1.29633 / 10.0);
    weight2 = (1.75068 / 10.0 / 2.0);
  } else {
    wp1 = 8.0;
    wp2 = 0.0;
    wp3 = 0.0;
    wp4 = 0.0;
    wp5 = 0.0;
    wp6 = 0.0;

    weight1 = (XBR_WEIGHT * 1.29633 / 10.0);
    weight2 = (XBR_WEIGHT * 1.75068 / 10.0 / 2.0);
  }
  // end settings //

  float2 tc = texel_centre(uv);

  // Skip pixels on wrong grid
  float2 fp = frac(tc * SourceSize.xy);
  float2 dir = fp - float2(0.5, 0.5);
  if ((dir.x * dir.y) > 0.0) return lerp(tap(XbrSource, tc), tap(Source, tc), step(0.0, dir.x));

  float2 g1 = (fp.x > 0.5) ? float2(0.5 / SourceSize.x, 0.0) : float2(0.0, 0.5 / SourceSize.y);
  float2 g2 = (fp.x > 0.5) ? float2(0.0, 0.5 / SourceSize.y) : float2(0.5 / SourceSize.x, 0.0);

  Taps t;
  t.P0 = tap(XbrSource, tc - 3.0 * g1);
  t.P1 = tap(Source, tc - 3.0 * g2);
  t.P2 = tap(Source, tc + 3.0 * g2);
  t.P3 = tap(XbrSource, tc + 3.0 * g1);

  t.B = tap(Source, tc - 2.0 * g1 - g2);
  t.C = tap(XbrSource, tc - g1 - 2.0 * g2);
  t.D = tap(Source, tc - 2.0 * g1 + g2);
  t.E = tap(XbrSource, tc - g1);
  t.F = tap(Source, tc - g2);
  t.G = tap(XbrSource, tc - g1 + 2.0 * g2);
  t.H = tap(Source, tc + g2);
  t.I = tap(XbrSource, tc + g1);

  t.F4 = tap(XbrSource, tc + g1 - 2.0 * g2);
  t.I4 = tap(Source, tc + 2.0 * g1 - g2);
  t.H5 = tap(XbrSource, tc + g1 + 2.0 * g2);
  t.I5 = tap(Source, tc + 2.0 * g1 + g2);
  float3 color = xbr(t, wp1, wp2, wp3, wp4, wp5, wp6, weight1, weight2);
  return float4(color, RGBtoYUV(color));
}
)hlsl";

// Upstream's pass2, at the size of its input: every pixel again, from the
// 4x4 around its top left corner, which moves pass1's picture back down and
// right by the quarter of an input pixel it was off.
constexpr char kXbrPass2[] = R"hlsl(
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
  // settings //
  float wp1, wp2, wp3, wp4, wp5, wp6, weight1, weight2;
  if (MODE == 1.0) {
    wp1 = 0.0;
    wp2 = 0.0;
    wp3 = 0.0;
    wp4 = 1.0;
    wp5 = 0.0;
    wp6 = 0.0;

    weight1 = (XBR_WEIGHT * 1.29633 / 10.0);
    weight2 = (XBR_WEIGHT * 1.75068 / 10.0 / 2.0);
  } else if (MODE == 2.0) {
    wp1 = 1.0;
    wp2 = 0.0;
    wp3 = 1.0;
    wp4 = 3.0;
    wp5 = -2.0;
    wp6 = 0.0;

    weight1 = (1.29633 / 10.0);
    weight2 = (1.75068 / 10.0 / 2.0);
  } else {
    wp1 = 1.0;
    wp2 = 0.0;
    wp3 = 2.0;
    wp4 = 3.0;
    wp5 = -2.0;
    wp6 = 1.0;

    weight1 = (XBR_WEIGHT * 1.29633 / 10.0);
    weight2 = (XBR_WEIGHT * 1.75068 / 10.0 / 2.0);
  }
  // end settings //

  float2 tc = texel_centre(uv);
  float dx = SourceSize.z, dy = SourceSize.w;
  Taps t;
  t.P0 = tap(Source, tc + float2(-2.0 * dx, -2.0 * dy));
  t.P1 = tap(Source, tc + float2(dx, -2.0 * dy));
  t.P2 = tap(Source, tc + float2(-2.0 * dx, dy));
  t.P3 = tap(Source, tc + float2(dx, dy));
  t.B = tap(Source, tc + float2(-dx, -2.0 * dy));
  t.C = tap(Source, tc + float2(0.0, -2.0 * dy));
  t.H5 = tap(Source, tc + float2(-dx, dy));
  t.I5 = tap(Source, tc + float2(0.0, dy));
  t.D = tap(Source, tc + float2(-2.0 * dx, -dy));
  t.F4 = tap(Source, tc + float2(dx, -dy));
  t.G = tap(Source, tc + float2(-2.0 * dx, 0.0));
  t.I4 = tap(Source, tc + float2(dx, 0.0));
  t.E = tap(Source, tc + float2(-dx, -dy));
  t.F = tap(Source, tc + float2(0.0, -dy));
  t.H = tap(Source, tc + float2(-dx, 0.0));
  t.I = tap(Source, tc);
  return float4(xbr(t, wp1, wp2, wp3, wp4, wp5, wp6, weight1, weight2), 1.0);
}
)hlsl";

// Our own: the 4x picture into the fit rectangle with a Catmull-Rom cubic,
// which hands texels back as they are where they line up and keeps edges
// crisp. It shrinks the picture as often as it grows it (480 lines on a
// 1080p monitor are 2.25 screen pixels a line, the 4x picture 4), so its
// kernel spans output pixels, not texels, while it shrinks: then it filters
// every texel rather than skipping some, which would bring the steps back
// as jaggies. The result is held to the range of the texels under the
// kernel's positive lobe, so its negative lobes can't ring (no dark fringe
// beside a bright edge).
constexpr char kFitPass[] = R"hlsl(
float catmull_rom(float x) {
  x = abs(x);
  if (x < 1.0) return (1.5 * x - 2.5) * x * x + 1.0;
  if (x < 2.0) return ((-0.5 * x + 2.5) * x - 4.0) * x + 2.0;
  return 0.0;
}

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
  int2 last = int2(SourceSize.xy) - 1;
  // This pixel's centre in the 4x picture, in texels (their centres whole).
  float2 p = uv * SourceSize.xy - 0.5;
  // The kernel's unit: a texel while the picture grows, an output pixel
  // while it shrinks (at most 8 texels, a fit rectangle half the frame's
  // size; a smaller one skips some).
  float2 s = clamp(SourceSize.xy * OutputSize.zw, 1.0, 8.0);
  int2 lo = int2(floor(p - 2.0 * s)) + 1;
  int2 hi = int2(floor(p + 2.0 * s));
  float3 sum = 0.0;
  float wsum = 0.0;
  float3 least = 1.0, most = 0.0;
  [loop] for (int y = lo.y; y <= hi.y; ++y) {
    float ty = (y - p.y) / s.y;
    float wy = catmull_rom(ty);
    [loop] for (int x = lo.x; x <= hi.x; ++x) {
      float tx = (x - p.x) / s.x;
      float w = catmull_rom(tx) * wy;
      float3 c = Source.Load(int3(clamp(int2(x, y), 0, last), 0)).rgb;
      sum += w * c;
      wsum += w;
      if (abs(tx) < 1.0 && abs(ty) < 1.0) {
        least = min(least, c);
        most = max(most, c);
      }
    }
  }
  return float4(clamp(sum / wsum, least, most), 1.0);
}
)hlsl";

// Upstream's defaults, as its super-xbr.slangp runs them: Mode 1
// ("Details"), an edge strength of 3 in each pass (the preset's; the
// parameters' own default is 5), XBR_WEIGHT 0 in pass0 (its parameter's
// default) and 1 in pass1 and pass2 (a constant there), the adaptive mode's
// sharpness 0.4 and 1 (unused in Mode 1).
constexpr float kMode = 1.0f;
constexpr float kEdgeStrength = 3.0f;
constexpr float kEdgeSharp = 0.4f;
constexpr float kTextureSharp = 1.0f;

PassSpec xbr_pass(const char* name, const char* xbr_source, const char* body, float scale, float weight) {
  PassSpec p;
  p.name = name;
  if (xbr_source) p.hlsl = std::string("#define XbrSource ") + xbr_source + "\n";
  p.hlsl += kXbrCommon;
  p.hlsl += body;
  p.size = PassSize::source;
  p.scale = scale;
  p.format = PassFormat::rgba8;
  p.params = {kMode, kEdgeStrength, weight, 0, kEdgeSharp, kTextureSharp, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  return p;
}

} // namespace

std::vector<PassSpec> smooth_passes() {
  std::vector<PassSpec> passes;
  // Super-xBR 2x from the frame (the first pass's Source is the frame), then
  // 2x again from that: pass1 of each reads its luma pass's picture, Pass0
  // the first time and Pass4 the second.
  passes.push_back(xbr_pass("smooth-xbr1-luma", nullptr, kLumaPass, 1.0f, 0.0f));
  passes.push_back(xbr_pass("smooth-xbr1-corners", nullptr, kXbrPass0, 1.0f, 0.0f));
  passes.push_back(xbr_pass("smooth-xbr1-double", "Pass0", kXbrPass1, 2.0f, 1.0f));
  passes.push_back(xbr_pass("smooth-xbr1-refine", nullptr, kXbrPass2, 1.0f, 1.0f));
  passes.push_back(xbr_pass("smooth-xbr2-luma", nullptr, kLumaPass, 1.0f, 0.0f));
  passes.push_back(xbr_pass("smooth-xbr2-corners", nullptr, kXbrPass0, 1.0f, 0.0f));
  passes.push_back(xbr_pass("smooth-xbr2-double", "Pass4", kXbrPass1, 2.0f, 1.0f));
  passes.push_back(xbr_pass("smooth-xbr2-refine", nullptr, kXbrPass2, 1.0f, 1.0f));
  PassSpec fit;
  fit.name = "smooth-fit";
  fit.hlsl = kFitPass;
  fit.size = PassSize::fit;
  passes.push_back(std::move(fit));
  return passes;
}

} // namespace adw::scr
