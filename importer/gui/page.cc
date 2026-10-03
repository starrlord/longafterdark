#include "page.h"

#include <windowsx.h>
#include <commctrl.h>

#include <algorithm>
#include <cmath>

#include "adw/ui/capture.h"

namespace adw::import::gui {

namespace {

constexpr wchar_t kPageClass[] = L"AdwImportPage";
constexpr wchar_t kPanelClass[] = L"AdwImportPanel";

HINSTANCE instance() { return GetModuleHandleW(nullptr); }

void register_classes() {
  static bool done = false;
  if (done) return;
  done = true;
  // Plain classes: each window's procedure is set right after it is created,
  // once it knows its Page or ScrollPanel (GWLP_USERDATA).
  WNDCLASSEXW wc{sizeof(wc)};
  wc.style = CS_DBLCLKS;
  wc.hInstance = instance();
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpfnWndProc = DefWindowProcW;
  wc.lpszClassName = kPageClass;
  RegisterClassExW(&wc);
  wc.lpszClassName = kPanelClass;
  RegisterClassExW(&wc);
}

std::wstring window_text(HWND h) {
  int n = GetWindowTextLengthW(h);
  std::wstring s(n + 1, L'\0');
  GetWindowTextW(h, s.data(), n + 1);
  s.resize(n);
  return s;
}

// Paints into a memory bitmap, then copies: no flicker.
struct Buffered {
  HDC target, dc = nullptr;
  HBITMAP bmp = nullptr;
  HGDIOBJ old = nullptr;
  int w, h;
  Buffered(HDC t, int width, int height) : target(t), w(std::max(1, width)), h(std::max(1, height)) {
    dc = CreateCompatibleDC(t);
    bmp = CreateCompatibleBitmap(t, w, h);
    old = SelectObject(dc, bmp);
  }
  ~Buffered() {
    BitBlt(target, 0, 0, w, h, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
  }
};

}  // namespace

// ---- the app mark as an icon ----------------------------------------------------------

HICON make_mark_icon(int size) {
  // GDI and GDI+ over an HDC leave alpha alone, so the mark is drawn over
  // black and over white and its alpha read from the difference.
  auto render = [&](COLORREF bg, std::vector<uint8_t>& out) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    HGDIOBJ old = SelectObject(mem, dib);
    RECT r{0, 0, size, size};
    ui::fill_rect(mem, r, bg);
    ui::draw_app_mark(mem, r);
    GdiFlush();
    out.assign(static_cast<uint8_t*>(bits), static_cast<uint8_t*>(bits) + (size_t)size * size * 4);
    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
  };
  std::vector<uint8_t> on_black, on_white;
  render(RGB(0, 0, 0), on_black);
  render(RGB(255, 255, 255), on_white);
  BITMAPV5HEADER bh{};
  bh.bV5Size = sizeof(bh);
  bh.bV5Width = size;
  bh.bV5Height = -size;
  bh.bV5Planes = 1;
  bh.bV5BitCount = 32;
  bh.bV5Compression = BI_BITFIELDS;
  bh.bV5RedMask = 0x00FF0000;
  bh.bV5GreenMask = 0x0000FF00;
  bh.bV5BlueMask = 0x000000FF;
  bh.bV5AlphaMask = 0xFF000000;
  void* bits = nullptr;
  HDC screen = GetDC(nullptr);
  HBITMAP color = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&bh), DIB_RGB_COLORS, &bits, nullptr, 0);
  ReleaseDC(nullptr, screen);
  if (!color) return nullptr;
  auto* px = static_cast<uint8_t*>(bits);
  for (size_t i = 0; i < (size_t)size * size; ++i) {
    const uint8_t* b = &on_black[i * 4];
    const uint8_t* w = &on_white[i * 4];
    const int a = std::clamp(255 - (int)(w[1] - b[1]), 0, 255);
    uint8_t* d = &px[i * 4];
    // Straight (not premultiplied) colour for the icon: black-backed colour / alpha.
    for (int c = 0; c < 3; ++c) d[c] = a ? (uint8_t)std::min(255, b[c] * 255 / a) : 0;
    d[3] = (uint8_t)a;
  }
  HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
  ICONINFO ii{TRUE, 0, 0, mask, color};
  HICON icon = CreateIconIndirect(&ii);
  DeleteObject(mask);
  DeleteObject(color);
  return icon;
}

