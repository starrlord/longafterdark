// PKZIP 2.0 archives with traditional PKWARE encryption ("ZipCrypto") —
// what the After Dark 3.x InstallShield installers unpack (PACKAGES.md §8.3).
//
// The archive is held in memory (the largest in the corpus is under 1 MB).
// The central directory is authoritative for names, sizes, CRCs and flags;
// each member's data is found through its local header. Only what the
// corpus uses is accepted — a single disk, no ZIP64, no strong encryption,
// stored (0) or deflated (8) members with bare file names — and everything
// else is refused as a damaged or foreign source, never guessed at. A ZIP
// source may instead keep each install disk's files in a folder of its own
// (ZipNames::disk_folders; the Internet Archive's copies of ScreamSavers,
// Marvel Comics Screen Posters and Snoopy's Screen Savers): one level of
// DISK<n> folders, never deeper; or hold its files in folders of any names
// (ZipNames::paths, where the source's rule says what it may hold: one
// folder of a release's files, or floppy images in any folder; source.h);
// the installers' own archives keep the bare names. Member names are UTF-8
// from the constructor on: as stored when the archive flags them UTF-8
// (general-purpose bit 11; a byte that is not UTF-8 becomes U+FFFD) or they
// are UTF-8, else decoded from code page 437
// — so two names are one only when Windows would take them for one file.
// Extraction streams: decrypt, raw-inflate through zlib, and check the size
// and CRC-32 of every member.
//
// The password is never stored anywhere: derive_password() recovers it from
// the installer script at import time (§8.4) and it lives only in memory.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace adw::import {

class ZipError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// The member names an archive may hold. `bare`: file names only (every
// installer's archive). `disk_folders`: a bare name, or a bare name in a
// folder DISK<n> ("DISK2/SETUP.PKG"), or such a folder's own entry
// ("DISK2/"); any other folder, and anything deeper, is refused. `paths`: any
// relative path of usable names ("Intermission 4.0/ITM4W-D1.IMA"), a folder's
// own entry ending in '/' — what a ZIP source holds, whose layout is the
// source's rule (source.h), not the archive's.
enum class ZipNames { bare, disk_folders, paths };

// n for a folder named DISK<n>, n = 1..99 written without leading zeros, in
// any case ("DISK1", "Disk2", "disk12"); 0 for every other name ("DISK0",
// "DISK01", "DISK 1", "DISK100").
unsigned disk_folder_number(std::string_view name);

struct ZipMember {
  std::string name;  // UTF-8 (a bare 8.3 name in the corpus, or "DISK2/SETUP.PKG"): as stored, or from code page 437
  uint16_t version_made = 0, flags = 0, method = 0;
  uint16_t mod_time = 0, mod_date = 0;  // DOS, local time
  uint32_t crc = 0;
  uint32_t csize = 0, usize = 0;
  uint32_t local_offset = 0;
  unsigned disk = 0;       // disk_folders: the n of the DISK<n> folder it is in; 0 at the archive's root
  // disk_folders: a DISK<n> folder's own entry ("DISK2/"); paths: any
  // folder's ("Intermission 4.0/"). It holds no data.
  bool directory = false;
  bool encrypted() const { return (flags & 1) != 0; }
  // The name without its DISK<n> folder ("SETUP.PKG"); "" for a folder's own entry.
  std::string file_name() const { return disk ? name.substr(name.find('/') + 1) : name; }
};

class ZipArchive {
 public:
  // Parses the end record and the central directory. `name` is used in
  // messages only. Throws ZipError.
  ZipArchive(std::shared_ptr<const std::vector<uint8_t>> data, std::string name, ZipNames names = ZipNames::bare);

  const std::string& name() const { return name_; }
  const std::vector<ZipMember>& members() const { return members_; }
  const ZipMember* find(std::string_view member) const;  // case-insensitive

  // Streams the member's plain bytes to `sink` in chunks of at most 64 KiB,
  // decrypting with `password` when the member is encrypted. Throws ZipError
  // on a wrong password, damaged data, a size or CRC-32 mismatch.
  void extract(const ZipMember& m, std::string_view password,
               const std::function<void(const uint8_t*, size_t)>& sink) const;

  // The 12-byte encryption header's check byte only (1 in 256 wrong
  // passwords pass it). False for an unencrypted member.
  bool header_check(const ZipMember& m, std::string_view password) const;
  // The header check, then a full extraction with the CRC-32 check.
  bool password_opens(const ZipMember& m, std::string_view password) const;

 private:
  std::shared_ptr<const std::vector<uint8_t>> data_;
  std::string name_;
  std::vector<ZipMember> members_;

  // Offset and length of the member's stored bytes (encryption header included).
  std::pair<size_t, size_t> payload(const ZipMember& m) const;
};

// The candidate passwords in an InstallShield script (PACKAGES.md §8.4), in
// trial order: first what follows "Cannot initialize for unzip!" (the script
// hands the next string to DUNZIP.DLL), then everything else in file order,
// without duplicates. A candidate is a string of 4..32 printable ASCII bytes,
// either length-prefixed (the script's own string encoding: a 16-bit length,
// then the bytes) or a maximal run of printable bytes.
std::vector<std::string> password_candidates(std::span<const uint8_t> script);

// The first candidate that opens the smallest encrypted member across `zips`
// (header check, then full decrypt + inflate + CRC-32), and also opens the
// smallest encrypted member of a different archive when there is one.
// "" when no member is encrypted; nullopt when no candidate works.
std::optional<std::string> derive_password(std::span<const uint8_t> script,
                                           const std::vector<const ZipArchive*>& zips);

}  // namespace adw::import
