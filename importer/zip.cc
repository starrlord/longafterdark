#include "zip.h"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <set>

#include "names.h"

namespace adw::import {

namespace {

constexpr uint32_t kLocalSig = 0x04034b50, kCentralSig = 0x02014b50, kEndSig = 0x06054b50;

uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t le32(const uint8_t* p) { return uint32_t(p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24); }

// CRC-32 (reflected 0xEDB88320) as ZipCrypto steps it: one byte, no pre- or
// post-inversion.
const std::array<uint32_t, 256>& crc_table() {
  static const std::array<uint32_t, 256> t = [] {
    std::array<uint32_t, 256> a{};
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      a[i] = c;
    }
    return a;
  }();
  return t;
}

inline uint32_t crc_step(uint32_t crc, uint8_t b) { return crc_table()[(crc ^ b) & 0xFF] ^ (crc >> 8); }

// Traditional PKWARE stream cipher (APPNOTE 6.1).
struct ZipCrypto {
  uint32_t k0 = 0x12345678, k1 = 0x23456789, k2 = 0x34567890;
  explicit ZipCrypto(std::string_view password) {
    for (char c : password) update(uint8_t(c));
  }
  void update(uint8_t p) {
    k0 = crc_step(k0, p);
    k1 = (k1 + (k0 & 0xFF)) * 134775813u + 1;
    k2 = crc_step(k2, uint8_t(k1 >> 24));
  }
  uint8_t decrypt(uint8_t c) {
    uint16_t t = uint16_t(k2 | 2);
    uint8_t p = uint8_t(c ^ uint8_t((uint32_t(t) * (t ^ 1)) >> 8));
    update(p);
    return p;
  }
};

bool printable(uint8_t c) { return c >= 0x20 && c < 0x7F; }

}  // namespace

unsigned disk_folder_number(std::string_view name) {
  if (name.size() < 5 || name.size() > 6 || !iequals(name.substr(0, 4), "DISK")) return 0;
  unsigned n = 0;
  for (char c : name.substr(4)) {
    if (c < '0' || c > '9') return 0;
    n = n * 10 + unsigned(c - '0');
  }
  return name[4] == '0' ? 0 : n;  // "DISK0", "DISK01": no disk
}

ZipArchive::ZipArchive(std::shared_ptr<const std::vector<uint8_t>> data, std::string name, ZipNames names)
    : data_(std::move(data)), name_(std::move(name)) {
  const std::vector<uint8_t>& d = *data_;
  if (d.size() < 22) throw ZipError(name_ + ": too small to be a ZIP archive");
  // The end record sits in the last 22 + 65535 (comment) bytes.
  size_t lo = d.size() > 65557 ? d.size() - 65557 : 0;
  size_t end = SIZE_MAX;
  for (size_t p = d.size() - 22 + 1; p-- > lo;) {
    if (le32(&d[p]) == kEndSig && p + 22 + le16(&d[p + 20]) <= d.size()) {
      end = p;
      break;
    }
  }
  if (end == SIZE_MAX) throw ZipError(name_ + ": not a ZIP archive (no end-of-central-directory record)");
  const uint8_t* e = &d[end];
  uint16_t disk = le16(e + 4), cd_disk = le16(e + 6), here = le16(e + 8), total = le16(e + 10);
  uint32_t cd_size = le32(e + 12), cd_off = le32(e + 16);
  if (here == 0xFFFF || total == 0xFFFF || cd_size == 0xFFFFFFFF || cd_off == 0xFFFFFFFF)
    throw ZipError(name_ + ": ZIP64 archives are not supported");
  if (disk != 0 || cd_disk != 0 || here != total) throw ZipError(name_ + ": multi-disk archives are not supported");
  if (uint64_t(cd_off) + cd_size > end) throw ZipError(name_ + ": the central directory lies outside the archive");
  size_t p = cd_off;
  std::set<std::string> seen;
  for (uint16_t i = 0; i < total; i++) {
    if (p + 46 > size_t(cd_off) + cd_size || le32(&d[p]) != kCentralSig)
      throw ZipError(name_ + ": damaged central directory");
    const uint8_t* c = &d[p];
    ZipMember m;
    m.version_made = le16(c + 4);
    m.flags = le16(c + 8);
    m.method = le16(c + 10);
    m.mod_time = le16(c + 12);
    m.mod_date = le16(c + 14);
    m.crc = le32(c + 16);
    m.csize = le32(c + 20);
    m.usize = le32(c + 24);
    uint16_t nlen = le16(c + 28), xlen = le16(c + 30), clen = le16(c + 32), start_disk = le16(c + 34);
    m.local_offset = le32(c + 42);
    if (p + 46 + nlen + xlen + clen > size_t(cd_off) + cd_size) throw ZipError(name_ + ": damaged central directory");
    m.name.assign(reinterpret_cast<const char*>(c + 46), nlen);
    // The name as UTF-8, before anything reads it (zip.h). Bit 11 says it is
    // UTF-8 (APPNOTE 4.4.4): each byte that is not UTF-8 becomes U+FFFD.
    // Without the bit the specification says code page 437 (what
    // Explorer and 7-Zip write on an English Windows, their OEM code page),
    // but archivers elsewhere write UTF-8 without it: a name that is UTF-8
    // stays so, any other is code page 437.
    if (m.flags & 0x800) m.name = to_valid_utf8(m.name);
    else if (!is_utf8(m.name)) m.name = oem437_to_utf8(m.name);
    p += 46 + size_t(nlen) + xlen + clen;
    if (m.csize == 0xFFFFFFFF || m.usize == 0xFFFFFFFF || m.local_offset == 0xFFFFFFFF)
      throw ZipError(name_ + ": ZIP64 members are not supported");
    if (start_disk != 0) throw ZipError(name_ + ": multi-disk archives are not supported");
    if (m.flags & 0x40) throw ZipError(name_ + "!" + m.name + ": strong encryption is not supported");
    if (m.method != 0 && m.method != 8)
      throw ZipError(name_ + "!" + m.name + ": compression method " + std::to_string(m.method) + " is not supported");
    // The file's own name: the whole name, or (disk_folders) what follows a
    // DISK<n>/ folder, which must be a bare name too.
    std::string file = m.name;
    const size_t slash = m.name.find('/');
    if (names == ZipNames::disk_folders && slash != std::string::npos)
      m.disk = disk_folder_number(std::string_view(m.name).substr(0, slash));
    if (m.disk) {
      file = m.name.substr(slash + 1);
      m.directory = file.empty();
    }
    if (file.find_first_of("/\\:") != std::string::npos)
      throw ZipError(name_ + ": member \"" + m.name + "\" is not a bare file name" +
                     (names == ZipNames::disk_folders ? " or a file in a DISK<n> folder" : ""));
    if (m.directory) {
      // A folder's own entry is never read; one that holds data is no
      // folder entry at all.
      if (m.usize != 0) throw ZipError(name_ + ": member \"" + m.name + "\" is a folder entry that holds data");
    } else {
      try {
        check_component(file, m.disk ? name_ + "!" + m.name.substr(0, slash) : name_);
      } catch (const ImportError& ex) {
        throw ZipError(ex.what());
      }
    }
    // One name as Windows compares them (non-ASCII letters too; names.h).
    if (!seen.insert(name_key(m.name)).second) throw ZipError(name_ + ": two members are named " + m.name);
    if (m.encrypted() && m.csize < 12) throw ZipError(name_ + "!" + m.name + ": encrypted member shorter than its header");
    members_.push_back(std::move(m));
  }
}

