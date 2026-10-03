#include "szdd.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#include <vector>

namespace adw::import {

namespace {

constexpr uint8_t kMagic[8] = {'S', 'Z', 'D', 'D', 0x88, 0xF0, 0x27, 0x33};
constexpr size_t kHeader = 14;

uint32_t le32(const uint8_t* p) { return uint32_t(p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24); }

// Whole 8-byte records "DLL " (in any case: The Flintstones' June build's
// DIBDLL.DLL ends "dll 0601") + four ASCII digits, and nothing else: the
// version stamps Delrina's Intermission Installer reads after the data.
bool version_stamps(std::span<const uint8_t> rest) {
  if (rest.empty() || rest.size() % 8) return false;
  for (size_t i = 0; i < rest.size(); i += 8) {
    if ((rest[i] | 0x20) != 'd' || (rest[i + 1] | 0x20) != 'l' || (rest[i + 2] | 0x20) != 'l' || rest[i + 3] != ' ')
      return false;
    for (size_t k = 4; k < 8; k++)
      if (rest[i + k] < '0' || rest[i + k] > '9') return false;
  }
  return true;
}

}  // namespace

SzddHeader szdd_header(std::span<const uint8_t> f, std::string_view name) {
  const std::string n(name);
  static constexpr uint8_t kKwaj[4] = {'K', 'W', 'A', 'J'};
  if (f.size() >= 4 && std::equal(kKwaj, kKwaj + 4, f.begin())) throw SzddError(n + ": a KWAJ file, not SZDD");
  const size_t m = std::min<size_t>(f.size(), sizeof(kMagic));
  if (f.size() < 4 || !std::equal(f.begin(), f.begin() + m, kMagic)) throw SzddError(n + ": not an SZDD file");
  if (f.size() < kHeader) throw SzddError(n + ": the SZDD header is cut short");
  if (f[8] != 'A') {
    char mode[16];
    snprintf(mode, sizeof(mode), f[8] >= 0x20 && f[8] < 0x7F ? "'%c'" : "0x%02X", f[8]);
    throw SzddError(n + ": SZDD mode " + mode + " is not supported");
  }
  SzddHeader h;
  h.missing = char(f[9]);
  h.size = le32(&f[10]);
  return h;
}

void szdd_expand(std::span<const uint8_t> f, std::string_view name,
                 const std::function<void(const uint8_t*, size_t)>& sink) {
  const SzddHeader h = szdd_header(f, name);
  const std::string n(name);
  constexpr size_t kChunk = 64 * 1024;
  // COMPRESS's window: 4096 spaces, the first byte written at 0xFF0. Matches
  // may read spaces no byte of the file ever put there (the disc's
  // STRESS.DL_ does, 24 times), and bytes the match itself is writing.
  std::array<uint8_t, 4096> win;
  win.fill(0x20);
  size_t wpos = 0xFF0;
  std::vector<uint8_t> out;
  out.reserve(std::min<size_t>(kChunk, h.size));
  auto put = [&](uint8_t b) {
    out.push_back(b);
    win[wpos] = b;
    wpos = (wpos + 1) & 0xFFF;
    if (out.size() == kChunk) {
      sink(out.data(), out.size());
      out.clear();
    }
  };
  const auto early = [&]() { return SzddError(n + ": the compressed data ends early"); };
  uint64_t produced = 0;
  size_t i = kHeader;
  while (produced < h.size) {
    if (i >= f.size()) throw early();
    const uint8_t flags = f[i++];
    for (int bit = 0; bit < 8 && produced < h.size; bit++) {
      if (flags & (1u << bit)) {
        if (i >= f.size()) throw early();
        put(f[i++]);
        produced++;
        continue;
      }
      if (f.size() - i < 2) throw early();
      const uint32_t pos = f[i] | uint32_t(f[i + 1] & 0xF0) << 4;
      const uint32_t len = (f[i + 1] & 0x0F) + 3u;
      i += 2;
      if (len > h.size - produced) throw SzddError(n + ": a match passes the expanded size");
      for (uint32_t k = 0; k < len; k++) put(win[(pos + k) & 0xFFF]);
      produced += len;
    }
  }
  if (i != f.size() && !version_stamps(f.subspan(i)))
    throw SzddError(n + ": " + std::to_string(f.size() - i) + " byte(s) left after the compressed data");
  if (!out.empty()) sink(out.data(), out.size());
}

}  // namespace adw::import
