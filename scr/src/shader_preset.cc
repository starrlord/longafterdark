// Look::preset (shader_preset.h): RetroArch shader presets drawn by
// librashader, which the user supplies. Nothing of librashader is built in:
// the DLL is loaded by full path, and the few functions we call are declared
// here, from its documented C ABI.
#include "shader_preset.h"

#include <windows.h>

#include <d3d11.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

#include "log.h"
#include "paths.h"

namespace adw::scr {

namespace {

// ---- librashader's C ABI ------------------------------------------------------------
// What we call of librashader's C API, declared from its documentation (as
// host/win16/signatures16.cc declares Win16's). ABI versions don't mix: these
// are ABI 2's (librashader 0.5.0 on). Within an ABI the API version only adds,
// backwards compatible. The one versioned option struct we pass, the chain's
// (LibraChainOptions: its shader cache on or off), says API 0, the first, and
// a frame gets none (null: librashader's defaults), so any API of ABI 2 will
// do: ABI 2 began at API 1.
constexpr size_t kLibraAbi = 2;
constexpr size_t kLibraMinApi = 1;

struct LibraError;           // what a libra_error_t points at: opaque
using LibraPreset = void*;   // libra_shader_preset_t
using LibraChain = void*;    // libra_d3d11_filter_chain_t
struct LibraViewport {       // libra_viewport_t: where in the output the last pass draws
  float x, y;
  uint32_t width, height;
};
// filter_chain_d3d11_opt_t's fields at API 0. librashader reads the fields of
// the version given and no others, but copies the struct whole as its own
// version declares it: the spare room keeps a later, longer one within ours.
struct LibraChainOptions {
  size_t version = 0;
  bool force_no_mipmaps = false;
  bool disable_cache = false;
  uint8_t spare[64] = {};
};

using LibraVersionFn = size_t (*)();
using PresetCreateFn = LibraError* (*)(const char* utf8_path, LibraPreset* out);
using PresetFreeFn = LibraError* (*)(LibraPreset* preset);
// Both creates take the preset over (and null it), whether they succeed or not.
using ChainCreateFn = LibraError* (*)(LibraPreset* preset, ID3D11Device* device, const void* options,
                                      LibraChain* out);
using ChainCreateDeferredFn = LibraError* (*)(LibraPreset* preset, ID3D11Device* device,
                                              ID3D11DeviceContext* context, const void* options, LibraChain* out);
using ChainFrameFn = LibraError* (*)(LibraChain* chain, ID3D11DeviceContext* context, size_t frame_count,
                                     ID3D11ShaderResourceView* image, ID3D11RenderTargetView* out,
                                     const LibraViewport* viewport, const float* mvp, const void* options);
using ChainFreeFn = LibraError* (*)(LibraChain* chain);
using ErrorWriteFn = int32_t (*)(LibraError* error, char** out);
using ErrorFreeStringFn = int32_t (*)(char** out);
using ErrorFreeFn = int32_t (*)(LibraError** error);

struct Library {
  bool ok = false;
  bool found = false;   // a librashader.dll was there: loaded or refused, for good
  std::string why;      // !ok: why not, naming where it looked
  std::string path;     // the DLL in use, UTF-8
  size_t abi = 0, api = 0;
  PresetCreateFn preset_create = nullptr;
  PresetFreeFn preset_free = nullptr;
  ChainCreateFn chain_create = nullptr;
  ChainCreateDeferredFn chain_create_deferred = nullptr;
  ChainFrameFn chain_frame = nullptr;
  ChainFreeFn chain_free = nullptr;
  ErrorWriteFn error_write = nullptr;
  ErrorFreeStringFn error_free_string = nullptr;
  ErrorFreeFn error_free = nullptr;
};

template <typename Fn>
void find(HMODULE m, const char* name, Fn& out, std::string& missing) {
  out = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(m, name)));
  if (!out) missing += (missing.empty() ? "" : ", ") + std::string(name);
}

std::string system_message(DWORD code) {
  wchar_t* text = nullptr;
  FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                 code, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
  std::wstring w = text ? text : L"";
  if (text) LocalFree(text);
  while (!w.empty() && (w.back() == L'\n' || w.back() == L'\r' || w.back() == L' ' || w.back() == L'.')) w.pop_back();
  return w.empty() ? "unknown error" : narrow(w);
}

std::string hresult_text(HRESULT hr) {
  char buf[16];
  snprintf(buf, sizeof buf, "0x%08lX", (unsigned long)hr);
  return buf;
}

