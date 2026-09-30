// Synthetic PKZIP 2.0 archives for the importer tests: stored or deflated
// members (zlib), optionally encrypted with traditional PKWARE encryption
// ("ZipCrypto") under a test-only password, laid out as the AD 3.x
// installers' archives are (local header, data, central directory, end
// record; bare 8.3 names, made by 2.0/FAT, flag bits 0 and 1, bit 3 clear),
// and ZIPs of install disks kept apart in DISK<n> folders. Knobs let a test
// produce the damaged and foreign shapes the reader must refuse. No After
// Dark bytes, and never the real archive password.
#pragma once

#include <zlib.h>

#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace test {

// The password the synthetic installers use. Deliberately not the real one.
inline constexpr char kTestZipPassword[] = "unit-test-pw7";

struct ZipSpec {
  std::string name;
  std::vector<uint8_t> data;
  bool deflate = true;
  bool encrypt = true;
  uint16_t dos_time = 0x7A00;  // 15:16:00
  uint16_t dos_date = 0x1EF1;  // 1995-07-17
  uint16_t method_override = 0xFFFF;  // write this method id instead (unsupported-method tests)
  uint16_t extra_flags = 0;           // OR'ed into the general-purpose flags (bit 3, bit 6 tests)
};

namespace zipdetail {

inline uint32_t crc_step(uint32_t crc, uint8_t b) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    init = true;
  }
  return table[(crc ^ b) & 0xFF] ^ (crc >> 8);
}

struct Keys {
  uint32_t k0 = 0x12345678, k1 = 0x23456789, k2 = 0x34567890;
  explicit Keys(const std::string& pw) {
    for (char c : pw) update(uint8_t(c));
  }
  void update(uint8_t p) {
    k0 = crc_step(k0, p);
    k1 = (k1 + (k0 & 0xFF)) * 134775813u + 1;
    k2 = crc_step(k2, uint8_t(k1 >> 24));
  }
  uint8_t encrypt(uint8_t p) {
    uint16_t t = uint16_t(k2 | 2);
    uint8_t c = uint8_t(p ^ uint8_t((uint32_t(t) * (t ^ 1)) >> 8));
    update(p);
    return c;
  }
};

inline void put16(std::vector<uint8_t>& v, uint16_t x) {
  v.push_back(uint8_t(x));
  v.push_back(uint8_t(x >> 8));
}
inline void put32(std::vector<uint8_t>& v, uint32_t x) {
  put16(v, uint16_t(x));
  put16(v, uint16_t(x >> 16));
}

inline std::vector<uint8_t> deflate_raw(const std::vector<uint8_t>& in) {
  z_stream s{};
  deflateInit2(&s, 9, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
  std::vector<uint8_t> out(deflateBound(&s, uLong(in.size())) + 16);
  s.next_in = const_cast<Bytef*>(in.data());
  s.avail_in = uInt(in.size());
  s.next_out = out.data();
  s.avail_out = uInt(out.size());
  deflate(&s, Z_FINISH);
  out.resize(s.total_out);
  deflateEnd(&s);
  return out;
}

}  // namespace zipdetail

class ZipBuilder {
 public:
  std::string password = kTestZipPassword;
  std::vector<ZipSpec> members;
  // Damage knobs for the end record.
  bool zip64_marker = false;  // entries = 0xFFFF
  uint16_t disk_number = 0;   // nonzero = "multi-disk"

  ZipBuilder& add(const std::string& name, std::vector<uint8_t> data, bool deflate = true, bool encrypt = true) {
    ZipSpec s;
    s.name = name;
    s.data = std::move(data);
    s.deflate = deflate;
    s.encrypt = encrypt;
    members.push_back(std::move(s));
    return *this;
  }

  // Offsets of each member's central-directory record (filled by build()).
  std::vector<size_t> central_offsets;

