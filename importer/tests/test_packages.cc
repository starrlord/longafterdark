// Multi-package imports on synthetic sources (PACKAGES.md §9, tests/
// pkg_fixture.h):
//   identification   every release as a folder, an ISO and a FAT image (Star
//                    Wars Screen Entertainment also as five floppies, in
//                    either order, and a flat ZIP; Star Trek: The Screen
//                    Saver as two floppies in either order, the ZIP of
//                    them, a flat ZIP and an ISO; the Looney Tunes,
//                    ScreamSavers and the Disney Collection as a folder and
//                    a flat ZIP, ScreamSavers also as three floppies in any
//                    order and in DISK1-DISK3 folders, loose or zipped, each
//                    disk alone; Marvel Comics Screen Posters and Snoopy's
//                    Screen Savers as a folder, a flat ZIP, DISK1/DISK2
//                    folders loose or zipped and two floppies in either
//                    order, each disk alone; SETUP.PKG's tag member, size and
//                    damage, disk 1's library volume beside it, a volume
//                    naming one past the registry's); unknown and ambiguous
//                    sources; the ad3zip
//                    fingerprint (engine library, folder file and 3.2's
//                    marker: ScreamSavers never taken for 3.2, a source that
//                    is both ambiguous); --package; image md5s (a ZIP's too:
//                    the releases known by the ZIP of their install files)
//                    and known sets of install
//                    disks (complete, a ZIP's image given before it read once;
//                    one disk alone, two copies of one; every disk with a
//                    second copy of one or a stranger, logged as such); ZIP
//                    member names and a volume label in code page 437 (two
//                    names that differ in a letter outside ASCII are two; the
//                    record is UTF-8); Presage's INSTALL.DAT; Microsoft
//                    Setup's SETUP.LST
//   recipes          exact file sets, fix-ups only from matching sources,
//                    the §4.2 invariants (and the intermission, ad2kwaj and
//                    islib recipes' own), required files and archives (every
//                    install disk), the derived password (never written or
//                    logged), I5 (the owners' notes, the Disney Collection's
//                    BEAUTYOL.ZIP and swse's, startrek's, marvel's and
//                    snoopy's decoys never opened, an AD 3.x install's
//                    SETUP.PKG never read; ARJ volume chains that point past
//                    the registry's archives refused, the volume they name
//                    never opened; other .ARJ skipped; library members the
//                    table does not name never decoded), damaged ARJ, SZDD,
//                    KWAJ files and InstallShield libraries (a KWAJ file cut
//                    to a clean prefix refused before a byte is written; a
//                    DCL literal changed: 3), names that differ only in a
//                    non-ASCII letter's case (one file to Windows) refused
//   atomicity        a package import leaves Deluxe and every other package
//                    byte for byte; re-imports replace only their package;
//                    failures and cancels change nothing
//   recovery         every interrupted swap and removal state
//   catalog          ids, order, the displayName rule, overrides, trimming,
//                    sameAs, the packages array, Deluxe entries unchanged,
//                    After Dark 2.0's About texts and "screen" (startrek,
//                    marvel and screams only)
//   commands         --catalog-only without FILES, --remove, --list-packages,
//                    and adimport.exe's options and exit codes
//
//   test_import_packages <adimport.exe> <scratch>
#include <phosg/JSON.hh>

#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <tuple>

#include "catalog.h"
#include "importer.h"
#include "md5.h"
#include "names.h"
#include "pkg_fixture.h"
#include "run_process.h"

using namespace adw::import;
namespace fs = std::filesystem;

namespace {

std::vector<std::string> g_log;

ImportOptions opts_for(const fs::path& root, const test::TestRegistry& reg, bool check = true) {
  ImportOptions o;
  o.assets_root = root;
  o.check_known = check;
  o.registry = reg.span();
  o.log = [](const std::string& s) {
    g_log.push_back(s);
    fprintf(stderr, "  log: %s\n", s.c_str());
  };
  return o;
}

Source folder(const fs::path& p, const std::string& package = "") {
  Source s;
  s.kind = Source::Kind::folder;
  s.path = p;
  s.package = package;
  return s;
}

Source image(const fs::path& p, std::vector<fs::path> more = {}, const std::string& package = "") {
  Source s;
  s.kind = Source::Kind::image;
  s.path = p;
  s.more_images = std::move(more);
  s.package = package;
  return s;
}

ImportResult run(const char* what, const Source& s, const ImportOptions& o, Status want) {
  ImportResult r = run_import(s, o);
  fprintf(stderr, "[%s] %s: %s\n", what, status_name(r.status), r.message.c_str());
  CHECK_EQ(r.status, want);
  return r;
}

// Every file under `dir`: relative path -> bytes.
test::Tree snapshot(const fs::path& dir) {
  test::Tree t;
  for (const std::string& rel : test::list_tree(dir)) t[rel] = test::read_bytes(test::path_under(dir, rel));
  return t;
}

// The same without the package's import record (whose importedUtc a
// re-import of identical files changes).
test::Tree snapshot_files(const fs::path& dir) {
  test::Tree t = snapshot(dir);
  t.erase("import.json");
  return t;
}

// The installed files of a package (relative to <win>), import.json aside.
void check_installed(const fs::path& win, const test::PkgFixture& f, const std::string& root) {
  test::Tree have;
  for (auto& [rel, d] : snapshot(test::path_under(win, root)))
    if (rel != "import.json") have[root + "/" + rel] = d;
  test::Tree want;
  for (auto& [rel, d] : f.expect)
    if (rel.rfind(root + "/", 0) == 0) want[rel] = d;
  for (auto& [rel, d] : want) {
    auto it = have.find(rel);
    if (it == have.end()) {
      test::g_failures++;
      fprintf(stderr, "  missing: %s\n", rel.c_str());
    } else if (it->second != d) {
      test::g_failures++;
      fprintf(stderr, "  differs: %s\n", rel.c_str());
    }
  }
  for (auto& [rel, d] : have)
    if (!want.count(rel)) {
      test::g_failures++;
      fprintf(stderr, "  unexpected: %s\n", rel.c_str());
    }
}

phosg::JSON json_at(const fs::path& p) { return phosg::JSON::parse(test::read_text(p)); }

std::vector<std::string> catalog_ids(const fs::path& win) {
  std::vector<std::string> ids;
  phosg::JSON cat = json_at(win / L"catalog-win.json");
  for (auto& m : cat.at("modules").as_list()) ids.push_back(m->get_string("id"));
  return ids;
}

const phosg::JSON* module_by_id(const phosg::JSON& cat, const std::string& id) {
  for (auto& m : cat.at("modules").as_list())
    if (m->get_string("id") == id) return m.get();
  return nullptr;
}

std::vector<std::string> concat(std::initializer_list<std::vector<std::string>> parts) {
  std::vector<std::string> out;
  for (auto& p : parts) out.insert(out.end(), p.begin(), p.end());
  return out;
}

bool no_leftovers(const fs::path& win) {
  std::error_code ec;
  bool ok = true;
  auto scan = [&](const fs::path& d) {
    for (auto& e : fs::directory_iterator(d, ec)) {
      std::wstring n = e.path().filename().wstring();
      if (n.find(L".importing-") != std::wstring::npos || n.find(L".old-") != std::wstring::npos ||
          n.find(L".removing-") != std::wstring::npos || n.find(L".tmp-") != std::wstring::npos || n == L"import.lock") {
        fprintf(stderr, "  leftover: %s\n", to_utf8(e.path().wstring()).c_str());
        ok = false;
      }
    }
  };
  scan(win);
  scan(win / L"packages");
  return ok;
}

bool logged(const std::string& needle) {
  for (auto& l : g_log)
    if (l.find(needle) != std::string::npos) return true;
  return false;
}

// Nothing the importer wrote or said holds the archive password.
void check_no_password(const fs::path& root) {
  for (auto& l : g_log) CHECK(l.find(test::kTestZipPassword) == std::string::npos);
  for (const std::string& rel : test::list_tree(root)) {
    auto b = test::read_bytes(test::path_under(root, rel));
    std::string s(b.begin(), b.end());
    if (s.find(test::kTestZipPassword) != std::string::npos) {
      test::g_failures++;
      fprintf(stderr, "  the password is in %s\n", rel.c_str());
    }
  }
}

std::string with_zip(test::PkgFixture& f, const std::string& path,
                     const std::vector<std::pair<std::string, std::vector<uint8_t>>>& members) {
  f.source[path] = test::zip_of(members);
  return path;
}

// A registry whose manifests describe the fixtures exactly (so a folder or
// ISO source verifies "files" and the ad10 fix-ups apply).
test::TestRegistry registry_for(const std::map<std::string, const test::PkgFixture*>& fx) {
  test::TestRegistry reg;
  for (auto& [id, f] : fx) reg.manifest(id, test::manifest_of(f->expect));
  return reg;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: test_import_packages <adimport.exe> [scratch]\n");
    return 2;
  }
  std::wstring exe = fs::absolute(argv[1]).wstring();
  fs::path dir = test::scratch(argc - 1, argv + 1, "adw-import-packages");
  test::sandbox_data_root(dir / L"localappdata");  // no default may reach the real data folder

  const test::PkgFixture deluxe = test::deluxe_fixture(), ad10 = test::ad10_fixture(), ad32 = test::ad32_fixture(),
                         tt = test::tt_fixture(), simpsons = test::simpsons_fixture(), swse = test::swse_fixture(),
                         startrek = test::startrek_fixture(), looney = test::looney_fixture(),
                         screams = test::screams_fixture(), disney = test::disney_fixture(),
                         farside = test::farside_fixture(), dilbert = test::dilbert_fixture();
  const test::IslibFixture marvel = test::marvel_fixture(), snoopy = test::snoopy_fixture();
  test::TestRegistry reg = registry_for({{"deluxe", &deluxe},
                                         {"ad10", &ad10},
                                         {"ad32", &ad32},
                                         {"tt", &tt},
                                         {"simpsons", &simpsons},
                                         {"swse", &swse},
                                         {"startrek", &startrek},
                                         {"marvel", &marvel},
                                         {"snoopy", &snoopy},
                                         {"looney", &looney},
                                         {"screams", &screams},
                                         {"disney", &disney},
                                         {"farside", &farside},
                                         {"dilbert", &dilbert}});

  // ---- the registry's box covers (COVERS.md §2.2, §2.3) ----------------------------------------------
  {
    auto hex32 = [](const char* s) {
      return s && strlen(s) == 32 && std::string_view(s).find_first_not_of("0123456789abcdef") == std::string::npos;
    };
    std::set<std::wstring> names;
    for (const Package& p : builtin_packages()) {
      CHECK(!p.covers.empty());  // every package has at least one cover source
      for (const CoverSource& s : p.covers) {
        const std::string art = s.art ? s.art : "";
        CHECK(art == "box" || art == "disc" || art == "splash" || art == "panel");
        CHECK(s.label && *s.label && s.credit && *s.credit);
        CHECK(s.crop.x >= 0 && s.crop.y >= 0 && s.crop.w >= 0 && s.crop.h >= 0);
        CHECK((s.crop.w == 0) == (s.crop.h == 0));
        if (s.kind == CoverSource::Kind::download) {
          CHECK(s.url && std::string_view(s.url).rfind("https://", 0) == 0);
          CHECK(hex32(s.md5));
          CHECK(s.size > 0);
          CHECK(s.file_name && *s.file_name);
          CHECK(s.file_name && names.insert(s.file_name).second);  // unique: <download dir>\covers\<file_name>
          CHECK(!s.path && !s.resource_type);
        } else {
          CHECK(s.path && *s.path && std::string_view(s.path).find('\\') == std::string_view::npos);
          CHECK(s.path_md5 && (!*s.path_md5 || hex32(s.path_md5)));
          CHECK(s.resource_type == 0 || (s.resource_type == 2 && s.resource_id != 0));
          CHECK(!s.url);
        }
      }
    }
    // The decisions COVERS.md §2.3 records: Deluxe, Totally Twisted and the
    // Simpsons start with a box front (a download), 3.2 with its disc art;
    // the installer splashes are cropped above their warning text; the
    // Simpsons' splash is SETUP.EXE's bitmap 7500.
    auto first_disc = [](const char* id) -> const CoverSource* {
      for (const CoverSource& s : find_package(id)->covers)
        if (s.kind == CoverSource::Kind::disc) return &s;
      return nullptr;
    };
    for (const char* id : {"deluxe", "tt", "simpsons"}) {
      CHECK(find_package(id)->covers[0].kind == CoverSource::Kind::download);
      CHECK_EQ(std::string(find_package(id)->covers[0].art), std::string("box"));
    }
    CHECK(find_package("ad32")->covers[0].kind == CoverSource::Kind::disc);
    CHECK_EQ(find_package("ad32")->covers[0].crop.h, 183);
    CHECK(first_disc("tt") && first_disc("tt")->crop.h == 204);
    // Deluxe's portrait setup art ranks above the disc-label scan.
    CHECK_EQ(std::string(find_package("deluxe")->covers[1].path), std::string("ADE/PAGE1.BMP"));
    const CoverSource& splash = find_package("simpsons")->covers.back();
    CHECK(splash.kind == CoverSource::Kind::disc && splash.resource_type == 2 && splash.resource_id == 7500);
    CHECK_EQ(splash.crop.h, 172);
    CHECK_EQ(std::string(find_package("simpsons")->covers[0].art), std::string("box"));
    // Star Wars Screen Entertainment: two box fronts, then two disc labels,
    // the second cropped to the disc; nothing on the disc (every picture is
    // inside the ARJ archives).
    const Package* sw = find_package("swse");
    CHECK(sw && sw->covers.size() == 4);
    if (sw && sw->covers.size() == 4) {
      for (const CoverSource& c : sw->covers) CHECK(c.kind == CoverSource::Kind::download);
      CHECK(std::string(sw->covers[0].art) == "box" && std::string(sw->covers[1].art) == "box");
      CHECK(std::string(sw->covers[3].art) == "disc");
      CHECK(sw->covers[3].crop.x == 20 && sw->covers[3].crop.y == 14 && sw->covers[3].crop.w == 1424 &&
            sw->covers[3].crop.h == 1424);
    }
    // Star Trek: The Screen Saver: the Internet Archive's box front, the same
    // at 600 dpi, then disk 1's label — drawn as a picture ("panel"), never
    // cut to a disc; nothing on the disks (every picture is KWAJ-compressed).
    const Package* st = find_package("startrek");
    CHECK(st && st->covers.size() == 3);
    if (st && st->covers.size() == 3) {
      for (const CoverSource& c : st->covers) CHECK(c.kind == CoverSource::Kind::download);
      CHECK(std::string(st->covers[0].art) == "box" && std::string(st->covers[1].art) == "box");
      CHECK_EQ(std::string(st->covers[0].md5), std::string("157eb04fcc9bdc0d4a831148ca6cc260"));
      CHECK_EQ(std::string(st->covers[2].art), std::string("panel"));
      CHECK_EQ(std::string(st->covers[2].label), std::string("Disk label"));
    }
    // The Looney Tunes and the Disney Collection: Berkeley's box front
    // through the Wayback Machine, then the installer splash (SETUP.BMP at
    // the copy's root) cropped above its warning text; ScreamSavers: the title
    // art in SETUP.EXE, bitmap 7500, cropped above its copyright block.
    for (const char* id : {"looney", "disney"}) {
      const Package* p = find_package(id);
      CHECK(p && p->covers.size() >= 2);
      if (!p || p->covers.size() < 2) continue;
      CHECK(p->covers[0].kind == CoverSource::Kind::download && std::string(p->covers[0].art) == "box");
      CHECK(std::string_view(p->covers[0].url).rfind("https://web.archive.org/web/1997", 0) == 0);
      CHECK(p->covers[1].kind == CoverSource::Kind::disc && std::string(p->covers[1].path) == "SETUP.BMP" &&
            std::string(p->covers[1].art) == "splash");
    }
    CHECK_EQ(find_package("looney")->covers[1].crop.h, 161);
    CHECK_EQ(find_package("disney")->covers[1].crop.h, 172);
    {
      const Package* sc = find_package("screams");
      CHECK(sc && sc->covers.size() == 1);
      if (sc && sc->covers.size() == 1) {
        const CoverSource& c = sc->covers[0];
        CHECK(c.kind == CoverSource::Kind::disc && std::string(c.path) == "SETUP.EXE" && c.resource_type == 2 &&
              c.resource_id == 7500);
        CHECK(c.crop.x == 0 && c.crop.y == 0 && c.crop.w == 350 && c.crop.h == 119);
      }
    }
  }

  // ---- the registry: the releases known by the ZIP of their install files ----------------------
  {
    // Registry order: the first seven keep their places (the GUI's command
    // ids go by it), the five after them in the order the plan gives, then
    // the two Delrina Intermission releases.
    std::vector<std::string> order;
    for (const Package& p : builtin_packages()) order.push_back(p.id);
    CHECK((std::vector<std::string>(order.begin(), order.begin() + std::min<size_t>(order.size(), 7)) ==
           std::vector<std::string>{"deluxe", "ad10", "ad32", "tt", "simpsons", "swse", "startrek"}));
    CHECK((order == std::vector<std::string>{"deluxe", "ad10", "ad32", "tt", "simpsons", "swse", "startrek", "marvel",
                                             "snoopy", "looney", "screams", "disney", "farside", "dilbert"}));
    for (const char* id : {"looney", "screams", "disney"}) CHECK(std::find(order.begin(), order.end(), id) != order.end());
    CHECK(std::find(order.begin(), order.end(), "looney") < std::find(order.begin(), order.end(), "screams"));
    CHECK(std::find(order.begin(), order.end(), "screams") < std::find(order.begin(), order.end(), "disney"));
    for (const Package& p : builtin_packages()) {
      if (p.recipe != Recipe::ad3zip) {
        CHECK(p.never_opened.empty());
        continue;
      }
      // The fingerprint's three names; 3.2 alone needs a marker.
      CHECK(p.engine_dll && *p.engine_dll && p.folder_afi && *p.folder_afi);
      CHECK_EQ(p.marker != nullptr, std::string(p.id) == "ad32");
      // An archive never opened is never one that must be there.
      for (const char* n : p.never_opened)
        for (const char* a : p.required_archives) CHECK(!iequals(n, a));
      // A known image is a disc, floppies, or the one ZIP of the install
      // files, and each "zip" download of such a ZIP is that known image.
      for (const Download& d : p.downloads) {
        bool known = false;
        for (const KnownImage& k : p.images) known = known || (std::string_view(k.md5) == d.md5 && k.size == d.size);
        if (std::string_view(d.kind) == "zip" && known) {
          const KnownImage* k = nullptr;
          for (const KnownImage& x : p.images)
            if (std::string_view(x.md5) == d.md5) k = &x;
          CHECK(k && std::string_view(k->medium).rfind("ZIP", 0) == 0);
        }
      }
    }
    // Each of the three is verified by its ZIP's md5 (the Internet Archive's
    // after-dark-collection copy, the user's file byte for byte), which is
    // also its download; the Looney Tunes' CD is known by md5, size and
    // volume id alone: its only copy online is named after a product serial,
    // so no URL of it appears anywhere.
    struct Zip {
      const char* id;
      const char* md5;
      uint64_t size;
      size_t modules_zips;
    };
    for (const Zip& z : std::vector<Zip>{{"looney", "642b358a4854c481fe99984b8452ceb5", 2900525, 13},
                                         {"screams", "37a47b25dd35b214f94f57b6a0c2bd02", 3453163, 15},
                                         {"disney", "2f38df15494728b5bc20d26c36ba84c7", 3560012, 16}}) {
      const Package* p = find_package(z.id);
      CHECK(p != nullptr);
      if (!p) continue;
      CHECK(!p->images.empty() && std::string(p->images[0].md5) == z.md5 && p->images[0].size == z.size &&
            std::string_view(p->images[0].medium).rfind("ZIP", 0) == 0 && p->images[0].disk == 0);
      CHECK(p->downloads.size() == 1 && std::string(p->downloads[0].md5) == z.md5 && p->downloads[0].size == z.size &&
            std::string(p->downloads[0].kind) == "zip");
      CHECK(std::string_view(p->downloads[0].url).rfind("https://archive.org/download/after-dark-collection/", 0) == 0);
      CHECK_EQ(p->required_archives.size(), z.modules_zips);
      CHECK(!p->manifest.empty());
    }
    const Package* lt = find_package("looney");
    CHECK(lt && lt->images.size() == 2);
    if (lt && lt->images.size() == 2) {
      CHECK(std::string(lt->images[1].md5) == "6ad72e19b2cf6fcb9e67427f8e600449" && lt->images[1].size == 6625280 &&
            std::string(lt->images[1].volume_id) == "LOONEY_T");
    }
    for (const Package& p : builtin_packages()) {
      for (const Download& d : p.downloads) {
        CHECK(std::string_view(d.url).find("ZQA") == std::string_view::npos);
        CHECK(std::wstring_view(d.file_name).find(L"ZQA") == std::wstring_view::npos);
      }
      for (const CoverSource& c : p.covers) CHECK(!c.url || std::string_view(c.url).find("ZQA") == std::string_view::npos);
    }
    // Only the Disney Collection has an archive it never opens, and only
    // ScreamSavers (of these) a fixed screen; the five names the Disney name
    // resources squeezed are spelt out.
    CHECK((find_package("disney")->never_opened.size() == 1 &&
           std::string(find_package("disney")->never_opened[0]) == "BEAUTYOL.ZIP"));
    CHECK(find_package("screams")->screen && std::string(find_package("screams")->screen) == "640x480");
    CHECK(!find_package("looney")->screen && !find_package("disney")->screen);
    CHECK_EQ(find_package("disney")->name_overrides.size(), size_t(5));
    CHECK(find_package("looney")->name_overrides.empty() && find_package("screams")->name_overrides.empty());
  }

  // ---- the registry: the InstallShield 2 packages (islib) ---------------------------------------
  {
    for (const Package& p : builtin_packages()) {
      if (p.recipe != Recipe::islib) {
        CHECK(!p.tag_library && !p.tag_member && p.library_members.empty());
        continue;
      }
      // The fingerprint: the tag member in the tag library, and disk 1's
      // library volume beside SETUP.PKG.
      CHECK(p.tag_library && *p.tag_library && p.tag_member && *p.tag_member);
      CHECK(!p.required_archives.empty() && p.module_dir && p.module_dirs.size() == 1 &&
            std::string_view(p.module_dirs[0]) == p.module_dir);
      CHECK(p.never_opened.empty() && p.fixups.empty() && p.loose_files.empty());
      // The table: each library one the recipe reads (every install disk's),
      // each place under the module folder or ENGINE, in 8.3 upper case,
      // once; the tag member one of its rows; and exactly the manifest's files.
      std::set<std::string> tos, manifest;
      bool tag = false;
      for (const LibraryMember& row : p.library_members) {
        bool listed = false;
        for (const char* a : p.required_archives) listed = listed || std::string_view(a) == row.library;
        CHECK(listed);
        const std::string to = row.to;
        CHECK(to.rfind(std::string(p.module_dir) + "/", 0) == 0 || to.rfind("ENGINE/", 0) == 0);
        CHECK(to == ascii_upper(to) && to.substr(to.rfind('/') + 1) == row.member);
        CHECK(tos.insert(to).second);
        tag = tag || iequals(row.member, p.tag_member);
      }
      CHECK(tag);
      for (const KnownFile& k : p.manifest) manifest.insert(std::string(k.path).substr(std::string(p.root).size() + 1));
      CHECK(tos == manifest);
      for (const char* r : p.required) CHECK(tos.count(r));
      // Known by the ZIPs of their install files, which are their downloads.
      CHECK(!p.images.empty() && !p.downloads.empty());
      for (const KnownImage& k : p.images) CHECK(std::string_view(k.medium).rfind("ZIP", 0) == 0 && k.disk == 0);
      for (const Download& d : p.downloads) {
        bool known = false;
        for (const KnownImage& k : p.images) known = known || (std::string_view(k.md5) == d.md5 && k.size == d.size);
        CHECK(known && std::string_view(d.kind) == "zip");
      }
    }
    const Package* mv = find_package("marvel");
    CHECK(mv && std::string(mv->title) == "Marvel Comics Screen Posters" && std::string(mv->short_title) == "Marvel");
    CHECK(mv && mv->recipe == Recipe::islib && std::string(mv->root) == "packages/marvel");
    CHECK(mv && std::string(mv->released) == "1993-12" && mv->screen && std::string(mv->screen) == "640x480");
    CHECK(mv && std::string(mv->tag_library) == "modules.lib" && std::string(mv->tag_member) == "MARVEL.AD");
    CHECK(mv && mv->library_members.size() == 64 && mv->manifest.size() == 64);
    CHECK((mv && std::vector<std::string>(mv->required_archives.begin(), mv->required_archives.end()) ==
                     std::vector<std::string>{"IMAGES.1", "IMAGES.2", "MODULES.LIB", "ENGINE.LIB", "WIN.LIB"}));
    CHECK((mv && std::vector<std::string>(mv->required.begin(), mv->required.end()) ==
                     std::vector<std::string>{"AFTERDRK/DECO.DLL", "AFTERDRK/MRVLIMAG/MRVLIMAG.ADC", "ENGINE/AD_SND.DLL"}));
    // Both static ZIPs, the flat one first (its item has the box photo).
    CHECK(mv && mv->images.size() == 2 && mv->downloads.size() == 2);
    if (mv && mv->images.size() == 2 && mv->downloads.size() == 2) {
      CHECK(std::string(mv->images[0].md5) == "4c608dbbeb34108b30ede88304912c94" && mv->images[0].size == 2039771);
      CHECK(std::string(mv->images[1].md5) == "6981b36abb04779a076466fabad3721c" && mv->images[1].size == 2046286);
      CHECK_EQ(std::string(mv->downloads[0].url),
               std::string("https://archive.org/download/afterdarkmarvelscreenposters/"
                           "After%20Dark%20-%20Marvel%20Screen%20Posters.zip"));
      CHECK_EQ(std::string(mv->downloads[1].url),
               std::string("https://archive.org/download/after-dark-collection/After%20Dark%20-%20Marvel%20Comics.zip"));
    }
    // The box photo, cropped to the box; then the magazine advertisement.
    CHECK(mv && mv->covers.size() == 2);
    if (mv && mv->covers.size() == 2) {
      const CoverSource& box = mv->covers[0];
      CHECK(box.kind == CoverSource::Kind::download && std::string(box.art) == "box" &&
            std::string(box.md5) == "1b9294c6bd03c5b14ed366cc652c8e7c");
      CHECK(box.crop.x == 28 && box.crop.y == 64 && box.crop.w == 1132 && box.crop.h == 1390);
      CHECK(mv->covers[1].kind == CoverSource::Kind::download && std::string(mv->covers[1].art) == "panel");
    }
    const Package* sn = find_package("snoopy");
    CHECK(sn && std::string(sn->title) == "Snoopy's Screen Savers" && std::string(sn->short_title) == "Snoopy");
    CHECK(sn && sn->recipe == Recipe::islib && std::string(sn->root) == "packages/snoopy" && !sn->screen);
    CHECK(sn && std::string(sn->released) == "1994-10" && sn->required.empty());
    CHECK(sn && std::string(sn->tag_library) == "AD_MODS.z" && std::string(sn->tag_member) == "IS_FLY.AD");
    CHECK(sn && sn->library_members.size() == 8 && sn->manifest.size() == 8);
    CHECK((sn && std::vector<std::string>(sn->required_archives.begin(), sn->required_archives.end()) ==
                     std::vector<std::string>{"AD_MODS.1", "AD_MODS.2"}));
    CHECK(sn && sn->images.size() == 1 && std::string(sn->images[0].md5) == "a712447e1c957767bdbca884cead02dc" &&
          sn->images[0].size == 1993700);
    CHECK(sn && sn->downloads.size() == 1 &&
          std::string(sn->downloads[0].url) ==
              "https://archive.org/download/after-dark-collection/After%20Dark%20-%20Snoopy.zip");
    // No box scan exists: the picture beside the installer's readme, on disk 1.
    CHECK(sn && sn->covers.size() == 1 && sn->covers[0].kind == CoverSource::Kind::disc &&
          std::string(sn->covers[0].path) == "AD_MODS.BMP" && std::string(sn->covers[0].art) == "panel");
    // Their modules are no other release's (the catalog gets no sameAs); only
    // Marvel's AD_SND 1.0 is Star Trek's, byte for byte.
    std::set<std::string> modules;
    for (const Package& p : builtin_packages())
      for (const KnownFile& k : p.manifest) {
        if (!ends_with_i(k.path, ".AD")) continue;
        if (p.recipe == Recipe::islib) CHECK(!modules.count(k.md5));
        modules.insert(k.md5);
      }
  }

