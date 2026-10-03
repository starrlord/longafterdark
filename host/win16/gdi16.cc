// GDI (and USER's palette pair, SelectPalette/RealizePalette = USER.282/283)
// on real GDI over 8-bit key surfaces (gdi16.hh; API_SURFACE.md §2 "GDI",
// 97 imports).
//
// Rules every shim follows:
//   * a guest COLORREF reaches real GDI only as Gdi16::key(hdc, c), and a DC
//     is sync()ed (brush/pen re-made for its palette, text colours keyed)
//     before real GDI draws with it;
//   * every Win16 surface holds hardware palette indices (Win16 has no DIB
//     sections), so blits between them are plain real BitBlt/StretchBlt —
//     except a DIB driver DC's (CreateDC("DIB"), gdi16.hh) and a memory DC
//     made compatible with one, whose pixel values are that DIB's own
//     indices: colours reach them through Gdi16::dib_index (DIB.DRV's
//     matching), and blits still copy values unchanged;
//   * device-independent bits (SetDIBitsToDevice, StretchDIBits, SetDIBits,
//     CreateDIBitmap) are translated to hardware indices first — through the
//     DC's palette, as a Win95 palette device matched them; on a DC with DIB
//     colour semantics as DIB.DRV translated between two colour tables — and
//     handed to real GDI as an 8bpp DIB with the key table, so real GDI still
//     does the geometry (clipping, mirroring, stretching, ROPs); into a
//     monochrome bitmap (SetDIBits, CreateDIBitmap, and SetDIBitsToDevice or
//     StretchDIBits on a memory DC holding one) real GDI gets the DIB's own
//     colour table instead (real_bmi) and applies its own rule: 1 only for
//     the table's entry nearest white (the first of equal ones), 0 for every
//     other entry (with the key table only index 255 would be 1: Star Trek's
//     masks came out black);
//   * colours read back (GetPixel, GetDIBits, GetNearestColor) are converted
//     from indices to what they mean;
//   * a flood fill (FloodFill, ExtFloodFill) compares pixel indices: its
//     colour is keyed as SetPixel's, and real GDI fills.
//
// No module of the corpus draws DIB bits onto a DIB DC, or onto a memory DC
// compatible with one, nor asks one for GetNearestColor (the 14 SWSE modules
// traced): those branches are held by win16.unit's test_dib_translation, not
// by any module's stream.
//
// Known gaps, deliberately left (no module of the supported releases needs
// more, except for GetDIBits rows, below; API_SURFACE.md §2 GDI):
//   * the DIB driver takes 8-bit DIBs only (DIB.DRV also took 1 and 4 bits
//     per pixel: refused and logged). On a DIB DC whose colour table is
//     RGBQUADs (SWSE's canvases hold index tables) a DIB pattern brush keeps
//     its indices, and DIB bits with an RGB table always go to their nearest
//     entries; DIB.DRV matched the pattern's colours too, and kept the
//     indices when the two tables were equal (it differs where a table
//     repeats a colour).
//   * EnumFonts calls nothing back (MESSAGE3 only).
//   * GetDIBits writes 4-, 8- and 24-bit rows (a monochrome bitmap's black
//     and white as hardware indices 0 and 255, real GDI's values) and 1-bit
//     rows of monochrome bitmaps only (real GDI's, asked in a 40-byte header
//     of our own); 16- and 32-bit requests are refused. Its rows are the
//     bitmap's own width and height whatever the header says, where real
//     GDI follows the header (measured): Haunted asks for 640 pixels of each
//     648-pixel line and gets 648 (4 bytes more per line, which the next
//     line overwrites), and ADXPL40 and ADXPL41 (33 modules of Totally
//     Twisted, the 10th Anniversary and Looney Tunes) ask for 24 scans of a
//     76-row bitmap with a 24-row header and get its bottom rows where real
//     GDI gives its top ones: the frozen streams hold the host's answer, and
//     in Chameleon a stray icon covers the "Accessories" label after half a
//     minute.
//     With DIB_PAL_COLORS the bits stay hardware indices and the colour
//     table describes them (entry h: the DC palette's logical entry nearest
//     hardware colour h, GetNearestPaletteIndex's rule), so a SetDIBits
//     through the same palette gives the colours back; Windows NT (and Wine)
//     return the logical indices themselves, which no module here needs
//     (tried: ADXPL41's white labels turned cyan, SIMPTRIV moved).
//   * CreatePatternBrush keeps a copy of the bitmap's top-left 8×8, as
//     Windows 3.1 and 95 did; CreateBrushIndirect's BS_PATTERN still paints
//     the guest's bitmap itself, whole, as before (its one known caller,
//     GUTS, makes solid brushes).
//   * Mapping modes other than MM_TEXT are passed to real GDI untested
//     (WMORPH only; ScreamSavers copy the screen DC's MM_TEXT).
#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <unordered_map>
#include <vector>

#include "adw/core/log.h"
#include "win16/dos16.hh"
#include "win16/gdi16.hh"
#include "win16/shim_families16.hh"

