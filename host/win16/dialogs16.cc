// Real dialogs for Win16 modules in configure mode (dialogs16.hh).
#include "win16/dialogs16.hh"

#include <windows.h>
#include <commdlg.h>

#include <algorithm>
#include <cstring>
#include <exception>
#include <functional>
#include <map>
#include <optional>
#include <set>

#include "adw/core/log.h"
#include "adw/core/text.h"
#include "win16/dos16.hh"
#include "win16/gdi16.hh"
#include "win16/modules16.hh"
#include "win16/runtime16.hh"
#include "win16/shim_families16.hh"
#include "win32/config_script.hh"
#include "win32/display.hh"
#include "win32/vfs.hh"

namespace adw::win16 {

namespace {

// The settings window disables itself for a whole configure run
// (INTERACTION.md §6.3), so a modal dialog, message box or file dialog it
// owns would find its owner already disabled: the modal loop then neither
// disables nor re-enables it, and the owner is still disabled when the dialog
// goes. Lent: enabled just before the modal UI, which disables it again at
// once and re-enables it as it ends, as for any owner. (It stays enabled for
// the few moments the host has left; the settings window re-enables itself
// when the host exits anyway. adhostwin_main.cc hands it the foreground.)
struct OwnerLend {
  explicit OwnerLend(HWND o) {
    if (o && IsWindow(o) && !IsWindowEnabled(o)) EnableWindow(o, TRUE);
  }
  OwnerLend(const OwnerLend&) = delete;
  OwnerLend& operator=(const OwnerLend&) = delete;
};


// ---- text (the guest's strings are code page 1252) ----------------------------------------------------------

std::wstring w1252(std::string_view s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(1252, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(std::max(n, 0)), L'\0');
  if (n > 0) MultiByteToWideChar(1252, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}

std::string a1252(std::wstring_view w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(1252, 0, w.data(), int(w.size()), nullptr, 0, "?", nullptr);
  std::string s(size_t(std::max(n, 0)), '\0');
  if (n > 0) WideCharToMultiByte(1252, 0, w.data(), int(w.size()), s.data(), n, "?", nullptr);
  return s;
}

std::string upper(std::string_view s) {
  std::string u(s);
  for (char& c : u) c = char(toupper(uint8_t(c)));
  return u;
}

std::wstring class_of(HWND h) {
  wchar_t buf[64] = {};
  GetClassNameW(h, buf, 64);
  return buf;
}

// ---- the template converter's byte readers ------------------------------------------------------------------

struct Reader {
  std::string_view d;
  size_t p = 0;
  bool bad = false;
  uint8_t u8() {
    if (p + 1 > d.size()) {
      bad = true;
      return 0;
    }
    return uint8_t(d[p++]);
  }
  uint16_t u16() {
    uint16_t lo = u8();
    return uint16_t(lo | (uint16_t(u8()) << 8));
  }
  uint32_t u32() {
    uint32_t lo = u16();
    return lo | (uint32_t(u16()) << 16);
  }
  std::string sz() {
    size_t e = d.find('\0', p);
    if (e == std::string_view::npos) {
      bad = true;
      p = d.size();
      return {};
    }
    std::string s(d.substr(p, e - p));
    p = e + 1;
    return s;
  }
};

struct Writer {
  std::vector<uint8_t> b;
  void u16(uint16_t v) {
    b.push_back(uint8_t(v));
    b.push_back(uint8_t(v >> 8));
  }
  void u32(uint32_t v) {
    u16(uint16_t(v));
    u16(uint16_t(v >> 16));
  }
  void wstr(std::string_view ansi) {
    for (wchar_t c : w1252(ansi)) u16(uint16_t(c));
    u16(0);
  }
  void align4() {
    while (b.size() % 4) b.push_back(0);
  }
};

}  // namespace

// ---- the template converter ------------------------------------------------------------------------------------

DialogTemplate32 convert_dialog_template16(std::string_view in, uint32_t ex_style) {
  DialogTemplate32 out;
  Reader r{in};
  uint32_t style = r.u32();
  uint8_t count = r.u8();
  int16_t x = int16_t(r.u16()), y = int16_t(r.u16()), cx = int16_t(r.u16()), cy = int16_t(r.u16());
  // Menu: none, 0xFF + ordinal, or a name — dropped either way (the real
  // dialog runs in this process, whose resources have no such menu).
  if (r.p < in.size() && uint8_t(in[r.p]) == 0xFF) {
    r.u8();
    r.u16();
  } else {
    r.sz();
  }
  // A custom dialog class: dropped (DefDlgProc's class runs the real dialog).
  if (r.p < in.size() && uint8_t(in[r.p]) == 0xFF) {
    r.u8();
    r.u16();
  } else {
    r.sz();
  }
  out.caption = r.sz();
  uint16_t points = 0;
  std::string face;
  if (style & DS_SETFONT) {
    points = r.u16();
    face = r.sz();
  }
  if (r.bad) {
    out.error = "truncated dialog header";
    return out;
  }
  Writer w;
  // DS_SYSMODAL is refused for a configure dialog; the rest carries over.
  w.u32(style & ~uint32_t(DS_SYSMODAL));
  w.u32(ex_style);
  w.u16(count);
  w.u16(uint16_t(x));
  w.u16(uint16_t(y));
  w.u16(uint16_t(cx));
  w.u16(uint16_t(cy));
  w.u16(0);  // menu
  w.u16(0);  // class
  w.wstr(out.caption);
  if (style & DS_SETFONT) {
    w.u16(points);
    w.wstr(face);
  }
  for (int i = 0; i < count; i++) {
    int16_t ix = int16_t(r.u16()), iy = int16_t(r.u16()), icx = int16_t(r.u16()), icy = int16_t(r.u16());
    uint16_t id = r.u16();
    uint32_t istyle = r.u32();
    uint16_t atom = 0;
    std::string cls;
    if (r.p < in.size() && (uint8_t(in[r.p]) & 0x80)) {
      atom = r.u8();
    } else {
      cls = r.sz();
    }
    bool text_ordinal = false;
    uint16_t ordinal = 0;
    std::string text;
    if (r.p < in.size() && uint8_t(in[r.p]) == 0xFF) {
      r.u8();
      ordinal = r.u16();
      text_ordinal = true;
    } else {
      text = r.sz();
    }
    uint8_t extra = r.u8();
    r.p += extra;
    if (r.bad || r.p > in.size()) {
      out.error = "truncated item " + std::to_string(i);
      return out;
    }
    // Predefined classes by name are the same atoms.
    if (!atom && !cls.empty()) {
      static const std::pair<const char*, uint16_t> kNames[] = {{"BUTTON", 0x80}, {"EDIT", 0x81},     {"STATIC", 0x82},
                                                                {"LISTBOX", 0x83}, {"SCROLLBAR", 0x84}, {"COMBOBOX", 0x85}};
      for (auto& [n, a] : kNames) {
        if (upper(cls) == n) atom = a;
      }
      if (!atom) out.classes.push_back(cls);
    }
    // A static icon names the guest's icon resource, which this process lacks.
    if (atom == 0x82 && (istyle & 0x1F) == SS_ICON) {
      text_ordinal = false;
      text.clear();
    }
    w.align4();
    w.u32(istyle);
    w.u32(0);
    w.u16(uint16_t(ix));
    w.u16(uint16_t(iy));
    w.u16(uint16_t(icx));
    w.u16(uint16_t(icy));
    w.u16(id);
    if (atom) {
      w.u16(0xFFFF);
      w.u16(atom);
    } else {
      w.wstr(cls);
    }
    if (text_ordinal) {
      w.u16(0xFFFF);
      w.u16(ordinal);
    } else {
      w.wstr(text);
    }
    w.u16(0);  // no creation data: the guest's extra bytes mean nothing to a real control
  }
  out.bytes = std::move(w.b);
  out.items = count;
  out.ok = true;
  return out;
}

// ---- message translation ---------------------------------------------------------------------------------------

Ctl16 control_kind16(std::wstring_view cls) {
  auto is = [&](const wchar_t* n) { return _wcsnicmp(cls.data(), n, std::max(cls.size(), wcslen(n))) == 0 && cls.size() == wcslen(n); };
  if (is(L"Button")) return Ctl16::button;
  if (is(L"Edit")) return Ctl16::edit;
  if (is(L"ListBox") || is(L"ComboLBox")) return Ctl16::listbox;
  if (is(L"ComboBox")) return Ctl16::combobox;
  if (is(L"Static")) return Ctl16::statik;
  if (is(L"ScrollBar")) return Ctl16::scrollbar;
  return Ctl16::other;
}

uint32_t msg16_to_32(Ctl16 kind, uint16_t msg) {
  if (msg < WM_USER) return msg;
  switch (kind) {
    case Ctl16::button:  // BM_GETCHECK .. BM_SETSTYLE
      return msg <= WM_USER + 4 ? 0x00F0u + (msg - WM_USER) : 0;
    case Ctl16::edit: {  // EM_GETSEL .. EM_GETPASSWORDCHAR
      if (msg > WM_USER + 0x22) return 0;
      uint16_t n = uint16_t(msg - WM_USER);
      // Local handles, the Win16 word-break function and EM_SETFONT have no Win32 form.
      if (n == 0x0C || n == 0x0D || n == 0x13 || n == 0x1A || n == 0x20 || n == 0x21) return 0;
      return 0x00B0u + n;
    }
    case Ctl16::listbox:  // LB_ADDSTRING (WM_USER+1) .. LB_FINDSTRINGEXACT
      return msg >= WM_USER + 1 && msg <= WM_USER + 0x23 ? 0x0180u + (msg - WM_USER - 1) : 0;
    case Ctl16::combobox:  // CB_GETEDITSEL .. CB_FINDSTRINGEXACT
      return msg <= WM_USER + 0x18 ? 0x0140u + (msg - WM_USER) : 0;
    case Ctl16::statik:     // STM_SETICON/STM_GETICON: the guest's icons are not real ones
    case Ctl16::scrollbar:  // Win16 has no scroll-bar messages
      return 0;
    case Ctl16::other:
      return msg;  // a dialog's DM_* or a custom control's own messages
  }
  return 0;
}

uint16_t msg32_to_16(Ctl16 kind, uint32_t msg) {
  switch (kind) {
    case Ctl16::button:
      return msg >= 0xF0 && msg <= 0xF4 ? uint16_t(WM_USER + (msg - 0xF0)) : 0;
    case Ctl16::edit:
      return msg >= 0xB0 && msg <= 0xD2 ? uint16_t(WM_USER + (msg - 0xB0)) : 0;
    case Ctl16::listbox:
      return msg >= 0x180 && msg <= 0x1A2 ? uint16_t(WM_USER + 1 + (msg - 0x180)) : 0;
    case Ctl16::combobox:
      return msg >= 0x140 && msg <= 0x158 ? uint16_t(WM_USER + (msg - 0x140)) : 0;
    case Ctl16::statik:
    case Ctl16::scrollbar:
      return 0;
    case Ctl16::other:
      return msg < 0x10000 ? uint16_t(msg) : 0;
  }
  return 0;
}

// ---- configure mode ----------------------------------------------------------------------------------------------

namespace {

constexpr const char* U = "USER";
constexpr uint32_t kRealProcToken = 0xFFF00000;  // a real window procedure the guest holds (selector 0xFFF0: never valid)

// One real window the guest knows.
struct RealWnd16 {
  HWND h = nullptr;
  bool dialog = false;
  uint32_t dlgproc = 0;       // a dialog's DLGPROC
  uint32_t wndproc = 0;       // a guest-class window's WNDPROC, or the guest's subclass of a real control
  WNDPROC real_old = nullptr; // the real procedure a guest subclass replaced
  uint16_t hinst = 0;
  std::map<int16_t, uint16_t> words;  // window words (DWL_USER, a class's extra bytes)
  std::map<std::string, uint16_t> props;
  std::map<UINT_PTR, uint32_t> timers;  // guest TIMERPROCs
};

// The message a real procedure is forwarding to the guest right now, so a
// DefWindowProc/CallWindowProc from the guest with the same Win16 values goes
// on with the original Win32 ones (their pointers are real).
struct Forwarding {
  HWND h;
  UINT msg;
  WPARAM wp;
  LPARAM lp;
  uint16_t msg16, wp16;
  uint32_t lp16;
};

struct Dialogs16 : RuntimeState16 {
  explicit Dialogs16(Runtime16& rt) : rt(rt) {}
  ~Dialogs16() override {
    for (HBRUSH b : owned_brushes) DeleteObject(b);
  }
  Runtime16& rt;
  Configure16* cfg = nullptr;
  POINT origin{0, 0};  // the guest's screen on the real one (guest_screen_origin16)
  std::map<uint16_t, RealWnd16> wnds;
  std::map<HWND, uint16_t> ids;
  uint16_t next = kRealHwnd16First;
  // Dialogs being created (the guest procedure is needed before
  // WM_INITDIALOG: WM_MEASUREITEM and WM_SETFONT come first).
  struct Creating {
    uint32_t proc = 0, param = 0;
    uint16_t hinst = 0;
    bool attached = false;
  };
  std::vector<Creating> creating;
  // Windows being created by the guest's CreateWindow: its lpParam.
  std::vector<uint32_t> create_params;
  std::map<std::string, uint32_t> classes;  // real classes made for guest classes (upper name → proc)
  std::vector<Forwarding> forwarding;
  // Guest scratch memory for the structures a message carries (a stack).
  uint16_t scratch_handle = 0;
  uint32_t scratch = 0, scratch_top = 0;
  std::map<std::pair<uint16_t, COLORREF>, HBRUSH> brushes;
  std::vector<HBRUSH> owned_brushes;
  std::exception_ptr error;  // a guest failure inside a real callback, rethrown when the real call returns
  // DC wrappers: a guest DC standing for a real one.
  struct Wrap {
    HWND h = nullptr;
    HDC real = nullptr;
    bool release = false;  // GetDC: ReleaseDC when done
    bool paint = false;    // BeginPaint: EndPaint when done
    PAINTSTRUCT ps{};
    int w = 0, hgt = 0;
    uint16_t bmp = 0;  // the key surface (0 for a colour-only wrapper)
  };
  std::map<uint16_t, Wrap> wraps;
  std::map<COLORREF, uint8_t> nearest;  // real colour → hardware index (surface initialization)
  std::map<HFONT, uint16_t> fonts;      // real fonts as guest font objects
};

thread_local Dialogs16* g_dlg = nullptr;

Dialogs16& dl(Runtime16& rt) { return rt.state<Dialogs16>(); }

RealWnd16* rw(Dialogs16& d, uint16_t h16) {
  auto it = d.wnds.find(h16);
  if (it == d.wnds.end() || !IsWindow(it->second.h)) return nullptr;
  return &it->second;
}

RealWnd16* rw_of(Dialogs16& d, HWND h) {
  auto it = d.ids.find(h);
  return it == d.ids.end() ? nullptr : rw(d, it->second);
}

void forget(Dialogs16& d, HWND h) {
  auto it = d.ids.find(h);
  if (it == d.ids.end()) return;
  d.wnds.erase(it->second);
  d.ids.erase(it);
}

// ---- guest scratch ----
struct ScratchMark {
  explicit ScratchMark(Dialogs16& d) : d(d), saved(d.scratch_top) {}
  ~ScratchMark() { d.scratch_top = saved; }
  Dialogs16& d;
  uint32_t saved;
};

uint32_t scratch(Dialogs16& d, uint32_t n) {
  if (!d.scratch) {
    d.scratch_handle = d.rt.global().alloc(GlobalHeap16::kZeroInit, 0xFFF0);
    d.scratch = d.scratch_handle ? d.rt.global().lock(d.scratch_handle) : 0;
    if (!d.scratch) throw GuestError16(GuestError16::Kind::fatal, "configure: no guest memory for message structures");
  }
  n = (n + 3) & ~3u;
  if (d.scratch_top + n > 0xFFF0) throw GuestError16(GuestError16::Kind::fatal, "configure: message scratch exhausted");
  uint32_t p = d.scratch + d.scratch_top;
  d.scratch_top += n;
  return p;
}

uint32_t scratch_str(Dialogs16& d, std::string_view s) {
  uint32_t p = scratch(d, uint32_t(s.size() + 1));
  d.rt.write_bytes(p, s.data(), s.size());
  d.rt.wr8(p + uint32_t(s.size()), 0);
  return p;
}

// ---- colours ----
// A guest COLORREF (RGB, PALETTEINDEX, PALETTERGB) as the colour the
// hardware palette shows for it through the DC's palette.
COLORREF real_color(Runtime16& rt, uint16_t hdc16, COLORREF c) {
  Gdi16& g = rt.state<Gdi16>();
  win32::Display& disp = g.display();
  return disp.hardware_color(disp.map_index(c, hdc16 ? g.dc_palette(hdc16) : nullptr));
}

// A guest brush as a real one (a solid brush of its colour; system colour
// brushes (HBRUSH)(COLOR_x + 1) as the real system brush).
HBRUSH real_brush(Dialogs16& d, uint16_t hbr16, uint16_t hdc16) {
  if (!hbr16) return nullptr;
  if (hbr16 <= 31) return GetSysColorBrush(hbr16 - 1);
  Gdi16& g = d.rt.state<Gdi16>();
  Obj16* b = g.get(hbr16, G16::brush);
  if (!b) return nullptr;
  if (b->style == BS_NULL) return static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
  COLORREF c = real_color(d.rt, hdc16, b->color);
  auto key = std::make_pair(hbr16, c);
  auto it = d.brushes.find(key);
  if (it != d.brushes.end()) return it->second;
  HBRUSH r = CreateSolidBrush(c);
  d.brushes[key] = r;
  d.owned_brushes.push_back(r);
  return r;
}

// A real font (a dialog's, a control's) as a guest font object, so what the
// guest draws in a wrapper DC uses the font the real control would.
uint16_t guest_font(Dialogs16& d, HFONT f) {
  if (!f) return 0;
  auto it = d.fonts.find(f);
  if (it != d.fonts.end()) return it->second;
  LOGFONTW lw{};
  if (!GetObjectW(f, sizeof(lw), &lw)) return 0;
  LOGFONTA la{};
  la.lfHeight = lw.lfHeight;
  la.lfWidth = lw.lfWidth;
  la.lfEscapement = lw.lfEscapement;
  la.lfOrientation = lw.lfOrientation;
  la.lfWeight = lw.lfWeight;
  la.lfItalic = lw.lfItalic;
  la.lfUnderline = lw.lfUnderline;
  la.lfStrikeOut = lw.lfStrikeOut;
  la.lfCharSet = lw.lfCharSet;
  la.lfOutPrecision = lw.lfOutPrecision;
  la.lfClipPrecision = lw.lfClipPrecision;
  la.lfQuality = NONANTIALIASED_QUALITY;  // an 8-bit key surface takes no blended edges
  la.lfPitchAndFamily = lw.lfPitchAndFamily;
  std::string face = a1252(lw.lfFaceName);
  strncpy(la.lfFaceName, face.c_str(), LF_FACESIZE - 1);
  Obj16 o;
  o.type = G16::font;
  o.host = CreateFontIndirectA(&la);
  o.font = la;
  if (!o.host) return 0;
  uint16_t h = d.rt.state<Gdi16>().add(o);
  d.fonts[f] = h;
  return h;
}

// ---- DC wrappers ----
// A guest DC for `real` (w × h at the window's client origin). With a
// surface, the guest draws on an 8-bit key surface first filled from the real
// pixels (nearest hardware colour); flush_wrap() turns it into real colours.
uint16_t make_wrap(Dialogs16& d, HWND h, HDC real, int w, int hgt, bool surface) {
  Gdi16& g = d.rt.state<Gdi16>();
  uint16_t dc = g.create_memory_dc();
  if (!dc) return 0;
  Dialogs16::Wrap wr;
  wr.h = h;
  wr.real = real;
  wr.w = std::max(w, 1);
  wr.hgt = std::max(hgt, 1);
  // The window's own font (a control's, else its dialog's), as a real DC
  // for it would come.
  HFONT font = h ? reinterpret_cast<HFONT>(SendMessageW(h, WM_GETFONT, 0, 0)) : nullptr;
  if (!font && h && GetParent(h)) font = reinterpret_cast<HFONT>(SendMessageW(GetParent(h), WM_GETFONT, 0, 0));
  if (uint16_t gf = guest_font(d, font)) g.select(dc, gf);
  if (surface) {
    wr.bmp = g.create_device_bitmap(wr.w, wr.hgt, 8);
    Obj16* b = g.get(wr.bmp, G16::bitmap);
    if (!b || !b->bmp.bits) {
      g.destroy(dc);
      return 0;
    }
    g.select(dc, wr.bmp);
    // The real pixels as hardware indices.
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = wr.w;
    bi.bmiHeader.biHeight = -wr.hgt;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* px = nullptr;
    HBITMAP tmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &px, nullptr, 0);
    HDC mem = CreateCompatibleDC(nullptr);
    if (tmp && mem) {
      HGDIOBJ old = SelectObject(mem, tmp);
      BitBlt(mem, 0, 0, wr.w, wr.hgt, real, 0, 0, SRCCOPY);
      GdiFlush();
      win32::Display& disp = g.display();
      const uint32_t* src = static_cast<const uint32_t*>(px);
      for (int y = 0; y < wr.hgt; y++) {
        for (int x = 0; x < wr.w; x++) {
          uint32_t v = src[size_t(y) * size_t(wr.w) + size_t(x)];
          COLORREF c = RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
          auto it = d.nearest.find(c);
          uint8_t idx;
          if (it != d.nearest.end()) {
            idx = it->second;
          } else {
            idx = uint8_t(disp.nearest_index(c, false));
            d.nearest[c] = idx;
          }
          b->bmp.bits[size_t(y) * b->bmp.stride + size_t(x)] = idx;
        }
      }
      SelectObject(mem, old);
    }
    if (mem) DeleteDC(mem);
    if (tmp) DeleteObject(tmp);
  }
  d.wraps[dc] = wr;
  return dc;
}

void flush_wrap(Dialogs16& d, uint16_t dc) {
  auto it = d.wraps.find(dc);
  if (it == d.wraps.end() || !it->second.bmp) return;
  Dialogs16::Wrap& wr = it->second;
  Gdi16& g = d.rt.state<Gdi16>();
  Obj16* b = g.get(wr.bmp, G16::bitmap);
  if (!b || !b->bmp.bits) return;
  GdiFlush();
  win32::Display& disp = g.display();
  std::vector<uint32_t> rgb(size_t(wr.w) * size_t(wr.hgt));
  for (int y = 0; y < wr.hgt; y++) {
    for (int x = 0; x < wr.w; x++) {
      COLORREF c = disp.hardware_color(b->bmp.bits[size_t(y) * b->bmp.stride + size_t(x)]);
      rgb[size_t(y) * size_t(wr.w) + size_t(x)] = (uint32_t(GetRValue(c)) << 16) | (uint32_t(GetGValue(c)) << 8) | GetBValue(c);
    }
  }
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
  bi.bmiHeader.biWidth = wr.w;
  bi.bmiHeader.biHeight = -wr.hgt;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  SetDIBitsToDevice(wr.real, 0, 0, DWORD(wr.w), DWORD(wr.hgt), 0, 0, 0, UINT(wr.hgt), rgb.data(), &bi, DIB_RGB_COLORS);
}

void drop_wrap(Dialogs16& d, uint16_t dc) {
  auto it = d.wraps.find(dc);
  if (it == d.wraps.end()) return;
  Gdi16& g = d.rt.state<Gdi16>();
  uint16_t bmp = it->second.bmp;
  d.wraps.erase(it);
  g.destroy(dc);
  if (bmp) g.destroy(bmp);
}

// ---- calling the guest ----
uint32_t call_proc(Dialogs16& d, uint32_t proc, uint16_t h16, uint16_t msg, uint16_t wp, uint32_t lp) {
  trace("dlg16", "-> %04X:%04X(hwnd %04X, msg %04X, %04X, %08X)", proc >> 16, proc & 0xFFFF, h16, msg, wp, lp);
  return d.rt.call_far(proc, {w16(h16), w16(msg), w16(wp), l16(lp)});
}

// Runs `fn` for a real callback: a guest failure (or anything else thrown)
// must not cross the real window manager's frames. It is kept, the dialog
// ends, and the failure is rethrown when the real call returns (rethrow()).
template <typename F>
LRESULT guarded(Dialogs16& d, HWND h, F&& fn) {
  if (d.error) return 0;
  try {
    return fn();
  } catch (...) {
    d.error = std::current_exception();
    if (d.cfg) d.cfg->failed = true;
    // End every real dialog: the guest cannot go on.
    for (auto& [id, w] : d.wnds) {
      if (w.dialog && IsWindow(w.h)) EndDialog(w.h, -1);
    }
    (void)h;
    return 0;
  }
}

void rethrow(Dialogs16& d) {
  if (d.error) {
    std::exception_ptr e = d.error;
    d.error = nullptr;
    std::rethrow_exception(e);
  }
}

// A point on the screen (an lParam's MAKELONG(x, y)): from the real screen to
// the guest's (guest_screen_origin16), and back.
uint32_t guest_screen_point(const Dialogs16& d, LPARAM lp) {
  return (uint32_t(uint16_t(int16_t(HIWORD(lp)) - d.origin.y)) << 16) | uint16_t(int16_t(LOWORD(lp)) - d.origin.x);
}

LPARAM real_screen_point(const Dialogs16& d, uint32_t lp) {
  return MAKELPARAM(int16_t(LOWORD(lp)) + d.origin.x, int16_t(HIWORD(lp)) + d.origin.y);
}

// ---- Win32 → Win16: a real message into the guest -----------------------------------------------------------------

struct Into16 {
  bool forward = false;
  uint16_t msg = 0, wp = 0;
  uint32_t lp = 0;
  uint16_t wrap = 0;           // a DC wrapper made for the message
  uint32_t item = 0;           // the *ITEM structure's far pointer
  HDC ctlcolor_dc = nullptr;   // WM_CTLCOLOR*: the real DC
};

// Messages a guest window or dialog procedure (`dialog`: a DLGPROC) receives.
// The rest stay with the real default procedures.
bool to16(Dialogs16& d, HWND h, UINT msg, WPARAM wp, LPARAM lp, bool dialog, Into16* o) {
  auto h16 = [&](HWND x) { return real_hwnd16(d.rt, x); };
  o->msg = uint16_t(msg);
  o->wp = uint16_t(wp);
  o->lp = uint32_t(lp);
  switch (msg) {
    case WM_COMMAND:
      o->wp = LOWORD(wp);
      o->lp = (uint32_t(HIWORD(wp)) << 16) | h16(reinterpret_cast<HWND>(lp));
      break;
    case WM_CTLCOLORMSGBOX:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSCROLLBAR:
    case WM_CTLCOLORSTATIC:
      o->msg = 0x0019;  // WM_CTLCOLOR
      o->wrap = make_wrap(d, reinterpret_cast<HWND>(lp), reinterpret_cast<HDC>(wp), 1, 1, false);
      if (!o->wrap) return false;
      o->ctlcolor_dc = reinterpret_cast<HDC>(wp);
      o->wp = o->wrap;
      o->lp = (uint32_t(msg - WM_CTLCOLORMSGBOX) << 16) | h16(reinterpret_cast<HWND>(lp));
      break;
    case WM_HSCROLL:
    case WM_VSCROLL:
      o->wp = LOWORD(wp);
      o->lp = (uint32_t(h16(reinterpret_cast<HWND>(lp))) << 16) | HIWORD(wp);
      break;
    case WM_DRAWITEM: {
      const DRAWITEMSTRUCT* di = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
      if (di->CtlType == ODT_MENU) return false;
      RECT cr{};
      GetClientRect(di->hwndItem, &cr);
      o->wrap = make_wrap(d, di->hwndItem, di->hDC, cr.right, cr.bottom, true);
      if (!o->wrap) return false;
      o->item = scratch(d, 26);
      d.rt.wr16(o->item + 0, uint16_t(di->CtlType));
      d.rt.wr16(o->item + 2, uint16_t(di->CtlID));
      d.rt.wr16(o->item + 4, uint16_t(di->itemID));
      d.rt.wr16(o->item + 6, uint16_t(di->itemAction));
      d.rt.wr16(o->item + 8, uint16_t(di->itemState));
      d.rt.wr16(o->item + 10, h16(di->hwndItem));
      d.rt.wr16(o->item + 12, o->wrap);
      write16(d.rt, o->item + 14, to_rect16(di->rcItem));
      d.rt.wr32(o->item + 22, uint32_t(di->itemData));
      o->lp = o->item;
      break;
    }
    case WM_MEASUREITEM: {
      const MEASUREITEMSTRUCT* mi = reinterpret_cast<const MEASUREITEMSTRUCT*>(lp);
      if (mi->CtlType == ODT_MENU) return false;
      o->item = scratch(d, 14);
      d.rt.wr16(o->item + 0, uint16_t(mi->CtlType));
      d.rt.wr16(o->item + 2, uint16_t(mi->CtlID));
      d.rt.wr16(o->item + 4, uint16_t(mi->itemID));
      d.rt.wr16(o->item + 6, uint16_t(mi->itemWidth));
      d.rt.wr16(o->item + 8, uint16_t(mi->itemHeight));
      d.rt.wr32(o->item + 10, uint32_t(mi->itemData));
      o->lp = o->item;
      break;
    }
    case WM_COMPAREITEM: {
      const COMPAREITEMSTRUCT* ci = reinterpret_cast<const COMPAREITEMSTRUCT*>(lp);
      o->item = scratch(d, 18);
      d.rt.wr16(o->item + 0, uint16_t(ci->CtlType));
      d.rt.wr16(o->item + 2, uint16_t(ci->CtlID));
      d.rt.wr16(o->item + 4, h16(ci->hwndItem));
      d.rt.wr16(o->item + 6, uint16_t(ci->itemID1));
      d.rt.wr32(o->item + 8, uint32_t(ci->itemData1));
      d.rt.wr16(o->item + 12, uint16_t(ci->itemID2));
      d.rt.wr32(o->item + 14, uint32_t(ci->itemData2));
      o->lp = o->item;
      break;
    }
    case WM_DELETEITEM: {
      const DELETEITEMSTRUCT* di = reinterpret_cast<const DELETEITEMSTRUCT*>(lp);
      o->item = scratch(d, 12);
      d.rt.wr16(o->item + 0, uint16_t(di->CtlType));
      d.rt.wr16(o->item + 2, uint16_t(di->CtlID));
      d.rt.wr16(o->item + 4, uint16_t(di->itemID));
      d.rt.wr16(o->item + 6, h16(di->hwndItem));
      d.rt.wr32(o->item + 8, uint32_t(di->itemData));
      o->lp = o->item;
      break;
    }
    case WM_ACTIVATE:
      o->wp = LOWORD(wp);
      o->lp = (uint32_t(HIWORD(wp)) << 16) | h16(reinterpret_cast<HWND>(lp));
      break;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
      o->wp = h16(reinterpret_cast<HWND>(wp));
      o->lp = 0;
      break;
    case WM_GETDLGCODE:
      o->lp = 0;  // Win16's LPMSG is not given
      break;
    case WM_SETFONT:
      // To a guest's control, the real font as a guest font object (the
      // dialog's, from its template): it draws its label in it, as on
      // Windows 3.1 (ANTSW's check boxes, texts and frames keep it in a window
      // long; given 0, they drew in the system font and their labels were cut
      // short). A DLGPROC keeps 0: its controls get the font themselves.
      o->wp = dialog ? 0 : guest_font(d, reinterpret_cast<HFONT>(wp));
      break;
    case WM_TIMER:
      o->lp = 0;
      break;
    case WM_MOVE:
      // A top-level window's client origin, on the guest's screen (a child's
      // is in its parent's client area).
      if (!(GetWindowLongW(h, GWL_STYLE) & WS_CHILD)) o->lp = guest_screen_point(d, lp);
      break;
    case WM_NCHITTEST:
      // To a window procedure: the point on the guest's screen, and its answer
      // is the real one. Intermission's frames (ANTSW's ANT3DBOX, ANT3DGROUP,
      // ASW3DBOX, ASW3DGROUP) lie above the check boxes, sliders and edits
      // they frame (they come first in the template) and answer HTTRANSPARENT,
      // so the click goes on to the control under them, as on Windows 3.1;
      // the real default procedure's HTCLIENT, which answered for them before,
      // gave them every click. Not to a DLGPROC: its answer would be its
      // DWL_MSGRESULT, which the guest keeps to itself.
      if (dialog) return false;
      o->lp = guest_screen_point(d, lp);
      break;
    case BM_GETCHECK:
    case BM_SETCHECK:
    case BM_GETSTATE:
    case BM_SETSTATE:
    case BM_SETSTYLE: {
      // A button message to a guest's control (a window of a guest class, or
      // a real button the guest subclassed): Win16 numbered them WM_USER + n
      // (msg32_to_16), as Windows 3.1's CheckDlgButton, IsDlgButtonChecked and
      // CheckRadioButton sent them. The real ones, the dialog manager's and the
      // configure script's CHECK so reach ANTSW's check box, which takes
      // WM_USER (get) and WM_USER + 1 (set). To a DLGPROC, WM_USER + n are DM_*.
      if (dialog) return false;
      RealWnd16* w = rw_of(d, h);
      if (!w || (w->real_old && control_kind16(class_of(h)) != Ctl16::button)) return false;
      o->msg = msg32_to_16(Ctl16::button, msg);
      break;
    }
    case WM_PAINT:
    case WM_CLOSE:
    case WM_DESTROY:
    case WM_SHOWWINDOW:
    case WM_ENABLE:
    case WM_SIZE:
    case WM_CANCELMODE:
      break;
    case WM_SYSCOMMAND:
      o->wp = uint16_t(wp & 0xFFFF);
      break;
    default:
      if ((msg >= WM_KEYFIRST && msg <= WM_KEYLAST) || (msg >= WM_MOUSEFIRST && msg <= WM_MBUTTONDBLCLK)) break;
      // The dialog's and the guest's own messages (DM_*, WM_USER + n).
      if (msg >= WM_USER && msg < 0x8000) break;
      return false;
  }
  o->forward = true;
  return true;
}

// After the guest's procedure returned `r` (AX for a DLGPROC, DX:AX for a WNDPROC).
LRESULT from16(Dialogs16& d, UINT msg, LPARAM lp, const Into16& o, uint32_t r, bool dialog, bool* handled) {
  *handled = true;
  switch (msg) {
    case WM_CTLCOLORMSGBOX:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSCROLLBAR:
    case WM_CTLCOLORSTATIC: {
      uint16_t hbr = uint16_t(r);
      if (!hbr) {
        *handled = false;
        return 0;
      }
      // The colours the guest set in the wrapper go to the real DC.
      Gdi16& g = d.rt.state<Gdi16>();
      if (Dc16* dc = g.dc(o.wrap)) {
        SetTextColor(o.ctlcolor_dc, real_color(d.rt, o.wrap, dc->s.text));
        SetBkColor(o.ctlcolor_dc, real_color(d.rt, o.wrap, dc->s.bk));
        if (HDC wh = g.host_dc(o.wrap)) SetBkMode(o.ctlcolor_dc, GetBkMode(wh));
      }
      return reinterpret_cast<LRESULT>(real_brush(d, hbr, o.wrap));
    }
    case WM_DRAWITEM:
      flush_wrap(d, o.wrap);
      return TRUE;
    case WM_MEASUREITEM: {
      MEASUREITEMSTRUCT* mi = reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
      mi->itemWidth = d.rt.rd16(o.item + 6);
      mi->itemHeight = d.rt.rd16(o.item + 8);
      return TRUE;
    }
    case WM_COMPAREITEM:
    case WM_NCHITTEST:  // an int: HTTRANSPARENT is AX = 0xFFFF, whatever DX holds
      return LRESULT(int16_t(r));
    case BM_GETCHECK:
    case BM_GETSTATE:  // a WORD
      return LRESULT(uint16_t(r));
    default:
      break;
  }
  if (dialog) return LRESULT(int16_t(r));
  return LRESULT(int32_t(r));
}

// ---- Win16 → Win32: what the guest sends a real window ---------------------------------------------------------------

bool has_strings(HWND h, Ctl16 kind) {
  LONG st = GetWindowLongW(h, GWL_STYLE);
  if (kind == Ctl16::listbox) return !(st & (LBS_OWNERDRAWFIXED | LBS_OWNERDRAWVARIABLE)) || (st & LBS_HASSTRINGS);
  if (kind == Ctl16::combobox) return !(st & (CBS_OWNERDRAWFIXED | CBS_OWNERDRAWVARIABLE)) || (st & CBS_HASSTRINGS);
  return true;
}

WPARAM sx(uint16_t v) { return WPARAM(LONG_PTR(int16_t(v))); }

// A directory listing for LB_DIR/CB_DIR/DlgDirList, as Windows 3.1 formatted
// it: files, then "[dir]" entries (DDL_DIRECTORY), then the drives
// (DDL_DRIVES): "[-c-]", and "[-h-]" when the host's drives are mounted as
// H: (a module's own folder dialog reaches the user's files so: Sounder's
// "Sounds.." lists .WAV folders); DDL_EXCLUSIVE lists only the special
// entries.
std::vector<std::string> dir_entries(Runtime16& rt, const std::string& spec, uint16_t attr) {
  std::vector<std::string> out;
  win32::Vfs& vfs = rt.vfs();
  std::string full = vfs.full_path(spec.empty() ? "*.*" : spec);
  size_t slash = full.find_last_of('\\');
  std::string dir = full.substr(0, slash);
  std::string pat = full.substr(slash + 1);
  if (dir.size() == 2) dir += '\\';
  bool exclusive = attr & DDL_EXCLUSIVE;
  std::vector<std::string> files, dirs;
  for (const win32::Vfs::DirEntry& e : vfs.list(dir, "*")) {
    std::string name = e.short_name.empty() ? e.name : e.short_name;
    bool is_dir = e.attributes & FILE_ATTRIBUTE_DIRECTORY;
    if (is_dir) {
      if (name == ".") continue;
      if ((attr & DDL_DIRECTORY)) dirs.push_back("[" + upper(name) + "]");
    } else if (!exclusive && win32::Vfs::wild_match(pat, name)) {
      files.push_back(upper(name));
    }
  }
  out = files;
  out.insert(out.end(), dirs.begin(), dirs.end());
  if (attr & DDL_DRIVES) {
    out.push_back("[-c-]");
    if (vfs.is_dir("H:\\")) out.push_back("[-h-]");
  }
  return out;
}

// How a Win16 message reaches a real window: SendMessage, PostMessage, the
// real procedure a guest subclass replaced (CallWindowProc), or the real
// default procedure (the guest's DefWindowProc).
enum class Via { send, post, old_proc, def };

uint32_t send_real(Dialogs16& d, HWND h, uint16_t msg, uint16_t wp, uint32_t lp, Via via) {
  Runtime16& rt = d.rt;
  RealWnd16* w = rw_of(d, h);
  // A window whose procedure is the guest's (a guest class, or a real control
  // the guest subclassed): SendMessage calls it directly, as USER did for a
  // window of the same task — the Win16 values arrive untouched.
  if (via == Via::send && w && w->wndproc) {
    uint32_t r = call_proc(d, w->wndproc, real_hwnd16(rt, h), msg, wp, lp);
    rethrow(d);
    return r;
  }
  bool post = via == Via::post;
  WNDPROC old = w ? w->real_old : nullptr;
  // Control messages are numbered by class; a guest class's own are the guest's.
  bool guest_class = w && w->wndproc && !w->real_old;
  Ctl16 kind = guest_class ? Ctl16::other : control_kind16(class_of(h));
  uint32_t m = msg16_to_32(kind, msg);
  if (!m) {
    trace("dlg16", "message %04X to a %s control: no Win32 form; 0", msg, narrow(class_of(h)).c_str());
    return 0;
  }
  auto deliver = [&](UINT mm, WPARAM ww, LPARAM ll) -> LRESULT {
    switch (via) {
      case Via::post:
        return PostMessageW(h, mm, ww, ll);
      case Via::old_proc:
        return old ? CallWindowProcW(old, h, mm, ww, ll) : DefWindowProcW(h, mm, ww, ll);
      case Via::def:
        return DefWindowProcW(h, mm, ww, ll);
      case Via::send:
        break;
    }
    return SendMessageW(h, mm, ww, ll);
  };
  auto send = deliver;
  auto str = [&](uint32_t fp) { return w1252(rt.read_str(fp)); };
  switch (m) {
    case WM_SETTEXT: {
      if (post) return 0;  // the string would not outlive the call
      std::wstring t = str(lp);
      return uint32_t(deliver(WM_SETTEXT, 0, LPARAM(t.c_str())));
    }
    case WM_GETTEXT: {
      if (post || !wp || !lp) return 0;
      std::wstring t(size_t(wp) + 1, L'\0');
      int n = int(deliver(WM_GETTEXT, wp, LPARAM(t.data())));
      t.resize(size_t(std::max(n, 0)));
      return uint32_t(rt.write_str(lp, a1252(t), wp));
    }
    case WM_COMMAND:
      return uint32_t(send(WM_COMMAND, MAKEWPARAM(wp, HIWORD(lp)), LPARAM(real_window16(rt, LOWORD(lp)))));
    case WM_NCHITTEST:  // a point on the guest's screen
      return uint32_t(send(m, WPARAM(wp), real_screen_point(d, lp)));
    case WM_NEXTDLGCTL:
      return uint32_t(send(m, LOWORD(lp) ? WPARAM(real_window16(rt, wp)) : WPARAM(sx(wp)), LOWORD(lp)));
    case WM_SETFONT: {
      Obj16* f = rt.state<Gdi16>().get(wp, G16::font);
      return uint32_t(send(m, WPARAM(f ? f->host : nullptr), lp));
    }
    case WM_GETFONT:
      return 0;
    case EM_SETSEL:  // Win16: lParam = MAKELONG(start, end)
      return uint32_t(send(m, sx(LOWORD(lp)), LPARAM(int16_t(HIWORD(lp)))));
    case EM_LINESCROLL:  // Win16: lParam = MAKELONG(lines, chars)
      return uint32_t(send(m, sx(HIWORD(lp)), LPARAM(int16_t(LOWORD(lp)))));
    case EM_GETRECT: {
      RECT r{};
      deliver(m, 0, LPARAM(&r));
      if (lp) write16(rt, lp, to_rect16(r));
      return 0;
    }
    case EM_SETRECT:
    case EM_SETRECTNP: {
      RECT r = to_rect(read16<RECT16>(rt, lp));
      return uint32_t(send(m, 0, LPARAM(&r)));
    }
    case EM_REPLACESEL: {
      std::wstring t = str(lp);
      return uint32_t(deliver(m, wp, LPARAM(t.c_str())));
    }
    case EM_GETLINE: {
      uint16_t cap = rt.rd16(lp);
      std::wstring t(size_t(cap) + 1, L'\0');
      *reinterpret_cast<WORD*>(t.data()) = cap;
      int n = int(deliver(m, sx(wp), LPARAM(t.data())));
      t.resize(size_t(std::max(n, 0)));
      std::string a = a1252(t);
      rt.write_bytes(lp, a.data(), std::min<size_t>(a.size(), cap));
      return uint32_t(std::min<size_t>(a.size(), cap));
    }
    case EM_SETTABSTOPS:
    case LB_SETTABSTOPS: {
      std::vector<INT> tabs;
      for (uint16_t i = 0; i < wp && lp; i++) tabs.push_back(int16_t(rt.rd16(lp + 2u * i)));
      return uint32_t(deliver(m, tabs.size(), LPARAM(tabs.empty() ? nullptr : tabs.data())));
    }
    case LB_ADDSTRING:
    case LB_INSERTSTRING:
    case LB_FINDSTRING:
    case LB_SELECTSTRING:
    case LB_FINDSTRINGEXACT:
    case CB_ADDSTRING:
    case CB_INSERTSTRING:
    case CB_FINDSTRING:
    case CB_SELECTSTRING:
    case CB_FINDSTRINGEXACT: {
      WPARAM ww = (m == LB_ADDSTRING || m == CB_ADDSTRING) ? 0 : sx(wp);
      if (!has_strings(h, kind)) return uint32_t(send(m, ww, LPARAM(lp)));
      std::wstring t = str(lp);
      return uint32_t(deliver(m, ww, LPARAM(t.c_str())));
    }
    case LB_GETTEXT:
    case CB_GETLBTEXT: {
      if (!has_strings(h, kind)) {
        LRESULT data = deliver(m == LB_GETTEXT ? LB_GETITEMDATA : CB_GETITEMDATA, sx(wp), 0);
        if (lp) rt.wr32(lp, uint32_t(data));
        return 4;
      }
      LRESULT n = deliver(m == LB_GETTEXT ? LB_GETTEXTLEN : CB_GETLBTEXTLEN, sx(wp), 0);
      if (n < 0) return uint32_t(n);
      std::wstring t(size_t(n) + 1, L'\0');
      n = deliver(m, sx(wp), LPARAM(t.data()));
      if (n < 0) return uint32_t(n);
      t.resize(size_t(n));
      std::string a = a1252(t);
      if (lp) rt.write_str(lp, a, a.size() + 1);
      return uint32_t(a.size());
    }
    case LB_DIR:
    case CB_DIR: {
      std::string spec = rt.read_str(lp);
      LRESULT last = LB_ERR;
      for (const std::string& e : dir_entries(rt, spec, wp)) {
        std::wstring t = w1252(e);
        last = deliver(m == LB_DIR ? LB_ADDSTRING : CB_ADDSTRING, 0, LPARAM(t.c_str()));
      }
      return uint32_t(last);
    }
    case LB_GETSELITEMS: {
      std::vector<INT> items(wp);
      LRESULT n = deliver(m, wp, LPARAM(items.data()));
      for (LRESULT i = 0; i < n && lp; i++) rt.wr16(lp + 2u * uint32_t(i), uint16_t(items[size_t(i)]));
      return uint32_t(n);
    }
    case LB_GETITEMRECT: {
      RECT r{};
      LRESULT v = deliver(m, sx(wp), LPARAM(&r));
      if (lp) write16(rt, lp, to_rect16(r));
      return uint32_t(v);
    }
    case CB_GETDROPPEDCONTROLRECT: {  // on the screen: the guest's (guest_screen_origin16)
      RECT r{};
      LRESULT v = deliver(m, 0, LPARAM(&r));
      if (v) OffsetRect(&r, -d.origin.x, -d.origin.y);
      if (lp) write16(rt, lp, to_rect16(r));
      return uint32_t(v);
    }
    case LB_SETSEL:  // Win16: lParam's low word is the index (-1 = all)
      return uint32_t(send(m, wp, LPARAM(int16_t(LOWORD(lp)))));
    case LB_SETCURSEL:
    case LB_GETSEL:
    case LB_GETTEXTLEN:
    case LB_DELETESTRING:
    case LB_SETTOPINDEX:
    case LB_GETITEMDATA:
    case LB_SETITEMDATA:
    case LB_SETCARETINDEX:
    case LB_GETITEMHEIGHT:
    case LB_SETITEMHEIGHT:
    case CB_SETCURSEL:
    case CB_DELETESTRING:
    case CB_GETLBTEXTLEN:
    case CB_GETITEMDATA:
    case CB_SETITEMDATA:
    case CB_GETITEMHEIGHT:
    case CB_SETITEMHEIGHT:
      return uint32_t(send(m, sx(wp), LPARAM(lp)));
    default:
      return uint32_t(send(m, WPARAM(wp), LPARAM(lp)));
  }
}

// ---- the real procedures ---------------------------------------------------------------------------------------

LRESULT forward_to_guest(Dialogs16& d, RealWnd16& w, uint32_t proc, HWND h, UINT msg, WPARAM wp, LPARAM lp,
                         bool dialog, bool* handled) {
  *handled = false;
  uint16_t h16 = real_hwnd16(d.rt, h);
  // Timers with a guest TIMERPROC.
  if (msg == WM_TIMER) {
    auto t = w.timers.find(UINT_PTR(wp));
    if (t != w.timers.end()) {
      call_proc(d, t->second, h16, WM_TIMER, uint16_t(wp), d.rt.tick_count());
      *handled = true;
      return 0;
    }
  }
  ScratchMark mark(d);
  Into16 o;
  if (!to16(d, h, msg, wp, lp, dialog, &o)) return 0;
  d.forwarding.push_back(Forwarding{h, msg, wp, lp, o.msg, o.wp, o.lp});
  uint32_t r = 0;
  try {
    r = call_proc(d, proc, h16, o.msg, o.wp, o.lp);
  } catch (...) {
    d.forwarding.pop_back();
    if (o.wrap) drop_wrap(d, o.wrap);
    throw;
  }
  d.forwarding.pop_back();
  LRESULT v = from16(d, msg, lp, o, r, dialog, handled);
  if (o.wrap) drop_wrap(d, o.wrap);
  if (dialog && !(msg >= WM_CTLCOLORMSGBOX && msg <= WM_CTLCOLORSTATIC) && msg != WM_DRAWITEM && msg != WM_MEASUREITEM &&
      msg != WM_COMPAREITEM) {
    *handled = uint16_t(r) != 0;
  }
  return v;
}

INT_PTR CALLBACK host_dlgproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  Dialogs16* dp = g_dlg;
  if (!dp) return FALSE;
  Dialogs16& d = *dp;
  return guarded(d, h, [&]() -> LRESULT {
    RealWnd16* w = rw_of(d, h);
    if ((!w || !w->dialog) && !d.creating.empty() && !d.creating.back().attached) {
      // The dialog being created: its first message (WM_SETFONT, WM_MEASUREITEM,
      // …). It may have been met already, as the parent of a custom control
      // created before it got a message of its own.
      Dialogs16::Creating& c = d.creating.back();
      c.attached = true;
      uint16_t h16 = real_hwnd16(d.rt, h);
      w = rw(d, h16);
      if (w) {
        w->dialog = true;
        w->dlgproc = c.proc;
        w->hinst = c.hinst;
      }
    }
    if (!w || !w->dlgproc) return FALSE;
    if (msg == WM_NCDESTROY) {
      forget(d, h);
      return FALSE;
    }
    if (msg == WM_INITDIALOG) {
      uint32_t param = d.creating.empty() ? 0 : d.creating.back().param;
      uint16_t h16 = real_hwnd16(d.rt, h);
      uint16_t focus = real_hwnd16(d.rt, reinterpret_cast<HWND>(wp));
      d.cfg->shown++;
      // Hidden: off every monitor before anything shows.
      if (d.cfg->script->hidden()) win32::ConfigScript::park(h);
      uint32_t r = call_proc(d, w->dlgproc, h16, WM_INITDIALOG, focus, param);
      // The ADCONFIG* hooks act on the dialog once the guest has filled it —
      // and, for one DialogBox shows only when its queue first goes idle,
      // once it has been shown (MESSAGE3 builds its edit box on WM_SHOWWINDOW).
      if (GetWindowLongW(h, GWL_STYLE) & WS_VISIBLE) {
        d.cfg->script->attach(h, [](HWND x) { EndDialog(x, IDCANCEL); });
      } else {
        w->words[int16_t(0x7FF0)] = 1;  // attach when shown
      }
      return LRESULT(uint16_t(r) != 0);
    }
    bool handled = false;
    LRESULT v = forward_to_guest(d, *w, w->dlgproc, h, msg, wp, lp, /*dialog=*/true, &handled);
    if (msg == WM_SHOWWINDOW && wp) {
      w = rw_of(d, h);
      if (w && w->words.count(int16_t(0x7FF0))) {
        w->words.erase(int16_t(0x7FF0));
        d.cfg->script->attach(h, [](HWND x) { EndDialog(x, IDCANCEL); });
      }
    }
    if (!handled) return FALSE;
    // The messages whose DLGPROC result is the answer itself (DefDlgProc
    // returns it as is); for the rest TRUE means "handled".
    if ((msg >= WM_CTLCOLORMSGBOX && msg <= WM_CTLCOLORSTATIC) || msg == WM_COMPAREITEM) return v;
    return TRUE;
  });
}

// Real windows of guest-registered classes (custom controls in a dialog
// template, windows the guest creates on a real parent) and real controls
// the guest subclassed.
LRESULT CALLBACK host_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  Dialogs16* dp = g_dlg;
  if (!dp) return DefWindowProcW(h, msg, wp, lp);
  Dialogs16& d = *dp;
  RealWnd16* w = rw_of(d, h);
  if (!w) {
    // First contact: which guest class?
    auto it = d.classes.find(upper(a1252(class_of(h))));
    if (it == d.classes.end()) return DefWindowProcW(h, msg, wp, lp);
    uint16_t h16 = real_hwnd16(d.rt, h);
    w = rw(d, h16);
    if (!w) return DefWindowProcW(h, msg, wp, lp);
    w->wndproc = it->second;
  }
  WNDPROC real_old = w->real_old;
  auto deflt = [&]() { return real_old ? CallWindowProcW(real_old, h, msg, wp, lp) : DefWindowProcW(h, msg, wp, lp); };
  if (msg == WM_NCDESTROY) {
    LRESULT v = deflt();
    forget(d, h);
    return v;
  }
  if (d.error) return deflt();
  bool handled = false;
  LRESULT v = 0;
  if (msg == WM_CREATE) {
    // CREATESTRUCT16: lpCreateParams, hInstance, hMenu, hwndParent, cy, cx, y, x, style, lpszName, lpszClass, dwExStyle.
    const CREATESTRUCTW* cs = reinterpret_cast<const CREATESTRUCTW*>(lp);
    v = guarded(d, h, [&]() -> LRESULT {
      ScratchMark mark(d);
      uint32_t p = scratch(d, 34);
      uint32_t param = d.create_params.empty() ? 0 : d.create_params.back();
      d.rt.wr32(p, param);
      d.rt.wr16(p + 4, w->hinst);
      d.rt.wr16(p + 6, uint16_t(reinterpret_cast<uintptr_t>(cs->hMenu)));
      d.rt.wr16(p + 8, real_hwnd16(d.rt, cs->hwndParent));
      d.rt.wr16(p + 10, uint16_t(cs->cy));
      d.rt.wr16(p + 12, uint16_t(cs->cx));
      d.rt.wr16(p + 14, uint16_t(cs->y));
      d.rt.wr16(p + 16, uint16_t(cs->x));
      d.rt.wr32(p + 18, uint32_t(cs->style));
      d.rt.wr32(p + 22, scratch_str(d, cs->lpszName ? a1252(cs->lpszName) : std::string()));
      d.rt.wr32(p + 26, scratch_str(d, a1252(class_of(h))));
      d.rt.wr32(p + 30, uint32_t(cs->dwExStyle));
      d.forwarding.push_back(Forwarding{h, msg, wp, lp, uint16_t(WM_CREATE), 0, p});
      uint32_t r = 0;
      try {
        r = call_proc(d, w->wndproc, real_hwnd16(d.rt, h), WM_CREATE, 0, p);
      } catch (...) {
        d.forwarding.pop_back();
        throw;
      }
      d.forwarding.pop_back();
      return LRESULT(int16_t(r));
    });
    return v;
  }
  v = guarded(d, h, [&]() -> LRESULT { return forward_to_guest(d, *w, w->wndproc, h, msg, wp, lp, false, &handled); });
  if (d.error) return deflt();
  // Messages the guest never sees stay with the real default procedure.
  if (!handled && !(msg == WM_COMMAND || msg == WM_PAINT || msg == WM_TIMER || msg == WM_GETDLGCODE ||
                    (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) || (msg >= WM_MOUSEFIRST && msg <= WM_MBUTTONDBLCLK) ||
                    msg == WM_SETFOCUS || msg == WM_KILLFOCUS || msg == WM_ENABLE || msg == WM_SIZE || msg == WM_MOVE ||
                    msg == WM_DESTROY || msg == WM_CLOSE || msg == WM_SHOWWINDOW || msg == WM_SYSCOMMAND ||
                    msg == WM_ACTIVATE || msg == WM_HSCROLL || msg == WM_VSCROLL || msg == WM_CANCELMODE ||
                    (msg >= WM_USER && msg < 0x8000) || msg == WM_SETFONT))
    return deflt();
  return v;
}