  // Sources on disk.
  fs::path src = dir / L"src";
  test::write_tree(src / L"deluxe", deluxe.source);
  test::write_tree(src / L"ad10", ad10.source);
  test::write_tree(src / L"ad32", ad32.source);
  test::write_tree(src / L"tt", tt.source);
  test::write_tree(src / L"simpsons", simpsons.source);
  test::write_bytes(src / L"ad10.iso", test::iso_of(ad10, true, "AD10TH"));
  test::write_bytes(src / L"ad32.iso", test::iso_of(ad32, false, "ADW320_C"));
  test::write_bytes(src / L"tt.iso", test::iso_of(tt, false, "TTW320CD"));
  test::write_bytes(src / L"deluxe.iso", test::iso_of(deluxe, true, "AD_DELUXE"));
  test::write_bytes(src / L"simpsons.img", test::fat_of(simpsons.source, 0, "SERIAL.TXT"));
  test::write_bytes(src / L"disk1.img", test::fat_of(simpsons.source, 1, "SERIAL.TXT"));
  test::write_bytes(src / L"disk2.img", test::fat_of(simpsons.source, 2, "SERIAL.TXT"));
  // Star Wars Screen Entertainment: the folder, the CD (level 1, no Joliet,
  // as the real one), its five 1.44 MB install floppies and a flat ZIP.
  test::write_tree(src / L"swse", swse.source);
  test::write_bytes(src / L"swse.iso", test::iso_of(swse, false, "SWSE"));
  std::vector<fs::path> swse_disks;
  for (int k = 1; k <= 5; k++) {
    swse_disks.push_back(src / (L"swse-disk" + std::to_wstring(k) + L".img"));
    test::write_bytes(swse_disks.back(), test::fat_of(swse.source, k, "", test::swse_disk, test::FatBuilder::floppy144()));
  }
  test::write_bytes(src / L"swse.zip", test::zip_folder(swse.source));
  // Star Trek: The Screen Saver: the folder, its two 1.44 MB install
  // floppies, the same disks as another copy wrote them (other image bytes,
  // the same files), the Internet Archive's ZIP of the two images (disk 2
  // first, a label scan beside them), a flat ZIP and an ISO of the files.
  test::write_tree(src / L"startrek", startrek.source);
  const fs::path st1 = src / L"st-disk1.img", st2 = src / L"st-disk2.img";
  const fs::path st1b = src / L"st-disk1-copy.img", st2b = src / L"st-disk2-copy.img";
  auto st_disk = [&](int k, const std::string& label) {
    test::FatBuilder b = test::FatBuilder::floppy144();
    b.label = label;
    return test::fat_of(startrek.source, k, "", test::startrek_disk, std::move(b));
  };
  test::write_bytes(st1, st_disk(1, ""));
  test::write_bytes(st2, st_disk(2, ""));
  test::write_bytes(st1b, st_disk(1, "WIN9XCOPY"));
  test::write_bytes(st2b, st_disk(2, "WIN9XCOPY"));
  test::write_bytes(src / L"startrek-images.zip",
                    test::zip_of_images({{"st-disk2.img", test::read_bytes(st2)}, {"st-disk1.img", test::read_bytes(st1)}}));
  test::write_bytes(src / L"startrek.zip", test::zip_folder(startrek.source));
  test::write_bytes(src / L"startrek.iso", test::iso_of(startrek, false, "STARTREK"));
  // Marvel Comics Screen Posters and Snoopy's Screen Savers: each as a flat
  // folder and a flat ZIP of both disks' files, their two disks in
  // DISK1/DISK2 folders (a folder, and a ZIP as the user's copies are) and as
  // two floppies, and each disk's files alone.
  for (const auto& [name, f] : std::vector<std::pair<std::wstring, const test::IslibFixture*>>{{L"marvel", &marvel},
                                                                                                 {L"snoopy", &snoopy}}) {
    test::write_tree(src / name, f->source);
    test::write_bytes(src / (name + L".zip"), test::zip_folder(f->source));
    test::write_bytes(src / (name + L"-disks.zip"), test::zip_of_disks(f->disks()));
    for (int k = 1; k <= 2; k++) {
      test::write_tree(src / (name + L"-disks") / (L"Disk" + std::to_wstring(k)), f->disk_files(k));
      test::write_tree(src / (name + L"-disk" + std::to_wstring(k)), f->disk_files(k));
      test::write_bytes(src / (name + L"-disk" + std::to_wstring(k) + L".img"), test::floppy_of(f->disk_files(k)));
    }
  }
  // The Looney Tunes, ScreamSavers and the Disney Collection: each as a
  // folder and a flat ZIP of every disk's files with the owner's note (the
  // user's copies), and each disk's files alone; ScreamSavers also as its
  // three 1.44 MB floppies and in DISK1-DISK3 folders, as a folder and as a
  // ZIP (the user's copy's shape).
  test::write_tree(src / L"looney", looney.source);
  test::write_bytes(src / L"looney.zip", test::zip_folder(looney.source));
  test::write_tree(src / L"screams", screams.source);
  test::write_bytes(src / L"screams.zip", test::zip_folder(screams.source));
  test::write_disk_folders(src / L"screams-disks", screams.source, test::screams_disk, 3);
  test::write_bytes(src / L"screams-disks.zip", test::zip_disk_folders(screams.source, test::screams_disk, 3));
  std::vector<fs::path> sc_disks;
  for (int k = 1; k <= 3; k++) {
    sc_disks.push_back(src / (L"screams-disk" + std::to_wstring(k) + L".img"));
    test::write_bytes(sc_disks.back(),
                      test::fat_of(screams.source, k, "", test::screams_disk, test::FatBuilder::floppy144()));
  }
  test::write_tree(src / L"disney", disney.source);
  test::write_bytes(src / L"disney.zip", test::zip_folder(disney.source));
  for (int k = 1; k <= 3; k++) {
    test::write_tree(src / (L"disney-disk" + std::to_wstring(k)), test::disk_files(disney.source, k, test::disney_disk));
    test::write_tree(src / (L"screams-disk" + std::to_wstring(k)), test::disk_files(screams.source, k, test::screams_disk));
    if (k <= 2)
      test::write_tree(src / (L"looney-disk" + std::to_wstring(k)), test::disk_files(looney.source, k, test::looney_disk));
  }

