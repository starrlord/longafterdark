// The Direct3D 11 presenter (present_d3d.h): the looks' way of drawing a
// window. Nothing here is reached with the default settings: the saver calls
// in only for a look (LookOptions::any()) or AD_SCR_PRESENT=d3d11. Then
// d3d11.dll and d3dcompiler_47.dll are loaded on first use, from System32 by
// full path, and dxgi.dll is reached only through the device (never named).
// (Direct2D, the default way, loads d3d11.dll and dxgi.dll for itself as it
// draws on the GPU; the HLSL compiler only this file loads.)
//
// The swap chain is the blt model (DXGI_SWAP_EFFECT_DISCARD), as Direct2D's
// window render target is: a window that has presented with the flip model
// keeps showing its last flipped frame after the swap chain is gone, over
// whatever GDI draws there afterwards (the saver's messages), where the blt
// model hands the window back to GDI (research/looks/core/FACTS.md).
#include "present_d3d.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>

#include "hlsl_core.h"
#include "hlsl_crt.h"
#include "hlsl_smooth.h"
#include "paths.h"
#include "shader_preset.h"

namespace adw::scr {

namespace {

template <typename T>
class ComPtr {
 public:
  ComPtr() = default;
  ComPtr(const ComPtr& o) : p_(o.p_) {
    if (p_) p_->AddRef();
  }
  ComPtr(ComPtr&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
  ComPtr& operator=(ComPtr o) noexcept {
    std::swap(p_, o.p_);
    return *this;
  }
  ~ComPtr() { reset(); }
  void reset() {
    if (p_) p_->Release();
    p_ = nullptr;
  }
  T** out() {
    reset();
    return &p_;
  }
  T* get() const { return p_; }
  T* operator->() const { return p_; }
  explicit operator bool() const { return p_ != nullptr; }
  template <typename U>
  HRESULT as(ComPtr<U>& u) const {
    return p_ ? p_->QueryInterface(__uuidof(U), reinterpret_cast<void**>(u.out())) : E_POINTER;
  }

 private:
  T* p_ = nullptr;
};

bool fail(std::string* error, const std::string& what, HRESULT hr = S_OK) {
  if (error) {
    if (hr != S_OK) {
      char buf[32];
      snprintf(buf, sizeof(buf), " (hr=0x%08lX)", (unsigned long)hr);
      *error = what + buf;
    } else {
      *error = what;
    }
  }
  return false;
}

bool is_device_lost(HRESULT hr) {
  return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG ||
         hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

std::string narrow_utf8(const wchar_t* w) {
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return {};
  std::string s((size_t)n - 1, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}

// ---- the libraries -------------------------------------------------------------------

// Everything below runs under this lock: one immediate context serves every
// window and the off-screen renders, and it is not thread-safe. (Made on
// first use, as everything here: nothing runs at the program's start.)
std::recursive_mutex& d3d_lock() {
  static auto* lock = new std::recursive_mutex;
  return *lock;
}

struct Libraries {
  bool tried = false;
  PFN_D3D11_CREATE_DEVICE create_device = nullptr;
  pD3DCompile compile = nullptr;
  std::string why;   // what is missing
};

// d3d11.dll and the HLSL compiler, loaded once, by full path from System32
// (never the DLL search order).
const Libraries& libraries() {
  static Libraries libs;
  if (libs.tried) return libs;
  libs.tried = true;
  HMODULE d3d11 = LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (d3d11) {
    libs.create_device = reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(
        reinterpret_cast<void*>(GetProcAddress(d3d11, "D3D11CreateDevice")));
  }
  if (!libs.create_device) {
    libs.why = "no d3d11.dll";
    return libs;
  }
  HMODULE compiler = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (compiler) {
    libs.compile = reinterpret_cast<pD3DCompile>(reinterpret_cast<void*>(GetProcAddress(compiler, "D3DCompile")));
  }
  if (!libs.compile) libs.why = "no d3dcompiler_47.dll";
  return libs;
}

bool libraries_ok(std::string* why) {
  const Libraries& l = libraries();
  if (l.create_device && l.compile) return true;
  return fail(why, l.why);
}

// ---- HLSL ----------------------------------------------------------------------------

// Compiled bytecode by target and full source, for the process (every device
// makes its shader objects from it).
std::map<std::string, ComPtr<ID3DBlob>>& bytecode_cache() {
  static auto* cache = new std::map<std::string, ComPtr<ID3DBlob>>;
  return *cache;
}

// The compiler's first lines, for the log.
std::string first_lines(const std::string& text, int lines) {
  std::string out;
  size_t p = 0;
  for (int i = 0; i < lines && p < text.size(); ++i) {
    size_t nl = text.find('\n', p);
    std::string line = text.substr(p, nl == std::string::npos ? std::string::npos : nl - p);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    // The compiler names the source by its pass name; the path before it is noise.
    if (!line.empty()) out += (out.empty() ? "" : " | ") + line;
    if (nl == std::string::npos) break;
    p = nl + 1;
  }
  if (out.size() > 400) out = out.substr(0, 397) + "...";
  return out;
}

bool compile_hlsl(const std::string& source, const char* target, const std::string& name, ComPtr<ID3DBlob>& out,
                  std::string* error) {
  const std::string key = std::string(target) + "\n" + source;
  auto& cache = bytecode_cache();
  if (auto it = cache.find(key); it != cache.end()) {
    out = it->second;
    return true;
  }
  if (!libraries_ok(error)) return false;
  ComPtr<ID3DBlob> code, errors;
  const HRESULT hr = libraries().compile(source.data(), source.size(), name.c_str(), nullptr, nullptr, "main", target,
                                         D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.out(), errors.out());
  if (FAILED(hr) || !code) {
    std::string text;
    if (errors) text.assign(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
    return fail(error, "pass " + name + " does not compile: " + first_lines(text, 3), hr);
  }
  cache.emplace(key, code);
  out = code;
  return true;
}

// ---- a device ------------------------------------------------------------------------

// One Direct3D 11 device and what every pass on it shares.
struct Gpu {
  ComPtr<ID3D11Device> dev;
  ComPtr<ID3D11DeviceContext> ctx;
  D3D_FEATURE_LEVEL level{};
  std::string adapter;    // "NVIDIA GeForce RTX 4070", "WARP", "Microsoft Basic Render Driver"
  bool warp = false;
  uint64_t generation = 0;
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11SamplerState> point, linear;
  ComPtr<ID3D11Buffer> constants;
  ComPtr<ID3D11RasterizerState> raster;
  std::map<std::string, ComPtr<ID3D11PixelShader>> shaders;   // by full source
  bool can_time = true;   // timestamp queries
  int max_texture = 8192;

  explicit operator bool() const { return (bool)dev; }
};

// Made on first use and never destroyed: nothing of Direct3D is released
// while the process exits.
uint64_t g_generations = 0;
// The windows' device (hardware): the process's.
Gpu& hw_gpu() {
  static auto* gpu = new Gpu;
  return *gpu;
}
// The off-screen renders' while there is none.
Gpu& warp_gpu() {
  static auto* gpu = new Gpu;
  return *gpu;
}
std::string g_hw_failed;  // why the hardware device could not be made (not tried again)
// d3d_choose_adapter's (lookshot --adapter); none: the default adapter.
ComPtr<IDXGIAdapter>& chosen_adapter() {
  static auto* adapter = new ComPtr<IDXGIAdapter>;
  return *adapter;
}
bool g_simulate_loss = false;   // d3d_simulate_device_loss: the next present() finds the device gone

const char* level_name(D3D_FEATURE_LEVEL l) {
  switch (l) {
    case D3D_FEATURE_LEVEL_11_1: return "11_1";
    case D3D_FEATURE_LEVEL_11_0: return "11_0";
    case D3D_FEATURE_LEVEL_10_1: return "10_1";
    case D3D_FEATURE_LEVEL_10_0: return "10_0";
    default: return "?";
  }
}

bool make_device(Gpu& g, bool warp, std::string* error) {
  if (!libraries_ok(error)) return false;
  static const D3D_FEATURE_LEVEL kLevels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                              D3D_FEATURE_LEVEL_10_0};
  Gpu n;
  // The default adapter, or the one d3d_choose_adapter picked (then the driver type is "unknown").
  IDXGIAdapter* adapter = warp ? nullptr : chosen_adapter().get();
  const D3D_DRIVER_TYPE type = warp ? D3D_DRIVER_TYPE_WARP : adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;
  HRESULT hr = libraries().create_device(adapter, type, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, kLevels,
                                         (UINT)std::size(kLevels), D3D11_SDK_VERSION, n.dev.out(), &n.level,
                                         n.ctx.out());
  if (FAILED(hr) || !n.dev || !n.ctx) {
    return fail(error, warp ? "no WARP device of feature level 10.0 or up"
                            : "no Direct3D 11 device of feature level 10.0 or up", hr);
  }
  n.warp = warp;
  n.max_texture = n.level >= D3D_FEATURE_LEVEL_11_0 ? 16384 : 8192;
  {
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    if (SUCCEEDED(n.dev.as(dxgi)) && SUCCEEDED(dxgi->GetAdapter(adapter.out())) && SUCCEEDED(adapter->GetDesc(&desc)))
      n.adapter = narrow_utf8(desc.Description);
    if (warp) n.adapter = "WARP";
    if (n.adapter.empty()) n.adapter = "unknown adapter";
  }
  ComPtr<ID3DBlob> vs;
  if (!compile_hlsl(hlsl::kFullScreenVs, "vs_4_0", "fullscreen-vs", vs, error)) return false;
  hr = n.dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, n.vs.out());
  if (FAILED(hr)) return fail(error, "CreateVertexShader", hr);
  D3D11_SAMPLER_DESC sd{};
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  hr = n.dev->CreateSamplerState(&sd, n.point.out());
  if (SUCCEEDED(hr)) {
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    hr = n.dev->CreateSamplerState(&sd, n.linear.out());
  }
  if (FAILED(hr)) return fail(error, "CreateSamplerState", hr);
  D3D11_BUFFER_DESC bd{};
  bd.ByteWidth = sizeof(PassConstants);
  bd.Usage = D3D11_USAGE_DYNAMIC;
  bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  hr = n.dev->CreateBuffer(&bd, nullptr, n.constants.out());
  if (FAILED(hr)) return fail(error, "CreateBuffer", hr);
  D3D11_RASTERIZER_DESC rd{};
  rd.FillMode = D3D11_FILL_SOLID;
  rd.CullMode = D3D11_CULL_NONE;
  rd.DepthClipEnable = TRUE;
  rd.ScissorEnable = TRUE;
  hr = n.dev->CreateRasterizerState(&rd, n.raster.out());
  if (FAILED(hr)) return fail(error, "CreateRasterizerState", hr);
  n.generation = ++g_generations;
  g = std::move(n);
  return true;
}

// The windows' device, made on first use (and again after a loss).
bool hw_device(std::string* error) {
  if (hw_gpu()) return true;
  if (!g_hw_failed.empty()) return fail(error, g_hw_failed);
  std::string why;
  if (make_device(hw_gpu(), false, &why)) return true;
  g_hw_failed = why;
  return fail(error, why);
}

// The device went away (a driver update or reset, a GPU removed): every
// window drops what it made on it (their generation no longer matches) and
// the next present makes a new one.
void lose_device(Gpu& g) {
  if (g.ctx) {
    g.ctx->ClearState();
    g.ctx->Flush();
  }
  g = Gpu{};
}

// A failed creation call: was it the device going away?
bool device_gone(Gpu& g, HRESULT hr) {
  return is_device_lost(hr) || (g.dev && g.dev->GetDeviceRemovedReason() != S_OK);
}

ID3D11PixelShader* pixel_shader(Gpu& g, const std::string& source, const std::string& name, std::string* error) {
  if (auto it = g.shaders.find(source); it != g.shaders.end()) return it->second.get();
  ComPtr<ID3DBlob> code;
  if (!compile_hlsl(source, "ps_4_0", name, code, error)) return nullptr;
  ComPtr<ID3D11PixelShader> ps;
  const HRESULT hr = g.dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, ps.out());
  if (FAILED(hr)) {
    fail(error, "CreatePixelShader " + name, hr);
    return nullptr;
  }
  return (g.shaders[source] = ps).get();
}

// A pass's source as compiled: the prelude, then the pass.
std::string with_prelude(const std::string& hlsl) { return std::string(kPassPrelude) + hlsl; }

// ---- textures ------------------------------------------------------------------------

struct Target {
  ComPtr<ID3D11Texture2D> tex;
  ComPtr<ID3D11RenderTargetView> rtv;
  ComPtr<ID3D11ShaderResourceView> srv;
  int w = 0, h = 0;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  void reset() { *this = Target{}; }
};

DXGI_FORMAT dxgi_format(PassFormat f) {
  return f == PassFormat::rgba16f ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
}

HRESULT ensure_target(Gpu& g, Target& t, int w, int h, DXGI_FORMAT format) {
  w = std::clamp(w, 1, g.max_texture);
  h = std::clamp(h, 1, g.max_texture);
  if (t.tex && t.w == w && t.h == h && t.format == format) return S_OK;
  t.reset();
  D3D11_TEXTURE2D_DESC td{};
  td.Width = (UINT)w;
  td.Height = (UINT)h;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = format;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  HRESULT hr = g.dev->CreateTexture2D(&td, nullptr, t.tex.out());
  if (SUCCEEDED(hr)) hr = g.dev->CreateRenderTargetView(t.tex.get(), nullptr, t.rtv.out());
  if (SUCCEEDED(hr)) hr = g.dev->CreateShaderResourceView(t.tex.get(), nullptr, t.srv.out());
  if (FAILED(hr)) {
    t.reset();
    return hr;
  }
  t.w = w;
  t.h = h;
  t.format = format;
  return S_OK;
}

// A texture the CPU fills (the frame), shader-readable.
struct Upload {
  ComPtr<ID3D11Texture2D> tex;
  ComPtr<ID3D11ShaderResourceView> srv;
  int w = 0, h = 0;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};

HRESULT ensure_upload(Gpu& g, Upload& u, int w, int h, DXGI_FORMAT format) {
  if (u.tex && u.w == w && u.h == h && u.format == format) return S_OK;
  u = Upload{};
  D3D11_TEXTURE2D_DESC td{};
  td.Width = (UINT)w;
  td.Height = (UINT)h;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = format;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  HRESULT hr = g.dev->CreateTexture2D(&td, nullptr, u.tex.out());
  if (SUCCEEDED(hr)) hr = g.dev->CreateShaderResourceView(u.tex.get(), nullptr, u.srv.out());
  if (FAILED(hr)) {
    u = Upload{};
    return hr;
  }
  u.w = w;
  u.h = h;
  u.format = format;
  return S_OK;
}

// ---- GPU time ------------------------------------------------------------------------

// A ring of timestamp queries, read back a few frames later without waiting.
struct Timing {
  struct Slot {
    ComPtr<ID3D11Query> disjoint, begin, end;
    bool pending = false;
    uint32_t epoch = 0;     // Timing::epoch when it was begun
    bool discard = false;   // the first frame of its epoch: not kept
  };
  std::array<Slot, 4> ring;
  int next = 0;           // the slot the next frame uses (the oldest)
  bool open = false;      // a frame is being timed
  double gpu_ms = -1;
  // What is drawn changed (restart): frames from before are not kept, nor
  // the first one after (it compiles its shaders on the GPU's side).
  uint32_t epoch = 0;
  bool first = false;

