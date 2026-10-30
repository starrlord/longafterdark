#include "present.h"

#include <d2d1.h>
#include <d2d1_1.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

namespace adw::scr {

namespace {

template <typename T>
struct Com {
  T* p = nullptr;
  Com() = default;
  Com(const Com&) = delete;
  Com& operator=(const Com&) = delete;
  ~Com() { reset(); }
  void reset() {
    if (p) p->Release();
    p = nullptr;
  }
  T** out() {
    reset();
    return &p;
  }
  T* operator->() const { return p; }
  explicit operator bool() const { return p != nullptr; }
};

bool fail(std::string* error, const char* what, HRESULT hr) {
  if (error) {
    char buf[160];
    snprintf(buf, sizeof(buf), "%s (hr=0x%08lX)", what, (unsigned long)hr);
    *error = buf;
  }
  return false;
}

// One factory for the process, made on first use; it is multithreaded, so
// the off-screen renders (render_frame_bgr) may come from any thread.
ID2D1Factory* d2d_factory(HRESULT* hr_out = nullptr) {
  static std::once_flag once;
  static ID2D1Factory* factory = nullptr;
  static HRESULT hr = E_FAIL;
  std::call_once(once, [] {
    D2D1_FACTORY_OPTIONS options{};
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory), &options,
                           reinterpret_cast<void**>(&factory));
  });
  if (hr_out) *hr_out = hr;
  return SUCCEEDED(hr) ? factory : nullptr;
}

// The frame as a bitmap on `rt`: 32-bit, the alpha byte ignored.
bool make_bitmap(ID2D1RenderTarget* rt, const Frame& f, std::vector<uint32_t>& scratch, Com<ID2D1Bitmap>& bmp,
                 SizeI& bmp_size, std::string* error) {
  const bool same = bmp && bmp_size.w == f.width && bmp_size.h == f.height;
  if (!same) {
    D2D1_BITMAP_PROPERTIES props{};
    props.pixelFormat = {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE};
    props.dpiX = props.dpiY = 96.0f;
    HRESULT hr = rt->CreateBitmap(D2D1_SIZE_U{(UINT32)f.width, (UINT32)f.height}, nullptr, 0, &props, bmp.out());
    if (FAILED(hr)) return fail(error, "CreateBitmap", hr);
    bmp_size = {f.width, f.height};
  }
  HRESULT hr;
  if (f.bpp == 32) {
    hr = bmp->CopyFromMemory(nullptr, f.bits.data(), (UINT32)f.stride);
  } else {
    frame_to_bgrx(f, scratch);
    hr = bmp->CopyFromMemory(nullptr, scratch.data(), (UINT32)f.width * 4);
  }
  if (FAILED(hr)) return fail(error, "CopyFromMemory", hr);
  return true;
}

// How many times each frame pixel is repeated before the final, filtered
// step of an upscale (the smooth filter): the scale rounded up, so the last
// step shrinks a little and only blends where two pixels meet. 1: no
// intermediate (1:1, a downscale, or nearest).
int prescale_factor(const Frame& f, const RectI& fit, Filter filter) {
  if (filter == Filter::nearest || f.width <= 0 || f.height <= 0) return 1;
  const double scale = std::max((double)fit.w / f.width, (double)fit.h / f.height);
  int k = (int)std::ceil(scale - 1e-6);
  // Keep the intermediate within any GPU's texture limit.
  while (k > 1 && (f.width * k > 8192 || f.height * k > 8192)) --k;
  return std::max(1, k);
}

