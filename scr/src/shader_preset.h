// Look::preset (looks.h): a RetroArch shader preset (.slangp), the one the
// settings' ShaderPreset names, run by librashader. Long After Dark ships
// neither librashader nor any preset: the user puts librashader.dll where
// the saver looks for it (by full path only, never the DLL search order) and
// points ShaderPreset at a preset of their own, such as one from RetroArch's
// slang-shaders. Without the DLL, or with a version this build can't speak,
// the look isn't available and the window draws the way it did before.
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

// One window's filter chain.
class ShaderPreset {
 public:
  ShaderPreset();
  ~ShaderPreset();
  ShaderPreset(const ShaderPreset&) = delete;
  ShaderPreset& operator=(const ShaderPreset&) = delete;

  // Reads the preset at `path` and builds its filter chain on `device`.
  // false: `*error` says why (no library, an unreadable preset, a shader
  // that doesn't compile).
  bool create(ID3D11Device* device, ID3D11DeviceContext* context, const std::wstring& path, std::string* error);
  bool ready() const;
  // Draws `source` (the frame resolved to colour, src_w x src_h) through
  // the chain into `target` (target_w x target_h), within `viewport` (the
  // fit rectangle, in the target's pixels); the rest of the target is left
  // as it is. `frame_count`: frames the window has presented (the presets'
  // FrameCount).
  bool draw(ID3D11DeviceContext* context, ID3D11ShaderResourceView* source, int src_w, int src_h,
            ID3D11RenderTargetView* target, int target_w, int target_h, const RectI& viewport, uint64_t frame_count,
            std::string* error);
  // Drops the chain (the device was lost, or the window let go).
  void reset();
  // For the log, e.g. "crt-royale.slangp, librashader 0.6.2".
  std::string describe() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace adw::scr
