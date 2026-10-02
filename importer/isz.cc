#include "isz.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <numeric>
#include <set>

#include "names.h"
#include "status.h"
#include "winutil.h"

namespace adw::import {

namespace {

using Sink = std::function<void(const uint8_t*, size_t)>;

uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t le32(const uint8_t* p) { return uint32_t(p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24); }

std::string hex(uint32_t v, int digits) {
  char b[16];
  snprintf(b, sizeof(b), "0x%0*X", digits, unsigned(v));
  return b;
}

std::string num(uint64_t v) { return std::to_string(v); }

// ---- the container (SURVEY_REPORT.md, "The InstallShield library") -------------------------
//
// All values little-endian. The header (255 bytes):
//   0x00  8  signature 13 5D 65 8C 3A 01 02 00       0x08  1  0
//   0x09  1  password flag (0)                      0x0A  2  flags: 1 = a volume of a split set
//   0x0C  2  files in the whole library             0x0E  2  DOS date, 0x10 2 DOS time
//   0x12  4  the library's size before it was split (255 + all compressed data + both tables)
//   0x16  4  the members' uncompressed sizes added up
//   0x1A  4  0xFF; in a volume after the first: header + the part continued into it + tables
//   0x1E  1  volumes (volume 1 only)                0x1F  1  this volume's number (0 unsplit)
//   0x20  1  (u32 at 0x12) mod 253 (split only)
//   0x21  4  0xFF unsplit; split: where the member continuing into the next volume starts,
//            0 in the last volume
//   0x25  4  split, a volume after the first: where the part continued from the previous
//            volume ends (in volume 1 it is left over: 0xEA4 in Marvel's IMAGES.1)
//   0x29  4  directory table offset, 0x2D 4 its size, 0x31 2 directories
//   0x33  4  file table offset, 0x37 4 its size     0x3B  4  split: this volume's size
//   0x3F-0xFE  zero
// A directory entry: u16 files, u16 entry size (11 + n), u16 n, the name, NUL, u32 0.
// A file entry (43 + n bytes): u8 last volume, u16 directory, u32 size, u32 compressed
// size, u32 data offset (in its first volume), u16 DOS date, u16 DOS time, u32 attributes,
// u16 entry size, u16 flags (0x0100: continues into the next volume; 0x0010: stored), u8 0
// or 1 (1 in the releases, 0 in ICOMP's output), u8 first volume, u8 n, the name, NUL,
// then u32 version MS, u32 version LS, u32 reserved (all 0).

constexpr uint8_t kSignature[8] = {0x13, 0x5D, 0x65, 0x8C, 0x3A, 0x01, 0x02, 0x00};
constexpr size_t kHeader = 0xFF;                   // the header's size: where the data starts
constexpr uint16_t kSplitSet = 0x0001;             // library flag
constexpr uint16_t kContinues = 0x0100;            // member flag
constexpr uint16_t kStored = 0x0010;               // member flag (ICOMP -sn; never in the releases)
constexpr uint32_t kAttributes = 0x27;             // read-only, hidden, system, archive
constexpr size_t kDirEntry = 11, kFileEntry = 43;  // an entry's size besides its name
constexpr size_t kFileFields = 0x1E;               // a file entry's fields before its name

bool dos_date_valid(uint16_t date, uint16_t time) {
  const unsigned month = (date >> 5) & 15, day = date & 31, hour = time >> 11, minute = (time >> 5) & 63;
  return month >= 1 && month <= 12 && day >= 1 && hour <= 23 && minute <= 59 && (time & 31) <= 29;
}

struct Header {
  uint16_t flags = 0, files = 0, date = 0, time = 0;
  uint32_t library_size = 0, total_size = 0, taken = 0;
  uint8_t volumes = 0, volume = 0, check = 0;
  uint32_t split_start = 0, cont_end = 0, dir_off = 0, dir_size = 0;
  uint16_t dirs = 0;
  uint32_t file_off = 0, file_size = 0, volume_size = 0;
  bool split() const { return (flags & kSplitSet) != 0; }
};

// Every rule the header can be held to on its own.
Header read_header(std::span<const uint8_t> d, const std::string& vol) {
  if (d.size() < kHeader) throw IszError(vol + ": not an InstallShield compressed library (too small)");
  if (!std::equal(std::begin(kSignature), std::end(kSignature), d.begin()))
    throw IszError(vol + ": not an InstallShield compressed library (no signature)");
  const uint8_t* h = d.data();
  if (h[9]) throw IszError(vol + ": password-protected libraries are not supported");
  if (h[8]) throw IszError(vol + ": damaged header (byte 0x08 is " + hex(h[8], 2) + ")");
  for (size_t i = 0x3F; i < kHeader; i++)
    if (h[i]) throw IszError(vol + ": damaged header (byte " + hex(uint32_t(i), 2) + " is not zero)");
  Header x;
  x.flags = le16(h + 0x0A);
  x.files = le16(h + 0x0C);
  x.date = le16(h + 0x0E);
  x.time = le16(h + 0x10);
  x.library_size = le32(h + 0x12);
  x.total_size = le32(h + 0x16);
  x.taken = le32(h + 0x1A);
  x.volumes = h[0x1E];
  x.volume = h[0x1F];
  x.check = h[0x20];
  x.split_start = le32(h + 0x21);
  x.cont_end = le32(h + 0x25);
  x.dir_off = le32(h + 0x29);
  x.dir_size = le32(h + 0x2D);
  x.dirs = le16(h + 0x31);
  x.file_off = le32(h + 0x33);
  x.file_size = le32(h + 0x37);
  x.volume_size = le32(h + 0x3B);
  if (x.flags & ~kSplitSet) throw IszError(vol + ": unknown library flags " + hex(x.flags, 4));
  if (!dos_date_valid(x.date, x.time)) throw IszError(vol + ": damaged header (an invalid date)");
  if (!x.split()) {
    if (x.volumes || x.volume || x.check || x.volume_size || x.cont_end || x.split_start != kHeader ||
        x.taken != kHeader)
      throw IszError(vol + ": damaged header (split-set fields in an unsplit library)");
    return x;
  }
  if (x.volume == 0) throw IszError(vol + ": damaged header (volume 0 of a split set)");
  if (x.volume == 1 && x.volumes < 2)
    throw IszError(vol + ": damaged header (volume 1 of a set of " + num(x.volumes) + ")");
  if (x.volume > 1 && x.volumes)
    throw IszError(vol + ": damaged header (a volume count in volume " + num(x.volume) + ")");
  if (x.check != x.library_size % 253)
    throw IszError(vol + ": damaged header (check byte " + num(x.check) + ", not " + num(x.library_size % 253) + ")");
  if (x.volume == 1 && x.taken != kHeader)
    throw IszError(vol + ": damaged header (field 0x1A is " + num(x.taken) + ")");
  return x;
}

struct Entry {  // one file-table entry
  std::string name;
  uint8_t first_volume = 0, last_volume = 0;
  uint16_t flags = 0, date = 0, time = 0;
  uint32_t size = 0, csize = 0, offset = 0, attributes = 0;
};

struct Volume {
  Header h;
  std::vector<Entry> files;
  std::span<const uint8_t> tables;  // both tables, which every volume of a set repeats
};

// One file of a library, parsed and checked on its own: its header, its size,
// where its tables are, and both tables.
Volume read_volume(const IszVolume& v) {
  if (!v.data) throw IszError(v.name + ": no data");
  std::span<const uint8_t> d(*v.data);
  const std::string& vol = v.name;
  Volume r;
  r.h = read_header(d, vol);
  const Header& h = r.h;
  const uint32_t recorded = h.split() ? h.volume_size : h.library_size;
  if (recorded != d.size())
    throw IszError(vol + ": " + num(d.size()) + " bytes, the header records " + num(recorded) +
                   " (truncated or padded)");
  // The data, then the directory table, then the file table, then the end.
  if (h.dir_off < kHeader || uint64_t(h.dir_off) + h.dir_size != h.file_off ||
      uint64_t(h.file_off) + h.file_size != d.size())
    throw IszError(vol + ": the tables are not where the header says");
  // A later volume counts the part continued into it and the tables as taken.
  if (h.split() && h.volume > 1 && uint64_t(h.cont_end) + h.dir_size + h.file_size != h.taken)
    throw IszError(vol + ": damaged header (field 0x1A is " + num(h.taken) +
                   ", not the continued part and the tables)");

  // The directory table: one directory, unnamed, holding every file.
  if (h.dirs != 1)
    throw IszError(vol + ": " + num(h.dirs) + " directories (only a library of one unnamed directory is supported)");
  size_t p = h.dir_off;
  const size_t dir_end = h.file_off;
  if (dir_end - p < 6) throw IszError(vol + ": damaged directory table");
  const size_t dir_files = le16(&d[p]), dir_esize = le16(&d[p + 2]), dir_n = le16(&d[p + 4]);
  if (dir_esize != kDirEntry + dir_n || dir_end - p < dir_esize)
    throw IszError(vol + ": damaged directory table (entry size)");
  if (d[p + 6 + dir_n] != 0 || le32(&d[p + 7 + dir_n]) != 0)
    throw IszError(vol + ": damaged directory table (the name's end)");
  if (dir_n) throw IszError(vol + ": named directories are not supported");
  if (dir_files != h.files)
    throw IszError(vol + ": the directory holds " + num(dir_files) + " files, the header records " + num(h.files));
  p += dir_esize;
  if (p != dir_end)
    throw IszError(vol + ": damaged directory table (" + num(dir_end - p) + " byte(s) after its entry)");

  // The file table.
  std::set<std::string> names;
  const size_t end = d.size();
  for (size_t i = 0; i < h.files; i++) {
    const std::string at = " (entry " + num(i) + ")";
    if (end - p < kFileFields) throw IszError(vol + ": damaged file table" + at);
    const uint8_t* e = &d[p];
    Entry f;
    f.last_volume = e[0x00];
    const uint16_t dir = le16(e + 0x01);
    f.size = le32(e + 0x03);
    f.csize = le32(e + 0x07);
    f.offset = le32(e + 0x0B);
    f.date = le16(e + 0x0F);
    f.time = le16(e + 0x11);
    f.attributes = le32(e + 0x13);
    const size_t esize = le16(e + 0x17);
    f.flags = le16(e + 0x19);
    const uint8_t b1b = e[0x1B];
    f.first_volume = e[0x1C];
    const size_t n = e[0x1D];
    if (esize != kFileEntry + n || end - p < esize) throw IszError(vol + ": damaged file table" + at + " (entry size)");
    const std::string raw(reinterpret_cast<const char*>(e + kFileFields), n);
    if (e[kFileFields + n] != 0) throw IszError(vol + ": damaged file table" + at + " (the name's end)");
    if (raw.empty() || raw.find('\0') != std::string::npos)
      throw IszError(vol + ": damaged file table" + at + " (an empty name, or a NUL inside it)");
    for (size_t k = kFileFields + n + 1; k < esize; k++)
      if (e[k]) throw IszError(vol + ": damaged file table" + at + " (version fields not zero)");
    f.name = oem437_to_utf8(raw);
    if (f.name.find_first_of("/\\:") != std::string::npos)
      throw IszError(vol + ": member \"" + f.name + "\" is not a bare file name");
    try {
      check_component(f.name, vol);
    } catch (const ImportError& ex) {
      throw IszError(ex.what());
    }
    // One name, as Windows compares them (code page 437's letters too).
    if (!names.insert(name_key(f.name)).second) throw IszError(vol + ": two members are named " + f.name);
    const std::string what = vol + "!" + f.name;
    if (f.flags & kStored) throw IszError(what + ": stored (uncompressed) members are not supported");
    if (f.flags & ~kContinues) throw IszError(what + ": unsupported flags " + hex(f.flags, 4));
    if (f.attributes & ~kAttributes) throw IszError(what + ": unsupported attributes " + hex(f.attributes, 2));
    if (dir != 0) throw IszError(what + ": damaged entry (directory " + num(dir) + " of 1)");
    if (b1b > 1) throw IszError(what + ": damaged entry (byte 0x1B is " + num(b1b) + ")");
    if (!dos_date_valid(f.date, f.time)) throw IszError(what + ": damaged entry (an invalid date)");
    r.files.push_back(std::move(f));
    p += esize;
  }
  if (p != end) throw IszError(vol + ": damaged file table (" + num(end - p) + " byte(s) after its entries)");
  r.tables = d.subspan(h.dir_off);
  return r;
}

// ---- PKWARE DCL explode (SURVEY_REPORT.md, "PKWARE DCL implode") ------------------------
//
// Written from the format's public description (Ben Rudiak-Gould's
// comp.compression note of August 2001, which the survey's reference
// tools/dclexplode.py follows). Byte 0: the literal mode (0: a literal is 8
// raw bits); byte 1: the dictionary bits k (the window is 64 << k bytes).
// Then a bit stream, each byte's least significant bit first. A token is a
// flag bit: 0, a literal; 1, a copy: a LENGTH code and its extra bits
// (2..518; 519 ends the data), then a DISTANCE code and its extra bits (2
// for a length of 2, else k): distance = (symbol << extra) + value + 1. A
// copy goes byte by byte, so it may overlap itself. The codes are fixed
// canonical Huffman codes (shorter codes first, within a length the lower
// symbols first); the stream holds each code's complement, most significant
// bit first.

// The code lengths, symbol by symbol (the format's constants).
constexpr std::array<uint8_t, 16> kLengthBits = {2, 3, 3, 3, 4, 4, 4, 5, 5, 5, 5, 6, 6, 6, 7, 7};
constexpr std::array<uint8_t, 64> kDistanceBits = {2, 4, 4, 5, 5, 5, 5, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
                                                   7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
                                                   7, 7, 7, 7, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8};
constexpr std::array<uint16_t, 16> kLengthBase = {3, 2, 4, 5, 6, 7, 8, 9, 10, 12, 16, 24, 40, 72, 136, 264};
constexpr std::array<uint8_t, 16> kLengthExtra = {0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8};
constexpr unsigned kEndLength = 519;

// A canonical code, for decoding one bit at a time: how many codes each
// length has, and the symbols in code order.
struct Code {
  std::array<uint8_t, 9> count{};  // by length, 1..8
  std::array<uint8_t, 64> symbol{};
  int max_bits = 0;
};

template <size_t N>
Code make_code(const std::array<uint8_t, N>& bits) {
  Code c;
  size_t k = 0;
  for (int len = 1; len <= 8; len++)
    for (size_t s = 0; s < N; s++)
      if (bits[s] == len) {
        c.count[size_t(len)]++;
        c.symbol[k++] = uint8_t(s);
        c.max_bits = len;
      }
  return c;
}

const Code& length_code() {
  static const Code c = make_code(kLengthBits);
  return c;
}
const Code& distance_code() {
  static const Code c = make_code(kDistanceBits);
  return c;
}

// The compressed bytes, one piece per segment, read as one stream.
class BitIn {
 public:
  explicit BitIn(const std::vector<std::span<const uint8_t>>& pieces) : pieces_(pieces) {
    for (const auto& s : pieces_) left_ += s.size();
  }

