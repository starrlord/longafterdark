// The Direct3D 11 presenter's own shaders (present_d3d.cc): the full-screen
// triangle every pass draws, the resolve of a frame to colour (Original), the
// sharp look and the ambient bars. The looks' passes are elsewhere
// (hlsl_crt.h, hlsl_smooth.h); all of them follow present_d3d.h's pass
// contract.
#pragma once

namespace adw::scr::hlsl {

// Every pass's vertex shader: one triangle that covers the viewport, uv
// running 0..1 across it (0, 0 at the top left).
inline constexpr char kFullScreenVs[] = R"hlsl(
struct VsOut {
  float4 pos : SV_Position;
  float2 uv : TEXCOORD0;
};
VsOut main(uint id : SV_VertexID) {
  VsOut o;
  float2 uv = float2((id << 1) & 2, id & 2);
  o.uv = uv;
  o.pos = float4(uv.x * 2 - 1, 1 - uv.y * 2, 0, 1);
  return o;
}
)hlsl";

// The resolve: an 8-bit frame's indices through its palette (a 256x1
// B8G8R8A8 texture, RGBQUAD's own byte order), or a 32-bit frame as it is,
// into Original at the frame's size, alpha 1 (the frames' fourth byte is
// padding). Compiled on its own, without the prelude.
inline constexpr char kResolve8Ps[] = R"hlsl(
Texture2D<uint> Indices : register(t0);
Texture2D Palette : register(t1);
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
  uint i = Indices.Load(int3(pos.xy, 0));
  return float4(Palette.Load(int3(i, 0, 0)).rgb, 1);
}
)hlsl";

inline constexpr char kResolve32Ps[] = R"hlsl(
Texture2D Frame : register(t0);
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
  return float4(Frame.Load(int3(pos.xy, 0)).rgb, 1);
}
)hlsl";

// The sharp look, one fit pass: what Direct2D draws today (present.cc,
// draw()). An upscale there is the frame repeated k = ceil(scale) times,
// nearest neighbour, then drawn linearly to the fit, so every frame pixel is
// a crisp block, blended only in the one column or row where two blocks
// meet, by how much of each it covers. Here that is one step: for each
// output pixel, where its linear sample falls in the k-times picture and
// which (at most two) frame pixels the sample spans there. At 1:1 it is the
// frame's own pixels. A downscale (a small /window) is Direct2D's
// high-quality cubic: Keys' cubic (Catmull-Rom) widened to the ratio, as
// measured on Direct2D's drawing (research/looks/core/FACTS.md).
inline constexpr char kSharpPs[] = R"hlsl(
// One axis of the upscale: output pixel `o` of `m` (the fit), a frame of `n`
// pixels repeated `k` times. The frame pixels a and b the linear sample
// spans, and b's weight.
void sharp_axis(float o, float n, float m, uint k, out uint a, out uint b, out float t) {
  float u = (o + 0.5) * (n * k / m) - 0.5;   // in the k-times picture, pixel centres on integers
  float j = floor(u);
  t = u - j;
  int last = (int)(n * k) - 1;
  a = (uint)clamp((int)j, 0, last) / k;
  b = (uint)clamp((int)j + 1, 0, last) / k;
}

// Keys' cubic, a = -0.5 (Catmull-Rom).
float keys(float x) {
  x = abs(x);
  return x < 1 ? (1.5 * x - 2.5) * x * x + 1 : x < 2 ? ((-0.5 * x + 2.5) * x - 4) * x + 2 : 0;
}

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
  float2 n = OriginalSize.xy, m = OutputSize.xy;
  float2 o = floor(pos.xy - FitRect.xy);   // this pixel within the fit
  // Direct2D's prescale: the scale rounded up, within 8192 pixels a side.
  uint k = (uint)ceil(max(m.x / n.x, m.y / n.y) - 1e-6);
  k = max(1, min(k, min((uint)(8192 / n.x), (uint)(8192 / n.y))));
  if (k > 1 || all(m == n)) {
    uint ax, bx, ay, by;
    float tx, ty;
    sharp_axis(o.x, n.x, m.x, k, ax, bx, tx);
    sharp_axis(o.y, n.y, m.y, k, ay, by, ty);
    float3 c00 = Original.Load(int3(ax, ay, 0)).rgb, c10 = Original.Load(int3(bx, ay, 0)).rgb;
    float3 c01 = Original.Load(int3(ax, by, 0)).rgb, c11 = Original.Load(int3(bx, by, 0)).rgb;
    return float4(lerp(lerp(c00, c10, tx), lerp(c01, c11, tx), ty), 1);
  }
  // A downscale: the frame pixels within two of this pixel's widths, by
  // Keys' cubic at their distance in those widths.
  float2 r = n / m;
  float2 c = (o + 0.5) * r;   // this pixel's centre in the frame
  int2 first = (int2)floor(c - 2 * r), last = min((int2)ceil(c + 2 * r), first + 64);
  float3 sum = 0;
  float wsum = 0;
  [loop] for (int y = first.y; y <= last.y; ++y) {
    float wy = keys((y + 0.5 - c.y) / r.y);
    [loop] for (int x = first.x; x <= last.x; ++x) {
      float w = keys((x + 0.5 - c.x) / r.x) * wy;
      sum += Original.Load(int3(clamp(int2(x, y), 0, (int2)n - 1), 0)).rgb * w;
      wsum += w;
    }
  }
  return float4(sum / wsum, 1);
}
)hlsl";