// ---- Page -----------------------------------------------------------------------------

Page::Page(Session& s) : s_(s) {}

Page::~Page() {
  destroy_window();
  if (glyph_font_) DeleteObject(glyph_font_);
  if (caution_brush_) DeleteObject(caution_brush_);
  if (critical_brush_) DeleteObject(critical_brush_);
}

void Page::destroy_window() {
  if (!hwnd_) return;
  HWND h = hwnd_;
  ui::forget_looks(h);
  SetWindowLongPtrW(h, GWLP_USERDATA, 0);
  for (ScrollPanel* p : panels_)
    if (p->hwnd()) SetWindowLongPtrW(p->hwnd(), GWLP_USERDATA, 0);
  hwnd_ = nullptr;
  DestroyWindow(h);
}

void Page::finish(int outcome) {
  outcome_ = outcome;
  done_ = true;
  // Wake the loop in run() (GetMessage) when this came from another thread's post.
  if (hwnd_) PostMessageW(hwnd_, WM_NULL, 0, 0);
}

RECT Page::work_area() const {
  if (s_.offscreen) {
    const int w = s_.work_w ? px(s_.work_w) : 100000, h = s_.work_h ? px(s_.work_h) : 100000;
    return RECT{0, 0, w, h};
  }
  MONITORINFO mi{sizeof(mi)};
  GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
  return mi.rcWork;
}

bool Page::open(const Page* prev) {
  register_classes();
  t_.load(s_.theme_mode);
  POINT at{CW_USEDEFAULT, CW_USEDEFAULT};
  if (prev && prev->hwnd_) {
    RECT r{};
    GetWindowRect(prev->hwnd_, &r);
    at = POINT{r.left, r.top};
  } else {
    // The monitor under the pointer: where the .scr's Import… button was clicked.
    POINT cursor{};
    GetCursorPos(&cursor);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &mi);
    at = POINT{mi.rcWork.left + 64, mi.rcWork.top + 64};
  }
  if (s_.offscreen) at = POINT{-32000, -32000};
  // A dialog's frame: the close box only (a long download may be minimized).
  const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN | (minimizable() ? WS_MINIMIZEBOX : 0);
  const DWORD ex = WS_EX_CONTROLPARENT | WS_EX_APPWINDOW;
  hwnd_ = CreateWindowExW(ex, kPageClass, kTitle, style, at.x, at.y, 400, 300, nullptr, nullptr, instance(), nullptr);
  if (!hwnd_) return false;
  SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  SetWindowLongPtrW(hwnd_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&Page::proc));
  const int dpi = s_.forced_dpi ? s_.forced_dpi : (int)GetDpiForWindow(hwnd_);
  t_.set_dpi(dpi);
  // The window's icon: the moon mark, for the taskbar and Alt+Tab.
  const UINT wdpi = GetDpiForWindow(hwnd_);
  if (s_.icon_dpi != (int)wdpi) {
    if (s_.icon_big) DestroyIcon(s_.icon_big);
    if (s_.icon_small) DestroyIcon(s_.icon_small);
    s_.icon_big = make_mark_icon(GetSystemMetricsForDpi(SM_CXICON, wdpi));
    s_.icon_small = make_mark_icon(GetSystemMetricsForDpi(SM_CXSMICON, wdpi));
    s_.icon_dpi = (int)wdpi;
  }
  SendMessageW(hwnd_, WM_SETICON, ICON_BIG, (LPARAM)s_.icon_big);
  SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, (LPARAM)s_.icon_small);
  build();
  apply_fonts();
  ui::apply_window_chrome(hwnd_, t_.pal);
  ui::allow_dark_menus(t_.pal.dark && !t_.pal.high_contrast);
  for (ScrollPanel* p : panels_) ui::theme_native_control(p->hwnd(), t_.pal.dark && !t_.pal.high_contrast);
  fit(prev != nullptr);
  // As after a mouse click: focus rects and mnemonics appear once the keyboard is used.
  SendMessageW(hwnd_, WM_CHANGEUISTATE, MAKEWPARAM(UIS_SET, UISF_HIDEFOCUS | UISF_HIDEACCEL), 0);
  if (s_.offscreen) {
    ui::park_offscreen(hwnd_);
  } else {
    // SWP_SHOWWINDOW rather than ShowWindow: a launcher's STARTUPINFO show
    // state (a hidden console's SW_HIDE) must not hide the first window.
    SetWindowPos(hwnd_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(hwnd_);
  }
  if (HWND f = item(initial_focus())) {
    last_focus_ = f;
    if (!s_.offscreen) SetFocus(f);
  }
  if (!s_.offscreen) UpdateWindow(hwnd_);
  return true;
}

