#include "sevenzip.h"

#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <set>

#include "names.h"
#include "status.h"

namespace adw::import {

namespace {

constexpr uint8_t kSignature[6] = {'7', 'z', 0xBC, 0xAF, 0x27, 0x1C};
constexpr size_t kSignatureHeaderSize = 32;
constexpr size_t kChunk = 64 * 1024;

// Property ids (7zFormat.txt).
enum : uint8_t {
  kEnd = 0x00,
  kHeader = 0x01,
  kArchiveProperties = 0x02,
  kAdditionalStreamsInfo = 0x03,
  kMainStreamsInfo = 0x04,
  kFilesInfo = 0x05,
  kPackInfo = 0x06,
  kUnpackInfo = 0x07,
  kSubStreamsInfo = 0x08,
  kSize = 0x09,
  kCrc = 0x0A,
  kFolder = 0x0B,
  kCodersUnpackSize = 0x0C,
  kNumUnpackStream = 0x0D,
  kEmptyStream = 0x0E,
  kEmptyFile = 0x0F,
  kAnti = 0x10,
  kName = 0x11,
  kMTime = 0x14,
  kWinAttributes = 0x15,
  kEncodedHeader = 0x17,
};

constexpr uint32_t kAttributeDirectory = 0x10;  // FILE_ATTRIBUTE_DIRECTORY

uint32_t le32(const uint8_t* p) { return uint32_t(p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24); }
uint64_t le64(const uint8_t* p) { return le32(p) | uint64_t(le32(p + 4)) << 32; }

uint32_t crc32_of(const uint8_t* p, size_t n) {
  uLong c = crc32(0, nullptr, 0);
  while (n) {
    const uInt k = uInt(std::min<size_t>(n, 1u << 30));
    c = crc32(c, p, k);
    p += k;
    n -= k;
  }
  return uint32_t(c);
}

std::string mb(uint64_t bytes) { return std::to_string((bytes + (1 << 20) - 1) >> 20) + " MB"; }

// A coder's method by its id (Methods.txt), for the three this reader
// decodes and for the messages that refuse the others.
enum class Method { copy, lzma, lzma2, other };

struct KnownMethod {
  uint8_t size;
  uint8_t id[4];
  const char* name;
  Method method;
};

const KnownMethod kMethods[] = {
    {1, {0x00}, "Copy", Method::copy},
    {1, {0x21}, "LZMA2", Method::lzma2},
    {3, {0x03, 0x01, 0x01}, "LZMA", Method::lzma},
    {1, {0x03}, "Delta", Method::other},
    {1, {0x04}, "BCJ (x86)", Method::other},
    {1, {0x05}, "PPC", Method::other},
    {1, {0x06}, "IA64", Method::other},
    {1, {0x07}, "ARM", Method::other},
    {1, {0x08}, "ARMT", Method::other},
    {1, {0x09}, "SPARC", Method::other},
    {1, {0x0A}, "ARM64", Method::other},
    {1, {0x0B}, "RISCV", Method::other},
    {4, {0x03, 0x03, 0x01, 0x03}, "BCJ (x86)", Method::other},
    {4, {0x03, 0x03, 0x01, 0x1B}, "BCJ2", Method::other},
    {4, {0x03, 0x03, 0x02, 0x05}, "PPC", Method::other},
    {4, {0x03, 0x03, 0x04, 0x01}, "IA64", Method::other},
    {4, {0x03, 0x03, 0x05, 0x01}, "ARM", Method::other},
    {4, {0x03, 0x03, 0x07, 0x01}, "ARMT", Method::other},
    {4, {0x03, 0x03, 0x08, 0x05}, "SPARC", Method::other},
    {3, {0x03, 0x04, 0x01}, "PPMd", Method::other},
    {3, {0x04, 0x01, 0x08}, "Deflate", Method::other},
    {3, {0x04, 0x01, 0x09}, "Deflate64", Method::other},
    {3, {0x04, 0x02, 0x02}, "BZip2", Method::other},
    {4, {0x06, 0xF1, 0x07, 0x01}, "7zAES", Method::other},
};

const KnownMethod* known_method(const std::vector<uint8_t>& id) {
  for (const KnownMethod& k : kMethods)
    if (std::equal(id.begin(), id.end(), k.id, k.id + k.size)) return &k;
  return nullptr;
}

std::string method_name(const std::vector<uint8_t>& id) {
  if (const KnownMethod* k = known_method(id)) return k->name;
  static const char* hex = "0123456789ABCDEF";
  std::string s = "method ";
  for (uint8_t b : id) s += std::string{hex[b >> 4], hex[b & 15]};
  return id.empty() ? "an empty method id" : s;
}

// ---- the header's encoding ----------------------------------------------------------

// Bounded reads over the header's bytes; every overrun is a damaged header.
class Reader {
 public:
  Reader(const uint8_t* p, size_t n, const std::string& archive) : p_(p), end_(p + n), archive_(&archive) {}

  [[noreturn]] void fail(const std::string& why) const { throw SevenZipError(*archive_ + ": damaged header (" + why + ")"); }
  [[noreturn]] void refuse(const std::string& why) const { throw SevenZipError(*archive_ + ": " + why); }

