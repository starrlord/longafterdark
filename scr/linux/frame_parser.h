// Incremental parser for the host's stdout frame stream (DESIGN.md §1), the
// Windows saver's (scr/src/frame_parser.*) without Windows types:
//   P8: "P8\n<w> <h>\n" + 768-byte RGB palette + w*h index bytes
//   P6: "P6\n<w> <h>\n255\n" + w*h*3 RGB bytes
// Whitespace and '#' comments separate the tokens, and exactly one delimiter
// byte precedes the body. The parser resynchronises on garbage instead of
// stalling: a host that leaks a stray printf to stdout costs one frame, not
// the stream. Every header value is checked before it is used: dimensions
// are digits only, 1..8192 per axis and at most 4096x4096 pixels in all, a
// header is at most 4096 bytes, and a P6 maxval is 1..255; anything else is
// garbage and costs one resync rather than a wait for a body that never
// comes (or an allocation of gigabytes).
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace lad {

inline constexpr int kMaxFrameDim = 8192;                            // per axis
inline constexpr size_t kMaxFramePixels = size_t(4096) * 4096;       // per frame
inline constexpr size_t kMaxFrameHeader = 4096;                      // bytes

struct RawFrame {
  int width = 0, height = 0;
  int format = 0;                 // 8 = P8 (palette + indices), 6 = P6 (RGB)
  std::vector<uint8_t> palette;   // P8: 768 bytes (256 x RGB); P6: empty
  std::vector<uint8_t> pixels;    // P8: w*h indices; P6: w*h*3 RGB; top-down, unpadded
};

struct FrameHeader {
  size_t body_start = 0;          // offset of the first body byte
  int width = 0, height = 0, format = 0;
};

enum class HeaderResult { ok, need_more, invalid };

HeaderResult parse_frame_header(const uint8_t* p, size_t n, FrameHeader& out);
size_t frame_body_size(const FrameHeader& h);

class FrameParser {
 public:
  void feed(const void* data, size_t n);
  // Moves the next complete frame into `out` (reusing its buffers). Returns
  // false when no whole frame is buffered yet.
  bool take(RawFrame& out);
  size_t buffered() const { return buf_.size() - head_; }
  uint64_t resyncs() const { return resyncs_; }

 private:
  void resync();
  void compact();

  std::vector<uint8_t> buf_;
  size_t head_ = 0;
  uint64_t resyncs_ = 0;
};

}  // namespace lad
