// The frame every adimport window is built on (COVERS.md §4.1): a themed
// top-level window titled "Long After Dark" with the shared header band,
// a body laid out on the §3.3 grid, and a footer; live light/dark/high
// contrast, per-monitor DPI v2, the dialog keys, and TDM_CLICK_BUTTON.
//
// Each page is its own window. The flow opens the next page at the previous
// one's position before closing the previous one, so a test that waits for
// the old window to be gone always finds the new one.
#pragma once

#include <windows.h>

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "adw/ui/image.h"
#include "adw/ui/theme.h"
#include "adw/ui/widgets.h"
#include "gui.h"
#include "model.h"

namespace adw::import::gui {

// The app's name, which the screen saver's windows carry too (the tests find
// the windows by it).
inline constexpr wchar_t kTitle[] = L"Long After Dark";
// TaskDialog's TDM_CLICK_BUTTON: the tests press buttons with it (wParam = command id).
inline constexpr UINT kClickButton = WM_USER + 102;

// State shared by the windows of one adimport --gui run.
struct Session {
  Request req;
  std::filesystem::path assets;   // req.dest, else default_assets_root()
  bool autoclose = false;         // AD_GUI_AUTOCLOSE: no result page
  Tally tally;
  // The screenshot hook: windows are parked off-screen and cloaked, and the
  // theme and DPI may be forced.
  bool offscreen = false;
  ui::ThemeMode theme_mode = ui::ThemeMode::system;
  int forced_dpi = 0;
  // Off-screen only: the work area the window is fitted to, in DIPs (0: unlimited), so a
  // screenshot does not depend on the machine's monitor.
  int work_w = 0, work_h = 0;
  // The app mark as the windows' icon (taskbar, Alt+Tab).
  HICON icon_big = nullptr, icon_small = nullptr;
  int icon_dpi = 0;
  // What Sources says next time it opens, once ("Removed …").
  std::wstring notice;
};

enum class Ink { text, text2, text3, accent, caution, critical };
enum class Face { caption, body, body_strong, subtitle, glyph_large };
enum class Bg { base, card, caution, critical };

class ScrollPanel;

class Page {
 public:
  explicit Page(Session& s);
  virtual ~Page();
  Page(const Page&) = delete;
  Page& operator=(const Page&) = delete;

  // Creates the window at `prev`'s position (or centred on the monitor under
  // the pointer), builds and lays it out, and shows it (or parks it off-screen
  // for the screenshot hook).
  bool open(const Page* prev);
  // Runs the window until the page finishes; returns its outcome.
  int run();
  HWND hwnd() const { return hwnd_; }
  const ui::Theme& theme() const { return t_; }
  // The screenshot hook: draw the focus ring on this control as if it had the focus.
  void show_focus_on(int id);
  // Where the body's plain margin is, in client pixels (the tests' base pixel).
  POINT base_probe() const;

 protected:
  // ---- what a page provides -----------------------------------------------------------
  virtual std::wstring header_name() const { return L"Import a release"; }
  virtual std::wstring header_tagline() const { return L""; }
  virtual void build() = 0;
  // Places the controls for a client `w` px wide, with `max_h` px of height available;
  // returns the client height the page takes (at most `max_h` when it can shrink).
  virtual int layout(int w, int max_h) = 0;
  virtual void paint(HDC dc) { (void)dc; }
  virtual void command(int id, int code, HWND ctl) = 0;
  // Esc, the close box and TDM_CLICK_BUTTON(IDCANCEL) with no Cancel button.
  virtual void cancel() { finish(IDCANCEL); }
  virtual void timer(UINT_PTR id) { (void)id; }
  virtual int default_id() const { return IDOK; }
  virtual int initial_focus() const { return 0; }
  virtual bool minimizable() const { return false; }
  virtual void theme_changed() {}
  // A button the page draws itself (NM_CUSTOMDRAW): true with `*result` set,
  // else adw_ui draws it by its role.
  virtual bool draw_button(NMCUSTOMDRAW* cd, LRESULT* result) { (void)cd, (void)result; return false; }
  // WM_CONTEXTMENU from `ctl` (anywhere in the window, panels included) at
  // `pt` (screen; {-1, -1} from the keyboard): true when the page showed a menu.
  virtual bool context_menu(HWND ctl, POINT pt) { (void)ctl, (void)pt; return false; }
  // A command that comes from one of the page's menus, not a control: it is
  // carried out with no control (and TDM_CLICK_BUTTON presses it) when this
  // says it is one the page offers now.
  virtual bool menu_command(int id) const { (void)id; return false; }

