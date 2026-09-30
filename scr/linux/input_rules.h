// The saver's input rules (INTERACTION.md §4): which user input wakes the
// saver and which belongs to the module. The Windows saver's
// scr/src/input_rules.* without Windows types, so the Linux player decides
// exactly as LongAfterDark.scr does, and the whole table is unit-tested.
//
// The input owner's host publishes a status record after every step (read
// from its STATUS lines, status.h); each input line the player sends it has
// a number (HostProcess::send_input). For an event that would end the saver
// when the module is not interactive, decide() answers
//   forward  the event is the module's (or harmless): keep running
//   exit     the user is back: end the saver
//   hold     not known yet: the host has not stepped with the input sent
//            before this one (a Caps Lock press a moment ago may be starting
//            a game), or it may consume input without being interactive (a
//            keyboard hook, a module reading the saver window's queue). Ask
//            again on every status change; at most kHoldLimit.
#pragma once

#include <chrono>
#include <cstdint>
#include <string>

#include "geometry.h"
#include "status.h"

namespace lad {

using InputClock = std::chrono::steady_clock;

inline constexpr auto kHoldLimit = std::chrono::milliseconds(300);
// Physical pixels (Euclidean) the pointer may drift from its baseline before
// a move counts as the user coming back.
inline constexpr double kMoveThreshold = 10.0;

// Windows virtual-key codes the rules name.
inline constexpr int VK_SHIFT = 0x10, VK_CONTROL = 0x11, VK_MENU = 0x12, VK_CAPITAL = 0x14, VK_ESCAPE = 0x1B,
                     VK_F10 = 0x79, VK_NUMLOCK = 0x90, VK_LSHIFT = 0xA0, VK_RSHIFT = 0xA1, VK_LCONTROL = 0xA2,
                     VK_RCONTROL = 0xA3;

enum class InputKind {
  key_down,     // WM_KEYDOWN
  key_up,       // WM_KEYUP
  syskey_down,  // WM_SYSKEYDOWN (Alt, Alt+x, F10)
  syskey_up,    // WM_SYSKEYUP
  button_down,  // a mouse button pressed
  button_up,
  wheel,        // a wheel notch (X buttons 4-7)
  move,         // pointer motion
  deactivate,   // switching away (focus taken by another window)
};

struct InputEvent {
  InputKind kind = InputKind::key_down;
  int vk = 0;          // keys
  double dist = 0;     // move: distance from the baseline, physical px
};

// What the player knows about the input owner's host.
struct OwnerStatus {
  bool running = false;          // there is a host process, and it has not exited
  bool have = false;             // ...and it has published a status record
  HostStatus rec{};
  bool interactive() const { return have && (rec.flags & ADWS_INTERACTIVE); }
  bool key_filter() const { return have && (rec.flags & ADWS_KEY_FILTER); }
  bool cursor() const { return have && (rec.flags & ADWS_CURSOR); }
  bool rotate_ok() const { return have && (rec.flags & ADWS_ROTATE_OK); }
  bool wake() const { return have && (rec.flags & ADWS_WAKE); }
};

// One event's decision state.
struct HoldSeqs {
  uint64_t n = 0;                  // number of the input line sent for this event (0 = none sent)
  bool holding = false;            // already held: wait for input_applied >= n
  InputClock::time_point since{};  // when the event happened
};

enum class Verdict { forward, exit, hold };

// Caps Lock, Num Lock, Shift and Ctrl (and their left/right forms) never
// wake the saver (AFTERDAR.SCR 0x4028db..0x4028f2).
bool exempt_key(int vk);

// §4.2 + §4.3. Pure: the same inputs give the same answer.
Verdict decide(const InputEvent& ev, const OwnerStatus& st, const HoldSeqs& seqs, InputClock::time_point now);

// The exit reason for the log, e.g. "key vk=0x41", "syskey vk=0x12",
// "move dx=12 dy=-3", "button", "wheel", "deactivated".
std::string exit_reason(const InputEvent& ev, long dx = 0, long dy = 0);

// The pointer (window coordinates) mapped into the frame rectangle `fit`
// (in the same window) and scaled to the host's emulated screen `emu`,
// clamped to [0, emu - 1] (INTERACTION.md §4.2, "MOUSE coordinates").
PointI map_to_frame(PointI in_window, const RectI& fit, SizeI emu);

std::string key_line(int vk, bool down);
std::string caps_line(bool on);
std::string numlock_line(bool on);   // only for a host whose --capabilities says numlock=1
std::string mouse_line(int x, int y, uint32_t buttons);

}  // namespace lad