// The DLL at `path`, if it is a librashader this build speaks. Its own
// folder and System32 answer for what it needs; the DLL search order never
// does. false: `*why`.
bool load_from(const std::wstring& path, Library& lib, std::string* why) {
  const std::string where = "librashader.dll at \"" + narrow(path) + "\"";
  HMODULE m = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!m) {
    // The system's words for these two mislead here (the file is there; "%1").
    const DWORD e = GetLastError();
    const std::string code = " (error " + std::to_string(e) + ")";
    if (e == ERROR_MOD_NOT_FOUND) {
      *why = where + " didn't load: a DLL it needs is missing" + code +
             ". librashader's Windows build needs the Visual C++ 2015-2022 x64 runtime and DirectX's D3DX9_43.dll, "
             "in its folder or in System32";
    } else if (e == ERROR_BAD_EXE_FORMAT) {
      *why = where + " didn't load: it isn't a 64-bit Windows DLL" + code + ". It must be the x86_64 build";
    } else {
      *why = where + " didn't load: " + system_message(e) + code;
    }
    return false;
  }
  LibraVersionFn abi_fn = nullptr, api_fn = nullptr;
  std::string missing;
  find(m, "libra_instance_abi_version", abi_fn, missing);
  find(m, "libra_instance_api_version", api_fn, missing);
  if (!missing.empty()) {
    FreeLibrary(m);
    *why = where + " isn't librashader's C API (it has no " + missing + ")";
    return false;
  }
  const size_t abi = abi_fn(), api = api_fn();
  if (abi != kLibraAbi || api < kLibraMinApi) {
    FreeLibrary(m);
    *why = where + " speaks librashader's ABI " + std::to_string(abi) + " (API " + std::to_string(api) +
           "), and this build speaks ABI " + std::to_string(kLibraAbi) + " (API " + std::to_string(kLibraMinApi) +
           " and later: librashader 0.5.0 to at least 0.12.0)";
    return false;
  }
  find(m, "libra_preset_create", lib.preset_create, missing);
  find(m, "libra_preset_free", lib.preset_free, missing);
  find(m, "libra_d3d11_filter_chain_create", lib.chain_create, missing);
  find(m, "libra_d3d11_filter_chain_create_deferred", lib.chain_create_deferred, missing);
  find(m, "libra_d3d11_filter_chain_frame", lib.chain_frame, missing);
  find(m, "libra_d3d11_filter_chain_free", lib.chain_free, missing);
  find(m, "libra_error_write", lib.error_write, missing);
  find(m, "libra_error_free_string", lib.error_free_string, missing);
  find(m, "libra_error_free", lib.error_free, missing);
  if (!missing.empty()) {
    FreeLibrary(m);
    *why = where + " has no " + missing + " (a build without its Direct3D 11 runtime?)";
    return false;
  }
  lib.ok = true;
  lib.path = narrow(path);
  lib.abi = abi;
  lib.api = api;
  return true;
}

// Where librashader.dll may be, in order: next to the running program, then
// in the data folder's "librashader" folder.
std::vector<std::wstring> library_places() {
  return {join_path(dir_of(exe_path()), L"librashader.dll"),
          join_path(join_path(app_data_root(), L"librashader"), L"librashader.dll")};
}

// The library's state and its lock. Never freed (nor the DLL): a chain may
// still be building on its own thread as the process ends.
Library& library_state() {
  static Library* const l = new Library;
  return *l;
}

std::mutex& library_lock() {
  static std::mutex* const m = new std::mutex;
  return *m;
}

// Looks for the DLL and loads it. Once one was there (loaded or refused),
// that is final for the process, and `l` never changes again; a look that
// found none is made again on the next call (one put in place meanwhile).
void search(Library& l) {
  const std::vector<std::wstring> places = library_places();
  std::string failures;
  for (const std::wstring& p : places) {
    if (!file_exists(p)) continue;
    l.found = true;
    std::string why;
    if (load_from(p, l, &why)) break;
    failures += (failures.empty() ? "" : "; ") + why;
  }
  if (l.ok) {
    log_line("preset: librashader at %s (ABI %zu, API %zu)", l.path.c_str(), l.abi, l.api);
  } else if (!failures.empty()) {
    l.why = failures;
  } else {
    l.why = "no librashader.dll: looked for \"" + narrow(places[0]) + "\" and \"" + narrow(places[1]) + "\"";
  }
}

bool library_loaded(std::string* why) {
  std::lock_guard<std::mutex> lock(library_lock());
  Library& l = library_state();
  if (!l.ok && !l.found) search(l);
  if (!l.ok && why) *why = l.why;
  return l.ok;
}

