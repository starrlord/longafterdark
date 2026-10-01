// The configure-mode test hooks (INTERACTION.md §6.6), shared by both lanes:
// they act on the REAL dialogs a module's button opens, through the real
// controls, so the guest sees the same notifications as from a user.
//
//   ADCONFIGSCRIPT=<file>   the script below
//   ADCONFIGHIDDEN=1        dialogs never appear: parked off every monitor,
//                           cloaked (DWMWA_CLOAK), WS_EX_NOACTIVATE, never
//                           activated; message boxes and file dialogs are
//                           answered only by the script (unanswered: IDCANCEL
//                           / cancel, logged)
//   ADCONFIGDUMP=1          log each dialog's controls (id, class, style, text)
//   ADCONFIGTIMEOUTMS=<ms>  (default 10000) a dialog still open when its part of
//                           the script has run (or any dialog, when hidden) is
//                           closed with IDCANCEL after this long; the run then
//                           counts as failed (the host exits 1)
//
// Script (one action per line; '#' starts a comment; ids decimal or 0x hex):
//
//   TEXT <id> <text…>          WM_SETTEXT on an edit control (+ EN_CHANGE)
//   CHECK <id> <0|1|2>         BM_SETCHECK + BN_CLICKED
//   SELECT <id> <index>        LB/CB current selection + LBN_/CBN_SELCHANGE
//   PICK <id> <text…>          the same for the item whose text this is (any
//                              case; LB_/CB_FINDSTRINGEXACT), wherever a sorted
//                              list holds it: "PICK 204 [-h-]"
//   MULTI <id> <i,j,…>         multi-select list box selection + LBN_SELCHANGE
//   CLICK <id>                 BN_CLICKED (1 = IDOK, 2 = IDCANCEL)
//   PRESS <id> [<x> <y>]       a click where a user's lands: at the control's
//                              centre (or at x, y in its client area), the
//                              window the system's hit test finds there (see
//                              hit_window) gets WM_LBUTTONDOWN and WM_LBUTTONUP,
//                              and the dialog then handles what they posted;
//                              refused when that point is in no window's client
//                              area (a scroll bar, a caption)
//   FILE <host path>           answer the next file dialog without showing it
//   ANSWER <IDOK|IDCANCEL|IDYES|IDNO|IDABORT|IDRETRY|IDIGNORE|n>
//                              answer the next message box without showing it
//   NEXT                       the following lines apply to the next dialog opened
//
// The dialog lines form blocks separated by NEXT; each dialog takes the next
// block when it opens (a dialog opened by an action of an outer block takes
// the following block, runs, and the outer block then continues). FILE and
// ANSWER lines are consumed in order by file dialogs and message boxes,
// whenever those appear.
//
// Use: the lane calls attach() once a real dialog exists (after its
// WM_INITDIALOG), and asks message_box() / file_dialog() before showing one.
// The runner subclasses the dialog; its actions run from a posted message, so
// they happen inside the dialog's message loop.
#pragma once

#include <windows.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace adw {
struct Env;
}

namespace adw::win32 {

class ConfigScript {
 public:
  struct Action {
    enum class Kind { text, check, select, pick, multi, click, press } kind = Kind::click;
    int id = 0;
    int value = 0;           // CHECK state, SELECT index
    std::vector<int> items;  // MULTI
    std::wstring text;       // TEXT, PICK
    bool at = false;         // PRESS: at (x, y) in the control's client area, not its centre
    int x = 0, y = 0;
    int line = 0;            // script line (for logs)
  };

  ConfigScript();
  ~ConfigScript();
  ConfigScript(const ConfigScript&) = delete;
  ConfigScript& operator=(const ConfigScript&) = delete;

  // ADCONFIGSCRIPT / ADCONFIGHIDDEN / ADCONFIGDUMP / ADCONFIGTIMEOUTMS. False
  // (and *error) when the script cannot be read or has a malformed line.
  bool load_env(const Env& env, std::string* error);
  // The script's text (load_env reads the file and calls this; tests call it).
  bool parse(std::string_view text, std::string* error);
  void set_hidden(bool on) { hidden_ = on; }
  void set_dump(bool on) { dump_ = on; }
  void set_timeout_ms(uint32_t ms) { timeout_ms_ = ms; }

  bool scripted() const { return scripted_; }
  bool hidden() const { return hidden_; }
  bool dump() const { return dump_; }
  uint32_t timeout_ms() const { return timeout_ms_; }
  // A dialog had to be closed by the timeout.
  bool timed_out() const { return timed_out_; }
  // Dialogs attached so far.
  int dialogs() const { return dialogs_; }

  // A real dialog exists: park it (hidden), dump it, subclass it, and post
  // its block of actions. `force_close(dlg)` ends the dialog when the guest
  // ignores the timeout's IDCANCEL (the lane's EndDialog).
  void attach(HWND dlg, std::function<void(HWND)> force_close);

  // The next message box: the scripted answer; IDCANCEL when hidden and none
  // is left (logged); nullopt = show it.
  std::optional<int> message_box(std::wstring_view text);
  // The next file dialog: the scripted host path; L"" (cancel) when hidden and
  // none is left (logged); nullopt = show it.
  std::optional<std::wstring> file_dialog();

  // One action on a dialog, through its real controls. False when the
  // control is missing or the action does not fit its class (PICK: or no
  // item has that text; PRESS: or the point is in no window's client area).
  static bool apply(HWND dlg, const Action& a);
  // The window a click at `pt` (screen) lands on, found as the system's hit
  // test finds it among windows of the calling thread: from `top` down,
  // children before their parent, the top of the z-order first, only visible
  // windows whose rectangle holds the point; a disabled child is passed over,
  // and a window that answers WM_NCHITTEST with HTTRANSPARENT leaves the point
  // to what lies under it (Intermission's frames over their check boxes).
  // *ht = the hit-test code (HTCLIENT: the client area). Null: none.
  static HWND hit_window(HWND top, POINT pt, LRESULT* ht);
  // Logs the dialog's controls on stderr.
  static void dump_dialog(HWND dlg);
  // Off every monitor, cloaked, never activated.
  static void park(HWND dlg);

  // (internal) the subclass procedure's view.
  struct Attached;

 private:
  static LRESULT CALLBACK subclass_proc(HWND, UINT, WPARAM, LPARAM);

  bool scripted_ = false, hidden_ = false, dump_ = false, timed_out_ = false;
  uint32_t timeout_ms_ = 10000;
  std::vector<std::vector<Action>> blocks_;
  size_t next_block_ = 0;
  std::deque<int> answers_;
  std::deque<std::wstring> files_;
  int dialogs_ = 0;
};

}  // namespace adw::win32