  size_t bytes_left() const { return left_; }

  uint8_t byte() {
    while (pos_ == pieces_[piece_].size()) piece_++, pos_ = 0;
    left_--;
    return pieces_[piece_][pos_++];
  }

  // The next n (0..8) bits, the first read the least significant.
  uint32_t bits(int n) {
    while (count_ < n) {
      if (!left_) throw IszError("the compressed data ends before its end code (truncated)");
      buf_ |= uint32_t(byte()) << count_;
      count_ += 8;
    }
    const uint32_t v = buf_ & ((1u << n) - 1);
    buf_ >>= n;
    count_ -= n;
    return v;
  }

  // One symbol: the code's bits, most significant first, each the
  // complement of the bit read. Codes of one length are consecutive values,
  // the first of each length is (the first of the length before + its
  // count) << 1; every code here is complete, so a symbol is always found.
  unsigned symbol(const Code& c) {
    unsigned code = 0, first = 0, index = 0;
    for (int len = 1; len <= c.max_bits; len++) {
      code = code << 1 | (bits(1) ^ 1);
      const unsigned n = c.count[size_t(len)];
      if (code - first < n) return c.symbol[index + (code - first)];
      index += n;
      first = (first + n) << 1;
    }
    throw IszError("damaged compressed data (no such code)");  // unreachable: the codes are complete
  }