// The library, for those who know it loaded (it no longer changes then).
const Library& library() {
  library_loaded(nullptr);
  return library_state();
}

// librashader's message for `err`, which this frees.
std::string take_error(const Library& lib, LibraError* err) {
  std::string text;
  char* s = nullptr;
  if (lib.error_write(err, &s) == 0 && s) {
    text = s;
    lib.error_free_string(&s);
  }
  lib.error_free(&err);
  // Some run over several lines: one, for the log. (A compiler's log quoted
  // inside keeps its escaped "\n": its paths' backslashes look the same.)
  std::string one;
  for (char c : text) {
    if (c == '\r' || c == '\n' || c == '\t') c = ' ';
    if (c == ' ' && (one.empty() || one.back() == ' ')) continue;
    one += c;
  }
  while (!one.empty() && one.back() == ' ') one.pop_back();
  return one.empty() ? std::string("librashader failed and gave no reason") : one;
}

void free_chain(const Library& lib, LibraChain& chain) {
  if (!chain) return;
  if (LibraError* e = lib.chain_free(&chain)) take_error(lib, e);
  chain = nullptr;
}

// A filter chain being built off the UI thread, shared by the ShaderPreset
// and the thread building it: either may let go first.
struct Build {
  enum class State { building, ready, failed };
  std::mutex mu;
  std::condition_variable over;
  State state = State::building;
  std::string error;                    // failed: why
  LibraChain chain = nullptr;           // ready, until the ShaderPreset takes it
  ID3D11CommandList* setup = nullptr;   // ready: what the build recorded (its textures' uploads)
  bool abandoned = false;               // the ShaderPreset let go: the builder frees what it made

  // What is still here, when nobody will take it.
  void drop(const Library& lib) {
    free_chain(lib, chain);
    if (setup) setup->Release();
    setup = nullptr;
  }
};

// One build at a time, process-wide: a second window's build of the same
// preset then finds librashader's shader cache warm, instead of compiling
// the same passes beside the first. Never destroyed, like the library.
std::mutex& build_lock() {
  static std::mutex* const m = new std::mutex;
  return *m;
}

// The build's own thread. `preset`, `device` and `deferred` are its to free.
void build_chain(std::shared_ptr<Build> b, LibraPreset preset, ID3D11Device* device, ID3D11DeviceContext* deferred,
                 LibraChainOptions options, std::string name) {
  const Library& lib = library();
  std::string error;
  LibraChain chain = nullptr;
  ID3D11CommandList* setup = nullptr;
  double ms = 0;
  {
    std::lock_guard<std::mutex> one_at_a_time(build_lock());
    const auto t0 = std::chrono::steady_clock::now();
    if (LibraError* e = lib.chain_create_deferred(&preset, device, deferred, &options, &chain)) {
      error = take_error(lib, e);
    } else if (HRESULT hr = deferred->FinishCommandList(FALSE, &setup); FAILED(hr)) {
      error = "the chain's set-up commands didn't record (" + hresult_text(hr) + ")";
    }
    ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  }
  deferred->Release();
  device->Release();
  if (error.empty()) {
    log_line("preset: %s built in %.0f ms", name.c_str(), ms);
  } else {
    log_line("preset: %s failed after %.0f ms: %s", name.c_str(), ms, error.c_str());
  }
  {
    std::lock_guard<std::mutex> lock(b->mu);
    b->chain = chain;
    b->setup = setup;
    if (!error.empty() || b->abandoned) b->drop(lib);
    b->error = error;
    b->state = error.empty() ? Build::State::ready : Build::State::failed;
  }
  b->over.notify_all();
}

bool is_relative(const std::wstring& p) {
  if (!p.empty() && (p[0] == L'\\' || p[0] == L'/')) return false;   // \\server\share, or \ of this drive
  return !(p.size() >= 3 && p[1] == L':' && (p[2] == L'\\' || p[2] == L'/'));
}

std::string level_text(D3D_FEATURE_LEVEL fl) {
  return std::to_string(((unsigned)fl >> 12) & 0xF) + "." + std::to_string(((unsigned)fl >> 8) & 0xF);
}

} // namespace

bool shader_preset_library(std::string* why) { return library_loaded(why); }

struct ShaderPreset::Impl {
  std::string name;                     // the preset's file name, UTF-8, for the log
  bool shader_cache = true;             // librashader's (set_shader_cache)
  ID3D11Device* device = nullptr;       // held from create() to reset()
  std::shared_ptr<Build> build;         // the build under way, or done
  LibraChain chain = nullptr;           // taken from the build when it was ready
  ID3D11CommandList* setup = nullptr;   // the build's recording, played before the first frame
  // The texture the chain draws into: the viewport's size, the target's format.
  ID3D11Texture2D* out = nullptr;
  ID3D11RenderTargetView* out_rtv = nullptr;
  int out_w = 0, out_h = 0;
  DXGI_FORMAT out_format = DXGI_FORMAT_UNKNOWN;

