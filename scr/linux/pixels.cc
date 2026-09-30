#include "pixels.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace lad {

namespace {

// A mask's lowest bit and width; false when it is empty or has holes.
bool mask_shape(uint32_t m, int* shift, int* bits) {
  if (m == 0) return false;
  int s = 0;
  while (!(m & 1u)) {
    m >>= 1;
    ++s;
  }
  int b = 0;
  while (m & 1u) {
    m >>= 1;
    ++b;
  }
  if (m != 0) return false;   // bits above a gap
  *shift = s;
  *bits = b;
  return true;
}

const char* class_name(int c) {
  switch (c) {
    case kStaticGray: return "StaticGray";
    case kGrayScale: return "GrayScale";
    case kStaticColor: return "StaticColor";
    case kPseudoColor: return "PseudoColor";
    case kTrueColor: return "TrueColor";
    case kDirectColor: return "DirectColor";
  }
  return "unknown";
}

uint32_t level(uint8_t c, int bits) {
  const uint32_t top = bits >= 32 ? 0xFFFFFFFFu : (1u << bits) - 1u;
  return (uint32_t)(((uint64_t)c * top + 127) / 255);
}

constexpr bool kHostMsbFirst = __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__;

// One row of pixel values into an image row, in the image's byte order.
template <int Bytes, bool Msb>
void store_row(uint8_t* dst, const uint32_t* v, int w) {
  if constexpr (Bytes == 4) {
    if constexpr (Msb == kHostMsbFirst) {
      memcpy(dst, v, (size_t)w * 4);
    } else {
      for (int x = 0; x < w; ++x) {
        const uint32_t s = __builtin_bswap32(v[x]);
        memcpy(dst + (size_t)x * 4, &s, 4);
      }
    }
  } else if constexpr (Bytes == 2) {
    for (int x = 0; x < w; ++x) {
      uint16_t s = (uint16_t)v[x];
      if constexpr (Msb != kHostMsbFirst) s = __builtin_bswap16(s);
      memcpy(dst + (size_t)x * 2, &s, 2);
    }
  } else {
    for (int x = 0; x < w; ++x) {
      uint8_t* p = dst + (size_t)x * 3;
      const uint32_t s = v[x];
      if constexpr (Msb) {
        p[0] = (uint8_t)(s >> 16);
        p[1] = (uint8_t)(s >> 8);
        p[2] = (uint8_t)s;
      } else {
        p[0] = (uint8_t)s;
        p[1] = (uint8_t)(s >> 8);
        p[2] = (uint8_t)(s >> 16);
      }
    }
  }
}

void store(const PixelFormat& pf, uint8_t* dst, const uint32_t* v, int w) {
  switch (pf.bytes * 2 + (pf.msb_first ? 1 : 0)) {
    case 8: store_row<4, false>(dst, v, w); break;
    case 9: store_row<4, true>(dst, v, w); break;
    case 4: store_row<2, false>(dst, v, w); break;
    case 5: store_row<2, true>(dst, v, w); break;
    case 6: store_row<3, false>(dst, v, w); break;
    case 7: store_row<3, true>(dst, v, w); break;
    default: break;   // no other format is ever made
  }
}

bool same_format(const PixelFormat& a, const PixelFormat& b) {
  return a.bytes == b.bytes && a.msb_first == b.msb_first && a.fill == b.fill && a.mask[0] == b.mask[0] &&
         a.mask[1] == b.mask[1] && a.mask[2] == b.mask[2];
}

}  // namespace

bool make_pixel_format(int visual_class, int depth, int bits_per_pixel, bool msb_first, unsigned long red_mask,
                       unsigned long green_mask, unsigned long blue_mask, PixelFormat* out, std::string* why) {
  auto refuse = [&](const std::string& text) {
    if (why) *why = text;
    return false;
  };
  const bool an = depth == 8 || depth == 11 || depth == 18;
  const std::string what =
      std::string(an ? "an " : "a ") + std::to_string(depth) + "-bit " + class_name(visual_class) + " visual";
  if (visual_class != kTrueColor) return refuse(what + " (only TrueColor visuals are drawn in)");
  if (bits_per_pixel != 16 && bits_per_pixel != 24 && bits_per_pixel != 32) {
    return refuse(what + " at " + std::to_string(bits_per_pixel) + " bits per pixel (only 16, 24 or 32 are drawn in)");
  }
  if (depth < 3 || depth > bits_per_pixel) {
    return refuse(what + " at " + std::to_string(bits_per_pixel) + " bits per pixel");
  }
  const uint64_t depth_bits = depth >= 32 ? 0xFFFFFFFFull : (1ull << depth) - 1;
  const unsigned long masks[3] = {red_mask, green_mask, blue_mask};
  PixelFormat pf;
  pf.bytes = bits_per_pixel / 8;
  pf.msb_first = msb_first;
  pf.depth = depth;
  uint64_t all = 0;
  for (int c = 0; c < 3; ++c) {
    const uint64_t m = masks[c];
    if (m > 0xFFFFFFFFull || (m & ~depth_bits) || (m & all) ||
        !mask_shape((uint32_t)m, &pf.shift[c], &pf.bits[c]) || pf.bits[c] > 16) {
      char buf[96];
      snprintf(buf, sizeof(buf), " with masks 0x%lx, 0x%lx, 0x%lx", red_mask, green_mask, blue_mask);
      return refuse(what + buf);
    }
    pf.mask[c] = (uint32_t)m;
    all |= m;
  }
  pf.fill = (uint32_t)(depth_bits & ~all);
  if (out) *out = pf;
  return true;
}

