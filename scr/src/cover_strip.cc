#include "cover_strip.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

#include "../res/resource.h"
#include "adw/ui/widgets.h"
#include "log.h"
#include "paths.h"
#include "releases.h"

namespace adw::scr {

namespace {

constexpr UINT_PTR kTileSubclass = 21, kChevronSubclass = 22;
enum MenuCommand : UINT { kMenuShowOnly = 1, kMenuShowAll = 2, kMenuChangeCover = 3, kMenuRemove = 4 };

RECT rc_of(const Rc& r) { return RECT{r.x, r.y, r.right(), r.bottom()}; }
RECT offset(RECT r, int dx, int dy) {
  OffsetRect(&r, dx, dy);
  return r;
}

// Paint into a memory bitmap, then copy once.
struct Buffer {
  HDC target, dc = nullptr;
  HBITMAP bmp = nullptr;
  HGDIOBJ old = nullptr;
  RECT rc;
  Buffer(HDC t, const RECT& r) : target(t), rc(r) {
    dc = CreateCompatibleDC(t);
    bmp = CreateCompatibleBitmap(t, std::max(1L, r.right - r.left), std::max(1L, r.bottom - r.top));
    old = SelectObject(dc, bmp);
    SetViewportOrgEx(dc, -r.left, -r.top, nullptr);
  }
  ~Buffer() {
    SetViewportOrgEx(dc, 0, 0, nullptr);
    BitBlt(target, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
  }
};

CoverStrip* strip_of(HWND container) { return reinterpret_cast<CoverStrip*>(GetWindowLongPtrW(container, GWLP_USERDATA)); }

} // namespace

// ---- the container ---------------------------------------------------------------------

void CoverStrip::register_class(HINSTANCE hinst) {
  static bool done = false;
  if (done) return;
  WNDCLASSEXW wc{sizeof(wc)};
  wc.lpfnWndProc = container_proc;
  wc.hInstance = hinst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = kCoverStripClass;
  done = RegisterClassExW(&wc) != 0;
}

LRESULT CALLBACK CoverStrip::container_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  CoverStrip* self = strip_of(h);
  switch (msg) {
    case WM_ERASEBKGND:
      if (!self) break;
      {
        RECT r{};
        GetClientRect(h, &r);
        fill_rect((HDC)wp, r, self->t_->pal.base);
      }
      return 1;
    case WM_PRINTCLIENT:
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      HDC dc = msg == WM_PAINT ? BeginPaint(h, &ps) : (HDC)wp;
      if (self) {
        RECT r{};
        GetClientRect(h, &r);
        fill_rect(dc, r, self->t_->pal.base);
      }
      if (msg == WM_PAINT) EndPaint(h, &ps);
      return 0;
    }
    case WM_NOTIFY:
      if (self) return self->on_notify(reinterpret_cast<NMHDR*>(lp));
      break;
    case WM_COMMAND:
      if (self) self->on_command(LOWORD(wp), HIWORD(wp), (HWND)lp);
      return 0;
    case WM_CONTEXTMENU:
      if (self) self->on_context_menu((HWND)wp, lp);
      return 0;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
      if (self) self->on_wheel(GET_WHEEL_DELTA_WPARAM(wp), msg == WM_MOUSEHWHEEL);
      return 0;
    case WM_NCDESTROY:
      SetWindowLongPtrW(h, GWLP_USERDATA, 0);
      break;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

CoverStrip::CoverStrip(HWND container, const Theme* theme, Callbacks cb, bool tooltips)
    : container_(container), t_(theme), cb_(std::move(cb)), tooltips_(tooltips) {
  SetWindowLongPtrW(container_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  HINSTANCE hinst = (HINSTANCE)GetWindowLongPtrW(container_, GWLP_HINSTANCE);
  // The chevrons first: above the tiles in the z-order (the tiles clip
  // around them), and out of the tab order.
  auto chevron = [&](int id, const wchar_t* name) {
    HWND b = CreateWindowExW(0, WC_BUTTONW, name, WS_CHILD | WS_CLIPSIBLINGS | WS_GROUP | BS_PUSHBUTTON, 0, 0, 1, 1,
                             container_, (HMENU)(INT_PTR)id, hinst, nullptr);
    SetWindowSubclass(b, chevron_proc, kChevronSubclass, reinterpret_cast<DWORD_PTR>(this));
    return b;
  };
  chev_left_ = chevron(IDC_STRIP_PREV, L"Scroll left");
  chev_right_ = chevron(IDC_STRIP_NEXT, L"Scroll right");
  if (tooltips_) {
    tip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT,
                           CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, container_, nullptr, hinst, nullptr);
    SendMessageW(tip_, TTM_SETMAXTIPWIDTH, 0, t_->px(360));
  }
}

CoverStrip::~CoverStrip() {
  if (IsWindow(container_)) {
    destroy_tiles();
    SetWindowLongPtrW(container_, GWLP_USERDATA, 0);
  }
  if (tip_ && IsWindow(tip_)) DestroyWindow(tip_);
  drop_caption_fonts();
}

void CoverStrip::drop_caption_fonts() {
  for (auto& [size, f] : caption_fonts_) DeleteObject(f);
  caption_fonts_.clear();
  caption_fonts_dpi_ = 0;
  caption_base_ = nullptr;
}

HFONT CoverStrip::caption_font(int size) {
  HFONT base = t_->fonts.caption;
  if (size >= kStripCaptionSizes[0] || !base) return base;
  if (caption_fonts_dpi_ != t_->dpi || caption_base_ != base) {
    drop_caption_fonts();
    caption_fonts_dpi_ = t_->dpi;
    caption_base_ = base;
  }
  auto it = caption_fonts_.find(size);
  if (it != caption_fonts_.end()) return it->second;
  LOGFONTW lf{};
  GetObjectW(base, sizeof(lf), &lf);
  lf.lfHeight = -MulDiv(size, t_->dpi, 96);
  lf.lfWidth = 0;
  HFONT f = CreateFontIndirectW(&lf);
  if (!f) return base;
  caption_fonts_.emplace(size, f);
  return f;
}

// ---- tiles -------------------------------------------------------------------------------

void CoverStrip::destroy_tiles() {
  for (HWND b : buttons_) {
    if (tip_) {
      TOOLINFOW ti{sizeof(ti)};
      ti.hwnd = container_;
      ti.uId = (UINT_PTR)b;
      SendMessageW(tip_, TTM_DELTOOL, 0, (LPARAM)&ti);
    }
    if (IsWindow(b)) DestroyWindow(b);
  }
  buttons_.clear();
}

void CoverStrip::set_tiles(const std::vector<StripTile>& tiles, const std::vector<std::string>& selected) {
  HWND focus = GetFocus();
  const bool had_focus = std::find(buttons_.begin(), buttons_.end(), focus) != buttons_.end();
  destroy_tiles();
  tiles_ = tiles;
  images_.assign(tiles_.size(), nullptr);
  HINSTANCE hinst = (HINSTANCE)GetWindowLongPtrW(container_, GWLP_HINSTANCE);
  for (size_t i = 0; i < tiles_.size(); ++i) {
    const StripTile& t = tiles_[i];
    DWORD style = WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | BS_AUTOCHECKBOX | BS_PUSHLIKE | BS_NOTIFY;
    if (i == 0) style |= WS_GROUP;
    HWND b = CreateWindowExW(0, WC_BUTTONW, t.name.c_str(), style, 0, 0, 1, 1, container_,
                             (HMENU)(INT_PTR)(IDC_COVER_TILE_BASE + (int)i), hinst, nullptr);
    SendMessageW(b, WM_SETFONT, (WPARAM)t_->fonts.caption, FALSE);
    const bool on = std::find(selected.begin(), selected.end(), t.id) != selected.end();
    SendMessageW(b, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
    SetWindowSubclass(b, tile_proc, kTileSubclass, reinterpret_cast<DWORD_PTR>(this));
    buttons_.push_back(b);
    if (tip_) {
      TOOLINFOW ti{sizeof(ti)};
      ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
      ti.hwnd = container_;
      ti.uId = (UINT_PTR)b;
      ti.lpszText = const_cast<wchar_t*>(t.tip.c_str());
      SendMessageW(tip_, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    }
    // The picture, read once per tile.png (the catalog's md5 changes with it).
    if (!t.tile_path.empty()) {
      const std::wstring key = t.tile_path + L"|" + widen(t.tile_md5);
      auto it = cache_.find(key);
      if (it == cache_.end()) {
        auto img = std::make_shared<Image>();
        std::string err;
        if (!load_image(t.tile_path, *img, &err)) {
          log_line("dialog: cover %s: %s (%s); the generated cover stands in", t.id.c_str(), err.c_str(),
                   narrow(t.tile_path).c_str());
          img.reset();
        }
        it = cache_.emplace(key, img).first;
      }
      images_[i] = it->second;
    }
  }
  const int home = home_tile();
  set_tab_stop(home);
  first_ = 0;
  // A filter on a cover past the first stop (a saved Collections, a reload
  // keeping the filter): the first selected cover shows, not only the status.
  reveal_ = home > 0 ? home : -1;
  place_tiles();
  if (had_focus && home >= 0) SetFocus(buttons_[home]);
}

HWND CoverStrip::tile_hwnd(int i) const { return i >= 0 && i < (int)buttons_.size() ? buttons_[i] : nullptr; }

std::vector<std::string> CoverStrip::selected() const {
  std::vector<std::string> ids;
  for (size_t i = 0; i < buttons_.size() && i < tiles_.size(); ++i) {
    if (SendMessageW(buttons_[i], BM_GETCHECK, 0, 0) == BST_CHECKED) ids.push_back(tiles_[i].id);
  }
  return ids;
}

size_t CoverStrip::selected_count() const { return selected().size(); }

bool CoverStrip::filter_active() const { return adw::scr::filter_active(selected_count(), tiles_.size()); }

void CoverStrip::select(const std::vector<std::string>& ids) {
  for (size_t i = 0; i < buttons_.size() && i < tiles_.size(); ++i) {
    const bool on = std::find(ids.begin(), ids.end(), tiles_[i].id) != ids.end();
    SendMessageW(buttons_[i], BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
  }
  refresh();
}

int CoverStrip::home_tile() const {
  if (buttons_.empty()) return -1;
  for (size_t i = 0; i < buttons_.size(); ++i) {
    if (SendMessageW(buttons_[i], BM_GETCHECK, 0, 0) == BST_CHECKED) return (int)i;
  }
  return 0;
}

int CoverStrip::index_of(HWND h) const {
  for (size_t i = 0; i < buttons_.size(); ++i) {
    if (buttons_[i] == h) return (int)i;
  }
  return -1;
}

// One tile at a time is a tab stop: the one last focused.
void CoverStrip::set_tab_stop(int i) {
  for (size_t k = 0; k < buttons_.size(); ++k) {
    LONG_PTR style = GetWindowLongPtrW(buttons_[k], GWL_STYLE);
    LONG_PTR want = (int)k == i ? (style | WS_TABSTOP) : (style & ~(LONG_PTR)WS_TABSTOP);
    if (want != style) SetWindowLongPtrW(buttons_[k], GWL_STYLE, want);
  }
}

// ---- layout and scrolling ----------------------------------------------------------------

void CoverStrip::layout(const StripInput& in, POINT origin) {
  in_ = in;
  origin_ = origin;
  place_tiles();
}

void CoverStrip::place_tiles() {
  in_.first = first_;
  in_.tiles = (int)tiles_.size();
  if (reveal_ >= 0 && in_.w > 0) {
    in_.first = first_ = strip_first_showing(in_, reveal_);
    reveal_ = -1;
  }
  S_ = layout_strip(in_);
  first_ = S_.first;
  const int fm = focus_margin(t_->dpi);
  RECT box{};
  GetClientRect(container_, &box);
  HDWP dwp = BeginDeferWindowPos((int)buttons_.size() + 2);
  for (size_t i = 0; i < buttons_.size() && i < S_.cells.size(); ++i) {
    const Rc& c = S_.cells[i];
    const int w = c.w + 2 * fm, h = c.h + 2 * fm;
    int x = c.x - fm - origin_.x;
    // A tile the view can't hold whole isn't shown at all: it waits just
    // outside the container, on its side of the row (still a window for the
    // dialog manager and screen readers; taking the focus scrolls it in).
    if (!S_.whole[i]) x = (int)i < first_ ? box.left - w - 1 : box.right + 1;
    if (dwp) dwp = DeferWindowPos(dwp, buttons_[i], nullptr, x, c.y - fm - origin_.y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
  }
  auto chevron = [&](HWND b, const Rc& r) {
    if (!dwp) return;
    if (r.empty()) {
      dwp = DeferWindowPos(dwp, b, nullptr, 0, 0, 0, 0, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_HIDEWINDOW);
    } else {
      dwp = DeferWindowPos(dwp, b, nullptr, r.x - origin_.x, r.y - origin_.y, r.w, r.h,
                           SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
  };
  chevron(chev_left_, S_.chevron_left);
  chevron(chev_right_, S_.chevron_right);
  if (dwp) EndDeferWindowPos(dwp);
  refresh();
}

void CoverStrip::scroll_by(int tiles) {
  const int want = std::clamp(first_ + tiles, 0, S_.max_first);
  if (want == first_) return;
  first_ = want;
  place_tiles();
}

void CoverStrip::ensure_visible(int tile) {
  in_.first = first_;
  in_.tiles = (int)tiles_.size();
  const int want = strip_first_showing(in_, tile);
  if (want == first_) return;
  first_ = want;
  place_tiles();
}

void CoverStrip::refresh() {
  if (tip_) theme_native_control(tip_, t_->pal.dark && !t_->pal.high_contrast);
  for (HWND b : buttons_) {
    SendMessageW(b, WM_SETFONT, (WPARAM)t_->fonts.caption, FALSE);   // a new DPI made new fonts
    InvalidateRect(b, nullptr, FALSE);
  }
  for (HWND b : {chev_left_, chev_right_}) InvalidateRect(b, nullptr, FALSE);
  InvalidateRect(container_, nullptr, TRUE);
}

void CoverStrip::on_wheel(int delta, bool horizontal) {
  if (!S_.overflow) return;
  wheel_ += delta;
  const int steps = wheel_ / WHEEL_DELTA;
  wheel_ -= steps * WHEEL_DELTA;
  if (steps) scroll_by(horizontal ? steps : -steps);
}

// ---- input -------------------------------------------------------------------------------

void CoverStrip::on_command(int id, int code, HWND from) {
  if (from == chev_left_ || from == chev_right_) {
    if (code == BN_CLICKED) scroll_by(from == chev_left_ ? -1 : 1);
    return;
  }
  const int i = index_of(from);
  if (i < 0) return;
  (void)id;
  if (code == BN_CLICKED || code == BN_DOUBLECLICKED) {
    ensure_visible(i);   // Space on a focused tile the chevrons scrolled away: back in view
    refresh();           // every cover's dimming follows the filter
    if (cb_.changed) cb_.changed();
  }
}

void CoverStrip::on_context_menu(HWND from, LPARAM lp) {
  const int i = index_of(from);
  if (i < 0) return;
  POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
  if (lp == -1) {
    // From the keyboard (Shift+F10, the Apps key): at the tile's bottom-left.
    const Rc& c = S_.cells[i];
    pt = POINT{c.x - origin_.x, c.bottom() - origin_.y};
    ClientToScreen(container_, &pt);
  }
  HMENU menu = CreatePopupMenu();
  const std::wstring only = L"Show only " + tiles_[i].short_title;
  AppendMenuW(menu, MF_STRING, kMenuShowOnly, only.c_str());
  AppendMenuW(menu, MF_STRING | (filter_active() ? 0 : MF_GRAYED), kMenuShowAll, L"Show all releases");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  const bool can = cb_.can_change_cover && cb_.can_change_cover();
  AppendMenuW(menu, MF_STRING | (can ? 0 : MF_GRAYED), kMenuChangeCover, L"Change cover…");
  // Last, as the one that takes something away: adimport asks first.
  const std::wstring remove = L"Remove " + tiles_[i].short_title + L"…";
  const bool can_remove = cb_.can_remove && cb_.can_remove();
  AppendMenuW(menu, MF_STRING | (can_remove ? 0 : MF_GRAYED), kMenuRemove, remove.c_str());
  const UINT cmd = (UINT)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0,
                                        container_, nullptr);
  DestroyMenu(menu);
  switch (cmd) {
    case kMenuShowOnly:
      select({tiles_[i].id});
      ensure_visible(i);
      if (cb_.changed) cb_.changed();
      break;
    case kMenuShowAll:
      select({});
      if (cb_.changed) cb_.changed();
      break;
    case kMenuChangeCover:
      if (cb_.change_cover) cb_.change_cover(i);
      break;
    case kMenuRemove:
      if (cb_.remove) cb_.remove(i);
      break;
  }
}

LRESULT CALLBACK CoverStrip::tile_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
  auto* self = reinterpret_cast<CoverStrip*>(ref);
  switch (msg) {
    case WM_GETDLGCODE:
      // Arrows move between the tiles here (wrapping), not in the dialog manager.
      return DefSubclassProc(h, msg, wp, lp) | DLGC_WANTARROWS;
    case WM_KEYDOWN: {
      const int n = (int)self->buttons_.size(), i = self->index_of(h);
      if (n == 0 || i < 0) break;
      // On several rows, Up and Down go to the tile above or below (the last
      // one when the row below is shorter), and stop at the first and last
      // rows; on one row they step back and on, as Left and Right do.
      const int cols = self->S_.rows > 1 ? std::max(1, self->S_.cols) : 0;
      int to = -1;
      switch (wp) {
        case VK_LEFT: to = (i + n - 1) % n; break;
        case VK_RIGHT: to = (i + 1) % n; break;
        case VK_UP: to = cols ? (i >= cols ? i - cols : i) : (i + n - 1) % n; break;
        case VK_DOWN: to = cols ? (i / cols < (n - 1) / cols ? std::min(n - 1, i + cols) : i) : (i + 1) % n; break;
        case VK_HOME: to = 0; break;
        case VK_END: to = n - 1; break;
      }
      if (to < 0) break;
      SetFocus(self->buttons_[to]);
      return 0;
    }
    case WM_SETFOCUS: {
      const int i = self->index_of(h);
      self->set_tab_stop(i);
      self->ensure_visible(i);
      InvalidateRect(h, nullptr, FALSE);
      break;
    }
    case WM_KILLFOCUS:
      InvalidateRect(h, nullptr, FALSE);
      break;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
      return SendMessageW(GetParent(h), msg, wp, lp);
    case WM_NCDESTROY:
      RemoveWindowSubclass(h, tile_proc, kTileSubclass);
      break;
  }
  return DefSubclassProc(h, msg, wp, lp);
}

// A chevron scrolls on a click but never takes the keyboard focus from
// where it is (it isn't a tab stop either).
LRESULT CALLBACK CoverStrip::chevron_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
  auto* self = reinterpret_cast<CoverStrip*>(ref);
  auto inside = [&] {
    RECT r{};
    GetClientRect(h, &r);
    POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    return PtInRect(&r, pt) != FALSE;
  };
  switch (msg) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
      SetCapture(h);
      self->pushing_ = true;
      SendMessageW(h, BM_SETSTATE, TRUE, 0);
      return 0;
    case WM_MOUSEMOVE:
      if (self->pushing_ && GetCapture() == h) {
        SendMessageW(h, BM_SETSTATE, inside() ? TRUE : FALSE, 0);
        return 0;
      }
      break;
    case WM_LBUTTONUP:
      if (self->pushing_) {
        const bool hit = inside();
        self->pushing_ = false;
        ReleaseCapture();
        SendMessageW(h, BM_SETSTATE, FALSE, 0);
        if (hit) SendMessageW(GetParent(h), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(h), BN_CLICKED), (LPARAM)h);
        return 0;
      }
      break;
    case WM_CAPTURECHANGED:
      if (self->pushing_) {
        self->pushing_ = false;
        SendMessageW(h, BM_SETSTATE, FALSE, 0);
      }
      break;
    case WM_NCDESTROY:
      RemoveWindowSubclass(h, chevron_proc, kChevronSubclass);
      break;
  }
  return DefSubclassProc(h, msg, wp, lp);
}

// ---- drawing -------------------------------------------------------------------------------

LRESULT CoverStrip::on_notify(NMHDR* hdr) {
  if (hdr->code != NM_CUSTOMDRAW) return 0;
  auto* cd = reinterpret_cast<NMCUSTOMDRAW*>(hdr);
  if (cd->dwDrawStage != CDDS_PREPAINT) return CDRF_DODEFAULT;
  if (hdr->hwndFrom == chev_left_ || hdr->hwndFrom == chev_right_) {
    draw_chevron(cd, hdr->hwndFrom == chev_left_);
    return CDRF_SKIPDEFAULT;
  }
  const int i = index_of(hdr->hwndFrom);
  if (i < 0 || i >= (int)S_.cells.size()) return CDRF_DODEFAULT;
  draw_tile(cd, i);
  return CDRF_SKIPDEFAULT;
}

void CoverStrip::draw_tile(NMCUSTOMDRAW* cd, int i) {
  const Theme& t = *t_;
  const Palette& p = t.pal;
  HWND h = cd->hdr.hwndFrom;
  RECT cr{};
  GetClientRect(h, &cr);
  Buffer buf(cd->hdc, cr);
  HDC dc = buf.dc;
  fill_rect(dc, cr, p.base);
  const int fm = focus_margin(t.dpi);
  const bool compact = S_.mode == StripMode::compact;
  // Dialog-client geometry into this window's client: the cell sits at (fm, fm).
  const Rc& cell_d = S_.cells[i];
  const int dx = fm - cell_d.x, dy = fm - cell_d.y;
  const RECT cell = offset(rc_of(cell_d), dx, dy);
  const RECT art = offset(rc_of(S_.arts[i]), dx, dy);
  const bool hot = (cd->uItemState & CDIS_HOT) != 0 || hover_override_ == i;
  const bool pressed = (cd->uItemState & CDIS_SELECTED) != 0;
  const bool checked = SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
  const bool dimmed = filter_active() && !checked;
  const float s = t.dpi / 96.0f;

  // Hover backdrop (under high contrast, a ring in the hot-light colour).
  if (p.high_contrast) {
    if (hot || pressed) stroke_round(dc, cell, t.pxf(6), p.accent_text, (float)t.hairline());
  } else if (pressed || hot) {
    fill_round(dc, cell, t.pxf(6), pressed ? p.control_pressed : p.row_hover);
  }
  // The cover, then the veil of an unselected one while a filter is on.
  const Image* img = images_[i] ? images_[i].get() : nullptr;
  draw_cover(dc, art, img, tiles_[i].title, t);
  if (dimmed && !p.high_contrast) {
    fill_round(dc, art, t.pxf(4), p.base, hot ? 70 : 140);
    // The veil takes a dark cover almost into a dark base: an outline keeps
    // its shape (and that there is a cover to click) in view.
    stroke_round(dc, art, t.pxf(4), blend(p.base, p.text, 0.28), (float)t.hairline());
  }
  if (checked) {
    // The selection ring, 2 to 4 DIP outside the art.
    RECT ring = art;
    InflateRect(&ring, t.px(4), t.px(4));
    stroke_round(dc, ring, t.pxf(8), p.accent, std::max(1.0f, std::round(2 * s)));
    // The check badge inside the art's top-right corner.
    const float d = t.pxf(compact ? 16.0f : 18.0f), inset = t.pxf(2);
    const float cx = art.right - inset - d / 2, cy = art.top + inset + d / 2;
    fill_ellipse(dc, cx, cy, d / 2, p.accent);
    stroke_ellipse(dc, cx, cy, d / 2, p.high_contrast ? p.base : RGB(0xFF, 0xFF, 0xFF), std::max(1.0f, s),
                   p.high_contrast ? 255 : 178);
    RECT g{(int)std::floor(cx - d / 2), (int)std::floor(cy - d / 2), (int)std::ceil(cx + d / 2), (int)std::ceil(cy + d / 2)};
    draw_text(dc, L"", g, t.fonts.icons_small, p.on_accent, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
  }
  // The caption (regular tiles only; compact ones leave it to the tooltip).
  if (!compact && !S_.captions[i].empty()) {
    // The tile's whole window is the caption's room (the cell and its focus
    // margins); a title too long for it at 12 DIP gets 11 or 10 before an ellipsis.
    RECT c = offset(rc_of(S_.captions[i]), dx, dy);
    c.left = cr.left;
    c.right = cr.right;
    const std::wstring& text = tiles_[i].short_title;
    const int size = strip_caption_size(c.right - c.left, [&](int sz) { return (int)measure_text(dc, text, caption_font(sz)).cx; });
    draw_text(dc, text, c, caption_font(size), dimmed && !p.high_contrast ? p.text2 : p.text,
              DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
  }
  const bool focus = ((cd->uItemState & CDIS_FOCUS) || focused_window() == h) && keyboard_cues(h);
  if (focus) draw_focus_ring(dc, cr, t.pxf(6) + fm, p, s);
}

void CoverStrip::draw_chevron(NMCUSTOMDRAW* cd, bool left) {
  const Theme& t = *t_;
  const Palette& p = t.pal;
  RECT cr{};
  GetClientRect(cd->hdr.hwndFrom, &cr);
  Buffer buf(cd->hdc, cr);
  HDC dc = buf.dc;
  fill_rect(dc, cr, p.base);
  const bool hot = (cd->uItemState & CDIS_HOT) != 0, pressed = (cd->uItemState & CDIS_SELECTED) != 0;
  if (p.high_contrast) {
    if (hot || pressed) stroke_round(dc, cr, t.pxf(4), p.accent_text, (float)t.hairline());
  } else if (hot || pressed) {
    fill_round(dc, cr, t.pxf(4), pressed ? p.row_selected : p.row_hover);
  }
  draw_text(dc, left ? L"" : L"", cr, t.fonts.icons_small, p.high_contrast ? p.text : p.text2,
            DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
}

} // namespace adw::scr
