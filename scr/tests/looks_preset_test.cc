// scr_unit looks: shader presets (librashader) (looks_test.h, shader_preset.h).
// librashader isn't shipped, so by default the checks are that it isn't
// loaded and every call fails cleanly, saying where the DLL was looked for.
// With AD_LOOKS_TEST_LIBRASHADER_DIR naming a folder that holds a
// librashader.dll (read by this test only, never by the saver), the test
// puts that DLL where the loader looks (a data folder of its own) and draws
// for real: a preset of our own (one pass inverting the colours) from a
// folder with a non-ASCII name, presets that fail in each way, and the
// first .slangp in that folder, if it has one. librashader's shader cache
// is off for them all: the test writes nothing into the user's profile.
#include "looks_test.h"

#include <d3d11.h>

#include <algorithm>
#include <cstdlib>

#include "paths.h"
#include "shader_preset.h"

namespace adw::scr::looks_test {

namespace {

// One pass: the colours inverted, sampled nearest, drawn straight into the
// viewport.
constexpr char kInvertSlang[] = R"(#version 450
layout(std140, set = 0, binding = 0) uniform UBO {
  mat4 MVP;
} global;

#pragma stage vertex
layout(location = 0) in vec4 Position;
layout(location = 1) in vec2 TexCoord;
layout(location = 0) out vec2 vTexCoord;
void main() {
  gl_Position = global.MVP * Position;
  vTexCoord = TexCoord;
}

#pragma stage fragment
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;
layout(set = 0, binding = 2) uniform sampler2D Source;
void main() {
  FragColor = vec4(vec3(1.0) - texture(Source, vTexCoord).rgb, 1.0);
}
)";
constexpr char kInvertSlangp[] = "shaders = 1\nshader0 = invert.slang\nfilter_linear0 = false\nscale_type0 = viewport\n";
// A pass that doesn't compile, and a preset naming a pass that isn't there.
constexpr char kBrokenSlangp[] = "shaders = 1\nshader0 = broken.slang\n";
constexpr char kMissingPassSlangp[] = "shaders = 1\nshader0 = nowhere.slang\n";

// The source frame's colour at (x, y).
void source_rgb(int x, int y, uint8_t rgb[3]) {
  rgb[0] = (uint8_t)(x * 16 + 7);
  rgb[1] = (uint8_t)(y * 20 + 3);
  rgb[2] = (uint8_t)((x * 5 + y * 11) * 3);
}

struct Gpu {
  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* context = nullptr;
  ID3D11ShaderResourceView* source = nullptr;
  ID3D11Texture2D* target = nullptr;
  ID3D11RenderTargetView* rtv = nullptr;
  ID3D11Texture2D* staging = nullptr;
  int sw = 0, sh = 0, tw = 0, th = 0;
  ~Gpu() {
    for (IUnknown* u : std::initializer_list<IUnknown*>{staging, rtv, target, source, context, device}) {
      if (u) u->Release();
    }
  }
};

// A device of feature level 11.0 (the hardware's, else WARP), an sw x sh
// source (RGBA8, as the presenter resolves a frame) and a tw x th BGRA8
// target. d3d11.dll from System32: scr_unit doesn't link it.
bool make_gpu(Gpu& g, int sw, int sh, int tw, int th) {
  HMODULE d3d11 = LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  auto create = d3d11 ? reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(
                            reinterpret_cast<void*>(GetProcAddress(d3d11, "D3D11CreateDevice")))
                      : nullptr;
  if (!create) return false;
  const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
  for (D3D_DRIVER_TYPE type : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
    if (SUCCEEDED(create(nullptr, type, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &g.device, nullptr, &g.context))) break;
  }
  if (!g.device) return false;
  g.sw = sw;
  g.sh = sh;
  g.tw = tw;
  g.th = th;
  std::vector<uint8_t> rgba((size_t)sw * sh * 4);
  for (int y = 0; y < sh; ++y) {
    for (int x = 0; x < sw; ++x) {
      uint8_t* p = &rgba[((size_t)y * sw + x) * 4];
      source_rgb(x, y, p);
      p[3] = 255;
    }
  }
  D3D11_TEXTURE2D_DESC d{};
  d.Width = (UINT)sw;
  d.Height = (UINT)sh;
  d.MipLevels = 1;
  d.ArraySize = 1;
  d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  d.SampleDesc.Count = 1;
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  const D3D11_SUBRESOURCE_DATA init{rgba.data(), (UINT)sw * 4, 0};
  ID3D11Texture2D* tex = nullptr;
  if (FAILED(g.device->CreateTexture2D(&d, &init, &tex))) return false;
  const HRESULT hr = g.device->CreateShaderResourceView(tex, nullptr, &g.source);
  tex->Release();
  if (FAILED(hr)) return false;
  d.Width = (UINT)tw;
  d.Height = (UINT)th;
  d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  d.BindFlags = D3D11_BIND_RENDER_TARGET;
  if (FAILED(g.device->CreateTexture2D(&d, nullptr, &g.target))) return false;
  if (FAILED(g.device->CreateRenderTargetView(g.target, nullptr, &g.rtv))) return false;
  d.Usage = D3D11_USAGE_STAGING;
  d.BindFlags = 0;
  d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  return SUCCEEDED(g.device->CreateTexture2D(&d, nullptr, &g.staging));
}

// The target filled with the marker colour: what must stay outside the viewport.
constexpr uint8_t kMarker[3] = {0, 200, 0};

void fill_marker(Gpu& g) {
  const float c[4] = {kMarker[0] / 255.f, kMarker[1] / 255.f, kMarker[2] / 255.f, 1};
  g.context->ClearRenderTargetView(g.rtv, c);
}

// The target as RGB rows.
std::vector<uint8_t> read_target(Gpu& g) {
  std::vector<uint8_t> rgb((size_t)g.tw * g.th * 3);
  g.context->CopyResource(g.staging, g.target);
  D3D11_MAPPED_SUBRESOURCE map{};
  if (FAILED(g.context->Map(g.staging, 0, D3D11_MAP_READ, 0, &map))) return {};
  for (int y = 0; y < g.th; ++y) {
    const uint8_t* row = (const uint8_t*)map.pData + (size_t)y * map.RowPitch;
    for (int x = 0; x < g.tw; ++x) {
      uint8_t* d = &rgb[((size_t)y * g.tw + x) * 3];
      d[0] = row[x * 4 + 2];
      d[1] = row[x * 4 + 1];
      d[2] = row[x * 4];
    }
  }
  g.context->Unmap(g.staging, 0);
  return rgb;
}

bool is_marker(const std::vector<uint8_t>& rgb, int w, int x, int y) {
  const uint8_t* p = &rgb[((size_t)y * w + x) * 3];
  return p[0] == kMarker[0] && p[1] == kMarker[1] && p[2] == kMarker[2];
}

// Every pixel outside `r` (clipped to the target) still the marker.
bool marker_outside(const std::vector<uint8_t>& rgb, const Gpu& g, const RectI& r) {
  for (int y = 0; y < g.th; ++y) {
    for (int x = 0; x < g.tw; ++x) {
      const bool inside = x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
      if (!inside && !is_marker(rgb, g.tw, x, y)) return false;
    }
  }
  return true;
}

// Within `v` (the source drawn twice its size), source pixel (sx, sy)
// inverted, for every one whose 2x2 block is on the target.
bool inverted_inside(const std::vector<uint8_t>& rgb, const Gpu& g, const RectI& v) {
  for (int sy = 0; sy < g.sh; ++sy) {
    for (int sx = 0; sx < g.sw; ++sx) {
      for (int k = 0; k < 4; ++k) {
        const int x = v.x + sx * 2 + (k & 1), y = v.y + sy * 2 + (k >> 1);
        if (x < 0 || y < 0 || x >= g.tw || y >= g.th) continue;
        uint8_t want[3];
        source_rgb(sx, sy, want);
        const uint8_t* p = &rgb[((size_t)y * g.tw + x) * 3];
        for (int c = 0; c < 3; ++c) {
          if (std::abs((int)p[c] - (255 - (int)want[c])) > 1) return false;
        }
      }
    }
  }
  return true;
}

bool copy_dll(const std::wstring& from, const std::wstring& to) {
  if (!ensure_dir(dir_of(to))) return false;
  if (CopyFileW(from.c_str(), to.c_str(), FALSE)) return true;
  // Still loaded by another run of the suite: the same file will do.
  WIN32_FILE_ATTRIBUTE_DATA a{}, b{};
  return GetFileAttributesExW(from.c_str(), GetFileExInfoStandard, &a) &&
         GetFileAttributesExW(to.c_str(), GetFileExInfoStandard, &b) && a.nFileSizeLow == b.nFileSizeLow &&
         a.nFileSizeHigh == b.nFileSizeHigh;
}

std::wstring first_slangp(const std::wstring& dir) {
  WIN32_FIND_DATAW fd{};
  HANDLE h = FindFirstFileW(join_path(dir, L"*.slangp").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return {};
  std::wstring best = fd.cFileName;
  while (FindNextFileW(h, &fd)) best = std::min(best, std::wstring(fd.cFileName));
  FindClose(h);
  return join_path(dir, best);
}

// No librashader where the loader looks: nothing loads, and every call
// fails cleanly, saying where it looked.
void checks_without() {
  std::string why;
  if (shader_preset_library(&why)) {
    printf("looks: a librashader.dll is installed where the saver looks: the checks without it are skipped\n");
    return;
  }
  LCHECK(why.find("librashader.dll") != std::string::npos);
  LCHECK(why.find(narrow(dir_of(exe_path()))) != std::string::npos);
  LCHECK(why.find(narrow(join_path(app_data_root(), L"librashader"))) != std::string::npos);
  ShaderPreset p;
  std::string error;
  LCHECK(!p.create(nullptr, nullptr, L"C:\\nowhere\\crt.slangp", &error) && error == why);
  LCHECK(!p.ready() && !p.failed() && !p.wait(0));
  error.clear();
  LCHECK(!p.draw(nullptr, nullptr, 0, 0, nullptr, 0, 0, RectI{0, 0, 1, 1}, 0, &error) && !error.empty());
  LCHECK(p.describe().find("without librashader") != std::string::npos);
  p.reset();
}

// librashader loaded from `dir`'s copy: our own presets drawn for real.
void checks_with(const std::wstring& dir) {
  std::string why;
  if (!shader_preset_library(&why)) {
    fprintf(stderr, "looks: librashader from %s didn't load: %s\n", narrow(dir).c_str(), why.c_str());
    LCHECK(!"librashader loads");
    return;
  }
  Gpu g;
  if (!make_gpu(g, 16, 12, 64, 48)) {
    printf("looks: no Direct3D 11 device of feature level 11.0: the shader preset checks are skipped\n");
    return;
  }
  // Our presets, in a folder with a non-ASCII name, in the data folder.
  const std::wstring folder = L"Pr\u00e8sets \u2713 \u8272";
  const std::wstring presets = join_path(app_data_root(), folder);
  LCHECK(write_file_atomic(join_path(presets, L"invert.slang"), kInvertSlang));
  LCHECK(write_file_atomic(join_path(presets, L"invert.slangp"), kInvertSlangp));
  std::string broken = kInvertSlang;
  broken.replace(broken.rfind("1.0);"), 5, "1.0) +;");
  LCHECK(write_file_atomic(join_path(presets, L"broken.slang"), broken));
  LCHECK(write_file_atomic(join_path(presets, L"broken.slangp"), kBrokenSlangp));
  LCHECK(write_file_atomic(join_path(presets, L"missing-pass.slangp"), kMissingPassSlangp));

  std::string error;
  {
    // A preset that isn't there; one naming a pass that isn't there: create() says so.
    ShaderPreset p;
    p.set_shader_cache(false);
    LCHECK(!p.create(g.device, g.context, join_path(presets, L"nothing.slangp"), &error));
    LCHECK(error.find("isn't there") != std::string::npos && error.find("nothing.slangp") != std::string::npos);
    error.clear();
    LCHECK(!p.create(g.device, g.context, join_path(presets, L"missing-pass.slangp"), &error));
    LCHECK(error.find("missing-pass.slangp") != std::string::npos && error.find("nowhere.slang") != std::string::npos);
    LCHECK(!p.ready() && !p.failed());
  }
  {
    // A pass that doesn't compile: the build fails, and says so.
    ShaderPreset p;
    p.set_shader_cache(false);
    LCHECK(p.create(g.device, g.context, join_path(presets, L"broken.slangp"), &error));
    LCHECK(!p.wait(60000) && !p.ready());
    error.clear();
    LCHECK(p.failed(&error) && error.find("broken.slangp") != std::string::npos);
    LCHECK(!p.draw(g.context, g.source, g.sw, g.sh, g.rtv, g.tw, g.th, RectI{0, 0, 32, 24}, 0, &error));
  }
  {
    // Ours: a relative path is the data folder's (settings.ini's folder).
    ShaderPreset p;
    p.set_shader_cache(false);
    LCHECK(p.create(g.device, g.context, join_path(folder, L"invert.slangp"), &error));
    LCHECK(p.wait(60000) && p.ready() && !p.failed());
    LCHECK(p.describe().find("invert.slangp, librashader") != std::string::npos);
    // Twice the source's size, off the target's corner: the rest of the
    // target stays as it was.
    const RectI v{10, 7, g.sw * 2, g.sh * 2};
    fill_marker(g);
    error.clear();
    LCHECK(p.draw(g.context, g.source, g.sw, g.sh, g.rtv, g.tw, g.th, v, 0, &error) && error.empty());
    std::vector<uint8_t> rgb = read_target(g);
    LCHECK(!rgb.empty() && marker_outside(rgb, g, v) && inverted_inside(rgb, g, v));
    // Partly off the target: what is on it is drawn, nothing else.
    const RectI off{g.tw - g.sw, g.th - g.sh, g.sw * 2, g.sh * 2};
    fill_marker(g);
    LCHECK(p.draw(g.context, g.source, g.sw, g.sh, g.rtv, g.tw, g.th, off, 1, &error));
    rgb = read_target(g);
    LCHECK(!rgb.empty() && marker_outside(rgb, g, off) && inverted_inside(rgb, g, off));
    // reset() (a lost device): nothing to draw with, until created again.
    p.reset();
    LCHECK(!p.ready() && !p.draw(g.context, g.source, g.sw, g.sh, g.rtv, g.tw, g.th, v, 2, &error));
    LCHECK(p.create(g.device, g.context, join_path(presets, L"invert.slangp"), &error) && p.wait(60000));
    fill_marker(g);
    LCHECK(p.draw(g.context, g.source, g.sw, g.sh, g.rtv, g.tw, g.th, v, 3, &error));
    rgb = read_target(g);
    LCHECK(!rgb.empty() && marker_outside(rgb, g, v) && inverted_inside(rgb, g, v));
  }
  {
    // Dropped while it builds: no wait, no leak of the build's work.
    ShaderPreset p;
    p.set_shader_cache(false);
    LCHECK(p.create(g.device, g.context, join_path(presets, L"invert.slangp"), &error));
    p.reset();
    LCHECK(!p.ready() && !p.failed());
  }
  // The folder's own preset, if it has one (crt-lottes, say): built and
  // drawn into the middle of the target, the rest untouched.
  const std::wstring theirs = first_slangp(dir);
  if (theirs.empty()) {
    printf("looks: no .slangp in %s: only our own presets drawn\n", narrow(dir).c_str());
    return;
  }
  ShaderPreset p;
  p.set_shader_cache(false);
  error.clear();
  LCHECK(p.create(g.device, g.context, theirs, &error) && error.empty());
  if (!p.wait(120000)) {
    p.failed(&error);
    fprintf(stderr, "looks: %s: %s\n", narrow(theirs).c_str(), error.c_str());
    LCHECK(!"the folder's preset builds");
    return;
  }
  const RectI v{8, 6, 48, 36};
  fill_marker(g);
  LCHECK(p.draw(g.context, g.source, g.sw, g.sh, g.rtv, g.tw, g.th, v, 0, &error));
  const std::vector<uint8_t> rgb = read_target(g);
  LCHECK(!rgb.empty() && marker_outside(rgb, g, v));
  printf("looks: drew %s (%s)\n", narrow(theirs).c_str(), p.describe().c_str());
}

} // namespace

void preset_checks() {
  // The loader looks next to scr_unit.exe and in the data folder: a data
  // folder of the test's own, so a user's is never looked at, and (with the
  // variable) where the test puts the DLL.
  const std::wstring dir = env_w(L"AD_LOOKS_TEST_LIBRASHADER_DIR");
  wchar_t tmp[MAX_PATH + 1] = {};
  GetTempPathW(MAX_PATH, tmp);
  const std::wstring scratch = join_path(tmp, L"adw-scr-looks-preset");
  const std::wstring saved_base = env_w(L"AD_LOCALAPPDATA"), saved_settings = env_w(L"AD_SETTINGS");
  const bool had_base = env_set(L"AD_LOCALAPPDATA"), had_settings = env_set(L"AD_SETTINGS");
  SetEnvironmentVariableW(L"AD_SETTINGS", nullptr);
  if (dir.empty()) {
    SetEnvironmentVariableW(L"AD_LOCALAPPDATA", join_path(scratch, L"none").c_str());
    checks_without();
  } else {
    SetEnvironmentVariableW(L"AD_LOCALAPPDATA", join_path(scratch, L"with").c_str());
    const std::wstring dll = join_path(join_path(app_data_root(), L"librashader"), L"librashader.dll");
    if (copy_dll(join_path(dir, L"librashader.dll"), dll)) {
      checks_with(dir);
    } else {
      fprintf(stderr, "looks: no librashader.dll copied from %s\n", narrow(dir).c_str());
      LCHECK(!"AD_LOOKS_TEST_LIBRASHADER_DIR holds a librashader.dll");
    }
  }
  SetEnvironmentVariableW(L"AD_LOCALAPPDATA", had_base ? saved_base.c_str() : nullptr);
  SetEnvironmentVariableW(L"AD_SETTINGS", had_settings ? saved_settings.c_str() : nullptr);
}

} // namespace adw::scr::looks_test
