// Stub: track CORE replaces this file with the Direct3D 11 presenter
// (present_d3d.h). Until then nothing here can draw, so every caller falls
// back to the old way.
#include "present_d3d.h"

#include "hlsl_crt.h"
#include "hlsl_smooth.h"

namespace adw::scr {

struct D3DPresenter::Impl {
  LookOptions opts;
};

D3DPresenter::D3DPresenter(LookOptions opts) : impl_(std::make_unique<Impl>()) { impl_->opts = std::move(opts); }
D3DPresenter::~D3DPresenter() = default;

bool D3DPresenter::present(HWND, const Frame&, const RectI&, std::string* error, bool* device_lost) {
  if (device_lost) *device_lost = false;
  if (error) *error = "direct3d not built yet";
  return false;
}

bool D3DPresenter::clear(HWND) { return false; }
bool D3DPresenter::ready() const { return false; }
void D3DPresenter::release() {}
std::string D3DPresenter::describe() const { return look_name(impl_->opts.look); }
double D3DPresenter::gpu_ms() const { return -1; }
const LookOptions& D3DPresenter::options() const { return impl_->opts; }

bool d3d_available(std::string* why) {
  if (why) *why = "direct3d not built yet";
  return false;
}

bool d3d_prepare(const LookOptions&, std::string* why) { return d3d_available(why); }

bool render_frame_bgr_d3d(const Frame&, int, int, const RectI&, const LookOptions&, std::vector<uint8_t>& bgr,
                          std::string* error) {
  bgr.clear();
  return d3d_available(error);
}

std::vector<PassSpec> look_passes(Look look) {
  switch (look) {
    case Look::crt: return crt_passes(false);
    case Look::crt_curved: return crt_passes(true);
    case Look::smooth: return smooth_passes();
    default: return {};
  }
}

} // namespace adw::scr
