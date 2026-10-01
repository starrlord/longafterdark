#include "config_dialog.h"

#include <windows.h>
#include <commctrl.h>
#include <oleacc.h>
#include <shellapi.h>
#include <windowsx.h>
#include <uxtheme.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "../res/resource.h"
#include "test_hooks.h"
#include "adw/ui/capture.h"
#include "adw/ui/image.h"
#include "adw/ui/theme.h"
#include "adw/ui/widgets.h"
#include "catalog.h"
#include "cover_strip.h"
#include "dialog_support.h"
#include "host_process.h"
#include "live_preview.h"
#include "log.h"
#include "module_icons.h"
#include "paths.h"
#include "releases.h"
#include "settings.h"
#include "thumbnails.h"
#include "ui_model.h"

// The settings dialog (/c). Stock Win32 controls from IDD_SETTINGS, laid out
// in code for the window's size and DPI (ui_model.h) and painted in the
// Windows 11 style in light, dark and high-contrast modes (ui_theme.h,
// ui_widgets.h). What it reads and writes is unchanged: catalog-win.json in,
// settings.ini out, by the rules in settings.h (DESIGN.md §6a).

namespace adw::scr {

// The look (theme, widgets, capture, covers) is adw_ui's, shared with adimport.
using namespace adw::ui;

namespace {

constexpr UINT WM_APP_PREVIEW_DONE = WM_APP + 10;
constexpr UINT WM_APP_IMPORT_DONE = WM_APP + 11;
constexpr UINT WM_APP_REBUILD_PANEL = WM_APP + 12;
constexpr UINT WM_APP_LANE_PROBE = WM_APP + 13;
constexpr UINT WM_APP_THUMBS = WM_APP + 14;            // from the ThumbnailQueue (wParam: kThumb*)
constexpr UINT WM_APP_SCHEDULE_THUMBS = WM_APP + 15;
constexpr UINT WM_APP_CONFIGURE_DONE = WM_APP + 16;    // wParam: exit code, lParam: std::string* (its JSON line)
constexpr UINT WM_APP_COVER_DONE = WM_APP + 17;        // "Change cover…"'s adimport exited (wParam: exit code)
constexpr UINT WM_APP_FOOTER_CREDIT_FOCUS = WM_APP + 18;  // after an activation's focus restore (dialog_proc)
constexpr UINT WM_APP_REMOVE_DONE = WM_APP + 19;       // "Remove …"'s adimport exited (wParam: exit code)

// ListView group ids: 1 + the release's index in the catalog (COVERS.md §1.7).
int group_id(int release) { return 1 + release; }
// Row geometry of the module list, DIPs.
constexpr int kRowH = 40, kIconDip = 28, kBoxDip = kListBoxDip;
// Space between one group's last row and the next group's header, DIPs.
constexpr int kGroupGapDip = 12;

struct State {
  HINSTANCE hinst = nullptr;
  HWND dlg = nullptr, list = nullptr, panel = nullptr, preview = nullptr;
  Settings settings;                                    // as loaded (the base for saving)
  std::map<std::string, std::map<int, int>> controls;   // edited control values
  Catalog catalog;
  std::vector<bool> present;                            // per catalog index: its file exists
  std::wstring win_dir;
  int shown = -1;                                       // catalog index shown in the details
  int scroll = 0;                                       // panel scroll offset, px (always a row's top)
  PanelLayout P;                                        // the panel's rows as last placed
  int list_trim = 0;                                    // px the list stops short of its card's foot (position_list)
  HWND tip = nullptr;                                   // tooltips of the panel's read-only rows
  std::vector<UINT_PTR> tip_tools;                      // ...their ids (slot + 1)
  std::vector<int> panel_stops;                         // scroll positions: row tops (empty: no scrolling)
  int peek_row = -1;                                    // first row below the fold (drawn faded), or -1
  HWINEVENTHOOK focus_hook = nullptr;                   // scrolls a panel row into view when it takes focus
  bool populating = false;
  bool preview_running = false, import_running = false;
  std::wstring preview_ini;
  std::wstring import_note;                             // why the last import changed nothing
  bool random = true;
  bool sound_on = true;                                 // the Sound dropdown says "Primary monitor" (AUDIO.md §9)
  // "A different module on each monitor" (ui_model.h: per_monitor_choice):
  // the monitors there are to tell apart, counted again at each display
  // change (the test hook's staged layouts follow those changes).
  int monitors = 1;
  size_t display_changes = 0;
  // What this adhostwin can run (dialog_support.h, module_run): its
  // `--capabilities` answer (the lanes and module ABIs it lists; asked once,
  // and again at a new catalog only while it hasn't answered), and the
  // modules whose own runs exited 3 (no lane for them, whatever it listed).
  bool probing = false;                                 // the host is being asked right now
  HostCapabilities caps;                                // what `adhostwin --capabilities` said
  std::set<std::string> cant_run;                       // ids whose preview or thumbnail exited 3
  // A module button's run (INTERACTION.md §6.3): one at a time.
  bool configuring = false;
  std::string configure_id;                             // the module whose button runs
  int configure_slot = -1;
  // What the last run of each module button left to say under its row
  // (module id -> catalog index -> note), and which ran fine (the "Custom"
  // hint shows under those).
  std::map<std::string, std::map<int, std::wstring>> button_notes;
  std::map<std::string, std::set<int>> button_ok;

  // Look.
  Theme theme;
  ThemeMode theme_mode = ThemeMode::system;
  int forced_dpi = 0;                                   // screenshot hook: lay out at this DPI
  bool offscreen = false;                               // screenshot hook: never activated, never focused
  bool thumbgen_allowed = false;                        // take the missing thumbnails in the background
  std::unique_ptr<ThumbnailQueue> thumbgen;
  bool thumbgen_ran = false;                            // scheduled at least once with the lane known
  WindowLayout L;
  ModuleIcons icons;
  HIMAGELIST row_images = nullptr;                      // sets the list's row height
  HICON logo = nullptr;
  int logo_px = 0;
  BadgeKind badge_kind = BadgeKind::neutral;
  int title_lines = 1;                                  // the module's name: 1 line, or wrapped onto 2
  HFONT title_font = nullptr;                           // ...in the subtitle face or the smaller one
  bool has_credits = false;
  bool hover_preview = false;

