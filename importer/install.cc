#include "install.h"

#include <windows.h>

#include <algorithm>
#include <map>
#include <set>
#include <string_view>

#include "covers_internal.h"
#include "importer.h"
#include "minijson.h"
#include "names.h"

namespace adw::import {

namespace fs = std::filesystem;

namespace {

void remove_quietly(const fs::path& p) { remove_tree(p); }

std::wstring pid_tag() { return std::to_wstring(GetCurrentProcessId()); }

bool starts_with(std::wstring_view s, std::wstring_view prefix) {
  return s.size() > prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

// Deluxe's recovery, exactly as before packages existed. A finished Deluxe
// import performs, in order: write import.json.tmp-N and
// catalog-win.json.tmp-N, FILES -> FILES.old-N, FILES.importing-N -> FILES,
// import.json.tmp-N -> import.json, catalog-win.json.tmp-N ->
// catalog-win.json, delete FILES.old-N; a failed one deletes its tmps before
// its stage. So what an import left behind says how far it got, and two of
// those states hold data that must not simply be deleted:
//  - no FILES but a FILES.old-N: it died between the two FILES renames and
//    FILES.old-N is the only copy of the installed tree — put it back;
//  - FILES.old-N but no stage: its new tree is in place and only the renames
//    of its import.json and/or its catalog (whichever tmps are left) did not
//    happen — finish them, so the record and the catalog describe the tree.
void recover_deluxe(const fs::path& win, const LogFn& log) {
  struct Left {
    bool stage = false, old = false, tmp = false, cat = false;
  };
  const fs::path files_dir = win / L"FILES", import_json = win / L"import.json",
                 catalog = win / to_wide(kCatalogFileName);
  const std::wstring cat_prefix = to_wide(kCatalogFileName) + L".tmp-";
  std::map<std::wstring, Left> by_tag;
  std::error_code ec;
  for (auto& e : fs::directory_iterator(win, ec)) {
    std::wstring n = e.path().filename().wstring();
    for (auto [prefix, field] : {std::pair{L"FILES.importing-", &Left::stage}, std::pair{L"FILES.old-", &Left::old},
                                 std::pair{L"import.json.tmp-", &Left::tmp}, std::pair{cat_prefix.c_str(), &Left::cat}}) {
      std::wstring_view p(prefix);
      if (starts_with(n, p)) by_tag[n.substr(p.size())].*field = true;
    }
  }
  bool files_present = fs::exists(files_dir, ec);
  bool restored = false;
  for (auto& [tag, left] : by_tag) {
    fs::path stage = win / (L"FILES.importing-" + tag), old = win / (L"FILES.old-" + tag),
             tmp = win / (L"import.json.tmp-" + tag), cat = win / (cat_prefix + tag);
    DWORD err = 0;
    if (!files_present && left.old && move_with_retry(old, files_dir, 0, err)) {
      files_present = restored = true;
      left.old = false;
      if (log) log("restored the FILES tree an interrupted import had moved aside");
    } else if (files_present && !restored && left.old && !left.stage) {
      if (left.tmp && move_with_retry(tmp, import_json, MOVEFILE_REPLACE_EXISTING, err)) {
        left.tmp = false;
        if (log) log("completed the import.json of an interrupted import");
      }
      if (left.cat && move_with_retry(cat, catalog, MOVEFILE_REPLACE_EXISTING, err)) {
        left.cat = false;
        if (log) log("completed the catalog of an interrupted import");
      }
    }
    if (left.tmp) fs::remove(tmp, ec);
    if (left.cat) fs::remove(cat, ec);
    if (left.stage) remove_quietly(stage);
    if (left.old) remove_quietly(old);
  }
}

std::string read_text_file(const fs::path& p, uint64_t max = 64ull << 20) {
  Handle h(CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                       FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
  if (!h.valid()) return {};
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(h.get(), &size) || uint64_t(size.QuadPart) > max) return {};
  std::string data(size_t(size.QuadPart), '\0');
  DWORD got = 0;
  if (!data.empty() && (!ReadFile(h.get(), data.data(), DWORD(data.size()), &got, nullptr) || got != data.size()))
    return {};
  return data;
}

bool is_registry_id(std::span<const Package> registry, const std::string& name) {
  for (const Package& p : registry)
    if (!p.is_deluxe() && name == p.id) return true;
  return false;
}

}  // namespace

Handle lock_win_dir(const fs::path& win) {
  Handle lock(CreateFileW((win / L"import.lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                          FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_DELETE_ON_CLOSE, nullptr));
  if (!lock.valid()) {
    DWORD e = GetLastError();
    throw ImportError(Status::error, e == ERROR_SHARING_VIOLATION
                                         ? "another import into " + to_utf8(win.wstring()) + " is running"
                                         : "cannot lock " + to_utf8(win.wstring()) + ": " + win_error_string(e));
  }
  return lock;
}

ImportRecord read_import_record(const fs::path& import_json) {
  ImportRecord r;
  std::string text = read_text_file(import_json);
  if (text.empty()) return r;
  auto j = parse_json(text);
  if (!j || j->kind != JsonValue::Kind::object) return r;
  r.present = true;
  r.version = j->int_in("version");
  r.verified = j->str("verified");
  r.imported_utc = j->str("importedUtc");
  if (const JsonValue* s = j->get("source"); s && s->kind == JsonValue::Kind::object)
    r.image_md5 = s->str(r.version == 1 ? "isoMd5" : "imageMd5");
  r.file_count = uint64_t(std::max<int64_t>(0, j->integer("fileCount")));
  return r;
}

fs::path package_root(const fs::path& win, const Package& p) {
  fs::path root = win;
  std::string_view r = p.root;
  size_t i = 0;
  while (i < r.size()) {
    size_t j = r.find('/', i);
    if (j == std::string_view::npos) j = r.size();
    root /= to_wide(r.substr(i, j - i));
    i = j + 1;
  }
  return root;
}

fs::path import_record_path(const fs::path& win, const Package& p) {
  return p.is_deluxe() ? win / L"import.json" : package_root(win, p) / L"import.json";
}

std::vector<CatalogTree> installed_trees(const fs::path& win, std::span<const Package> registry_in, const LogFn& log,
                                         const Package* skip) {
  std::span<const Package> registry = registry_or_builtin(registry_in);
  std::vector<CatalogTree> out;
  std::error_code ec;
  for (const Package& p : registry) {
    if (&p == skip) continue;
    fs::path root = package_root(win, p);
    bool installed = p.is_deluxe() ? fs::is_directory(root, ec) : fs::is_regular_file(root / L"import.json", ec);
    if (!installed) continue;
    ImportRecord rec = read_import_record(import_record_path(win, p));
    CatalogTree t;
    t.package = &p;
    t.dir = root;
    t.verified = rec.present && !rec.verified.empty() ? rec.verified : "unknown";
    t.imported_utc = rec.imported_utc;
    // COVERS.md §2.7, §2.10: checked (and a stale tile rendered again) on
    // every catalog write; every caller holds the lock.
    t.cover = covers_detail::catalog_cover(win, p, log);
    out.push_back(std::move(t));
  }
  // Anything else in packages\ is not ours to catalogue (staging names are
  // the recovery's business).
  std::vector<std::string> strangers;
  for (auto& e : fs::directory_iterator(win / L"packages", ec)) {
    std::string n = to_utf8(e.path().filename().wstring());
    if (n.find('.') != std::string::npos && (n.find(".importing-") != std::string::npos ||
                                             n.find(".old-") != std::string::npos || n.find(".removing-") != std::string::npos))
      continue;
    if (!is_registry_id(registry, n)) strangers.push_back(n);
  }
  if (!strangers.empty() && log) {
    std::string s;
    for (auto& n : strangers) s += (s.empty() ? "" : ", ") + n;
    log("ignoring " + s + " in " + to_utf8((win / L"packages").wstring()) + " (not a known package)");
  }
  return out;
}

CatalogDoc write_catalog_tmp(const fs::path& win, const std::vector<CatalogTree>& trees, const fs::path& tmp,
                             const LogFn& log) {
  (void)win;
  CatalogDoc doc = build_catalog(trees, log);
  DWORD err = 0;
  if (!write_whole_file(tmp, render_catalog(doc), err))
    throw ImportError(Status::error, "cannot write " + to_utf8(tmp.wstring()) + ": " + win_error_string(err));
  return doc;
}

CatalogDoc rewrite_catalog(const fs::path& win, std::span<const Package> registry, const LogFn& log) {
  fs::path tmp = win / (to_wide(kCatalogFileName) + L".tmp-" + pid_tag());
  fs::path catalog = win / to_wide(kCatalogFileName);
  CatalogDoc doc;
  try {
    doc = write_catalog_tmp(win, installed_trees(win, registry, log), tmp, log);
    DWORD err = 0;
    if (!move_with_retry(tmp, catalog, MOVEFILE_REPLACE_EXISTING, err))
      throw ImportError(Status::error, "cannot replace " + to_utf8(catalog.wstring()) + " (close Long After Dark's "
                                       "settings and try again): " + win_error_string(err));
  } catch (...) {
    std::error_code ec;
    fs::remove(tmp, ec);
    throw;
  }
  return doc;
}

void recover(const fs::path& win, std::span<const Package> registry, const LogFn& log) {
  std::error_code ec;
  const std::wstring cat_prefix = to_wide(kCatalogFileName) + L".tmp-";
  std::set<std::wstring> deluxe_tags, cat_tags;
  std::vector<fs::path> removing;
  for (auto& e : fs::directory_iterator(win, ec)) {
    std::wstring n = e.path().filename().wstring();
    for (std::wstring_view p : {std::wstring_view(L"FILES.importing-"), std::wstring_view(L"FILES.old-"),
                                std::wstring_view(L"import.json.tmp-")})
      if (starts_with(n, p)) deluxe_tags.insert(n.substr(p.size()));
    if (starts_with(n, cat_prefix)) cat_tags.insert(n.substr(cat_prefix.size()));
    if (starts_with(n, L"FILES.removing-")) removing.push_back(e.path());
  }
  // A catalog tmp of a Deluxe import is Deluxe's recovery's to finish or
  // delete; any other was left by a package operation, and the catalog is
  // then rebuilt from what is installed.
  bool regen = false;
  for (const auto& t : cat_tags)
    if (!deluxe_tags.count(t)) regen = true;
  recover_deluxe(win, log);
  for (const fs::path& r : removing) {
    // An interrupted --remove deluxe: finish it.
    remove_quietly(r);
    if (!fs::exists(win / L"FILES", ec)) fs::remove(win / L"import.json", ec);
    if (log) log("finished removing After Dark 4.0 Deluxe (an interrupted --remove)");
    regen = true;
  }
  fs::path pk = win / L"packages";
  if (fs::is_directory(pk, ec)) {
    std::vector<fs::path> entries;
    for (auto& e : fs::directory_iterator(pk, ec)) entries.push_back(e.path());
    for (const fs::path& e : entries) {
      std::wstring n = e.filename().wstring();
      size_t at;
      if ((at = n.find(L".old-")) != std::wstring::npos && at > 0) {
        fs::path live = pk / n.substr(0, at);
        DWORD err = 0;
        if (!fs::exists(live, ec) && move_with_retry(e, live, 0, err)) {
          if (log) log("restored " + to_utf8(n.substr(0, at)) + ", which an interrupted import had moved aside");
        } else {
          remove_quietly(e);
        }
        regen = true;
      } else if (n.find(L".importing-") != std::wstring::npos) {
        remove_quietly(e);
      } else if (n.find(L".removing-") != std::wstring::npos) {
        remove_quietly(e);
        regen = true;
      }
    }
  }
  // Covers: the stages of interrupted imports, and stray tmp files.
  if (covers_detail::recover_covers(win, log)) regen = true;
  if (regen) {
    try {
      CatalogDoc doc = rewrite_catalog(win, registry, log);
      if (log) {
        const size_t n = doc.modules.size();
        log("rewrote the catalog after an interrupted operation (" + std::to_string(n) + (n == 1 ? " module)" : " modules)"));
      }
    } catch (const std::exception& e) {
      if (log) log(std::string("could not rewrite the catalog: ") + e.what());
    }
  }
}

// ---- public: catalog-only, list, remove ----------------------------------------------

CatalogResult regenerate_catalog(const fs::path& assets_root, const LogFn& log, std::span<const Package> registry) {
  CatalogResult r;
  fs::path win = win_assets_dir(assets_root.empty() ? default_assets_root() : assets_root);
  r.path = win / to_wide(kCatalogFileName);
  fs::path tmp = win / (to_wide(kCatalogFileName) + L".tmp-" + pid_tag());
  try {
    std::error_code ec;
    if (!fs::is_directory(win, ec))
      throw ImportError(Status::source_invalid, "nothing imported at " + to_utf8(win.wstring()) + "; run an import first");
    // The import's lock: an import swapping a package must not be scanned mid-way.
    Handle lock = lock_win_dir(win);
    recover(win, registry, log);
    std::vector<CatalogTree> trees = installed_trees(win, registry, log);
    if (trees.empty())
      throw ImportError(Status::source_invalid,
                        "nothing imported at " + to_utf8(win.wstring()) + " (no installed package); run an import first");
    CatalogDoc doc = write_catalog_tmp(win, trees, tmp, log);
    DWORD err = 0;
    if (!move_with_retry(tmp, r.path, MOVEFILE_REPLACE_EXISTING, err))
      throw ImportError(Status::error, "cannot replace " + to_utf8(r.path.wstring()) + ": " + win_error_string(err));
    r.modules = doc.modules.size();
    for (const auto& m : doc.modules) r.controls += m.controls.size();
    r.status = Status::ok;
    r.message = "wrote " + to_utf8(r.path.wstring()) + ": " + std::to_string(r.modules) + " module" +
                (r.modules == 1 ? "" : "s") + ", " + std::to_string(r.controls) + " control" +
                (r.controls == 1 ? "" : "s");
  } catch (const ImportError& e) {
    r.status = e.status();
    r.message = e.what();
  } catch (const std::exception& e) {
    r.status = Status::error;
    r.message = e.what();
  }
  if (r.status != Status::ok) {
    std::error_code ec;
    fs::remove(tmp, ec);
  }
  return r;
}

std::vector<PackageState> list_packages(const fs::path& assets_root, std::span<const Package> registry_in) {
  std::span<const Package> registry = registry_or_builtin(registry_in);
  fs::path win = win_assets_dir(assets_root.empty() ? default_assets_root() : assets_root);
  std::vector<PackageState> out;
  std::error_code ec;
  for (const Package& p : registry) {
    PackageState s;
    s.package = &p;
    s.root = package_root(win, p);
    s.installed = p.is_deluxe() ? fs::is_directory(s.root, ec) : fs::is_regular_file(s.root / L"import.json", ec);
    if (s.installed) {
      ImportRecord rec = read_import_record(import_record_path(win, p));
      s.verified = rec.present ? rec.verified : "unknown";
      s.imported_utc = rec.imported_utc;
      s.image_md5 = rec.image_md5;
      s.file_count = rec.file_count;
    }
    out.push_back(std::move(s));
  }
  return out;
}

RemoveResult remove_package(const std::string& id, const fs::path& assets_root, const LogFn& log,
                            std::span<const Package> registry) {
  RemoveResult r;
  fs::path win = win_assets_dir(assets_root.empty() ? default_assets_root() : assets_root);
  fs::path removing;
  try {
    const Package* p = find_package(id, registry);
    if (!p) throw ImportError(Status::error, "unknown package \"" + id + "\" (known: " + known_releases(registry) + ")");
    std::error_code ec;
    if (!fs::is_directory(win, ec)) throw ImportError(Status::error, std::string(p->title) + " is not installed");
    Handle lock = lock_win_dir(win);
    recover(win, registry, log);
    fs::path root = package_root(win, *p);
    bool installed = p->is_deluxe() ? fs::is_directory(root, ec) : fs::is_regular_file(root / L"import.json", ec);
    if (!installed)
      throw ImportError(Status::error, std::string(p->title) + " is not installed at " + to_utf8(win.wstring()));
    removing = root;
    removing += L".removing-" + pid_tag();
    DWORD err = 0;
    if (!move_with_retry(root, removing, 0, err))
      throw ImportError(Status::error, "cannot remove " + to_utf8(root.wstring()) +
                                           " (a file in it is in use — close Long After Dark and try again): " +
                                           win_error_string(err));
    if (p->is_deluxe()) fs::remove(win / L"import.json", ec);
    CatalogDoc doc = rewrite_catalog(win, registry, log);
    remove_quietly(removing);
    r.catalog_modules = doc.modules.size();
    r.status = Status::ok;
    r.message = "removed " + std::string(p->title) + "; catalog: " + std::to_string(r.catalog_modules) + " module" +
                (r.catalog_modules == 1 ? "" : "s");
  } catch (const ImportError& e) {
    r.status = e.status();
    r.message = e.what();
  } catch (const std::exception& e) {
    r.status = Status::error;
    r.message = e.what();
  }
  return r;
}

}  // namespace adw::import
