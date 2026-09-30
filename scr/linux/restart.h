// When a host is started again, the Windows saver's rule (scr/src/saver.cc:
// SaverWindow::watchdog, spawn and present_latest) without its windows, so
// the Linux player restarts a host exactly as LongAfterDark.scr does:
//
//  * a run fails when its host ends (it exits, stalls, or sends no first
//    frame) with no frame shown, or less than 5 s after it was started (the
//    5 s count from the start, not from the first frame); any other run
//    resets the count of failed runs;
//  * the next host starts 250 ms after a good run, else 0.5, 1, 2, 4 ...
//    seconds after the failed runs so far, 30 s at most, for as long as the
//    player runs: restarts never stop;
//  * when three runs in a row have failed and the last showed no frame, the
//    window says the module could not be started, until a frame comes;
//  * a rotation of more than one module skips a module after three failed
//    runs, and once every module of it was skipped in turn without a frame,
//    it tries again every 30 s;
//  * a host that can't be spawned at all is tried again after 1 s per
//    failure so far, 30 s at most.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace lad {

// A host that ran this long with frames was healthy: its end resets the count.
inline constexpr std::chrono::seconds kHealthyRun{5};
// Failed runs in a row before a rotation skips the module, and before the
// window says it could not be started (when the last run showed no frame).
inline constexpr int kFailedRunsBeforeMessage = 3;

// What follows the end of a host.
struct RestartStep {
  bool message = false;                  // say the module could not be started (until a frame comes)
  bool skip = false;                     // the rotation moves on to its next module
  std::chrono::milliseconds delay{0};    // when the next host starts
};

class RestartRule {
 public:
  // A host ended by itself, stalled or sent no first frame, having shown
  // `frames` frames in the `lived` it ran. `can_skip`: a rotation of more
  // than one module, of `rotation_size` modules.
  RestartStep host_ended(uint64_t frames, std::chrono::steady_clock::duration lived, bool can_skip,
                         size_t rotation_size);
  // A host could not be spawned: the delay before the next try.
  std::chrono::milliseconds spawn_failed();
  // A frame was shown: the rotation's run of modules without one ends.
  void frame_shown() { dead_modules_ = 0; }
  // Another module plays (the rotation moved on): its runs count afresh.
  void new_module() { failures_ = 0; }

  int failures() const { return failures_; }
  size_t dead_modules() const { return dead_modules_; }

 private:
  int failures_ = 0;          // failed runs in a row of the current module
  size_t dead_modules_ = 0;   // modules skipped in a row without one frame
};

}  // namespace lad
