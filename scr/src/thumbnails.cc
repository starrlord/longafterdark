#include "thumbnails.h"

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <thread>

#include "adw/ui/capture.h"
#include "geometry.h"
#include "log.h"
#include "paths.h"
#include "sound.h"
#include "ui_model.h"

namespace adw::scr {

using namespace adw::ui;
using namespace std::chrono_literals;

namespace {

constexpr int kThumbMinPx = 96, kThumbMaxPx = 256;
constexpr unsigned long long kCandidates[] = {45, 120, 240, 400};

struct Bitmapinfo256 {
  BITMAPINFOHEADER h;
  RGBQUAD colors[256];
};

unsigned rgb_of(const Frame& f, int x, int y) {
  const uint8_t* row = f.bits.data() + (size_t)y * f.stride;
  if (f.bpp == 8) {
    const RGBQUAD& q = f.palette[row[x]];
    return (unsigned)q.rgbRed << 16 | (unsigned)q.rgbGreen << 8 | q.rgbBlue;
  }
  const uint8_t* p = row + (size_t)x * 4;
  return (unsigned)p[2] << 16 | (unsigned)p[1] << 8 | p[0];
}

// The crop as BGR rows, `out_side` square: the frame's own pixels when the
// crop is already that size, else GDI's HALFTONE resampling.
bool crop_bgr(const Frame& f, const ThumbCrop& c, int out_side, std::vector<uint8_t>& bgr) {
  bgr.assign((size_t)out_side * out_side * 3, 0);
  if (c.side == out_side) {
    for (int y = 0; y < out_side; ++y) {
      for (int x = 0; x < out_side; ++x) {
        const unsigned v = rgb_of(f, c.x + x, c.y + y);
        uint8_t* d = &bgr[((size_t)y * out_side + x) * 3];
        d[0] = v & 0xFF;
        d[1] = (v >> 8) & 0xFF;
        d[2] = (v >> 16) & 0xFF;
      }
    }
    return true;
  }
  BITMAPINFO out{};
  out.bmiHeader = {sizeof(BITMAPINFOHEADER), out_side, -out_side, 1, 32, BI_RGB, 0, 0, 0, 0, 0};
  void* bits = nullptr;
  HDC screen = GetDC(nullptr);
  HDC dc = CreateCompatibleDC(screen);
  HBITMAP dib = CreateDIBSection(screen, &out, DIB_RGB_COLORS, &bits, nullptr, 0);
  ReleaseDC(nullptr, screen);
  bool ok = false;
  if (dc && dib) {
    HGDIOBJ old = SelectObject(dc, dib);
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
    // GDI measures a top-down DIB's source rectangle from its bottom row.
    ok = StretchDIBits(dc, 0, 0, out_side, out_side, c.x, f.height - c.y - c.side, c.side, c.side, f.bits.data(),
                       reinterpret_cast<const BITMAPINFO*>(&bmi), DIB_RGB_COLORS, SRCCOPY) != 0;
    GdiFlush();
    const uint8_t* src = static_cast<const uint8_t*>(bits);
    for (size_t i = 0; i < (size_t)out_side * out_side; ++i) {
      bgr[i * 3] = src[i * 4];
      bgr[i * 3 + 1] = src[i * 4 + 1];
      bgr[i * 3 + 2] = src[i * 4 + 2];
    }
    SelectObject(dc, old);
  }
  if (dib) DeleteObject(dib);
  if (dc) DeleteDC(dc);
  return ok;
}

} // namespace

std::wstring thumb_file(const std::wstring& dir, const std::string& id) {
  if (dir.empty() || id.empty()) return L"";
  // Ids are "ad40.toasters"-like; keep the file name tame whatever they hold.
  std::wstring name;
  for (wchar_t c : widen(id)) name += (iswalnum(c) || c == L'.' || c == L'-' || c == L'_') ? c : L'_';
  return join_path(dir, name + L".v2.png");
}

// ---- ThumbTaker ------------------------------------------------------------------------

bool ThumbTaker::feed(const Frame& f, unsigned long long n) {
  if (!active() || f.width <= 0 || f.height <= 0 || f.stride <= 0) return false;
  if (std::find(std::begin(kCandidates), std::end(kCandidates), n) != std::end(kCandidates)) {
    const ThumbCrop c = choose_thumb_crop(f.width, f.height, [&](int x, int y) { return rgb_of(f, x, y); });
    const int side = std::clamp(c.side, kThumbMinPx, kThumbMaxPx);
    std::vector<uint8_t> bgr;
    if (c.side > 0 && crop_bgr(f, c, side, bgr)) {
      const ThumbQuality q = judge_thumb(bgr.data(), side, side);
      if (q.good && q.score > best_score_) {
        best_score_ = q.score;
        best_side_ = side;
        best_ = std::move(bgr);
      }
    }
  }
  return n >= std::end(kCandidates)[-1] ? finish() : false;
}

bool ThumbTaker::finish() {
  if (done_ || path_.empty()) return false;
  done_ = true;
  if (best_.empty()) return false;
  // Written aside and moved into place: the list never reads half a file.
  ensure_dir(dir_of(path_));
  const std::wstring tmp = path_ + L".tmp";
  std::string err;
  if (!save_png_bgr(tmp, best_side_, best_side_, best_, &err) ||
      !MoveFileExW(tmp.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    DeleteFileW(tmp.c_str());
    log_line("thumbnail %s not saved: %s", narrow(path_).c_str(), err.c_str());
    return false;
  }
  // The earlier kind of thumbnail ("<id>.png") is superseded.
  const std::wstring suffix = L".v2.png";
  if (path_.size() > suffix.size() && path_.compare(path_.size() - suffix.size(), suffix.size(), suffix) == 0) {
    DeleteFileW((path_.substr(0, path_.size() - suffix.size()) + L".png").c_str());
  }
  log_line("thumbnail %s (%dpx, score %.0f)", narrow(path_).c_str(), best_side_, best_score_);
  return true;
}

// ---- ThumbnailQueue ----------------------------------------------------------------------

namespace {

constexpr wchar_t kQueueClass[] = L"LongAfterDarkThumbnailQueue";
constexpr UINT WM_APP_FRAME = WM_APP + 1, WM_APP_HOSTEXIT = WM_APP + 2;
constexpr UINT_PTR kTimerNext = 1, kTimerWatch = 2;
constexpr UINT kJobGapMs = 250;
constexpr long long kJobMaxMs = 12000, kFirstFrameMaxMs = 9000;

LRESULT CALLBACK queue_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_NCCREATE) {
    SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR) reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
  }
  auto* q = reinterpret_cast<ThumbnailQueue*>(GetWindowLongPtrW(h, GWLP_USERDATA));
  if (q && msg != WM_NCCREATE && msg != WM_NCDESTROY) return q->handle(h, msg, wp, lp);
  return DefWindowProcW(h, msg, wp, lp);
}

