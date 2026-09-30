// The installed state of a win directory (internal): the import lock, the
// repair of what an interrupted import or removal left behind (PACKAGES.md
// §5.1), which packages are installed, and the merged catalog over them.
#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "catalog.h"
#include "packages.h"
#include "winutil.h"

namespace adw::import {

using LogFn = std::function<void(const std::string&)>;

// <win>\import.lock: an unshared, delete-on-close file, so a crashed import
// can never leave it held; holding it is what makes the recovery sweep safe
// (anything it finds is dead). Throws ImportError(error) when another
// operation holds it. `win` must exist.
Handle lock_win_dir(const std::filesystem::path& win);

// Under the lock, at the start of every operation: Deluxe's recovery of an
// interrupted FILES swap (unchanged), then for every package: a
// packages\<id>.old-<pid> is put back when packages\<id> is missing (the run
// died between its renames), else deleted; every *.importing-* and
// *.removing-* is deleted. The catalog is rewritten when anything was
// recovered or a package operation's catalog tmp was left behind.
void recover(const std::filesystem::path& win, std::span<const Package> registry, const LogFn& log);

// What a package's import record says (empty strings when there is none).
struct ImportRecord {
  bool present = false;
  int version = 0;
  std::string verified, imported_utc;
  std::string image_md5;  // source.imageMd5 (version 2) or isoMd5 (1): one image or ZIP; "" for any other source
  uint64_t file_count = 0;
};
ImportRecord read_import_record(const std::filesystem::path& import_json);

// The package's root under <win> ("FILES" or "packages/<id>").
std::filesystem::path package_root(const std::filesystem::path& win, const Package& p);
// Its import record: <win>\import.json for Deluxe, <root>\import.json otherwise.
std::filesystem::path import_record_path(const std::filesystem::path& win, const Package& p);

// The installed packages, registry order: Deluxe when <win>\FILES is a
// directory, another package when <win>\packages\<id>\import.json exists.
// Unknown directories in packages\ are ignored (logged once). `skip`, when
// set, is left out (the package an import is replacing).
std::vector<CatalogTree> installed_trees(const std::filesystem::path& win, std::span<const Package> registry,
                                         const LogFn& log, const Package* skip = nullptr);

// Writes <win>\catalog-win.json.tmp-<pid> for `trees` and returns the
// document; the caller renames it into place.
CatalogDoc write_catalog_tmp(const std::filesystem::path& win, const std::vector<CatalogTree>& trees,
                             const std::filesystem::path& tmp, const LogFn& log);
// Rebuilds catalog-win.json over the installed packages, atomically.
CatalogDoc rewrite_catalog(const std::filesystem::path& win, std::span<const Package> registry, const LogFn& log);

}  // namespace adw::import