  void reset() { *this = Timing{}; }
  void restart() {
    ++epoch;
    gpu_ms = -1;
    first = true;
  }

  // Reads the slots whose results are in, oldest first, up to the first that
  // isn't (the latest known stays the newest); `wait`: block for them (a
  // little: the off-screen renders, whose read back has waited for the GPU
  // already). The disjoint query can be back before the timestamps (Intel's
  // driver, a long frame): the slot then waits for its next poll.
  void poll(Gpu& g, bool wait) {
    const UINT flags = wait ? 0 : D3D11_ASYNC_GETDATA_DONOTFLUSH;
    for (int i = 0; i < (int)ring.size(); ++i) {
      Slot& s = ring[(next + i) % ring.size()];
      if (!s.pending) continue;
      D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
      UINT64 t0 = 0, t1 = 0;
      HRESULT hr = S_FALSE;
      for (int tries = 0; tries < (wait ? 1000 : 1); ++tries) {
        if (tries) Sleep(tries < 10 ? 0 : 1);
        hr = g.ctx->GetData(s.disjoint.get(), &dj, sizeof(dj), flags);
        if (hr == S_OK) hr = g.ctx->GetData(s.begin.get(), &t0, sizeof(t0), flags);
        if (hr == S_OK) hr = g.ctx->GetData(s.end.get(), &t1, sizeof(t1), flags);
        if (hr != S_FALSE) break;
      }
      if (hr == S_FALSE) break;   // not back yet, nor any after it
      s.pending = false;
      if (hr == S_OK && !dj.Disjoint && dj.Frequency && t1 >= t0 && s.epoch == epoch && !s.discard)
        gpu_ms = (double)(t1 - t0) * 1000.0 / (double)dj.Frequency;
    }
  }

