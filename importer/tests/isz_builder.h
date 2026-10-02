// Synthetic InstallShield 2 compressed libraries and PKWARE DCL implode
// streams for the importer tests (isz.h; research/win/pkg/installshield/
// SURVEY_REPORT.md, "Test strategy"):
//   * a port of the survey's tools/dclwrite.py: it writes the tokens it is
//     given one by one (literals, copies, the end code, raw bits to craft
//     damage) with the format's fixed codes, computed here from their code
//     lengths independently of importer/isz.cc — there is no match search,
//     nothing here compresses anything;
//   * a port of tools/zwrite.py: it lays out given member streams as a
//     library, or as a split set cut after given numbers of data bytes (a
//     member cut by a boundary continues in the next volume), exactly as the
//     releases' libraries are laid out, with the offsets a test needs to
//     break any field afterwards.
// No bytes of any release: every stream here is written from made-up data.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace test {

// ---- DCL implode tokens (tools/dclwrite.py) ------------------------------------------------

struct DclToken {
  enum class Kind { literal, copy, end, bits } kind = Kind::literal;
  uint32_t a = 0, b = 0;  // literal: the byte; copy: length (2..518), distance; bits: value, count

  static DclToken lit(uint8_t byte) { return {Kind::literal, byte, 0}; }
  static DclToken copy(uint32_t length, uint32_t distance) { return {Kind::copy, length, distance}; }
  static DclToken end() { return {Kind::end, 0, 0}; }
  static DclToken raw(uint32_t value, uint32_t count) { return {Kind::bits, value, count}; }
};

inline std::vector<DclToken> dcl_literals(const std::vector<uint8_t>& data) {
  std::vector<DclToken> t;
  for (uint8_t b : data) t.push_back(DclToken::lit(b));
  return t;
}

inline std::vector<DclToken> dcl_literals(const std::string& text) {
  return dcl_literals(std::vector<uint8_t>(text.begin(), text.end()));
}

namespace dcldetail {

// The code lengths, symbol by symbol (the format's constants).
inline constexpr std::array<uint8_t, 16> kLengthBits = {2, 3, 3, 3, 4, 4, 4, 5, 5, 5, 5, 6, 6, 6, 7, 7};
inline constexpr std::array<uint16_t, 16> kLengthBase = {3, 2, 4, 5, 6, 7, 8, 9, 10, 12, 16, 24, 40, 72, 136, 264};
inline constexpr std::array<uint8_t, 16> kLengthExtra = {0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8};
inline std::vector<uint8_t> distance_bits() {
  std::vector<uint8_t> v = {2, 4, 4};
  v.insert(v.end(), 4, 5);
  v.insert(v.end(), 15, 6);
  v.insert(v.end(), 26, 7);
  v.insert(v.end(), 16, 8);
  return v;
}
// The literal code of the coded-literal ("ASCII") mode: one hex digit per
// symbol, 0x00 first (only to write the survey's two mode-1 vectors, which
// the reader refuses).
inline std::vector<uint8_t> literal_bits() {
  static const char* const kHex[8] = {
      "bcccccccc87cc7ccccccccccccdccccc",  // 0x00-0x1f
      "4a8caca87789767876777787788cb79b",  // 0x20-0x3f
      "c676657886b967667b66679899b8b9c8",  // 0x40-0x5f
      "c566656665b756556a55558788abbccc",  // 0x60-0x7f
      "dddddddddddddddddddddddddddddddd",  // 0x80-0x9f
      "ddddddddddddddddcccccccccccccccc",  // 0xa0-0xbf
      "cccccccccccccccccccccccccccccccc",  // 0xc0-0xdf
      "dcdddcdddcddddcdddcccddddddddddd",  // 0xe0-0xff
  };
  std::vector<uint8_t> v;
  for (const char* row : kHex)
    for (const char* p = row; *p; p++) v.push_back(uint8_t(*p <= '9' ? *p - '0' : *p - 'a' + 10));
  return v;
}

// Canonical code values (before the stream complements them), by symbol:
// for each length in turn, the symbols of that length in order take
// consecutive values; the first value of a length is (the last length's
// first + its count) << 1.
inline std::vector<std::pair<uint32_t, int>> canonical(const std::vector<uint8_t>& bits) {
  std::vector<std::pair<uint32_t, int>> codes(bits.size(), {0, 0});
  uint32_t first = 0;
  for (int len = 1; len <= 16; len++) {
    uint32_t next = first;
    for (size_t s = 0; s < bits.size(); s++)
      if (bits[s] == len) codes[s] = {next++, len};
    first = next << 1;
  }
  return codes;
}

inline const std::vector<std::pair<uint32_t, int>>& length_codes() {
  static const auto c = canonical(std::vector<uint8_t>(kLengthBits.begin(), kLengthBits.end()));
  return c;
}
inline const std::vector<std::pair<uint32_t, int>>& distance_codes() {
  static const auto c = canonical(distance_bits());
  return c;
}
inline const std::vector<std::pair<uint32_t, int>>& literal_codes() {
  static const auto c = canonical(literal_bits());
  return c;
}

// Least significant bit first, as the format reads.
class BitWriter {
 public:
  void put(uint32_t value, uint32_t nbits) {
    for (uint32_t i = 0; i < nbits; i++) {
      acc_ = uint8_t(acc_ | ((value >> i) & 1) << n_);
      if (++n_ == 8) {
        out_.push_back(acc_);
        acc_ = 0;
        n_ = 0;
      }
    }
  }
  // A Huffman code: its complement, most significant bit first.
  void put_code(const std::pair<uint32_t, int>& code) {
    for (int i = code.second - 1; i >= 0; i--) put(((code.first >> i) & 1) ^ 1, 1);
  }
  // Pads the last byte with zero bits, or with one bits.
  std::vector<uint8_t> finish(bool pad_ones) {
    if (n_) put(pad_ones ? 0xFF : 0, 8 - n_);
    return out_;
  }

