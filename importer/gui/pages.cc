#include "pages.h"

#include <commctrl.h>
#include <shobjidl.h>

#include <algorithm>
#include <cstdlib>

#include "winutil.h"

namespace adw::import::gui {

namespace fs = std::filesystem;

namespace {

std::wstring env(const wchar_t* name) {
  const wchar_t* v = _wgetenv(name);
  return v ? v : L"";
}

std::wstring window_text(HWND h) {
  int n = GetWindowTextLengthW(h);
  std::wstring s(n + 1, L'\0');
  GetWindowTextW(h, s.data(), n + 1);
  s.resize(n);
  return s;
}

// AD_IMPORT_TEST_PICK: the tests' stand-in for the file dialogs ("a|b" = two files).
std::vector<fs::path> test_pick() {
  std::vector<fs::path> out;
  std::wstring v = env(L"AD_IMPORT_TEST_PICK");
  size_t p = 0;
  while (!v.empty() && p <= v.size()) {
    size_t bar = v.find(L'|', p);
    std::wstring part = v.substr(p, bar == std::wstring::npos ? std::wstring::npos : bar - p);
    if (!part.empty()) out.emplace_back(part);
    if (bar == std::wstring::npos) break;
    p = bar + 1;
  }
  return out;
}

std::vector<fs::path> run_open_dialog(HWND owner, bool folder, bool multi, const wchar_t* title,
                                      const COMDLG_FILTERSPEC* types, UINT ntypes) {
  std::vector<fs::path> out;
  IFileOpenDialog* d = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog, (void**)&d)))
    return out;
  DWORD opts = 0;
  d->GetOptions(&opts);
  d->SetOptions(dialog_options(opts, folder, multi));
  d->SetTitle(title);
  if (types && ntypes) d->SetFileTypes(ntypes, types);
  if (SUCCEEDED(d->Show(owner))) {
    IShellItemArray* items = nullptr;
    if (SUCCEEDED(d->GetResults(&items))) {
      out = dialog_paths(items);
      items->Release();
    }
  }
  d->Release();
  return out;
}

// A page going away while its worker runs (the process is quitting, or the
// window is destroyed some other way): the worker has been cancelled through
// its token, which stops even a download blocked on the network at once, and
// what is left (a rename's retries, a stage's cleanup) takes seconds at
// most. The window is hidden meanwhile and messages keep flowing, so nothing
// shows "Not responding"; a WM_QUIT that arrives is passed on afterwards.
void wait_for_worker(std::thread& worker, const std::atomic<bool>& done, HWND hwnd) {
  if (!worker.joinable()) return;
  if (hwnd && IsWindow(hwnd)) ShowWindow(hwnd, SW_HIDE);
  bool quit = false;
  WPARAM quit_code = 0;
  while (!done.load()) {
    MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
      if (m.message == WM_QUIT) {
        quit = true;
        quit_code = m.wParam;
        continue;
      }
      TranslateMessage(&m);
      DispatchMessageW(&m);
    }
  }
  if (worker.joinable()) worker.join();
  if (quit) PostQuitMessage(int(quit_code));
}

int registry_index(const std::string& id) {
  auto reg = builtin_packages();
  for (size_t i = 0; i < reg.size(); ++i)
    if (id == reg[i].id) return (int)i;
  return -1;
}

HWND make_progress(HWND parent, const ui::Theme* t) {
  HWND bar = CreateWindowExW(0, PROGRESS_CLASSW, L"Progress", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, parent, nullptr,
                             GetModuleHandleW(nullptr), nullptr);
  SendMessageW(bar, PBM_SETRANGE32, 0, 1000);
  ui::set_surface(bar, ui::Surface::base);
  ui::subclass_progress(bar, t);
  return bar;
}

void set_marquee_style(HWND bar, bool on) {
  const LONG st = GetWindowLongW(bar, GWL_STYLE);
  if (on) {
    SetWindowLongW(bar, GWL_STYLE, st | PBS_MARQUEE);
    SendMessageW(bar, PBM_SETMARQUEE, TRUE, 30);
  } else {
    SendMessageW(bar, PBM_SETMARQUEE, FALSE, 0);
    SetWindowLongW(bar, GWL_STYLE, st & ~PBS_MARQUEE);
    SendMessageW(bar, PBM_SETRANGE32, 0, 1000);
  }
}

}  // namespace

DWORD dialog_options(DWORD base, bool folder, bool multi) {
  // File-system paths only (never a library or a phone); a file must exist;
  // several images at once for split floppies.
  return base | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST | (multi ? FOS_ALLOWMULTISELECT : 0));
}

std::vector<fs::path> dialog_paths(IShellItemArray* items) {
  std::vector<fs::path> out;
  DWORD n = 0;
  if (!items || FAILED(items->GetCount(&n))) return out;
  for (DWORD i = 0; i < n; i++) {
    IShellItem* item = nullptr;
    if (FAILED(items->GetItemAt(i, &item))) continue;
    PWSTR p = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) out.push_back(fs::path(p));
    CoTaskMemFree(p);
    item->Release();
  }
  return out;
}

std::vector<fs::path> pick_source(HWND owner, bool folder) {
  if (!env(L"AD_IMPORT_TEST_PICK").empty()) return test_pick();
  std::vector<fs::path> out;
  if (folder) {
    out = run_open_dialog(owner, true, false, L"Choose the CD drive, or a folder holding a copy of the disc or floppies",
                          nullptr, 0);
  } else {
    COMDLG_FILTERSPEC types[] = {
        {L"Disc and floppy images, and ZIPs or 7z archives of them or of install files (*.iso; *.bin; *.img; *.ima; "
         L"*.vfd; *.flp; *.zip; *.7z)",
         L"*.iso;*.bin;*.img;*.ima;*.vfd;*.flp;*.zip;*.7z"},
        {L"All files", L"*.*"}};
    out = run_open_dialog(owner, false, true, L"Choose an image of a CD or floppy disk (every disk of a set)",
                          types, 2);
  }
  std::sort(out.begin(), out.end());   // disk 1 before disk 2 when the names say so
  return out;
}

std::optional<fs::path> pick_picture(HWND owner) {
  std::vector<fs::path> p;
  if (!env(L"AD_IMPORT_TEST_PICK").empty()) {
    p = test_pick();
  } else {
    COMDLG_FILTERSPEC types[] = {
        {L"Pictures (*.png; *.jpg; *.jpeg; *.gif; *.bmp; *.tif; *.tiff; *.webp; *.avif; *.heic; *.jxr)",
         L"*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.tif;*.tiff;*.webp;*.avif;*.heic;*.jxr"},
        {L"All files", L"*.*"}};
    p = run_open_dialog(owner, false, false, L"Choose a picture for the box cover", types, 2);
  }
  if (p.empty()) return std::nullopt;
  return p.front();
}

// ==== Sources ===========================================================================

SourcesPage::SourcesPage(Session& s) : Page(s) {}
SourcesPage::~SourcesPage() { destroy_window(); }