  // Releases (COVERS.md §1). The list shows `model`: the catalog grouped by
  // release under the strip's filter.
  ListModel model;
  std::vector<int> full_order;                          // catalog indices in the unfiltered list's order
  std::vector<std::wstring> row_label;                  // per catalog index: what its row says
  // Random's checks: one per catalog id, for every release, including the
  // ones the filter hides (the list view shows those of its rows).
  std::set<std::string> checks;
  // The chosen module: what Single plays, and what the details show while
  // its row is listed. A filter that hides its release never changes it (the
  // details then show a listed module: details_after_filter).
  std::string chosen;
  std::unique_ptr<CoverStrip> strip;
  bool strip_on = false;                                // two or more releases: the strip shows
  bool filter_on = false;                               // some, not all, releases selected
  bool live_region = false;                             // the status line is a polite live region
  bool cover_running = false;                           // "Change cover…"'s adimport is running
  std::string cover_id;
  bool remove_running = false;                          // "Remove …"'s adimport is running
  std::string remove_id;                                // ...for this release
  bool covers_missing = false;                          // a release still shows a generated cover: "Get the covers"
  HWND chip_tip = nullptr;                              // the release chip's "Also on:" tooltip (tool 1)
  // Group headers drawn ellipsized: group id -> the header's rect in the list
  // and the whole title, a tool of chip_tip on the list (set_header_tip).
  std::map<int, std::pair<RECT, std::wstring>> header_tips;
  std::map<int, std::wstring> header_drawn;             // group id -> its title as last drawn (the screenshot report)
  std::wstring chip_text;
  std::wstring rotation_tip_text;                       // the rotation line's tooltip (tool 2): copies, the lead
  // The footer's credit (ui_model.h: layout_footer_credit), the link IDC_FOOTER_CREDIT.
  FooterCreditLayout footer_credit;
  int assets_right = 0;                                 // px: where the assets line's text ends
  bool footer_credit_hover = false;                            // screenshot hook: the link drawn as under the pointer
};

State* g_state = nullptr;   // for the focus event hook (one dialog per process)

// ---- small helpers ----------------------------------------------------------------

int dpi_of(const State& st) {
  if (st.forced_dpi) return st.forced_dpi;
  UINT d = st.dlg ? GetDpiForWindow(st.dlg) : 0;
  return d ? (int)d : 96;
}

int selected_module(const State& st) {
  int item = ListView_GetNextItem(st.list, -1, LVNI_SELECTED);
  if (item < 0) return -1;
  LVITEMW it{};
  it.mask = LVIF_PARAM;
  it.iItem = item;
  if (!ListView_GetItem(st.list, &it)) return -1;
  return (int)it.lParam;
}

int item_for_module(const State& st, int module_index) {
  LVFINDINFOW fi{};
  fi.flags = LVFI_PARAM;
  fi.lParam = module_index;
  return ListView_FindItem(st.list, -1, &fi);
}

// Random's checks, global: every catalog id checked, in the unfiltered
// list's order (what Randomize is written from). `total` = the catalog's size.
std::vector<std::string> checked_ids(const State& st, int* total) {
  std::vector<std::string> ids;
  if (total) *total = (int)st.catalog.modules.size();
  for (int mi : st.full_order) {
    const std::string& id = st.catalog.modules[mi].id;
    if (st.checks.count(id)) ids.push_back(id);
  }
  return ids;
}

// ...and among the rows the list shows: how many, how many checked.
struct ShownChecks {
  size_t shown = 0, checked = 0;
};
ShownChecks shown_checks(const State& st) {
  ShownChecks s;
  for (const ListGroup& g : st.model.groups) {
    for (const ListRow& r : g.rows) {
      ++s.shown;
      s.checked += st.checks.count(st.catalog.modules[r.module].id);
    }
  }
  return s;
}

int module_index(const State& st, const std::string& id) {
  for (size_t i = 0; i < st.catalog.modules.size(); ++i) {
    if (st.catalog.modules[i].id == id) return (int)i;
  }
  return -1;
}

// The strip's selection as a filter (empty = every release).
std::vector<std::string> current_filter(const State& st) {
  if (!st.strip_on || !st.strip) return {};
  return effective_collections(st.strip->selected(), st.catalog);
}

int control_value(const State& st, const Module& m, const Control& c) {
  if (auto it = st.controls.find(m.id); it != st.controls.end()) {
    if (auto v = it->second.find(c.index); v != it->second.end()) return c.clamp(v->second);
  }
  return c.def;
}

bool is_present(const State& st, int i) { return i >= 0 && i < (int)st.present.size() && st.present[i]; }

// The monitors connected now; in the test build, those AD_SCR_TEST_MONITORS
// stages when it stages any (geometry.h: parse_staged_monitors, as the saver
// reads them), its `layout` after as many display changes.
int count_monitors(size_t layout) {
#if AD_SCR_TEST_HOOKS
  const std::vector<StagedMonitor> staged = parse_staged_monitors(env_w(L"AD_SCR_TEST_MONITORS"), layout);
  if (!staged.empty()) return (int)staged.size();
#else
  (void)layout;
#endif
  return std::max(1, GetSystemMetrics(SM_CMONITORS));
}

// The Monitors dropdown says "All monitors" (not "Primary monitor only").
bool monitors_all(const State& st) { return SendDlgItemMessageW(st.dlg, IDC_MONITORS, CB_GETCURSEL, 0, 0) != 1; }

const Module* shown_module(const State& st) {
  return st.shown >= 0 && st.shown < (int)st.catalog.modules.size() ? &st.catalog.modules[st.shown] : nullptr;
}

bool welcome(const State& st) { return st.catalog.modules.empty(); }

// The module the file names to play first in front of its Randomize list
// (Settings::has_lead): Random keeps it on OK. "" when there is none.
std::string lead_id(const State& st) { return st.settings.has_lead() ? st.settings.module : std::string(); }
// Its row's badge in Random (and, for screen readers, its item text).
constexpr wchar_t kPlaysFirst[] = L"Plays first";

// Whether this adhostwin can run the module (module_run): it waits while
// the host is asked; it is "Coming soon" when the host doesn't list its lane
// or its ABI, or when a run of this very module exited 3.
ModuleRun run_state(const State& st, const Module& m) {
  return module_run(m, st.caps, st.probing, st.cant_run.count(m.id) != 0);
}
bool coming_soon(const State& st, const Module& m) { return run_state(st, m) == ModuleRun::coming_soon; }

// A module button the dialog can press (INTERACTION.md §6.3): the host opens
// module windows for its lane (--capabilities: configure=…) and the module
// file is there.
bool button_live(const State& st, int module_index) {
  if (!is_present(st, module_index)) return false;
  const Module& m = st.catalog.modules[module_index];
  return st.caps.can_configure(m.lane) && !coming_soon(st, m);
}

// The popup with an item "Custom" (Messages 4.0's and Message Mayhem's
// "Message:"), whose custom text a button edits; null when there is none.
const Control* custom_popup(const Module& m) {
  for (const Control& c : m.controls) {
    if (c.type != ControlType::popup) continue;
    for (const std::string& item : c.items) {
      std::string t = item;
      while (!t.empty() && t.back() == ' ') t.pop_back();
      if (t.size() == 6 && _stricmp(t.c_str(), "custom") == 0) return &c;
    }
  }
  return nullptr;
}

int custom_item_index(const Control& c) {
  for (size_t i = 0; i < c.items.size(); ++i) {
    std::string t = c.items[i];
    while (!t.empty() && t.back() == ' ') t.pop_back();
    if (_stricmp(t.c_str(), "custom") == 0) return (int)i;
  }
  return -1;
}

// What a module button's row says under it: the last run's outcome
// ("Nothing to set here", "Couldn't open this option (code N)"), or after a
// run that worked, where the module shows custom text only when its popup is
// on "Custom", how to get it shown. Nothing is switched for the user.
std::wstring button_note(const State& st, const Module& m, const Control& c) {
  if (auto it = st.button_notes.find(m.id); it != st.button_notes.end()) {
    if (auto n = it->second.find(c.index); n != it->second.end() && !n->second.empty()) return n->second;
  }
  auto ok = st.button_ok.find(m.id);
  if (ok == st.button_ok.end() || !ok->second.count(c.index)) return {};
  const Control* p = custom_popup(m);
  if (!p) return {};
  const int custom = custom_item_index(*p);
  if (custom < 0 || control_value(st, m, *p) - p->min == custom) return {};
  std::wstring label = widen(p->name);
  while (!label.empty() && (label.back() == L' ' || label.back() == L':')) label.pop_back();
  return L"Choose “Custom” under " + label + L": to show it";
}

// A module button's tooltip: what it opens, and that Cancel here does not
// take back what the module keeps itself (true of the 1996 control panels too).
constexpr wchar_t kButtonWhy[] =
    L"Opens the module’s own settings window. What you set there is saved by the module at once; "
    L"Cancel here doesn’t undo it.";

// Every settable control of the module at its catalog default.
bool at_defaults(const State& st, const Module& m) {
  return std::all_of(m.controls.begin(), m.controls.end(),
                     [&](const Control& c) { return !c.settable() || control_value(st, m, c) == c.def; });
}

std::wstring without_mnemonic(std::wstring s) {
  s.erase(std::remove(s.begin(), s.end(), L'&'), s.end());
  return s;
}

std::wstring crlf(const std::wstring& s) {
  std::wstring out;
  for (wchar_t ch : s) {
    if (ch == L'\n' && (out.empty() || out.back() != L'\r')) out += L'\r';
    out += ch;
  }
  return out;
}

// "Objects:" -> "Objects": labels sit above their controls here.
std::wstring label_text(const std::string& name) {
  std::wstring s = widen(name);
  while (!s.empty() && (s.back() == L':' || s.back() == L' ')) s.pop_back();
  return s;
}

// We set fonts and positions ourselves at every DPI; the dialog manager's
// per-monitor rescaling of template controls would fight that.
void keep_own_layout(HWND c) {
  const auto both = (DIALOG_CONTROL_DPI_CHANGE_BEHAVIORS)(DCDC_DISABLE_FONT_UPDATE | DCDC_DISABLE_RELAYOUT);
  SetDialogControlDpiChangeBehavior(c, both, both);
}

void place(HWND h, const Rc& r, int grow = 0) {
  if (!h) return;
  SetWindowPos(h, nullptr, r.x - grow, r.y - grow, r.w + 2 * grow, r.h + 2 * grow,
               SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
}

std::wstring window_text(HWND h) {
  int n = GetWindowTextLengthW(h);
  std::wstring s(n + 1, L'\0');
  GetWindowTextW(h, s.data(), n + 1);
  s.resize(n);
  return s;
}

std::string module_cvset(const State& st, const Module& m) {
  // As the saver sends it: only values the user has set, clamped by the catalog.
  std::map<int, int> cv;
  if (auto it = st.controls.find(m.id); it != st.controls.end()) {
    for (auto [idx, val] : it->second) {
      if (const Control* c = m.control(idx); c && c->settable()) cv[idx] = c->clamp(val);
    }
  }
  return format_cvset(cv);
}

// ---- live preview -------------------------------------------------------------------

// While adimport removes a release ("Remove …"), none of its modules runs:
// its folder must be free to be moved aside.
bool being_removed(const State& st, const Module& m) {
  return st.remove_running && m.release >= 0 && m.release == st.catalog.release_index(st.remove_id);
}

void refresh_preview(State& st) {
  if (!st.preview) return;
  const int i = st.shown;
  if (i < 0 || i >= (int)st.catalog.modules.size()) {
    // Just the night sky: the welcome beside it says what to do.
    live_preview_message(st.preview, L"", L"");
    return;
  }
  const Module& m = st.catalog.modules[i];
  if (!is_present(st, i)) {
    live_preview_message(st.preview, L"Module file missing", L"Import its disc again to restore it.");
    return;
  }
  if (being_removed(st, m)) {
    live_preview_message(st.preview, L"", L"");
    return;
  }
  switch (run_state(st, m)) {
    case ModuleRun::coming_soon:
      live_preview_message(st.preview, L"Coming soon", L"Modules like this one will run in a future version.");
      return;
    case ModuleRun::waiting:
      // The host is being asked what it runs; start nothing yet.
      live_preview_message(st.preview, L"", L"");
      return;
    case ModuleRun::runs:
      break;
  }
  LiveTarget t;
  t.id = m.id;
  // Its screen: its own when it has one (an Intermission, Star Trek,
  // ScreamSavers or Marvel module's 640x480; geometry.h: own_screen,
  // module_screen).
  t.abi = m.abi;
  t.screen = m.screen;
  t.host_exe = host_exe_path();
  t.module_path = resolve_module_path(st.win_dir, m.path);
  t.win_dir = st.win_dir;
  t.cvset = module_cvset(st, m);
  t.name = widen(m.name);
  // A thumbnail for the list, for a module that has no picture yet.
  if (!st.icons.has_picture(m, st.win_dir)) t.thumb_path = st.icons.thumb_path(m.id);
  live_preview_run(st.preview, t);
}

// ---- fonts, colours, layout ----------------------------------------------------------

HFONT font_for(const State& st, int id) {
  const Fonts& f = st.theme.fonts;
  switch (id) {
    case IDC_MODULES_LABEL: return f.body_strong;
    case IDC_MODULE_TITLE: return f.subtitle;
    case IDC_MODULE_BADGE:
    case IDC_CREDITS:
    case IDC_ASSETS_STATUS:
    case IDC_ROTATION_SUMMARY:
    case IDC_STRIP_STATUS:
    case IDC_SOUND_NOTE:
    case IDC_MODULES_COUNT: return f.caption;
    case IDC_ABOUT: return f.body_gray;   // fades at its foot: no ClearType fringes
    default: return f.body;
  }
}

COLORREF text_color_for(const State& st, int id) {
  const Palette& p = st.theme.pal;
  switch (id) {
    case IDC_MODULES_COUNT: return p.text2;
    case IDC_ROTATION_SUMMARY:
    case IDC_ABOUT:
    case IDC_ASSETS_STATUS: return p.text2;
    case IDC_CREDITS: return p.text2;
    case IDC_PANEL_EMPTY: return p.text2;
    case IDC_STRIP_STATUS: return p.text2;   // the strip's only instruction: text3 (3.1:1 in light mode) is too faint
    case IDC_SOUND_NOTE: return p.text2;
    // Volume is greyed with its slider while Sound is Off.
    case IDC_VOLUME_LABEL: return st.sound_on ? p.text : p.text_disabled;
    case IDC_VOLUME_VALUE: return st.sound_on ? p.text2 : p.text_disabled;
    default:
      if (id >= IDC_PANEL_BASE && (id - IDC_PANEL_BASE) % IDC_PANEL_STRIDE == IDC_PART_VALUE) return p.text2;
      return p.text;
  }
}

void set_fonts(State& st) {
  for (HWND c = GetWindow(st.dlg, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
    if (c == st.panel || c == st.preview) continue;
    SendMessageW(c, WM_SETFONT, (WPARAM)font_for(st, GetDlgCtrlID(c)), FALSE);
  }
}

// Row height of the module list: set through a 1-pixel-wide state image
// list (the rows are painted in full by custom draw).
void set_row_height(State& st) {
  const int h = st.theme.px(kRowH);
  HIMAGELIST il = ImageList_Create(1, h, ILC_COLOR32, 2, 0);
  for (int i = 0; i < 2; ++i) {
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), 1, h, 1, 32, BI_RGB, 0, 0, 0, 0, 0};
    void* bits = nullptr;
    HBITMAP b = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ImageList_Add(il, b, nullptr);
    DeleteObject(b);
  }
  HIMAGELIST old = ListView_SetImageList(st.list, il, LVSIL_STATE);
  if (old && old != il) ImageList_Destroy(old);
  st.row_images = il;
  // Room after each group, so the next group's header sits apart from the
  // rows above it (the list view already leaves 3 px).
  LVGROUPMETRICS gm{sizeof(gm)};
  gm.mask = LVGMF_BORDERSIZE;
  gm.Bottom = (UINT)std::max(0, st.theme.px(kGroupGapDip) - 3);
  ListView_SetGroupMetrics(st.list, &gm);
}

void apply_theme(State& st) {
  st.theme.load(st.theme_mode);
  const Palette& p = st.theme.pal;
  apply_window_chrome(st.dlg, p);
  const bool dark = p.dark && !p.high_contrast;
  theme_native_control(st.list, dark);
  theme_native_control(GetDlgItem(st.dlg, IDC_ABOUT), dark);
  if (st.tip) theme_native_control(st.tip, dark);
  if (st.chip_tip) theme_native_control(st.chip_tip, dark);
  // Popup menus (the covers' context menu) follow the app mode.
  allow_dark_menus(dark);
  if (st.strip) st.strip->refresh();
  if (st.panel) theme_native_control(st.panel, dark);
  ListView_SetBkColor(st.list, p.card);
  ListView_SetTextBkColor(st.list, p.card);
  ListView_SetTextColor(st.list, p.text);
  if (st.preview) live_preview_set_palette(st.preview, &st.theme.pal, st.theme.dpi);
  const int face = st.theme.px(32) + 2 * focus_margin(st.theme.dpi);
  for (int id : {IDC_DURATION, IDC_SCALE, IDC_MONITORS, IDC_SOUND}) size_combo(GetDlgItem(st.dlg, id), st.theme, face);
  RedrawWindow(st.dlg, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

void load_logo(State& st) {
  const int px = st.theme.px(32);
  if (st.logo && st.logo_px == px) return;
  if (st.logo) DestroyIcon(st.logo);
  st.logo = (HICON)LoadImageW(st.hinst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, px, px, 0);
  st.logo_px = px;
  UINT dpi = st.dlg ? GetDpiForWindow(st.dlg) : 96;
  HICON big = (HICON)LoadImageW(st.hinst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetricsForDpi(SM_CXICON, dpi),
                                GetSystemMetricsForDpi(SM_CYICON, dpi), 0);
  HICON small = (HICON)LoadImageW(st.hinst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                  GetSystemMetricsForDpi(SM_CXSMICON, dpi), GetSystemMetricsForDpi(SM_CYSMICON, dpi), 0);
  if (HICON old = (HICON)SendMessageW(st.dlg, WM_SETICON, ICON_BIG, (LPARAM)big)) DestroyIcon(old);
  if (HICON old = (HICON)SendMessageW(st.dlg, WM_SETICON, ICON_SMALL, (LPARAM)small)) DestroyIcon(old);
}

// A status line of caption text (one or two lines), centred vertically on
// `r`. `fit`: the window only as wide as its text (its widest line), so it
// covers nothing beside it. Returns that width, px.
int place_caption(State& st, HWND h, const Rc& r, bool fit = false) {
  HDC dc = GetDC(h);
  RECT m{0, 0, std::max(1, r.w), 0};
  HGDIOBJ old = SelectObject(dc, st.theme.fonts.caption);
  std::wstring text = window_text(h);
  DrawTextW(dc, text.c_str(), -1, &m, DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
  SelectObject(dc, old);
  ReleaseDC(h, dc);
  const int th = std::max(1, std::min<int>(r.h, m.bottom - m.top));
  const int tw = std::clamp<int>(m.right - m.left, 0, r.w);
  // 2 DIP to spare, so the static never breaks its lines otherwise.
  place(h, Rc{r.x, r.y + (r.h - th) / 2, fit ? std::min(r.w, tw + st.theme.px(2)) : r.w, th});
  InvalidateRect(h, nullptr, TRUE);
  return tw;
}

void place_footer_credit(State& st);

// The footer's assets line is one or two lines of caption text, centred on
// the buttons; the credit goes in the room it leaves before Preview.
void place_assets_status(State& st) {
  Rc r = st.L.assets;
  if (welcome(st)) {
    r.w += r.x - st.L.import.x;
    r.x = st.L.import.x;
  }
  st.assets_right = r.x + place_caption(st, GetDlgItem(st.dlg, IDC_ASSETS_STATUS), r, true);
  place_footer_credit(st);
}

// The footer's credit (ui_model.h: layout_footer_credit), laid out for the assets
// line's text as it now reads: the link IDC_FOOTER_CREDIT, hidden when it doesn't
// fit whole with the footer's gap each side.
void place_footer_credit(State& st) {
  HWND link = GetDlgItem(st.dlg, IDC_FOOTER_CREDIT);
  if (!link) return;
  const Theme& t = st.theme;
  FooterCreditInput in;
  in.assets_right = st.assets_right;
  {
    HDC dc = GetDC(st.dlg);
    in.lead_w = measure_text(dc, kFooterCreditLead, t.fonts.caption).cx;
    // A space's width, as it sets between two words.
    in.space_w = measure_text(dc, L"a b", t.fonts.caption).cx - measure_text(dc, L"ab", t.fonts.caption).cx;
    const SIZE name = measure_text(dc, kFooterCreditName, t.fonts.caption);
    in.name_w = name.cx;
    in.line_h = name.cy;
    ReleaseDC(st.dlg, dc);
  }
  st.footer_credit = layout_footer_credit(st.L, in);
  if (st.footer_credit.shown) {
    place(link, st.footer_credit.box, focus_margin(t.dpi));
    ShowWindow(link, SW_SHOWNA);
    InvalidateRect(link, nullptr, TRUE);
  } else {
    // Too narrow: hidden whole, never clipped. The keyboard moves on to the
    // next control rather than stay on a hidden one, through the dialog
    // manager (WM_NEXTDLGCTL, sent while the link still has the focus), which
    // moves the default push button with it as Tab does: the link gives up
    // BS_DEFPUSHBUTTON and Preview takes it. A plain SetFocus left both as
    // they were, so Enter on Preview went to the dialog's default, OK, which
    // saves and closes.
    if (GetFocus() == link) {
      if (HWND next = GetNextDlgTabItem(st.dlg, link, FALSE); next && next != link) {
        SendMessageW(st.dlg, WM_NEXTDLGCTL, (WPARAM)next, TRUE);
      }
    }
    ShowWindow(link, SW_HIDE);
  }
}

// The rotation line's tooltip (the chip's tooltip control, tool 2), over the
// line while it shows and has something to explain (rotation_tip).
void update_rotation_tip(State& st) {
  if (!st.chip_tip) return;
  HWND h = GetDlgItem(st.dlg, IDC_ROTATION_SUMMARY);
  RECT r{};
  if (st.random && !st.rotation_tip_text.empty() && IsWindowVisible(h)) {
    GetWindowRect(h, &r);
    MapWindowPoints(nullptr, st.dlg, reinterpret_cast<POINT*>(&r), 2);
  }
  TOOLINFOW ti{sizeof(ti)};
  ti.hwnd = st.dlg;
  ti.uId = 2;
  ti.rect = r;
  SendMessageW(st.chip_tip, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
  ti.lpszText = const_cast<wchar_t*>(st.rotation_tip_text.c_str());
  SendMessageW(st.chip_tip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
}

// The rotation line under the list: "All 84 selected · 23 can run now" goes
// onto two lines, broken at the dot, when it doesn't fit on one.
void set_rotation_summary(State& st, std::wstring text) {
  HWND h = GetDlgItem(st.dlg, IDC_ROTATION_SUMMARY);
  for (size_t at; (at = text.find(L'\n')) != std::wstring::npos;) text.replace(at, 1, L" · ");
  const Rc& r = st.L.rotation_summary;
  if (!r.empty()) {
    HDC dc = GetDC(h);
    if (measure_text(dc, text, st.theme.fonts.caption).cx > r.w) {
      if (size_t at = text.find(L" · "); at != std::wstring::npos) text.replace(at, 3, L"\n");
    }
    ReleaseDC(h, dc);
  }
  if (window_text(h) != text) SetWindowTextW(h, text.c_str());
  if (!r.empty()) place_caption(st, h, r);
  update_rotation_tip(st);
}

// "Restore defaults": shown while the module has anything to restore,
// enabled once something differs from the catalog's defaults.
void update_defaults_button(State& st) {
  HWND b = GetDlgItem(st.dlg, IDC_PANEL_DEFAULTS);
  const Module* m = shown_module(st);
  const bool show = m && !welcome(st) &&
                    std::any_of(m->controls.begin(), m->controls.end(), [](const Control& c) { return c.settable(); });
  const bool enable = show && !at_defaults(st, *m);
  if (!enable && GetFocus() == b) SetFocus(st.list);   // don't strand the keyboard on a dead button
  if ((IsWindowEnabled(b) != FALSE) != enable) {
    EnableWindow(b, enable);
    InvalidateRect(b, nullptr, TRUE);
  }
  ShowWindow(b, show ? SW_SHOWNA : SW_HIDE);
}

void position_panel(State& st);
void update_group_names(State& st);
bool can_change_cover(const State& st);
void update_preview_button(State& st);
void place_list(State& st);
bool settle_list_top(State& st);

// The box-cover strip across the top, and beside it its status line with the
// "Show all" link under it, the two centred together on the covers
// (COVERS.md §1.2, §1.6). Hidden with fewer than two releases.
void place_strip(State& st) {
  HWND box = GetDlgItem(st.dlg, IDC_COVER_STRIP), status = GetDlgItem(st.dlg, IDC_STRIP_STATUS);
  HWND show_all = GetDlgItem(st.dlg, IDC_STRIP_SHOW_ALL), get_covers = GetDlgItem(st.dlg, IDC_STRIP_GET_COVERS);
  const bool on = st.strip_on && st.strip && st.L.strip_mode != StripMode::hidden;
  // Under the status line: "Show all" while a filter is on, else "Get the
  // covers" while a release still shows a generated cover (installs from
  // before covers, or offline imports: nothing fetches them on its own).
  const bool want_get = on && !st.filter_on && st.covers_missing;
  HWND link = st.filter_on || !want_get ? show_all : get_covers;
  HWND other = link == show_all ? get_covers : show_all;
  if (GetFocus() == other && IsWindowVisible(other)) SetFocus(st.strip ? st.strip->tile_hwnd(std::max(0, st.strip->home_tile())) : st.list);
  ShowWindow(other, SW_HIDE);
  if (!on) {
    for (HWND h : {box, status, link}) {
      if (GetFocus() == h || IsChild(h, GetFocus())) SetFocus(st.list);
      ShowWindow(h, SW_HIDE);
    }
    return;
  }
  const Theme& t = st.theme;
  const int fm = focus_margin(t.dpi);
  const Rc& a = st.L.strip;
  SetWindowPos(box, nullptr, a.x - fm, a.y - fm, a.w + 2 * fm, a.h + 2 * fm, SWP_NOZORDER | SWP_NOACTIVATE);
  st.strip->layout(st.L.strip_in, POINT{a.x - fm, a.y - fm});
  ShowWindow(box, SW_SHOWNA);
  // The status text (one or two caption lines) and, while a filter is on,
  // the link under it.
  const Rc& b = st.L.strip_status;
  HDC dc = GetDC(status);
  RECT m{0, 0, std::max(1, b.w), 0};
  HGDIOBJ old = SelectObject(dc, t.fonts.caption);
  const std::wstring text = window_text(status);
  DrawTextW(dc, text.c_str(), -1, &m, DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
  SelectObject(dc, old);
  const int link_text = measure_text(dc, without_mnemonic(window_text(link)), t.fonts.body).cx;
  ReleaseDC(status, dc);
  const int th = std::max(1, (int)(m.bottom - m.top)), lh = t.px(24), gap = t.px(4);
  const bool show_link = st.filter_on || want_get;
  if (link == get_covers) EnableWindow(get_covers, can_change_cover(st));   // one adimport at a time
  const int total = th + (show_link ? gap + lh : 0);
  const int y0 = b.y + (b.h - total) / 2;
  place(status, Rc{b.x, y0, b.w, th});
  ShowWindow(status, SW_SHOWNA);
  InvalidateRect(status, nullptr, TRUE);
  if (show_link) {
    const int pad = t.px(kLinkPad);
    place(link, Rc{b.x - pad, y0 + th + gap, std::min(b.w + pad, link_text + 2 * pad), lh}, fm);
    ShowWindow(link, SW_SHOWNA);
  } else {
    if (GetFocus() == link) SetFocus(st.strip->tile_hwnd(std::max(0, st.strip->home_tile())));
    ShowWindow(link, SW_HIDE);
  }
}

// The release chip's tooltip ("Also on: …"), over the chip itself: the
// static is transparent to the mouse, so the dialog holds the tool.
void update_chip_tip(State& st) {
  if (!st.chip_tip) return;
  TOOLINFOW ti{sizeof(ti)};
  ti.hwnd = st.dlg;
  ti.uId = 1;
  const Rc& b = st.L.module_badge;
  ti.rect = st.chip_text.empty() || welcome(st) ? RECT{0, 0, 0, 0} : RECT{b.x, b.y, b.right(), b.bottom()};
  SendMessageW(st.chip_tip, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
  ti.lpszText = const_cast<wchar_t*>(st.chip_text.c_str());
  SendMessageW(st.chip_tip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
}

void layout(State& st) {
  RECT cr{};
  GetClientRect(st.dlg, &cr);
  auto item = [&](int id) { return GetDlgItem(st.dlg, id); };
  // The rotation block (summary, Select all, Clear, "Change module every")
  // only in Random mode, and only when there is a list to rotate.
  const bool rows = ListView_GetItemCount(st.list) > 0;
  const bool rotation = st.random && rows;
  LayoutInput in{cr.right, cr.bottom, st.theme.dpi, rotation};
  // Under it, with several monitors: "A different module on each monitor".
  in.per_monitor = rotation && per_monitor_choice(st.random, monitors_all(st), st.monitors).shown;
  in.strip_tiles = st.strip_on && st.strip ? (int)st.strip->count() : 0;
  {
    // The links' text widths, so their text lines up with the card's edge.
    HDC dc = GetDC(st.dlg);
    auto dips = [&](int id) {
      SIZE sz = measure_text(dc, without_mnemonic(window_text(item(id))), st.theme.fonts.body);
      return (int)std::ceil(sz.cx * 96.0 / st.theme.dpi);
    };
    in.link_all_w = dips(IDC_CHECK_ALL);
    in.link_none_w = dips(IDC_CHECK_NONE);
    ReleaseDC(st.dlg, dc);
  }
  st.L = layout_window(in);
  // The module's name: the subtitle face when it fits beside the tile, else
  // the smaller face, else two lines of that (the layout makes room).
  {
    HWND title = item(IDC_MODULE_TITLE);
    const std::wstring text = window_text(title);
    const Fonts& f = st.theme.fonts;
    HDC dc = GetDC(title);
    st.title_font = f.subtitle;
    st.title_lines = 1;
    if (!welcome(st) && measure_text(dc, text, f.subtitle).cx > st.L.module_title.w) {
      st.title_font = f.subtitle_small;
      if (measure_text(dc, text, f.subtitle_small).cx > st.L.module_title.w) st.title_lines = 2;
    }
    ReleaseDC(title, dc);
    if (st.title_lines == 2) {
      in.title_lines = 2;
      st.L = layout_window(in);
    }
    InvalidateRect(title, nullptr, TRUE);
  }
  const WindowLayout& L = st.L;
  const int fm = focus_margin(st.theme.dpi);
  const bool hello = welcome(st);
  // Segmented pair: each half grows outward only, so they meet in the middle.
  {
    Rc a = L.mode_single, b = L.mode_random;
    SetWindowPos(item(IDC_MODE_SINGLE), nullptr, a.x - fm, a.y - fm, a.w + fm, a.h + 2 * fm, SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(item(IDC_MODE_RANDOM), nullptr, b.x, b.y - fm, b.w + fm, b.h + 2 * fm, SWP_NOZORDER | SWP_NOACTIVATE);
  }
  place(item(IDC_MODULES_LABEL), L.modules_label);
  place(item(IDC_MODULES_COUNT), L.modules_count);
  place_list(st);
  // Nothing to select, clear or rotate in an empty list.
  for (int id : {IDC_ROTATION_SUMMARY, IDC_CHECK_ALL, IDC_CHECK_NONE, IDC_DURATION_LABEL, IDC_DURATION}) {
    ShowWindow(item(id), rotation ? SW_SHOWNA : SW_HIDE);
  }
  {
    // Gone (a monitor unplugged): the keyboard moves on rather than stay on it.
    HWND pm = item(IDC_PER_MONITOR);
    if (!in.per_monitor && GetFocus() == pm) SetFocus(rotation ? item(IDC_DURATION) : st.list);
    ShowWindow(pm, in.per_monitor ? SW_SHOWNA : SW_HIDE);
  }
  const int combo_h = st.theme.px(32) + 2 * fm;
  const int drop_h = st.theme.px(32) * 8 + st.theme.px(8);
  auto place_combo = [&](int id, const Rc& r) {
    SetWindowPos(item(id), nullptr, r.x - fm, r.y - fm, r.w + 2 * fm, combo_h + drop_h, SWP_NOZORDER | SWP_NOACTIVATE);
  };
  if (rotation) {
    set_rotation_summary(st, window_text(item(IDC_ROTATION_SUMMARY)));
    place(item(IDC_CHECK_ALL), L.check_all, fm);
    place(item(IDC_CHECK_NONE), L.check_none, fm);
    place(item(IDC_DURATION_LABEL), L.duration_label);
    place_combo(IDC_DURATION, L.duration);
    if (in.per_monitor) place(item(IDC_PER_MONITOR), L.per_monitor, fm);
  }

  const int line = std::max(1, st.theme.body_line_height());
  if (hello) {
    // Not imported: one welcome across the details card. The night sky with
    // the moon and a toaster or two on top, then the name, what importing
    // does, and the button that does it.
    const Theme& t = st.theme;
    const Rc& card = L.details_card;
    const int ix = card.x + t.px(20), iw = card.w - t.px(40), iy = card.y + t.px(20), ib = card.bottom() - t.px(20);
    HWND about = item(IDC_ABOUT);
    const int margin = t.px(16);
    HDC dc = GetDC(about);
    RECT m{0, 0, std::max(1, iw - margin), 0};
    HGDIOBJ old = SelectObject(dc, t.fonts.body_gray);
    std::wstring text = window_text(about);
    DrawTextW(dc, text.c_str(), -1, &m, DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX | DT_CALCRECT);
    SelectObject(dc, old);
    ReleaseDC(about, dc);
    const int text_h = std::max(line, (int)(m.bottom - m.top) / line * line + ((m.bottom - m.top) % line ? line : 0));
    const int below = t.px(20) + t.px(28) + t.px(8) + text_h + t.px(16) + t.px(32);
    const int hero_h = std::clamp(ib - iy - below, t.px(120), iw * 9 / 16);
    place(st.preview, Rc{ix, iy, iw, hero_h});
    const int ty = iy + hero_h + t.px(20);
    place(item(IDC_MODULE_TITLE), Rc{ix, ty, iw, t.px(28)});
    place(item(IDC_MODULE_BADGE), Rc{});
    const int ay = ty + t.px(28) + t.px(8);
    const int ah = std::max(line, std::min(text_h, ib - t.px(48) - ay) / line * line);
    place_with_overlay_scrollbar(about, ix, ay, iw, ah);
    SendMessageW(about, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(0, margin));
    place(item(IDC_WELCOME_IMPORT), Rc{ix, ay + ah + t.px(16), t.px(176), t.px(32)}, fm);
  } else {
    place(st.preview, L.preview);
    place(item(IDC_MODULE_TITLE), L.module_title);
    place(item(IDC_MODULE_BADGE), L.module_badge);
    Rc about = L.about;
    if (st.has_credits) about.h = L.credits.y - dip(8, st.theme.dpi) - about.y;
    else about.h = L.credits.bottom() - about.y;
    // Whole lines only: no half-cut line at the bottom edge.
    about.h = std::max(line, about.h / line * line);
    place_with_overlay_scrollbar(item(IDC_ABOUT), about.x, about.y, about.w, about.h);
    // Text stops short of the overlay scroll bar.
    SendMessageW(item(IDC_ABOUT), EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(0, st.theme.px(16)));
  }
  live_preview_hero(st.preview, hello);
  ShowWindow(item(IDC_WELCOME_IMPORT), hello ? SW_SHOWNA : SW_HIDE);
  ShowWindow(st.panel, hello ? SW_HIDE : SW_SHOWNA);
  place(item(IDC_CREDITS), L.credits);
  ShowWindow(item(IDC_CREDITS), st.has_credits && !hello ? SW_SHOWNA : SW_HIDE);
  place_with_overlay_scrollbar(st.panel, L.panel.x, L.panel.y, L.panel.w, L.panel.h);
  place(item(IDC_PANEL_DEFAULTS), L.defaults, fm);
  update_defaults_button(st);

  place(item(IDC_SCALE_LABEL), L.scale_label);
  place(item(IDC_MONITORS_LABEL), L.monitors_label);
  place_combo(IDC_SCALE, L.scale);
  place_combo(IDC_MONITORS, L.monitors);
  place(item(IDC_STRETCH), L.stretch, fm);
  place(item(IDC_SOUND_LABEL), L.sound_label);
  place_combo(IDC_SOUND, L.sound);
  place(item(IDC_VOLUME_LABEL), L.volume_label);
  place(item(IDC_VOLUME_VALUE), L.volume_value);
  place(item(IDC_VOLUME), L.volume, fm);
  place(item(IDC_SOUND_NOTE), L.sound_note);
  // Until something is imported the welcome's own button is the one Import;
  // the assets line takes the footer's place.
  place(item(IDC_IMPORT), L.import, fm);
  ShowWindow(item(IDC_IMPORT), hello ? SW_HIDE : SW_SHOWNA);
  place(item(IDC_PREVIEW), L.preview_button, fm);
  place(item(IDOK), L.ok, fm);
  place(item(IDCANCEL), L.cancel, fm);
  place_assets_status(st);
  place_strip(st);
  update_chip_tip(st);
  // Exactly the width that shows: nothing past the column is left for the
  // list view to paint itself.
  ListView_SetColumnWidth(st.list, 0, std::max(1, L.list.w));
  // A new height may have moved the list's scroll position: nothing cut at its top.
  if (rows) settle_list_top(st);
  position_panel(st);
  InvalidateRect(st.dlg, nullptr, TRUE);
}

// ---- module-settings panel -----------------------------------------------------------

// A module button's row: the saver never presses module buttons (ABI.md
// §2.10.4), so the row says what it is instead of offering a dead button.
constexpr wchar_t kReadOnlyNote[] = L"Not available in this version";
constexpr wchar_t kReadOnlyWhy[] = L"The original module sets this in a window of its own, which this version can’t open.";

void update_slider_label(State& st, int slot, const Control& c, int value) {
  SetDlgItemTextW(st.panel, IDC_PANEL_BASE + slot * IDC_PANEL_STRIDE + IDC_PART_VALUE, widen(c.value_label(value)).c_str());
}

// The panel shows whole rows only. It scrolls from row top to row top; a
// row that doesn't fit below the fold is moved out of sight (not hidden, so
// Tab still reaches it and scrolls it in), and the panel paints the top of
// the first such row faded, as a sign there is more below.
void position_panel(State& st) {
  if (!st.panel) return;
  const Module* m = shown_module(st);
  const int view = st.L.panel.h, width = st.L.panel.w;
  const int dpi = st.theme.dpi, fm = focus_margin(dpi);
  static const std::vector<Control> kNone;
  const std::vector<Control>& controls = m && !welcome(st) ? m->controls : kNone;
  // Which module buttons are live (the host can open them).
  std::vector<bool> live(controls.size(), false);
  bool any_live = false;
  for (size_t i = 0; i < controls.size(); ++i) {
    live[i] = controls[i].type == ControlType::button && button_live(st, st.shown);
    any_live |= live[i];
  }
  // A note may take two lines in a narrow column: room for the longest one
  // it can show (the read-only row's, a run's outcome, the "Custom" hint).
  auto note_h = [&](int w) {
    HDC dc = GetDC(st.panel);
    HGDIOBJ old = SelectObject(dc, st.theme.fonts.caption);
    std::vector<std::wstring> texts = {kReadOnlyNote};
    if (any_live && m) {
      texts.push_back(configure_outcome_note(0xC0000005));
      for (const Control& c : controls)
        if (c.type == ControlType::button) texts.push_back(button_note(st, *m, c));
      if (const Control* p = custom_popup(*m)) {
        std::wstring label = widen(p->name);
        texts.push_back(L"Choose “Custom” under " + label + L": to show it");
      }
    }
    int h = 0;
    for (const std::wstring& t : texts) {
      RECT r{0, 0, std::max(1, w - st.theme.px(18)), 0};
      DrawTextW(dc, t.c_str(), -1, &r, DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
      h = std::max(h, (int)(r.bottom - r.top));
    }
    SelectObject(dc, old);
    ReleaseDC(st.panel, dc);
    return h;
  };
  // Room for the focus ring on both sides of every control.
  int cw = std::max(1, width - 2 * fm);
  PanelLayout P = layout_panel(controls, cw, dpi, note_h(cw), &live);
  const bool scroll = !controls.empty() && P.content_h + 2 * fm > view;
  if (scroll) {
    cw = std::max(1, width - 2 * fm - st.theme.px(12));   // and for the scroll thumb
    P = layout_panel(controls, cw, dpi, note_h(cw), &live);
  }
  // A live button is as wide as its text needs (within the column).
  if (any_live) {
    HDC dc = GetDC(st.panel);
    for (size_t i = 0; i < P.rows.size() && i < controls.size(); ++i) {
      if (!live[i]) continue;
      PanelRow& row = P.rows[i];
      const int want = measure_text(dc, widen(controls[i].name), st.theme.fonts.body).cx +
                       st.theme.px((int)kPanelButtonPadDip);
      row.input.w = std::min(row.input.w, std::max(want, st.theme.px((int)kPanelButtonMinDip)));
    }
    ReleaseDC(st.panel, dc);
  }
  // A value readout is as wide as its longest possible text; the label gets the rest.
  if (!controls.empty()) {
    HDC dc = GetDC(st.panel);
    for (size_t i = 0; i < P.rows.size() && i < controls.size(); ++i) {
      PanelRow& row = P.rows[i];
      if (row.value.empty()) continue;
      const Control& c = controls[i];
      if (c.type != ControlType::slider) continue;   // a live button's note is not a readout
      int widest = 0;
      if (c.stepped()) {
        for (const auto& item : c.items) widest = std::max<int>(widest, measure_text(dc, widen(item), st.theme.fonts.body).cx);
      } else {
        for (int v : {c.min, c.max}) widest = std::max<int>(widest, measure_text(dc, widen(c.value_label(v)), st.theme.fonts.body).cx);
      }
      const int right = row.value.right();
      const int vw = std::min(row.value.right() - row.label.x - st.theme.px(48), widest + st.theme.px(2));
      row.value.x = right - vw;
      row.value.w = vw;
      row.label.w = row.value.x - st.theme.px(8) - row.label.x;
    }
    ReleaseDC(st.panel, dc);
  }
  // Scroll stops: the row tops, up to the first from which the rest all fit.
  const int avail = view - 2 * fm;
  st.panel_stops.clear();
  if (scroll) {
    for (const PanelRow& r : P.rows) {
      st.panel_stops.push_back(r.top);
      if (P.content_h - r.top <= avail) break;
    }
  }
  if (st.panel_stops.empty()) {
    st.scroll = 0;
  } else {
    int snapped = st.panel_stops.front();
    for (int s : st.panel_stops) {
      if (s <= st.scroll) snapped = s;
    }
    st.scroll = snapped;
  }
  ShowScrollBar(st.panel, SB_VERT, scroll);
  if (scroll) {
    // The thumb reaches the end of its track at the last stop.
    SCROLLINFO si{sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS, 0, st.panel_stops.back() + view - 1, (UINT)view, st.scroll, 0};
    SetScrollInfo(st.panel, SB_VERT, &si, TRUE);
  }
  const int ox = fm, oy = fm - st.scroll;
  st.peek_row = -1;
  auto put = [&](int id, Rc r, int grow, int push) {
    HWND h = GetDlgItem(st.panel, id);
    if (!h) return;
    if (r.empty()) {
      // A label with nothing to show (a blank catalog name) stays, at no
      // size, for the control's accessible name.
      SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_NOZORDER | SWP_NOACTIVATE);
      return;
    }
    r.x += ox;
    r.y += oy + push;
    wchar_t cls[32] = {};
    GetClassNameW(h, cls, 32);
    if (wcscmp(cls, WC_COMBOBOXW) == 0) {
      SetWindowPos(h, nullptr, r.x - grow, r.y - grow, r.w + 2 * grow, r.h + 2 * grow + st.theme.px(32) * 8,
                   SWP_NOZORDER | SWP_NOACTIVATE);
    } else {
      place(h, r, grow);
    }
  };
  for (size_t slot = 0; slot < P.rows.size(); ++slot) {
    const PanelRow& row = P.rows[slot];
    // Out of sight unless the whole row (focus ring included) shows.
    const bool fits = row.bottom + oy + fm <= view;
    if (!fits && st.peek_row < 0) st.peek_row = (int)slot;
    const int push = fits ? 0 : view + st.theme.px(64);
    const int base = IDC_PANEL_BASE + (int)slot * IDC_PANEL_STRIDE;
    put(base + IDC_PART_LABEL, row.label, 0, push);
    put(base + IDC_PART_INPUT, row.input, controls[slot].type == ControlType::button && !live[slot] ? 0 : fm, push);
    put(base + IDC_PART_VALUE, row.value, 0, push);
    if (st.tip && controls[slot].type == ControlType::button && !live[slot]) {
      TOOLINFOW ti{sizeof(ti)};
      ti.hwnd = st.panel;
      ti.uId = slot + 1;
      ti.rect = fits ? RECT{ox, row.top + oy, ox + width, row.bottom + oy} : RECT{0, 0, 0, 0};
      SendMessageW(st.tip, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
    }
  }
  if (!P.empty_note.empty()) put(IDC_PANEL_EMPTY, P.empty_note, 0, 0);
  // "Restore defaults" just under the last row when they all fit; at the
  // column's foot (layout) when they scroll.
  {
    Rc d = st.L.defaults;
    if (!scroll && !controls.empty()) d.y = std::min(d.y, st.L.panel.y + fm + P.content_h + st.theme.px(8));
    place(GetDlgItem(st.dlg, IDC_PANEL_DEFAULTS), d, fm);
  }
  st.P = P;
  InvalidateRect(st.panel, nullptr, TRUE);
}

// The faded top of the first row below the fold (its label, or a checkbox's
// box and text, or a dropdown's face), painted by the panel itself.
void paint_panel_peek(State& st, HDC dc) {
  const Module* m = shown_module(st);
  if (!m || st.peek_row < 0 || st.peek_row >= (int)st.P.rows.size() || st.peek_row >= (int)m->controls.size()) return;
  const Theme& t = st.theme;
  const Palette& p = t.pal;
  const PanelRow& row = st.P.rows[st.peek_row];
  const Control& c = m->controls[st.peek_row];
  RECT pc{};
  GetClientRect(st.panel, &pc);
  const int fm = focus_margin(t.dpi), view = st.L.panel.h;
  int ox = fm, oy = fm - st.scroll;
  // Its natural place, or when that is past the fold, the fold itself (as
  // long as that leaves a gap under the last row that shows).
  const int lowest = view - t.px(12);
  if (row.top + oy > lowest) {
    const int prev_bottom = st.peek_row > 0 ? st.P.rows[st.peek_row - 1].bottom + oy : 0;
    if (lowest < prev_bottom + t.px(4)) return;
    oy = lowest - row.top;
  }
  const int top = row.top + oy;
  RECT band{0, top, pc.right, std::min(view, top + t.px(20))};
  HRGN clip = CreateRectRgn(band.left, band.top, band.right, band.bottom);
  SelectClipRgn(dc, clip);
  if (!row.label.empty()) {
    RECT lr{row.label.x + ox, row.label.y + oy, row.label.right() + ox, row.label.bottom() + oy};
    draw_text(dc, label_text(c.name), lr, t.fonts.body, p.text,
              DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
  } else if (c.type == ControlType::checkbox) {
    const int box = t.px(20);
    const int y = row.input.y + oy + (row.input.h - box) / 2, x = row.input.x + ox;
    RECT b{x, y, x + box, y + box};
    stroke_round(dc, b, t.pxf(4), p.strong_stroke, (float)t.hairline());
    RECT tr{b.right + t.px(8), row.input.y + oy, row.input.right() + ox, row.input.bottom() + oy};
    draw_text(dc, label_text(c.name), tr, t.fonts.body, p.text, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
  } else if (!row.input.empty()) {
    RECT b{row.input.x + ox, row.input.y + oy, row.input.right() + ox, row.input.bottom() + oy};
    draw_control_body(dc, b, t.pxf(4), p.control, p.control_stroke, p.control_stroke_bottom);
  }
  SelectClipRgn(dc, nullptr);
  DeleteObject(clip);
  fade_rect(dc, band, p.card, 110, 255);
}

// Scrolls the panel to the stop at or before `pos` (px), or to the next one down.
void scroll_panel_to(State& st, int pos) {
  if (st.panel_stops.empty()) return;
  int snapped = st.panel_stops.front();
  for (int s : st.panel_stops) {
    if (s <= pos) snapped = s;
  }
  if (snapped == st.scroll) return;
  st.scroll = snapped;
  position_panel(st);
}

int panel_stop_index(const State& st) {
  for (size_t i = 0; i < st.panel_stops.size(); ++i) {
    if (st.panel_stops[i] == st.scroll) return (int)i;
  }
  return 0;
}

void scroll_panel_rows(State& st, int rows) {
  if (st.panel_stops.empty()) return;
  const int i = std::clamp(panel_stop_index(st) + rows, 0, (int)st.panel_stops.size() - 1);
  scroll_panel_to(st, st.panel_stops[i]);
}

// Tab (or a click) reached a row out of sight: bring it in whole.
void ensure_row_visible(State& st, int slot) {
  if (st.panel_stops.empty() || slot < 0 || slot >= (int)st.P.rows.size()) return;
  const PanelRow& r = st.P.rows[slot];
  const int avail = st.L.panel.h - 2 * focus_margin(st.theme.dpi);
  if (r.top >= st.scroll && r.bottom - st.scroll <= avail) return;
  int target = st.panel_stops.back();
  if (r.top < st.scroll) {
    target = r.top;
  } else {
    for (int s : st.panel_stops) {
      if (r.bottom - s <= avail) {
        target = s;
        break;
      }
    }
  }
  scroll_panel_to(st, std::min(target, st.panel_stops.back()));
}

void CALLBACK focus_event(HWINEVENTHOOK, DWORD, HWND h, LONG obj, LONG, DWORD, DWORD) {
  State* st = g_state;
  if (!st || !st->panel || obj != OBJID_CLIENT || !h || GetParent(h) != st->panel) return;
  const int rel = GetDlgCtrlID(h) - IDC_PANEL_BASE;
  if (rel >= 0) ensure_row_visible(*st, rel / IDC_PANEL_STRIDE);
}

void build_panel(State& st, int module_index) {
  SendMessageW(st.panel, WM_SETREDRAW, FALSE, 0);
  forget_looks(st.panel);
  for (HWND c = GetWindow(st.panel, GW_CHILD); c;) {
    HWND next = GetWindow(c, GW_HWNDNEXT);
    DestroyWindow(c);
    c = next;
  }
  st.scroll = 0;
  if (st.tip) {
    for (UINT_PTR id : st.tip_tools) {
      TOOLINFOW ti{sizeof(ti)};
      ti.hwnd = st.panel;
      ti.uId = id;
      SendMessageW(st.tip, TTM_DELTOOL, 0, (LPARAM)&ti);
    }
  }
  st.tip_tools.clear();
  const Theme& t = st.theme;
  auto add = [&](const wchar_t* cls, const std::wstring& text, DWORD style, int id, HFONT font) {
    HWND c = CreateWindowExW(0, cls, text.c_str(), WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, st.panel,
                             (HMENU)(INT_PTR)id, st.hinst, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)font, FALSE);
    set_surface(c, Surface::card);
    keep_own_layout(c);
    return c;
  };
  if (module_index >= 0 && module_index < (int)st.catalog.modules.size()) {
    const Module& m = st.catalog.modules[module_index];
    for (size_t slot = 0; slot < m.controls.size(); ++slot) {
      const Control& c = m.controls[slot];
      int base = IDC_PANEL_BASE + (int)slot * IDC_PANEL_STRIDE;
      int v = control_value(st, m, c);
      std::wstring name = label_text(c.name);
      switch (c.type) {
        case ControlType::slider: {
          // A string slider ("Never / Rarely / Often / Always") moves between
          // its stops, like the original control panel's, with repeated
          // labels shown as one stop (ui_model.h: VisualStops); the trackbar
          // position is the stop, the value stored is the catalog's.
          const bool stepped = c.stepped();
          add(WC_STATICW, name, SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, base + IDC_PART_LABEL, t.fonts.body);
          HWND tb = add(TRACKBAR_CLASSW, name, TBS_HORZ | TBS_NOTICKS | TBS_BOTH | WS_TABSTOP, base + IDC_PART_INPUT,
                        t.fonts.body);
          int ticks = 0;
          if (stepped) {
            const VisualStops vs = visual_stops(c);
            ticks = vs.count();
            SendMessageW(tb, TBM_SETRANGEMIN, FALSE, 0);
            SendMessageW(tb, TBM_SETRANGEMAX, FALSE, std::max(0, vs.count() - 1));
            SendMessageW(tb, TBM_SETPAGESIZE, 0, 1);
            SendMessageW(tb, TBM_SETPOS, TRUE, vs.run_of_value(c, v));
          } else {
            SendMessageW(tb, TBM_SETRANGEMIN, FALSE, std::min(c.min, c.max));
            SendMessageW(tb, TBM_SETRANGEMAX, FALSE, std::max(c.min, c.max));
            SendMessageW(tb, TBM_SETPAGESIZE, 0, std::max(1, std::abs(c.max - c.min) / 10));
            SendMessageW(tb, TBM_SETPOS, TRUE, v);
          }
          subclass_trackbar(tb, &st.theme, ticks);
          add(WC_STATICW, widen(c.value_label(v)), SS_RIGHT | SS_NOPREFIX | SS_ENDELLIPSIS, base + IDC_PART_VALUE,
              t.fonts.body);
          break;
        }
        case ControlType::checkbox: {
          HWND cb = add(WC_BUTTONW, name, BS_AUTOCHECKBOX | WS_TABSTOP, base + IDC_PART_INPUT, t.fonts.body);
          SendMessageW(cb, BM_SETCHECK, v ? BST_CHECKED : BST_UNCHECKED, 0);
          break;
        }
        case ControlType::popup: {
          // A blank catalog name (Flocks' "Birds / Polliwogs / …") shows no
          // label; screen readers still get one.
          add(WC_STATICW, blank_label(c.name) ? std::wstring(L"Style") : name, SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS,
              base + IDC_PART_LABEL, t.fonts.body);
          HWND combo = add(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP,
                           base + IDC_PART_INPUT, t.fonts.body);
          subclass_combo(combo, &st.theme);
          for (const auto& item : c.items) SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)widen(item).c_str());
          SendMessageW(combo, CB_SETCURSEL, (WPARAM)(v - c.min), 0);
          size_combo(combo, t, t.px(32) + 2 * focus_margin(t.dpi));
          break;
        }
        case ControlType::button:
          if (button_live(st, module_index)) {
            // The module's own button (INTERACTION.md §6.3): the host runs
            // its handler, and the module's dialogs open owned by this window.
            std::wstring text = widen(c.name);
            for (size_t p = 0; (p = text.find(L'&', p)) != std::wstring::npos; p += 2) text.insert(p, 1, L'&');
            HWND b = add(WC_BUTTONW, text, BS_PUSHBUTTON | WS_TABSTOP, base + IDC_PART_INPUT, t.fonts.body);
            set_button_role(b, ButtonRole::standard);
            // Under it, what the last run left to say (or nothing).
            add(WC_STATICW, button_note(st, m, c), SS_OWNERDRAW, base + IDC_PART_VALUE, t.fonts.caption);
            if (st.tip) {
              TOOLINFOW ti{sizeof(ti)};
              ti.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
              ti.hwnd = st.panel;
              ti.uId = (UINT_PTR)b;
              ti.lpszText = const_cast<wchar_t*>(kButtonWhy);
              SendMessageW(st.tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
              st.tip_tools.push_back((UINT_PTR)b);
            }
            break;
          }
          // The original control panel's buttons open the module's own
          // dialogs; without a host that can open them here, a read-only
          // row: the name, and a (disabled) note saying so.
          add(WC_STATICW, name, SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, base + IDC_PART_LABEL, t.fonts.body);
          add(WC_STATICW, kReadOnlyNote, SS_OWNERDRAW | WS_DISABLED, base + IDC_PART_INPUT, t.fonts.caption);
          // Why, when the pointer rests on the row (placed by position_panel).
          if (st.tip) {
            TOOLINFOW ti{sizeof(ti)};
            ti.uFlags = TTF_SUBCLASS;
            ti.hwnd = st.panel;
            ti.uId = slot + 1;
            ti.lpszText = const_cast<wchar_t*>(kReadOnlyWhy);
            SendMessageW(st.tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
            st.tip_tools.push_back(slot + 1);
          }
          break;
        default:
          add(WC_STATICW, name + L" (not adjustable here)", SS_LEFT | SS_NOPREFIX, base + IDC_PART_LABEL, t.fonts.body);
          break;
      }
    }
    const bool live = button_live(st, module_index);
    bool any_settable = std::any_of(m.controls.begin(), m.controls.end(), [&](const Control& c) {
      return c.settable() || (live && c.type == ControlType::button);
    });
    if (!any_settable) add(WC_STATICW, L"This module has no settings.", SS_LEFT | SS_NOPREFIX, IDC_PANEL_EMPTY, t.fonts.body);
  }
  position_panel(st);
  update_defaults_button(st);
  SendMessageW(st.panel, WM_SETREDRAW, TRUE, 0);
  RedrawWindow(st.panel, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

// ---- details card ----------------------------------------------------------------------

void show_details(State& st, int module_index) {
  st.shown = module_index;
  std::wstring title, badge, about, credits;
  st.badge_kind = BadgeKind::neutral;
  st.chip_text.clear();
  if (module_index >= 0 && module_index < (int)st.catalog.modules.size()) {
    const Module& m = st.catalog.modules[module_index];
    title = widen(m.name);
    // A neutral chip with the release's short title; the lane never shows
    // (COVERS.md §1.7). Other releases with the same bytes are its tooltip.
    if (m.release >= 0 && m.release < (int)st.catalog.releases.size()) badge = widen(st.catalog.releases[m.release].short_title);
    st.chip_text = also_on_tip(also_on(st.catalog, module_index));
    if (!is_present(st, module_index)) {
      badge += badge.empty() ? L"File missing" : L" · File missing";
      st.badge_kind = BadgeKind::critical;
    } else if (coming_soon(st, m)) {
      badge += L" · Coming soon";
      st.badge_kind = BadgeKind::caution;
    }
    about = widen(tidy_about(m.about, m.name));
    credits = widen(m.credits);
    // "By …\n ©1994 …": one tidy line each.
    std::wstring tidy;
    for (size_t i = 0; i < credits.size(); ++i) {
      if (credits[i] == L'\n') {
        while (!tidy.empty() && tidy.back() == L' ') tidy.pop_back();
        tidy += L'\n';
        while (i + 1 < credits.size() && credits[i + 1] == L' ') ++i;
      } else if (!(credits[i] == L' ' && !tidy.empty() && tidy.back() == L' ')) {
        tidy += credits[i];
      }
    }
    credits = tidy;
  } else if (st.catalog.modules.empty()) {
    title = L"Welcome to Long After Dark";
    about = welcome_text();
  } else if (st.model.shown == 0) {
    // The strip's filter leaves no rows (a release with no modules listed).
    title = L"No modules to show";
    about = L"The releases selected above have no modules. Select other covers, or choose Show all.";
  } else {
    title = L"No module selected";
    about = L"Choose a module from the list to see it here.";
  }
  SetDlgItemTextW(st.dlg, IDC_MODULE_TITLE, title.c_str());
  SetDlgItemTextW(st.dlg, IDC_MODULE_BADGE, badge.c_str());
  SetDlgItemTextW(st.dlg, IDC_ABOUT, crlf(about).c_str());
  SetDlgItemTextW(st.dlg, IDC_CREDITS, credits.c_str());
  st.has_credits = !credits.empty();
  // The layout follows the welcome's text, whether there are credits, and
  // how much room the module's name needs.
  layout(st);
  build_panel(st, module_index);
  refresh_preview(st);
  update_preview_button(st);
  RECT icon{st.L.module_icon.x, st.L.module_icon.y, st.L.module_icon.right(), st.L.module_icon.bottom()};
  InvalidateRect(st.dlg, &icon, FALSE);
  InvalidateRect(GetDlgItem(st.dlg, IDC_MODULE_BADGE), nullptr, TRUE);
}

// ---- module list ------------------------------------------------------------------------

void add_group(HWND list, int id, const wchar_t* title) {
  LVGROUP g{};
  g.cbSize = sizeof(g);
  g.mask = LVGF_HEADER | LVGF_GROUPID;
  g.pszHeader = const_cast<wchar_t*>(title);
  g.iGroupId = id;
  ListView_InsertGroup(list, -1, &g);
}

void update_summary(State& st) {
  // Random's line and its links count the rows shown (COVERS.md §1.8).
  const ShownChecks sc = shown_checks(st);
  const int total = (int)sc.shown;
  // How many of the checked modules can run now (the host may lack a lane or
  // a module ABI, or a module exited 3).
  long long runnable = 0;
  for (const ListGroup& g : st.model.groups) {
    for (const ListRow& r : g.rows) {
      const Module& m = st.catalog.modules[r.module];
      if (st.checks.count(m.id) && is_present(st, r.module) && !coming_soon(st, m)) ++runnable;
    }
  }
  // How many different modules those checks are: byte-identical copies on
  // several releases play once per pass (effective_rotation, COVERS.md §1.8).
  std::set<std::string> keys;
  for (const ListGroup& g : st.model.groups) {
    for (const ListRow& r : g.rows) {
      const Module& m = st.catalog.modules[r.module];
      if (st.checks.count(m.id)) keys.insert(same_as_key(m));
    }
  }
  const long long distinct = (long long)keys.size();
  set_rotation_summary(st, rotation_summary(sc.checked, sc.shown, runnable, distinct));
  const int lead = module_index(st, lead_id(st));
  st.rotation_tip_text =
      rotation_tip(sc.checked, distinct, lead >= 0 ? widen(st.catalog.modules[lead].display_name) : std::wstring());
  update_rotation_tip(st);
  SetDlgItemTextW(st.dlg, IDC_MODULES_COUNT, modules_count_label(sc.shown, st.catalog.modules.size()).c_str());
  // "Select all" and "Clear" only while they would change something.
  const int n = (int)sc.checked;
  HWND all = GetDlgItem(st.dlg, IDC_CHECK_ALL), none = GetDlgItem(st.dlg, IDC_CHECK_NONE);
  for (auto [b, on] : {std::pair{all, st.random && n < total}, std::pair{none, st.random && n > 0}}) {
    if ((IsWindowEnabled(b) != FALSE) == on) continue;
    if (!on && GetFocus() == b) SetFocus(st.list);
    EnableWindow(b, on);
    InvalidateRect(b, nullptr, TRUE);
  }
  // The groups' own checkboxes follow their rows, and so do their names.
  update_group_names(st);
  if (st.random) InvalidateRect(st.list, nullptr, FALSE);
}

// ---- list placement ----------------------------------------------------------------------
// The list view scrolls itself (LVM_SCROLL, SB_LINE*) only by a "line" of its
// own reckoning, which is neither the rows' pitch nor a group header's
// height, so a list placed with it opens on a cut row. It is placed instead
// with LVM_ENSUREVISIBLE on a row above the view, which brings exactly that
// row's top to the top edge. A row near the end that the list can't scroll
// that far for is reached by letting the list stop short of its card's foot
// (st.list_trim, less than a row).

struct ListGeom {
  struct Edge {
    int top, bottom;   // content px (the list scrolled to its start)
    int item;          // list item, or -1 for a group header
    int group;         // the header's group id
  };
  int pos = 0, view = 0, content = 0;   // scroll position, visible height, total height (px)
  std::vector<Edge> edges;              // rows and group headers, top to bottom
  int max_pos() const { return std::max(0, content - view); }
  const Edge* at_top() const {          // what straddles the top edge, if anything
    for (const Edge& e : edges) {
      if (e.top < pos && e.bottom > pos) return &e;
    }
    return nullptr;
  }
};

ListGeom list_geom(const State& st) {
  ListGeom g;
  RECT cr{};
  GetClientRect(st.list, &cr);
  g.view = cr.bottom;
  SCROLLINFO si{sizeof(si), SIF_ALL};
  if ((GetWindowLongW(st.list, GWL_STYLE) & WS_VSCROLL) && GetScrollInfo(st.list, SB_VERT, &si)) {
    g.pos = si.nPos;
    g.content = si.nMax + 1;
  }
  for (int i = 0, n = ListView_GetItemCount(st.list); i < n; ++i) {
    RECT r{};
    if (ListView_GetItemRect(st.list, i, &r, LVIR_BOUNDS)) g.edges.push_back({r.top + g.pos, r.bottom + g.pos, i, 0});
  }
  for (const ListGroup& lg : st.model.groups) {
    const int id = group_id(lg.release);
    if (!ListView_HasGroup(st.list, id)) continue;
    RECT r{}, all{};
    if (ListView_GetGroupRect(st.list, id, LVGGR_HEADER, &r)) g.edges.push_back({r.top + g.pos, r.bottom + g.pos, -1, id});
    if (ListView_GetGroupRect(st.list, id, LVGGR_GROUP, &all)) g.content = std::max<int>(g.content, all.bottom + g.pos);
  }
  std::sort(g.edges.begin(), g.edges.end(), [](const ListGeom::Edge& a, const ListGeom::Edge& b) { return a.top < b.top; });
  for (const auto& e : g.edges) g.content = std::max(g.content, e.bottom);
  return g;
}

void scroll_list_to(HWND list, bool end) { ListView_Scroll(list, 0, end ? (1 << 20) : -(1 << 20)); }

// Row `item`'s top to the top edge (the list must be able to scroll that far).
void align_row_top(State& st, int item) {
  scroll_list_to(st.list, true);                 // the row is now at or above the top edge
  ListView_EnsureVisible(st.list, item, FALSE);  // a row above the view comes down to the edge exactly
}

// Whatever straddles the top edge, a row or a group header, is brought into
// view whole: a row by its own top, a header by the row before it.
bool settle_list_top(State& st) {
  for (int pass = 0; pass < 4; ++pass) {
    ListGeom g = list_geom(st);
    const ListGeom::Edge* cut = g.at_top();
    if (!cut) return true;
    if (cut->item >= 0) {
      ListView_EnsureVisible(st.list, cut->item, FALSE);
      continue;
    }
    int prev = -1;
    for (const auto& e : g.edges) {
      if (e.item >= 0 && e.bottom <= cut->top) prev = e.item;
    }
    if (prev >= 0) ListView_EnsureVisible(st.list, prev, FALSE);
    else scroll_list_to(st.list, false);
  }
  return list_geom(st).at_top() == nullptr;
}

void place_list(State& st) {
  const Rc& r = st.L.list;
  place_with_overlay_scrollbar(st.list, r.x, r.y, r.w, std::max(1, r.h - st.list_trim));
}

// Shows `item`: from the start of the list when it is in view there (so
// every screen opens the list the same way), else with it about mid-list;
// either way with nothing cut at the top edge.
void position_list(State& st, int item) {
  if (st.list_trim) {
    st.list_trim = 0;
    place_list(st);
  }
  scroll_list_to(st.list, false);
  if (item < 0) return;
  ListGeom g = list_geom(st);
  const ListGeom::Edge* it = nullptr;
  for (const auto& e : g.edges) {
    if (e.item == item) it = &e;
  }
  if (!it || it->bottom <= g.view - st.theme.px(8)) return;
  // The row to put at the top: the one nearest to centring the item, among
  // those that show it whole and that the list can scroll to.
  const int want = it->top - (g.view - (it->bottom - it->top)) / 2;
  int top_item = -1, best = INT_MAX;
  for (const auto& e : g.edges) {
    if (e.item < 0 || e.top > it->top || it->bottom - e.top > g.view || e.top > g.max_pos()) continue;
    if (std::abs(e.top - want) < best) {
      best = std::abs(e.top - want);
      top_item = e.item;
    }
  }
  if (top_item < 0) {
    // Near the end: the first row from which the item shows whole, reached
    // by stopping the list that much short of the card's foot.
    for (const auto& e : g.edges) {
      if (e.item >= 0 && e.top <= it->top && it->bottom - e.top <= g.view) {
        top_item = e.item;
        st.list_trim = std::max(0, e.top - g.max_pos());
        place_list(st);
        break;
      }
    }
  }
  if (top_item >= 0) align_row_top(st, top_item);
  settle_list_top(st);
}

// The catalog row at the list's top edge (the first one whose top is in view), or -1.
int top_row_module(const State& st) {
  ListGeom g = list_geom(st);
  for (const auto& e : g.edges) {
    if (e.item < 0 || e.top < g.pos) continue;
    LVITEMW it{};
    it.mask = LVIF_PARAM;
    it.iItem = e.item;
    return ListView_GetItem(st.list, &it) ? (int)it.lParam : -1;
  }
  return -1;
}

// Fills the list from the catalog under the strip's filter: one group per
// release, the global checks, and the chosen module selected when its
// release is shown. Else no row is selected, the chosen module stays chosen
// (Single still saves it), and the details show a module the list does show
// (details_after_filter: the one shown before while it is listed, else the
// first row, else the "no modules" state).
// `keep_view`: a filter change, which keeps the scroll position while the
// selected row is still shown, and the details (and live preview) while
// their module is still listed.
// A row's item text: what screen readers read and type-ahead matches (the
// row itself is drawn by draw_row): its name, "(missing)" for a missing file,
// and in Random ", plays first" for the module the file names to lead.
std::wstring row_item_text(const State& st, int module) {
  std::wstring text = module < (int)st.row_label.size() ? st.row_label[module] : std::wstring();
  if (!is_present(st, module)) text += L"  (missing)";
  else if (st.random && st.catalog.modules[module].id == lead_id(st)) text += L", plays first";
  return text;
}

// The lead's row after a change of mode (only its text depends on it).
void refresh_lead_row(State& st) {
  const int lead = module_index(st, lead_id(st));
  if (lead < 0) return;
  const int item = item_for_module(st, lead);
  if (item < 0) return;
  std::wstring text = row_item_text(st, lead);
  ListView_SetItemText(st.list, item, 0, text.data());
}

void populate_list(State& st, bool keep_view = false) {
  const int top_before = keep_view ? top_row_module(st) : -1;
  st.populating = true;
  SendMessageW(st.list, WM_SETREDRAW, FALSE, 0);
  ListView_DeleteAllItems(st.list);
  ListView_RemoveAllGroups(st.list);
  ListView_EnableGroupView(st.list, TRUE);

  st.model = build_list(st.catalog, current_filter(st));
  st.full_order = build_list(st.catalog, {}).order();
  st.row_label.assign(st.catalog.modules.size(), std::wstring());
  const int chosen = module_index(st, st.chosen);
  int select_item = -1, n = 0;
  for (const ListGroup& g : st.model.groups) {
    const std::wstring title = widen(st.catalog.releases[g.release].title);
    add_group(st.list, group_id(g.release), title.c_str());
    for (const ListRow& r : g.rows) {
      const Module& m = st.catalog.modules[r.module];
      st.row_label[r.module] = widen(r.label);
      std::wstring text = row_item_text(st, r.module);
      LVITEMW it{};
      it.mask = LVIF_TEXT | LVIF_PARAM | LVIF_GROUPID;
      it.iItem = n++;
      it.pszText = text.data();
      it.lParam = r.module;
      it.iGroupId = group_id(g.release);
      int pos = ListView_InsertItem(st.list, &it);
      ListView_SetCheckState(st.list, pos, st.checks.count(m.id) ? TRUE : FALSE);
      if (r.module == chosen) select_item = pos;
    }
  }
  ListView_SetColumnWidth(st.list, 0, std::max(1, st.L.list.w));
  if (select_item < 0 && chosen < 0 && !st.model.groups.empty()) {
    // Nothing chosen yet: what the user sees first, the first row of the first group.
    select_item = item_for_module(st, st.model.groups.front().rows.front().module);
    st.chosen = st.catalog.modules[st.model.groups.front().rows.front().module].id;
  }
  if (select_item >= 0) {
    ListView_SetItemState(st.list, select_item, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
  }
  st.populating = false;
  SendMessageW(st.list, WM_SETREDRAW, TRUE, 0);
  const int top_item = top_before >= 0 && st.model.shows(top_before) ? item_for_module(st, top_before) : -1;
  if (keep_view && select_item >= 0 && top_item >= 0) {
    // The same rows at the top as before, nothing cut at the edge.
    scroll_list_to(st.list, false);
    align_row_top(st, top_item);
    settle_list_top(st);
  } else {
    position_list(st, select_item);
  }
  InvalidateRect(st.list, nullptr, TRUE);
  update_summary(st);
  const int details = details_after_filter(st.model, module_index(st, st.chosen), keep_view ? st.shown : -1);
  if (!keep_view || details != st.shown) show_details(st, details);
}

// The list's client area that shows (its native scroll bar is hidden past it).
RECT list_visible_rect(const State& st) {
  RECT cr{};
  GetClientRect(st.list, &cr);
  if (st.L.list.w > 0) cr.right = std::min<LONG>(cr.right, st.L.list.w);
  return cr;
}

// The parts of a module row, for painting and for hit-testing its checkbox.
struct RowParts {
  RECT row{}, box{}, icon{}, text{};
};

// The list can scroll: its overlay scroll bar runs down the right edge.
bool list_scrolls(const State& st) {
  SCROLLINFO si{sizeof(si), SIF_RANGE | SIF_PAGE};
  return (GetWindowLongW(st.list, GWL_STYLE) & WS_VSCROLL) && GetScrollInfo(st.list, SB_VERT, &si) &&
         si.nPage > 0 && si.nMax - si.nMin + 1 > (int)si.nPage;
}

RowParts row_parts(const State& st, const RECT& bounds) {
  const Theme& t = st.theme;
  RowParts p;
  // The highlight stops short of the scroll bar's gutter, when there is one.
  const int right_inset = list_scrolls(st) ? t.px(12) : t.px(4);
  p.row = RECT{bounds.left + t.px(4), bounds.top + t.px(2), bounds.right - right_inset, bounds.bottom - t.px(2)};
  const int cy = (p.row.top + p.row.bottom) / 2;
  int x = p.row.left + t.px(12);
  if (st.random) {
    const int b = t.px(kBoxDip);
    p.box = RECT{x, cy - b / 2, x + b, cy - b / 2 + b};
    x += b + t.px(12);
  }
  const int ic = t.px(kIconDip);
  p.icon = RECT{x, cy - ic / 2, x + ic, cy - ic / 2 + ic};
  x += ic + t.px(12);
  p.text = RECT{x, p.row.top, p.row.right - t.px(12), p.row.bottom};
  return p;
}

// A group's rotation state: how many of its modules are checked, of how many.
struct GroupChecks {
  int checked = 0, total = 0;
};
GroupChecks group_checks(const State& st, int group) {
  GroupChecks g;
  for (int i = 0, n = ListView_GetItemCount(st.list); i < n; ++i) {
    LVITEMW it{};
    it.mask = LVIF_PARAM | LVIF_GROUPID;
    it.iItem = i;
    if (!ListView_GetItem(st.list, &it) || it.iGroupId != group) continue;
    ++g.total;
    g.checked += ListView_GetCheckState(st.list, i) ? 1 : 0;
  }
  return g;
}

// Checks or clears every row of a group (a release's "check all").
void set_group_checks(State& st, int group, bool on) {
  for (int i = 0, n = ListView_GetItemCount(st.list); i < n; ++i) {
    LVITEMW it{};
    it.mask = LVIF_GROUPID;
    it.iItem = i;
    if (ListView_GetItem(st.list, &it) && it.iGroupId == group) ListView_SetCheckState(st.list, i, on ? TRUE : FALSE);
  }
  InvalidateRect(st.list, nullptr, FALSE);
}

// What screen readers call a group (its LVGROUP header; the list's own
// painting of it is covered by draw_group_headers): the release's title and,
// in Random mode, its group checkbox's state, "Totally Twisted After Dark, 4
// of 13 in rotation" ("all 13", "none"; a release of one module, Marvel
// Comics Screen Posters, "1 in rotation", as the rotation line words it).
std::wstring group_name(const State& st, const ListGroup& lg) {
  std::wstring name = widen(st.catalog.releases[lg.release].title);
  if (!st.random) return name;
  const GroupChecks gc = group_checks(st, group_id(lg.release));
  if (gc.total == 0) return name;
  return name + L", " +
         (gc.checked == gc.total ? (gc.total == 1 ? L"" : L"all ") + std::to_wstring(gc.total) + L" in rotation"
          : gc.checked == 0      ? std::wstring(L"none in rotation")
                                 : std::to_wstring(gc.checked) + L" of " + std::to_wstring(gc.total) + L" in rotation");
}

void update_group_names(State& st) {
  if (!st.list || st.populating) return;
  for (const ListGroup& lg : st.model.groups) {
    const int id = group_id(lg.release);
    if (!ListView_HasGroup(st.list, id)) continue;
    const std::wstring name = group_name(st, lg);
    wchar_t now[256] = {};
    LVGROUP g{};
    g.cbSize = sizeof(g);
    g.mask = LVGF_HEADER;
    g.pszHeader = now;
    g.cchHeader = 256;
    ListView_GetGroupInfo(st.list, id, &g);
    if (name == now) continue;
    LVGROUP set{};
    set.cbSize = sizeof(set);
    set.mask = LVGF_HEADER;
    set.pszHeader = const_cast<wchar_t*>(name.c_str());
    ListView_SetGroupInfo(st.list, id, &set);
  }
}

// The group-wide checkbox at a header's left (Random mode), where a row's own sits.
RECT group_box(const State& st, const RECT& header) {
  const int b = st.theme.px(kBoxDip), x = header.left + st.theme.px(16);
  const int cy = (header.top + header.bottom) / 2;
  return RECT{x, cy - b / 2, x + b, cy - b / 2 + b};
}

int first_group(const State& st) {
  int first = -1, best = INT_MAX;
  for (const ListGroup& lg : st.model.groups) {
    const int id = group_id(lg.release);
    RECT r{};
    if (ListView_HasGroup(st.list, id) && ListView_GetGroupRect(st.list, id, LVGGR_HEADER, &r) && r.top < best) {
      best = r.top;
      first = id;
    }
  }
  return first;
}

// The whole title of a group header drawn ellipsized, as its tooltip: a tool
// of chip_tip on the list (id kHeaderTipBase + the group's), over the header
// (`r`, list client px); an empty `r` puts it out of reach. Only what changed
// is sent, since every paint of the list passes here.
constexpr UINT_PTR kHeaderTipBase = 100;
void set_header_tip(State& st, int group, const RECT& r, const std::wstring& title) {
  if (!st.chip_tip || !st.list) return;
  TOOLINFOW ti{sizeof(ti)};
  ti.hwnd = st.list;
  ti.uId = kHeaderTipBase + (UINT_PTR)group;
  auto it = st.header_tips.find(group);
  if (it == st.header_tips.end()) {
    if (IsRectEmpty(&r)) return;
    ti.uFlags = TTF_SUBCLASS;
    ti.rect = r;
    ti.lpszText = const_cast<wchar_t*>(title.c_str());
    SendMessageW(st.chip_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    st.header_tips[group] = {r, title};
    return;
  }
  if (!EqualRect(&it->second.first, &r)) {
    ti.rect = r;
    SendMessageW(st.chip_tip, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
    it->second.first = r;
  }
  if (!title.empty() && it->second.second != title) {
    ti.lpszText = const_cast<wchar_t*>(title.c_str());
    SendMessageW(st.chip_tip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
    it->second.second = title;
  }
}

// Group headers: the list view sends no custom draw for them, so they are
// painted over its own at the end of each paint (it paints into its double
// buffer, so nothing flickers). Every group after the first is set apart by
// a hairline in the gap above it; in Random mode a checkbox at the left
// checks or clears the whole group (and shows a dash while it is mixed).
void draw_group_headers(State& st, HDC dc) {
  const Theme& t = st.theme;
  const Palette& p = t.pal;
  RECT cr = list_visible_rect(st);
  const int first = first_group(st);
  std::set<int> tipped;   // headers in view this time (the others lose their tooltip)
  for (const ListGroup& lg : st.model.groups) {
    const int id = group_id(lg.release);
    RECT r{};
    if (!ListView_HasGroup(st.list, id) || !ListView_GetGroupRect(st.list, id, LVGGR_HEADER, &r)) continue;
    if (id != first) {
      const int y = r.top - t.px(kGroupGapDip) / 2 - t.hairline() / 2;
      RECT line{cr.left + t.px(16), y, cr.right - t.px(16), y + t.hairline()};
      if (line.bottom > cr.top && line.top < cr.bottom) fill_rect(dc, line, p.high_contrast ? p.text3 : p.card_stroke);
    }
    if (r.bottom <= cr.top || r.top >= cr.bottom) continue;
    r.left = cr.left;
    r.right = cr.right;
    fill_rect(dc, r, p.card);
    // The release's title; every one of its modules being one this host
    // can't run yet makes the whole release "Coming soon".
    const std::wstring name = widen(st.catalog.releases[lg.release].title);
    const bool all_soon = std::all_of(lg.rows.begin(), lg.rows.end(),
                                      [&](const ListRow& row) { return coming_soon(st, st.catalog.modules[row.module]); });
    const GroupChecks gc = group_checks(st, id);
    if (st.random && gc.total) {
      RECT b = group_box(st, r);
      const float rad = t.pxf(4);
      if (gc.checked) {
        fill_round(dc, b, rad, p.accent);
        if (gc.checked == gc.total) {
          draw_check(dc, b, p.on_accent, 1.6f * t.dpi / 96.0f);
        } else {
          const int cy = (b.top + b.bottom) / 2, hw = t.px(5);
          const int cx = (b.left + b.right) / 2, th = std::max(2, t.px(2));
          RECT dash{cx - hw, cy - th / 2, cx + hw, cy - th / 2 + th};
          fill_round(dc, dash, th / 2.0f, p.on_accent);
        }
      } else {
        fill_round(dc, b, rad, p.control);
        stroke_round(dc, b, rad, p.strong_stroke, (float)t.hairline());
      }
    }
    // The count, and the "Coming soon" pill at the right, always show whole:
    // a title too long for what is left ("Star Wars Screen Entertainment" in
    // the narrowest list) is ellipsized (layout_group_header), and the whole
    // title is its tooltip; screen readers read it whole anyway (group_name).
    const std::wstring count = std::to_wstring(gc.total), soon = L"Coming soon";
    const bool pill = all_soon && !lg.rows.empty();
    // The frame: the title after the group's checkbox (Random), the count,
    // the pill 8 DIP short of the scroll bar's gutter (group_header_frame).
    GroupHeaderInput hin = group_header_frame(r.right - r.left, t.dpi, st.random && gc.total, list_scrolls(st));
    hin.left += r.left;
    hin.right += r.left;
    hin.pill_right += r.left;
    const RECT tr{hin.left, r.top, hin.right, r.bottom};
    hin.title = name;
    hin.count_w = measure_text(dc, count, t.fonts.caption).cx;
    hin.pill_w = pill ? measure_text(dc, soon, t.fonts.caption).cx + t.px(16) : 0;
    const GroupHeaderLayout hl =
        layout_group_header(hin, [&](const std::wstring& s) { return (int)measure_text(dc, s, t.fonts.body_strong).cx; });
    // Unclipped: the list view's header box can be shorter than the line at
    // high DPI, and release titles have descenders ("Anniversary"); the
    // header is painted after the rows, over the card's own margin under it.
    draw_text(dc, hl.title, tr, t.fonts.body_strong, p.text, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_NOCLIP);
    RECT cnt{hl.count_x, tr.top, pill ? hl.pill_x : tr.right, tr.bottom};
    draw_text(dc, count, cnt, t.fonts.caption, p.text2, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    if (pill) {
      const int bh = t.px(20), cy = (tr.top + tr.bottom) / 2;
      RECT b{hl.pill_x, cy - bh / 2, hin.pill_right, cy - bh / 2 + bh};
      draw_badge(dc, b, soon, BadgeKind::caution, t, p.card);
    }
    set_header_tip(st, id, hl.ellipsized ? r : RECT{}, name);
    tipped.insert(id);
    st.header_drawn[id] = hl.title;
  }
  // Headers scrolled out of view (or gone with a filter) have no tooltip.
  std::vector<int> out_of_view;
  for (const auto& [id, tip] : st.header_tips) {
    if (!tipped.count(id)) out_of_view.push_back(id);
  }
  for (int id : out_of_view) set_header_tip(st, id, RECT{}, L"");
}

void draw_row(State& st, NMLVCUSTOMDRAW* cd) {
  const Theme& t = st.theme;
  const Palette& p = t.pal;
  const int item = (int)cd->nmcd.dwItemSpec;
  RECT bounds{};
  ListView_GetItemRect(st.list, item, &bounds, LVIR_BOUNDS);
  RECT cr = list_visible_rect(st);
  bounds.left = cr.left;
  bounds.right = cr.right;
  if (bounds.bottom <= bounds.top || bounds.right <= bounds.left) return;
  HDC target = cd->nmcd.hdc;
  // Off-screen, then one copy.
  HDC dc = CreateCompatibleDC(target);
  HBITMAP bmp = CreateCompatibleBitmap(target, bounds.right - bounds.left, bounds.bottom - bounds.top);
  HGDIOBJ old = SelectObject(dc, bmp);
  SetViewportOrgEx(dc, -bounds.left, -bounds.top, nullptr);
  fill_rect(dc, bounds, p.card);

  const int mi = (int)cd->nmcd.lItemlParam;
  const bool valid = mi >= 0 && mi < (int)st.catalog.modules.size();
  const UINT state = ListView_GetItemState(st.list, item, LVIS_SELECTED | LVIS_FOCUSED);
  const bool selected = state & LVIS_SELECTED;
  const bool hot = (cd->nmcd.uItemState & CDIS_HOT) != 0;
  const bool list_focus = GetFocus() == st.list;
  RowParts rp = row_parts(st, bounds);
  if (selected || hot) {
    COLORREF f = selected ? (hot ? p.row_selected_hover : p.row_selected) : p.row_hover;
    fill_round(dc, rp.row, t.pxf(4), f);
  }
  if (selected && !p.high_contrast) {
    const int ph = t.px(16), pw = std::max(2, t.px(3));
    const int cy = (rp.row.top + rp.row.bottom) / 2;
    RECT pill{rp.row.left, cy - ph / 2, rp.row.left + pw, cy - ph / 2 + ph};
    fill_round(dc, pill, pw / 2.0f, p.accent);
  }
  if (valid) {
    const Module& m = st.catalog.modules[mi];
    const bool missing = !is_present(st, mi), soon = coming_soon(st, m);
    const bool dim = missing || soon;
    if (st.random) {
      const bool checked = ListView_GetCheckState(st.list, item) != 0;
      // Under high contrast the selected row is itself the highlight colour.
      const bool inverted = p.high_contrast && selected;
      // A module that can't run keeps its check (it plays once it can) but
      // at the row's dimmed strength.
      const bool faint = dim && !p.high_contrast;
      const COLORREF under = selected ? (hot ? p.row_selected_hover : p.row_selected) : hot ? p.row_hover : p.card;
      if (checked) {
        COLORREF f = inverted ? p.on_accent : p.accent;
        if (faint) f = blend(f, under, 0.55);
        fill_round(dc, rp.box, t.pxf(4), f);
        draw_check(dc, rp.box, inverted ? p.accent : faint ? blend(p.on_accent, f, 0.25) : p.on_accent,
                   1.6f * t.dpi / 96.0f);
      } else {
        fill_round(dc, rp.box, t.pxf(4), p.control);
        stroke_round(dc, rp.box, t.pxf(4), faint ? p.text3 : p.strong_stroke, (float)t.hairline());
      }
    }
    st.icons.draw(dc, m, st.win_dir, rp.icon, dim);
    RECT tr = rp.text;
    // Modules this host can't run yet are dimmed; the group header and the
    // details say why. Only a missing file gets a badge of its own.
    // In Random, the module the file names to play first (lead_id) says so.
    std::wstring badge = missing ? L"Missing" : st.random && m.id == lead_id(st) ? kPlaysFirst : L"";
    if (!badge.empty()) {
      SIZE bs = measure_text(dc, badge, t.fonts.caption);
      const int bw = bs.cx + t.px(16), bh = t.px(20);
      const int cy = (rp.row.top + rp.row.bottom) / 2;
      RECT b{tr.right - bw, cy - bh / 2, tr.right, cy - bh / 2 + bh};
      COLORREF under = selected ? (hot ? p.row_selected_hover : p.row_selected) : hot ? p.row_hover : p.card;
      draw_badge(dc, b, badge, missing ? BadgeKind::critical : BadgeKind::neutral, t, under);
      tr.right = b.left - t.px(8);
    }
    COLORREF ink = p.high_contrast && selected ? p.on_accent : dim ? p.text2 : p.text;
    draw_text(dc, mi < (int)st.row_label.size() ? st.row_label[mi] : widen(m.name), tr, t.fonts.body, ink, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
  }
  if ((state & LVIS_FOCUSED) && (list_focus || focused_window() == st.list) && keyboard_cues(st.list)) {
    draw_focus_ring(dc, rp.row, t.pxf(4), p, t.dpi / 96.0f);
  }
  SetViewportOrgEx(dc, 0, 0, nullptr);
  BitBlt(target, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top, dc, 0, 0, SRCCOPY);
  SelectObject(dc, old);
  DeleteObject(bmp);
  DeleteDC(dc);
}

// Not imported: a few faint rows the shape of the real ones, and a line
// saying what will fill them (the welcome beside it says how). The same
// rows when the strip's filter leaves none, saying so.
void draw_empty_list(State& st, HDC dc) {
  const Theme& t = st.theme;
  const Palette& p = t.pal;
  RECT cr{};
  GetClientRect(st.list, &cr);
  fill_rect(dc, cr, p.card);
  RECT vis = list_visible_rect(st);
  const int row = t.px(kRowH), rows = 4, gap = t.px(16);
  const wchar_t* line = welcome(st) ? L"Your modules appear here after import" : L"No modules in the releases selected";
  SIZE cap = measure_text(dc, line, t.fonts.caption);
  const int block = rows * row + gap + cap.cy;
  int y = vis.top + std::max(0, ((int)(vis.bottom - vis.top) - block) / 2 - t.px(24));
  const int x = vis.left + t.px(16), avail = vis.right - vis.left - t.px(16) - t.px(28) - t.px(12) - t.px(24);
  static const float kBar[rows] = {0.62f, 0.44f, 0.54f, 0.36f};
  for (int i = 0; i < rows; ++i, y += row) {
    // Each a little fainter than the one before.
    const double k = (p.high_contrast ? 0.5 : p.dark ? 0.10 : 0.07) * (1.0 - 0.18 * i);
    const COLORREF ghost = blend(p.card, p.text, k);
    const int cy = y + row / 2, ic = t.px(kIconDip);
    RECT tile{x, cy - ic / 2, x + ic, cy - ic / 2 + ic};
    fill_round(dc, tile, t.pxf(6), ghost);
    const int bh = t.px(10);
    RECT bar{tile.right + t.px(12), cy - bh / 2, tile.right + t.px(12) + (int)(avail * kBar[i]), cy - bh / 2 + bh};
    fill_round(dc, bar, bh / 2.0f, ghost);
  }
  RECT a{vis.left + t.px(16), y + gap, vis.right - t.px(16), y + gap + cap.cy};
  draw_text(dc, line, a, t.fonts.caption, p.text2, DT_CENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
}

LRESULT list_custom_draw(State& st, NMLVCUSTOMDRAW* cd) {
  switch (cd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
      if (ListView_GetItemCount(st.list) == 0) {
        draw_empty_list(st, cd->nmcd.hdc);
        return CDRF_SKIPDEFAULT;
      }
      return CDRF_NOTIFYITEMDRAW | CDRF_NOTIFYPOSTPAINT;
    case CDDS_ITEMPREPAINT:
      draw_row(st, cd);
      return CDRF_SKIPDEFAULT;
    case CDDS_POSTPAINT: {
      // The list view paints a sliver of its own along its last two pixel
      // columns (in the top padding and the gaps between rows); cover it.
      RECT vis = list_visible_rect(st);
      RECT strip{vis.right - st.theme.px(2), vis.top, vis.right, vis.bottom};
      fill_rect(cd->nmcd.hdc, strip, st.theme.pal.card);
      draw_group_headers(st, cd->nmcd.hdc);
      paint_overlay_scrollbar(st.list, cd->nmcd.hdc);
      return CDRF_DODEFAULT;
    }
  }
  return CDRF_DODEFAULT;
}

// The release of the list's group under `client` (its header or one of its
// rows), -1 when there is none.
int release_at(const State& st, POINT client) {
  const RECT vis = list_visible_rect(st);
  for (const ListGroup& lg : st.model.groups) {
    RECT r{};
    if (!ListView_HasGroup(st.list, group_id(lg.release)) ||
        !ListView_GetGroupRect(st.list, group_id(lg.release), LVGGR_HEADER, &r))
      continue;
    r.left = vis.left;
    r.right = vis.right;
    if (PtInRect(&r, client)) return lg.release;
  }
  LVHITTESTINFO hi{};
  hi.pt = client;
  const int item = ListView_SubItemHitTest(st.list, &hi);
  if (item < 0) return -1;
  LVITEMW it{};
  it.mask = LVIF_PARAM;
  it.iItem = item;
  if (!ListView_GetItem(st.list, &it) || it.lParam < 0 || it.lParam >= (LPARAM)st.catalog.modules.size()) return -1;
  return st.catalog.modules[it.lParam].release;
}

// Random mode's list menu: "Check all in <release>" / "Clear all in
// <release>", the keyboard's way to a group's own checkbox. false when there
// is no release to offer it for.
bool list_context_menu(State& st, LPARAM lp) {
  int release = -1;
  POINT at{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
  if (lp == -1) {
    // From the keyboard: the focused row's release, under that row's text.
    int item = ListView_GetNextItem(st.list, -1, LVNI_FOCUSED);
    if (item < 0) item = ListView_GetNextItem(st.list, -1, LVNI_SELECTED);
    if (item < 0) return false;
    LVITEMW it{};
    it.mask = LVIF_PARAM;
    it.iItem = item;
    if (!ListView_GetItem(st.list, &it) || it.lParam < 0 || it.lParam >= (LPARAM)st.catalog.modules.size()) return false;
    release = st.catalog.modules[it.lParam].release;
    RECT r{};
    ListView_GetItemRect(st.list, item, &r, LVIR_BOUNDS);
    at = POINT{r.left + st.theme.px(48), r.bottom};
    ClientToScreen(st.list, &at);
  } else {
    POINT client = at;
    ScreenToClient(st.list, &client);
    release = release_at(st, client);
  }
  const ListGroup* lg = st.model.group(release);
  if (!lg || release < 0 || release >= (int)st.catalog.releases.size()) return false;
  const GroupChecks gc = group_checks(st, group_id(release));
  const std::wstring name = widen(st.catalog.releases[release].short_title);
  HMENU menu = CreatePopupMenu();
  enum : UINT { kCheckAll = 1, kClearAll = 2 };
  const std::wstring check = L"Check all in " + name, clear = L"Clear all in " + name;
  AppendMenuW(menu, MF_STRING | (gc.checked < gc.total ? 0 : MF_GRAYED), kCheckAll, check.c_str());
  AppendMenuW(menu, MF_STRING | (gc.checked > 0 ? 0 : MF_GRAYED), kClearAll, clear.c_str());
  const UINT cmd = (UINT)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN, at.x, at.y, 0,
                                        st.dlg, nullptr);
  DestroyMenu(menu);
  if (cmd == kCheckAll || cmd == kClearAll) {
    set_group_checks(st, group_id(release), cmd == kCheckAll);
    log_line("dialog: %s all in %s", cmd == kCheckAll ? "check" : "clear", st.catalog.releases[release].id.c_str());
  }
  return true;
}

// Clicks on a row's checkbox (Random mode) toggle it; elsewhere they select.
LRESULT CALLBACK list_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
  auto* st = reinterpret_cast<State*>(ref);
  switch (msg) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
      if (!st->random) break;
      LVHITTESTINFO hi{};
      hi.pt = {(short)LOWORD(lp), (short)HIWORD(lp)};
      // A group's own checkbox: every module of the release, checked or
      // cleared at once.
      for (const ListGroup& lg : st->model.groups) {
        const int id = group_id(lg.release);
        RECT r{};
        if (!ListView_HasGroup(h, id) || !ListView_GetGroupRect(h, id, LVGGR_HEADER, &r)) continue;
        r.left = list_visible_rect(*st).left;
        RECT hit = group_box(*st, r);
        InflateRect(&hit, st->theme.px(6), st->theme.px(4));
        if (!PtInRect(&hit, hi.pt)) continue;
        const GroupChecks gc = group_checks(*st, id);
        SetFocus(h);
        set_group_checks(*st, id, gc.checked < gc.total);
        return 0;
      }
      int item = ListView_SubItemHitTest(h, &hi);
      if (item < 0 || !(hi.flags & LVHT_ONITEM)) break;
      RECT bounds{};
      ListView_GetItemRect(h, item, &bounds, LVIR_BOUNDS);
      RECT cr = list_visible_rect(*st);
      bounds.left = cr.left;
      bounds.right = cr.right;
      RowParts rp = row_parts(*st, bounds);
      RECT hit = rp.box;
      InflateRect(&hit, st->theme.px(6), (rp.row.bottom - rp.row.top - (hit.bottom - hit.top)) / 2);
      if (!PtInRect(&hit, hi.pt)) break;
      SetFocus(h);
      ListView_SetCheckState(h, item, !ListView_GetCheckState(h, item));
      return 0;
    }
    case WM_CONTEXTMENU:
      // Random mode: a release's "check all" from the keyboard (Shift+F10, the
      // Apps key) or a right-click, for the group under the pointer or the
      // focused row's.
      if (st->random && list_context_menu(*st, lp)) return 0;
      break;
    case WM_NCDESTROY:
      RemoveWindowSubclass(h, list_proc, 3);
      break;
  }
  return DefSubclassProc(h, msg, wp, lp);
}

// ---- assets, mode ----------------------------------------------------------------------

// Preview runs the module the details show: not while one runs already, not
// while they show none (nothing imported, or a filter that leaves no rows),
// and not for a module this host can't run yet (or whose file is gone).
void update_preview_button(State& st) {
  const Module* shown = shown_module(st);
  const bool unrunnable = shown && (coming_soon(st, *shown) || !is_present(st, st.shown) || being_removed(st, *shown));
  HWND b = GetDlgItem(st.dlg, IDC_PREVIEW);
  const bool on = !st.preview_running && shown && !unrunnable;
  if ((IsWindowEnabled(b) != FALSE) == on) return;
  if (!on && GetFocus() == b) SetFocus(st.list);
  EnableWindow(b, on);
  InvalidateRect(b, nullptr, TRUE);
}

void update_assets_status(State& st) {
  std::wstring text = assets_summary(count_assets(st.catalog, st.present));
  // The outcome of an import that changed nothing replaces the (unchanged)
  // module count until the next one.
  if (!st.import_note.empty()) text = st.import_note;
  if (st.import_running) text = L"Importing…";
  SetDlgItemTextW(st.dlg, IDC_ASSETS_STATUS, text.c_str());
  place_assets_status(st);
  // One adimport at a time: Import… is greyed while a cover changes or a
  // release is being removed too.
  const bool busy = st.import_running || st.cover_running || st.remove_running;
  EnableWindow(GetDlgItem(st.dlg, IDC_IMPORT), !busy);
  EnableWindow(GetDlgItem(st.dlg, IDC_STRIP_GET_COVERS), can_change_cover(st));
  InvalidateRect(GetDlgItem(st.dlg, IDC_STRIP_GET_COVERS), nullptr, TRUE);
  update_preview_button(st);
  // Until there is something to choose, importing is the thing to do: the
  // welcome's own button says so (the footer's Import is the same command).
  const bool empty = st.catalog.modules.empty();
  EnableWindow(GetDlgItem(st.dlg, IDC_WELCOME_IMPORT), !busy);
  set_button_role(GetDlgItem(st.dlg, IDOK), empty ? ButtonRole::standard : ButtonRole::accent);
  // Nothing to choose between until there are modules.
  for (int id : {IDC_MODE_SINGLE, IDC_MODE_RANDOM}) {
    if ((IsWindowEnabled(GetDlgItem(st.dlg, id)) != FALSE) == !empty) continue;
    EnableWindow(GetDlgItem(st.dlg, id), !empty);
    InvalidateRect(GetDlgItem(st.dlg, id), nullptr, TRUE);
  }
  for (int id : {IDC_IMPORT, IDOK, IDC_ASSETS_STATUS, IDC_WELCOME_IMPORT}) InvalidateRect(GetDlgItem(st.dlg, id), nullptr, TRUE);
}

// ---- sound (AUDIO.md §9) ---------------------------------------------------------------

void update_volume_value(State& st) {
  const int v = (int)SendDlgItemMessageW(st.dlg, IDC_VOLUME, TBM_GETPOS, 0, 0);
  SetDlgItemTextW(st.dlg, IDC_VOLUME_VALUE, std::to_wstring(v).c_str());
}

// Volume is for the sound there is: greyed while Sound is Off (its label
// then has no mnemonic either, which would send the focus past it).
void update_sound(State& st) {
  st.sound_on = SendDlgItemMessageW(st.dlg, IDC_SOUND, CB_GETCURSEL, 0, 0) != 1;
  HWND vol = GetDlgItem(st.dlg, IDC_VOLUME);
  if ((IsWindowEnabled(vol) != FALSE) != st.sound_on) {
    if (!st.sound_on && GetFocus() == vol) SetFocus(GetDlgItem(st.dlg, IDC_SOUND));
    EnableWindow(vol, st.sound_on);
  }
  SetDlgItemTextW(st.dlg, IDC_VOLUME_LABEL, st.sound_on ? L"&Volume" : L"Volume");
  for (int id : {IDC_VOLUME_LABEL, IDC_VOLUME_VALUE, IDC_VOLUME}) InvalidateRect(GetDlgItem(st.dlg, id), nullptr, TRUE);
}

// "A different module on each monitor" (ui_model.h: per_monitor_choice),
// which layout() shows in Random on a PC with several monitors: greyed,
// keeping its check, while only the primary monitor plays.
void update_per_monitor(State& st) {
  HWND pm = GetDlgItem(st.dlg, IDC_PER_MONITOR);
  const bool on = per_monitor_choice(st.random, monitors_all(st), st.monitors).enabled;
  if ((IsWindowEnabled(pm) != FALSE) == on) return;
  if (!on && GetFocus() == pm) SetFocus(GetDlgItem(st.dlg, IDC_MONITORS));
  EnableWindow(pm, on);
  InvalidateRect(pm, nullptr, TRUE);
}

void update_mode(State& st) {
  st.random = IsDlgButtonChecked(st.dlg, IDC_MODE_RANDOM) == BST_CHECKED;
  refresh_lead_row(st);
  EnableWindow(GetDlgItem(st.dlg, IDC_DURATION), st.random);
  update_per_monitor(st);
  layout(st);
  update_preview_button(st);
  // The rotation row changes the list's height: show the selection afresh.
  position_list(st, ListView_GetNextItem(st.list, -1, LVNI_SELECTED));
  update_summary(st);
  InvalidateRect(st.list, nullptr, TRUE);
  for (int id : {IDC_MODE_SINGLE, IDC_MODE_RANDOM, IDC_DURATION_LABEL}) InvalidateRect(GetDlgItem(st.dlg, id), nullptr, TRUE);
}

void load_catalog_into(State& st) {
  st.win_dir = win_assets_dir();
  std::string err;
  if (!load_catalog(catalog_path(), st.catalog, &err)) log_line("dialog: catalog: %s", err.c_str());
  st.present.clear();
  for (const auto& m : st.catalog.modules) st.present.push_back(file_exists(resolve_module_path(st.win_dir, m.path)));
  st.icons.clear();
}

// ---- the box-cover strip (COVERS.md §1) ----------------------------------------------

// The status line is a polite live region: Narrator reads each new status.
void announce(State& st, HWND status) {
  if (!st.live_region) {
    // LiveSetting_Property_GUID, CLSID_AccPropServices, IID_IAccPropServices.
    static const GUID kLiveSetting = {0xc12bcd8e, 0x2a8e, 0x4950, {0x8a, 0xe7, 0x36, 0x25, 0x11, 0x1d, 0x58, 0xeb}};
    static const GUID kClsid = {0xb5f8350b, 0x0548, 0x48b1, {0xa6, 0xee, 0x88, 0xbd, 0x00, 0xb4, 0xa5, 0xe7}};
    static const GUID kIid = {0x6e26e776, 0x04f0, 0x495d, {0x80, 0xe4, 0x33, 0x30, 0x35, 0x2e, 0x31, 0x69}};
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IAccPropServices* props = nullptr;
    if (SUCCEEDED(CoCreateInstance(kClsid, nullptr, CLSCTX_INPROC_SERVER, kIid, reinterpret_cast<void**>(&props))) && props) {
      VARIANT v;
      VariantInit(&v);
      v.vt = VT_I4;
      v.lVal = 1;   // Polite
      st.live_region = SUCCEEDED(props->SetHwndProp(status, (DWORD)OBJID_CLIENT, (DWORD)CHILDID_SELF, kLiveSetting, v));
      props->Release();
    }
    if (SUCCEEDED(co)) CoUninitialize();
  }
  NotifyWinEvent(0x8019 /* EVENT_OBJECT_LIVEREGIONCHANGED */, status, OBJID_CLIENT, CHILDID_SELF);
}

// The status line beside the strip, and whether a filter is on (some, but
// not all, releases selected).
void update_strip_status(State& st, bool announce_change = true) {
  const size_t k = st.strip ? st.strip->selected_count() : 0, n = st.strip ? st.strip->count() : 0;
  st.filter_on = st.strip_on && filter_active(k, n);
  HWND status = GetDlgItem(st.dlg, IDC_STRIP_STATUS);
  const std::wstring text = st.strip_on ? strip_status(k, n) : std::wstring();
  if (window_text(status) != text) {
    SetWindowTextW(status, text.c_str());
    if (st.strip_on && announce_change) announce(st, status);
  }
  InvalidateRect(status, nullptr, TRUE);
}

// The tiles for the catalog's releases: shown with two or more, each
// selected when `selected` names it.
void load_strip(State& st, const std::vector<std::string>& selected) {
  st.strip_on = strip_shown(st.catalog);
  st.covers_missing = std::any_of(st.catalog.releases.begin(), st.catalog.releases.end(),
                                  [](const Release& r) { return r.cover.generated(); });
  if (!st.strip) return;
  std::vector<StripTile> tiles;
  std::vector<std::string> on;
  if (st.strip_on) {
    for (size_t i = 0; i < st.catalog.releases.size(); ++i) {
      const Release& r = st.catalog.releases[i];
      const size_t n = st.catalog.modules_in((int)i);
      StripTile t;
      t.id = r.id;
      t.title = widen(r.title);
      t.short_title = widen(r.short_title);
      t.name = tile_name(r, n);
      t.tip = tile_tip(r, n);
      if (!r.cover.generated()) {
        t.tile_path = resolve_module_path(st.win_dir, r.cover.tile);
        t.tile_md5 = r.cover.tile_md5;
      }
      tiles.push_back(std::move(t));
      if (std::find(selected.begin(), selected.end(), r.id) != selected.end()) on.push_back(r.id);
    }
  }
  st.strip->set_tiles(tiles, on);
  update_strip_status(st, false);
}

// A tile, "Show only" or "Show all": the list regroups under the new filter,
// keeping every check, the chosen module and (while its row still shows)
// the scroll position.
void on_filter_changed(State& st) {
  update_strip_status(st);
  populate_list(st, true);
  place_strip(st);
  log_line("dialog: filter %s", [&] {
    std::string s;
    for (const auto& id : current_filter(st)) s += (s.empty() ? "" : ",") + id;
    return s.empty() ? std::string("(all)") : s;
  }().c_str());
}

// Ask the host what it can do (`--capabilities`; at the first catalog, and
// at a later one only until it has answered: reload_catalog): which lanes
// and module ABIs it runs, and which lanes can open a module's own settings
// windows. No module runs for this; until it answers, none starts.
void start_lane_probe(State& st) {
  std::wstring host = host_exe_path();
  if (st.catalog.modules.empty() || !file_exists(host)) return;
  st.probing = true;
  HWND dlg = st.dlg;
  std::thread([dlg, host] {
    auto* caps = new HostCapabilities(probe_capabilities(host));
    if (!PostMessageW(dlg, WM_APP_LANE_PROBE, 0, reinterpret_cast<LPARAM>(caps))) delete caps;
  }).detach();
}

// Which modules this host can run has changed (its --capabilities answer, or
// a module's exit 3); `shown_before` is how the module the details show
// stood before. The rows' dimming and the groups' pills, "… can run now",
// Preview and the thumbnails follow, and so do the details when their own
// module changed: rebuilt when it became (or stopped being) "Coming soon",
// only its preview started when it had waited for the answer (rebuilding the
// panel for anything less would take the keyboard focus from under the
// user). Returns whether the details were rebuilt.
bool availability_changed(State& st, ModuleRun shown_before) {
  InvalidateRect(st.list, nullptr, TRUE);
  update_summary(st);   // "… can run now"
  bool rebuilt = false;
  if (const Module* m = shown_module(st)) {
    const ModuleRun now = run_state(st, *m);
    if (now != shown_before && (now == ModuleRun::coming_soon || shown_before == ModuleRun::coming_soon)) {
      show_details(st, st.shown);
      rebuilt = true;
    } else if (now != shown_before) {
      refresh_preview(st);
    }
  }
  update_preview_button(st);
  PostMessageW(st.dlg, WM_APP_SCHEDULE_THUMBS, 0, 0);
  return rebuilt;
}

// A module's preview or thumbnail exited 3 before a frame: the host has no
// lane for it (host.h: kExitLaneMissing), whatever it listed; a host too old
// to answer --capabilities says so this way, one module at a time. It alone
// becomes "Coming soon"; the rest of its lane and ABI are left as they are.
// (A host that lacks a module's ABI fails it with exit 1 instead, like a
// damaged module: its abis= answer is what covers that.)
void mark_cant_run(State& st, const std::vector<std::string>& ids) {
  const Module* m = shown_module(st);
  const ModuleRun before = m ? run_state(st, *m) : ModuleRun::runs;
  bool any = false;
  for (const std::string& id : ids) {
    if (id.empty() || !st.catalog.find(id) || !st.cant_run.insert(id).second) continue;
    log_line("dialog: %s exited 3: this host can't run it", id.c_str());
    any = true;
  }
  if (any) availability_changed(st, before);
}

// ---- thumbnails ------------------------------------------------------------------------

// Every module still without a picture gets a thumbnail taken in the
// background (thumbnails.h: ThumbnailQueue), one at a time, in the order the
// list shows them. They wait for the host's answer (--capabilities), and a
// module this host can't run (run_state) is left out.
void schedule_thumbnails(State& st) {
  if (!st.thumbgen_allowed || st.import_running || st.remove_running) return;
  std::vector<ThumbJob> jobs;
  const std::wstring host = host_exe_path();
  if (file_exists(host)) {
    for (const auto& e : list_geom(st).edges) {
      if (e.item < 0) continue;
      LVITEMW it{};
      it.mask = LVIF_PARAM;
      it.iItem = e.item;
      if (!ListView_GetItem(st.list, &it) || it.lParam < 0 || it.lParam >= (LPARAM)st.catalog.modules.size()) continue;
      const int mi = (int)it.lParam;
      const Module& m = st.catalog.modules[mi];
      if (!is_present(st, mi) || run_state(st, m) != ModuleRun::runs) continue;
      if (st.icons.has_picture(m, st.win_dir)) continue;
      jobs.push_back(ThumbJob{m.id, host, resolve_module_path(st.win_dir, m.path), st.win_dir, st.icons.thumb_path(m.id),
                              module_cvset(st, m), m.abi, m.screen});
    }
  }
  if (!st.probing) st.thumbgen_ran = true;
  if (jobs.empty() && !st.thumbgen) return;
  if (!st.thumbgen) st.thumbgen = std::make_unique<ThumbnailQueue>(st.dlg, WM_APP_THUMBS);
  log_line("dialog: %zu thumbnail(s) to take", jobs.size());
  st.thumbgen->set_jobs(std::move(jobs));
}

// ---- gather / save --------------------------------------------------------------------

DialogChoice current_choice(const State& st) {
  DialogChoice c;
  c.random = IsDlgButtonChecked(st.dlg, IDC_MODE_RANDOM) == BST_CHECKED;
  int total = 0;
  c.checked = checked_ids(st, &total);   // global: every release's checks
  c.total = (size_t)total;
  if (module_index(st, st.chosen) >= 0) c.selected = st.chosen;
  c.strip = st.strip_on && st.strip;
  if (c.strip) {
    c.collections = st.strip->selected();
    c.releases = st.strip->count();
  }
  c.shown_checked = shown_checks(st).checked;
  return c;
}

Settings gather(State& st) {
  // Mode and lists by the rules in settings.h: Random keeps a named Module
  // that leads the file's list, and "Single module" keeps the checklist
  // aside (RandomizeSaved) rather than dropping it.
  Settings s = apply_dialog_choice(st.settings, current_choice(st));
  s.controls = st.controls;
  LRESULT dsel = SendDlgItemMessageW(st.dlg, IDC_DURATION, CB_GETCURSEL, 0, 0);
  if (dsel != CB_ERR) {
    s.duration_min = (int)std::min<LRESULT>(SendDlgItemMessageW(st.dlg, IDC_DURATION, CB_GETITEMDATA, dsel, 0), 24 * 60 * 7);
  }
  LRESULT sel = SendDlgItemMessageW(st.dlg, IDC_SCALE, CB_GETCURSEL, 0, 0);
  if (sel != CB_ERR) s.scale = (double)SendDlgItemMessageW(st.dlg, IDC_SCALE, CB_GETITEMDATA, sel, 0) / 100.0;
  s.all_monitors = monitors_all(st);
  // As it stands, even while hidden (one monitor, Single module) or greyed
  // (Primary monitor only): the file's value stays until the user changes it.
  s.different_per_monitor = IsDlgButtonChecked(st.dlg, IDC_PER_MONITOR) == BST_CHECKED;
  s.stretch_to_fit = IsDlgButtonChecked(st.dlg, IDC_STRETCH) == BST_CHECKED;
  // Sound (AUDIO.md §9); SoundMonitor stays as loaded (reserved).
  s.sound = SendDlgItemMessageW(st.dlg, IDC_SOUND, CB_GETCURSEL, 0, 0) != 1;
  s.volume = std::clamp((int)SendDlgItemMessageW(st.dlg, IDC_VOLUME, TBM_GETPOS, 0, 0), 0, 100);
  return s;
}

bool on_ok(State& st) {
  // Judged on the dialog, not on what gather() makes of it: with a leading
  // Module, "Random" with nothing checked would quietly save a single module.
  // Random needs a checked module among the rows shown (COVERS.md §1.8).
  DialogChoice c = current_choice(st);
  if (!random_allowed(c)) {
    MessageBoxW(st.dlg, kRandomNeedsChecks, L"Long After Dark", MB_OK | MB_ICONINFORMATION);
    return false;
  }
  Settings s = gather(st);
  if (!save_settings(settings_path(), s)) {
    std::wstring msg = L"Could not save settings to\n" + settings_path();
    MessageBoxW(st.dlg, msg.c_str(), L"Long After Dark", MB_OK | MB_ICONERROR);
    return false;
  }
  log_line("dialog: saved %s", narrow(settings_path()).c_str());
  return true;
}

// ---- child processes (Preview, Import) -----------------------------------------------

bool launch(const ChildLaunch& c, HWND notify, UINT done_msg) {
  DWORD err = 0;
  HANDLE h = start_child(c, &err);
  if (!h) {
    log_line("dialog: cannot start %s: error %lu", narrow(c.exe).c_str(), (unsigned long)err);
    return false;
  }
  // Wait off the UI thread; the dialog stays live and re-enables the button
  // when the child exits.
  std::thread([h, notify, done_msg] {
    WaitForSingleObject(h, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(h, &code);
    CloseHandle(h);
    PostMessageW(notify, done_msg, code, 0);
  }).detach();
  return true;
}

void delete_preview_settings(State& st) {
  if (st.preview_ini.empty()) return;
  DeleteFileW(st.preview_ini.c_str());   // usually gone already: the saver deletes it once read
  st.preview_ini.clear();
}

void on_preview(State& st) {
  // The module the details show (the chosen one while the list shows its row).
  const int mi = st.shown;
  if (mi < 0 || mi >= (int)st.catalog.modules.size() || st.preview_running) return;
  if (being_removed(st, st.catalog.modules[mi])) return;
  // Run exactly what the dialog shows (even unsaved): a throwaway settings
  // file handed to "/s" through AD_SETTINGS (dialog_support.h). Its host gets
  // the module's own screen, as the saver's always do (module_screen: an
  // Intermission, Star Trek, ScreamSavers or Marvel module's 640x480,
  // whatever the Resolution setting).
  Settings s = gather(st);
  s.module = st.catalog.modules[mi].id;
  s.randomize.clear();   // just this module, for as long as the preview runs
  s.randomize_saved.clear();
  s.randomize_saved_none = false;
  s.duration_min = 0;
  st.preview_ini = preview_settings_path(temp_dir(), GetCurrentProcessId());
  if (!write_file_atomic(st.preview_ini, serialize_settings(s))) {
    st.preview_ini.clear();
    return;
  }
  ChildLaunch c;
  c.exe = exe_path();
  c.args = L"/s";
  // The preview's settings file is a throwaway in %TEMP%, but its modules'
  // own state and its last-exit log are the user's (INTERACTION.md §7.1, §9.1).
  c.env_block = build_environment_block({{L"AD_SETTINGS", st.preview_ini},
                                         {kPreviewSettingsEnv, L"1"},
                                         {L"AD_SCR_STATE", state_dir()},
                                         {L"AD_SCR_LASTLOG", last_exit_log_path()}});
  if (launch(c, st.dlg, WM_APP_PREVIEW_DONE)) {
    st.preview_running = true;
    if (st.preview) live_preview_pause(st.preview, true);   // one module at a time
    if (st.thumbgen) st.thumbgen->pause(true);
    update_assets_status(st);
  } else {
    delete_preview_settings(st);
  }
}

// ---- module buttons (INTERACTION.md §6.3) -------------------------------------------

// The notes under the shown module's live buttons, after a run or a change
// of the popup the "Custom" hint watches. Controls stay; the rows are placed
// again (a note may need a second line).
void update_button_notes(State& st) {
  const Module* m = shown_module(st);
  if (!m || !button_live(st, st.shown)) return;
  bool any = false;
  for (size_t slot = 0; slot < m->controls.size(); ++slot) {
    const Control& c = m->controls[slot];
    if (c.type != ControlType::button) continue;
    HWND note = GetDlgItem(st.panel, IDC_PANEL_BASE + (int)slot * IDC_PANEL_STRIDE + IDC_PART_VALUE);
    if (!note) continue;
    std::wstring text = button_note(st, *m, c);
    if (window_text(note) != text) {
      SetWindowTextW(note, text.c_str());
      InvalidateRect(note, nullptr, TRUE);
      any = true;
    }
  }
  if (any) position_panel(st);
}

// Runs a module's button in `adhostwin --configure` (§6.1): its dialogs open
// owned by this window. The dialog is disabled meanwhile (as the AD 3
// control panel disabled itself, INTERACTION.md §1.4) and the live preview
// paused; the run is waited for on a worker thread (the UI keeps pumping:
// an owned window shares our input queue).
void start_configure(State& st, int module_index, int slot) {
  if (st.configuring || st.preview_running || st.import_running || st.remove_running) return;   // one run at a time
  if (module_index < 0 || module_index >= (int)st.catalog.modules.size() || !button_live(st, module_index)) return;
  const Module& m = st.catalog.modules[module_index];
  if (slot < 0 || slot >= (int)m.controls.size() || m.controls[slot].type != ControlType::button) return;
  const Control& c = m.controls[slot];
  auto tool = std::make_unique<HostTool>();
  std::wstring err;
  const std::wstring path = resolve_module_path(st.win_dir, m.path);
  // The dialog's current values, unsaved edits included: the module's
  // dialog starts from what the user sees here.
  const std::string cvset = module_cvset(st, m);
  std::vector<std::pair<std::wstring, std::wstring>> env = {
      {L"AD_ASSETS_DIR", assets_root()},
      {L"ADCVSET", widen(cvset)},
      {L"ADSTATE", state_dir()},
  };
  st.button_notes[m.id].erase(c.index);
  st.button_ok[m.id].erase(c.index);
  if (!tool->start(host_exe_path(), configure_args(path, c.index, (uintptr_t)st.dlg), env, &err)) {
    log_line("dialog: configure %s button %d: %s", m.id.c_str(), c.index, narrow(err).c_str());
    st.button_notes[m.id][c.index] = configure_outcome_note(1);
    update_button_notes(st);
    return;
  }
  st.configuring = true;
  st.configure_id = m.id;
  st.configure_slot = c.index;
  log_line("dialog: configure %s button %d cvset=%s pid=%lu", m.id.c_str(), c.index, cvset.c_str(), tool->pid);
  if (st.preview) live_preview_pause(st.preview, true);
  if (st.thumbgen) st.thumbgen->pause(true);
  EnableWindow(st.dlg, FALSE);
  HWND dlg = st.dlg;
  std::thread([tool = std::move(tool), dlg] {
    std::string out;
    DWORD code = 1;
    tool->finish(INFINITE, &out, &code);
    auto* line = new std::string(out);
    if (!PostMessageW(dlg, WM_APP_CONFIGURE_DONE, code, reinterpret_cast<LPARAM>(line))) delete line;
  }).detach();
}

void on_configure_done(State& st, DWORD code, const std::string& out) {
  st.configuring = false;
  // Whatever the exit (a crashed host included), the dialog is usable again.
  EnableWindow(st.dlg, TRUE);
  SetForegroundWindow(st.dlg);
  std::string json = out.substr(0, out.find_first_of("\r\n"));
  log_line("dialog: configure %s button %d exited with %lu: %s", st.configure_id.c_str(), st.configure_slot,
           (unsigned long)code, json.c_str());
  const std::string id = st.configure_id;
  st.button_notes[id][st.configure_slot] = configure_outcome_note(code);
  if (code == 0) st.button_ok[id].insert(st.configure_slot);
  // The module keeps what it saved in its own files; its thumbnail and the
  // live preview may show the old look, so both are taken again.
  DeleteFileW(st.icons.thumb_path(id).c_str());
  st.icons.forget(id);
  InvalidateRect(st.list, nullptr, FALSE);
  if (st.preview) {
    live_preview_pause(st.preview, false);
    const Module* m = shown_module(st);
    if (m && m->id == id) live_preview_restart(st.preview);
  }
  if (st.thumbgen) st.thumbgen->pause(false);
  update_button_notes(st);
  PostMessageW(st.dlg, WM_APP_SCHEDULE_THUMBS, 0, 0);
}

// After an import or a cover change that did something: the new catalog,
// keeping the user's checks, filter, chosen module and unsaved values.
void reload_catalog(State& st) {
  int total = 0;
  const bool all_checked = (int)checked_ids(st, &total).size() == total;
  const std::vector<std::string> filter = st.strip ? st.strip->selected() : std::vector<std::string>{};
  const bool had_strip = st.strip_on;
  load_catalog_into(st);
  // Newly imported modules join the rotation when everything was checked.
  if (all_checked) {
    for (const Module& m : st.catalog.modules) st.checks.insert(m.id);
  }
  load_strip(st, filter);
  log_line("dialog: catalog reloaded (%zu modules, %zu releases)", st.catalog.modules.size(), st.catalog.releases.size());
  if (st.strip_on && !had_strip) {
    // A second release arrived: room for the strip without shrinking the columns.
    RECT cr{}, wr{};
    GetClientRect(st.dlg, &cr);
    GetWindowRect(st.dlg, &wr);
    const int min_h = dip(kMinClientHStrip, dpi_of(st));
    if (cr.bottom < min_h) {
      SetWindowPos(st.dlg, nullptr, 0, 0, wr.right - wr.left, wr.bottom - wr.top + (min_h - cr.bottom),
                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
  }
  // A module that exited 3 gets another chance. The host is the same
  // program, so its answer stands: it is asked only when there is none yet
  // (a first import leaves the welcome, which never asked; a host that
  // didn't answer). Asking again would stop the live preview and the
  // background thumbnail of every lane until it answered (module_run:
  // waiting) and grey the module buttons meanwhile, all for nothing.
  st.cant_run.clear();
  if (!st.caps.known && !st.probing) start_lane_probe(st);
  populate_list(st);
  update_mode(st);
  update_assets_status(st);
  PostMessageW(st.dlg, WM_APP_SCHEDULE_THUMBS, 0, 0);
}

void on_import(State& st) {
  if (st.import_running || st.cover_running || st.remove_running) return;   // one adimport at a time
  std::wstring exe = import_exe_path();
  if (!file_exists(exe)) {
    std::wstring msg = L"The importer (adimport.exe) was not found at\n" + exe + L"\n\nIt is installed next to LongAfterDark.scr.";
    MessageBoxW(st.dlg, msg.c_str(), L"Long After Dark", MB_OK | MB_ICONWARNING);
    return;
  }
  ChildLaunch c;
  c.exe = exe;
  c.args = L"--gui";
  c.console_program = true;   // its own task dialogs only, never a console window
  if (launch(c, st.dlg, WM_APP_IMPORT_DONE)) {
    st.import_running = true;
    st.import_note.clear();
    if (st.thumbgen) st.thumbgen->set_jobs({});
    update_assets_status(st);
  }
}

void on_import_done(State& st, DWORD code) {
  st.import_running = false;
  ImportOutcome outcome = classify_import_exit(code);
  log_line("dialog: importer exited with %lu (%s)", (unsigned long)code,
           outcome == ImportOutcome::imported ? "imported" : outcome == ImportOutcome::cancelled ? "cancelled" : "failed");
  st.import_note = import_outcome_note(code);
  if (outcome != ImportOutcome::imported) {
    // Imports are atomic: the catalog and files are as they were, so the
    // list (and the user's unsaved checks and edits in it) stays as it is.
    update_assets_status(st);
    return;
  }
  reload_catalog(st);
}

// "Change cover…" (COVERS.md §1.11): adimport's own cover window, started
// the way Import… starts it. The dialog never writes under the assets root.
bool can_change_cover(const State& st) {
  return !st.import_running && !st.cover_running && !st.remove_running && file_exists(import_exe_path());
}

void on_change_cover(State& st, int tile) {
  if (!st.strip || tile < 0 || tile >= (int)st.strip->count() || !can_change_cover(st)) return;
  const std::string id = st.strip->tile(tile).id;
  ChildLaunch c;
  c.exe = import_exe_path();
  c.args = L"--gui --change-cover " + quote_arg(widen(id));
  c.console_program = true;   // its own windows only, never a console window
  if (launch(c, st.dlg, WM_APP_COVER_DONE)) {
    st.cover_running = true;
    st.cover_id = id;
    if (st.thumbgen) st.thumbgen->pause(true);
    log_line("dialog: change cover %s", id.c_str());
    update_assets_status(st);
  }
}

// "Get the covers" (under the strip's status line while a release still shows
// a generated cover): adimport's progress window over every installed
// release's cover downloads, started like "Change cover…" and finished by
// the same message (exit 0: a cover changed, so the catalog is read again).
void on_get_covers(State& st) {
  if (!st.strip_on || !can_change_cover(st)) return;
  ChildLaunch c;
  c.exe = import_exe_path();
  c.args = L"--gui --refresh-covers all";
  c.console_program = true;   // its own windows only, never a console window
  if (launch(c, st.dlg, WM_APP_COVER_DONE)) {
    st.cover_running = true;
    st.cover_id = "(all)";
    if (st.thumbgen) st.thumbgen->pause(true);
    log_line("dialog: get the covers");
    update_assets_status(st);
  }
}

void on_change_cover_done(State& st, DWORD code) {
  st.cover_running = false;
  log_line("dialog: change cover %s exited with %lu", st.cover_id.c_str(), (unsigned long)code);
  if (st.thumbgen) st.thumbgen->pause(false);
  // 0: a cover changed (the catalog says which tile to show now); anything
  // else changed nothing.
  if (code == 0) reload_catalog(st);
  else update_assets_status(st);
}

// "Remove …" (COVERS.md §1.11): adimport's own window asks first and removes
// the release (`--gui --remove <id>`), started the way "Change cover…" is,
// so the dialog never writes under the assets root. Nothing of the release
// may be open meanwhile, or its folder can't be moved aside: no thumbnail
// is taken and no module's own settings window opens until adimport has
// exited, and none of its modules runs in the live preview or Preview
// (being_removed); a module's own settings window running greys the item.
bool can_remove(const State& st) { return can_change_cover(st) && !st.configuring; }

void on_remove_release(State& st, int tile) {
  if (!st.strip || tile < 0 || tile >= (int)st.strip->count() || !can_remove(st)) return;
  const std::string id = st.strip->tile(tile).id;
  ChildLaunch c;
  c.exe = import_exe_path();
  c.args = L"--gui --remove " + quote_arg(widen(id));
  c.console_program = true;   // its own windows only, never a console window
  if (!launch(c, st.dlg, WM_APP_REMOVE_DONE)) return;
  st.remove_running = true;
  st.remove_id = id;
  if (st.thumbgen) st.thumbgen->set_jobs({});
  refresh_preview(st);   // the night sky, while it shows one of the release's modules
  log_line("dialog: remove %s", id.c_str());
  update_assets_status(st);
}

void on_remove_done(State& st, DWORD code) {
  st.remove_running = false;
  log_line("dialog: remove %s exited with %lu", st.remove_id.c_str(), (unsigned long)code);
  // 0: the release is gone (the catalog no longer lists it); anything else
  // removed nothing (adimport said why), and the preview carries on.
  if (code == 0) {
    st.import_note.clear();   // the module count says what is left
    reload_catalog(st);
  } else {
    refresh_preview(st);
    update_assets_status(st);
    PostMessageW(st.dlg, WM_APP_SCHEDULE_THUMBS, 0, 0);
  }
}

// ---- painting ------------------------------------------------------------------------

RECT rc_of(const Rc& r) { return RECT{r.x, r.y, r.right(), r.bottom()}; }

void paint(State& st, HDC target) {
  RECT cr{};
  GetClientRect(st.dlg, &cr);
  HDC dc = CreateCompatibleDC(target);
  HBITMAP bmp = CreateCompatibleBitmap(target, std::max(1L, cr.right), std::max(1L, cr.bottom));
  HGDIOBJ old = SelectObject(dc, bmp);
  const Theme& t = st.theme;
  const Palette& p = t.pal;
  const WindowLayout& L = st.L;
  fill_rect(dc, cr, p.base);

  // Header: the moon-and-stars mark, the name, and what this window is, on
  // one compact line, over the night band (adw_ui, shared with adimport).
  paint_header(dc, rc_of(L.header), rc_of(L.logo), rc_of(L.title), st.logo, L"Long After Dark", L"Screen saver settings", t);

  paint_card(dc, rc_of(L.list_card), t);
  paint_card(dc, rc_of(L.details_card), t);
  paint_card(dc, rc_of(L.options_card), t);
  // The module's tile beside its name (the welcome has the header's moon only).
  if (st.shown >= 0 && st.shown < (int)st.catalog.modules.size()) {
    const Module& m = st.catalog.modules[st.shown];
    st.icons.draw(dc, m, st.win_dir, rc_of(L.module_icon), !is_present(st, st.shown) || coming_soon(st, m));
  }

  // Footer: a hairline above the buttons.
  RECT line{0, L.footer.y, cr.right, L.footer.y + t.hairline()};
  fill_rect(dc, line, p.divider);

  BitBlt(target, 0, 0, cr.right, cr.bottom, dc, 0, 0, SRCCOPY);
  SelectObject(dc, old);
  DeleteObject(bmp);
  DeleteDC(dc);
}

// Colours for the statics and the read-only About box, on whichever surface they sit.
INT_PTR ctl_color(State& st, HDC dc, HWND ctl) {
  const Palette& p = st.theme.pal;
  COLORREF bg = surface_color(ctl, p);
  SetTextColor(dc, text_color_for(st, GetDlgCtrlID(ctl)));
  SetBkColor(dc, bg);
  SetBkMode(dc, OPAQUE);
  return (INT_PTR)(bg == p.card ? st.theme.card_brush : st.theme.base_brush);
}

// The footer's credit (IDC_FOOTER_CREDIT, NM_CUSTOMDRAW): the dialog's link look,
// adw_ui's ButtonRole::subtle (a fill under the pointer and a deeper one
// pressed, the focus ring round its box), with its text in the caption face
// of the assets line beside it: the lead in text2, the name in the accent
// text colour and underlined under the pointer (the hover cue high contrast
// keeps: its hover fill is the window colour). Pressed under high contrast:
// the highlight and its text.
LRESULT draw_footer_credit(State& st, NMCUSTOMDRAW* cd) {
  if (cd->dwDrawStage != CDDS_PREPAINT) return CDRF_DODEFAULT;
  HWND h = cd->hdr.hwndFrom;
  const Theme& t = st.theme;
  const Palette& p = t.pal;
  const RECT cr = cd->rc;
  const int w = cr.right - cr.left, hgt = cr.bottom - cr.top;
  if (w <= 0 || hgt <= 0 || !st.footer_credit.shown) return CDRF_SKIPDEFAULT;
  HDC dc = CreateCompatibleDC(cd->hdc);
  HBITMAP bmp = CreateCompatibleBitmap(cd->hdc, w, hgt);
  HGDIOBJ old = SelectObject(dc, bmp);
  const RECT all{0, 0, w, hgt};
  fill_rect(dc, all, p.base);
  const int fm = focus_margin(t.dpi);
  const float s = t.dpi / 96.0f, radius = 4 * s;
  const bool pressed = (cd->uItemState & CDIS_SELECTED) != 0;
  const bool hot = (cd->uItemState & CDIS_HOT) != 0 || st.footer_credit_hover;
  const bool focus = ((cd->uItemState & CDIS_FOCUS) || focused_window() == h) && keyboard_cues(h);
  const RECT body{fm, fm, w - fm, hgt - fm};
  if (hot || pressed) fill_round(dc, body, radius, pressed ? p.row_selected : p.row_hover);
  const COLORREF lead_ink = pressed && p.high_contrast ? p.on_accent : p.text2;
  const COLORREF name_ink = !pressed ? p.accent_text : p.high_contrast ? p.on_accent : blend(p.accent_text, p.base, 0.25);
  // The texts where the layout put them, in this window's coordinates.
  const int ox = st.footer_credit.box.x - fm, oy = st.footer_credit.box.y - fm;
  auto local = [&](const Rc& r) { return RECT{r.x - ox, r.y - oy, r.right() - ox + 1, r.bottom() - oy}; };
  const UINT fmt = DT_SINGLELINE | DT_LEFT | DT_TOP | DT_NOPREFIX;
  draw_text(dc, kFooterCreditLead, local(st.footer_credit.lead), t.fonts.caption, lead_ink, fmt);
  HFONT underlined = nullptr;
  if (hot) {
    LOGFONTW lf{};
    if (GetObjectW(t.fonts.caption, sizeof(lf), &lf)) {
      lf.lfUnderline = TRUE;
      underlined = CreateFontIndirectW(&lf);
    }
  }
  draw_text(dc, kFooterCreditName, local(st.footer_credit.name), underlined ? underlined : t.fonts.caption, name_ink,
            fmt);
  if (underlined) DeleteObject(underlined);
  if (focus) draw_focus_ring(dc, all, radius + fm, p, s);
  BitBlt(cd->hdc, cr.left, cr.top, w, hgt, dc, 0, 0, SRCCOPY);
  SelectObject(dc, old);
  DeleteObject(bmp);
  DeleteDC(dc);
  return CDRF_SKIPDEFAULT;
}

// What screen readers hear of the credit besides its name (its window text,
// "Made With Love by StarrLord"): where it goes, as its MSAA description and
// its UI Automation help text (UIA's own property: its button proxy doesn't
// map the description).
void describe_footer_credit(HWND link) {
  // CLSID_AccPropServices, IID_IAccPropServices, PROPID_ACC_DESCRIPTION, HelpText_Property_GUID.
  static const GUID kClsid = {0xb5f8350b, 0x0548, 0x48b1, {0xa6, 0xee, 0x88, 0xbd, 0x00, 0xb4, 0xa5, 0xe7}};
  static const GUID kIid = {0x6e26e776, 0x04f0, 0x495d, {0x80, 0xe4, 0x33, 0x30, 0x35, 0x2e, 0x31, 0x69}};
  static const GUID kDescription = {0x4d48dfe4, 0xbd3f, 0x491f, {0xa6, 0x48, 0x49, 0x2d, 0x6f, 0x20, 0xc5, 0x88}};
  static const GUID kHelpText = {0x08555685, 0x0977, 0x45c7, {0xa7, 0xa6, 0xab, 0xaf, 0x56, 0x84, 0x12, 0x1a}};
  const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  IAccPropServices* props = nullptr;
  if (SUCCEEDED(CoCreateInstance(kClsid, nullptr, CLSCTX_INPROC_SERVER, kIid, reinterpret_cast<void**>(&props))) && props) {
    const std::wstring text = std::wstring(L"Opens ") + kFooterCreditUrl + L" in your browser";
    for (const GUID* prop : {&kDescription, &kHelpText}) {
      props->SetHwndPropStr(link, (DWORD)OBJID_CLIENT, (DWORD)CHILDID_SELF, *prop, text.c_str());
    }
    props->Release();
  }
  if (SUCCEEDED(co)) CoUninitialize();
}

// The credit's link: the project's page in the default browser. The test
// build (AD_SCR_TEST_HOOKS) never opens anything, whatever runs it: it logs
// the request and appends it to AD_SCR_TEST_OPEN_LOG, so no test starts a
// browser.
void open_credit(State& st) {
#if AD_SCR_TEST_HOOKS
  (void)st;
  log_line("dialog: open %s (the test build opens nothing)", narrow(kFooterCreditUrl).c_str());
  if (const std::wstring f = env_w(L"AD_SCR_TEST_OPEN_LOG"); !f.empty()) {
    std::string text;
    read_file(f, text);
    write_file_atomic(f, text + "open\t" + narrow(kFooterCreditUrl) + "\n");
  }
#else
  const auto r = (INT_PTR)ShellExecuteW(st.dlg, L"open", kFooterCreditUrl, nullptr, nullptr, SW_SHOWNORMAL);
  log_line("dialog: open %s%s", narrow(kFooterCreditUrl).c_str(), r > 32 ? "" : (" failed (" + std::to_string(r) + ")").c_str());
#endif
}

void dpi_changed(State& st, int dpi) {
  st.theme.set_dpi(dpi);
  set_fonts(st);
  set_row_height(st);
  load_logo(st);
  if (st.preview) live_preview_set_palette(st.preview, &st.theme.pal, dpi);
  const int face = st.theme.px(32) + 2 * focus_margin(dpi);
  for (int id : {IDC_DURATION, IDC_SCALE, IDC_MONITORS, IDC_SOUND}) size_combo(GetDlgItem(st.dlg, id), st.theme, face);
}

// ---- panel dialog ------------------------------------------------------------------

INT_PTR CALLBACK panel_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  auto* st = reinterpret_cast<State*>(GetWindowLongPtrW(h, DWLP_USER));
  if (msg == WM_INITDIALOG) {
    SetWindowLongPtrW(h, DWLP_USER, lp);
    return FALSE;
  }
  if (!st) return FALSE;
  const Module* m = (st->shown >= 0 && st->shown < (int)st->catalog.modules.size()) ? &st->catalog.modules[st->shown] : nullptr;
  auto slot_of = [&](int id, int* part) {
    int rel = id - IDC_PANEL_BASE;
    if (rel < 0 || !m) return -1;
    if (part) *part = rel % IDC_PANEL_STRIDE;
    int slot = rel / IDC_PANEL_STRIDE;
    return slot < (int)m->controls.size() ? slot : -1;
  };
  switch (msg) {
    case WM_CTLCOLORDLG:
      return (INT_PTR)st->theme.card_brush;
    case WM_ERASEBKGND: {
      RECT r{};
      GetClientRect(h, &r);
      fill_rect((HDC)wp, r, st->theme.pal.card);
      paint_panel_peek(*st, (HDC)wp);
      SetWindowLongPtrW(h, DWLP_MSGRESULT, 1);
      return TRUE;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
      return ctl_color(*st, (HDC)wp, (HWND)lp);
    case WM_CTLCOLORLISTBOX:
      SetTextColor((HDC)wp, st->theme.pal.text);
      SetBkColor((HDC)wp, st->theme.pal.card);
      return (INT_PTR)combo_list_brush(st->theme);
    case WM_MEASUREITEM:
      measure_combo_item(st->theme, reinterpret_cast<MEASUREITEMSTRUCT*>(lp));
      return TRUE;
    case WM_DRAWITEM: {
      auto* di = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
      if (di->CtlType == ODT_STATIC) {
        // A module button's note: an info glyph and caption text, quiet.
        const Theme& t = st->theme;
        fill_rect(di->hDC, di->rcItem, t.pal.card);
        if (GetWindowTextLengthW(di->hwndItem) == 0) return TRUE;   // a live button with nothing to say
        RECT g = di->rcItem;
        g.right = g.left + t.px(12);
        draw_text(di->hDC, L"\uE946", g, t.fonts.icons_small, t.pal.text2, DT_SINGLELINE | DT_TOP | DT_LEFT | DT_NOPREFIX);
        RECT r = di->rcItem;
        r.left += t.px(12) + t.px(6);
        draw_text(di->hDC, window_text(di->hwndItem), r, t.fonts.caption, t.pal.text2, DT_WORDBREAK | DT_NOPREFIX);
        return TRUE;
      }
      draw_combo_item(st->theme, di);
      return TRUE;
    }
    case WM_NOTIFY: {
      auto* hdr = reinterpret_cast<NMHDR*>(lp);
      if (hdr->code != NM_CUSTOMDRAW) break;
      wchar_t cls[32] = {};
      GetClassNameW(hdr->hwndFrom, cls, 32);
      LRESULT r = wcscmp(cls, TRACKBAR_CLASSW) == 0 ? custom_draw_trackbar(st->theme, reinterpret_cast<NMCUSTOMDRAW*>(lp))
                                                     : custom_draw_button(st->theme, reinterpret_cast<NMCUSTOMDRAW*>(lp));
      SetWindowLongPtrW(h, DWLP_MSGRESULT, r);
      return TRUE;
    }
    case WM_HSCROLL: {
      int part = 0, slot = slot_of(GetDlgCtrlID((HWND)lp), &part);
      if (slot < 0 || part != IDC_PART_INPUT) break;
      const Control& c = m->controls[slot];
      if (c.type != ControlType::slider) break;
      int pos = (int)SendMessageW((HWND)lp, TBM_GETPOS, 0, 0);
      const int current = control_value(*st, *m, c);
      int v = c.clamp(pos);
      if (c.stepped()) {
        // The run already showing keeps whatever value it holds; another
        // run stores its own (ui_model.h: VisualStops).
        const VisualStops vs = visual_stops(c);
        const int run = std::clamp(pos, 0, std::max(0, vs.count() - 1));
        v = vs.run_of_value(c, current) == run || vs.count() == 0 ? current : vs.value[run];
      }
      if (v != current) {
        st->controls[m->id][c.index] = v;
        update_slider_label(*st, slot, c, v);
        update_defaults_button(*st);
      }
      // Restart the live preview once the thumb is let go.
      if (LOWORD(wp) != TB_THUMBTRACK) refresh_preview(*st);
      return TRUE;
    }
    case WM_COMMAND: {
      int id = LOWORD(wp), code = HIWORD(wp);
      int part = 0, slot = slot_of(id, &part);
      if (slot < 0 || part != IDC_PART_INPUT) break;
      const Control& c = m->controls[slot];
      if (c.type == ControlType::button && code == BN_CLICKED) {
        start_configure(*st, st->shown, slot);
        return TRUE;
      }
      if (c.type == ControlType::checkbox && code == BN_CLICKED) {
        st->controls[m->id][c.index] = IsDlgButtonChecked(h, id) == BST_CHECKED ? 1 : 0;
        update_defaults_button(*st);
        refresh_preview(*st);
        return TRUE;
      }
      if (c.type == ControlType::popup && code == CBN_SELCHANGE) {
        int sel = (int)SendDlgItemMessageW(h, id, CB_GETCURSEL, 0, 0);
        if (sel >= 0) st->controls[m->id][c.index] = c.clamp(c.min + sel);
        update_defaults_button(*st);
        refresh_preview(*st);
        update_button_notes(*st);   // the "Custom" hint follows the popup
        return TRUE;
      }
      break;
    }
    case WM_VSCROLL: {
      // By whole rows (position_panel: scroll stops at row tops).
      SCROLLINFO si{sizeof(si), SIF_ALL};
      GetScrollInfo(h, SB_VERT, &si);
      switch (LOWORD(wp)) {
        case SB_LINEUP: scroll_panel_rows(*st, -1); break;
        case SB_LINEDOWN: scroll_panel_rows(*st, 1); break;
        case SB_PAGEUP: scroll_panel_rows(*st, -2); break;
        case SB_PAGEDOWN: scroll_panel_rows(*st, 2); break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: {
          // The nearest stop to where the thumb is.
          const int want = LOWORD(wp) == SB_THUMBTRACK ? si.nTrackPos : (int)HIWORD(wp);
          int best = st->scroll;
          for (int s : st->panel_stops) {
            if (std::abs(s - want) < std::abs(best - want)) best = s;
          }
          scroll_panel_to(*st, best);
          break;
        }
        case SB_TOP: scroll_panel_to(*st, 0); break;
        case SB_BOTTOM: scroll_panel_to(*st, INT_MAX); break;
      }
      return TRUE;
    }
    case WM_MOUSEWHEEL:
      scroll_panel_rows(*st, GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -1 : 1);
      return TRUE;
    case WM_DPICHANGED_AFTERPARENT:
      PostMessageW(h, WM_APP_REBUILD_PANEL, 0, 0);
      return TRUE;
    case WM_APP_REBUILD_PANEL:
      build_panel(*st, st->shown);
      refresh_preview(*st);
      return TRUE;
  }
  return FALSE;
}

// ---- dialog -----------------------------------------------------------------------------

void size_and_center(State& st) {
  const int dpi = (int)GetDpiForWindow(st.dlg);
  const int ldpi = dpi_of(st);
  HWND owner = GetWindow(st.dlg, GW_OWNER);
  const bool anchor_owner = owner && IsWindowVisible(owner);
  HMONITOR mon = MonitorFromWindow(anchor_owner ? owner : st.dlg, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi{sizeof(mi)};
  GetMonitorInfoW(mon, &mi);
  const RECT& wa = mi.rcWork;
  DWORD style = (DWORD)GetWindowLongW(st.dlg, GWL_STYLE), ex = (DWORD)GetWindowLongW(st.dlg, GWL_EXSTYLE);
  auto window_for = [&](int cw, int ch) {
    RECT r{0, 0, cw, ch};
    AdjustWindowRectExForDpi(&r, style, FALSE, ex, (UINT)dpi);
    return r;
  };
  // With the strip, room for its regular covers' rows (ui_model.h: design_client_h).
  const int releases = st.strip_on ? (int)st.catalog.releases.size() : 0;
  int cw = dip(kDesignClientW, ldpi), ch = dip(design_client_h(releases), ldpi);
  RECT wr = window_for(cw, ch);
  // Never larger than the work area (a small laptop screen at 150%).
  const int maxw = (wa.right - wa.left) * 96 / 100, maxh = (wa.bottom - wa.top) * 96 / 100;
  if (!st.forced_dpi) {
    if (wr.right - wr.left > maxw) cw -= (wr.right - wr.left) - maxw;
    if (wr.bottom - wr.top > maxh) ch -= (wr.bottom - wr.top) - maxh;
    wr = window_for(cw, ch);
  }
  const int w = wr.right - wr.left, h = wr.bottom - wr.top;
  RECT anchor = wa;
  if (anchor_owner) GetWindowRect(owner, &anchor);
  int x = (anchor.left + anchor.right - w) / 2, y = (anchor.top + anchor.bottom - h) / 2;
  x = std::clamp(x, (int)wa.left, std::max((int)wa.left, (int)wa.right - w));
  y = std::clamp(y, (int)wa.top, std::max((int)wa.top, (int)wa.bottom - h));
  SetWindowPos(st.dlg, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

void init_dialog(State& st) {
  // The screenshot hook's window must never become active: it would take
  // the keyboard from whatever the user is typing into (and a stray letter
  // would pick a module by type-ahead).
  if (st.offscreen) SetWindowLongW(st.dlg, GWL_EXSTYLE, GetWindowLongW(st.dlg, GWL_EXSTYLE) | WS_EX_NOACTIVATE);
  SetDialogDpiChangeBehavior(st.dlg, DDC_DISABLE_ALL, DDC_DISABLE_ALL);
  for (HWND c = GetWindow(st.dlg, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
    keep_own_layout(c);
  }
  st.theme.load(st.theme_mode);
  st.theme.set_dpi(dpi_of(st));
  load_logo(st);

  load_settings(settings_path(), st.settings);
  st.controls = st.settings.controls;
  st.icons.set_thumbs_dir(thumbs_dir());
  load_catalog_into(st);

  auto item = [&](int id) { return GetDlgItem(st.dlg, id); };
  // Which surface each control sits on (for its background).
  for (int id : {IDC_MODULE_TITLE, IDC_MODULE_BADGE, IDC_ABOUT, IDC_CREDITS, IDC_SCALE_LABEL, IDC_SCALE,
                 IDC_MONITORS_LABEL, IDC_MONITORS, IDC_STRETCH, IDC_SOUND_LABEL, IDC_SOUND, IDC_VOLUME_LABEL, IDC_VOLUME,
                 IDC_VOLUME_VALUE, IDC_SOUND_NOTE, IDC_PANEL_DEFAULTS, IDC_WELCOME_IMPORT}) {
    set_surface(item(id), Surface::card);
  }
  set_button_role(item(IDC_MODE_SINGLE), ButtonRole::segment_left);
  set_button_role(item(IDC_MODE_RANDOM), ButtonRole::segment_right);
  set_button_role(item(IDC_CHECK_ALL), ButtonRole::subtle);
  set_button_role(item(IDC_CHECK_NONE), ButtonRole::subtle);
  set_button_role(item(IDC_PREVIEW), ButtonRole::standard, L'');
  set_button_role(item(IDC_IMPORT), ButtonRole::standard, L'\uE958');         // a disc: from your CD
  set_button_role(item(IDC_WELCOME_IMPORT), ButtonRole::accent, L'\uE958');
  set_button_role(item(IDC_PANEL_DEFAULTS), ButtonRole::subtle, L'\uE7A7');   // undo
  set_button_role(item(IDOK), ButtonRole::accent);
  for (int id : {IDC_DURATION, IDC_SCALE, IDC_MONITORS, IDC_SOUND}) subclass_combo(item(id), &st.theme);
  HWND about = item(IDC_ABOUT);

  st.list = item(IDC_MODULE_LIST);
  ListView_SetExtendedListViewStyle(st.list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
  set_surface(st.list, Surface::card);
  LVCOLUMNW col{};
  col.mask = LVCF_WIDTH;
  col.cx = 100;
  ListView_InsertColumn(st.list, 0, &col);
  SetWindowSubclass(st.list, list_proc, 3, reinterpret_cast<DWORD_PTR>(&st));
  attach_overlay_scrollbar(st.list, &st.theme);
  attach_overlay_scrollbar(about, &st.theme);

  st.panel = CreateDialogParamW(st.hinst, MAKEINTRESOURCEW(IDD_PANEL), st.dlg, panel_proc, (LPARAM)&st);
  SetWindowLongPtrW(st.panel, GWLP_ID, IDC_PANEL);
  SetDialogDpiChangeBehavior(st.panel, DDC_DISABLE_ALL, DDC_DISABLE_ALL);
  // Its thumb stays visible (6 DIP) whenever rows are out of sight.
  attach_overlay_scrollbar(st.panel, &st.theme, true);
  // Tab order: the panel's controls come right after the About text, then
  // "Restore defaults" under them.
  SetWindowPos(st.panel, item(IDC_CREDITS), 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  SetWindowPos(item(IDC_PANEL_DEFAULTS), st.panel, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  // A panel row reached by Tab (or a click) while out of sight scrolls into view.
  g_state = &st;
  st.focus_hook = SetWinEventHook(EVENT_OBJECT_FOCUS, EVENT_OBJECT_FOCUS, nullptr, focus_event, GetCurrentProcessId(),
                                  GetCurrentThreadId(), WINEVENT_OUTOFCONTEXT);
  ShowWindow(st.panel, SW_SHOW);
  if (!st.offscreen) {
    st.tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT,
                             CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, st.panel, nullptr, st.hinst, nullptr);
    SendMessageW(st.tip, TTM_SETMAXTIPWIDTH, 0, 320 * 2);
    // The release chip's "Also on:" (a tool over its place in the dialog).
    st.chip_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                                  CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, st.dlg, nullptr, st.hinst, nullptr);
    SendMessageW(st.chip_tip, TTM_SETMAXTIPWIDTH, 0, 320 * 2);
    TOOLINFOW ti{sizeof(ti)};
    ti.uFlags = TTF_SUBCLASS;
    ti.hwnd = st.dlg;
    ti.uId = 1;
    ti.lpszText = const_cast<wchar_t*>(L"");
    SendMessageW(st.chip_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    ti.uId = 2;   // the rotation line (update_rotation_tip)
    SendMessageW(st.chip_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    // The footer's credit: where its link goes.
    ti.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
    ti.uId = (UINT_PTR)item(IDC_FOOTER_CREDIT);
    ti.lpszText = const_cast<wchar_t*>(kFooterCreditUrl);
    SendMessageW(st.chip_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
  }
  // The link's window text, which screen readers read, is the phrase it
  // draws (the .rc's is a placeholder).
  SetWindowTextW(item(IDC_FOOTER_CREDIT), (std::wstring(kFooterCreditLead) + L" " + kFooterCreditName).c_str());
  describe_footer_credit(item(IDC_FOOTER_CREDIT));
  st.preview = create_live_preview(st.dlg, IDC_LIVE_PREVIEW, st.hinst);
  SetWindowPos(st.preview, item(IDC_CHECK_NONE), 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

  apply_theme(st);
  set_fonts(st);
  set_row_height(st);

  // Previews from dialogs that died (or were killed) left their file behind.
  if (int n = sweep_stale_preview_settings(temp_dir())) log_line("dialog: removed %d stale preview settings file(s)", n);

  // Random whenever the saver would rotate (Module=random, or a Randomize
  // list next to a named module, which the saver plays first).
  CheckRadioButton(st.dlg, IDC_MODE_SINGLE, IDC_MODE_RANDOM, st.settings.rotates() ? IDC_MODE_RANDOM : IDC_MODE_SINGLE);
  st.random = st.settings.rotates();

  HWND dur = item(IDC_DURATION);
  int dsel = 0;
  for (const DurationChoice& c : duration_choices(st.settings.duration_min)) {
    int i = (int)SendMessageW(dur, CB_ADDSTRING, 0, (LPARAM)c.label.c_str());
    SendMessageW(dur, CB_SETITEMDATA, i, c.minutes);
    if (c.minutes == st.settings.duration_min) dsel = i;
  }
  SendMessageW(dur, CB_SETCURSEL, dsel, 0);

  HWND scale = item(IDC_SCALE);
  auto add_scale = [&](const wchar_t* text, int hundredths) {
    int i = (int)SendMessageW(scale, CB_ADDSTRING, 0, (LPARAM)text);
    SendMessageW(scale, CB_SETITEMDATA, i, hundredths);
    return i;
  };
  // CB_ADDSTRING on an unsorted combo appends, so indices are insertion order.
  add_scale(L"Classic — 480 lines", 100);
  add_scale(L"Sharp — 720 lines", 150);
  int want = (int)(st.settings.scale * 100 + 0.5), sel = want == 150 ? 1 : 0;
  if (want != 100 && want != 150) {
    wchar_t buf[64];
    swprintf(buf, 64, L"Custom — %d lines", (int)(480 * st.settings.scale + 0.5));
    sel = add_scale(buf, want);
  }
  SendMessageW(scale, CB_SETCURSEL, sel, 0);

  HWND mon = item(IDC_MONITORS);
  SendMessageW(mon, CB_ADDSTRING, 0, (LPARAM)L"All monitors");
  SendMessageW(mon, CB_ADDSTRING, 0, (LPARAM)L"Primary monitor only");
  SendMessageW(mon, CB_SETCURSEL, st.settings.all_monitors ? 0 : 1, 0);
  // Random on several monitors: the same module on all of them, or
  // (DifferentPerMonitor=1) a different one on each.
  CheckDlgButton(st.dlg, IDC_PER_MONITOR, st.settings.different_per_monitor ? BST_CHECKED : BST_UNCHECKED);
  st.monitors = count_monitors(0);
  // 640x480 modules stretched over the monitor (StretchToFit=1), or kept in
  // shape between bars; the live preview shows it as it stands.
  CheckDlgButton(st.dlg, IDC_STRETCH, st.settings.stretch_to_fit ? BST_CHECKED : BST_UNCHECKED);
  live_preview_set_stretch(st.preview, st.settings.stretch_to_fit);

  // Sound (AUDIO.md §9): where it plays, or Off; and After Dark's volume.
  HWND snd = item(IDC_SOUND);
  SendMessageW(snd, CB_ADDSTRING, 0, (LPARAM)L"Primary monitor");
  SendMessageW(snd, CB_ADDSTRING, 0, (LPARAM)L"Off");
  SendMessageW(snd, CB_SETCURSEL, st.settings.sound ? 0 : 1, 0);
  init_slider(item(IDC_VOLUME), &st.theme, SliderSpec{0, 100, 10, std::clamp(st.settings.volume, 0, 100), L"Volume"});
  update_volume_value(st);
  update_sound(st);

  // The box-cover strip: one tile per release, selected as Collections says.
  set_button_role(item(IDC_STRIP_SHOW_ALL), ButtonRole::subtle);
  set_button_role(item(IDC_STRIP_GET_COVERS), ButtonRole::subtle);
  {
    CoverStrip::Callbacks cb;
    cb.changed = [&st] { on_filter_changed(st); };
    cb.change_cover = [&st](int tile) { on_change_cover(st, tile); };
    cb.can_change_cover = [&st] { return can_change_cover(st); };
    cb.remove = [&st](int tile) { on_remove_release(st, tile); };
    cb.can_remove = [&st] { return can_remove(st); };
    st.strip = std::make_unique<CoverStrip>(item(IDC_COVER_STRIP), &st.theme, std::move(cb), !st.offscreen);
    load_strip(st, effective_collections(st.settings.collections, st.catalog));
  }

  size_and_center(st);
  layout(st);

  // The rotation list, or while a single module is chosen the checklist kept
  // for Random ("-": the user left nothing checked). A list naming nothing
  // in the catalog any more (a re-import renamed everything) would show
  // nothing checked: treat it as "all".
  const std::vector<std::string>& list = dialog_checklist(st.settings);
  std::set<std::string> checked(list.begin(), list.end());
  bool any_known = std::any_of(st.catalog.modules.begin(), st.catalog.modules.end(),
                               [&](const Module& m) { return checked.count(m.id) != 0; });
  st.checks.clear();
  if (!dialog_checklist_none(st.settings)) {
    for (const Module& m : st.catalog.modules) {
      if (!any_known || checked.count(m.id)) st.checks.insert(m.id);
    }
  }
  st.chosen = st.settings.is_random() ? std::string() : st.settings.module;
  start_lane_probe(st);   // first: every module's preview waits for its answer
  populate_list(st);

  update_mode(st);
  update_assets_status(st);
  if (!st.offscreen) SetFocus(st.list);
  PostMessageW(st.dlg, WM_APP_SCHEDULE_THUMBS, 0, 0);
  log_line("dialog ready modules=%zu settings=%s", st.catalog.modules.size(), narrow(settings_path()).c_str());
}

INT_PTR CALLBACK dialog_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  auto* st = reinterpret_cast<State*>(GetWindowLongPtrW(h, DWLP_USER));
  switch (msg) {
    case WM_INITDIALOG:
      SetWindowLongPtrW(h, DWLP_USER, lp);
      st = reinterpret_cast<State*>(lp);
      st->dlg = h;
      init_dialog(*st);
      return FALSE;   // focus already set (or, off-screen, none wanted)
    case WM_ERASEBKGND:
      return TRUE;    // WM_PAINT covers every pixel
    case WM_ACTIVATE:
      // The dialog manager gives the focus back, on activation, to the control
      // that had it when the dialog lost it. If that is the footer credit and
      // it has hidden since (the window narrowed while inactive), the keyboard
      // moves on as place_footer_credit moves it: checked once that restore
      // is done.
      if (st && st->list && LOWORD(wp) != WA_INACTIVE) PostMessageW(h, WM_APP_FOOTER_CREDIT_FOCUS, 0, 0);
      break;   // DefDlgProc saves and restores the focus
    case WM_APP_FOOTER_CREDIT_FOCUS:
      if (st && st->list) {
        HWND link = GetDlgItem(h, IDC_FOOTER_CREDIT);
        if (link && GetFocus() == link && !IsWindowVisible(link)) {
          if (HWND next = GetNextDlgTabItem(h, link, FALSE); next && next != link) {
            SendMessageW(h, WM_NEXTDLGCTL, (WPARAM)next, TRUE);
          }
        }
      }
      return TRUE;
    case WM_PAINT: {
      if (!st || !st->list) break;
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      paint(*st, dc);
      EndPaint(h, &ps);
      return TRUE;
    }
    case WM_PRINTCLIENT:
      if (st && st->list) paint(*st, (HDC)wp);
      return TRUE;
    case WM_SIZE:
      if (st && st->list) layout(*st);
      return TRUE;
    case WM_GETMINMAXINFO: {
      if (!st || !st->dlg) break;
      auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
      // With the strip, 80 DIP more: its compact band over today's minimum.
      RECT r{0, 0, dip(kMinClientW, dpi_of(*st)), dip(st->strip_on ? kMinClientHStrip : kMinClientH, dpi_of(*st))};
      AdjustWindowRectExForDpi(&r, (DWORD)GetWindowLongW(h, GWL_STYLE), FALSE, (DWORD)GetWindowLongW(h, GWL_EXSTYLE),
                               GetDpiForWindow(h));
      mm->ptMinTrackSize = {r.right - r.left, r.bottom - r.top};
      // The screenshot hook: a size= larger than this machine's screen (the
      // regular strip at 150% and up) is laid out as on a monitor that holds it.
      if (st->offscreen) mm->ptMaxTrackSize = {std::max<LONG>(mm->ptMaxTrackSize.x, 16384), std::max<LONG>(mm->ptMaxTrackSize.y, 16384)};
      return TRUE;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
      if (!st || !st->list) break;
      return ctl_color(*st, (HDC)wp, (HWND)lp);
    case WM_CTLCOLORLISTBOX:
      if (!st || !st->list) break;
      SetTextColor((HDC)wp, st->theme.pal.text);
      SetBkColor((HDC)wp, st->theme.pal.card);
      return (INT_PTR)combo_list_brush(st->theme);
    case WM_MEASUREITEM:
      if (!st) break;
      measure_combo_item(st->theme, reinterpret_cast<MEASUREITEMSTRUCT*>(lp));
      return TRUE;
    case WM_DRAWITEM: {
      if (!st || !st->list) break;
      auto* di = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
      if (di->CtlID == IDC_MODULE_TITLE) {
        // The module's name, in the face (and on the lines) layout() chose.
        const Theme& t = st->theme;
        fill_rect(di->hDC, di->rcItem, t.pal.card);
        const UINT fmt = st->title_lines >= 2 ? DT_WORDBREAK | DT_END_ELLIPSIS | DT_EDITCONTROL | DT_NOPREFIX
                                              : DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX;
        draw_text(di->hDC, window_text(di->hwndItem), di->rcItem, st->title_font ? st->title_font : t.fonts.subtitle,
                  t.pal.text, fmt);
        return TRUE;
      }
      if (di->CtlID == IDC_MODULE_BADGE) {
        // One or two pills: "Deluxe · Coming soon".
        std::wstring text = window_text(di->hwndItem);
        fill_rect(di->hDC, di->rcItem, st->theme.pal.card);
        size_t dot = text.find(L" · ");
        std::wstring first = dot == std::wstring::npos ? text : text.substr(0, dot);
        std::wstring second = dot == std::wstring::npos ? L"" : text.substr(dot + 3);
        RECT r = di->rcItem;
        if (!first.empty()) {
          BadgeKind k = second.empty() ? st->badge_kind : BadgeKind::neutral;
          draw_badge(di->hDC, r, first, k, st->theme, st->theme.pal.card);
          r.left += measure_text(di->hDC, first, st->theme.fonts.caption).cx + st->theme.px(16) + st->theme.px(6);
        }
        if (!second.empty()) draw_badge(di->hDC, r, second, st->badge_kind, st->theme, st->theme.pal.card);
        return TRUE;
      }
      draw_combo_item(st->theme, di);
      return TRUE;
    }
    case WM_NOTIFY: {
      if (!st || !st->list) break;
      auto* hdr = reinterpret_cast<NMHDR*>(lp);
      if (hdr->idFrom == IDC_MODULE_LIST) {
        if (hdr->code == NM_CUSTOMDRAW) {
          SetWindowLongPtrW(h, DWLP_MSGRESULT, list_custom_draw(*st, reinterpret_cast<NMLVCUSTOMDRAW*>(lp)));
          return TRUE;
        }
        if (hdr->code == LVN_ITEMCHANGING && !st->populating && !st->random) {
          // Single module: the checklist is put aside; only Random edits it.
          auto* nm = reinterpret_cast<NMLISTVIEW*>(lp);
          if ((nm->uChanged & LVIF_STATE) && ((nm->uNewState ^ nm->uOldState) & LVIS_STATEIMAGEMASK)) {
            SetWindowLongPtrW(h, DWLP_MSGRESULT, TRUE);
            return TRUE;
          }
        }
        if (hdr->code == LVN_ITEMCHANGED && !st->populating) {
          auto* nm = reinterpret_cast<NMLISTVIEW*>(lp);
          const bool valid = nm->lParam >= 0 && nm->lParam < (LPARAM)st->catalog.modules.size();
          if ((nm->uChanged & LVIF_STATE) && (nm->uNewState & LVIS_SELECTED) && !(nm->uOldState & LVIS_SELECTED)) {
            if (valid) st->chosen = st->catalog.modules[nm->lParam].id;
            show_details(*st, (int)nm->lParam);
          }
          if ((nm->uChanged & LVIF_STATE) && ((nm->uNewState ^ nm->uOldState) & LVIS_STATEIMAGEMASK)) {
            // The list shows its rows' checks; the dialog keeps every release's.
            if (valid && nm->iItem >= 0) {
              const std::string& id = st->catalog.modules[nm->lParam].id;
              if (ListView_GetCheckState(st->list, nm->iItem)) st->checks.insert(id);
              else st->checks.erase(id);
            }
            update_summary(*st);
          }
        } else if (hdr->code == NM_DBLCLK) {
          on_preview(*st);
        } else if (hdr->code == NM_SETFOCUS || hdr->code == NM_KILLFOCUS) {
          InvalidateRect(st->list, nullptr, FALSE);
        }
        break;
      }
      if (hdr->code == NM_CUSTOMDRAW && hdr->idFrom == IDC_FOOTER_CREDIT) {
        SetWindowLongPtrW(h, DWLP_MSGRESULT, draw_footer_credit(*st, reinterpret_cast<NMCUSTOMDRAW*>(lp)));
        return TRUE;
      }
      if (hdr->code == NM_CUSTOMDRAW) {
        wchar_t cls[32] = {};
        GetClassNameW(hdr->hwndFrom, cls, 32);
        if (wcscmp(cls, WC_BUTTONW) == 0) {
          SetWindowLongPtrW(h, DWLP_MSGRESULT, custom_draw_button(st->theme, reinterpret_cast<NMCUSTOMDRAW*>(lp)));
          return TRUE;
        }
        if (wcscmp(cls, TRACKBAR_CLASSW) == 0) {   // Volume
          SetWindowLongPtrW(h, DWLP_MSGRESULT, custom_draw_trackbar(st->theme, reinterpret_cast<NMCUSTOMDRAW*>(lp)));
          return TRUE;
        }
      }
      break;
    }
    case WM_COMMAND: {
      if (!st || !st->list) break;
      switch (LOWORD(wp)) {
        case IDOK:
          if (on_ok(*st)) EndDialog(h, IDOK);
          return TRUE;
        case IDCANCEL:
          EndDialog(h, IDCANCEL);
          return TRUE;
        case IDC_MODE_SINGLE:
        case IDC_MODE_RANDOM:
          update_mode(*st);
          return TRUE;
        case IDC_CHECK_ALL:
        case IDC_CHECK_NONE: {
          BOOL on = LOWORD(wp) == IDC_CHECK_ALL;
          for (int i = 0, n = ListView_GetItemCount(st->list); i < n; ++i) ListView_SetCheckState(st->list, i, on);
          update_summary(*st);
          return TRUE;
        }
        case IDC_PREVIEW:
          on_preview(*st);
          return TRUE;
        case IDC_SOUND:
          if (HIWORD(wp) == CBN_SELCHANGE) update_sound(*st);
          return TRUE;
        case IDC_MONITORS:
          if (HIWORD(wp) == CBN_SELCHANGE) update_per_monitor(*st);
          return TRUE;
        case IDC_STRETCH:
          if (HIWORD(wp) == BN_CLICKED)
            live_preview_set_stretch(st->preview, IsDlgButtonChecked(st->dlg, IDC_STRETCH) == BST_CHECKED);
          return TRUE;
        case IDC_STRIP_SHOW_ALL:
          if (st->strip && st->strip->selected_count()) {
            // Back to every release; the keyboard goes to the tiles (the link hides).
            st->strip->select({});
            on_filter_changed(*st);
            if (HWND tile = st->strip->tile_hwnd(0); tile && GetFocus() == nullptr) SetFocus(tile);
          }
          return TRUE;
        case IDC_IMPORT:
        case IDC_WELCOME_IMPORT:
          on_import(*st);
          return TRUE;
        case IDC_STRIP_GET_COVERS:
          on_get_covers(*st);
          return TRUE;
        case IDC_FOOTER_CREDIT:
          open_credit(*st);
          return TRUE;
        case IDC_PANEL_DEFAULTS:
          if (const Module* m = shown_module(*st)) {
            st->controls.erase(m->id);
            build_panel(*st, st->shown);
            refresh_preview(*st);
          }
          return TRUE;
      }
      break;
    }
    case WM_HSCROLL:
      // The Volume slider (arrows by 1, Page Up/Down by 10, Home/End, the mouse).
      if (!st || !st->list || (HWND)lp != GetDlgItem(h, IDC_VOLUME)) break;
      update_volume_value(*st);
      return TRUE;
    case WM_MOUSEMOVE: {
      // The preview lets the pointer through to us: name the module on hover.
      if (!st || !st->preview) break;
      POINT pt{(short)LOWORD(lp), (short)HIWORD(lp)};
      RECT pr{};
      GetWindowRect(st->preview, &pr);
      MapWindowPoints(nullptr, h, reinterpret_cast<POINT*>(&pr), 2);
      const bool over = PtInRect(&pr, pt) != FALSE;
      if (over != st->hover_preview) {
        st->hover_preview = over;
        live_preview_set_hover(st->preview, over);
        if (over) {
          TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
          TrackMouseEvent(&tme);
        }
      }
      break;
    }
    case WM_SETCURSOR:
      // A link's pointer over the credit (a button passes WM_SETCURSOR to us first).
      if (st && st->list && (HWND)wp == GetDlgItem(h, IDC_FOOTER_CREDIT) && LOWORD(lp) == HTCLIENT) {
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        SetWindowLongPtrW(h, DWLP_MSGRESULT, TRUE);
        return TRUE;
      }
      break;
    case WM_MOUSELEAVE:
      if (st && st->hover_preview) {
        st->hover_preview = false;
        live_preview_set_hover(st->preview, false);
      }
      break;
    case WM_APP_PREVIEW_DONE:
      if (!st) break;
      st->preview_running = false;
      delete_preview_settings(*st);
      if (st->preview) live_preview_pause(st->preview, false);
      if (st->thumbgen) st->thumbgen->pause(false);
      update_assets_status(*st);
      return TRUE;
    case WM_APP_IMPORT_DONE:
      if (!st) break;
      on_import_done(*st, (DWORD)wp);
      return TRUE;
    case WM_APP_COVER_DONE:
      if (!st) break;
      on_change_cover_done(*st, (DWORD)wp);
      return TRUE;
    case WM_APP_REMOVE_DONE:
      if (!st) break;
      on_remove_done(*st, (DWORD)wp);
      return TRUE;
    case WM_APP_LANE_PROBE: {
      std::unique_ptr<HostCapabilities> caps(reinterpret_cast<HostCapabilities*>(lp));
      if (!st || !caps) break;
      const bool was_live = button_live(*st, st->shown);
      const Module* m = shown_module(*st);
      const ModuleRun before = m ? run_state(*st, *m) : ModuleRun::runs;
      st->probing = false;
      st->caps = *caps;
      log_line("dialog: host capabilities: %s", caps->known ? caps->line.c_str() : "(no answer)");
      // Every module waited for this. A host too old to answer is taken to
      // run everything: a module whose lane it lacks exits 3 there, and
      // becomes "Coming soon" by itself (mark_cant_run).
      const bool rebuilt = availability_changed(*st, before);
      // The shown module's buttons may have come alive (rebuilt only then:
      // a rebuild takes the keyboard focus from under the user).
      if (!rebuilt && m && button_live(*st, st->shown) != was_live &&
          std::any_of(m->controls.begin(), m->controls.end(),
                      [](const Control& c) { return c.type == ControlType::button; })) {
        build_panel(*st, st->shown);
      }
      return TRUE;
    }
    case WM_APP_CONFIGURE_DONE: {
      std::unique_ptr<std::string> out(reinterpret_cast<std::string*>(lp));
      if (!st) break;
      on_configure_done(*st, (DWORD)wp, out ? *out : std::string());
      return TRUE;
    }
    // No re-disabling while a configure run goes on: a module's modal dialog
    // re-enables its owner as it closes and hands it the activation
    // (adhostwin_main.cc); a window disabled again at that moment cannot take
    // it, and Windows activates some other application's window instead, so
    // the settings window would drop behind it. The host exits moments later
    // (on_configure_done), and a further dialog it opens disables us again.
    case WM_APP_SCHEDULE_THUMBS:
      if (st) schedule_thumbnails(*st);
      return TRUE;
    case WM_APP_THUMBS:
      if (!st) break;
      if (wp == kThumbLaneMissing && st->thumbgen) mark_cant_run(*st, st->thumbgen->take_cant_run());
      if (wp == kThumbSaved) {
        st->icons.recheck_pictureless();
        InvalidateRect(st->list, nullptr, FALSE);
        RECT icon = rc_of(st->L.module_icon);
        InvalidateRect(h, &icon, FALSE);
      }
      return TRUE;
    case WM_APP_LIVE_STATUS:
      if (st && wp == kLiveLaneMissing) mark_cant_run(*st, live_preview_take_cant_run(st->preview));
      if (st && wp == kLiveThumbSaved) {
        // A new thumbnail: modules still on the placeholder look again.
        st->icons.recheck_pictureless();
        InvalidateRect(st->list, nullptr, FALSE);
        RECT icon = rc_of(st->L.module_icon);
        InvalidateRect(h, &icon, FALSE);
      }
      return TRUE;
    case WM_DPICHANGED: {
      if (!st) break;
      if (!st->forced_dpi) dpi_changed(*st, HIWORD(wp));
      auto* r = reinterpret_cast<RECT*>(lp);
      SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      build_panel(*st, st->shown);
      layout(*st);
      // The list scrolls in pixels, which don't rescale: place it afresh.
      position_list(*st, ListView_GetNextItem(st->list, -1, LVNI_SELECTED));
      RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
      return TRUE;
    }
    case WM_DISPLAYCHANGE:
      // A monitor plugged in or out: "A different module on each monitor"
      // shows only while there are several.
      if (st && st->list) {
        const int n = count_monitors(++st->display_changes);
        if (n != st->monitors) {
          log_line("dialog: %d monitor(s)", n);
          st->monitors = n;
          update_per_monitor(*st);
          layout(*st);
          // In Random the list's height changed with it: the selection afresh.
          if (st->random) position_list(*st, ListView_GetNextItem(st->list, -1, LVNI_SELECTED));
        }
      }
      break;
    case WM_SETTINGCHANGE:
    case WM_SYSCOLORCHANGE:
    case WM_THEMECHANGED:
    case WM_DWMCOLORIZATIONCOLORCHANGED:
      // Light/dark, the accent or high contrast changed: the palette, the
      // chrome, the native parts, popup menus and a full redraw.
      if (st && st->list && st->theme_mode == ThemeMode::system && is_theme_change(msg, wp, lp)) apply_theme(*st);
      break;
    case WM_DESTROY:
      if (st) st->thumbgen.reset();
      if (st && st->focus_hook) {
        UnhookWinEvent(st->focus_hook);
        st->focus_hook = nullptr;
      }
      g_state = nullptr;
      forget_looks(h);
      if (st && st->logo) {
        DestroyIcon(st->logo);
        st->logo = nullptr;
      }
      break;
  }
  return FALSE;
}

// ---- screenshot hook (AD_SCR_TEST_SCREENSHOT) ----------------------------------------------
// A test hook: compiled into LongAfterDark-test.scr only (AD_SCR_TEST_HOOKS,
// scr/CMakeLists.txt); the LongAfterDark.scr that ships never reads it.
#if AD_SCR_TEST_HOOKS
// Renders the dialog to a PNG without it ever appearing on screen: created
// hidden, parked off every monitor and cloaked, drawn with PrintWindow.
//   AD_SCR_TEST_SCREENSHOT=<png>
//   AD_SCR_TEST_SCREENSHOT_STATE=key=value;…   theme=light|dark|hc  module=<id>
//     mode=single|random  dpi=<n>  size=<w>x<h> (DIPs)  focus=list|ok|single|random|duration|preview|slider
//     wait=<ms> (at most; default 5000)  frames=<n> (live-preview frames to wait for; default 45)
//     dpichange=<n> (send WM_DPICHANGED as if dragged to a monitor at that DPI)
//     hover=preview (the pointer over the live preview: the module's name shows)
//     thumbgen=1|wait (take missing thumbnails in the background; wait: until all are taken,
//     within wait=<ms>)
//     report=<path> (write where the list shows in the picture, the card colour, and
//     whether anything straddles the list's top edge: the smoke tests check the pixels;
//     also where the box-cover strip's tiles area shows, strip_mode=regular|compact|hidden,
//     the base colour and how many rows the list shows; each group's accessible name and
//     its title as drawn; the host's capabilities line, the modules "Coming soon", and the
//     shown module's id, chip, whether its buttons are live and Preview is enabled; the
//     strip's scroll position, its rows and the most tiles a row holds, each tile's window
//     or "hidden", its chevrons and status line)
//     collections=<id>,… (the strip's filter)  focus=strip (the first selected tile, else
//     the first)  hover=strip:<id> (that tile hovered)
//     sound=off (the Sound dropdown at Off)  volume=<0..100>  focus=sound|volume
//     hover=credit (the footer's credit under the pointer)  pressed=credit (...held down)
//     focus=credit (its focus ring); the report says where it shows (credit=, credit_lead=,
//     credit_name=; "hidden" when it doesn't fit), the assets line's text (assets_text=) and
//     Preview (preview_button=), in the picture's pixels
//     monitors=primary (the Monitors dropdown at Primary monitor only)  different=1|0 (the
//     "A different module on each monitor" box checked or not)  focus=permonitor (its focus
//     ring); the report says how many monitors the dialog counts (monitors=), where the box
//     shows (per_monitor=x,y,w,h or "hidden"), whether it is enabled and checked
//     (per_monitor_enabled=, per_monitor_checked=) and its text fits beside its box
//     (per_monitor_fits=), and where "Change module every"'s dropdown and the list's card are
//     (duration=, list_card=)
//     stretch=1|0 (the "Stretch to fit the screen" box checked or not; the live preview
//     follows)  focus=stretch (its focus ring); the report says where it shows (stretch=),
//     whether it is checked (stretch_checked=) and its text fits (stretch_fits=), and where
//     the options card, Resolution's and Sound's dropdowns are (options_card=, scale=, sound=)

std::map<std::wstring, std::wstring> parse_state(const std::wstring& s) {
  std::map<std::wstring, std::wstring> kv;
  size_t p = 0;
  while (p < s.size()) {
    size_t semi = s.find(L';', p);
    std::wstring part = s.substr(p, semi == std::wstring::npos ? std::wstring::npos : semi - p);
    size_t eq = part.find(L'=');
    if (eq != std::wstring::npos) kv[part.substr(0, eq)] = part.substr(eq + 1);
    else if (!part.empty()) kv[part] = L"1";
    if (semi == std::wstring::npos) break;
    p = semi + 1;
  }
  return kv;
}

int run_screenshot(State& st, const std::wstring& png) {
  auto kv = parse_state(env_w(L"AD_SCR_TEST_SCREENSHOT_STATE"));
  // One of Windows 11's contrast themes standing in for the system colours
  // under theme=hc (adw_ui's set_test_hc_scheme).
  if (std::wstring hc = env_w(L"AD_UI_TEST_HC_SCHEME"); !hc.empty()) set_test_hc_scheme(hc.c_str());
  if (kv[L"theme"] == L"dark") st.theme_mode = ThemeMode::dark;
  if (kv[L"theme"] == L"light") st.theme_mode = ThemeMode::light;
  if (kv[L"theme"] == L"hc") st.theme_mode = ThemeMode::high_contrast;
  if (!kv[L"dpi"].empty()) st.forced_dpi = std::clamp(_wtoi(kv[L"dpi"].c_str()), 72, 480);
  st.offscreen = true;
  // Thumbnails in the background only when asked ("wait": until they are all taken).
  st.thumbgen_allowed = kv[L"thumbgen"] == L"1" || kv[L"thumbgen"] == L"wait";
  HWND owner = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"", WS_POPUP, -32000, -32000, 1, 1, nullptr, nullptr,
                               st.hinst, nullptr);
  HWND dlg = CreateDialogParamW(st.hinst, MAKEINTRESOURCEW(IDD_SETTINGS), owner, dialog_proc, (LPARAM)&st);
  if (!dlg) return 1;
  park_offscreen(dlg);
  if (!kv[L"size"].empty()) {
    int w = 0, h = 0;
    if (swscanf(kv[L"size"].c_str(), L"%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
      RECT r{0, 0, dip(w, dpi_of(st)), dip(h, dpi_of(st))};
      AdjustWindowRectExForDpi(&r, (DWORD)GetWindowLongW(dlg, GWL_STYLE), FALSE, (DWORD)GetWindowLongW(dlg, GWL_EXSTYLE),
                               GetDpiForWindow(dlg));
      SetWindowPos(dlg, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
  }
  if (!kv[L"dpichange"].empty()) {
    // As Windows does when the window is dragged to a monitor of another
    // scale: the new DPI and a suggested window rect scaled to match.
    const int to = std::clamp(_wtoi(kv[L"dpichange"].c_str()), 72, 480), from = (int)GetDpiForWindow(dlg);
    RECT wr{};
    GetWindowRect(dlg, &wr);
    RECT sug{wr.left, wr.top, wr.left + MulDiv(wr.right - wr.left, to, from), wr.top + MulDiv(wr.bottom - wr.top, to, from)};
    SendMessageW(dlg, WM_DPICHANGED, MAKEWPARAM(to, to), (LPARAM)&sug);
  }
  if (kv[L"mode"] == L"random" || kv[L"mode"] == L"single") {
    CheckRadioButton(dlg, IDC_MODE_SINGLE, IDC_MODE_RANDOM, kv[L"mode"] == L"random" ? IDC_MODE_RANDOM : IDC_MODE_SINGLE);
    update_mode(st);
  }
  if (kv.count(L"collections") && st.strip) {
    // The strip's filter, as if those tiles had been clicked.
    std::vector<std::string> ids;
    std::string rest = narrow(kv[L"collections"]);
    for (size_t p = 0; p <= rest.size();) {
      size_t comma = rest.find(',', p);
      std::string id = rest.substr(p, comma == std::string::npos ? std::string::npos : comma - p);
      if (!id.empty()) ids.push_back(id);
      if (comma == std::string::npos) break;
      p = comma + 1;
    }
    st.strip->select(ids);
    on_filter_changed(st);
  }
  if (!kv[L"module"].empty()) {
    std::string id = narrow(kv[L"module"]);
    for (size_t i = 0; i < st.catalog.modules.size(); ++i) {
      if (st.catalog.modules[i].id != id) continue;
      int item = item_for_module(st, (int)i);
      if (item >= 0) {
        ListView_SetItemState(st.list, item, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        position_list(st, item);
      } else {
        // Its release is filtered out: chosen all the same, no row selected,
        // and the details show a module the list shows.
        st.chosen = id;
        ListView_SetItemState(st.list, -1, 0, LVIS_SELECTED);
        show_details(st, details_after_filter(st.model, (int)i, st.shown));
      }
    }
  }
  if (kv[L"sound"] == L"off") {
    SendDlgItemMessageW(dlg, IDC_SOUND, CB_SETCURSEL, 1, 0);
    update_sound(st);
  }
  if (!kv[L"volume"].empty()) {
    SendDlgItemMessageW(dlg, IDC_VOLUME, TBM_SETPOS, TRUE, std::clamp(_wtoi(kv[L"volume"].c_str()), 0, 100));
    update_volume_value(st);
  }
  if (kv[L"monitors"] == L"primary") {
    SendDlgItemMessageW(dlg, IDC_MONITORS, CB_SETCURSEL, 1, 0);
    update_per_monitor(st);
  }
  if (!kv[L"different"].empty()) CheckDlgButton(dlg, IDC_PER_MONITOR, kv[L"different"] == L"1" ? BST_CHECKED : BST_UNCHECKED);
  if (!kv[L"stretch"].empty()) {
    CheckDlgButton(dlg, IDC_STRETCH, kv[L"stretch"] == L"1" ? BST_CHECKED : BST_UNCHECKED);
    live_preview_set_stretch(st.preview, kv[L"stretch"] == L"1");
  }
  if (kv[L"hover"] == L"preview") {
    st.hover_preview = true;
    live_preview_set_hover(st.preview, true);
  }
  if (kv[L"hover"] == L"credit") {
    st.footer_credit_hover = true;
    InvalidateRect(GetDlgItem(dlg, IDC_FOOTER_CREDIT), nullptr, TRUE);
  }
  if (kv[L"pressed"] == L"credit") SendDlgItemMessageW(dlg, IDC_FOOTER_CREDIT, BM_SETSTATE, TRUE, 0);
  if (kv[L"hover"].rfind(L"strip:", 0) == 0 && st.strip) {
    const std::string id = narrow(kv[L"hover"].substr(6));
    for (size_t i = 0; i < st.strip->count(); ++i) {
      if (st.strip->tile(i).id == id) st.strip->set_hover_override((int)i);
    }
  }
  // As after a mouse click: no focus rects or mnemonics unless asked for
  // (otherwise they'd follow whatever input the machine saw last).
  SendMessageW(dlg, WM_CHANGEUISTATE, MAKEWPARAM(UIS_SET, UISF_HIDEFOCUS | UISF_HIDEACCEL), 0);
  if (!kv[L"focus"].empty()) {
    static const std::map<std::wstring, int> ids = {{L"list", IDC_MODULE_LIST}, {L"ok", IDOK}, {L"single", IDC_MODE_SINGLE},
                                                    {L"random", IDC_MODE_RANDOM}, {L"duration", IDC_DURATION},
                                                    {L"preview", IDC_PREVIEW}, {L"sound", IDC_SOUND},
                                                    {L"volume", IDC_VOLUME}, {L"credit", IDC_FOOTER_CREDIT},
                                                    {L"permonitor", IDC_PER_MONITOR}, {L"stretch", IDC_STRETCH}};
    SendMessageW(dlg, WM_CHANGEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS | UISF_HIDEACCEL), 0);
    HWND f = nullptr;
    if (auto it = ids.find(kv[L"focus"]); it != ids.end()) f = GetDlgItem(dlg, it->second);
    else if (kv[L"focus"] == L"slider") f = GetDlgItem(st.panel, IDC_PANEL_BASE + IDC_PART_INPUT);
    else if (kv[L"focus"] == L"strip" && st.strip) {
      // As when the tile takes the focus: scrolled into view.
      st.strip->ensure_visible(st.strip->home_tile());
      f = st.strip->tile_hwnd(st.strip->home_tile());
    }
    if (f) {
      set_focus_override(f);
      RedrawWindow(dlg, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
    }
  }
  // Let the list paint, the lane probe answer and the live preview run a while.
  const int wait = kv[L"wait"].empty() ? 5000 : _wtoi(kv[L"wait"].c_str());
  const unsigned long long frames = kv[L"frames"].empty() ? 45 : (unsigned long long)_wtoi(kv[L"frames"].c_str());
  const ULONGLONG start = GetTickCount64(), until = start + (ULONGLONG)std::max(300, wait);
  MSG m;
  for (;;) {
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&m);
      DispatchMessageW(&m);
    }
    ULONGLONG now = GetTickCount64();
    if (now >= until) break;
    if (kv[L"thumbgen"] == L"wait") {
      if (st.thumbgen_ran && (!st.thumbgen || st.thumbgen->idle())) break;
    } else if (now - start > 1500 && live_preview_frames(st.preview) >= frames) {
      break;
    }
    MsgWaitForMultipleObjects(0, nullptr, FALSE, 15, QS_ALLINPUT);
  }
  // Again, in case anything cleared it meanwhile: the picture shows the cues
  // only when asked to.
  if (kv[L"focus"].empty()) SendMessageW(dlg, WM_CHANGEUISTATE, MAKEWPARAM(UIS_SET, UISF_HIDEFOCUS | UISF_HIDEACCEL), 0);
  RedrawWindow(dlg, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW | RDW_FRAME);
  // A cloaked window that never rendered through a plain PrintWindow can
  // capture black (adw_ui's capture notes): render it once that way first.
  settle_for_capture(dlg, 0);
  std::string err;
  POINT origin{};
  bool ok = capture_window_png(dlg, png, &err, &origin);
  if (!ok) log_line("screenshot failed: %s", err.c_str());
  if (ok && !kv[L"report"].empty()) {
    // For the tests: where the list shows in the picture, the card's colour,
    // and whether anything straddles the list's top edge.
    RECT vis = list_visible_rect(st);
    MapWindowPoints(st.list, nullptr, reinterpret_cast<POINT*>(&vis), 2);
    const COLORREF card = st.theme.pal.card;
    char buf[512];
    int n = snprintf(buf, sizeof(buf), "list=%ld,%ld,%ld,%ld\ncard=%02X%02X%02X\ntop=%s\ndpi=%d\n", vis.left - origin.x,
                     vis.top - origin.y, vis.right - vis.left, vis.bottom - vis.top, GetRValue(card), GetGValue(card),
                     GetBValue(card), list_geom(st).at_top() ? "cut" : "clean", st.theme.dpi);
    // The strip: where its tiles area shows in the picture, and its form.
    const StripMode mode = st.strip_on ? st.L.strip_mode : StripMode::hidden;
    RECT sa{st.L.strip.x, st.L.strip.y, st.L.strip.right(), st.L.strip.bottom()};
    MapWindowPoints(dlg, nullptr, reinterpret_cast<POINT*>(&sa), 2);
    if (mode == StripMode::hidden) sa = RECT{0, 0, 0, 0};
    else OffsetRect(&sa, -origin.x, -origin.y);
    const COLORREF base = st.theme.pal.base;
    snprintf(buf + n, sizeof(buf) - n, "strip=%ld,%ld,%ld,%ld\nstrip_mode=%s\nbase=%02X%02X%02X\nshown=%zu\nscroll=%d\n",
             sa.left, sa.top, sa.right - sa.left, sa.bottom - sa.top,
             mode == StripMode::regular ? "regular" : mode == StripMode::compact ? "compact" : "hidden", GetRValue(base),
             GetGValue(base), GetBValue(base), st.model.shown, list_geom(st).pos);
    // What screen readers call each group (its LVGROUP header), and its
    // title as the header last drew it (whole, or ellipsized: "drawn<g>=").
    std::string report = buf;
    // The strip's own windows as the picture shows them: its scroll position
    // and stops, each tile ("hidden" while it lies outside the strip, not
    // shown), the chevrons and the status line, in the picture's pixels.
    if (mode != StripMode::hidden && st.strip) {
      auto shot = [&](HWND h, bool in_strip) {
        RECT r{}, box{}, both{};
        if (!h || !IsWindowVisible(h) || !GetWindowRect(h, &r)) return std::string("hidden");
        GetWindowRect(st.strip->hwnd(), &box);
        if (in_strip && !IntersectRect(&both, &r, &box)) return std::string("hidden");
        OffsetRect(&r, -origin.x, -origin.y);
        return std::to_string(r.left) + "," + std::to_string(r.top) + "," + std::to_string(r.right - r.left) + "," +
               std::to_string(r.bottom - r.top);
      };
      const StripLayout& g = st.strip->geometry();
      report += "strip_first=" + std::to_string(g.first) + "\nstrip_max_first=" + std::to_string(g.max_first) +
                "\nstrip_slots=" + std::to_string(g.slots) + "\nstrip_rows=" + std::to_string(g.rows) +
                "\nstrip_cols=" + std::to_string(g.cols) + "\n";
      for (size_t i = 0; i < st.strip->count(); ++i) {
        report += "tile" + std::to_string(i) + "=" + shot(st.strip->tile_hwnd((int)i), true) + "\n";
      }
      report += "chevron_left=" + shot(GetDlgItem(st.strip->hwnd(), IDC_STRIP_PREV), true) +
                "\nchevron_right=" + shot(GetDlgItem(st.strip->hwnd(), IDC_STRIP_NEXT), true) +
                "\nstrip_status=" + shot(GetDlgItem(dlg, IDC_STRIP_STATUS), false) + "\n";
    }
    for (size_t g = 0; g < st.model.groups.size(); ++g) {
      wchar_t name[256] = {};
      LVGROUP info{};
      info.cbSize = sizeof(info);
      info.mask = LVGF_HEADER;
      info.pszHeader = name;
      info.cchHeader = 256;
      const int gid = group_id(st.model.groups[g].release);
      ListView_GetGroupInfo(st.list, gid, &info);
      report += "group" + std::to_string(g) + "=" + narrow(name) + "\n";
      if (auto d = st.header_drawn.find(gid); d != st.header_drawn.end()) {
        report += "drawn" + std::to_string(g) + "=" + narrow(d->second) + "\n";
      }
    }
    // What this host can run (the smoke tests' config-abi): its answer, the
    // modules "Coming soon", and the module the details show ("details="):
    // its chip, its buttons live or not, Preview enabled or not.
    std::string soon;
    for (const Module& m : st.catalog.modules) {
      if (coming_soon(st, m)) soon += (soon.empty() ? "" : ",") + m.id;
    }
    const Module* shown = shown_module(st);
    report += "caps=" + (st.caps.known ? st.caps.line : std::string()) + "\nsoon=" + soon +
              "\ndetails=" + (shown ? shown->id : std::string()) +
              "\nbadge=" + narrow(window_text(GetDlgItem(dlg, IDC_MODULE_BADGE))) +
              "\nbutton_live=" + (button_live(st, st.shown) ? "1" : "0") +
              "\npreview_enabled=" + (IsWindowEnabled(GetDlgItem(dlg, IDC_PREVIEW)) ? "1" : "0") + "\n";
    // The footer's credit (the smoke tests' and the renders' checks): its
    // link's box and its two texts, the assets line's text and Preview.
    auto pic = [&](const Rc& r) {
      RECT a{r.x, r.y, r.right(), r.bottom()};
      MapWindowPoints(dlg, nullptr, reinterpret_cast<POINT*>(&a), 2);
      OffsetRect(&a, -origin.x, -origin.y);
      return std::to_string(a.left) + "," + std::to_string(a.top) + "," + std::to_string(a.right - a.left) + "," +
             std::to_string(a.bottom - a.top);
    };
    RECT ar{};
    GetWindowRect(GetDlgItem(dlg, IDC_ASSETS_STATUS), &ar);
    MapWindowPoints(nullptr, dlg, reinterpret_cast<POINT*>(&ar), 2);
    const bool credit =
        st.footer_credit.shown && (GetWindowLongW(GetDlgItem(dlg, IDC_FOOTER_CREDIT), GWL_STYLE) & WS_VISIBLE) != 0;
    report += "credit=" + (credit ? pic(st.footer_credit.box) : std::string("hidden")) +
              "\ncredit_lead=" + (credit ? pic(st.footer_credit.lead) : std::string("hidden")) +
              "\ncredit_name=" + (credit ? pic(st.footer_credit.name) : std::string("hidden")) +
              "\nassets_text=" + pic(Rc{(int)ar.left, (int)ar.top, st.assets_right - (int)ar.left, (int)(ar.bottom - ar.top)}) +
              "\npreview_button=" + pic(st.L.preview_button) + "\n";
    // "A different module on each monitor": where it shows, its state, and
    // whether its text fits beside its box (as custom_draw_button lays a
    // checkbox out: the 20-DIP box, 8 DIP, the text); its neighbours.
    {
      HWND pm = GetDlgItem(dlg, IDC_PER_MONITOR);
      const bool shown = (GetWindowLongW(pm, GWL_STYLE) & WS_VISIBLE) != 0 && !st.L.per_monitor.empty();
      HDC dc = GetDC(pm);
      const int text_w = measure_text(dc, without_mnemonic(window_text(pm)), st.theme.fonts.body).cx;
      ReleaseDC(pm, dc);
      const bool fits = st.L.per_monitor.w - st.theme.px(20) - st.theme.px(8) >= text_w;
      const bool dur = (GetWindowLongW(GetDlgItem(dlg, IDC_DURATION), GWL_STYLE) & WS_VISIBLE) != 0;
      report += "monitors=" + std::to_string(st.monitors) + "\nper_monitor=" + (shown ? pic(st.L.per_monitor) : "hidden") +
                "\nper_monitor_enabled=" + (IsWindowEnabled(pm) ? "1" : "0") +
                "\nper_monitor_checked=" + (IsDlgButtonChecked(dlg, IDC_PER_MONITOR) == BST_CHECKED ? "1" : "0") +
                "\nper_monitor_fits=" + (fits ? "1" : "0") + "\nduration=" + (dur ? pic(st.L.duration) : "hidden") +
                "\nlist_card=" + pic(st.L.list_card) + "\n";
    }
    // "Stretch to fit the screen": where it shows, whether it is checked and
    // its text fits beside its box; the options card and Resolution and
    // Sound's dropdowns around it.
    {
      HWND sb = GetDlgItem(dlg, IDC_STRETCH);
      const bool shown = (GetWindowLongW(sb, GWL_STYLE) & WS_VISIBLE) != 0 && !st.L.stretch.empty();
      HDC dc = GetDC(sb);
      const int text_w = measure_text(dc, without_mnemonic(window_text(sb)), st.theme.fonts.body).cx;
      ReleaseDC(sb, dc);
      const bool fits = st.L.stretch.w - st.theme.px(20) - st.theme.px(8) >= text_w;
      report += "stretch=" + (shown ? pic(st.L.stretch) : std::string("hidden")) +
                "\nstretch_checked=" + (IsDlgButtonChecked(dlg, IDC_STRETCH) == BST_CHECKED ? "1" : "0") +
                "\nstretch_fits=" + (fits ? "1" : "0") + "\noptions_card=" + pic(st.L.options_card) +
                "\nscale=" + pic(st.L.scale) + "\nsound=" + pic(st.L.sound) + "\n";
    }
    write_file_atomic(kv[L"report"], report);
  }
  DestroyWindow(dlg);
  DestroyWindow(owner);
  return ok ? 0 : 1;
}
#endif  // AD_SCR_TEST_HOOKS

} // namespace

int run_settings_dialog(const Args& args, void* hinstance) {
  INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
  InitCommonControlsEx(&icc);
  // COM for WIC (the covers' pictures) and the status line's live region.
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  gdiplus_startup();
  CoverStrip::register_class(static_cast<HINSTANCE>(hinstance));
  int result = 0;
  {
    State st;
    st.hinst = static_cast<HINSTANCE>(hinstance);
#if AD_SCR_TEST_HOOKS
    if (std::wstring shot = env_w(L"AD_SCR_TEST_SCREENSHOT"); !shot.empty()) {
      result = run_screenshot(st, shot);
    } else
#endif
    {
      HWND owner = nullptr;
      if (args.has_hwnd && IsWindow(reinterpret_cast<HWND>(args.hwnd))) owner = reinterpret_cast<HWND>(args.hwnd);
      st.thumbgen_allowed = env_w(L"AD_SCR_THUMBGEN") != L"0";
      INT_PTR r = DialogBoxParamW(st.hinst, MAKEINTRESOURCEW(IDD_SETTINGS), owner, dialog_proc, (LPARAM)&st);
      // A Preview still running deletes its own file once it has read it (it may
      // not have yet); otherwise nothing needs it any more.
      if (!st.preview_running) delete_preview_settings(st);
      result = r == IDOK ? 0 : (r == -1 ? 1 : 0);
    }
  }
  gdiplus_shutdown();
  if (SUCCEEDED(com)) CoUninitialize();
  return result;
}

} // namespace adw::scr