bool ensure_real_class(Dialogs16& d, const std::string& name) {
  std::string key = upper(name);
  if (d.classes.count(key)) return true;
  Class16View k;
  if (!user16_class(d.rt, name, &k)) return false;
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = k.style & (CS_VREDRAW | CS_HREDRAW | CS_DBLCLKS | CS_PARENTDC | CS_SAVEBITS | CS_BYTEALIGNCLIENT);
  wc.lpfnWndProc = host_wndproc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = k.background ? real_brush(d, k.background, 0) : nullptr;
  std::wstring wname = w1252(name);
  wc.lpszClassName = wname.c_str();
  if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    log("configure: cannot register a real class for \"%s\" (error %lu)", name.c_str(), GetLastError());
    return false;
  }
  d.classes[key] = k.proc;
  trace("dlg16", "guest class \"%s\" (proc %04X:%04X) registered as a real class", name.c_str(), k.proc >> 16,
        k.proc & 0xFFFF);
  return true;
}

// The window a guest-given owner stands for: a real one, else the --owner.
HWND owner_of(Dialogs16& d, uint16_t owner16) {
  if (HWND h = real_window16(d.rt, owner16)) return h;
  return d.cfg->owner;
}

// A position the guest gives a real window of this style (MoveWindow,
// SetWindowPos, CreateWindow): a top-level window's is on the guest's
// screen, so it moves by the origin (guest_screen_origin16); a child's is in
// its parent's client area and stays.
POINT real_pos(const Dialogs16& d, uint32_t style, int x, int y) {
  if (style & WS_CHILD) return POINT{x, y};
  return POINT{x + d.origin.x, y + d.origin.y};
}

