// adw::import — puts the original Windows modules of the known releases
// (the After Dark releases of the registry, packages.h, and Star Wars Screen
// Entertainment) where the hosts look for them (DESIGN.md §6, PACKAGES.md):
//
//   <assets>\win\FILES\{AD40,CLASSIC,ENGINE,AFI}\**   After Dark 4.0 Deluxe (8.3 upper-case names, as on the CD)
//   <assets>\win\import.json                           Deluxe's record (version 1)
//   <assets>\win\packages\<id>\…                       every other package (packages.h), with its import.json (v2)
//   <assets>\win\catalog-win.json                      the modules of every installed package (catalog.h)
//
// <assets> is %AD_ASSETS_DIR% or <data folder>\assets, where the data folder
// is %LOCALAPPDATA%\LongAfterDark (data_folder() below). A source is a disc
// image (ISO-9660 or a FAT floppy image, sniffed by content; several floppy
// images are read as one), a ZIP of install files or of floppy images, a
// mounted disc or folder, or a download of a package's Internet Archive copy
// (packages.h `downloads`: a disc image, the images of every install disk, or
// a ZIP of the install files). The importer identifies which release it is
// (image md5s — one image, or a set of install disks — then file
// fingerprints), extracts it with that release's recipe, verifies it against
// the release's manifest, and swaps in only that release's directory:
// importing one package never touches another, and a failed or cancelled
// import leaves everything as it was.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "packages.h"
#include "status.h"

