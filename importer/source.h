// SourceFs — one read-only view over every kind of source the importer
// takes (PACKAGES.md §3): an ISO-9660/Joliet image, a FAT12/16 floppy image,
// a flat ZIP of install files (the Internet Archive's Simpsons copies), the
// floppy images a ZIP holds (the Internet Archive's ZIP of Star Trek: The
// Screen Saver's two disks), a 7z of either (sevenzip.h: the user's copy of
// Johnny Castaway is a 7z of its floppy image), a host folder (a CD drive, a
// copy of a disc or floppies), or several images unioned into one tree
// (split floppies).
//
// Disk sets. A source whose root holds nothing but folders named DISK<n>
// (zip.h disk_folder_number: DISK1..DISK99, any case) is a release's install
// disks kept apart — the Internet Archive's ZIPs of ScreamSavers (DISK1-3),
// Marvel Comics Screen Posters and Snoopy's Screen Savers (Disk1-2), or a
// folder they were unzipped into — and is read as the union of those folders
// (union_of, in disk order): directories merge, and a name in two disks must
// be the same file (the same size and the same bytes), else the source is
// invalid — when the file is read, never when it is only listed: what no
// recipe reads (a BBS's notes, which differ from one disk's ZIP to the
// next) never stops an import. Any disks may be there (disk 2 alone is a
// source too; the recipe says whether the release is complete). A ZIP's
// disks are flat: its members are then "DISK<n>/<bare name>" and the
// folders' own entries, nothing deeper. A root that holds anything besides
// its DISK<n> folders — a file, another folder — is read as it is: a
// folder's or an image's DISK<n> folders are then ordinary folders, and a
// ZIP is refused, since a ZIP of install files holds its files at its root.
//
// A ZIP or 7z of install files may hold them, or only its DISK<n> folders,
// in one folder that holds everything instead (the Internet Archive's ZIP
// of On the Road Again's files, "Opus n Bill - On the Road Again/"): that
// folder is then read as its root, the same rule for both; any other
// folder, and anything deeper, is refused (a ZIP or 7z never nests the
// files otherwise). Floppy images in a ZIP or 7z may sit in any folder.
//
// Names are what the importer installs under: 8.3 upper case as the source
// lists them (an ISO entry's primary-volume name when its Joliet name could
// be paired with one; a folder entry's listed name, upper-cased — never the
// volume's generated "~1" alias). Lookups are case-insensitive and also
// accept an entry's alternative (Joliet) name. Every failure to read is an
// ImportError(source_invalid): a damaged or unreadable source.
#pragma once

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "status.h"

namespace adw::import {

using Sink = std::function<void(const uint8_t*, size_t)>;

struct SourceNode {
  std::string name;      // installed name (upper case); "" for a root
  std::string alt_name;  // another name the entry answers to (a Joliet long name), "" = none
  bool is_dir = false;
  uint64_t size = 0;
  std::optional<FILETIME> mtime;     // UTC; nothing when the source has none or it is unrepresentable
  std::shared_ptr<const void> impl;  // the reader's own handle for the entry
};

class SourceFs {
 public:
  virtual ~SourceFs() = default;
  virtual SourceNode root() const = 0;
  // Children of a directory, in the source's order.
  virtual std::vector<SourceNode> list(const SourceNode& dir) const = 0;
  // Streams a file's bytes in order.
  virtual void read(const SourceNode& file, const Sink& sink) const = 0;
  // "iso9660", "iso9660+joliet", "fat12", "fat16", "zip", "7z" or "folder".
  virtual std::string format() const = 0;
  virtual std::string volume_id() const { return {}; }
  // What a directory is, independent of the name it was reached by: an ISO
  // directory's extent, a FAT subdirectory's first cluster, a folder's file
  // id. Two directory entries with the same key are one directory listed
  // twice, which no real disc does (a crafted image can, at every level, and
  // would multiply the files an import plans). "" when the source cannot
  // tell; such a directory is never counted as seen.
  virtual std::string dir_key(const SourceNode& dir) const { (void)dir; return {}; }