void SourcesPage::build() {
  rows_ = installed_rows(s_.assets);
  // Releases imported before covers existed (or offline) show a generated
  // cover: offer to fetch their pictures (never done on its own, COVERS.md §2.10).
  if (s_.req.cover_download) missing_ = missing_cover_ids(rows_);
  // Everything between the header and the footer sits in one body that
  // scrolls on a very short screen; the installed list scrolls inside its
  // card first.
  body_ = std::make_unique<ScrollPanel>(*this, &t_, ui::Surface::base);
  HWND body = body_->hwnd();
  // Every release by name until some are imported; then how many, the list
  // below naming what is here (ten and more names would crowd it out).
  intro_ = add_text(sources_intro(!rows_.empty()), Face::body, Ink::text2, body);
  // What the last window did, once ("Removed …").
  if (!s_.notice.empty()) {
    notice_ = add_text(s_.notice, Face::body, Ink::text, body);
    s_.notice.clear();
  }
  if (!rows_.empty()) {
    installed_label_ = add_text(L"Installed", Face::body_strong, Ink::text, body, true);
    list_ = std::make_unique<ScrollPanel>(*this, &t_, ui::Surface::card, body);
    SetWindowTextW(list_->hwnd(), L"Installed");
    // One cover per release, each a button (its text is what screen readers
    // call it; the page draws it) that opens the release's menu. Only one is
    // a tab stop (the first, then the one last focused); arrows move.
    if (!s_.offscreen) {
      tip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                             CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, hwnd_, nullptr,
                             GetModuleHandleW(nullptr), nullptr);
      SendMessageW(tip_, TTM_SETMAXTIPWIDTH, 0, px(360));
      ui::theme_native_control(tip_, t_.pal.dark && !t_.pal.high_contrast);
    }
    tiles_.resize(rows_.size());
    for (size_t i = 0; i < rows_.size(); ++i) {
      if (!rows_[i].cover.tile.empty()) ui::load_image(rows_[i].cover.tile.wstring(), tiles_[i]);
      HWND b = add_button(kIdTileBase + registry_index(rows_[i].id), installed_tile_name(rows_[i]), ui::ButtonRole::standard,
                          0, list_->hwnd(), ui::Surface::card);
      LONG_PTR style = GetWindowLongPtrW(b, GWL_STYLE) & ~(LONG_PTR)(BS_MULTILINE | WS_TABSTOP);
      if (i == 0) style |= WS_TABSTOP;
      SetWindowLongPtrW(b, GWL_STYLE, style);
      SetWindowSubclass(b, tile_proc, 1, reinterpret_cast<DWORD_PTR>(this));
      tile_btn_.push_back(b);
      if (tip_) {
        TOOLINFOW ti{sizeof(ti)};
        ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        ti.hwnd = hwnd_;
        ti.uId = (UINT_PTR)b;
        const std::wstring tip = installed_tile_tip(rows_[i]);
        ti.lpszText = const_cast<wchar_t*>(tip.c_str());
        SendMessageW(tip_, TTM_ADDTOOLW, 0, (LPARAM)&ti);
      }
    }
    installed_note_ = add_text(L"Click a cover to change it or to remove the release. Importing a release again replaces "
                               L"it; the others are kept.",
                               Face::caption, Ink::text3, body);
    if (!missing_.empty()) {
      covers_note_ = add_text(missing_covers_note(missing_.size()), Face::body, Ink::text2, body);
      covers_link_ = add_button(kIdGetCovers, L"Get the covers", ui::ButtonRole::subtle, L'', body);
    }
  }
  from_label_ = add_text(L"Import from", Face::body_strong, Ink::text, body, true);
  card_image_ = add_button(kIdImage,
                           L"A disc image…\nAn ISO image of a CD or floppy images (.img), or a ZIP or 7z of them or "
                           L"of the install files; select every disk of a set.",
                           ui::ButtonRole::card, L'', body);
  card_folder_ = add_button(kIdFolder, L"A drive or folder…\nThe CD drive, or a folder holding a copy of the disc or floppies.",
                            ui::ButtonRole::card, L'', body);
  std::wstring dl = download_card_text(s_.req.package);
  if (!dl.empty()) card_download_ = add_button(kIdDownload, dl, ui::ButtonRole::card, L'', body);
  caution_ = add_text(L"", Face::body, Ink::text, body, false, Bg::caution);
  ShowWindow(caution_, SW_HIDE);
  body_->painter = [this](HDC dc, int dy) {
    if (list_card_.right > list_card_.left) {
      RECT card = list_card_;
      OffsetRect(&card, 0, dy);
      ui::paint_card(dc, card, t_);
    }
    paint_info_box(dc, caution_, Bg::caution, body_->hwnd());
  };
  footer_text_ = add_text(L"Files are copied to " + win_assets_dir(s_.assets).wstring(), Face::caption, Ink::text3, nullptr,
                          true, Bg::base, SS_PATHELLIPSIS);
  cancel_ = add_button(IDCANCEL, s_.tally.changed ? L"Close" : L"Cancel", ui::ButtonRole::standard);
}