  // What is left of the last byte read (fewer than 8 bits).
  uint32_t leftover() const { return buf_; }

 private:
  const std::vector<std::span<const uint8_t>>& pieces_;
  size_t piece_ = 0, pos_ = 0, left_ = 0;
  uint32_t buf_ = 0;
  int count_ = 0;
};

// The output: a 64 KiB ring (the window of at most 4096 bytes, and up to
// 32 KiB not yet handed on), emitted in chunks of at most 32 KiB. No token
// may produce a byte past `size` (`exact`: the member's recorded size; else
// the caller's bound, IszTooLarge) or copy from before the first byte.
class Output {
 public:
  Output(uint64_t size, bool exact, const Sink& sink) : size_(size), exact_(exact), sink_(sink), ring_(kRing) {}

  uint64_t produced() const { return produced_; }

  void literal(uint8_t b) {
    if (produced_ == size_) past();
    put(b);
  }

  void copy(uint32_t dist, uint32_t len) {
    if (dist > produced_)
      throw IszError("a copy reaches " + num(dist) + " bytes back with only " + num(produced_) + " written");
    if (len > size_ - produced_) past();
    const uint64_t from = produced_ - dist;
    for (uint32_t i = 0; i < len; i++) put(ring_[size_t((from + i) & kMask)]);
  }

