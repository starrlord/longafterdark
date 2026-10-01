// importer/gui/gui.h — owned by T. Frozen by COVERS.md §8.2: adimport.cc calls it.
#pragma once
#include <filesystem>
#include <optional>
#include <string>

#include "importer.h"   // Source

namespace adw::import::gui {

struct Request {
  std::optional<Source> source;   // --image/--iso/--from/--download given: no chooser
  bool download_all = false;      // --download all (`source` is then the download base)
  std::filesystem::path dest;     // --dest; empty = default_assets_root()
  std::string package;            // --package <id>: limits the download list
  bool no_verify = false;         // --no-verify
  bool cover_download = true;     // false = --no-cover-download
  std::string change_cover;       // --change-cover <id>: only the cover window (§2.11)
  // Added at the critique's fix round (additive; adimport.cc fills them):
  std::filesystem::path download_dir;   // --download-dir; empty = default_download_dir()
  bool refresh_covers = false;          // --gui --refresh-covers: only a progress window over refresh_covers
  std::string refresh_id;               // ...of this package ("" = every installed one)
  bool force = false;                   // --refresh-covers --force
  // Added with "Remove …" (additive; adimport.cc fills it):
  std::string remove;                   // --gui --remove <id>: only the window that asks, then removes it
};

// Shows the importer's windows (COVERS.md §4) and returns the exit code (adw::import::Status).
// Initializes COM, common controls and GDI+ itself; honours AD_GUI_AUTOCLOSE.
int run(const Request& r);

}  // namespace adw::import::gui
