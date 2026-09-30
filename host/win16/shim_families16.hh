// The Win16 API families, one file each, and the helpers they share.
//
// A family registers its handlers with `r.impl("KERNEL", "GlobalAlloc", fn)`
// (the signature — ordinal, argument bytes, AX vs DX:AX — comes from
// signatures16.cc); a function whose row is a stub (no known argument list)
// is declared with `r.add(module, ordinal, name, conv, ret16, bytes, fn)`.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "loader/image.hh"
#include "win16/runtime16.hh"

namespace adw::win16 {

void register_kernel16(Runtime16& rt);
void register_user16(Runtime16& rt);
void register_gdi16(Runtime16& rt);
// MMSYSTEM, WIN87EM, COMMDLG, KEYBOARD, SHELL, SOUND, TOOLHELP, VER.
void register_system16(Runtime16& rt);
// MMSYSTEM's sound half (sound16.cc; register_system16 calls it).
void register_sound16(Runtime16& rt);
// Every family above plus the DOS/BIOS interrupt services (dos16.hh).
void register_all16(Runtime16& rt);
// The host's own AD_SND (adsnd16.cc): After Dark 3.0's sound library as a
// system module named AD_SND, with AD_SND 3.0.3's entries. Not one of
// register_all16's families: the ne16 lane's AD3 protocol registers it, for
// the native bridge, only when the package's engine dir holds no AD_SND.DLL
// (ne16/package.hh host_ad_snd). Once registered, AD_SND by name — an import
// or LoadLibrary, whatever the path — is this module.
void register_host_ad_snd(Runtime16& rt);

// The saver window (full screen, visible): the HWND the lane hands OLDMOD16.
uint16_t user16_saver_window(Runtime16& rt);

// A window class the guest registered (RegisterClass), for the real-dialog
// layer (dialogs16.cc) to give real windows of that class. False when unknown.
struct Class16View {
  std::string name;
  uint32_t proc = 0;
  uint16_t style = 0, hinst = 0, background = 0, cursor = 0;
  int16_t cls_extra = 0, wnd_extra = 0;
};
bool user16_class(Runtime16& rt, std::string_view name, Class16View* out);
// The Windows 95 default system colour (GetSysColor).
uint32_t user16_sys_color(int index);

// Messages the host posts for MMSYSTEM (sound16.cc: MM_WOM_*, MM_MCINOTIFY;
// AUDIO.md §8.6): they wait in the guest's posted queue like PostMessage's,
// so a guest that pumps its own queue takes them there; the lane's pump
// (sound16.hh audio16_pump) sends those still waiting to their window
// procedures, where the 1996 host's message loop dispatched them.
void user16_post_host(Runtime16& rt, uint16_t hwnd, uint16_t msg, uint16_t wparam, uint32_t lparam);
// Sends up to `max` host-posted messages still in the queue to their window
// procedures, in queue order (one for a window that is gone is dropped);
// returns how many left the queue.
int user16_dispatch_host(Runtime16& rt, int max = 64);
bool user16_window_exists(Runtime16& rt, uint16_t hwnd);
// The message loop INTERMIS ran between two calls of an Intermission saver
// (INTERMIS 1:09da..1:0a64: PeekMessage, GetMessage, DispatchMessage), for the
// ne16 lane's Intermission protocol (the AD3 path never calls it): sends the
// guest's own posted messages — not the host-posted ones (user16_dispatch_host),
// not the tagged input messages, which stay for the lane — to the window
// procedures of their (live) windows, in queue order, and then the WM_TIMERs
// that are due to their TIMERPROC or window; messages posted to the task
// itself (hwnd 0: SWSE FORCETOWAKE's PostAppMessage(GetCurrentTask(), …)) are
// removed and counted (StepReport16::task_posts, last_task_msg), nobody
// takes them. At most `max` messages are handled; returns how many were.
int user16_dispatch_guest(Runtime16& rt, int max = 64);

// The task this runtime is (GetCurrentTask; made on first use), and the task
// the synthetic desktop's Program Manager belongs to (GetWindowTask, IsTask;
// no task list shows it).
uint16_t kernel16_current_task(Runtime16& rt);
uint16_t kernel16_shell_task(Runtime16& rt);

// ---- helpers shared by the families ----

// DS at the call (Local* work on the caller's DS; so does MakeProcInstance's world).
uint16_t caller_ds(Call16& c);
// A resource type/name argument: MAKEINTRESOURCE (selector 0) or a string
// ("#123" means 123, as FindResource reads it).
loader::ResId res_id(Runtime16& rt, uint32_t fp);
std::string upper16(std::string_view s);
bool ieq16(std::string_view a, std::string_view b);

}  // namespace adw::win16