// ---- the ambient bars (LookOptions::ambient) ------------------------------------
// The frame shrunk to a small picture (about 1/16 a side), blurred there,
// then scaled to cover the window behind the frame: the bars glow with the
// colours near the frame's edges. All of it costs a few small passes and one
// over the window.

// Original shrunk to this pass's output, by area.
inline constexpr char kAmbientDownPs[] = R"hlsl(
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
  float2 n = OriginalSize.xy;
  float2 r = n / OutputSize.xy;
  float2 lo = floor(pos.xy) * r, hi = lo + r;
  int2 first = (int2)floor(lo), last = min((int2)ceil(hi) - 1, first + 63);
  float3 sum = 0;
  float wsum = 0;
  [loop] for (int y = first.y; y <= last.y; ++y) {
    float wy = min(hi.y, y + 1) - max(lo.y, y);
    [loop] for (int x = first.x; x <= last.x; ++x) {
      float w = (min(hi.x, x + 1) - max(lo.x, x)) * wy;
      sum += Original.Load(int3(clamp(int2(x, y), 0, (int2)n - 1), 0)).rgb * w;
      wsum += w;
    }
  }
  return float4(sum / max(wsum, 1e-6), 1);
}
)hlsl";

// A Gaussian along Params[0].xy (1, 0 or 0, 1): nine taps of Source, the
// edges repeated.
inline constexpr char kAmbientBlurPs[] = R"hlsl(
static const float kWeights[5] = {0.2270270270, 0.1945945946, 0.1216216216, 0.0540540541, 0.0162162162};
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
  int2 p = (int2)pos.xy, dir = (int2)Params[0].xy, edge = (int2)SourceSize.xy - 1;
  float3 sum = Source.Load(int3(p, 0)).rgb * kWeights[0];
  [unroll] for (int i = 1; i < 5; ++i) {
    sum += (Source.Load(int3(clamp(p + dir * i, 0, edge), 0)).rgb +
            Source.Load(int3(clamp(p - dir * i, 0, edge), 0)).rgb) * kWeights[i];
  }
  return float4(sum, 1);
}
)hlsl";

// The blurred picture (Source) over the whole window: scaled to cover it,
// the frame's shape kept (the fit's), centred, the overflow cropped; a cubic
// B-spline between its pixels so nothing shows of them; dimmed to Params[0].x
// and its colour kept by Params[0].y (a little desaturated). A fixed pinch of
// noise keeps the dim ramps from banding (fixed: nothing moves).
inline constexpr char kAmbientFillPs[] = R"hlsl(
float3 bspline(float2 p) {   // p in Source's pixels, a pixel's centre at +0.5
  float2 x = p - 0.5;
  float2 i = floor(x), f = x - i, f2 = f * f, f3 = f2 * f;
  float2 w0 = (1 - 3 * f + 3 * f2 - f3) / 6;
  float2 w1 = (4 - 6 * f2 + 3 * f3) / 6;
  float2 w2 = (1 + 3 * f + 3 * f2 - 3 * f3) / 6;
  float2 w3 = f3 / 6;
  float2 g0 = w0 + w1, g1 = w2 + w3;
  float2 h0 = (i - 1 + w1 / g0 + 0.5) * SourceSize.zw;
  float2 h1 = (i + 1 + w3 / g1 + 0.5) * SourceSize.zw;
  return g0.y * (g0.x * Source.SampleLevel(LinearClamp, h0, 0).rgb +
                 g1.x * Source.SampleLevel(LinearClamp, float2(h1.x, h0.y), 0).rgb) +
         g1.y * (g0.x * Source.SampleLevel(LinearClamp, float2(h0.x, h1.y), 0).rgb +
                 g1.x * Source.SampleLevel(LinearClamp, h1, 0).rgb);
}

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
  float2 win = WindowSize.xy, shape = max(FitRect.zw, 1);
  float s = max(win.x / shape.x, win.y / shape.y);
  float2 shown = shape * s;
  float2 p = (pos.xy - (win - shown) * 0.5) / shown;   // 0..1 across the covering picture
  float3 c = bspline(p * SourceSize.xy);
  float l = dot(c, float3(0.299, 0.587, 0.114));
  c = lerp(float3(l, l, l), c, Params[0].y) * Params[0].x;
  float noise = frac(52.9829189 * frac(dot(pos.xy, float2(0.06711056, 0.00583715)))) - 0.5;
  return float4(saturate(c + noise / 255), 1);
}
)hlsl";

} // namespace adw::scr::hlsl