  size_t left() const { return size_t(end_ - p_); }
  uint8_t byte() {
    if (p_ == end_) fail("it ends early");
    return *p_++;
  }
  const uint8_t* bytes(uint64_t n) {
    if (n > left()) fail("it ends early");
    const uint8_t* at = p_;
    p_ += n;
    return at;
  }
  uint32_t u32() { return le32(bytes(4)); }
  uint64_t u64() { return le64(bytes(8)); }
  // 7z's variable-length number: the first byte's leading 1 bits count the
  // little-endian bytes that follow; its remaining bits are the top ones.
  uint64_t number() {
    const uint8_t first = byte();
    uint64_t value = 0;
    uint8_t mask = 0x80;
    for (int i = 0; i < 8; i++) {
      if (!(first & mask)) return value | uint64_t(first & (mask - 1)) << (8 * i);
      value |= uint64_t(byte()) << (8 * i);
      mask >>= 1;
    }
    return value;
  }
  // A count of things that each take at least a byte of the header, at most
  // `cap` of them.
  size_t count(uint64_t cap, const char* what) {
    const uint64_t n = number();
    if (n > cap) refuse("more than " + std::to_string(cap) + " " + what + " (" + std::to_string(n) + ")");
    if (n > left()) fail(std::to_string(n) + " " + what + " in " + std::to_string(left()) + " bytes");
    return size_t(n);
  }
  // A property's bytes, as a reader of their own.
  Reader sub(uint64_t n) {
    const uint8_t* at = bytes(n);
    return Reader(at, size_t(n), *archive_);
  }
  // A vector of bits, most significant first.
  std::vector<bool> bits(size_t n) {
    std::vector<bool> v(n);
    uint8_t b = 0, mask = 0;
    for (size_t i = 0; i < n; i++) {
      if (!mask) {
        b = byte();
        mask = 0x80;
      }
      v[i] = (b & mask) != 0;
      mask >>= 1;
    }
    return v;
  }
  // "All are defined" or a bit per item.
  std::vector<bool> defined(size_t n) { return byte() ? std::vector<bool>(n, true) : bits(n); }
  std::vector<std::optional<uint32_t>> digests(size_t n) {
    const std::vector<bool> def = defined(n);
    std::vector<std::optional<uint32_t>> out(n);
    for (size_t i = 0; i < n; i++)
      if (def[i]) out[i] = u32();
    return out;
  }
  void expect(uint8_t id, const char* what) {
    if (byte() != id) fail(std::string("no ") + what + " where one belongs");
  }
  void done(const char* what) const {
    if (left()) fail(std::string(what) + " is longer than its contents");
  }

 private:
  const uint8_t* p_;
  const uint8_t* end_;
  const std::string* archive_;
};

// ---- LZMA (lzma-specification.txt) ----------------------------------------------------

class Damaged {
 public:
  explicit Damaged(std::string why) : why(std::move(why)) {}
  std::string why;
};

class RangeDecoder {
 public:
  void init(const uint8_t* p, size_t n) {
    start_ = p_ = p;
    end_ = p + n;
    if (n < 5) throw Damaged("the range coder's first five bytes are missing");
    if (*p_++ != 0) throw Damaged("the range coder's first byte is not zero");
    code_ = 0;
    for (int i = 0; i < 4; i++) code_ = code_ << 8 | *p_++;
    range_ = 0xFFFFFFFF;
    if (code_ == range_) throw Damaged("the range coder starts at its end");
  }
  unsigned bit(uint16_t& prob) {
    const uint32_t bound = (range_ >> 11) * prob;
    unsigned b;
    if (code_ < bound) {
      prob = uint16_t(prob + ((2048 - prob) >> 5));
      range_ = bound;
      b = 0;
    } else {
      prob = uint16_t(prob - (prob >> 5));
      code_ -= bound;
      range_ -= bound;
      b = 1;
    }
    normalize();
    return b;
  }
  uint32_t direct(unsigned n) {
    uint32_t res = 0;
    do {
      range_ >>= 1;
      code_ -= range_;
      const uint32_t t = 0 - (code_ >> 31);
      code_ += range_ & t;
      if (code_ == range_) throw Damaged("the range coder overflows");
      normalize();
      res = (res << 1) + (t + 1);
    } while (--n);
    return res;
  }
  bool flushed() const { return code_ == 0; }
  size_t consumed() const { return size_t(p_ - start_); }

 private:
  void normalize() {
    if (range_ < (1u << 24)) {
      if (p_ == end_) throw Damaged("the packed data ends early");
      range_ <<= 8;
      code_ = code_ << 8 | *p_++;
    }
  }
  const uint8_t* start_ = nullptr;
  const uint8_t* p_ = nullptr;
  const uint8_t* end_ = nullptr;
  uint32_t range_ = 0, code_ = 0;
};

constexpr unsigned kNumStates = 12, kPosBitsMax = 4, kNumLenToPosStates = 4, kEndPosModelIndex = 14,
                   kNumFullDistances = 128, kNumAlignBits = 4, kMatchMinLen = 2;

unsigned tree(RangeDecoder& rc, uint16_t* probs, unsigned bits) {
  unsigned m = 1;
  for (unsigned i = 0; i < bits; i++) m = m << 1 | rc.bit(probs[m]);
  return m - (1u << bits);
}

unsigned reverse_tree(RangeDecoder& rc, uint16_t* probs, unsigned bits) {
  unsigned m = 1, symbol = 0;
  for (unsigned i = 0; i < bits; i++) {
    const unsigned b = rc.bit(probs[m]);
    m = m << 1 | b;
    symbol |= b << i;
  }
  return symbol;
}

struct LenDecoder {
  uint16_t choice, choice2, low[1 << kPosBitsMax][1 << 3], mid[1 << kPosBitsMax][1 << 3], high[1 << 8];
  void reset() {
    choice = choice2 = 1024;
    std::fill(&low[0][0], &low[0][0] + sizeof(low) / 2, uint16_t(1024));
    std::fill(&mid[0][0], &mid[0][0] + sizeof(mid) / 2, uint16_t(1024));
    std::fill(high, high + 256, uint16_t(1024));
  }
  unsigned decode(RangeDecoder& rc, unsigned pos_state) {
    if (!rc.bit(choice)) return tree(rc, low[pos_state], 3);
    if (!rc.bit(choice2)) return 8 + tree(rc, mid[pos_state], 3);
    return 16 + tree(rc, high, 8);
  }
};

class Lzma {
 public:
  enum class Stop { limit, marker };

