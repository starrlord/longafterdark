#include "live_preview.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <thread>
#include <utility>

#include "../res/resource.h"
#include "adw/ui/capture.h"
#include "adw/ui/theme.h"
#include "geometry.h"
#include "host_process.h"
#include "log.h"
#include "sound.h"
#include "thumbnails.h"
#include "paths.h"
#include "ui_model.h"

namespace adw::scr {

using namespace adw::ui;

namespace {

using namespace std::chrono_literals;

constexpr wchar_t kClass[] = L"LongAfterDarkLivePreview";
constexpr UINT WM_APP_FRAME = WM_APP + 1;
constexpr UINT WM_APP_HOSTEXIT = WM_APP + 2;
constexpr UINT_PTR kTimerStart = 1, kTimerWatch = 2, kTimerRespawn = 3;
constexpr UINT kStartDelayMs = 220;     // settle time while arrowing through the list
constexpr int kMaxRestarts = 2;

struct Bitmapinfo256 {
  BITMAPINFOHEADER h;
  RGBQUAD colors[256];
};

bool same_target(const LiveTarget& a, const LiveTarget& b) {
  return a.id == b.id && a.abi == b.abi && a.screen == b.screen && a.host_exe == b.host_exe &&
         a.module_path == b.module_path && a.win_dir == b.win_dir && a.cvset == b.cvset;
}

class Preview {
 public:
  explicit Preview(HWND h) : hwnd_(h) {
    job_ = create_kill_on_close_job();
    pacer_.start();
    SetTimer(hwnd_, kTimerWatch, 400, nullptr);
  }
  ~Preview() {
    stop_host(true);
    pacer_.stop();
    if (job_) CloseHandle(job_);
    free_buffer();
  }

  void set_palette(const Palette* pal, int dpi) {
    pal_ = pal;
    if (dpi != dpi_ || !fonts_.body) fonts_.create(dpi, true);   // grayscale: it's drawn on black
    dpi_ = dpi;
    InvalidateRect(hwnd_, nullptr, FALSE);
  }

  void set_hero(bool on) {
    if (on == hero_) return;
    hero_ = on;
    if (on) message(L"", L"");
    InvalidateRect(hwnd_, nullptr, FALSE);
  }

  void set_hover(bool h) {
    if (h == hover_) return;
    hover_ = h;
    InvalidateRect(hwnd_, nullptr, FALSE);
  }

  void run(const LiveTarget& t) {
    if (have_target_ && same_target(t, target_) && (host_ || pending_)) return;
    stop_host(false);
    target_ = t;
    taker_ = ThumbTaker(t.thumb_path);
    have_target_ = true;
    pending_ = true;
    restarts_ = 0;
    title_.clear();
    detail_.clear();
    current_.reset();
    shown_ = 0;
    KillTimer(hwnd_, kTimerRespawn);
    SetTimer(hwnd_, kTimerStart, kStartDelayMs, nullptr);
    InvalidateRect(hwnd_, nullptr, FALSE);
  }

  void message(const std::wstring& title, const std::wstring& detail) {
    stop_host(false);
    pending_ = false;
    have_target_ = false;
    KillTimer(hwnd_, kTimerStart);
    KillTimer(hwnd_, kTimerRespawn);
    current_.reset();
    title_ = title;
    detail_ = detail;
    InvalidateRect(hwnd_, nullptr, FALSE);
  }

  // Paused (the full-screen Preview, a module button's run): no GOs, and a
  // start that comes due meanwhile waits until the pause ends.
  void pause(bool p) {
    pacer_.set_paused(p);
    if (!p && pending_ && !host_) SetTimer(hwnd_, kTimerStart, kStartDelayMs, nullptr);
  }

  // The same target again, from scratch: a new host (after a module's own
  // settings window changed what it will read at start).
  void restart() {
    if (!have_target_) return;
    LiveTarget t = target_;
    have_target_ = false;
    run(t);
  }
  unsigned long long frames() const { return shown_; }
  std::vector<std::string> take_cant_run() { return std::exchange(cant_run_, {}); }