int Page::run() {
  MSG m;
  while (!done_) {
    BOOL r = GetMessageW(&m, nullptr, 0, 0);
    if (r == 0) {
      PostQuitMessage((int)m.wParam);
      break;
    }
    if (r < 0) break;
    if (!hwnd_ || !IsDialogMessageW(hwnd_, &m)) {
      TranslateMessage(&m);
      DispatchMessageW(&m);
    }
    keep_focus_visible();
  }
  return outcome_;
}

void Page::keep_focus_visible() {
  HWND f = GetFocus();
  if (!f || f == last_focus_) return;
  if (hwnd_ && IsChild(hwnd_, f)) {
    last_focus_ = f;
    // Inner panels first (they were made after the ones holding them), so
    // an outer one scrolls to where the control ends up.
    for (auto it = panels_.rbegin(); it != panels_.rend(); ++it)
      if ((*it)->contains(f)) (*it)->ensure_visible(f);
  }
}

void Page::show_focus_on(int id) {
  HWND f = item(id);
  if (!f) return;
  SendMessageW(hwnd_, WM_CHANGEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS | UISF_HIDEACCEL), 0);
  ui::set_focus_override(f);
  for (auto it = panels_.rbegin(); it != panels_.rend(); ++it)
    if ((*it)->contains(f)) (*it)->ensure_visible(f);
  RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

POINT Page::base_probe() const { return POINT{px(4), header_h() + px(4)}; }