namespace adw::win16 {

using win32::Display;
using win32::LogicalPalette;

namespace {

constexpr const char* G = "GDI";

Gdi16& gt(Runtime16& rt) { return rt.state<Gdi16>(); }
Gdi16& gt(Call16& c) { return c.rt.state<Gdi16>(); }

DWORD pack_point(POINT p) { return (uint32_t(uint16_t(p.y)) << 16) | uint16_t(p.x); }
DWORD pack_size(SIZE s) { return (uint32_t(uint16_t(s.cy)) << 16) | uint16_t(s.cx); }

// ---- guest DIBs ------------------------------------------------------------------------------------

struct Dib16 {
  BITMAPINFOHEADER h{};
  std::vector<RGBQUAD> colors;  // DIB_RGB_COLORS
  std::vector<uint16_t> pal;    // DIB_PAL_COLORS
  uint32_t stride = 0;
  int w = 0, height = 0;  // |biHeight|
  bool top_down = false;
};

bool read_dib(Runtime16& rt, uint32_t bmi, uint16_t usage, Dib16& d) {
  if (!bmi) return false;
  uint32_t size = rt.rd32(bmi);
  uint32_t ncolors = 0;
  uint32_t table;
  bool core = size == sizeof(BITMAPCOREHEADER);
  if (core) {
    BITMAPCOREHEADER ch = read16<BITMAPCOREHEADER>(rt, bmi);
    d.h.biSize = sizeof(BITMAPINFOHEADER);
    d.h.biWidth = ch.bcWidth;
    d.h.biHeight = ch.bcHeight;
    d.h.biPlanes = 1;
    d.h.biBitCount = ch.bcBitCount;
    d.h.biCompression = BI_RGB;
    ncolors = ch.bcBitCount <= 8 ? 1u << ch.bcBitCount : 0;
    table = bmi + size;
  } else if (size >= sizeof(BITMAPINFOHEADER) && size < 0x1000) {
    d.h = read16<BITMAPINFOHEADER>(rt, bmi);
    ncolors = d.h.biBitCount <= 8 && d.h.biBitCount ? (d.h.biClrUsed ? std::min<uint32_t>(d.h.biClrUsed, 256)
                                                                     : 1u << d.h.biBitCount)
                                                    : 0;
    table = bmi + size;
  } else {
    return false;
  }
  for (uint32_t i = 0; i < ncolors; i++) {
    if (usage == DIB_PAL_COLORS) {
      d.pal.push_back(rt.rd16(table + 2 * i));
    } else if (core) {
      uint32_t a = table + 3 * i;
      d.colors.push_back(RGBQUAD{rt.rd8(a), rt.rd8(a + 1), rt.rd8(a + 2), 0});
    } else {
      uint32_t v = rt.rd32(table + 4 * i);
      d.colors.push_back(RGBQUAD{BYTE(v), BYTE(v >> 8), BYTE(v >> 16), 0});
    }
  }
  d.w = d.h.biWidth;
  d.height = d.h.biHeight < 0 ? -d.h.biHeight : d.h.biHeight;
  d.top_down = d.h.biHeight < 0;
  d.stride = uint32_t(((d.w * d.h.biBitCount + 31) / 32) * 4);
  return d.w > 0 && d.height > 0 && d.w <= 16384 && d.height <= 16384;
}

// Colour index of a DIB → hardware index when drawn through `hdc` — or, on
// a DC with DIB colour semantics (a DIB DC), → that DIB's pixel value.
std::array<uint8_t, 256> dib_xlate(Gdi16& g, const Dib16& d, uint16_t usage, uint16_t hdc) {
  std::array<uint8_t, 256> x{};
  Display& disp = g.display();
  LogicalPalette* pal = g.dc_palette(hdc);
  size_t n = usage == DIB_PAL_COLORS ? d.pal.size() : d.colors.size();
  if (Dc16* dc = g.dc(hdc); dc && dc->dib_header) {
    // DIB.DRV's translation between two colour tables (1:0653): the identity
    // when the destination has an index table (SWSE's canvases) or none, or
    // the source's is an identity index table; else each source colour's
    // nearest entry. A DIB_PAL_COLORS source's colours are its indices'
    // entries in the DC's palette, as GDI gave a non-palette device RGBs.
    bool src_identity = usage == DIB_PAL_COLORS;
    for (size_t i = 0; i < d.pal.size() && src_identity; i++) src_identity = d.pal[i] == i;
    bool identity = src_identity || g.dib_table(dc->dib_header).vga;
    for (size_t i = 0; i < 256; i++) {
      if (identity || i >= n) {
        x[i] = uint8_t(identity ? i : 0);
        continue;
      }
      COLORREF rgb;
      if (usage == DIB_PAL_COLORS) {
        size_t e = d.pal[i];
        rgb = pal && e < pal->entries.size() ? RGB(pal->entries[e].peRed, pal->entries[e].peGreen, pal->entries[e].peBlue) : 0;
      } else {
        rgb = RGB(d.colors[i].rgbRed, d.colors[i].rgbGreen, d.colors[i].rgbBlue);
      }
      x[i] = uint8_t(g.dib_index(*dc, rgb));
    }
    return x;
  }
  for (size_t i = 0; i < 256; i++) {
    if (i >= n) {
      x[i] = 0;
      continue;
    }
    if (usage == DIB_PAL_COLORS) {
      x[i] = uint8_t(disp.map_index(0x01000000 | d.pal[i], pal));
    } else {
      const RGBQUAD& q = d.colors[i];
      x[i] = uint8_t(disp.device_index_for_rgb(RGB(q.rgbRed, q.rgbGreen, q.rgbBlue), pal));
    }
  }
  return x;
}

// Expands RLE8/RLE4 bits into rows (bottom-up order, as the DIB stores them).
std::vector<uint8_t> decode_rle(const uint8_t* src, size_t src_size, const Dib16& d) {
  bool rle4 = d.h.biCompression == BI_RLE4;
  uint32_t stride = uint32_t((d.w + 3) & ~3);  // decoded into 8 bits per pixel
  std::vector<uint8_t> out(size_t(stride) * d.height, 0);
  int x = 0, y = 0;
  size_t p = 0;
  auto put = [&](int v) {
    if (x < d.w && y < d.height) out[size_t(y) * stride + size_t(x)] = uint8_t(v);
    x++;
  };
  while (p + 1 < src_size && y < d.height) {
    uint8_t n = src[p], v = src[p + 1];
    p += 2;
    if (n) {
      for (int i = 0; i < n; i++) put(rle4 ? ((i & 1) ? (v & 0xF) : (v >> 4)) : v);
    } else if (v == 0) {
      x = 0;
      y++;
    } else if (v == 1) {
      break;
    } else if (v == 2) {
      if (p + 1 >= src_size) break;
      x += src[p];
      y += src[p + 1];
      p += 2;
    } else {
      for (int i = 0; i < v && p < src_size; i++) {
        uint8_t b = src[p + (rle4 ? size_t(i / 2) : size_t(i))];
        put(rle4 ? ((i & 1) ? (b & 0xF) : (b >> 4)) : b);
      }
      size_t used = rle4 ? (size_t(v) + 1) / 2 : v;
      p += (used + 1) & ~size_t(1);
    }
  }
  return out;
}

// `lines` rows of guest DIB bits → 8bpp hardware indices, same orientation,
// DWORD rows. Deep (16/24/32-bit) pixels are matched through the DC's palette.
// Only rows [row_lo, row_hi) (in memory order) are translated — a blit of a
// small rectangle out of a full-screen DIB (ADXPL300's dirty rectangles)
// needs no more; the others stay 0.
std::vector<uint8_t> dib_to_indices(Runtime16& rt, Gdi16& g, const Dib16& d, uint32_t bits, int lines,
                                    uint16_t usage, uint16_t hdc, int row_lo = 0, int row_hi = 0x7FFFFFFF) {
  uint32_t out_stride = uint32_t((d.w + 3) & ~3);
  std::vector<uint8_t> out(size_t(out_stride) * std::max(lines, 0), 0);
  if (lines <= 0 || !bits) return out;
  auto x = dib_xlate(g, d, usage, hdc);
  if (d.h.biCompression == BI_RLE8 || d.h.biCompression == BI_RLE4) {
    uint32_t n = d.h.biSizeImage ? d.h.biSizeImage : 0x10000;
    // The RLE stream: at most biSizeImage bytes, but never past its block.
    uint32_t lin = rt.linear(bits, 1);
    uint32_t avail = rt.ldt().limit_of(uint16_t(bits >> 16)) - (bits & 0xFFFF) + 1;
    const uint8_t* src = rt.mem().at<uint8_t>(lin, std::min(n, avail));
    std::vector<uint8_t> flat = decode_rle(src, std::min(n, avail), d);
    for (int y = 0; y < lines && y < d.height; y++) {
      for (int i = 0; i < d.w; i++) out[size_t(y) * out_stride + size_t(i)] = x[flat[size_t(y) * out_stride + size_t(i)]];
    }
    return out;
  }
  uint32_t lin = rt.linear(bits, d.stride * uint32_t(lines));
  const uint8_t* src = rt.mem().at<uint8_t>(lin, size_t(d.stride) * lines);
  Display& disp = g.display();
  LogicalPalette* pal = g.dc_palette(hdc);
  Dc16* dib_dc = g.dc(hdc);
  if (dib_dc && !dib_dc->dib_header) dib_dc = nullptr;
  std::unordered_map<uint32_t, uint8_t> cache;
  auto deep = [&](uint8_t r, uint8_t gg, uint8_t b) {
    uint32_t k = RGB(r, gg, b);
    auto it = cache.find(k);
    if (it != cache.end()) return it->second;
    uint8_t v = uint8_t(dib_dc ? g.dib_index(*dib_dc, k) : disp.device_index_for_rgb(k, pal));
    cache[k] = v;
    return v;
  };
  for (int y = std::max(row_lo, 0); y < std::min(lines, row_hi); y++) {
    const uint8_t* s = src + size_t(y) * d.stride;
    uint8_t* o = out.data() + size_t(y) * out_stride;
    switch (d.h.biBitCount) {
      case 1:
        for (int i = 0; i < d.w; i++) o[i] = x[(s[i >> 3] >> (7 - (i & 7))) & 1];
        break;
      case 4:
        for (int i = 0; i < d.w; i++) o[i] = x[(s[i >> 1] >> ((i & 1) ? 0 : 4)) & 0xF];
        break;
      case 8:
        for (int i = 0; i < d.w; i++) o[i] = x[s[i]];
        break;
      case 16:
        for (int i = 0; i < d.w; i++) {
          uint16_t v = uint16_t(s[2 * i] | (s[2 * i + 1] << 8));
          o[i] = deep(uint8_t(((v >> 10) & 31) << 3), uint8_t(((v >> 5) & 31) << 3), uint8_t((v & 31) << 3));
        }
        break;
      case 24:
        for (int i = 0; i < d.w; i++) o[i] = deep(s[3 * i + 2], s[3 * i + 1], s[3 * i]);
        break;
      case 32:
        for (int i = 0; i < d.w; i++) o[i] = deep(s[4 * i + 2], s[4 * i + 1], s[4 * i]);
        break;
      default:
        break;
    }
  }
  return out;
}

// The BITMAPINFO real GDI gets for translated bits: 8bpp, the key table.
struct KeyBmi {
  BITMAPINFOHEADER h;
  RGBQUAD c[256];
};
KeyBmi key_bmi(const Dib16& d) {
  KeyBmi b{};
  b.h.biSize = sizeof(BITMAPINFOHEADER);
  b.h.biWidth = d.w;
  b.h.biHeight = d.top_down ? -d.height : d.height;
  b.h.biPlanes = 1;
  b.h.biBitCount = 8;
  b.h.biCompression = BI_RGB;
  b.h.biClrUsed = 256;
  memcpy(b.c, Display::key_table().data(), sizeof(b.c));
  return b;
}

// The BITMAPINFO real GDI gets for a DIB's own bits into a monochrome target:
// the DIB's colours (a DIB_PAL_COLORS table's entries as the DC's palette maps
// them). Real GDI then sets to 1 only the pixels of the table's entry nearest
// white (the first of equal ones) and every other entry to 0, however light
// (beside a white, yellow and light grey are 0; test_mono_dib_targets). The
// key table would not do: its entry i is the grey RGB(i, i, i), so only
// hardware index 255 would be 1 (a white that the selected palette holds at
// slot 24 would come out black).
struct RealBmi {
  BITMAPINFOHEADER h;
  RGBQUAD c[256];
};
RealBmi real_bmi(Gdi16& g, const Dib16& d, uint16_t hdc) {
  RealBmi bi{};
  bi.h = d.h;
  bi.h.biSize = sizeof(BITMAPINFOHEADER);
  for (size_t i = 0; i < d.colors.size() && i < 256; i++) bi.c[i] = d.colors[i];
  for (size_t i = 0; i < d.pal.size() && i < 256; i++) {
    COLORREF c = g.index_rgb(g.display().map_index(0x01000000 | d.pal[i], g.dc_palette(hdc)));
    bi.c[i] = RGBQUAD{GetBValue(c), GetGValue(c), GetRValue(c), 0};
  }
  return bi;
}

// A memory DC whose selected bitmap is monochrome (and an uncompressed DIB
// for it): SetDIBitsToDevice and StretchDIBits hand real GDI the DIB's own
// bits and colours there (real_bmi) instead of hardware indices. Star Trek's
// AD_MOD.DLL makes its masks so: Scotty's Files' blueprints are 1-bpp DIBs,
// white on black, stretched into monochrome bitmaps.
bool mono_target(Gdi16& g, uint16_t hdc, const Dib16& d) {
  Dc16* dc = g.dc(hdc);
  if (!dc || dc->screen || dc->dib_device || dc->dib_header || d.h.biCompression != BI_RGB) return false;
  Obj16* b = dc->s.bitmap ? g.get(dc->s.bitmap, G16::bitmap) : nullptr;
  return b && b->bmp.bpp == 1;
}

// Writes translated DIB rows into an 8-bit device bitmap (SetDIBits, CreateDIBitmap).
int set_bitmap_rows(Runtime16& rt, Gdi16& g, Obj16& bmp, uint16_t hdc, uint16_t start, uint16_t lines, uint32_t bits,
                    const Dib16& d, uint16_t usage) {
  if (bmp.bmp.bpp == 1 || !bmp.bmp.bits) {
    // A monochrome target: hand real GDI the DIB with real colours.
    std::vector<uint8_t> raw(size_t(d.stride) * lines);
    rt.read_bytes(bits, raw.data(), raw.size());
    RealBmi bi = real_bmi(g, d, hdc);
    HDC screen = GetDC(nullptr);
    int r = SetDIBits(screen, static_cast<HBITMAP>(bmp.host), start, lines, raw.data(),
                      reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS);
    ReleaseDC(nullptr, screen);
    return r;
  }
  GdiFlush();
  std::vector<uint8_t> idx = dib_to_indices(rt, g, d, bits, lines, usage, hdc);
  uint32_t istride = uint32_t((d.w + 3) & ~3);
  int w = std::min(d.w, bmp.bmp.w);
  for (int i = 0; i < lines; i++) {
    int scan = start + i;  // counted from the bottom for a bottom-up DIB
    int y = d.top_down ? scan : d.height - 1 - scan;
    // A DIB taller or shorter than the bitmap lines up at the bottom (bottom-up) / top (top-down).
    if (!d.top_down) y -= d.height - bmp.bmp.h;
    if (y < 0 || y >= bmp.bmp.h) continue;
    memcpy(bmp.bmp.bits + size_t(y) * bmp.bmp.stride, idx.data() + size_t(i) * istride, size_t(w));
  }
  return lines;
}

// A blit ROP that reads the source / the pattern.
bool rop_uses_src(DWORD rop) { return ((rop >> 2) ^ rop) & 0x330000; }
bool rop_uses_pat(DWORD rop) { return ((rop >> 4) ^ rop) & 0x0F0000; }

// Reads an array of POINT16s.
std::vector<POINT> read_points(Runtime16& rt, uint32_t fp, int n) {
  std::vector<POINT> pts;
  for (int i = 0; i < n && i < 8192; i++) {
    POINT16 p = read16<POINT16>(rt, fp + 4u * uint32_t(i));
    pts.push_back(POINT{p.x, p.y});
  }
  return pts;
}

LOGFONTA to_logfont(const LOGFONT16& f) {
  LOGFONTA lf{};
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
  // Anti-aliased text would put colours between the key colours into an
  // 8-bit surface — i.e. arbitrary palette indices. 1996 GDI never smoothed.
  lf.lfQuality = NONANTIALIASED_QUALITY;
  lf.lfPitchAndFamily = f.lfPitchAndFamily;
  memcpy(lf.lfFaceName, f.lfFaceName, 31);
  lf.lfFaceName[31] = 0;
  return lf;
}

uint16_t make_font(Gdi16& g, const LOGFONTA& lf) {
  HFONT f = CreateFontIndirectA(&lf);
  if (!f) return 0;
  Obj16 o;
  o.type = G16::font;
  o.host = f;
  o.font = lf;
  return g.add(o);
}

HRGN region_of(Gdi16& g, uint16_t h) { return static_cast<HRGN>(g.get(h, G16::region) ? g.host(h) : nullptr); }

// CreateBitmap (and CreateBitmapIndirect): a w×h device bitmap, monochrome
// when planes × bits per pixel is 1 and an 8-bit key surface otherwise, set
// from device-dependent bits (WORD-aligned rows, top-down) when there are
// any — a monochrome or 8-bit bitmap's; other depths' bits are not read.
uint16_t create_bitmap16(Runtime16& rt, int16_t w, int16_t h, uint16_t planes, uint16_t bpp, uint32_t bits) {
  Gdi16& g = gt(rt);
  int depth = planes * bpp == 1 ? 1 : 8;
  uint16_t hb = g.create_device_bitmap(w, h, depth);
  Obj16* o = g.get(hb, G16::bitmap);
  if (o && bits) {
    uint32_t src_stride = uint32_t(((std::max<int>(w, 1) * planes * bpp + 15) / 16) * 2);
    std::vector<uint8_t> raw(size_t(src_stride) * std::max<int>(h, 1));
    rt.read_bytes(bits, raw.data(), raw.size());
    if (depth == 1) {
      SetBitmapBits(static_cast<HBITMAP>(o->host), DWORD(raw.size()), raw.data());
    } else if (bpp == 8) {
      for (int y = 0; y < o->bmp.h; y++) memcpy(o->bmp.bits + size_t(y) * o->bmp.stride, raw.data() + size_t(y) * src_stride, size_t(o->bmp.w));
    }
  }
  return hb;
}

// The colour table real GDI writes for GetDIBits' 4-bit rows, whatever the
// bitmap (measured: Windows 11 on an 8-bit key surface): the 16 VGA
// colours with dark grey before light grey (DIB.DRV's table, gdi16_objects.cc,
// has them the other way round). Each pixel becomes its nearest entry
// (squared distance, the first of equals — as real GDI chose for all 32
// colours probed, ties included).
constexpr RGBQUAD kDib4Colors[16] = {
    {0, 0, 0, 0},          {0, 0, 0x80, 0},    {0, 0x80, 0, 0},    {0, 0x80, 0x80, 0},
    {0x80, 0, 0, 0},       {0x80, 0, 0x80, 0}, {0x80, 0x80, 0, 0}, {0x80, 0x80, 0x80, 0},
    {0xC0, 0xC0, 0xC0, 0}, {0, 0, 0xFF, 0},    {0, 0xFF, 0, 0},    {0, 0xFF, 0xFF, 0},
    {0xFF, 0, 0, 0},       {0xFF, 0, 0xFF, 0}, {0xFF, 0xFF, 0, 0}, {0xFF, 0xFF, 0xFF, 0}};

// ---- flood fills -------------------------------------------------------------------------------------

// A DC's clip region as a w×h mask of device pixels (1 inside); empty when
// the DC has none.
std::vector<uint8_t> clip_mask(HDC h, int w, int ht) {
  std::vector<uint8_t> m;
  HRGN rgn = CreateRectRgn(0, 0, 0, 0);
  if (rgn && GetClipRgn(h, rgn) == 1) {
    m.assign(size_t(w) * size_t(ht), 0);
    DWORD n = GetRegionData(rgn, 0, nullptr);
    std::vector<uint8_t> buf(n);
    RGNDATA* rd = reinterpret_cast<RGNDATA*>(buf.data());
    if (n >= sizeof(RGNDATAHEADER) && GetRegionData(rgn, n, rd) == n) {
      const RECT* rc = reinterpret_cast<const RECT*>(rd->Buffer);
      for (DWORD i = 0; i < rd->rdh.nCount; i++) {
        int l = std::max<int>(rc[i].left, 0), r = std::min<int>(rc[i].right, w);
        for (int y = std::max<int>(rc[i].top, 0); y < std::min<int>(rc[i].bottom, ht) && l < r; y++) {
          uint8_t* row = m.data() + size_t(y) * size_t(w);
          std::fill(row + l, row + r, uint8_t(1));
        }
      }
    }
  }
  if (rgn) DeleteObject(rgn);
  return m;
}

// The pixels a flood fill from device point (x, y) paints, as real GDI
// fills: the 4-connected area of pixels whose value is v (surface) or is not
// v (border), inside the w×h surface and the clip mask — none when (x, y)
// itself is not in it (test_flood_fill holds the count to what real GDI
// painted). px(x, y) reads a pixel's value.
template <typename Px>
int64_t flood_area(int w, int h, const std::vector<uint8_t>& clip, Px px, int x, int y, int v, bool surface) {
  auto inside = [&](int xx, int yy) {
    return (clip.empty() || clip[size_t(yy) * size_t(w) + size_t(xx)]) && ((px(xx, yy) == v) == surface);
  };
  if (x < 0 || y < 0 || x >= w || y >= h || !inside(x, y)) return 0;
  std::vector<uint8_t> done(size_t(w) * size_t(h), 0);
  std::vector<POINT> seeds{POINT{x, y}};
  int64_t n = 0;
  while (!seeds.empty()) {
    POINT s = seeds.back();
    seeds.pop_back();
    size_t row = size_t(s.y) * size_t(w);
    if (done[row + size_t(s.x)]) continue;
    // Its whole run on the row (a run is marked at once, so a seed inside
    // one already taken is skipped above), then one seed per run of the
    // rows above and below that touches it.
    int l = s.x, r = s.x;
    while (l > 0 && inside(l - 1, s.y)) l--;
    while (r + 1 < w && inside(r + 1, s.y)) r++;
    std::fill(done.begin() + ptrdiff_t(row + size_t(l)), done.begin() + ptrdiff_t(row + size_t(r) + 1), uint8_t(1));
    n += r - l + 1;
    for (int ny : {s.y - 1, s.y + 1}) {
      if (ny < 0 || ny >= h) continue;
      size_t nrow = size_t(ny) * size_t(w);
      for (int i = l; i <= r; i++) {
        if (done[nrow + size_t(i)] || !inside(i, ny)) continue;
        seeds.push_back(POINT{i, ny});
        while (i < r && inside(i + 1, ny)) i++;
      }
    }
  }
  return n;
}

// What FloodFill/ExtFloodFill from logical (x, y) of hdc will paint, in
// pixels, read from the surface as the fill compares it: an 8-bit surface's
// values against the key colour's (a DIB DC's in its own row order), a
// monochrome bitmap's bits against the colour's nearest of black and white.
int64_t flood_pixels(Runtime16& rt, Gdi16& g, uint16_t hdc, HDC h, int x, int y, COLORREF key, uint16_t type) {
  Dc16* d = g.dc(hdc);
  Bitmap16* s = g.dc_surface(hdc);
  if (!d || !s || (type != FLOODFILLBORDER && type != FLOODFILLSURFACE)) return 0;
  bool surface = type == FLOODFILLSURFACE;
  POINT p{x, y};
  LPtoDP(h, &p, 1);
  GdiFlush();
  std::vector<uint8_t> clip = clip_mask(h, s->w, s->h);
  if (s->bpp == 1) {
    Obj16* b = g.get(d->s.bitmap, G16::bitmap);
    if (!b || !b->host) return 0;
    std::vector<uint8_t> bits(size_t(s->stride) * size_t(s->h));
    GetBitmapBits(static_cast<HBITMAP>(b->host), LONG(bits.size()), bits.data());
    int v = (GetNearestColor(h, key) & 0xFFFFFF) == 0xFFFFFF ? 1 : 0;
    auto bit = [&](int px, int py) { return (bits[size_t(py) * s->stride + size_t(px >> 3)] >> (7 - (px & 7))) & 1; };
    return flood_area(s->w, s->h, clip, bit, p.x, p.y, v, surface);
  }
  if (!s->bits) return 0;
  bool bottom_up = false;
  if (d->dib_device) {
    try {
      bottom_up = read16<BITMAPINFOHEADER>(rt, d->dib_header).biHeight > 0;
    } catch (const GuestError16&) {
      return 0;  // the DIB is gone
    }
  }
  auto value = [&](int px, int py) {
    return int(s->bits[size_t(bottom_up ? s->h - 1 - py : py) * s->stride + size_t(px)]);
  };
  return flood_area(s->w, s->h, clip, value, p.x, p.y, int(GetRValue(key)), surface);
}

}  // namespace

uint16_t gdi16_bitmap_from_dib(Runtime16& rt, uint16_t hdc, uint32_t packed, uint16_t usage) {
  Gdi16& g = gt(rt);
  Dib16 dib;
  if (!read_dib(rt, packed, usage, dib)) return 0;
  uint32_t hsize = rt.rd32(packed);
  bool core = hsize == sizeof(BITMAPCOREHEADER);
  size_t ncol = usage == DIB_PAL_COLORS ? dib.pal.size() : dib.colors.size();
  uint32_t bits = packed + hsize + uint32_t(ncol * (usage == DIB_PAL_COLORS ? 2 : core ? 3 : 4));
  if (dib.h.biCompression == BI_BITFIELDS && !core) bits += 12;
  uint16_t hb = g.create_device_bitmap(dib.w, dib.height, dib.h.biBitCount == 1 && ncol == 2 && usage != DIB_PAL_COLORS ? 1 : 8);
  Obj16* o = g.get(hb, G16::bitmap);
  if (o) set_bitmap_rows(rt, g, *o, hdc, 0, uint16_t(dib.height), bits, dib, usage);
  return hb;
}

// ---- registration -----------------------------------------------------------------------------------

void register_gdi16(Runtime16& rt) {
  Shim16Registry& r = rt.shims();

  // ---- DCs ----
  r.impl(G, "CreateCompatibleDC", [](Call16& c) {
    uint16_t like = c.w();
    Gdi16& g = gt(c);
    uint16_t h = g.create_memory_dc();
    // Compatible with a DIB DC: the DIB device's memory DC, whose pixel
    // values mean that DIB's colour table too.
    Dc16* src = g.dc(like);
    if (Dc16* d = g.dc(h); d && src && src->dib_header) {
      d->dib_header = src->dib_header;
      g.sync(h);
    }
    c.ret(h);
  });
  // CreateDC(driver, device, output, lpInitData): "DISPLAY", a screen DC;
  // "DIB" (or "DIB.DRV"), the DIB driver over the packed DIB lpInitData
  // points to (gdi16.hh). Any other driver: 0.
  r.impl(G, "CreateDC", [](Call16& c) {
    std::string driver = c.rt.read_str(c.ptr());
    c.ptr();
    c.ptr();
    uint32_t init = c.ptr();
    Gdi16& g = gt(c);
    if (ieq16(driver, "DISPLAY")) return c.ret(g.create_screen_dc(0));
    if (ieq16(driver, "DIB") || ieq16(driver, "DIB.DRV")) return c.ret(g.create_dib_dc(init));
    trace("dib16", "CreateDC(\"%s\"): no such driver", driver.c_str());
    c.ret(0);
  });
  r.impl(G, "CreateIC", [](Call16& c) {
    std::string driver = c.rt.read_str(c.ptr());
    c.ptr();
    c.ptr();
    c.ptr();
    c.ret(ieq16(driver, "DISPLAY") ? gt(c).create_ic() : 0);
  });
  r.impl(G, "DeleteDC", [](Call16& c) { c.ret_bool(gt(c).destroy(c.w())); });
  r.impl(G, "SaveDC", [](Call16& c) {
    uint16_t h = c.w();
    Dc16* d = gt(c).dc(h);
    if (!d) return c.ret(0);
    int n = SaveDC(gt(c).host_dc(h));
    d->saved.push_back(d->s);
    c.ret(uint16_t(n));
  });
  r.impl(G, "RestoreDC", [](Call16& c) {
    uint16_t h = c.w();
    int16_t level = c.sw();
    Gdi16& g = gt(c);
    Dc16* d = g.dc(h);
    if (!d || d->saved.empty()) return c.ret(0);
    int n = int(d->saved.size());
    int target = level < 0 ? n + level : level - 1;  // index of the state to restore
    if (target < 0 || target >= n) return c.ret(0);
    BOOL ok = RestoreDC(g.host_dc(h), level);
    Dc16State st = d->saved[size_t(target)];
    d->saved.resize(size_t(target));
    // What real RestoreDC re-selected are the real objects of that moment;
    // re-derive them from the guest state.
    uint16_t bmp = st.bitmap;
    // The bitmap selected until now leaves the DC (unless it is the one
    // coming back), so it can be deleted or selected elsewhere again.
    if (d->s.bitmap != bmp) {
      if (Obj16* cur = g.get(d->s.bitmap, G16::bitmap)) {
        if (cur->bmp.selected_in == h) cur->bmp.selected_in = 0;
      }
    }
    d->s = st;
    if (Obj16* b = g.get(bmp, G16::bitmap)) {
      if (!b->stock) b->bmp.selected_in = h;
    }
    g.sync(h);
    c.ret_bool(ok);
  });
  r.impl(G, "GetDeviceCaps", [](Call16& c) {
    uint16_t hdc = c.w();
    int16_t idx = c.sw();
    Gdi16& g = gt(c);
    Dc16* d = g.dc(hdc);
    if (d && d->dib_device) return c.ret(uint16_t(g.dib_device_caps(*d, idx)));
    c.ret(uint16_t(g.display().device_caps(idx)));
  });

  // ---- objects ----
  r.impl(G, "GetStockObject", [](Call16& c) { c.ret(gt(c).stock(c.w())); });
  r.impl(G, "SelectObject", [](Call16& c) {
    uint16_t hdc = c.w(), h = c.w();
    c.ret(gt(c).select(hdc, h));
  });
  r.impl(G, "DeleteObject", [](Call16& c) { c.ret_bool(gt(c).destroy(c.w())); });
  r.impl(G, "UnrealizeObject", [](Call16& c) {
    Obj16* o = gt(c).get(c.w());
    if (o && o->type == G16::palette && !o->stock) o->pal->realized = false;
    c.ret_bool(o != nullptr);
  });
  r.impl(G, "IsGDIObject", [](Call16& c) {
    Obj16* o = gt(c).get(c.w());
    static const uint16_t kType[] = {0, 7, 5, 2, 1, 3, 6, 4};  // Win16 OBJ_* numbering
    c.ret(o ? kType[size_t(o->type)] : 0);
  });
  r.impl(G, "GetObject", [](Call16& c) {
    uint16_t h = c.w();
    int16_t size = c.sw();
    uint32_t buf = c.ptr();
    Obj16* o = gt(c).get(h);
    if (!o) return c.ret(0);
    auto put = [&](const void* data, size_t n) {
      size_t m = size < 0 ? 0 : std::min(n, size_t(size));
      if (buf && m) c.rt.write_bytes(buf, data, m);
      c.ret(uint16_t(buf ? m : n));
    };
    switch (o->type) {
      case G16::bitmap: {
        BITMAP16 b{0, int16_t(o->bmp.w), int16_t(o->bmp.h),
                   int16_t(o->bmp.bpp == 1 ? ((o->bmp.w + 15) / 16) * 2 : (o->bmp.w + 1) & ~1), 1,
                   uint8_t(o->bmp.bpp), 0};
        return put(&b, sizeof(b));
      }
      case G16::pen: {
        LOGPEN16 p{uint16_t(o->style), POINT16{int16_t(o->width), 0}, o->color};
        return put(&p, sizeof(p));
      }
      case G16::brush: {
        LOGBRUSH16 b{uint16_t(o->style), o->color, int16_t(o->hatch)};
        return put(&b, sizeof(b));
      }
      case G16::font: {
        LOGFONT16 f{};
        f.lfHeight = int16_t(o->font.lfHeight);
        f.lfWidth = int16_t(o->font.lfWidth);
        f.lfEscapement = int16_t(o->font.lfEscapement);
        f.lfOrientation = int16_t(o->font.lfOrientation);
        f.lfWeight = int16_t(o->font.lfWeight);
        f.lfItalic = o->font.lfItalic;
        f.lfUnderline = o->font.lfUnderline;
        f.lfStrikeOut = o->font.lfStrikeOut;
        f.lfCharSet = o->font.lfCharSet;
        f.lfOutPrecision = o->font.lfOutPrecision;
        f.lfClipPrecision = o->font.lfClipPrecision;
        f.lfQuality = o->font.lfQuality;
        f.lfPitchAndFamily = o->font.lfPitchAndFamily;
        memcpy(f.lfFaceName, o->font.lfFaceName, 32);
        return put(&f, sizeof(f));
      }
      case G16::palette: {
        uint16_t n = uint16_t(o->pal->entries.size());
        return put(&n, 2);
      }
      default:
        return c.ret(0);
    }
  });
  r.impl(G, "CreateSolidBrush", [](Call16& c) { c.ret(gt(c).create_brush(c.l())); });
  // CreateBrushIndirect(LOGBRUSH FAR*): BS_SOLID, BS_NULL, BS_HATCHED,
  // BS_PATTERN (lbHatch = a bitmap) and BS_DIBPATTERN (lbHatch = a global
  // packed DIB; lbColor's low word DIB_RGB_COLORS/DIB_PAL_COLORS). GUTS makes
  // its brushes this way.
  r.impl(G, "CreateBrushIndirect", [](Call16& c) {
    LOGBRUSH16 lb = read16<LOGBRUSH16>(c.rt, c.ptr());
    Gdi16& g = gt(c);
    switch (lb.lbStyle) {
      case BS_SOLID:
        return c.ret(g.create_brush(lb.lbColor));
      case BS_NULL:
        return c.ret(g.create_brush(0, BS_NULL));
      case BS_HATCHED:
        return c.ret(g.create_brush(lb.lbColor, BS_HATCHED, lb.lbHatch));
      case BS_PATTERN:
      case BS_DIBPATTERN: {
        uint16_t bmp = uint16_t(lb.lbHatch);
        if (lb.lbStyle == BS_DIBPATTERN) {
          uint32_t dib = c.rt.global().lock(uint16_t(lb.lbHatch));
          bmp = dib ? gdi16_bitmap_from_dib(c.rt, 0, dib) : 0;
          if (dib) c.rt.global().unlock(uint16_t(lb.lbHatch));
        }
        if (!g.get(bmp, G16::bitmap)) return c.ret(0);
        uint16_t h = g.create_brush(0, BS_PATTERN);
        if (Obj16* o = g.get(h, G16::brush)) {
          o->pattern = bmp;
          // The DIB's bitmap is the brush's own (BS_PATTERN's is the guest's).
          if (lb.lbStyle == BS_DIBPATTERN) g.get(bmp)->pattern_of = h;
        } else if (lb.lbStyle == BS_DIBPATTERN) {
          g.destroy(bmp);  // no brush (the table is full): nor its bitmap
        }
        return c.ret(h);
      }
      default:
        return c.ret(g.create_brush(lb.lbColor));
    }
  });
  // CreateHatchBrush(style, colour): HS_* over the DC's background (BIOS,
  // BLUPRINT, CANTINA, POSTERS, STORYBRD hatch the panel behind their text).
  r.impl(G, "CreateHatchBrush", [](Call16& c) {
    int16_t style = c.sw();
    uint32_t color = c.l();
    c.ret(gt(c).create_brush(color, BS_HATCHED, std::clamp<int>(style, HS_HORIZONTAL, HS_DIAGCROSS)));
  });
  // CreateDIBPatternBrush(hPackedDIB, usage): a pattern brush from a packed
  // DIB in a global block (DIB_RGB_COLORS: an RGBQUAD table; DIB_PAL_COLORS:
  // WORD indices). The brush keeps a copy, so the block may go. On a DIB DC
  // it paints the pattern's own indices (RCLOCK fills its clock's regions
  // with three, between SetROP2(R2_MASKPEN) and R2_MERGEPEN, 1:0C93..1:0CF5);
  // elsewhere its colours as the display matched them (CreateBrushIndirect's
  // BS_DIBPATTERN), through a bitmap of its own that DeleteObject deletes
  // with it. ANTSW's and INTRMLIB's LibEntry make theirs from bitmap
  // resources. 0 when the block holds no DIB.
  r.impl(G, "CreateDIBPatternBrush", [](Call16& c) {
    uint16_t hdib = c.w(), usage = c.w();
    Gdi16& g = gt(c);
    uint32_t p = c.rt.global().lock(hdib);
    if (!p) return c.ret(0);
    uint32_t size = std::min<uint32_t>(c.rt.global().size(hdib), 0x10000 - (p & 0xFFFF));
    std::vector<uint8_t> copy(size);
    c.rt.read_bytes(p, copy.data(), copy.size());
    uint16_t bmp = gdi16_bitmap_from_dib(c.rt, 0, p, usage == DIB_PAL_COLORS ? DIB_PAL_COLORS : DIB_RGB_COLORS);
    c.rt.global().unlock(hdib);
    if (!g.get(bmp, G16::bitmap)) return c.ret(0);
    uint16_t h = g.create_brush(0, BS_PATTERN);
    if (Obj16* o = g.get(h, G16::brush)) {
      o->pattern = bmp;
      g.get(bmp)->pattern_of = h;
      o->dib_pattern = std::move(copy);
      o->dib_usage = usage;
    } else {
      g.destroy(bmp);  // no brush (the table is full): nor its bitmap
    }
    c.ret(h);
  });
  // CreatePatternBrush(hbm): a brush of the bitmap's top-left 8×8 pixels
  // (Windows 3.1 and 95 brushes were 8×8, whatever the bitmap; a smaller
  // one tiles at its own size). Windows copied the pattern into the brush,
  // so a program may delete the bitmap once the brush is made and the brush
  // still paints (Windows 11's does too): the brush gets a bitmap of its
  // own, which DeleteObject deletes with it. A monochrome pattern paints
  // its 0 bits in the DC's text colour and its 1 bits in its background
  // colour: Little Mermaid's "Plain" sea fills each row with an 8×8 dither
  // of the two (MERMAID 8:2F09, then FillRect). 0 for no bitmap.
  r.impl(G, "CreatePatternBrush", [](Call16& c) {
    uint16_t hbm = c.w();
    Gdi16& g = gt(c);
    Obj16* src = g.get(hbm, G16::bitmap);
    if (!src || !src->host) return c.ret(0);
    int w = std::min(src->bmp.w, 8), h = std::min(src->bmp.h, 8);
    uint16_t own = g.create_device_bitmap(w, h, src->bmp.bpp);
    Obj16* o = g.get(own, G16::bitmap);
    if (!o) return c.ret(0);
    GdiFlush();
    const uint32_t ss = src->bmp.stride, os = o->bmp.stride;
    if (src->bmp.bpp == 1) {
      std::vector<uint8_t> in(size_t(ss) * size_t(src->bmp.h)), out(size_t(os) * size_t(h));
      GetBitmapBits(static_cast<HBITMAP>(src->host), LONG(in.size()), in.data());
      for (int y = 0; y < h; y++) memcpy(out.data() + size_t(y) * os, in.data() + size_t(y) * ss, os);
      SetBitmapBits(static_cast<HBITMAP>(o->host), DWORD(out.size()), out.data());
    } else {
      for (int y = 0; y < h; y++) memcpy(o->bmp.bits + size_t(y) * os, src->bmp.bits + size_t(y) * ss, size_t(w));
    }
    uint16_t hb = g.create_brush(0, BS_PATTERN);
    if (Obj16* b = g.get(hb, G16::brush)) {
      b->pattern = own;
      o->pattern_of = hb;
    } else {
      g.destroy(own);  // no brush (the table is full): nor its bitmap
    }
    c.ret(hb);
  });
  // GetDCOrg(hdc): DX:AX = the DC's origin on the screen — (0, 0) for the
  // full-screen saver window's DC and memory DCs; a child window's corner for
  // GetDC(child).
  r.impl(G, "GetDCOrg", [](Call16& c) {
    Dc16* d = gt(c).dc(c.w());
    c.ret32(d ? (uint32_t(uint16_t(d->org_y)) << 16) | uint16_t(d->org_x) : 0);
  });
  r.impl(G, "CreatePen", [](Call16& c) {
    int16_t style = c.sw(), width = c.sw();
    uint32_t color = c.l();
    c.ret(gt(c).create_pen(style, std::max<int>(width, 0), color));
  });
  r.impl(G, "CreateFontIndirect", [](Call16& c) {
    LOGFONT16 f = read16<LOGFONT16>(c.rt, c.ptr());
    c.ret(make_font(gt(c), to_logfont(f)));
  });
  // AddFontResource(lpszFilename): the number of fonts added. A file the
  // guest's disk does not hold adds none: 0, as Windows answered (Johnny
  // Castaway asks for WILLY.FON, which its floppy never had, and its
  // CreateFont of "Willy Beamish Dialog" then gets the font mapper's choice).
  // A font file that is there is not loaded either (no module of the
  // supported releases ships one): 0, logged. A handle (HIWORD 0) adds none.
  r.impl(G, "AddFontResource", [](Call16& c) {
    uint32_t p = c.ptr();
    if (!(p >> 16)) return c.ret(0);
    std::string name = c.rt.read_str(p);
    const bool there = c.rt.state<DosFiles>().exists(name);
    if (there) log("win16: AddFontResource(\"%s\"): font files are not loaded; 0 fonts added", name.c_str());
    trace("gdi16", "AddFontResource(\"%s\") -> 0 (%s)", name.c_str(), there ? "not loaded" : "no such file");
    c.ret(0);
  });
  r.impl(G, "RemoveFontResource", [](Call16& c) {
    c.ptr();
    c.ret(0);
  });
  r.impl(G, "CreateFont", [](Call16& c) {
    LOGFONT16 f{};
    f.lfHeight = c.sw();
    f.lfWidth = c.sw();
    f.lfEscapement = c.sw();
    f.lfOrientation = c.sw();
    f.lfWeight = c.sw();
    f.lfItalic = uint8_t(c.w());
    f.lfUnderline = uint8_t(c.w());
    f.lfStrikeOut = uint8_t(c.w());
    f.lfCharSet = uint8_t(c.w());
    f.lfOutPrecision = uint8_t(c.w());
    f.lfClipPrecision = uint8_t(c.w());
    f.lfQuality = uint8_t(c.w());
    f.lfPitchAndFamily = uint8_t(c.w());
    std::string face = c.rt.read_str(c.ptr(), 31);
    memcpy(f.lfFaceName, face.c_str(), face.size() + 1);
    c.ret(make_font(gt(c), to_logfont(f)));
  });
  r.impl(G, "CreateBitmap", [](Call16& c) {
    int16_t w = c.sw(), h = c.sw();
    uint16_t planes = c.w(), bpp = c.w();
    uint32_t bits = c.ptr();
    c.ret(create_bitmap16(c.rt, w, h, planes, bpp, bits));
  });
  // CreateBitmapIndirect(const BITMAP FAR*): CreateBitmap of the structure's
  // width, height, planes, bits per pixel and bits, the rows WORD-aligned
  // as CreateBitmap reads them (what bmWidthBytes must say; it is not
  // read). Little Mermaid's "Plain" sea makes an 8×8 monochrome pattern
  // this way for each shade of its rows (MERMAID 8:2EFE).
  r.impl(G, "CreateBitmapIndirect", [](Call16& c) {
    BITMAP16 b = read16<BITMAP16>(c.rt, c.ptr());
    c.ret(create_bitmap16(c.rt, b.bmWidth, b.bmHeight, b.bmPlanes, b.bmBitsPixel, b.bmBits));
  });
  r.impl(G, "CreateCompatibleBitmap", [](Call16& c) {
    uint16_t hdc = c.w();
    int16_t w = c.sw(), h = c.sw();
    Gdi16& g = gt(c);
    // Compatible with a memory DC that still holds its 1x1 monochrome bitmap
    // means monochrome — the classic Windows gotcha, kept.
    Bitmap16* s = g.dc_surface(hdc);
    int bpp = s && s->bpp == 1 ? 1 : 8;
    c.ret(g.create_device_bitmap(w, h, bpp));
  });
  r.impl(G, "GetBitmapBits", [](Call16& c) {
    uint16_t hb = c.w();
    uint32_t count = c.l();
    uint32_t buf = c.ptr();
    Obj16* o = gt(c).get(hb, G16::bitmap);
    if (!o || !buf) return c.ret32(0);
    GdiFlush();
    uint32_t stride = o->bmp.bpp == 1 ? o->bmp.stride : uint32_t((o->bmp.w + 1) & ~1);
    std::vector<uint8_t> out(size_t(stride) * o->bmp.h, 0);
    if (o->bmp.bpp == 1) {
      GetBitmapBits(static_cast<HBITMAP>(o->host), LONG(out.size()), out.data());
    } else {
      for (int y = 0; y < o->bmp.h; y++) memcpy(out.data() + size_t(y) * stride, o->bmp.bits + size_t(y) * o->bmp.stride, size_t(o->bmp.w));
    }
    uint32_t n = std::min<uint32_t>(count, uint32_t(out.size()));
    for (uint32_t done = 0, dst = buf; done < n;) {
      uint32_t chunk = std::min(n - done, 0x10000 - (dst & 0xFFFF));
      c.rt.write_bytes(dst, out.data() + done, chunk);
      done += chunk;
      dst = Runtime16::huge_add(dst, chunk);
    }
    c.ret32(n);
  });
  r.impl(G, "SetBitmapBits", [](Call16& c) {
    uint16_t hb = c.w();
    uint32_t count = c.l();
    uint32_t buf = c.ptr();
    Obj16* o = gt(c).get(hb, G16::bitmap);
    if (!o || !buf) return c.ret32(0);
    GdiFlush();
    uint32_t stride = o->bmp.bpp == 1 ? o->bmp.stride : uint32_t((o->bmp.w + 1) & ~1);
    std::vector<uint8_t> in(size_t(stride) * o->bmp.h, 0);
    uint32_t n = std::min<uint32_t>(count, uint32_t(in.size()));
    for (uint32_t done = 0, src = buf; done < n;) {
      uint32_t chunk = std::min(n - done, 0x10000 - (src & 0xFFFF));
      c.rt.read_bytes(src, in.data() + done, chunk);
      done += chunk;
      src = Runtime16::huge_add(src, chunk);
    }
    if (o->bmp.bpp == 1) {
      SetBitmapBits(static_cast<HBITMAP>(o->host), DWORD(n), in.data());
    } else {
      for (int y = 0; y < o->bmp.h; y++) memcpy(o->bmp.bits + size_t(y) * o->bmp.stride, in.data() + size_t(y) * stride, size_t(o->bmp.w));
    }
    c.ret32(n);
  });

  // ---- attributes ----
  r.impl(G, "SetBkColor", [](Call16& c) {
    uint16_t hdc = c.w();
    uint32_t col = c.l();
    Dc16* d = gt(c).dc(hdc);
    if (!d) return c.ret32(0x80000000);
    COLORREF old = d->s.bk;
    d->s.bk = col;
    if (HDC h = gt(c).host_dc(hdc)) SetBkColor(h, gt(c).key(hdc, col));
    c.ret32(old);
  });
  r.impl(G, "SetTextColor", [](Call16& c) {
    uint16_t hdc = c.w();
    uint32_t col = c.l();
    Dc16* d = gt(c).dc(hdc);
    if (!d) return c.ret32(0x80000000);
    COLORREF old = d->s.text;
    d->s.text = col;
    if (HDC h = gt(c).host_dc(hdc)) SetTextColor(h, gt(c).key(hdc, col));
    c.ret32(old);
  });
  r.impl(G, "GetBkColor", [](Call16& c) {
    Dc16* d = gt(c).dc(c.w());
    c.ret32(d ? d->s.bk : 0);
  });
  r.impl(G, "GetTextColor", [](Call16& c) {
    Dc16* d = gt(c).dc(c.w());
    c.ret32(d ? d->s.text : 0);
  });
  auto passthrough_mode = [&](const char* name, int (*set)(HDC, int)) {
    r.impl(G, name, [set](Call16& c) {
      uint16_t hdc = c.w();
      int16_t v = c.sw();
      HDC h = gt(c).host_dc(hdc);
      c.ret(h ? uint16_t(set(h, v)) : 0);
    });
  };
  passthrough_mode("SetBkMode", [](HDC h, int v) { return ::SetBkMode(h, v); });
  passthrough_mode("SetROP2", [](HDC h, int v) { return ::SetROP2(h, v); });
  passthrough_mode("SetPolyFillMode", [](HDC h, int v) { return ::SetPolyFillMode(h, v); });
  passthrough_mode("SetStretchBltMode", [](HDC h, int v) { return ::SetStretchBltMode(h, v); });
  passthrough_mode("SetMapMode", [](HDC h, int v) { return ::SetMapMode(h, v); });
  r.impl(G, "SetTextAlign", [](Call16& c) {
    uint16_t hdc = c.w(), v = c.w();
    HDC h = gt(c).host_dc(hdc);
    c.ret(h ? uint16_t(::SetTextAlign(h, v)) : 0);
  });
  r.impl(G, "GetBkMode", [](Call16& c) {
    HDC h = gt(c).host_dc(c.w());
    c.ret(h ? uint16_t(::GetBkMode(h)) : 0);
  });
  r.impl(G, "GetPolyFillMode", [](Call16& c) {
    HDC h = gt(c).host_dc(c.w());
    c.ret(h ? uint16_t(::GetPolyFillMode(h)) : 0);
  });
  // GetMapMode(hdc): the ScreamSavers modules give their memory DC the
  // screen DC's mode, SetMapMode(mem, GetMapMode(screen)) — MM_TEXT.
  r.impl(G, "GetMapMode", [](Call16& c) {
    HDC h = gt(c).host_dc(c.w());
    c.ret(h ? uint16_t(::GetMapMode(h)) : 0);
  });
  auto origin = [&](const char* name, BOOL (*fn)(HDC, int, int, POINT*)) {
    r.impl(G, name, [fn](Call16& c) {
      uint16_t hdc = c.w();
      int16_t x = c.sw(), y = c.sw();
      HDC h = gt(c).host_dc(hdc);
      POINT old{0, 0};
      if (h) fn(h, x, y, &old);
      c.ret32(pack_point(old));
    });
  };
  origin("SetViewportOrg", [](HDC h, int x, int y, POINT* p) { return ::SetViewportOrgEx(h, x, y, p); });
  origin("SetWindowOrg", [](HDC h, int x, int y, POINT* p) { return ::SetWindowOrgEx(h, x, y, p); });
  origin("OffsetViewportOrg", [](HDC h, int x, int y, POINT* p) { return ::OffsetViewportOrgEx(h, x, y, p); });
  origin("SetBrushOrg", [](HDC h, int x, int y, POINT* p) { return ::SetBrushOrgEx(h, x, y, p); });
  auto extent = [&](const char* name, BOOL (*fn)(HDC, int, int, SIZE*)) {
    r.impl(G, name, [fn](Call16& c) {
      uint16_t hdc = c.w();
      int16_t x = c.sw(), y = c.sw();
      HDC h = gt(c).host_dc(hdc);
      SIZE old{0, 0};
      if (h) fn(h, x, y, &old);
      c.ret32(pack_size(old));
    });
  };
  extent("SetWindowExt", [](HDC h, int x, int y, SIZE* s) { return ::SetWindowExtEx(h, x, y, s); });
  extent("SetViewportExt", [](HDC h, int x, int y, SIZE* s) { return ::SetViewportExtEx(h, x, y, s); });
  auto scale = [&](const char* name, BOOL (*fn)(HDC, int, int, int, int, SIZE*)) {
    r.impl(G, name, [fn](Call16& c) {
      uint16_t hdc = c.w();
      int16_t a = c.sw(), b = c.sw(), d = c.sw(), e = c.sw();
      HDC h = gt(c).host_dc(hdc);
      SIZE old{0, 0};
      if (h) fn(h, a, b, d, e, &old);
      c.ret32(pack_size(old));
    });
  };
  scale("ScaleWindowExt", [](HDC h, int a, int b, int c, int d, SIZE* s) { return ::ScaleWindowExtEx(h, a, b, c, d, s); });
  scale("ScaleViewportExt",
        [](HDC h, int a, int b, int c, int d, SIZE* s) { return ::ScaleViewportExtEx(h, a, b, c, d, s); });
  r.impl(G, "GetWindowOrg", [](Call16& c) {
    HDC h = gt(c).host_dc(c.w());
    POINT p{0, 0};
    if (h) GetWindowOrgEx(h, &p);
    c.ret32(pack_point(p));
  });
  r.impl(G, "GetCurrentPosition", [](Call16& c) {
    HDC h = gt(c).host_dc(c.w());
    POINT p{0, 0};
    if (h) GetCurrentPositionEx(h, &p);
    c.ret32(pack_point(p));
  });

  // ---- drawing ----
  r.impl(G, "MoveTo", [](Call16& c) {
    uint16_t hdc = c.w();
    int16_t x = c.sw(), y = c.sw();
    HDC h = gt(c).host_dc(hdc);
    POINT old{0, 0};
    if (h) MoveToEx(h, x, y, &old);
    c.ret32(pack_point(old));
  });
  r.impl(G, "LineTo", [](Call16& c) {
    uint16_t hdc = c.w();
    int16_t x = c.sw(), y = c.sw();
    HDC h = gt(c).host_dc(hdc);
    if (!h) return c.ret(0);
    gt(c).sync(hdc);
    c.ret_bool(::LineTo(h, x, y));
  });
  // LineDDA(xStart, yStart, xEnd, yEnd, lpLineFunc, lpData), no result: the
  // points of the line from the start to the end, the end excluded as
  // LineTo's, each handed to lpLineFunc(x, y, lpData) (FAR PASCAL) in order
  // — Bresenham's steps along the longer axis, the shorter one stepping when
  // the error passes 0 (a tie keeps it); a line of one point (start = end)
  // has none. No DC and nothing drawn: what the callback does with a point
  // is its own (Intermission's Plants grows its stems a point at a time).
  r.impl(G, "LineDDA", [](Call16& c) {
    int x = c.sw(), y = c.sw();
    const int x2 = c.sw(), y2 = c.sw();
    const uint32_t proc = c.ptr(), data = c.l();
    if (!proc) return;
    int dx = x2 - x, dy = y2 - y;
    const int sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    dx = std::abs(dx);
    dy = std::abs(dy);
    const bool across = dx > dy;  // more horizontal than vertical
    const int major = across ? dx : dy, minor = across ? dy : dx;
    int err = 2 * minor - major;
    for (int i = 0; i < major; i++) {
      c.rt.call_far(proc, {w16(uint16_t(x)), w16(uint16_t(y)), l16(data)});
      if (err > 0) {
        if (across) {
          y += sy;
        } else {
          x += sx;
        }
        err += 2 * minor - 2 * major;
      } else {
        err += 2 * minor;
      }
      if (across) {
        x += sx;
      } else {
        y += sy;
      }
    }
  });
  auto shape4 = [&](const char* name, BOOL (*fn)(HDC, int, int, int, int)) {
    r.impl(G, name, [fn](Call16& c) {
      uint16_t hdc = c.w();
      int16_t a = c.sw(), b = c.sw(), d = c.sw(), e = c.sw();
      HDC h = gt(c).host_dc(hdc);
      if (!h) return c.ret(0);
      gt(c).sync(hdc);
      c.ret_bool(fn(h, a, b, d, e));
    });
  };
  shape4("Rectangle", [](HDC h, int a, int b, int c, int d) { return ::Rectangle(h, a, b, c, d); });
  shape4("Ellipse", [](HDC h, int a, int b, int c, int d) { return ::Ellipse(h, a, b, c, d); });
  r.impl(G, "RoundRect", [](Call16& c) {
    uint16_t hdc = c.w();
    int16_t v[6];
    for (auto& x : v) x = c.sw();
    HDC h = gt(c).host_dc(hdc);
    if (!h) return c.ret(0);
    gt(c).sync(hdc);
    c.ret_bool(::RoundRect(h, v[0], v[1], v[2], v[3], v[4], v[5]));
  });
  auto shape8 = [&](const char* name, BOOL (*fn)(HDC, int, int, int, int, int, int, int, int)) {
    r.impl(G, name, [fn](Call16& c) {
      uint16_t hdc = c.w();
      int16_t v[8];
      for (auto& x : v) x = c.sw();
      HDC h = gt(c).host_dc(hdc);
      if (!h) return c.ret(0);
      gt(c).sync(hdc);
      c.ret_bool(fn(h, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]));
    });
  };
  shape8("Arc", [](HDC h, int a, int b, int c, int d, int e, int f, int g, int i) { return ::Arc(h, a, b, c, d, e, f, g, i); });
  shape8("Pie", [](HDC h, int a, int b, int c, int d, int e, int f, int g, int i) { return ::Pie(h, a, b, c, d, e, f, g, i); });
  r.impl(G, "Polygon", [](Call16& c) {
    uint16_t hdc = c.w();
    uint32_t pts = c.ptr();
    int16_t n = c.sw();
    HDC h = gt(c).host_dc(hdc);
    if (!h || n <= 0) return c.ret(0);
    auto p = read_points(c.rt, pts, n);
    gt(c).sync(hdc);
    c.ret_bool(::Polygon(h, p.data(), int(p.size())));
  });
  r.impl(G, "Polyline", [](Call16& c) {
    uint16_t hdc = c.w();
    uint32_t pts = c.ptr();
    int16_t n = c.sw();
    HDC h = gt(c).host_dc(hdc);
    if (!h || n <= 0) return c.ret(0);
    auto p = read_points(c.rt, pts, n);
    gt(c).sync(hdc);
    c.ret_bool(::Polyline(h, p.data(), int(p.size())));
  });
  r.impl(G, "SetPixel", [](Call16& c) {
    uint16_t hdc = c.w();
    int16_t x = c.sw(), y = c.sw();
    uint32_t col = c.l();
    Gdi16& g = gt(c);
    HDC h = g.host_dc(hdc);
    if (!h) return c.ret32(0xFFFFFFFF);
    COLORREF k = g.key(hdc, col);
    COLORREF got = ::SetPixel(h, x, y, k);
    if (got == CLR_INVALID) return c.ret32(0xFFFFFFFF);
    Bitmap16* s = g.dc_surface(hdc);
    c.ret32(s && s->bpp == 1 ? got : g.surface_rgb(hdc, GetRValue(got)));
  });
  r.impl(G, "GetPixel", [](Call16& c) {
    uint16_t hdc = c.w();
    int16_t x = c.sw(), y = c.sw();
    Gdi16& g = gt(c);
    HDC h = g.host_dc(hdc);
    if (!h) return c.ret32(0xFFFFFFFF);
    GdiFlush();
    COLORREF got = ::GetPixel(h, x, y);
    if (got == CLR_INVALID) return c.ret32(0xFFFFFFFF);
    Bitmap16* s = g.dc_surface(hdc);
    c.ret32(s && s->bpp == 1 ? got : g.surface_rgb(hdc, GetRValue(got)));
  });
  // FloodFill (GDI.25: up to the colour, FLOODFILLBORDER) and ExtFloodFill
  // (GDI.372: FLOODFILLBORDER, or FLOODFILLSURFACE: as far as the colour
  // goes). The colour is keyed as SetPixel's, so the fill compares pixel
  // indices, as a Win95 palette device compared physical colours; real GDI
  // fills from the logical point, 4-connected, with the DC's brush and ROP2,
  // within the surface and the clip region, and answers FALSE when the
  // point is outside them or not in the area. Snoopy's modules make their
  // sprite masks so: a white scratch bitmap, the sprite blitted in,
  // BLACK_BRUSH, ExtFloodFill(2, 2, white, FLOODFILLSURFACE) — the outside
  // turns black — then two blits to a monochrome mask; without it every
  // sprite was drawn in a white box. The pixels filled are charged, as a
  // fill's pixels are (PatBlt's and FillRect's rectangle): counted by the
  // host by real GDI's rule (flood_pixels), a fill of nothing costing nothing.
  auto flood = [](Call16& c, bool ext) {
    uint16_t hdc = c.w();
    int16_t x = c.sw(), y = c.sw();
    uint32_t col = c.l();
    uint16_t type = ext ? c.w() : uint16_t(FLOODFILLBORDER);
    Gdi16& g = gt(c);
    HDC h = g.host_dc(hdc);
    if (!h) return c.ret(0);
    COLORREF k = g.key(hdc, col);
    c.rt.charge_pixels(flood_pixels(c.rt, g, hdc, h, x, y, k, type));
    g.sync(hdc);
    c.ret_bool(::ExtFloodFill(h, x, y, k, type));
  };
  r.impl(G, "FloodFill", [flood](Call16& c) { flood(c, false); });
  r.impl(G, "ExtFloodFill", [flood](Call16& c) { flood(c, true); });
  r.impl(G, "PatBlt", [](Call16& c) {
    uint16_t hdc = c.w();
    int16_t x = c.sw(), y = c.sw(), w = c.sw(), h = c.sw();
    uint32_t rop = c.l();
    HDC d = gt(c).host_dc(hdc);
    if (!d) return c.ret(0);
    c.rt.charge_pixels(int64_t(std::abs(int(w))) * std::abs(int(h)));
    gt(c).sync(hdc);
    c.ret_bool(::PatBlt(d, x, y, w, h, rop));
  });
  r.impl(G, "BitBlt", [](Call16& c) {
    uint16_t dst = c.w();
    int16_t x = c.sw(), y = c.sw(), w = c.sw(), h = c.sw();
    uint16_t src = c.w();
    int16_t sx = c.sw(), sy = c.sw();
    uint32_t rop = c.l();
    Gdi16& g = gt(c);
    HDC d = g.host_dc(dst);
    if (!d) return c.ret(0);
    c.rt.charge_pixels(int64_t(std::abs(int(w))) * std::abs(int(h)));
    g.sync(dst);
    if (!rop_uses_src(rop) || !src) return c.ret_bool(::PatBlt(d, x, y, w, h, rop));
    HDC s = g.host_dc(src);
    if (!s) return c.ret(0);
    g.sync(src);
    c.ret_bool(::BitBlt(d, x, y, w, h, s, sx, sy, rop));
  });
  r.impl(G, "StretchBlt", [](Call16& c) {
    uint16_t dst = c.w();
    int16_t x = c.sw(), y = c.sw(), w = c.sw(), h = c.sw();
    uint16_t src = c.w();
    int16_t sx = c.sw(), sy = c.sw(), sw = c.sw(), sh = c.sw();
    uint32_t rop = c.l();
    Gdi16& g = gt(c);
    HDC d = g.host_dc(dst);
    if (!d) return c.ret(0);
    c.rt.charge_pixels(int64_t(std::abs(int(w))) * std::abs(int(h)));
    g.sync(dst);
    if (!rop_uses_src(rop) || !src) return c.ret_bool(::PatBlt(d, x, y, w, h, rop));
    HDC s = g.host_dc(src);
    if (!s) return c.ret(0);
    g.sync(src);
    c.ret_bool(::StretchBlt(d, x, y, w, h, s, sx, sy, sw, sh, rop));
  });

