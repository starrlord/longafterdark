// --download of every package from its Internet Archive copies (packages.h
// `downloads`), against a loopback server serving synthetic sources shaped
// like the real ones (tests/pkg_fixture.h; no After Dark bytes):
//   images    Deluxe, 10th Anniversary, 3.2, Totally Twisted and Star Wars
//             Screen Entertainment as ISOs whose md5 is the package's known
//             image: verified "image"; Star Trek: The Screen Saver as a copy
//             of two floppy images (both parts fetched and checked, progress
//             over the pair, a copy used only when both parts verify, a
//             partly downloaded copy completed), a known set of install
//             disks: verified "image"
//   zip       the Simpsons as a flat ZIP of its install files, read as the
//             install folder: verified "files" (and Star Wars Screen
//             Entertainment's flat ZIP when its disc image is gone); the same
//             ZIP given as an image; ZIPs that nest the files or are
//             password-protected; the Looney Tunes, ScreamSavers (its disks
//             in DISK1-DISK3 folders), the Disney Collection and Snoopy's
//             Screen Savers (Disk1/Disk2 folders) as the one ZIP of their
//             install files, and Marvel Comics Screen Posters as either of
//             its two (flat, or in Disk1/Disk2 folders when the flat one is
//             gone), whose md5s are the known images: verified "image", the
//             owners' notes in them never read
//   copies    a 404, a wrong size (refused before a byte is written, the
//             .part kept for the next copy to resume) and a wrong md5 each
//             fall back to the next copy; when every copy fails: 3 for a
//             wrong file, 4 for none reachable, with the advice to import
//             from the disc, and nothing left behind; a file already
//             downloaded from any copy is used without a request
//   records   import.json's download record (version 1 and 2)
//   custom    --url with and without --package, --md5 over the registry's
//             (over a floppy set's copies: each copy's first image's md5
//             only)
//   all       import_downloads over every package; a cancel stops it
//   registry  the built-in copies: https://archive.org/download/ URLs, the
//             known image md5s and sizes, file names per content, the
//             verified Simpsons ZIPs, Star Wars Screen Entertainment's ISO,
//             Redump BIN and ZIP, Star Trek: The Screen Saver's two pairs of
//             disk images (each pair a complete known disk set), the five
//             after-dark-collection ZIPs and Marvel Comics Screen Posters'
//             flat ZIP, all known images
//   adimport  --download <id> / all, their conflicts, --list-packages sizes
//
//   test_import_pkg_download <adimport.exe> <scratch>
#include "http_server.h"

#include <phosg/JSON.hh>

#include <map>
#include <set>

#include "importer.h"
#include "md5.h"
#include "names.h"
#include "pkg_fixture.h"
#include "run_process.h"

using namespace adw::import;
namespace fs = std::filesystem;