int SourcesPage::layout(int w, int max_h) {
  const int x0 = px(kMargin), cw = w - 2 * x0, fm = ui::focus_margin(t_.dpi);
  const int hl = t_.hairline(), pad = px(4);
  // The installed covers: a grid in the card, 8 DIP in from its edges and
  // apart, with as many columns as cells of at least 100 DIP fit, sharing
  // the width (fewer releases than that keep the cells' width, from the
  // left). A cell is its cover (64x80, 8 DIP from its top) with the short
  // title and the module count under it, a caption line each.
  const int pw = cw - 2 * hl, edge = px(8), gap = px(8);
  const int n = (int)rows_.size();
  const int cap = std::max(1, (pw - 2 * edge + gap) / (px(100) + gap));
  const int cell_w = std::max(px(72), (pw - 2 * edge - (cap - 1) * gap) / cap);
  int line = px(16);
  if (HDC dc = GetDC(hwnd_)) {
    line = std::max(line, (int)ui::measure_text(dc, L"Ag", t_.fonts.caption, DT_SINGLELINE).cy);
    ReleaseDC(hwnd_, dc);
  }
  const int cell_h = px(8) + px(80) + px(6) + 2 * line + px(8);
  cols_ = cap;
  row_h_ = cell_h + gap;
  const int rows = (n + cap - 1) / cap;
  // Whole rows of the grid show in a list `k * row_h_ + 2 * edge - gap` tall.
  const int frame = 2 * edge - gap, natural = rows ? rows * row_h_ + frame : 0;
  // The body's content, in its own coordinates, with the installed list
  // `list_h` tall; placed only when `apply`. Returns the content height.
  auto content = [&](int list_h, bool apply) {
    int y = px(4);
    const int ih = text_height(intro_, cw);
    if (apply) body_->put(intro_, x0, y, cw, ih);
    y += ih + px(kGap);
    if (notice_) {
      const int nh = text_height(notice_, cw);
      if (apply) body_->put(notice_, x0, y - px(8), cw, nh);
      y += nh + px(kGap) - px(8);
    }
    if (list_) {
      const int lh = text_height(installed_label_, cw);
      if (apply) body_->put(installed_label_, x0, y, cw, lh);
      y += lh + px(8);
      if (apply) {
        list_card_ = RECT{x0, y, x0 + cw, y + list_h + 2 * pad};
        body_->put(list_->hwnd(), x0 + hl, y + pad, list_->window_width(cw - 2 * hl), list_h);
      }
      y += list_h + 2 * pad + px(8);
      if (covers_note_) {
        // "3 releases have no cover picture yet."  [Get the covers]
        const int lw = button_width(covers_link_, 0) + px(24);
        const int tw = std::max(px(80), cw - lw - px(kGap));
        const int th = text_height(covers_note_, tw), rh = std::max(th, px(kControlH));
        if (apply) {
          body_->put(covers_note_, x0, y + (rh - th) / 2, tw, th);
          body_->put(covers_link_, x0 + cw - lw + px(8) - fm, y + (rh - px(kControlH)) / 2 - fm, lw + 2 * fm,
                     px(kControlH) + 2 * fm);
        }
        y += rh + px(8);
      }
      const int nh = text_height(installed_note_, cw);
      if (apply) body_->put(installed_note_, x0, y, cw, nh);
      y += nh + px(kGap);
    } else if (apply) {
      list_card_ = RECT{};
    }
    const int fh = text_height(from_label_, cw);
    if (apply) body_->put(from_label_, x0, y, cw, fh);
    y += fh + px(8);
    for (HWND card : {card_image_, card_folder_, card_download_}) {
      if (!card) continue;
      const int ch = ui::card_height(t_, window_text(card), cw + 2 * fm, false);
      if (apply) body_->put(card, x0 - fm, y - fm, cw + 2 * fm, ch);
      y += ch - 2 * fm + px(4);
    }
    if (IsWindowVisible(caution_)) {
      y += px(12);
      const int th = text_height(caution_, cw - px(52));
      if (apply) body_->put(caution_, x0 + px(40), y + px(12), cw - px(52), th);
      y += th + px(24);
    }
    return y + px(16);
  };
  // Too tall for the work area (on a short screen; three rows of six covers
  // hold sixteen releases): the grid scrolls in its card, showing as many whole
  // rows as fit (two at the least) when that makes the page fit, else as
  // many as fit down to one (the part of a row that shows says there are
  // more); when even one row does not fit, the whole body scrolls instead,
  // with the grid at its full height, so only one thing ever scrolls.
  const int top = header_h(), footer = px(kFooter);
  int list_h = natural;
  if (top + content(natural, false) + footer > max_h) {
    const int shrunk = std::min(natural, 2 * row_h_ + frame);
    // The content grows with the list's height one for one.
    const int room = max_h - top - footer - content(0, false);
    if (room >= shrunk) list_h = std::max(shrunk, (room - frame) / row_h_ * row_h_ + frame);
    else if (room >= std::min(natural, row_h_ + frame)) list_h = room;
  }
  list_heights_ = list_ ? ListHeights{list_h, natural, row_h_, rows, cap} : ListHeights{};
  const int content_h = content(list_h, true);
  const int view_h = std::max(px(96), std::min(content_h, max_h - top - footer));
  if (list_) {
    for (int i = 0; i < n; ++i) {
      const int x = edge + (i % cap) * (cell_w + gap), y = edge + (i / cap) * row_h_;
      list_->put(tile_btn_[i], x - fm, y - fm, cell_w + 2 * fm, cell_h + 2 * fm);
    }
    list_->set_view(pw, list_h, natural);   // where the body put it
  }
  body_->place(0, top, w, view_h, content_h);
  const int y = top + view_h;
  footer_top_ = y;
  const int bw = button_width(cancel_);
  const int th = text_height(footer_text_, cw - bw - px(kGap));
  place_body(cancel_, w - x0 - bw, y + (footer - px(kControlH)) / 2, bw, px(kControlH));
  place(footer_text_, x0, y + (footer - th) / 2, cw - bw - px(kGap), th);
  return y + footer;
}

void SourcesPage::paint(HDC) {}

int SourcesPage::tile_index(HWND h) const {
  for (size_t i = 0; i < tile_btn_.size(); ++i)
    if (tile_btn_[i] == h) return (int)i;
  return -1;
}

// A cover the arrow keys reach takes the focus and becomes the tab stop.
void SourcesPage::focus_tile(int i) {
  if (i < 0 || i >= (int)tile_btn_.size()) return;
  SetFocus(tile_btn_[i]);
}

LRESULT CALLBACK SourcesPage::tile_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
  auto* self = reinterpret_cast<SourcesPage*>(ref);
  switch (msg) {
    case WM_GETDLGCODE:
      // The arrows move between the covers here, not in the dialog manager.
      return DefSubclassProc(h, msg, wp, lp) | DLGC_WANTARROWS;
    case WM_KEYDOWN: {
      const int n = (int)self->tile_btn_.size(), i = self->tile_index(h), cols = std::max(1, self->cols_);
      if (n == 0 || i < 0) break;
      int to = -1;
      switch (wp) {
        case VK_LEFT: to = (i + n - 1) % n; break;
        case VK_RIGHT: to = (i + 1) % n; break;
        // Up and Down: the cover above or below (the last one when the row
        // below is shorter), stopping at the first and last rows.
        case VK_UP: to = i >= cols ? i - cols : i; break;
        case VK_DOWN: to = i / cols < (n - 1) / cols ? std::min(n - 1, i + cols) : i; break;
        case VK_HOME: to = 0; break;
        case VK_END: to = n - 1; break;
      }
      if (to < 0) break;
      self->focus_tile(to);
      return 0;
    }
    case WM_SETFOCUS:
      // The one tab stop among the covers is the one last focused.
      for (HWND b : self->tile_btn_) {
        const LONG_PTR style = GetWindowLongPtrW(b, GWL_STYLE);
        const LONG_PTR want = b == h ? (style | WS_TABSTOP) : (style & ~(LONG_PTR)WS_TABSTOP);
        if (want != style) SetWindowLongPtrW(b, GWL_STYLE, want);
      }
      break;
    case WM_NCDESTROY:
      RemoveWindowSubclass(h, tile_proc, 1);
      break;
  }
  return DefSubclassProc(h, msg, wp, lp);
}

// A cover: its picture with the short title and the module count under it,
// on the card, with a fill under the pointer and the focus ring around it.
bool SourcesPage::draw_button(NMCUSTOMDRAW* cd, LRESULT* result) {
  const int i = tile_index(cd->hdr.hwndFrom);
  if (i < 0) return false;
  *result = CDRF_SKIPDEFAULT;
  if (cd->dwDrawStage != CDDS_PREPAINT) {
    *result = CDRF_DODEFAULT;
    return true;
  }
  const ui::Palette& p = t_.pal;
  const float s = t_.dpi / 96.0f;
  HWND h = cd->hdr.hwndFrom;
  const RECT cr = cd->rc;
  const int w = std::max(1L, cr.right - cr.left), hgt = std::max(1L, cr.bottom - cr.top);
  HDC dc = CreateCompatibleDC(cd->hdc);
  HBITMAP bmp = CreateCompatibleBitmap(cd->hdc, w, hgt);
  HGDIOBJ old = SelectObject(dc, bmp);
  SetViewportOrgEx(dc, -cr.left, -cr.top, nullptr);
  ui::fill_rect(dc, cr, p.card);
  const int fm = ui::focus_margin(t_.dpi);
  RECT body = cr;
  InflateRect(&body, -fm, -fm);
  const bool hot = (cd->uItemState & CDIS_HOT) != 0, pressed = (cd->uItemState & CDIS_SELECTED) != 0;
  if (p.high_contrast) {
    if (hot || pressed) ui::stroke_round(dc, body, 4 * s, p.accent, 2.0f * t_.hairline());
  } else if (hot || pressed) {
    ui::fill_round(dc, body, 4 * s, pressed ? p.control_pressed : p.control_hover);
  }
  const int aw = px(64), ah = px(80);
  const RECT art{body.left + (body.right - body.left - aw) / 2, body.top + px(8),
                 body.left + (body.right - body.left - aw) / 2 + aw, body.top + px(8) + ah};
  ui::draw_cover(dc, art, tiles_[i].empty() ? nullptr : &tiles_[i], rows_[i].title, t_);
  const int line = (body.bottom - px(8) - (art.bottom + px(6))) / 2;
  const UINT flags = DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX;
  RECT title{body.left + px(4), art.bottom + px(6), body.right - px(4), art.bottom + px(6) + line};
  ui::draw_text(dc, rows_[i].short_title, title, t_.fonts.caption, p.text, flags);
  RECT count{title.left, title.bottom, title.right, title.bottom + line};
  ui::draw_text(dc, rows_[i].modules, count, t_.fonts.caption, p.text2, flags);
  const bool focus = ((cd->uItemState & CDIS_FOCUS) || ui::focused_window() == h) && ui::keyboard_cues(h);
  if (focus) ui::draw_focus_ring(dc, cr, 4 * s + fm, p, s);
  SetViewportOrgEx(dc, 0, 0, nullptr);
  BitBlt(cd->hdc, cr.left, cr.top, w, hgt, dc, 0, 0, SRCCOPY);
  SelectObject(dc, old);
  DeleteObject(bmp);
  DeleteDC(dc);
  return true;
}