// Black, and the frame scaled into `fit`: the same drawing on screen and off.
// The smooth filter is GDI HALFTONE's look for an upscale, done on the GPU:
// every pixel stays a crisp, evenly sized block, and only where two blocks
// meet does a column or row of pixels blend them, by how much of each it
// covers (the frame repeated k times, nearest neighbour, into `mid`, then
// drawn to the window linearly, a shrink of less than one pixel in k). A
// downscale is filtered with high-quality cubic (linear without
// ID2D1DeviceContext).
bool draw(ID2D1RenderTarget* rt, ID2D1DeviceContext* dc, ID2D1Bitmap* bmp, const Frame& f, const RectI& fit,
          Filter filter, Com<ID2D1BitmapRenderTarget>& mid, SizeI& mid_size, std::string* error) {
  const D2D1_COLOR_F black{0.0f, 0.0f, 0.0f, 1.0f};
  const D2D1_RECT_F dst{(float)fit.x, (float)fit.y, (float)(fit.x + fit.w), (float)(fit.y + fit.h)};
  const int k = prescale_factor(f, fit, filter);
  ID2D1Bitmap* src = bmp;
  Com<ID2D1Bitmap> big;
  if (k > 1) {
    const SizeI want{f.width * k, f.height * k};
    if (!mid || !(mid_size == want)) {
      const D2D1_SIZE_F dips{(float)want.w, (float)want.h};
      const D2D1_SIZE_U px{(UINT32)want.w, (UINT32)want.h};
      const D2D1_PIXEL_FORMAT fmt{DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED};
      HRESULT hr = rt->CreateCompatibleRenderTarget(&dips, &px, &fmt, D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS_NONE,
                                                    mid.out());
      if (FAILED(hr)) {
        mid_size = {};
        return fail(error, "CreateCompatibleRenderTarget", hr);
      }
      mid_size = want;
    }
    mid->BeginDraw();
    const D2D1_RECT_F all{0.0f, 0.0f, (float)want.w, (float)want.h};
    mid->DrawBitmap(bmp, &all, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR, nullptr);
    HRESULT hr = mid->EndDraw();
    if (SUCCEEDED(hr)) hr = mid->GetBitmap(big.out());
    if (FAILED(hr)) {
      mid.reset();
      mid_size = {};
      return fail(error, "prescale", hr);
    }
    src = big.p;
  }
  rt->Clear(&black);
  if (k > 1) {
    rt->DrawBitmap(src, &dst, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, nullptr);
  } else if (filter == Filter::nearest || (fit.w == f.width && fit.h == f.height)) {
    rt->DrawBitmap(src, &dst, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR, nullptr);
  } else if (dc) {
    dc->DrawBitmap(src, &dst, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
  } else {
    rt->DrawBitmap(src, &dst, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, nullptr);
  }
  return true;
}

struct Bitmapinfo256 {
  BITMAPINFOHEADER h;
  RGBQUAD colors[256];
};

} // namespace

// ---- conversion and GDI -------------------------------------------------------------

void frame_to_bgrx(const Frame& f, std::vector<uint32_t>& out) {
  out.resize((size_t)std::max(0, f.width) * std::max(0, f.height));
  if (f.width <= 0 || f.height <= 0) return;
  if (f.bpp == 32) {
    for (int y = 0; y < f.height; ++y) memcpy(out.data() + (size_t)y * f.width, f.bits.data() + (size_t)y * f.stride, (size_t)f.width * 4);
    return;
  }
  uint32_t lut[256];
  for (int i = 0; i < 256; ++i) {
    const RGBQUAD& q = f.palette[i];
    lut[i] = (uint32_t)q.rgbBlue | ((uint32_t)q.rgbGreen << 8) | ((uint32_t)q.rgbRed << 16) | 0xFF000000u;
  }
  for (int y = 0; y < f.height; ++y) {
    const uint8_t* s = f.bits.data() + (size_t)y * f.stride;
    uint32_t* d = out.data() + (size_t)y * f.width;
    for (int x = 0; x < f.width; ++x) d[x] = lut[s[x]];
  }
}