  void flush() {
    while (emitted_ < produced_) {
      const size_t at = size_t(emitted_ & kMask);
      const size_t n = std::min<size_t>({size_t(produced_ - emitted_), kRing - at, kChunk});
      sink_(&ring_[at], n);
      emitted_ += n;
    }
  }

 private:
  static constexpr size_t kRing = 65536, kMask = kRing - 1, kChunk = 32768;
  const uint64_t size_;
  const bool exact_;
  const Sink& sink_;
  std::vector<uint8_t> ring_;
  uint64_t produced_ = 0, emitted_ = 0;

  [[noreturn]] void past() const {
    if (!exact_) throw IszTooLarge("the data expands past " + num(size_) + " bytes, the most allowed");
    throw IszError("the data expands past its recorded size, " + num(size_) + " bytes");
  }

  void put(uint8_t b) {
    ring_[size_t(produced_ & kMask)] = b;
    if (++produced_ - emitted_ >= kChunk) flush();
  }
};

// One stream to its end code: exactly `size` bytes (`exact`, the member's
// recorded size), or at most `size` (an InstallShield 1 file, which records
// none). Returns how many there were.
uint64_t explode_pieces(const std::vector<std::span<const uint8_t>>& pieces, uint64_t size, bool exact,
                        const Sink& sink) {
  BitIn in(pieces);
  if (in.bytes_left() < 2) throw IszError("the compressed data is shorter than its 2-byte header");
  const unsigned mode = in.byte(), dict = in.byte();
  if (mode == 1) throw IszError("PKWARE's coded-literal (ASCII) mode is not supported");
  if (mode != 0) throw IszError("unknown literal mode " + num(mode));
  if (dict < 4 || dict > 6) throw IszError("dictionary bits " + num(dict) + " (only 4, 5 and 6 exist)");
  Output out(size, exact, sink);
  for (;;) {
    if (!in.bits(1)) {
      out.literal(uint8_t(in.bits(8)));
      continue;
    }
    const unsigned s = in.symbol(length_code());
    const unsigned len = kLengthBase[s] + in.bits(kLengthExtra[s]);
    if (len == kEndLength) break;
    const int extra = len == 2 ? 2 : int(dict);
    const uint32_t dist = (in.symbol(distance_code()) << extra) + in.bits(extra) + 1;
    out.copy(dist, len);
  }
  // The end code: exactly the recorded size, and nothing after it but the
  // rest of its last byte, zero.
  if (exact && out.produced() != size)
    throw IszError("the data ends after " + num(out.produced()) + " bytes, " + num(size) + " recorded");
  if (in.bytes_left()) throw IszError(num(in.bytes_left()) + " byte(s) follow the end code");
  if (in.leftover()) throw IszError("the bits after the end code are not zero");
  out.flush();
  return out.produced();
}

}  // namespace

namespace isz_detail {

void explode(std::span<const uint8_t> in, uint32_t size, const Sink& sink) { explode_pieces({in}, size, true, sink); }

}  // namespace isz_detail

// ---- the library -------------------------------------------------------------------------------

IszLibrary::IszLibrary(std::vector<IszVolume> volumes) : volumes_(std::move(volumes)) {
  if (volumes_.empty()) throw IszError("no InstallShield library volumes");
  std::vector<Volume> parsed;
  for (const IszVolume& v : volumes_) parsed.push_back(read_volume(v));
  // Set order: volume 1 first (an unsplit library is volume 0).
  std::vector<size_t> order(volumes_.size());
  std::iota(order.begin(), order.end(), size_t(0));
  std::stable_sort(order.begin(), order.end(),
                   [&](size_t a, size_t b) { return parsed[a].h.volume < parsed[b].h.volume; });
  {
    std::vector<IszVolume> vs;
    std::vector<Volume> ps;
    for (size_t i : order) vs.push_back(volumes_[i]), ps.push_back(parsed[i]);
    volumes_ = std::move(vs);
    parsed = std::move(ps);
  }
  auto vol = [&](size_t i) -> const std::string& { return volumes_[i].name; };
  const Header& h1 = parsed[0].h;
  const size_t n = h1.split() ? h1.volumes : 1;
  for (size_t i = 0; i < parsed.size(); i++) {
    if (!parsed[i].h.split() && parsed.size() > 1)
      throw IszError(vol(i) + " is an unsplit library, not a volume of a set with " + vol(i ? 0 : 1));
  }
  if (h1.split()) {
    // A missing volume is named as the set would name it ("IMAGES.1" beside
    // IMAGES.2), unless a given file has that name: that file holds another
    // volume (the files swapped, or another set's), and is there.
    auto given = [&](const std::optional<std::string>& name) -> std::optional<size_t> {
      for (size_t i = 0; name && i < volumes_.size(); i++)
        if (name_key(vol(i)) == name_key(*name)) return i;
      return std::nullopt;
    };
    if (h1.volume != 1) {
      const auto first = isz_volume_name(vol(0), 1);
      if (const auto i = given(first))
        throw IszError("the set's volume 1 is missing (" + vol(*i) + " is volume " + num(parsed[*i].h.volume) + ")");
      throw IszError("the set's volume 1" + (first ? " (" + *first + ")" : std::string()) + " is missing");
    }
    for (size_t i = 1; i < parsed.size(); i++) {
      const Header& h = parsed[i].h;
      if (h.volume == parsed[i - 1].h.volume)
        throw IszError(vol(i - 1) + " and " + vol(i) + " are both volume " + num(h.volume) + " of the set");
      if (h.volume > n) throw IszError(vol(i) + ": volume " + num(h.volume) + " of a set of " + num(n));
      if (h.files != h1.files || h.date != h1.date || h.time != h1.time || h.library_size != h1.library_size ||
          h.total_size != h1.total_size || h.dirs != h1.dirs || h.dir_size != h1.dir_size ||
          h.file_size != h1.file_size)
        throw IszError(vol(i) + ": not a volume of the same set as " + vol(0) + " (its header differs)");
      if (!std::equal(parsed[i].tables.begin(), parsed[i].tables.end(), parsed[0].tables.begin(),
                      parsed[0].tables.end()))
        throw IszError(vol(i) + ": not a volume of the same set as " + vol(0) + " (its tables differ)");
    }
    for (size_t k = 1; k <= n; k++)
      if (k > parsed.size() || parsed[k - 1].h.volume != k) {
        const auto name = isz_volume_name(vol(0), unsigned(k));
        if (const auto i = given(name))
          throw IszError(vol(0) + ": the library continues on volume " + num(k) + " of " + num(n) +
                         ", which is missing (" + vol(*i) + " is volume " + num(parsed[*i].h.volume) + ")");
        throw IszError(vol(0) + ": the library continues on " + (name ? *name : "another volume") + " (volume " +
                       num(k) + " of " + num(n) + "), which is missing");
      }
  }

  // What the header records of the members.
  const std::vector<Entry>& files = parsed[0].files;
  uint64_t total = 0, compressed = 0;
  for (const Entry& f : files) total += f.size, compressed += f.csize;
  if (total != h1.total_size)
    throw IszError(vol(0) + ": the members' sizes add up to " + num(total) + ", the header records " +
                   num(h1.total_size));
  if (kHeader + compressed + h1.dir_size + h1.file_size != h1.library_size)
    throw IszError(vol(0) + ": the members' data and the tables add up to " +
                   num(kHeader + compressed + h1.dir_size + h1.file_size) + " bytes, the header records " +
                   num(h1.library_size));

  // Where each member's data is: back to back from byte 255 in table order,
  // across the volumes; one member crosses each boundary, its head ending
  // one volume (at the offset that volume's header names) and its tail
  // opening the next (up to the offset that one's header names).
  size_t cur = 0;        // the volume (set order) the previous member ended in
  size_t pos = kHeader;  // where in it
  auto dir = [&](size_t v) -> size_t { return parsed[v].h.dir_off; };
  for (const Entry& f : files) {
    const std::string what = vol(cur) + "!" + f.name;
    IszMember m;
    m.name = f.name;
    m.size = f.size;
    m.csize = f.csize;
    m.dos_datetime = uint32_t(f.date) << 16 | f.time;
    m.attributes = f.attributes;
    const bool continues = (f.flags & kContinues) != 0;
    if (!h1.split()) {
      if (f.first_volume || f.last_volume || continues) throw IszError(what + ": volume fields in an unsplit library");
    } else {
      const size_t a = f.first_volume, b = f.last_volume;
      if (a < 1 || b < a || b > n)
        throw IszError(what + ": damaged entry (volumes " + num(a) + " to " + num(b) + " of a set of " + num(n) + ")");
      if (b > a + 1)
        throw IszError(what + ": spans " + num(b - a + 1) +
                       " volumes (a member spanning more than two is not supported)");
      if (continues != (b == a + 1))
        throw IszError(what + ": damaged entry (volumes " + num(a) + " to " + num(b) +
                       (continues ? ", continued" : ", not continued") + ")");
      if (a != cur + 1) {
        if (a < cur + 1)
          throw IszError(what + ": out of order (its data is in volume " + num(a) +
                         ", the previous member's in volume " + num(cur + 1) + ")");
        if (pos != dir(cur))
          throw IszError(vol(cur) + ": " + num(dir(cur) - pos) + " unused byte(s) before its tables");
        throw IszError(vol(cur) +
                       ": the volume ends between two members (a boundary no member crosses is not supported)");
      }
    }
    if (f.offset != pos) throw IszError(what + ": its data is at " + num(f.offset) + ", expected " + num(pos));
    if (!continues) {
      if (f.csize > dir(cur) - pos) throw IszError(what + ": its data runs into the tables");
      m.segments.push_back({cur, pos, f.csize});
      m.volumes = vol(cur);
      pos += f.csize;
    } else {
      const Header& ha = parsed[cur].h;
      const Header& hb = parsed[cur + 1].h;
      if (ha.split_start != f.offset)
        throw IszError(vol(cur) + ": its header says the member continuing into the next volume starts at " +
                       num(ha.split_start) + ", " + f.name + " starts at " + num(f.offset));
      // Its head is the rest of the volume, its tail at least a byte: a
      // boundary at either end of it would lie between two members.
      const size_t head = dir(cur) - pos;
      if (head == 0 || head == f.csize)
        throw IszError(vol(cur) +
                       ": the volume ends between two members (a boundary no member crosses is not supported)");
      if (head > f.csize)
        throw IszError(what + ": continues into the next volume, but " + num(head - f.csize) + " byte(s) of " +
                       vol(cur) + " are left after it");
      const size_t tail = f.csize - head;
      if (hb.cont_end != kHeader + tail)
        throw IszError(vol(cur + 1) + ": its header says the part continued from " + vol(cur) + " ends at " +
                       num(hb.cont_end) + ", " + f.name + " needs " + num(kHeader + tail));
      if (tail > dir(cur + 1) - kHeader) throw IszError(what + ": its data runs into the tables of " + vol(cur + 1));
      m.segments.push_back({cur, pos, uint32_t(head)});
      m.segments.push_back({cur + 1, kHeader, uint32_t(tail)});
      m.volumes = vol(cur) + "+" + vol(cur + 1);
      cur++;
      pos = kHeader + tail;
    }
    members_.push_back(std::move(m));
  }
  // Every volume used up; nothing continues past the last.
  if (pos != dir(cur)) throw IszError(vol(cur) + ": " + num(dir(cur) - pos) + " unused byte(s) before its tables");
  if (h1.split()) {
    if (cur + 1 != n)
      throw IszError(vol(cur) +
                     ": the volume ends between two members (a boundary no member crosses is not supported)");
    if (parsed[cur].h.split_start != 0)
      throw IszError(vol(cur) + ": its header says a member continues past the last volume");
  }
}

const IszMember* IszLibrary::find(std::string_view name) const {
  for (const IszMember& m : members_)
    if (iequals(m.name, name)) return &m;
  return nullptr;
}

void IszLibrary::extract(const IszMember& m, const Sink& sink) const {
  const std::string what = m.volumes + "!" + m.name;
  std::vector<std::span<const uint8_t>> pieces;
  uint64_t csize = 0;
  for (const IszSegment& s : m.segments) {
    if (s.volume >= volumes_.size()) throw IszError(what + ": no such volume");
    const std::vector<uint8_t>& d = *volumes_[s.volume].data;
    if (s.offset > d.size() || d.size() - s.offset < s.csize) throw IszError(what + ": the library is truncated");
    pieces.emplace_back(d.data() + s.offset, s.csize);
    csize += s.csize;
  }
  if (csize != m.csize) throw IszError(what + ": its segments are not its compressed size");
  // Messages from the decoder are completed with `what`; whatever the
  // caller's sink throws passes through unchanged.
  bool in_sink = false;
  const Sink emit = [&](const uint8_t* p, size_t n) {
    in_sink = true;
    sink(p, n);
    in_sink = false;
  };
  try {
    explode_pieces(pieces, m.size, true, emit);
  } catch (const IszError& e) {
    if (in_sink) throw;
    throw IszError(what + ": " + e.what());
  }
}

IszHeader isz_header(std::span<const uint8_t> data, std::string_view volume_name) {
  const Header h = read_header(data, std::string(volume_name));
  IszHeader r;
  r.split = h.split();
  r.volume = h.volume;
  r.volumes = h.volumes;
  r.files = h.files;
  r.dos_datetime = uint32_t(h.date) << 16 | h.time;
  r.library_size = h.library_size;
  r.total_size = h.total_size;
  return r;
}

std::optional<std::string> isz_volume_name(std::string_view first, unsigned n) {
  const size_t dot = first.rfind('.');
  if (dot == std::string_view::npos || dot + 1 == first.size() || n == 0) return std::nullopt;
  for (char c : first.substr(dot + 1))
    if (c < '0' || c > '9') return std::nullopt;
  return std::string(first.substr(0, dot + 1)) + std::to_string(n);
}

// ---- InstallShield 1's compressed files ------------------------------------------------------

namespace {

constexpr uint8_t kIs1Magic[8] = {0x65, 0x5D, 0x13, 0x8C, 0x08, 0x01, 0x03, 0x00};
constexpr size_t kIs1Fields = 0x1D;  // the header's fields before the name
// The largest such file the importer reads (the release's largest is 1 MB).
constexpr uint64_t kIs1MaxFile = 16ull << 20;

// An 8.3 DOS name: 1 to 8 characters, then optionally a dot and 1 to 3.
bool dos_8_3(const std::string& raw) {
  const size_t dot = raw.find('.');
  if (dot == std::string::npos) return raw.size() >= 1 && raw.size() <= 8;
  const size_t ext = raw.size() - dot - 1;
  return dot >= 1 && dot <= 8 && ext >= 1 && ext <= 3 && raw.find('.', dot + 1) == std::string::npos;
}

}  // namespace

Is1Header is1_header(std::span<const uint8_t> d, std::string_view where_view) {
  const std::string where(where_view);
  if (d.size() < kIs1Fields || !std::equal(kIs1Magic, kIs1Magic + 8, d.data()))
    throw IszError(where + ": not an InstallShield 1 compressed file");
  if (d.size() > kIs1MaxFile) throw IszError(where + ": larger than any InstallShield 1 file the importer reads");
  auto bad = [&](const char* what) { return IszError(where + ": damaged header (" + std::string(what) + ")"); };
  if (le32(d.data() + 0x08) != 1 || d[0x0C] != 0x00 || d[0x0D] != 0x12) throw bad("its fixed fields");
  if (le32(d.data() + 0x12) != 0 || le16(d.data() + 0x1A) != 0) throw bad("its zero fields");
  const uint16_t date = le16(d.data() + 0x16), time = le16(d.data() + 0x18);
  if (!dos_date_valid(date, time)) throw bad("an invalid date");
  Is1Header h;
  const size_t n = d[0x1C];
  if (d.size() < kIs1Fields + n + 1) throw IszError(where + ": truncated (inside the header)");
  const std::string raw(reinterpret_cast<const char*>(d.data() + kIs1Fields), n);
  if (d[kIs1Fields + n] != 0) throw bad("the stored name's end");
  if (raw.find('\0') != std::string::npos || !dos_8_3(raw)) throw bad("the stored name is no 8.3 DOS name");
  h.name = oem437_to_utf8(raw);
  if (h.name.find_first_of("/\\:") != std::string::npos) throw bad("the stored name is no bare file name");
  try {
    check_component(h.name, where);
  } catch (const ImportError&) {
    throw bad("the stored name is no usable file name");
  }
  h.data_offset = kIs1Fields + n + 1;
  h.csize = le32(d.data() + 0x0E);
  h.dos_datetime = uint32_t(date) << 16 | time;
  if (h.csize != d.size() - h.data_offset)
    throw IszError(where + ": the header records " + num(h.csize) + " compressed bytes, the file holds " +
                   num(d.size() - h.data_offset) + " (truncated, or something appended)");
  return h;
}

uint64_t is1_expand(std::span<const uint8_t> file, std::string_view where, uint64_t max_size, const Sink& sink) {
  const Is1Header h = is1_header(file, where);
  // Messages from the decoder are completed with `where`; whatever the
  // caller's sink throws passes through unchanged.
  bool in_sink = false;
  const Sink emit = [&](const uint8_t* p, size_t n) {
    in_sink = true;
    sink(p, n);
    in_sink = false;
  };
  try {
    return explode_pieces({file.subspan(h.data_offset)}, max_size, false, emit);
  } catch (const IszTooLarge& e) {
    if (in_sink) throw;
    throw IszTooLarge(std::string(where) + ": " + e.what());
  } catch (const IszError& e) {
    if (in_sink) throw;
    throw IszError(std::string(where) + ": " + e.what());
  }
}

}  // namespace adw::import
