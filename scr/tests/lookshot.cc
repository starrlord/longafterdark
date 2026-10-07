// scr_lookshot (lookshot.exe): a picture drawn the way a /s or /window
// window draws a frame with a look (looks.h, present_d3d.h), into a PNG, and
// how long the drawing took. For checking and tuning the looks; a console
// program beside scr_unit, never packaged.
//
//   lookshot --look sharp|crt|crt-curved|smooth|preset [--preset <file.slangp>]
//            [--ambient] --size WxH [--fit x,y,w,h | --stretch] [--palette8]
//            [--d2d] [--hw [--adapter <name|index>]] [--window [--lose]] [--repeat n]
//            <in.png|in.ppm> <out.png>
//
// The picture (a PNG, or a binary PPM as adhostwin's ADOUT writes) is the
// frame: 32-bit, or with --palette8 8-bit through an exact palette when it
// has at most 256 colours, so the palette's path runs. It is drawn into the
// fit rectangle of a WxH picture: fit_rect (the frame's shape kept,
// centred), the given --fit, or with --stretch the whole picture.
//   --d2d     today's Direct2D picture instead (render_frame_bgr, the sharp
//             upscale), to compare against
//   --hw      on the hardware device the windows use, not WARP
//   --adapter with --hw: the device on the adapter whose name contains this
//             (or whose index it is), not the default one (an integrated
//             GPU beside a discrete one, say)
//   --window  also presents it --repeat times through a D3DPresenter into a
//             WxH window that is never shown, for the costs a window pays
//             (CPU per present, and the GPU's time); the PNG is still drawn
//             off screen
//   --lose    with --window: the device is lost (d3d_simulate_device_loss)
//             halfway, and must be reported once and got over, while a
//             second window recovers unasked
//   --repeat  draws it n times and prints the mean time
#include <windows.h>
#include <objbase.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "adw/ui/capture.h"
#include "adw/ui/image.h"
#include "geometry.h"
#include "host_process.h"
#include "looks.h"
#include "present.h"
#include "present_d3d.h"

using namespace adw::scr;
using Clock = std::chrono::steady_clock;

