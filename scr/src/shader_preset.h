// Look::preset (looks.h): a RetroArch shader preset (.slangp), the one the
// settings' ShaderPreset names, run by librashader. Long After Dark ships
// neither librashader nor any preset: the user puts librashader.dll where
// the saver looks for it (by full path only, never the DLL search order) and
// points ShaderPreset at a preset of their own, such as one from RetroArch's
// slang-shaders. Without the DLL, or with a version this build can't speak,
// the look isn't available and the window draws the way it did before.
//
// Where the DLL is looked for, in order: next to the running program
// (LongAfterDark.scr), then <data folder>\librashader\librashader.dll
// (paths.h app_data_root). This build speaks librashader's C ABI 2
// (librashader 0.5.0 and later, to at least 0.12.0). Its Windows build needs
// the Visual C++ 2015-2022 x64 runtime and DirectX's D3DX9_43.dll beside it
// or in System32, and its Direct3D 11 runtime needs feature level 11.0.
//
// Building a filter chain compiles every pass's shaders: a big preset
// (crt-royale) takes seconds the first time (librashader keeps a cache of
// them in its own folder, %LOCALAPPDATA%\librashader). So create() reads the
// preset at once and builds the chain off the UI thread, on a deferred
// context of the device; until ready() the caller draws the sharp look.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "geometry.h"

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;
struct ID3D11RenderTargetView;

namespace adw::scr {

// librashader.dll, loaded once per process. false: not there, or not a
// version this build speaks; `*why` says which (and where it looked).
bool shader_preset_library(std::string* why = nullptr);

// One window's filter chain. All of it runs on the thread that owns the
// device's immediate context (the UI thread), but the chain's build.
class ShaderPreset {
 public:
  ShaderPreset();
  ~ShaderPreset();
  ShaderPreset(const ShaderPreset&) = delete;
  ShaderPreset& operator=(const ShaderPreset&) = delete;

  // Reads the preset at `path` and starts building its filter chain on
  // `device` (a relative path is taken from the data folder, where
  // settings.ini is: paths.h app_data_root). false: `*error` says why (no
  // library, no such file, a preset that doesn't parse, a device below
  // feature level 11.0). true: the build is under way; ready() or failed()
  // says how it ends. The build never touches
  // `context` (the immediate one): it records on a deferred context, which
  // draw() plays on `context` before the first frame. On a device made
  // D3D11_CREATE_DEVICE_SINGLETHREADED (no deferred contexts) it builds here
  // and now instead.
  bool create(ID3D11Device* device, ID3D11DeviceContext* context, const std::wstring& path, std::string* error);
  // librashader keeps the shaders it compiles in a cache of its own
  // (%LOCALAPPDATA%\librashader), which makes the next build quick. On, but
  // for a create() after set_shader_cache(false): the tests', which leave
  // the user's profile alone.
  void set_shader_cache(bool on);
  // The chain is built: draw() draws with it.
  bool ready() const;
  // The build failed (a shader that doesn't compile, a texture that doesn't
  // load): `*error` says why, librashader's words. It never gets ready.
  bool failed(std::string* error = nullptr) const;
  // Waits for the build to end, at most `timeout_ms`: true when ready().
  // For pictures drawn off screen (render_frame_bgr_d3d, the tests).
  bool wait(unsigned timeout_ms = 60000);
  // Draws `source` (the frame resolved to colour, src_w x src_h) through
  // the chain into `target` (target_w x target_h), within `viewport` (the
  // fit rectangle, in the target's pixels); the rest of the target is left
  // as it is. `frame_count`: frames the window has presented (the presets'
  // FrameCount). false before ready(), or when librashader fails (`*error`).
  // The chain draws into a texture of its own, the viewport's size, which
  // is then copied into the target: librashader clears its whole output,
  // and sizes the passes from it. `target` must be a 2D render target of
  // one sample. Afterwards the context's state is librashader's (it puts
  // back only the blend and rasterizer states): the caller sets the rest of
  // its own again.
  bool draw(ID3D11DeviceContext* context, ID3D11ShaderResourceView* source, int src_w, int src_h,
            ID3D11RenderTargetView* target, int target_w, int target_h, const RectI& viewport, uint64_t frame_count,
            std::string* error);
  // Drops the chain (the device was lost, or the window let go). Never
  // waits: a build still under way is thrown away when it ends.
  void reset();
  // For the log, e.g. "crt-royale.slangp, librashader ABI 2 (API 5)".
  std::string describe() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace adw::scr