// A cover's menu, at `pt` (screen), or under the cover ({-1, -1}: a click,
// Enter or Space, Shift+F10, the Apps key).
void SourcesPage::show_menu(int i, POINT pt) {
  if (i < 0 || i >= (int)rows_.size()) return;
  if (pt.x == -1 && pt.y == -1) {
    RECT r{};
    GetWindowRect(tile_btn_[i], &r);
    const int fm = ui::focus_margin(t_.dpi);
    pt = POINT{r.left + fm, r.bottom - fm};
  }
  const int reg = registry_index(rows_[i].id);
  HMENU menu = CreatePopupMenu();
  AppendMenuW(menu, MF_STRING, kIdChangeCoverBase + reg, L"Change cover…");
  // Last, as the one that takes something away: the next window asks first.
  AppendMenuW(menu, MF_STRING, kIdRemoveBase + reg, (L"Remove " + rows_[i].short_title + L"…").c_str());
  const int cmd = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0,
                                      hwnd_, nullptr);
  DestroyMenu(menu);
  if (cmd) command(cmd, BN_CLICKED, nullptr);
}

bool SourcesPage::context_menu(HWND ctl, POINT pt) {
  const int i = tile_index(ctl);
  if (i < 0) return false;
  SetFocus(ctl);
  show_menu(i, pt);
  return true;
}

// "Change cover…" and "Remove…" of an installed release (its cover's menu,
// or TDM_CLICK_BUTTON).
bool SourcesPage::menu_command(int id) const {
  auto reg = builtin_packages();
  for (int base : {kIdChangeCoverBase, kIdRemoveBase}) {
    if (id < base || id >= base + (int)reg.size()) continue;
    const std::string rid = reg[id - base].id;
    return std::any_of(rows_.begin(), rows_.end(), [&](const InstalledRow& r) { return r.id == rid; });
  }
  return false;
}

void SourcesPage::theme_changed() {
  if (tip_) ui::theme_native_control(tip_, t_.pal.dark && !t_.pal.high_contrast);
}

void SourcesPage::show_caution(const std::wstring& text) {
  set_text(caution_, text);
  ShowWindow(caution_, SW_SHOW);
  relayout();
}

void SourcesPage::command(int id, int, HWND) {
  switch (id) {
    case kIdImage: {
      auto p = pick_source(hwnd_, false);
      if (p.empty()) return;
      paths = std::move(p);
      choice = Choice::image;
      finish(id);
      return;
    }
    case kIdFolder: {
      auto p = pick_source(hwnd_, true);
      if (p.empty()) return;
      std::string why;
      if (!identify_folder(p.front(), &why)) {
        // The reason may start with the folder itself ("<folder> holds DISK1
        // and DISK2 beside other files or folders …"): then it is not named twice.
        const std::wstring where = p.front().wstring(), reason = to_wide(why);
        show_caution(L"That is not a disc Long After Dark knows. " +
                     (reason.rfind(where, 0) == 0 ? reason : where + L": " + reason) +
                     L". Choose the CD drive itself (for example E:\\) or a copy of the disc or floppies.");
        return;
      }
      paths = {p.front()};
      choice = Choice::folder;
      finish(id);
      return;
    }
    case kIdDownload:
      choice = Choice::downloads;
      finish(id);
      return;
    case kIdGetCovers:
      if (missing_.empty()) return;
      cover_ids = missing_;
      choice = Choice::get_covers;
      finish(id);
      return;
    case IDCANCEL:
      choice = Choice::cancel;
      finish(id);
      return;
  }
  auto reg = builtin_packages();
  if (id >= kIdTileBase && id < kIdTileBase + (int)reg.size()) {
    // A cover: its menu, under it.
    for (size_t i = 0; i < rows_.size(); ++i)
      if (rows_[i].id == reg[id - kIdTileBase].id) show_menu((int)i, POINT{-1, -1});
    return;
  }
  if (!menu_command(id)) return;
  if (id >= kIdChangeCoverBase && id < kIdChangeCoverBase + (int)reg.size()) {
    cover_id = reg[id - kIdChangeCoverBase].id;
    choice = Choice::change_cover;
    finish(id);
  } else if (id >= kIdRemoveBase && id < kIdRemoveBase + (int)reg.size()) {
    cover_id = reg[id - kIdRemoveBase].id;
    choice = Choice::remove;
    finish(id);
  }
}

// ==== Downloads =========================================================================

DownloadsPage::DownloadsPage(Session& s) : Page(s) {}
DownloadsPage::~DownloadsPage() { destroy_window(); }

void DownloadsPage::build() {
  // --download-dir, else the default: where "already downloaded" is looked for and what the note names.
  const fs::path dl_dir = s_.req.download_dir.empty() ? default_download_dir() : s_.req.download_dir;
  rows_ = download_rows(s_.assets, s_.req.package, dl_dir);
  all_ = all_missing_row(rows_);
  intro_ = add_text(L"Choose the release to download and import. Importing one that is already imported replaces it; "
                    L"the others are kept.",
                    Face::body, Ink::text2);
  list_ = std::make_unique<ScrollPanel>(*this, &t_, ui::Surface::base);
  SetWindowTextW(list_->hwnd(), L"Releases");
  tiles_.resize(rows_.size());
  for (size_t i = 0; i < rows_.size(); ++i) {
    // COVERS.md §4.2: the cover of an imported release, else the generated one.
    if (rows_[i].installed && !rows_[i].cover.tile.empty()) ui::load_image(rows_[i].cover.tile.wstring(), tiles_[i]);
    covers_.push_back(std::make_unique<ui::CardCover>(ui::CardCover{tiles_[i].empty() ? nullptr : &tiles_[i], rows_[i].title}));
    HWND c = add_button(kIdDownloadBase + (int)i, rows_[i].text, ui::ButtonRole::card, 0, list_->hwnd());
    ui::set_card_cover(c, covers_.back().get());
    cards_.push_back(c);
  }
  if (all_) all_card_ = add_button(kIdDownloadAll, all_->text, ui::ButtonRole::card, L'\uE896', list_->hwnd());
  note_ = add_text(L"Each download is checked against the size and md5 the Internet Archive publishes and kept in " +
                       dl_dir.wstring() + L", so it is fetched only once.",
                   Face::caption, Ink::text3);
  back_ = add_button(IDCANCEL, L"Back", ui::ButtonRole::standard);
}