long long ms_since(Clock::time_point t) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count();
}

} // namespace

ThumbnailQueue::ThumbnailQueue(HWND notify, UINT msg) : notify_(notify), msg_(msg) {
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = queue_proc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = kQueueClass;
  RegisterClassExW(&wc);   // already registered: fine
  hwnd_ = CreateWindowExW(0, kQueueClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, this);
  job_ = create_kill_on_close_job();
  pacer_.start();
}

ThumbnailQueue::~ThumbnailQueue() {
  if (host_) {
    pacer_.remove(host_.get());
    host_->stop();
    host_.reset();
  }
  pacer_.stop();
  if (hwnd_) {
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
    DestroyWindow(hwnd_);
  }
  if (job_) CloseHandle(job_);
}

void ThumbnailQueue::set_jobs(std::vector<ThumbJob> jobs) {
  jobs_.assign(std::make_move_iterator(jobs.begin()), std::make_move_iterator(jobs.end()));
  if (host_) {
    // The module running now carries on if it is still wanted.
    auto it = std::find_if(jobs_.begin(), jobs_.end(), [&](const ThumbJob& j) { return j.id == current_.id; });
    if (it != jobs_.end()) jobs_.erase(it);
    else end_job(false);
  }
  if (!host_) SetTimer(hwnd_, kTimerNext, kJobGapMs, nullptr);
}

void ThumbnailQueue::pause(bool paused) {
  if (paused == paused_) return;
  paused_ = paused;
  pacer_.set_paused(paused);
  if (!paused) {
    started_ = Clock::now();   // the time limit starts over
    if (!host_) SetTimer(hwnd_, kTimerNext, kJobGapMs, nullptr);
  }
}