namespace {

int usage() {
  fprintf(stderr,
          "usage: lookshot --look sharp|crt|crt-curved|smooth|preset [--preset <file.slangp>] [--ambient]\n"
          "                --size WxH [--fit x,y,w,h | --stretch] [--palette8] [--d2d] [--hw [--adapter <name|index>]]\n"
          "                [--window [--lose]] [--repeat n] <in.png|in.ppm> <out.png>\n");
  return 2;
}

std::string narrow(const std::wstring& w) {
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return {};
  std::string s((size_t)n - 1, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
  return s;
}

bool ends_with(const std::wstring& s, const wchar_t* tail) {
  const size_t n = wcslen(tail);
  return s.size() >= n && _wcsicmp(s.c_str() + s.size() - n, tail) == 0;
}

// A binary PPM (P6, maxval 255) as a 32-bit frame.
bool read_ppm(const std::wstring& path, Frame& f, std::string* error) {
  FILE* fp = _wfopen(path.c_str(), L"rb");
  if (!fp) {
    *error = "cannot open " + narrow(path);
    return false;
  }
  std::vector<uint8_t> data;
  uint8_t buf[65536];
  for (size_t n; (n = fread(buf, 1, sizeof(buf), fp)) > 0;) data.insert(data.end(), buf, buf + n);
  fclose(fp);
  size_t p = 0;
  auto token = [&](long& out) {
    while (p < data.size()) {
      if (data[p] == '#') {
        while (p < data.size() && data[p] != '\n') ++p;
      } else if (isspace(data[p])) {
        ++p;
      } else {
        break;
      }
    }
    if (p >= data.size() || !isdigit(data[p])) return false;
    out = 0;
    while (p < data.size() && isdigit(data[p])) out = out * 10 + (data[p++] - '0');
    return true;
  };
  long w = 0, h = 0, maxval = 0;
  if (data.size() < 2 || data[0] != 'P' || data[1] != '6') {
    *error = "not a binary PPM (P6)";
    return false;
  }
  p = 2;
  if (!token(w) || !token(h) || !token(maxval) || w <= 0 || h <= 0 || w > 16384 || h > 16384 || maxval != 255) {
    *error = "a PPM this tool doesn't read (P6, maxval 255 only)";
    return false;
  }
  ++p;   // the one blank after maxval
  if (data.size() - p < (size_t)w * h * 3) {
    *error = "the PPM is short";
    return false;
  }
  f.width = (int)w;
  f.height = (int)h;
  f.bpp = 32;
  f.stride = (int)w * 4;
  f.bits.assign((size_t)f.stride * h, 0);
  const uint8_t* s = data.data() + p;
  for (size_t i = 0, n = (size_t)w * h; i < n; ++i, s += 3) {
    f.bits[i * 4 + 0] = s[2];
    f.bits[i * 4 + 1] = s[1];
    f.bits[i * 4 + 2] = s[0];
  }
  return true;
}

// Any picture WIC reads, as a 32-bit frame (opaque: the alpha is dropped).
bool read_picture(const std::wstring& path, Frame& f, std::string* error) {
  if (ends_with(path, L".ppm")) return read_ppm(path, f, error);
  adw::ui::Image img;
  if (!adw::ui::load_image(path, img, error)) return false;
  f.width = img.w;
  f.height = img.h;
  f.bpp = 32;
  f.stride = img.w * 4;
  f.bits.assign((size_t)f.stride * img.h, 0);
  for (size_t i = 0, n = (size_t)img.w * img.h; i < n; ++i) {
    memcpy(&f.bits[i * 4], &img.pbgra[i * 4], 3);
  }
  return true;
}

// The same picture as an 8-bit frame with an exact palette; false when it has
// more than 256 colours.
bool to_palette8(const Frame& f32, Frame& f8) {
  std::unordered_map<uint32_t, uint8_t> index;
  Frame out;
  out.width = f32.width;
  out.height = f32.height;
  out.bpp = 8;
  out.stride = (f32.width + 3) & ~3;
  out.bits.assign((size_t)out.stride * out.height, 0);
  for (int y = 0; y < f32.height; ++y) {
    for (int x = 0; x < f32.width; ++x) {
      const uint8_t* s = &f32.bits[(size_t)y * f32.stride + (size_t)x * 4];
      const uint32_t key = (uint32_t)s[0] | ((uint32_t)s[1] << 8) | ((uint32_t)s[2] << 16);
      auto it = index.find(key);
      if (it == index.end()) {
        if (index.size() == 256) return false;
        const uint8_t i = (uint8_t)index.size();
        it = index.emplace(key, i).first;
        out.palette[i] = RGBQUAD{s[0], s[1], s[2], 0};
      }
      out.bits[(size_t)y * out.stride + x] = it->second;
    }
  }
  f8 = std::move(out);
  return true;
}

bool parse_size(const wchar_t* s, int& w, int& h) { return swscanf(s, L"%dx%d", &w, &h) == 2 && w > 0 && h > 0; }

LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_ERASEBKGND) return 1;
  return DefWindowProcW(hwnd, msg, wp, lp);
}

void pump() {
  MSG msg;
  while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
}

} // namespace