  // ---- text ----
  r.impl(G, "TextOut", [](Call16& c) {
    uint16_t hdc = c.w();
    int16_t x = c.sw(), y = c.sw();
    uint32_t s = c.ptr();
    int16_t n = c.sw();
    HDC h = gt(c).host_dc(hdc);
    if (!h) return c.ret(0);
    std::string text = n > 0 ? c.rt.read_str(s, size_t(n)) : std::string();
    text.resize(std::max<int>(n, 0), '\0');
    gt(c).sync(hdc);
    c.ret_bool(::TextOutA(h, x, y, text.data(), int(text.size())));
  });
  r.impl(G, "ExtTextOut", [](Call16& c) {
    uint16_t hdc = c.w();
    int16_t x = c.sw(), y = c.sw();
    uint16_t opts = c.w();
    uint32_t rp = c.ptr(), s = c.ptr();
    uint16_t n = c.w();
    uint32_t dx = c.ptr();
    HDC h = gt(c).host_dc(hdc);
    if (!h) return c.ret(0);
    std::string text = c.rt.read_str(s, n);
    text.resize(n, '\0');
    RECT rc{};
    if (rp) rc = to_rect(read16<RECT16>(c.rt, rp));
    std::vector<INT> widths;
    if (dx) {
      for (uint16_t i = 0; i < n; i++) widths.push_back(int16_t(c.rt.rd16(dx + 2u * i)));
    }
    gt(c).sync(hdc);
    c.ret_bool(::ExtTextOutA(h, x, y, opts, rp ? &rc : nullptr, text.data(), n, dx ? widths.data() : nullptr));
  });
  r.impl(G, "GetTextExtent", [](Call16& c) {
    uint16_t hdc = c.w();
    uint32_t s = c.ptr();
    int16_t n = c.sw();
    HDC h = gt(c).host_dc(hdc);
    SIZE sz{0, 0};
    if (h && n > 0) {
      std::string text = c.rt.read_str(s, size_t(n));
      text.resize(size_t(n), '\0');
      GetTextExtentPoint32A(h, text.data(), n, &sz);
    }
    c.ret32(pack_size(sz));
  });
  r.impl(G, "GetTextMetrics", [](Call16& c) {
    uint16_t hdc = c.w();
    uint32_t out = c.ptr();
    HDC h = gt(c).host_dc(hdc);
    TEXTMETRICA tm{};
    if (!h || !GetTextMetricsA(h, &tm)) return c.ret(0);
    TEXTMETRIC16 t{int16_t(tm.tmHeight), int16_t(tm.tmAscent), int16_t(tm.tmDescent),
                   int16_t(tm.tmInternalLeading), int16_t(tm.tmExternalLeading), int16_t(tm.tmAveCharWidth),
                   int16_t(tm.tmMaxCharWidth), int16_t(tm.tmWeight), tm.tmItalic, tm.tmUnderlined, tm.tmStruckOut,
                   uint8_t(tm.tmFirstChar), uint8_t(tm.tmLastChar), uint8_t(tm.tmDefaultChar),
                   uint8_t(tm.tmBreakChar), tm.tmPitchAndFamily, tm.tmCharSet, int16_t(tm.tmOverhang),
                   int16_t(tm.tmDigitizedAspectX), int16_t(tm.tmDigitizedAspectY)};
    write16(c.rt, out, t);
    c.ret(1);
  });
  // No font enumeration: the callback is never called (a known gap, above).
  r.impl(G, "EnumFonts", [](Call16& c) { c.ret(1); });

