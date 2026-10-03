// Members of a tar (POSIX ustar, or the older V7 headers) — how the Internet
// Archive serves the only intact copy of The Flintstones Screen Saver
// Collection: three ZIPs among some 160 files of a BBS collection's tar
// (packages.h DownloadMember). Only what that needs: find a member by name,
// and copy its bytes out, checked against the size and md5 the registry
// publishes for it.
//
// The headers are walked from the start, one 512-byte header and its
// padded data after another; only the headers are read, never the data of a
// member that is not asked for. Every header's checksum must hold and its
// size must be octal digits that stay inside the file; anything else ends
// the walk as a damaged archive. A header's name is its ustar prefix and
// name joined by '/'; GNU and pax extension headers are skipped like any
// other entry (a member named only through one is not found). Two zero
// blocks, or the end of the file, end the archive.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace adw::import {

class TarError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// A regular file in the tar: its name and where its bytes are.
struct TarEntry {
  std::string name;  // "wgam0219/FLINTST1.ZIP", as the header holds it
  uint64_t offset = 0;
  uint64_t size = 0;
};

// The regular files named `names` (compared exactly, as the tar holds
// them), in that order; nullopt for one the tar does not hold. The walk
// stops as soon as every name is found. `where` names the tar in messages.
// Throws TarError for a damaged archive or a read error.
std::vector<std::optional<TarEntry>> find_tar_entries(const std::filesystem::path& tar,
                                                      const std::vector<std::string>& names,
                                                      const std::string& where);

// Streams one entry's bytes to `sink` in chunks of at most 1 MiB. Throws
// TarError when the file is shorter than the entry says.
void read_tar_entry(const std::filesystem::path& tar, const TarEntry& e,
                    const std::function<void(const uint8_t*, size_t)>& sink, const std::string& where);

}  // namespace adw::import