const ZipMember* ZipArchive::find(std::string_view member) const {
  for (const ZipMember& m : members_)
    if (iequals(m.name, member)) return &m;
  return nullptr;
}

std::pair<size_t, size_t> ZipArchive::payload(const ZipMember& m) const {
  const std::vector<uint8_t>& d = *data_;
  size_t p = m.local_offset;
  if (p + 30 > d.size() || le32(&d[p]) != kLocalSig) throw ZipError(name_ + "!" + m.name + ": damaged local header");
  size_t start = p + 30 + le16(&d[p + 26]) + le16(&d[p + 28]);
  if (start > d.size() || d.size() - start < m.csize) throw ZipError(name_ + "!" + m.name + ": the archive is truncated");
  return {start, m.csize};
}

bool ZipArchive::header_check(const ZipMember& m, std::string_view password) const {
  if (!m.encrypted()) return false;
  auto [off, len] = payload(m);
  (void)len;
  ZipCrypto z(password);
  uint8_t last = 0;
  for (int i = 0; i < 12; i++) last = z.decrypt((*data_)[off + i]);
  uint8_t want = (m.flags & 0x08) ? uint8_t(m.mod_time >> 8) : uint8_t(m.crc >> 24);
  return last == want;
}

void ZipArchive::extract(const ZipMember& m, std::string_view password,
                         const std::function<void(const uint8_t*, size_t)>& sink) const {
  const std::string what = name_ + "!" + m.name;
  auto [off, len] = payload(m);
  const uint8_t* src = data_->data() + off;
  std::optional<ZipCrypto> z;
  if (m.encrypted()) {
    if (!header_check(m, password)) throw ZipError(what + ": wrong password");
    z.emplace(password);
    for (int i = 0; i < 12; i++) z->decrypt(src[i]);
    src += 12;
    len -= 12;
  }
  uLong crc = crc32(0, nullptr, 0);
  uint64_t out_total = 0;
  auto emit = [&](const uint8_t* p, size_t n) {
    if (!n) return;
    out_total += n;
    if (out_total > m.usize) throw ZipError(what + ": more data than the recorded size");
    crc = crc32(crc, p, uInt(n));
    sink(p, n);
  };
  constexpr size_t kChunk = 64 * 1024;
  std::vector<uint8_t> plain(kChunk), out(kChunk);
  if (m.method == 0) {
    for (size_t done = 0; done < len;) {
      size_t n = std::min(kChunk, len - done);
      for (size_t i = 0; i < n; i++) plain[i] = z ? z->decrypt(src[done + i]) : src[done + i];
      emit(plain.data(), n);
      done += n;
    }
  } else {
    z_stream s{};
    if (inflateInit2(&s, -15) != Z_OK) throw ZipError(what + ": cannot start inflate");
    struct Guard {
      z_stream* s;
      ~Guard() { inflateEnd(s); }
    } guard{&s};
    int rc = Z_OK;
    size_t done = 0;
    while (rc != Z_STREAM_END) {
      if (s.avail_in == 0 && done < len) {
        size_t n = std::min(kChunk, len - done);
        for (size_t i = 0; i < n; i++) plain[i] = z ? z->decrypt(src[done + i]) : src[done + i];
        done += n;
        s.next_in = plain.data();
        s.avail_in = uInt(n);
      }
      s.next_out = out.data();
      s.avail_out = uInt(out.size());
      rc = inflate(&s, Z_NO_FLUSH);
      // No progress with every input byte consumed: the stream is cut short.
      if (rc == Z_BUF_ERROR && s.avail_in == 0 && done >= len)
        throw ZipError(what + ": the compressed data ends early");
      if (rc != Z_OK && rc != Z_STREAM_END)
        throw ZipError(what + ": damaged compressed data (" + std::string(s.msg ? s.msg : "inflate error") + ")");
      emit(out.data(), out.size() - s.avail_out);
    }
  }
  if (out_total != m.usize)
    throw ZipError(what + ": " + std::to_string(out_total) + " bytes, the archive records " + std::to_string(m.usize));
  if (uint32_t(crc) != m.crc) throw ZipError(what + ": CRC-32 mismatch");
}

