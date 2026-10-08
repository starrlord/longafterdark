// The CRT look: a good VGA monitor of 1995 (31 kHz, 480 lines), the kind the
// modules were drawn on. Our own shader, after the ideas every CRT shader
// shares (Timothy Lottes's public-domain crt-lottes among them): a soft spot
// across, each line a beam of its own down, a phosphor mask, a little glow,
// in linear light.
//
// Four passes (present_d3d.h's contract):
//   crt-linear  the frame decoded to linear light, at its own size (16-bit
//               float, so the dark end keeps its steps)
//   crt-glow-h  half size: a wide horizontal blur of it (the glow)
//   crt-glow-v  the same down
//   crt (crt-curved)  the fit rectangle: the beam, the scanlines, the mask,
//               the glow and, behind curved glass, the curve; encoded again
// About 0.2 ms of an RTX 4090's time at 3840x2160, all four.
#include "hlsl_crt.h"

namespace adw::scr {

namespace {

// What the look is made of (frame pixels, or lines, unless said otherwise).
// The pictures in research/looks/crt/ were tuned with these.
constexpr float kGamma = 2.2f;       // the palette's bytes are about this much gamma-encoded
constexpr float kGlowWidth = 3.0f;   // the glow's blur (sigma)
constexpr float kGlow = 0.035f;      // how much of the light goes to the glow
constexpr float kSpot = 0.33f;       // the spot across (sigma): soft, but text stays crisp
constexpr float kLineDark = 0.30f;   // a dark line's beam (sigma, in lines)...
constexpr float kLineBright = 0.44f; // ...and a bright one's: bright lines all but close the gaps
constexpr float kMask = 0.22f;       // how far the mask dims the two phosphors not lit
constexpr float kBend = 0.045f;      // curved: how far the corners pull in (of the half height)
constexpr float kCorner = 0.035f;    // curved: the corners' radius (of the height)
constexpr float kVignette = 0.07f;   // curved: how much darker the edges are (the corners twice)

constexpr char kLinear[] = R"hlsl(
// Params[0].x: the gamma.
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
  float3 c = Original.Load(int3(pos.xy, 0)).rgb;
  return float4(pow(saturate(c), Params[0].x), 1);
}
)hlsl";

// Half size: each tap, halfway between texels, is the mean of a 2x2 block.
constexpr char kGlowH[] = R"hlsl(
// Params[0].x: the glow's width (sigma, frame pixels).
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
  float k = -0.5 / (Params[0].x * Params[0].x);
  float3 sum = 0;
  float total = 0;
  [unroll] for (int i = -4; i <= 4; ++i) {
    float d = 2.0 * i;
    float w = exp(k * d * d);
    sum += w * Source.SampleLevel(LinearClamp, uv + float2(d * SourceSize.z, 0), 0).rgb;
    total += w;
  }
  return float4(sum / total, 1);
}
)hlsl";

constexpr char kGlowV[] = R"hlsl(
// Params[0].x: the glow's width (sigma, frame pixels; this texture is half the frame).
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
  float k = -0.5 / (Params[0].x * Params[0].x);
  float3 sum = 0;
  float total = 0;
  [unroll] for (int i = -4; i <= 4; ++i) {
    float d = 2.0 * i;
    float w = exp(k * d * d);
    sum += w * Source.SampleLevel(LinearClamp, uv + float2(0, i * SourceSize.w), 0).rgb;
    total += w;
  }
  return float4(sum / total, 1);
}
)hlsl";

constexpr char kFinal[] = R"hlsl(
// Params[0]: x gamma, y the glow's share, z curved (1) or flat (0)
// Params[1]: x the spot across (sigma, frame pixels), y a dark line's beam,
//            z a bright line's (sigma, lines)
// Params[2]: x the mask's strength, y vignette, z bend, w corner radius
// Pass0 is the frame in linear light, Pass2 the glow.

