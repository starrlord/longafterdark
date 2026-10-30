#include "desktop_seed.h"

#include <cstring>

namespace adw::scr {

std::vector<uint8_t> encode_p6(const uint8_t* bgrx, int w, int h, int stride) {
  std::vector<uint8_t> out;
  if (!bgrx || w <= 0 || h <= 0) return out;
  std::string header = "P6\n" + std::to_string(w) + " " + std::to_string(h) + "\n255\n";
  out.resize(header.size() + (size_t)w * h * 3);
  memcpy(out.data(), header.data(), header.size());
  uint8_t* d = out.data() + header.size();
  for (int y = 0; y < h; ++y) {
    const uint8_t* s = bgrx + (size_t)y * stride;
    for (int x = 0; x < w; ++x, s += 4, d += 3) {
      d[0] = s[2];
      d[1] = s[1];
      d[2] = s[0];
    }
  }
  return out;
}

HANDLE write_seed_file(const std::wstring& path, const std::vector<uint8_t>& p6, std::wstring* error) {
  auto fail = [&](const wchar_t* what) {
    if (error) *error = std::wstring(what) + L" failed (error " + std::to_wstring(GetLastError()) + L")";
    return INVALID_HANDLE_VALUE;
  };
  if (p6.empty()) {
    SetLastError(ERROR_INVALID_DATA);
    return fail(L"capture");
  }
  // Not inheritable: the hosts open it by name (their own handles), so no
  // host outlives it by holding ours.
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ | FILE_SHARE_DELETE,
                         nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
  if (h == INVALID_HANDLE_VALUE) return fail(L"CreateFile");
  size_t off = 0;
  while (off < p6.size()) {
    DWORD put = 0;
    DWORD chunk = (DWORD)std::min<size_t>(p6.size() - off, 1u << 20);
    if (!WriteFile(h, p6.data() + off, chunk, &put, nullptr) || put == 0) {
      HANDLE bad = fail(L"WriteFile");
      CloseHandle(h);   // deletes it
      return bad;
    }
    off += put;
  }
  return h;
}

std::vector<uint8_t> capture_monitor_p6(const RECT& monitor, SizeI emu) {
  const int mw = monitor.right - monitor.left, mh = monitor.bottom - monitor.top;
  std::vector<uint8_t> out;
  capture_monitor_shots(monitor, {{{0, 0, mw, mh}, emu}}, [&](size_t, const std::vector<uint8_t>& p6) { out = p6; });
  return out;
}

namespace {
// A top-down 32-bpp DIB section of w x h.
HBITMAP make_dib(HDC dc, int w, int h, void** bits) {
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;   // top-down
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  return CreateDIBSection(dc, &bi, DIB_RGB_COLORS, bits, nullptr, 0);
}
}  // namespace

void capture_monitor_shots(const RECT& monitor, const std::vector<SeedShot>& shots, const SeedTake& take) {
  const int mw = monitor.right - monitor.left, mh = monitor.bottom - monitor.top;
  bool taken = false;
  HDC screen = mw > 0 && mh > 0 && !shots.empty() ? GetDC(nullptr) : nullptr;
  if (screen) {
    HDC src = CreateCompatibleDC(screen);
    void* src_bits = nullptr;
    HBITMAP src_bmp = src ? make_dib(screen, mw, mh, &src_bits) : nullptr;
    if (src_bmp) {
      HGDIOBJ old_src = SelectObject(src, src_bmp);
      // CAPTUREBLT: layered windows too, as the user sees the desktop.
      if (BitBlt(src, 0, 0, mw, mh, screen, monitor.left, monitor.top, SRCCOPY | CAPTUREBLT)) {
        shrink_parts(src, mw, mh, shots, take);
        taken = true;
      }
      SelectObject(src, old_src);
      DeleteObject(src_bmp);
    }
    if (src) DeleteDC(src);
    ReleaseDC(nullptr, screen);
  }
  if (!taken) {
    for (size_t i = 0; i < shots.size(); ++i) take(i, {});
  }
}

void capture_spanned_shots(const RECT& bounds,
                           const std::vector<RECT>& monitors,
                           const std::vector<SeedShot>& shots,
                           const SeedTake& take) {
  const int vw = bounds.right - bounds.left, vh = bounds.bottom - bounds.top;
  bool taken = false;
  HDC screen = vw > 0 && vh > 0 && !shots.empty() ? GetDC(nullptr) : nullptr;
  if (screen) {
    HDC src = CreateCompatibleDC(screen);
    void* src_bits = nullptr;
    HBITMAP src_bmp = src ? make_dib(screen, vw, vh, &src_bits) : nullptr;
    if (src_bmp) {
      memset(src_bits, 0, (size_t)vw * vh * 4);
      HGDIOBJ old_src = SelectObject(src, src_bmp);
      bool any_blt = false;
      for (const RECT& m : monitors) {
        const int mw = m.right - m.left, mh = m.bottom - m.top;
        if (mw > 0 && mh > 0) {
          if (BitBlt(src, m.left - bounds.left, m.top - bounds.top, mw, mh,
                     screen, m.left, m.top, SRCCOPY | CAPTUREBLT)) {
            any_blt = true;
          }
        }
      }
      if (any_blt) {
        shrink_parts(src, vw, vh, shots, take);
        taken = true;
      }
      SelectObject(src, old_src);
      DeleteObject(src_bmp);
    }
    if (src) DeleteDC(src);
    ReleaseDC(nullptr, screen);
  }
  if (!taken) {
    for (size_t i = 0; i < shots.size(); ++i) take(i, {});
  }
}

void shrink_parts(HDC src, int mw, int mh, const std::vector<SeedShot>& shots, const SeedTake& take) {
  HDC dst = src ? CreateCompatibleDC(src) : nullptr;
  for (size_t i = 0; i < shots.size(); ++i) {
    const SeedShot& s = shots[i];
    std::vector<uint8_t> p6;
    if (dst && s.emu.w > 0 && s.emu.h > 0 && s.src.w > 0 && s.src.h > 0 && s.src.x >= 0 && s.src.y >= 0 &&
        s.src.x + s.src.w <= mw && s.src.y + s.src.h <= mh) {
      void* dst_bits = nullptr;
      if (HBITMAP dst_bmp = make_dib(src, s.emu.w, s.emu.h, &dst_bits)) {
        HGDIOBJ old_dst = SelectObject(dst, dst_bmp);
        SetStretchBltMode(dst, HALFTONE);
        SetBrushOrgEx(dst, 0, 0, nullptr);
        if (StretchBlt(dst, 0, 0, s.emu.w, s.emu.h, src, s.src.x, s.src.y, s.src.w, s.src.h, SRCCOPY)) {
          GdiFlush();
          p6 = encode_p6(static_cast<const uint8_t*>(dst_bits), s.emu.w, s.emu.h, s.emu.w * 4);
        }
        SelectObject(dst, old_dst);
        DeleteObject(dst_bmp);   // before it is taken: the P6 alone is held while it is written
      }
    }
    take(i, p6);
  }
  if (dst) DeleteDC(dst);
}

}  // namespace adw::scr