int DownloadsPage::layout(int w, int max_h) {
  const int x0 = px(kMargin), cw = w - 2 * x0, fm = ui::focus_margin(t_.dpi);
  int y = header_h() + px(4);
  const int ih = text_height(intro_, cw);
  place(intro_, x0, y, cw, ih);
  y += ih + px(kGap);
  // The footer: the note beside Back.
  const int bw = button_width(back_);
  const int note_w = cw - bw - px(24);
  const int nh = text_height(note_, note_w);
  const int footer_h = std::max(px(kFooter), nh + px(32));
  std::vector<HWND> all_cards = cards_;
  if (all_card_) all_cards.push_back(all_card_);
  // The cards in the list's own coordinates (each window is its body plus the
  // focus margin, which the list keeps clear at its edges).
  auto content = [&](int card_w, bool apply) {
    int yc = fm;
    for (HWND c : all_cards) {
      const int ch = ui::card_height(t_, window_text(c), card_w + 2 * fm, c != all_card_);
      if (apply) list_->put(c, 0, yc - fm, card_w + 2 * fm, ch);
      yc += ch - 2 * fm + px(4);
    }
    return yc - px(4) + fm;
  };
  const int avail = std::max(px(80), max_h - (y - fm) - px(kGap) - footer_h);
  int card_w = cw;
  int natural = content(card_w, false);
  if (natural > avail) {
    // Room for the scroll bar at the right.
    card_w = cw - px(12);
    natural = content(card_w, false);
  }
  content(card_w, true);
  const int list_h = std::min(natural, avail);
  list_->place(x0 - fm, y - fm, cw + 2 * fm, list_h, natural);
  y = y - fm + list_h + px(kGap);
  footer_top_ = y;
  place_body(back_, w - x0 - bw, y + (footer_h - px(kControlH)) / 2, bw, px(kControlH));
  place(note_, x0, y + (footer_h - nh) / 2, note_w, nh);
  return y + footer_h;
}

void DownloadsPage::command(int id, int, HWND) {
  if (id == IDCANCEL) {
    ids.clear();
    finish(id);
  } else if (id == kIdDownloadAll && all_) {
    ids = all_->ids;
    finish(id);
  } else if (id >= kIdDownloadBase && id < kIdDownloadBase + (int)rows_.size()) {
    ids = {rows_[size_t(id - kIdDownloadBase)].id};
    finish(id);
  }
}

// ==== Progress ==========================================================================

ProgressPage::ProgressPage(Session& s, Job job, bool start) : Page(s), job_(std::move(job)), start_(start) {}

ProgressPage::~ProgressPage() {
  cancel_ = true;
  token_.cancel();
  if (hwnd_) KillTimer(hwnd_, 1);
  wait_for_worker(worker_, finished_, hwnd_);
  destroy_window();
}

void ProgressPage::build() {
  // The release a download starts with (a custom URL names none until it is identified).
  const Package* first = nullptr;
  if (job_.source.kind == Source::Kind::download) {
    if (!job_.all.empty()) first = find_package(job_.all.front());
    else if (job_.source.url.empty() || !job_.source.package.empty())
      first = find_package(job_.source.package.empty() ? "deluxe" : job_.source.package);
  }
  const std::wstring instruction =
      job_.covers_job ? phase_instruction(Progress::Phase::cover, "")
      : job_.source.kind == Source::Kind::download
          ? phase_instruction(Progress::Phase::download, first ? first->title : "", 0, std::max<size_t>(job_.all.size(), 1))
          : L"Importing";
  phase_ = add_text(instruction, Face::body_strong, Ink::text);
  bar_ = make_progress(hwnd_, &t_);
  amount_ = add_text(L"Starting\u2026", Face::body, Ink::text2, nullptr, true);
  item_ = add_text(L"", Face::caption, Ink::text3, nullptr, true);
  cancel_btn_ = add_button(IDCANCEL, L"Cancel", ui::ButtonRole::standard);
  if (!start_) return;
  SetTimer(hwnd_, 1, 100, nullptr);
  if (job_.covers_job) {
    // "Get the covers": refresh_covers, no import.
    worker_ = std::thread([this] {
      CoverOptions o;
      o.assets_root = s_.req.dest;
      o.download_dir = s_.req.download_dir;
      o.allow_download = s_.req.cover_download;
      o.force = job_.force;
      o.cancel = &token_;
      o.progress = [this](const Progress& p) {
        std::lock_guard<std::mutex> lock(m_);
        progress_ = p;
        have_progress_ = true;
        return !cancel_.load();
      };
      std::vector<CoverResult> rs = refresh_covers(job_.covers, o);
      {
        std::lock_guard<std::mutex> lock(m_);
        cover_results_ = std::move(rs);
      }
      finished_ = true;
    });
    return;
  }
  worker_ = std::thread([this] {
    ImportOptions o;
    o.assets_root = s_.req.dest;
    o.check_known = !s_.req.no_verify;
    o.cover_download = s_.req.cover_download;
    o.cover_download_dir = s_.req.download_dir;   // empty: the download's own folder, else the default
    o.cancel = &token_;
    o.progress = [this](const Progress& p) {
      std::lock_guard<std::mutex> lock(m_);
      progress_ = p;
      have_progress_ = true;
      return !cancel_.load();
    };
    std::vector<ImportResult> rs;
    if (job_.all.empty()) {
      rs.push_back(run_import(job_.source, o));
    } else {
      rs = import_downloads(job_.all, job_.source, o, [this](size_t i) {
        // Each release starts from nothing, as the first one did: its download
        // with no bytes yet ("Starting…"), not the last report of the one
        // before ("Finishing <previous>") until its first bytes come.
        const Package* next = find_package(job_.all[i]);
        std::lock_guard<std::mutex> lock(m_);
        step_ = i;
        progress_ = Progress{};
        progress_.phase = Progress::Phase::download;
        progress_.package = next ? next->title : job_.all[i];
        have_progress_ = true;
      });
    }
    {
      std::lock_guard<std::mutex> lock(m_);
      results_ = std::move(rs);
    }
    finished_ = true;
  });
}

int ProgressPage::layout(int w, int) {
  const int x0 = px(kMargin), cw = w - 2 * x0;
  int y = header_h() + px(12);
  const int ph = text_height(phase_, cw);
  place(phase_, x0, y, cw, ph);
  y += ph + px(12);
  place(bar_, x0, y, cw, px(12));
  y += px(12) + px(12);
  const int ah = text_height(amount_, cw);
  place(amount_, x0, y, cw, ah);
  y += ah + px(4);
  const int th = text_height(item_, cw);
  place(item_, x0, y, cw, th);
  y += th + px(24);
  footer_top_ = y;
  const int bw = button_width(cancel_btn_);
  place_body(cancel_btn_, w - x0 - bw, y + (px(kFooter) - px(kControlH)) / 2, bw, px(kControlH));
  return y + px(kFooter);
}

void ProgressPage::set_marquee(bool on) {
  if (on == marquee_) return;
  marquee_ = on;
  set_marquee_style(bar_, on);
}

void ProgressPage::show_progress(const Progress& p, size_t step, bool cancelling) {
  {
    std::lock_guard<std::mutex> lock(m_);
    progress_ = p;
    have_progress_ = true;
    step_ = step;
  }
  if (cancelling) {
    cancel_ = true;
    EnableWindow(cancel_btn_, FALSE);
  }
  refresh();
}