// DialogBox & co.: `tmpl` the Win16 template bytes.
int32_t run_dialog(Call16& c, uint16_t hinst, std::string_view tmpl, uint16_t owner16, uint32_t proc, uint32_t param,
                   bool modal) {
  Dialogs16& d = dl(c.rt);
  bool hidden = d.cfg->script->hidden();
  DialogTemplate32 t = convert_dialog_template16(tmpl, hidden ? (WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW) : 0);
  if (!t.ok) {
    log("configure: the dialog template does not convert: %s", t.error.c_str());
    d.cfg->failed = true;
    return -1;
  }
  for (const std::string& cls : t.classes) {
    if (!ensure_real_class(d, cls)) log("configure: dialog item class \"%s\" is not one the module registered", cls.c_str());
  }
  d.creating.push_back(Dialogs16::Creating{proc, param, hinst, false});
  trace("dlg16", "%s \"%s\" (%d items), owner %04X", modal ? "DialogBox" : "CreateDialog", t.caption.c_str(), t.items, owner16);
  HWND owner = owner_of(d, owner16);
  INT_PTR r = -1;
  Dialogs16* saved = g_dlg;
  g_dlg = &d;
  if (modal) {
    OwnerLend lend(owner);
    r = DialogBoxIndirectParamW(GetModuleHandleW(nullptr), reinterpret_cast<LPCDLGTEMPLATEW>(t.bytes.data()), owner,
                                host_dlgproc, 0);
    if (r == -1 && !d.error) log("configure: the dialog \"%s\" could not be shown (error %lu)", t.caption.c_str(), GetLastError());
  } else {
    HWND h = CreateDialogIndirectParamW(GetModuleHandleW(nullptr), reinterpret_cast<LPCDLGTEMPLATEW>(t.bytes.data()),
                                        owner, host_dlgproc, 0);
    r = h ? real_hwnd16(c.rt, h) : 0;
    if (h && !hidden) ShowWindow(h, SW_SHOW);
  }
  g_dlg = saved;
  d.creating.pop_back();
  rethrow(d);
  if (d.cfg->script->timed_out()) d.cfg->failed = true;
  trace("dlg16", "dialog \"%s\" -> %lld", t.caption.c_str(), (long long)r);
  return int32_t(r);
}

