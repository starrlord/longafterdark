// InstallShield 2 compressed libraries ("Z" libraries: IMAGES.1 + IMAGES.2,
// MODULES.LIB, AD_MODS.1 + AD_MODS.2) — what the installers of Marvel Comics
// Screen Posters (1993) and Snoopy's Screen Savers (1994) unpack. The format
// is research/win/pkg/installshield/SURVEY_REPORT.md; this reader was written
// from that public description and the survey's Python reference
// (tools/is3z.py, tools/dclexplode.py), not from any third-party decoder.
//
// A library is one file, or a split set: every volume repeats a 255-byte
// header and, at its end, the directory and file tables; the members'
// compressed data runs back to back from byte 255 in table order, across the
// volumes, and exactly one member crosses each boundary between two
// consecutive volumes (its head ends one volume, its tail opens the next).
// The volumes are held in memory (the largest is 1.1 MB). Only what the two
// releases (and InstallShield's own ICOMP 3.00) write is accepted, and every
// field is checked: the signature, the header's fixed and zero bytes, valid
// DOS dates, the tables exactly where the header says and ending the file,
// every size and count the header and the tables record, entries of exactly
// their sizes with NUL-terminated names that pass the importer's name rules
// (names.h; code page 437 as UTF-8, as the DOS-era readers take them), no
// gap and no overlap between members, identical tables in every volume of a
// set, and a complete set. What neither release has is refused by name, never
// guessed at: a password, a stored (uncompressed) member, a named or second
// directory, a member spanning more than two volumes, a boundary between two
// members, PKWARE's coded-literal ("ASCII") mode.
//
// Every member is PKWARE Data Compression Library "implode" data: binary
// literals and a 1024-, 2048- or 4096-byte window (dictionary bits 4-6, the
// three levels of ICOMP; the releases use 6), three fixed Huffman codes. The
// decoder is strict: a copy never reaches before the first byte written, the
// output never passes the member's recorded size and equals it at the end
// code, the end code is the last thing in the data and the bits after it in
// its last byte are zero. Nothing in the format has a checksum: a damaged
// member that still decodes to its recorded size is caught only by the
// release's manifest (importer.cc), as with SZDD and KWAJ.
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

class IszError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

struct IszVolume {
  std::string name;                                  // "IMAGES.1" (messages, import.json `from`)
  std::shared_ptr<const std::vector<uint8_t>> data;  // the whole volume
};

// One piece of a member's compressed data: all of it, or the part stored in
// one volume of a split set.
struct IszSegment {
  size_t volume = 0;  // index into the library's volumes() (set order)
  size_t offset = 0;  // of its bytes in that volume
  uint32_t csize = 0;
};

struct IszMember {
  std::string name;                  // a bare name as stored (code page 437, as UTF-8), in the one unnamed directory
  uint32_t size = 0;                 // uncompressed: the recorded size, exactly what extract() produces
  uint32_t csize = 0;                // compressed, every segment together
  uint32_t dos_datetime = 0;         // DOS date << 16 | time (local time)
  uint32_t attributes = 0;           // DOS attributes: 0x01 read-only, 0x02 hidden, 0x04 system, 0x20 archive
  std::vector<IszSegment> segments;  // in stream order: one, or two for the member crossing into the next volume
  std::string volumes;               // "IMAGES.1+IMAGES.2": the volumes its segments are in
};

class IszLibrary {
 public:
  // One unsplit library, or every volume of one split set in any order.
  // Parses and checks every header, both tables of every volume and the
  // placement of every member's data. Throws IszError.
  explicit IszLibrary(std::vector<IszVolume> volumes);

  const std::vector<IszVolume>& volumes() const { return volumes_; }  // set order: volume 1 first
  const std::vector<IszMember>& members() const { return members_; }  // table order
  const IszMember* find(std::string_view name) const;                 // case-insensitive

  // Streams the member's bytes to `sink` in chunks of at most 64 KiB,
  // decoding its segments as one stream: exactly its recorded size, or an
  // IszError ("<volumes>!<member>: ...") and never a byte past that size.
  void extract(const IszMember& m, const std::function<void(const uint8_t*, size_t)>& sink) const;

 private:
  std::vector<IszVolume> volumes_;
  std::vector<IszMember> members_;
};

// What a volume's 255-byte header says, for identification (a recipe
// collecting the volumes of a set before it builds the library).
struct IszHeader {
  bool split = false;         // one volume of a split set
  unsigned volume = 0;        // this volume's number, 1-based (0 when not split)
  unsigned volumes = 0;       // how many volumes the set has (volume 1 only; 0 elsewhere)
  unsigned files = 0;         // members of the whole library
  uint32_t dos_datetime = 0;  // the library's, DOS date << 16 | time
  uint32_t library_size = 0;  // the library before it was split (an unsplit one's file size)
  uint32_t total_size = 0;    // the members' uncompressed sizes added up
};

// The header alone (the first 255 bytes of `data`; nothing past them is
// read): the signature and every rule the header can be held to on its own.
// Throws IszError when `data` does not start with a valid header.
IszHeader isz_header(std::span<const uint8_t> data, std::string_view volume_name);

// The name of volume `n` of the set whose volume 1 is `first`: the numeric
// extension replaced ("IMAGES.1", 2 -> "IMAGES.2"; case kept); nullopt when
// `first` has no numeric extension.
std::optional<std::string> isz_volume_name(std::string_view first, unsigned n);

namespace isz_detail {  // exposed for tests/test_isz.cc

// One PKWARE DCL implode stream (its 2-byte header included) to exactly
// `size` bytes, in chunks of at most 64 KiB. Throws IszError without a
// volume or member name (the caller adds it); no byte past `size` is ever
// produced.
void explode(std::span<const uint8_t> in, uint32_t size, const std::function<void(const uint8_t*, size_t)>& sink);

}  // namespace isz_detail

}  // namespace adw::import