void ProgressPage::refresh() {
  Progress p;
  size_t step = 0;
  {
    std::lock_guard<std::mutex> lock(m_);
    if (!have_progress_) return;
    p = progress_;
    step = step_;
  }
  const ULONGLONG now = GetTickCount64();
  const size_t steps = std::max<size_t>(job_.all.size(), 1);
  if (!shown_any_ || p.phase != shown_phase_ || p.package != shown_package_ || step != shown_step_) {
    shown_any_ = true;
    shown_phase_ = p.phase;
    shown_package_ = p.package;
    shown_step_ = step;
    const std::wstring before = window_text(phase_);
    set_text(phase_, phase_instruction(p.phase, p.package, step, steps));
    if (window_text(phase_) != before) relayout();
    speed_tick_ = now;
    speed_bytes_ = p.done;
    speed_.clear();
  }
  if (p.done < speed_bytes_) speed_tick_ = now, speed_bytes_ = p.done;   // restarted from 0
  if (p.phase == Progress::Phase::download && now >= speed_tick_ + 2000) {
    const double rate = double(p.done - speed_bytes_) / (double(now - speed_tick_) / 1000.0);
    speed_ = mb(uint64_t(rate)) + "/s";
    speed_tick_ = now;
    speed_bytes_ = p.done;
  }
  set_marquee(p.total == 0);
  if (p.total) SendMessageW(bar_, PBM_SETPOS, WPARAM(std::min<uint64_t>(p.done, p.total) * 1000 / p.total), 0);
  if (cancel_) {
    set_text(amount_, L"Cancelling\u2026");
    set_text(item_, L"");
  } else {
    // A cover taken from the disc has no size to count ("0.0 MB" would say
    // nothing), nor has the finishing step (one step, not bytes); a download
    // with no bytes yet is starting, as the window says when it opens.
    const bool no_amount =
        (p.phase == Progress::Phase::cover && p.total == 0 && p.done == 0) || p.phase == Progress::Phase::finalize;
    const bool starting = p.phase == Progress::Phase::download && p.total == 0 && p.done == 0;
    set_text(amount_, no_amount ? std::wstring()
                      : starting ? std::wstring(L"Starting\u2026")
                                 : amount_line(p.done, p.total, speed_));
    // What the step itself names (the file copied or downloaded, the cover's
    // source), else nothing: the last log line may be about another step.
    set_text(item_, to_wide(p.item));
  }
}

void ProgressPage::timer(UINT_PTR id) {
  if (id != 1) return;
  if (finished_) {
    KillTimer(hwnd_, 1);
    if (worker_.joinable()) worker_.join();
    finish(IDOK);
    return;
  }
  refresh();
}

void ProgressPage::command(int id, int, HWND) {
  if (id != IDCANCEL || finished_ || !start_) return;
  // Keep the window up until the worker has stopped and cleaned its stage.
  cancel_ = true;
  token_.cancel();
  EnableWindow(cancel_btn_, FALSE);
  SetFocus(hwnd_);
  set_text(amount_, L"Cancelling\u2026");
  set_text(item_, L"");
}

// ==== Result ============================================================================

ResultPage::ResultPage(Session& s, ResultText text) : Page(s), rt_(std::move(text)) {}
ResultPage::~ResultPage() { destroy_window(); }

void ResultPage::build() {
  heading_ = add_text(rt_.heading, Face::subtitle, Ink::text);
  body_ = add_text(rt_.body, Face::body, Ink::text);
  if (rt_.kind != Outcome::success && !rt_.details.empty())
    copy_ = add_button(kIdCopyDetails, L"Copy details", ui::ButtonRole::subtle, L'\uE8C8');
  done_ = add_button(IDOK, L"Done", ui::ButtonRole::accent);
}

int ResultPage::layout(int w, int) {
  const int x0 = px(kMargin), cw = w - 2 * x0, fm = ui::focus_margin(t_.dpi);
  int y = header_h() + px(12);
  glyph_ = RECT{x0, y, x0 + px(40), y + px(40)};
  const int tx = x0 + px(56), tw = cw - px(56);
  const int hh = text_height(heading_, tw);
  // A one-line heading sits on the glyph's middle.
  place(heading_, tx, y + std::max(0, (px(40) - hh) / 2), tw, hh);
  y += std::max(px(40), hh) + px(12);
  const int bh = text_height(body_, tw);
  place(body_, tx, y, tw, bh);
  y += bh;
  if (copy_) {
    y += px(8);
    // The link's glyph lines up with the text column.
    const int lw = button_width(copy_, 0) + px(24);
    place_body(copy_, tx - px(8), y, lw, px(kControlH));
    y += px(kControlH);
  }
  y += px(24);
  (void)fm;
  footer_top_ = y;
  const int bw = button_width(done_);
  place_body(done_, w - x0 - bw, y + (px(kFooter) - px(kControlH)) / 2, bw, px(kControlH));
  return y + px(kFooter);
}

void ResultPage::paint(HDC dc) {
  const ui::Palette& p = t_.pal;
  const wchar_t* glyph = rt_.kind == Outcome::success ? L"\uE930" : rt_.kind == Outcome::partial ? L"\uE7BA" : L"\uEA39";
  const COLORREF c = rt_.kind == Outcome::success ? (p.high_contrast ? p.text : p.accent)
                     : rt_.kind == Outcome::partial ? p.caution_text
                                                    : p.critical_text;
  ui::draw_text(dc, glyph, glyph_, font(Face::glyph_large), c, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
}

void ResultPage::command(int id, int, HWND) {
  if (id == IDOK) {
    finish(IDOK);
  } else if (id == kIdCopyDetails) {
    if (OpenClipboard(hwnd_)) {
      EmptyClipboard();
      const size_t bytes = (rt_.details.size() + 1) * sizeof(wchar_t);
      if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        memcpy(GlobalLock(g), rt_.details.c_str(), bytes);
        GlobalUnlock(g);
        if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
      }
      CloseClipboard();
      SetWindowTextW(copy_, L"Copied");
      relayout();
    }
  }
}

// ==== Cover =============================================================================

CoverPage::CoverPage(Session& s, std::string id) : Page(s), id_(std::move(id)) {
  const Package* p = find_package(id_);
  title_ = p ? to_wide(p->title) : to_wide(id_);
}

CoverPage::~CoverPage() {
  cancel_ = true;
  token_.cancel();
  if (hwnd_) KillTimer(hwnd_, 2);
  wait_for_worker(worker_, action_done_, hwnd_);
  destroy_window();
}

void CoverPage::load_info(const CoverInfo* info) {
  info_ = info ? *info : cover_info(id_, s_.assets);
  tile_ = ui::Image{};
  if (!info_.tile.empty()) ui::load_image(info_.tile.wstring(), tile_);
}