 private:
  std::vector<uint8_t> out_;
  uint8_t acc_ = 0;
  uint32_t n_ = 0;
};

// The LENGTH symbol of a length, and its extra bits: (symbol, value, count).
inline std::array<uint32_t, 3> length_symbol(uint32_t length) {
  for (int s = 15; s >= 0; s--) {
    const uint32_t base = kLengthBase[size_t(s)], extra = kLengthExtra[size_t(s)];
    if (base <= length && length < base + (1u << extra)) return {uint32_t(s), length - base, extra};
  }
  abort();
}

}  // namespace dcldetail

// The stream the tokens make: the 2-byte header (literal mode, dictionary
// bits), then each token's bits. Lengths 2..518 (the end code is 519), a
// distance 1..(64 << dict_bits) (a length-2 copy: 1..256). Nothing is
// checked: a test writes what it needs, damage included.
inline std::vector<uint8_t> dcl_write(const std::vector<DclToken>& tokens, int mode = 0, int dict_bits = 6,
                                      bool pad_ones = false) {
  using namespace dcldetail;
  BitWriter w;
  for (const DclToken& t : tokens) {
    switch (t.kind) {
      case DclToken::Kind::literal:
        w.put(0, 1);
        if (mode == 0)
          w.put(t.a, 8);
        else
          w.put_code(literal_codes()[t.a & 0xFF]);
        break;
      case DclToken::Kind::copy:
      case DclToken::Kind::end: {
        const uint32_t length = t.kind == DclToken::Kind::end ? 519 : t.a;
        w.put(1, 1);
        const auto [s, value, count] = length_symbol(length);
        w.put_code(length_codes()[s]);
        if (count) w.put(value, count);
        if (t.kind == DclToken::Kind::end) break;
        const uint32_t dist = t.b - 1;
        const uint32_t k = length == 2 ? 2 : uint32_t(dict_bits);
        w.put_code(distance_codes()[dist >> k]);
        w.put(dist & ((1u << k) - 1), k);
        break;
      }
      case DclToken::Kind::bits: w.put(t.a, t.b); break;
    }
  }
  std::vector<uint8_t> out = {uint8_t(mode), uint8_t(dict_bits)};
  const std::vector<uint8_t> body = w.finish(pad_ones);
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

// ---- InstallShield 1's "$" files (isz.h) ------------------------------------------------------

// One "$" file: the header, `name` stored (as given: tests store bad ones
// too), and `stream` (a DCL stream from dcl_write) as its data, with the
// compressed size it has (`csize`, when set, records another). Dated
// 1992-12-09 12:34:56 unless `date`/`time` say otherwise.
inline std::vector<uint8_t> is1_file(const std::string& name, const std::vector<uint8_t>& stream,
                                     std::optional<uint32_t> csize = std::nullopt, uint16_t date = 0x1989,
                                     uint16_t time = 0x645C) {
  std::vector<uint8_t> f = {0x65, 0x5D, 0x13, 0x8C, 0x08, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x12};
  const uint32_t c = csize.value_or(uint32_t(stream.size()));
  for (int i = 0; i < 4; i++) f.push_back(uint8_t(c >> (8 * i)));
  f.insert(f.end(), {0, 0, 0, 0});
  f.insert(f.end(), {uint8_t(date), uint8_t(date >> 8), uint8_t(time), uint8_t(time >> 8), 0, 0});
  f.push_back(uint8_t(name.size()));
  f.insert(f.end(), name.begin(), name.end());
  f.push_back(0);
  f.insert(f.end(), stream.begin(), stream.end());
  return f;
}

// The same holding `data` as literals (nothing here compresses).
inline std::vector<uint8_t> is1_literals(const std::string& name, const std::vector<uint8_t>& data) {
  std::vector<DclToken> t = dcl_literals(data);
  t.push_back(DclToken::end());
  return is1_file(name, dcl_write(t));
}

// ---- libraries (tools/zwrite.py) --------------------------------------------------------------

// The library header's fields: 0x0A u16 flags, 0x0C u16 files, 0x0E u16 date,
// 0x10 u16 time, 0x12 u32 library size, 0x16 u32 total size, 0x1A u32 taken,
// 0x1E u8 volumes, 0x1F u8 volume, 0x20 u8 check, 0x21 u32 split start, 0x25
// u32 continued end, 0x29 u32 directory offset, 0x2D u32 its size, 0x31 u16
// directories, 0x33 u32 file table offset, 0x37 u32 its size, 0x3B u32 volume
// size. A file entry's: +0x00 u8 last volume, +0x01 u16 directory, +0x03 u32
// size, +0x07 u32 compressed size, +0x0B u32 offset, +0x0F u16 date, +0x11 u16
// time, +0x13 u32 attributes, +0x17 u16 entry size, +0x19 u16 flags, +0x1B u8,
// +0x1C u8 first volume, +0x1D u8 name length, +0x1E the name.
inline constexpr size_t kIszHeader = 0xFF;

struct IszSpec {
  std::string name;               // the stored bytes
  uint32_t size = 0;              // the uncompressed size recorded
  std::vector<uint8_t> stream;    // the compressed bytes, laid out as given
  std::optional<uint32_t> csize;  // recorded instead of the stream's size
  uint16_t dir = 0;               // its directory's index
  uint16_t date = 0x1B8D;         // 1993-12-13
  uint16_t time = 0;
  uint32_t attrs = 0;
};

struct IszDirSpec {
  std::string name;
  uint16_t files = 0;
};

struct IszBuilt {
  std::vector<std::vector<uint8_t>> volumes;  // volume 1 first (one for an unsplit library)
  std::vector<size_t> entries;                // each file entry's offset in the file table
};

namespace iszdetail {

inline void put16(std::vector<uint8_t>& v, uint16_t x) {
  v.push_back(uint8_t(x));
  v.push_back(uint8_t(x >> 8));
}
inline void put32(std::vector<uint8_t>& v, uint32_t x) {
  put16(v, uint16_t(x));
  put16(v, uint16_t(x >> 16));
}
inline void set16(std::vector<uint8_t>& v, size_t at, uint16_t x) {
  v[at] = uint8_t(x);
  v[at + 1] = uint8_t(x >> 8);
}
inline void set32(std::vector<uint8_t>& v, size_t at, uint32_t x) {
  set16(v, at, uint16_t(x));
  set16(v, at + 2, uint16_t(x >> 16));
}

inline std::vector<uint8_t> dir_table(const std::vector<IszDirSpec>& dirs) {
  std::vector<uint8_t> out;
  for (const IszDirSpec& d : dirs) {
    put16(out, d.files);
    put16(out, uint16_t(11 + d.name.size()));
    put16(out, uint16_t(d.name.size()));
    out.insert(out.end(), d.name.begin(), d.name.end());
    out.insert(out.end(), 5, 0);  // NUL, u32 0
  }
  return out;
}

inline std::vector<uint8_t> file_entry(const IszSpec& m, uint32_t offset, uint8_t first_vol, uint8_t last_vol,
                                       uint16_t flags, uint8_t byte1b) {
  std::vector<uint8_t> e;
  e.push_back(last_vol);
  put16(e, m.dir);
  put32(e, m.size);
  put32(e, m.csize ? *m.csize : uint32_t(m.stream.size()));
  put32(e, offset);
  put16(e, m.date);
  put16(e, m.time);
  put32(e, m.attrs);
  put16(e, uint16_t(43 + m.name.size()));
  put16(e, flags);
  e.push_back(byte1b);
  e.push_back(first_vol);
  e.push_back(uint8_t(m.name.size()));
  e.insert(e.end(), m.name.begin(), m.name.end());
  e.insert(e.end(), 13, 0);  // NUL, version MS, version LS, reserved
  return e;
}

struct HeaderFields {
  uint16_t flags = 0, files = 0, date = 0, time = 0;
  uint32_t library_size = 0, total_size = 0, taken = kIszHeader;
  uint8_t volumes = 0, volume = 0, check = 0;
  uint32_t split_start = kIszHeader, cont_end = 0, dir_off = 0, dir_size = 0;
  uint16_t dirs = 0;
  uint32_t file_off = 0, file_size = 0, volume_size = 0;
};

inline std::vector<uint8_t> header(const HeaderFields& f) {
  static const uint8_t kSig[8] = {0x13, 0x5D, 0x65, 0x8C, 0x3A, 0x01, 0x02, 0x00};
  std::vector<uint8_t> h(kIszHeader, 0);
  memcpy(h.data(), kSig, 8);
  set16(h, 0x0A, f.flags);
  set16(h, 0x0C, f.files);
  set16(h, 0x0E, f.date);
  set16(h, 0x10, f.time);
  set32(h, 0x12, f.library_size);
  set32(h, 0x16, f.total_size);
  set32(h, 0x1A, f.taken);
  h[0x1E] = f.volumes;
  h[0x1F] = f.volume;
  h[0x20] = f.check;
  set32(h, 0x21, f.split_start);
  set32(h, 0x25, f.cont_end);
  set32(h, 0x29, f.dir_off);
  set32(h, 0x2D, f.dir_size);
  set16(h, 0x31, f.dirs);
  set32(h, 0x33, f.file_off);
  set32(h, 0x37, f.file_size);
  set32(h, 0x3B, f.volume_size);
  return h;
}

}  // namespace iszdetail

// An unsplit library (zwrite.write_library): the header, the streams back to
// back from byte 255, the directory table (one unnamed directory holding
// every member unless `dirs` says otherwise), the file table.
inline IszBuilt isz_library(const std::vector<IszSpec>& members, std::vector<IszDirSpec> dirs = {},
                            uint16_t date = 0x1B8D, uint16_t time = 0xBABD, uint8_t byte1b = 1) {
  using namespace iszdetail;
  if (dirs.empty()) dirs = {{"", uint16_t(members.size())}};
  IszBuilt b;
  std::vector<uint8_t> data, entries;
  uint32_t off = uint32_t(kIszHeader), total = 0;
  for (const IszSpec& m : members) {
    b.entries.push_back(entries.size());
    const std::vector<uint8_t> e = file_entry(m, off, 0, 0, 0, byte1b);
    entries.insert(entries.end(), e.begin(), e.end());
    data.insert(data.end(), m.stream.begin(), m.stream.end());
    off += uint32_t(m.stream.size());
    total += m.size;
  }
  const std::vector<uint8_t> dt = dir_table(dirs);
  HeaderFields f;
  f.files = uint16_t(members.size());
  f.date = date;
  f.time = time;
  f.dir_off = uint32_t(kIszHeader + data.size());
  f.dir_size = uint32_t(dt.size());
  f.dirs = uint16_t(dirs.size());
  f.file_off = f.dir_off + f.dir_size;
  f.file_size = uint32_t(entries.size());
  f.library_size = f.file_off + f.file_size;
  f.total_size = total;
  std::vector<uint8_t> v = header(f);
  v.insert(v.end(), data.begin(), data.end());
  v.insert(v.end(), dt.begin(), dt.end());
  v.insert(v.end(), entries.begin(), entries.end());
  b.volumes.push_back(std::move(v));
  return b;
}

// A split set (zwrite.write_split): the streams back to back, cut so that
// volume i holds data_sizes[i] data bytes (the last volume the rest). A
// member cut by a boundary continues in the next volume(s) — which the
// reader accepts only across one boundary, and a cut that falls between two
// members it refuses: the tests write those shapes on purpose.
inline IszBuilt isz_split(const std::vector<IszSpec>& members, const std::vector<size_t>& data_sizes,
                          uint16_t date = 0x1B8D, uint16_t time = 0xBABD, uint8_t byte1b = 1) {
  using namespace iszdetail;
  std::vector<uint8_t> stream;
  for (const IszSpec& m : members) stream.insert(stream.end(), m.stream.begin(), m.stream.end());
  std::vector<std::pair<size_t, size_t>> cuts;  // [a, b) of the stream, per volume
  size_t pos = 0;
  for (size_t n : data_sizes) {
    const size_t e = std::min(pos + n, stream.size());
    cuts.push_back({pos, e});
    pos = e;
  }
  cuts.push_back({pos, stream.size()});
  const size_t nvol = cuts.size();
  // (volume, offset) of a stream position; `end`: a position that ends a member.
  auto locate = [&](size_t at, bool end) -> std::pair<size_t, size_t> {
    for (size_t v = 0; v < nvol; v++)
      if ((cuts[v].first <= at && at < cuts[v].second) || (end && cuts[v].first < at && at <= cuts[v].second))
        return {v + 1, kIszHeader + (at - cuts[v].first)};
    return {nvol, kIszHeader + (at - cuts[nvol - 1].first)};  // an empty member at the very end
  };
  const std::vector<IszDirSpec> dirs = {{"", uint16_t(members.size())}};
  const std::vector<uint8_t> dt = dir_table(dirs);
  IszBuilt b;
  std::vector<uint8_t> entries;
  struct Where {
    size_t first, offset, last;
  };
  std::vector<Where> where;
  size_t p = 0;
  uint32_t total = 0;
  for (const IszSpec& m : members) {
    const auto [fv, fo] = locate(p, false);
    const size_t lv = m.stream.empty() ? fv : locate(p + m.stream.size(), true).first;
    where.push_back({fv, fo, lv});
    b.entries.push_back(entries.size());
    const std::vector<uint8_t> e =
        file_entry(m, uint32_t(fo), uint8_t(fv), uint8_t(lv), uint16_t(lv > fv ? 0x0100 : 0), byte1b);
    entries.insert(entries.end(), e.begin(), e.end());
    p += m.stream.size();
    total += m.size;
  }
  const uint32_t library_size = uint32_t(kIszHeader + stream.size() + dt.size() + entries.size());
  for (size_t v = 0; v < nvol; v++) {
    const size_t vn = v + 1;
    const auto [a, e] = cuts[v];
    HeaderFields f;
    f.flags = 1;
    f.files = uint16_t(members.size());
    f.date = date;
    f.time = time;
    f.library_size = library_size;
    f.total_size = total;
    f.volumes = vn == 1 ? uint8_t(nvol) : 0;
    f.volume = uint8_t(vn);
    f.check = uint8_t(library_size % 253);
    f.dir_off = uint32_t(kIszHeader + (e - a));
    f.dir_size = uint32_t(dt.size());
    f.dirs = 1;
    f.file_off = f.dir_off + f.dir_size;
    f.file_size = uint32_t(entries.size());
    f.volume_size = f.file_off + f.file_size;
    // Where the member continuing into the next volume starts (0: none).
    f.split_start = 0;
    for (const Where& w : where)
      if (w.first == vn && w.last > vn) f.split_start = uint32_t(w.offset);
    // Where the part continued from the previous volume ends.
    f.cont_end = 0;
    if (vn > 1) {
      f.cont_end = uint32_t(kIszHeader);
      size_t q = 0;
      for (size_t i = 0; i < members.size(); i++) {
        q += members[i].stream.size();
        if (where[i].first < vn && vn <= where[i].last) f.cont_end = uint32_t(kIszHeader + std::min(q, e) - a);
      }
    }
    f.taken = vn == 1 ? uint32_t(kIszHeader) : f.cont_end + f.dir_size + f.file_size;
    std::vector<uint8_t> vol = header(f);
    vol.insert(vol.end(), stream.begin() + ptrdiff_t(a), stream.begin() + ptrdiff_t(e));
    vol.insert(vol.end(), dt.begin(), dt.end());
    vol.insert(vol.end(), entries.begin(), entries.end());
    b.volumes.push_back(std::move(vol));
  }
  return b;
}

// Fields of a built library, read back (little-endian).
inline uint32_t isz_get(const std::vector<uint8_t>& v, size_t at, int width) {
  uint32_t x = 0;
  for (int i = width - 1; i >= 0; i--) x = x << 8 | v[at + size_t(i)];
  return x;
}
inline void isz_set(std::vector<uint8_t>& v, size_t at, uint32_t x, int width) {
  for (int i = 0; i < width; i++) v[at + size_t(i)] = uint8_t(x >> (8 * i));
}
// Where file entry `i` is in volume `v` of `b`.
inline size_t isz_entry_at(const IszBuilt& b, size_t v, size_t i) {
  return isz_get(b.volumes[v], 0x33, 4) + b.entries[i];
}
// A field of entry `i`, set in every volume (their tables stay identical).
inline void isz_set_entry(IszBuilt& b, size_t i, size_t field, uint32_t x, int width) {
  for (size_t v = 0; v < b.volumes.size(); v++) isz_set(b.volumes[v], isz_entry_at(b, v, i) + field, x, width);
}

}  // namespace test
