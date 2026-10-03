#include "tar.h"

#include <windows.h>

#include <algorithm>

#include "winutil.h"

namespace adw::import {

namespace {

constexpr size_t kBlock = 512;
// No real tar of the corpus has more than a few hundred entries; a walk this
// long is a crafted file.
constexpr size_t kMaxHeaders = 1u << 20;

[[noreturn]] void bad(const std::string& where, const std::string& what) {
  throw TarError(where + ": " + what + " (not a tar, or a damaged one)");
}

Handle open_read(const std::filesystem::path& p, const std::string& where) {
  Handle h(CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN,
                       nullptr));
  if (!h.valid()) throw TarError("cannot read " + where + ": " + win_error_string(GetLastError()));
  return h;
}

void read_at(HANDLE h, uint64_t offset, uint8_t* out, size_t n, const std::string& where) {
  LARGE_INTEGER at{};
  at.QuadPart = int64_t(offset);
  if (!SetFilePointerEx(h, at, nullptr, FILE_BEGIN)) throw TarError("cannot read " + where);
  size_t have = 0;
  while (have < n) {
    DWORD got = 0;
    if (!ReadFile(h, out + have, DWORD(std::min<size_t>(n - have, 1 << 20)), &got, nullptr))
      throw TarError("read error on " + where + ": " + win_error_string(GetLastError()));
    if (!got) throw TarError(where + " ends inside an entry");
    have += got;
  }
}

// An octal field: digits, optionally led by spaces and ended by a NUL or a
// space. nullopt for anything else (GNU's base-256 sizes among them: no
// member of the corpus is that large).
std::optional<uint64_t> octal(const uint8_t* f, size_t n) {
  size_t i = 0;
  while (i < n && f[i] == ' ') i++;
  uint64_t v = 0;
  size_t digits = 0;
  for (; i < n && f[i] >= '0' && f[i] <= '7'; i++, digits++) {
    if (v >> 60) return std::nullopt;
    v = v * 8 + uint64_t(f[i] - '0');
  }
  if (!digits) return std::nullopt;
  for (; i < n; i++)
    if (f[i] != 0 && f[i] != ' ') return std::nullopt;
  return v;
}

std::string field(const uint8_t* f, size_t n) {
  size_t len = 0;
  while (len < n && f[len]) len++;
  return std::string(reinterpret_cast<const char*>(f), len);
}

}  // namespace

std::vector<std::optional<TarEntry>> find_tar_entries(const std::filesystem::path& tar,
                                                      const std::vector<std::string>& names,
                                                      const std::string& where) {
  std::vector<std::optional<TarEntry>> out(names.size());
  Handle h = open_read(tar, where);
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(h.get(), &size)) throw TarError("cannot read " + where);
  const uint64_t total = uint64_t(size.QuadPart);
  size_t left = names.size();
  uint64_t at = 0;
  bool zero_before = false;
  uint8_t b[kBlock];
  for (size_t n = 0; left && at + kBlock <= total; n++) {
    if (n >= kMaxHeaders) bad(where, "more than " + std::to_string(kMaxHeaders) + " entries");
    read_at(h.get(), at, b, kBlock, where);
    if (std::all_of(b, b + kBlock, [](uint8_t c) { return c == 0; })) {
      if (zero_before) break;  // the end of the archive
      zero_before = true;
      at += kBlock;
      continue;
    }
    zero_before = false;
    // The checksum: every byte of the header, its own field read as spaces.
    const auto sum = octal(b + 148, 8);
    uint64_t want = 0;
    for (size_t i = 0; i < kBlock; i++) want += (i >= 148 && i < 156) ? ' ' : b[i];
    if (!sum || *sum != want) bad(where, "a header at byte " + std::to_string(at) + " fails its checksum");
    const auto len = octal(b + 124, 12);
    if (!len) bad(where, "a header at byte " + std::to_string(at) + " has no size");
    const uint64_t data = at + kBlock;
    if (*len > total - std::min(total, data)) bad(where, "an entry at byte " + std::to_string(at) + " runs past its end");
    // ustar: a prefix joined to the name with '/'.
    std::string name = field(b, 100);
    if (std::equal(b + 257, b + 262, reinterpret_cast<const uint8_t*>("ustar"))) {
      const std::string prefix = field(b + 345, 155);
      if (!prefix.empty()) name = prefix + "/" + name;
    }
    const char type = char(b[156]);
    if (type == '0' || type == '\0' || type == '7') {  // a regular file
      for (size_t i = 0; i < names.size(); i++)
        if (!out[i] && names[i] == name) {
          out[i] = TarEntry{name, data, *len};
          left--;
        }
    }
    at = data + (*len + kBlock - 1) / kBlock * kBlock;
  }
  return out;
}

void read_tar_entry(const std::filesystem::path& tar, const TarEntry& e,
                    const std::function<void(const uint8_t*, size_t)>& sink, const std::string& where) {
  Handle h = open_read(tar, where);
  std::vector<uint8_t> buf(1 << 20);
  for (uint64_t done = 0; done < e.size;) {
    const size_t n = size_t(std::min<uint64_t>(buf.size(), e.size - done));
    read_at(h.get(), e.offset + done, buf.data(), n, where);
    sink(buf.data(), n);
    done += n;
  }
}

}  // namespace adw::import