void Page::fit(bool keep_pos) {
  const DWORD style = (DWORD)GetWindowLongW(hwnd_, GWL_STYLE), ex = (DWORD)GetWindowLongW(hwnd_, GWL_EXSTYLE);
  RECT fr{0, 0, 0, 0};
  AdjustWindowRectExForDpi(&fr, style, FALSE, ex, GetDpiForWindow(hwnd_));
  const int frame_w = fr.right - fr.left, frame_h = fr.bottom - fr.top;
  const RECT wa = work_area();
  const int max_cw = std::max(px(320), (int)(wa.right - wa.left) - frame_w);
  const int max_ch = std::max(px(240), (int)(wa.bottom - wa.top) - frame_h);
  // COVERS.md §4.2: 816 DIP wide (seven covers to a row on Sources: the twenty releases on
  // three rows, every one showing at once on a 1080-line screen), 560 at the least (a narrow
  // work area wins over both).
  int cw = std::min(px(816), max_cw);
  const int ch = std::min(layout(cw, max_ch), max_ch);
  client_w_ = cw;
  client_h_ = ch;
  const int W = cw + frame_w, H = ch + frame_h;
  RECT cur{};
  GetWindowRect(hwnd_, &cur);
  int x = cur.left, y = cur.top;
  if (!s_.offscreen) {
    if (!keep_pos) {
      x = wa.left + ((wa.right - wa.left) - W) / 2;
      y = wa.top + ((wa.bottom - wa.top) - H) / 2;
    }
    // Wholly on the work area of the monitor it is on.
    x = std::clamp(x, (int)wa.left, std::max((int)wa.left, (int)wa.right - W));
    y = std::clamp(y, (int)wa.top, std::max((int)wa.top, (int)wa.bottom - H));
  }
  SetWindowPos(hwnd_, nullptr, x, y, W, H, SWP_NOZORDER | SWP_NOACTIVATE);
  // The system can still refuse a window taller than its maximum tracking
  // size (the virtual screen): lay the page out again for the client it got,
  // so the footer stays in view and the body scrolls instead.
  RECT got{};
  GetClientRect(hwnd_, &got);
  if (got.bottom > 0 && got.bottom < ch) client_h_ = std::min(layout(cw, (int)got.bottom), (int)got.bottom);
  RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

void Page::relayout() {
  if (!hwnd_) return;
  fit(true);
}

void Page::apply_fonts() {
  for (auto& [h, look] : texts_) SendMessageW(h, WM_SETFONT, (WPARAM)font(look.face), FALSE);
  for (HWND b : buttons_) SendMessageW(b, WM_SETFONT, (WPARAM)t_.fonts.body, FALSE);
}

void Page::apply_theme() {
  t_.load(s_.theme_mode);
  if (caution_brush_) DeleteObject(caution_brush_);
  if (critical_brush_) DeleteObject(critical_brush_);
  caution_brush_ = critical_brush_ = nullptr;
  ui::apply_window_chrome(hwnd_, t_.pal);
  ui::allow_dark_menus(t_.pal.dark && !t_.pal.high_contrast);
  for (ScrollPanel* p : panels_) ui::theme_native_control(p->hwnd(), t_.pal.dark && !t_.pal.high_contrast);
  theme_changed();
  RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

void Page::apply_dpi(int dpi) {
  t_.set_dpi(dpi);
  apply_fonts();
}

HFONT Page::font(Face f) const {
  switch (f) {
    case Face::caption: return t_.fonts.caption;
    case Face::body: return t_.fonts.body;
    case Face::body_strong: return t_.fonts.body_strong;
    case Face::subtitle: return t_.fonts.subtitle;
    case Face::glyph_large: {
      // Segoe Fluent Icons at 32 DIP (the result page's glyph), made on demand per DPI.
      auto* self = const_cast<Page*>(this);
      if (!glyph_font_ || glyph_font_dpi_ != t_.dpi) {
        if (glyph_font_) DeleteObject(glyph_font_);
        self->glyph_font_ = CreateFontW(-px(32), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                                        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                                        t_.icons_font_is_fluent ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets");
        self->glyph_font_dpi_ = t_.dpi;
      }
      return glyph_font_;
    }
  }
  return t_.fonts.body;
}

COLORREF Page::ink(Ink i) const {
  const ui::Palette& p = t_.pal;
  switch (i) {
    case Ink::text: return p.text;
    case Ink::text2: return p.text2;
    case Ink::text3: return p.text3;
    case Ink::accent: return p.high_contrast ? p.text : p.accent_text;
    case Ink::caution: return p.caution_text;
    case Ink::critical: return p.critical_text;
  }
  return p.text;
}

COLORREF Page::bg_color(Bg b) const {
  switch (b) {
    case Bg::base: return t_.pal.base;
    case Bg::card: return t_.pal.card;
    case Bg::caution: return t_.pal.caution;
    case Bg::critical: return t_.pal.critical;
  }
  return t_.pal.base;
}

HWND Page::add_text(const std::wstring& text, Face face, Ink ink_, HWND parent, bool single, Bg bg, DWORD extra) {
  // SS_EDITCONTROL: a path or URL longer than the line breaks within itself
  // instead of running past the edge.
  DWORD style = WS_CHILD | WS_VISIBLE | SS_NOPREFIX | (single ? SS_LEFTNOWORDWRAP : SS_LEFT | SS_EDITCONTROL) | extra;
  if (single && !(extra & SS_ELLIPSISMASK)) style |= SS_ENDELLIPSIS;
  HWND h = CreateWindowExW(0, WC_STATICW, text.c_str(), style, 0, 0, 10, 10, parent ? parent : hwnd_, nullptr,
                           instance(), nullptr);
  texts_[h] = TextLook{face, ink_, bg};
  SendMessageW(h, WM_SETFONT, (WPARAM)font(face), FALSE);
  return h;
}

void Page::set_text(HWND h, const std::wstring& text) {
  if (window_text(h) != text) SetWindowTextW(h, text.c_str());
  InvalidateRect(h, nullptr, TRUE);
}

void Page::set_ink(HWND h, Ink i) {
  auto it = texts_.find(h);
  if (it != texts_.end() && it->second.ink != i) {
    it->second.ink = i;
    InvalidateRect(h, nullptr, TRUE);
  }
}

void Page::set_bg(HWND h, Bg b) {
  auto it = texts_.find(h);
  if (it != texts_.end() && it->second.bg != b) {
    it->second.bg = b;
    InvalidateRect(h, nullptr, TRUE);
  }
}

HWND Page::add_button(int id, const std::wstring& text, ui::ButtonRole role, wchar_t glyph, HWND parent,
                      ui::Surface surface) {
  HWND h = CreateWindowExW(0, WC_BUTTONW, text.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_MULTILINE,
                           0, 0, 10, 10, parent ? parent : hwnd_, (HMENU)(INT_PTR)id, instance(), nullptr);
  ui::set_button_role(h, role, glyph);
  ui::set_surface(h, surface);
  SendMessageW(h, WM_SETFONT, (WPARAM)t_.fonts.body, FALSE);
  buttons_.push_back(h);
  return h;
}

int Page::button_width(HWND h, int min_dips) const {
  HDC dc = GetDC(h);
  SIZE s = ui::measure_text(dc, window_text(h), t_.fonts.body, DT_SINGLELINE);
  ReleaseDC(h, dc);
  return std::max(px(min_dips), (int)s.cx + px(24));
}

void Page::place_body(HWND h, int x, int y, int w, int hgt) const {
  const int fm = ui::focus_margin(t_.dpi);
  SetWindowPos(h, nullptr, x - fm, y - fm, w + 2 * fm, hgt + 2 * fm, SWP_NOZORDER | SWP_NOACTIVATE);
}

void Page::place(HWND h, int x, int y, int w, int hgt) const {
  SetWindowPos(h, nullptr, x, y, w, hgt, SWP_NOZORDER | SWP_NOACTIVATE);
}

int Page::text_height(HWND h, int w) const {
  auto it = texts_.find(h);
  HFONT f = it != texts_.end() ? font(it->second.face) : t_.fonts.body;
  const bool single = (GetWindowLongW(h, GWL_STYLE) & SS_TYPEMASK) == SS_LEFTNOWORDWRAP;
  HDC dc = GetDC(h);
  HGDIOBJ old = SelectObject(dc, f);
  std::wstring text = window_text(h);
  RECT r{0, 0, std::max(1, w), 0};
  if (text.empty()) text = L" ";
  DrawTextW(dc, text.c_str(), (int)text.size(), &r,
            DT_CALCRECT | DT_NOPREFIX | (single ? DT_SINGLELINE : DT_WORDBREAK | DT_EDITCONTROL));
  SelectObject(dc, old);
  ReleaseDC(h, dc);
  return r.bottom - r.top;
}

HWND Page::item(int id) const {
  if (!hwnd_ || !id) return nullptr;
  if (HWND h = GetDlgItem(hwnd_, id)) return h;
  for (ScrollPanel* p : panels_)
    if (HWND h = GetDlgItem(p->hwnd(), id)) return h;
  return nullptr;
}

void Page::paint_footer(HDC dc, int top) const {
  RECT line{0, top, client_w_, top + t_.hairline()};
  ui::fill_rect(dc, line, t_.pal.divider);
}

void Page::paint_info_box(HDC dc, HWND text, Bg bg, HWND on) const {
  if (!text || !IsWindowVisible(text)) return;
  RECT r{};
  GetWindowRect(text, &r);
  MapWindowPoints(nullptr, on ? on : hwnd_, reinterpret_cast<POINT*>(&r), 2);
  // The glyph column at the left and 12 DIP around the text.
  RECT box{r.left - px(40), r.top - px(12), r.right + px(12), r.bottom + px(12)};
  const ui::Palette& p = t_.pal;
  ui::fill_round(dc, box, t_.pxf(4), bg_color(bg));
  ui::stroke_round(dc, box, t_.pxf(4), p.high_contrast ? p.text : p.card_stroke, (float)t_.hairline());
  RECT glyph{box.left + px(12), r.top, box.left + px(32), r.top + px(20)};
  ui::draw_text(dc, bg == Bg::critical ? L"\uEA39" : L"\uE7BA", glyph, t_.fonts.icons,
                bg == Bg::critical ? p.critical_text : p.caution_text, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
}

// ---- messages ---------------------------------------------------------------------------

LRESULT CALLBACK Page::proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  auto* self = reinterpret_cast<Page*>(GetWindowLongPtrW(h, GWLP_USERDATA));
  if (!self || self->hwnd_ != h) return DefWindowProcW(h, msg, wp, lp);
  return self->handle(msg, wp, lp);
}

void Page::route_command(int id, int code, HWND ctl) {
  if (id == IDCANCEL) {
    // Esc, the close box, and a click on Cancel all come here. A greyed
    // Cancel means "not now" (the worker is still stopping).
    HWND c = item(IDCANCEL);
    if (c && !IsWindowEnabled(c)) return;
    if (!c || !IsWindowVisible(c)) {
      cancel();
      return;
    }
  }
  if (ctl) {
    if (!IsWindowEnabled(ctl)) return;
  } else if (id != IDCANCEL && id != IDOK && !menu_command(id)) {
    return;
  }
  command(id, code, ctl);
}

LRESULT Page::handle(UINT msg, WPARAM wp, LPARAM lp) {
  if (ui::is_theme_change(msg, wp, lp)) {
    // Light, dark and high contrast follow the system live (a forced mode, the
    // screenshot hook's, reloads as itself).
    apply_theme();
    return DefWindowProcW(hwnd_, msg, wp, lp);
  }
  switch (msg) {
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC target = BeginPaint(hwnd_, &ps);
      RECT cr{};
      GetClientRect(hwnd_, &cr);
      {
        Buffered b(target, cr.right, cr.bottom);
        ui::fill_rect(b.dc, cr, t_.pal.base);
        RECT band{0, 0, cr.right, header_h()};
        RECT logo{px(kMargin), px(8), px(kMargin) + px(32), px(40)};
        RECT title{px(kMargin) + px(44), px(8), cr.right - px(kMargin), px(40)};
        ui::paint_header(b.dc, band, logo, title, nullptr, header_name(), header_tagline(), t_);
        paint(b.dc);
        if (footer_top_ > 0) paint_footer(b.dc, footer_top_);
      }
      EndPaint(hwnd_, &ps);
      return 0;
    }
    case WM_PRINTCLIENT: {
      RECT cr{};
      GetClientRect(hwnd_, &cr);
      HDC dc = (HDC)wp;
      ui::fill_rect(dc, cr, t_.pal.base);
      RECT band{0, 0, cr.right, header_h()};
      RECT logo{px(kMargin), px(8), px(kMargin) + px(32), px(40)};
      RECT title{px(kMargin) + px(44), px(8), cr.right - px(kMargin), px(40)};
      ui::paint_header(dc, band, logo, title, nullptr, header_name(), header_tagline(), t_);
      paint(dc);
      if (footer_top_ > 0) paint_footer(dc, footer_top_);
      return 0;
    }
    case WM_CTLCOLORSTATIC: {
      HWND ctl = (HWND)lp;
      HDC dc = (HDC)wp;
      auto it = texts_.find(ctl);
      Bg bg = it != texts_.end() ? it->second.bg : Bg::base;
      SetTextColor(dc, it != texts_.end() ? ink(it->second.ink) : t_.pal.text);
      SetBkColor(dc, bg_color(bg));
      SetBkMode(dc, OPAQUE);
      switch (bg) {
        case Bg::card: return (LRESULT)t_.card_brush;
        case Bg::caution:
          if (!caution_brush_) caution_brush_ = CreateSolidBrush(t_.pal.caution);
          return (LRESULT)caution_brush_;
        case Bg::critical:
          if (!critical_brush_) critical_brush_ = CreateSolidBrush(t_.pal.critical);
          return (LRESULT)critical_brush_;
        case Bg::base: break;
      }
      return (LRESULT)t_.base_brush;
    }
    case WM_CTLCOLORBTN:
      return (LRESULT)t_.base_brush;
    case WM_NOTIFY: {
      auto* nm = reinterpret_cast<NMHDR*>(lp);
      if (nm->code == NM_CUSTOMDRAW) {
        wchar_t cls[32] = {};
        GetClassNameW(nm->hwndFrom, cls, 32);
        if (wcscmp(cls, WC_BUTTONW) == 0) {
          LRESULT r = 0;
          if (draw_button(reinterpret_cast<NMCUSTOMDRAW*>(lp), &r)) return r;
          return ui::custom_draw_button(t_, reinterpret_cast<NMCUSTOMDRAW*>(lp));
        }
      }
      break;
    }
    case WM_CONTEXTMENU:
      // From a control anywhere in the window (a child's DefWindowProc passes it up).
      if (context_menu((HWND)wp, POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)})) return 0;
      break;
    case WM_COMMAND: {
      const int id = LOWORD(wp), code = HIWORD(wp);
      HWND ctl = (HWND)lp;
      if (ctl && code != BN_CLICKED) break;
      route_command(id, code, ctl);
      return 0;
    }
    case kClickButton: {
      // As a task dialog: press that button (when it is there and enabled).
      const int id = (int)wp;
      HWND c = item(id);
      if (c && IsWindowVisible(c) && IsWindowEnabled(c)) {
        PostMessageW(hwnd_, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), (LPARAM)c);
      } else if (!c && (id == IDCANCEL || id == IDOK || menu_command(id))) {
        PostMessageW(hwnd_, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), 0);
      }
      return 0;
    }
    case DM_GETDEFID:
      return default_id() ? MAKELRESULT(default_id(), DC_HASDEFID) : 0;
    case WM_CLOSE:
      route_command(IDCANCEL, BN_CLICKED, nullptr);
      return 0;
    case WM_TIMER:
      timer((UINT_PTR)wp);
      return 0;
    case WM_ACTIVATE:
      if (LOWORD(wp) == WA_INACTIVE) {
        HWND f = GetFocus();
        if (f && IsChild(hwnd_, f)) last_focus_ = f;
      } else if (last_focus_ && IsWindow(last_focus_) && IsWindowEnabled(last_focus_) && IsWindowVisible(last_focus_)) {
        SetFocus(last_focus_);
        return 0;
      }
      break;
    case WM_SETFOCUS:
      if (last_focus_ && IsWindow(last_focus_) && IsWindowEnabled(last_focus_)) SetFocus(last_focus_);
      return 0;
    case WM_GETMINMAXINFO:
      // Off screen (the screenshot hook) the page gets the size it asks for,
      // as on a monitor large enough for it, whatever this machine's screens are.
      if (s_.offscreen) {
        auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
        mm->ptMaxTrackSize = POINT{32000, 32000};
        return 0;
      }
      break;
    case WM_DPICHANGED: {
      if (s_.forced_dpi) break;
      apply_dpi(HIWORD(wp));
      auto* r = reinterpret_cast<RECT*>(lp);
      SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      fit(true);
      return 0;
    }
  }
  return DefWindowProcW(hwnd_, msg, wp, lp);
}