  // ---- regions ----
  r.impl(G, "CreateRectRgn", [](Call16& c) {
    int16_t a = c.sw(), b = c.sw(), d = c.sw(), e = c.sw();
    c.ret(gt(c).wrap_region(CreateRectRgn(a, b, d, e)));
  });
  r.impl(G, "CreateRectRgnIndirect", [](Call16& c) {
    RECT rc = to_rect(read16<RECT16>(c.rt, c.ptr()));
    c.ret(gt(c).wrap_region(CreateRectRgnIndirect(&rc)));
  });
  r.impl(G, "CreateEllipticRgn", [](Call16& c) {
    int16_t a = c.sw(), b = c.sw(), d = c.sw(), e = c.sw();
    c.ret(gt(c).wrap_region(CreateEllipticRgn(a, b, d, e)));
  });
  r.impl(G, "CreateEllipticRgnIndirect", [](Call16& c) {
    RECT rc = to_rect(read16<RECT16>(c.rt, c.ptr()));
    c.ret(gt(c).wrap_region(CreateEllipticRgnIndirect(&rc)));
  });
  r.impl(G, "CreateRoundRectRgn", [](Call16& c) {
    int16_t v[6];
    for (auto& x : v) x = c.sw();
    c.ret(gt(c).wrap_region(CreateRoundRectRgn(v[0], v[1], v[2], v[3], v[4], v[5])));
  });
  r.impl(G, "CreatePolygonRgn", [](Call16& c) {
    uint32_t pts = c.ptr();
    int16_t n = c.sw(), mode = c.sw();
    auto p = read_points(c.rt, pts, n);
    c.ret(p.empty() ? 0 : gt(c).wrap_region(CreatePolygonRgn(p.data(), int(p.size()), mode)));
  });
  r.impl(G, "CombineRgn", [](Call16& c) {
    uint16_t d = c.w(), s1 = c.w(), s2 = c.w();
    int16_t mode = c.sw();
    Gdi16& g = gt(c);
    HRGN rd = region_of(g, d), r1 = region_of(g, s1), r2 = region_of(g, s2);
    c.ret(rd && r1 ? uint16_t(::CombineRgn(rd, r1, r2 ? r2 : r1, mode)) : 0);  // ERROR
  });
  r.impl(G, "SetRectRgn", [](Call16& c) {
    uint16_t h = c.w();
    int16_t a = c.sw(), b = c.sw(), d = c.sw(), e = c.sw();
    if (HRGN rg = region_of(gt(c), h)) SetRectRgn(rg, a, b, d, e);
  });
  r.impl(G, "OffsetRgn", [](Call16& c) {
    uint16_t h = c.w();
    int16_t x = c.sw(), y = c.sw();
    HRGN rg = region_of(gt(c), h);
    c.ret(rg ? uint16_t(::OffsetRgn(rg, x, y)) : 0);
  });
  r.impl(G, "GetRgnBox", [](Call16& c) {
    uint16_t h = c.w();
    uint32_t out = c.ptr();
    HRGN rg = region_of(gt(c), h);
    RECT rc{};
    int t = rg ? ::GetRgnBox(rg, &rc) : 0;
    write16(c.rt, out, to_rect16(rc));
    c.ret(uint16_t(t));
  });
  r.impl(G, "PtInRegion", [](Call16& c) {
    uint16_t h = c.w();
    int16_t x = c.sw(), y = c.sw();
    HRGN rg = region_of(gt(c), h);
    c.ret_bool(rg && ::PtInRegion(rg, x, y));
  });
  r.impl(G, "RectInRegionOld", [](Call16& c) {
    uint16_t h = c.w();
    RECT rc = to_rect(read16<RECT16>(c.rt, c.ptr()));
    HRGN rg = region_of(gt(c), h);
    c.ret_bool(rg && ::RectInRegion(rg, &rc));
  });
  r.impl(G, "EqualRgn", [](Call16& c) {
    HRGN a = region_of(gt(c), c.w()), b = region_of(gt(c), c.w());
    c.ret_bool(a && b && ::EqualRgn(a, b));
  });
  r.impl(G, "SelectClipRgn", [](Call16& c) {
    uint16_t hdc = c.w(), h = c.w();
    HDC d = gt(c).host_dc(hdc);
    c.ret(d ? uint16_t(::SelectClipRgn(d, region_of(gt(c), h))) : 0);
  });
  r.impl(G, "ExcludeClipRect", [](Call16& c) {
    uint16_t hdc = c.w();
    int16_t a = c.sw(), b = c.sw(), d = c.sw(), e = c.sw();
    HDC h = gt(c).host_dc(hdc);
    c.ret(h ? uint16_t(::ExcludeClipRect(h, a, b, d, e)) : 0);
  });
  // IntersectClipRect(hdc, left, top, right, bottom): the clip region, cut to
  // the rectangle (logical units); the result is the new region's type
  // (NULLREGION, SIMPLEREGION, COMPLEXREGION), ERROR (0) for no DC. Today's
  // GDI answers COMPLEXREGION whatever is left, so the type is the clip
  // box's, as Windows 3.1 gave the new region's.
  r.impl(G, "IntersectClipRect", [](Call16& c) {
    uint16_t hdc = c.w();
    int16_t a = c.sw(), b = c.sw(), d = c.sw(), e = c.sw();
    HDC h = gt(c).host_dc(hdc);
    if (!h || ::IntersectClipRect(h, a, b, d, e) == ERROR) return c.ret(0);
    RECT rc{};
    c.ret(uint16_t(::GetClipBox(h, &rc)));
  });
  r.impl(G, "GetClipBox", [](Call16& c) {
    uint16_t hdc = c.w();
    uint32_t out = c.ptr();
    HDC h = gt(c).host_dc(hdc);
    RECT rc{};
    int t = h ? ::GetClipBox(h, &rc) : 0;
    write16(c.rt, out, to_rect16(rc));
    c.ret(uint16_t(t));
  });
  r.impl(G, "PtVisible", [](Call16& c) {
    uint16_t hdc = c.w();
    int16_t x = c.sw(), y = c.sw();
    HDC h = gt(c).host_dc(hdc);
    c.ret_bool(h && ::PtVisible(h, x, y));
  });
  r.impl(G, "RectVisibleOld", [](Call16& c) {
    uint16_t hdc = c.w();
    RECT rc = to_rect(read16<RECT16>(c.rt, c.ptr()));
    HDC h = gt(c).host_dc(hdc);
    c.ret_bool(h && ::RectVisible(h, &rc));
  });
  r.impl(G, "FillRgn", [](Call16& c) {
    uint16_t hdc = c.w(), h = c.w(), hbr = c.w();
    Gdi16& g = gt(c);
    HDC d = g.host_dc(hdc);
    HRGN rg = region_of(g, h);
    if (!d || !rg) return c.ret(0);
    uint16_t saved = g.dc(hdc)->s.brush;
    g.sync_brush(hdc, hbr);
    BOOL ok = ::PaintRgn(d, rg);
    g.sync_brush(hdc, saved);
    c.ret_bool(ok);
  });
  // PaintRgn(hdc, hrgn): the region filled with the DC's own brush (ICLOCK
  // paints a region in its display colour and another with BLACK_BRUSH every
  // frame, 1:0D69 / 1:0DA4).
  r.impl(G, "PaintRgn", [](Call16& c) {
    uint16_t hdc = c.w(), h = c.w();
    Gdi16& g = gt(c);
    HDC d = g.host_dc(hdc);
    HRGN rg = region_of(g, h);
    if (!d || !rg) return c.ret(0);
    g.sync(hdc);
    c.ret_bool(::PaintRgn(d, rg));
  });
  r.impl(G, "FrameRgn", [](Call16& c) {
    uint16_t hdc = c.w(), h = c.w(), hbr = c.w();
    int16_t w = c.sw(), hh = c.sw();
    Gdi16& g = gt(c);
    HDC d = g.host_dc(hdc);
    HRGN rg = region_of(g, h);
    Obj16* b = g.get(hbr, G16::brush);
    if (!d || !rg || !b) return c.ret(0);
    // A temporary real brush in the DC's key colour.
    HBRUSH real = CreateSolidBrush(g.key(hdc, b->color));
    BOOL ok = ::FrameRgn(d, rg, real, w, hh);
    DeleteObject(real);
    c.ret_bool(ok);
  });