namespace {

std::vector<std::string> g_log;

ImportOptions opts_for(const fs::path& root, const test::TestRegistry& reg) {
  ImportOptions o;
  o.assets_root = root;
  o.registry = reg.span();
  o.log = [](const std::string& s) {
    g_log.push_back(s);
    fprintf(stderr, "  log: %s\n", s.c_str());
  };
  return o;
}

Source download(const fs::path& dl, const std::string& package, const std::string& url = "",
                const std::string& md5 = "") {
  Source s;
  s.kind = Source::Kind::download;
  s.path = dl;
  s.package = package;
  s.url = url;
  s.expected_md5 = md5;
  return s;
}

Source image(const fs::path& p) {
  Source s;
  s.kind = Source::Kind::image;
  s.path = p;
  return s;
}

ImportResult run(const std::string& what, const Source& s, const ImportOptions& o, Status want) {
  ImportResult r = run_import(s, o);
  fprintf(stderr, "[%s] %s: %s\n", what.c_str(), status_name(r.status), r.message.c_str());
  CHECK_EQ(r.status, want);
  return r;
}

phosg::JSON json_at(const fs::path& p) { return phosg::JSON::parse(test::read_text(p)); }

bool logged(const std::string& needle) {
  for (auto& l : g_log)
    if (l.find(needle) != std::string::npos) return true;
  return false;
}

std::string md5_of(const std::vector<uint8_t>& b) { return md5_hex(b.data(), b.size()); }

fs::path with_suffix(fs::path p, const wchar_t* suffix) {
  p += suffix;
  return p;
}

std::vector<std::string> paths(test::Server& srv) {
  std::vector<std::string> v;
  for (auto& r : srv.requests()) v.push_back(r.path);
  return v;
}

// The files installed under <win>/<root> (import.json aside) are exactly the fixture's.
void check_installed(const fs::path& win, const test::PkgFixture& f, const std::string& root) {
  test::Tree have, want;
  fs::path base = test::path_under(win, root);
  for (const std::string& rel : test::list_tree(base))
    if (rel != "import.json") have[root + "/" + rel] = test::read_bytes(test::path_under(base, rel));
  for (auto& [rel, d] : f.expect)
    if (rel.rfind(root + "/", 0) == 0) want[rel] = d;
  for (auto& [rel, d] : want) {
    auto it = have.find(rel);
    if (it == have.end() || it->second != d) {
      test::g_failures++;
      fprintf(stderr, "  %s: %s\n", it == have.end() ? "missing" : "differs", rel.c_str());
    }
  }
  for (auto& [rel, d] : have)
    if (!want.count(rel)) {
      test::g_failures++;
      fprintf(stderr, "  unexpected: %s\n", rel.c_str());
    }
}

std::vector<std::string> catalog_ids(const fs::path& win) {
  std::vector<std::string> ids;
  phosg::JSON cat = json_at(win / L"catalog-win.json");  // alive for the loop (a temporary would not be)
  for (auto& m : cat.at("modules").as_list()) ids.push_back(m->get_string("id"));
  return ids;
}

// A flat ZIP of the install files at the source's root, no password — the
// Internet Archive's Simpsons copies (which lack the owner's notes).
std::vector<uint8_t> flat_zip(const test::Tree& t, uint16_t dos_date = 0x1EF1, const std::string& prefix = "") {
  test::ZipBuilder b;
  b.password = "";
  for (const auto& [rel, d] : t) {
    if (rel == "CEREAL.TXT" || rel == "SERIAL.TXT") continue;
    b.add(prefix + rel, d, /*deflate=*/true, /*encrypt=*/false);
    b.members.back().dos_date = dos_date;
  }
  return b.build();
}

bool is_md5(const std::string& s) {
  return s.size() == 32 && s.find_first_not_of("0123456789abcdef") == std::string::npos;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: test_import_pkg_download <adimport.exe> [scratch]\n");
    return 2;
  }
  std::wstring exe = fs::absolute(argv[1]).wstring();
  fs::path dir = test::scratch(argc - 1, argv + 1, "adw-import-pkg-download");
  test::sandbox_data_root(dir / L"localappdata");  // no default may reach the real data folder
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);

  const test::PkgFixture deluxe = test::deluxe_fixture(), ad10 = test::ad10_fixture(), ad32 = test::ad32_fixture(),
                         tt = test::tt_fixture(), simpsons = test::simpsons_fixture(), swse = test::swse_fixture(),
                         startrek = test::startrek_fixture(), looney = test::looney_fixture(),
                         screams = test::screams_fixture(), disney = test::disney_fixture(),
                         farside = test::farside_fixture(), dilbert = test::dilbert_fixture();
  const test::IslibFixture marvel = test::marvel_fixture(), snoopy = test::snoopy_fixture();
  test::TestRegistry reg;
  for (auto& [id, f] : std::map<std::string, const test::PkgFixture*>{{"deluxe", &deluxe},
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
                                                                     {"dilbert", &dilbert}})
    reg.manifest(id, test::manifest_of(f->expect));

  // What the server publishes. The disc images' md5s are their packages'
  // known images, as the real downloads' are.
  const auto deluxe_iso = test::iso_of(deluxe, true, "AD_DELUXE"), ad10_iso = test::iso_of(ad10, true, "AD10TH"),
             ad32_iso = test::iso_of(ad32, false, "ADW320_C"), tt_iso = test::iso_of(tt, false, "TTW320CD");
  const auto simp_zip = flat_zip(simpsons.source), simp_zip94 = flat_zip(simpsons.source, 0x1C58);
  const auto swse_iso = test::iso_of(swse, false, "SWSE"), swse_zip = flat_zip(swse.source);
  reg.image("deluxe", md5_of(deluxe_iso), deluxe_iso.size());
  reg.image("ad10", md5_of(ad10_iso), ad10_iso.size());
  reg.image("ad32", md5_of(ad32_iso), ad32_iso.size());
  reg.image("tt", md5_of(tt_iso), tt_iso.size());
  reg.image("swse", md5_of(swse_iso), swse_iso.size());
  CHECK(md5_of(simp_zip) != md5_of(simp_zip94));
  // Star Trek: The Screen Saver's two install floppies, and the same disks
  // as another copy wrote them (other bytes, the same files): four known
  // disk images.
  auto st_disk = [&](int k, const std::string& label) {
    test::FatBuilder b = test::FatBuilder::floppy144();
    b.label = label;
    return test::fat_of(startrek.source, k, "", test::startrek_disk, std::move(b));
  };
  const auto st1 = st_disk(1, ""), st2 = st_disk(2, ""), st1b = st_disk(1, "WIN9XCOPY"), st2b = st_disk(2, "WIN9XCOPY");
  reg.disk_images("startrek", {{md5_of(st1), st1.size(), 1},
                               {md5_of(st2), st2.size(), 2},
                               {md5_of(st1b), st1b.size(), 1},
                               {md5_of(st2b), st2b.size(), 2}});
  // The Looney Tunes, ScreamSavers and the Disney Collection: the one ZIP of
  // their install files, owner's note and all (ScreamSavers' in DISK1-DISK3
  // folders), whose md5 is the known image, as the real ones' are: a "zip"
  // copy verified "image".
  const auto looney_zip = test::zip_folder(looney.source), disney_zip = test::zip_folder(disney.source),
             screams_zip = test::zip_disk_folders(screams.source, test::screams_disk, 3);
  reg.image("looney", md5_of(looney_zip), looney_zip.size());
  reg.image("screams", md5_of(screams_zip), screams_zip.size());
  reg.image("disney", md5_of(disney_zip), disney_zip.size());
  // Marvel Comics Screen Posters: a flat ZIP of the install files and a ZIP
  // of both disks in Disk1/Disk2 folders, owners' notes and all, both known;
  // Snoopy's Screen Savers: the ZIP of both disks.
  const auto marvel_flat = test::zip_folder(marvel.source), marvel_disks = test::zip_of_disks(marvel.disks()),
             snoopy_zip = test::zip_of_disks(snoopy.disks());
  reg.zip_images("marvel", {{md5_of(marvel_flat), marvel_flat.size()}, {md5_of(marvel_disks), marvel_disks.size()}});
  reg.zip_images("snoopy", {{md5_of(snoopy_zip), snoopy_zip.size()}});
  // The Far Side: a ZIP of each install disk's files (a BBS's note in each,
  // the same bytes), the five its known disk images; Dilbert: the ZIP of
  // every disk's files, its known image.
  std::vector<std::vector<uint8_t>> fs_zips;
  std::vector<test::TestRegistry::Disk> fs_known;
  for (int k = 1; k <= 5; k++) {
    test::Tree t = test::disk_files(farside.source, k, test::farside_disk);
    t["FILE_ID.DIZ"] = test::vec("A made-up BBS's note.\r\n");
    fs_zips.push_back(test::zip_folder(t));
    fs_known.push_back({md5_of(fs_zips.back()), fs_zips.back().size(), k});
  }
  reg.disk_images("farside", fs_known);
  const auto dilbert_zip = test::zip_folder(dilbert.source);
  reg.image("dilbert", md5_of(dilbert_zip), dilbert_zip.size());

  test::Server srv(test::pattern(1000, 1));
  for (int k = 1; k <= 5; k++) srv.serve("/fs" + std::to_string(k) + ".zip", fs_zips[size_t(k - 1)]);
  srv.serve("/dilbert.zip", dilbert_zip);
  srv.serve("/marvel-flat.zip", marvel_flat);
  srv.serve("/marvel-disks.zip", marvel_disks);
  srv.serve("/snoopy.zip", snoopy_zip);
  srv.serve("/looney.zip", looney_zip);
  srv.serve("/screams.zip", screams_zip);
  srv.serve("/disney.zip", disney_zip);
  srv.serve("/deluxe.iso", deluxe_iso);
  srv.serve("/ad10th.iso", ad10_iso);
  srv.serve("/ad32.iso", ad32_iso);
  srv.serve("/tt.iso", tt_iso);
  srv.serve("/SIMPSONS.zip", simp_zip);
  srv.serve("/simpsons-1994.zip", simp_zip94);
  srv.serve("/AfterDarkStarWars.iso", swse_iso);
  srv.serve("/SWSE.zip", swse_zip);
  srv.serve("/st1.img", st1);
  srv.serve("/st2.img", st2);
  srv.serve("/st1b.img", st1b);
  srv.serve("/st2b.img", st2b);
  auto st2_bad = st2;
  st2_bad[st2_bad.size() / 2] ^= 0xFF;
  srv.serve("/st2-bad.img", st2_bad);
  auto tt_short = tt_iso;
  tt_short.resize(tt_short.size() - 2048);  // another size: refused before the transfer
  srv.serve("/tt-short.iso", tt_short);
  auto ad32_bad = ad32_iso;
  ad32_bad[ad32_bad.size() / 2] ^= 0xFF;  // the right size, other bytes: refused by md5
  srv.serve("/ad32-bad.iso", ad32_bad);

  using Copy = test::TestRegistry::Copy;
  const Copy deluxe_copy{srv.url("/r/deluxe.iso"), L"deluxe.iso", deluxe_iso.size(), md5_of(deluxe_iso)};
  const Copy ad10_copy{srv.url("/r/ad10th.iso"), L"ad10th.iso", ad10_iso.size(), md5_of(ad10_iso)};
  const Copy ad32_gone{srv.url("/r/gone.iso"), L"ad32.iso", ad32_iso.size(), md5_of(ad32_iso)};
  const Copy ad32_copy{srv.url("/r/ad32.iso"), L"ad32.iso", ad32_iso.size(), md5_of(ad32_iso)};
  const Copy ad32_bad_copy{srv.url("/r/ad32-bad.iso"), L"ad32.iso", ad32_iso.size(), md5_of(ad32_iso)};
  const Copy tt_copy{srv.url("/r/tt.iso"), L"TTW320CD.ISO", tt_iso.size(), md5_of(tt_iso)};
  const Copy simp_copy{srv.url("/r/SIMPSONS.zip"), L"SIMPSONS.zip", simp_zip.size(), md5_of(simp_zip), "zip"};
  const Copy simp94_copy{srv.url("/r/simpsons-1994.zip"), L"simpsons-1994.zip", simp_zip94.size(), md5_of(simp_zip94),
                         "zip"};
  const Copy swse_copy{srv.url("/r/AfterDarkStarWars.iso"), L"AfterDarkStarWars.iso", swse_iso.size(), md5_of(swse_iso)};
  const Copy swse_zip_copy{srv.url("/r/SWSE.zip"), L"SWSE.zip", swse_zip.size(), md5_of(swse_zip), "zip"};
  // Two copies of two parts each, as the Internet Archive's two items are.
  const Copy st_copy{srv.url("/r/st1.img"), L"st1.img", st1.size(), md5_of(st1), "image",
                     {{srv.url("/r/st2.img"), L"st2.img", st2.size(), md5_of(st2)}}};
  const Copy st_copy_b{srv.url("/r/st1b.img"), L"st1b.img", st1b.size(), md5_of(st1b), "image",
                       {{srv.url("/r/st2b.img"), L"st2b.img", st2b.size(), md5_of(st2b)}}};
  const Copy looney_copy{srv.url("/r/looney.zip"), L"After Dark - Looney Tunes.zip", looney_zip.size(),
                         md5_of(looney_zip), "zip"};
  const Copy screams_copy{srv.url("/r/screams.zip"), L"After Dark - Scream Savers.zip", screams_zip.size(),
                          md5_of(screams_zip), "zip"};
  const Copy disney_copy{srv.url("/r/disney.zip"), L"After Dark - Disney Collection.zip", disney_zip.size(),
                         md5_of(disney_zip), "zip"};
  const Copy marvel_flat_copy{srv.url("/r/marvel-flat.zip"), L"After Dark - Marvel Screen Posters.zip",
                              marvel_flat.size(), md5_of(marvel_flat), "zip"};
  const Copy marvel_disks_copy{srv.url("/r/marvel-disks.zip"), L"After Dark - Marvel Comics.zip", marvel_disks.size(),
                               md5_of(marvel_disks), "zip"};
  const Copy snoopy_copy{srv.url("/r/snoopy.zip"), L"After Dark - Snoopy.zip", snoopy_zip.size(), md5_of(snoopy_zip),
                         "zip"};
  // A "zip" copy of five parts, one ZIP per install disk.
  std::vector<test::TestRegistry::Part> fs_more;
  for (int k = 2; k <= 5; k++)
    fs_more.push_back({srv.url("/r/fs" + std::to_string(k) + ".zip"), L"PNX-FSC" + std::to_wstring(k) + L".ZIP",
                       fs_zips[size_t(k - 1)].size(), md5_of(fs_zips[size_t(k - 1)])});
  const Copy farside_copy{srv.url("/r/fs1.zip"), L"PNX-FSC1.ZIP", fs_zips[0].size(), md5_of(fs_zips[0]), "zip", fs_more};
  const Copy dilbert_copy{srv.url("/r/dilbert.zip"), L"DilbertS.zip", dilbert_zip.size(), md5_of(dilbert_zip), "zip"};
  // Shaped like the real registry: archive.org's hop to a storage node, and
  // a dead first copy for ad32 (its later copies are the fallbacks).
  auto good_registry = [&] {
    reg.downloads("deluxe", {deluxe_copy});
    reg.downloads("ad10", {ad10_copy});
    reg.downloads("ad32", {ad32_gone, ad32_copy});
    reg.downloads("tt", {tt_copy});
    reg.downloads("simpsons", {simp_copy, simp94_copy});
    reg.downloads("swse", {swse_copy, swse_zip_copy});
    reg.downloads("startrek", {st_copy, st_copy_b});
    reg.downloads("marvel", {marvel_flat_copy, marvel_disks_copy});
    reg.downloads("snoopy", {snoopy_copy});
    reg.downloads("looney", {looney_copy});
    reg.downloads("screams", {screams_copy});
    reg.downloads("disney", {disney_copy});
    reg.downloads("farside", {farside_copy});
    reg.downloads("dilbert", {dilbert_copy});
  };
  good_registry();

  // ---- every package on its own, one shared downloads folder ---------------------------------
  const fs::path dl = dir / L"downloads";
  struct One {
    const char* id;
    const test::PkgFixture* f;
    const char* root;
    const wchar_t* file;
    std::string served;  // the path the copy redirects to
    const char* verified;
    const char* format;
  };
  for (const One& o : std::vector<One>{
           {"deluxe", &deluxe, "FILES", L"deluxe.iso", "/deluxe.iso", "image", "iso9660+joliet"},
           {"ad10", &ad10, "packages/ad10", L"ad10th.iso", "/ad10th.iso", "image", "iso9660+joliet"},
           {"ad32", &ad32, "packages/ad32", L"ad32.iso", "/ad32.iso", "image", "iso9660"},
           {"tt", &tt, "packages/tt", L"TTW320CD.ISO", "/tt.iso", "image", "iso9660"},
           {"simpsons", &simpsons, "packages/simpsons", L"SIMPSONS.zip", "/SIMPSONS.zip", "files", "zip"},
           {"swse", &swse, "packages/swse", L"AfterDarkStarWars.iso", "/AfterDarkStarWars.iso", "image", "iso9660"},
           {"startrek", &startrek, "packages/startrek", L"st1.img", "/st1.img", "image", "fat12"},
           {"marvel", &marvel, "packages/marvel", L"After Dark - Marvel Screen Posters.zip", "/marvel-flat.zip", "image",
            "zip"},
           {"snoopy", &snoopy, "packages/snoopy", L"After Dark - Snoopy.zip", "/snoopy.zip", "image", "zip"},
           {"looney", &looney, "packages/looney", L"After Dark - Looney Tunes.zip", "/looney.zip", "image", "zip"},
           {"screams", &screams, "packages/screams", L"After Dark - Scream Savers.zip", "/screams.zip", "image", "zip"},
           {"disney", &disney, "packages/disney", L"After Dark - Disney Collection.zip", "/disney.zip", "image",
            "zip"},
           {"farside", &farside, "packages/farside", L"PNX-FSC1.ZIP", "/fs1.zip", "image", "zip"},
           {"dilbert", &dilbert, "packages/dilbert", L"DilbertS.zip", "/dilbert.zip", "image", "zip"},
       }) {
    fs::path root = dir / (L"alone-" + to_wide(o.id));
    srv.clear();
    g_log.clear();
    ImportResult r = run(std::string("download ") + o.id, download(dl, o.id), opts_for(root, reg), Status::ok);
    CHECK_EQ(r.package_id, std::string(o.id));
    CHECK_EQ(r.verified, std::string(o.verified));
    CHECK_EQ(r.format, std::string(o.format));
    CHECK(r.missing_known.empty());
    check_installed(root / L"win", *o.f, o.root);
    CHECK(catalog_ids(root / L"win") == o.f->ids);
    CHECK(fs::exists(dl / o.file));
    CHECK(!fs::exists(with_suffix(dl / o.file, L".part")));
    CHECK(!fs::exists(with_suffix(dl / o.file, L".lock")));
    CHECK_EQ(r.final_url, srv.url(o.served));
    CHECK(r.download_md5_checked);
    const std::string id = o.id;
    const std::string want_url = id == "deluxe"     ? deluxe_copy.url
                                 : id == "ad10"     ? ad10_copy.url
                                 : id == "ad32"     ? ad32_copy.url
                                 : id == "tt"       ? tt_copy.url
                                 : id == "swse"     ? swse_copy.url
                                 : id == "startrek" ? st_copy.url
                                 : id == "marvel"   ? marvel_flat_copy.url
                                 : id == "snoopy"   ? snoopy_copy.url
                                 : id == "looney"   ? looney_copy.url
                                 : id == "screams"  ? screams_copy.url
                                 : id == "disney"   ? disney_copy.url
                                 : id == "farside"  ? farside_copy.url
                                 : id == "dilbert"  ? dilbert_copy.url
                                                    : simp_copy.url;
    CHECK_EQ(r.url, want_url);
    if (id == "deluxe") {
      // Deluxe's record stays version 1: kind download, the URL fetched, where it led.
      phosg::JSON j = json_at(r.import_json);
      CHECK_EQ(j.get_int("version"), int64_t(1));
      CHECK_EQ(j.at("source").get_string("kind"), std::string("download"));
      CHECK_EQ(j.at("source").get_string("url"), want_url);
      CHECK_EQ(j.at("source").get_string("finalUrl"), srv.url(o.served));
      CHECK_EQ(j.at("source").get_bool("isoMd5Known"), true);
    } else {
      phosg::JSON j = json_at(r.import_json);
      const phosg::JSON& s = j.at("source");
      CHECK_EQ(j.get_int("version"), int64_t(2));
      CHECK_EQ(s.get_string("kind"), std::string("download"));
      CHECK_EQ(s.get_string("format"), std::string(o.format));
      CHECK_EQ(s.get_string("url"), want_url);
      CHECK_EQ(s.get_string("finalUrl"), srv.url(o.served));
      CHECK_EQ(s.get_bool("md5Checked"), true);
      CHECK_EQ(s.get_bool("imageMd5Known"), id != "simpsons");
      CHECK_EQ(j.get_string("verified"), std::string(o.verified));
      if (id == "startrek") {
        // Both disks' images, one part each; the record's URL is the first's.
        CHECK(!s.contains("imageMd5"));
        const auto& parts = s.at("parts").as_list();
        CHECK(parts.size() == 2 && parts[0]->get_string("md5") == md5_of(st1) &&
              parts[1]->get_string("md5") == md5_of(st2));
        CHECK(test::read_bytes(dl / L"st2.img") == st2);
        CHECK(!fs::exists(with_suffix(dl / L"st2.img", L".part")));
      } else if (id == "farside") {
        // The five disks' ZIPs, one part each, in disk order.
        CHECK(!s.contains("imageMd5"));
        const auto& parts = s.at("parts").as_list();
        CHECK_EQ(parts.size(), size_t(5));
        for (size_t k = 0; k < parts.size() && k < fs_zips.size(); k++)
          CHECK_EQ(parts[k]->get_string("md5"), md5_of(fs_zips[k]));
        CHECK(test::read_bytes(dl / L"PNX-FSC5.ZIP") == fs_zips[4]);
      } else {
        CHECK_EQ(s.get_string("imageMd5"), md5_file_hex(dl / o.file));
      }
    }
    if (id == "ad32") {
      // The first copy is gone (404): the second one is used.
      auto p = paths(srv);
      CHECK(!p.empty() && p.front() == "/r/gone.iso");
      CHECK(std::find(p.begin(), p.end(), "/ad32.iso") != p.end());
      CHECK(logged("trying another copy: " + ad32_copy.url));
    }
    if (id == "simpsons") {
      CHECK(logged("a ZIP of install files"));
      std::map<std::string, std::string> from;
      phosg::JSON j = json_at(r.import_json);
      for (auto& f : j.at("files").as_list()) from[f->get_string("path")] = f->get_string("from");
      CHECK_EQ(from["packages/simpsons/SIMPSONS/BURNS.AD"], std::string("BURNS.ZIP!BURNS.AD"));
      CHECK_EQ(from["packages/simpsons/ENGINE/ADTASK.DLL"], std::string("ENGINE.ZIP!ADTASK.DLL"));
      // The installer archives' password is still never written or logged.
      for (auto& l : g_log) CHECK(l.find(test::kTestZipPassword) == std::string::npos);
      CHECK(test::read_text(r.import_json).find(test::kTestZipPassword) == std::string::npos);
    }
    if (id == "farside") {
      // Five ZIPs, one per install disk, each part fetched and checked: the
      // known set, verified "image"; the BBS's note in each never named.
      CHECK(logged("ZIPs of the install disks' files, the known copies of The Far Side Screen Saver Collection (by "
                   "their md5s)"));
      for (int k = 2; k <= 5; k++) CHECK(fs::exists(dl / (L"PNX-FSC" + std::to_wstring(k) + L".ZIP")));
      CHECK(test::read_text(r.import_json).find("FILE_ID") == std::string::npos);
    }
    if (id == "looney" || id == "screams" || id == "disney" || id == "marvel" || id == "snoopy" || id == "dilbert") {
      // A "zip" copy whose md5 is the known image: verified as that image,
      // the owners' notes in it never read.
      CHECK(logged("a ZIP of install files, the known copy of " + std::string(find_package(id)->title) + " (by its md5)"));
      CHECK(!logged("checking every file"));
      std::string text = test::read_text(r.import_json);
      for (auto& l : g_log) text += l;
      for (const char* note : {"_SN.TXT", "REG'D", "DIZNY", "SERIAL", "REG#", "ADNEWS"})
        CHECK(ascii_upper(text).find(note) == std::string::npos);
    }
    if (id == "snoopy") CHECK(logged("reading After Dark - Snoopy.zip as the union of its folders Disk1 and Disk2"));
  }

  // ---- Star Wars Screen Entertainment from its flat ZIP when the disc image is gone ----------
  {
    const Copy gone{srv.url("/r/gone-swse.iso"), L"AfterDarkStarWars.iso", swse_iso.size(), md5_of(swse_iso)};
    reg.downloads("swse", {gone, swse_zip_copy});
    fs::path dlz = dir / L"downloads-swse-zip", root = dir / L"swse-zip";
    g_log.clear();
    ImportResult r = run("swse, image gone: the ZIP", download(dlz, "swse"), opts_for(root, reg), Status::ok);
    CHECK_EQ(r.url, swse_zip_copy.url);
    CHECK_EQ(r.verified, std::string("files"));
    CHECK_EQ(r.format, std::string("zip"));
    CHECK(logged("a ZIP of install files"));
    check_installed(root / L"win", swse, "packages/swse");
    CHECK(catalog_ids(root / L"win") == swse.ids);
    phosg::JSON j = json_at(r.import_json);
    CHECK_EQ(j.at("source").get_string("kind"), std::string("download"));
    CHECK_EQ(j.at("source").get_bool("imageMd5Known"), false);
    CHECK_EQ(j.at("package").get_string("recipe"), std::string("intermission"));
    good_registry();
  }

  // ---- Marvel Comics Screen Posters from its second copy when the first is gone -----------------
  // The ZIP of both disks' folders is a known image too: verified "image".
  {
    const Copy gone{srv.url("/r/gone-marvel.zip"), L"After Dark - Marvel Screen Posters.zip", marvel_flat.size(),
                    md5_of(marvel_flat), "zip"};
    reg.downloads("marvel", {gone, marvel_disks_copy});
    fs::path dlm = dir / L"downloads-marvel", root = dir / L"marvel-second";
    g_log.clear();
    ImportResult r = run("marvel, the flat ZIP gone: the disks' ZIP", download(dlm, "marvel"), opts_for(root, reg),
                         Status::ok);
    CHECK_EQ(r.url, marvel_disks_copy.url);
    CHECK_EQ(r.verified, std::string("image"));
    CHECK(logged("trying another copy: " + marvel_disks_copy.url));
    CHECK(logged("reading After Dark - Marvel Comics.zip as the union of its folders Disk1 and Disk2"));
    check_installed(root / L"win", marvel, "packages/marvel");
    CHECK_EQ(json_at(r.import_json).at("package").get_string("recipe"), std::string("islib"));
    good_registry();
  }

  // ---- Star Trek: The Screen Saver: copies of two images each ---------------------------------
  {
    const uint64_t pair = st1.size() + st2.size();
    // Progress runs over the whole pair: the second part's bytes come after
    // the first's, out of both parts' published sizes.
    {
      fs::path dlp = dir / L"downloads-st-progress";
      uint64_t last_done = 0, max_total = 0, min_total = UINT64_MAX;
      bool backwards = false;
      ImportOptions o = opts_for(dir / L"st-progress", reg);
      o.progress = [&](const Progress& p) {
        if (p.phase != Progress::Phase::download) return true;
        backwards = backwards || p.done < last_done;
        last_done = p.done;
        max_total = std::max(max_total, p.total);
        min_total = std::min(min_total, p.total);
        return true;
      };
      ImportResult r = run("startrek, progress over both parts", download(dlp, "startrek"), o, Status::ok);
      CHECK(!backwards);
      CHECK_EQ(last_done, pair);
      CHECK(min_total == pair && max_total == pair);
      CHECK_EQ(r.verified, std::string("image"));
    }
    // A copy is used only when every part verifies: a second part gone (404)
    // or with other bytes moves on to the next copy, both of whose parts are
    // then fetched.
    for (const auto& [what, second] : std::vector<std::pair<std::string, std::string>>{
             {"second part gone", "/r/st2-gone.img"}, {"second part damaged", "/r/st2-bad.img"}}) {
      Copy broken = st_copy;
      broken.more[0].url = srv.url(second);
      reg.downloads("startrek", {broken, st_copy_b});
      fs::path dlb = dir / to_wide("downloads-st-" + what);
      srv.clear();
      g_log.clear();
      ImportResult r = run("startrek, " + what, download(dlb, "startrek"), opts_for(dir / to_wide("st-" + what), reg),
                           Status::ok);
      CHECK_EQ(r.url, st_copy_b.url);
      CHECK(logged("trying another copy: " + st_copy_b.url));
      CHECK_EQ(r.verified, std::string("image"));
      CHECK(test::read_bytes(dlb / L"st1b.img") == st1b && test::read_bytes(dlb / L"st2b.img") == st2b);
      check_installed(dir / to_wide("st-" + what) / L"win", startrek, "packages/startrek");
      auto p = paths(srv);
      CHECK(std::find(p.begin(), p.end(), "/st1.img") != p.end());  // the first part was fetched, the pair refused
    }
    good_registry();
    // Both parts already here: nothing is requested. Only the first part
    // here: the copy is completed (the first part reused, not fetched again).
    {
      fs::path dlr = dir / L"downloads-st-reuse";
      test::write_bytes(dlr / L"st1.img", st1);
      test::write_bytes(dlr / L"st2.img", st2);
      srv.clear();
      ImportResult r = run("startrek, both parts on disk", download(dlr, "startrek"), opts_for(dir / L"st-reuse", reg),
                           Status::ok);
      CHECK(srv.requests().empty());
      CHECK(r.final_url.empty());
      fs::path dlh = dir / L"downloads-st-half";
      test::write_bytes(dlh / L"st1.img", st1);
      srv.clear();
      r = run("startrek, one part on disk", download(dlh, "startrek"), opts_for(dir / L"st-half", reg), Status::ok);
      auto p = paths(srv);
      CHECK(std::find(p.begin(), p.end(), "/st1.img") == p.end() && std::find(p.begin(), p.end(), "/st2.img") != p.end());
      CHECK(r.final_url.empty());  // the first part's, which was reused
      CHECK_EQ(r.verified, std::string("image"));
      // The other copy's pair on disk is preferred to fetching the first —
      // also over a first copy of which only one part is here.
      fs::path dlo = dir / L"downloads-st-other";
      test::write_bytes(dlo / L"st1b.img", st1b);
      test::write_bytes(dlo / L"st2b.img", st2b);
      test::write_bytes(dlo / L"st1.img", st1);
      srv.clear();
      r = run("startrek, the other copy on disk", download(dlo, "startrek"), opts_for(dir / L"st-other", reg),
              Status::ok);
      CHECK(srv.requests().empty());
      CHECK_EQ(r.url, st_copy_b.url);
    }
    // Every copy fails: 4, and nothing is imported.
    {
      Copy gone = st_copy, gone_b = st_copy_b;
      gone.more[0].url = srv.url("/r/nope1.img");
      gone_b.more[0].url = srv.url("/r/nope2.img");
      reg.downloads("startrek", {gone, gone_b});
      fs::path root = dir / L"st-fail";
      ImportResult r = run("startrek, no complete copy", download(dir / L"downloads-st-fail", "startrek"),
                           opts_for(root, reg), Status::network);
      CHECK(r.message.find("none of the 2 copies") != std::string::npos);
      CHECK(r.message.find("Star Trek: The Screen Saver from its disc with --image or --from") != std::string::npos);
      CHECK(!fs::exists(root / L"win" / L"packages" / L"startrek"));
      good_registry();
    }
  }

  // ---- reuse: nothing is requested for a file already downloaded ---------------------------
  {
    srv.clear();
    g_log.clear();
    ImportResult r = run("ad10 again", download(dl, "ad10"), opts_for(dir / L"again-ad10", reg), Status::ok);
    CHECK(srv.requests().empty());
    CHECK(r.final_url.empty());
    CHECK_EQ(r.verified, std::string("image"));
    CHECK(logged("using the already-downloaded"));
    CHECK(!json_at(r.import_json).at("source").contains("finalUrl"));
    CHECK_EQ(json_at(r.import_json).at("source").get_string("url"), ad10_copy.url);

    // ... from whichever copy it came: the second Simpsons ZIP alone on disk.
    fs::path dl2 = dir / L"downloads-second-copy";
    test::write_bytes(dl2 / L"simpsons-1994.zip", simp_zip94);
    srv.clear();
    r = run("simpsons, second copy on disk", download(dl2, "simpsons"), opts_for(dir / L"second-copy", reg), Status::ok);
    CHECK(srv.requests().empty());
    CHECK_EQ(r.url, simp94_copy.url);
    check_installed(dir / L"second-copy" / L"win", simpsons, "packages/simpsons");

    // A file of the wrong size under the first copy's name is fetched again,
    // not hashed (and not preferred over the network).
    fs::path dl3 = dir / L"downloads-stale";
    test::write_bytes(dl3 / L"SIMPSONS.zip", std::vector<uint8_t>(simp_zip.begin(), simp_zip.begin() + 1000));
    srv.clear();
    g_log.clear();
    r = run("simpsons, stale file", download(dl3, "simpsons"), opts_for(dir / L"stale", reg), Status::ok);
    CHECK(logged("is 1000 bytes, expected"));
    CHECK(test::read_bytes(dl3 / L"SIMPSONS.zip") == simp_zip);
  }

  // ---- a copy of another size: refused before a byte is written; the next
  // copy resumes the .part an earlier transfer left ------------------------------------------
  {
    reg.downloads("tt", {{srv.url("/r/tt-short.iso"), L"TTW320CD.ISO", tt_iso.size(), md5_of(tt_iso)}, tt_copy});
    fs::path dl4 = dir / L"downloads-resume";
    test::write_bytes(dl4 / L"TTW320CD.ISO.part", std::vector<uint8_t>(tt_iso.begin(), tt_iso.begin() + 5000));
    srv.clear();
    g_log.clear();
    ImportResult r = run("tt, first copy the wrong size", download(dl4, "tt"), opts_for(dir / L"resume", reg), Status::ok);
    CHECK_EQ(r.url, tt_copy.url);
    CHECK(logged("bytes, not the published " + std::to_string(tt_iso.size())));
    bool resumed = false;
    for (auto& q : srv.requests()) resumed = resumed || (q.path == "/tt.iso" && q.range && *q.range == 5000);
    CHECK(resumed);
    CHECK(test::read_bytes(dl4 / L"TTW320CD.ISO") == tt_iso);
    check_installed(dir / L"resume" / L"win", tt, "packages/tt");
  }

  // ---- a copy with other bytes: refused by md5, deleted, the next copy used ----------------
  {
    reg.downloads("ad32", {ad32_bad_copy, ad32_copy});
    fs::path dl5 = dir / L"downloads-md5";
    g_log.clear();
    ImportResult r = run("ad32, first copy damaged", download(dl5, "ad32"), opts_for(dir / L"md5", reg), Status::ok);
    CHECK_EQ(r.url, ad32_copy.url);
    CHECK(logged("downloaded file has md5 " + md5_of(ad32_bad)));
    CHECK_EQ(md5_file_hex(dl5 / L"ad32.iso"), md5_of(ad32_iso));
  }

  // ---- every copy fails: 3 for a wrong file, 4 for none reachable; nothing is left ---------
  {
    fs::path dl6 = dir / L"downloads-fail", root = dir / L"fail";
    reg.downloads("ad32", {ad32_bad_copy});
    ImportResult r = run("ad32, the only copy damaged", download(dl6, "ad32"), opts_for(root, reg), Status::verify_failed);
    CHECK(r.message.find("--image or --from") != std::string::npos);
    CHECK(!fs::exists(dl6 / L"ad32.iso") && !fs::exists(dl6 / L"ad32.iso.part"));
    CHECK(!fs::exists(root / L"win" / L"packages" / L"ad32"));

    reg.downloads("ad32", {ad32_gone, {srv.url("/r/gone-too.iso"), L"ad32.iso", ad32_iso.size(), md5_of(ad32_iso)}});
    r = run("ad32, every copy gone", download(dl6, "ad32"), opts_for(root, reg), Status::network);
    CHECK(r.message.find("none of the 2 copies") != std::string::npos);
    CHECK(r.message.find("After Dark 3.2") != std::string::npos);

    reg.downloads("ad32", {ad32_bad_copy, ad32_gone});
    run("ad32, one copy wrong, one gone", download(dl6, "ad32"), opts_for(root, reg), Status::verify_failed);
    CHECK(!fs::exists(root / L"win" / L"packages" / L"ad32"));
    CHECK(!fs::exists(dl6 / L"ad32.iso"));

    // A package with no known copy.
    reg.downloads("ad32", {});
    r = run("ad32, no copy known", download(dl6, "ad32"), opts_for(root, reg), Status::error);
    CHECK(r.message.find("no Internet Archive copy of After Dark 3.2") != std::string::npos);
    good_registry();
  }

  // ---- a custom URL ------------------------------------------------------------------------
  {
    fs::path dl7 = dir / L"downloads-custom";
    ImportResult r = run("--url --package tt", download(dl7, "tt", srv.url("/r/tt.iso"), md5_of(tt_iso)),
                         opts_for(dir / L"custom-tt", reg), Status::ok);
    CHECK_EQ(r.url, srv.url("/r/tt.iso"));
    CHECK(fs::exists(dl7 / L"tt.iso"));  // the URL's own name
    CHECK_EQ(r.verified, std::string("image"));
    r = run("--url, no package", download(dl7, "", srv.url("/SIMPSONS.zip")), opts_for(dir / L"custom-any", reg),
            Status::ok);
    CHECK_EQ(r.package_id, std::string("simpsons"));
    CHECK(!r.download_md5_checked);
    CHECK_EQ(json_at(r.import_json).at("source").get_bool("md5Checked"), false);
    r = run("--url, wrong --md5", download(dl7, "tt", srv.url("/tt-short.iso"), md5_of(tt_iso)),
            opts_for(dir / L"custom-bad", reg), Status::verify_failed);
    // --md5 without --url replaces the registry's (and its size).
    r = run("--md5 over the registry", download(dir / L"downloads-md5-override", "tt", "", md5_of(tt_iso)),
            opts_for(dir / L"custom-md5", reg), Status::ok);
    CHECK_EQ(r.url, tt_copy.url);
    // Over a floppy set's copy it replaces the first image's md5 (and size)
    // only: the second image is still checked against its own.
    r = run("--md5 over a floppy set", download(dir / L"downloads-st-md5", "startrek", "", md5_of(st1)),
            opts_for(dir / L"custom-st-md5", reg), Status::ok);
    CHECK_EQ(r.url, st_copy.url);
    CHECK_EQ(r.verified, std::string("image"));
    CHECK(r.download_md5_checked);
    {
      phosg::JSON j = json_at(r.import_json);
      const phosg::JSON& s = j.at("source");
      CHECK_EQ(s.get_bool("md5Checked"), true);
      const auto& parts = s.at("parts").as_list();
      CHECK(parts.size() == 2 && parts[0]->get_string("md5") == md5_of(st1) &&
            parts[1]->get_string("md5") == md5_of(st2));
    }
    check_installed(dir / L"custom-st-md5" / L"win", startrek, "packages/startrek");
    // The other copy's disk 1 named by --md5: the first copy's first image is
    // then refused, and the second copy used (its second image still checked
    // against its own md5).
    g_log.clear();
    r = run("--md5 of the other copy's disk 1", download(dir / L"downloads-st-md5b", "startrek", "", md5_of(st1b)),
            opts_for(dir / L"custom-st-md5b", reg), Status::ok);
    CHECK_EQ(r.url, st_copy_b.url);
    CHECK(logged("trying another copy: " + st_copy_b.url));
    CHECK_EQ(r.verified, std::string("image"));
    {
      phosg::JSON j = json_at(r.import_json);
      const auto& parts = j.at("source").at("parts").as_list();
      CHECK(parts.size() == 2 && parts[0]->get_string("md5") == md5_of(st1b) &&
            parts[1]->get_string("md5") == md5_of(st2b));
    }
    // ...and its size: with the first image published 512 bytes longer than
    // the server's, the copy is refused as served, and --md5 takes it.
    {
      Copy resized = st_copy;
      resized.size += 512;
      reg.downloads("startrek", {resized});
      r = run("a floppy set's first image, another size published", download(dir / L"downloads-st-size", "startrek"),
              opts_for(dir / L"custom-st-size", reg), Status::verify_failed);
      r = run("--md5 over that size", download(dir / L"downloads-st-size2", "startrek", "", md5_of(st1)),
              opts_for(dir / L"custom-st-size2", reg), Status::ok);
      CHECK_EQ(r.verified, std::string("image"));
      good_registry();
    }
  }

  // ---- ZIPs given as images ----------------------------------------------------------------
  {
    ImportResult r = run("the Simpsons ZIP as --image", image(dl / L"SIMPSONS.zip"), opts_for(dir / L"zip-image", reg),
                         Status::ok);
    CHECK_EQ(r.verified, std::string("files"));
    phosg::JSON j = json_at(r.import_json);
    CHECK_EQ(j.at("source").get_string("kind"), std::string("zip"));
    CHECK_EQ(j.at("source").get_string("format"), std::string("zip"));
    CHECK(!j.at("source").contains("url"));
    check_installed(dir / L"zip-image" / L"win", simpsons, "packages/simpsons");

    test::write_bytes(dir / L"nested.zip", flat_zip(simpsons.source, 0x1EF1, "SIMPSONS/"));
    r = run("a ZIP with the files in a folder", image(dir / L"nested.zip"), opts_for(dir / L"zip-nested", reg),
            Status::source_invalid);
    CHECK(r.message.find("at its root") != std::string::npos);
    test::write_bytes(dir / L"locked.zip", test::zip_of({{"INSTALL.INS", test::install_ins("")}}));
    r = run("a password-protected ZIP", image(dir / L"locked.zip"), opts_for(dir / L"zip-locked", reg),
            Status::source_invalid);
    CHECK(r.message.find("password-protected") != std::string::npos);
    CHECK(!fs::exists(dir / L"zip-nested" / L"win" / L"packages"));
  }

  // ---- --download all -----------------------------------------------------------------------
  {
    const std::vector<std::string> ids = downloadable_packages(reg.span());
    CHECK(ids == std::vector<std::string>({"deluxe", "ad10", "ad32", "tt", "simpsons", "swse", "startrek", "marvel",
                                           "snoopy", "looney", "screams", "disney", "farside", "dilbert"}));
    fs::path root = dir / L"all";
    std::vector<size_t> started;
    Source base = download(dir / L"downloads-all", "");
    std::vector<ImportResult> rs = import_downloads(ids, base, opts_for(root, reg), [&](size_t i) { started.push_back(i); });
    CHECK_EQ(rs.size(), size_t(14));
    for (auto& r : rs) CHECK_EQ(r.status, Status::ok);
    CHECK(started == std::vector<size_t>({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13}));
    std::vector<std::string> want;
    for (const test::PkgFixture* f : std::vector<const test::PkgFixture*>{&deluxe, &ad10, &ad32, &tt, &simpsons, &swse,
                                                                         &startrek, &marvel, &snoopy, &looney, &screams,
                                                                         &disney, &farside, &dilbert})
      want.insert(want.end(), f->ids.begin(), f->ids.end());
    CHECK(catalog_ids(root / L"win") == want);
    CHECK_EQ(rs.back().installed.size(), size_t(14));
    CHECK_EQ(rs.back().verified, std::string("image"));

    // Cancelled during the second: the first stays imported, nothing after it starts.
    fs::path croot = dir / L"all-cancel";
    ImportOptions o = opts_for(croot, reg);
    o.progress = [](const Progress& p) { return p.package != "After Dark 10th Anniversary"; };
    rs = import_downloads(ids, download(dir / L"downloads-cancel", ""), o);
    CHECK_EQ(rs.size(), size_t(2));
    if (rs.size() == 2) {
      CHECK_EQ(rs[0].status, Status::ok);
      CHECK_EQ(rs[1].status, Status::cancelled);
    }
    CHECK(fs::exists(croot / L"win" / L"FILES"));
    CHECK(!fs::exists(croot / L"win" / L"packages" / L"ad10"));
  }

  // ---- the built-in copies -------------------------------------------------------------------
  {
    CHECK(downloadable_packages() == std::vector<std::string>({"deluxe", "ad10", "ad32", "tt", "simpsons", "swse",
                                                                "startrek", "marvel", "snoopy", "looney", "screams",
                                                                "disney", "farside", "dilbert", "tng", "castaway"}));
    for (const Package& p : builtin_packages()) {
      CHECK(!p.downloads.empty());
      std::map<std::string, std::wstring> name_of_md5;
      std::map<std::wstring, std::string> md5_of_name;
      for (const Download& d : p.downloads) {
        const std::string kind = d.kind;
        fprintf(stderr, "  %s: %s (%llu bytes, %s)\n", p.id, d.url, (unsigned long long)download_size(d), kind.c_str());
        CHECK(kind == "image" || kind == "zip");
        // Every file of the copy: the first, and the images (or a "zip"
        // copy's ZIPs: The Far Side's, Dilbert's second) of further disks.
        std::vector<DownloadPart> parts = {{d.url, d.file_name, d.size, d.md5}};
        parts.insert(parts.end(), d.more_images.begin(), d.more_images.end());
        std::vector<int> disks;
        uint64_t total = 0;
        for (const DownloadPart& q : parts) {
          const std::string url = q.url, md5 = q.md5;
          const std::wstring file = q.file_name;
          CHECK(url.rfind("https://archive.org/download/", 0) == 0);
          CHECK(url.find_first_of(" \"<>[]()") == std::string::npos);  // percent-encoded as published
          CHECK(is_md5(md5));
          CHECK(q.size > 0);
          CHECK(!file.empty() && file.find_first_of(L"<>:\"/\\|?*") == std::wstring::npos);
          // An image is one of the package's known images (verified: image);
          // a ZIP is too only when no image of the original media exists and
          // that ZIP is the release's known copy (Marvel Comics Screen
          // Posters' two, Snoopy's Screen Savers', the Looney Tunes',
          // ScreamSavers', the Disney Collection's), which says so in its
          // medium.
          const KnownImage* known = nullptr;
          for (const KnownImage& k : p.images)
            if (md5 == k.md5 && q.size == k.size) known = &k;
          if (kind == "image" || !d.more_images.empty()) CHECK(known != nullptr);
          if (kind == "zip" && known) CHECK(std::string_view(known->medium).rfind("ZIP", 0) == 0);
          if (known && (kind == "image" || !d.more_images.empty())) disks.push_back(known->disk);
          // Same bytes, same name (a transfer resumes across copies); other bytes, another name.
          if (name_of_md5.count(md5)) CHECK(name_of_md5[md5] == file);
          if (md5_of_name.count(file)) CHECK_EQ(md5_of_name[file], md5);
          name_of_md5[md5] = file;
          md5_of_name[file] = md5;
          total += q.size;
        }
        CHECK_EQ(download_size(d), total);
        // An image copy is a whole release: one whole-release image, or every
        // install disk of the package exactly once (a copy of a ZIP per disk too).
        if (kind == "image" || !d.more_images.empty()) {
          std::sort(disks.begin(), disks.end());
          int n = 0;
          for (const KnownImage& k : p.images) n = std::max(n, k.disk);
          std::vector<int> want(size_t(n ? n : 1), 0);
          for (int i = 0; i < n; i++) want[size_t(i)] = i + 1;
          CHECK(disks == want);
        }
      }
    }
    // The Simpsons ZIPs checked on 2026-09-26 (research/win/pkg/sources).
    const Package* s = find_package("simpsons");
    CHECK(s && s->downloads.size() == 2);
    if (s && s->downloads.size() == 2) {
      CHECK_EQ(std::string(s->downloads[0].md5), std::string("90a85bf64c971fe65045f1afc6db0eba"));
      CHECK_EQ(s->downloads[0].size, uint64_t(2752575));
      CHECK_EQ(std::string(s->downloads[1].md5), std::string("1d6083344b16f09e61acf016eca25026"));
      CHECK_EQ(s->downloads[1].size, uint64_t(2752010));
    }
    CHECK_EQ(find_package("ad32")->downloads.size(), size_t(3));
    CHECK_EQ(std::string(find_package("deluxe")->downloads[0].url), std::string(kDeluxeIsoUrl));
    // Star Wars Screen Entertainment (verified 2026-09-28, research/win/pkg/swse):
    // the exact ISO, the Redump BIN of the same pressing (both known images),
    // then a flat ZIP of the disc's files.
    const Package* sw = find_package("swse");
    CHECK(sw && sw->downloads.size() == 3 && sw->images.size() == 2);
    if (sw && sw->downloads.size() == 3 && sw->images.size() == 2) {
      CHECK_EQ(std::string(sw->downloads[0].md5), std::string("bfa63c1bce15dcbea965dfd7c2ed44e8"));
      CHECK_EQ(sw->downloads[0].size, uint64_t(7227392));
      CHECK_EQ(std::string(sw->downloads[1].md5), std::string("ce51614a3484b9269b5ed9e61510e971"));
      CHECK_EQ(sw->downloads[1].size, uint64_t(9005808));
      CHECK_EQ(std::string(sw->downloads[2].kind), std::string("zip"));
      CHECK_EQ(std::string(sw->downloads[2].md5), std::string("c7a4c5322a784a3e88964c5a7f499226"));
      CHECK_EQ(sw->downloads[2].size, uint64_t(7013137));
      CHECK_EQ(std::string(sw->images[0].volume_id), std::string("SWSE"));
      // Not "floppy": the GUI says "disc" for it (the medium is a CD).
      CHECK(std::string_view(sw->images[0].medium).find("floppy") == std::string_view::npos);
    }
    // Star Trek: The Screen Saver (verified 2026-09-29, research/win/pkg/
    // startrek/importer/sources.json): the two images of item
    // afterdark-20b_startrek, then those of item
    // startrektosscreensaver1992win (the same disks as a Windows 9x copy
    // wrote to them); the four known disk images; "floppy" media, so the GUI
    // says "disks".
    const Package* st = find_package("startrek");
    CHECK(st && st->downloads.size() == 2 && st->images.size() == 4);
    if (st && st->downloads.size() == 2 && st->images.size() == 4) {
      const Download &a = st->downloads[0], &b = st->downloads[1];
      CHECK_EQ(std::string(a.url),
               std::string("https://archive.org/download/afterdark-20b_startrek/afterdark-20b_startrek_disk1.img"));
      CHECK_EQ(std::string(a.md5), std::string("28e33608b8d3bafa28585472c4a7a9ac"));
      CHECK(a.more_images.size() == 1 && std::string(a.more_images[0].md5) == "c630da5f6839303b599947f56fdd7c25");
      CHECK_EQ(std::string(b.url), std::string("https://archive.org/download/startrektosscreensaver1992win/startrek1.img"));
      CHECK_EQ(std::string(b.md5), std::string("6ee71b45e32b07001d46ab8c80af589d"));
      CHECK(b.more_images.size() == 1 && std::string(b.more_images[0].md5) == "af9d29a7ddea2c03618899c1c5733c67");
      CHECK_EQ(download_size(a), uint64_t(2 * 1474560));
      for (const KnownImage& k : st->images) {
        CHECK(k.size == 1474560 && (k.disk == 1 || k.disk == 2));
        CHECK(std::string_view(k.medium).find("floppy") != std::string_view::npos);
      }
    }
    // The Looney Tunes, ScreamSavers and the Disney Collection (verified
    // 2026-09-29, research/win/pkg/{looney,scream,disney}): each the one file
    // of item after-dark-collection, the user's copy byte for byte, a "zip"
    // copy whose md5 is the known image (so the GUI says "the known ZIP");
    // the Looney Tunes' CD is a known image with no copy listed (its only one
    // online is named after a product serial).
    struct Collection {
      const char* id;
      const char* url;
      const wchar_t* file;
      uint64_t size;
      const char* md5;
    };
    for (const Collection& c : std::vector<Collection>{
             {"looney", "https://archive.org/download/after-dark-collection/After%20Dark%20-%20Looney%20Tunes.zip",
              L"After Dark - Looney Tunes.zip", 2900525, "642b358a4854c481fe99984b8452ceb5"},
             {"screams", "https://archive.org/download/after-dark-collection/After%20Dark%20-%20Scream%20Savers.zip",
              L"After Dark - Scream Savers.zip", 3453163, "37a47b25dd35b214f94f57b6a0c2bd02"},
             {"disney", "https://archive.org/download/after-dark-collection/After%20Dark%20-%20Disney%20Collection.zip",
              L"After Dark - Disney Collection.zip", 3560012, "2f38df15494728b5bc20d26c36ba84c7"}}) {
      const Package* p = find_package(c.id);
      CHECK(p && p->downloads.size() == 1 && !p->images.empty());
      if (!p || p->downloads.size() != 1 || p->images.empty()) continue;
      const Download& d = p->downloads[0];
      CHECK(std::string(d.url) == c.url && std::wstring(d.file_name) == c.file && d.size == c.size &&
            std::string(d.md5) == c.md5 && std::string(d.kind) == "zip");
      CHECK(std::string(p->images[0].md5) == c.md5 && p->images[0].size == c.size);
      CHECK(std::string_view(p->images[0].medium).rfind("ZIP", 0) == 0);
    }
    const Package* lt = find_package("looney");
    CHECK(lt && lt->images.size() == 2 && std::string(lt->images[1].volume_id) == "LOONEY_T" &&
          std::string_view(lt->images[1].medium).find("floppy") == std::string_view::npos);
    // Marvel Comics Screen Posters (verified 2026-09-29,
    // research/win/pkg/marvel/ia/sources.json): the flat ZIP of item
    // afterdarkmarvelscreenposters first, then the after-dark-collection
    // copy, the user's file byte for byte; Snoopy's Screen Savers: the
    // after-dark-collection copy. Each ZIP is a known image; no URL of either
    // item's serial number file appears.
    struct Zip {
      const char* id;
      size_t index;
      const char* url;
      const wchar_t* file;
      uint64_t size;
      const char* md5;
    };
    for (const Zip& z : std::vector<Zip>{
             {"marvel", 0,
              "https://archive.org/download/afterdarkmarvelscreenposters/After%20Dark%20-%20Marvel%20Screen%20Posters.zip",
              L"After Dark - Marvel Screen Posters.zip", 2039771, "4c608dbbeb34108b30ede88304912c94"},
             {"marvel", 1, "https://archive.org/download/after-dark-collection/After%20Dark%20-%20Marvel%20Comics.zip",
              L"After Dark - Marvel Comics.zip", 2046286, "6981b36abb04779a076466fabad3721c"},
             {"snoopy", 0, "https://archive.org/download/after-dark-collection/After%20Dark%20-%20Snoopy.zip",
              L"After Dark - Snoopy.zip", 1993700, "a712447e1c957767bdbca884cead02dc"}}) {
      const Package* p = find_package(z.id);
      CHECK(p && z.index < p->downloads.size() && z.index < p->images.size());
      if (!p || z.index >= p->downloads.size() || z.index >= p->images.size()) continue;
      const Download& d = p->downloads[z.index];
      CHECK(std::string(d.url) == z.url && std::wstring(d.file_name) == z.file && d.size == z.size &&
            std::string(d.md5) == z.md5 && std::string(d.kind) == "zip");
      CHECK(std::string(p->images[z.index].md5) == z.md5 && p->images[z.index].size == z.size);
      CHECK(std::string_view(p->images[z.index].medium).rfind("ZIP", 0) == 0);
    }
    CHECK_EQ(find_package("marvel")->downloads.size(), size_t(2));
    CHECK_EQ(find_package("snoopy")->downloads.size(), size_t(1));
    for (const char* id : {"marvel", "snoopy"})
      for (const Download& d : find_package(id)->downloads)
        CHECK(ascii_upper(d.url).find("SERIAL") == std::string::npos);
  }

  // ---- adimport.exe ---------------------------------------------------------------------------
  {
    auto cli = [&](const std::vector<std::wstring>& args, const char* what, std::string* out = nullptr) {
      test::ProcessResult r = test::run_process(exe, args, 300000);
      fprintf(stderr, "[%s] exit %d\n%s", what, r.exit_code, r.output.c_str());
      if (out) *out = r.output;
      return r.exit_code;
    };
    std::string out;
    CHECK_EQ(cli({L"--help"}, "--help", &out), 0);
    CHECK(out.find("--download all") != std::string::npos);
    CHECK_EQ(cli({L"--download", L"all", L"--package", L"tt"}, "--download all --package"), 1);
    CHECK_EQ(cli({L"--download", L"all", L"--url", L"http://127.0.0.1:9/x.iso"}, "--download all --url"), 1);
    CHECK_EQ(cli({L"--download", L"nosuch"}, "--download <unknown id>"), 1);
    CHECK_EQ(cli({L"--download", L"ad32", L"--package", L"tt"}, "--download ad32 --package tt"), 1);
    CHECK_EQ(cli({L"--package", L"tt", L"--download", L"ad32"}, "--package tt --download ad32"), 1);
    CHECK_EQ(cli({L"--list-packages", L"--dest", (dir / L"cli-empty").wstring()}, "--list-packages", &out), 0);
    CHECK(out.find("download 2.6 MB (ZIP of the install files)") != std::string::npos);
    CHECK(out.find("download 381.7 MB (disc image)") != std::string::npos);
    CHECK(out.find("download 6.9 MB (disc image)") != std::string::npos);  // swse
    CHECK(out.find("  startrek  Star Trek: The Screen Saver                  "
                   "not installed; download 2.8 MB (2 floppy images)") != std::string::npos);
    CHECK(out.find("  looney    The Looney Tunes Screen Saver                "
                   "not installed; download 2.8 MB (ZIP of the install files)") != std::string::npos);
    CHECK(out.find("  screams   ScreamSavers                                 "
                   "not installed; download 3.3 MB (ZIP of the install files)") != std::string::npos);
    CHECK(out.find("  marvel    Marvel Comics Screen Posters                 "
                   "not installed; download 1.9 MB (ZIP of the install files)") != std::string::npos);
    CHECK(out.find("  snoopy    Snoopy's Screen Savers                       "
                   "not installed; download 1.9 MB (ZIP of the install files)") != std::string::npos);
    CHECK(out.find("  farside   The Far Side Screen Saver Collection         "
                   "not installed; download 5.5 MB (5 ZIPs of the install disks' files)") != std::string::npos);
    CHECK(out.find("  dilbert   Scott Adams' Dilbert Screen Saver Collection "
                   "not installed; download 4.3 MB (ZIP of the install files)") != std::string::npos);
    CHECK(!fs::exists(dir / L"cli-empty"));

    // The built-in manifests are the real ones: the synthetic files need --no-verify.
    fs::path cdl = dir / L"cli-downloads", croot = dir / L"cli";
    CHECK_EQ(
        cli({L"--no-cover-download", L"--download", L"tt", L"--url", to_wide(srv.url("/r/tt.iso")), L"--md5",
             to_wide(md5_of(tt_iso)), L"--download-dir", cdl.wstring(), L"--dest", croot.wstring(), L"--no-verify"},
            "--download tt --url", &out),
        0);
    CHECK(out.find("Totally Twisted") != std::string::npos);
    CHECK_EQ(json_at(croot / L"win" / L"packages" / L"tt" / L"import.json").at("source").get_string("kind"),
             std::string("download"));
    CHECK_EQ(cli({L"--no-cover-download", L"--download", L"simpsons", L"--url", to_wide(srv.url("/r/SIMPSONS.zip")),
                  L"--md5", to_wide(md5_of(simp_zip)), L"--download-dir", cdl.wstring(), L"--dest", croot.wstring(),
                  L"--no-verify", L"--quiet"},
                 "--download simpsons --url"),
             0);
    CHECK_EQ(json_at(croot / L"win" / L"packages" / L"simpsons" / L"import.json").at("source").get_string("format"),
             std::string("zip"));
    CHECK_EQ(
        cli({L"--no-cover-download", L"--download", L"--package", L"tt", L"--url", to_wide(srv.url("/r/tt-short.iso")),
             L"--md5", to_wide(md5_of(tt_iso)), L"--download-dir", cdl.wstring(), L"--dest", croot.wstring()},
            "--download --package tt, wrong file"),
        3);
    CHECK_EQ(cli({L"--no-cover-download", L"--image", (cdl / L"SIMPSONS.zip").wstring(), L"--dest",
                  (dir / L"cli-zip").wstring(), L"--no-verify", L"--quiet"},
                 "--image <zip>"),
             0);
  }
  return test::finish("import.pkg_download");
}
