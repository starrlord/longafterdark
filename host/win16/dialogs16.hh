// Real dialogs for Win16 modules in configure mode (INTERACTION.md §6.2,
// "Win16"): a Classic module's button handler (MODULE(7 + slot), reached
// through OLDMOD16's BUTTONPUSHED16 or the native bridge) opens its own
// dialogs with DialogBox & co.; in `adhostwin --configure` they become REAL
// Win32 dialogs owned by the settings window, whose host dialog procedure
// forwards every message the module cares about to the module's 16-bit
// DLGPROC (Runtime16::call_far, Pascal), translating it on the way:
//
//   * the template: our own Win16 DLGTEMPLATE → Win32 DLGTEMPLATE converter
//     (convert_dialog_template16; DWORD-aligned items, UTF-16 strings from
//     code page 1252; nothing from Wine/WineVDM);
//   * handles: a real window is shown to the guest as an HWND16 from a
//     reserved range (kRealHwnd16First.., multiples of 4 — no selector, GDI
//     object, icon or emulated window has one), mapped both ways;
//   * placement: the guest keeps its emulated desktop's screen coordinates,
//     and that desktop lies over the owner window (guest_screen_origin16);
//     GetCursorPos is the real cursor on it;
//   * messages Win32 → Win16 (WM_COMMAND, WM_CTLCOLOR*, scroll messages,
//     the *ITEM structs, …) into the guest, and Win16 → Win32 for what the
//     guest sends a real control: Win16's control messages are WM_USER-based
//     and depend on the target's class (msg16_to_32) — EM 0x400+n ↔ 0xB0+n,
//     BM 0x400+n ↔ 0xF0+n, LB 0x401+n ↔ 0x180+n, CB 0x400+n ↔ 0x140+n —
//     with far pointers marshalled and EM_SETSEL/EM_LINESCROLL repacked; a
//     real BM_* to a guest's control arrives as BM 0x400+n; WM_NCHITTEST
//     reaches a guest's window procedure, and its answer is the real one
//     (Intermission's frames are transparent to clicks: HTTRANSPARENT);
//   * DCs: a real HDC reaches the guest as a gdi16 DC wrapper for the length
//     of one message (WM_CTLCOLOR: its colours and the returned brush go back
//     to the real DC; WM_DRAWITEM, WM_PAINT, GetDC: the guest draws on an
//     8-bit key surface — gdi16's model, palettes and all — that is then
//     turned into real colours through the hardware palette and blitted);
//   * MessageBox → a real MessageBoxW; COMMDLG GetOpenFileName/GetSaveFileName
//     → the real dialogs, returning 8.3 H:\ paths (win32::Vfs::host_to_guest);
//     COMMDLG ChooseFont → the real font dialog (screen fonts), its choice
//     written back as the 16-bit LOGFONT, point size, colour and font type
//     (SWTEXT's Select Font; cancelled, logged, when hidden: no script line
//     picks a font); WinHelp → logged, 1; WinExec("notepad <file>") → the
//     real Notepad on the file's upper-layer copy (NONSENSE's Edit Names).
//
// The ADCONFIG* test hooks (win32/config_script.hh) script the real
// dialogs (PRESS clicks where a user's click lands). In the saver nothing
// here is installed: dialogs stay refused.
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace adw::win32 {
class ConfigScript;
}

namespace adw::win16 {

class Runtime16;

// ---- the template converter ----
struct DialogTemplate32 {
  bool ok = false;
  std::string error;
  std::vector<uint8_t> bytes;        // the Win32 DLGTEMPLATE (DWORD-aligned)
  std::string caption;
  int items = 0;
  std::vector<std::string> classes;  // item classes given by name (custom controls), as written
};
// A Win16 DLGTEMPLATE (as the RT_DIALOG resource holds it) as a Win32 one.
// The menu and a custom dialog class are dropped (the real dialog runs in
// this process, which has neither); SS_ICON items lose their ordinal (their
// icon is the guest's); `ex_style` is OR'ed into the dialog's extended style.
DialogTemplate32 convert_dialog_template16(std::string_view win16, uint32_t ex_style = 0);

// ---- message translation ----
enum class Ctl16 : uint8_t { other, button, edit, listbox, combobox, statik, scrollbar };
// A real window's class name → which Win16 control message set it takes.
Ctl16 control_kind16(std::wstring_view real_class);
// A Win16 message the guest sends to a control of that kind → the Win32
// number (msg itself when it is not class-specific); 0 = a Win16 control
// message with no Win32 counterpart.
uint32_t msg16_to_32(Ctl16 kind, uint16_t msg);
// The inverse (for messages a control sends back that carry its own
// numbers): 0 = none.
uint16_t msg32_to_16(Ctl16 kind, uint32_t msg);

// ---- configure mode ----
constexpr uint16_t kRealHwnd16First = 0xC000, kRealHwnd16Limit = 0xE000;

struct Configure16 {
  HWND owner = nullptr;                     // --owner, or null
  win32::ConfigScript* script = nullptr;    // ADCONFIG* hooks (never null while enabled)
  int shown = 0;                            // dialogs, message boxes, file dialogs, programs started
  bool failed = false;                      // a dialog could not be shown, or the script timed out
  std::vector<std::string> notes;           // for the JSON "message"
};
// Installs the real-dialog shims on `rt` (configure mode only; after
// register_all16). `cfg` must outlive the runtime's use of them.
void enable_real_dialogs16(Runtime16& rt, Configure16* cfg);
// The HWND16 a real window is shown as (allocated on first use); 0 for null.
uint16_t real_hwnd16(Runtime16& rt, HWND h);
// The real window behind an HWND16 from the reserved range, or null.
HWND real_window16(Runtime16& rt, uint16_t h16);

// Where the guest's screen lies on the real one (INTERACTION.md §6.2;
// host/win16/README.md). The guest keeps the screen coordinates of its w × h
// emulated desktop (Marvel and Lunatic Fringe centre their dialogs on
// 640 × 480 without asking for the screen size): a top-level real window it
// puts at (x, y) goes to origin + (x, y), and what it reads back (window
// rectangles, ClientToScreen/ScreenToClient, CB_GETDROPPEDCONTROLRECT, a
// top-level window's WM_MOVE) comes the other way. The origin centres that
// desktop on the owner window, moved inside the owner monitor's work area
// (centred on the work area when the desktop is the larger); all in the
// dialog thread's coordinates. enable_real_dialogs16 takes it from the
// --owner; with none, or in a hidden run (its dialogs are parked off every
// monitor), it is (0, 0).
POINT guest_screen_origin16(const RECT& owner, const RECT& work, int w, int h);

}  // namespace adw::win16