// ---- ScrollPanel ----------------------------------------------------------------------

ScrollPanel::ScrollPanel(Page& page, const ui::Theme* t, ui::Surface surface, HWND parent) : t_(t) {
  register_classes();
  hwnd_ = CreateWindowExW(WS_EX_CONTROLPARENT, kPanelClass, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPCHILDREN,
                          0, 0, 10, 10, parent ? parent : page.hwnd_, nullptr, instance(), nullptr);
  SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  SetWindowLongPtrW(hwnd_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&ScrollPanel::proc));
  ui::set_surface(hwnd_, surface);
  ui::attach_overlay_scrollbar(hwnd_, t);
  page.add_panel(this);
}

ScrollPanel::~ScrollPanel() {
  if (hwnd_ && IsWindow(hwnd_)) SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
}

void ScrollPanel::put(HWND h, int x, int y, int w, int hgt) {
  for (Item& it : items_) {
    if (it.h == h) {
      it.r = RECT{x, y, x + w, y + hgt};
      SetWindowPos(h, nullptr, x, y - scroll_, w, hgt, SWP_NOZORDER | SWP_NOACTIVATE);
      return;
    }
  }
  items_.push_back(Item{h, RECT{x, y, x + w, y + hgt}});
  SetWindowPos(h, nullptr, x, y - scroll_, w, hgt, SWP_NOZORDER | SWP_NOACTIVATE);
}

