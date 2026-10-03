// A ustar writer for the tests (tar.h): made-up members, each a 512-byte
// header (name, or a prefix and a name, octal size and checksum) and its
// data padded to 512, then two zero blocks. Nothing else of tar is written.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace test {

struct TarBuilder {
  struct Entry {
    std::string name;    // as written in the name field (at most 100 bytes)
    std::string prefix;  // the ustar prefix field ("" = none)
    std::vector<uint8_t> data;
    char type = '0';
  };
  std::vector<Entry> entries;
  bool ustar = true;  // the "ustar" magic; false: a V7 header

  void add(const std::string& name, std::vector<uint8_t> data, char type = '0', const std::string& prefix = "") {
    entries.push_back({name, prefix, std::move(data), type});
  }

  static std::vector<uint8_t> header(const Entry& e, bool ustar) {
    std::vector<uint8_t> h(512, 0);
    memcpy(h.data(), e.name.data(), std::min<size_t>(e.name.size(), 100));
    auto octal = [&](size_t at, size_t width, uint64_t v) {
      char buf[32];
      snprintf(buf, sizeof(buf), "%0*llo", int(width - 1), (unsigned long long)v);
      memcpy(h.data() + at, buf, width - 1);
    };
    octal(100, 8, 0644);
    octal(108, 8, 0);
    octal(116, 8, 0);
    octal(124, 12, e.data.size());
    octal(136, 12, 0);
    h[156] = uint8_t(e.type);
    if (ustar) {
      memcpy(h.data() + 257, "ustar", 6);
      memcpy(h.data() + 263, "00", 2);
      memcpy(h.data() + 345, e.prefix.data(), std::min<size_t>(e.prefix.size(), 155));
    }
    memset(h.data() + 148, ' ', 8);
    unsigned sum = 0;
    for (uint8_t c : h) sum += c;
    char buf[16];
    snprintf(buf, sizeof(buf), "%06o", sum);
    memcpy(h.data() + 148, buf, 6);
    h[154] = 0;
    h[155] = ' ';
    return h;
  }

  std::vector<uint8_t> build() const {
    std::vector<uint8_t> out;
    for (const Entry& e : entries) {
      const auto h = header(e, ustar);
      out.insert(out.end(), h.begin(), h.end());
      out.insert(out.end(), e.data.begin(), e.data.end());
      out.resize((out.size() + 511) / 512 * 512, 0);
    }
    out.resize(out.size() + 1024, 0);
    return out;
  }
};

}  // namespace test