  // ---- palettes ----
  r.impl(G, "CreatePalette", [](Call16& c) {
    uint32_t lp = c.ptr();
    uint16_t n = c.rt.rd16(lp + 2);
    std::vector<PALETTEENTRY> e(std::min<uint16_t>(n, 1024));
    if (!e.empty()) c.rt.read_bytes(lp + 4, e.data(), e.size() * sizeof(PALETTEENTRY));
    c.ret(gt(c).create_palette(e));
  });
  r.impl(G, "GetPaletteEntries", [](Call16& c) {
    uint16_t h = c.w(), start = c.w(), n = c.w();
    uint32_t out = c.ptr();
    Obj16* o = gt(c).get(h, G16::palette);
    if (!o) return c.ret(0);
    auto& e = o->pal->entries;
    if (!out) return c.ret(uint16_t(e.size()));
    uint16_t k = 0;
    for (; k < n && size_t(start) + k < e.size(); k++) write16(c.rt, out + 4u * k, e[start + k]);
    c.ret(k);
  });
  r.impl(G, "SetPaletteEntries", [](Call16& c) {
    uint16_t h = c.w(), start = c.w(), n = c.w();
    uint32_t in = c.ptr();
    Obj16* o = gt(c).get(h, G16::palette);
    if (!o || o->stock) return c.ret(0);
    auto& e = o->pal->entries;
    uint16_t k = 0;
    for (; k < n && size_t(start) + k < e.size(); k++) e[start + k] = read16<PALETTEENTRY>(c.rt, in + 4u * k);
    c.ret(k);
  });
  r.impl(G, "AnimatePalette", [](Call16& c) {
    uint16_t h = c.w(), start = c.w(), n = c.w();
    uint32_t in = c.ptr();
    Obj16* o = gt(c).get(h, G16::palette);
    if (!o || o->stock || !n) return;
    std::vector<PALETTEENTRY> e(n);
    c.rt.read_bytes(in, e.data(), e.size() * sizeof(PALETTEENTRY));
    gt(c).display().animate(*o->pal, start, n, e.data());
  });
  r.impl(G, "GetNearestPaletteIndex", [](Call16& c) {
    uint16_t h = c.w();
    uint32_t col = c.l();
    Obj16* o = gt(c).get(h, G16::palette);
    c.ret(o && !o->pal->entries.empty() ? uint16_t(Display::nearest_in(*o->pal, col & 0xFFFFFF)) : 0);
  });
  r.impl(G, "GetSystemPaletteEntries", [](Call16& c) {
    c.w();
    uint16_t start = c.w(), n = c.w();
    uint32_t out = c.ptr();
    auto& sys = gt(c).display().system_palette();
    if (!out) return c.ret(256);
    uint16_t k = 0;
    for (; k < n && start + k < 256; k++) write16(c.rt, out + 4u * k, sys[size_t(start + k)]);
    c.ret(k);
  });
  r.impl(G, "SetSystemPaletteUse", [](Call16& c) {
    c.w();
    c.ret(uint16_t(gt(c).display().set_palette_use(c.w())));
  });
  // GetSystemPaletteUse(hdc): SYSPAL_STATIC (1) unless SetSystemPaletteUse
  // changed it. SWSE keeps NUMCOLORS static colours in its identity palettes
  // only when it is static (1:4105, 1:4339), and RESTORESYSTEMPALETTE resets
  // it when it is not (1:4707).
  r.impl(G, "GetSystemPaletteUse", [](Call16& c) {
    c.w();
    c.ret(uint16_t(gt(c).display().palette_use()));
  });
  // The display driver's palette entries (the Windows 3.1 DDK's display
  // driver interface): SetPalette (DISPLAY.22) and GetPalette (DISPLAY.23),
  // (nStartIndex, nNumEntries, lpPalette), far Pascal, no result — what GDI
  // itself called to load the hardware palette, and what a program reaches
  // with GetModuleHandle("DISPLAY") and GetProcAddress (Intermission's Fade
  // Out reads the palette, then fades it to black and back). An entry is 4
  // bytes, red, green, blue and an unused fourth, a COLORREF's order.
  // SetPalette loads the hardware palette — the statics too, and behind
  // every logical palette's back (GDI's own record of which slot is whose
  // stays as it was) — and the screen shows the new colours at once;
  // GetPalette copies the hardware palette out, the fourth byte 0. Indices
  // past 255 are not there.
  r.add("DISPLAY", 22, "SetPalette", Conv16::pascal_, true, 8, [](Call16& c) {
    uint16_t start = c.w(), n = c.w();
    uint32_t in = c.ptr();
    Display& disp = gt(c).display();
    if (start >= 256 || !n) return;
    n = uint16_t(std::min<int>(n, 256 - start));
    std::vector<PALETTEENTRY> e(disp.system_palette().begin() + start, disp.system_palette().begin() + start + n);
    for (uint16_t k = 0; k < n; k++) {
      uint8_t q[4];
      c.rt.read_bytes(in + 4u * k, q, 4);
      e[k].peRed = q[0];
      e[k].peGreen = q[1];
      e[k].peBlue = q[2];
    }
    disp.set_system_entries(start, n, e.data());
  });
  r.add("DISPLAY", 23, "GetPalette", Conv16::pascal_, true, 8, [](Call16& c) {
    uint16_t start = c.w(), n = c.w();
    uint32_t out = c.ptr();
    const auto& sys = gt(c).display().system_palette();
    for (uint16_t k = 0; k < n && start + k < 256; k++) {
      const PALETTEENTRY& e = sys[size_t(start + k)];
      const uint8_t q[4] = {e.peRed, e.peGreen, e.peBlue, 0};
      c.rt.write_bytes(out + 4u * k, q, 4);
    }
  });
  r.impl(G, "GetNearestColor", [](Call16& c) {
    uint16_t hdc = c.w();
    uint32_t col = c.l();
    Gdi16& g = gt(c);
    Dc16* d = g.dc(hdc);
    if (d && d->dib_header) return c.ret32(g.surface_rgb(hdc, g.dib_index(*d, col)));
    c.ret32(g.index_rgb(g.display().map_index(col, g.dc_palette(hdc))));
  });
  // USER.282/283 in Win16.
  r.impl("USER", "SelectPalette", [](Call16& c) {
    uint16_t hdc = c.w(), hpal = c.w(), force = c.w();
    Gdi16& g = gt(c);
    Dc16* d = g.dc(hdc);
    if (!d || !g.get(hpal, G16::palette)) return c.ret(0);
    uint16_t old = d->s.palette;
    d->s.palette = hpal;
    d->s.force_background = force != 0;
    g.sync(hdc);  // PALETTEINDEX colours now map through the new palette
    c.ret(old);
  });
  r.impl("USER", "RealizePalette", [](Call16& c) {
    uint16_t hdc = c.w();
    Gdi16& g = gt(c);
    Dc16* d = g.dc(hdc);
    if (!d) return c.ret(0);
    // The DIB driver is no palette device (no RC_PALETTE): nothing to realize.
    if (d->dib_device) return c.ret(0);
    Obj16* p = g.get(d->s.palette, G16::palette);
    if (!p || p->stock) return c.ret(0);
    // A screen DC realizes into the hardware palette (foreground, as the
    // active saver window did); a memory DC maps onto it as it stands.
    bool background = !d->screen || d->s.force_background;
    UINT n = g.display().realize(*p->pal, background);
    g.sync(hdc);
    c.ret(uint16_t(n));
  });

