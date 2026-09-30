#include "restart.h"

#include <algorithm>

namespace lad {

using namespace std::chrono_literals;
using std::chrono::milliseconds;

RestartStep RestartRule::host_ended(uint64_t frames, std::chrono::steady_clock::duration lived, bool can_skip,
                                    size_t rotation_size) {
  RestartStep r;
  failures_ = (frames > 0 && lived >= kHealthyRun) ? 0 : failures_ + 1;
  if (frames > 0) dead_modules_ = 0;
  // Said rather than sitting on a black screen: most likely the module's
  // lane is not in this adhostwin (exit 3), or its files are damaged.
  r.message = failures_ >= kFailedRunsBeforeMessage && frames == 0;
  // A module that can't stay up is skipped when there is anything else to show.
  if (failures_ >= kFailedRunsBeforeMessage && can_skip) {
    if (frames == 0) ++dead_modules_;
    failures_ = 0;
    r.skip = true;
  }
  r.delay = failures_ == 0 ? milliseconds(250) : std::min<milliseconds>(30s, 500ms * (1 << std::min(failures_ - 1, 6)));
  // Every module of the rotation failed in turn: no churning through process
  // launches, a relaxed pace instead.
  if (r.skip && dead_modules_ >= rotation_size) r.delay = 30s;
  return r;
}

milliseconds RestartRule::spawn_failed() {
  ++failures_;
  return std::min<milliseconds>(30s, 1s * failures_);
}

}  // namespace lad