  // The chain, taken over from the build once it is ready.
  bool take() {
    if (chain) return true;
    if (!build) return false;
    std::lock_guard<std::mutex> lock(build->mu);
    if (build->state != Build::State::ready) return false;
    chain = build->chain;
    setup = build->setup;
    build->chain = nullptr;
    build->setup = nullptr;
    return chain != nullptr;
  }

  void release_out() {
    if (out_rtv) out_rtv->Release();
    if (out) out->Release();
    out_rtv = nullptr;
    out = nullptr;
    out_w = out_h = 0;
    out_format = DXGI_FORMAT_UNKNOWN;
  }

  bool ensure_out(int w, int h, DXGI_FORMAT format, std::string* why) {
    if (out && out_w == w && out_h == h && out_format == format) return true;
    release_out();
    D3D11_TEXTURE2D_DESC d{};
    d.Width = (UINT)w;
    d.Height = (UINT)h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET;
    HRESULT hr = device->CreateTexture2D(&d, nullptr, &out);
    if (SUCCEEDED(hr)) hr = device->CreateRenderTargetView(out, nullptr, &out_rtv);
    if (FAILED(hr)) {
      release_out();
      *why = "no " + std::to_string(w) + "x" + std::to_string(h) + " texture for the chain to draw into (" +
             hresult_text(hr) + ")";
      return false;
    }
    out_w = w;
    out_h = h;
    out_format = format;
    return true;
  }
};

ShaderPreset::ShaderPreset() : impl_(std::make_unique<Impl>()) {}
ShaderPreset::~ShaderPreset() { reset(); }

bool ShaderPreset::create(ID3D11Device* device, ID3D11DeviceContext* context, const std::wstring& path,
                          std::string* error) {
  reset();
  Impl& m = *impl_;
  auto fail = [&](std::string why) {
    if (error) *error = std::move(why);
    return false;
  };
  std::string why;
  if (!shader_preset_library(&why)) return fail(why);
  const Library& lib = library();
  if (!device || !context) return fail("no Direct3D device to build the shader preset on");
  if (path.empty()) return fail("no shader preset (ShaderPreset in settings.ini)");
  // A relative path is taken from the data folder (settings.ini's): a
  // screen saver's working folder is whatever started it, and the settings
  // window's Preview runs on a settings file of its own in %TEMP%.
  const std::wstring full = is_relative(path) ? join_path(app_data_root(), path) : path;
  m.name = narrow(full.substr(full.find_last_of(L"\\/") + 1));
  if (!file_exists(full)) return fail("the shader preset \"" + narrow(full) + "\" isn't there");
  if (const D3D_FEATURE_LEVEL fl = device->GetFeatureLevel(); fl < D3D_FEATURE_LEVEL_11_0) {
    return fail("librashader's Direct3D 11 shaders need feature level 11.0, and this device has " + level_text(fl));
  }
  // The preset alone: quick, so a missing or malformed one fails here. The
  // path goes as UTF-8, which librashader turns back into the wide one.
  LibraPreset preset = nullptr;
  if (LibraError* e = lib.preset_create(narrow(full).c_str(), &preset)) return fail(m.name + ": " + take_error(lib, e));
  m.device = device;
  m.device->AddRef();
  m.build = std::make_shared<Build>();
  LibraChainOptions options;
  options.disable_cache = !m.shader_cache;
  ID3D11DeviceContext* deferred = nullptr;
  if (SUCCEEDED(device->CreateDeferredContext(0, &deferred))) {
    device->AddRef();   // the build's own
    std::thread(build_chain, m.build, preset, device, deferred, options, m.name).detach();
    return true;
  }
  // No deferred contexts (a device made single-threaded): here and now, on
  // the immediate context.
  LibraChain chain = nullptr;
  if (LibraError* e = lib.chain_create(&preset, device, &options, &chain)) {
    const std::string text = take_error(lib, e);
    std::lock_guard<std::mutex> lock(m.build->mu);
    m.build->state = Build::State::failed;
    m.build->error = text;
    return fail(m.name + ": " + text);
  }
  std::lock_guard<std::mutex> lock(m.build->mu);
  m.build->chain = chain;
  m.build->state = Build::State::ready;
  return true;
}