  void begin(Gpu& g) {
    open = false;
    if (!g.can_time) return;
    Slot& s = ring[next];
    if (s.pending) return;   // still in flight four frames on: skip this one
    if (!s.disjoint) {
      D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
      D3D11_QUERY_DESC qt{D3D11_QUERY_TIMESTAMP, 0};
      if (FAILED(g.dev->CreateQuery(&qd, s.disjoint.out())) || FAILED(g.dev->CreateQuery(&qt, s.begin.out())) ||
          FAILED(g.dev->CreateQuery(&qt, s.end.out()))) {
        s = Slot{};
        g.can_time = false;
        return;
      }
    }
    g.ctx->Begin(s.disjoint.get());
    g.ctx->End(s.begin.get());
    s.epoch = epoch;
    s.discard = first;
    first = false;
    open = true;
  }

  void end(Gpu& g) {
    if (!open) return;
    Slot& s = ring[next];
    g.ctx->End(s.end.get());
    g.ctx->End(s.disjoint.get());
    s.pending = true;
    next = (next + 1) % (int)ring.size();
    open = false;
  }
};

// ---- drawing -------------------------------------------------------------------------

// The ambient bars' numbers: the small picture is the frame shrunk about
// kAmbientShrink times a side (at least kAmbientMin pixels), blurred
// kAmbientRounds times each way, shown at kAmbientDim of its brightness with
// kAmbientColour of its colour.
constexpr int kAmbientShrink = 16, kAmbientMin = 8, kAmbientRounds = 2;
constexpr float kAmbientDim = 0.35f, kAmbientColour = 0.75f;

// What a window (or an off-screen render) keeps on one device: the frame's
// textures, Original, the passes' outputs and the ambient pictures.
struct Surfaces {
  uint64_t generation = 0;   // the device's they were made on
  Upload indices, palette, frame32;
  Target original;
  std::vector<Target> passes;
  Target ambient[2];
  // The look's passes, compiled on this device.
  std::vector<PassSpec> specs;
  std::vector<ID3D11PixelShader*> shaders;
  bool compiled = false;
  bool custom = false;   // specs are the caller's (render_passes_bgr_d3d), not the look's
  std::unique_ptr<ShaderPreset> preset;
  std::string preset_error;   // the chain failed: not tried again on this device
  Timing timing;