  // ---- every release alone, in every source form ------------------------------------------------
  {
    fs::path root = dir / L"alone-deluxe";
    ImportResult r = run("deluxe folder", folder(src / L"deluxe"), opts_for(root, reg), Status::ok);
    CHECK_EQ(r.package_id, std::string("deluxe"));
    CHECK_EQ(r.files_dir, root / L"win" / L"FILES");
    CHECK_EQ(r.verified, std::string("files"));
    check_installed(root / L"win", deluxe, "FILES");
    phosg::JSON j = json_at(root / L"win" / L"import.json");
    CHECK_EQ(j.get_int("version"), int64_t(1));  // Deluxe's record stays version 1
    CHECK(!j.contains("package"));
    CHECK(catalog_ids(root / L"win") == deluxe.ids);
    CHECK(no_leftovers(root / L"win"));
  }
  {
    // A source that lacks one of the release's files (not a required one):
    // every file it has matches, but "files" would claim the whole release,
    // so it is "partial", and the missing one is listed.
    test::Tree partial = deluxe.source;
    partial.erase("ADE/FILES/AFI/AD2.AFI");
    test::write_tree(src / L"deluxe-partial", partial);
    fs::path root = dir / L"partial-deluxe";
    ImportResult r = run("deluxe folder, one file missing", folder(src / L"deluxe-partial"), opts_for(root, reg), Status::ok);
    CHECK_EQ(r.verified, std::string("partial"));
    CHECK(r.missing_known == std::vector<std::string>{"FILES/AFI/AD2.AFI"});
    phosg::JSON j = json_at(root / L"win" / L"import.json");
    CHECK_EQ(j.get_string("verified"), std::string("partial"));
    CHECK_EQ(j.at("missingKnown").as_list().size(), size_t(1));
    CHECK(no_leftovers(root / L"win"));
  }
  {
    // A file of another size under a path the manifest lists: refused before
    // anything is copied (it could only fail the verification later).
    test::Tree bigger = deluxe.source;
    bigger["ADE/FILES/AD40/TOASTERS.MID"].resize(bigger["ADE/FILES/AD40/TOASTERS.MID"].size() + 100000, 0x5A);
    test::write_tree(src / L"deluxe-bigger", bigger);
    fs::path root = dir / L"bigger-deluxe";
    ImportOptions o = opts_for(root, reg);
    bool copied = false;
    o.progress = [&](const Progress& p) {
      copied = copied || p.phase == Progress::Phase::copy;
      return true;
    };
    ImportResult r = run("deluxe folder, a file of another size", folder(src / L"deluxe-bigger"), o, Status::verify_failed);
    CHECK(r.message.find("FILES/AD40/TOASTERS.MID") != std::string::npos);
    CHECK(!copied);
    CHECK(!fs::exists(root / L"win" / L"FILES"));
    CHECK(no_leftovers(root / L"win"));
    // --no-verify imports it as it is.
    o = opts_for(root, reg, false);
    r = run("the same with --no-verify", folder(src / L"deluxe-bigger"), o, Status::ok);
    CHECK_EQ(r.verified, std::string("none"));
  }
  {
    fs::path root = dir / L"alone-ad10";
    ImportResult r = run("ad10 iso", image(src / L"ad10.iso"), opts_for(root, reg), Status::ok);
    CHECK_EQ(r.package_id, std::string("ad10"));
    CHECK_EQ(r.format, std::string("iso9660+joliet"));
    CHECK_EQ(r.files_dir, root / L"win" / L"packages" / L"ad10");
    CHECK_EQ(r.verified, std::string("files"));
    CHECK_EQ(r.package_modules, size_t(7));
    check_installed(root / L"win", ad10, "packages/ad10");
    CHECK(!fs::exists(root / L"win" / L"FILES"));
    CHECK(catalog_ids(root / L"win") == ad10.ids);
    phosg::JSON j = json_at(r.import_json);
    CHECK_EQ(j.get_int("version"), int64_t(2));
    CHECK_EQ(j.at("package").get_string("id"), std::string("ad10"));
    CHECK_EQ(j.at("package").get_string("recipe"), std::string("tree"));
    CHECK_EQ(j.at("source").get_string("kind"), std::string("iso"));
    CHECK_EQ(j.at("source").get_string("volumeId"), std::string("AD10TH"));
    CHECK_EQ(j.at("source").get_string("imageMd5"), md5_file_hex(src / L"ad10.iso"));
    CHECK_EQ(j.at("source").get_bool("imageMd5Known"), false);
    std::map<std::string, std::string> from;
    for (auto& f : j.at("files").as_list()) from[f->get_string("path")] = f->get_string("from");
    CHECK_EQ(from["packages/ad10/AD10TH/MUSIC/Toasters2k.mid"], std::string("alias:AD10TH/TOASTER1.MID"));
    CHECK_EQ(from["packages/ad10/AD10TH/TT_SND.DLL"], std::string("alias:AD10TH/MUSIC/TT_SND.DLL"));
    CHECK_EQ(from["packages/ad10/AD10TH/PICTURES/SOMEPICT.BMP"], std::string("ADE/FILES/AD10TH/PICTURES/SOMEPICT.BMP"));
    for (auto& f : j.at("files").as_list()) CHECK_EQ(f->get_string("known"), std::string("match"));
    // The early Toasters 2k build has its override; the installed one keeps the name.
    phosg::JSON cat = json_at(root / L"win" / L"catalog-win.json");
    CHECK_EQ(module_by_id(cat, "ad10.toast2k")->get_string("displayName"), std::string("Toasters 2k (early build)"));
    CHECK_EQ(module_by_id(cat, "ad10.toaster2")->get_string("displayName"), std::string("Toasters 2k"));
    CHECK_EQ(module_by_id(cat, "ad10.baddog")->get_string("lane"), std::string("pe32"));
    CHECK_EQ(module_by_id(cat, "ad10.baddog3")->get_string("lane"), std::string("ne16"));
    CHECK_EQ(module_by_id(cat, "ad10.baddog3")->get_string("displayName"), std::string("Bad Dog!"));  // other lane
    CHECK_EQ(module_by_id(cat, "ad10.starryni")->get_string("path"), std::string("packages/ad10/ENGINE/STARRYNI.AD"));
    CHECK(no_leftovers(root / L"win"));
  }
  for (bool as_iso : {false, true}) {
    fs::path root = dir / (as_iso ? L"alone-ad32-iso" : L"alone-ad32-folder");
    g_log.clear();
    ImportResult r = run(as_iso ? "ad32 iso" : "ad32 folder", as_iso ? image(src / L"ad32.iso") : folder(src / L"ad32"),
                         opts_for(root, reg), Status::ok);
    CHECK_EQ(r.package_id, std::string("ad32"));
    CHECK_EQ(r.format, std::string(as_iso ? "iso9660" : "folder"));
    check_installed(root / L"win", ad32, "packages/ad32");
    CHECK(catalog_ids(root / L"win") == ad32.ids);
    phosg::JSON j = json_at(r.import_json);
    CHECK_EQ(j.at("package").get_string("recipe"), std::string("ad3zip"));
    CHECK_EQ(j.at("source").get_string("kind"), std::string(as_iso ? "iso" : "folder"));
    std::map<std::string, std::string> from;
    for (auto& f : j.at("files").as_list()) from[f->get_string("path")] = f->get_string("from");
    CHECK_EQ(from["packages/ad32/AD32/TOILET.AD"], std::string("INSTALL/TOILET.ZIP!TOILET.AD"));
    CHECK_EQ(from["packages/ad32/AD32/FOLDER.AFI"], std::string("INSTALL/AFI.ZIP!AD3.AFI"));
    CHECK_EQ(from["packages/ad32/AD32/MUSIC/OMTW.MID"], std::string("INSTALL/MUSICG.ZIP!OMTW.MID"));
    CHECK_EQ(from["packages/ad32/ENGINE/ADTASK.DLL"], std::string("INSTALL/ENGINE.ZIP!ADTASK.DLL"));
    CHECK(logged("skipped INSTALL/EXTRA.ZIP") || logged("skipped EXTRA.ZIP"));
    CHECK(logged("recovered the archive password"));
    // A ZIP member's copy keeps the member's DOS time (1995-07-17 15:16 local).
    WIN32_FILE_ATTRIBUTE_DATA a{};
    GetFileAttributesExW((r.files_dir / L"AD32" / L"GUTS.AD").c_str(), GetFileExInfoStandard, &a);
    FILETIME local;
    FileTimeToLocalFileTime(&a.ftLastWriteTime, &local);
    SYSTEMTIME st{};
    FileTimeToSystemTime(&local, &st);
    CHECK(st.wYear == 1995 && st.wMonth == 7 && st.wDay == 17);
    check_no_password(root);
    CHECK(no_leftovers(root / L"win"));
  }
  {
    fs::path root = dir / L"alone-tt";
    ImportResult r = run("tt iso", image(src / L"tt.iso"), opts_for(root, reg), Status::ok);
    CHECK_EQ(r.package_id, std::string("tt"));
    check_installed(root / L"win", tt, "packages/tt");
    CHECK(catalog_ids(root / L"win") == tt.ids);
    check_no_password(root);
  }
  {
    // The merged floppy image: SERIAL.TXT's chain loops, so reading it would
    // fail — the import must never touch it (or CEREAL.TXT).
    fs::path root = dir / L"alone-simpsons";
    g_log.clear();
    ImportResult r = run("simpsons floppy", image(src / L"simpsons.img"), opts_for(root, reg), Status::ok);
    CHECK_EQ(r.package_id, std::string("simpsons"));
    CHECK_EQ(r.format, std::string("fat12"));
    check_installed(root / L"win", simpsons, "packages/simpsons");
    CHECK(catalog_ids(root / L"win") == simpsons.ids);
    phosg::JSON j = json_at(r.import_json);
    CHECK_EQ(j.at("source").get_string("kind"), std::string("floppy"));
    CHECK_EQ(j.at("source").get_string("format"), std::string("fat12"));
    std::string text = test::read_text(r.import_json) + test::read_text(root / L"win" / L"catalog-win.json");
    for (auto& l : g_log) text += l;
    CHECK(text.find("SERIAL") == std::string::npos && text.find("CEREAL") == std::string::npos);
    check_no_password(root);
    // Trimmed names: "Grampa's Wisdom " and "Snowball I " lose the space.
    phosg::JSON cat = json_at(root / L"win" / L"catalog-win.json");
    CHECK_EQ(module_by_id(cat, "simpsons.grampa")->get_string("displayName"), std::string("Grampa's Wisdom"));
    CHECK_EQ(module_by_id(cat, "simpsons.grampa")->get_string("moduleName"), std::string("Grampa's Wisdom"));
    CHECK_EQ(module_by_id(cat, "simpsons.snowball")->get_string("displayName"), std::string("Snowball I"));
  }
  {
    // The same from the two floppies (split), in either order.
    for (bool reversed : {false, true}) {
      fs::path root = dir / (reversed ? L"split-simpsons-rev" : L"split-simpsons");
      ImportResult r = run("simpsons split floppies",
                           reversed ? image(src / L"disk2.img", {src / L"disk1.img"})
                                    : image(src / L"disk1.img", {src / L"disk2.img"}),
                           opts_for(root, reg), Status::ok);
      check_installed(root / L"win", simpsons, "packages/simpsons");
      phosg::JSON j = json_at(r.import_json);
      CHECK_EQ(j.at("source").at("parts").as_list().size(), size_t(2));
      CHECK(!j.at("source").contains("imageMd5"));
      CHECK_EQ(r.verified, std::string("files"));
    }
    // One disk alone is not a complete release.
    run("simpsons disk 1 alone", image(src / L"disk1.img"), opts_for(dir / L"split-1", reg), Status::source_invalid);
    run("simpsons disk 2 alone", image(src / L"disk2.img"), opts_for(dir / L"split-2", reg), Status::source_invalid);
    // A folder source with the owner's notes locked: they are never opened.
    HANDLE h1 = CreateFileW((src / L"simpsons" / L"SERIAL.TXT").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    HANDLE h2 = CreateFileW((src / L"simpsons" / L"CEREAL.TXT").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(h1 != INVALID_HANDLE_VALUE && h2 != INVALID_HANDLE_VALUE);
    run("simpsons folder, notes locked", folder(src / L"simpsons"), opts_for(dir / L"simpsons-folder", reg), Status::ok);
    check_installed(dir / L"simpsons-folder" / L"win", simpsons, "packages/simpsons");
    CloseHandle(h1);
    CloseHandle(h2);
  }

  // ---- Star Wars Screen Entertainment: Presage's installer, Intermission modules --------------------
  {
    auto froms = [](const fs::path& import_json) {
      std::map<std::string, std::string> from;
      phosg::JSON j = json_at(import_json);
      for (auto& f : j.at("files").as_list()) from[f->get_string("path")] = f->get_string("from");
      return from;
    };
    const auto& d = swse_disks;
    struct Form {
      const wchar_t* name;
      Source source;
      const char* format;
      const char* kind;
      size_t parts;
    };
    for (const Form& form : std::vector<Form>{
             {L"folder", folder(src / L"swse"), "folder", "folder", 0},
             {L"iso", image(src / L"swse.iso"), "iso9660", "iso", 0},
             {L"floppies", image(d[0], {d[1], d[2], d[3], d[4]}), "fat12", "floppy", 5},
             {L"floppies-reversed", image(d[4], {d[3], d[2], d[1], d[0]}), "fat12", "floppy", 5},
             {L"zip", image(src / L"swse.zip"), "zip", "zip", 0}}) {
      fs::path root = dir / (std::wstring(L"alone-swse-") + form.name);
      g_log.clear();
      ImportResult r = run(("swse " + to_utf8(form.name)).c_str(), form.source, opts_for(root, reg), Status::ok);
      CHECK_EQ(r.package_id, std::string("swse"));
      CHECK_EQ(r.format, std::string(form.format));
      CHECK_EQ(r.verified, std::string("files"));
      CHECK_EQ(r.package_modules, swse.ids.size());
      CHECK_EQ(r.files_dir, root / L"win" / L"packages" / L"swse");
      check_installed(root / L"win", swse, "packages/swse");
      CHECK(catalog_ids(root / L"win") == swse.ids);
      phosg::JSON j = json_at(r.import_json);
      CHECK_EQ(j.at("package").get_string("recipe"), std::string("intermission"));
      CHECK_EQ(j.at("source").get_string("kind"), std::string(form.kind));
      CHECK_EQ(j.at("source").at("parts").as_list().size(), form.parts);
      for (auto& f : j.at("files").as_list()) CHECK_EQ(f->get_string("known"), std::string("match"));
      // Where each file came from: archive members (a split one names its
      // volumes), SZDD files under their installed names, the settings.
      auto from = froms(r.import_json);
      CHECK_EQ(from["packages/swse/SAVER/JAWAS.IMX"], std::string("SWSE2.ARJ+SWSE2.A01!JAWAS.IMX"));
      CHECK_EQ(from["packages/swse/SAVER/STORYBRD.IMX"], std::string("SWSE2.A01+SWSE2.A02!STORYBRD.IMX"));
      CHECK_EQ(from["packages/swse/SAVER/SWSFX.DLL"], std::string("SWSE2.A02+SWSE2.A03!SWSFX.DLL"));
      CHECK_EQ(from["packages/swse/SAVER/ANTSW.DLL"], std::string("SWSE1.ARJ!ANTSW.DLL"));
      CHECK_EQ(from["packages/swse/SAVER/SWTEXT.TXT"], std::string("SWSE2.A03!SWTEXT.TXT"));
      CHECK_EQ(from["packages/swse/ENGINE/IMIMXPLY.IMQ"], std::string("SWSE1.ARJ!IMIMXPLY.IMQ"));
      CHECK_EQ(from["packages/swse/ENGINE/INTERMIS.EXE"], std::string("SWSE1.ARJ!INTERMIS.EXE"));
      CHECK_EQ(from["packages/swse/SAVER/BATTLE.MID"], std::string("GM_BATTL.MI_"));
      CHECK_EQ(from["packages/swse/SAVER/SWTHEME.MID"], std::string("GM_TITLE.MI_"));
      CHECK_EQ(from["packages/swse/SAVER/STRESS.DLL"], std::string("STRESS.DL_"));
      CHECK_EQ(from["packages/swse/WINDOWS/SWSE.INI"], std::string("SWSE.INI"));
      // The members the recipe skips are listed, never decoded (they are damaged).
      CHECK(logged("SWSE1.ARJ!IMAD_PLY.IMQ") && logged("SWSE1.ARJ!AD_SND.DLL") && logged("SWSE1.ARJ!INTERMSN.HLP"));
      // No decoy is named anywhere.
      std::string text = test::read_text(r.import_json) + test::read_text(root / L"win" / L"catalog-win.json");
      for (auto& l : g_log) text += l;
      for (const std::string& decoy : test::swse_decoys()) CHECK(text.find(decoy) == std::string::npos);
      // An archive member keeps its DOS time (1994-10-12, local).
      WIN32_FILE_ATTRIBUTE_DATA a{};
      GetFileAttributesExW((r.files_dir / L"SAVER" / L"VADER.IMX").c_str(), GetFileExInfoStandard, &a);
      FILETIME local;
      FileTimeToLocalFileTime(&a.ftLastWriteTime, &local);
      SYSTEMTIME st{};
      FileTimeToSystemTime(&local, &st);
      CHECK(st.wYear == 1994 && st.wMonth == 10 && st.wDay == 12);
      // The catalog: IMX entries, names from the registry.
      phosg::JSON cat = json_at(root / L"win" / L"catalog-win.json");
      const phosg::JSON* v = module_by_id(cat, "swse.vader");
      CHECK(v != nullptr);
      if (v) {
        CHECK_EQ(v->get_string("displayName"), std::string("Darth Vader"));
        CHECK_EQ(v->get_string("lane"), std::string("ne16"));
        CHECK_EQ(v->get_string("entry"), std::string("SAVERDRAW"));
        CHECK_EQ(v->get_string("abi"), std::string("intermission"));
        CHECK_EQ(v->get_string("path"), std::string("packages/swse/SAVER/VADER.IMX"));
        CHECK_EQ(v->at("controls").as_list().size(), size_t(1));
        CHECK((v->at("needs").as_list().size() == 4));  // INTRMLIB, READJPG, STRESS, SWSE
      }
      CHECK(no_leftovers(root / L"win"));
    }
    // The image md5 names it: verified "image".
    {
      test::TestRegistry by_md5;
      by_md5.image("swse", md5_file_hex(src / L"swse.iso"), fs::file_size(src / L"swse.iso"));
      ImportResult r = run("swse, known image md5", image(src / L"swse.iso"), opts_for(dir / L"swse-md5", by_md5), Status::ok);
      CHECK_EQ(r.verified, std::string("image"));
      CHECK(r.iso_md5_known);
      CHECK_EQ(json_at(r.import_json).at("source").get_string("volumeId"), std::string("SWSE"));
    }
    // Every install disk is needed.
    {
      ImportResult r = run("swse disk 1 alone", image(d[0]), opts_for(dir / L"swse-d1", reg), Status::source_invalid);
      CHECK(r.message.find("missing SWSE2.ARJ, SWSE2.A01, SWSE2.A02, SWSE2.A03;") != std::string::npos);
      CHECK(r.message.find("needs every install disk") != std::string::npos);
      r = run("swse disks 1-4", image(d[0], {d[1], d[2], d[3]}), opts_for(dir / L"swse-d14", reg), Status::source_invalid);
      CHECK(r.message.find("missing SWSE2.A03;") != std::string::npos);
      r = run("swse disks 2-5", image(d[1], {d[2], d[3], d[4]}), opts_for(dir / L"swse-d25", reg), Status::source_invalid);
      CHECK(r.message.find("not a known release") != std::string::npos);
      CHECK(!fs::exists(dir / L"swse-d1" / L"win" / L"packages" / L"swse"));
    }
    // I5: the decoys, locked in a folder source, are never opened.
    {
      std::vector<HANDLE> held;
      for (const std::string& decoy : test::swse_decoys())
        held.push_back(CreateFileW((src / L"swse" / to_wide(decoy)).c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr));
      for (HANDLE h : held) CHECK(h != INVALID_HANDLE_VALUE);
      run("swse folder, decoys locked", folder(src / L"swse"), opts_for(dir / L"swse-locked", reg), Status::ok);
      check_installed(dir / L"swse-locked" / L"win", swse, "packages/swse");
      for (HANDLE h : held) CloseHandle(h);
    }
    // I5: the volume chains stay within the registry's archives. A volume
    // that says the archive goes on past them (VOLUME_FLAG on SWSE2.A03's or
    // SWSE1.ARJ's main header) is a damaged source that names the volume it
    // points at — not a missing install disk — and a file of that name,
    // locked, is never opened; another .ARJ beside them is skipped, logged
    // and never opened either.
    {
      auto variant = [&](const wchar_t* name, const std::function<void(test::PkgFixture&)>& change) {
        test::PkgFixture f = swse;
        change(f);
        fs::path p = src / name;
        test::write_tree(p, f.source);
        return p;
      };
      auto goes_on = [](const char* volume) {
        return [volume](test::PkgFixture& f) { f.source[volume] = test::arj_with_main_flags(f.source[volume], 0x14); };
      };
      const std::string past_a03 = "SWSE2.A03 says the archive continues on SWSE2.A04, which is not one of Star Wars "
                                   "Screen Entertainment's install disks (a damaged or foreign volume?)";
      ImportResult r = run("swse, SWSE2.A03 goes on", folder(variant(L"v-swse-a03on", goes_on("SWSE2.A03"))),
                           opts_for(dir / L"swse-a03on", reg), Status::source_invalid);
      CHECK_EQ(r.message, past_a03);
      CHECK(r.message.find("install disk;") == std::string::npos && r.message.find("needs every") == std::string::npos);
      fs::path a04 = variant(L"v-swse-a04", [&](test::PkgFixture& f) {
        goes_on("SWSE2.A03")(f);
        f.source["SWSE2.A04"] = test::arj_volume("SWSE2.A04", false, {test::arj_stored_entry("EXTRA.TXT", {1})});
      });
      HANDLE held = CreateFileW((a04 / L"SWSE2.A04").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
      CHECK(held != INVALID_HANDLE_VALUE);
      r = run("swse, SWSE2.A03 goes on, SWSE2.A04 locked", folder(a04), opts_for(dir / L"swse-a04", reg),
              Status::source_invalid);
      CHECK_EQ(r.message, past_a03);
      CloseHandle(held);
      r = run("swse, SWSE1.ARJ goes on", folder(variant(L"v-swse-s1on", goes_on("SWSE1.ARJ"))),
              opts_for(dir / L"swse-s1on", reg), Status::source_invalid);
      CHECK(r.message.find("SWSE1.ARJ says the archive continues on SWSE1.A01, which is not one of") !=
            std::string::npos);
      CHECK(!fs::exists(dir / L"swse-a03on" / L"win" / L"packages" / L"swse"));
      fs::path other = variant(L"v-swse-otherarj", [](test::PkgFixture& f) {
        f.source["OTHER.ARJ"] = test::arj_volume("OTHER.ARJ", false, {test::arj_stored_entry("EXTRA.TXT", {1})});
      });
      held = CreateFileW((other / L"OTHER.ARJ").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
      CHECK(held != INVALID_HANDLE_VALUE);
      g_log.clear();
      run("swse, another .ARJ (locked)", folder(other), opts_for(dir / L"swse-otherarj", reg), Status::ok);
      CHECK(logged("skipped OTHER.ARJ (not in the recipe of Star Wars Screen Entertainment)"));
      check_installed(dir / L"swse-otherarj" / L"win", swse, "packages/swse");
      CloseHandle(held);
    }
    // Identification: the script's short name, INSTALL.DAT's size, --package, ambiguity.
    {
      auto sw = [&](const wchar_t* name, const std::function<void(test::PkgFixture&)>& change) {
        test::PkgFixture f = swse;
        change(f);
        fs::path p = src / name;
        test::write_tree(p, f.source);
        return p;
      };
      auto other = sw(L"v-swse-other", [](test::PkgFixture& f) { f.source["INSTALL.DAT"] = test::intermission_dat("OTHER"); });
      ImportResult r = run("swse, another short name", folder(other), opts_for(dir / L"swse-other", reg), Status::source_invalid);
      CHECK(r.message.find("not a known release") != std::string::npos);
      auto big = sw(L"v-swse-bigdat", [](test::PkgFixture& f) {
        auto dat = test::intermission_dat("SWSE");
        dat.resize(70 * 1024, ' ');
        f.source["INSTALL.DAT"] = dat;
      });
      r = run("swse, a 70 KB INSTALL.DAT", folder(big), opts_for(dir / L"swse-bigdat", reg), Status::source_invalid);
      CHECK(r.message.find("not a known release") != std::string::npos);
      auto nofirst = sw(L"v-swse-nofirst", [](test::PkgFixture& f) { f.source.erase("SWSE1.ARJ"); });
      r = run("swse, no SWSE1.ARJ beside INSTALL.DAT", folder(nofirst), opts_for(dir / L"swse-nofirst", reg),
              Status::source_invalid);
      CHECK(r.message.find("not a known release") != std::string::npos);
      r = run("--package swse on ad32", folder(src / L"ad32", "swse"), opts_for(dir / L"swse-pkg", reg), Status::source_invalid);
      CHECK(r.message.find("not Star Wars Screen Entertainment") != std::string::npos);
      test::write_tree(src / L"both-swse", ad32.source);
      test::write_tree(src / L"both-swse", swse.source);
      r = run("ad32 and swse in one folder", folder(src / L"both-swse"), opts_for(dir / L"swse-both", reg),
              Status::source_invalid);
      CHECK(r.message.find("ambiguous") != std::string::npos);
      CHECK(r.message.find("Star Wars Screen Entertainment") != std::string::npos);
      r = run("both + --package swse", folder(src / L"both-swse", "swse"), opts_for(dir / L"swse-both-1", reg), Status::ok);
      CHECK_EQ(r.package_id, std::string("swse"));
      r = run("both + --package ad32", folder(src / L"both-swse", "ad32"), opts_for(dir / L"swse-both-2", reg), Status::ok);
      CHECK_EQ(r.package_id, std::string("ad32"));
      CHECK(identify_folder(src / L"swse") == find_package("swse"));
    }
    // Damaged sources: exit 2 (3 for an SZDD literal, which has no checksum),
    // and nothing changes, not even the catalog.
    {
      fs::path croot = dir / L"swse-corrupt";
      run("setup tt", image(src / L"tt.iso"), opts_for(croot, reg), Status::ok);
      run("setup swse", folder(src / L"swse"), opts_for(croot, reg), Status::ok);
      const test::Tree before = snapshot(croot / L"win");
      auto corrupt = [&](const char* what, const wchar_t* name, const std::function<void(test::PkgFixture&)>& change,
                         Status want, const char* in_message) {
        test::PkgFixture f = swse;
        change(f);
        fs::path p = src / name;
        test::write_tree(p, f.source);
        ImportResult r = run(what, folder(p), opts_for(croot, reg), want);
        CHECK(r.message.find(in_message) != std::string::npos);
        CHECK(snapshot(croot / L"win") == before);
        CHECK(no_leftovers(croot / L"win"));
      };
      corrupt("a flipped byte in a split segment", L"v-swse-flip",
              [](test::PkgFixture& f) {
                auto& v = f.source["SWSE2.A01"];
                v[v.size() - 30] ^= 0x10;  // STORYBRD.IMX's first segment
              },
              Status::source_invalid, "SWSE2.A01!STORYBRD.IMX: CRC-32 mismatch");
      corrupt("a flipped byte in an LZH segment", L"v-swse-lzh",
              [](test::PkgFixture& f) {
                auto& v = f.source["SWSE2.A03"];
                v[120] ^= 0x04;  // inside SWSFX.DLL's second segment
              },
              Status::source_invalid, "SWSE2.A03!SWSFX.DLL");
      corrupt("a truncated .A02", L"v-swse-cut",
              [](test::PkgFixture& f) { f.source["SWSE2.A02"].resize(f.source["SWSE2.A02"].size() - 100); },
              Status::source_invalid, "SWSE2.A02: the archive is truncated");
      corrupt(".A01 of another set", L"v-swse-swap",
              [](test::PkgFixture& f) {
                f.source["SWSE2.A01"] = test::arj_volume(
                    "SWSE2.A01", true, {test::arj_stored_entry("ICLOCK.IMX", test::blob("another build"), 0x18, 100)});
              },
              Status::source_invalid, "SWSE2.ARJ!JAWAS.IMX continues, but SWSE2.A01 does not continue it");
      corrupt("an .A01 that is no ARJ volume", L"v-swse-junk",
              [](test::PkgFixture& f) { f.source["SWSE2.A01"] = test::blob("junk"); }, Status::source_invalid,
              "SWSE2.A01: not an ARJ archive");
      corrupt("a cut GM_CNTNA.MI_", L"v-swse-cutmid",
              [](test::PkgFixture& f) { f.source["GM_CNTNA.MI_"].resize(f.source["GM_CNTNA.MI_"].size() - 5); },
              Status::source_invalid, "GM_CNTNA.MI_: the compressed data ends early");
      corrupt("a KWAJ STRESS.DL_", L"v-swse-kwaj",
              [](test::PkgFixture& f) {
                auto& v = f.source["STRESS.DL_"];
                v[0] = 'K', v[1] = 'W', v[2] = 'A', v[3] = 'J';
              },
              Status::source_invalid, "KWAJ");
      corrupt("an SZDD literal changed (no checksum: the manifest tells)", L"v-swse-lit",
              [](test::PkgFixture& f) { f.source["GM_EMPIR.MI_"][15] ^= 0x01; }, Status::verify_failed,
              "differ from the release of Star Wars Screen Entertainment");
      // The planned sizes (ARJ headers, SZDD headers) are what the staging
      // budget and the manifest check see, before a byte is written.
      auto refused_early = [&](const char* what, const fs::path& source, ImportOptions o, Status want,
                               const std::string& in_message) {
        bool copied = false;
        o.progress = [&](const Progress& pr) {
          copied = copied || pr.phase == Progress::Phase::copy;
          return true;
        };
        ImportResult r = run(what, folder(source), o, want);
        CHECK(r.message.find(in_message) != std::string::npos);
        CHECK(!copied);
        CHECK(snapshot(croot / L"win") == before);
      };
      ImportOptions few = opts_for(croot, reg);
      few.max_files = 10;
      refused_early("swse over a 10-file budget", src / L"swse", few, Status::source_invalid, "more than 10 files");
      ImportOptions small = opts_for(croot, reg);
      small.max_bytes = 4000;
      refused_early("swse over a 4000-byte budget", src / L"swse", small, Status::source_invalid,
                    "more than 4000 bytes");
      {
        test::PkgFixture f = swse;
        auto longer = test::vec("MThd");
        auto body = test::blob("a longer piece of music", 900);
        longer.insert(longer.end(), body.begin(), body.end());
        f.source["GM_BATTL.MI_"] = test::szdd_encode(longer);
        test::write_tree(src / L"v-swse-size", f.source);
        refused_early("an SZDD file of another size", src / L"v-swse-size", opts_for(croot, reg), Status::verify_failed,
                      "packages/swse/SAVER/BATTLE.MID is " + std::to_string(longer.size()) + " bytes");
      }
      {
        // Two members whose names differ only in a code page 437 letter's
        // case (u-umlaut 0x81, U-umlaut 0x9A) are one name to Windows.
        test::PkgFixture f = test::swse_fixture([](std::vector<test::ArjEntry>& e) {
          e.push_back(test::arj_stored_entry("\x81.DLL", test::blob("one")));
          e.push_back(test::arj_stored_entry("\x9A.DLL", test::blob("two")));
        });
        test::write_tree(src / L"v-swse-cp437", f.source);
        refused_early("two members, one name to Windows", src / L"v-swse-cp437", opts_for(croot, reg),
                      Status::source_invalid, "SWSE1.ARJ: two members are named \xC3\x9C.DLL");
      }
      // A cancelled re-import changes nothing either.
      ImportOptions cancel = opts_for(croot, reg);
      cancel.progress = [](const Progress& pr) { return !(pr.phase == Progress::Phase::copy && pr.done > 2000); };
      run("swse re-import, cancelled while copying", image(src / L"swse.iso"), cancel, Status::cancelled);
      CHECK(snapshot(croot / L"win") == before);
      CHECK(no_leftovers(croot / L"win"));
    }
    // The intermission invariants (registries without manifests: nothing to
    // verify against) and the required files.
    {
      const Package& builtin = *find_package("swse");
      std::vector<LooseFile> loose(builtin.loose_files.begin(), builtin.loose_files.end());
      auto variant_of = [&](const wchar_t* name, const std::function<void(std::vector<test::ArjEntry>&)>& swse1,
                            const std::map<std::string, std::vector<uint8_t>>& extra = {},
                            const std::vector<std::string>& drop = {}) {
        test::PkgFixture f = test::swse_fixture(swse1);
        for (auto& [n, b] : extra) f.source[n] = b;
        for (auto& n : drop) f.source.erase(n);
        fs::path p = src / name;
        test::write_tree(p, f.source);
        return p;
      };
      auto swse_inv = [&](const char* what, const fs::path& source, const char* expect, const test::TestRegistry& r) {
        fs::path root = dir / (L"inv-" + source.filename().wstring());
        ImportResult res = run(what, folder(source), opts_for(root, r, false), Status::source_invalid);
        CHECK(res.message.find(expect) != std::string::npos);
        CHECK(!fs::exists(root / L"win" / L"packages" / L"swse"));
        CHECK(!fs::exists(root / L"win") || no_leftovers(root / L"win"));
      };
      auto drop_member = [](const char* n) {
        return [n](std::vector<test::ArjEntry>& e) {
          e.erase(std::remove_if(e.begin(), e.end(), [&](const test::ArjEntry& x) { return x.h.name == n; }), e.end());
        };
      };
      test::TestRegistry none;
      swse_inv("required: no SAVER\\ANTSW.DLL", variant_of(L"v-swse-noantsw", drop_member("ANTSW.DLL")),
               "missing required file(s): packages/swse/SAVER/ANTSW.DLL", none);
      test::TestRegistry lax;
      lax.get("swse").required = {};
      swse_inv("I2: INTRMLIB.DLL's ANTSW is missing", variant_of(L"v-swse-i2dll", drop_member("ANTSW.DLL")),
               "breaks I2: SAVER\\INTRMLIB.DLL needs ANTSW", lax);
      swse_inv("I2: an IMX module needs FOO",
               variant_of(L"v-swse-i2", [](std::vector<test::ArjEntry>& e) {
                 e.push_back(test::arj_stored_entry("NEEDY.IMX", test::imx_module("NEEDY", {"KERNEL", "FOO"})));
               }),
               "breaks I2: SAVER\\NEEDY.IMX needs FOO", none);
      swse_inv("I3: no ENGINE\\IMIMXPLY.IMQ", variant_of(L"v-swse-i3", drop_member("IMIMXPLY.IMQ")),
               "breaks I3: no ENGINE\\IMIMXPLY.IMQ", lax);
      swse_inv("I3: no WINDOWS\\SWSE.INI", variant_of(L"v-swse-i3ini", {}, {}, {"SWSE.INI"}),
               "breaks I3: no WINDOWS\\SWSE.INI", lax);
      {
        test::TestRegistry r;
        std::vector<LooseFile> l = loose;
        l.push_back({"IMIMXPLY.IMQ", "SAVER/IMIMXPLY.IMQ", Codec::plain});
        r.get("swse").loose_files = l;
        swse_inv("I1: an IMX reader beside the modules",
                 variant_of(L"v-swse-i1", {}, {{"IMIMXPLY.IMQ", test::ne_dll("IMIMXPLY", {"KERNEL"})}}),
                 "breaks I1: SAVER\\IMIMXPLY.IMQ belongs in ENGINE", r);
        l = loose;
        l.push_back({"SNDDRV.DLL", "ENGINE/AD_SND.DLL", Codec::plain});
        r.get("swse").loose_files = l;
        swse_inv("I3: After Dark's AD_SND in ENGINE", variant_of(L"v-swse-i3snd", {}, {{"SNDDRV.DLL", test::blob("snd")}}),
                 "breaks I3: ENGINE\\AD_SND.DLL", r);
        l = loose;
        l[1].to = "SAVER/MUSIC/BATTLE.MID";
        r.get("swse").loose_files = l;
        swse_inv("I4: a MIDI file in a subfolder", variant_of(L"v-swse-i4", {}), "breaks I4", r);
      }
    }
  }

  // ---- Star Trek: The Screen Saver: Microsoft Setup's KWAJ files, After Dark 2.0 modules ---------
  {
    auto froms = [](const fs::path& import_json) {
      std::map<std::string, std::string> from;
      phosg::JSON j = json_at(import_json);
      for (auto& f : j.at("files").as_list()) from[f->get_string("path")] = f->get_string("from");
      return from;
    };
    // Where the text of a startrek catalog entry says what the rules made of it.
    auto check_catalog = [&](const fs::path& win) {
      phosg::JSON cat = json_at(win / L"catalog-win.json");
      for (auto& m : cat.at("modules").as_list()) {
        if (m->get_string("package") != "startrek") {
          CHECK(!m->contains("screen"));
          continue;
        }
        CHECK_EQ(m->get_string("lane"), std::string("ne16"));
        CHECK_EQ(m->get_string("entry"), std::string("MODULE"));
        CHECK(!m->contains("abi"));
        // The fixed screen, on every entry of the package (last: below).
        CHECK_EQ(m->get_string("screen"), std::string("640x480"));
        const std::string about = m->get_string("about");
        CHECK(about.find("Authorized User") == std::string::npos);
        CHECK(about.find("wrapped by hand.") != std::string::npos);
        CHECK(about.find(" \n") == std::string::npos);
        CHECK(m->get_string("displayName").front() != ' ');
      }
      const std::string text = test::read_text(win / L"catalog-win.json");
      size_t at = text.find("\"id\": \"startrek.tribble\"");
      size_t end = text.find("\n  }", at);
      CHECK(at != std::string::npos && text.rfind("\"screen\": \"640x480\"\n", end) > at);
      const phosg::JSON* planets = module_by_id(cat, "startrek.planets");
      CHECK(planets && planets->get_string("displayName") == "Planetary Atlas" &&
            planets->get_string("moduleName") == "Planetary Atlas");
      const phosg::JSON* cells = module_by_id(cat, "startrek.braincel");
      CHECK(cells && cells->get_string("displayName") == "Brain Cells" && cells->get_string("moduleName") == "Brain Cells");
      if (cells)
        CHECK_EQ(cells->get_string("about"), std::string("ABOUT Brain Cells\n\nA made-up module whose sentence is "
                                                         "wrapped by hand.\n\nMade up for the tests."));
      const phosg::JSON* comms = module_by_id(cat, "startrek.comms");
      CHECK(comms && comms->at("controls").as_list().size() == 2);
      if (comms && comms->at("controls").as_list().size() == 2) {
        const auto& b = *comms->at("controls").as_list()[1];
        CHECK(b.get_int("index") == 3 && b.get_string("name") == "Edit Custom..." && b.get_string("type") == "button");
      }
      const phosg::JSON* sounder = module_by_id(cat, "startrek.sounder");
      CHECK(sounder && sounder->at("needs").as_list().size() == 1 && sounder->at("needs").as_list()[0]->as_string() == "AD_SND");
      if (sounder && sounder->at("controls").as_list().size() == 2)
        CHECK(sounder->at("controls").as_list()[1]->get_int("index") == 2);
      const phosg::JSON* mission = module_by_id(cat, "startrek.mission");
      CHECK(mission && mission->at("controls").as_list().empty());
    };
    struct Form {
      const wchar_t* name;
      Source source;
      const char* format;
      const char* kind;
      size_t parts;
    };
    for (const Form& form : std::vector<Form>{{L"folder", folder(src / L"startrek"), "folder", "folder", 0},
                                              {L"floppies", image(st1, {st2}), "fat12", "floppy", 2},
                                              {L"floppies-reversed", image(st2, {st1}), "fat12", "floppy", 2},
                                              {L"zip-of-images", image(src / L"startrek-images.zip"), "fat12", "floppy", 2},
                                              {L"flat-zip", image(src / L"startrek.zip"), "zip", "zip", 0},
                                              {L"iso", image(src / L"startrek.iso"), "iso9660", "iso", 0}}) {
      fs::path root = dir / (std::wstring(L"alone-startrek-") + form.name);
      g_log.clear();
      ImportResult r = run(("startrek " + to_utf8(form.name)).c_str(), form.source, opts_for(root, reg), Status::ok);
      CHECK_EQ(r.package_id, std::string("startrek"));
      CHECK_EQ(r.format, std::string(form.format));
      CHECK_EQ(r.verified, std::string("files"));
      CHECK_EQ(r.package_modules, startrek.ids.size());
      CHECK_EQ(r.files_dir, root / L"win" / L"packages" / L"startrek");
      check_installed(root / L"win", startrek, "packages/startrek");
      CHECK(catalog_ids(root / L"win") == startrek.ids);
      phosg::JSON j = json_at(r.import_json);
      CHECK_EQ(j.at("package").get_string("recipe"), std::string("ad2kwaj"));
      CHECK_EQ(j.at("source").get_string("kind"), std::string(form.kind));
      CHECK_EQ(j.at("source").at("parts").as_list().size(), form.parts);
      for (auto& f : j.at("files").as_list()) CHECK_EQ(f->get_string("known"), std::string("match"));
      auto from = froms(r.import_json);
      CHECK_EQ(from["packages/startrek/AFTERDRK/BRAINCEL.AD"], std::string("BRAINCEL.AD_"));
      CHECK_EQ(from["packages/startrek/AFTERDRK/ST_RES/ST_VGA.DLL"], std::string("ST_VGA.DL_"));
      CHECK_EQ(from["packages/startrek/AFTERDRK/SOUNDS/JIM.WAV"], std::string("JIM.WA_"));
      CHECK_EQ(from["packages/startrek/ENGINE/AD.EXE"], std::string("AD.EX_"));
      CHECK_EQ(from.size(), size_t(27));
      // No decoy is named anywhere, and no WINDOWS folder is made.
      std::string text = test::read_text(r.import_json) + test::read_text(root / L"win" / L"catalog-win.json");
      for (auto& l : g_log) text += l;
      for (const std::string& decoy : test::startrek_decoys()) CHECK(text.find(decoy) == std::string::npos);
      CHECK(!fs::exists(r.files_dir / L"WINDOWS"));
      check_catalog(root / L"win");
      if (std::wstring_view(form.name) == L"zip-of-images") {
        CHECK(logged("reading the 2 floppy images in "));
        CHECK(logged("ignoring the rest of ") && logged(": disk1.jpg"));
        const auto& parts = j.at("source").at("parts").as_list();
        CHECK(parts.size() == 2 && ends_with_i(parts[0]->get_string("path"), "startrek-images.zip!st-disk2.img") &&
              ends_with_i(parts[1]->get_string("path"), "startrek-images.zip!st-disk1.img"));
        CHECK(parts.size() == 2 && parts[0]->get_string("md5") == md5_file_hex(st2));
      }
      if (form.parts == 2) {
        // A KWAJ file's copy keeps the compressed file's own time (the
        // floppies': 1994-07-17, local).
        WIN32_FILE_ATTRIBUTE_DATA a{};
        GetFileAttributesExW((r.files_dir / L"AFTERDRK" / L"TRIBBLE.AD").c_str(), GetFileExInfoStandard, &a);
        FILETIME local;
        FileTimeToLocalFileTime(&a.ftLastWriteTime, &local);
        SYSTEMTIME st{};
        FileTimeToSystemTime(&local, &st);
        CHECK(st.wYear == 1994 && st.wMonth == 7 && st.wDay == 17);
      }
      CHECK(no_leftovers(root / L"win"));
    }
    // A known set of install disks: verified "image", from either copy of
    // each disk, in any order and any form; one disk alone or two copies of
    // one are not the release. The registry knows the four disk images and
    // the manifest, as the real one does.
    {
      test::TestRegistry by_md5;
      by_md5.manifest("startrek", test::manifest_of(startrek.expect));
      by_md5.disk_images("startrek", {{md5_file_hex(st1), fs::file_size(st1), 1},
                                      {md5_file_hex(st2), fs::file_size(st2), 2},
                                      {md5_file_hex(st1b), fs::file_size(st1b), 1},
                                      {md5_file_hex(st2b), fs::file_size(st2b), 2}});
      CHECK(md5_file_hex(st1) != md5_file_hex(st1b));
      test::write_bytes(src / L"st-disk2-only.zip", test::zip_of_images({{"disk2.img", test::read_bytes(st2b)}}, false));
      const std::string every_disk_and_another =
          "the images hold every install disk of Star Trek: The Screen Saver (by md5) and another image besides; "
          "checking files individually";
      int n = 0;
      for (const auto& [what, source] : std::vector<std::pair<std::string, Source>>{
               {"disks 1, 2", image(st1, {st2})},
               {"disks 2, 1", image(st2, {st1})},
               {"one disk of each copy", image(st2b, {st1})},
               {"the ZIP of the images", image(src / L"startrek-images.zip")},
               {"disk 1 and a ZIP of disk 2", image(st1, {src / L"st-disk2-only.zip"})},
               {"disk 1, then the ZIP of both", image(st1, {src / L"startrek-images.zip"})}}) {
        fs::path root = dir / (L"startrek-set-" + std::to_wstring(n++));
        g_log.clear();
        ImportResult r = run(("startrek known set: " + what).c_str(), source, opts_for(root, by_md5), Status::ok);
        CHECK_EQ(r.verified, std::string("image"));
        CHECK(r.iso_md5_known);
        CHECK(r.iso_md5.empty());
        CHECK_EQ(r.parts.size(), size_t(2));
        phosg::JSON s = json_at(r.import_json).at("source");
        CHECK_EQ(s.get_bool("imageMd5Known"), true);
        CHECK(!s.contains("imageMd5"));
        CHECK(logged("the images are the known install disks of Star Trek: The Screen Saver"));
        check_installed(root / L"win", startrek, "packages/startrek");
        if (what == "disk 1, then the ZIP of both") {
          // The ZIP's disk 1 is the image already given: read once, and
          // logged as ignored.
          const std::string zip = to_utf8(fs::absolute(src / L"startrek-images.zip").make_preferred().wstring());
          CHECK(logged("ignoring " + zip + "!st-disk1.img: the same image as " +
                       to_utf8(fs::absolute(st1).make_preferred().wstring())));
          CHECK(r.parts.size() == 2 && r.parts[1].path == zip + "!st-disk2.img");
        }
      }
      // Names in code page 437, as Explorer and 7-Zip on an English Windows
      // write a name that fits it (general-purpose bit 11 clear; 0x81
      // u-umlaut, 0x94 o-umlaut), and a volume label in it: decoded, and
      // import.json is UTF-8 JSON.
      auto utf8_source = [&](const ImportResult& r) {
        CHECK(test::strict_utf8(test::read_text(r.import_json)));
        return json_at(r.import_json).at("source");
      };
      test::write_bytes(src / L"st-cp437.zip", test::zip_of_images({{"DISK\x81\x94" "1.IMG", test::read_bytes(st1)},
                                                                   {"DISK\x81\x94" "2.IMG", test::read_bytes(st2)}}));
      // Two names that differ only in a letter outside ASCII: two names.
      test::write_bytes(src / L"st-cp437-pair.zip",
                        test::zip_of_images({{"TREK\x81.IMG", test::read_bytes(st1)}, {"TREK\x94.IMG", test::read_bytes(st2)}},
                                            false));
      for (const auto& [zip, one, two] : std::vector<std::tuple<std::wstring, std::string, std::string>>{
               {L"st-cp437.zip", "DISK\xC3\xBC\xC3\xB6" "1.IMG", "DISK\xC3\xBC\xC3\xB6" "2.IMG"},
               {L"st-cp437-pair.zip", "TREK\xC3\xBC.IMG", "TREK\xC3\xB6.IMG"}}) {
        ImportResult r = run(("startrek known set, code page 437 names: " + to_utf8(zip)).c_str(), image(src / zip),
                             opts_for(dir / (L"st-437-" + zip), by_md5), Status::ok);
        CHECK_EQ(r.verified, std::string("image"));
        check_installed(dir / (L"st-437-" + zip) / L"win", startrek, "packages/startrek");
        if (r.status != Status::ok) continue;
        const phosg::JSON s = utf8_source(r);
        const std::string where = to_utf8(fs::absolute(src / zip).make_preferred().wstring()) + "!";
        const auto& parts = s.at("parts").as_list();
        CHECK(parts.size() == 2 && parts[0]->get_string("path") == where + one && parts[1]->get_string("path") == where + two);
        CHECK_EQ(s.get_string("path"), where + one + " + " + where + two);
      }
      {
        const fs::path labelled = src / L"st-disk1-label437.img";
        test::write_bytes(labelled, st_disk(1, "STAR TR\x81K"));
        ImportResult r = run("startrek, a code page 437 volume label", image(labelled, {st2}),
                             opts_for(dir / L"st-label437", by_md5), Status::ok);
        CHECK_EQ(r.volume_id, std::string("STAR TR\xC3\xBCK"));
        if (r.status == Status::ok) CHECK_EQ(utf8_source(r).get_string("volumeId"), std::string("STAR TR\xC3\xBCK"));
      }
      ImportResult r = run("startrek disk 1 alone", image(st1), opts_for(dir / L"st-d1", by_md5), Status::source_invalid);
      CHECK_EQ(r.message, std::string("the source is missing ST_SND.DL_; importing Star Trek: The Screen Saver needs "
                                      "every install disk"));
      CHECK(logged("the source holds install disk 1 of 2 of Star Trek: The Screen Saver (by md5), not the whole set"));
      r = run("startrek disk 2 alone", image(st2b), opts_for(dir / L"st-d2", by_md5), Status::source_invalid);
      CHECK_EQ(r.message, std::string("this image is install disk 2 of 2 of Star Trek: The Screen Saver (by its md5); "
                                      "import every disk together (--image … --image …, or the ZIP they came in)"));
      g_log.clear();
      r = run("startrek two copies of disk 1", image(st1, {st1b}), opts_for(dir / L"st-d11", by_md5),
              Status::source_invalid);
      CHECK(r.message.find("missing ST_SND.DL_; importing Star Trek: The Screen Saver needs every install disk") !=
            std::string::npos);
      CHECK(logged("the source holds install disk 1 of 2 of Star Trek: The Screen Saver (by md5), not the whole set"));
      // Disk 2 with an image that holds no release at all.
      test::FatBuilder junk = test::FatBuilder::floppy144();
      junk.file("README.TXT", test::vec("not a disk of any release"));
      test::write_bytes(src / L"junk-disk.img", junk.build());
      r = run("startrek disk 2 and a stranger", image(st2, {src / L"junk-disk.img"}), opts_for(dir / L"st-d2j", by_md5),
              Status::source_invalid);
      CHECK_EQ(r.message, std::string("these images hold install disk 2 of 2 of Star Trek: The Screen Saver (by their "
                                      "md5s) but not the rest of it; import every disk together (--image … --image …, "
                                      "or the ZIP they came in)"));
      // Both copies of disk 2 are still disk 2 alone, named once.
      r = run("startrek both copies of disk 2", image(st2, {st2b}), opts_for(dir / L"st-d22", by_md5),
              Status::source_invalid);
      CHECK_EQ(r.message, std::string("these images hold install disk 2 of 2 of Star Trek: The Screen Saver (by their "
                                      "md5s) but not the rest of it; import every disk together (--image … --image …, "
                                      "or the ZIP they came in)"));
      // The whole set and an image of nothing known: the release, but not
      // "verified": "image" (the images are not only its disks); every file
      // is checked instead. The log says every disk is there, not that one
      // is missing.
      g_log.clear();
      r = run("startrek set and a stranger", image(st1, {st2, src / L"junk-disk.img"}), opts_for(dir / L"st-set-j", by_md5),
              Status::ok);
      CHECK_EQ(r.verified, std::string("files"));
      CHECK(!r.iso_md5_known);
      CHECK(logged(every_disk_and_another));
      CHECK(!logged("not the whole set"));
      check_installed(dir / L"st-set-j" / L"win", startrek, "packages/startrek");
      // Every install disk exactly once: disk 1 and both copies of disk 2 are
      // the release, verified file by file, but not its known set.
      g_log.clear();
      r = run("startrek disk 1 and both copies of disk 2", image(st1, {st2, st2b}), opts_for(dir / L"st-set-22", by_md5),
              Status::ok);
      CHECK_EQ(r.verified, std::string("files"));
      CHECK(!r.iso_md5_known);
      CHECK_EQ(r.parts.size(), size_t(3));
      CHECK_EQ(json_at(r.import_json).at("source").get_bool("imageMd5Known"), false);
      CHECK(logged(every_disk_and_another));
      CHECK(!logged("not the whole set"));
      check_installed(dir / L"st-set-22" / L"win", startrek, "packages/startrek");
      g_log.clear();
      r = run("startrek set, disk 2 again and a stranger", image(st1, {st2, st2b, src / L"junk-disk.img"}),
              opts_for(dir / L"st-set-22j", by_md5), Status::ok);
      CHECK_EQ(r.verified, std::string("files"));
      CHECK(logged("the images hold every install disk of Star Trek: The Screen Saver (by md5) and 2 other images "
                   "besides; checking files individually"));
      // A disk missing, a stranger beside it: still "not the whole set".
      g_log.clear();
      r = run("startrek disk 1 and a stranger", image(st1, {src / L"junk-disk.img"}), opts_for(dir / L"st-d1j", by_md5),
              Status::source_invalid);
      CHECK(r.message.find("missing ST_SND.DL_; importing Star Trek: The Screen Saver needs every install disk") !=
            std::string::npos);
      CHECK(logged("the source holds install disk 1 of 2 of Star Trek: The Screen Saver (by md5), not the whole set; "
                   "checking files individually"));
      CHECK(!logged("the images hold every install disk"));
      r = run("startrek set + --package swse", image(st1, {st2}, "swse"), opts_for(dir / L"st-pkg", by_md5),
              Status::source_invalid);
      CHECK_EQ(r.message, std::string("these images are Star Trek: The Screen Saver (by their md5s), not Star Wars "
                                      "Screen Entertainment"));
      // Known images of two releases together.
      test::TestRegistry two;
      two.disk_images("startrek", {{md5_file_hex(st1), fs::file_size(st1), 1}, {md5_file_hex(st2), fs::file_size(st2), 2}});
      two.image("simpsons", md5_file_hex(src / L"simpsons.img"), fs::file_size(src / L"simpsons.img"));
      r = run("startrek disk + the Simpsons image", image(st1, {src / L"simpsons.img"}), opts_for(dir / L"st-two", two),
              Status::source_invalid);
      CHECK(r.message.find("two different releases") != std::string::npos);
      // Without the known md5s, disk 2 is just an unknown image.
      r = run("startrek disk 2, md5 unknown", image(st2), opts_for(dir / L"st-d2u", reg), Status::source_invalid);
      CHECK(r.message.find("not a known release") != std::string::npos);
      CHECK(!fs::exists(dir / L"st-d1" / L"win" / L"packages" / L"startrek"));
    }
    // I5: the decoys, locked in a folder source, are never opened.
    {
      std::vector<HANDLE> held;
      for (const std::string& decoy : test::startrek_decoys())
        held.push_back(CreateFileW((src / L"startrek" / to_wide(decoy)).c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0,
                                   nullptr));
      for (HANDLE h : held) CHECK(h != INVALID_HANDLE_VALUE);
      run("startrek folder, decoys locked", folder(src / L"startrek"), opts_for(dir / L"startrek-locked", reg), Status::ok);
      check_installed(dir / L"startrek-locked" / L"win", startrek, "packages/startrek");
      for (HANDLE h : held) CloseHandle(h);
    }
    // Identification: the setup window's title (any ASCII case), SETUP.LST's
    // size, disk 1's tag file beside it, --package.
    {
      auto st = [&](const wchar_t* name, const std::function<void(test::Tree&)>& change) {
        fs::path p = src / name;
        test::write_tree(p, test::startrek_fixture(change).source);
        return p;
      };
      auto other = st(L"v-st-title", [](test::Tree& t) { t["SETUP.LST"] = test::setup_lst("Star Trek: The Next Generation"); });
      ImportResult r = run("startrek, another title", folder(other), opts_for(dir / L"st-title", reg), Status::source_invalid);
      CHECK(r.message.find("not a known release") != std::string::npos);
      auto upper = st(L"v-st-upper", [](test::Tree& t) { t["SETUP.LST"] = test::setup_lst("STAR TREK\xAE: THE SCREEN SAVER"); });
      r = run("startrek, the title in capitals", folder(upper), opts_for(dir / L"st-upper", reg), Status::ok);
      CHECK_EQ(r.package_id, std::string("startrek"));
      auto big = st(L"v-st-biglst", [](test::Tree& t) { t["SETUP.LST"].resize(70 * 1024, ' '); });
      r = run("startrek, a 70 KB SETUP.LST", folder(big), opts_for(dir / L"st-biglst", reg), Status::source_invalid);
      CHECK(r.message.find("not a known release") != std::string::npos);
      auto notag = st(L"v-st-notag", [](test::Tree& t) { t.erase("MISSION.AD_"); });
      r = run("startrek, no MISSION.AD_ beside SETUP.LST", folder(notag), opts_for(dir / L"st-notag", reg),
              Status::source_invalid);
      CHECK(r.message.find("not a known release") != std::string::npos);
      auto nodisk2 = st(L"v-st-nodisk2", [](test::Tree& t) { t.erase("ST_SND.DL_"); });
      r = run("startrek, disk 2's tag file missing", folder(nodisk2), opts_for(dir / L"st-nodisk2", reg),
              Status::source_invalid);
      CHECK_EQ(r.message, std::string("the source is missing ST_SND.DL_; importing Star Trek: The Screen Saver needs "
                                      "every install disk"));
      r = run("--package startrek on ad32", folder(src / L"ad32", "startrek"), opts_for(dir / L"st-pkg-ad32", reg),
              Status::source_invalid);
      CHECK(r.message.find("not Star Trek: The Screen Saver") != std::string::npos);
      CHECK(identify_folder(src / L"startrek") == find_package("startrek"));
    }
    // Damaged KWAJ files: 2 for what the reader refuses, 3 for what only the
    // manifest can tell (KWAJ has no checksum and no length), and nothing
    // changes, not even the catalog.
    {
      fs::path croot = dir / L"startrek-corrupt";
      run("setup tt", image(src / L"tt.iso"), opts_for(croot, reg), Status::ok);
      run("setup startrek", folder(src / L"startrek"), opts_for(croot, reg), Status::ok);
      const test::Tree before = snapshot(croot / L"win");
      auto corrupt = [&](const char* what, const wchar_t* name, const std::function<void(test::Tree&)>& change,
                         Status want, const std::string& in_message, bool copies = true) {
        fs::path p = src / name;
        test::write_tree(p, test::startrek_fixture(change).source);
        ImportOptions o = opts_for(croot, reg);
        bool copied = false;
        o.progress = [&](const Progress& pr) {
          copied = copied || pr.phase == Progress::Phase::copy;
          return true;
        };
        ImportResult r = run(what, folder(p), o, want);
        CHECK(r.message.find(in_message) != std::string::npos);
        if (!copies) CHECK(!copied);
        CHECK(snapshot(croot / L"win") == before);
        CHECK(no_leftovers(croot / L"win"));
      };
      corrupt("a table of another type", L"v-st-type", [](test::Tree& t) { t["COMMS.AD_"][14] = 0x40; },
              Status::source_invalid, "COMMS.AD_: MATCHLEN: code-length encoding 4 is not one of 0-3", false);
      corrupt("a header flag", L"v-st-flag", [](test::Tree& t) { t["COMMS.AD_"][12] = 0x01; }, Status::source_invalid,
              "COMMS.AD_: KWAJ header flags 0x0001 are not supported", false);
      corrupt("an SZDD file", L"v-st-szdd",
              [](test::Tree& t) { t["AD_RSRC.DL_"] = test::szdd_encode(test::vec("an SZDD file where KWAJ belongs")); },
              Status::source_invalid, "AD_RSRC.DL_: an SZDD file, not KWAJ", false);
      corrupt("tables cut short", L"v-st-cuttab", [](test::Tree& t) { t["SPOCK.AD_"].resize(16); },
              Status::source_invalid, "SPOCK.AD_: the compressed data ends inside the code tables", false);
      corrupt("a token cut short", L"v-st-cuttok", [](test::Tree& t) { t["JIM.WA_"].resize(t["JIM.WA_"].size() - 5); },
              Status::source_invalid, "JIM.WA_: the compressed data ends inside a token", false);
      corrupt("a literal changed (no checksum: the manifest tells)", L"v-st-lit",
              [](test::Tree& t) { t["TRIBBLE.AD_"][14 + 10] ^= 0x01; }, Status::verify_failed,
              "differ from the release of Star Trek: The Screen Saver");
      // Cut after its first run of 32 literals (24 bits of tables, 265 bits
      // a run: the next run begins 7 bits before the end, inside the padding):
      // a clean prefix, which the manifest's size refuses before a byte is
      // written.
      const size_t wav = startrek.expect.at("packages/startrek/AFTERDRK/SOUNDS/JIM.WAV").size();
      corrupt("cut to a clean prefix", L"v-st-prefix", [](test::Tree& t) { t["JIM.WA_"].resize(14 + (24 + 265 + 7) / 8); },
              Status::verify_failed,
              "packages/startrek/AFTERDRK/SOUNDS/JIM.WAV is 32 bytes, not " + std::to_string(wav), false);
      // The staging budget sees the expanded sizes, before a byte is written.
      auto budget = [&](const char* what, ImportOptions o, const std::string& in_message) {
        bool copied = false;
        o.progress = [&](const Progress& pr) {
          copied = copied || pr.phase == Progress::Phase::copy;
          return true;
        };
        ImportResult r = run(what, folder(src / L"startrek"), o, Status::source_invalid);
        CHECK(r.message.find(in_message) != std::string::npos);
        CHECK(!copied);
        CHECK(snapshot(croot / L"win") == before);
      };
      ImportOptions few = opts_for(croot, reg);
      few.max_files = 10;
      budget("startrek over a 10-file budget", few, "more than 10 files");
      ImportOptions small = opts_for(croot, reg);
      small.max_bytes = 4000;
      budget("startrek over a 4000-byte budget", small,
             "the source's files add up to more than 4000 bytes (at PLANETS.AD_); no known release is that large");
      // A cancelled re-import changes nothing either.
      ImportOptions cancel = opts_for(croot, reg);
      cancel.progress = [](const Progress& pr) { return !(pr.phase == Progress::Phase::copy && pr.done > 2000); };
      run("startrek re-import, cancelled while copying", image(st1, {st2}), cancel, Status::cancelled);
      CHECK(snapshot(croot / L"win") == before);
      CHECK(no_leftovers(croot / L"win"));
    }
    // The ad2kwaj invariants (registries without manifests: nothing to
    // verify against) and the required files.
    {
      const Package& builtin = *find_package("startrek");
      const std::vector<LooseFile> loose(builtin.loose_files.begin(), builtin.loose_files.end());
      auto st_inv = [&](const char* what, const wchar_t* name, const std::function<void(test::Tree&)>& change,
                        const std::function<void(std::vector<LooseFile>&)>& table, const std::string& expect,
                        bool lax = false) {
        fs::path p = src / name;
        test::write_tree(p, test::startrek_fixture(change).source);
        test::TestRegistry r;
        std::vector<LooseFile> l = loose;
        if (table) table(l);
        r.get("startrek").loose_files = l;
        if (lax) r.get("startrek").required = {};
        fs::path root = dir / (L"inv-" + std::wstring(name));
        ImportResult res = run(what, folder(p), opts_for(root, r, false), Status::source_invalid);
        CHECK(res.message.find(expect) != std::string::npos);
        CHECK(!fs::exists(root / L"win" / L"packages" / L"startrek"));
        CHECK(!fs::exists(root / L"win") || no_leftovers(root / L"win"));
      };
      auto move_row = [](const char* from, const char* to) {
        return [from, to](std::vector<LooseFile>& l) {
          for (LooseFile& lf : l)
            if (std::string_view(lf.from) == from) lf.to = to;
        };
      };
      auto add_row = [](const char* from, const char* to, Codec codec = Codec::kwaj) {
        return [from, to, codec](std::vector<LooseFile>& l) { l.push_back({from, to, codec}); };
      };
      st_inv("I1: AD.EXE beside the modules", L"v-st-i1exe", {}, move_row("AD.EX_", "AFTERDRK/AD.EXE"),
             "breaks I1: AFTERDRK\\AD.EXE belongs in ENGINE");
      st_inv("I1: AD_SND beside the modules", L"v-st-i1snd", {}, add_row("AD_SND.DL_", "AFTERDRK/AD_SND.DLL"),
             "breaks I1: AFTERDRK\\AD_SND.DLL would shadow the engine's");
      st_inv("I2: a module needs FOO", L"v-st-i2",
             [](test::Tree& t) { t["NEEDY.AD_"] = test::kwaj_literals(test::ad20_module(" Needy", {"KERNEL", "FOO"})); },
             add_row("NEEDY.AD_", "AFTERDRK/NEEDY.AD"), "breaks I2: AFTERDRK\\NEEDY.AD needs FOO, which is not beside it");
      st_inv("I2: the sound driver needs MMEXTRA", L"v-st-i2drv",
             [](test::Tree& t) { t["AD_MME.DR_"] = test::kwaj_literals(test::ne_dll("AD_MME", {"KERNEL", "MMEXTRA"})); },
             {}, "breaks I2: AFTERDRK\\AD_MME.DRV needs MMEXTRA, which is not beside it");
      st_inv("I3: no ENGINE\\AD_SND.DLL", L"v-st-i3snd", {},
             [](std::vector<LooseFile>& l) {
               l.erase(std::remove_if(l.begin(), l.end(), [](const LooseFile& lf) { return std::string_view(lf.from) == "AD_SND.DL_"; }),
                       l.end());
             },
             "breaks I3: no ENGINE\\AD_SND.DLL", true);
      st_inv("I3: After Dark 4.0's ADTASK in ENGINE", L"v-st-i3task", {}, add_row("ST_SVGA.DL_", "ENGINE/ADTASK.DLL"),
             "breaks I3: ENGINE\\ADTASK.DLL belongs to After Dark 3.x and 4.x");
      st_inv("I3: a WINDOWS folder", L"v-st-i3win", {}, add_row("SETUP.LST", "WINDOWS/AD_PREFS.INI", Codec::plain),
             "breaks I3: WINDOWS\\AD_PREFS.INI: an After Dark 2.0 package has no WINDOWS folder");
      st_inv("I4: the sound database beside the modules", L"v-st-i4", {},
             move_row("ST_SND.DL_", "AFTERDRK/ST_SND.DLL"),
             "breaks I4: the sound database AFTERDRK\\ST_SND.DLL is not in AFTERDRK\\ST_RES", true);
      st_inv("required: no AFTERDRK\\AD_MOD.DLL", L"v-st-req", [](test::Tree& t) { t.erase("AD_MOD.DL_"); }, {},
             "missing required file(s): packages/startrek/AFTERDRK/AD_MOD.DLL");
    }
  }

  // ---- Marvel Comics Screen Posters and Snoopy's Screen Savers: InstallShield 2 libraries ------------
  {
    auto froms = [](const fs::path& import_json) {
      std::map<std::string, std::string> from;
      phosg::JSON j = json_at(import_json);
      for (auto& f : j.at("files").as_list()) from[f->get_string("path")] = f->get_string("from");
      return from;
    };
    // Nothing the import wrote or said names a file it must never open: the
    // installer's, the libraries the recipe does not read, the owners' notes
    // (the folder forms below hold them locked too).
    auto never_named = [&](const fs::path& root, const ImportResult& r, const test::IslibFixture& f) {
      std::string text = test::read_text(r.import_json) + test::read_text(root / L"win" / L"catalog-win.json");
      for (auto& l : g_log) text += l;
      const std::string upper = ascii_upper(text);
      for (const std::vector<std::string>* names : {&f.notes, &f.decoys})
        for (const std::string& n : *names)
          if (upper.find(ascii_upper(n)) != std::string::npos) {
            test::g_failures++;
            fprintf(stderr, "  %s is named\n", n.c_str());
          }
    };
    // A copy's local date and time (DOS times are local).
    auto local_time = [](const fs::path& p) {
      WIN32_FILE_ATTRIBUTE_DATA a{};
      GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a);
      SYSTEMTIME utc{}, st{};
      FileTimeToSystemTime(&a.ftLastWriteTime, &utc);
      SystemTimeToTzSpecificLocalTime(nullptr, &utc, &st);  // that date's daylight rules, as dos_filetime's
      return st;
    };
    struct Form {
      std::string id;
      const test::IslibFixture* f;
      std::wstring name;
      Source source;
      const char* format;
      const char* kind;
      size_t parts;
    };
    std::vector<Form> forms;
    for (const auto& [id, f] : std::vector<std::pair<std::string, const test::IslibFixture*>>{{"marvel", &marvel},
                                                                                              {"snoopy", &snoopy}}) {
      const std::wstring n = to_wide(id);
      const fs::path d1 = src / (n + L"-disk1.img"), d2 = src / (n + L"-disk2.img");
      forms.push_back({id, f, n + L"-folder", folder(src / n), "folder", "folder", 0});
      forms.push_back({id, f, n + L"-zip", image(src / (n + L".zip")), "zip", "zip", 0});
      forms.push_back({id, f, n + L"-disk-folders", folder(src / (n + L"-disks")), "folder", "folder", 0});
      forms.push_back({id, f, n + L"-disk-zip", image(src / (n + L"-disks.zip")), "zip", "zip", 0});
      forms.push_back({id, f, n + L"-floppies", image(d1, {d2}), "fat12", "floppy", 2});
      forms.push_back({id, f, n + L"-floppies-reversed", image(d2, {d1}), "fat12", "floppy", 2});
    }
    for (const Form& form : forms) {
      fs::path root = dir / (L"alone-" + form.name);
      g_log.clear();
      ImportResult r = run(to_utf8(form.name).c_str(), form.source, opts_for(root, reg), Status::ok);
      CHECK_EQ(r.package_id, form.id);
      CHECK_EQ(r.format, std::string(form.format));
      CHECK_EQ(r.verified, std::string("files"));
      CHECK_EQ(r.package_modules, form.f->ids.size());
      check_installed(root / L"win", *form.f, "packages/" + form.id);
      if (r.status != Status::ok) continue;
      CHECK(catalog_ids(root / L"win") == form.f->ids);
      phosg::JSON j = json_at(r.import_json);
      CHECK_EQ(j.at("package").get_string("recipe"), std::string("islib"));
      CHECK_EQ(j.at("source").get_string("kind"), std::string(form.kind));
      CHECK_EQ(j.at("source").at("parts").as_list().size(), form.parts);
      for (auto& f : j.at("files").as_list()) CHECK_EQ(f->get_string("known"), std::string("match"));
      // A member's 'from' names its volume, or both volumes when it crosses
      // the boundary; its copy keeps its DOS time.
      auto from = froms(r.import_json);
      phosg::JSON cat = json_at(root / L"win" / L"catalog-win.json");
      if (form.id == "marvel") {
        CHECK_EQ(from.size(), size_t(64));
        CHECK_EQ(from["packages/marvel/AFTERDRK/MRVLIMAG/XMEN2099.FIF"], std::string("IMAGES.1+IMAGES.2!XMEN2099.FIF"));
        CHECK_EQ(from["packages/marvel/AFTERDRK/MRVLIMAG/AV2.FTT"], std::string("IMAGES.1!AV2.FTT"));
        CHECK_EQ(from["packages/marvel/AFTERDRK/MRVLIMAG/XMENATTA.FIF"], std::string("IMAGES.2!XMENATTA.FIF"));
        CHECK_EQ(from["packages/marvel/AFTERDRK/MARVEL.AD"], std::string("MODULES.LIB!MARVEL.AD"));
        CHECK_EQ(from["packages/marvel/ENGINE/AD.EXE"], std::string("ENGINE.LIB!AD.EXE"));
        CHECK_EQ(from["packages/marvel/ENGINE/AD_SND.DLL"], std::string("WIN.LIB!AD_SND.DLL"));
        // The members no row names are listed, never decoded (decoding any
        // of them would fail the import).
        CHECK(logged("skipped ENGINE.LIB!ADINIT.EXE, ENGINE.LIB!AD_LIB.DLL, "));
        CHECK(logged(", WIN.LIB!AD_PREFS.INI (not in the recipe of Marvel Comics Screen Posters)"));
        const SYSTEMTIME st = local_time(r.files_dir / L"AFTERDRK" / L"MARVEL.AD");
        CHECK(st.wYear == 1993 && st.wMonth == 12 && st.wDay == 13 && st.wHour == 23 && st.wMinute == 18);
        const phosg::JSON* m = module_by_id(cat, "marvel.marvel");
        CHECK(m != nullptr);
        if (m) {
          CHECK_EQ(m->get_string("displayName"), std::string("Marvel Comics"));
          CHECK_EQ(m->get_string("lane"), std::string("ne16"));
          CHECK_EQ(m->get_string("screen"), std::string("640x480"));
          CHECK(m->at("needs").as_list().size() == 1 && m->at("needs").as_list()[0]->as_string() == "DECO");
        }
      } else {
        CHECK_EQ(from.size(), size_t(8));
        CHECK_EQ(from["packages/snoopy/AFTERDRK/IS_FLY.AD"], std::string("AD_MODS.1+AD_MODS.2!IS_FLY.AD"));
        CHECK_EQ(from["packages/snoopy/AFTERDRK/IS_COLAG.AD"], std::string("AD_MODS.1!IS_COLAG.AD"));
        CHECK_EQ(from["packages/snoopy/AFTERDRK/IS_THRPY.AD"], std::string("AD_MODS.2!IS_THRPY.AD"));
        CHECK(!logged("skipped"));  // the table takes every member
        CHECK(!fs::exists(r.files_dir / L"ENGINE"));  // the release ships no engine
        const SYSTEMTIME st = local_time(r.files_dir / L"AFTERDRK" / L"IS_FLY.AD");
        CHECK(st.wYear == 1994 && st.wMonth == 10 && st.wDay == 13 && st.wHour == 15 && st.wMinute == 32);
        size_t sound = 0;
        for (auto& m : cat.at("modules").as_list()) {
          CHECK(!m->contains("screen"));
          sound += m->at("needs").as_list().size() == 1 && m->at("needs").as_list()[0]->as_string() == "AD_SND";
        }
        CHECK_EQ(sound, size_t(6));
        const phosg::JSON* linus = module_by_id(cat, "snoopy.is_linus");
        CHECK(linus && linus->get_string("displayName") == "Linus & Snoopy");
      }
      // The disk sets say how they were read (a folder's names upper case).
      if (form.name.find(L"-disk-") != std::wstring::npos)
        CHECK(logged(" as the union of its folders Disk1 and Disk2 (one install disk each)") ||
              logged(" as the union of its folders DISK1 and DISK2 (one install disk each)"));
      CHECK(!fs::exists(r.files_dir / L"WINDOWS"));
      never_named(root, r, *form.f);
      CHECK(no_leftovers(root / L"win"));
    }
    // The ZIP's md5 is a known image: verified "image", flat or with the
    // disks in folders.
    for (const auto& [id, zip, f] : std::vector<std::tuple<std::string, fs::path, const test::IslibFixture*>>{
             {"marvel", src / L"marvel.zip", &marvel},
             {"marvel", src / L"marvel-disks.zip", &marvel},
             {"snoopy", src / L"snoopy-disks.zip", &snoopy}}) {
      test::TestRegistry by_md5 = registry_for({{id, f}});
      by_md5.image(id, md5_file_hex(zip), fs::file_size(zip));
      fs::path root = dir / (L"known-zip-" + zip.stem().wstring());
      g_log.clear();
      ImportResult r = run((id + ", the known ZIP").c_str(), image(zip), opts_for(root, by_md5), Status::ok);
      CHECK_EQ(r.verified, std::string("image"));
      CHECK(r.iso_md5_known);
      CHECK(logged("a ZIP of install files, the known copy of " + std::string(find_package(id)->title) + " (by its md5)"));
      check_installed(root / L"win", *f, "packages/" + id);
      if (r.status == Status::ok) CHECK_EQ(json_at(r.import_json).at("source").get_string("imageMd5"), md5_file_hex(zip));
    }
    // Every install disk is needed: disk 1 (SETUP.PKG and the first volume)
    // is identified and refused; disk 2 alone is no release.
    {
      const std::string mv = "the source is missing IMAGES.2, MODULES.LIB, ENGINE.LIB, WIN.LIB; importing Marvel Comics "
                             "Screen Posters needs every install disk";
      const std::string sn = "the source is missing AD_MODS.2; importing Snoopy's Screen Savers needs every install disk";
      int n = 0;
      for (const auto& [what, source, message] : std::vector<std::tuple<std::string, Source, std::string>>{
               {"marvel disk 1 alone", folder(src / L"marvel-disk1"), mv},
               {"marvel disk 1 alone (floppy)", image(src / L"marvel-disk1.img"), mv},
               {"snoopy disk 1 alone", folder(src / L"snoopy-disk1"), sn},
               {"snoopy disk 1 alone (floppy)", image(src / L"snoopy-disk1.img"), sn}}) {
        ImportResult r = run(what.c_str(), source, opts_for(dir / (L"islib-disk1-" + std::to_wstring(n++)), reg),
                             Status::source_invalid);
        CHECK_EQ(r.message, message);
      }
      for (const auto& [what, source] : std::vector<std::pair<std::string, Source>>{
               {"marvel disk 2 alone", folder(src / L"marvel-disk2")},
               {"marvel disk 2 alone (floppy)", image(src / L"marvel-disk2.img")},
               {"snoopy disk 2 alone", folder(src / L"snoopy-disk2")},
               {"snoopy disk 2 alone (floppy)", image(src / L"snoopy-disk2.img")}}) {
        ImportResult r = run(what.c_str(), source, opts_for(dir / L"islib-disk2", reg), Status::source_invalid);
        CHECK(r.message.find("not a known release") != std::string::npos);
      }
      CHECK(!fs::exists(dir / L"islib-disk1-0" / L"win" / L"packages"));
      CHECK(!fs::exists(dir / L"islib-disk2" / L"win" / L"packages"));
    }
    // I5: the installer's files, the libraries the recipe does not read and
    // the owners' notes, locked in the folder sources, are never opened; nor
    // is an AD 3.x install's SETUP.PKG, which no library volume is beside.
    {
      std::vector<HANDLE> held;
      auto lock = [&](const fs::path& p) {
        held.push_back(CreateFileW(p.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr));
        CHECK(held.back() != INVALID_HANDLE_VALUE);
      };
      for (const auto& [name, f] : std::vector<std::pair<std::wstring, const test::IslibFixture*>>{{L"marvel", &marvel},
                                                                                                     {L"snoopy", &snoopy}})
        for (const std::vector<std::string>* names : {&f->decoys, &f->notes})
          for (const std::string& n : *names) {
            lock(src / name / to_wide(n));
            const int k = f->disk.at(n);
            lock(src / (name + L"-disks") / (L"Disk" + std::to_wstring(k ? k : 1)) / to_wide(n));
            if (!k) lock(src / (name + L"-disks") / L"Disk2" / to_wide(n));
          }
      lock(src / L"looney" / L"SETUP.PKG");
      for (const auto& [id, name, f] : std::vector<std::tuple<std::string, std::wstring, const test::PkgFixture*>>{
               {"marvel", L"marvel", &marvel}, {"marvel", L"marvel-disks", &marvel}, {"snoopy", L"snoopy", &snoopy},
               {"snoopy", L"snoopy-disks", &snoopy}, {"looney", L"looney", &looney}}) {
        fs::path root = dir / (L"locked-islib-" + name);
        run((to_utf8(name) + " folder, never-read files locked").c_str(), folder(src / name), opts_for(root, reg),
            Status::ok);
        check_installed(root / L"win", *f, "packages/" + id);
      }
      for (HANDLE h : held) CloseHandle(h);
    }
    // Identification: SETUP.PKG must list the tag member in the tag library,
    // be a package list at all (a damaged one names nothing, and crashes
    // nothing), be of plausible size, and have disk 1's library volume beside
    // it; --package.
    {
      auto variant = [&](const wchar_t* name, const test::IslibFixture& base,
                         const std::function<void(test::Tree&)>& change) {
        test::Tree t = base.source;
        change(t);
        fs::path p = src / name;
        test::write_tree(p, t);
        return p;
      };
      const auto renamed = test::marvel_fixture([](std::vector<test::IslibLibrary>& libs) {
        for (auto& lib : libs)
          for (auto& m : lib.members)
            if (m.name == "MARVEL.AD") m.name = "MARVELX.AD";
      });
      const std::vector<std::pair<std::string, fs::path>> unknown = {
          {"no MARVEL.AD in modules.lib", variant(L"v-mv-tag", renamed, [](test::Tree&) {})},
          {"a 70 KB SETUP.PKG", variant(L"v-mv-bigpkg", marvel, [](test::Tree& t) { t["SETUP.PKG"].resize(70 * 1024); })},
          {"SETUP.PKG's magic", variant(L"v-mv-magic", marvel, [](test::Tree& t) { t["SETUP.PKG"][1] = 0xA4; })},
          {"SETUP.PKG's disk table past its end",
           variant(L"v-mv-table", marvel, [](test::Tree& t) { t["SETUP.PKG"][3] = 0x7F; })},
          {"SETUP.PKG's first group a byte short", variant(L"v-mv-group", marvel, [](test::Tree& t) { t["SETUP.PKG"][10]--; })},
          {"SETUP.PKG cut short",
           variant(L"v-mv-cutpkg", marvel, [](test::Tree& t) { t["SETUP.PKG"].resize(t["SETUP.PKG"].size() - 1); })},
          {"SETUP.PKG with a byte after its disk table",
           variant(L"v-mv-strypkg", marvel, [](test::Tree& t) { t["SETUP.PKG"].push_back(0); })},
          {"no IMAGES.1 beside SETUP.PKG", variant(L"v-mv-noimg1", marvel, [](test::Tree& t) { t.erase("IMAGES.1"); })},
          {"no AD_MODS.1 beside SETUP.PKG", variant(L"v-sn-nomods1", snoopy, [](test::Tree& t) { t.erase("AD_MODS.1"); })},
      };
      for (const auto& [what, p] : unknown) {
        std::string why;
        CHECK(identify_folder(p, &why, reg.span()) == nullptr);
        CHECK(why.find("not a known release") != std::string::npos);
        fprintf(stderr, "[%s] %s\n", what.c_str(), why.c_str());
      }
      CHECK(identify_folder(src / L"marvel", nullptr, reg.span()) == &reg.get("marvel"));
      CHECK(identify_folder(src / L"snoopy-disks", nullptr, reg.span()) == &reg.get("snoopy"));
      CHECK(identify_folder(src / L"marvel") == find_package("marvel"));  // the built-in registry too
      // The disks beside something else (a desktop.ini, as on a copy Explorer
      // customised) are read as they are, so no release: the reason says why,
      // before the error (the GUI's caution shows it; the log is the CLI's).
      {
        const fs::path beside = src / L"marvel-disks-ini";
        for (int k = 1; k <= 2; k++) test::write_tree(beside / (L"Disk" + std::to_wstring(k)), marvel.disk_files(k));
        test::write_bytes(beside / L"desktop.ini", test::blob("desktop.ini", 40));
        std::string why;
        CHECK(identify_folder(beside, &why, reg.span()) == nullptr);
        const std::string note = to_utf8(beside.wstring()) +
                                 " holds DISK1 and DISK2 beside other files or folders (DESKTOP.INI): reading it as it "
                                 "is (install disks kept apart are read together only from a folder that holds nothing "
                                 "else); not a known release";
        CHECK(why.rfind(note, 0) == 0);
        fprintf(stderr, "[disks beside desktop.ini] %s\n", why.c_str());
      }
      ImportResult r = run("--package marvel on Snoopy", folder(src / L"snoopy", "marvel"),
                           opts_for(dir / L"id-mv-sn", reg), Status::source_invalid);
      CHECK_EQ(r.message,
               std::string("the source is not Marvel Comics Screen Posters (it looks like Snoopy's Screen Savers)"));
      r = run("--package snoopy on 3.2", folder(src / L"ad32", "snoopy"), opts_for(dir / L"id-sn-ad32", reg),
              Status::source_invalid);
      CHECK_EQ(r.message, std::string("the source is not Snoopy's Screen Savers (it looks like After Dark 3.2)"));
    }
    // A volume that says the library goes on past the registry's volumes:
    // damaged or foreign, and the file it names (locked) is never opened.
    {
      test::Tree t = marvel.source;
      t["IMAGES.1"][0x1E] = 3;  // volume 1 of a set of three
      test::write_tree(src / L"v-mv-3vol", t);
      test::write_bytes(src / L"v-mv-3vol" / L"IMAGES.3", test::blob("a third volume", 400));
      HANDLE held =
          CreateFileW((src / L"v-mv-3vol" / L"IMAGES.3").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
      CHECK(held != INVALID_HANDLE_VALUE);
      ImportResult r = run("marvel, IMAGES.1 says there are three volumes", folder(src / L"v-mv-3vol"),
                           opts_for(dir / L"mv-3vol", reg), Status::source_invalid);
      CHECK_EQ(r.message, std::string("IMAGES.1 says the library continues on IMAGES.3, which is not one of the install "
                                      "disks of Marvel Comics Screen Posters (a damaged or foreign volume?)"));
      CloseHandle(held);
    }
    // Damaged sources: 2 for what the reader refuses, 3 for a literal changed
    // (DCL has no checksum: the manifest tells), and nothing changes, not
    // even the catalog.
    {
      fs::path croot = dir / L"islib-corrupt";
      run("setup tt", image(src / L"tt.iso"), opts_for(croot, reg), Status::ok);
      run("setup marvel", folder(src / L"marvel"), opts_for(croot, reg), Status::ok);
      const test::Tree before = snapshot(croot / L"win");
      auto corrupt = [&](const char* what, const wchar_t* name, const test::Tree& t, Status want,
                         const std::string& in_message, bool copies = true) {
        fs::path p = src / name;
        test::write_tree(p, t);
        ImportOptions o = opts_for(croot, reg);
        bool copied = false;
        o.progress = [&](const Progress& pr) {
          copied = copied || pr.phase == Progress::Phase::copy;
          return true;
        };
        ImportResult r = run(what, folder(p), o, want);
        CHECK(r.message.find(in_message) != std::string::npos);
        if (!copies) CHECK(!copied);
        CHECK(snapshot(croot / L"win") == before);
        CHECK(no_leftovers(croot / L"win"));
      };
      // The fixture with one member edited before the libraries are laid out.
      auto with_member = [](const std::string& member, const std::function<void(test::IslibMember&)>& edit) {
        return test::marvel_fixture([&](std::vector<test::IslibLibrary>& libs) {
          for (auto& lib : libs)
            for (auto& m : lib.members)
              if (m.name == member) edit(m);
        });
      };
      auto without_member = [](const std::string& member) {
        return test::marvel_fixture([&](std::vector<test::IslibLibrary>& libs) {
          for (auto& lib : libs)
            lib.members.erase(std::remove_if(lib.members.begin(), lib.members.end(),
                                             [&](const test::IslibMember& m) { return m.name == member; }),
                              lib.members.end());
        });
      };
      {
        test::Tree t = marvel.source;
        t["IMAGES.2"].resize(t["IMAGES.2"].size() - 10);
        corrupt("IMAGES.2 cut short", L"v-mv-cut2", t, Status::source_invalid, "IMAGES.2: ", false);
      }
      {
        test::Tree t = marvel.source;
        t["MODULES.LIB"] = test::blob("no library at all", 500);
        corrupt("MODULES.LIB no library", L"v-mv-junk", t, Status::source_invalid,
                "MODULES.LIB: not an InstallShield compressed library (no signature)", false);
      }
      {
        // IMAGES.2 of another set: its tables are not IMAGES.1's.
        test::Tree t = marvel.source;
        t["IMAGES.2"] = with_member("WOLVIE.FIF", [](test::IslibMember& m) { m.data.push_back(0x55); }).source.at("IMAGES.2");
        corrupt("IMAGES.2 of another set", L"v-mv-swap2", t, Status::source_invalid,
                "IMAGES.2: not a volume of the same set as IMAGES.1", false);
      }
      // The disks' volumes swapped (a mislabelled copy): the file named as
      // the set's first volume holds volume 2. It is there, so it is never
      // said to be missing.
      for (const auto& [what, name, f, first, second] :
           std::vector<std::tuple<const char*, const wchar_t*, const test::IslibFixture*, std::string, std::string>>{
               {"IMAGES.1 and IMAGES.2 swapped", L"v-mv-swapped", &marvel, "IMAGES.1", "IMAGES.2"},
               {"AD_MODS.1 and AD_MODS.2 swapped", L"v-sn-swapped", &snoopy, "AD_MODS.1", "AD_MODS.2"}}) {
        test::Tree t = f->source;
        std::swap(t.at(first), t.at(second));
        corrupt(what, name, t, Status::source_invalid,
                first + " is volume 2 of its set, not volume 1 (a mislabelled or foreign volume?)", false);
      }
      {
        // A library that does not hold the member SETUP.PKG says it does:
        // not the disks of one release.
        test::Tree t = marvel.source;
        t["MODULES.LIB"] = without_member("MARVEL.AD").source.at("MODULES.LIB");
        corrupt("MODULES.LIB without MARVEL.AD", L"v-mv-notag", t, Status::source_invalid,
                "SETUP.PKG lists MARVEL.AD in modules.lib, but MODULES.LIB holds no such member (not the disks of one "
                "release?)",
                false);
      }
      corrupt("a member's stream with dictionary bits 7", L"v-mv-dict",
              with_member("DECO.DLL",
                          [](test::IslibMember& m) {
                            m.stream = test::dcl_stream(m.data);
                            m.stream[1] = 7;
                          })
                  .source,
              Status::source_invalid, "MODULES.LIB!DECO.DLL: dictionary bits 7");
      corrupt("a crossing member cut before its end code", L"v-mv-noend",
              with_member("XMEN2099.FIF",
                          [](test::IslibMember& m) {
                            m.stream = test::dcl_stream(m.data);
                            m.stream.resize(m.stream.size() - 2);
                          })
                  .source,
              Status::source_invalid, "IMAGES.1+IMAGES.2!XMEN2099.FIF: ");
      corrupt("a literal changed (no checksum: the manifest tells)", L"v-mv-lit",
              with_member("DECO.DLL",
                          [](test::IslibMember& m) {
                            m.stream = test::dcl_stream(m.data);
                            m.stream[2] ^= 0x02;  // the first literal's lowest bit
                          })
                  .source,
              Status::verify_failed,
              "differ from the release of Marvel Comics Screen Posters (first: packages/marvel/AFTERDRK/DECO.DLL)");
      // The recorded sizes are what the staging budget and the manifest see,
      // before a byte is written.
      auto refused_early = [&](const char* what, const fs::path& source, ImportOptions o, Status want,
                               const std::string& in_message) {
        bool copied = false;
        o.progress = [&](const Progress& pr) {
          copied = copied || pr.phase == Progress::Phase::copy;
          return true;
        };
        ImportResult r = run(what, folder(source), o, want);
        CHECK(r.message.find(in_message) != std::string::npos);
        CHECK(!copied);
        CHECK(snapshot(croot / L"win") == before);
      };
      ImportOptions few = opts_for(croot, reg);
      few.max_files = 10;
      refused_early("marvel over a 10-file budget", src / L"marvel", few, Status::source_invalid, "more than 10 files");
      ImportOptions small = opts_for(croot, reg);
      small.max_bytes = 4000;
      refused_early("marvel over a 4000-byte budget", src / L"marvel", small, Status::source_invalid,
                    "more than 4000 bytes");
      test::write_tree(src / L"v-mv-size",
                       with_member("DECO.DLL", [](test::IslibMember& m) { m.data.resize(m.data.size() + 100, 0x5A); }).source);
      refused_early("a member of another size", src / L"v-mv-size", opts_for(croot, reg), Status::verify_failed,
                    "packages/marvel/AFTERDRK/DECO.DLL is " +
                        std::to_string(marvel.expect.at("packages/marvel/AFTERDRK/DECO.DLL").size() + 100) + " bytes");
      // A cancelled re-import changes nothing either.
      ImportOptions cancel = opts_for(croot, reg);
      cancel.progress = [](const Progress& pr) { return !(pr.phase == Progress::Phase::copy && pr.done > 2000); };
      run("marvel re-import, cancelled while copying", image(src / L"marvel-disks.zip"), cancel, Status::cancelled);
      CHECK(snapshot(croot / L"win") == before);
      CHECK(no_leftovers(croot / L"win"));
      // A member of the table the libraries lack (not the tag member):
      // logged, not planned; the manifest reports it missing ("partial").
      test::write_tree(src / L"v-mv-nocover", without_member("COVER.FTT").source);
      g_log.clear();
      ImportResult r = run("marvel without COVER.FTT", folder(src / L"v-mv-nocover"), opts_for(dir / L"mv-nocover", reg),
                           Status::ok);
      CHECK_EQ(r.verified, std::string("partial"));
      CHECK(r.missing_known == std::vector<std::string>{"packages/marvel/AFTERDRK/MRVLIMAG/COVER.FTT"});
      CHECK(logged("the source's IMAGES.1 has no COVER.FTT"));
    }
    // The islib invariants (registries without manifests: nothing to verify
    // against) and the required files.
    {
      const Package& builtin = *find_package("marvel");
      const std::vector<LibraryMember> table(builtin.library_members.begin(), builtin.library_members.end());
      std::deque<std::vector<LibraryMember>> tables;
      auto isl_inv = [&](const char* what, const wchar_t* name, const test::IslibFixture& f,
                         const std::function<void(std::vector<LibraryMember>&)>& edit, const std::string& expect,
                         bool lax = false) {
        fs::path p = src / name;
        test::write_tree(p, f.source);
        test::TestRegistry r;
        tables.push_back(table);
        if (edit) edit(tables.back());
        r.get("marvel").library_members = tables.back();
        if (lax) r.get("marvel").required = {};
        fs::path root = dir / (L"inv-" + std::wstring(name));
        ImportResult res = run(what, folder(p), opts_for(root, r, false), Status::source_invalid);
        CHECK(res.message.find(expect) != std::string::npos);
        CHECK(!fs::exists(root / L"win" / L"packages" / L"marvel"));
        CHECK(!fs::exists(root / L"win") || no_leftovers(root / L"win"));
      };
      auto move_row = [](const char* member, const char* to) {
        return [member, to](std::vector<LibraryMember>& t) {
          for (LibraryMember& row : t)
            if (std::string_view(row.member) == member) row.to = to;
        };
      };
      auto add_row = [](const char* library, const char* member, const char* to) {
        return [library, member, to](std::vector<LibraryMember>& t) { t.push_back({library, member, to}); };
      };
      // The fixture with a member's bytes replaced, or without the member.
      auto replaced = [](const char* member, std::vector<uint8_t> data) {
        return test::marvel_fixture([&](std::vector<test::IslibLibrary>& libs) {
          for (auto& lib : libs)
            for (auto& m : lib.members)
              if (m.name == member) m.data = data;
        });
      };
      auto dropped = [](const char* member) {
        return test::marvel_fixture([&](std::vector<test::IslibLibrary>& libs) {
          for (auto& lib : libs)
            lib.members.erase(std::remove_if(lib.members.begin(), lib.members.end(),
                                             [&](const test::IslibMember& m) { return m.name == member; }),
                              lib.members.end());
        });
      };
      isl_inv("I1: AD.EXE beside the module", L"v-mv-i1exe", marvel, move_row("AD.EXE", "AFTERDRK/AD.EXE"),
              "breaks I1: AFTERDRK\\AD.EXE belongs in ENGINE");
      isl_inv("I1: AD_SND beside the module", L"v-mv-i1snd", marvel, move_row("AD_SND.DLL", "AFTERDRK/AD_SND.DLL"),
              "breaks I1: AFTERDRK\\AD_SND.DLL would shadow the engine's", true);
      isl_inv("I2: the module needs FOO", L"v-mv-i2",
              replaced("MARVEL.AD", test::ne_module("Marvel Comics", {"KERNEL", "DECO", "FOO"})), {},
              "breaks I2: AFTERDRK\\MARVEL.AD needs FOO, which is not beside it");
      isl_inv("I2: its decoder needs BAR", L"v-mv-i2dll", replaced("DECO.DLL", test::ne_dll("DECO", {"KERNEL", "BAR"})),
              {}, "breaks I2: AFTERDRK\\DECO.DLL needs BAR, which is not beside it");
      isl_inv("I3: After Dark 3.x's ADTASK in ENGINE", L"v-mv-i3task", marvel,
              add_row("ENGINE.LIB", "AD.EXE", "ENGINE/ADTASK.DLL"),
              "breaks I3: ENGINE\\ADTASK.DLL belongs to After Dark 3.x and 4.x");
      isl_inv("I3: a WINDOWS folder", L"v-mv-i3win", marvel, add_row("WIN.LIB", "AD_SND.DLL", "WINDOWS/AD_SND.DLL"),
              "breaks I3: WINDOWS\\AD_SND.DLL: an InstallShield 2 package has no WINDOWS folder");
      isl_inv("I3: no ENGINE\\AD_SND.DLL", L"v-mv-i3snd", dropped("AD_SND.DLL"), {}, "breaks I3: no ENGINE\\AD_SND.DLL",
              true);
      isl_inv("required: no AFTERDRK\\DECO.DLL", L"v-mv-req", dropped("DECO.DLL"), {},
              "missing required file(s): packages/marvel/AFTERDRK/DECO.DLL");
    }
  }

  // ---- The Looney Tunes, ScreamSavers and the Disney Collection: AD 3.x installs known by a ZIP -------
  {
    auto froms = [](const fs::path& import_json) {
      std::map<std::string, std::string> from;
      phosg::JSON j = json_at(import_json);
      for (auto& f : j.at("files").as_list()) from[f->get_string("path")] = f->get_string("from");
      return from;
    };
    // Nothing the import wrote or said names the owner's note (or opens it:
    // the folder forms below hold it locked).
    auto never_named = [&](const fs::path& root, const ImportResult& r, const std::string& note) {
      std::string text = test::read_text(r.import_json) + test::read_text(root / L"win" / L"catalog-win.json");
      for (auto& l : g_log) text += l;
      std::string upper = ascii_upper(note), base = upper.substr(0, upper.find('.'));
      CHECK(ascii_upper(text).find(base) == std::string::npos);
    };
    struct Form {
      const char* id;
      const test::PkgFixture* f;
      const wchar_t* name;
      Source source;
      const char* format;
      const char* kind;
      size_t parts;
      const char* note;
    };
    std::vector<Form> forms = {
        {"looney", &looney, L"looney-folder", folder(src / L"looney"), "folder", "folder", 0, test::kLooneyNote},
        {"looney", &looney, L"looney-zip", image(src / L"looney.zip"), "zip", "zip", 0, test::kLooneyNote},
        {"screams", &screams, L"screams-folder", folder(src / L"screams"), "folder", "folder", 0, test::kScreamsNote},
        {"screams", &screams, L"screams-zip", image(src / L"screams.zip"), "zip", "zip", 0, test::kScreamsNote},
        {"screams", &screams, L"screams-floppies", image(sc_disks[0], {sc_disks[1], sc_disks[2]}), "fat12", "floppy",
         3, test::kScreamsNote},
        {"screams", &screams, L"screams-floppies-rev", image(sc_disks[2], {sc_disks[0], sc_disks[1]}), "fat12",
         "floppy", 3, test::kScreamsNote},
        {"disney", &disney, L"disney-folder", folder(src / L"disney"), "folder", "folder", 0, test::kDisneyNote},
        {"disney", &disney, L"disney-zip", image(src / L"disney.zip"), "zip", "zip", 0, test::kDisneyNote},
    };
    for (const Form& form : forms) {
      fs::path root = dir / (std::wstring(L"alone-") + form.name);
      g_log.clear();
      ImportResult r = run(to_utf8(form.name).c_str(), form.source, opts_for(root, reg), Status::ok);
      CHECK_EQ(r.package_id, std::string(form.id));
      CHECK_EQ(r.format, std::string(form.format));
      CHECK_EQ(r.verified, std::string("files"));
      CHECK_EQ(r.package_modules, form.f->ids.size());
      const std::string pkg_root = std::string("packages/") + form.id;
      check_installed(root / L"win", *form.f, pkg_root);
      CHECK(catalog_ids(root / L"win") == form.f->ids);
      if (r.status != Status::ok) continue;
      phosg::JSON j = json_at(r.import_json);
      CHECK_EQ(j.at("package").get_string("recipe"), std::string("ad3zip"));
      CHECK_EQ(j.at("source").get_string("kind"), std::string(form.kind));
      CHECK_EQ(j.at("source").at("parts").as_list().size(), form.parts);
      for (auto& f : j.at("files").as_list()) CHECK_EQ(f->get_string("known"), std::string("match"));
      auto from = froms(r.import_json);
      CHECK(logged("recovered the archive password"));
      check_no_password(root);
      never_named(root, r, form.note);
      const std::string id = form.id;
      if (id == "looney") {
        // The sound database comes from MUSIC.ZIP (there is no MUSICG.ZIP) and sits beside the modules.
        CHECK_EQ(from["packages/looney/LNYTUNES/LT_SOUND.DLL"], std::string("MUSIC.ZIP!LT_SOUND.DLL"));
        CHECK_EQ(from["packages/looney/LNYTUNES/MUSIC/GB&U.MID"], std::string("MUSIC.ZIP!GB&U.MID"));
        CHECK_EQ(from["packages/looney/LNYTUNES/FOLDER.AFI"], std::string("AFI.ZIP!LNYTUNES.AFI"));
      } else if (id == "screams") {
        CHECK_EQ(from["packages/screams/SCREAMS/FOLDER.AFI"], std::string("AFI.ZIP!SCREAMS.AFI"));
        CHECK_EQ(from["packages/screams/SCREAMS/ADXPL300.DLL"], std::string("MODMISC.ZIP!ADXPL300.DLL"));
      } else {
        // The copy's mixed-case names install upper case; the General MIDI
        // set wins; BEAUTYOL.ZIP is named as skipped and never opened (its
        // BEAUTY.AD would otherwise be a second file for one path).
        CHECK_EQ(from["packages/disney/DISNEY/BEAUTY.AD"], std::string("BEAUTY.ZIP!BEAUTY.AD"));
        CHECK_EQ(from["packages/disney/DISNEY/DIS_SND.DLL"], std::string("MODMISC.ZIP!DIS_SND.DLL"));
        CHECK_EQ(from["packages/disney/DISNEY/MUSIC/BANDB.MID"], std::string("MUSICG.ZIP!BANDB.MID"));
        CHECK_EQ(from["packages/disney/DISNEY/FOLDER.AFI"], std::string("AFI.ZIP!DISNEY.AFI"));
        CHECK(logged("skipped BEAUTYOL.ZIP (never opened: not in the recipe of The Disney Collection Screen Saver)"));
      }
      CHECK(no_leftovers(root / L"win"));
    }
    // ScreamSavers' disks in DISK1-DISK3 folders, as a folder and as a ZIP
    // (its only copy anywhere is such a ZIP): the files of the flat forms.
    for (const auto& [what, source] : std::vector<std::pair<std::string, Source>>{
             {"screams, DISK1-DISK3 folders", folder(src / L"screams-disks")},
             {"screams, a ZIP of DISK1-DISK3 folders", image(src / L"screams-disks.zip")}}) {
      fs::path root = dir / to_wide("alone-" + what.substr(what.find(", ") + 2));
      g_log.clear();
      ImportResult r = run(what.c_str(), source, opts_for(root, reg), Status::ok);
      CHECK_EQ(r.package_id, std::string("screams"));
      CHECK_EQ(r.verified, std::string("files"));
      check_installed(root / L"win", screams, "packages/screams");
      if (r.status == Status::ok) never_named(root, r, test::kScreamsNote);
    }
    // The ZIP's md5 is the known image: verified "image", whether the copy
    // is flat or keeps its disks in folders.
    for (const auto& [id, zip, f] : std::vector<std::tuple<std::string, fs::path, const test::PkgFixture*>>{
             {"looney", src / L"looney.zip", &looney},
             {"screams", src / L"screams-disks.zip", &screams},
             {"disney", src / L"disney.zip", &disney}}) {
      test::TestRegistry by_md5 = registry_for({{id, f}});
      by_md5.image(id, md5_file_hex(zip), fs::file_size(zip));
      fs::path root = dir / to_wide("known-zip-" + id);
      g_log.clear();
      ImportResult r = run((id + ", the known ZIP").c_str(), image(zip), opts_for(root, by_md5), Status::ok);
      CHECK_EQ(r.verified, std::string("image"));
      CHECK(r.iso_md5_known);
      CHECK_EQ(r.iso_md5, md5_file_hex(zip));
      CHECK(logged("a ZIP of install files, the known copy of " + std::string(find_package(id)->title) + " (by its md5)"));
      CHECK(!logged("checking every file"));
      check_installed(root / L"win", *f, "packages/" + id);
      if (r.status == Status::ok) {
        phosg::JSON s = json_at(r.import_json).at("source");
        CHECK_EQ(s.get_bool("imageMd5Known"), true);
        CHECK_EQ(s.get_string("imageMd5"), md5_file_hex(zip));
        CHECK_EQ(s.get_string("format"), std::string("zip"));
      }
    }
    // I5: the owners' notes and BEAUTYOL.ZIP, locked in the folder sources,
    // are never opened.
    {
      std::vector<HANDLE> held;
      for (const fs::path& p : {src / L"looney" / to_wide(test::kLooneyNote), src / L"screams" / to_wide(test::kScreamsNote),
                                src / L"disney" / to_wide(test::kDisneyNote), src / L"disney" / L"Beautyol.zip",
                                src / L"screams-disks" / L"DISK1" / to_wide(test::kScreamsNote)})
        held.push_back(CreateFileW(p.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr));
      for (HANDLE h : held) CHECK(h != INVALID_HANDLE_VALUE);
      for (const auto& [id, name, f] : std::vector<std::tuple<std::string, std::wstring, const test::PkgFixture*>>{
               {"looney", L"looney", &looney}, {"screams", L"screams", &screams}, {"screams", L"screams-disks", &screams},
               {"disney", L"disney", &disney}}) {
        fs::path root = dir / (L"locked-" + name);
        run((to_utf8(name) + " folder, notes locked").c_str(), folder(src / name), opts_for(root, reg), Status::ok);
        check_installed(root / L"win", *f, "packages/" + id);
      }
      for (HANDLE h : held) CloseHandle(h);
    }
    // The archive never opened: read, BEAUTYOL.ZIP's BEAUTY.AD would be a
    // second file for BEAUTY.ZIP's path; a copy without it is the release
    // all the same.
    {
      test::TestRegistry opened = registry_for({{"disney", &disney}});
      opened.get("disney").never_opened = {};
      ImportResult r = run("disney, BEAUTYOL.ZIP not listed as never opened", folder(src / L"disney"),
                           opts_for(dir / L"disney-opened", opened), Status::source_invalid);
      CHECK_EQ(r.message, std::string("two source files map to packages/disney/DISNEY/BEAUTY.AD"));
      test::PkgFixture without = disney;
      without.source.erase("Beautyol.zip");
      test::write_tree(src / L"disney-no-ol", without.source);
      g_log.clear();
      run("disney without BEAUTYOL.ZIP", folder(src / L"disney-no-ol"), opts_for(dir / L"disney-no-ol", reg), Status::ok);
      check_installed(dir / L"disney-no-ol" / L"win", disney, "packages/disney");
      CHECK(!logged("BEAUTYOL"));
    }
    // Every install disk is needed: ScreamSavers' disk 1 is identified and
    // refused for the modules on the others; the other disks alone are no
    // install (no script, or no MODMISC.ZIP and ENGINE.ZIP); likewise every
    // disk of the Disney Collection and of the Looney Tunes alone.
    {
      ImportResult r = run("screams disk 1 alone (floppy)", image(sc_disks[0]), opts_for(dir / L"sc-d1", reg),
                           Status::source_invalid);
      CHECK_EQ(r.message, std::string("the source is missing AMPHIBO.ZIP, BUGZAP.ZIP, GRISTLE.ZIP, HEADBUTT.ZIP, "
                                      "INFECTO.ZIP, LOCKJAW.ZIP, MALIGNO.ZIP, MELTICOR.ZIP, MOONBITE.ZIP, "
                                      "SNAPPY.ZIP, SPEWER.ZIP, STICKY.ZIP, TWISTER.ZIP; importing ScreamSavers "
                                      "needs every install disk"));
      r = run("screams disk 1 alone (folder)", folder(src / L"screams-disk1"), opts_for(dir / L"sc-d1f", reg),
              Status::source_invalid);
      CHECK(r.message.find("needs every install disk") != std::string::npos);
      r = run("screams disks 1 and 2", image(sc_disks[0], {sc_disks[1]}), opts_for(dir / L"sc-d12", reg),
              Status::source_invalid);
      CHECK(r.message.find("missing AMPHIBO.ZIP, BUGZAP.ZIP, GRISTLE.ZIP, HEADBUTT.ZIP, SNAPPY.ZIP, SPEWER.ZIP, "
                           "STICKY.ZIP, TWISTER.ZIP;") != std::string::npos);
      int n = 0;
      for (const auto& [what, source] : std::vector<std::pair<std::string, Source>>{
               {"screams disk 2 alone", image(sc_disks[1])},
               {"screams disk 3 alone", folder(src / L"screams-disk3")},
               {"disney disk 1 alone", folder(src / L"disney-disk1")},
               {"disney disk 2 alone", folder(src / L"disney-disk2")},
               {"disney disk 3 alone", folder(src / L"disney-disk3")},
               {"looney disk 1 alone", folder(src / L"looney-disk1")},
               {"looney disk 2 alone", folder(src / L"looney-disk2")}}) {
        r = run(what.c_str(), source, opts_for(dir / (L"one-disk-" + std::to_wstring(n++)), reg), Status::source_invalid);
        CHECK(r.message.find("not a known release") != std::string::npos);
      }
      CHECK(!fs::exists(dir / L"sc-d1" / L"win" / L"packages" / L"screams"));
    }
    // Identification (PACKAGES.md §3): the engine library in MODMISC.ZIP, the
    // folder file in AFI.ZIP, and 3.2's marker. ScreamSavers ships 3.2's
    // ADXPL300.DLL and an AD3.AFI, and every AFI.ZIP holds DISNEY.AFI: each
    // release is still exactly one package, and a ScreamSavers source is never
    // After Dark 3.2 (with --no-verify it would have replaced a 3.2 import).
    {
      CHECK(identify_folder(src / L"looney", nullptr, reg.span()) == &reg.get("looney"));
      CHECK(identify_folder(src / L"screams", nullptr, reg.span()) == &reg.get("screams"));
      CHECK(identify_folder(src / L"disney", nullptr, reg.span()) == &reg.get("disney"));
      CHECK(identify_folder(src / L"ad32", nullptr, reg.span()) == &reg.get("ad32"));
      CHECK(identify_folder(src / L"tt", nullptr, reg.span()) == &reg.get("tt"));
      CHECK(identify_folder(src / L"simpsons", nullptr, reg.span()) == &reg.get("simpsons"));
      CHECK(identify_folder(src / L"looney") == find_package("looney"));  // the built-in registry too
      // The windows' check of a folder: a copy of the disks in DISK<n> folders too.
      CHECK(identify_folder(src / L"screams-disks", nullptr, reg.span()) == &reg.get("screams"));
      ImportResult r = run("--package ad32 on ScreamSavers", folder(src / L"screams", "ad32"),
                           opts_for(dir / L"id-sc-ad32", reg), Status::source_invalid);
      CHECK_EQ(r.message, std::string("the source is not After Dark 3.2 (it looks like ScreamSavers)"));
      r = run("--package screams on 3.2", image(src / L"ad32.iso", {}, "screams"), opts_for(dir / L"id-ad32-sc", reg),
              Status::source_invalid);
      CHECK_EQ(r.message, std::string("the source is not ScreamSavers (it looks like After Dark 3.2)"));
      r = run("--no-verify on ScreamSavers", folder(src / L"screams"), opts_for(dir / L"id-sc-nv", reg, false),
              Status::ok);
      CHECK_EQ(r.package_id, std::string("screams"));
      CHECK(!fs::exists(dir / L"id-sc-nv" / L"win" / L"packages" / L"ad32"));
      // A crafted source with 3.2's marker too is both: ambiguous, unless --package says which.
      test::write_tree(src / L"screams-marker", test::screams_fixture(true).source);
      r = run("ScreamSavers with 3.2's marker", folder(src / L"screams-marker"), opts_for(dir / L"id-both-sc", reg),
              Status::source_invalid);
      CHECK_EQ(r.message, std::string("ambiguous source: it looks like After Dark 3.2 and ScreamSavers; choose one "
                                      "with --package"));
      r = run("... + --package screams", folder(src / L"screams-marker", "screams"), opts_for(dir / L"id-both-sc2", reg,
                                                                                              false),
              Status::ok);
      CHECK_EQ(r.package_id, std::string("screams"));
      // Without its marker, without its folder file, or without AFI.ZIP at
      // all, 3.2 is no release; nor is a MODMISC.ZIP with no engine library.
      auto ad32_variant = [&](const wchar_t* name, const std::function<void(test::PkgFixture&)>& change) {
        test::PkgFixture f = ad32;
        change(f);
        fs::path p = src / name;
        test::write_tree(p, f.source);
        return p;
      };
      const auto no_marker = ad32_variant(L"v-ad32-nomarker", [](test::PkgFixture& f) {
        with_zip(f, "INSTALL/MODMISC.ZIP", {{"ADXPL300.DLL", test::blob("x")}, {"ADTOOL.DLL", test::blob("t")}});
      });
      const auto no_folder = ad32_variant(L"v-ad32-noafi3", [](test::PkgFixture& f) {
        with_zip(f, "INSTALL/AFI.ZIP", {{"AD2.AFI", test::blob("a")}, {"DISNEY.AFI", test::blob("d")}});
      });
      const auto no_afi = ad32_variant(L"v-ad32-noafizip", [](test::PkgFixture& f) { f.source.erase("INSTALL/AFI.ZIP"); });
      for (const auto& [what, p] : std::vector<std::pair<std::string, fs::path>>{
               {"3.2 without AD30RSDB.DLL", no_marker}, {"3.2 without AD3.AFI", no_folder}, {"3.2 without AFI.ZIP", no_afi}}) {
        std::string why;
        CHECK(identify_folder(p, &why, reg.span()) == nullptr);
        CHECK(why.find("not a known release") != std::string::npos);
        fprintf(stderr, "[%s] %s\n", what.c_str(), why.c_str());
      }
      // The Disney Collection's DISNEY.AFI is on every disk set above: only
      // ADXPL100.DLL beside it makes a source the Disney Collection.
      for (const char* id : {"ad32", "tt", "simpsons", "looney", "screams"})
        CHECK(identify_folder(src / to_wide(id), nullptr, reg.span()) != &reg.get("disney"));
    }
    // The ad3zip invariants for the new shapes: a *_SOUND.DLL sound database
    // (the Looney Tunes' LT_SOUND.DLL) outside the module folder breaks I4 as
    // a *_SND.DLL does.
    {
      test::PkgFixture f = looney;
      with_zip(f, "MUSIC.ZIP", {{"ACME.MID", test::blob("acme mid")}});
      with_zip(f, "BITMAPS.ZIP", {{"LT_SOUND.DLL", test::blob("LT_SOUND", 3000)}});
      test::write_tree(src / L"v-looney-i4", f.source);
      test::TestRegistry lax;
      lax.get("looney").required = {};
      fs::path root = dir / L"inv-looney-i4";
      ImportResult r = run("I4: LT_SOUND.DLL in a subfolder", folder(src / L"v-looney-i4"), opts_for(root, lax, false),
                           Status::source_invalid);
      CHECK(r.message.find("breaks I4: the sound database LT_SOUND.DLL is not in a module folder") != std::string::npos);
      CHECK(!fs::exists(root / L"win" / L"packages" / L"looney"));
    }
    // The catalog of each: trimmed names (the Looney Tunes' "Michigan J.
    // Frog ", the Disney Collection's two leading spaces), code page 1252
    // decoded (Pepe's D'amour), the Disney Collection's five names spelt out,
    // and a fixed 640x480 screen for ScreamSavers' entries alone.
    {
      fs::path root = dir / L"cat-three";
      for (const auto& [what, source] : std::vector<std::pair<std::string, Source>>{
               {"looney", image(src / L"looney.zip")}, {"screams", image(src / L"screams.zip")},
               {"disney", image(src / L"disney.zip")}})
        run(("three: " + what).c_str(), source, opts_for(root, reg), Status::ok);
      phosg::JSON cat = json_at(root / L"win" / L"catalog-win.json");
      std::map<std::string, std::string> want = {
          {"looney.frog", "Michigan J. Frog"},     {"looney.pepe", "Desquetoppe D\xE2\x80\x99" "amour"},
          {"looney.ltmessgs", "Messages"},         {"looney.wockets", "Wockets' Wed Gware"},
          {"screams.twister", "Spin Out"},         {"screams.lockjaw", "All Tied Up"},
          {"disney.dalm", "101 Dalmatians"},       {"disney.dsclocks", "Disney Clocks"},
          {"disney.falling", "Falling Flower"},    {"disney.firewrk", "Magic Kingdom"},
          {"disney.mermaid", "Little Mermaid"},    {"disney.beauty", "Beauty"},
          {"disney.sorcerer", "The Sorcerer"},     {"disney.hook", "Captain Hook"}};
      for (const auto& [id, name] : want) {
        const phosg::JSON* m = module_by_id(cat, id);
        CHECK(m != nullptr);
        if (m) CHECK_EQ(m->get_string("displayName"), name);
        if (m) CHECK_EQ(m->get_string("moduleName"), name);
      }
      for (auto& m : cat.at("modules").as_list()) {
        const std::string p = m->get_string("package");
        CHECK_EQ(m->contains("screen"), p == "screams");
        if (p == "screams") CHECK_EQ(m->get_string("screen"), std::string("640x480"));
        CHECK(!m->contains("abi") && !m->contains("sameAs"));
        CHECK_EQ(m->get_string("lane"), std::string("ne16"));
      }
      const auto& pk = cat.at("packages").as_list();
      std::vector<std::string> order;
      for (auto& p : pk) order.push_back(p->get_string("id"));
      CHECK((order == std::vector<std::string>{"looney", "screams", "disney"}));  // 1995-04, 1995-04, 1995-09
    }
  }

  // ---- identification ---------------------------------------------------------------------------
  {
    test::write_tree(src / L"unknown", {{"README.TXT", test::vec("hello")}, {"STUFF/X.DLL", test::blob("x")}});
    ImportResult r = run("unknown folder", folder(src / L"unknown"), opts_for(dir / L"id-unknown", reg),
                         Status::source_invalid);
    CHECK(r.message.find("known:") != std::string::npos);
    for (const Package& p : builtin_packages()) CHECK(r.message.find(p.title) != std::string::npos);
    CHECK(!fs::exists(dir / L"id-unknown" / L"win"));

    // Deluxe's FILES and an AD 3.2 INSTALL in one folder: ambiguous, unless --package says which.
    test::write_tree(src / L"both", deluxe.source);
    test::write_tree(src / L"both", ad32.source);
    r = run("ambiguous", folder(src / L"both"), opts_for(dir / L"id-both", reg), Status::source_invalid);
    CHECK(r.message.find("ambiguous") != std::string::npos);
    r = run("ambiguous + --package ad32", folder(src / L"both", "ad32"), opts_for(dir / L"id-both-ad32", reg), Status::ok);
    CHECK_EQ(r.package_id, std::string("ad32"));
    r = run("ambiguous + --package deluxe", folder(src / L"both", "deluxe"), opts_for(dir / L"id-both-deluxe", reg),
            Status::ok);
    CHECK_EQ(r.package_id, std::string("deluxe"));
    // --package that does not fit.
    r = run("--package mismatch", folder(src / L"ad32", "tt"), opts_for(dir / L"id-mismatch", reg), Status::source_invalid);
    CHECK(r.message.find("Totally Twisted") != std::string::npos);
    run("--package unknown id", folder(src / L"ad32", "nosuch"), opts_for(dir / L"id-nosuch", reg), Status::error);

    // The image md5 names the package: verified "image", no manifest needed.
    test::TestRegistry by_md5;
    by_md5.image("ad32", md5_file_hex(src / L"ad32.iso"), fs::file_size(src / L"ad32.iso"));
    r = run("known image md5", image(src / L"ad32.iso"), opts_for(dir / L"id-md5", by_md5), Status::ok);
    CHECK_EQ(r.verified, std::string("image"));
    CHECK(r.iso_md5_known);
    CHECK_EQ(json_at(r.import_json).at("source").get_bool("imageMd5Known"), true);
    run("known md5 + other --package", image(src / L"ad32.iso", {}, "tt"), opts_for(dir / L"id-md5-pkg", by_md5),
        Status::source_invalid);
    // ... and its contents must then be that package's.
    test::TestRegistry wrong_md5;
    wrong_md5.image("tt", md5_file_hex(src / L"ad32.iso"), fs::file_size(src / L"ad32.iso"));
    r = run("md5 of one package, contents of another", image(src / L"ad32.iso"), opts_for(dir / L"id-md5-wrong", wrong_md5),
            Status::source_invalid);
    CHECK(r.message.find("md5 of Totally Twisted") != std::string::npos);
    // The same image given twice (or a copy of it) is one image: still the
    // known image, verified "image", one part.
    fs::copy_file(src / L"ad32.iso", src / L"ad32-copy.iso", fs::copy_options::overwrite_existing);
    for (const fs::path& again : {src / L"ad32.iso", src / L"ad32-copy.iso"}) {
      g_log.clear();
      r = run("known image given twice", image(src / L"ad32.iso", {again}),
              opts_for(dir / (again == src / L"ad32.iso" ? L"id-twice" : L"id-twice-copy"), by_md5), Status::ok);
      CHECK_EQ(r.verified, std::string("image"));
      CHECK(r.iso_md5_known);
      CHECK_EQ(r.parts.size(), size_t(1));
      CHECK(!json_at(r.import_json).at("source").contains("parts") ||
            json_at(r.import_json).at("source").at("parts").as_list().empty());
      CHECK(logged("ignoring"));
    }
    // Known images of two different releases are refused as such, not by
    // the first file the two happen to disagree on.
    test::TestRegistry two;
    two.image("ad32", md5_file_hex(src / L"ad32.iso"), fs::file_size(src / L"ad32.iso"));
    two.image("tt", md5_file_hex(src / L"tt.iso"), fs::file_size(src / L"tt.iso"));
    r = run("two releases as images", image(src / L"tt.iso", {src / L"ad32.iso"}), opts_for(dir / L"id-two", two),
            Status::source_invalid);
    CHECK(r.message.find("two different releases") != std::string::npos);
    CHECK(!fs::exists(dir / L"id-two" / L"win" / L"packages"));
    // Unknown images of different releases: the union says why it refuses.
    r = run("two unknown releases as images", image(src / L"tt.iso", {src / L"ad32.iso"}),
            opts_for(dir / L"id-two-unknown", reg), Status::source_invalid);
    CHECK(r.message.find("not the disks of one release") != std::string::npos);
    // identify_folder (the GUI's check).
    std::string why;
    CHECK(identify_folder(src / L"tt") == find_package("tt"));
    CHECK(identify_folder(src / L"unknown", &why) == nullptr && why.find("known:") != std::string::npos);
    CHECK(identify_folder(src / L"deluxe" / L"ADE" / L"FILES") == find_package("deluxe"));
  }

  // ---- recipes: fix-ups, invariants, required files, the password -----------------------------------
  {
    // A fix-up is made only from a source that matched the release.
    test::TestRegistry partial = registry_for({{"ad10", &ad10}});
    std::vector<KnownFile> m = test::manifest_of(ad10.expect);
    for (KnownFile& k : m)
      if (std::string(k.path) == "packages/ad10/AD10TH/TOASTER1.MID") k.md5 = "00000000000000000000000000000000";
    partial.manifest("ad10", m);
    g_log.clear();
    fs::path root = dir / L"fixups-partial";
    run("ad10, one fix-up source differs (--no-verify)", folder(src / L"ad10"), opts_for(root, partial, false), Status::ok);
    fs::path music = root / L"win" / L"packages" / L"ad10" / L"AD10TH" / L"MUSIC";
    CHECK(!fs::exists(music / L"Toasters2k.mid"));
    CHECK(fs::exists(music / L"Flying Toasters.mid") && fs::exists(music / L"Baby Toasters.mid"));
    CHECK(fs::exists(root / L"win" / L"packages" / L"ad10" / L"AD10TH" / L"TT_SND.DLL"));
    CHECK(logged("Toasters2k.mid skipped"));
    // Verified, the same mismatch fails the import and installs nothing.
    run("ad10, a file differs", folder(src / L"ad10"), opts_for(dir / L"fixups-verify", partial), Status::verify_failed);
    CHECK(!fs::exists(dir / L"fixups-verify" / L"win" / L"packages" / L"ad10"));
  }
  auto variant = [&](const test::PkgFixture& base, const wchar_t* name,
                     const std::function<void(test::PkgFixture&)>& change) {
    test::PkgFixture f = base;
    change(f);
    fs::path p = src / name;
    test::write_tree(p, f.source);
    return p;
  };
  auto invariant = [&](const char* what, const fs::path& source, const char* expect_in_message,
                       const test::TestRegistry& r = test::TestRegistry(), bool check = false) {
    fs::path root = dir / (L"inv-" + source.filename().wstring());
    ImportResult res = run(what, folder(source), opts_for(root, r, check), Status::source_invalid);
    CHECK(res.message.find(expect_in_message) != std::string::npos);
    CHECK(!fs::exists(root / L"win" / L"packages" / L"ad32") && !fs::exists(root / L"win" / L"packages" / L"tt"));
    CHECK(!fs::exists(root / L"win") || no_leftovers(root / L"win"));
  };
  {
    test::TestRegistry none;  // no manifests: nothing to verify against
    auto i1 = variant(ad32, L"v-i1", [](test::PkgFixture& f) {
      with_zip(f, "INSTALL/MODMISC.ZIP", {{"ADXPL300.DLL", test::blob("x")}, {"AD30RSDB.DLL", test::blob("r")},
                                          {"AD_SND.DLL", test::blob("old snd")}});
    });
    invariant("I1: AD_SND beside the modules", i1, "I1", none);
    auto i2 = variant(ad32, L"v-i2", [](test::PkgFixture& f) {
      with_zip(f, "INSTALL/NEEDY.ZIP", {{"NEEDY.AD", test::ne_module("Needy", {"KERNEL", "NOTHERE"})}});
    });
    invariant("I2: a DLL the module needs is missing", i2, "I2", none);
    test::TestRegistry lax;
    lax.get("ad32").required = {};
    auto i3 = variant(ad32, L"v-i3", [](test::PkgFixture& f) {
      with_zip(f, "INSTALL/ENGINE.ZIP", {{"AD_SND.DLL", test::blob("snd")}, {"MULTI.AM3", test::pattern(16, 1)}});
    });
    invariant("I3: no ADTASK or OLDMOD16", i3, "I3", lax);
    invariant("required: no ENGINE\\ADTASK.DLL", i3, "missing required", none);
    auto i4 = variant(tt, L"v-i4", [](test::PkgFixture& f) {
      with_zip(f, "INSTALL/CHAM.ZIP", {{"CHAM.AD", test::ne_module("Chameleon", {"ADXPL40"})}, {"EXTRA.MID", test::blob("m")}});
    });
    invariant("I4: MIDI outside MUSIC", i4, "I4", none);
    auto arch = variant(simpsons, L"v-archives", [](test::PkgFixture& f) { f.source.erase("BURNS.ZIP"); });
    invariant("a Simpsons module archive is missing", arch, "BURNS.ZIP", none);
    auto nopw = variant(ad32, L"v-nopw", [](test::PkgFixture& f) { f.source["INSTALL/INSTALL.INS"] = test::install_ins(""); });
    invariant("no password in INSTALL.INS", nopw, "password", none);
    auto corrupt = variant(ad32, L"v-corrupt", [](test::PkgFixture& f) {
      auto& z = f.source["INSTALL/GUTS.ZIP"];
      z[30 + 7 + 12 + 5] ^= 0xFF;  // inside GUTS.AD's compressed data
    });
    invariant("a damaged archive member", corrupt, "GUTS", none);
    auto notzip = variant(ad32, L"v-notzip", [](test::PkgFixture& f) { f.source["INSTALL/HELP.ZIP"] = test::blob("junk"); });
    invariant("a .ZIP that is not one", notzip, "HELP.ZIP", none);
    // Two archives' members whose names differ only in the case of a letter
    // outside ASCII (u-umlaut and U-umlaut) are one file to Windows: the
    // source is refused while it is planned, never when the second file
    // cannot be created.
    auto umlaut = variant(ad32, L"v-umlaut", [](test::PkgFixture& f) {
      with_zip(f, "INSTALL/UML1.ZIP", {{"M\xC3\xBCSIK.AD", test::ne_module("Musik", {"KERNEL"})}});
      with_zip(f, "INSTALL/UML2.ZIP", {{"M\xC3\x9CSIK.AD", test::ne_module("Musik", {"KERNEL"})}});
    });
    invariant("two archives' members, one name to Windows", umlaut, "two source files map to packages/ad32/", none);
  }

  // ---- per-package atomicity ------------------------------------------------------------------------
  fs::path aroot = dir / L"atomic";
  fs::path awin = aroot / L"win";
  {
    run("deluxe", folder(src / L"deluxe"), opts_for(aroot, reg), Status::ok);
    test::Tree files0 = snapshot(awin / L"FILES");
    std::string json0 = test::read_text(awin / L"import.json");
    ImportResult r = run("ad32 beside deluxe", folder(src / L"ad32"), opts_for(aroot, reg), Status::ok);
    CHECK(snapshot(awin / L"FILES") == files0);
    CHECK(test::read_text(awin / L"import.json") == json0);
    CHECK(catalog_ids(awin) == concat({deluxe.ids, ad32.ids}));
    CHECK_EQ(r.catalog_modules, deluxe.ids.size() + ad32.ids.size());
    CHECK_EQ(r.package_modules, ad32.ids.size());
    CHECK((r.installed == std::vector<std::string>{"After Dark 4.0 Deluxe", "After Dark 3.2"}));
    run("tt beside them", image(src / L"tt.iso"), opts_for(aroot, reg), Status::ok);
    test::Tree ad32_0 = snapshot(awin / L"packages" / L"ad32");
    test::Tree tt_0 = snapshot_files(awin / L"packages" / L"tt");
    test::Tree tt_full = snapshot(awin / L"packages" / L"tt");
    CHECK(snapshot(awin / L"FILES") == files0);
    CHECK(catalog_ids(awin) == concat({deluxe.ids, ad32.ids, tt.ids}));

    // A re-import replaces only its own package (a stray file disappears).
    test::write_bytes(awin / L"packages" / L"ad32" / L"AD32" / L"STRAY.AD", {1, 2, 3});
    run("ad32 again", image(src / L"ad32.iso"), opts_for(aroot, reg), Status::ok);
    CHECK(!fs::exists(awin / L"packages" / L"ad32" / L"AD32" / L"STRAY.AD"));
    check_installed(awin, ad32, "packages/ad32");
    CHECK(snapshot(awin / L"packages" / L"tt") == tt_full);
    CHECK(snapshot(awin / L"FILES") == files0);
    CHECK(test::read_text(awin / L"import.json") == json0);
    CHECK(no_leftovers(awin));

    // Failures and cancels change nothing, not even the catalog.
    auto everything = [&] { return snapshot(awin); };
    test::Tree before = everything();
    auto bad = variant(ad32, L"v-corrupt2", [](test::PkgFixture& f) { f.source["INSTALL/TOILET.ZIP"].resize(40); });
    run("corrupt re-import", folder(bad), opts_for(aroot, reg), Status::source_invalid);
    CHECK(everything() == before);
    ImportOptions o = opts_for(aroot, reg);
    o.progress = [](const Progress& p) { return !(p.phase == Progress::Phase::copy && p.done > 1000); };
    run("cancelled re-import", folder(src / L"ad32"), o, Status::cancelled);
    CHECK(everything() == before);
    o.progress = [](const Progress& p) { return !(p.phase == Progress::Phase::finalize && p.done == 0); };
    run("cancelled at the last moment", folder(src / L"tt"), o, Status::cancelled);
    CHECK(everything() == before);
    // The progress reports name the package once it is identified.
    std::set<std::string> named;
    o.progress = [&](const Progress& p) {
      named.insert(p.package);
      return !(p.phase == Progress::Phase::finalize && p.done == p.total);  // a late cancel is ignored
    };
    run("late cancel", folder(src / L"tt"), o, Status::ok);
    CHECK(named.count("Totally Twisted After Dark"));
    CHECK(snapshot_files(awin / L"packages" / L"tt") == tt_0);
    // A second operation while one holds the lock.
    {
      HANDLE held = CreateFileW((awin / L"import.lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                                FILE_FLAG_DELETE_ON_CLOSE, nullptr);
      run("concurrent import", folder(src / L"tt"), opts_for(aroot, reg), Status::error);
      CHECK_EQ(regenerate_catalog(aroot, {}, reg.span()).status, Status::error);
      CHECK_EQ(remove_package("tt", aroot, {}, reg.span()).status, Status::error);
      CloseHandle(held);
    }
    // Deluxe's own re-import leaves the packages alone.
    tt_full = snapshot(awin / L"packages" / L"tt");
    run("deluxe again", image(src / L"deluxe.iso"), opts_for(aroot, reg), Status::ok);
    CHECK(snapshot(awin / L"packages" / L"tt") == tt_full);
    check_installed(awin, ad32, "packages/ad32");
    CHECK(catalog_ids(awin) == concat({deluxe.ids, ad32.ids, tt.ids}));
    CHECK(no_leftovers(awin));
    (void)ad32_0;
  }

  // ---- recovery ----------------------------------------------------------------------------------------
  {
    auto expected_catalog = [&](const fs::path& root) {
      std::vector<CatalogTree> trees;
      fs::path w = root / L"win";
      for (const Package& p : reg.span()) {
        fs::path pr = p.is_deluxe() ? w / L"FILES" : w / L"packages" / to_wide(p.id);
        if (p.is_deluxe() ? fs::is_directory(pr) : fs::exists(pr / L"import.json")) {
          CatalogTree t;
          t.package = &p;
          t.dir = pr;
          phosg::JSON j = json_at(p.is_deluxe() ? w / L"import.json" : pr / L"import.json");
          t.verified = j.get_string("verified");
          t.imported_utc = j.get_string("importedUtc");
          trees.push_back(t);
        }
      }
      return render_catalog(build_catalog(trees));
    };
    auto move = [](const fs::path& a, const fs::path& b) {
      bool ok = false;
      for (int i = 0; i < 50 && !ok; i++)
        if (!(ok = MoveFileExW(a.c_str(), b.c_str(), 0))) Sleep(100);
      CHECK(ok);
    };
    fs::path root = dir / L"recover";
    fs::path w = root / L"win", pk = w / L"packages";
    run("setup deluxe", folder(src / L"deluxe"), opts_for(root, reg), Status::ok);
    run("setup ad32", folder(src / L"ad32"), opts_for(root, reg), Status::ok);
    run("setup tt", folder(src / L"tt"), opts_for(root, reg), Status::ok);
    test::Tree ad32_0 = snapshot(pk / L"ad32");
    const std::string full = test::read_text(w / L"catalog-win.json");

    // Died between the two renames: packages\ad32 parked, a stage and a catalog tmp left.
    move(pk / L"ad32", pk / L"ad32.old-777");
    test::write_bytes(pk / L"ad32.importing-777" / L"AD32" / L"PARTIAL.AD", {1});
    test::write_bytes(w / L"catalog-win.json.tmp-777", test::vec("{\"pending\": 1}"));
    test::write_bytes(w / L"catalog-win.json", test::vec("stale"));
    g_log.clear();
    CatalogResult cr = regenerate_catalog(root, [](const std::string& s) { g_log.push_back(s); }, reg.span());
    CHECK_EQ(cr.status, Status::ok);
    CHECK(snapshot(pk / L"ad32") == ad32_0);
    CHECK(logged("restored ad32"));
    CHECK(no_leftovers(w));
    CHECK_EQ(test::read_text(w / L"catalog-win.json"), expected_catalog(root));

    // Died after the swap, before the catalog rename: the old tree is deleted
    // and the catalog rebuilt — by an import that is then cancelled.
    test::write_bytes(pk / L"tt.old-778" / L"TWISTED" / L"OLD.AD", {2});
    test::write_bytes(w / L"catalog-win.json.tmp-778", test::vec("{}"));
    test::write_bytes(w / L"catalog-win.json", test::vec("stale"));
    ImportOptions o = opts_for(root, reg);
    o.progress = [](const Progress& p) { return p.phase != Progress::Phase::copy; };
    run("cancelled import after a crash", folder(src / L"ad32"), o, Status::cancelled);
    CHECK(!fs::exists(pk / L"tt.old-778"));
    CHECK(fs::exists(pk / L"tt" / L"import.json"));
    CHECK_EQ(test::read_text(w / L"catalog-win.json"), expected_catalog(root));
    CHECK(no_leftovers(w));

    // An interrupted --remove of a package, and of Deluxe.
    move(pk / L"tt", pk / L"tt.removing-779");
    CHECK_EQ(regenerate_catalog(root, {}, reg.span()).status, Status::ok);
    CHECK(!fs::exists(pk / L"tt.removing-779") && !fs::exists(pk / L"tt"));
    CHECK(catalog_ids(w) == concat({deluxe.ids, ad32.ids}));
    move(w / L"FILES", w / L"FILES.removing-780");
    CHECK_EQ(regenerate_catalog(root, {}, reg.span()).status, Status::ok);
    CHECK(!fs::exists(w / L"FILES.removing-780") && !fs::exists(w / L"FILES") && !fs::exists(w / L"import.json"));
    CHECK(catalog_ids(w) == ad32.ids);
    // A folder in packages\ that is no package is left alone (and logged).
    fs::create_directories(pk / L"zzz");
    g_log.clear();
    CHECK_EQ(regenerate_catalog(root, [](const std::string& s) { g_log.push_back(s); }, reg.span()).status, Status::ok);
    CHECK(fs::exists(pk / L"zzz") && logged("ignoring zzz"));
    CHECK(catalog_ids(w) == ad32.ids);
    (void)full;
  }

  // ---- the Delrina Intermission releases: The Far Side, Dilbert ---------------------------------------
  {
    // The registry: Delrina's installer (no INSTALL.DAT shortname), every
    // disk's tag file, the fingerprint's file on disk 1 among the table's;
    // every installed file a loose file under its own name, in SAVER or
    // ENGINE, once, and exactly the manifest's; the readers in ENGINE, the
    // ASA reader required, never AD_SND; a name for every module; the copies
    // online and the known images they are.
    struct Want {
      const char* id;
      size_t disks, modules, copies;
      const char* released;
    };
    for (const Want& w : {Want{"farside", 5, 14, 1, "1994-06"}, Want{"dilbert", 4, 16, 2, "1994-10"}}) {
      const Package* p = find_package(w.id);
      CHECK(p != nullptr);
      if (!p) continue;
      CHECK(p->recipe == Recipe::intermission && p->delrina_installer() && !p->install_name);
      CHECK(!find_package("swse")->delrina_installer());
      CHECK(p->marker && p->module_dir && std::string(p->module_dir) == "SAVER");
      CHECK(p->module_dirs.size() == 1 && std::string(p->module_dirs[0]) == "SAVER");
      CHECK_EQ(std::string(p->released), std::string(w.released));
      CHECK(!p->screen && p->fixups.empty() && p->never_opened.empty() && p->copy_dirs.empty() && !p->setup_title);
      CHECK_EQ(p->required_archives.size(), w.disks);
      for (size_t k = 0; k < p->required_archives.size(); k++)
        CHECK_EQ(std::string(p->required_archives[k]), "DISK" + std::to_string(k + 1));
      std::set<std::string> tos, manifest;
      bool marker_listed = false;
      size_t modules = 0;
      for (const LooseFile& lf : p->loose_files) {
        const std::string to = lf.to;
        CHECK(to.rfind("SAVER/", 0) == 0 || to.rfind("ENGINE/", 0) == 0);
        CHECK(to == ascii_upper(to) && to.substr(to.find('/') + 1) == lf.from);
        CHECK(tos.insert(std::string(p->root) + "/" + to).second);
        CHECK(!is_intermission_reader(lf.from) || to.rfind("ENGINE/", 0) == 0);
        CHECK(!iequals(lf.from, "AD_SND.DLL") && !iequals(lf.from, "IMIMXPLY.IMQ"));
        marker_listed = marker_listed || iequals(lf.from, p->marker);
        if (to.rfind("SAVER/", 0) != 0 || !(ends_with_i(to, ".ASA") || ends_with_i(to, ".IMQ"))) continue;
        modules++;
        bool named = false;
        for (const NameOverride& o : p->name_overrides) named = named || to == o.module;
        CHECK(named);
      }
      CHECK(marker_listed && iequals(p->marker, std::string(w.id) == "farside" ? "PTERY.IMQ" : "DB-CLOCK.IMQ"));
      CHECK_EQ(modules, w.modules);
      CHECK_EQ(p->name_overrides.size(), w.modules);
      for (const KnownFile& k : p->manifest) manifest.insert(k.path);
      CHECK(tos == manifest);
      for (const char* r : p->required) CHECK(tos.count(std::string(p->root) + "/" + r) == 1);
      CHECK(tos.count(std::string(p->root) + "/ENGINE/IMASAPLY.IMQ") == 1);
      // Every copy's file (and every part's) a known image: a ZIP of the
      // install files (each disk's, the parts in disk order); "zip" copies.
      CHECK_EQ(p->downloads.size(), w.copies);
      std::set<std::wstring> names;
      for (const Download& d : p->downloads) {
        CHECK_EQ(std::string(d.kind), std::string("zip"));
        std::vector<std::pair<std::string, uint64_t>> files = {{d.md5, d.size}};
        for (const DownloadPart& q : d.more_images) files.push_back({q.md5, q.size});
        CHECK(names.insert(d.file_name).second);
        for (const DownloadPart& q : d.more_images) CHECK(names.insert(q.file_name).second);
        CHECK(std::string_view(d.url).rfind("https://archive.org/download/", 0) == 0);
        for (size_t i = 0; i < files.size(); i++) {
          const KnownImage* k = nullptr;
          for (const KnownImage& x : p->images)
            if (files[i].first == x.md5 && files[i].second == x.size) k = &x;
          CHECK(k && std::string_view(k->medium).rfind("ZIP", 0) == 0);
          if (k) CHECK_EQ(k->disk, files.size() == 1 ? 0 : int(i + 1));
        }
      }
      CHECK(!p->covers.empty() && p->covers[0].kind == CoverSource::Kind::download &&
            std::string(p->covers[0].art) == "box" && p->covers[0].crop.w > 0);
    }
    CHECK_EQ(find_package("farside")->downloads[0].more_images.size(), size_t(4));
    CHECK(find_package("dilbert")->downloads[0].more_images.empty() &&
          find_package("dilbert")->downloads[1].more_images.size() == 3);
    CHECK(find_package("dilbert")->covers.size() == 2 &&
          find_package("dilbert")->covers[1].kind == CoverSource::Kind::disc &&
          std::string(find_package("dilbert")->covers[1].path) == "INSTALL.BMP");
    CHECK(is_intermission_reader("IMASAPLY.IMQ") && is_intermission_reader("imimxply.imq") &&
          is_intermission_reader("IMAD_PLY.IMQ") && !is_intermission_reader("PTERY.IMQ") &&
          !is_intermission_reader("DB-CLOCK.IMQ") && !is_intermission_reader("IMASAPLY.DLL") &&
          !is_intermission_reader("IMASAPLYX.IMQ"));

    // Every source form: a folder (the disks' files together, decoys among
    // them), a flat ZIP, DISK1..DISKn folders loose and zipped, the 1.44 MB
    // floppies in any order, and a ZIP of each disk's files (a BBS's copy:
    // the same note in each) — those known by their md5s, verified "image".
    struct Rel {
      const char* id;
      const test::PkgFixture* f;
      int (*disk_of)(const std::string&);
      int disks;
      const std::vector<std::string>* decoys;
    };
    for (const Rel& rel : {Rel{"farside", &farside, test::farside_disk, 5, &test::farside_decoys()},
                           Rel{"dilbert", &dilbert, test::dilbert_disk, 4, &test::dilbert_decoys()}}) {
      const std::wstring id = to_wide(rel.id);
      const Package& pkg = *find_package(rel.id);
      const std::string title = pkg.title, pkg_root = std::string("packages/") + rel.id;
      test::write_tree(src / id, rel.f->source);
      test::write_bytes(src / (id + L".zip"), test::zip_folder(rel.f->source));
      test::write_disk_folders(src / (id + L"-disks"), rel.f->source, rel.disk_of, rel.disks);
      test::write_bytes(src / (id + L"-disks.zip"), test::zip_disk_folders(rel.f->source, rel.disk_of, rel.disks));
      std::vector<fs::path> floppies, zips;
      std::vector<test::TestRegistry::Disk> known;
      for (int k = 1; k <= rel.disks; k++) {
        test::Tree t = test::disk_files(rel.f->source, k, rel.disk_of);
        test::write_tree(src / (id + L"-disk" + std::to_wstring(k)), t);
        floppies.push_back(src / (id + L"-disk" + std::to_wstring(k) + L".img"));
        test::write_bytes(floppies.back(), test::floppy_of(t));
        t["FILE_ID.DIZ"] = test::vec("A made-up BBS's note, the same on every disk.\r\n");
        const auto z = test::zip_folder(t);
        zips.push_back(src / (id + L"-disk" + std::to_wstring(k) + L".zip"));
        test::write_bytes(zips.back(), z);
        known.push_back({md5_hex(z.data(), z.size()), z.size(), k});
      }
      test::TestRegistry by_md5 = registry_for({{rel.id, rel.f}});
      by_md5.disk_images(rel.id, known);
      auto rest = [](const std::vector<fs::path>& v, size_t first) { return std::vector<fs::path>(v.begin() + first, v.end()); };
      std::vector<fs::path> floppies_rev(floppies.rbegin(), floppies.rend());
      struct Form {
        std::string name;
        Source source;
        const test::TestRegistry* r;
        const char* format;
        const char* verified;
      };
      const std::vector<Form> forms = {
          {"folder", folder(src / id), &reg, "folder", "files"},
          {"flat zip", image(src / (id + L".zip")), &reg, "zip", "files"},
          {"disk folders", folder(src / (id + L"-disks")), &reg, "folder", "files"},
          {"disk folders zipped", image(src / (id + L"-disks.zip")), &reg, "zip", "files"},
          {"floppies", image(floppies[0], rest(floppies, 1)), &reg, "fat12", "files"},
          {"floppies, last first", image(floppies_rev[0], rest(floppies_rev, 1)), &reg, "fat12", "files"},
          {"a zip per disk", image(zips[0], rest(zips, 1)), &by_md5, "zip", "image"},
      };
      for (const Form& form : forms) {
        const std::string what = std::string(rel.id) + " " + form.name;
        fs::path root = dir / (L"delrina-" + id + L"-" + to_wide(std::to_string(&form - forms.data())));
        g_log.clear();
        ImportResult r = run(what.c_str(), form.source, opts_for(root, *form.r), Status::ok);
        CHECK_EQ(r.package_id, std::string(rel.id));
        CHECK_EQ(r.format, std::string(form.format));
        CHECK_EQ(r.verified, std::string(form.verified));
        CHECK_EQ(r.package_modules, rel.f->ids.size());
        check_installed(root / L"win", *rel.f, pkg_root);
        CHECK(catalog_ids(root / L"win") == rel.f->ids);
        CHECK(no_leftovers(root / L"win"));
        if (r.status != Status::ok) continue;
        CHECK_EQ(json_at(r.import_json).at("package").get_string("recipe"), std::string("intermission"));
        if (std::string(form.verified) == "image")
          CHECK(logged("ZIPs of the install disks' files, the known copies of " + title + " (by their md5s)"));
      }

      // The catalog: Intermission entries in the ne16 lane, named by the
      // registry; an ASA's and an IMQ module's entry is SAVERMAIN, with the
      // Configure... button behind its reader's dialog (none for an IMQ module
      // without one: the fixture's DIL-WHAK).
      {
        phosg::JSON cat = json_at(dir / (L"delrina-" + id + L"-0") / L"win" / L"catalog-win.json");
        for (const std::string& mid : rel.f->ids) {
          const phosg::JSON* m = module_by_id(cat, mid);
          CHECK(m != nullptr);
          if (!m) continue;
          CHECK_EQ(m->get_string("abi"), std::string("intermission"));
          CHECK_EQ(m->get_string("lane"), std::string("ne16"));
          CHECK_EQ(m->get_string("entry"), std::string("SAVERMAIN"));
          CHECK(!m->contains("screen"));
          const std::string path = m->get_string("path");
          const std::string in_pkg = path.substr(pkg_root.size() + 1);
          std::string name;
          for (const NameOverride& o : pkg.name_overrides)
            if (in_pkg == o.module) name = o.name;
          CHECK(!name.empty() && m->get_string("moduleName") == name && m->get_string("displayName") == name);
          const bool dialog = mid != "dilbert.dil-whak";
          CHECK_EQ(m->at("controls").as_list().size(), size_t(dialog ? 1 : 0));
          if (dialog) CHECK_EQ(m->at("controls").as_list().at(0)->get_string("name"), std::string("Configure..."));
          if (ends_with_i(path, ".ASA")) CHECK(m->at("needs").as_list().empty() && m->at("system").as_list().empty());
          else CHECK(!m->at("needs").as_list().empty());
        }
        CHECK(!module_by_id(cat, std::string(rel.id) + ".imasaply") && !module_by_id(cat, std::string(rel.id) + ".lastdisk"));
      }

      // I5: the decoys, locked in a folder source, are never opened.
      {
        std::vector<HANDLE> held;
        for (const std::string& decoy : *rel.decoys)
          held.push_back(CreateFileW((src / id / to_wide(decoy)).c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr));
        for (HANDLE h : held) CHECK(h != INVALID_HANDLE_VALUE);
        run((std::string(rel.id) + " folder, decoys locked").c_str(), folder(src / id),
            opts_for(dir / (L"delrina-" + id + L"-locked"), reg), Status::ok);
        check_installed(dir / (L"delrina-" + id + L"-locked") / L"win", *rel.f, pkg_root);
        for (HANDLE h : held) CloseHandle(h);
      }

      // Disk 1 alone is the release without its other disks; any other disk
      // alone, or disk 1 without the installer or the release's own file
      // beside its tag, is no known release; Star Wars Screen Entertainment
      // is never taken for it, nor it for Star Wars.
      {
        std::string missing;
        for (int k = 2; k <= rel.disks; k++) missing += (k == 2 ? "DISK" : ", DISK") + std::to_string(k);
        ImportResult r = run("disk 1 alone", folder(src / (id + L"-disk1")), opts_for(dir / (L"delrina-" + id + L"-d1"), reg),
                             Status::source_invalid);
        CHECK(r.message.find("the source is missing " + missing + "; importing " + title + " needs every install disk") !=
              std::string::npos);
        r = run("disk 2 alone", folder(src / (id + L"-disk2")), opts_for(dir / (L"delrina-" + id + L"-d2"), reg),
                Status::source_invalid);
        CHECK(r.message.find("not a known release") != std::string::npos);
        for (const char* gone : {"IMINST2.EXE", pkg.marker, "DISK1"}) {
          test::Tree t = rel.f->source;
          t.erase(gone);
          const fs::path p = src / (id + L"-without-" + to_wide(gone));
          test::write_tree(p, t);
          r = run((std::string("without ") + gone).c_str(), folder(p), opts_for(dir / (L"delrina-" + id + L"-wo"), reg),
                  Status::source_invalid);
          CHECK(r.message.find("not a known release") != std::string::npos);
        }
        r = run("--package swse", folder(src / id, "swse"), opts_for(dir / (L"delrina-" + id + L"-swse"), reg),
                Status::source_invalid);
        CHECK(r.message.find("the source is not Star Wars Screen Entertainment (it looks like " + title + ")") !=
              std::string::npos);
        r = run("swse as it", folder(src / L"swse", rel.id), opts_for(dir / (L"delrina-" + id + L"-sw"), reg),
                Status::source_invalid);
        CHECK(r.message.find("the source is not " + title + " (it looks like Star Wars Screen Entertainment)") !=
              std::string::npos);
        CHECK(!fs::exists(dir / (L"delrina-" + id + L"-d1") / L"win" / L"packages" / id));
      }

      // A version stamp that is no stamp is data left over: a damaged file.
      {
        test::Tree t = rel.f->source;
        auto& lib = t["INTRMLIB.DLL"];
        lib[lib.size() - 2] = 'X';
        const fs::path p = src / (id + L"-badstamp");
        test::write_tree(p, t);
        ImportResult r = run("a damaged stamp", folder(p), opts_for(dir / (L"delrina-" + id + L"-stamp"), reg),
                             Status::source_invalid);
        CHECK(r.message.find("INTRMLIB.DLL: 8 byte(s) left after the compressed data") != std::string::npos);
      }

      // The intermission invariants for a Delrina release (registries
      // without the required files, so the layout check is what refuses):
      // its readers stay in ENGINE (I1), and ASA modules need the ASA reader
      // there (I3).
      {
        std::vector<LooseFile> moved, dropped;
        for (const LooseFile& lf : pkg.loose_files) {
          if (std::string(lf.from) == "IMASAPLY.IMQ") {
            moved.push_back({lf.from, "SAVER/IMASAPLY.IMQ", lf.codec});
            continue;
          }
          moved.push_back(lf);
          dropped.push_back(lf);
        }
        test::TestRegistry i1, i3;
        i1.get(rel.id).required = {};
        i1.get(rel.id).loose_files = moved;
        i3.get(rel.id).required = {};
        i3.get(rel.id).loose_files = dropped;
        ImportResult r = run("I1: the ASA reader beside the modules", folder(src / id),
                             opts_for(dir / (L"delrina-" + id + L"-i1"), i1, false), Status::source_invalid);
        CHECK(r.message.find("breaks I1: SAVER\\IMASAPLY.IMQ belongs in ENGINE") != std::string::npos);
        r = run("I3: no ASA reader", folder(src / id), opts_for(dir / (L"delrina-" + id + L"-i3"), i3, false),
                Status::source_invalid);
        CHECK(r.message.find("breaks I3: no ENGINE\\IMASAPLY.IMQ") != std::string::npos);
        CHECK(!fs::exists(dir / (L"delrina-" + id + L"-i1") / L"win" / L"packages" / id));
      }
    }

    // Both releases' files in one folder: both fingerprints, an ambiguous
    // source (--package chooses).
    {
      test::write_tree(src / L"farside-dilbert", farside.source);
      test::write_tree(src / L"farside-dilbert", dilbert.source);
      ImportResult r = run("farside + dilbert", folder(src / L"farside-dilbert"), opts_for(dir / L"delrina-both", reg),
                           Status::source_invalid);
      CHECK(r.message.find("ambiguous source: it looks like The Far Side Screen Saver Collection and Scott Adams' "
                           "Dilbert Screen Saver Collection") != std::string::npos);
    }
  }

  // ---- --catalog-only without FILES, win_assets_dir, --remove, list --------------------------------------
  {
    fs::path root = dir / L"nofiles";
    run("ad32 alone", folder(src / L"ad32"), opts_for(root, reg), Status::ok);
    run("simpsons alone", image(src / L"simpsons.img"), opts_for(root, reg), Status::ok);
    CHECK(!fs::exists(root / L"win" / L"FILES"));
    CHECK_EQ(win_assets_dir(root), root / L"win");
    CHECK_EQ(win_assets_dir(root / L"win"), root / L"win");  // a win dir holding only packages
    test::write_bytes(root / L"win" / L"catalog-win.json", test::vec("stale"));
    CatalogResult cr = regenerate_catalog(root, {}, reg.span());
    CHECK_EQ(cr.status, Status::ok);
    CHECK_EQ(cr.modules, ad32.ids.size() + simpsons.ids.size());
    CHECK(catalog_ids(root / L"win") == concat({ad32.ids, simpsons.ids}));
    // A root that is itself a win dir holding only packages\ still resolves.
    fs::create_directories(dir / L"bare" / L"packages");
    CHECK_EQ(win_assets_dir(dir / L"bare"), dir / L"bare");
    CHECK_EQ(win_assets_dir(dir / L"fresh"), dir / L"fresh" / L"win");

    auto states = list_packages(root, reg.span());
    CHECK_EQ(states.size(), size_t(14));
    for (auto& s : states) {
      bool want = std::string(s.package->id) == "ad32" || std::string(s.package->id) == "simpsons";
      CHECK_EQ(s.installed, want);
      if (want) CHECK(s.verified == "files" && !s.imported_utc.empty() && s.file_count > 0);
    }

    RemoveResult rr = remove_package("ad32", root, {}, reg.span());
    CHECK_EQ(rr.status, Status::ok);
    CHECK(!fs::exists(root / L"win" / L"packages" / L"ad32"));
    CHECK(catalog_ids(root / L"win") == simpsons.ids);
    CHECK_EQ(rr.catalog_modules, simpsons.ids.size());
    CHECK_EQ(remove_package("ad32", root, {}, reg.span()).status, Status::error);    // not installed
    CHECK_EQ(remove_package("nosuch", root, {}, reg.span()).status, Status::error);  // unknown
    CHECK(no_leftovers(root / L"win"));
    // Removing Deluxe takes FILES and its import.json.
    fs::path droot = dir / L"remove-deluxe";
    run("deluxe", folder(src / L"deluxe"), opts_for(droot, reg), Status::ok);
    run("tt", folder(src / L"tt"), opts_for(droot, reg), Status::ok);
    CHECK_EQ(remove_package("deluxe", droot, {}, reg.span()).status, Status::ok);
    CHECK(!fs::exists(droot / L"win" / L"FILES") && !fs::exists(droot / L"win" / L"import.json"));
    CHECK(catalog_ids(droot / L"win") == tt.ids);
    // Nothing installed at all: --catalog-only has nothing to do.
    CHECK_EQ(remove_package("tt", droot, {}, reg.span()).status, Status::ok);
    CHECK_EQ(regenerate_catalog(droot, {}, reg.span()).status, Status::source_invalid);
  }

  // ---- the merged catalog -------------------------------------------------------------------------------------
  {
    fs::path d = dir / L"cat-deluxe", all = dir / L"cat-all";
    run("deluxe only", folder(src / L"deluxe"), opts_for(d, reg), Status::ok);
    run("deluxe", image(src / L"deluxe.iso"), opts_for(all, reg), Status::ok);
    // Out of registry order on purpose: the catalog is in registry order anyway.
    run("simpsons", image(src / L"disk1.img", {src / L"disk2.img"}), opts_for(all, reg), Status::ok);
    run("tt", image(src / L"tt.iso"), opts_for(all, reg), Status::ok);
    run("swse", image(swse_disks[2], {swse_disks[0], swse_disks[4], swse_disks[1], swse_disks[3]}), opts_for(all, reg),
        Status::ok);
    run("startrek", image(src / L"startrek-images.zip"), opts_for(all, reg), Status::ok);
    run("disney", image(src / L"disney.zip"), opts_for(all, reg), Status::ok);
    run("ad32", image(src / L"ad32.iso"), opts_for(all, reg), Status::ok);
    run("screams", image(sc_disks[1], {sc_disks[2], sc_disks[0]}), opts_for(all, reg), Status::ok);
    run("snoopy", image(src / L"snoopy-disks.zip"), opts_for(all, reg), Status::ok);
    run("looney", folder(src / L"looney"), opts_for(all, reg), Status::ok);
    run("marvel", image(src / L"marvel-disk2.img", {src / L"marvel-disk1.img"}), opts_for(all, reg), Status::ok);
    run("dilbert", image(src / L"dilbert.zip"), opts_for(all, reg), Status::ok);
    run("ad10", image(src / L"ad10.iso"), opts_for(all, reg), Status::ok);
    run("farside", folder(src / L"farside-disks"), opts_for(all, reg), Status::ok);
    phosg::JSON cat = json_at(all / L"win" / L"catalog-win.json");
    CHECK_EQ(cat.get_string("generator"), std::string(kCatalogGenerator));
    std::vector<std::string> ids;
    for (auto& m : cat.at("modules").as_list()) ids.push_back(m->get_string("id"));
    CHECK(ids == concat({deluxe.ids, ad10.ids, ad32.ids, tt.ids, simpsons.ids, swse.ids, startrek.ids, marvel.ids,
                         snoopy.ids, looney.ids, screams.ids, disney.ids, farside.ids, dilbert.ids}));
    // The top-level packages list: oldest release first (the cover strip's and the list
    // groups' order), while modules above stay in registry order. Star Trek:
    // The Screen Saver (1992-11) comes first, Marvel Comics Screen Posters
    // (1993-12) next, then The Far Side (1994-06); Star Wars Screen
    // Entertainment ties with the Simpsons (1994-08) and follows it, as in
    // the registry, and Snoopy's Screen Savers and Dilbert (1994-10) follow
    // them, in registry order too; ScreamSavers ties with the Looney Tunes
    // (1995-04) the same way, and the Disney Collection (1995-09) comes
    // between Totally Twisted and Deluxe.
    const auto& pk = cat.at("packages").as_list();
    CHECK_EQ(pk.size(), size_t(14));
    std::vector<std::pair<std::string, size_t>> want_pk = {
        {"startrek", startrek.ids.size()}, {"marvel", marvel.ids.size()},   {"farside", farside.ids.size()},
        {"simpsons", simpsons.ids.size()}, {"swse", swse.ids.size()},       {"snoopy", snoopy.ids.size()},
        {"dilbert", dilbert.ids.size()},   {"looney", looney.ids.size()},   {"screams", screams.ids.size()},
        {"ad32", ad32.ids.size()},         {"tt", tt.ids.size()},           {"disney", disney.ids.size()},
        {"deluxe", deluxe.ids.size()},     {"ad10", ad10.ids.size()}};
    for (size_t i = 0; i < pk.size(); i++) {
      const Package* p = find_package(pk[i]->get_string("id"));
      CHECK(p && pk[i]->get_string("released") == std::string(p->released));
    }
    for (size_t i = 0; i < pk.size() && i < want_pk.size(); i++) {
      CHECK_EQ(pk[i]->get_string("id"), want_pk[i].first);
      CHECK_EQ(size_t(pk[i]->get_int("modules")), want_pk[i].second);
      const Package* p = find_package(want_pk[i].first);
      CHECK_EQ(pk[i]->get_string("title"), std::string(p->title));
      CHECK_EQ(pk[i]->get_string("shortTitle"), std::string(p->short_title));
      CHECK_EQ(pk[i]->get_string("root"), std::string(p->root));
      CHECK(!pk[i]->get_string("verified").empty() && !pk[i]->get_string("importedUtc").empty());
    }
    // Display names: unique per lane, case-insensitively.
    std::map<std::string, std::string> want_names = {
        {"ad40.baddog", "Bad Dog!"},
        {"classic.toilets", "Flying Toilets"},
        {"ad10.baddog", "Bad Dog! (10th Anniversary)"},
        {"ad10.baddog3", "Bad Dog!"},  // the other lane
        {"ad10.toast2k", "Toasters 2k (early build)"},
        {"ad10.toaster2", "Toasters 2k"},
        {"ad10.toasters", "Flying Toasters! (10th Anniversary)"},
        {"ad10.toilet", "Flying Toilets (10th Anniversary)"},
        {"ad10.starryni", "Starry Night Display (10th Anniversary)"},
        {"ad32.boris", "Boris"},
        {"ad32.borisb", "Boris (After Dark 3.2)"},
        {"ad32.guts", "Guts"},
        {"ad32.guts2", "guts (After Dark 3.2)"},
        {"ad32.guts3", "Guts (After Dark 3.2, GUTS3.AD)"},
        {"ad32.same", "Same Module (After Dark 3.2)"},
        {"ad32.toilet", "Flying Toilets (After Dark 3.2)"},
        {"tt.toilet", "Flying Toilets (Totally Twisted)"},
        {"tt.cham", "Chameleon"},
        {"simpsons.grampa", "Grampa's Wisdom"},
        {"swse.vader", "Darth Vader"},
        {"swse.battles", "Space Battles"},
        {"swse.swtext", "Scrolling Text"},
        {"startrek.planets", "Planetary Atlas"},
        {"startrek.braincel", "Brain Cells"},
        {"startrek.sounder", "Sounder"},
        {"ad32.messages", "Messages"},
        {"looney.ltmessgs", "Messages (Looney Tunes)"},  // 3.2's comes first in registry order
        {"looney.frog", "Michigan J. Frog"},
        {"screams.gristle", "Gristle Slam"},
        {"disney.dalm", "101 Dalmatians"},
        {"disney.mermaid", "Little Mermaid"},
        {"marvel.marvel", "Marvel Comics"},
        {"snoopy.is_fly", "Flying Ace"},
        {"snoopy.is_linus", "Linus & Snoopy"},
    };
    for (auto& [id, name] : want_names) {
      const phosg::JSON* m = module_by_id(cat, id);
      CHECK(m != nullptr);
      if (m) CHECK_EQ(m->get_string("displayName"), name);
    }
    CHECK_EQ(module_by_id(cat, "ad32.guts2")->get_string("moduleName"), std::string("guts"));
    CHECK_EQ(module_by_id(cat, "ad10.toast2k")->get_string("moduleName"), std::string("Toasters 2k (early build)"));
    // Only the Intermission modules carry "abi", as their last field.
    for (auto& m : cat.at("modules").as_list()) {
      const std::string package = m->get_string("package");
      const bool imx = package == "swse" || package == "farside" || package == "dilbert";
      CHECK_EQ(m->contains("abi"), imx);
      // (The fixture's DIL-WHAK has no dialog, so no button.)
      if (imx)
        CHECK(m->get_string("abi") == "intermission" &&
              m->at("controls").as_list().size() == (m->get_string("id") == "dilbert.dil-whak" ? 0u : 1u));
    }
    {
      const std::string text = test::read_text(all / L"win" / L"catalog-win.json");
      size_t at = text.find("\"id\": \"swse.vader\"");
      size_t end = text.find("\n  }", at);
      CHECK(at != std::string::npos && text.rfind("\"abi\": \"intermission\"\n", end) > at);
    }
    // Only Star Trek: The Screen Saver's, Marvel Comics Screen Posters' and
    // ScreamSavers' modules carry "screen" (last).
    for (auto& m : cat.at("modules").as_list()) {
      const std::string p = m->get_string("package");
      const bool fixed = p == "startrek" || p == "marvel" || p == "screams";
      CHECK_EQ(m->contains("screen"), fixed);
      if (fixed) CHECK_EQ(m->get_string("screen"), std::string("640x480"));
    }
    // Every entry carries the package fields and its md5; sameAs names the
    // first entry with the same bytes.
    std::map<std::string, std::string> first;
    for (auto& m : cat.at("modules").as_list()) {
      CHECK(m->contains("package") && m->contains("packageTitle") && m->contains("moduleName") && m->contains("md5"));
      std::string md5 = m->get_string("md5");
      CHECK_EQ(md5, md5_file_hex(all / L"win" / to_wide(m->get_string("path"))));
      auto [it, fresh] = first.emplace(md5, m->get_string("id"));
      if (fresh) CHECK(!m->contains("sameAs"));
      else CHECK_EQ(m->get_string("sameAs"), it->second);
    }
    CHECK_EQ(module_by_id(cat, "ad32.same")->get_string("sameAs"), std::string("classic.samemod"));
    CHECK_EQ(module_by_id(cat, "ad10.starryni")->get_string("sameAs"), std::string("ad40.starryni"));
    CHECK_EQ(module_by_id(cat, "tt.toilet")->get_string("sameAs"), std::string("ad10.toilet"));
    CHECK_EQ(module_by_id(cat, "ad10.toilet")->get_string("package"), std::string("ad10"));
    CHECK_EQ(module_by_id(cat, "ad10.toilet")->get_string("packageTitle"), std::string("After Dark 10th Anniversary"));
    // Deluxe's entries are exactly what a Deluxe-only catalog lists.
    phosg::JSON only = json_at(d / L"win" / L"catalog-win.json");
    for (auto& m : only.at("modules").as_list()) {
      const phosg::JSON* same = module_by_id(cat, m->get_string("id"));
      CHECK(same && same->serialize() == m->serialize());
    }
    // A re-import of swse leaves every other package byte for byte.
    {
      test::Tree others;
      for (auto& [rel, bytes] : snapshot(all / L"win"))
        if (rel.rfind("packages/swse/", 0) != 0 && rel != "catalog-win.json") others[rel] = bytes;
      test::Tree mine = snapshot_files(all / L"win" / L"packages" / L"swse");
      run("swse again", image(src / L"swse.iso"), opts_for(all, reg), Status::ok);
      test::Tree after;
      for (auto& [rel, bytes] : snapshot(all / L"win"))
        if (rel.rfind("packages/swse/", 0) != 0 && rel != "catalog-win.json") after[rel] = bytes;
      CHECK(after == others);
      CHECK(snapshot_files(all / L"win" / L"packages" / L"swse") == mine);
      CHECK(catalog_ids(all / L"win") == ids);
    }
    // Names depend on what is installed; ids never do: without Deluxe, ad10's
    // Bad Dog! keeps its name.
    CHECK_EQ(remove_package("deluxe", all, {}, reg.span()).status, Status::ok);
    phosg::JSON less = json_at(all / L"win" / L"catalog-win.json");
    CHECK_EQ(module_by_id(less, "ad10.baddog")->get_string("displayName"), std::string("Bad Dog!"));
    CHECK_EQ(module_by_id(less, "ad32.same")->get_string("displayName"), std::string("Same Module"));
    CHECK(!module_by_id(less, "ad32.same")->contains("sameAs"));
  }

  // ---- adimport.exe (built-in registry: synthetic files differ from the real manifests) ------------------
  {
    auto cli = [&](const std::vector<std::wstring>& args, const char* what, std::string* out = nullptr) {
      test::ProcessResult r = test::run_process(exe, args, 300000);
      fprintf(stderr, "[%s] exit %d\n%s", what, r.exit_code, r.output.c_str());
      if (out) *out = r.output;
      return r.exit_code;
    };
    fs::path root = dir / L"cli";
    std::string out;
    CHECK_EQ(cli({L"--no-cover-download", L"--image", (src / L"simpsons.img").wstring(), L"--dest", root.wstring()},
                 "real manifest"),
             3);
    CHECK(!fs::exists(root / L"win" / L"packages" / L"simpsons"));
    CHECK_EQ(cli({L"--no-cover-download", L"--iso", (src / L"simpsons.img").wstring(), L"--dest", root.wstring(),
                  L"--no-verify"},
                 "--iso with a floppy image", &out),
             0);
    CHECK(out.find("The Simpsons Screen Saver") != std::string::npos);
    CHECK_EQ(cli({L"--no-cover-download", L"--image", (src / L"disk1.img").wstring(), L"--image",
                  (src / L"disk2.img").wstring(), L"--dest", root.wstring(), L"--no-verify", L"--quiet"},
                 "two --image"),
             0);
    CHECK_EQ(cli({L"--no-cover-download", L"--from", (src / L"tt").wstring(), L"--dest", root.wstring(), L"--no-verify",
                  L"--package", L"tt"},
                 "--from --package"),
             0);
    CHECK_EQ(cli({L"--no-cover-download", L"--image", (src / L"ad32.iso").wstring(), L"--dest", root.wstring(),
                  L"--no-verify", L"--package", L"tt"},
                 "--package mismatch"),
             2);
    // Star Wars Screen Entertainment through adimport.exe: the synthetic files
    // are not the release (3), --no-verify imports them, five --image.
    CHECK_EQ(cli({L"--no-cover-download", L"--image", (src / L"swse.iso").wstring(), L"--dest", root.wstring()},
                 "swse, real manifest", &out),
             3);
    CHECK(out.find("Star Wars Screen Entertainment") != std::string::npos);
    CHECK_EQ(cli({L"--no-cover-download", L"--image", swse_disks[0].wstring(), L"--image", swse_disks[1].wstring(),
                  L"--image", swse_disks[2].wstring(), L"--image", swse_disks[3].wstring(), L"--image",
                  swse_disks[4].wstring(), L"--dest", root.wstring(), L"--no-verify", L"--quiet"},
                 "swse, five --image"),
             0);
    CHECK(fs::exists(root / L"win" / L"packages" / L"swse" / L"SAVER" / L"VADER.IMX"));
    // Star Trek: The Screen Saver through adimport.exe: the synthetic files
    // are not the release (3); --no-verify imports them, from the two disks
    // or the ZIP of them.
    CHECK_EQ(cli({L"--no-cover-download", L"--image", st1.wstring(), L"--image", st2.wstring(), L"--dest",
                  root.wstring()},
                 "startrek, real manifest", &out),
             3);
    CHECK(out.find("Star Trek: The Screen Saver") != std::string::npos);
    CHECK_EQ(cli({L"--no-cover-download", L"--image", (src / L"startrek-images.zip").wstring(), L"--dest",
                  root.wstring(), L"--no-verify"},
                 "startrek, the ZIP of its images", &out),
             0);
    CHECK(out.find("imported 27 files") != std::string::npos);
    CHECK(fs::exists(root / L"win" / L"packages" / L"startrek" / L"AFTERDRK" / L"ST_RES" / L"ST_SND.DLL"));
    CHECK_EQ(cli({L"--no-cover-download", L"--image", st2.wstring(), L"--dest", root.wstring(), L"--no-verify"},
                 "startrek, disk 2 alone"),
             2);
    // Marvel Comics Screen Posters and Snoopy's Screen Savers through
    // adimport.exe: the synthetic files are not the release (3); --no-verify
    // imports them, from the ZIP of their disks' folders or the two floppies.
    CHECK_EQ(cli({L"--no-cover-download", L"--image", (src / L"marvel-disks.zip").wstring(), L"--dest", root.wstring()},
                 "marvel, real manifest", &out),
             3);
    CHECK(out.find("Marvel Comics Screen Posters") != std::string::npos);
    CHECK_EQ(cli({L"--no-cover-download", L"--image", (src / L"marvel-disks.zip").wstring(), L"--dest", root.wstring(),
                  L"--no-verify"},
                 "marvel, the ZIP of its disks' folders", &out),
             0);
    CHECK(out.find("imported 64 files") != std::string::npos);
    CHECK(fs::exists(root / L"win" / L"packages" / L"marvel" / L"AFTERDRK" / L"MRVLIMAG" / L"XMEN2099.FIF"));
    CHECK_EQ(cli({L"--no-cover-download", L"--image", (src / L"snoopy-disk1.img").wstring(), L"--image",
                  (src / L"snoopy-disk2.img").wstring(), L"--dest", root.wstring(), L"--no-verify", L"--quiet"},
                 "snoopy, two --image"),
             0);
    CHECK(fs::exists(root / L"win" / L"packages" / L"snoopy" / L"AFTERDRK" / L"IS_FLY.AD"));
    CHECK_EQ(cli({L"--no-cover-download", L"--image", (src / L"snoopy-disk1.img").wstring(), L"--dest", root.wstring(),
                  L"--no-verify"},
                 "snoopy, disk 1 alone"),
             2);
    CHECK_EQ(cli({L"--image", (src / L"ad32.iso").wstring(), L"--package", L"nosuch"}, "--package unknown"), 1);
    CHECK_EQ(cli({L"--image", (src / L"ad32.iso").wstring(), L"--from", src.wstring()}, "--image + --from"), 1);
    CHECK_EQ(cli({L"--list-packages", L"--dest", root.wstring()}, "--list-packages", &out), 0);
    CHECK(out.find("simpsons") != std::string::npos && out.find("installed, ") != std::string::npos &&
          out.find("not installed") != std::string::npos);
    // The title column fits the longest title: the state starts in one column on every line.
    {
      std::vector<size_t> cols;
      size_t pos = 0;
      while ((pos = out.find("\n  ", pos)) != std::string::npos) {
        size_t eol = out.find('\n', pos + 1);
        std::string line = out.substr(pos + 1, eol == std::string::npos ? std::string::npos : eol - pos - 1);
        size_t col = line.find("installed");
        if (col != std::string::npos) cols.push_back(line.rfind("not ", col) == col - 4 ? col - 4 : col);
        pos += 3;
      }
      CHECK_EQ(cols.size(), size_t(14));
      for (size_t c : cols) CHECK_EQ(c, cols.front());
      CHECK(out.find("  swse      Star Wars Screen Entertainment               installed, ") != std::string::npos);
      CHECK(out.find("  startrek  Star Trek: The Screen Saver                  installed, ") != std::string::npos);
      CHECK(out.find("; download 2.8 MB (2 floppy images)") != std::string::npos);
      CHECK(out.find("  marvel    Marvel Comics Screen Posters                 installed, ") != std::string::npos);
      CHECK(out.find("  snoopy    Snoopy's Screen Savers                       installed, ") != std::string::npos);
      CHECK(out.find("; download 1.9 MB (ZIP of the install files)") != std::string::npos);
      CHECK(out.find("  disney    The Disney Collection Screen Saver           "
                     "not installed; download 3.4 MB (ZIP of the install files)") != std::string::npos);
      CHECK(out.find("  farside   The Far Side Screen Saver Collection         "
                     "not installed; download 5.5 MB (5 ZIPs of the install disks' files)") != std::string::npos);
      CHECK(out.find("  dilbert   Scott Adams' Dilbert Screen Saver Collection not installed; download 4.3 MB (ZIP of "
                     "the install files)") != std::string::npos);
    }
    CHECK_EQ(cli({L"--list-packages", L"--image", L"x"}, "--list-packages + a source"), 1);
    CHECK_EQ(cli({L"--remove", L"tt", L"--catalog-only"}, "--remove + --catalog-only"), 1);
    CHECK_EQ(cli({L"--remove", L"nosuch", L"--dest", root.wstring()}, "--remove unknown"), 1);
    CHECK_EQ(cli({L"--remove", L"tt", L"--dest", root.wstring()}, "--remove", &out), 0);
    CHECK(!fs::exists(root / L"win" / L"packages" / L"tt"));
    CHECK_EQ(cli({L"--remove", L"tt", L"--dest", root.wstring()}, "--remove again"), 1);
    // --catalog-only with no FILES at all.
    CHECK_EQ(cli({L"--catalog-only", L"--dest", root.wstring()}, "--catalog-only without FILES", &out), 0);
    CHECK(out.find(std::to_string(simpsons.ids.size() + swse.ids.size() + startrek.ids.size() + marvel.ids.size() +
                                  snoopy.ids.size()) +
                   " modules") != std::string::npos);
    CHECK(!fs::exists(root / L"win" / L"FILES"));
  }
  return test::finish("import.packages");
}