void crop_frame(const Frame& src, const RectI& sub, Frame& dst) {
  int x0 = std::clamp(sub.x, 0, std::max(0, src.width - 1));
  int y0 = std::clamp(sub.y, 0, std::max(0, src.height - 1));
  int w = std::clamp(sub.w, 1, std::max(1, src.width - x0));
  int h = std::clamp(sub.h, 1, std::max(1, src.height - y0));
  dst.width = w;
  dst.height = h;
  dst.bpp = src.bpp;
  if (src.bpp == 8) {
    dst.stride = (w + 3) & ~3;
    dst.palette = src.palette;
    dst.bits.assign((size_t)dst.stride * h, 0);
    for (int y = 0; y < h; ++y) {
      memcpy(dst.bits.data() + (size_t)y * dst.stride,
             src.bits.data() + (size_t)(y0 + y) * src.stride + x0,
             (size_t)w);
    }
  } else {
    dst.stride = w * 4;
    dst.bits.resize((size_t)dst.stride * h);
    for (int y = 0; y < h; ++y) {
      memcpy(dst.bits.data() + (size_t)y * dst.stride,
             src.bits.data() + (size_t)(y0 + y) * src.stride + (size_t)x0 * 4,
             (size_t)w * 4);
    }
  }
}

void stretch_frame_gdi(HDC dc, const Frame& f, const RectI& fit, Filter filter) {
  Bitmapinfo256 bmi{};
  bmi.h.biSize = sizeof(BITMAPINFOHEADER);
  bmi.h.biWidth = f.width;
  bmi.h.biHeight = -f.height;     // top-down, as the stream sends rows
  bmi.h.biPlanes = 1;
  bmi.h.biBitCount = (WORD)f.bpp;
  bmi.h.biCompression = BI_RGB;
  if (f.bpp == 8) {
    bmi.h.biClrUsed = 256;
    std::copy(f.palette.begin(), f.palette.end(), bmi.colors);
  }
  const bool halftone = filter == Filter::smooth;
  SetStretchBltMode(dc, halftone ? HALFTONE : COLORONCOLOR);
  if (halftone) SetBrushOrgEx(dc, 0, 0, nullptr);   // required after selecting HALFTONE
  StretchDIBits(dc, fit.x, fit.y, fit.w, fit.h, 0, 0, f.width, f.height, f.bits.data(),
                reinterpret_cast<const BITMAPINFO*>(&bmi), DIB_RGB_COLORS, SRCCOPY);
}

// ---- D2DPresenter ----------------------------------------------------------------------

struct D2DPresenter::Impl {
  Com<ID2D1HwndRenderTarget> rt;
  Com<ID2D1DeviceContext> dc;          // high-quality cubic downscales; absent: linear
  Com<ID2D1Bitmap> bmp;
  Com<ID2D1BitmapRenderTarget> mid;    // the smooth upscale's prescaled frame
  HWND hwnd = nullptr;
  SizeI size{}, bmp_size{}, mid_size{};
  std::vector<uint32_t> scratch;
  const char* smooth = "";
};

D2DPresenter::D2DPresenter() : impl_(std::make_unique<Impl>()) {}
D2DPresenter::~D2DPresenter() = default;

void D2DPresenter::release() {
  impl_->mid.reset();
  impl_->bmp.reset();
  impl_->dc.reset();
  impl_->rt.reset();
  impl_->bmp_size = {};
  impl_->mid_size = {};
}

const char* D2DPresenter::smooth_name() const { return impl_->smooth; }

bool D2DPresenter::ready() const { return impl_->rt.p != nullptr; }

bool D2DPresenter::clear(HWND hwnd) {
  Impl& d = *impl_;
  if (!d.rt || d.hwnd != hwnd) return false;
  RECT cr{};
  GetClientRect(hwnd, &cr);
  const SizeI size{std::max(1, (int)cr.right), std::max(1, (int)cr.bottom)};
  if (!(d.size == size)) {
    const D2D1_SIZE_U px{(UINT32)size.w, (UINT32)size.h};
    if (FAILED(d.rt->Resize(&px))) {
      release();
      return false;
    }
    d.size = size;
  }
  const D2D1_COLOR_F black{0.0f, 0.0f, 0.0f, 1.0f};
  d.rt->BeginDraw();
  d.rt->Clear(&black);
  if (FAILED(d.rt->EndDraw())) {
    release();
    return false;
  }
  return true;
}