std::string dialog_resource(Call16& c, uint16_t hinst, uint32_t name) {
  Module16* m = c.rt.modules().by_handle(hinst);
  const loader::ne::Resource* res =
      m ? c.rt.modules().find_resource(m, loader::ResId::of(uint16_t(5)), res_id(c.rt, name)) : nullptr;
  if (!res) return {};
  return std::string(m->image->resource_data(*res));
}

std::string global_bytes(Runtime16& rt, uint16_t h) {
  uint32_t n = rt.global().size(h);
  uint32_t p = rt.global().lock(h);
  if (!p || !n) return {};
  std::string s(n, '\0');
  rt.read_bytes(p, s.data(), std::min<uint32_t>(n, 0xFFFF));
  rt.global().unlock(h);
  return s;
}

// Wraps an existing shim: `fn` handles the call (true) or leaves it to the
// emulated implementation (false; the arguments are re-read from the start).
void wrap(Shim16Registry& r, const char* mod, const char* name, std::function<bool(Call16&)> fn) {
  Shim16Entry* e = r.find_name(mod, name);
  if (!e) throw std::logic_error(std::string("configure: no shim ") + mod + "." + name);
  Shim16Fn old = e->fn;
  e->fn = [old, fn](Call16& c) {
    if (fn(c)) return;
    c.rewind();
    if (old) old(c);
  };
}

void replace(Shim16Registry& r, const char* mod, const char* name, Shim16Fn fn) {
  Shim16Entry* e = r.find_name(mod, name);
  if (!e) throw std::logic_error(std::string("configure: no shim ") + mod + "." + name);
  e->fn = std::move(fn);
}

}  // namespace

uint16_t real_hwnd16(Runtime16& rt, HWND h) {
  if (!h) return 0;
  Dialogs16& d = dl(rt);
  auto it = d.ids.find(h);
  if (it != d.ids.end()) return it->second;
  if (!IsWindow(h)) return 0;
  for (int tries = 0; tries < (kRealHwnd16Limit - kRealHwnd16First) / 4; tries++) {
    uint16_t id = d.next;
    d.next = uint16_t(d.next + 4);
    if (d.next >= kRealHwnd16Limit) d.next = kRealHwnd16First;
    auto used = d.wnds.find(id);
    if (used != d.wnds.end() && IsWindow(used->second.h)) continue;
    if (used != d.wnds.end()) d.ids.erase(used->second.h);
    RealWnd16 w;
    w.h = h;
    d.wnds[id] = w;
    d.ids[h] = id;
    return id;
  }
  return 0;
}

