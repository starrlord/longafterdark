// scr_unit looks: the looks' rules, and (where Direct3D 11 can draw) what
// each look draws. The parts live in looks_*_test.cc (looks_test.h).
#include "looks_test.h"

#include <algorithm>

#include "looks.h"
#include "present_d3d.h"
#include "settings.h"

namespace adw::scr::looks_test {

int failures = 0;

Frame make_frame8(int w, int h, const std::function<uint8_t(int, int)>& index, const std::vector<RGBQUAD>& palette) {
  Frame f;
  f.width = w;
  f.height = h;
  f.bpp = 8;
  f.stride = (w + 3) & ~3;
  f.bits.assign((size_t)f.stride * h, 0);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) f.bits[(size_t)y * f.stride + x] = index(x, y);
  }
  for (size_t i = 0; i < palette.size() && i < 256; ++i) f.palette[i] = palette[i];
  return f;
}

Frame to_frame32(const Frame& f8) {
  Frame f;
  f.width = f8.width;
  f.height = f8.height;
  f.bpp = 32;
  f.stride = f8.width * 4;
  f.bits.assign((size_t)f.stride * f.height, 0);
  for (int y = 0; y < f8.height; ++y) {
    for (int x = 0; x < f8.width; ++x) {
      const RGBQUAD& q = f8.palette[f8.bits[(size_t)y * f8.stride + x]];
      uint8_t* d = &f.bits[(size_t)y * f.stride + (size_t)x * 4];
      d[0] = q.rgbBlue;
      d[1] = q.rgbGreen;
      d[2] = q.rgbRed;
      d[3] = 0;
    }
  }
  return f;
}

Frame grey_frame(int w, int h, uint8_t level) {
  return make_frame8(w, h, [](int, int) { return (uint8_t)1; }, {RGBQUAD{0, 0, 0, 0}, RGBQUAD{level, level, level, 0}});
}

int luma_at(const std::vector<uint8_t>& bgr, int w, int x, int y) {
  const uint8_t* p = &bgr[((size_t)y * w + x) * 3];
  return (p[0] * 114 + p[1] * 587 + p[2] * 299 + 500) / 1000;
}

double mean_luma(const std::vector<uint8_t>& bgr, int w, int x0, int y0, int x1, int y1) {
  double sum = 0;
  long long n = 0;
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x, ++n) sum += luma_at(bgr, w, x, y);
  }
  return n ? sum / n : 0;
}

// The rules that need no device: the names, the scanlines' strength, and
// what the settings file's keys come to.
void logic_checks() {
  for (Look l : {Look::sharp, Look::crt, Look::crt_curved, Look::smooth, Look::preset}) {
    Look back = Look::sharp;
    LCHECK(parse_look(look_name(l), &back) && back == l);
  }
  Look l = Look::smooth;
  LCHECK(parse_look("  CRT-Curved\t", &l) && l == Look::crt_curved);
  l = Look::smooth;
  LCHECK(!parse_look("vhs", &l) && l == Look::smooth);
  LCHECK(!parse_look("", &l));
  LCHECK(crt_scanline_strength(1.5) == 0.0 && crt_scanline_strength(2.25) == 0.0 && crt_scanline_strength(2.5) == 0.0);
  LCHECK(crt_scanline_strength(2.75) > 0.49 && crt_scanline_strength(2.75) < 0.51);
  LCHECK(crt_scanline_strength(3.0) == 1.0 && crt_scanline_strength(4.5) == 1.0);
  LCHECK(!LookOptions{}.any());
  LCHECK((LookOptions{Look::crt, false, {}}).any() && (LookOptions{Look::sharp, true, {}}).any());

  // The defaults: no keys, nothing on.
  const Settings none = parse_settings("[Saver]\r\nModule=random\r\n");
  LCHECK(none.look == "sharp" && !none.ambient_bars && none.shader_preset.empty());
  LCHECK(!look_options(none).any());
  // Each key read; a quoted path unquoted.
  Settings s = parse_settings("[Saver]\r\nLook=CRT-curved\r\nAmbientBars=yes\r\nShaderPreset=\"C:\\Sh aders\\x.slangp\"\r\n");
  LookOptions o = look_options(s);
  LCHECK(o.look == Look::crt_curved && o.ambient && o.preset == L"C:\\Sh aders\\x.slangp");
  // A look this version doesn't know draws as sharp but stays in the file;
  // preset without a ShaderPreset draws as sharp.
  s = parse_settings("[Saver]\r\nLook=vhs-tape\r\n");
  LCHECK(s.look == "vhs-tape" && look_options(s).look == Look::sharp);
  std::string text = serialize_settings(s, "[Saver]\r\nLook=vhs-tape\r\n");
  LCHECK(text.find("Look=vhs-tape") != std::string::npos);
  LCHECK(look_options(parse_settings("[Saver]\r\nLook=preset\r\n")).look == Look::sharp);
  // A Look that already says the look is left as written; a change is written.
  s = parse_settings("[Saver]\r\nLook=CRT\r\n");
  text = serialize_settings(s, "[Saver]\r\nLook=CRT\r\n");
  LCHECK(text.find("Look=CRT\r\n") != std::string::npos);
  s.look = "smooth";
  s.ambient_bars = true;
  text = serialize_settings(s, "[Saver]\r\nLook=CRT\r\n");
  LCHECK(text.find("Look=smooth\r\n") != std::string::npos && text.find("AmbientBars=1\r\n") != std::string::npos);
  // ShaderPreset has no UI: written only when it says something new.
  LCHECK(serialize_settings(parse_settings("[Saver]\r\n"), "[Saver]\r\n").find("ShaderPreset") == std::string::npos);
  text = serialize_settings(parse_settings("[Saver]\r\nShaderPreset=\"a b.slangp\"\r\n"),
                            "[Saver]\r\nShaderPreset=\"a b.slangp\"\r\n");
  LCHECK(text.find("ShaderPreset=\"a b.slangp\"") != std::string::npos);
  LCHECK(parse_settings(text) == parse_settings("[Saver]\r\nShaderPreset=a b.slangp\r\n") ||
         parse_settings(text).shader_preset == "a b.slangp");
}

} // namespace adw::scr::looks_test

int run_looks_tests() {
  using namespace adw::scr;
  using namespace adw::scr::looks_test;
  failures = 0;
  logic_checks();
  std::string why;
  if (!d3d_available(&why)) {
    printf("looks: no Direct3D 11 here (%s): what the looks draw is not checked\n", why.c_str());
    return failures;
  }
  core_checks();
  crt_checks();
  smooth_checks();
  preset_checks();
  return failures;
}