float3 texel(int2 p) {
  return Pass0.Load(int3(clamp(p, int2(0, 0), int2(OriginalSize.xy) - 1), 0)).rgb;
}

// A Gaussian of sigma s, d from its middle (not normalised).
float gauss(float d, float s) {
  return exp(-0.5 * d * d / (s * s));
}

// Row y of the frame through the spot, around frame pixel x = n (f from
// its centre): the three pixels nearest, `w` their weights, and in a small
// window more each side, out to r, each weighed by the spot (sigma sx)
// over `spot`, all its weights' sum.
float3 across(int n, int y, float f, float3 w, int r, float sx, float spot) {
  float3 c = w.x * texel(int2(n - 1, y)) + w.y * texel(int2(n, y)) + w.z * texel(int2(n + 1, y));
  [loop] for (int i = 2; i <= r; ++i) {
    c += gauss(f + i, sx) / spot * texel(int2(n - i, y));
    c += gauss(f - i, sx) / spot * texel(int2(n + i, y));
  }
  return c;
}

// The beam's sums down, line by line (main's Down).
struct Lines {
  float3 lit;     // with scanlines: each line's beam
  float3 blend;   // without: the lines blended
  float total;    // the blend's weights
};

// A line into the sums: c its row through the spot, dy lines from the
// screen pixel; its beam's sigma by its brightness, never under `least`,
// the blend's sb.
void add_line(inout Lines sum, float3 c, float dy, float sb, float least) {
  float3 sy = max(lerp(Params[1].y, Params[1].z, sqrt(saturate(c))), least);
  sum.lit += c * exp(-0.5 * dy * dy / (sy * sy)) / sy;
  float w = exp(-0.5 * dy * dy / (sb * sb));
  sum.blend += c * w;
  sum.total += w;
}

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
  // Screen pixels per frame pixel, across and down.
  float2 k = FitRect.zw * OriginalSize.zw;

  // Behind curved glass the picture bows out: further from the middle, it
  // is drawn smaller. Corners round, the edge soft, the corners a little
  // darker; nothing outside the glass.
  float2 p = uv;
  float glass = 1;
  if (Params[0].z > 0.5) {
    float2 c = uv * 2 - 1;
    float bend = Params[2].z;
    c *= 1 + float2(bend * FitRect.w / FitRect.z, bend) * c.yx * c.yx;
    p = c * 0.5 + 0.5;
    float2 inside = (0.5 - abs(p - 0.5)) * FitRect.zw;   // screen pixels to each edge
    float r = Params[2].w * FitRect.w;
    float2 q = r - inside;
    float edge = r - length(max(q, 0)) - min(max(q.x, q.y), 0);   // to the rounded edge
    glass = smoothstep(-0.5, max(1.5, 0.004 * FitRect.w), edge);
    glass *= 1 - Params[2].y * dot(c, c);
  }

  // The frame pixel nearest, and how far from its centre.
  float2 s = p * OriginalSize.xy - 0.5;
  float2 n = floor(s + 0.5);
  float2 f = s - n;

  // How far each side of it a screen pixel reads the frame, across and
  // down: one pixel while a screen pixel spans at most three (k >= 1/3:
  // every window from a third of the frame's size up); more in a smaller
  // window (16 at most), enough to cover all the frame under the screen
  // pixel, where three would leave whole rows and columns read by none, to
  // twinkle as things move across them. The three nearest are summed
  // unrolled, as they always were, and only the further ones loop: a loop
  // over them all cost a quarter more on an Intel UHD 770.
  int2 r = (int2)clamp(ceil(0.5 / k - 0.5), 1, 16);

  // Across: a soft spot, never narrower than half a screen pixel (a window
  // smaller than the frame would shimmer), its weights summing to 1.
  float sx = max(Params[1].x, 0.5 / k.x);
  float3 dx = float3(f.x + 1, f.x, f.x - 1);
  float3 wx = exp(-0.5 * dx * dx / (sx * sx));
  float spot = wx.x + wx.y + wx.z;
  [loop] for (int i = 2; i <= r.x; ++i) spot += gauss(f.x + i, sx) + gauss(f.x - i, sx);
  wx /= spot;

  // Down: with scanlines, each line a beam of its own, taller where it is
  // bright than where dark (each gun's current sets its beam's height: a
  // saturated blue is a bright blue beam), its light spread over its height,
  // so the gaps show. Scanlines only where a line has room (looks.h,
  // crt_scanline_strength); without, the lines blend into each other as the
  // spot blends pixels across. The three lines nearest, then in a small
  // window more each side, out to r.
  float lines = saturate((k.y - 2.5) / 0.5);
  float sb = max(Params[1].x, 0.5 / k.y);
  Lines sum = (Lines)0;
  [unroll] for (int j = -1; j <= 1; ++j) {
    add_line(sum, across((int)n.x, (int)n.y + j, f.x, wx, r.x, sx, spot), f.y - j, sb, 0.5 / k.y);
  }
  [loop] for (int d = 2; d <= r.y; ++d) {
    add_line(sum, across((int)n.x, (int)n.y - d, f.x, wx, r.x, sx, spot), f.y + d, sb, 0.5 / k.y);
    add_line(sum, across((int)n.x, (int)n.y + d, f.x, wx, r.x, sx, spot), f.y - d, sb, 0.5 / k.y);
  }
  float3 beam = lerp(sum.blend / sum.total, sum.lit * 0.3989423, lines);

  // The mask, at the screen's own pixels: an aperture grille, stripes of
  // red, green and blue (magenta and green where the screen has fewer
  // pixels to a frame pixel), dimming the phosphors not lit and making up
  // the light they lose. It fades out where a frame pixel is under two
  // screen pixels, so a small window isn't covered in a pattern.
  float m = Params[2].x * saturate((min(k.x, k.y) - 1.25) / 0.75);
  uint x = (uint)pos.x;
  float3 mask;
  if (k.x >= 3.5) {
    uint i = x % 3;
    mask = float3(i == 0, i == 1, i == 2);
    mask = lerp(1 - m, 1, mask) / (1 - m * 2.0 / 3.0);
  } else {
    float g = (float)(x & 1);
    mask = float3(1 - g, g, 1 - g);
    mask = lerp(1 - m, 1, mask) / (1 - m * 0.5);
  }

  float3 glow = Pass2.SampleLevel(LinearClamp, p, 0).rgb;
  float3 light = lerp(beam * mask, glow, Params[0].y) * glass;
  return float4(pow(saturate(light), 1 / Params[0].x), 1);
}
)hlsl";