  // ---- device-independent bitmaps ----
  r.impl(G, "SetDIBitsToDevice", [](Call16& c) {
    uint16_t hdc = c.w();
    int16_t x = c.sw(), y = c.sw(), cx = c.sw(), cy = c.sw(), xs = c.sw(), ys = c.sw();
    uint16_t start = c.w(), lines = c.w();
    uint32_t bits = c.ptr(), bmi = c.ptr();
    uint16_t usage = c.w();
    Gdi16& g = gt(c);
    HDC d = g.host_dc(hdc);
    Dib16 dib;
    if (!d || !read_dib(c.rt, bmi, usage, dib)) return c.ret(0);
    int n = std::min<int>(lines, dib.height);
    c.rt.charge_pixels(int64_t(std::max<int>(cx, 0)) * std::max<int>(cy, 0));
    if (mono_target(g, hdc, dib)) {
      const uint8_t* raw = n > 0 && bits ? c.rt.mem().at<uint8_t>(c.rt.linear(bits, dib.stride * uint32_t(n)),
                                                                  size_t(dib.stride) * size_t(n))
                                         : nullptr;
      RealBmi bi = real_bmi(g, dib, hdc);
      g.sync(hdc);
      return c.ret(uint16_t(::SetDIBitsToDevice(d, x, y, DWORD(std::max<int>(cx, 0)), DWORD(std::max<int>(cy, 0)), xs, ys,
                                                start, UINT(raw ? n : 0), raw, reinterpret_cast<BITMAPINFO*>(&bi),
                                                DIB_RGB_COLORS)));
    }
    std::vector<uint8_t> idx = dib_to_indices(c.rt, g, dib, bits, n, usage, hdc);
    KeyBmi kb = key_bmi(dib);
    g.sync(hdc);
    int r = ::SetDIBitsToDevice(d, x, y, DWORD(std::max<int>(cx, 0)), DWORD(std::max<int>(cy, 0)), xs, ys, start,
                                UINT(n), idx.data(), reinterpret_cast<BITMAPINFO*>(&kb), DIB_RGB_COLORS);
    c.ret(uint16_t(r));
  });
  r.impl(G, "StretchDIBits", [](Call16& c) {
    uint16_t hdc = c.w();
    int16_t x = c.sw(), y = c.sw(), w = c.sw(), h = c.sw(), xs = c.sw(), ys = c.sw(), ws = c.sw(), hs = c.sw();
    uint32_t bits = c.ptr(), bmi = c.ptr();
    uint16_t usage = c.w();
    uint32_t rop = c.l();
    Gdi16& g = gt(c);
    HDC d = g.host_dc(hdc);
    Dib16 dib;
    if (!d || !read_dib(c.rt, bmi, usage, dib)) return c.ret(0);
    // StretchDIBits' source origin is the DIB's first row in memory order
    // either way (bottom-up: y from the bottom).
    int lo = std::min<int>(ys, ys + hs), hi = std::max<int>(ys, ys + hs);
    c.rt.charge_pixels(int64_t(std::abs(int(w))) * std::abs(int(h)));
    if (mono_target(g, hdc, dib)) {
      if (!bits) return c.ret(0);
      const uint8_t* raw = c.rt.mem().at<uint8_t>(c.rt.linear(bits, dib.stride * uint32_t(dib.height)),
                                                  size_t(dib.stride) * size_t(dib.height));
      RealBmi bi = real_bmi(g, dib, hdc);
      g.sync(hdc);
      return c.ret(uint16_t(::StretchDIBits(d, x, y, w, h, xs, ys, ws, hs, raw, reinterpret_cast<BITMAPINFO*>(&bi),
                                            DIB_RGB_COLORS, rop)));
    }
    std::vector<uint8_t> idx = dib_to_indices(c.rt, g, dib, bits, dib.height, usage, hdc, lo - 1, hi + 1);
    if (tracing("dib16") && dib.h.biBitCount == 8 && dib.h.biCompression == BI_RGB) {
      // What the source rectangle holds, and what it becomes.
      std::map<int, int> src_hist, dst_hist;
      uint32_t ostride = uint32_t((dib.w + 3) & ~3);
      for (int yy = std::max(lo, 0); yy < std::min(hi, dib.height); yy++) {
        for (int xx = std::max<int>(xs, 0); xx < std::min<int>(xs + ws, dib.w); xx++) {
          src_hist[c.rt.rd8(Runtime16::huge_add(bits, uint32_t(yy) * dib.stride + uint32_t(xx)))]++;
          dst_hist[idx[size_t(yy) * ostride + size_t(xx)]]++;
        }
      }
      std::string s, t;
      for (auto& [k, v] : src_hist) s += " " + std::to_string(k) + ":" + std::to_string(v);
      for (auto& [k, v] : dst_hist) t += " " + std::to_string(k) + ":" + std::to_string(v);
      std::string p;
      for (size_t i = 0; i < dib.pal.size() && i < 24; i++) p += " " + std::to_string(dib.pal[i]);
      LogicalPalette* lp = g.dc_palette(hdc);
      trace("dib16", "StretchDIBits usage %u: source%s -> hardware%s; table%s; DC palette %04X of %zu entries", usage,
            s.c_str(), t.c_str(), p.c_str(), g.dc(hdc) ? g.dc(hdc)->s.palette : 0, lp ? lp->entries.size() : 0);
    }
    KeyBmi kb = key_bmi(dib);
    g.sync(hdc);
    int r = ::StretchDIBits(d, x, y, w, h, xs, ys, ws, hs, idx.data(), reinterpret_cast<BITMAPINFO*>(&kb),
                            DIB_RGB_COLORS, rop);
    c.ret(uint16_t(r));
  });
  r.impl(G, "SetDIBits", [](Call16& c) {
    uint16_t hdc = c.w(), hb = c.w(), start = c.w(), lines = c.w();
    uint32_t bits = c.ptr(), bmi = c.ptr();
    uint16_t usage = c.w();
    Gdi16& g = gt(c);
    Obj16* o = g.get(hb, G16::bitmap);
    Dib16 dib;
    if (!o || o->stock || !read_dib(c.rt, bmi, usage, dib)) return c.ret(0);
    c.rt.charge_pixels(int64_t(dib.w) * lines);
    c.ret(uint16_t(set_bitmap_rows(c.rt, g, *o, hdc, start, lines, bits, dib, usage)));
  });
  r.impl(G, "CreateDIBitmap", [](Call16& c) {
    uint16_t hdc = c.w();
    uint32_t hdr = c.ptr();
    uint32_t init = c.l();
    uint32_t bits = c.ptr(), bmi = c.ptr();
    uint16_t usage = c.w();
    Gdi16& g = gt(c);
    BITMAPINFOHEADER h = read16<BITMAPINFOHEADER>(c.rt, hdr);
    int w = h.biSize == sizeof(BITMAPCOREHEADER) ? int(c.rt.rd16(hdr + 4)) : int(h.biWidth);
    int ht = h.biSize == sizeof(BITMAPCOREHEADER) ? int(int16_t(c.rt.rd16(hdr + 6))) : int(h.biHeight);
    uint16_t hb = g.create_device_bitmap(w, std::abs(ht), 8);
    Obj16* o = g.get(hb, G16::bitmap);
    Dib16 dib;
    if (o && (init & 4 /*CBM_INIT*/) && bits && read_dib(c.rt, bmi, usage, dib)) {
      set_bitmap_rows(c.rt, g, *o, hdc, 0, uint16_t(dib.height), bits, dib, usage);
    }
    c.ret(hb);
  });
  r.impl(G, "GetDIBits", [](Call16& c) {
    uint16_t hdc = c.w();  // hardware indices whatever the DC; a DIB_PAL_COLORS table names its palette's entries
    uint16_t hb = c.w(), start = c.w(), lines = c.w();
    uint32_t bits = c.ptr(), bmi = c.ptr();
    uint16_t usage = c.w();
    Gdi16& g = gt(c);
    Obj16* o = g.get(hb, G16::bitmap);
    if (!o || !bmi) return c.ret(0);
    GdiFlush();
    uint32_t size = c.rt.rd32(bmi);
    if (size < sizeof(BITMAPINFOHEADER)) return c.ret(0);
    BITMAPINFOHEADER h = read16<BITMAPINFOHEADER>(c.rt, bmi);
    if (!h.biBitCount) {
      // Just describe the bitmap.
      h.biWidth = o->bmp.w;
      h.biHeight = o->bmp.h;
      h.biPlanes = 1;
      h.biBitCount = uint16_t(o->bmp.bpp);
      h.biCompression = BI_RGB;
      h.biSizeImage = uint32_t(((o->bmp.w * o->bmp.bpp + 31) / 32) * 4 * o->bmp.h);
      write16(c.rt, bmi, h);
      return c.ret(1);
    }
    int bpp = h.biBitCount;
    bool mono = o->bmp.bpp == 1;
    trace("dib16", "GetDIBits(%04X, bitmap %04X %dx%d %d-bit, scans %u+%u, usage %u): header %u bytes, %dx%d, "
          "%u bpp", hdc, hb, o->bmp.w, o->bmp.h, o->bmp.bpp, start, lines, usage, unsigned(size), int(h.biWidth),
          int(h.biHeight), bpp);
    if (mono && bpp == 1) {
      // Monochrome to 1-bit rows: real GDI's own answer, asked in a header
      // of our own — 40 bytes, the bitmap's own width and height (the sign
      // kept), as an 8-bit bitmap is answered below. Real GDI reads biSize
      // bytes of the header and writes rows as wide as its biWidth says,
      // whatever the bitmap (measured): the guest's own would run it past bi
      // and tmp.
      struct {
        BITMAPINFOHEADER h;
        RGBQUAD c[2];
      } bi{};
      bi.h = h;
      bi.h.biSize = sizeof(BITMAPINFOHEADER);
      bi.h.biWidth = o->bmp.w;
      bi.h.biHeight = h.biHeight < 0 ? -o->bmp.h : o->bmp.h;
      uint32_t rows = std::min<uint32_t>(lines, uint32_t(o->bmp.h));
      uint32_t stride = uint32_t(((o->bmp.w + 31) / 32) * 4);
      std::vector<uint8_t> tmp(size_t(stride) * rows);
      HDC screen = GetDC(nullptr);
      int got = ::GetDIBits(screen, static_cast<HBITMAP>(o->host), start, rows, bits ? tmp.data() : nullptr,
                            reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS);
      ReleaseDC(nullptr, screen);
      if (bits && got > 0) c.rt.write_bytes(bits, tmp.data(), size_t(stride) * uint32_t(got));
      bi.h.biSize = h.biSize;  // the guest's header keeps its size; its table follows it
      write16(c.rt, bmi, bi.h);
      for (int i = 0; i < 2; i++) c.rt.write_bytes(bmi + h.biSize + 4u * i, &bi.c[i], 4);
      return c.ret(uint16_t(std::max(got, 0)));
    }
    // 1-bit rows of an 8-bit bitmap, 16- and 32-bit rows: refused; so is the
    // stock 1x1 bitmap, which has no pixels here.
    if ((bpp != 4 && bpp != 8 && bpp != 24) || (mono && !o->host)) return c.ret(0);
    int w = o->bmp.w, ht = o->bmp.h;
    bool top_down = h.biHeight < 0;
    uint32_t stride = uint32_t(((w * bpp + 31) / 32) * 4);
    // The colour table: what each value returned means. 8-bit rows are the
    // hardware indices themselves; 4-bit rows each pixel's nearest of the 16
    // colours real GDI's table holds (kDib4Colors; Haunted captures the
    // desktop a line at a time so). A DIB_PAL_COLORS table names for each
    // value the DC palette's logical entry nearest its colour: the table
    // that describes the values returned, which a SetDIBits through the same
    // palette turns back into their colours. ADXPL41 draws its labels on a
    // canvas it round-trips so, SetDIBits → GDI → GetDIBits → SetDIBits,
    // through a 255-entry palette holding the high statics at 245..254;
    // with an identity table white (hardware 255) fell off the palette and
    // 246..254 moved one static on (Pepe's black label boxes, Sam's and
    // Taz's broken folders).
    LogicalPalette* lp = usage == DIB_PAL_COLORS ? g.dc_palette(hdc) : nullptr;
    std::array<uint8_t, 256> to4{};
    if (bpp == 4) {
      for (int i = 0; i < 256; i++) to4[size_t(i)] = uint8_t(Display::nearest_in(kDib4Colors, 16, g.index_rgb(i)));
    }
    if (bpp == 8 || bpp == 4) {
      for (int i = 0; i < (bpp == 8 ? 256 : 16); i++) {
        uint32_t a = bmi + h.biSize + (usage == DIB_PAL_COLORS ? 2u : 4u) * uint32_t(i);
        const RGBQUAD& q = kDib4Colors[i & 15];
        COLORREF col = bpp == 8 ? g.index_rgb(i) : RGB(q.rgbRed, q.rgbGreen, q.rgbBlue);
        if (usage == DIB_PAL_COLORS) {
          c.rt.wr16(a, uint16_t(lp && !lp->entries.empty() ? Display::nearest_in(*lp, col) : i));
        } else {
          c.rt.wr32(a, uint32_t(GetBValue(col)) | (uint32_t(GetGValue(col)) << 8) | (uint32_t(GetRValue(col)) << 16));
        }
      }
    }
    h.biWidth = w;
    h.biHeight = top_down ? -ht : ht;
    h.biCompression = BI_RGB;
    h.biSizeImage = stride * uint32_t(ht);
    h.biClrUsed = bpp == 8 ? 256 : 0;
    write16(c.rt, bmi, h);
    if (!bits) return c.ret(uint16_t(ht));
    // A monochrome bitmap's pixels become the hardware indices of black (0)
    // and white (255), statics under SYSPAL_STATIC and SYSPAL_NOSTATIC
    // alike, which the table above describes as for an 8-bit bitmap: real
    // GDI's values too (black 0 and white 255 in 8-bit rows, 0 and 15 in
    // 4-bit ones, 000000 and FFFFFF in 24-bit ones; measured).
    std::vector<uint8_t> packed, wide;
    if (mono) {
      packed.resize(size_t(o->bmp.stride) * size_t(ht));
      GetBitmapBits(static_cast<HBITMAP>(o->host), LONG(packed.size()), packed.data());
      wide.resize(size_t(w));
    }
    int n = 0;
    std::vector<uint8_t> row(stride);
    for (int i = 0; i < lines; i++) {
      int scan = start + i;
      if (scan >= ht) break;
      int y = top_down ? scan : ht - 1 - scan;
      const uint8_t* src;
      if (mono) {
        const uint8_t* p = packed.data() + size_t(y) * o->bmp.stride;
        for (int x = 0; x < w; x++) wide[size_t(x)] = ((p[x >> 3] >> (7 - (x & 7))) & 1) ? 255 : 0;
        src = wide.data();
      } else {
        src = o->bmp.bits + size_t(y) * o->bmp.stride;
      }
      std::fill(row.begin(), row.end(), 0);
      for (int x = 0; x < w; x++) {
        if (bpp == 8) {
          row[size_t(x)] = src[x];
        } else if (bpp == 4) {
          row[size_t(x) >> 1] |= uint8_t(to4[src[x]] << ((x & 1) ? 0 : 4));
        } else {
          COLORREF col = g.index_rgb(src[x]);
          row[3 * size_t(x)] = GetBValue(col);
          row[3 * size_t(x) + 1] = GetGValue(col);
          row[3 * size_t(x) + 2] = GetRValue(col);
        }
      }
      c.rt.write_bytes(Runtime16::huge_add(bits, uint32_t(i) * stride), row.data(), stride);
      n++;
    }
    c.ret(uint16_t(n));
  });