  // lc/lp/pb as one byte: (pb * 5 + lp) * 9 + lc. False when out of range.
  bool set_props(uint8_t d) {
    if (d >= 9 * 5 * 5) return false;
    lc_ = d % 9;
    lp_ = d / 9 % 5;
    pb_ = d / 45;
    literal_.assign(size_t(0x300) << (lc_ + lp_), 1024);
    return true;
  }
  unsigned lc() const { return lc_; }
  unsigned lp() const { return lp_; }
  void set_dictionary(uint32_t size) { dict_ = size; }

  void reset_state() {
    std::fill(literal_.begin(), literal_.end(), uint16_t(1024));
    for (uint16_t* t : {is_match_, is_rep0_long_}) std::fill(t, t + (kNumStates << kPosBitsMax), uint16_t(1024));
    for (uint16_t* t : {is_rep_, is_rep_g0_, is_rep_g1_, is_rep_g2_}) std::fill(t, t + kNumStates, uint16_t(1024));
    std::fill(&pos_slot_[0][0], &pos_slot_[0][0] + sizeof(pos_slot_) / 2, uint16_t(1024));
    std::fill(pos_special_, pos_special_ + sizeof(pos_special_) / 2, uint16_t(1024));
    std::fill(align_, align_ + sizeof(align_) / 2, uint16_t(1024));
    len_.reset();
    rep_len_.reset();
    state_ = 0;
    rep0_ = rep1_ = rep2_ = rep3_ = 0;
    pending_ = 0;
  }

  // Decodes into out[pos, limit), out being at least `limit` long; the
  // dictionary starts at `dict_start`, and the stream (or the LZMA2 chunk)
  // ends at `end` >= limit: no match may run past it. Stops at `limit`, in
  // the middle of a match if need be (the rest is pending), or after the
  // end marker.
  Stop decode(RangeDecoder& rc, uint8_t* out, uint64_t& pos, uint64_t dict_start, uint64_t limit, uint64_t end) {
    for (; pending_ && pos < limit; pending_--, pos++) out[pos] = out[pos - rep0_ - 1];
    while (pos < limit) {
      const uint64_t done = pos - dict_start;
      const unsigned pos_state = unsigned(done) & ((1u << pb_) - 1);
      if (!rc.bit(is_match_[(state_ << kPosBitsMax) + pos_state])) {
        const unsigned prev = done ? out[pos - 1] : 0;
        uint16_t* probs = &literal_[0x300 * ((((unsigned(done) & ((1u << lp_) - 1)) << lc_) + (prev >> (8 - lc_))))];
        unsigned symbol = 1;
        if (state_ >= 7) {
          unsigned match_byte = out[pos - rep0_ - 1];
          do {
            const unsigned match_bit = (match_byte >> 7) & 1;
            match_byte <<= 1;
            const unsigned b = rc.bit(probs[((1 + match_bit) << 8) + symbol]);
            symbol = symbol << 1 | b;
            if (match_bit != b) break;
          } while (symbol < 0x100);
        }
        while (symbol < 0x100) symbol = symbol << 1 | rc.bit(probs[symbol]);
        out[pos++] = uint8_t(symbol);
        state_ = state_ < 4 ? 0 : state_ < 10 ? state_ - 3 : state_ - 6;
        continue;
      }
      unsigned len;
      if (rc.bit(is_rep_[state_])) {
        if (!done) throw Damaged("a repeated match before the first byte");
        if (!rc.bit(is_rep_g0_[state_])) {
          if (!rc.bit(is_rep0_long_[(state_ << kPosBitsMax) + pos_state])) {
            state_ = state_ < 7 ? 9 : 11;
            check_distance(done);
            out[pos] = out[pos - rep0_ - 1];
            pos++;
            continue;
          }
        } else {
          uint32_t dist;
          if (!rc.bit(is_rep_g1_[state_])) {
            dist = rep1_;
          } else {
            if (!rc.bit(is_rep_g2_[state_])) {
              dist = rep2_;
            } else {
              dist = rep3_;
              rep3_ = rep2_;
            }
            rep2_ = rep1_;
          }
          rep1_ = rep0_;
          rep0_ = dist;
        }
        len = rep_len_.decode(rc, pos_state);
        state_ = state_ < 7 ? 8 : 11;
      } else {
        rep3_ = rep2_;
        rep2_ = rep1_;
        rep1_ = rep0_;
        len = len_.decode(rc, pos_state);
        state_ = state_ < 7 ? 7 : 10;
        rep0_ = distance(rc, len);
        if (rep0_ == 0xFFFFFFFF) return Stop::marker;
      }
      check_distance(done);
      len += kMatchMinLen;
      if (len > end - pos) throw Damaged("a match runs past the end of the data");
      const uint64_t n = std::min<uint64_t>(len, limit - pos);
      for (uint64_t i = 0; i < n; i++, pos++) out[pos] = out[pos - rep0_ - 1];
      pending_ = uint32_t(len - n);
    }
    return Stop::limit;
  }

  // At the end of the data: whether LZMA's end marker comes next (a match
  // of distance 0xFFFFFFFF). Anything else there is an error.
  void end_marker(RangeDecoder& rc, uint64_t pos, uint64_t dict_start) {
    const unsigned pos_state = unsigned(pos - dict_start) & ((1u << pb_) - 1);
    if (!rc.bit(is_match_[(state_ << kPosBitsMax) + pos_state]) || rc.bit(is_rep_[state_]))
      throw Damaged("more data than the recorded size");
    const unsigned len = len_.decode(rc, pos_state);
    if (distance(rc, len) != 0xFFFFFFFF) throw Damaged("more data than the recorded size");
  }

  uint32_t pending() const { return pending_; }

 private:
  void check_distance(uint64_t done) const {
    if (rep0_ >= done) throw Damaged("a match reaches before the first byte");
    if (rep0_ >= dict_) throw Damaged("a match reaches past the dictionary");
  }