void CoverPage::build() {
  load_info();
  origin_ = add_text(origin_line(info_), Face::body, Ink::text);
  choose_ = add_button(kIdChoosePicture, L"Choose a picture\u2026", ui::ButtonRole::accent);
  original_ = add_button(kIdUseOriginal, L"Use the original cover", ui::ButtonRole::standard);
  original_note_ = add_text(L"", Face::caption, Ink::text2);
  download_ = add_button(kIdDownloadCover, L"Download the original cover", ui::ButtonRole::standard);
  download_note_ = add_text(L"", Face::caption, Ink::text2);
  bar_ = make_progress(hwnd_, &t_);
  ShowWindow(bar_, SW_HIDE);
  status_ = add_text(L"", Face::body, Ink::text2);
  ShowWindow(status_, SW_HIDE);
  note_ = add_text(L"Pictures stay on this computer, in " + (win_assets_dir(s_.assets) / L"covers").wstring(), Face::caption,
                   Ink::text3);
  done_ = add_button(IDOK, L"Done", ui::ButtonRole::standard);
  update_buttons();
}

void CoverPage::update_buttons() {
  EnableWindow(choose_, !running_);
  EnableWindow(original_, !running_ && info_.has_user);
  // "Download the original cover" shows only when it can do something: a
  // download better than the current original exists and downloads are on.
  // Each of the two says what it would bring back or fetch.
  const std::wstring on = original_note(info_), dn = download_note(info_, s_.req.cover_download);
  const bool show_dl = !dn.empty();
  set_text(original_note_, on);
  set_text(download_note_, dn);
  ShowWindow(original_note_, on.empty() ? SW_HIDE : SW_SHOW);
  ShowWindow(download_note_, dn.empty() ? SW_HIDE : SW_SHOW);
  if (!show_dl && GetFocus() == download_) SetFocus(choose_);
  ShowWindow(download_, show_dl ? SW_SHOW : SW_HIDE);
  EnableWindow(download_, !running_ && show_dl);
  EnableWindow(done_, !running_);
  // (While the page is being built, open() lays it out next.)
  if (IsWindowVisible(hwnd_)) relayout();
}

int CoverPage::layout(int w, int) {
  const int x0 = px(kMargin), fm = ui::focus_margin(t_.dpi);
  int y = header_h() + px(16);
  preview_ = RECT{x0, y, x0 + px(192), y + px(240)};
  const int oh = text_height(origin_, px(192));
  place(origin_, x0, y + px(240) + px(12), px(192), oh);
  const int left_bottom = y + px(240) + px(12) + oh;
  const int rx = x0 + px(192) + px(24), rw = w - x0 - rx;
  // Shown by style (the page itself may not be showing yet).
  auto shown = [](HWND h) { return (GetWindowLongW(h, GWL_STYLE) & WS_VISIBLE) != 0; };
  int bw = px(200);
  for (HWND b : {choose_, original_, download_}) bw = std::max(bw, button_width(b));
  bw = std::min(bw, rw);
  int ry = y;
  // Each button, with the line saying what it brings under it (when it shows).
  const std::pair<HWND, HWND> column[] = {{choose_, nullptr}, {original_, original_note_}, {download_, download_note_}};
  for (auto [b, note] : column) {
    if (!shown(b)) continue;
    place_body(b, rx, ry, bw, px(kControlH));
    ry += px(kControlH) + px(8);
    if (note && shown(note)) {
      const int nh = text_height(note, rw);
      place(note, rx, ry - px(4), rw, nh);
      ry += nh + px(8);
    }
  }
  ry += px(8);
  if (IsWindowVisible(bar_)) {
    place(bar_, rx, ry, rw, px(12));
    ry += px(12) + px(8);
  }
  if (IsWindowVisible(status_)) {
    const int sh = text_height(status_, rw);
    place(status_, rx, ry, rw, sh);
    ry += sh;
  }
  (void)fm;
  // Under the preview (and everything else): where the pictures are kept.
  y = std::max(left_bottom, ry) + px(12);
  const int nw = w - 2 * x0, nh = text_height(note_, nw);
  place(note_, x0, y, nw, nh);
  y += nh + px(24);
  footer_top_ = y;
  const int dw = button_width(done_);
  place_body(done_, w - x0 - dw, y + (px(kFooter) - px(kControlH)) / 2, dw, px(kControlH));
  return y + px(kFooter);
}

void CoverPage::paint(HDC dc) { ui::draw_cover(dc, preview_, tile_.empty() ? nullptr : &tile_, title_, t_); }

void CoverPage::show_status(const std::wstring& text, Ink ink) {
  set_text(status_, text);
  set_ink(status_, ink);
  ShowWindow(status_, text.empty() ? SW_HIDE : SW_SHOW);
  relayout();
}

void CoverPage::show_running(const Progress& p) {
  running_ = true;
  update_buttons();
  ShowWindow(bar_, SW_SHOW);
  set_marquee_style(bar_, p.total == 0);
  if (p.total) SendMessageW(bar_, PBM_SETPOS, WPARAM(std::min<uint64_t>(p.done, p.total) * 1000 / p.total), 0);
  show_status(p.item.empty() ? L"Getting the cover art\u2026" : to_wide(p.item) + L"\u2026", Ink::text2);
}

void CoverPage::start(Action a, fs::path picture) {
  if (running_) return;
  running_ = true;
  cancel_ = false;
  token_.reset();   // the previous action's worker has been joined (finish_action)
  action_done_ = false;
  have_progress_ = false;
  update_buttons();
  SetFocus(hwnd_);
  ShowWindow(bar_, SW_SHOW);
  set_marquee_style(bar_, true);
  show_status(a == Action::set     ? L"Setting your picture as the cover\u2026"
              : a == Action::clear ? L"Going back to the original cover\u2026"
                                   : L"Getting the cover art\u2026",
              Ink::text2);
  CoverOptions o;
  o.assets_root = s_.req.dest;
  o.download_dir = s_.req.download_dir;
  o.allow_download = s_.req.cover_download;
  o.cancel = &token_;
  o.progress = [this](const Progress& p) {
    std::lock_guard<std::mutex> lock(m_);
    progress_ = p;
    have_progress_ = true;
    return !cancel_.load();
  };
  SetTimer(hwnd_, 2, 100, nullptr);
  worker_ = std::thread([this, a, picture, o] {
    std::vector<CoverResult> rs;
    switch (a) {
      case Action::set: rs.push_back(set_cover(id_, picture, o)); break;
      case Action::clear: rs.push_back(clear_cover(id_, o)); break;
      case Action::refresh: rs = refresh_covers({id_}, o); break;
    }
    {
      std::lock_guard<std::mutex> lock(m_);
      action_results_ = std::move(rs);
    }
    action_done_ = true;
  });
}

void CoverPage::finish_action() {
  KillTimer(hwnd_, 2);
  if (worker_.joinable()) worker_.join();
  running_ = false;
  std::vector<CoverResult> rs;
  {
    std::lock_guard<std::mutex> lock(m_);
    rs = std::move(action_results_);
  }
  bool changed = false;
  Status status = Status::ok;
  std::wstring message;
  for (const CoverResult& r : rs) {
    changed = changed || (r.status == Status::ok && r.changed);
    if (r.status != Status::ok && status == Status::ok) status = r.status;
    if (!r.message.empty()) message += (message.empty() ? L"" : L" ") + to_wide(r.message);
  }
  if (rs.empty()) status = Status::error;
  if (changed) s_.tally.cover_changed();
  if (message.empty()) {
    message = status == Status::ok ? (changed ? L"The cover was changed." : L"Nothing needed changing.")
                                   : L"The cover could not be changed.";
  }
  // What the tile shows now (the files may have changed whatever the status).
  load_info();
  set_text(origin_, origin_line(info_));
  ShowWindow(bar_, SW_HIDE);
  set_marquee_style(bar_, false);
  update_buttons();
  show_status(message, status == Status::ok ? Ink::text2 : status == Status::network ? Ink::caution : Ink::critical);
  InvalidateRect(hwnd_, &preview_, FALSE);
  HWND back = IsWindowEnabled(choose_) ? choose_ : done_;
  SetFocus(back);
}

