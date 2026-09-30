// The test and repair of UTF-8 (internal header): the importer's records
// (import.json, written with minijson.h's json_escape) and the names it
// reads from discs and archives are UTF-8 whatever bytes they came from.
//
// Standard C++ only, no Win32: winutil.h includes it for the importer, and
// minijson.cc, which the Linux player (scr/linux) compiles with g++ too,
// includes it directly.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace adw::import {

// The length of the well-formed UTF-8 sequence that starts at s[i], i <
// s.size() (RFC 3629: no overlong form, no UTF-16 surrogate, nothing past
// U+10FFFF), or 0 when none does there (a continuation byte, C0, C1, F5-FF,
// a sequence cut short, also by the end of `s`). Windows' own decoder
// (MB_ERR_INVALID_CHARS) draws the same line.
inline size_t utf8_sequence_length(std::string_view s, size_t i) {
  const auto at = [&](size_t k) { return static_cast<unsigned char>(s[k]); };
  const unsigned char c = at(i);
  if (c < 0x80) return 1;
  size_t n = 0;
  unsigned char lo = 0x80, hi = 0xBF;  // the second byte's range
  if (c >= 0xC2 && c <= 0xDF) {
    n = 2;
  } else if (c >= 0xE0 && c <= 0xEF) {
    n = 3;
    if (c == 0xE0) lo = 0xA0;       // below U+0800: overlong
    if (c == 0xED) hi = 0x9F;       // U+D800-DFFF: surrogates
  } else if (c >= 0xF0 && c <= 0xF4) {
    n = 4;
    if (c == 0xF0) lo = 0x90;       // below U+10000: overlong
    if (c == 0xF4) hi = 0x8F;       // past U+10FFFF
  } else {
    return 0;
  }
  if (s.size() - i < n || at(i + 1) < lo || at(i + 1) > hi) return 0;
  for (size_t k = 2; k < n; k++)
    if (at(i + k) < 0x80 || at(i + k) > 0xBF) return 0;
  return n;
}

// Whether every byte of `s` belongs to a well-formed UTF-8 sequence.
inline bool is_utf8(std::string_view s) {
  for (size_t i = 0, n = 0; i < s.size(); i += n)
    if (!(n = utf8_sequence_length(s, i))) return false;
  return true;
}

// `s` as well-formed UTF-8: every byte that starts no well-formed sequence
// becomes U+FFFD, the replacement character; the rest is kept as it is.
inline std::string to_valid_utf8(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    const size_t n = utf8_sequence_length(s, i);
    if (n) {
      out.append(s.substr(i, n));
      i += n;
    } else {
      out += "\xEF\xBF\xBD";
      i++;
    }
  }
  return out;
}

}  // namespace adw::import