  LRESULT handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
      case WM_ERASEBKGND:
        return 1;
      case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd_, &ps);
        compose(dc);
        EndPaint(hwnd_, &ps);
        return 0;
      }
      case WM_SIZE:
        free_buffer();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
      case WM_TIMER:
        if (wp == kTimerStart) {
          KillTimer(hwnd_, kTimerStart);
          if (pending_ && !pacer_.paused()) spawn();
        } else if (wp == kTimerRespawn) {
          KillTimer(hwnd_, kTimerRespawn);
          if (have_target_ && !host_) spawn();
        } else if (wp == kTimerWatch) {
          watch();
          // "Starting…" appears once a slow module has kept us waiting.
          if (host_ && !current_ && waiting_ms() > 1200 && !starting_shown_) {
            starting_shown_ = true;
            InvalidateRect(hwnd_, nullptr, FALSE);
          }
        }
        return 0;
      case WM_APP_FRAME:
        if (wp == (WPARAM)generation_) present();
        return 0;
      case WM_APP_HOSTEXIT:
        if (wp == (WPARAM)generation_) watch();
        return 0;
      case WM_NCHITTEST:
        return HTTRANSPARENT;   // clicks go to the dialog underneath
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
  }

 private:
  long long waiting_ms() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started_).count();
  }

  void spawn() {
    pending_ = false;
    if (!have_target_) return;
    if (!file_exists(target_.host_exe)) {
      message(L"Preview unavailable", L"adhostwin.exe was not found next to LongAfterDark.scr.");
      return;
    }
    RECT cr{};
    GetClientRect(hwnd_, &cr);
    double aspect = cr.bottom > 0 ? (double)cr.right / cr.bottom : 16.0 / 9.0;
    // A full-size screen, shown scaled down: the preview looks like the
    // saver. The module's own rule (module_screen): an After Dark module's is
    // 480 lines at the preview's aspect (whatever the Resolution setting), a
    // module's own screen (an Intermission, Star Trek, ScreamSavers or Marvel
    // module's 640x480) that, pillarboxed in the wide preview as on a
    // widescreen monitor.
    const SizeI emu = module_screen(own_screen(target_.abi, target_.screen), std::max(aspect, 4.0 / 3.0), 1.0).emu;
    HostSpec spec;
    spec.exe = target_.host_exe;
    spec.module_path = target_.module_path;
    spec.working_dir = target_.win_dir;
    spec.env = {
        {L"ADSTREAM", L"1"},
        {L"ADSCREENW", std::to_wstring(emu.w)},
        {L"ADSCREENH", std::to_wstring(emu.h)},
        {L"ADCVSET", widen(target_.cvset)},
        {L"AD_ASSETS_DIR", assets_root()},
    };
    // A thumbnail in the dialog: always silent (AUDIO.md §9; the Preview
    // button's "/s" is what plays).
    add_sound_env(spec.env, sound_for(Settings{}, HostRole::live_preview, false, false));
    spec.stderr_path = env_w(L"AD_SCR_HOSTLOG");
    spec.priority_class = BELOW_NORMAL_PRIORITY_CLASS;
    ++generation_;
    host_ = std::make_unique<HostProcess>(HostProcess::Notify{hwnd_, WM_APP_FRAME, WM_APP_HOSTEXIT, (WPARAM)generation_});
    host_->set_min_go_interval(33ms);   // 30 fps is plenty for a thumbnail
    std::wstring err;
    if (!host_->start(spec, job_, &err)) {
      log_line("live preview: cannot start %s: %s", narrow(spec.module_path).c_str(), narrow(err).c_str());
      host_.reset();
      message(L"Preview unavailable", L"adhostwin.exe could not be started.");
      return;
    }
    pacer_.add(host_.get());
    started_ = Clock::now();
    starting_shown_ = false;
    log_line("live preview: spawn %s size=%dx%d abi=%s screen=%dx%d cvset=%s pid=%lu", narrow(spec.module_path).c_str(),
             emu.w, emu.h, target_.abi.c_str(), target_.screen.w, target_.screen.h, target_.cvset.c_str(), host_->pid());
  }

  void stop_host(bool wait) {
    // Leaving a module early still keeps the best frame it showed.
    if (taker_.finish() && !wait) PostMessageW(GetParent(hwnd_), WM_APP_LIVE_STATUS, kLiveThumbSaved, 0);
    if (!host_) return;
    pacer_.remove(host_.get());
    if (wait) {
      host_->stop();
      host_.reset();
    } else {
      std::thread([h = std::move(host_)] { h->stop(); }).detach();
    }
    ++generation_;
  }

  void watch() {
    if (!host_ || !host_->exited()) return;
    DWORD code = host_->exit_code();
    uint64_t frames = host_->frames();
    stop_host(false);
    log_line("live preview: host exited code=%lu frames=%llu", (unsigned long)code, (unsigned long long)frames);
    if (code == 3 && frames == 0) {
      // adhostwin: "valid module whose lane is not built into this adhostwin"
      // (host.h: kExitLaneMissing; a module ABI it lacks fails with 1, as a
      // damaged module does, and its --capabilities abis= is what says so).
      // It speaks for this module only (the dialog marks just it:
      // config_dialog.cc).
      cant_run_.push_back(target_.id);
      message(L"Coming soon", L"Modules like this one will run in a future version.");
      PostMessageW(GetParent(hwnd_), WM_APP_LIVE_STATUS, kLiveLaneMissing, 0);
      return;
    }
    if (++restarts_ > kMaxRestarts) {
      // The exit code is for the log (above); the user gets what to do next,
      // in words that fit every release (whatever it came on).
      message((target_.name.empty() ? std::wstring(L"This module") : target_.name) + L" couldn’t start",
              L"Try another module, or import its disc again.");
      return;
    }
    SetTimer(hwnd_, kTimerRespawn, 800, nullptr);
  }

  void present() {
    if (!host_) return;
    auto f = host_->take_frame();
    if (!f) return;
    if (current_) host_->recycle(std::move(current_));
    current_ = std::move(f);
    ++shown_;
    // A thumbnail for the list, from the most detailed of a few frames.
    if (taker_.feed(*current_, shown_)) PostMessageW(GetParent(hwnd_), WM_APP_LIVE_STATUS, kLiveThumbSaved, 0);
    if (HDC dc = GetDC(hwnd_)) {
      compose(dc);
      ReleaseDC(hwnd_, dc);
    }
  }

  void free_buffer() {
    if (buf_) {
      SelectObject(buf_dc_, buf_old_);
      DeleteObject(buf_);
      DeleteDC(buf_dc_);
    }
    buf_ = nullptr;
    buf_dc_ = nullptr;
    buf_w_ = buf_h_ = 0;
  }

  HDC buffer(HDC dc, int w, int h) {
    if (buf_ && buf_w_ == w && buf_h_ == h) return buf_dc_;
    free_buffer();
    buf_dc_ = CreateCompatibleDC(dc);
    buf_ = CreateCompatibleBitmap(dc, w, h);
    buf_old_ = SelectObject(buf_dc_, buf_);
    buf_w_ = w;
    buf_h_ = h;
    return buf_dc_;
  }

  // A few faint stars behind the messages: the saver's night sky, kept clear
  // of the message itself.
  void draw_sky(HDC dc, int w, int h, const RECT& keep_clear) {
    uint32_t seed = 0x2545F491u;
    auto rnd = [&] {
      seed ^= seed << 13;
      seed ^= seed >> 17;
      seed ^= seed << 5;
      return seed;
    };
    const float s = dpi_ / 96.0f;
    const int n = std::max(12, w * h / (int)(2600 * s * s));
    for (int i = 0; i < n; ++i) {
      float x = (float)(rnd() % (uint32_t)std::max(1, w)), y = (float)(rnd() % (uint32_t)std::max(1, h));
      int b = 60 + (int)(rnd() % 110);
      float r = (rnd() % 7 == 0 ? 1.1f : 0.6f) * s;
      POINT pt{(LONG)x, (LONG)y};
      if (PtInRect(&keep_clear, pt)) continue;
      fill_ellipse(dc, x, y, r, RGB(b, b, std::min(255, b + 20)));
    }
  }

  // The welcome's picture: navy fading to black, stars of every brightness,
  // the moon, and two toasters on their way across.
  void draw_hero(HDC dc, const RECT& cr) {
    const float s = dpi_ / 96.0f, w = (float)cr.right, h = (float)cr.bottom;
    fill_gradient(dc, cr, RGB(0x1B, 0x1F, 0x3A), RGB(0x02, 0x02, 0x06));
    const float mx = w * 0.80f, my = std::max(40 * s, h * 0.34f), mr = 30 * s;
    uint32_t seed = 0x9E3779B9u;
    auto rnd = [&] {
      seed ^= seed << 13;
      seed ^= seed >> 17;
      seed ^= seed << 5;
      return seed;
    };
    const int n = std::max(24, (int)(w * h / (1500 * s * s)));
    for (int i = 0; i < n; ++i) {
      const float x = (float)(rnd() % 10000) / 10000 * w, y = (float)(rnd() % 10000) / 10000 * h;
      const int b = 90 + (int)(rnd() % 150);
      const float r = (0.7f + (float)(rnd() % 100) / 100 * 0.9f) * s;
      if (std::hypot(x - mx, y - my) < mr * 1.4f) continue;
      fill_ellipse(dc, x, y, r, blend(RGB(0x02, 0x02, 0x06), RGB(0xF4, 0xF2, 0xE6), b / 255.0));
    }
    draw_sparkle(dc, w * 0.62f, h * 0.18f, 5 * s, RGB(0xFF, 0xFB, 0xEE), 230);
    draw_sparkle(dc, w * 0.09f, h * 0.22f, 3.5f * s, RGB(0xFF, 0xFB, 0xEE), 190);
    draw_sparkle(dc, w * 0.92f, h * 0.78f, 3 * s, RGB(0xFF, 0xFB, 0xEE), 170);
    draw_crescent(dc, mx, my, mr, 70);
    // Toasters fly right to left, the far one smaller and higher.
    draw_flying_toaster(dc, w * 0.40f, h * 0.16f, 44 * s, 0.15f, 215);
    draw_flying_toaster(dc, w * 0.14f, h * 0.46f, 68 * s, 0.85f, 255);
  }

  void compose(HDC target) {
    RECT cr{};
    GetClientRect(hwnd_, &cr);
    if (cr.right <= 0 || cr.bottom <= 0) return;
    HDC dc = buffer(target, cr.right, cr.bottom);
    fill_rect(dc, cr, RGB(0, 0, 0));
    const float s = dpi_ / 96.0f;
    if (hero_) {
      draw_hero(dc, cr);
    } else if (current_) {
      const Frame& f = *current_;
      RectI r = fit_rect(f.width, f.height, cr.right, cr.bottom);
      Bitmapinfo256 bmi{};
      bmi.h.biSize = sizeof(BITMAPINFOHEADER);
      bmi.h.biWidth = f.width;
      bmi.h.biHeight = -f.height;
      bmi.h.biPlanes = 1;
      bmi.h.biBitCount = (WORD)f.bpp;
      bmi.h.biCompression = BI_RGB;
      if (f.bpp == 8) {
        bmi.h.biClrUsed = 256;
        std::copy(f.palette.begin(), f.palette.end(), bmi.colors);
      }
      SetStretchBltMode(dc, HALFTONE);
      SetBrushOrgEx(dc, 0, 0, nullptr);
      StretchDIBits(dc, r.x, r.y, r.w, r.h, 0, 0, f.width, f.height, f.bits.data(),
                    reinterpret_cast<const BITMAPINFO*>(&bmi), DIB_RGB_COLORS, SRCCOPY);
    } else {
      std::wstring title = title_, detail = detail_;
      if (title.empty() && host_ && starting_shown_) title = L"Starting…";
      RECT clear{0, 0, 0, 0};
      if (!title.empty() && pal_ && fonts_.body) {
        const Fonts& fonts = fonts_;
        const int gap = (int)(4 * s);
        SIZE ts = measure_text(dc, title, fonts.body_strong);
        // Two balanced lines rather than a long one and an orphan.
        int bw = (int)(cr.right * 0.8);
        SIZE one = measure_text(dc, detail, fonts.caption);
        if (one.cx > bw) bw = std::min<int>(bw, one.cx / 2 + (int)(24 * s));
        RECT box{(cr.right - bw) / 2, 0, (cr.right + bw) / 2, 0};
        RECT dm = box;
        HGDIOBJ old = SelectObject(dc, fonts.caption);
        DrawTextW(dc, detail.c_str(), -1, &dm, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
        SelectObject(dc, old);
        int dh = detail.empty() ? 0 : dm.bottom - dm.top;
        int total = ts.cy + (dh ? gap + dh : 0);
        int y = (cr.bottom - total) / 2;
        const int wide = std::max<int>(ts.cx, dm.right - dm.left), pad = (int)(12 * s);
        clear = RECT{(cr.right - wide) / 2 - pad, y - pad, (cr.right + wide) / 2 + pad, y + total + pad};
        draw_sky(dc, cr.right, cr.bottom, clear);
        draw_text(dc, title, RECT{0, y, cr.right, y + ts.cy}, fonts.body_strong, RGB(0xF2, 0xF2, 0xF2),
                  DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (dh) {
          RECT d{box.left, y + ts.cy + gap, box.right, y + ts.cy + gap + dh};
          draw_text(dc, detail, d, fonts.caption, RGB(0xA8, 0xA8, 0xB4), DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
        }
      } else {
        draw_sky(dc, cr.right, cr.bottom, clear);
      }
      // A toaster passing high in the corner, clear of any message.
      if (cr.right > 120 * s && cr.bottom > 90 * s) {
        const float tw = 30 * s, tx = cr.right * 0.07f, ty = std::max(8 * s, cr.bottom * 0.08f);
        if ((float)clear.top > ty + tw || clear.bottom <= clear.top) draw_flying_toaster(dc, tx, ty, tw, 0.6f, 150);
      }
    }
    // The module's name along the foot while the pointer is over it.
    if (hover_ && current_ && !target_.name.empty() && fonts_.body) {
      const int band = (int)(40 * s);
      RECT foot{0, cr.bottom - band, cr.right, cr.bottom};
      fade_rect(dc, foot, RGB(0, 0, 0), 0, 200);
      RECT tr{(int)(12 * s), cr.bottom - band, cr.right - (int)(12 * s), cr.bottom - (int)(9 * s)};
      draw_text(dc, target_.name, tr, fonts_.body_strong, RGB(0xF4, 0xF4, 0xF4),
                DT_SINGLELINE | DT_BOTTOM | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    if (pal_) {
      // A little monitor: rounded, a hairline bezel and a faint inner highlight.
      const float radius = 6 * s;
      const int hair = std::max(1, (int)std::lround(s));
      mask_round_corners(dc, cr, radius, pal_->card);
      COLORREF border = pal_->high_contrast ? pal_->card_stroke
                        : pal_->dark        ? RGB(0x3C, 0x3C, 0x3C)
                                            : RGB(0xD0, 0xD0, 0xD0);
      stroke_round(dc, cr, radius, border, (float)hair);
      if (!pal_->high_contrast) {
        RECT in{cr.left + hair, cr.top + hair, cr.right - hair, cr.bottom - hair};
        stroke_round(dc, in, radius - hair, RGB(255, 255, 255), 1.0f, 26);
      }
    }
    BitBlt(target, 0, 0, cr.right, cr.bottom, dc, 0, 0, SRCCOPY);
  }

  HWND hwnd_;
  const Palette* pal_ = nullptr;
  int dpi_ = 96;
  Fonts fonts_;
  bool hover_ = false, hero_ = false;
  ThumbTaker taker_;
  HANDLE job_ = nullptr;
  Pacer pacer_;
  std::unique_ptr<HostProcess> host_;
  uint64_t generation_ = 0;
  std::unique_ptr<Frame> current_;
  LiveTarget target_;
  bool have_target_ = false, pending_ = false, starting_shown_ = false;
  std::vector<std::string> cant_run_;   // ids that exited 3 before a frame, not yet taken
  int restarts_ = 0;
  Clock::time_point started_{};
  std::wstring title_, detail_;
  unsigned long long shown_ = 0;
  HDC buf_dc_ = nullptr;
  HBITMAP buf_ = nullptr;
  HGDIOBJ buf_old_ = nullptr;
  int buf_w_ = 0, buf_h_ = 0;
};

Preview* of(HWND h) { return reinterpret_cast<Preview*>(GetWindowLongPtrW(h, GWLP_USERDATA)); }

LRESULT CALLBACK preview_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_NCCREATE) {
    SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(new Preview(h)));
  } else if (msg == WM_NCDESTROY) {
    delete of(h);
    SetWindowLongPtrW(h, GWLP_USERDATA, 0);
    return DefWindowProcW(h, msg, wp, lp);
  }
  Preview* p = of(h);
  return p ? p->handle(msg, wp, lp) : DefWindowProcW(h, msg, wp, lp);
}

} // namespace

