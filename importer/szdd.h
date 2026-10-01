// SZDD: files compressed by Microsoft's COMPRESS.EXE ("A" mode, LZSS), the
// "*.XX_" files installers expand on the way (Star Wars Screen
// Entertainment's STRESS.DL_ and GM_*.MI_; research/win/pkg/swse/survey/
// importer_design.md §3). Windows' own EXPAND.EXE gives the same bytes for
// the disc's files and for the test vectors.
//
// A 14-byte header — magic "SZDD" 88 F0 27 33, mode 'A', the extension
// character COMPRESS dropped, the expanded size — then flag bytes, each
// saying for the next eight tokens (least significant bit first) whether it
// is a literal byte (1) or a two-byte match (0: a 12-bit window position
// and a length of 3..18) into a 4096-byte window that starts filled with
// spaces, written from 0xFF0. The expander is strict: it must produce exactly
// the header's size, no match may pass it, and no whole byte may be left
// after the last token (all verified 0 over the disc's 24 SZDD files) but
// Delrina's version stamps: its Intermission Installer's files (The Far
// Side's, Dilbert's) end their shared libraries with one or two 8-byte
// records "DLL " + four digits ("DLL 0401"; the installer compares them so
// an older library never replaces a newer one), which are not data.
//
// SZDD has no checksum. A damaged file from a source without a known image
// md5 is caught only by the manifest, as a verify failure (3), and not at all
// under --no-verify; a damaged ARJ or ZIP member is a corrupt source (2).
#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <stdexcept>
#include <string_view>

namespace adw::import {

class SzddError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

struct SzddHeader {
  uint32_t size = 0;  // the expanded size
  char missing = 0;   // the extension character COMPRESS dropped ('\0' = none: the installer names the file)
};

// The header. Throws SzddError for another magic (a KWAJ file says so: it is
// kwaj.h's), a mode other than 'A', or a file shorter than the header; `name`
// is for messages.
SzddHeader szdd_header(std::span<const uint8_t> file, std::string_view name);

// Streams the expanded bytes to `sink` in chunks of at most 64 KiB. Throws
// SzddError when the data ends before the header's size, a match would pass
// it, or whole bytes are left after the last token that are not Delrina's
// version stamps (above).
void szdd_expand(std::span<const uint8_t> file, std::string_view name,
                 const std::function<void(const uint8_t*, size_t)>& sink);

}  // namespace adw::import