int wmain(int argc, wchar_t** argv) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  LookOptions opts;
  int W = 0, H = 0, repeat = 1;
  RectI fit{};
  bool have_fit = false, stretch = false, palette8 = false, d2d = false, hw = false, window = false, lose = false;
  std::string adapter;
  std::vector<std::wstring> files;
  for (int i = 1; i < argc; ++i) {
    const std::wstring a = argv[i];
    const bool more = i + 1 < argc;
    if (a == L"--look" && more) {
      if (!parse_look(narrow(argv[++i]), &opts.look)) {
        fprintf(stderr, "lookshot: no look called %s\n", narrow(argv[i]).c_str());
        return 2;
      }
    } else if (a == L"--preset" && more) {
      opts.preset = argv[++i];
    } else if (a == L"--ambient") {
      opts.ambient = true;
    } else if (a == L"--size" && more) {
      if (!parse_size(argv[++i], W, H)) return usage();
    } else if (a == L"--fit" && more) {
      if (swscanf(argv[++i], L"%d,%d,%d,%d", &fit.x, &fit.y, &fit.w, &fit.h) != 4) return usage();
      have_fit = true;
    } else if (a == L"--stretch") {
      stretch = true;
    } else if (a == L"--palette8") {
      palette8 = true;
    } else if (a == L"--d2d") {
      d2d = true;
    } else if (a == L"--hw") {
      hw = true;
    } else if (a == L"--adapter" && more) {
      adapter = narrow(argv[++i]);
    } else if (a == L"--window") {
      window = true;
    } else if (a == L"--lose") {
      window = lose = true;
    } else if (a == L"--repeat" && more) {
      repeat = std::max(1, _wtoi(argv[++i]));
    } else if (a.rfind(L"--", 0) == 0) {
      return usage();
    } else {
      files.push_back(a);
    }
  }
  if (files.size() != 2 || W <= 0 || H <= 0) return usage();
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);

  Frame frame;
  std::string err;
  if (!read_picture(files[0], frame, &err)) {
    fprintf(stderr, "lookshot: %s: %s\n", narrow(files[0]).c_str(), err.c_str());
    return 1;
  }
  if (palette8) {
    Frame f8;
    if (to_palette8(frame, f8)) {
      frame = std::move(f8);
    } else {
      fprintf(stderr, "lookshot: more than 256 colours: drawn as a 32-bit frame\n");
    }
  }
  if (!have_fit) fit = stretch ? RectI{0, 0, W, H} : fit_rect(frame.width, frame.height, W, H);

  std::string device = d2d ? "direct2d (software)" : "warp";
  if (!d2d && hw) {
    if (!adapter.empty() && !d3d_choose_adapter(adapter, &err)) {
      fprintf(stderr, "lookshot: %s\n", err.c_str());
      return 1;
    }
    // What the saver does as its windows open (d3d_prepare): the device (with
    // the sharp look's shader), then the look's shaders compiled.
    const auto t0 = Clock::now();
    bool ok = d3d_prepare(LookOptions{}, &err);
    const auto t1 = Clock::now();
    ok = ok && d3d_prepare(opts, &err);
    if (!ok) {
      fprintf(stderr, "lookshot: no hardware device for %s: %s\n", look_name(opts.look), err.c_str());
      return 1;
    }
    printf("lookshot: d3d_prepare: the device %.1f ms, then the look's shaders %.1f ms\n",
           std::chrono::duration<double, std::milli>(t1 - t0).count(),
           std::chrono::duration<double, std::milli>(Clock::now() - t1).count());
    device = "hardware";
  }
  std::vector<uint8_t> bgr;
  double total_ms = 0, gpu_total = 0;
  int gpu_n = 0;
  // With --repeat, a first drawing makes what later ones reuse (the device,
  // the shaders); it is not counted.
  const int runs = repeat > 1 ? repeat + 1 : 1;
  for (int i = 0; i < runs; ++i) {
    const auto t0 = Clock::now();
    const bool ok = d2d ? render_frame_bgr(frame, W, H, fit, true, Filter::smooth, bgr, &err)
                        : render_frame_bgr_d3d(frame, W, H, fit, opts, bgr, &err);
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    if (!ok) {
      fprintf(stderr, "lookshot: %s: %s\n", d2d ? "direct2d" : look_name(opts.look), err.c_str());
      return 1;
    }
    if (i == 0 && repeat > 1) continue;
    total_ms += ms;
    if (!d2d && render_frame_gpu_ms() >= 0) {
      gpu_total += render_frame_gpu_ms();
      ++gpu_n;
    }
  }
  const int counted = repeat > 1 ? repeat : 1;
  if (!adw::ui::save_png_bgr(files[1], W, H, bgr, &err)) {
    fprintf(stderr, "lookshot: %s: %s\n", narrow(files[1]).c_str(), err.c_str());
    return 1;
  }
  const std::string what = d2d ? std::string("direct2d sharp") : std::string(look_name(opts.look)) +
                                                                     (opts.ambient ? ", ambient bars" : "");
  printf("lookshot: %s, %dx%d frame (%d-bit) into %dx%d, fit %d,%d,%d,%d, %s: %.2f ms a picture (mean of %d, with "
         "the read back)",
         what.c_str(), frame.width, frame.height, frame.bpp, W, H, fit.x, fit.y, fit.w, fit.h, device.c_str(),
         total_ms / counted, counted);
  if (gpu_n) printf(", GPU %.3f ms", gpu_total / gpu_n);
  printf("\n");

  if (window && !d2d) {
    // The window's costs: a D3DPresenter into a window that is never shown,
    // paced as the saver paces (a frame every ~16 ms), so each present finds
    // the GPU idle as it would.
    WNDCLASSW wc{};
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"lookshot";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"lookshot", L"lookshot", WS_POPUP, 0, 0, W, H,
                                nullptr, nullptr, wc.hInstance, nullptr);
    // --lose: a second window beside it, which must get over the loss unasked.
    HWND other = lose ? CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"lookshot", L"lookshot 2", WS_POPUP, 0,
                                        0, W / 2, H / 2, nullptr, nullptr, wc.hInstance, nullptr)
                      : nullptr;
    if (!hwnd || (lose && !other)) {
      fprintf(stderr, "lookshot: CreateWindow failed\n");
      return 1;
    }
    D3DPresenter p(opts), q(opts);
    const int warmup = 4, n = std::max(repeat, 8);
    double cpu = 0, gpu = 0;
    int gpus = 0, counted = 0, losses = 0, failed = 0;
    for (int i = 0; i < warmup + n; ++i) {
      if (lose && i == warmup + n / 2) d3d_simulate_device_loss();
      const auto t0 = Clock::now();
      bool lost = false;
      const bool ok = p.present(hwnd, frame, fit, &err, &lost);
      const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
      if (!ok) {
        fprintf(stderr, "lookshot: present %d: %s%s\n", i, err.c_str(), lost ? " (device lost)" : "");
        if (!lost || !lose || ++losses > 1) failed = 1;
        if (failed) break;
        continue;
      }
      if (other && !q.present(other, frame, fit_rect(frame.width, frame.height, W / 2, H / 2), &err, &lost)) {
        fprintf(stderr, "lookshot: the second window's present %d: %s%s\n", i, err.c_str(), lost ? " (device lost)" : "");
        failed = 1;
        break;
      }
      if (i >= warmup && !(lose && i >= warmup + n / 2 && i <= warmup + n / 2 + 1)) {
        cpu += ms;
        ++counted;
        if (p.gpu_ms() >= 0) {
          gpu += p.gpu_ms();
          ++gpus;
        }
      }
      pump();
      Sleep(16);
    }
    if (!failed) {
      printf("lookshot: window %dx%d: %s: %.3f ms CPU a present (mean of %d)", W, H, p.describe().c_str(),
             cpu / std::max(1, counted), counted);
      if (gpus) printf(", GPU %.3f ms", gpu / gpus);
      printf("\n");
      if (lose) {
        printf("lookshot: device loss: %s\n", losses == 1 ? "reported once, then a new device: ok"
                                                          : "never reported: FAILED");
        failed = losses != 1;
      }
    }
    p.release();
    q.release();
    DestroyWindow(hwnd);
    if (other) DestroyWindow(other);
    if (failed) return 1;
  }
  return 0;
}