  uint32_t distance(RangeDecoder& rc, unsigned len) {
    const unsigned slot = tree(rc, pos_slot_[std::min(len, kNumLenToPosStates - 1)], 6);
    if (slot < 4) return slot;
    const unsigned direct_bits = (slot >> 1) - 1;
    uint32_t dist = (2 | (slot & 1)) << direct_bits;
    if (slot < kEndPosModelIndex) return dist + reverse_tree(rc, pos_special_ + dist - slot, direct_bits);
    dist += rc.direct(direct_bits - kNumAlignBits) << kNumAlignBits;
    return dist + reverse_tree(rc, align_, kNumAlignBits);
  }

  unsigned lc_ = 0, lp_ = 0, pb_ = 0;
  uint32_t dict_ = 0;
  std::vector<uint16_t> literal_;
  uint16_t is_match_[kNumStates << kPosBitsMax], is_rep_[kNumStates], is_rep_g0_[kNumStates], is_rep_g1_[kNumStates],
      is_rep_g2_[kNumStates], is_rep0_long_[kNumStates << kPosBitsMax];
  uint16_t pos_slot_[kNumLenToPosStates][1 << 6], pos_special_[1 + kNumFullDistances - kEndPosModelIndex],
      align_[1 << kNumAlignBits];
  LenDecoder len_, rep_len_;
  unsigned state_ = 0;
  uint32_t rep0_ = 0, rep1_ = 0, rep2_ = 0, rep3_ = 0;
  uint32_t pending_ = 0;  // what is left of a match `limit` cut short
};

// LZMA2's dictionary size from its one property byte (0..40).
uint32_t lzma2_dictionary(uint8_t p) { return p == 40 ? 0xFFFFFFFF : uint32_t(2 | (p & 1)) << (p / 2 + 11); }

}  // namespace

// ---- blocks ----------------------------------------------------------------------------

struct SevenZipArchive::Block {
  Method method = Method::copy;
  std::vector<uint8_t> props;
  uint64_t pack_offset = 0, pack_size = 0;  // in the archive
  uint64_t unpack_size = 0;
  std::optional<uint32_t> crc;
  size_t files = 0;  // the files whose data it holds
};

// One block decoded into memory as far as the reads went.
struct SevenZipArchive::Decoder {
  Decoder(const uint8_t* archive, const Block& b, size_t index) : block(b), index(index) {
    in = archive + b.pack_offset;
    in_size = size_t(b.pack_size);
    if (b.method == Method::lzma) {
      lzma.set_props(b.props[0]);
      lzma.set_dictionary(std::max<uint32_t>(le32(&b.props[1]), 4096));  // the specification's minimum
      lzma.reset_state();
    } else if (b.method == Method::lzma2) {
      lzma.set_dictionary(lzma2_dictionary(b.props[0]));
    }
  }

  const Block& block;
  const size_t index;
  const uint8_t* in = nullptr;  // the packed stream
  size_t in_size = 0, in_pos = 0;
  std::vector<uint8_t> out;     // out[0, pos) decoded
  uint64_t pos = 0;
  bool ended = false;           // decoded whole, its end and CRC-32 checked
  std::string error;            // why it cannot go on, once it failed

  Lzma lzma;
  RangeDecoder rc;
  bool started = false;
  uint64_t dict_start = 0;
  // LZMA2: what comes next, the end of the chunk being decoded (output, and
  // input for an LZMA chunk), and the lowest control byte an LZMA chunk may
  // have next (0xE0 at the start: a dictionary reset with properties).
  enum class Chunk { control, copy, lzma } chunk = Chunk::control;
  uint64_t chunk_end = 0;
  size_t chunk_in_end = 0;
  unsigned need = 0xE0;

  // Decodes until `target` (<= the block's size) bytes are there; then,
  // with the whole block decoded, checks its end and CRC-32. Throws Damaged.
  void decode_to(uint64_t target) {
    if (!error.empty()) throw Damaged(error);
    try {
      if (target > pos) {
        reserve(target);
        switch (block.method) {
          case Method::copy:
            memcpy(out.data() + pos, in + pos, size_t(target - pos));
            pos = target;
            break;
          case Method::lzma:
            if (!started) {
              rc.init(in, in_size);
              started = true;
            }
            if (lzma.decode(rc, out.data(), pos, 0, target, block.unpack_size) == Lzma::Stop::marker)
              throw Damaged("the end marker comes before the recorded size");
            break;
          case Method::lzma2:
            lzma2_to(target);
            break;
          case Method::other:
            throw Damaged("an unsupported method");
        }
      }
      if (pos == block.unpack_size && !ended) finish();
    } catch (const Damaged& e) {
      error = e.why;
      throw;
    }
  }

 private:
  // Room for `target` bytes, grown as the data really decodes (never from
  // the recorded size alone).
  void reserve(uint64_t target) {
    if (out.size() >= target) return;
    const uint64_t grown = std::max<uint64_t>(uint64_t(out.size()) * 2, 1 << 20);
    out.resize(size_t(std::max(target, std::min(grown, block.unpack_size))));
  }

  uint8_t in_byte() {
    if (in_pos == in_size) throw Damaged("the packed data ends early");
    return in[in_pos++];
  }