bool D2DPresenter::present(HWND hwnd, const Frame& f, const RectI& fit, Filter filter, std::string* error,
                           bool* device_lost) {
  if (device_lost) *device_lost = false;
  Impl& d = *impl_;
  if (f.width <= 0 || f.height <= 0 || f.bits.empty()) return fail(error, "empty frame", E_INVALIDARG);
  RECT cr{};
  GetClientRect(hwnd, &cr);
  const SizeI size{std::max(1, (int)cr.right), std::max(1, (int)cr.bottom)};
  if (d.rt && d.hwnd != hwnd) release();
  HRESULT hr = S_OK;
  if (!d.rt) {
    ID2D1Factory* factory = d2d_factory(&hr);
    if (!factory) return fail(error, "D2D1CreateFactory", hr);
    D2D1_RENDER_TARGET_PROPERTIES rp{};
    rp.type = D2D1_RENDER_TARGET_TYPE_DEFAULT;   // the GPU, else Direct2D's own software rasterizer
    rp.pixelFormat = {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE};
    rp.dpiX = rp.dpiY = 96.0f;                   // one DIP = one pixel: fit is in pixels
    D2D1_HWND_RENDER_TARGET_PROPERTIES hp{};
    hp.hwnd = hwnd;
    hp.pixelSize = {(UINT32)size.w, (UINT32)size.h};
    // Never wait for the vertical blank here: the pacer already paces on
    // DwmFlush, and with a window per monitor each would wait in turn.
    hp.presentOptions = D2D1_PRESENT_OPTIONS_IMMEDIATELY;
    hr = factory->CreateHwndRenderTarget(&rp, &hp, d.rt.out());
    if (FAILED(hr)) return fail(error, "CreateHwndRenderTarget", hr);
    d.hwnd = hwnd;
    d.size = size;
    if (FAILED(d.rt->QueryInterface(__uuidof(ID2D1DeviceContext), reinterpret_cast<void**>(d.dc.out())))) d.dc.reset();
    d.smooth = d.dc ? "sharp upscale, high-quality cubic downscale" : "sharp upscale, linear downscale";
  } else if (!(d.size == size)) {
    const D2D1_SIZE_U px{(UINT32)size.w, (UINT32)size.h};
    hr = d.rt->Resize(&px);
    if (FAILED(hr)) {
      release();
      return fail(error, "Resize", hr);
    }
    d.size = size;
  }
  if (!make_bitmap(d.rt.p, f, d.scratch, d.bmp, d.bmp_size, error)) {
    release();
    return false;
  }
  d.rt->BeginDraw();
  std::string why;
  const bool drawn = draw(d.rt.p, d.dc.p, d.bmp.p, f, fit, filter, d.mid, d.mid_size, &why);
  hr = d.rt->EndDraw();
  // A lost device fails the prescale too: the target's EndDraw says which it was.
  if (hr == (HRESULT)D2DERR_RECREATE_TARGET) {
    release();
    if (device_lost) *device_lost = true;
    return fail(error, "device lost (D2DERR_RECREATE_TARGET)", hr);
  }
  if (FAILED(hr)) {
    release();
    return fail(error, "EndDraw", hr);
  }
  if (!drawn) {
    release();
    if (error) *error = why;
    return false;
  }
  return true;
}

// ---- off screen ------------------------------------------------------------------------