PassSpec pass(const char* name, const char* hlsl, PassSize size, float scale) {
  PassSpec p;
  p.name = name;
  p.hlsl = hlsl;
  p.size = size;
  p.scale = scale;
  p.format = PassFormat::rgba16f;
  return p;
}

} // namespace

std::vector<PassSpec> crt_passes(bool curved) {
  std::vector<PassSpec> passes;
  passes.push_back(pass("crt-linear", kLinear, PassSize::original, 1.0f));
  passes.back().params[0] = kGamma;
  passes.push_back(pass("crt-glow-h", kGlowH, PassSize::original, 0.5f));
  passes.back().params[0] = kGlowWidth;
  passes.push_back(pass("crt-glow-v", kGlowV, PassSize::source, 1.0f));
  passes.back().params[0] = kGlowWidth;
  PassSpec last = pass(curved ? "crt-curved" : "crt", kFinal, PassSize::fit, 1.0f);
  last.format = PassFormat::rgba8;
  last.params = {kGamma, kGlow, curved ? 1.0f : 0.0f, 0,
                 kSpot, kLineDark, kLineBright, 0,
                 kMask, kVignette, kBend, kCorner,
                 0, 0, 0, 0};
  passes.push_back(std::move(last));
  return passes;
}

} // namespace adw::scr