  void lzma2_to(uint64_t target) {
    for (;;) {
      if (chunk == Chunk::lzma && pos == chunk_end) {
        if (lzma.pending() || !rc.flushed() || rc.consumed() != chunk_in_end - in_pos)
          throw Damaged("an LZMA2 chunk does not end where its sizes say");
        in_pos = chunk_in_end;
        chunk = Chunk::control;
      }
      if (chunk == Chunk::copy && pos == chunk_end) chunk = Chunk::control;
      if (pos >= target) return;
      if (chunk == Chunk::copy) {
        const size_t n = size_t(std::min(target, chunk_end) - pos);
        memcpy(out.data() + pos, in + in_pos, n);
        in_pos += n;
        pos += n;
        continue;
      }
      if (chunk == Chunk::lzma) {
        if (lzma.decode(rc, out.data(), pos, dict_start, std::min(target, chunk_end), chunk_end) == Lzma::Stop::marker)
          throw Damaged("an end marker inside an LZMA2 chunk");
        continue;
      }
      const unsigned c = in_byte();
      if (c == 0) throw Damaged("the data ends at byte " + std::to_string(pos) + " of " + std::to_string(block.unpack_size));
      if (c < 0x80) {
        // An uncompressed chunk: 1 resets the dictionary, 2 does not.
        if (c > 2) throw Damaged("an unknown LZMA2 chunk type");
        if (c == 2 && need == 0xE0) throw Damaged("an LZMA2 chunk that needs the dictionary reset first");
        if (c == 1) {
          dict_start = pos;
          need = 0xC0;
        }
        const unsigned hi = in_byte();
        const uint64_t size = (hi << 8 | in_byte()) + 1;
        if (size > block.unpack_size - pos) throw Damaged("more data than the recorded size");
        if (size > in_size - in_pos) throw Damaged("the packed data ends early");
        chunk = Chunk::copy;
        chunk_end = pos + size;
        continue;
      }
      // An LZMA chunk: bits 5-6 say what it resets (0 nothing, 1 the state,
      // 2 the state with new properties, 3 the dictionary too).
      if (c < need) throw Damaged("an LZMA2 chunk without the reset it needs");
      need = 0;
      const unsigned b1 = in_byte(), b2 = in_byte();
      const uint64_t unpacked = (uint64_t(c & 0x1F) << 16 | b1 << 8 | b2) + 1;
      const unsigned p1 = in_byte(), p2 = in_byte();
      const size_t packed = (p1 << 8 | p2) + 1;
      const unsigned mode = (c >> 5) & 3;
      if (mode >= 2) {
        const uint8_t props = in_byte();
        if (!lzma.set_props(props) || lzma.lc() + lzma.lp() > 4) throw Damaged("bad LZMA2 chunk properties");
      }
      if (mode == 3) dict_start = pos;
      if (mode >= 1) lzma.reset_state();
      if (unpacked > block.unpack_size - pos) throw Damaged("more data than the recorded size");
      if (packed > in_size - in_pos) throw Damaged("the packed data ends early");
      rc.init(in + in_pos, packed);
      chunk = Chunk::lzma;
      chunk_end = pos + unpacked;
      chunk_in_end = in_pos + packed;
    }
  }