bool render_frame_bgr(const Frame& f, int w, int h, const RectI& fit, bool d2d, Filter filter,
                      std::vector<uint8_t>& bgr, std::string* error) {
  if (w <= 0 || h <= 0 || f.width <= 0 || f.height <= 0) return fail(error, "empty picture", E_INVALIDARG);
  bgr.assign((size_t)w * h * 3, 0);
  if (!d2d) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!dib || !mem || !bits) {
      if (dib) DeleteObject(dib);
      if (mem) DeleteDC(mem);
      return fail(error, "CreateDIBSection", E_OUTOFMEMORY);
    }
    HGDIOBJ old = SelectObject(mem, dib);
    RECT all{0, 0, w, h};
    FillRect(mem, &all, (HBRUSH)GetStockObject(BLACK_BRUSH));
    stretch_frame_gdi(mem, f, fit, filter);
    GdiFlush();
    const uint8_t* s = static_cast<const uint8_t*>(bits);
    for (size_t i = 0, n = (size_t)w * h; i < n; ++i) memcpy(&bgr[i * 3], s + i * 4, 3);
    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    return true;
  }

  HRESULT hr = S_OK;
  ID2D1Factory* factory = d2d_factory(&hr);
  if (!factory) return fail(error, "D2D1CreateFactory", hr);
  const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  bool ok = false;
  {
    Com<IWICImagingFactory> wic;
    Com<IWICBitmap> target;
    Com<ID2D1RenderTarget> rt;
    Com<ID2D1DeviceContext> dc;
    Com<ID2D1Bitmap> bmp;
    Com<ID2D1BitmapRenderTarget> mid;
    Com<IWICBitmapLock> lock;
    SizeI bmp_size{}, mid_size{};
    bool drawn = false;
    std::vector<uint32_t> scratch;
    UINT stride = 0, cb = 0;
    BYTE* px = nullptr;
    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.out()));
    if (FAILED(hr)) { fail(error, "WIC factory", hr); goto done; }
    hr = wic->CreateBitmap((UINT)w, (UINT)h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, target.out());
    if (FAILED(hr)) { fail(error, "WIC CreateBitmap", hr); goto done; }
    {
      D2D1_RENDER_TARGET_PROPERTIES rp{};
      rp.type = D2D1_RENDER_TARGET_TYPE_SOFTWARE;
      rp.pixelFormat = {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED};
      rp.dpiX = rp.dpiY = 96.0f;
      hr = factory->CreateWicBitmapRenderTarget(target.p, &rp, rt.out());
      if (FAILED(hr)) { fail(error, "CreateWicBitmapRenderTarget", hr); goto done; }
    }
    if (FAILED(rt->QueryInterface(__uuidof(ID2D1DeviceContext), reinterpret_cast<void**>(dc.out())))) dc.reset();
    if (!make_bitmap(rt.p, f, scratch, bmp, bmp_size, error)) goto done;
    rt->BeginDraw();
    drawn = draw(rt.p, dc.p, bmp.p, f, fit, filter, mid, mid_size, error);
    hr = rt->EndDraw();
    if (!drawn) goto done;
    if (FAILED(hr)) { fail(error, "EndDraw", hr); goto done; }
    {
      WICRect all{0, 0, w, h};
      hr = target->Lock(&all, WICBitmapLockRead, lock.out());
      if (FAILED(hr)) { fail(error, "WIC Lock", hr); goto done; }
      if (FAILED(hr = lock->GetStride(&stride)) || FAILED(hr = lock->GetDataPointer(&cb, &px)) || !px) {
        fail(error, "WIC data", hr);
        goto done;
      }
      for (int y = 0; y < h; ++y) {
        const BYTE* s = px + (size_t)y * stride;
        uint8_t* o = bgr.data() + (size_t)y * w * 3;
        for (int x = 0; x < w; ++x) memcpy(o + x * 3, s + x * 4, 3);   // opaque: premultiplied = straight
      }
    }
    ok = true;
  }
done:
  if (SUCCEEDED(co)) CoUninitialize();
  return ok;
}

} // namespace adw::scr