namespace adw::import {

class CancelToken;  // download.h

inline constexpr char kDeluxeIsoUrl[] =
    "https://archive.org/download/after-dark-4-deluxe/"
    "After%20Dark%204.0%20Deluxe%20%281996%29%28Berkeley%20Systems%29%5BMac-PC%5D.iso";
inline constexpr char kDeluxeIsoMd5[] = "d875a60338b73f44b7befa06bdd33aeb";
inline constexpr wchar_t kDeluxeIsoFileName[] = L"After Dark 4.0 Deluxe (1996)(Berkeley Systems)[Mac-PC].iso";

// Deluxe's copy folders and required engines as <win>-relative paths (the
// registry's "deluxe" entry holds the same, relative to FILES).
inline constexpr const char* kImportDirs[] = {"AD40", "CLASSIC", "ENGINE", "AFI"};
inline constexpr const char* kRequiredFiles[] = {
    "FILES/AD40/ADXPL510.DLL", "FILES/CLASSIC/ADXPL300.DLL", "FILES/ENGINE/OLDMOD16.DLL"};

inline constexpr char kToolName[] = "adimport 1.3";

// ImportOptions' default staging budget (see there).
inline constexpr uint64_t kMaxImportFiles = 20000;
inline constexpr uint64_t kMaxImportBytes = 2ull << 30;

struct Progress {
  // `cover` (COVERS.md §2.4) comes after the stage is verified and before the
  // swap: the package's box cover is captured (`item` names the source,
  // "Box front from Wikisimpsons"; `total` is the download size, 0 for local
  // work). --refresh-covers and the cover commands report only this phase.
  enum class Phase { download, check_image, copy, verify, cover, finalize };
  Phase phase = Phase::copy;
  uint64_t done = 0, total = 0;  // bytes within the phase (total 0 = unknown)
  std::string item;              // file being handled, when there is one
  std::string package;           // title of the identified package ("" until identified)
};
const char* phase_name(Progress::Phase p);

struct Source {
  // `iso` (alias `image`): one or more image files (or a ZIP of install
  // files, or of floppy images: each image in it is one of the images);
  // `folder`: a drive root or folder; `download`: a package's Internet
  // Archive copy (`package`, Deluxe when empty), or `url`.
  enum class Kind { iso, image = iso, folder, download };
  Kind kind = Kind::iso;
  std::filesystem::path path;  // image: the (first) image; folder: drive root / folder; download: downloads dir (empty = default)
  std::vector<std::filesystem::path> more_images;  // image: further images read as one with `path` (split floppies)
  // download only. Empty: the package's registry copies, tried in order, each
  // checked against its published size and md5. Set: that URL, saved under
  // its own file name, checked against `expected_md5` when that is set.
  std::string url;
  // download only: replaces the expected md5 (of `url`, or of every registry
  // copy, whose published size is then not enforced either); empty = the
  // registry's, or unchecked for a custom `url`.
  std::string expected_md5;
  std::string package;         // restrict identification to this registry id ("" = any): adimport --package
};

struct ImportOptions {
  std::filesystem::path assets_root;  // empty = default_assets_root()
  // Check every file against the package's built-in manifest and fail
  // (verify_failed) on a mismatch. Off only for synthetic test trees and
  // deliberate imports of other builds (adimport --no-verify).
  bool check_known = true;
  std::function<bool(const Progress&)> progress;  // return false to cancel
  std::function<void(const std::string&)> log;    // one-line human notes
  // The package registry (empty = builtin_packages()). Tests substitute one
  // with synthetic manifests and image md5s.
  std::span<const Package> registry;
  // COVERS.md §2.4: the import captures the package's box cover from its
  // registry cover sources. false (adimport --no-cover-download) skips the
  // download sources; on-disc art is still used. A cover never fails an import.
  bool cover_download = true;
  // Each cover download's connect / between-bytes timeout (15 s); tests shorten it.
  int cover_timeout_ms = 15000;
  // Where cover downloads are kept (<dir>\covers\<file>); empty = the download
  // source's directory, else default_download_dir().
  std::filesystem::path cover_download_dir;
  // The staging budget: an import that would copy more files or bytes than
  // this is refused (source_invalid) while it is planned, before anything is
  // written. The largest release is 175 files and 45 MB, so only a damaged
  // or crafted source comes near; tests lower them.
  uint64_t max_files = kMaxImportFiles;
  uint64_t max_bytes = kMaxImportBytes;
  // Cancels a running import at once, even inside a download that is waiting
  // on the network (download.h CancelToken); `progress` returning false is
  // noticed only at the next report. Optional; the caller keeps it alive.
  const CancelToken* cancel = nullptr;
};

struct ImportedFile {
  std::string path;  // relative to <assets>\win, '/'-separated: "FILES/AD40/ADXPL510.DLL"
  uint64_t size = 0;
  std::string md5;
  enum class Known { unknown, match, mismatch };
  Known known = Known::unknown;
  std::string from;  // where it came from in the source ("INSTALL/GUTS.ZIP!GUTS.AD", "alias:AD10TH/BABY.MID")
};

struct ImagePart {
  std::string path;
  uint64_t size = 0;
  std::string md5;
};

struct ImportResult {
  Status status = Status::error;
  std::string message;                     // failure reason, or a one-line summary
  std::vector<ImportedFile> files;         // sorted by path; installed only when status == ok
  std::vector<std::string> missing_known;  // manifest files the source did not have
  std::string source;                      // human-readable: image path, folder, URL
  std::string url;                         // download: the URL fetched (a registry copy — its first file —, or Source::url)
  std::string final_url;                   // download: after redirects ("" when an earlier download was reused)
  bool download_md5_checked = false;       // download: the file matched an expected md5 (published, or --md5)
  std::string iso_md5;                     // single-image sources (one image in a ZIP of floppy images too)
  uint64_t iso_size = 0;
  // iso_md5 is the package's known image, or the parts are every one of its
  // install disks: "verified": "image".
  bool iso_md5_known = false;
  bool joliet = false;
  std::string format;                      // iso9660 | iso9660+joliet | fat12 | fat16 | folder
  std::string volume_id;
  // Every image of a multi-image source; an image a ZIP holds is
  // "<zip path>!<member>".
  std::vector<ImagePart> parts;
  std::string verified;                    // "image" | "files" | "partial" | "none" (see import.json)
  std::string package_id, package_title;   // the identified package
  std::string build;                       // its build (Package::build or a Build's id); "" for a release with one
  std::filesystem::path files_dir;         // the package root: <assets>\win\FILES or <assets>\win\packages\<id>
  std::filesystem::path import_json;       // <assets>\win\import.json (Deluxe) or <package root>\import.json
  std::filesystem::path catalog;           // <assets>\win\catalog-win.json (catalog.h)
  size_t package_modules = 0;              // modules of this package in the new catalog
  size_t catalog_modules = 0;              // modules the new catalog lists (every installed package)
  std::vector<std::string> installed;      // titles of the installed packages afterwards, registry order
};

// Never throws; everything is reported through ImportResult::status.
ImportResult run_import(const Source& src, const ImportOptions& opts);

// adimport --download all (and the GUI's "every release not imported yet"):
// run_import of each of `ids` in turn, as a download of that package into
// `base`'s download directory (base.url and base.expected_md5 are ignored).
// Goes on after a failure, stops after a cancel; one result per package
// attempted, in order. `starting`, when set, is told the index of each
// package before its import begins.
std::vector<ImportResult> import_downloads(const std::vector<std::string>& ids, const Source& base,
                                           const ImportOptions& opts,
                                           const std::function<void(size_t index)>& starting = {});

// ---- the data folder -------------------------------------------------------------
// Long After Dark keeps everything per user in one folder,
// %LOCALAPPDATA%\LongAfterDark. The base is the one adhostwin and
// LongAfterDark.scr use (host/core/include/adw/core/data_root.h:
// AD_LOCALAPPDATA when set, else LOCALAPPDATA, else the known folder), so all
// three programs agree on the folder. It is only a path: working it out reads
// the environment, never the disk, and creates nothing. A run with explicit
// locations only (--dest or AD_ASSETS_DIR, and --download-dir where a
// download happens) never uses it.
// <base>\LongAfterDark; "LongAfterDark" (relative) in the unlikely case that
// no base can be found at all.
std::filesystem::path data_folder();

std::filesystem::path default_assets_root();   // %AD_ASSETS_DIR%, else <data folder>\assets
std::filesystem::path default_download_dir();  // <data folder>\downloads
// Where the files go for an assets root (PACKAGES.md §5.2): <root>\win when
// it holds FILES, packages or catalog-win.json; else <root> itself when that
// holds one of them; else <root>\win — the rule adhostwin applies to
// AD_ASSETS_DIR.
std::filesystem::path win_assets_dir(const std::filesystem::path& root);

// The FILES directory inside a folder source: <dir>\ADE\FILES, <dir>\FILES,
// or <dir> itself when it directly holds AD40 + ENGINE (Deluxe's shape).
std::optional<std::filesystem::path> locate_files_dir(const std::filesystem::path& dir);

// Which package a folder holds (the GUI's check before it starts an import).
// nullptr with the reason in `why` when it is none, or more than one; the
// reason starts with what open_folder noted while reading the folder (DISK<n>
// folders read as they are because something else is beside them), then the
// error.
const Package* identify_folder(const std::filesystem::path& dir, std::string* why = nullptr,
                               std::span<const Package> registry = {});

// Manifest of the Deluxe release (the registry's "deluxe" manifest).
std::span<const KnownFile> known_files();

// ---- installed packages -----------------------------------------------------------

struct PackageState {
  const Package* package = nullptr;
  bool installed = false;
  std::filesystem::path root;  // where it is (or would be) installed
  std::string verified, imported_utc;
  // The md5 of the image (or ZIP) it was imported from, when it came from
  // one ("": a folder, a set of disks): which known image "verified: image"
  // means, for the windows' wording.
  std::string image_md5;
  uint64_t file_count = 0;
};
// Every registry package with its installed state (adimport --list-packages).
// Reads only; takes no lock.
std::vector<PackageState> list_packages(const std::filesystem::path& assets_root,
                                        std::span<const Package> registry = {});

struct RemoveResult {
  Status status = Status::error;
  std::string message;
  size_t catalog_modules = 0;
};
// adimport --remove <id>: under import.lock, moves the package root aside,
// rewrites the catalog without it, then deletes it (Deluxe: FILES and
// import.json). Never throws.
RemoveResult remove_package(const std::string& id, const std::filesystem::path& assets_root,
                            const std::function<void(const std::string&)>& log = {},
                            std::span<const Package> registry = {});

}  // namespace adw::import
