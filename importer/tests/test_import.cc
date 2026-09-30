// run_import end to end on synthetic sources: image and folder imports are
// byte-identical to the fixture with 8.3 upper-case names, import.json says
// what was imported (parsed independently with phosg), catalog-win.json
// lists the modules of exactly the installed tree, a re-import replaces the
// tree atomically, and failures (bad source, missing engine, cancel,
// known-file mismatch) leave the previous tree, record and catalog exactly
// as they were. And the UTF-8 the records are written in: utf8.h's test
// and repair of it against Windows' own decoder, and json_escape.
#include <phosg/JSON.hh>

#include <functional>
#include <map>
#include <set>

#include "catalog.h"
#include "fixture.h"
#include "importer.h"
#include "md5.h"
#include "minijson.h"
#include "module_builder.h"

using namespace adw::import;
namespace fs = std::filesystem;

namespace {

std::map<std::string, std::vector<uint8_t>> expected_tree(const std::vector<test::FixtureFile>& files) {
  std::map<std::string, std::vector<uint8_t>> m;
  for (auto& f : files)
    if (!f.out.empty()) m[f.out.substr(strlen("FILES/"))] = f.data;
  return m;
}

// The imported tree equals `want` exactly: same names, same bytes, nothing else.
void check_tree(const fs::path& files_dir, const std::map<std::string, std::vector<uint8_t>>& want) {
  auto have = test::list_tree(files_dir);
  std::set<std::string> have_set(have.begin(), have.end());
  for (auto& [rel, data] : want) {
    if (!have_set.count(rel)) {
      test::g_failures++;
      fprintf(stderr, "  missing from import: %s\n", rel.c_str());
      continue;
    }
    if (test::read_bytes(files_dir / to_wide(rel)) != data) {
      test::g_failures++;
      fprintf(stderr, "  content differs: %s\n", rel.c_str());
    }
  }
  for (auto& rel : have)
    if (!want.count(rel)) {
      test::g_failures++;
      fprintf(stderr, "  unexpected file in import: %s\n", rel.c_str());
    }
}

void check_import_json(const ImportResult& r, const std::map<std::string, std::vector<uint8_t>>& want,
                       const char* kind, const fs::path& iso) {
  std::string text = test::read_text(r.import_json);
  CHECK(!text.empty());
  phosg::JSON j;
  try {
    j = phosg::JSON::parse(text);
  } catch (const std::exception& e) {
    test::g_failures++;
    fprintf(stderr, "import.json does not parse: %s\n", e.what());
    return;
  }
  CHECK_EQ(j.get_int("version"), int64_t(1));
  CHECK_EQ(j.at("source").get_string("kind"), std::string(kind));
  CHECK(!j.get_string("importedUtc").empty());
  if (!iso.empty()) {
    CHECK_EQ(j.at("source").get_string("isoMd5"), md5_file_hex(iso));
    CHECK_EQ(j.at("source").get_int("isoSize"), int64_t(fs::file_size(iso)));
    CHECK_EQ(j.at("source").get_bool("isoMd5Known"), false);
    CHECK_EQ(j.at("source").get_bool("joliet"), true);
    CHECK_EQ(j.at("source").get_string("volumeId"), std::string("AD_TEST"));
  }
  CHECK_EQ(j.get_string("verified"), std::string("none"));
  CHECK_EQ(size_t(j.get_int("fileCount")), want.size());
  uint64_t total = 0;
  for (auto& [rel, data] : want) total += data.size();
  CHECK_EQ(uint64_t(j.get_int("totalBytes")), total);
  const auto& files = j.at("files").as_list();
  CHECK_EQ(files.size(), want.size());
  std::string prev;
  for (const auto& f : files) {
    std::string path = f->get_string("path");
    CHECK(path > prev);  // sorted, unique
    prev = path;
    CHECK(path.rfind("FILES/", 0) == 0);
    auto it = want.find(path.substr(6));
    CHECK(it != want.end());
    if (it == want.end()) continue;
    CHECK_EQ(uint64_t(f->get_int("size")), uint64_t(it->second.size()));
    CHECK_EQ(f->get_string("md5"), md5_hex(it->second.data(), it->second.size()));
    // Synthetic content is never the real release: unknown, or a mismatch
    // for the paths the built-in manifest lists (verification was off).
    std::string known = f->get_string("known");
    CHECK(known == "unknown" || known == "mismatch");
  }
}

ImportOptions opts_for(const fs::path& assets) {
  ImportOptions o;
  o.assets_root = assets;
  o.check_known = false;
  o.cover_download = false;  // offline: the built-in registry's covers are downloads
  o.log = [](const std::string& s) { fprintf(stderr, "  log: %s\n", s.c_str()); };
  return o;
}

std::string ascii_upper_copy(std::string s) {
  for (char& c : s)
    if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
  return s;
}

// A directory junction at `link` to `target` (what mklink /J makes; no
// privilege needed). False when the volume does not support them.
bool make_junction(const fs::path& link, const fs::path& target) {
  if (!CreateDirectoryW(link.c_str(), nullptr)) return false;
  HANDLE h = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                         FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  const std::wstring print = fs::absolute(target).make_preferred().wstring(), sub = L"\\??\\" + print;
  const size_t names = (sub.size() + 1 + print.size() + 1) * sizeof(wchar_t);
  std::vector<uint8_t> b(16 + names, 0);
  auto put16 = [&](size_t at, size_t v) { b[at] = uint8_t(v), b[at + 1] = uint8_t(v >> 8); };
  const uint32_t tag = 0xA0000003;  // IO_REPARSE_TAG_MOUNT_POINT
  memcpy(b.data(), &tag, 4);
  put16(4, 8 + names);                                     // ReparseDataLength
  put16(8, 0);                                             // SubstituteNameOffset
  put16(10, sub.size() * sizeof(wchar_t));                 // SubstituteNameLength
  put16(12, (sub.size() + 1) * sizeof(wchar_t));           // PrintNameOffset
  put16(14, print.size() * sizeof(wchar_t));               // PrintNameLength
  memcpy(b.data() + 16, sub.c_str(), sub.size() * sizeof(wchar_t));
  memcpy(b.data() + 16 + (sub.size() + 1) * sizeof(wchar_t), print.c_str(), print.size() * sizeof(wchar_t));
  DWORD got = 0;
  const bool ok = DeviceIoControl(h, 0x000900A4 /* FSCTL_SET_REPARSE_POINT */, b.data(), DWORD(b.size()), nullptr, 0,
                                  &got, nullptr) != 0;
  CloseHandle(h);
  if (!ok) RemoveDirectoryW(link.c_str());
  return ok;
}

bool no_leftovers(const fs::path& win) {
  std::error_code ec;
  for (auto& e : fs::directory_iterator(win, ec)) {
    std::wstring n = e.path().filename().wstring();
    if (n.find(L"FILES.") == 0 || n.find(L"import.json.tmp") == 0 || n.find(L"catalog-win.json.tmp") == 0) {
      fprintf(stderr, "  leftover: %s\n", to_utf8(n).c_str());
      return false;
    }
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  fs::path dir = test::scratch(argc, argv, "adw-import-import");
  // No default may reach the real data folder (the defaults are checked below).
  const fs::path lad = test::sandbox_data_root(dir / L"localappdata");
  test::IsoBuilder b;
  auto fixture = test::build_fixture(b);
  auto want = expected_tree(fixture);
  CHECK_EQ(want.size(), test::imported_count(fixture));
  fs::path iso = dir / L"deluxe-synthetic.iso";
  test::write_bytes(iso, b.build());

  // ---- image import --------------------------------------------------------
  fs::path assets = dir / L"assets";
  fs::create_directories(assets / L"win");
  test::write_bytes(assets / L"win" / L"notes.txt", {'k', 'e', 'e', 'p'});  // a sibling of FILES
  test::write_bytes(assets / L"win" / L"catalog-win.json", {'o', 'l', 'd'});
  {
    Source s;
    s.kind = Source::Kind::iso;
    s.path = iso;
    std::vector<Progress::Phase> phases;
    ImportOptions o = opts_for(assets);
    o.progress = [&](const Progress& p) {
      if (phases.empty() || phases.back() != p.phase) phases.push_back(p.phase);
      CHECK(p.total == 0 || p.done <= p.total);
      return true;
    };
    ImportResult r = run_import(s, o);
    CHECK_EQ(r.status, Status::ok);
    if (r.status != Status::ok) fprintf(stderr, "  %s\n", r.message.c_str());
    CHECK(r.joliet);
    CHECK(!r.iso_md5_known);
    CHECK_EQ(r.iso_md5, md5_file_hex(iso));
    CHECK_EQ(r.files.size(), want.size());
    CHECK_EQ(r.files_dir, assets / L"win" / L"FILES");
    check_tree(r.files_dir, want);
    check_import_json(r, want, "iso", iso);
    CHECK(no_leftovers(assets / L"win"));
    CHECK(!fs::exists(assets / L"win" / L"import.lock"));  // delete-on-close
    CHECK(test::read_text(assets / L"win" / L"notes.txt") == "keep");
    // The import ends with a catalog of what it installed; the synthetic
    // fixture holds no real modules (its *.AD files are noise), so it is
    // empty — but it is the importer's, not the stale one.
    CHECK_EQ(r.catalog, assets / L"win" / L"catalog-win.json");
    CHECK_EQ(r.catalog_modules, size_t(0));
    // PACKAGES.md §6: the catalog also lists the installed packages (here Deluxe alone).
    phosg::JSON cat0 = phosg::JSON::parse(test::read_text(r.catalog));
    CHECK(cat0.at("modules").as_list().empty());
    CHECK_EQ(cat0.at("packages").as_list().size(), size_t(1));
    CHECK_EQ(cat0.at("packages").as_list().at(0)->get_string("id"), std::string("deluxe"));
    CHECK(r.message.find("catalog: 0 modules") != std::string::npos);
    // COVERS.md §2.4: the box cover between verify and finalize (here the
    // disc has no setup art and downloads are off: the cover stays generated).
    std::vector<Progress::Phase> expect_phases = {Progress::Phase::check_image, Progress::Phase::copy,
                                                  Progress::Phase::verify, Progress::Phase::cover,
                                                  Progress::Phase::finalize};
    CHECK(phases == expect_phases);

    // Recording time 1996-09-12 22:14:10 at GMT-7 -> 1996-09-13 05:14:10 UTC.
    WIN32_FILE_ATTRIBUTE_DATA a{};
    GetFileAttributesExW((r.files_dir / L"AFI" / L"AD2.AFI").c_str(), GetFileExInfoStandard, &a);
    SYSTEMTIME st{};
    FileTimeToSystemTime(&a.ftLastWriteTime, &st);
    CHECK(st.wYear == 1996 && st.wMonth == 9 && st.wDay == 13 && st.wHour == 5 && st.wMinute == 14 &&
          st.wSecond == 10);
  }

  // ---- re-import replaces the tree (a stray file disappears) -----------------
  test::write_bytes(assets / L"win" / L"FILES" / L"AD40" / L"STRAY.AD", {1, 2, 3});
  {
    Source s;
    s.kind = Source::Kind::iso;
    s.path = iso;
    ImportResult r = run_import(s, opts_for(assets));
    CHECK_EQ(r.status, Status::ok);
    check_tree(r.files_dir, want);
    CHECK(no_leftovers(assets / L"win"));
  }

  // ---- an assets root that is already the win directory ----------------------
  // (AD_ASSETS_DIR=...\assets\win, which adhostwin accepts): updated in place.
  CHECK_EQ(win_assets_dir(assets), assets / L"win");
  CHECK_EQ(win_assets_dir(assets / L"win"), assets / L"win");
  CHECK_EQ(win_assets_dir(dir / L"fresh-root"), dir / L"fresh-root" / L"win");
  {
    Source s;
    s.kind = Source::Kind::iso;
    s.path = iso;
    ImportResult r = run_import(s, opts_for(assets / L"win"));
    CHECK_EQ(r.status, Status::ok);
    CHECK_EQ(r.files_dir, assets / L"win" / L"FILES");
    CHECK(!fs::exists(assets / L"win" / L"win"));
    check_tree(r.files_dir, want);
  }

  // ---- folder import: disc root, ADE, and FILES itself all work -------------
  fs::path src = dir / L"cdroot";
  test::write_fixture_folder(src, fixture, /*lower=*/true);
  int round = 0;
  for (const fs::path& from : {src, src / L"ADE", src / L"ADE" / L"FILES"}) {
    // A fresh root per round rather than deleting the last one: deleting a
    // tree of just-written .DLL/.AD files can stall ~15 s behind the virus
    // scanner, and that is test time, not importer behaviour.
    fs::path fa = dir / (L"assets-folder" + std::to_wstring(round++));
    Source s;
    s.kind = Source::Kind::folder;
    s.path = from;
    ImportResult r = run_import(s, opts_for(fa));
    CHECK_EQ(r.status, Status::ok);
    if (r.status != Status::ok) fprintf(stderr, "  %s\n", r.message.c_str());
    CHECK(r.iso_md5.empty());
    check_tree(r.files_dir, want);  // lower-case copies come back as upper-case 8.3
    check_import_json(r, want, "folder", {});
    CHECK(no_leftovers(fa / L"win"));
  }

  // ---- failures leave the previous tree untouched ----------------------------
  auto snapshot = [&] {
    std::map<std::string, std::vector<uint8_t>> m;
    for (auto& rel : test::list_tree(assets / L"win" / L"FILES"))
      m[rel] = test::read_bytes(assets / L"win" / L"FILES" / to_wide(rel));
    return m;
  };
  auto before = snapshot();
  std::string json_before = test::read_text(assets / L"win" / L"import.json");
  // A marker catalog: a failed import must leave even that alone.
  test::write_bytes(assets / L"win" / L"catalog-win.json", {'m', 'a', 'r', 'k'});
  auto expect_failure = [&](const char* what, const Source& s, Status want_status, ImportOptions o) {
    ImportResult r = run_import(s, o);
    fprintf(stderr, "  %s -> %s: %s\n", what, status_name(r.status), r.message.c_str());
    CHECK_EQ(r.status, want_status);
    CHECK(snapshot() == before);
    CHECK(test::read_text(assets / L"win" / L"import.json") == json_before);
    CHECK(test::read_text(assets / L"win" / L"catalog-win.json") == "mark");
    CHECK(no_leftovers(assets / L"win"));
  };
  {
    Source s;
    s.kind = Source::Kind::folder;
    s.path = dir / L"nowhere";
    expect_failure("missing folder", s, Status::source_invalid, opts_for(assets));
    s.kind = Source::Kind::iso;
    s.path = dir / L"nowhere.iso";
    expect_failure("missing image", s, Status::source_invalid, opts_for(assets));
    test::write_bytes(dir / L"not-an-image.iso", test::pattern(70000, 3));
    s.path = dir / L"not-an-image.iso";
    expect_failure("not an image", s, Status::source_invalid, opts_for(assets));
  }
  {
    // A disc without the 16-bit engine is not usable.
    test::IsoBuilder nb;
    for (auto& f : fixture)
      if (f.iso_path != "ADE/FILES/CLASSIC/ADXPL300.DLL") nb.file(f.iso_path, f.data);
    test::write_bytes(dir / L"no-engine.iso", nb.build());
    Source s;
    s.kind = Source::Kind::iso;
    s.path = dir / L"no-engine.iso";
    expect_failure("missing ADXPL300.DLL", s, Status::source_invalid, opts_for(assets));
  }
  for (const char* hostile : {"ADE/FILES/AD40/../../ESCAPE.DLL", "ADE/FILES/AD40/C:EVIL.AD", "ADE/FILES/AD40/CON",
                              "ADE/FILES/CLASSIC/NUL.AD", "ADE/FILES/ENGINE/COM1.DLL"}) {
    // Names that would reach outside the stage (or that Windows cannot
    // represent faithfully) reject the source instead of being written.
    test::IsoBuilder hb;
    hb.joliet = false;  // no Joliet twin to supply a clean name
    for (auto& f : fixture) hb.file(f.iso_path, f.data);
    hb.file(hostile, {1, 2, 3});
    test::write_bytes(dir / L"hostile.iso", hb.build());
    Source s;
    s.kind = Source::Kind::iso;
    s.path = dir / L"hostile.iso";
    expect_failure(hostile, s, Status::source_invalid, opts_for(assets));
    CHECK(!fs::exists(assets / L"win" / L"ESCAPE.DLL") && !fs::exists(assets / L"ESCAPE.DLL"));
  }
  // One directory listed under several names (records sharing an extent):
  // no real disc does it, and planned name by name it multiplies at every
  // level (branching^depth copies of the same bytes). Refused before
  // anything is staged, in both trees.
  for (bool joliet : {false, true}) {
    struct Case {
      const char* what;
      std::function<void(test::IsoBuilder&)> shape;
    };
    const Case cases[] = {
        {"siblings", [](test::IsoBuilder& ab) {
           test::IsoBuilder::Node& pics = ab.dir("ADE/FILES/AD40/PICTURES");
           ab.dir("ADE/FILES/AD40/ALIAS1").same_as = &pics;
           ab.dir("ADE/FILES/AD40/ALIAS2").same_as = &pics;
         }},
        {"loop", [](test::IsoBuilder& ab) { ab.dir("ADE/FILES/AD40/LOOP").same_as = &ab.dir("ADE/FILES/AD40"); }},
        {"3^12 bomb", [](test::IsoBuilder& ab) {
           // L1..L12, each holding three names for the next level.
           std::string at = "ADE/FILES/AD40";
           std::vector<test::IsoBuilder::Node*> level;
           for (int d = 1; d <= 12; d++) level.push_back(&ab.dir(at += "/L" + std::to_string(d)));
           ab.file(at + "/X.DAT", test::pattern(4096, 9));
           for (int d = 0; d + 1 < 12; d++)
             for (const char* n : {"/B", "/C"}) {
               std::string parent;
               for (int k = 0; k <= d; k++) parent += "/L" + std::to_string(k + 1);
               ab.dir("ADE/FILES/AD40" + parent + n).same_as = level[size_t(d + 1)];
             }
         }},
    };
    for (const Case& c : cases) {
      test::IsoBuilder ab;
      ab.joliet = joliet;
      for (auto& f : fixture) ab.file(f.iso_path, f.data);
      c.shape(ab);
      test::write_bytes(dir / L"aliased.iso", ab.build());
      Source s;
      s.kind = Source::Kind::iso;
      s.path = dir / L"aliased.iso";
      bool copied = false;
      ImportOptions o = opts_for(assets);
      o.progress = [&](const Progress& p) {
        copied = copied || p.phase == Progress::Phase::copy;
        return true;
      };
      const std::string what = std::string("aliased folders (") + c.what + (joliet ? ", Joliet)" : ")");
      ImportResult r = run_import(s, o);
      fprintf(stderr, "  %s -> %s: %s\n", what.c_str(), status_name(r.status), r.message.c_str());
      CHECK_EQ(r.status, Status::source_invalid);
      CHECK(r.message.find("more than once") != std::string::npos);
      CHECK(!copied);
      CHECK(snapshot() == before);
      CHECK(no_leftovers(assets / L"win"));
    }
  }
  {
    // The same through a folder: a junction that makes one directory of the
    // copy appear twice (a sibling), or inside itself (a loop).
    fs::path jsrc = dir / L"junctions";
    test::write_fixture_folder(jsrc, fixture, false);
    const fs::path ad40 = jsrc / L"ADE" / L"FILES" / L"AD40";
    if (make_junction(ad40 / L"PICS2", ad40 / L"PICTURES")) {
      for (int round = 0; round < 2; round++) {
        Source s;
        s.kind = Source::Kind::folder;
        s.path = jsrc;
        ImportResult r = run_import(s, opts_for(assets));
        fprintf(stderr, "  junction (%s) -> %s: %s\n", round ? "loop" : "sibling", status_name(r.status),
                r.message.c_str());
        CHECK_EQ(r.status, Status::source_invalid);
        CHECK(r.message.find("more than once") != std::string::npos);
        CHECK(snapshot() == before);
        CHECK(no_leftovers(assets / L"win"));
        RemoveDirectoryW((ad40 / L"PICS2").c_str());  // the junction itself, never its target
        if (!round) CHECK(make_junction(ad40 / L"LOOP", ad40));
      }
      RemoveDirectoryW((ad40 / L"LOOP").c_str());
      CHECK(fs::exists(ad40 / L"PICTURES" / L"PIC1.BMP"));
    } else {
      fprintf(stderr, "  (junctions not supported here; folder aliasing not exercised)\n");
    }
  }
  {
    // The staging budget: a source that plans more files, or more bytes,
    // than allowed is refused while it is planned, before any copy.
    Source s;
    s.kind = Source::Kind::iso;
    s.path = iso;
    ImportOptions o = opts_for(assets);
    bool copied = false;
    o.progress = [&](const Progress& p) {
      copied = copied || p.phase == Progress::Phase::copy;
      return true;
    };
    o.max_files = 5;
    ImportResult r = run_import(s, o);
    fprintf(stderr, "  budget (files) -> %s: %s\n", status_name(r.status), r.message.c_str());
    CHECK_EQ(r.status, Status::source_invalid);
    CHECK(r.message.find("more than 5 files") != std::string::npos);
    o.max_files = kMaxImportFiles;
    o.max_bytes = 20000;
    r = run_import(s, o);
    fprintf(stderr, "  budget (bytes) -> %s: %s\n", status_name(r.status), r.message.c_str());
    CHECK_EQ(r.status, Status::source_invalid);
    CHECK(r.message.find("more than 20000 bytes") != std::string::npos);
    CHECK(!copied);
    CHECK(snapshot() == before);
    CHECK(no_leftovers(assets / L"win"));
    // The defaults hold every release with room to spare (the largest
    // manifest is 175 files, 45 MB).
    for (const Package& p : builtin_packages()) {
      uint64_t bytes = 0;
      for (const KnownFile& k : p.manifest) bytes += k.size;
      CHECK(p.manifest.size() * 10 < kMaxImportFiles);
      CHECK(bytes * 10 < kMaxImportBytes);
    }
  }
  {
    // Cancel midway through the copy.
    Source s;
    s.kind = Source::Kind::iso;
    s.path = iso;
    ImportOptions o = opts_for(assets);
    o.progress = [](const Progress& p) { return !(p.phase == Progress::Phase::copy && p.done > 20000); };
    expect_failure("cancel", s, Status::cancelled, o);
  }
  {
    // A second import into the same root while one holds the lock.
    HANDLE held = CreateFileW((assets / L"win" / L"import.lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    CHECK(held != INVALID_HANDLE_VALUE);
    Source s;
    s.kind = Source::Kind::iso;
    s.path = iso;
    expect_failure("concurrent import", s, Status::error, opts_for(assets));
    CloseHandle(held);
    CHECK(!fs::exists(assets / L"win" / L"import.lock"));
  }
  if (!known_files().empty()) {
    // Synthetic bytes under a path the Deluxe manifest knows: verify fails,
    // and since the sizes differ too, before a byte is copied.
    Source s;
    s.kind = Source::Kind::iso;
    s.path = iso;
    ImportOptions o = opts_for(assets);
    o.check_known = true;
    bool copied = false;
    o.progress = [&](const Progress& p) {
      copied = copied || p.phase == Progress::Phase::copy;
      return true;
    };
    expect_failure("known-file mismatch", s, Status::verify_failed, o);
    CHECK(!copied);
  } else {
    fprintf(stderr, "  (known-file manifest is empty; mismatch check not exercised)\n");
  }

  // ---- Cancel after the swap is too late to cancel ---------------------------
  // The finalize(1/1) report comes after FILES and import.json are in place;
  // a Cancel there (the GUI's button pressed at the last moment) must not
  // turn a finished import into "cancelled", which the .scr reads as "nothing
  // changed".
  {
    fs::path la = dir / L"assets-late-cancel";
    Source s;
    s.kind = Source::Kind::iso;
    s.path = iso;
    ImportOptions o = opts_for(la);
    o.progress = [](const Progress& p) { return !(p.phase == Progress::Phase::finalize && p.done == p.total); };
    ImportResult r = run_import(s, o);
    CHECK_EQ(r.status, Status::ok);
    check_tree(r.files_dir, want);
    CHECK(fs::exists(r.import_json));
    // ... while a Cancel at finalize(0/1), before the first rename, still is.
    o.progress = [](const Progress& p) { return !(p.phase == Progress::Phase::finalize && p.done == 0); };
    std::string json = test::read_text(r.import_json);
    test::write_bytes(r.catalog, {'m', 'a', 'r', 'k'});
    CHECK_EQ(run_import(s, o).status, Status::cancelled);
    CHECK(test::read_text(r.import_json) == json);
    CHECK(test::read_text(r.catalog) == "mark");
    CHECK(no_leftovers(la / L"win"));
  }

  // ---- the catalog lists the modules of the tree it was installed with -------
  {
    // The fixture as a folder, plus modules the catalog can read: an AD4
    // module, the MSVC one beside the engine, and a Classic one. The
    // fixture's own *.AD files are noise and are left out (and logged).
    fs::path cd = dir / L"cd-with-modules";
    test::write_fixture_folder(cd, fixture, false);
    test::PeSpec pe;
    pe.imports = {"ADXPL510.DLL", "KERNEL32.DLL"};
    pe.exports = {"Module"};
    pe.resources = {{16, "", 1, 0x409, test::version_resource({{"FileDescription", "Synthetic Four"}})},
                    {1000, "", 1, 0x409, test::checkbox_record("Clear Screen", 1)}};
    std::string ad4 = test::build_pe(pe);
    pe.exports = {"_Module@4"};
    pe.imports = {"KERNEL32.DLL"};
    std::string msvc = test::build_pe(pe);
    test::NeSpec ne;
    ne.module_refs = {"KERNEL", "ADXPL300"};
    ne.exports = {"MODULE"};
    ne.resources = {{2000, "", 20, std::string("Synthetic Three") + '\0'},
                    {1000, "", 1, test::popup_record("Mode", {"A", "B"}, 1)}};
    std::string classic = test::build_ne(ne);
    auto put = [&](const wchar_t* rel, const std::string& data) {
      test::write_bytes(cd / L"ADE" / L"FILES" / rel, std::vector<uint8_t>(data.begin(), data.end()));
    };
    put(L"AD40\\SYNTH4.AD", ad4);
    put(L"ENGINE\\STARRYNI.AD", msvc);
    put(L"CLASSIC\\SYNTH3.AD", classic);
    fs::path ma = dir / L"assets-modules";
    Source s;
    s.kind = Source::Kind::folder;
    s.path = cd;
    std::vector<std::string> logs;
    ImportOptions o = opts_for(ma);
    o.log = [&](const std::string& l) { logs.push_back(l); };
    ImportResult r = run_import(s, o);
    CHECK_EQ(r.status, Status::ok);
    CHECK_EQ(r.catalog_modules, size_t(3));
    std::string text = test::read_text(r.catalog);
    // Describes the installed tree (PACKAGES.md §6: with the packages array).
    CatalogTree tree;
    tree.package = find_package("deluxe");
    tree.dir = r.files_dir;
    tree.verified = r.verified;
    tree.imported_utc = phosg::JSON::parse(test::read_text(r.import_json)).get_string("importedUtc");
    CHECK_EQ(text, render_catalog(build_catalog({tree})));
    phosg::JSON cat = phosg::JSON::parse(text);
    const auto& mods = cat.at("modules").as_list();
    CHECK_EQ(mods.size(), size_t(3));
    if (mods.size() == 3) {
      CHECK_EQ(mods[0]->get_string("id"), std::string("ad40.synth4"));
      CHECK_EQ(mods[0]->get_string("path"), std::string("FILES/AD40/SYNTH4.AD"));
      CHECK_EQ(mods[0]->get_string("displayName"), std::string("Synthetic Four"));
      CHECK_EQ(mods[1]->get_string("id"), std::string("ad40.starryni"));
      CHECK_EQ(mods[1]->get_string("entry"), std::string("_Module@4"));
      CHECK_EQ(mods[2]->get_string("id"), std::string("classic.synth3"));
      CHECK_EQ(mods[2]->get_string("lane"), std::string("ne16"));
      CHECK_EQ(mods[2]->at("controls").as_list().size(), size_t(1));
    }
    size_t skipped = 0;
    for (auto& l : logs) skipped += l.find("catalog: skipped") != std::string::npos;
    CHECK_EQ(skipped, size_t(1 + 80));   // FLYINGTO.AD + the 80 CLASSIC/F0xx.AD placeholders
    CHECK(no_leftovers(ma / L"win"));
  }

  // ---- recovery from an import that died part-way ----------------------------
  auto cancel_in_copy = [](ImportOptions o) {
    o.progress = [](const Progress& p) { return p.phase != Progress::Phase::copy; };
    return o;
  };
  {
    // Died between its two renames: no FILES, the installed tree parked in
    // FILES.old-N. The next import (here: one that is then cancelled) must
    // put it back, not sweep away the only copy.
    fs::path ca = dir / L"assets-crash-mid-swap";
    Source s;
    s.kind = Source::Kind::iso;
    s.path = iso;
    CHECK_EQ(run_import(s, opts_for(ca)).status, Status::ok);
    fs::path win = ca / L"win";
    std::string json = test::read_text(win / L"import.json");
    // Retried: a virus scanner may still hold the just-written files.
    bool moved = false;
    for (int i = 0; i < 50 && !moved; i++)
      if (!(moved = MoveFileExW((win / L"FILES").c_str(), (win / L"FILES.old-424242").c_str(), 0))) Sleep(100);
    CHECK(moved);
    test::write_bytes(win / L"FILES.importing-424242" / L"AD40" / L"PARTIAL.AD", {1});
    test::write_bytes(win / L"import.json.tmp-424242", {'{', '}'});
    ImportResult r = run_import(s, cancel_in_copy(opts_for(ca)));
    CHECK_EQ(r.status, Status::cancelled);
    check_tree(win / L"FILES", want);
    CHECK(test::read_text(win / L"import.json") == json);
    CHECK(no_leftovers(win));
  }
  {
    // Died after installing its tree but before renaming its import.json:
    // FILES is the new tree, FILES.old-N and import.json.tmp-N remain and
    // the stage is gone. The next import completes the import.json rename.
    fs::path ca = dir / L"assets-crash-before-json";
    Source s;
    s.kind = Source::Kind::iso;
    s.path = iso;
    CHECK_EQ(run_import(s, opts_for(ca)).status, Status::ok);
    fs::path win = ca / L"win";
    test::write_bytes(win / L"FILES.old-434343" / L"AD40" / L"OLD.AD", {2});
    const std::string pending = "{\"pending\": true}";
    test::write_bytes(win / L"import.json.tmp-434343", std::vector<uint8_t>(pending.begin(), pending.end()));
    ImportResult r = run_import(s, cancel_in_copy(opts_for(ca)));
    CHECK_EQ(r.status, Status::cancelled);
    check_tree(win / L"FILES", want);
    CHECK(test::read_text(win / L"import.json") == pending);
    CHECK(no_leftovers(win));
  }
  {
    // Died after its import.json rename but before its catalog's: the
    // catalog tmp is completed too, so the catalog matches the tree.
    fs::path ca = dir / L"assets-crash-before-catalog";
    Source s;
    s.kind = Source::Kind::iso;
    s.path = iso;
    CHECK_EQ(run_import(s, opts_for(ca)).status, Status::ok);
    fs::path win = ca / L"win";
    std::string json = test::read_text(win / L"import.json");
    test::write_bytes(win / L"FILES.old-454545" / L"AD40" / L"OLD.AD", {2});
    const std::string pending = "{\"version\": 1, \"modules\": [\"pending\"]}";
    test::write_bytes(win / L"catalog-win.json.tmp-454545", std::vector<uint8_t>(pending.begin(), pending.end()));
    ImportResult r = run_import(s, cancel_in_copy(opts_for(ca)));
    CHECK_EQ(r.status, Status::cancelled);
    CHECK(test::read_text(win / L"catalog-win.json") == pending);
    CHECK(test::read_text(win / L"import.json") == json);
    CHECK(no_leftovers(win));
    // A catalog tmp next to a stage is a failed import's: deleted, not used.
    test::write_bytes(win / L"FILES.importing-464646" / L"AD40" / L"PARTIAL.AD", {1});
    test::write_bytes(win / L"catalog-win.json.tmp-464646", {'x'});
    CHECK_EQ(run_import(s, cancel_in_copy(opts_for(ca))).status, Status::cancelled);
    CHECK(test::read_text(win / L"catalog-win.json") == pending);
    CHECK(no_leftovers(win));
  }

  // ---- a recording time Windows cannot represent -----------------------------
  // (hour 30): the copy keeps its import time instead of a bogus 1601 date.
  {
    test::IsoBuilder tb;
    tb.rec_time[3] = 30;
    for (auto& f : fixture) tb.file(f.iso_path, f.data);
    test::write_bytes(dir / L"bad-time.iso", tb.build());
    Source s;
    s.kind = Source::Kind::iso;
    s.path = dir / L"bad-time.iso";
    ImportResult r = run_import(s, opts_for(dir / L"assets-bad-time"));
    CHECK_EQ(r.status, Status::ok);
    WIN32_FILE_ATTRIBUTE_DATA a{};
    GetFileAttributesExW((r.files_dir / L"AFI" / L"AD2.AFI").c_str(), GetFileExInfoStandard, &a);
    SYSTEMTIME st{};
    FileTimeToSystemTime(&a.ftLastWriteTime, &st);
    CHECK(st.wYear >= 2020);
  }

  // ---- AD_ASSETS_DIR is trimmed as adhostwin trims it -----------------------
  // Set, it is the whole answer; blank, the default is <data folder>\assets.
  // The data folder is <AD_LOCALAPPDATA>\LongAfterDark, else (AD_LOCALAPPDATA
  // blank) <LOCALAPPDATA>\LongAfterDark, as adhostwin and the .scr take it,
  // and working it out creates nothing.
  {
    fs::path want_root = dir / L"env-root";
    _wputenv_s(L"AD_ASSETS_DIR", (L"  " + want_root.wstring() + L" \t").c_str());
    CHECK_EQ(default_assets_root(), want_root);
    _wputenv_s(L"AD_ASSETS_DIR", L"   ");
    CHECK_EQ(data_folder(), lad / L"LongAfterDark");
    CHECK_EQ(default_assets_root(), lad / L"LongAfterDark" / L"assets");  // blank = the data folder's
    CHECK_EQ(default_download_dir(), lad / L"LongAfterDark" / L"downloads");
    _wputenv_s(L"AD_ASSETS_DIR", L"");
    // Trimmed, with a trailing separator.
    test::set_env(adw::kDataRootBaseVar, L" " + lad.wstring() + L"\\ ");
    CHECK_EQ(data_folder(), lad / L"LongAfterDark");
    // Blank: LOCALAPPDATA (only the environment is read, so a scratch value
    // stands in for the real one here).
    const std::wstring localappdata = test::get_env(L"LOCALAPPDATA");
    const fs::path other = dir / L"other-localappdata";
    test::set_env(L"LOCALAPPDATA", other.wstring());
    test::set_env(adw::kDataRootBaseVar, L"  ");
    CHECK_EQ(data_folder(), other / L"LongAfterDark");
    CHECK_EQ(default_download_dir(), other / L"LongAfterDark" / L"downloads");
    test::set_env(L"LOCALAPPDATA", localappdata);
    test::sandbox_data_root(lad);
    CHECK(!fs::exists(lad) && !fs::exists(other));
  }

  // ---- the records are UTF-8 whatever they are given ---------------------------
  // utf8.h's utf8_sequence_length draws the line Windows' strict decoder
  // draws: every string of one or two bytes, every three-byte string that
  // starts past 0xBF, four-byte strings around every edge. to_valid_utf8
  // keeps UTF-8 as it is and makes U+FFFD of every other byte; json_escape
  // (every string of import.json) writes what phosg reads back as the input,
  // or as the input so repaired, and never a byte that is not UTF-8.
  {
    auto hex = [](const std::string& s) {
      std::string h;
      char b[4];
      for (unsigned char c : s) snprintf(b, sizeof(b), "%02x", c), h += b;
      return h;
    };
    size_t tried = 0, differ = 0;
    auto against_windows = [&](const std::string& s) {
      tried++;
      if (is_utf8(s) != test::strict_utf8(s) && differ++ < 10)
        fprintf(stderr, "  is_utf8(%s) differs from Windows' decoder\n", hex(s).c_str());
    };
    for (int a = 0; a < 256; a++) {
      against_windows(std::string(1, char(a)));
      for (int b2 = 0; b2 < 256; b2++) {
        against_windows({char(a), char(b2)});
        if (a >= 0xC0)
          for (int c = 0; c < 256; c++) against_windows({char(a), char(b2), char(c)});
        if (a >= 0xF0)
          for (int c : {0x00, 0x7F, 0x80, 0x9F, 0xA0, 0xBF, 0xC0, 0xFF})
            for (int d : {0x00, 0x7F, 0x80, 0x8F, 0x90, 0xBF, 0xC0, 0xFF}) against_windows({char(a), char(b2), char(c), char(d)});
      }
    }
    fprintf(stderr, "  UTF-8: %zu strings against Windows' decoder, %zu differ\n", tried, differ);
    CHECK(tried > 4000000);
    CHECK_EQ(differ, size_t(0));
    // The edges one by one: the first and last code point of each length,
    // around the surrogates; overlong forms, surrogates, past U+10FFFF, bytes
    // that never start a sequence, a sequence cut short — also where the
    // string ends inside a sequence whose next bytes lie just past it.
    struct Edge {
      std::string_view bytes;
      size_t length;
    };
    for (const Edge& e : std::initializer_list<Edge>{
             {"\x7F", 1}, {"\xC2\x80", 2}, {"\xDF\xBF", 2}, {"\xE0\xA0\x80", 3}, {"\xED\x9F\xBF", 3}, {"\xEE\x80\x80", 3},
             {"\xEF\xBF\xBF", 3}, {"\xF0\x90\x80\x80", 4}, {"\xF4\x8F\xBF\xBF", 4}, {"\xC0\xAF", 0}, {"\xC1\xBF", 0},
             {"\xE0\x9F\xBF", 0}, {"\xED\xA0\x80", 0}, {"\xF0\x8F\xBF\xBF", 0}, {"\xF4\x90\x80\x80", 0},
             {"\xF5\x80\x80\x80", 0}, {"\x80", 0}, {"\xFF", 0}, {"\xE2\x82", 0}, {"\xC3\x28", 0},
             {std::string_view("\xE2\x82\xAC", 2), 0}, {std::string_view("\xF0\x9F\x98\x80", 3), 0},
             {std::string_view("\xC3\xBC", 1), 0}}) {
      if (utf8_sequence_length(e.bytes, 0) != e.length) {
        test::g_failures++;
        fprintf(stderr, "  utf8_sequence_length(%s) = %zu, not %zu\n", hex(std::string(e.bytes)).c_str(),
                utf8_sequence_length(e.bytes, 0), e.length);
      }
    }
    CHECK_EQ(to_valid_utf8("M\xC3\xBCSIK \xE2\x82\xAC \xF0\x9F\x98\x80"), std::string("M\xC3\xBCSIK \xE2\x82\xAC \xF0\x9F\x98\x80"));
    CHECK_EQ(to_valid_utf8("DISK\x81\x94" "1.IMG"), std::string("DISK\xEF\xBF\xBD\xEF\xBF\xBD" "1.IMG"));
    // An overlong '/' and a cut sequence: a U+FFFD for each byte, never a '/'.
    CHECK_EQ(to_valid_utf8("\xC0\xAF\xE2\x82"), std::string("\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD"));
    CHECK_EQ(json_escape("q\"b\\s\n\r\t\x01\x1F\x7F"), std::string("q\\\"b\\\\s\\n\\r\\t\\u0001\\u001f\x7F"));
    CHECK_EQ(json_escape("C:\\x.zip!DISK\x81\x94" "1.IMG"), std::string("C:\\\\x.zip!DISK\xEF\xBF\xBD\xEF\xBF\xBD" "1.IMG"));
    CHECK_EQ(json_escape("TREK\xC3\xBC.IMG"), std::string("TREK\xC3\xBC.IMG"));
    size_t bad = 0, strings = 0;
    auto round_trip = [&](const std::string& in) {
      strings++;
      const std::string esc = json_escape(in);
      std::string back = "(no parse)";
      try {
        back = phosg::JSON::parse("\"" + esc + "\"").as_string();
      } catch (const std::exception&) {
      }
      const std::string want = test::strict_utf8(in) ? in : to_valid_utf8(in);
      if ((!test::strict_utf8(esc) || back != want) && bad++ < 10)
        fprintf(stderr, "  json_escape(%s) = %s\n", hex(in).c_str(), hex(esc).c_str());
    };
    for (int a = 0; a < 256; a++) {
      round_trip(std::string(1, char(a)));
      for (int b2 = 0; b2 < 256; b2++) round_trip({char(a), char(b2)});
    }
    for (uint32_t k = 0; k < 4000; k++) {
      auto p = test::pattern(1 + k % 40, k);
      round_trip(std::string(p.begin(), p.end()));
    }
    fprintf(stderr, "  json_escape: %zu strings read back, %zu wrong\n", strings, bad);
    CHECK_EQ(bad, size_t(0));
  }

  // ---- the built-in manifest is well formed ----------------------------------
  // (Its content is checked against the real image by import.e2e.)
  {
    std::set<std::string> paths;
    for (const KnownFile& k : known_files()) {
      std::string p = k.path;
      CHECK(p.rfind("FILES/AD40/", 0) == 0 || p.rfind("FILES/CLASSIC/", 0) == 0 ||
            p.rfind("FILES/ENGINE/", 0) == 0 || p.rfind("FILES/AFI/", 0) == 0);
      CHECK(p == ascii_upper_copy(p));
      CHECK(paths.insert(p).second);
      std::string m = k.md5;
      CHECK(m.size() == 32 && m.find_first_not_of("0123456789abcdef") == std::string::npos);
    }
    CHECK(std::is_sorted(paths.begin(), paths.end()));
    if (!paths.empty())
      for (const char* req : kRequiredFiles) CHECK(paths.count(req));
    fprintf(stderr, "  manifest: %zu known files\n", paths.size());
  }
  return test::finish("import.import");
}
