// Direct3D 11 presentation, for the looks (looks.h). A window draws through
// it only while a look is on (a Look other than sharp, or AmbientBars); with
// the defaults the saver never makes a device and draws with Direct2D or GDI
// as before (present.h). AD_SCR_PRESENT=d3d11 draws the sharp look through it
// too, to compare the two ways.
//
// The frame goes up as it comes: an 8-bit frame as an R8_UINT texture of its
// indices and a 256x1 texture of its palette (a quarter of the bytes of the
// 32-bit bitmap Direct2D takes, and no conversion on the CPU), a 32-bit one
// as it is. A first pass resolves it to colour at the frame's own size (the
// passes' Original); the look's passes take it from there into the window's
// swap chain, the last one into the fit rectangle. With AmbientBars, the
// window's bars show a blurred, dimmed copy of the frame first.
//
// One device serves every window of the process (they all run on the UI
// thread); a device lost (a driver update or reset, a GPU removed) is made
// again on the next present, for all of them. HLSL is compiled at run time
// by d3dcompiler_47.dll (System32 on every Windows 10 and 11), loaded when
// first needed; without it there is no Direct3D presentation and the window
// draws the way it did before (the caller falls back).
#pragma once

#include <windows.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "geometry.h"
#include "host_process.h"
#include "looks.h"

namespace adw::scr {

class D3DPresenter {
 public:
  explicit D3DPresenter(LookOptions opts);
  ~D3DPresenter();
  D3DPresenter(const D3DPresenter&) = delete;
  D3DPresenter& operator=(const D3DPresenter&) = delete;

