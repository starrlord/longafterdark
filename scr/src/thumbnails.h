// Module thumbnails for the settings dialog's list and details tile: a
// module with no icon of its own is shown by a square of one of its own
// frames (ui_model.h: choose_thumb_crop, judge_thumb), kept as a PNG in the
// thumbnails folder (paths.h: thumbs_dir) and never taken of a blank or
// near-blank screen.
//
// Two things take them:
//   * the dialog's live preview, of the module it is showing (live_preview.h);
//   * ThumbnailQueue, which runs every module still without a picture, one
//     at a time, in a host of its own at idle priority and off-screen, for a
//     few seconds each: after an import, and whenever the dialog opens with
//     any missing. Nothing it runs is ever shown.
#pragma once

#include <windows.h>

#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "catalog.h"
#include "host_process.h"

namespace adw::scr {

// The file for module `id` in `dir`: "<id>.v2.png". The version goes up when
// the way they are taken improves, so older ones are taken again.
std::wstring thumb_file(const std::wstring& dir, const std::string& id);

// Watches a running module's frames and keeps the most detailed crop among
// frames 45, 120, 240 and 400; saves it (a square PNG at the crop's own
// size, 96 to 256 px) after frame 400, or earlier on finish(). Nothing is
// saved when no frame was worth keeping.
class ThumbTaker {
 public:
  ThumbTaker() = default;
  explicit ThumbTaker(std::wstring path) : path_(std::move(path)) {}
  bool active() const { return !path_.empty() && !done_; }
  // The n-th frame shown (1-based). True when this call saved the file.
  bool feed(const Frame& f, unsigned long long n);
  // The module stops here: save the best kept so far, if any. True when saved.
  bool finish();

 private:
  std::wstring path_;
  bool done_ = false;
  double best_score_ = 0;
  int best_side_ = 0;
  std::vector<uint8_t> best_;   // BGR, best_side_ square
};

struct ThumbJob {
  std::string id;
  std::wstring host_exe, module_path, win_dir, thumb_path;
  std::string cvset;             // the module's settings, as the saver would send them
  // Its catalog ABI and "screen", which size its screen (geometry.h:
  // own_screen, module_screen).
  std::string abi = kAfterDarkAbi;
  SizeI screen;
  // Its host controls' variables (catalog.h: HostControlValues::env), as the
  // saver would set them: Intermission 4.0's Speed.
  std::vector<std::pair<std::wstring, std::wstring>> env;
};

// Posted to the queue's owner: wParam says what happened.
inline constexpr WPARAM kThumbSaved = 1;          // a thumbnail was written (lParam: 0)
inline constexpr WPARAM kThumbLaneMissing = 2;    // the host has no lane for a module (exit 3): take_cant_run() names it
inline constexpr WPARAM kThumbIdle = 3;           // nothing more queued

class ThumbnailQueue {
 public:
  ThumbnailQueue(HWND notify, UINT msg);
  ~ThumbnailQueue();
  ThumbnailQueue(const ThumbnailQueue&) = delete;
  ThumbnailQueue& operator=(const ThumbnailQueue&) = delete;

  // Replaces what is queued; the module running now is finished first
  // unless it is no longer wanted.
  void set_jobs(std::vector<ThumbJob> jobs);
  // While paused (the full-screen Preview runs) no frames are asked for.
  void pause(bool paused);
  bool idle() const { return !host_ && jobs_.empty(); }
  // The ids of the modules whose host exited 3 before a frame since the last
  // call (each kThumbLaneMissing names one), oldest first; the list is emptied.
  std::vector<std::string> take_cant_run() { return std::exchange(cant_run_, {}); }

  LRESULT handle(HWND h, UINT msg, WPARAM wp, LPARAM lp);

 private:
  void start_next();
  void end_job(bool lane_missing);
  void stop_host();

  HWND hwnd_ = nullptr, notify_ = nullptr;
  UINT msg_ = 0;
  HANDLE job_ = nullptr;
  Pacer pacer_;
  std::deque<ThumbJob> jobs_;
  std::vector<std::string> cant_run_;   // ids that exited 3 before a frame, not yet taken
  std::unique_ptr<HostProcess> host_;
  ThumbJob current_;
  ThumbTaker taker_;
  uint64_t generation_ = 0;
  unsigned long long frames_ = 0;
  Clock::time_point started_{};
  bool paused_ = false;
};

} // namespace adw::scr