std::string describe_format(const PixelFormat& pf) {
  // The channels from the most significant down: RGB or BGR.
  int order[3] = {0, 1, 2};
  std::sort(order, order + 3, [&](int a, int b) { return pf.shift[a] > pf.shift[b]; });
  std::string names, sizes;
  for (int i = 0; i < 3; ++i) {
    names += "RGB"[order[i]];
    sizes += (i ? "-" : "") + std::to_string(pf.bits[order[i]]);
  }
  return "TrueColor, depth " + std::to_string(pf.depth) + ", " + std::to_string(pf.bytes * 8) + " bits per pixel, " +
         names + " " + sizes + (pf.fill ? " (alpha set)" : "") + (pf.msb_first ? ", MSB first" : ", LSB first");
}

uint32_t pack_rgb(const PixelFormat& pf, uint8_t r, uint8_t g, uint8_t b) {
  const uint8_t c[3] = {r, g, b};
  uint32_t v = pf.fill;
  for (int i = 0; i < 3; ++i) v |= level(c[i], pf.bits[i]) << pf.shift[i];
  return v;
}

void FrameScaler::tables_for(const PixelFormat& pf) {
  if (have_tables_ && same_format(pf, tables_pf_)) return;
  for (int c = 0; c < 3; ++c) {
    for (int i = 0; i < 256; ++i) ch_[c][i] = level((uint8_t)i, pf.bits[c]) << pf.shift[c];
  }
  tables_pf_ = pf;
  have_tables_ = true;
}

void FrameScaler::scale(const RawFrame& f, const PixelFormat& pf, uint8_t* data, size_t stride, int w, int h) {
  if (!data || w <= 0 || h <= 0 || f.width <= 0 || f.height <= 0) return;
  if (pf.bytes < 2 || pf.bytes > 4 || stride < (size_t)w * (size_t)pf.bytes) return;
  const size_t npix = (size_t)f.width * (size_t)f.height;
  if (f.format == 8) {
    if (f.palette.size() < 768 || f.pixels.size() < npix) return;
  } else if (f.format == 6) {
    if (f.pixels.size() < npix * 3) return;
  } else {
    return;
  }
  tables_for(pf);
  if (cols_src_ != f.width || cols_dst_ != w) {
    cols_.resize((size_t)w);
    for (int x = 0; x < w; ++x) cols_[(size_t)x] = (uint32_t)((uint64_t)x * (uint64_t)f.width / (uint64_t)w);
    cols_src_ = f.width;
    cols_dst_ = w;
  }
  line_.resize((size_t)w);
  uint32_t pal[256];
  if (f.format == 8) {
    const uint8_t* p = f.palette.data();
    for (int i = 0; i < 256; ++i) pal[i] = ch_[0][p[i * 3]] | ch_[1][p[i * 3 + 1]] | ch_[2][p[i * 3 + 2]] | pf.fill;
  }
  const size_t row_bytes = (size_t)w * (size_t)pf.bytes;
  size_t prev_sy = (size_t)-1;
  for (int y = 0; y < h; ++y) {
    const size_t sy = (size_t)((uint64_t)y * (uint64_t)f.height / (uint64_t)h);
    uint8_t* dst = data + (size_t)y * stride;
    if (sy == prev_sy) {
      memcpy(dst, dst - stride, row_bytes);
      continue;
    }
    prev_sy = sy;
    uint32_t* v = line_.data();
    if (f.format == 8) {
      const uint8_t* src = f.pixels.data() + sy * (size_t)f.width;
      for (int x = 0; x < w; ++x) v[x] = pal[src[cols_[(size_t)x]]];
    } else {
      const uint8_t* src = f.pixels.data() + sy * (size_t)f.width * 3;
      for (int x = 0; x < w; ++x) {
        const uint8_t* p = src + (size_t)cols_[(size_t)x] * 3;
        v[x] = ch_[0][p[0]] | ch_[1][p[1]] | ch_[2][p[2]] | pf.fill;
      }
    }
    store(pf, dst, v, w);
  }
}

}  // namespace lad