  void reset() {
    generation = 0;
    indices = palette = frame32 = Upload{};
    original.reset();
    passes.clear();
    ambient[0].reset();
    ambient[1].reset();
    specs.clear();
    shaders.clear();
    compiled = false;
    custom = false;
    if (preset) preset->reset();
    preset.reset();
    preset_error.clear();
    timing.reset();
  }
};

// The sharp look: one pass (hlsl_core.h).
std::vector<PassSpec> sharp_passes() {
  PassSpec p;
  p.name = "sharp";
  p.hlsl = hlsl::kSharpPs;
  p.size = PassSize::fit;
  return {p};
}

// Compiles the look's passes (or the caller's, `s.custom`) on `g` into `s`,
// once per device.
bool compile_look(Gpu& g, Surfaces& s, Look look, std::string* error) {
  if (s.compiled) return true;
  if (!s.custom) s.specs = look_passes(look);
  if (s.specs.empty()) return fail(error, std::string("look not available: ") + look_name(look));
  s.shaders.clear();
  for (const PassSpec& p : s.specs) {
    ID3D11PixelShader* ps = pixel_shader(g, with_prelude(p.hlsl), p.name, error);
    if (!ps) return false;
    s.shaders.push_back(ps);
  }
  s.compiled = true;
  return true;
}

void set_constants(Gpu& g, const PassConstants& c) {
  D3D11_MAPPED_SUBRESOURCE m{};
  if (SUCCEEDED(g.ctx->Map(g.constants.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
    memcpy(m.pData, &c, sizeof(c));
    g.ctx->Unmap(g.constants.get(), 0);
  }
}

void size4(float* out, int w, int h) {
  out[0] = (float)w;
  out[1] = (float)h;
  out[2] = 1.0f / (float)std::max(1, w);
  out[3] = 1.0f / (float)std::max(1, h);
}

// The state every pass shares (set again after anything else, such as a
// shader preset, has drawn).
void bind_common(Gpu& g) {
  g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  g.ctx->IASetInputLayout(nullptr);
  g.ctx->VSSetShader(g.vs.get(), nullptr, 0);
  ID3D11SamplerState* samplers[2] = {g.point.get(), g.linear.get()};
  g.ctx->PSSetSamplers(0, 2, samplers);
  ID3D11Buffer* cb = g.constants.get();
  g.ctx->PSSetConstantBuffers(0, 1, &cb);
  g.ctx->RSSetState(g.raster.get());
  g.ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
  g.ctx->OMSetDepthStencilState(nullptr, 0);
}

// One pass: `ps` over the full-screen triangle into `rtv` (rt_w x rt_h),
// within `vp`, reading `srvs` (t0..t7); drawn only within `clips` (`n` of
// them, in the target's pixels) when given, else all of `vp`.
void draw_pass(Gpu& g, ID3D11PixelShader* ps, ID3D11ShaderResourceView* const (&srvs)[8], ID3D11RenderTargetView* rtv,
               int rt_w, int rt_h, const RectI& vp, const PassConstants& c, const RectI* clips = nullptr,
               int n = 0) {
  set_constants(g, c);
  // The target first: binding it unbinds it as an input, if it was one.
  g.ctx->OMSetRenderTargets(1, &rtv, nullptr);
  g.ctx->PSSetShaderResources(0, 8, srvs);
  g.ctx->PSSetShader(ps, nullptr, 0);
  D3D11_VIEWPORT v{(float)vp.x, (float)vp.y, (float)vp.w, (float)vp.h, 0.0f, 1.0f};
  g.ctx->RSSetViewports(1, &v);
  if (!clips) {
    clips = &vp;
    n = 1;
  }
  for (int i = 0; i < n; ++i) {
    const RectI& r = clips[i];
    const D3D11_RECT sc{std::max({0, r.x, vp.x}), std::max({0, r.y, vp.y}), std::min({rt_w, r.x + r.w, vp.x + vp.w}),
                        std::min({rt_h, r.y + r.h, vp.y + vp.h})};
    if (sc.right <= sc.left || sc.bottom <= sc.top) continue;
    g.ctx->RSSetScissorRects(1, &sc);
    g.ctx->Draw(3, 0);
  }
  // Nothing stays bound as an input, so the next pass may write it.
  ID3D11ShaderResourceView* none[8] = {};
  g.ctx->PSSetShaderResources(0, 8, none);
}

// The frame up to the GPU and resolved to colour: Original.
HRESULT resolve(Gpu& g, Surfaces& s, const Frame& f, std::string* error) {
  HRESULT hr = ensure_target(g, s.original, f.width, f.height, DXGI_FORMAT_R8G8B8A8_UNORM);
  if (FAILED(hr)) {
    fail(error, "CreateTexture2D (Original)", hr);
    return hr;
  }
  ID3D11PixelShader* ps = nullptr;
  ID3D11ShaderResourceView* srvs[8] = {};
  if (f.bpp == 8) {
    hr = ensure_upload(g, s.indices, f.width, f.height, DXGI_FORMAT_R8_UINT);
    if (SUCCEEDED(hr)) hr = ensure_upload(g, s.palette, 256, 1, DXGI_FORMAT_B8G8R8A8_UNORM);
    if (FAILED(hr)) {
      fail(error, "CreateTexture2D (frame)", hr);
      return hr;
    }
    g.ctx->UpdateSubresource(s.indices.tex.get(), 0, nullptr, f.bits.data(), (UINT)f.stride, 0);
    g.ctx->UpdateSubresource(s.palette.tex.get(), 0, nullptr, f.palette.data(), 256 * 4, 0);
    ps = pixel_shader(g, hlsl::kResolve8Ps, "resolve8", error);
    srvs[0] = s.indices.srv.get();
    srvs[1] = s.palette.srv.get();
  } else {
    hr = ensure_upload(g, s.frame32, f.width, f.height, DXGI_FORMAT_B8G8R8A8_UNORM);
    if (FAILED(hr)) {
      fail(error, "CreateTexture2D (frame)", hr);
      return hr;
    }
    g.ctx->UpdateSubresource(s.frame32.tex.get(), 0, nullptr, f.bits.data(), (UINT)f.stride, 0);
    ps = pixel_shader(g, hlsl::kResolve32Ps, "resolve32", error);
    srvs[0] = s.frame32.srv.get();
  }
  if (!ps) return E_FAIL;
  PassConstants c{};
  size4(c.source_size, f.width, f.height);
  size4(c.original_size, f.width, f.height);
  size4(c.output_size, f.width, f.height);
  draw_pass(g, ps, srvs, s.original.rtv.get(), f.width, f.height, RectI{0, 0, f.width, f.height}, c);
  return S_OK;
}

// Fills the whole target with the ambient glow (the bars' picture).
HRESULT ambient_fill(Gpu& g, Surfaces& s, const Frame& f, ID3D11RenderTargetView* rtv, int w, int h, const RectI& fit,
                     std::string* error) {
  const int sw = std::max(kAmbientMin, (f.width + kAmbientShrink / 2) / kAmbientShrink);
  const int sh = std::max(kAmbientMin, (f.height + kAmbientShrink / 2) / kAmbientShrink);
  for (Target& t : s.ambient) {
    const HRESULT hr = ensure_target(g, t, sw, sh, DXGI_FORMAT_R16G16B16A16_FLOAT);
    if (FAILED(hr)) {
      fail(error, "CreateTexture2D (ambient)", hr);
      return hr;
    }
  }
  ID3D11PixelShader* down = pixel_shader(g, with_prelude(hlsl::kAmbientDownPs), "ambient-down", error);
  ID3D11PixelShader* blur = down ? pixel_shader(g, with_prelude(hlsl::kAmbientBlurPs), "ambient-blur", error) : nullptr;
  ID3D11PixelShader* fill = blur ? pixel_shader(g, with_prelude(hlsl::kAmbientFillPs), "ambient-fill", error) : nullptr;
  if (!fill) return E_FAIL;
  PassConstants c{};
  size4(c.original_size, f.width, f.height);
  c.fit_rect[0] = (float)fit.x;
  c.fit_rect[1] = (float)fit.y;
  c.fit_rect[2] = (float)fit.w;
  c.fit_rect[3] = (float)fit.h;
  size4(c.window_size, w, h);
  const RectI small{0, 0, sw, sh};
  {
    ID3D11ShaderResourceView* srvs[8] = {s.original.srv.get(), s.original.srv.get()};
    size4(c.source_size, f.width, f.height);
    size4(c.output_size, sw, sh);
    draw_pass(g, down, srvs, s.ambient[0].rtv.get(), sw, sh, small, c);
  }
  size4(c.source_size, sw, sh);
  for (int i = 0; i < kAmbientRounds * 2; ++i) {
    const int from = i % 2, to = 1 - from;
    ID3D11ShaderResourceView* srvs[8] = {s.ambient[from].srv.get(), s.original.srv.get()};
    c.params[0] = i % 2 == 0 ? 1.0f : 0.0f;
    c.params[1] = i % 2 == 0 ? 0.0f : 1.0f;
    draw_pass(g, blur, srvs, s.ambient[to].rtv.get(), sw, sh, small, c);
  }
  // kAmbientRounds * 2 passes end where they began: in ambient[0]. It goes
  // over the whole window, but is drawn only in the bars: the look covers
  // the rest (a quarter of a 16:9 window for a 4:3 frame, not all of it).
  ID3D11ShaderResourceView* srvs[8] = {s.ambient[0].srv.get(), s.original.srv.get()};
  size4(c.output_size, w, h);
  c.params[0] = kAmbientDim;
  c.params[1] = kAmbientColour;
  const int x0 = std::clamp(fit.x, 0, w), x1 = std::clamp(fit.x + fit.w, x0, w);
  const int y0 = std::clamp(fit.y, 0, h), y1 = std::clamp(fit.y + fit.h, y0, h);
  const RectI bars[4] = {{0, 0, w, y0}, {0, y1, w, h - y1}, {0, y0, x0, y1 - y0}, {x1, y0, w - x1, y1 - y0}};
  draw_pass(g, fill, srvs, rtv, w, h, RectI{0, 0, w, h}, c, bars, 4);
  return S_OK;
}

// The output size of a pass before the last, from the one before it
// (`source`) and the frame.
SizeI pass_size(const PassSpec& p, SizeI source, SizeI original, const RectI& fit) {
  auto scaled = [&](SizeI base) {
    return SizeI{std::max(1, (int)std::lround(base.w * (double)p.scale)),
                 std::max(1, (int)std::lround(base.h * (double)p.scale))};
  };
  switch (p.size) {
    case PassSize::original: return scaled(original);
    case PassSize::fit: return SizeI{std::max(1, fit.w), std::max(1, fit.h)};
    case PassSize::source:
    default: return scaled(source);
  }
}

// Everything a window shows, into `rtv` (w x h): the frame resolved, the bars
// (black or the ambient glow), the look into `fit`. A shader preset whose
// chain is not ready yet draws as the sharp look.
HRESULT draw_frame(Gpu& g, Surfaces& s, const Frame& f, ID3D11RenderTargetView* rtv, int w, int h, const RectI& fit,
                   const LookOptions& opts, uint32_t frame_count, std::string* error) {
  // The look's passes (the sharp look stands in for a preset until its chain is ready).
  const bool use_preset = opts.look == Look::preset && s.preset && s.preset->ready();
  if (!s.compiled) {
    const Look look = opts.look == Look::preset ? Look::sharp : opts.look;
    if (!compile_look(g, s, look, error)) return E_FAIL;
  }
  bind_common(g);
  HRESULT hr = resolve(g, s, f, error);
  if (FAILED(hr)) return hr;
  const bool bars = fit.x > 0 || fit.y > 0 || fit.x + fit.w < w || fit.y + fit.h < h;
  if (opts.ambient && bars) {
    hr = ambient_fill(g, s, f, rtv, w, h, fit, error);
    if (FAILED(hr)) return hr;
  } else {
    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    g.ctx->ClearRenderTargetView(rtv, black);
  }
  if (fit.w <= 0 || fit.h <= 0) return S_OK;
  if (use_preset) {
    std::string why;
    if (!s.preset->draw(g.ctx.get(), s.original.srv.get(), f.width, f.height, rtv, w, h, fit, frame_count, &why)) {
      fail(error, "shader preset: " + why);
      return E_FAIL;
    }
    return S_OK;
  }
  const SizeI original{f.width, f.height};
  SizeI source = original;
  ID3D11ShaderResourceView* source_srv = s.original.srv.get();
  const size_t n = s.specs.size();
  if (s.passes.size() < n) s.passes.resize(n);
  for (size_t i = 0; i < n; ++i) {
    const PassSpec& p = s.specs[i];
    const bool last = i + 1 == n;
    ID3D11ShaderResourceView* srvs[8] = {source_srv, s.original.srv.get()};
    for (size_t j = 0; j < i && j < 6; ++j) srvs[2 + j] = s.passes[j].srv.get();
    PassConstants c{};
    size4(c.source_size, source.w, source.h);
    size4(c.original_size, original.w, original.h);
    c.fit_rect[0] = (float)fit.x;
    c.fit_rect[1] = (float)fit.y;
    c.fit_rect[2] = (float)fit.w;
    c.fit_rect[3] = (float)fit.h;
    size4(c.window_size, w, h);
    std::copy(p.params.begin(), p.params.end(), c.params);
    c.frame_count = frame_count;
    if (last) {
      size4(c.output_size, fit.w, fit.h);
      draw_pass(g, s.shaders[i], srvs, rtv, w, h, fit, c);
      break;
    }
    const SizeI out = pass_size(p, source, original, fit);
    hr = ensure_target(g, s.passes[i], out.w, out.h, dxgi_format(p.format));
    if (FAILED(hr)) {
      fail(error, "CreateTexture2D (pass " + p.name + ")", hr);
      return hr;
    }
    size4(c.output_size, s.passes[i].w, s.passes[i].h);
    draw_pass(g, s.shaders[i], srvs, s.passes[i].rtv.get(), s.passes[i].w, s.passes[i].h,
              RectI{0, 0, s.passes[i].w, s.passes[i].h}, c);
    source = {s.passes[i].w, s.passes[i].h};
    source_srv = s.passes[i].srv.get();
  }
  return S_OK;
}

// Makes the window's (or render's) shader preset chain on `g`, once.
void ensure_preset(Gpu& g, Surfaces& s, const LookOptions& opts) {
  if (opts.look != Look::preset || s.preset || !s.preset_error.empty()) return;
  s.preset = std::make_unique<ShaderPreset>();
  std::string why;
  if (!s.preset->create(g.dev.get(), g.ctx.get(), opts.preset, &why)) {
    s.preset.reset();
    s.preset_error = why.empty() ? "the shader preset failed" : why;
  }
}

// The file ShaderPreset::create() reads for ShaderPreset=`path`: a relative
// path is taken from the data folder, where settings.ini is, as there.
std::wstring preset_file(const std::wstring& path) {
  const bool rooted = (!path.empty() && (path[0] == L'\\' || path[0] == L'/')) ||
                      (path.size() >= 3 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/'));
  return rooted ? path : join_path(app_data_root(), path);
}

std::string look_text(const LookOptions& opts, const ShaderPreset* preset) {
  std::string text;
  if (opts.look == Look::preset) {
    if (preset) {
      text = "preset " + preset->describe();
    } else {
      const size_t slash = opts.preset.find_last_of(L"\\/");
      text = "preset " + narrow_utf8((slash == std::wstring::npos ? opts.preset : opts.preset.substr(slash + 1)).c_str());
    }
  } else {
    text = look_name(opts.look);
  }
  if (opts.ambient) text += ", ambient bars";
  return text;
}

std::string device_text(const Gpu& g) {
  return g.adapter + ", feature level " + level_name(g.level);
}

} // namespace

// ---- D3DPresenter ----------------------------------------------------------------------

struct D3DPresenter::Impl {
  LookOptions opts;
  HWND hwnd = nullptr;
  ComPtr<IDXGISwapChain1> swap;
  ComPtr<ID3D11RenderTargetView> back;
  SizeI size{};
  uint64_t generation = 0;   // the device the swap chain is on
  Surfaces s;
  uint32_t frames = 0;
  double gpu_ms = -1;
  std::string described;   // the device part of describe(), once known
  bool standing_in = false;     // the latest present drew sharp for a preset not built yet
  uint32_t drawn_changes = 0;   // the preset drawn at last, after the sharp look stood in

  void drop_swap_chain() {
    Gpu& g = hw_gpu();
    if (g.ctx && (swap || back)) {
      // Unbound and flushed, so the swap chain is really gone and GDI owns the window.
      g.ctx->ClearState();
      g.ctx->Flush();
    }
    back.reset();
    swap.reset();
    hwnd = nullptr;
    size = {};
  }
  void drop_all() {
    drop_swap_chain();
    s.reset();
    generation = 0;
  }
};

D3DPresenter::D3DPresenter(LookOptions opts) : impl_(std::make_unique<Impl>()) { impl_->opts = std::move(opts); }

D3DPresenter::~D3DPresenter() {
  std::lock_guard<std::recursive_mutex> lock(d3d_lock());
  if (impl_->generation == hw_gpu().generation) {
    impl_->drop_all();
  } else {
    // Made on a device since lost: let go without touching the new one.
    impl_->back.reset();
    impl_->swap.reset();
    impl_->s.reset();
  }
}

const LookOptions& D3DPresenter::options() const { return impl_->opts; }

bool D3DPresenter::ready() const {
  std::lock_guard<std::recursive_mutex> lock(d3d_lock());
  return impl_->swap && impl_->generation == hw_gpu().generation && (bool)hw_gpu();
}

void D3DPresenter::release() {
  std::lock_guard<std::recursive_mutex> lock(d3d_lock());
  if (impl_->generation == hw_gpu().generation) {
    impl_->drop_swap_chain();
  } else {
    impl_->back.reset();
    impl_->swap.reset();
    impl_->hwnd = nullptr;
  }
}

double D3DPresenter::gpu_ms() const { return impl_->gpu_ms; }

bool D3DPresenter::standing_in() const { return impl_->standing_in; }

uint32_t D3DPresenter::drawn_changes() const { return impl_->drawn_changes; }

std::string D3DPresenter::describe() const {
  std::lock_guard<std::recursive_mutex> lock(d3d_lock());
  std::string text = look_text(impl_->opts, impl_->s.preset.get());
  const Gpu& g = hw_gpu();
  const std::string device = !impl_->described.empty() ? impl_->described : g ? device_text(g) : std::string();
  if (!device.empty()) text += " (" + device + ", blt model)";
  return text;
}

bool D3DPresenter::clear(HWND hwnd) {
  std::lock_guard<std::recursive_mutex> lock(d3d_lock());
  Impl& d = *impl_;
  Gpu& g = hw_gpu();
  if (!d.swap || d.hwnd != hwnd || d.generation != g.generation || !g) return false;
  RECT cr{};
  GetClientRect(hwnd, &cr);
  const SizeI size{std::max(1, (int)cr.right), std::max(1, (int)cr.bottom)};
  HRESULT hr = S_OK;
  if (!(d.size == size)) {
    g.ctx->ClearState();
    d.back.reset();
    hr = d.swap->ResizeBuffers(0, (UINT)size.w, (UINT)size.h, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
      d.drop_swap_chain();
      return false;
    }
    d.size = size;
  }
  if (!d.back) {
    ComPtr<ID3D11Texture2D> bb;
    hr = d.swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(bb.out()));
    if (SUCCEEDED(hr)) hr = g.dev->CreateRenderTargetView(bb.get(), nullptr, d.back.out());
    if (FAILED(hr)) {
      d.drop_swap_chain();
      return false;
    }
  }
  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  g.ctx->ClearRenderTargetView(d.back.get(), black);
  hr = d.swap->Present(0, 0);
  if (FAILED(hr)) {
    if (is_device_lost(hr)) {
      d.drop_all();
      lose_device(g);
    } else {
      d.drop_swap_chain();
    }
    return false;
  }
  return true;
}

bool D3DPresenter::present(HWND hwnd, const Frame& f, const RectI& fit, std::string* error, bool* device_lost) {
  if (device_lost) *device_lost = false;
  std::lock_guard<std::recursive_mutex> lock(d3d_lock());
  Impl& d = *impl_;
  if (f.width <= 0 || f.height <= 0 || f.bits.size() < (size_t)f.stride * f.height || (f.bpp != 8 && f.bpp != 32))
    return fail(error, "empty frame");
  // What can't work whatever the window: a look not built, a preset without librashader.
  if (d.opts.look == Look::preset) {
    std::string why;
    if (!shader_preset_library(&why)) return fail(error, "shader preset: " + why);
  } else if (!d.s.compiled && d.opts.look != Look::sharp && look_passes(d.opts.look).empty()) {
    return fail(error, std::string("look not available: ") + look_name(d.opts.look));
  }
  if (!hwnd || !IsWindow(hwnd)) return fail(error, "no window");
  if (!hw_device(error)) return false;
  Gpu& g = hw_gpu();
  if (d.generation != g.generation) {
    // A new device (the first, or after a loss): everything made again on it.
    d.back.reset();
    d.swap.reset();
    d.s.reset();
    d.hwnd = nullptr;
    d.size = {};
    d.generation = g.generation;
    d.s.generation = g.generation;
    d.described = device_text(g);
  }
  if (d.swap && d.hwnd != hwnd) d.drop_swap_chain();
  RECT cr{};
  GetClientRect(hwnd, &cr);
  const SizeI size{std::max(1, (int)cr.right), std::max(1, (int)cr.bottom)};
  HRESULT hr = S_OK;
  auto lost = [&](const char* what) {
    d.drop_all();
    lose_device(g);
    if (device_lost) *device_lost = true;
    return fail(error, std::string("device lost (") + what + ")", hr);
  };
  if (g_simulate_loss) {
    g_simulate_loss = false;
    hr = DXGI_ERROR_DEVICE_REMOVED;
    return lost("simulated");
  }
  if (!d.swap) {
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    hr = g.dev.as(dxgi);
    if (SUCCEEDED(hr)) hr = dxgi->GetAdapter(adapter.out());
    if (SUCCEEDED(hr)) hr = adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(factory.out()));
    if (FAILED(hr)) return fail(error, "no DXGI 1.2 factory", hr);
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = (UINT)size.w;
    sd.Height = (UINT)size.h;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 1;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;   // the blt model: see the top of this file
    sd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    hr = factory->CreateSwapChainForHwnd(g.dev.get(), hwnd, &sd, nullptr, nullptr, d.swap.out());
    if (FAILED(hr)) {
      if (device_gone(g, hr)) return lost("CreateSwapChainForHwnd");
      return fail(error, "CreateSwapChainForHwnd", hr);
    }
    // Alt+Enter is no business of a screen saver's, and the saver's own
    // messages need no watching.
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    d.hwnd = hwnd;
    d.size = size;
  } else if (!(d.size == size)) {
    g.ctx->ClearState();
    d.back.reset();
    hr = d.swap->ResizeBuffers(0, (UINT)size.w, (UINT)size.h, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
      if (device_gone(g, hr)) return lost("ResizeBuffers");
      d.drop_swap_chain();
      return fail(error, "ResizeBuffers", hr);
    }
    d.size = size;
  }
  if (!d.back) {
    ComPtr<ID3D11Texture2D> bb;
    hr = d.swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(bb.out()));
    if (SUCCEEDED(hr)) hr = g.dev->CreateRenderTargetView(bb.get(), nullptr, d.back.out());
    if (FAILED(hr)) {
      if (device_gone(g, hr)) return lost("GetBuffer");
      d.drop_swap_chain();
      return fail(error, "the back buffer", hr);
    }
  }
  // A shader preset's chain is built off this thread (shader_preset.h): the
  // sharp look stands in until it is ready; a build that failed is this
  // window's fall back.
  ensure_preset(g, d.s, d.opts);
  if (d.opts.look == Look::preset) {
    std::string why = d.s.preset_error;
    if (!d.s.preset || (!d.s.preset->ready() && d.s.preset->failed(&why))) {
      // A chain that failed because the device went away is a loss, not the preset's fault.
      if (const HRESULT reason = g.dev->GetDeviceRemovedReason(); reason != S_OK) {
        hr = reason;
        return lost("shader preset");
      }
      return fail(error, "shader preset: " + why);
    }
  }
  // The preset drawn at last, after the sharp look stood in: its cost is
  // timed afresh.
  const bool standing_in = d.opts.look == Look::preset && !d.s.preset->ready();
  const bool changed = d.standing_in && !standing_in;
  if (changed) d.s.timing.restart();
  d.s.timing.poll(g, false);
  d.s.timing.begin(g);
  std::string why;
  hr = draw_frame(g, d.s, f, d.back.get(), size.w, size.h, fit, d.opts, d.frames, &why);
  d.s.timing.end(g);
  if (FAILED(hr)) {
    if (device_gone(g, hr)) return lost("drawing");
    if (error) *error = why;
    return false;
  }
  hr = d.swap->Present(0, 0);   // never waits for the vertical blank: the saver paces on DwmFlush
  if (FAILED(hr)) {
    if (device_gone(g, hr)) return lost("Present");
    d.drop_swap_chain();
    return fail(error, "Present", hr);
  }
  ++d.frames;
  d.gpu_ms = d.s.timing.gpu_ms;
  if (changed) ++d.drawn_changes;
  d.standing_in = standing_in;
  return true;
}

// ---- the rest of present_d3d.h -----------------------------------------------------------

bool d3d_available(std::string* why) {
  std::lock_guard<std::recursive_mutex> lock(d3d_lock());
  static int known = -1;
  static std::string reason;
  if (known < 0) {
    known = 0;
    if (!libraries_ok(&reason)) {
    } else if (hw_gpu() || warp_gpu()) {
      known = 1;
    } else {
      // Asks without making a device (none is kept for this).
      static const D3D_FEATURE_LEVEL kLevels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                                  D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
      for (D3D_DRIVER_TYPE type : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
        D3D_FEATURE_LEVEL level{};
        const HRESULT hr = libraries().create_device(nullptr, type, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, kLevels,
                                                     (UINT)std::size(kLevels), D3D11_SDK_VERSION, nullptr, &level,
                                                     nullptr);
        if (SUCCEEDED(hr)) {
          known = 1;
          break;
        }
      }
      if (!known) reason = "no Direct3D 11 device of feature level 10.0 or up";
    }
  }
  if (!known && why) *why = reason;
  return known == 1;
}

bool d3d_choose_adapter(const std::string& match, std::string* why) {
  std::lock_guard<std::recursive_mutex> lock(d3d_lock());
  if (hw_gpu()) return fail(why, "the device is made already");
  if (!libraries_ok(why)) return false;
  // The adapters, through a device's factory (dxgi.dll is never named).
  ComPtr<ID3D11Device> dev;
  ComPtr<IDXGIDevice> dxgi;
  ComPtr<IDXGIAdapter> first;
  ComPtr<IDXGIFactory1> factory;
  HRESULT hr = libraries().create_device(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                         dev.out(), nullptr, nullptr);
  if (SUCCEEDED(hr)) hr = dev.as(dxgi);
  if (SUCCEEDED(hr)) hr = dxgi->GetAdapter(first.out());
  if (SUCCEEDED(hr)) hr = first->GetParent(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory.out()));
  if (FAILED(hr)) return fail(why, "no DXGI factory", hr);
  std::string names;
  for (UINT i = 0;; ++i) {
    ComPtr<IDXGIAdapter1> a;
    if (factory->EnumAdapters1(i, a.out()) != S_OK) break;
    DXGI_ADAPTER_DESC1 desc{};
    a->GetDesc1(&desc);
    const std::string name = narrow_utf8(desc.Description);
    names += (names.empty() ? "" : ", ") + std::to_string(i) + ": " + name;
    std::string lower = name, want = match;
    for (char& c : lower) c = (char)tolower((unsigned char)c);
    for (char& c : want) c = (char)tolower((unsigned char)c);
    if (match == std::to_string(i) || (!want.empty() && lower.find(want) != std::string::npos)) {
      ComPtr<IDXGIAdapter> chosen;
      if (FAILED(a.as(chosen))) break;
      chosen_adapter() = chosen;
      return true;
    }
  }
  return fail(why, "no adapter matches \"" + match + "\" (" + names + ")");
}

void d3d_simulate_device_loss() {
  std::lock_guard<std::recursive_mutex> lock(d3d_lock());
  g_simulate_loss = true;
}

bool d3d_prepare(const LookOptions& opts, std::string* why) {
  std::lock_guard<std::recursive_mutex> lock(d3d_lock());
  if (!hw_device(why)) return false;
  Look look = opts.look;
  if (opts.look == Look::preset) {
    // The chain is each window's, built as it first draws: here, whether it
    // can be (librashader and the preset's file there), and the sharp look
    // that stands in meanwhile.
    std::string reason;
    if (!shader_preset_library(&reason)) return fail(why, "shader preset: " + reason);
    const std::wstring file = preset_file(opts.preset);
    if (!file_exists(file))
      return fail(why, "shader preset: the shader preset \"" + narrow_utf8(file.c_str()) + "\" isn't there");
    look = Look::sharp;
  }
  Surfaces s;
  if (!compile_look(hw_gpu(), s, look, why)) return false;
  if (opts.ambient) {
    for (const char* src : {hlsl::kAmbientDownPs, hlsl::kAmbientBlurPs, hlsl::kAmbientFillPs}) {
      if (!pixel_shader(hw_gpu(), with_prelude(src), "ambient", why)) return false;
    }
  }
  for (const char* src : {hlsl::kResolve8Ps, hlsl::kResolve32Ps}) {
    if (!pixel_shader(hw_gpu(), src, "resolve", why)) return false;
  }
  return true;
}

namespace {

// Off-screen renders keep their textures between calls (lookshot --repeat,
// the tests), per device.
struct Offscreen {
  uint64_t generation = 0;
  Surfaces s;
  LookOptions opts;
  Target target;
  ComPtr<ID3D11Texture2D> staging;
  int staging_w = 0, staging_h = 0;
  double gpu_ms = -1;
};
Offscreen& offscreen() {
  static auto* o = new Offscreen;
  return *o;
}

} // namespace

double render_frame_gpu_ms() {
  std::lock_guard<std::recursive_mutex> lock(d3d_lock());
  return offscreen().gpu_ms;
}

namespace {

// render_frame_bgr_d3d, with the caller's passes when `custom` is given.
bool render_offscreen(const Frame& f, int w, int h, const RectI& fit, const LookOptions& opts,
                      const std::vector<PassSpec>* custom, std::vector<uint8_t>& bgr, std::string* error) {
  bgr.clear();
  if (w <= 0 || h <= 0 || f.width <= 0 || f.height <= 0 || f.bits.size() < (size_t)f.stride * f.height ||
      (f.bpp != 8 && f.bpp != 32))
    return fail(error, "empty picture");
  std::lock_guard<std::recursive_mutex> lock(d3d_lock());
  if (custom && custom->empty()) return fail(error, "no passes");
  if (opts.look == Look::preset && !custom) {
    std::string why;
    if (!shader_preset_library(&why)) return fail(error, "shader preset: " + why);
  }
  // The windows' device when there is one, so the picture is theirs; else WARP.
  Gpu* g = hw_gpu() ? &hw_gpu() : nullptr;
  if (!g) {
    if (!warp_gpu() && !make_device(warp_gpu(), true, error)) return false;
    g = &warp_gpu();
  }
  Offscreen& o = offscreen();
  if (o.generation != g->generation || !(o.opts == opts) || custom || o.s.custom) {
    o.s.reset();
    o.target.reset();
    o.staging.reset();
    o.staging_w = o.staging_h = 0;
    o.generation = g->generation;
    o.opts = opts;
  }
  if (custom) {
    o.s.specs = *custom;
    o.s.custom = true;
  }
  HRESULT hr = ensure_target(*g, o.target, w, h, DXGI_FORMAT_B8G8R8A8_UNORM);
  if (FAILED(hr) || o.target.w != w || o.target.h != h) return fail(error, "CreateTexture2D (picture)", hr);
  if (!o.staging || o.staging_w != w || o.staging_h != h) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)w;
    td.Height = (UINT)h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = g->dev->CreateTexture2D(&td, nullptr, o.staging.out());
    if (FAILED(hr)) return fail(error, "CreateTexture2D (staging)", hr);
    o.staging_w = w;
    o.staging_h = h;
  }
  if (!custom) ensure_preset(*g, o.s, opts);
  if (opts.look == Look::preset && !custom) {
    if (!o.s.preset) return fail(error, "shader preset: " + o.s.preset_error);
    // An off-screen picture is of the preset itself: its chain, waited for.
    std::string why;
    if (!o.s.preset->wait(60000)) {
      if (!o.s.preset->failed(&why)) why = "not built in a minute";
      return fail(error, "shader preset: " + why);
    }
  }
  o.s.timing.begin(*g);
  std::string why;
  hr = draw_frame(*g, o.s, f, o.target.rtv.get(), w, h, fit, opts, 0, &why);
  o.s.timing.end(*g);
  if (FAILED(hr)) {
    if (device_gone(*g, hr)) {
      o.s.reset();
      o.target.reset();
      o.staging.reset();
      lose_device(*g);
      return fail(error, "device lost");
    }
    if (error) *error = why;
    return false;
  }
  g->ctx->CopyResource(o.staging.get(), o.target.tex.get());
  D3D11_MAPPED_SUBRESOURCE m{};
  hr = g->ctx->Map(o.staging.get(), 0, D3D11_MAP_READ, 0, &m);
  if (FAILED(hr)) {
    if (device_gone(*g, hr)) {
      o.s.reset();
      o.target.reset();
      o.staging.reset();
      lose_device(*g);
      return fail(error, "device lost", hr);
    }
    return fail(error, "Map (staging)", hr);
  }
  bgr.resize((size_t)w * h * 3);
  for (int y = 0; y < h; ++y) {
    const uint8_t* s = static_cast<const uint8_t*>(m.pData) + (size_t)y * m.RowPitch;
    uint8_t* d = bgr.data() + (size_t)y * w * 3;
    for (int x = 0; x < w; ++x, s += 4, d += 3) {
      d[0] = s[0];
      d[1] = s[1];
      d[2] = s[2];
    }
  }
  g->ctx->Unmap(o.staging.get(), 0);
  o.s.timing.poll(*g, true);
  o.gpu_ms = o.s.timing.gpu_ms;
  return true;
}

} // namespace

bool render_frame_bgr_d3d(const Frame& f, int w, int h, const RectI& fit, const LookOptions& opts,
                          std::vector<uint8_t>& bgr, std::string* error) {
  return render_offscreen(f, w, h, fit, opts, nullptr, bgr, error);
}

bool render_passes_bgr_d3d(const Frame& f, int w, int h, const RectI& fit, const std::vector<PassSpec>& passes,
                           bool ambient, std::vector<uint8_t>& bgr, std::string* error) {
  LookOptions opts;
  opts.ambient = ambient;
  return render_offscreen(f, w, h, fit, opts, &passes, bgr, error);
}

std::vector<PassSpec> look_passes(Look look) {
  switch (look) {
    case Look::sharp: return sharp_passes();
    case Look::crt: return crt_passes(false);
    case Look::crt_curved: return crt_passes(true);
    case Look::smooth: return smooth_passes();
    default: return {};
  }
}

} // namespace adw::scr