  // Draws `f` into `fit` of `hwnd`'s client area with the look, on black (or
  // the ambient fill). false when it couldn't (the caller draws this frame
  // the old way): `*error` says why, and `*device_lost` whether the device
  // went away (the next call starts afresh; anything else is not worth
  // retrying).
  bool present(HWND hwnd, const Frame& f, const RectI& fit, std::string* error = nullptr,
               bool* device_lost = nullptr);
  // Black over the whole window, while there is no frame (between modules).
  // false when there is no swap chain yet, or it failed (then released).
  bool clear(HWND hwnd);
  // A swap chain exists: the next present costs no set-up.
  bool ready() const;
  // Lets go of the window's swap chain, so GDI owns the window again (a
  // message drawn over black).
  void release();
  // What it draws, for the log: the look, the adapter (its name, "WARP" or
  // "Microsoft Basic Render Driver"), the feature level and the swap model,
  // e.g. "crt, ambient bars (NVIDIA GeForce RTX 4070, feature level 11_1,
  // blt model)"; while there is no device yet, the look alone.
  std::string describe() const;
  // The GPU time of the latest frame whose timestamps have come back, in
  // milliseconds; -1 while none has (or the device can't time). Read back
  // without waiting, so it trails the presents by a frame or three.
  double gpu_ms() const;
  // The latest present drew the sharp look in a shader preset's place: the
  // preset's chain is still being built (shader_preset.h).
  bool standing_in() const;
  // How many times what present() draws has changed under the same look:
  // the preset's chain ready at last, after the sharp look stood in. The
  // saver judges a look's cost on the frames after the latest change;
  // gpu_ms() goes back to -1 at a change and comes back with those frames
  // (not the first, which compiles the new shaders).
  uint32_t drawn_changes() const;
  const LookOptions& options() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Direct3D 11 can draw here at all: a device of feature level 10.0 or up and
// a shader compiler. For the log and for the tests' skips.
bool d3d_available(std::string* why = nullptr);

// Makes the device and compiles the look's shaders ahead of the first frame,
// so the first present doesn't stall on them (the saver calls it as its
// windows open). false: `*why` says what failed (the windows will fall back).
bool d3d_prepare(const LookOptions& opts, std::string* why = nullptr);

// For the tests (scr_lookshot --lose, LongAfterDark-test.scr's levers): the
// next present() of any window finds the device gone, as Present does after
// a driver reset (DXGI_ERROR_DEVICE_REMOVED): it reports device_lost, the
// device is dropped, and the next present makes a new one.
void d3d_simulate_device_loss();

// For measuring (scr_lookshot --adapter): the windows' device is made on the
// adapter whose name contains `match` (any case), or whose index it is,
// instead of the default one. Only before the device exists; false when
// none matches (`*why` lists the adapters).
bool d3d_choose_adapter(const std::string& match, std::string* why = nullptr);

// What a window shows, rendered off screen: `f` drawn with `opts` into `fit`
// of a w x h picture, as D3DPresenter draws it (bars black, or the ambient
// fill). On the process's device when the windows have made one, else on a
// WARP device of its own. BGR rows, top-down, w*3 bytes each (adw_ui's
// save_png_bgr). The test hook AD_SCR_TEST_CAPTURE writes these, the unit
// tests check the looks with them, and the scr_lookshot tool renders them.
bool render_frame_bgr_d3d(const Frame& f, int w, int h, const RectI& fit, const LookOptions& opts,
                          std::vector<uint8_t>& bgr, std::string* error = nullptr);
// The GPU time of the latest render_frame_bgr_d3d's drawing (its passes,
// not the read back), in milliseconds; -1 when the device can't time. For
// scr_lookshot's costs.
double render_frame_gpu_ms();
// render_frame_bgr_d3d with the caller's passes instead of a look's (the
// last one into the fit, as look_passes' last): for the tests of the pass
// runner, and for trying a pass list out.
struct PassSpec;
bool render_passes_bgr_d3d(const Frame& f, int w, int h, const RectI& fit, const std::vector<PassSpec>& passes,
                           bool ambient, std::vector<uint8_t>& bgr, std::string* error = nullptr);

// ---- the passes a look is made of ---------------------------------------------------
// Every look but preset is a list of passes (look_passes): pixel shaders, each
// drawn over a full-screen triangle into a texture of its own, the last one
// into the window's back buffer, its viewport the fit rectangle. Each pass's
// HLSL is compiled for shader model 4.0 (feature level 10.0 and up) after
// kPassPrelude, with this entry point:
//
//   float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
//
// `uv` runs 0..1 across the pass's output: for the last pass, across the fit
// rectangle, not the whole window. Original, the resolved frame, holds the
// palette's colours as they are (sRGB-encoded bytes in an RGBA8 UNORM
// texture, alpha 1): a pass that works in linear light decodes and encodes
// gamma itself. The last pass writes every pixel of the fit rectangle (no
// discard): nothing is drawn there before it.

enum class PassSize {
  source,     // `scale` times the previous pass's output (Source)
  original,   // `scale` times the frame (Original)
  fit,        // the fit rectangle: the last pass, and only it
};

enum class PassFormat { rgba8, rgba16f };

struct PassSpec {
  std::string name;               // for the log and errors, e.g. "crt-glow-h"
  std::string hlsl;               // the pass's own source; kPassPrelude goes in front
  PassSize size = PassSize::source;
  float scale = 1.0f;             // PassSize::source and ::original
  PassFormat format = PassFormat::rgba8;
  std::array<float, 16> params{}; // the cbuffer's Params[4]
};

// What every pass sees. Pass0..Pass5 are the outputs of the passes before
// this one, by their index in the list (later ones are unbound).
inline constexpr char kPassPrelude[] = R"hlsl(
Texture2D Source : register(t0);
Texture2D Original : register(t1);
Texture2D Pass0 : register(t2);
Texture2D Pass1 : register(t3);
Texture2D Pass2 : register(t4);
Texture2D Pass3 : register(t5);
Texture2D Pass4 : register(t6);
Texture2D Pass5 : register(t7);
SamplerState PointClamp : register(s0);
SamplerState LinearClamp : register(s1);
cbuffer PassConstants : register(b0) {
  float4 SourceSize;     // Source: w, h, 1/w, 1/h
  float4 OriginalSize;   // Original (the frame): w, h, 1/w, 1/h
  float4 OutputSize;     // this pass's output (the last pass: the fit rectangle): w, h, 1/w, 1/h
  float4 FitRect;        // the fit rectangle in the window, pixels: x, y, w, h
  float4 WindowSize;     // the window's client area: w, h, 1/w, 1/h
  float4 Params[4];      // PassSpec::params
  uint FrameCount;       // frames this window has presented
  uint3 PassPad;
};
)hlsl";

// The C++ side of the PassConstants cbuffer (160 bytes, as HLSL packs it).
// (Not "Pass": that is a reserved word to the HLSL compiler.)
struct PassConstants {
  float source_size[4];
  float original_size[4];
  float output_size[4];
  float fit_rect[4];
  float window_size[4];
  float params[16];
  uint32_t frame_count;
  uint32_t pad[3];
};
static_assert(sizeof(PassConstants) == 160, "the Pass cbuffer is 160 bytes");

// The passes of `look` (not Look::preset, which librashader draws:
// shader_preset.h). The last one is PassSize::fit.
std::vector<PassSpec> look_passes(Look look);

} // namespace adw::scr