void ScrollPanel::place(int x, int y, int w, int hgt, int content_h) {
  view_w_ = w;
  view_h_ = hgt;
  content_h_ = content_h;
  scroll_ = std::clamp(scroll_, 0, std::max(0, content_h_ - view_h_));
  ui::place_with_overlay_scrollbar(hwnd_, x, y, w, hgt);
  update_bar();
  reposition();
  RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

void ScrollPanel::set_view(int w, int hgt, int content_h) {
  RECT r{};
  GetWindowRect(hwnd_, &r);
  MapWindowPoints(nullptr, GetParent(hwnd_), reinterpret_cast<POINT*>(&r), 2);
  place(r.left, r.top, w, hgt, content_h);
}

int ScrollPanel::window_width(int w) const {
  return w + GetSystemMetricsForDpi(SM_CXVSCROLL, GetDpiForWindow(hwnd_));
}

void ScrollPanel::update_bar() {
  SCROLLINFO si{sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS};
  si.nMin = 0;
  si.nMax = std::max(0, content_h_ - 1);
  si.nPage = (UINT)std::max(0, view_h_);
  si.nPos = scroll_;
  SetScrollInfo(hwnd_, SB_VERT, &si, TRUE);
}

void ScrollPanel::reposition() {
  HDWP d = BeginDeferWindowPos((int)items_.size());
  for (const Item& it : items_) {
    if (!d) break;
    d = DeferWindowPos(d, it.h, nullptr, it.r.left, it.r.top - scroll_, it.r.right - it.r.left, it.r.bottom - it.r.top,
                       SWP_NOZORDER | SWP_NOACTIVATE);
  }
  if (d) EndDeferWindowPos(d);
}

void ScrollPanel::scroll_to(int y) {
  y = std::clamp(y, 0, std::max(0, content_h_ - view_h_));
  if (y == scroll_) return;
  scroll_ = y;
  update_bar();
  reposition();
  RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

void ScrollPanel::ensure_visible(HWND child) {
  for (const Item& it : items_) {
    if (it.h != child && !IsChild(it.h, child)) continue;
    RECT r = it.r;
    if (it.h != child) {
      // A control inside a panel of this one (Sources' installed list when
      // the whole body scrolls): where it is now within that item.
      RECT outer{}, inner{};
      GetWindowRect(it.h, &outer);
      GetWindowRect(child, &inner);
      r.top = it.r.top + (inner.top - outer.top);
      r.bottom = r.top + (inner.bottom - inner.top);
    }
    const int m = t_->px(8);
    if (r.top - m < scroll_) scroll_to(r.top - m);
    else if (r.bottom + m > scroll_ + view_h_) scroll_to(r.bottom + m - view_h_);
    return;
  }
}

bool ScrollPanel::contains(HWND h) const { return h && hwnd_ && IsChild(hwnd_, h); }

LRESULT CALLBACK ScrollPanel::proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  auto* self = reinterpret_cast<ScrollPanel*>(GetWindowLongPtrW(h, GWLP_USERDATA));
  if (!self || self->hwnd_ != h) return DefWindowProcW(h, msg, wp, lp);
  return self->handle(msg, wp, lp);
}

LRESULT ScrollPanel::handle(UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC target = BeginPaint(hwnd_, &ps);
      RECT cr{};
      GetClientRect(hwnd_, &cr);
      {
        Buffered b(target, cr.right, cr.bottom);
        ui::fill_rect(b.dc, cr, ui::surface_color(hwnd_, t_->pal));
        if (painter) painter(b.dc, -scroll_);
      }
      EndPaint(hwnd_, &ps);
      return 0;
    }
    case WM_PRINTCLIENT: {
      RECT cr{};
      GetClientRect(hwnd_, &cr);
      ui::fill_rect((HDC)wp, cr, ui::surface_color(hwnd_, t_->pal));
      if (painter) painter((HDC)wp, -scroll_);
      return 0;
    }
    case WM_VSCROLL: {
      const int line = t_->px(40), page = std::max(line, view_h_ * 9 / 10);
      switch (LOWORD(wp)) {
        case SB_LINEUP: scroll_to(scroll_ - line); break;
        case SB_LINEDOWN: scroll_to(scroll_ + line); break;
        case SB_PAGEUP: scroll_to(scroll_ - page); break;
        case SB_PAGEDOWN: scroll_to(scroll_ + page); break;
        case SB_TOP: scroll_to(0); break;
        case SB_BOTTOM: scroll_to(content_h_); break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: scroll_to((int)(short)HIWORD(wp) >= 0 ? (int)HIWORD(wp) : 0); break;
      }
      return 0;
    }
    case WM_MOUSEWHEEL: {
      const int delta = GET_WHEEL_DELTA_WPARAM(wp);
      scroll_to(scroll_ - delta * t_->px(48) / WHEEL_DELTA);
      return 0;
    }
    case WM_COMMAND:
    case WM_NOTIFY:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
      // The page handles its controls wherever they sit.
      return SendMessageW(GetParent(hwnd_), msg, wp, lp);
  }
  return DefWindowProcW(hwnd_, msg, wp, lp);
}

}  // namespace adw::import::gui
