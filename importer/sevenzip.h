// 7z archives (7-Zip's own format) — how the user's copy of Screen Antics:
// Johnny Castaway came (a 7z of its floppy image), and a form any release's
// floppy images or install files may come in, taken wherever a ZIP is
// (PACKAGES.md §8.10). Written from the format's public descriptions, which
// their author, Igor Pavlov, placed in the public domain: 7zFormat.txt (the
// container) and Methods.txt (the method ids), in 7-Zip's DOC folder, and
// lzma-specification.txt (the LZMA decoder), in the LZMA SDK; LZMA2's chunk
// framing as the LZMA SDK describes it. No third-party code is copied.
//
// The archive is held in memory (source.cc reads at most 256 MB). The
// signature header and the header are checked whole before anything is
// believed: the signature, the start header's CRC-32, the header inside the
// file and its CRC-32, and every count, size and offset against the bytes
// that are really there; nothing is allocated from a size that was not
// checked first. The header may be plain or itself LZMA-packed
// (kEncodedHeader, 7-Zip's default). Only what a 7-Zip of the last twenty
// years writes for an archive of plain files is accepted — pack info,
// blocks ("folders") of one coder each, LZMA, LZMA2 or Copy, substreams
// (several files in one solid block), UTF-16 names, empty files and folders,
// attributes and times — and everything else is refused by name, never
// guessed at: an encrypted archive or header (7zAES), a filter (BCJ, BCJ2,
// ARM64, Delta, ...) or any other chain of coders, another method (PPMd,
// BZip2, Deflate, ...), the first volume of a split archive (.7z.001), anti
// items, a block larger than kMaxBlockBytes. Every name passes the
// importer's name rules (names.h) component by component, and two names
// Windows would take for one are refused.
//
// Each block is decoded into memory as far as the reads need, one block at
// a time (the last one read stays until another is): the members of a solid
// block are slices of it, read in any order without decoding it twice. The
// LZMA decoder is strict: the range coder's first byte is zero, a distance
// never reaches before the first byte (or LZMA2's last dictionary reset) or
// past the dictionary, a match never runs past the block's recorded size,
// the stream ends exactly at that size (with or without LZMA's end marker)
// with the range coder flushed and every packed byte consumed; LZMA2 chunks
// follow the reset order its decoder requires, each LZMA chunk ends exactly
// at its packed and unpacked sizes, and the end byte ends the stream. Every
// file's CRC-32 and every block's are checked whenever the archive records
// them (7-Zip always does).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace adw::import {

class SevenZipError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// The six bytes every 7z archive starts with: '7' 'z' BC AF 27 1C.
bool is_7z_signature(std::span<const uint8_t> head);

struct SevenZipMember {
  std::string name;                // UTF-8 (UTF-16 in the archive), '/'-separated, no trailing '/'
  uint64_t size = 0;
  bool directory = false;          // a folder's own entry, which holds no data
  std::optional<uint32_t> crc;     // CRC-32 of the data, when the archive records it
  std::optional<uint64_t> mtime;   // FILETIME (100 ns since 1601, UTC), when recorded
  uint32_t attributes = 0;         // Windows attributes, when recorded
  size_t block = SIZE_MAX;         // the block holding its data; SIZE_MAX for an empty file or a folder
  uint64_t offset = 0;             // where its data starts in the block's unpacked bytes
};

class SevenZipArchive {
 public:
  // Every block is decoded into memory: at most this large (a solid block
  // of several files too).
  static constexpr uint64_t kMaxBlockBytes = 256ull << 20;
  // Entries (files and folders), blocks and packed streams, each.
  static constexpr uint64_t kMaxEntries = 65536;
  // The header, packed or not.
  static constexpr uint64_t kMaxHeaderBytes = 16ull << 20;

  // Parses the signature header and the header (decoding it when it is
  // packed). `name` is used in messages only. Throws SevenZipError.
  SevenZipArchive(std::shared_ptr<const std::vector<uint8_t>> data, std::string name);
  ~SevenZipArchive();

  const std::string& name() const { return name_; }
  // In the archive's order.
  const std::vector<SevenZipMember>& members() const { return members_; }
  const SevenZipMember* find(std::string_view member) const;  // case-insensitive, '/' or '\'
  // "LZMA", "LZMA2" or "Copy" (the member's block), "" for no data.
  std::string method(const SevenZipMember& m) const;
  // Whether the member's block holds other files too.
  bool solid(const SevenZipMember& m) const;

  // Streams the member's bytes to `sink` in chunks of at most 64 KiB (a
  // copy: the sink may read the archive again). Throws SevenZipError on
  // damaged data, a size or a CRC-32 mismatch. A sink that throws stops the
  // decoding there; what was decoded is kept for the next read.
  void extract(const SevenZipMember& m, const std::function<void(const uint8_t*, size_t)>& sink) const;

  struct Block;
  struct Decoder;

 private:
  std::shared_ptr<const std::vector<uint8_t>> data_;
  std::string name_;
  std::vector<SevenZipMember> members_;
  std::vector<Block> blocks_;
  mutable std::mutex mu_;
  mutable std::shared_ptr<Decoder> cache_;  // the block read last, as far as it was decoded
};

}  // namespace adw::import