  std::vector<uint8_t> build() {
    using namespace zipdetail;
    std::vector<uint8_t> out, central;
    central_offsets.clear();
    uint32_t seed = 0x9E3779B9;
    for (const ZipSpec& m : members) {
      uint32_t crc = uint32_t(crc32(0, m.data.data(), uInt(m.data.size())));
      std::vector<uint8_t> payload = m.deflate ? deflate_raw(m.data) : m.data;
      uint16_t flags = uint16_t((m.encrypt ? 1 : 0) | (m.deflate ? 2 : 0) | m.extra_flags);
      if (m.encrypt) {
        Keys k(password);
        std::vector<uint8_t> enc;
        for (int i = 0; i < 11; i++) {
          seed = seed * 1103515245u + 12345u;
          enc.push_back(k.encrypt(uint8_t(seed >> 16)));
        }
        enc.push_back(k.encrypt((m.extra_flags & 8) ? uint8_t(m.dos_time >> 8) : uint8_t(crc >> 24)));
        for (uint8_t b : payload) enc.push_back(k.encrypt(b));
        payload = std::move(enc);
      }
      uint16_t method = m.method_override != 0xFFFF ? m.method_override : (m.deflate ? 8 : 0);
      uint32_t local = uint32_t(out.size());
      put32(out, 0x04034b50);
      put16(out, 20);
      put16(out, flags);
      put16(out, method);
      put16(out, m.dos_time);
      put16(out, m.dos_date);
      put32(out, crc);
      put32(out, uint32_t(payload.size()));
      put32(out, uint32_t(m.data.size()));
      put16(out, uint16_t(m.name.size()));
      put16(out, 0);
      out.insert(out.end(), m.name.begin(), m.name.end());
      out.insert(out.end(), payload.begin(), payload.end());

      central_offsets.push_back(central.size());
      put32(central, 0x02014b50);
      put16(central, 20);  // made by 2.0, MS-DOS
      put16(central, 20);
      put16(central, flags);
      put16(central, method);
      put16(central, m.dos_time);
      put16(central, m.dos_date);
      put32(central, crc);
      put32(central, uint32_t(payload.size()));
      put32(central, uint32_t(m.data.size()));
      put16(central, uint16_t(m.name.size()));
      put16(central, 0);
      put16(central, 0);
      put16(central, 0);
      put16(central, 0);
      put32(central, 0x20);
      put32(central, local);
      central.insert(central.end(), m.name.begin(), m.name.end());
    }
    uint32_t cd_off = uint32_t(out.size());
    for (size_t& o : central_offsets) o += cd_off;
    out.insert(out.end(), central.begin(), central.end());
    put32(out, 0x06054b50);
    put16(out, disk_number);
    put16(out, disk_number);
    put16(out, zip64_marker ? 0xFFFF : uint16_t(members.size()));
    put16(out, zip64_marker ? 0xFFFF : uint16_t(members.size()));
    put32(out, uint32_t(central.size()));
    put32(out, cd_off);
    put16(out, 0);
    return out;
  }
};

// A one-call archive: every member encrypted and deflated under `password`.
inline std::vector<uint8_t> zip_of(const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files,
                                   const std::string& password = kTestZipPassword) {
  ZipBuilder b;
  b.password = password;
  for (const auto& [n, d] : files) b.add(n, d);
  return b.build();
}

// A ZIP of install disks kept apart, as the Internet Archive's copies of
// ScreamSavers, Marvel Comics Screen Posters and Snoopy's Screen Savers are:
// disk n's files (bare names) as "<spell><n>/<name>", deflated, unencrypted,
// each folder's own entry after its files (`folder_entries`).
inline std::vector<uint8_t> zip_of_disks(const std::map<int, std::map<std::string, std::vector<uint8_t>>>& disks,
                                         const std::string& spell = "Disk", bool folder_entries = true) {
  ZipBuilder b;
  b.password = "";
  for (const auto& [n, files] : disks) {
    const std::string folder = spell + std::to_string(n) + "/";
    for (const auto& [name, data] : files) b.add(folder + name, data, /*deflate=*/true, /*encrypt=*/false);
    if (folder_entries) b.add(folder, {}, /*deflate=*/false, /*encrypt=*/false);
  }
  return b.build();
}

}  // namespace test