  // ---- helpers ------------------------------------------------------------------------
  void finish(int outcome);
  bool finished() const { return done_; }
  int px(int dips) const { return t_.px(dips); }
  // A static text control (SS_NOPREFIX; wraps unless `single`).
  HWND add_text(const std::wstring& text, Face face, Ink ink, HWND parent = nullptr, bool single = false,
                Bg bg = Bg::base, DWORD extra_style = 0);
  void set_text(HWND h, const std::wstring& text);
  void set_ink(HWND h, Ink ink);
  void set_bg(HWND h, Bg bg);
  // A push button in one of adw_ui's roles; its window is its body grown by the focus margin.
  HWND add_button(int id, const std::wstring& text, ui::ButtonRole role, wchar_t glyph = 0, HWND parent = nullptr,
                  ui::Surface surface = ui::Surface::base);
  // The width a standard/accent/subtle button needs for its text (body only, px).
  int button_width(HWND h, int min_dips = 120) const;
  // Places a focusable control so that its visual body is (x, y, w, h).
  void place_body(HWND h, int x, int y, int w, int hgt) const;
  void place(HWND h, int x, int y, int w, int hgt) const;
  // The height `h`'s text takes at `w` px wide.
  int text_height(HWND h, int w) const;
  HFONT font(Face f) const;
  COLORREF ink(Ink i) const;
  COLORREF bg_color(Bg b) const;
  HWND item(int id) const;   // a control by id, anywhere in the window (panels included)
  void relayout();           // lays the page out again and fits the window to it
  void add_panel(ScrollPanel* p) { panels_.push_back(p); }
  // Layout constants (DIPs, COVERS.md §3.3).
  static constexpr int kMargin = 24, kGap = 16, kHeader = 48, kFooter = 64, kControlH = 32;
  int header_h() const { return px(kHeader); }
  // The footer band: a hairline above it, `h` px tall, at the bottom of a client `ch` px tall.
  void paint_footer(HDC dc, int top) const;
  // A caution / critical message box behind a static (an InfoBar).
  // `on` is the window whose DC this is (the page when null).
  void paint_info_box(HDC dc, HWND text, Bg bg, HWND on = nullptr) const;

  // Destroys the window (subclasses call it first thing in their destructors, while
  // everything its controls point at still exists).
  void destroy_window();

  Session& s_;
  ui::Theme t_;
  HWND hwnd_ = nullptr;
  int client_w_ = 0, client_h_ = 0;
  int footer_top_ = 0;   // where paint_footer draws its hairline (0 = no footer)

 private:
  friend class ScrollPanel;
  static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
  LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
  void route_command(int id, int code, HWND ctl);
  void apply_theme();
  void apply_dpi(int dpi);
  void apply_fonts();
  void fit(bool keep_pos);
  RECT work_area() const;
  void keep_focus_visible();

  struct TextLook {
    Face face;
    Ink ink;
    Bg bg;
  };
  std::map<HWND, TextLook> texts_;
  std::vector<HWND> buttons_;
  std::vector<ScrollPanel*> panels_;
  HFONT glyph_font_ = nullptr;
  int glyph_font_dpi_ = 0;
  HBRUSH caution_brush_ = nullptr, critical_brush_ = nullptr;
  bool done_ = false;
  int outcome_ = IDCANCEL;
  HWND last_focus_ = nullptr;
};

// A scrolling child container for lists longer than the window (COVERS.md §4.2):
// controls are placed in content coordinates and move as it scrolls; the thin
// Windows 11 scroll bar (attach_overlay_scrollbar) sits over its right edge.
class ScrollPanel {
 public:
  // A child of `parent` (the page when null: a panel may sit inside another).
  ScrollPanel(Page& page, const ui::Theme* t, ui::Surface surface, HWND parent = nullptr);
  ~ScrollPanel();
  HWND hwnd() const { return hwnd_; }
  // Adds `h` at (x, y, w, hgt) in content coordinates.
  void put(HWND h, int x, int y, int w, int hgt);
  // Places the panel so that `w` x `hgt` px of its `content_h` px of content show.
  void place(int x, int y, int w, int hgt, int content_h);
  // The same where it already is (a panel inside another, which put() it there
  // `window_width(w)` wide).
  void set_view(int w, int hgt, int content_h);
  // The window's width for `w` px showing: the native scroll bar's width more,
  // clipped off (attach_overlay_scrollbar draws the thin one over the content).
  int window_width(int w) const;
  int scroll() const { return scroll_; }
  void scroll_to(int y);
  void ensure_visible(HWND child);
  bool contains(HWND h) const;
  // Painted under the controls, in content coordinates shifted by -scroll().
  std::function<void(HDC, int dy)> painter;

 private:
  static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
  LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
  void reposition();
  void update_bar();

  const ui::Theme* t_;
  HWND hwnd_ = nullptr;
  struct Item {
    HWND h;
    RECT r;
  };
  std::vector<Item> items_;
  int scroll_ = 0, content_h_ = 0, view_h_ = 0, view_w_ = 0;
};

// The icon for the windows: the app mark (ui::draw_app_mark) at `size` px.
HICON make_mark_icon(int size);

}  // namespace adw::import::gui