  // The whole block is there: the stream must end here, exactly.
  void finish() {
    switch (block.method) {
      case Method::lzma:
        if (!started) {
          rc.init(in, in_size);
          started = true;
        }
        if (lzma.pending()) throw Damaged("a match runs past the end of the data");
        // Flushed with every byte consumed, or LZMA's end marker next.
        if (!rc.flushed() || rc.consumed() != in_size) {
          lzma.end_marker(rc, pos, 0);
          if (!rc.flushed()) throw Damaged("the range coder is not flushed at the end marker");
        }
        if (rc.consumed() != in_size) throw Damaged("packed data after the end of the stream");
        break;
      case Method::lzma2:
        if (in_byte() != 0) throw Damaged("more data than the recorded size");
        if (in_pos != in_size) throw Damaged("packed data after the end of the stream");
        break;
      default:
        break;
    }
    if (block.crc && crc32_of(out.data(), size_t(block.unpack_size)) != *block.crc)
      throw Damaged("the block's CRC-32 does not match");
    ended = true;
  }
};

namespace {

// The packed streams, blocks and substreams of the header (or of the packed
// header itself).
struct Streams {
  std::vector<SevenZipArchive::Block> blocks;
  std::vector<uint64_t> sizes;                  // each file's (unpack stream), blocks in order
  std::vector<std::optional<uint32_t>> crcs;
};

// One folder: one coder, LZMA, LZMA2 or Copy. Everything else is refused
// here, by name.
SevenZipArchive::Block read_folder(Reader& r) {
  struct Coder {
    std::vector<uint8_t> id, props;
    uint64_t in = 1, out = 1;
  };
  const size_t n = r.count(32, "coders in a block");
  if (!n) r.fail("a block without coders");
  std::vector<Coder> coders(n);
  uint64_t in_total = 0, out_total = 0;
  for (Coder& c : coders) {
    const uint8_t flags = r.byte();
    if (flags & 0xC0) r.fail("reserved coder flags");
    const uint8_t* id = r.bytes(flags & 0x0F);
    c.id.assign(id, id + (flags & 0x0F));
    if (flags & 0x10) {
      c.in = r.number();
      c.out = r.number();
      if (c.in > 32 || c.out > 32) r.fail("a coder of too many streams");
    }
    if (flags & 0x20) {
      const uint64_t size = r.number();
      if (size > 256) r.fail("coder properties of " + std::to_string(size) + " bytes");
      const uint8_t* p = r.bytes(size);
      c.props.assign(p, p + size);
    }
    in_total += c.in;
    out_total += c.out;
  }
  if (!out_total || in_total + 1 < out_total) r.fail("a block whose coders do not connect");
  for (uint64_t i = 0; i + 1 < out_total; i++) {
    if (r.number() >= in_total || r.number() >= out_total) r.fail("a bind pair out of range");
  }
  const uint64_t packed = in_total - (out_total - 1);
  if (packed > 1)
    for (uint64_t i = 0; i < packed; i++)
      if (r.number() >= in_total) r.fail("a packed stream index out of range");
  for (const Coder& c : coders)
    if (!c.id.empty() && c.id[0] == 0x06)
      r.refuse("it is encrypted (" + method_name(c.id) + "); password-protected archives are not supported");
  if (n > 1) {
    // In the order the data went through them, as 7-Zip lists a method
    // ("BCJ LZMA2:24"): the folder names the last coder first.
    std::string chain;
    for (auto c = coders.rbegin(); c != coders.rend(); ++c) chain += (chain.empty() ? "" : " + ") + method_name(c->id);
    r.refuse("a block packed with " + chain +
             " is not supported: only LZMA, LZMA2 or Copy, alone (7-Zip puts Windows programs through the BCJ "
             "filter unless it is given -mf=off)");
  }
  const Coder& c = coders.front();
  const KnownMethod* k = known_method(c.id);
  if (!k || k->method == Method::other || c.in != 1 || c.out != 1)
    r.refuse("the method " + method_name(c.id) + " is not supported: only LZMA, LZMA2 or Copy");
  SevenZipArchive::Block b;
  b.method = k->method;
  b.props = c.props;
  const bool ok = b.method == Method::copy    ? b.props.empty()
                  : b.method == Method::lzma  ? b.props.size() == 5 && b.props[0] < 9 * 5 * 5
                                              : b.props.size() == 1 && b.props[0] <= 40;
  if (!ok) r.fail(std::string(k->name) + " with bad properties");
  return b;
}

// A StreamsInfo record, after its id. `pack_area` is where the packed
// streams may lie: from the end of the signature header to the header.
Streams read_streams(Reader& r, uint64_t pack_area_end) {
  Streams s;
  uint64_t pack_pos = 0;
  std::vector<uint64_t> pack_sizes;
  uint8_t id = r.byte();
  if (id == kPackInfo) {
    pack_pos = r.number();
    const size_t n = r.count(SevenZipArchive::kMaxEntries, "packed streams");
    r.expect(kSize, "packed sizes");
    for (size_t i = 0; i < n; i++) pack_sizes.push_back(r.number());
    id = r.byte();
    if (id == kCrc) {
      r.digests(n);  // the packed streams' own CRCs (7-Zip writes none): the files' are checked
      id = r.byte();
    }
    if (id != kEnd) r.fail("pack info");
    id = r.byte();
  }
  if (id == kUnpackInfo) {
    r.expect(kFolder, "block list");
    const size_t n = r.count(SevenZipArchive::kMaxEntries, "blocks");
    if (r.byte() != 0) r.refuse("external block descriptions are not supported");
    for (size_t i = 0; i < n; i++) s.blocks.push_back(read_folder(r));
    r.expect(kCodersUnpackSize, "unpacked sizes");
    for (SevenZipArchive::Block& b : s.blocks) {
      b.unpack_size = r.number();
      if (b.unpack_size > SevenZipArchive::kMaxBlockBytes)
        r.refuse("a block of " + mb(b.unpack_size) + " is more than this reader holds (" +
                 mb(SevenZipArchive::kMaxBlockBytes) + ")");
    }
    id = r.byte();
    if (id == kCrc) {
      const auto crcs = r.digests(n);
      for (size_t i = 0; i < n; i++) s.blocks[i].crc = crcs[i];
      id = r.byte();
    }
    if (id != kEnd) r.fail("unpack info");
    id = r.byte();
  }
  for (SevenZipArchive::Block& b : s.blocks) b.files = 1;
  if (id == kSubStreamsInfo) {
    id = r.byte();
    if (id == kNumUnpackStream) {
      uint64_t total = 0;
      for (SevenZipArchive::Block& b : s.blocks) {
        b.files = r.count(SevenZipArchive::kMaxEntries, "files in a block");
        total += b.files;
        if (total > SevenZipArchive::kMaxEntries)
          r.refuse("more than " + std::to_string(SevenZipArchive::kMaxEntries) + " files");
      }
      id = r.byte();
    }
    for (const SevenZipArchive::Block& b : s.blocks) {
      if (!b.files) r.fail("a block that holds no file");
      if (b.files > 1 && id != kSize) r.fail("a solid block without its files' sizes");
      uint64_t sum = 0;
      for (size_t i = 0; i + 1 < b.files; i++) {
        const uint64_t size = r.number();
        if (size > b.unpack_size - sum) r.fail("files larger than their block");
        sum += size;
        s.sizes.push_back(size);
      }
      s.sizes.push_back(b.unpack_size - sum);
    }
    if (id == kSize) id = r.byte();
    // A CRC for every file, but the lone file of a block that has its own.
    size_t unknown = 0;
    for (const SevenZipArchive::Block& b : s.blocks)
      if (!(b.files == 1 && b.crc)) unknown += b.files;
    std::vector<std::optional<uint32_t>> digests(unknown);
    if (id == kCrc) {
      digests = r.digests(unknown);
      id = r.byte();
    }
    size_t k = 0;
    for (const SevenZipArchive::Block& b : s.blocks)
      for (size_t i = 0; i < b.files; i++) s.crcs.push_back(b.files == 1 && b.crc ? b.crc : digests[k++]);
    if (id != kEnd) r.fail("substreams info");
    id = r.byte();
  } else {
    for (const SevenZipArchive::Block& b : s.blocks) {
      s.sizes.push_back(b.unpack_size);
      s.crcs.push_back(b.crc);
    }
  }
  if (id != kEnd) r.fail("streams info");
  // One packed stream per block, in the pack area, in order.
  if (pack_sizes.size() != s.blocks.size())
    r.fail(std::to_string(pack_sizes.size()) + " packed streams for " + std::to_string(s.blocks.size()) + " blocks");
  uint64_t at = kSignatureHeaderSize;
  if (pack_pos > pack_area_end - at) r.fail("packed data outside the archive");
  at += pack_pos;
  for (size_t i = 0; i < s.blocks.size(); i++) {
    SevenZipArchive::Block& b = s.blocks[i];
    if (pack_sizes[i] > pack_area_end - at) r.fail("packed data outside the archive");
    b.pack_offset = at;
    b.pack_size = pack_sizes[i];
    at += pack_sizes[i];
    if (b.method == Method::copy && b.pack_size != b.unpack_size) r.fail("a stored block whose sizes differ");
  }
  return s;
}

// UTF-16LE names, each NUL-terminated, to UTF-8; an unpaired surrogate is damage.
std::vector<std::string> read_names(Reader& r, size_t n) {
  std::vector<std::string> names;
  for (size_t i = 0; i < n; i++) {
    std::string s;
    for (;;) {
      uint32_t c = r.byte();
      c |= uint32_t(r.byte()) << 8;
      if (!c) break;
      if (c >= 0xDC00 && c < 0xE000) r.fail("a name that is not UTF-16");
      if (c >= 0xD800 && c < 0xDC00) {
        uint32_t lo = r.byte();
        lo |= uint32_t(r.byte()) << 8;
        if (lo < 0xDC00 || lo >= 0xE000) r.fail("a name that is not UTF-16");
        c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
      }
      if (c < 0x80) {
        s += char(c);
      } else if (c < 0x800) {
        s += char(0xC0 | c >> 6);
        s += char(0x80 | (c & 0x3F));
      } else if (c < 0x10000) {
        s += char(0xE0 | c >> 12);
        s += char(0x80 | ((c >> 6) & 0x3F));
        s += char(0x80 | (c & 0x3F));
      } else {
        s += char(0xF0 | c >> 18);
        s += char(0x80 | ((c >> 12) & 0x3F));
        s += char(0x80 | ((c >> 6) & 0x3F));
        s += char(0x80 | (c & 0x3F));
      }
    }
    names.push_back(std::move(s));
  }
  return names;
}

}  // namespace

bool is_7z_signature(std::span<const uint8_t> head) {
  return head.size() >= sizeof kSignature && std::equal(kSignature, kSignature + sizeof kSignature, head.begin());
}

SevenZipArchive::~SevenZipArchive() = default;

SevenZipArchive::SevenZipArchive(std::shared_ptr<const std::vector<uint8_t>> data, std::string name)
    : data_(std::move(data)), name_(std::move(name)) {
  const std::vector<uint8_t>& d = *data_;
  if (!is_7z_signature(d)) throw SevenZipError(name_ + ": not a 7z archive");
  if (d.size() < kSignatureHeaderSize) throw SevenZipError(name_ + ": the archive is truncated");
  if (d[6] != 0)
    throw SevenZipError(name_ + ": 7z format version " + std::to_string(d[6]) + "." + std::to_string(d[7]) +
                        " is not supported");
  if (crc32_of(&d[12], 20) != le32(&d[8])) throw SevenZipError(name_ + ": damaged signature header (CRC-32 mismatch)");
  const uint64_t next_offset = le64(&d[12]), next_size = le64(&d[20]);
  const uint32_t next_crc = le32(&d[28]);
  const uint64_t after = d.size() - kSignatureHeaderSize;
  if (next_size == 0 && next_offset == 0 && next_crc == 0 && after == 0) return;  // an empty archive
  if (next_offset > after || next_size > after - next_offset)
    throw SevenZipError(name_ + ": the archive is truncated, or is the first volume of a split archive (.7z.001; "
                                "join the volumes into one file first)");
  if (next_offset + next_size != after) throw SevenZipError(name_ + ": data after the end of the archive");
  if (next_size == 0) throw SevenZipError(name_ + ": damaged signature header (no header)");
  if (next_size > kMaxHeaderBytes) throw SevenZipError(name_ + ": the header is implausibly large");
  const uint8_t* header = &d[kSignatureHeaderSize + next_offset];
  if (crc32_of(header, size_t(next_size)) != next_crc) throw SevenZipError(name_ + ": damaged header (CRC-32 mismatch)");
  const uint64_t pack_area_end = kSignatureHeaderSize + next_offset;

  Reader r(header, size_t(next_size), name_);
  std::vector<uint8_t> unpacked;  // the header, when it is packed
  uint8_t id = r.byte();
  if (id == kEncodedHeader) {
    Streams s = read_streams(r, pack_area_end);
    r.done("the packed header's record");
    if (s.blocks.size() != 1 || s.blocks[0].files != 1) r.fail("a packed header in several parts");
    Block& b = s.blocks[0];
    if (b.unpack_size > kMaxHeaderBytes) throw SevenZipError(name_ + ": the header is implausibly large");
    if (!b.crc) b.crc = s.crcs[0];
    Decoder dec(d.data(), b, 0);
    try {
      dec.decode_to(b.unpack_size);
    } catch (const Damaged& e) {
      throw SevenZipError(name_ + ": damaged header (the packed header: " + e.why + ")");
    }
    unpacked = std::move(dec.out);
    unpacked.resize(size_t(b.unpack_size));
    r = Reader(unpacked.data(), unpacked.size(), name_);
    id = r.byte();
    if (id == kEncodedHeader) r.refuse("a header packed twice is not supported");
  }
  if (id != kHeader) r.fail("no header record");

  id = r.byte();
  if (id == kArchiveProperties) {
    for (uint8_t t = r.byte(); t != kEnd; t = r.byte()) r.bytes(r.number());
    id = r.byte();
  }
  if (id == kAdditionalStreamsInfo) r.refuse("additional header streams are not supported");
  Streams s;
  if (id == kMainStreamsInfo) {
    s = read_streams(r, pack_area_end);
    id = r.byte();
  }
  size_t files = 0;
  std::vector<bool> empty_stream, empty_file;
  std::vector<std::string> names;
  bool have_names = false;
  std::vector<std::optional<uint64_t>> mtimes;
  std::vector<std::optional<uint32_t>> attributes;
  if (id == kFilesInfo) {
    files = r.count(kMaxEntries, "entries");
    empty_stream.assign(files, false);
    mtimes.resize(files);
    attributes.resize(files);
    size_t empties = 0;
    for (uint8_t t = r.byte(); t != kEnd; t = r.byte()) {
      Reader p = r.sub(r.number());
      switch (t) {
        case kEmptyStream:
          empty_stream = p.bits(files);
          empties = size_t(std::count(empty_stream.begin(), empty_stream.end(), true));
          empty_file.assign(empties, false);
          p.done("the empty-stream list");
          break;
        case kEmptyFile:
          empty_file = p.bits(empties);
          p.done("the empty-file list");
          break;
        case kAnti: {
          const std::vector<bool> anti = p.bits(empties);
          if (std::count(anti.begin(), anti.end(), true)) r.refuse("anti items (an update's deletions) are not supported");
          p.done("the anti-item list");
          break;
        }
        case kName:
          if (p.byte() != 0) r.refuse("external names are not supported");
          names = read_names(p, files);
          have_names = true;
          p.done("the name list");
          break;
        case kMTime: {
          const std::vector<bool> def = p.defined(files);
          if (p.byte() != 0) r.refuse("external times are not supported");
          for (size_t i = 0; i < files; i++)
            if (def[i]) mtimes[i] = p.u64();
          p.done("the time list");
          break;
        }
        case kWinAttributes: {
          const std::vector<bool> def = p.defined(files);
          if (p.byte() != 0) r.refuse("external attributes are not supported");
          for (size_t i = 0; i < files; i++)
            if (def[i]) attributes[i] = p.u32();
          p.done("the attribute list");
          break;
        }
        default:
          break;  // creation and access times, padding (kDummy), anything newer: nothing this reader uses
      }
    }
    id = r.byte();
  }
  if (id != kEnd) r.fail("header");
  r.done("the header");
  if (files && !have_names) r.fail("entries without names");

  // Every file with data takes the next stream, blocks in order.
  std::vector<size_t> stream_block;
  std::vector<uint64_t> stream_offset;
  for (size_t b = 0; b < s.blocks.size(); b++) {
    uint64_t offset = 0;
    for (size_t i = 0; i < s.blocks[b].files; i++) {
      stream_block.push_back(b);
      stream_offset.push_back(offset);
      offset += s.sizes[stream_block.size() - 1];
    }
  }
  size_t stream = 0, empty = 0;
  std::set<std::string> seen;
  for (size_t i = 0; i < files; i++) {
    SevenZipMember m;
    m.name = names[i];
    std::replace(m.name.begin(), m.name.end(), '\\', '/');
    m.mtime = mtimes[i];
    m.attributes = attributes[i].value_or(0);
    if (!empty_stream[i]) {
      if (stream == stream_block.size()) r.fail("more files than streams");
      if (m.attributes & kAttributeDirectory) r.fail("a folder that holds data");
      m.block = stream_block[stream];
      m.offset = stream_offset[stream];
      m.size = s.sizes[stream];
      m.crc = s.crcs[stream];
      stream++;
    } else {
      m.directory = !empty_file[empty++];
      if (!m.directory && (m.attributes & kAttributeDirectory)) r.fail("an empty file marked as a folder");
      if (m.directory) m.crc.reset();
      else m.crc = 0;  // the CRC-32 of nothing
    }
    // Each component a name the importer may write (names.h), the whole
    // relative: no drive, no root, no "..".
    size_t at = 0;
    for (;;) {
      const size_t slash = m.name.find('/', at);
      try {
        check_component(m.name.substr(at, slash == std::string::npos ? std::string::npos : slash - at), name_);
      } catch (const ImportError& ex) {
        throw SevenZipError(std::string(ex.what()) + " (entry \"" + m.name + "\")");
      }
      if (slash == std::string::npos) break;
      at = slash + 1;
    }
    if (!seen.insert(name_key(m.name)).second) throw SevenZipError(name_ + ": two entries are named " + m.name);
    members_.push_back(std::move(m));
  }
  if (stream != stream_block.size()) r.fail(std::to_string(stream_block.size() - stream) + " streams without a file");
  blocks_ = std::move(s.blocks);
}

const SevenZipMember* SevenZipArchive::find(std::string_view member) const {
  std::string want(member);
  std::replace(want.begin(), want.end(), '\\', '/');
  for (const SevenZipMember& m : members_)
    if (iequals(m.name, want)) return &m;
  return nullptr;
}

std::string SevenZipArchive::method(const SevenZipMember& m) const {
  if (m.block >= blocks_.size()) return "";
  switch (blocks_[m.block].method) {
    case Method::copy:
      return "Copy";
    case Method::lzma:
      return "LZMA";
    case Method::lzma2:
      return "LZMA2";
    default:
      return "";
  }
}

bool SevenZipArchive::solid(const SevenZipMember& m) const {
  return m.block < blocks_.size() && blocks_[m.block].files > 1;
}

void SevenZipArchive::extract(const SevenZipMember& m, const std::function<void(const uint8_t*, size_t)>& sink) const {
  const std::string what = name_ + "!" + m.name;
  if (m.directory) throw SevenZipError(what + " is a folder");
  uLong crc = crc32(0, nullptr, 0);
  if (m.size) {
    if (m.block >= blocks_.size()) throw SevenZipError(what + ": no such entry in this archive");
    std::shared_ptr<Decoder> dec;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (!cache_ || cache_->index != m.block) cache_ = std::make_shared<Decoder>(data_->data(), blocks_[m.block], m.block);
      dec = cache_;
    }
    std::vector<uint8_t> chunk(size_t(std::min<uint64_t>(m.size, kChunk)));
    for (uint64_t done = 0; done < m.size;) {
      const size_t n = size_t(std::min<uint64_t>(kChunk, m.size - done));
      {
        // The decoder is shared with any read of the same block, the sink's
        // included: the sink gets a copy, outside the lock.
        std::lock_guard<std::mutex> lock(mu_);
        try {
          dec->decode_to(m.offset + done + n);
        } catch (const Damaged& e) {
          throw SevenZipError(what + ": damaged " + method(m) + " data (" + e.why + ")");
        }
        memcpy(chunk.data(), dec->out.data() + m.offset + done, n);
      }
      crc = crc32(crc, chunk.data(), uInt(n));
      sink(chunk.data(), n);
      done += n;
    }
  }
  if (m.crc && uint32_t(crc) != *m.crc) throw SevenZipError(what + ": CRC-32 mismatch");
}

}  // namespace adw::import