void CoverPage::timer(UINT_PTR id) {
  if (id != 2) return;
  if (action_done_) {
    finish_action();
    return;
  }
  Progress p;
  bool have = false;
  {
    std::lock_guard<std::mutex> lock(m_);
    p = progress_;
    have = have_progress_;
  }
  if (!have) return;
  set_marquee_style(bar_, p.total == 0);
  if (p.total) SendMessageW(bar_, PBM_SETPOS, WPARAM(std::min<uint64_t>(p.done, p.total) * 1000 / p.total), 0);
  if (!p.item.empty()) set_text(status_, to_wide(p.item) + L"\u2026");
}

void CoverPage::cancel() {
  // Esc stops a download under way; otherwise it is Done.
  if (running_) {
    cancel_ = true;
    token_.cancel();
    return;
  }
  finish(IDOK);
}

void CoverPage::command(int id, int, HWND) {
  if (running_) return;
  switch (id) {
    case kIdChoosePicture: {
      auto p = pick_picture(hwnd_);
      if (p) start(Action::set, *p);
      return;
    }
    case kIdUseOriginal: start(Action::clear); return;
    case kIdDownloadCover: start(Action::refresh); return;
    case IDOK: finish(IDOK); return;
  }
}

// ==== Remove ============================================================================

RemovePage::RemovePage(Session& s, std::string id) : Page(s), id_(std::move(id)) {
  const Package* p = find_package(id_);
  title_ = p ? to_wide(p->title) : to_wide(id_);
}

RemovePage::~RemovePage() {
  if (hwnd_) KillTimer(hwnd_, 3);
  wait_for_worker(worker_, worker_done_, hwnd_);
  destroy_window();
}

void RemovePage::build() {
  for (InstalledRow& r : installed_rows(s_.assets)) {
    if (r.id != id_) continue;
    row_ = std::move(r);
    installed_ = true;
  }
  if (!installed_) {
    row_.id = id_;
    row_.title = title_;
    row_.cover = cover_info(id_, s_.assets);
  }
  if (!row_.cover.tile.empty()) ui::load_image(row_.cover.tile.wstring(), tile_);
  question_ = add_text(installed_ ? remove_question(title_) : title_, Face::subtitle, Ink::text);
  detail_ = add_text(installed_ ? row_.detail : L"", Face::caption, Ink::text2);
  text_ = add_text(installed_ ? remove_text(row_) : not_installed_text(title_), Face::body, Ink::text);
  bar_ = make_progress(hwnd_, &t_);
  ShowWindow(bar_, SW_HIDE);
  status_ = add_text(L"", Face::body, Ink::text2);
  ShowWindow(status_, SW_HIDE);
  // Remove is the accent button: the user chose "Remove…" to come here.
  remove_ = add_button(kIdRemove, L"Remove", ui::ButtonRole::accent);
  cancel_btn_ = add_button(IDCANCEL, installed_ ? L"Cancel" : L"Close", ui::ButtonRole::standard);
  if (!installed_) ShowWindow(remove_, SW_HIDE);
}

int RemovePage::layout(int w, int) {
  const int x0 = px(kMargin);
  int y = header_h() + px(16);
  // The cover at the left (96x120), everything else in a column beside it.
  art_ = RECT{x0, y, x0 + px(96), y + px(120)};
  const int tx = x0 + px(96) + px(24), tw = w - x0 - tx;
  auto shown = [](HWND h) { return (GetWindowLongW(h, GWL_STYLE) & WS_VISIBLE) != 0; };
  int ty = y;
  for (HWND h : {question_, detail_, text_}) {
    if (h == detail_ && row_.detail.empty()) continue;
    const int hh = text_height(h, tw);
    place(h, tx, ty, tw, hh);
    ty += hh + (h == question_ ? px(4) : px(12));
  }
  if (shown(bar_)) {
    place(bar_, tx, ty + px(4), tw, px(12));
    ty += px(12) + px(12);
  }
  if (shown(status_)) {
    const int sh = text_height(status_, tw);
    place(status_, tx, ty, tw, sh);
    ty += sh + px(12);
  }
  y = std::max<int>(art_.bottom + px(12), ty) + px(12);
  footer_top_ = y;
  const int bw = button_width(cancel_btn_), rw = button_width(remove_);
  const int by = y + (px(kFooter) - px(kControlH)) / 2;
  place_body(cancel_btn_, w - x0 - bw, by, bw, px(kControlH));
  place_body(remove_, w - x0 - bw - px(8) - rw, by, rw, px(kControlH));
  return y + px(kFooter);
}

void RemovePage::paint(HDC dc) { ui::draw_cover(dc, art_, tile_.empty() ? nullptr : &tile_, title_, t_); }

void RemovePage::set_status(const std::wstring& text, Ink ink) {
  set_text(status_, text);
  set_ink(status_, ink);
  ShowWindow(status_, text.empty() ? SW_HIDE : SW_SHOW);
  relayout();
}

void RemovePage::show_running() {
  running_ = true;
  EnableWindow(remove_, FALSE);
  EnableWindow(cancel_btn_, FALSE);
  ShowWindow(bar_, SW_SHOW);
  set_marquee_style(bar_, true);
  set_status(L"Removing " + title_ + L"…", Ink::text2);
}

void RemovePage::show_failure(const std::string& message) {
  running_ = false;
  ShowWindow(bar_, SW_HIDE);
  set_marquee_style(bar_, false);
  EnableWindow(remove_, TRUE);
  EnableWindow(cancel_btn_, TRUE);
  SetWindowTextW(remove_, L"Try again");
  set_status(remove_failed_text(message), Ink::critical);
}

void RemovePage::start() {
  if (running_ || !installed_) return;
  worker_done_ = false;
  show_running();
  SetFocus(hwnd_);
  SetTimer(hwnd_, 3, 100, nullptr);
  const fs::path dest = s_.req.dest;
  const std::string id = id_;
  worker_ = std::thread([this, dest, id] {
    RemoveResult r = remove_package(id, dest);
    {
      std::lock_guard<std::mutex> lock(m_);
      result_ = std::move(r);
    }
    worker_done_ = true;
  });
}

void RemovePage::finish_remove() {
  KillTimer(hwnd_, 3);
  if (worker_.joinable()) worker_.join();
  RemoveResult r;
  {
    std::lock_guard<std::mutex> lock(m_);
    r = result_;
  }
  if (r.status == Status::ok) {
    removed_ = true;
    s_.tally.release_removed();
    finish(IDOK);
    return;
  }
  s_.tally.import_result(r.status);
  show_failure(r.message);
  SetFocus(remove_);
}

void RemovePage::timer(UINT_PTR id) {
  if (id == 3 && worker_done_) finish_remove();
}

void RemovePage::cancel() {
  // Not while the folder is being moved aside: that takes a moment, and
  // stopping it half-way is what the importer's recovery is for, not Esc.
  if (running_) return;
  finish(IDCANCEL);
}

void RemovePage::command(int id, int, HWND) {
  if (running_) return;
  if (id == kIdRemove) start();
  else if (id == IDCANCEL) finish(IDCANCEL);
}

}  // namespace adw::import::gui