  // Case-insensitive lookup of a '/'- or '\'-separated path from the root.
  std::optional<SourceNode> find(std::string_view path) const;
  std::optional<SourceNode> child(const SourceNode& dir, std::string_view name) const;
  // The whole file; ImportError(source_invalid) when it is larger than `max_bytes`.
  std::vector<uint8_t> read_all(const SourceNode& file, uint64_t max_bytes = 64ull << 20) const;
};

// A DOS date/time (FAT directory entries, ZIP members: local time of the
// machine that wrote it) as UTC, interpreted in this machine's time zone
// with that date's daylight rules — what Windows shows for a floppy's files.
// Nothing for a zero or unrepresentable date.
std::optional<FILETIME> dos_filetime(uint16_t date, uint16_t time);

// An image file, sniffed by content: ISO-9660 (cooked or raw sectors) first,
// then a ZIP (a local file header at byte 0: its members are the root's
// files, bare names only, or a disk set's DISK<n>/<bare name>, either in one
// folder that holds everything or not; none password-protected), then a 7z
// (its signature at byte 0; the same rule, format "7z"), then FAT12/16. A
// disk set is read as its union (see above), and `note`, when given, then
// says so, and names the one folder read as the root ("" otherwise). Throws
// ImportError(source_invalid) when it is none of them.
std::unique_ptr<SourceFs> open_image(const std::filesystem::path& path, std::string* note = nullptr);

// A floppy image a ZIP holds: the member's name and its bytes.
struct ZippedImage {
  std::string name;
  std::shared_ptr<const std::vector<uint8_t>> bytes;
};
// The floppy images of a ZIP are held in memory together, at most this many
// bytes of them (44 high-density floppies; no release came on more than 5).
inline constexpr uint64_t kMaxZippedImageBytes = 64ull << 20;
// The floppy images in a ZIP (a local file header at byte 0, at most 256 MB),
// in any folder ("<folder>/DISK1.IMG", the Internet Archive's
// "Intermission 4.0/ITM4W-D1.IMA"): every member whose size is a DOS
// floppy's (a multiple of 512 bytes from 160 KB to 2.88 MB), inflated with
// its size and CRC-32 checked, that is a FAT12/16 volume. Every other member
// (a label scan, the metadata of a whole Internet Archive item) is only
// named in `ignored`, never inflated; nor is a floppy-sized one past its
// first 64 KiB (the output chunk that completes its first sector) when that
// sector is no boot sector (55 AA, a sector size FatImage takes). A 7z is
// read the same way (its signature at byte 0; sevenzip.h), each image
// decoded with its size and CRC-32 checked.
// Empty when the file is no ZIP or 7z, or holds no floppy image: it is then
// a ZIP or 7z of install files, read by open_image. Throws
// ImportError(source_invalid) for a floppy-sized member that is
// password-protected or damaged, and when the members with a boot sector add
// up to more than `max_bytes`.
std::vector<ZippedImage> floppy_images_in_zip(const std::filesystem::path& path,
                                              std::vector<std::string>* ignored = nullptr,
                                              uint64_t max_bytes = kMaxZippedImageBytes);
// A FAT12/16 image in memory (`name` for messages); a disk set as its union
// (`note` as for open_image). Throws ImportError(source_invalid) when it is
// not one.
std::unique_ptr<SourceFs> open_fat_image(std::shared_ptr<const std::vector<uint8_t>> bytes, const std::string& name,
                                         std::string* note = nullptr);
// A folder (or drive root). The root of a CD drive is read as the disc
// itself (raw ISO-9660, so a Joliet disc keeps its 8.3 names), falling back
// to the listing when the volume cannot be opened. A disk set is read as its
// union (see above). `note`, when given, says which of these happened, and
// names the DISK<n> folders of a root that holds other things beside them
// (read as it is); "" otherwise. Throws ImportError(source_invalid) when it
// is not a directory.
std::unique_ptr<SourceFs> open_folder(const std::filesystem::path& dir, std::string* note = nullptr);
// Several sources seen as one tree: directories merge, a file present in
// more than one must have the same size and the same bytes (both checked
// when it is read), else the source is invalid. `labels`, when
// given (one per part: a disk set's folder names), name the two parts that
// disagree in that message; otherwise it speaks of "the images".
std::unique_ptr<SourceFs> union_of(std::vector<std::unique_ptr<SourceFs>> parts, std::vector<std::string> labels = {});

}  // namespace adw::import
