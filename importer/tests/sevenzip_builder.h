// Builds 7z archives for test_sevenzip.cc: stored (Copy) blocks of made-up
// data, written by 7zFormat.txt — the signature header with its CRCs, the
// packed streams, and a plain header (pack info, one coder per block,
// substreams with every file's CRC-32, empty files, folders, UTF-16 names).
// A block is one file, or every file together (solid). A test may give the
// blocks another coder (an id and properties, its data passed through as it
// is) to see the reader refuse it, and declare other sizes than the data's.
#pragma once

#include <zlib.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace test {

struct SevenZipEntry {
  std::string name;  // UTF-8, '/'-separated as the archive stores it (7-Zip writes '\' on Windows)
  std::vector<uint8_t> data;
  bool directory = false;
};

class SevenZipBuilder {
 public:
  std::vector<SevenZipEntry> entries;
  bool solid = false;
  std::vector<uint8_t> coder = {0x00};  // Copy
  std::vector<uint8_t> props;
  std::optional<uint64_t> declared_block_size;  // instead of the first block's real size
  std::vector<uint8_t> trailing;                // bytes after the header

  void add(const std::string& name, std::vector<uint8_t> data) { entries.push_back({name, std::move(data), false}); }
  void add_dir(const std::string& name) { entries.push_back({name, {}, true}); }

  std::vector<uint8_t> build() const {
    // The blocks: the files with data, one each or all together.
    std::vector<std::vector<const SevenZipEntry*>> blocks;
    for (const SevenZipEntry& e : entries) {
      if (e.directory || e.data.empty()) continue;
      if (!solid || blocks.empty()) blocks.emplace_back();
      blocks.back().push_back(&e);
    }
    std::vector<uint8_t> packed;
    std::vector<uint64_t> block_sizes;
    for (const auto& b : blocks) {
      uint64_t size = 0;
      for (const SevenZipEntry* e : b) {
        packed.insert(packed.end(), e->data.begin(), e->data.end());
        size += e->data.size();
      }
      block_sizes.push_back(size);
    }
    std::vector<uint8_t> h;
    auto byte = [&](uint8_t b) { h.push_back(b); };
    auto u32 = [&](uint32_t v) {
      for (int i = 0; i < 4; i++) h.push_back(uint8_t(v >> (8 * i)));
    };
    byte(0x01);  // kHeader
    if (!blocks.empty()) {
      byte(0x04);  // kMainStreamsInfo
      byte(0x06);  // kPackInfo
      number(h, 0);
      number(h, blocks.size());
      byte(0x09);
      for (uint64_t s : block_sizes) number(h, s);
      byte(0x00);
      byte(0x07);  // kUnpackInfo
      byte(0x0B);
      number(h, blocks.size());
      byte(0);
      for (size_t i = 0; i < blocks.size(); i++) {
        number(h, 1);  // one coder
        byte(uint8_t(coder.size() | (props.empty() ? 0 : 0x20)));
        h.insert(h.end(), coder.begin(), coder.end());
        if (!props.empty()) {
          number(h, props.size());
          h.insert(h.end(), props.begin(), props.end());
        }
      }
      byte(0x0C);
      for (size_t i = 0; i < blocks.size(); i++)
        number(h, i == 0 && declared_block_size ? *declared_block_size : block_sizes[i]);
      byte(0x00);
      byte(0x08);  // kSubStreamsInfo
      byte(0x0D);
      for (const auto& b : blocks) number(h, b.size());
      byte(0x09);
      for (const auto& b : blocks)
        for (size_t i = 0; i + 1 < b.size(); i++) number(h, b[i]->data.size());
      byte(0x0A);
      byte(1);  // all defined
      for (const auto& b : blocks)
        for (const SevenZipEntry* e : b) u32(uint32_t(crc32(crc32(0, nullptr, 0), e->data.data(), uInt(e->data.size()))));
      byte(0x00);
      byte(0x00);
    }
    if (!entries.empty()) {
      byte(0x05);  // kFilesInfo
      number(h, entries.size());
      std::vector<bool> empty_stream, empty_file;
      for (const SevenZipEntry& e : entries) {
        empty_stream.push_back(e.directory || e.data.empty());
        if (empty_stream.back()) empty_file.push_back(!e.directory);
      }
      if (!empty_file.empty()) {
        property(h, 0x0E, bits(empty_stream));
        property(h, 0x0F, bits(empty_file));
      }
      std::vector<uint8_t> names = {0};  // not external
      for (const SevenZipEntry& e : entries) {
        for (uint32_t c : utf16(e.name)) {
          names.push_back(uint8_t(c));
          names.push_back(uint8_t(c >> 8));
        }
        names.push_back(0);
        names.push_back(0);
      }
      property(h, 0x11, names);
      std::vector<uint8_t> attrs = {1, 0};  // all defined, not external
      for (const SevenZipEntry& e : entries) {
        const uint32_t a = e.directory ? 0x10 : 0x20;
        for (int i = 0; i < 4; i++) attrs.push_back(uint8_t(a >> (8 * i)));
      }
      property(h, 0x15, attrs);
      byte(0x00);
    }
    byte(0x00);
    std::vector<uint8_t> out(32);
    const uint8_t sig[8] = {'7', 'z', 0xBC, 0xAF, 0x27, 0x1C, 0, 4};
    std::copy(sig, sig + 8, out.begin());
    put64(out, 12, packed.size());
    put64(out, 20, h.size());
    put32(out, 28, uint32_t(crc32(crc32(0, nullptr, 0), h.data(), uInt(h.size()))));
    put32(out, 8, uint32_t(crc32(crc32(0, nullptr, 0), out.data() + 12, 20)));
    out.insert(out.end(), packed.begin(), packed.end());
    out.insert(out.end(), h.begin(), h.end());
    out.insert(out.end(), trailing.begin(), trailing.end());
    return out;
  }