  // ---- printing and escapes: no printer ----
  r.impl(G, "Escape", [](Call16& c) { c.ret32(0); });
  r.impl(G, "StartDoc", [](Call16& c) { c.ret(0xFFFF); });

  // ---- arithmetic ----
  r.impl(G, "MulDiv", [](Call16& c) {
    int16_t a = c.sw(), b = c.sw(), d = c.sw();
    c.ret(uint16_t(gdi16_muldiv(a, b, d)));
  });
}

// MulDiv(a, b, c), Win16's (GDI.128): a*b with a 32-bit product, divided by
// c and rounded to the nearest integer, halves away from zero (the sign
// taken from a and b once c is made positive); -32768 when c is 0 or the
// result leaves -32767..32767 ("-32768 if either an overflow occurred or
// nDivisor was 0", the Windows 3.1 SDK; the same steps as Wine's MulDiv16,
// 16-bit negation included).
int16_t gdi16_muldiv(int16_t a, int16_t b, int16_t c) {
  if (!c) return -32768;
  if (c < 0) {
    a = int16_t(-a);
    c = int16_t(-c);
  }
  int32_t p = int32_t(a) * b;
  int32_t r = ((a < 0) == (b < 0)) ? (p + c / 2) / c : (p - c / 2) / c;
  if (r > 32767 || r < -32767) return -32768;
  return int16_t(r);
}

}  // namespace adw::win16