void ShaderPreset::set_shader_cache(bool on) { impl_->shader_cache = on; }

bool ShaderPreset::ready() const {
  const Impl& m = *impl_;
  if (m.chain) return true;
  if (!m.build) return false;
  std::lock_guard<std::mutex> lock(m.build->mu);
  return m.build->state == Build::State::ready;
}

bool ShaderPreset::failed(std::string* error) const {
  const Impl& m = *impl_;
  if (!m.build) return false;
  std::lock_guard<std::mutex> lock(m.build->mu);
  if (m.build->state != Build::State::failed) return false;
  if (error) *error = m.name + ": " + m.build->error;
  return true;
}

bool ShaderPreset::wait(unsigned timeout_ms) {
  Impl& m = *impl_;
  if (m.chain) return true;
  if (!m.build) return false;
  std::unique_lock<std::mutex> lock(m.build->mu);
  m.build->over.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                         [&] { return m.build->state != Build::State::building; });
  return m.build->state == Build::State::ready;
}

bool ShaderPreset::draw(ID3D11DeviceContext* context, ID3D11ShaderResourceView* source, int, int,
                        ID3D11RenderTargetView* target, int target_w, int target_h, const RectI& viewport,
                        uint64_t frame_count, std::string* error) {
  Impl& m = *impl_;
  auto fail = [&](std::string why) {
    if (error) *error = std::move(why);
    return false;
  };
  if (!m.take()) {
    std::string why;
    if (failed(&why)) return fail(why);
    return fail(m.build ? m.name + " isn't built yet" : std::string("no shader preset"));
  }
  if (!context || !source || !target) return fail("nothing to draw from or into");
  const Library& lib = library();
  if (m.setup) {
    // TRUE: the context's state as it was, not cleared.
    context->ExecuteCommandList(m.setup, TRUE);
    m.setup->Release();
    m.setup = nullptr;
  }
  // The part of the viewport on the target.
  const int x0 = std::max(viewport.x, 0), y0 = std::max(viewport.y, 0);
  const int x1 = std::min(viewport.x + viewport.w, target_w), y1 = std::min(viewport.y + viewport.h, target_h);
  if (viewport.w <= 0 || viewport.h <= 0 || x1 <= x0 || y1 <= y0) return true;
  D3D11_RENDER_TARGET_VIEW_DESC rd{};
  target->GetDesc(&rd);
  if (rd.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D) return fail("the target isn't a 2D texture of one sample");
  std::string why;
  if (!m.ensure_out(viewport.w, viewport.h, rd.Format, &why)) return fail(why);
  // The chain draws into its own texture, all of it the viewport: the last
  // pass clears its whole output, and librashader sizes the passes (and a
  // preset's FinalViewportSize) from the output, as RetroArch does from the
  // picture's rectangle.
  const LibraViewport whole{0, 0, (uint32_t)viewport.w, (uint32_t)viewport.h};
  if (LibraError* e = lib.chain_frame(&m.chain, context, (size_t)frame_count, source, m.out_rtv, &whole, nullptr,
                                      nullptr)) {
    return fail(m.name + ": " + take_error(lib, e));
  }
  ID3D11Resource* dest = nullptr;
  target->GetResource(&dest);
  const D3D11_BOX box{(UINT)(x0 - viewport.x), (UINT)(y0 - viewport.y), 0, (UINT)(x1 - viewport.x),
                      (UINT)(y1 - viewport.y), 1};
  context->CopySubresourceRegion(dest, rd.Texture2D.MipSlice, (UINT)x0, (UINT)y0, 0, m.out, 0, &box);
  dest->Release();
  return true;
}

void ShaderPreset::reset() {
  Impl& m = *impl_;
  if (!m.build && !m.chain && !m.device) return;
  const Library& lib = library();
  if (m.build) {
    std::lock_guard<std::mutex> lock(m.build->mu);
    if (m.build->state == Build::State::building) {
      m.build->abandoned = true;
    } else {
      m.build->drop(lib);
    }
  }
  m.build.reset();
  free_chain(lib, m.chain);
  if (m.setup) m.setup->Release();
  m.setup = nullptr;
  m.release_out();
  if (m.device) m.device->Release();
  m.device = nullptr;
}

std::string ShaderPreset::describe() const {
  const std::string name = impl_->name.empty() ? std::string("shader preset") : impl_->name;
  if (!library_loaded(nullptr)) return name + ", without librashader";
  const Library& lib = library();
  return name + ", librashader ABI " + std::to_string(lib.abi) + " (API " + std::to_string(lib.api) + ")";
}

} // namespace adw::scr