  // 7z's variable-length number.
  static void number(std::vector<uint8_t>& h, uint64_t v) {
    int n = 0;
    while (n < 8 && v >= (uint64_t(1) << (7 * (n + 1)))) n++;
    if (n == 8) {
      h.push_back(0xFF);
      for (int i = 0; i < 8; i++) h.push_back(uint8_t(v >> (8 * i)));
      return;
    }
    h.push_back(uint8_t((0xFF00 >> n) | (v >> (8 * n))));
    for (int i = 0; i < n; i++) h.push_back(uint8_t(v >> (8 * i)));
  }

 private:
  static void property(std::vector<uint8_t>& h, uint8_t id, const std::vector<uint8_t>& body) {
    h.push_back(id);
    number(h, body.size());
    h.insert(h.end(), body.begin(), body.end());
  }
  static std::vector<uint8_t> bits(const std::vector<bool>& v) {
    std::vector<uint8_t> out((v.size() + 7) / 8);
    for (size_t i = 0; i < v.size(); i++)
      if (v[i]) out[i / 8] |= uint8_t(0x80 >> (i % 8));
    return out;
  }
  static void put32(std::vector<uint8_t>& v, size_t at, uint32_t x) {
    for (int i = 0; i < 4; i++) v[at + i] = uint8_t(x >> (8 * i));
  }
  static void put64(std::vector<uint8_t>& v, size_t at, uint64_t x) {
    for (int i = 0; i < 8; i++) v[at + i] = uint8_t(x >> (8 * i));
  }
  // UTF-8 to UTF-16 code units (the tests' names are valid UTF-8).
  static std::vector<uint32_t> utf16(const std::string& s) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < s.size();) {
      const uint8_t c = uint8_t(s[i]);
      uint32_t cp;
      int n;
      if (c < 0x80) cp = c, n = 1;
      else if (c < 0xE0) cp = c & 0x1F, n = 2;
      else if (c < 0xF0) cp = c & 0x0F, n = 3;
      else cp = c & 0x07, n = 4;
      for (int k = 1; k < n; k++) cp = cp << 6 | (uint8_t(s[i + k]) & 0x3F);
      i += size_t(n);
      if (cp >= 0x10000) {
        out.push_back(0xD800 + ((cp - 0x10000) >> 10));
        out.push_back(0xDC00 + ((cp - 0x10000) & 0x3FF));
      } else {
        out.push_back(cp);
      }
    }
    return out;
  }
};

}  // namespace test