void ThumbnailQueue::start_next() {
  KillTimer(hwnd_, kTimerNext);
  if (host_ || paused_) return;
  while (!jobs_.empty()) {
    ThumbJob j = std::move(jobs_.front());
    jobs_.pop_front();
    // Taken meanwhile (by the live preview), or nothing to run.
    if (j.thumb_path.empty() || file_exists(j.thumb_path) || !file_exists(j.host_exe) || !file_exists(j.module_path)) continue;
    HostSpec spec;
    spec.exe = j.host_exe;
    spec.module_path = j.module_path;
    spec.working_dir = j.win_dir;
    // The screen the modules were made for, 640x480, a third of which fills
    // a tile: the module's own rule (module_screen) on a 4:3 display at 480
    // lines, which is 640x480 for an After Dark module and for an
    // Intermission, Star Trek, ScreamSavers or Marvel one alike (a module
    // whose catalog "screen" is another size gets that one).
    const SizeI emu = module_screen(own_screen(j.abi, j.screen), 4.0 / 3.0, 1.0).emu;
    spec.env = {
        {L"ADSTREAM", L"1"},
        {L"ADSCREENW", std::to_wstring(emu.w)},
        {L"ADSCREENH", std::to_wstring(emu.h)},
        {L"ADCVSET", widen(j.cvset)},
        {L"AD_ASSETS_DIR", assets_root()},
    };
    add_sound_env(spec.env, sound_for(Settings{}, HostRole::thumbnail, false, false));   // never plays (AUDIO.md §9)
    spec.stderr_path = env_w(L"AD_SCR_HOSTLOG");
    spec.priority_class = IDLE_PRIORITY_CLASS;
    ++generation_;
    host_ = std::make_unique<HostProcess>(HostProcess::Notify{hwnd_, WM_APP_FRAME, WM_APP_HOSTEXIT, (WPARAM)generation_});
    host_->set_min_go_interval(10ms);
    std::wstring err;
    if (!host_->start(spec, job_, &err)) {
      log_line("thumbs: cannot start %s: %s", narrow(spec.module_path).c_str(), narrow(err).c_str());
      host_.reset();
      continue;
    }
    pacer_.add(host_.get());
    current_ = std::move(j);
    taker_ = ThumbTaker(current_.thumb_path);
    frames_ = 0;
    started_ = Clock::now();
    SetTimer(hwnd_, kTimerWatch, 400, nullptr);
    log_line("thumbs: run %s size=%dx%d pid=%lu (%zu more queued)", current_.id.c_str(), emu.w, emu.h, host_->pid(),
             jobs_.size());
    return;
  }
  PostMessageW(notify_, msg_, kThumbIdle, 0);
}

void ThumbnailQueue::stop_host() {
  if (!host_) return;
  pacer_.remove(host_.get());
  std::thread([h = std::move(host_)] { h->stop(); }).detach();
  ++generation_;
}

void ThumbnailQueue::end_job(bool lane_missing) {
  KillTimer(hwnd_, kTimerWatch);
  const bool saved = taker_.finish();
  stop_host();
  if (saved) PostMessageW(notify_, msg_, kThumbSaved, 0);
  if (lane_missing) {
    // This module only: another of its lane or ABI may run (config_dialog.cc).
    cant_run_.push_back(current_.id);
    PostMessageW(notify_, msg_, kThumbLaneMissing, 0);
  }
  SetTimer(hwnd_, kTimerNext, kJobGapMs, nullptr);
}

LRESULT ThumbnailQueue::handle(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_TIMER:
      if (wp == kTimerNext) {
        start_next();
      } else if (wp == kTimerWatch && host_) {
        if (host_->exited()) {
          const DWORD code = host_->exit_code();
          log_line("thumbs: %s exited code=%lu frames=%llu", current_.id.c_str(), (unsigned long)code, frames_);
          end_job(code == 3 && frames_ == 0);
        } else if (!paused_ && (ms_since(started_) > kJobMaxMs || (frames_ == 0 && ms_since(started_) > kFirstFrameMaxMs))) {
          log_line("thumbs: %s timed out after %llu frames", current_.id.c_str(), frames_);
          end_job(false);
        }
      }
      return 0;
    case WM_APP_FRAME: {
      if (wp != (WPARAM)generation_ || !host_) return 0;
      auto f = host_->take_frame();
      if (!f) return 0;
      ++frames_;
      if (taker_.feed(*f, frames_)) PostMessageW(notify_, msg_, kThumbSaved, 0);
      host_->recycle(std::move(f));
      if (!taker_.active()) end_job(false);
      return 0;
    }
    case WM_APP_HOSTEXIT:
      if (wp == (WPARAM)generation_) SendMessageW(h, WM_TIMER, kTimerWatch, 0);
      return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

} // namespace adw::scr
