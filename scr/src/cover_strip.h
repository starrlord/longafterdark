// The settings dialog's box-cover strip (COVERS.md §1.2–§1.5): one tile per
// installed release across the top of the window, on as many rows as they
// need; clicking tiles filters the module list, and a tile's context menu
// changes its cover or removes the release.
//
// It is built from real controls, so the dialog manager, MSAA and UIA work
// with no accessibility code of its own: a container (IDC_COVER_STRIP, class
// kCoverStripClass, WS_EX_CONTROLPARENT, created from the dialog template)
// holds one BUTTON per tile (BS_AUTOCHECKBOX | BS_PUSHLIKE | BS_NOTIFY, id
// IDC_COVER_TILE_BASE + index; its check state is the selection, its window
// text the accessible name), custom-drawn through NM_CUSTOMDRAW, with a
// roving tab stop, and two chevron buttons that scroll the row by whole tiles
// when a window too short for the rows makes it one row that overflows (only
// whole tiles show: ui_model.h, layout_strip). The
// status line and the "Show all" link beside the strip are the dialog's own
// controls.
#pragma once

#include <windows.h>
#include <commctrl.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "adw/ui/image.h"
#include "adw/ui/theme.h"
#include "ui_model.h"

namespace adw::scr {

using namespace adw::ui;

inline constexpr wchar_t kCoverStripClass[] = L"AdwCoverStrip";

struct StripTile {
  std::string id;                   // release id
  std::wstring title, short_title;  // the cover's title (generated covers), the caption
  std::wstring name;                // accessible name: "<title>, N screen savers" (& doubled)
  std::wstring tip;                 // tooltip
  std::wstring tile_path;           // absolute path of the release's tile.png ("" = generated)
  std::string tile_md5;             // the catalog's tileMd5 (a new one reloads the picture)
};

class CoverStrip {
 public:
  struct Callbacks {
    std::function<void()> changed;              // the selection changed (by the user)
    std::function<void(int)> change_cover;      // "Change cover…" chosen for tile i
    std::function<bool()> can_change_cover;     // false: the menu item is greyed
    std::function<void(int)> remove;            // "Remove <release>…" chosen for tile i
    std::function<bool()> can_remove;           // false: the menu item is greyed
  };

  // The container's window class; once per process, before the dialog is created.
  static void register_class(HINSTANCE hinst);

  // Takes over the container the dialog template created. `tooltips` is off
  // for the off-screen screenshot hook.
  CoverStrip(HWND container, const Theme* theme, Callbacks cb, bool tooltips);
  ~CoverStrip();
  CoverStrip(const CoverStrip&) = delete;
  CoverStrip& operator=(const CoverStrip&) = delete;

  HWND hwnd() const { return container_; }
  // (Re)creates the tiles; `selected` lists the release ids to show selected.
  // Pictures are read here, and read again only when a tile's md5 changes.
  // The row starts unscrolled, or scrolled to show the first selected tile.
  void set_tiles(const std::vector<StripTile>& tiles, const std::vector<std::string>& selected);
  size_t count() const { return tiles_.size(); }
  const StripTile& tile(int i) const { return tiles_[i]; }
  HWND tile_hwnd(int i) const;
  // The selected release ids, in tile order.
  std::vector<std::string> selected() const;
  size_t selected_count() const;
  // Selects exactly `ids` (no `changed` callback).
  void select(const std::vector<std::string>& ids);
  // Places the tiles for `in` (the dialog's layout: DIPs), keeping the scroll
  // position (when the row scrolls). `origin` is the container's top-left in
  // dialog client pixels.
  void layout(const StripInput& in, POINT origin);
  const StripLayout& geometry() const { return S_; }
  // Scrolls by whole tiles (the chevrons and the wheel), or to show `tile`.
  void scroll_by(int tiles);
  void ensure_visible(int tile);
  // The tile Tab lands on when the dialog opens: the first selected, else the first.
  int home_tile() const;
  // Repaints every tile (a filter, theme or DPI change).
  void refresh();
  // Screenshot hook: draw tile `i` as if hovered (-1: none).
  void set_hover_override(int i) { hover_override_ = i; refresh(); }

 private:
  static LRESULT CALLBACK container_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
  static LRESULT CALLBACK tile_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref);
  static LRESULT CALLBACK chevron_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref);
  LRESULT on_notify(NMHDR* hdr);
  void on_command(int id, int code, HWND from);
  void on_context_menu(HWND from, LPARAM lp);
  void on_wheel(int delta, bool horizontal);
  void draw_tile(NMCUSTOMDRAW* cd, int i);
  void draw_chevron(NMCUSTOMDRAW* cd, bool left);
  // The caption face at `size` DIP (12: fonts.caption; 11 and 10 made from it, per DPI).
  HFONT caption_font(int size);
  void drop_caption_fonts();
  void place_tiles();
  void set_tab_stop(int i);
  int index_of(HWND h) const;
  bool filter_active() const;
  void destroy_tiles();

  HWND container_ = nullptr, tip_ = nullptr, chev_left_ = nullptr, chev_right_ = nullptr;
  const Theme* t_;
  Callbacks cb_;
  bool tooltips_;
  std::vector<StripTile> tiles_;
  std::vector<HWND> buttons_;
  std::vector<std::shared_ptr<Image>> images_;   // per tile; null = generated
  std::map<std::wstring, std::shared_ptr<Image>> cache_;   // path|md5 -> picture
  StripInput in_{};
  StripLayout S_{};
  POINT origin_{};
  int first_ = 0;
  int reveal_ = -1;        // a tile to scroll into view once the strip has its width (-1: none)
  int hover_override_ = -1;
  int wheel_ = 0;          // wheel delta not yet turned into a tile's scroll
  bool pushing_ = false;   // a chevron is held down
  std::map<int, HFONT> caption_fonts_;   // smaller caption faces, for caption_fonts_dpi_
  int caption_fonts_dpi_ = 0;
  HFONT caption_base_ = nullptr;         // the fonts.caption they were made from
};

} // namespace adw::scr