HWND real_window16(Runtime16& rt, uint16_t h16) {
  if (h16 < kRealHwnd16First || h16 >= kRealHwnd16Limit) return nullptr;
  Dialogs16& d = dl(rt);
  RealWnd16* w = rw(d, h16);
  return w ? w->h : nullptr;
}

POINT guest_screen_origin16(const RECT& owner, const RECT& work, int w, int h) {
  // Per axis: the desktop centred on the owner, then kept inside the work
  // area; one larger than the work area is centred on it.
  auto axis = [](LONG a, LONG b, LONG lo, LONG hi, int size) -> LONG {
    if (hi - lo < size) return lo + (hi - lo - size) / 2;
    return std::clamp<LONG>((a + b) / 2 - size / 2, lo, hi - size);
  };
  return POINT{axis(owner.left, owner.right, work.left, work.right, w),
               axis(owner.top, owner.bottom, work.top, work.bottom, h)};
}

void enable_real_dialogs16(Runtime16& rt, Configure16* cfg) {
  Dialogs16& d = dl(rt);
  d.cfg = cfg;
  g_dlg = &d;
  // Configure dialogs are designed for 96 DPI: the thread keeps their layout,
  // scaled by the system with crisp text (Windows 10 1703+; older: as is).
  using SetCtx = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
  if (auto f = reinterpret_cast<SetCtx>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext")))
    f(reinterpret_cast<DPI_AWARENESS_CONTEXT>(intptr_t(-5)));  // DPI_AWARENESS_CONTEXT_UNAWARE_GDISCALED
  // The guest's screen over the owner (guest_screen_origin16), measured in
  // this thread's coordinates, which the wraps below then use. The owner
  // does not move meanwhile (the settings window stays disabled for the
  // run). A hidden run parks its dialogs off every monitor: (0, 0).
  d.origin = POINT{0, 0};
  RECT orc{};
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  if (cfg && cfg->owner && !(cfg->script && cfg->script->hidden()) && GetWindowRect(cfg->owner, &orc) &&
      GetMonitorInfoW(MonitorFromWindow(cfg->owner, MONITOR_DEFAULTTONEAREST), &mi)) {
    int w = rt.display() ? rt.display()->width() : 640, h = rt.display() ? rt.display()->height() : 480;
    d.origin = guest_screen_origin16(orc, mi.rcWork, w, h);
    trace("dlg16", "the guest's %dx%d screen at (%ld,%ld): owner (%ld,%ld)-(%ld,%ld), work area (%ld,%ld)-(%ld,%ld)", w, h,
          d.origin.x, d.origin.y, orc.left, orc.top, orc.right, orc.bottom, mi.rcWork.left, mi.rcWork.top, mi.rcWork.right,
          mi.rcWork.bottom);
  }
  Shim16Registry& r = rt.shims();
  auto real = [](Call16& c, uint16_t h16) { return real_window16(c.rt, h16); };

  // ---- dialogs ----
  replace(r, U, "DialogBox", [](Call16& c) {
    uint16_t hinst = c.w();
    uint32_t name = c.ptr();
    uint16_t owner = c.w();
    uint32_t proc = c.ptr();
    std::string t = dialog_resource(c, hinst, name);
    if (t.empty()) return c.ret(uint16_t(-1));
    c.ret(uint16_t(run_dialog(c, hinst, t, owner, proc, 0, true)));
  });
  replace(r, U, "DialogBoxParam", [](Call16& c) {
    uint16_t hinst = c.w();
    uint32_t name = c.ptr();
    uint16_t owner = c.w();
    uint32_t proc = c.ptr();
    uint32_t param = c.l();
    std::string t = dialog_resource(c, hinst, name);
    if (t.empty()) return c.ret(uint16_t(-1));
    c.ret(uint16_t(run_dialog(c, hinst, t, owner, proc, param, true)));
  });
  replace(r, U, "DialogBoxIndirect", [](Call16& c) {
    uint16_t hinst = c.w(), htmpl = c.w(), owner = c.w();
    uint32_t proc = c.ptr();
    c.ret(uint16_t(run_dialog(c, hinst, global_bytes(c.rt, htmpl), owner, proc, 0, true)));
  });
  replace(r, U, "DialogBoxIndirectParam", [](Call16& c) {
    uint16_t hinst = c.w(), htmpl = c.w(), owner = c.w();
    uint32_t proc = c.ptr();
    uint32_t param = c.l();
    c.ret(uint16_t(run_dialog(c, hinst, global_bytes(c.rt, htmpl), owner, proc, param, true)));
  });
  replace(r, U, "CreateDialog", [](Call16& c) {
    uint16_t hinst = c.w();
    uint32_t name = c.ptr();
    uint16_t owner = c.w();
    uint32_t proc = c.ptr();
    std::string t = dialog_resource(c, hinst, name);
    c.ret(t.empty() ? 0 : uint16_t(run_dialog(c, hinst, t, owner, proc, 0, false)));
  });
  replace(r, U, "CreateDialogParam", [](Call16& c) {
    uint16_t hinst = c.w();
    uint32_t name = c.ptr();
    uint16_t owner = c.w();
    uint32_t proc = c.ptr();
    uint32_t param = c.l();
    std::string t = dialog_resource(c, hinst, name);
    c.ret(t.empty() ? 0 : uint16_t(run_dialog(c, hinst, t, owner, proc, param, false)));
  });
  replace(r, U, "CreateDialogIndirect", [](Call16& c) {
    uint16_t hinst = c.w();
    uint32_t tp = c.ptr();
    uint16_t owner = c.w();
    uint32_t proc = c.ptr();
    std::string t(0x2000, '\0');
    c.rt.read_bytes(tp, t.data(), std::min<uint32_t>(0x2000, 0x10000 - (tp & 0xFFFF)));
    c.ret(uint16_t(run_dialog(c, hinst, t, owner, proc, 0, false)));
  });
  replace(r, U, "EndDialog", [](Call16& c) {
    uint16_t h = c.w();
    int16_t v = c.sw();
    if (HWND rh = real_window16(c.rt, h)) {
      trace("dlg16", "EndDialog(%04X, %d)", h, v);
      EndDialog(rh, v);
    }
    c.ret(1);
  });
  replace(r, U, "IsDialogMessage", [](Call16& c) {
    uint16_t h = c.w();
    c.ptr();
    (void)h;
    c.ret(0);  // real dialogs get their keys from the real modal loop
  });
  replace(r, U, "GetDlgItem", [](Call16& c) {
    uint16_t h = c.w(), id = c.w();
    HWND rh = real_window16(c.rt, h);
    c.ret(rh ? real_hwnd16(c.rt, GetDlgItem(rh, int16_t(id))) : 0);
  });
  replace(r, U, "SendDlgItemMessage", [](Call16& c) {
    uint16_t h = c.w(), id = c.w(), msg = c.w(), wp = c.w();
    uint32_t lp = c.l();
    HWND rh = real_window16(c.rt, h);
    HWND item = rh ? GetDlgItem(rh, int16_t(id)) : nullptr;
    if (!item) return c.ret32(0);
    uint32_t v = send_real(dl(c.rt), item, msg, wp, lp, Via::send);
    rethrow(dl(c.rt));
    c.ret32(v);
  });
  replace(r, U, "SetDlgItemText", [](Call16& c) {
    uint16_t h = c.w(), id = c.w();
    uint32_t s = c.ptr();
    if (HWND rh = real_window16(c.rt, h)) SetDlgItemTextW(rh, int16_t(id), w1252(c.rt.read_str(s)).c_str());
    rethrow(dl(c.rt));
  });
  replace(r, U, "GetDlgItemText", [](Call16& c) {
    uint16_t h = c.w(), id = c.w();
    uint32_t buf = c.ptr();
    int16_t n = c.sw();
    HWND rh = real_window16(c.rt, h);
    if (!rh || n <= 0) return c.ret(0);
    std::wstring t(size_t(n) + 1, L'\0');
    UINT got = GetDlgItemTextW(rh, int16_t(id), t.data(), n);
    t.resize(got);
    c.ret(uint16_t(c.rt.write_str(buf, a1252(t), size_t(n))));
  });
  replace(r, U, "SetDlgItemInt", [](Call16& c) {
    uint16_t h = c.w(), id = c.w(), v = c.w(), sgn = c.w();
    if (HWND rh = real_window16(c.rt, h)) SetDlgItemInt(rh, int16_t(id), sgn ? UINT(int16_t(v)) : v, sgn != 0);
  });
  replace(r, U, "GetDlgItemInt", [](Call16& c) {
    uint16_t h = c.w(), id = c.w();
    uint32_t ok = c.ptr();
    uint16_t sgn = c.w();
    HWND rh = real_window16(c.rt, h);
    BOOL tr = FALSE;
    UINT v = rh ? GetDlgItemInt(rh, int16_t(id), &tr, sgn != 0) : 0;
    if (ok) c.rt.wr16(ok, uint16_t(tr));
    c.ret(uint16_t(v));
  });
  replace(r, U, "CheckDlgButton", [](Call16& c) {
    uint16_t h = c.w(), id = c.w(), v = c.w();
    if (HWND rh = real_window16(c.rt, h)) CheckDlgButton(rh, int16_t(id), v);
  });
  replace(r, U, "IsDlgButtonChecked", [](Call16& c) {
    uint16_t h = c.w(), id = c.w();
    HWND rh = real_window16(c.rt, h);
    c.ret(rh ? uint16_t(IsDlgButtonChecked(rh, int16_t(id))) : 0);
  });
  replace(r, U, "CheckRadioButton", [](Call16& c) {
    uint16_t h = c.w(), first = c.w(), last = c.w(), check = c.w();
    if (HWND rh = real_window16(c.rt, h)) CheckRadioButton(rh, int16_t(first), int16_t(last), int16_t(check));
  });
  replace(r, U, "MapDialogRect", [](Call16& c) {
    uint16_t h = c.w();
    uint32_t p = c.ptr();
    HWND rh = real_window16(c.rt, h);
    if (!rh) return;
    RECT rc = to_rect(read16<RECT16>(c.rt, p));
    MapDialogRect(rh, &rc);
    write16(c.rt, p, to_rect16(rc));
  });
  replace(r, U, "GetNextDlgTabItem", [](Call16& c) {
    uint16_t h = c.w(), ctl = c.w(), prev = c.w();
    HWND rh = real_window16(c.rt, h);
    c.ret(rh ? real_hwnd16(c.rt, GetNextDlgTabItem(rh, real_window16(c.rt, ctl), prev != 0)) : 0);
  });
  replace(r, U, "GetNextDlgGroupItem", [](Call16& c) {
    uint16_t h = c.w(), ctl = c.w(), prev = c.w();
    HWND rh = real_window16(c.rt, h);
    c.ret(rh ? real_hwnd16(c.rt, GetNextDlgGroupItem(rh, real_window16(c.rt, ctl), prev != 0)) : 0);
  });
  // DlgDirList(hDlg, lpPathSpec, nIDListBox, nIDStaticPath, uFileType): the
  // guest's disk (the Vfs), Windows 3.1's formatting; the spec keeps only
  // its file part, the static shows the directory. As USER did through DOS,
  // the listed drive and directory become the current ones (dos16.hh
  // dos_chdir): "h:*.WAV" lists H:'s own current directory, "c:*.WAV" C:'s,
  // where the list left it, and a module's getcwd() (INT 21h AH=19h, 47h)
  // then names the folder the user chose, drive and all. A directory that
  // does not exist, or is deeper than DOS's current directory could be
  // (kMaxCurDir), is refused (0): the list, the spec and the current
  // directory stay as they were.
  replace(r, U, "DlgDirList", [](Call16& c) {
    uint16_t h = c.w();
    uint32_t spec_p = c.ptr();
    uint16_t lb = c.w(), st = c.w(), attr = c.w();
    HWND rh = real_window16(c.rt, h);
    if (!rh) return c.ret(0);
    std::string spec = spec_p ? c.rt.read_str(spec_p) : "*.*";
    win32::Vfs& vfs = c.rt.vfs();
    std::string full = vfs.full_path(spec.empty() ? "*.*" : spec);
    size_t slash = full.find_last_of('\\');
    std::string dir = full.substr(0, slash), pat = full.substr(slash + 1);
    if (dir.size() == 2) dir += '\\';
    if (!pat.empty() && pat.find_first_of("*?") == std::string::npos && vfs.is_dir(full)) {
      dir = full;
      pat = "*.*";
    }
    if (dos_chdir(c.rt, dir, /*select_drive=*/true) != 0) {
      trace("dlg16", "DlgDirList refused %s: no such directory, or deeper than DOS's current directory", full.c_str());
      return c.ret(0);
    }
    if (lb) {
      HWND list = GetDlgItem(rh, int16_t(lb));
      if (list) {
        SendMessageW(list, LB_RESETCONTENT, 0, 0);
        const std::string listed = dir + (dir.back() == '\\' ? "" : "\\") + pat;
        std::vector<std::string> entries = dir_entries(c.rt, listed, attr);
        for (const std::string& e : entries) SendMessageW(list, LB_ADDSTRING, 0, LPARAM(w1252(e).c_str()));
        if (tracing("dlg16")) {
          std::string all;
          for (const std::string& e : entries) all += (all.empty() ? "" : " ") + e;
          trace("dlg16", "DlgDirList(%s, %04X) into %u: %s", listed.c_str(), attr, lb, all.c_str());
        }
      }
    }
    if (st) SetDlgItemTextW(rh, int16_t(st), w1252(upper(dir)).c_str());
    if (spec_p) c.rt.write_str(spec_p, pat, pat.size() + 1);
    c.ret(1);
  });
  // DlgDirSelect(hDlg, lpString, nIDListBox): the selection, "[dir]" → "dir\",
  // "[-c-]" → "c:"; TRUE for a directory or drive.
  replace(r, U, "DlgDirSelect", [](Call16& c) {
    uint16_t h = c.w();
    uint32_t out = c.ptr();
    uint16_t lb = c.w();
    HWND rh = real_window16(c.rt, h);
    HWND list = rh ? GetDlgItem(rh, int16_t(lb)) : nullptr;
    if (!list) return c.ret(0);
    LRESULT sel = SendMessageW(list, LB_GETCURSEL, 0, 0);
    if (sel < 0) return c.ret(0);
    std::wstring t(size_t(SendMessageW(list, LB_GETTEXTLEN, WPARAM(sel), 0)) + 1, L'\0');
    t.resize(size_t(SendMessageW(list, LB_GETTEXT, WPARAM(sel), LPARAM(t.data()))));
    std::string s = a1252(t);
    bool dir = false;
    if (s.size() >= 5 && s.front() == '[' && s[1] == '-' && s.back() == ']') {
      s = std::string(1, s[2]) + ":";
      dir = true;
    } else if (s.size() >= 2 && s.front() == '[' && s.back() == ']') {
      s = s.substr(1, s.size() - 2) + "\\";
      dir = true;
    }
    if (out) c.rt.write_str(out, s, s.size() + 1);
    c.ret(dir ? 1 : 0);
  });

  // ---- windows ----
  wrap(r, U, "SendMessage", [](Call16& c) {
    uint16_t h = c.w(), msg = c.w(), wp = c.w();
    uint32_t lp = c.l();
    HWND rh = real_window16(c.rt, h);
    if (!rh) return false;
    uint32_t v = send_real(dl(c.rt), rh, msg, wp, lp, Via::send);
    rethrow(dl(c.rt));
    c.ret32(v);
    return true;
  });
  wrap(r, U, "PostMessage", [](Call16& c) {
    uint16_t h = c.w(), msg = c.w(), wp = c.w();
    uint32_t lp = c.l();
    HWND rh = real_window16(c.rt, h);
    if (!rh) return false;
    c.ret(send_real(dl(c.rt), rh, msg, wp, lp, Via::post) ? 1 : 0);
    return true;
  });
  // The guest's DefWindowProc / CallWindowProc(original) on a real window:
  // for the message a real procedure is forwarding right now, the real
  // procedure goes on with the original Win32 values; anything else is
  // translated and marshalled as the guest's own sends are.
  auto default_proc = [](Call16& c, bool old_proc, HWND rh, uint16_t msg, uint16_t wp, uint32_t lp) -> uint32_t {
    Dialogs16& d = dl(c.rt);
    RealWnd16* w = rw_of(d, rh);
    WNDPROC real_proc = old_proc && w ? w->real_old : nullptr;
    for (auto it = d.forwarding.rbegin(); it != d.forwarding.rend(); ++it) {
      if (it->h == rh && it->msg16 == msg && it->wp16 == wp && it->lp16 == lp) {
        return uint32_t(real_proc ? CallWindowProcW(real_proc, rh, it->msg, it->wp, it->lp)
                                  : DefWindowProcW(rh, it->msg, it->wp, it->lp));
      }
    }
    return send_real(d, rh, msg, wp, lp, old_proc ? Via::old_proc : Via::def);
  };
  wrap(r, U, "DefWindowProc", [default_proc](Call16& c) {
    uint16_t h = c.w(), msg = c.w(), wp = c.w();
    uint32_t lp = c.l();
    HWND rh = real_window16(c.rt, h);
    if (!rh) return false;
    c.ret32(default_proc(c, false, rh, msg, wp, lp));
    return true;
  });
  wrap(r, U, "CallWindowProc", [default_proc](Call16& c) {
    uint32_t proc = c.ptr();
    uint16_t h = c.w(), msg = c.w(), wp = c.w();
    uint32_t lp = c.l();
    if ((proc & 0xFFFF0000) != kRealProcToken) return false;
    HWND rh = real_window16(c.rt, h);
    RealWnd16* w = rh ? rw_of(dl(c.rt), rh) : nullptr;
    if (!w || !w->real_old) return c.ret32(0), true;
    c.ret32(default_proc(c, true, rh, msg, wp, lp));
    return true;
  });
  wrap(r, U, "IsWindow", [real](Call16& c) {
    HWND rh = real(c, c.w());
    if (!rh) return false;
    c.ret(1);
    return true;
  });
  wrap(r, U, "IsWindowVisible", [real](Call16& c) {
    HWND rh = real(c, c.w());
    if (!rh) return false;
    c.ret(IsWindowVisible(rh) ? 1 : 0);
    return true;
  });
  wrap(r, U, "IsWindowEnabled", [real](Call16& c) {
    HWND rh = real(c, c.w());
    if (!rh) return false;
    c.ret(IsWindowEnabled(rh) ? 1 : 0);
    return true;
  });
  wrap(r, U, "EnableWindow", [real](Call16& c) {
    HWND rh = real(c, c.w());
    uint16_t en = c.w();
    if (!rh) return false;
    c.ret(EnableWindow(rh, en != 0) ? 1 : 0);
    return true;
  });
  wrap(r, U, "ShowWindow", [real](Call16& c) {
    HWND rh = real(c, c.w());
    int16_t cmd = c.sw();
    if (!rh) return false;
    c.ret(ShowWindow(rh, cmd) ? 1 : 0);
    return true;
  });
  // Positions (guest_screen_origin16): a top-level window's from the guest's
  // screen to the real one; rectangles and points on the screen back.
  wrap(r, U, "MoveWindow", [real](Call16& c) {
    HWND rh = real(c, c.w());
    int16_t x = c.sw(), y = c.sw(), w = c.sw(), h = c.sw();
    uint16_t repaint = c.w();
    if (!rh) return false;
    POINT p = real_pos(dl(c.rt), uint32_t(GetWindowLongW(rh, GWL_STYLE)), x, y);
    c.ret(MoveWindow(rh, p.x, p.y, w, h, repaint != 0) ? 1 : 0);
    return true;
  });
  wrap(r, U, "SetWindowPos", [real](Call16& c) {
    HWND rh = real(c, c.w());
    uint16_t after = c.w();
    int16_t x = c.sw(), y = c.sw(), w = c.sw(), h = c.sw();
    uint16_t flags = c.w();
    if (!rh) return false;
    HWND ins = after == 1 ? HWND_BOTTOM : after == 0 ? HWND_TOP : real_window16(c.rt, after);
    if (!ins) flags |= SWP_NOZORDER;
    POINT p{x, y};
    if (!(flags & SWP_NOMOVE)) p = real_pos(dl(c.rt), uint32_t(GetWindowLongW(rh, GWL_STYLE)), x, y);
    c.ret(SetWindowPos(rh, ins, p.x, p.y, w, h, flags) ? 1 : 0);
    return true;
  });
  wrap(r, U, "BringWindowToTop", [real](Call16& c) {
    HWND rh = real(c, c.w());
    if (!rh) return false;
    c.ret(BringWindowToTop(rh) ? 1 : 0);
    return true;
  });
  wrap(r, U, "GetWindowText", [real](Call16& c) {
    HWND rh = real(c, c.w());
    uint32_t buf = c.ptr();
    int16_t n = c.sw();
    if (!rh) return false;
    if (n <= 0) return c.ret(0), true;
    std::wstring t(size_t(n) + 1, L'\0');
    t.resize(size_t(GetWindowTextW(rh, t.data(), n)));
    c.ret(uint16_t(c.rt.write_str(buf, a1252(t), size_t(n))));
    return true;
  });
  wrap(r, U, "GetWindowTextLength", [real](Call16& c) {
    HWND rh = real(c, c.w());
    if (!rh) return false;
    c.ret(uint16_t(GetWindowTextLengthW(rh)));
    return true;
  });
  wrap(r, U, "SetWindowText", [real](Call16& c) {
    HWND rh = real(c, c.w());
    uint32_t s = c.ptr();
    if (!rh) return false;
    SetWindowTextW(rh, w1252(c.rt.read_str(s)).c_str());
    rethrow(dl(c.rt));
    return true;
  });
  wrap(r, U, "GetClassName", [real](Call16& c) {
    HWND rh = real(c, c.w());
    uint32_t buf = c.ptr();
    int16_t n = c.sw();
    if (!rh) return false;
    c.ret(n > 0 ? uint16_t(c.rt.write_str(buf, a1252(class_of(rh)), size_t(n))) : 0);
    return true;
  });
  wrap(r, U, "GetDlgCtrlID", [real](Call16& c) {
    HWND rh = real(c, c.w());
    if (!rh) return false;
    c.ret(uint16_t(GetDlgCtrlID(rh)));
    return true;
  });
  wrap(r, U, "GetParent", [real](Call16& c) {
    HWND rh = real(c, c.w());
    if (!rh) return false;
    c.ret(real_hwnd16(c.rt, GetParent(rh)));
    return true;
  });
  wrap(r, U, "GetWindow", [real](Call16& c) {
    HWND rh = real(c, c.w());
    uint16_t cmd = c.w();
    if (!rh) return false;
    c.ret(real_hwnd16(c.rt, GetWindow(rh, cmd)));
    return true;
  });
  wrap(r, U, "GetClientRect", [real](Call16& c) {
    HWND rh = real(c, c.w());
    uint32_t p = c.ptr();
    if (!rh) return false;
    RECT rc{};
    GetClientRect(rh, &rc);
    write16(c.rt, p, to_rect16(rc));
    return true;
  });
  wrap(r, U, "GetWindowRect", [real](Call16& c) {
    HWND rh = real(c, c.w());
    uint32_t p = c.ptr();
    if (!rh) return false;
    RECT rc{};
    GetWindowRect(rh, &rc);
    const POINT o = dl(c.rt).origin;
    OffsetRect(&rc, -o.x, -o.y);
    write16(c.rt, p, to_rect16(rc));
    return true;
  });
  wrap(r, U, "ClientToScreen", [real](Call16& c) {
    HWND rh = real(c, c.w());
    uint32_t p = c.ptr();
    if (!rh) return false;
    POINT16 p16 = read16<POINT16>(c.rt, p);
    POINT pt{p16.x, p16.y};
    ClientToScreen(rh, &pt);
    const POINT o = dl(c.rt).origin;
    write16(c.rt, p, POINT16{int16_t(pt.x - o.x), int16_t(pt.y - o.y)});
    return true;
  });
  wrap(r, U, "ScreenToClient", [real](Call16& c) {
    HWND rh = real(c, c.w());
    uint32_t p = c.ptr();
    if (!rh) return false;
    POINT16 p16 = read16<POINT16>(c.rt, p);
    const POINT o = dl(c.rt).origin;
    POINT pt{p16.x + o.x, p16.y + o.y};
    ScreenToClient(rh, &pt);
    write16(c.rt, p, POINT16{int16_t(pt.x), int16_t(pt.y)});
    return true;
  });
  // The real cursor, on the guest's screen: a guest control follows it while
  // a button is held (ANTSW's slider drags its thumb to it and repeats an
  // arrow while the cursor stays on it), and the saver's input state, which
  // the emulated GetCursorPos reads, never moves here. On a desktop that is
  // not the input desktop the real one fails (access denied): the emulated
  // one answers.
  wrap(r, U, "GetCursorPos", [](Call16& c) {
    uint32_t p = c.ptr();
    POINT pt{};
    if (!GetCursorPos(&pt)) return false;
    const POINT o = dl(c.rt).origin;
    write16(c.rt, p, POINT16{int16_t(pt.x - o.x), int16_t(pt.y - o.y)});
    return true;
  });
  wrap(r, U, "InvalidateRect", [real](Call16& c) {
    HWND rh = real(c, c.w());
    uint32_t p = c.ptr();
    uint16_t erase = c.w();
    if (!rh) return false;
    RECT rc{};
    if (p) rc = to_rect(read16<RECT16>(c.rt, p));
    InvalidateRect(rh, p ? &rc : nullptr, erase != 0);
    return true;
  });
  wrap(r, U, "ValidateRect", [real](Call16& c) {
    HWND rh = real(c, c.w());
    uint32_t p = c.ptr();
    if (!rh) return false;
    RECT rc{};
    if (p) rc = to_rect(read16<RECT16>(c.rt, p));
    ValidateRect(rh, p ? &rc : nullptr);
    return true;
  });
  wrap(r, U, "UpdateWindow", [real](Call16& c) {
    HWND rh = real(c, c.w());
    if (!rh) return false;
    UpdateWindow(rh);
    rethrow(dl(c.rt));
    return true;
  });
  wrap(r, U, "SetFocus", [real](Call16& c) {
    HWND rh = real(c, c.w());
    if (!rh) return false;
    c.ret(real_hwnd16(c.rt, SetFocus(rh)));
    return true;
  });
  wrap(r, U, "GetFocus", [](Call16& c) {
    HWND f = GetFocus();
    if (!f || !dl(c.rt).ids.count(f)) {
      // A real control the guest has not met yet is still one of its dialog's.
      HWND top = f ? GetAncestor(f, GA_ROOT) : nullptr;
      if (!top || !dl(c.rt).ids.count(top)) return false;
    }
    c.ret(real_hwnd16(c.rt, f));
    return true;
  });
  wrap(r, U, "SetActiveWindow", [real](Call16& c) {
    HWND rh = real(c, c.w());
    if (!rh) return false;
    c.ret(real_hwnd16(c.rt, SetActiveWindow(rh)));
    return true;
  });
  wrap(r, U, "GetActiveWindow", [](Call16& c) {
    HWND a = GetActiveWindow();
    if (!a || !dl(c.rt).ids.count(a)) return false;
    c.ret(real_hwnd16(c.rt, a));
    return true;
  });
  wrap(r, U, "DestroyWindow", [real](Call16& c) {
    HWND rh = real(c, c.w());
    if (!rh) return false;
    c.ret(DestroyWindow(rh) ? 1 : 0);
    rethrow(dl(c.rt));
    return true;
  });
  wrap(r, U, "SetCapture", [real](Call16& c) {
    HWND rh = real(c, c.w());
    if (!rh) return false;
    c.ret(real_hwnd16(c.rt, SetCapture(rh)));
    return true;
  });
  wrap(r, U, "GetCapture", [](Call16& c) {
    HWND cap = GetCapture();
    if (!cap || !dl(c.rt).ids.count(cap)) return false;
    c.ret(real_hwnd16(c.rt, cap));
    return true;
  });
  wrap(r, U, "ReleaseCapture", [](Call16& c) {
    if (GetCapture() && dl(c.rt).ids.count(GetCapture())) ReleaseCapture();
    return false;  // the emulated bookkeeping too
  });
  // Window words and longs: the real style/ID/parent/procedure; everything
  // else (DWL_USER, a guest class's extra bytes) kept here.
  auto get_long = [](Call16& c, HWND rh, int16_t i) -> uint32_t {
    Dialogs16& d = dl(c.rt);
    RealWnd16* w = rw_of(d, rh);
    switch (i) {
      case GWL_STYLE:
        return uint32_t(GetWindowLongW(rh, GWL_STYLE));
      case GWL_EXSTYLE:
        return uint32_t(GetWindowLongW(rh, GWL_EXSTYLE));
      case -4:  // GWL_WNDPROC
        if (w && w->wndproc) return w->wndproc;
        return kRealProcToken | real_hwnd16(c.rt, rh);
      case -12:  // GWW_ID
        return uint32_t(GetDlgCtrlID(rh));
      case -8:  // GWW_HWNDPARENT
        return real_hwnd16(c.rt, GetParent(rh));
      case -6:  // GWW_HINSTANCE
        return w ? w->hinst : 0;
      case 4:  // DWL_DLGPROC
        if (w && w->dialog) return w->dlgproc;
        [[fallthrough]];
      default:
        if (!w) return 0;
        return uint32_t(w->words.count(i) ? w->words[i] : 0) |
               (uint32_t(w->words.count(int16_t(i + 2)) ? w->words[int16_t(i + 2)] : 0) << 16);
    }
  };
  wrap(r, U, "GetWindowWord", [real, get_long](Call16& c) {
    HWND rh = real(c, c.w());
    int16_t i = c.sw();
    if (!rh) return false;
    c.ret(uint16_t(get_long(c, rh, i)));
    return true;
  });
  wrap(r, U, "GetWindowLong", [real, get_long](Call16& c) {
    HWND rh = real(c, c.w());
    int16_t i = c.sw();
    if (!rh) return false;
    c.ret32(get_long(c, rh, i));
    return true;
  });
  auto set_long = [get_long](Call16& c, HWND rh, int16_t i, uint32_t v, bool word) -> uint32_t {
    Dialogs16& d = dl(c.rt);
    RealWnd16* w = rw_of(d, rh);
    if (!w) {
      real_hwnd16(c.rt, rh);
      w = rw_of(d, rh);
      if (!w) return 0;
    }
    uint32_t old = get_long(c, rh, i);
    switch (i) {
      case GWL_STYLE:
        SetWindowLongW(rh, GWL_STYLE, LONG(v));
        return old;
      case GWL_EXSTYLE:
        SetWindowLongW(rh, GWL_EXSTYLE, LONG(v));
        return old;
      case -4:  // GWL_WNDPROC: the guest subclasses a real window
        if (!w->wndproc && !w->real_old) {
          w->real_old = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(rh, GWLP_WNDPROC, LONG_PTR(&host_wndproc)));
        }
        if ((v & 0xFFFF0000) == kRealProcToken) {
          // Handing the original back: unsubclass.
          if (w->real_old) SetWindowLongPtrW(rh, GWLP_WNDPROC, LONG_PTR(w->real_old));
          w->real_old = nullptr;
          w->wndproc = 0;
        } else {
          w->wndproc = v;
        }
        trace("dlg16", "SetWindowLong(%04X, GWL_WNDPROC, %08X)", real_hwnd16(c.rt, rh), v);
        return old;
      default:
        w->words[i] = uint16_t(v);
        if (!word) w->words[int16_t(i + 2)] = uint16_t(v >> 16);
        return old;
    }
  };
  wrap(r, U, "SetWindowWord", [real, set_long](Call16& c) {
    HWND rh = real(c, c.w());
    int16_t i = c.sw();
    uint16_t v = c.w();
    if (!rh) return false;
    c.ret(uint16_t(set_long(c, rh, i, v, true)));
    return true;
  });
  wrap(r, U, "SetWindowLong", [real, set_long](Call16& c) {
    HWND rh = real(c, c.w());
    int16_t i = c.sw();
    uint32_t v = c.l();
    if (!rh) return false;
    c.ret32(set_long(c, rh, i, v, false));
    return true;
  });
  auto prop_name = [](Call16& c, uint32_t name) {
    return upper((name >> 16) ? c.rt.read_str(name) : "#" + std::to_string(name & 0xFFFF));
  };
  wrap(r, U, "SetProp", [real, prop_name](Call16& c) {
    uint16_t h = c.w();
    HWND rh = real(c, h);
    uint32_t name = c.ptr();
    uint16_t v = c.w();
    if (!rh) return false;
    RealWnd16* w = rw(dl(c.rt), h);
    if (w) w->props[prop_name(c, name)] = v;
    c.ret(w ? 1 : 0);
    return true;
  });
  wrap(r, U, "GetProp", [real, prop_name](Call16& c) {
    uint16_t h = c.w();
    HWND rh = real(c, h);
    uint32_t name = c.ptr();
    if (!rh) return false;
    RealWnd16* w = rw(dl(c.rt), h);
    auto it = w ? w->props.find(prop_name(c, name)) : std::map<std::string, uint16_t>::iterator();
    c.ret(w && it != w->props.end() ? it->second : 0);
    return true;
  });
  wrap(r, U, "RemoveProp", [real, prop_name](Call16& c) {
    uint16_t h = c.w();
    HWND rh = real(c, h);
    uint32_t name = c.ptr();
    if (!rh) return false;
    RealWnd16* w = rw(dl(c.rt), h);
    uint16_t v = 0;
    if (w) {
      auto it = w->props.find(prop_name(c, name));
      if (it != w->props.end()) {
        v = it->second;
        w->props.erase(it);
      }
    }
    c.ret(v);
    return true;
  });
  wrap(r, U, "SetTimer", [real](Call16& c) {
    uint16_t h = c.w();
    HWND rh = real(c, h);
    uint16_t id = c.w(), ms = c.w();
    uint32_t proc = c.ptr();
    if (!rh) return false;
    RealWnd16* w = rw(dl(c.rt), h);
    if (w) {
      if (proc) w->timers[id] = proc;
      else w->timers.erase(id);
    }
    c.ret(SetTimer(rh, id, std::max<UINT>(ms, 1), nullptr) ? id : 0);
    return true;
  });
  wrap(r, U, "KillTimer", [real](Call16& c) {
    uint16_t h = c.w();
    HWND rh = real(c, h);
    uint16_t id = c.w();
    if (!rh) return false;
    if (RealWnd16* w = rw(dl(c.rt), h)) w->timers.erase(id);
    c.ret(KillTimer(rh, id) ? 1 : 0);
    return true;
  });
  // DCs of real windows: gdi16 wrappers (see make_wrap).
  wrap(r, U, "GetDC", [real](Call16& c) {
    HWND rh = real(c, c.w());
    if (!rh) return false;
    HDC hdc = GetDC(rh);
    RECT rc{};
    GetClientRect(rh, &rc);
    uint16_t dc = make_wrap(dl(c.rt), rh, hdc, rc.right, rc.bottom, true);
    if (!dc) {
      ReleaseDC(rh, hdc);
      return c.ret(0), true;
    }
    dl(c.rt).wraps[dc].release = true;
    c.ret(dc);
    return true;
  });
  wrap(r, U, "ReleaseDC", [](Call16& c) {
    c.w();
    uint16_t dc = c.w();
    Dialogs16& d = dl(c.rt);
    auto it = d.wraps.find(dc);
    if (it == d.wraps.end()) return false;
    flush_wrap(d, dc);
    if (it->second.release) ReleaseDC(it->second.h, it->second.real);
    drop_wrap(d, dc);
    c.ret(1);
    return true;
  });
  wrap(r, U, "BeginPaint", [real](Call16& c) {
    HWND rh = real(c, c.w());
    uint32_t ps = c.ptr();
    if (!rh) return false;
    Dialogs16& d = dl(c.rt);
    PAINTSTRUCT p{};
    HDC hdc = BeginPaint(rh, &p);
    RECT rc{};
    GetClientRect(rh, &rc);
    uint16_t dc = make_wrap(d, rh, hdc, rc.right, rc.bottom, true);
    if (!dc) {
      EndPaint(rh, &p);
      return c.ret(0), true;
    }
    d.wraps[dc].paint = true;
    d.wraps[dc].ps = p;
    uint8_t zero[32] = {};
    c.rt.write_bytes(ps, zero, sizeof(zero));
    c.rt.wr16(ps, dc);
    c.rt.wr16(ps + 2, uint16_t(p.fErase));
    write16(c.rt, ps + 4, to_rect16(p.rcPaint));
    c.ret(dc);
    return true;
  });
  wrap(r, U, "EndPaint", [](Call16& c) {
    c.w();
    uint32_t ps = c.ptr();
    uint16_t dc = c.rt.rd16(ps);
    Dialogs16& d = dl(c.rt);
    auto it = d.wraps.find(dc);
    if (it == d.wraps.end() || !it->second.paint) return false;
    flush_wrap(d, dc);
    PAINTSTRUCT p = it->second.ps;
    HWND h = it->second.h;
    drop_wrap(d, dc);
    EndPaint(h, &p);
    return true;
  });
  wrap(r, U, "EnumChildWindows", [real](Call16& c) {
    uint16_t h = c.w();
    HWND rh = real(c, h);
    uint32_t proc = c.ptr();
    uint32_t lp = c.l();
    if (!rh) return false;
    std::vector<HWND> kids;
    EnumChildWindows(
        rh, [](HWND k, LPARAM p) -> BOOL {
          reinterpret_cast<std::vector<HWND>*>(p)->push_back(k);
          return TRUE;
        },
        LPARAM(&kids));
    for (HWND k : kids) {
      if (!IsWindow(k)) continue;
      if (!(c.rt.call_far(proc, {w16(real_hwnd16(c.rt, k)), l16(lp)}) & 0xFFFF)) break;
    }
    c.ret(1);
    return true;
  });
  wrap(r, U, "SetScrollPos", [real](Call16& c) {
    HWND rh = real(c, c.w());
    int16_t bar = c.sw(), pos = c.sw();
    uint16_t redraw = c.w();
    if (!rh) return false;
    c.ret(uint16_t(SetScrollPos(rh, bar, pos, redraw != 0)));
    return true;
  });
  wrap(r, U, "GetScrollPos", [real](Call16& c) {
    HWND rh = real(c, c.w());
    int16_t bar = c.sw();
    if (!rh) return false;
    c.ret(uint16_t(GetScrollPos(rh, bar)));
    return true;
  });
  wrap(r, U, "SetScrollRange", [real](Call16& c) {
    HWND rh = real(c, c.w());
    int16_t bar = c.sw(), lo = c.sw(), hi = c.sw();
    uint16_t redraw = c.w();
    if (!rh) return false;
    SetScrollRange(rh, bar, lo, hi, redraw != 0);
    return true;
  });
  wrap(r, U, "GetScrollRange", [real](Call16& c) {
    HWND rh = real(c, c.w());
    int16_t bar = c.sw();
    uint32_t lo = c.ptr(), hi = c.ptr();
    if (!rh) return false;
    INT a = 0, b = 0;
    GetScrollRange(rh, bar, &a, &b);
    if (lo) c.rt.wr16(lo, uint16_t(a));
    if (hi) c.rt.wr16(hi, uint16_t(b));
    return true;
  });
  // CreateWindow(Ex) with a real parent: a real child window (a system class
  // or a real class made for the guest's class).
  auto create_real = [](Call16& c, uint32_t exstyle) -> bool {
    uint32_t cls = c.ptr(), title = c.ptr();
    uint32_t style = c.l();
    int16_t x = c.sw(), y = c.sw(), w = c.sw(), h = c.sw();
    uint16_t parent = c.w(), menu = c.w(), hinst = c.w();
    uint32_t param = c.ptr();
    HWND rp = real_window16(c.rt, parent);
    if (!rp) return false;
    Dialogs16& d = dl(c.rt);
    std::string cn = (cls >> 16) ? c.rt.read_str(cls) : "#" + std::to_string(cls & 0xFFFF);
    bool system = false;
    for (const char* n : {"BUTTON", "EDIT", "STATIC", "LISTBOX", "COMBOBOX", "SCROLLBAR"}) {
      if (upper(cn) == n) system = true;
    }
    if (!system && !ensure_real_class(d, cn)) {
      log("configure: CreateWindow of unknown class \"%s\" on a real window refused", cn.c_str());
      return c.ret(0), true;
    }
    auto cx = [](int16_t v) { return v == int16_t(0x8000) ? CW_USEDEFAULT : int(v); };
    // A top-level (owned) window's position is on the guest's screen.
    POINT at{cx(x), cx(y)};
    if (at.x != CW_USEDEFAULT && at.y != CW_USEDEFAULT) at = real_pos(d, style, at.x, at.y);
    d.create_params.push_back(param);
    HWND hw = CreateWindowExW(exstyle, w1252(cn).c_str(), w1252(c.rt.read_str(title)).c_str(), style, at.x, at.y, cx(w), cx(h),
                              rp, reinterpret_cast<HMENU>(uintptr_t(menu)), GetModuleHandleW(nullptr), nullptr);
    d.create_params.pop_back();
    rethrow(d);
    uint16_t h16 = real_hwnd16(c.rt, hw);
    if (RealWnd16* rwp = rw(d, h16)) rwp->hinst = hinst;
    trace("dlg16", "CreateWindow(\"%s\") on real %04X -> %04X", cn.c_str(), parent, h16);
    c.ret(h16);
    return true;
  };
  wrap(r, U, "CreateWindow", [create_real](Call16& c) { return create_real(c, 0); });
  wrap(r, U, "CreateWindowEx", [create_real](Call16& c) {
    uint32_t ex = c.l();
    // create_real reads the rest from where the cursor is.
    return create_real(c, ex);
  });

  // ---- message boxes, help, file dialogs, programs ----
  replace(r, U, "MessageBox", [](Call16& c) {
    uint16_t owner = c.w();
    std::string text = c.rt.read_str(c.ptr()), cap = c.rt.read_str(c.ptr());
    uint16_t type = c.w();
    Dialogs16& d = dl(c.rt);
    std::wstring wt = w1252(text), wc = w1252(cap);
    d.cfg->shown++;
    if (std::optional<int> a = d.cfg->script->message_box(wt)) return c.ret(uint16_t(*a));
    HWND o = owner_of(d, owner);
    Dialogs16* saved = g_dlg;
    g_dlg = &d;
    int v = 0;
    {
      OwnerLend lend(o);
      v = MessageBoxW(o, wt.c_str(), wc.c_str(), type & ~UINT(MB_SYSTEMMODAL));
    }
    g_dlg = saved;
    rethrow(d);
    c.ret(uint16_t(v));
  });
  replace(r, U, "WinHelp", [](Call16& c) {
    c.w();
    std::string file = c.rt.read_str(c.ptr());
    uint16_t cmd = c.w();
    c.l();
    log("configure: WinHelp(\"%s\", %u) — help files are not shown", file.c_str(), cmd);
    c.ret(1);
  });
  // GetOpenFileName/GetSaveFileName(OPENFILENAME16*): the real dialogs; the
  // chosen host path reaches the guest as an 8.3 H:\ path (or its mount form).
  auto file_dialog = [](Call16& c, bool save) {
    uint32_t ofn = c.ptr();
    Runtime16& rt = c.rt;
    Dialogs16& d = dl(rt);
    win32::Vfs& vfs = rt.vfs();
    auto rd_str_pairs = [&](uint32_t fp) {
      std::wstring out;
      if (!fp) return out;
      for (int k = 0; k < 64; k++) {
        std::string a = rt.read_str(fp);
        if (a.empty()) break;
        out += w1252(a);
        out.push_back(L'\0');
        fp += uint32_t(a.size() + 1);
      }
      out.push_back(L'\0');
      return out;
    };
    uint16_t owner16 = rt.rd16(ofn + 4);
    std::wstring filter = rd_str_pairs(rt.rd32(ofn + 8));
    uint32_t filter_index = rt.rd32(ofn + 20);
    uint32_t file_p = rt.rd32(ofn + 24), max_file = rt.rd32(ofn + 28);
    uint32_t title_p = rt.rd32(ofn + 32), max_title = rt.rd32(ofn + 36);
    std::string init_dir = rt.read_str(rt.rd32(ofn + 40));
    std::string title = rt.read_str(rt.rd32(ofn + 44));
    uint32_t flags = rt.rd32(ofn + 48);
    std::string def_ext = rt.read_str(rt.rd32(ofn + 56));
    std::string initial = file_p ? rt.read_str(file_p) : std::string();
    if (flags & (OFN_ENABLEHOOK | OFN_ENABLETEMPLATE | OFN_ENABLETEMPLATEHANDLE)) {
      log("configure: %s's hook/template is not supported; the standard dialog shows", save ? "GetSaveFileName" : "GetOpenFileName");
    }
    d.cfg->shown++;
    std::wstring host;
    if (std::optional<std::wstring> a = d.cfg->script->file_dialog()) {
      host = *a;
    } else {
      // The initial directory and file, guest → host.
      std::string gdir = init_dir;
      std::string gfile = initial;
      if (!gfile.empty() && gfile.find_first_of("\\:") != std::string::npos && gfile.find_first_of("*?") == std::string::npos) {
        std::string full = vfs.full_path(gfile);
        gdir = full.substr(0, full.find_last_of('\\'));
        gfile = full.substr(full.find_last_of('\\') + 1);
      }
      std::string hdir = gdir.empty() ? vfs.to_host(vfs.cwd()) : vfs.to_host(gdir);
      std::wstring whdir = widen(hdir);
      std::vector<wchar_t> buf(std::max<uint32_t>(max_file, 260) + 1, L'\0');
      std::wstring wfile = w1252(gfile);
      if (wfile.find_first_of(L"*?") == std::wstring::npos) wcsncpy(buf.data(), wfile.c_str(), buf.size() - 1);
      std::wstring wtitle = w1252(title), wext = w1252(def_ext);
      OPENFILENAMEW o{};
      o.lStructSize = sizeof(o);
      o.hwndOwner = owner_of(d, owner16);
      o.lpstrFilter = filter.size() > 1 ? filter.c_str() : nullptr;
      o.nFilterIndex = filter_index;
      o.lpstrFile = buf.data();
      o.nMaxFile = DWORD(buf.size());
      o.lpstrInitialDir = whdir.empty() ? nullptr : whdir.c_str();
      o.lpstrTitle = wtitle.empty() ? nullptr : wtitle.c_str();
      o.lpstrDefExt = wext.empty() ? nullptr : wext.c_str();
      o.Flags = (flags & (OFN_READONLY | OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST |
                          OFN_FILEMUSTEXIST | OFN_CREATEPROMPT | OFN_NOREADONLYRETURN)) |
                OFN_NOCHANGEDIR | OFN_EXPLORER;
      Dialogs16* saved = g_dlg;
      g_dlg = &d;
      BOOL ok = FALSE;
      {
        OwnerLend lend(o.hwndOwner);
        ok = save ? GetSaveFileNameW(&o) : GetOpenFileNameW(&o);
      }
      g_dlg = saved;
      rethrow(d);
      if (ok) host = buf.data();
      if (ok) rt.wr32(ofn + 20, o.nFilterIndex);
    }
    if (host.empty()) {
      trace("dlg16", "%s cancelled", save ? "GetSaveFileName" : "GetOpenFileName");
      return c.ret(0);
    }
    std::string guest = vfs.host_to_guest(narrow(host));
    if (guest.empty()) {
      log("configure: \"%s\" has no guest path (not on a drive letter)", narrow(host).c_str());
      return c.ret(0);
    }
    log("configure: file dialog: %s -> %s", narrow(host).c_str(), guest.c_str());
    if (file_p && max_file) rt.write_str(file_p, guest, max_file);
    size_t name_at = guest.find_last_of('\\') + 1;
    size_t dot = guest.find_last_of('.');
    rt.wr16(ofn + 52, uint16_t(name_at));
    rt.wr16(ofn + 54, uint16_t(dot != std::string::npos && dot > name_at ? dot + 1 : guest.size()));
    if (title_p && max_title) rt.write_str(title_p, guest.substr(name_at), max_title);
    c.ret(1);
  };
  replace(r, "COMMDLG", "GetOpenFileName", [file_dialog](Call16& c) { file_dialog(c, false); });
  replace(r, "COMMDLG", "GetSaveFileName", [file_dialog](Call16& c) { file_dialog(c, true); });
  // ChooseFont(CHOOSEFONT16*): the real font dialog (SWTEXT's Select Font:
  // CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_LIMITSIZE, 10..100 pt, SWTEXT
  // 1:0B2E..1:0B5E), started from the guest's LOGFONT; on OK the LOGFONT,
  // iPointSize, nFontType and (CF_EFFECTS) rgbColors go back. Hooks,
  // templates, CF_USESTYLE and printer fonts are not supported (the standard
  // screen-font dialog shows, logged). A hidden run has nothing to answer it
  // with (the script has no font line): cancelled, logged.
  replace(r, "COMMDLG", "ChooseFont", [](Call16& c) {
    uint32_t cf = c.ptr();
    Runtime16& rt = c.rt;
    Dialogs16& d = dl(rt);
    uint16_t owner16 = rt.rd16(cf + 4);
    uint32_t lf_p = rt.rd32(cf + 8);
    uint32_t flags = rt.rd32(cf + 0x0E);
    COLORREF color = rt.rd32(cf + 0x12) & 0xFFFFFF;
    int16_t size_min = int16_t(rt.rd16(cf + 0x2A)), size_max = int16_t(rt.rd16(cf + 0x2C));
    d.cfg->shown++;
    if (flags & (CF_ENABLEHOOK | CF_ENABLETEMPLATE | CF_ENABLETEMPLATEHANDLE | CF_USESTYLE | CF_PRINTERFONTS)) {
      log("configure: ChooseFont's hook/template/style/printer flags (%08X) are not supported; the screen-font dialog shows",
          flags);
    }
    if (d.cfg->script->hidden()) {
      log("configure: ChooseFont not shown (ADCONFIGHIDDEN): cancelled");
      return c.ret(0);
    }
    LOGFONTW lf{};
    if (lf_p && (flags & CF_INITTOLOGFONTSTRUCT)) {
      LOGFONT16 f = read16<LOGFONT16>(rt, lf_p);
      lf.lfHeight = f.lfHeight;
      lf.lfWidth = f.lfWidth;
      lf.lfEscapement = f.lfEscapement;
      lf.lfOrientation = f.lfOrientation;
      lf.lfWeight = f.lfWeight;
      lf.lfItalic = f.lfItalic;
      lf.lfUnderline = f.lfUnderline;
      lf.lfStrikeOut = f.lfStrikeOut;
      lf.lfCharSet = f.lfCharSet;
      lf.lfOutPrecision = f.lfOutPrecision;
      lf.lfClipPrecision = f.lfClipPrecision;
      lf.lfQuality = f.lfQuality;
      lf.lfPitchAndFamily = f.lfPitchAndFamily;
      std::wstring face = w1252(std::string(f.lfFaceName, strnlen(f.lfFaceName, sizeof(f.lfFaceName))));
      wcsncpy(lf.lfFaceName, face.c_str(), LF_FACESIZE - 1);
    }
    CHOOSEFONTW o{};
    o.lStructSize = sizeof(o);
    o.hwndOwner = owner_of(d, owner16);
    o.lpLogFont = &lf;
    o.Flags = (flags & (CF_INITTOLOGFONTSTRUCT | CF_EFFECTS | CF_ANSIONLY | CF_NOVECTORFONTS | CF_NOSIMULATIONS |
                        CF_LIMITSIZE | CF_FIXEDPITCHONLY | CF_FORCEFONTEXIST | CF_SCALABLEONLY | CF_TTONLY |
                        CF_NOFACESEL | CF_NOSTYLESEL | CF_NOSIZESEL)) |
              CF_SCREENFONTS;
    if (!lf_p) o.Flags &= ~DWORD(CF_INITTOLOGFONTSTRUCT);
    o.rgbColors = color;
    o.nSizeMin = size_min;
    o.nSizeMax = size_max;
    Dialogs16* saved = g_dlg;
    g_dlg = &d;
    BOOL ok = FALSE;
    {
      OwnerLend lend(o.hwndOwner);
      ok = ChooseFontW(&o);
    }
    g_dlg = saved;
    rethrow(d);
    if (!ok) {
      trace("dlg16", "ChooseFont cancelled");
      return c.ret(0);
    }
    LOGFONT16 f{};
    f.lfHeight = int16_t(lf.lfHeight);
    f.lfWidth = int16_t(lf.lfWidth);
    f.lfEscapement = int16_t(lf.lfEscapement);
    f.lfOrientation = int16_t(lf.lfOrientation);
    f.lfWeight = int16_t(lf.lfWeight);
    f.lfItalic = lf.lfItalic;
    f.lfUnderline = lf.lfUnderline;
    f.lfStrikeOut = lf.lfStrikeOut;
    f.lfCharSet = lf.lfCharSet;
    f.lfOutPrecision = lf.lfOutPrecision;
    f.lfClipPrecision = lf.lfClipPrecision;
    f.lfQuality = lf.lfQuality;
    f.lfPitchAndFamily = lf.lfPitchAndFamily;
    std::string face = a1252(lf.lfFaceName);
    memcpy(f.lfFaceName, face.c_str(), std::min<size_t>(face.size(), sizeof(f.lfFaceName) - 1));
    if (lf_p) write16(rt, lf_p, f);
    rt.wr16(cf + 0x0C, uint16_t(o.iPointSize));
    if (flags & CF_EFFECTS) rt.wr32(cf + 0x12, o.rgbColors);
    rt.wr16(cf + 0x28, uint16_t(o.nFontType));
    log("configure: font dialog: \"%s\", %d.%d pt", face.c_str(), o.iPointSize / 10, o.iPointSize % 10);
    c.ret(1);
  });
  // WinExec("notepad <file>"): NONSENSE's Edit Names hands its word list to
  // Notepad. The file is copied into the upper layer first, so the edits land
  // in the per-user state; the real Notepad opens that copy (not when hidden).
  replace(r, "KERNEL", "WinExec", [](Call16& c) {
    std::string cmd = c.rt.read_str(c.ptr());
    c.w();
    Dialogs16& d = dl(c.rt);
    std::string prog = cmd.substr(0, cmd.find(' '));
    std::string arg = cmd.size() > prog.size() ? cmd.substr(prog.size() + 1) : std::string();
    std::string base = upper(prog.substr(prog.find_last_of("\\/:") == std::string::npos ? 0 : prog.find_last_of("\\/:") + 1));
    if (base != "NOTEPAD" && base != "NOTEPAD.EXE") {
      log("configure: WinExec(\"%s\") refused", cmd.c_str());
      return c.ret(2);
    }
    win32::Vfs& vfs = c.rt.vfs();
    std::string guest = vfs.full_path(arg);
    std::string bytes;
    uint32_t err = 0;
    if (vfs.read_file(guest, &bytes) && !vfs.write_file(guest, bytes, &err)) {
      // Never hand Notepad the read-only layer (the imported package).
      log("configure: %s cannot be copied into the state (error %u); Notepad not started", guest.c_str(), err);
      return c.ret(2);
    }
    std::string host = vfs.to_host(guest);
    d.cfg->shown++;
    if (host.empty()) {
      log("configure: Notepad on %s: the file lives in memory (no ADSTATE); not started", guest.c_str());
      return c.ret(33);
    }
    if (d.cfg->script->hidden()) {
      log("configure: Notepad on %s (%s) not started (ADCONFIGHIDDEN)", guest.c_str(), host.c_str());
      return c.ret(33);
    }
    // The system's Notepad by full path: a bare "notepad.exe" would also be
    // looked for in this program's folder and the current directory.
    wchar_t sysdir[MAX_PATH] = {};
    UINT sn = GetSystemDirectoryW(sysdir, MAX_PATH);
    if (!sn || sn >= MAX_PATH) return c.ret(2);
    std::wstring exe = std::wstring(sysdir) + L"\\notepad.exe";
    std::wstring line = L"\"" + exe + L"\" \"" + widen(host) + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
      log("configure: Notepad did not start (error %lu)", GetLastError());
      return c.ret(2);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    log("configure: Notepad on %s", host.c_str());
    c.ret(33);
  });
  // EnumFonts(hdc, lpFaceName, lpFontFunc, lpData): the faces a Windows 95
  // machine had, those this host has too (MESSAGE3 lists them in its Font
  // box); FontFunc(LOGFONT FAR*, TEXTMETRIC FAR*, FontType, lpData) per face,
  // or per face match when one is named. (The saver keeps calling nothing back.)
  replace(r, "GDI", "EnumFonts", [](Call16& c) {
    c.w();
    uint32_t face_p = c.ptr();
    uint32_t proc = c.ptr();
    uint32_t data = c.l();
    std::string want = face_p ? c.rt.read_str(face_p) : std::string();
    static const char* kFaces[] = {"Arial",       "Courier",   "Courier New", "Fixedsys", "Modern",          "MS Sans Serif",
                                   "MS Serif",    "Roman",     "Script",      "Small Fonts", "Symbol",       "System",
                                   "Terminal",    "Times New Roman", "Wingdings"};
    Dialogs16& d = dl(c.rt);
    ScratchMark mark(d);
    uint32_t lf16 = scratch(d, sizeof(LOGFONT16)), tm16 = scratch(d, sizeof(TEXTMETRIC16));
    HDC screen = GetDC(nullptr);
    uint16_t result = 1;
    for (const char* face : kFaces) {
      if (!want.empty() && _stricmp(want.c_str(), face) != 0) continue;
      LOGFONTW q{};
      q.lfCharSet = DEFAULT_CHARSET;
      std::wstring wf = w1252(face);
      wcsncpy(q.lfFaceName, wf.c_str(), LF_FACESIZE - 1);
      struct Found {
        bool any = false;
        LOGFONTW lf{};
        TEXTMETRICW tm{};
        DWORD type = 0;
      } found;
      EnumFontFamiliesExW(
          screen, &q,
          [](const LOGFONTW* lf, const TEXTMETRICW* tm, DWORD type, LPARAM p) -> int {
            Found* f = reinterpret_cast<Found*>(p);
            f->any = true;
            f->lf = *lf;
            f->tm = *tm;
            f->type = type;
            return 0;
          },
          LPARAM(&found), 0);
      if (!found.any) continue;
      LOGFONT16 l{};
      l.lfHeight = int16_t(found.lf.lfHeight);
      l.lfWidth = int16_t(found.lf.lfWidth);
      l.lfWeight = int16_t(found.lf.lfWeight);
      l.lfItalic = found.lf.lfItalic;
      l.lfCharSet = found.lf.lfCharSet;
      l.lfOutPrecision = found.lf.lfOutPrecision;
      l.lfClipPrecision = found.lf.lfClipPrecision;
      l.lfQuality = found.lf.lfQuality;
      l.lfPitchAndFamily = found.lf.lfPitchAndFamily;
      strncpy(l.lfFaceName, face, sizeof(l.lfFaceName) - 1);
      TEXTMETRIC16 t{};
      t.tmHeight = int16_t(found.tm.tmHeight);
      t.tmAscent = int16_t(found.tm.tmAscent);
      t.tmDescent = int16_t(found.tm.tmDescent);
      t.tmInternalLeading = int16_t(found.tm.tmInternalLeading);
      t.tmExternalLeading = int16_t(found.tm.tmExternalLeading);
      t.tmAveCharWidth = int16_t(found.tm.tmAveCharWidth);
      t.tmMaxCharWidth = int16_t(found.tm.tmMaxCharWidth);
      t.tmWeight = int16_t(found.tm.tmWeight);
      t.tmItalic = found.tm.tmItalic;
      t.tmPitchAndFamily = found.tm.tmPitchAndFamily;
      t.tmCharSet = found.tm.tmCharSet;
      t.tmFirstChar = 0x20;
      t.tmLastChar = 0xFF;
      t.tmDefaultChar = 0x80;
      t.tmBreakChar = 0x20;
      write16(c.rt, lf16, l);
      write16(c.rt, tm16, t);
      uint16_t type = uint16_t((found.type & TRUETYPE_FONTTYPE) ? TRUETYPE_FONTTYPE : (found.type & RASTER_FONTTYPE) ? RASTER_FONTTYPE : 0);
      result = uint16_t(c.rt.call_far(proc, {l16(lf16), l16(tm16), w16(type), l16(data)}));
      if (!result) break;
    }
    ReleaseDC(nullptr, screen);
    c.ret(result);
  });
  // The guest's own message loops keep the real windows alive: PeekMessage
  // pumps what the real queue holds; GetMessage, when the guest's own queue
  // has nothing, waits for real input first (no hot spin while a modeless
  // dialog is up).
  auto pump = [](Dialogs16& d, bool wait) {
    if (d.wnds.empty()) return;
    MSG m;
    if (wait && !PeekMessageW(&m, nullptr, 0, 0, PM_NOREMOVE)) MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
    for (int i = 0; i < 16 && PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE); i++) {
      HWND top = m.hwnd ? GetAncestor(m.hwnd, GA_ROOT) : nullptr;
      if (top && IsDialogMessageW(top, &m)) continue;
      TranslateMessage(&m);
      DispatchMessageW(&m);
    }
    rethrow(d);
  };
  wrap(r, U, "PeekMessage", [pump](Call16& c) {
    pump(dl(c.rt), false);
    return false;
  });
  wrap(r, U, "GetMessage", [pump](Call16& c) {
    uint32_t out = c.ptr();
    uint16_t hwnd = c.w(), lo = c.w(), hi = c.w();
    // Only when the guest's own queue has nothing for this filter.
    uint16_t ds = c.rt.global().alloc(0, 32);
    uint32_t tmp = ds ? c.rt.global().lock(ds) : 0;
    bool have = false;
    if (tmp) {
      Shim16Entry* peek = c.rt.shims().find_name(U, "PeekMessage");
      have = c.rt.call_far(c.rt.thunk_far(*peek), {l16(tmp), w16(hwnd), w16(lo), w16(hi), w16(PM_NOREMOVE)}) & 0xFFFF;
      c.rt.global().free(ds);
    }
    (void)out;
    if (!have) pump(dl(c.rt), true);
    return false;
  });
}

}  // namespace adw::win16