void register_live_preview_class(HINSTANCE hinst) {
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = preview_proc;
  wc.hInstance = hinst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = kClass;
  RegisterClassExW(&wc);
}

HWND create_live_preview(HWND parent, int id, HINSTANCE hinst) {
  register_live_preview_class(hinst);
  return CreateWindowExW(0, kClass, L"Live preview", WS_CHILD | WS_VISIBLE, 0, 0, 16, 9, parent,
                         (HMENU)(INT_PTR)id, hinst, nullptr);
}

void live_preview_set_palette(HWND preview, const Palette* pal, int dpi) {
  if (Preview* p = of(preview)) p->set_palette(pal, dpi);
}

void live_preview_run(HWND preview, const LiveTarget& target) {
  if (Preview* p = of(preview)) p->run(target);
}

void live_preview_message(HWND preview, const std::wstring& title, const std::wstring& detail) {
  if (Preview* p = of(preview)) p->message(title, detail);
}

void live_preview_hero(HWND preview, bool on) {
  if (Preview* p = of(preview)) p->set_hero(on);
}

void live_preview_set_hover(HWND preview, bool hover) {
  if (Preview* p = of(preview)) p->set_hover(hover);
}

void live_preview_pause(HWND preview, bool paused) {
  if (Preview* p = of(preview)) p->pause(paused);
}

void live_preview_restart(HWND preview) {
  if (Preview* p = of(preview)) p->restart();
}

unsigned long long live_preview_frames(HWND preview) {
  Preview* p = of(preview);
  return p ? p->frames() : 0;
}

std::vector<std::string> live_preview_take_cant_run(HWND preview) {
  Preview* p = of(preview);
  return p ? p->take_cant_run() : std::vector<std::string>{};
}

} // namespace adw::scr
