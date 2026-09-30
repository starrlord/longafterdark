#include "input_rules.h"

#include <algorithm>
#include <cstdio>

namespace lad {

bool exempt_key(int vk) {
  switch (vk) {
    case VK_CAPITAL:
    case VK_NUMLOCK:
    case VK_SHIFT:
    case VK_CONTROL:
    case VK_LSHIFT:
    case VK_RSHIFT:
    case VK_LCONTROL:
    case VK_RCONTROL:
      return true;
    default:
      return false;
  }
}

Verdict decide(const InputEvent& ev, const OwnerStatus& st, const HoldSeqs& seqs, InputClock::time_point now) {
  // §4.2: the events that never need a verdict from the host.
  switch (ev.kind) {
    case InputKind::key_up:
    case InputKind::syskey_up:
    case InputKind::button_up:
      return Verdict::forward;
    case InputKind::syskey_down:  // Alt / F10: our escape, whatever the module wants
    case InputKind::deactivate:   // switching away
      return Verdict::exit;
    case InputKind::wheel:
      return st.interactive() ? Verdict::forward : Verdict::exit;
    case InputKind::key_down:
      if (exempt_key(ev.vk)) return Verdict::forward;
      break;
    case InputKind::move:
      if (st.interactive() || ev.dist <= kMoveThreshold) return Verdict::forward;
      break;
    case InputKind::button_down:
      break;
  }

  // §4.3 for a non-exempt key down, a button down, a move past the threshold.
  if (st.interactive()) return Verdict::forward;
  // No host (a message on the window, or it died): nobody to ask.
  if (!st.running || seqs.n == 0) return Verdict::exit;
  const HostStatus& r = st.rec;
  // Stale: the host has not stepped with the input sent before this one yet
  // (nothing published at all counts as stale: it is still starting).
  const bool stale = !st.have || r.input_applied + 1 < seqs.n;
  if (!seqs.holding && !stale && !st.key_filter()) return Verdict::exit;
  // Held: settled once the host has stepped with this line, or at the limit
  // on whatever the status says then.
  const bool settled = st.have && r.input_applied >= seqs.n;
  if (settled || now - seqs.since >= kHoldLimit) {
    return st.have && r.input_eaten >= seqs.n ? Verdict::forward : Verdict::exit;
  }
  return Verdict::hold;
}

std::string exit_reason(const InputEvent& ev, long dx, long dy) {
  char buf[64];
  switch (ev.kind) {
    case InputKind::key_down:
    case InputKind::key_up:
      snprintf(buf, sizeof(buf), "key vk=0x%02X", ev.vk & 0xFF);
      return buf;
    case InputKind::syskey_down:
    case InputKind::syskey_up:
      snprintf(buf, sizeof(buf), "syskey vk=0x%02X", ev.vk & 0xFF);
      return buf;
    case InputKind::button_down:
    case InputKind::button_up:
      return "button";
    case InputKind::wheel:
      return "wheel";
    case InputKind::move:
      snprintf(buf, sizeof(buf), "move dx=%ld dy=%ld", dx, dy);
      return buf;
    case InputKind::deactivate:
      return "deactivated";
  }
  return "?";
}

PointI map_to_frame(PointI in_window, const RectI& fit, SizeI emu) {
  PointI p{0, 0};
  if (fit.w <= 0 || fit.h <= 0 || emu.w <= 0 || emu.h <= 0) return p;
  const long cx = in_window.x - fit.x, cy = in_window.y - fit.y;
  // floor division, so a point just left of the frame clamps to 0 rather
  // than rounding toward it.
  auto scale = [](long v, int to, int from) {
    long long num = (long long)v * to;
    long long q = num / from;
    if (num < 0 && q * from != num) --q;
    return q;
  };
  p.x = (int)std::clamp<long long>(scale(cx, emu.w, fit.w), 0, emu.w - 1);
  p.y = (int)std::clamp<long long>(scale(cy, emu.h, fit.h), 0, emu.h - 1);
  return p;
}

std::string key_line(int vk, bool down) { return "KEY " + std::to_string(vk & 0xFF) + (down ? " 1" : " 0"); }
std::string caps_line(bool on) { return on ? "CAPS 1" : "CAPS 0"; }
std::string numlock_line(bool on) { return on ? "NUMLOCK 1" : "NUMLOCK 0"; }
std::string mouse_line(int x, int y, uint32_t buttons) {
  return "MOUSE " + std::to_string(x) + " " + std::to_string(y) + " " + std::to_string(buttons & 7);
}

}  // namespace lad
