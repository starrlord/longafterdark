// Stub: track PRESET replaces this file with the librashader loader and
// filter chain (shader_preset.h).
#include "shader_preset.h"

namespace adw::scr {

bool shader_preset_library(std::string* why) {
  if (why) *why = "shader presets not built yet";
  return false;
}

struct ShaderPreset::Impl {};

ShaderPreset::ShaderPreset() : impl_(std::make_unique<Impl>()) {}
ShaderPreset::~ShaderPreset() = default;

bool ShaderPreset::create(ID3D11Device*, ID3D11DeviceContext*, const std::wstring&, std::string* error) {
  return shader_preset_library(error);
}

bool ShaderPreset::ready() const { return false; }

bool ShaderPreset::draw(ID3D11DeviceContext*, ID3D11ShaderResourceView*, int, int, ID3D11RenderTargetView*, int, int,
                        const RectI&, uint64_t, std::string* error) {
  return shader_preset_library(error);
}

void ShaderPreset::reset() {}

std::string ShaderPreset::describe() const { return "shader preset"; }

} // namespace adw::scr
