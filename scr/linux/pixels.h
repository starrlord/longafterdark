// Frames into X images, the pure part of the Linux player's presentation:
// the pixel formats it draws in, and the scaler that writes a frame into an
// image. No X calls, so the unit tests cover every format and size.
//
// A format is any TrueColor visual at 16, 24 or 32 bits per pixel, taken by
// its channel masks: 8-8-8 RGB or BGR, 5-6-5, 5-5-5, 10-10-10 (depth 30),
// with a depth-32 visual's alpha bits set (opaque), in either image byte
// order. Anything else (PseudoColor, DirectColor, 8 bits per pixel, masks
// that overlap, have holes or lie outside the depth) is refused with a
// reason, never drawn in wrongly.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "frame_parser.h"

namespace lad {

// X's visual classes (X.h), so this header needs no X headers.
inline constexpr int kStaticGray = 0, kGrayScale = 1, kStaticColor = 2, kPseudoColor = 3, kTrueColor = 4,
                     kDirectColor = 5;

struct PixelFormat {
  int bytes = 4;                  // bytes per pixel in an image: 2, 3 or 4
  bool msb_first = false;         // the image's byte order: most significant byte first
  uint32_t mask[3] = {0, 0, 0};   // red, green, blue
  int shift[3] = {0, 0, 0};       // each mask's lowest bit
  int bits[3] = {0, 0, 0};        // and its width
  uint32_t fill = 0;              // the depth's bits outside the channels (a depth-32 visual's alpha): set
  int depth = 0;
};

// The format of a visual, from what X says of it: the visual's class
// (Visual::c_class) and masks, its depth, and the bits per pixel and byte
// order of an image of that depth. False, with *why saying what the visual
// is ("an 8-bit PseudoColor visual"), for anything the player can't draw in.
bool make_pixel_format(int visual_class, int depth, int bits_per_pixel, bool msb_first, unsigned long red_mask,
                       unsigned long green_mask, unsigned long blue_mask, PixelFormat* out, std::string* why);

// "TrueColor, depth 24, 32 bits per pixel, RGB 8-8-8, LSB first", for the log.
std::string describe_format(const PixelFormat& pf);

// The pixel value of an 8-bit-per-channel colour: each channel scaled to
// its mask's width (rounded), the fill bits set.
uint32_t pack_rgb(const PixelFormat& pf, uint8_t r, uint8_t g, uint8_t b);

// Nearest-neighbour scaling of frames into images of one format.
class FrameScaler {
 public:
  // Writes `f` scaled to w x h pixels into an image in format `pf` whose
  // rows are `stride` bytes apart, starting at `data`: the first w *
  // pf.bytes bytes of each of the h rows and nothing else, so never past
  // the image (the caller's image holds at least stride * h bytes, with
  // stride >= w * pf.bytes). A row that samples the same frame row as the
  // row above is copied from it. A frame whose buffers are shorter than its
  // header says, or an empty size, writes nothing.
  void scale(const RawFrame& f, const PixelFormat& pf, uint8_t* data, size_t stride, int w, int h);

 private:
  void tables_for(const PixelFormat& pf);

  std::vector<uint32_t> cols_;   // the frame column of each image column
  int cols_src_ = -1, cols_dst_ = -1;
  bool have_tables_ = false;
  PixelFormat tables_pf_{};
  uint32_t ch_[3][256] = {};     // each channel's value per 8-bit level, shifted into place
  std::vector<uint32_t> line_;   // one row's pixel values
};

}  // namespace lad