bool ZipArchive::password_opens(const ZipMember& m, std::string_view password) const {
  if (!header_check(m, password)) return false;
  try {
    extract(m, password, [](const uint8_t*, size_t) {});
    return true;
  } catch (const ZipError&) {
    return false;
  }
}

std::vector<std::string> password_candidates(std::span<const uint8_t> s) {
  struct Cand {
    size_t at;
    std::string text;
  };
  std::vector<Cand> all;
  // Length-prefixed strings (u16 length, then that many printable bytes).
  for (size_t i = 0; i + 2 <= s.size(); i++) {
    size_t n = size_t(s[i] | s[i + 1] << 8);
    if (n < 4 || n > 32 || i + 2 + n > s.size()) continue;
    bool ok = true;
    for (size_t k = 0; k < n && ok; k++) ok = printable(s[i + 2 + k]);
    if (ok) all.push_back({i + 2, std::string(reinterpret_cast<const char*>(&s[i + 2]), n)});
  }
  // Maximal runs of printable bytes.
  for (size_t i = 0; i < s.size();) {
    if (!printable(s[i])) {
      i++;
      continue;
    }
    size_t j = i;
    while (j < s.size() && printable(s[j])) j++;
    if (j - i >= 4 && j - i <= 32) all.push_back({i, std::string(reinterpret_cast<const char*>(&s[i]), j - i)});
    i = j;
  }
  std::stable_sort(all.begin(), all.end(), [](const Cand& a, const Cand& b) { return a.at < b.at; });
  std::vector<std::string> out;
  std::set<std::string> seen;
  // First: the first candidate after the message the script shows when the
  // unzip DLL fails to start — the next string it loads is the password.
  static const std::string_view kMarker = "Cannot initialize for unzip!";
  auto it = std::search(s.begin(), s.end(), kMarker.begin(), kMarker.end());
  if (it != s.end()) {
    size_t after = size_t(it - s.begin()) + kMarker.size();
    for (const Cand& c : all)
      if (c.at >= after) {
        out.push_back(c.text);
        seen.insert(c.text);
        break;
      }
  }
  for (const Cand& c : all)
    if (seen.insert(c.text).second) out.push_back(c.text);
  return out;
}

std::optional<std::string> derive_password(std::span<const uint8_t> script, const std::vector<const ZipArchive*>& zips) {
  // The check entries: the smallest encrypted member overall, and the
  // smallest encrypted member of any other archive.
  const ZipArchive* first_zip = nullptr;
  const ZipMember* first = nullptr;
  for (const ZipArchive* z : zips)
    for (const ZipMember& m : z->members())
      if (m.encrypted() && (!first || m.csize < first->csize)) first = &m, first_zip = z;
  if (!first) return std::string();
  const ZipArchive* second_zip = nullptr;
  const ZipMember* second = nullptr;
  for (const ZipArchive* z : zips) {
    if (z == first_zip) continue;
    for (const ZipMember& m : z->members())
      if (m.encrypted() && (!second || m.csize < second->csize)) second = &m, second_zip = z;
  }
  for (const std::string& c : password_candidates(script)) {
    if (!first_zip->password_opens(*first, c)) continue;
    if (second && !second_zip->password_opens(*second, c)) continue;
    return c;
  }
  return std::nullopt;
}

}  // namespace adw::import
