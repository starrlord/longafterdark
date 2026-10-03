// The real package sources end to end (opt-in: AD_E2E_PKG=1; exit 77 =
// skipped). PACKAGES.md §9.
//
//   test_import_pkg_real <adimport.exe> <scratch> [<image dir>]
//
// The images are looked for in AD_SOURCE_ISO_DIR (a ';'-separated list of
// folders), else the third argument (<repo>/source_iso), and in the folders
// directly in each (test_util.h image_dirs: <repo>/source_iso/Implemented
// too); they are identified by size and md5, never by file name. Star Trek:
// The Screen Saver's two install disks are two loose images, or the ZIP they
// came in (test_util.h find_disk_set); Marvel Comics Screen Posters, Snoopy's
// Screen Savers, the Looney Tunes, ScreamSavers and the Disney Collection are
// the user's ZIPs of their install files, their known images; The Far Side
// the five ZIPs of its disks' files (PNX-FSC1..5.ZIP), Dilbert the ZIPs of
// its four disks' files or the flat ZIP of them all (DilbertS.zip); Johnny
// Castaway its floppy's image, the KryoFlux dump's ZIP of it or the user's
// 7z of it (000580_jonny_castaway.7z); the Opus 'n Bill Screen Saver the
// ZIPs of its three disks' files (OPUS1..3NTA.ZIP, or another BBS's), On the
// Road Again the Internet Archive's ZIP of its files, The Flintstones the
// ZIPs of its June build's disks (FLINTST1..3.ZIP), Intermission 4.0 its three
// floppy images or the Internet Archive's ZIP of them (research/five/dl: add
// it to AD_SOURCE_ISO_DIR).
//   1. Each package's image(s) into a fresh root through adimport.exe: exit
//      0, verified "image", nothing missing, every file matches the package
//      manifest, the installed files are exactly the manifest (the §4 layout
//      and counts), the invariants hold (the intermission, ad2kwaj, islib
//      and is1 recipes' own), and the catalog has 46 / 44 / 13 / 15 / 14 /
//      16 / 1 / 8 / 12 / 15 / 16 / 14 / 16 / 13 / 1 / 16 / 16 / 15 / 54 modules in the right
//      lanes (castaway: one Windows 3.1 screen-saver program; swse:
//      Intermission IMX entries with their registry names and one button;
//      farside, dilbert: Intermission ASA and IMQ entries, SAVERMAIN, the
//      same; intermission: the same, with the Speed control after the
//      button on the registry's speed modules alone, each at its starting
//      stop; startrek: Classic
//      entries with their fixed screen, trimmed names, the Planetary Atlas
//      override and After Dark 2.0's About rules; marvel: its fixed screen
//      and two buttons; snoopy: eight Classic entries, 27 controls; screams:
//      the fixed screen; disney: its five names spelt out), and no owner's
//      note is named.
//   1b. The 10th Anniversary, Totally Twisted and Star Wars Screen
//      Entertainment CDs mounted by Windows, imported --from the drive: the
//      same files as from the image (skipped when mounting is refused, or for
//      an image Windows cannot mount).
//   1c. Star Wars Screen Entertainment's INSTALL.DAT, read from its image: the
//      registry's loose files agree with the installer's lines, and every
//      other line the installer could run is an archive or a file the recipe
//      never reads (I5).
//   1d. Star Trek: The Screen Saver's ST_NSTLL.IN_, KWAJ-expanded from disk
//      1: every row of the registry's table is an INF line with the same
//      disk, installed name and size, placed by its section; the disk tag
//      files are the INF's; every other line is a file the recipe never reads.
//   1e. Its two disks in a ZIP under code page 437 names (bit 11 clear, as
//      Explorer or 7-Zip on an English Windows writes a name that fits it),
//      twice: the known disk set, verified "image", the files of step 1, and
//      an import.json that is UTF-8 with the names decoded, also when the
//      names differ only in a letter outside ASCII.
//   1f. The Looney Tunes' CD (LOONEY_T, known by md5, size and volume id),
//      when an image folder holds it: verified "image", the ZIP's files.
//   1g. Marvel Comics Screen Posters' and Snoopy's Screen Savers' baked
//      tables against their disks, read from the user's ZIPs: each row's
//      member is in its library with the manifest's size; every member the
//      table leaves is one the recipe never installs; SETUP.PKG (parsed here,
//      on its own) lists each library the table reads with exactly its
//      members and sizes, the tag member in the tag library.
//   1h. Their disks copied out of the user's ZIPs (the previous owners'
//      notes skipped by name, never read): a flat folder, DISK1/DISK2
//      folders and a flat ZIP verify "files" with the ZIP's files; disk 1
//      alone needs every install disk (2); disk 2 alone is no release (2).
//   2. Every package plus Deluxe (imported --from the installed assets, which
//      are only read: AD_ASSETS_DIR picks them, e.g.
//      <repo>/build/win-pkg-setup/assets, else the data folder's) in one root:
//      429 modules (232 over the first seven), display names unique per lane,
//      sameAs consistent (73), and every Deluxe entry's existing fields equal
//      to the installed catalog's.
//   3. Re-importing each package into that root changes nothing else.
// Nothing is written outside <scratch>.
#include <phosg/JSON.hh>

#include <map>
#include <set>
#include <tuple>

#include "catalog.h"
#include "importer.h"
#include "isz.h"
#include "kwaj.h"
#include "md5.h"
#include "names.h"
#include "run_process.h"
#include "source.h"
#include "test_util.h"
#include "zip.h"
#include "zip_builder.h"

using namespace adw::import;
namespace fs = std::filesystem;

namespace {

using Tree = std::map<std::string, std::vector<uint8_t>>;

int adimport(const std::wstring& exe, const std::vector<std::wstring>& args, const char* what) {
  fprintf(stderr, "---- %s\n", what);
  ULONGLONG t0 = GetTickCount64();
  test::ProcessResult pr = test::run_process(exe, args, 1800000);
  fprintf(stderr, "%s", pr.output.c_str());
  fprintf(stderr, "adimport exited %d after %.1f s\n", pr.exit_code, (GetTickCount64() - t0) / 1000.0);
  return pr.exit_code;
}

phosg::JSON load(const fs::path& p) { return phosg::JSON::parse(test::read_text(p)); }

struct Expect {
  size_t files;                            // installed files, import.json aside (§4.3)
  std::map<std::string, size_t> per_dir;   // top-level dir -> files under it
  size_t modules, pe32;
};

const std::map<std::string, Expect> kExpect = {
    {"ad10", {147, {{"AD10TH", 127}, {"ENGINE", 13}, {"AFI", 7}}, 46, 17}},
    {"ad32", {89, {{"AD32", 84}, {"ENGINE", 5}}, 44, 0}},
    {"tt", {26, {{"TWISTED", 21}, {"ENGINE", 5}}, 13, 0}},
    {"simpsons", {30, {{"SIMPSONS", 25}, {"ENGINE", 5}}, 15, 0}},
    {"swse", {29, {{"SAVER", 26}, {"ENGINE", 2}, {"WINDOWS", 1}}, 14, 0}},
    {"startrek", {27, {{"AFTERDRK", 25}, {"ENGINE", 2}}, 16, 0}},
    {"marvel", {64, {{"AFTERDRK", 62}, {"ENGINE", 2}}, 1, 0}},
    {"snoopy", {8, {{"AFTERDRK", 8}}, 8, 0}},
    {"looney", {34, {{"LNYTUNES", 29}, {"ENGINE", 5}}, 12, 0}},
    {"screams", {23, {{"SCREAMS", 18}, {"ENGINE", 5}}, 15, 0}},
    {"disney", {31, {{"DISNEY", 26}, {"ENGINE", 5}}, 16, 0}},
    {"farside", {20, {{"SAVER", 18}, {"ENGINE", 2}}, 14, 0}},
    {"dilbert", {23, {{"SAVER", 21}, {"ENGINE", 2}}, 16, 0}},
    {"tng", {31, {{"ST-TNG", 26}, {"ENGINE", 5}}, 13, 0}},
    {"castaway", {3, {{"SCRANTIC", 3}}, 1, 0}},
    {"opus", {21, {{"SAVER", 19}, {"ENGINE", 2}}, 16, 0}},
    {"opusroad", {23, {{"SAVER", 20}, {"ENGINE", 3}}, 16, 0}},
    {"flintstones", {22, {{"SAVER", 19}, {"ENGINE", 3}}, 15, 0}},
    {"intermission", {73, {{"SAVER", 67}, {"ENGINE", 6}}, 54, 0}},
};
// The catalog over every release with the Deluxe tree: 232 entries over the
// first seven, 284 with Marvel Comics Screen Posters, Snoopy's Screen Savers,
// the Looney Tunes, ScreamSavers and the Disney Collection, 314 with The Far
// Side and Dilbert, 327 with Star Trek: The Next Generation Screen Saver,
// 328 with Johnny Castaway, 429 with the Opus 'n Bill Screen Saver, On the
// Road Again, The Flintstones and Intermission 4.0; still 73 of them the
// same bytes as an earlier entry.
size_t combined_modules() {
  size_t n = 84;
  for (const auto& [id, e] : kExpect) n += e.modules;
  return n;
}

Tree snapshot(const fs::path& dir, bool without_record = false) {
  Tree t;
  for (const std::string& rel : test::list_tree(dir))
    if (!(without_record && rel == "import.json")) t[rel] = test::read_bytes(dir / to_wide(rel));
  return t;
}

fs::path root_of(const fs::path& win, const Package& p) {
  fs::path root = win;
  for (std::string_view r = p.root; !r.empty();) {
    size_t j = r.find('/');
    root /= to_wide(r.substr(0, j));
    r = j == std::string_view::npos ? std::string_view() : r.substr(j + 1);
  }
  return root;
}

void check_package_root(const fs::path& win, const Package& p) {
  const fs::path root = root_of(win, p);
  const Expect& e = kExpect.at(p.id);
  // The import record.
  phosg::JSON j = load(root / L"import.json");
  CHECK_EQ(j.get_int("version"), int64_t(2));
  CHECK_EQ(j.get_string("verified"), std::string("image"));
  CHECK(j.at("missingKnown").as_list().empty());
  CHECK_EQ(size_t(j.get_int("fileCount")), e.files);
  CHECK_EQ(j.at("source").get_bool("imageMd5Known"), true);
  CHECK_EQ(j.at("package").get_string("recipe"), std::string(recipe_name(p.recipe)));
  for (auto& f : j.at("files").as_list()) CHECK_EQ(f->get_string("known"), std::string("match"));
  // Installed files == the manifest, byte for byte (by md5).
  std::map<std::string, const KnownFile*> manifest;
  for (const KnownFile& k : p.manifest) manifest[k.path] = &k;
  CHECK_EQ(manifest.size(), e.files);
  std::map<std::string, size_t> per_dir;
  size_t seen = 0;
  for (const std::string& rel : test::list_tree(root)) {
    if (rel == "import.json") continue;
    seen++;
    per_dir[rel.substr(0, rel.find('/'))]++;
    std::string key = std::string(p.root) + "/" + rel;
    auto it = manifest.find(key);
    if (it == manifest.end()) {
      test::g_failures++;
      fprintf(stderr, "  not in the manifest: %s\n", key.c_str());
      continue;
    }
    CHECK_EQ(md5_file_hex(root / to_wide(rel)), std::string(it->second->md5));
  }
  CHECK_EQ(seen, e.files);
  CHECK(per_dir == e.per_dir);
  // §4.2: I1 and I3 (the importer checked all of them; these are cheap to
  // re-check), the intermission, ad2kwaj and islib recipes' own for their
  // packages.
  const bool imx = p.recipe == Recipe::intermission, ad2 = p.recipe == Recipe::ad2kwaj,
             isl = p.recipe == Recipe::islib, is1 = p.recipe == Recipe::is1, delrina = p.delrina_installer();
  for (const char* dir : p.module_dirs) {
    if (std::string_view(dir) == "ENGINE") continue;
    for (const char* never : {"AD_SND.DLL", "OLDMOD16.DLL", "OLDMOD32.DLL", "ADTASK.DLL", "ADW30.EXE"})
      CHECK(!fs::exists(root / to_wide(dir) / to_wide(never)));
    if (imx) {
      // Intermission and its readers in ENGINE; a Delrina release's IMQ
      // modules (each its own reader) beside its ASA animations.
      CHECK(!fs::exists(root / to_wide(dir) / L"INTERMIS.EXE"));
      std::error_code ec;
      for (auto& f : fs::directory_iterator(root / to_wide(dir), ec)) {
        const std::string n = to_utf8(f.path().filename().wstring());
        CHECK(!ends_with_i(n, ".IMQ") || (delrina && !is_intermission_reader(n)) ||
              (std::string_view(p.id) == "intermission" && n == "IMIMXPLY.IMQ"));
        CHECK(!ends_with_i(n, ".ASA") || delrina);
      }
    }
    if (ad2) {
      CHECK(!fs::exists(root / to_wide(dir) / L"AD.EXE"));
      // The sound database where After Dark 2.0's AD_MOD opens it.
      CHECK(fs::exists(root / to_wide(dir) / L"ST_RES" / L"ST_SND.DLL"));
    }
    if (isl) CHECK(!fs::exists(root / to_wide(dir) / L"AD.EXE"));
  }
  if (delrina) {
    // The ASA reader and Intermission in ENGINE, nothing else; no WINDOWS
    // folder (the modules read ANTSW.INI, which the lane seeds).
    CHECK(fs::exists(root / L"ENGINE" / L"IMASAPLY.IMQ") && fs::exists(root / L"ENGINE" / L"INTERMIS.EXE"));
    // The IMX reader in ENGINE where the release has IMX modules.
    bool imx_modules = false;
    for (const LooseFile& lf : p.loose_files) imx_modules = imx_modules || ends_with_i(lf.to, ".IMX");
    CHECK_EQ(fs::exists(root / L"ENGINE" / L"IMIMXPLY.IMQ"), imx_modules);
    CHECK(!fs::exists(root / L"WINDOWS"));
    for (const char* n : {"OLDMOD16.DLL", "ADTASK.DLL", "AD_SND.DLL"}) CHECK(!fs::exists(root / L"ENGINE" / to_wide(n)));
  } else if (imx) {
    CHECK(fs::exists(root / L"ENGINE" / L"IMIMXPLY.IMQ"));
    CHECK(fs::exists(root / L"WINDOWS" / L"SWSE.INI"));
    for (const char* n : {"OLDMOD16.DLL", "ADTASK.DLL", "AD_SND.DLL"}) CHECK(!fs::exists(root / L"ENGINE" / to_wide(n)));
  } else if (ad2) {
    CHECK(fs::exists(root / L"ENGINE" / L"AD_SND.DLL"));
    for (const char* n : {"OLDMOD16.DLL", "ADTASK.DLL", "AFTERDAR.SCR"}) CHECK(!fs::exists(root / L"ENGINE" / to_wide(n)));
    CHECK(!fs::exists(root / L"WINDOWS"));  // the lane's seeds are its settings
  } else if (isl) {
    // Marvel's AD_SND 1.0 and pristine AD.EXE in ENGINE; Snoopy's Screen
    // Savers ship no engine at all; neither has a WINDOWS folder.
    if (std::string_view(p.id) == "marvel") {
      CHECK(fs::exists(root / L"ENGINE" / L"AD_SND.DLL") && fs::exists(root / L"ENGINE" / L"AD.EXE"));
      for (const char* n : {"OLDMOD16.DLL", "ADTASK.DLL", "AFTERDAR.SCR"})
        CHECK(!fs::exists(root / L"ENGINE" / to_wide(n)));
    } else {
      CHECK(!fs::exists(root / L"ENGINE"));
    }
    CHECK(!fs::exists(root / L"WINDOWS"));
  } else if (is1) {
    // The program and its data in its folder alone: no ENGINE, no WINDOWS.
    CHECK(!fs::exists(root / L"ENGINE") && !fs::exists(root / L"WINDOWS"));
  } else {
    CHECK(fs::exists(root / L"ENGINE" / L"AD_SND.DLL"));
    CHECK((fs::exists(root / L"ENGINE" / L"OLDMOD16.DLL") && fs::exists(root / L"ENGINE" / L"AFTERDAR.SCR")) ||
          fs::exists(root / L"ENGINE" / L"ADTASK.DLL"));
  }
  std::string text = test::read_text(root / L"import.json");
  CHECK(text.find("SERIAL") == std::string::npos && text.find("CEREAL") == std::string::npos);
  CHECK(text.find("README.TXT") == std::string::npos && text.find("WING.DL_") == std::string::npos);
  // The owners' notes in the Looney Tunes', ScreamSavers', the Disney
  // Collection's, Marvel Comics Screen Posters' and Snoopy's Screen Savers'
  // copies, the archive the Disney Collection never opens, and what the
  // islib recipe never reads (the installers' scripts, the libraries its
  // table does not read, a disk copier's leftovers).
  const std::string upper = ascii_upper(text);
  for (const char* never : {"_SN.TXT", "REG'D", "DIZNY", "BEAUTYOL", "SERIAL", "REG#", "ADNEWS", "INSTALL.INS",
                            "SETUP.INS", "WINSYS", "~INS0762", "CMOS.RAM", "DREAM.ON", "TXTSCR", "AD_CHANGES",
                            "AD_MODS.LIS"})
    CHECK(upper.find(never) == std::string::npos);
  // What InstallShield 1's floppy holds that the recipe never reads: the
  // installer, its logos, and the placeholder RESOURCE.001 (the installed
  // one comes from RESOURCE.00$).
  if (is1) {
    for (const char* never : {"SETUP.EXE", "INSTALL.EX$", "LOGO.BMP"}) CHECK(upper.find(never) == std::string::npos);
    CHECK(text.find("\"from\": \"RESOURCE.001\"") == std::string::npos);
  }
  if (ad2)  // what After Dark 2.0's installer copied that the recipe never reads
    for (const char* never : {"AD_PREFS", "AD_MPT", "SPALETTE", "AD_LIB", "AD_SB", "ST_NSTLL", "AD_MESG", "SETUP.EXE"})
      CHECK(text.find(never) == std::string::npos);
  // A BBS's notes beside the disks' files, and what Delrina's installer
  // copied that the recipe never reads.
  if (delrina)
    for (const char* never : {".NFO", "FILE_ID", "UNZIP_ME", "README", "PACKING.LST", "IMINST", "SETUP.EXE", "AD_SND",
                              "IWLIB", "ICONDLL", "ANTSW2", "LASTDISK", "INTERMIS.TXT", "DONTREAD", "SEEME", "ROI!",
                              "IMAD_PLY", "IMFLCPLY", "IMSEQPLY", "CURTCALL", "SAVERDEV", "WEED"})
      CHECK(upper.find(never) == std::string::npos);
}

void check_catalog_of(const phosg::JSON& cat, const Package& p) {
  const Expect& e = kExpect.at(p.id);
  const bool imx = p.recipe == Recipe::intermission, ad2 = p.recipe == Recipe::ad2kwaj,
             isl = p.recipe == Recipe::islib, is1 = p.recipe == Recipe::is1;
  size_t n = 0, pe = 0, controls = 0, speeds = 0;
  for (auto& m : cat.at("modules").as_list()) {
    if (m->get_string("package") != p.id) continue;
    n++;
    pe += m->get_string("lane") == "pe32";
    CHECK(m->get_string("id").rfind(std::string(p.id) + ".", 0) == 0);
    CHECK(m->get_string("path").rfind(std::string(p.root) + "/", 0) == 0);
    CHECK_EQ(m->contains("abi"), imx || is1);
    if (is1) {
      // Johnny Castaway: a Windows 3.1 screen-saver program (its
      // description names it Screen Antics; the registry, Johnny
      // Castaway), with Control Panel's Setup... (its own dialog).
      CHECK_EQ(m->get_string("abi"), std::string("scrnsave"));
      CHECK_EQ(m->get_string("lane"), std::string("ne16"));
      CHECK_EQ(m->get_string("entry"), std::string("SCREENSAVERPROC"));
      CHECK_EQ(m->get_string("id"), std::string("castaway.scrantic"));
      CHECK_EQ(m->get_string("moduleName"), std::string("Johnny Castaway"));
      CHECK_EQ(m->get_string("displayName"), std::string("Johnny Castaway"));
      CHECK(m->at("needs").as_list().empty() && !m->contains("sameAs"));
      const auto& ctl = m->at("controls").as_list();
      controls += ctl.size();
      CHECK(ctl.size() == 1 && ctl[0]->get_string("name") == "Setup..." && ctl[0]->get_string("type") == "button");
      continue;
    }
    CHECK_EQ(m->contains("screen"), p.screen != nullptr);  // startrek and screams
    if (p.screen) CHECK_EQ(m->get_string("screen"), std::string(p.screen));
    if (p.recipe == Recipe::ad3zip && std::string_view(p.id) != "ad32" && std::string_view(p.id) != "tt" &&
        std::string_view(p.id) != "simpsons") {
      // The Looney Tunes, ScreamSavers and the Disney Collection: Classic
      // entries, their names trimmed (the Disney Collection's name resources
      // start with two spaces) and the Disney Collection's five squeezed
      // names spelt out; no file is another release's.
      CHECK_EQ(m->get_string("lane"), std::string("ne16"));
      CHECK_EQ(m->get_string("entry"), std::string("MODULE"));
      CHECK(!m->contains("sameAs"));
      const std::string name = m->get_string("moduleName"), id = m->get_string("id");
      CHECK(!name.empty() && name.front() != ' ' && name.back() != ' ');
      static const std::map<std::string, std::string> spelt = {
          {"disney.dalm", "101 Dalmatians"},    {"disney.dsclocks", "Disney Clocks"}, {"disney.falling", "Falling Flower"},
          {"disney.firewrk", "Magic Kingdom"},  {"disney.mermaid", "Little Mermaid"}, {"looney.ltmessgs", "Messages"},
          {"looney.frog", "Michigan J. Frog"},  {"screams.twister", "Spin Out"}};
      if (auto it = spelt.find(id); it != spelt.end()) CHECK_EQ(name, it->second);
      const auto& needs = m->at("needs").as_list();
      const std::string p_id = p.id;
      if (p_id == "disney") CHECK_EQ(needs.size(), size_t(2));  // ADXPL100, AD_RSRC
      if (p_id == "looney") CHECK(needs.size() == 1 && needs[0]->as_string() == "ADXPL41");
      // Every TNG module but Warp Effect (an older build) imports ADXPL320.
      if (p_id == "tng" && id != "tng.warpefct") CHECK(needs.size() == 1 && needs[0]->as_string() == "ADXPL320");
      if (p_id == "screams") CHECK(needs.size() == 1 && needs[0]->as_string() == "AD_SND");
    }
    if (isl) {
      // Classic entries, no file another release's; Marvel's at its fixed
      // screen with its two buttons, needing its decoder; Snoopy's for any
      // screen, six of them needing AD_SND, no buttons.
      CHECK_EQ(m->get_string("lane"), std::string("ne16"));
      CHECK_EQ(m->get_string("entry"), std::string("MODULE"));
      CHECK(!m->contains("sameAs"));
      const std::string id = m->get_string("id"), name = m->get_string("displayName");
      CHECK(name == m->get_string("moduleName"));
      const auto& needs = m->at("needs").as_list();
      const auto& ctl = m->at("controls").as_list();
      controls += ctl.size();
      if (id == "marvel.marvel") {
        CHECK_EQ(name, std::string("Marvel Comics"));
        CHECK(needs.size() == 1 && needs[0]->as_string() == "DECO");
        CHECK(ctl.size() == 4 && ctl[0]->get_string("type") == "button" && ctl[0]->get_string("name") == "Saver.." &&
              ctl[1]->get_string("type") == "button" && ctl[1]->get_string("name") == "Posters...");
      } else {
        static const std::map<std::string, std::string> names = {
            {"snoopy.is_colag", "Collage"},      {"snoopy.is_dance", "Dance"},          {"snoopy.is_faces", "Faces"},
            {"snoopy.is_fly", "Flying Ace"},     {"snoopy.is_linus", "Linus & Snoopy"}, {"snoopy.is_litry", "Literary Ace"},
            {"snoopy.is_sptlt", "Spotlights"},   {"snoopy.is_thrpy", "Therapy"}};
        auto it = names.find(id);
        CHECK(it != names.end() && it->second == name);
        const bool silent = id == "snoopy.is_colag" || id == "snoopy.is_sptlt";
        CHECK_EQ(needs.size(), size_t(silent ? 0 : 1));
        if (!silent && needs.size() == 1) CHECK_EQ(needs[0]->as_string(), std::string("AD_SND"));
        for (auto& c : ctl) CHECK(c->get_string("type") != "button");
      }
      continue;
    }
    if (ad2) {
      // After Dark 2.0 modules: Classic entries with the release's fixed
      // 640x480 screen; the names trimmed (15 had a leading space),
      // PLANETS.AD's overridden; the registrant stand-in and hand-wrapped
      // breaks gone.
      CHECK_EQ(m->get_string("lane"), std::string("ne16"));
      CHECK_EQ(m->get_string("entry"), std::string("MODULE"));
      CHECK_EQ(m->get_string("screen"), std::string("640x480"));
      const std::string name = m->get_string("displayName"), about = m->get_string("about");
      CHECK(!name.empty() && name.front() != ' ' && name == m->get_string("moduleName"));
      CHECK(about.find("Authorized User") == std::string::npos);
      // No sentence wrapped by hand is left (" \n" before a lower-case
      // letter); " \n" before a capital or an empty line stays.
      bool wrapped = false;
      for (size_t at = about.find(" \n"); at != std::string::npos; at = about.find(" \n", at + 1))
        wrapped = wrapped || (at + 2 < about.size() && about[at + 2] >= 'a' && about[at + 2] <= 'z');
      CHECK(!wrapped);
      CHECK(!m->contains("sameAs"));  // no file is another release's
      const std::string id = m->get_string("id");
      if (id == "startrek.planets") CHECK_EQ(name, std::string("Planetary Atlas"));
      if (id == "startrek.braincel") CHECK(about.find("turn off your computer.") != std::string::npos);
      const auto& needs = m->at("needs").as_list();
      CHECK_EQ(needs.size(), size_t(id == "startrek.sounder" ? 1 : 2));
      const auto& ctl = m->at("controls").as_list();
      controls += ctl.size();
      for (auto& c : ctl)
        if (c->get_string("type") == "button")
          CHECK((id == "startrek.comms" && c->get_int("index") == 3 && c->get_string("name") == "Edit Custom...") ||
                (id == "startrek.sounder" && c->get_int("index") == 2 && c->get_string("name") == "Sounds.."));
      continue;
    }
    if (!imx) continue;
    // An Intermission module: its entry (an IMX module's SAVERDRAW, a
    // Delrina release's ASA animation's and IMQ module's SAVERMAIN, its
    // reader's), its one button (and, on Intermission 4.0's modules that
    // step once per call, the Speed control after it), its registry name.
    CHECK_EQ(m->get_string("lane"), std::string("ne16"));
    CHECK_EQ(m->get_string("abi"), std::string("intermission"));
    const bool imx_file = ends_with_i(m->get_string("path"), ".IMX");
    CHECK_EQ(m->get_string("entry"), std::string(p.delrina_installer() && !imx_file ? "SAVERMAIN" : "SAVERDRAW"));
    CHECK_EQ(m->get_string("about"), std::string());
    std::string rel = m->get_string("path").substr(std::string(p.root).size() + 1), name;
    const SpeedModule* speed = nullptr;
    for (const SpeedModule& s : p.speed_modules)
      if (iequals(s.module, rel)) speed = &s;
    speeds += speed != nullptr;
    const auto& controls = m->at("controls").as_list();
    CHECK_EQ(controls.size(), size_t(speed ? 2 : 1));
    if (!controls.empty()) {
      CHECK_EQ(controls[0]->get_string("name"), std::string("Configure..."));
      CHECK_EQ(controls[0]->get_string("type"), std::string("button"));
      CHECK(!controls[0]->contains("host"));
    }
    if (speed && controls.size() == 2) {
      // At the module's own starting stop (Dragon Kites at Slowest, ...).
      const phosg::JSON& s = *controls[1];
      CHECK(s.get_int("index") == 1 && s.get_string("name") == "Speed:" && s.get_string("kind") == "stringslider" &&
            s.get_string("type") == "slider" && s.get_int("defaultStop") == int(speed->start) &&
            s.get_int("default") == speed_control(speed->start).def && s.get_string("host") == "ADNE16IMXSPEED");
      CHECK(s.at("values").as_list().size() == 5 && s.at("items").as_list().size() == 5);
    }
    for (const NameOverride& o : p.name_overrides)
      if (iequals(o.module, rel)) name = o.name;
    CHECK(!name.empty());
    CHECK_EQ(m->get_string("moduleName"), name);
    // Its name, or (in a catalog of every release) told apart by its short
    // title where an earlier release has it (Rat Race, Logo, Einstein, Tunnel).
    const std::string shown = m->get_string("displayName");
    CHECK(shown == name || shown == name + " (" + p.short_title + ")");
    // Data a reader plays (an ASA animation, Intermission 4.0's FLI, MRF and
    // MSV) imports nothing.
    const bool data = ends_with_i(rel, ".ASA") || ends_with_i(rel, ".FLI") || ends_with_i(rel, ".MRF") ||
                      ends_with_i(rel, ".MSV");
    if (data) CHECK(m->at("needs").as_list().empty() && m->at("system").as_list().empty());
    for (auto& need : m->at("needs").as_list()) {
      const std::string d = need->as_string();
      if (p.delrina_installer()) CHECK(d == "INTRMLIB" || d == "ANTSW" || d == "DIBDLL" || d == "IM4_EXP" || d == "DECO");
      else CHECK(d == "INTRMLIB" || d == "READJPG" || d == "STRESS" || d == "SWSE");
    }
  }
  fprintf(stderr, "  catalog: %s %zu modules (%zu pe32, %zu ne16)\n", p.id, n, pe, n - pe);
  CHECK_EQ(n, e.modules);
  CHECK_EQ(pe, e.pe32);
  if (ad2) CHECK_EQ(controls, size_t(40));
  if (isl && std::string_view(p.id) == "snoopy") CHECK_EQ(controls, size_t(27));
  CHECK_EQ(speeds, p.speed_modules.size());  // every module listed has it (Intermission 4.0's alone)
}

// Star Wars Screen Entertainment's installer script, read from its image,
// against the recipe: every loose file is the installer's line for it (its
// source, its destination, compressed or not; General MIDI the chosen set),
// and every other line is an archive or a file the recipe never reads.
void check_install_dat(const fs::path& image) {
  const Package& p = *find_package("swse");
  auto src = open_image(image);
  auto node = src->find("INSTALL.DAT");
  CHECK(node.has_value());
  if (!node) return;
  const std::vector<uint8_t> bytes = src->read_all(*node, 64 * 1024);
  std::string text(bytes.begin(), bytes.end());
  struct Line {
    int number;
    std::string src, dst;
    int check = -1;  // CHECK=n; -1 = unconditional
  };
  std::vector<Line> lines;
  bool install = false;
  auto trim = [](std::string v) {
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(0, 1);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r')) v.pop_back();
    return v;
  };
  for (size_t i = 0; i < text.size();) {
    size_t eol = text.find('\n', i);
    std::string line = trim(text.substr(i, eol == std::string::npos ? std::string::npos : eol - i));
    i = eol == std::string::npos ? text.size() : eol + 1;
    if (line.empty()) continue;
    if (line.front() == '[') {
      install = iequals(line, "[install]");
      continue;
    }
    size_t eq = line.find('=');
    if (!install || eq == std::string::npos) continue;
    std::string key = trim(line.substr(0, eq));
    if (key.empty() || key.find_first_not_of("0123456789") != std::string::npos) continue;  // maxLineNum
    std::vector<std::string> f;
    std::string rest = line.substr(eq + 1);
    for (size_t a = 0; a <= rest.size();) {
      size_t b = rest.find(',', a);
      if (b == std::string::npos) b = rest.size();
      f.push_back(trim(rest.substr(a, b - a)));
      a = b + 1;
    }
    if (f.size() < 4) continue;
    Line l{std::stoi(key), ascii_upper(f[1]), ascii_upper(f[2])};
    for (size_t k = 4; k < f.size(); k++)
      if (ascii_upper(f[k]).rfind("CHECK=", 0) == 0 && f[k].size() > 6) l.check = std::stoi(f[k].substr(6));
    lines.push_back(l);
  }
  fprintf(stderr, "  INSTALL.DAT: %zu install lines\n", lines.size());
  CHECK(lines.size() > 30);
  auto installed_as = [&](const std::string& dst) {
    if (dst.rfind("(WIN)\\", 0) == 0) return "WINDOWS/" + dst.substr(6);
    if (dst.rfind("(SYS)\\", 0) == 0) return "ENGINE/" + dst.substr(6);
    return std::string(p.module_dir) + "/" + dst;
  };
  // Every loose file is one of the installer's lines.
  for (const LooseFile& lf : p.loose_files) {
    const Line* l = nullptr;
    for (const Line& x : lines)
      if (x.src == ascii_upper(lf.from) && (x.check == -1 || x.check == 24 || x.check == 100)) l = &x;
    CHECK(l != nullptr);
    if (!l) continue;
    fprintf(stderr, "  line %2d %-13s -> %-16s as %s\n", l->number, l->src.c_str(), l->dst.c_str(), lf.to);
    CHECK_EQ(installed_as(l->dst), ascii_upper(lf.to));
    CHECK_EQ(lf.codec == Codec::szdd, ends_with_i(l->src, "_"));
    CHECK(lf.codec != Codec::kwaj);
    if (ends_with_i(l->src, ".MI_")) CHECK_EQ(l->check, 24);  // the General MIDI set
  }
  // Every other line: an archive (every disk), or never read.
  static const std::set<std::string> kNeverRead = {"README.TXT", "INSTDETL.DAT", "INSTALL.EXE", "SVGA.EXE",
                                                   "SYSINI.DAT", "ANTHOOK.386",  "DVA.386",     "DVA.38_",
                                                   "IMCPL.CPL",  "SWSESET.EXE",  "DIB.DR_",     "WING.DL_",
                                                   "WING32.DL_", "WINGDE.DL_",   "WINGDIB.DR_", "WINGPAL.WN_"};
  for (const Line& l : lines) {
    bool loose = false;
    for (const LooseFile& lf : p.loose_files) loose = loose || l.src == ascii_upper(lf.from);
    bool archive = false;
    for (const char* a : p.required_archives) archive = archive || l.src == a;
    if (l.dst == "(DST)") {
      CHECK(archive);
    } else if (loose) {
      // One MIDI name per set: only CHECK=24's source is a loose file.
      if (ends_with_i(l.src, ".MI_")) CHECK_EQ(l.check, 24);
    } else if (ends_with_i(l.src, ".MI_")) {
      CHECK(l.check >= 20 && l.check <= 23);  // an FM/OPL set, never read
    } else if (!kNeverRead.count(l.src)) {
      test::g_failures++;
      fprintf(stderr, "  line %d installs %s, which the recipe neither takes nor lists as never read\n", l.number,
              l.src.c_str());
    }
  }
}

// Star Trek: The Screen Saver's Setup script, ST_NSTLL.INF (KWAJ-compressed as
// ST_NSTLL.IN_ on disk 1), against the registry's table: every row is an INF
// line with the same disk, installed name and size (field 15), placed where
// its section installs it, flattened as the recipe does (the installer's
// C:\WINDOWS DLLs beside the modules, AD_SND.DLL and AD.EXE in ENGINE); the
// disk tag files are the INF's; every other line installs a file the recipe
// never reads. `images`: both disks' images, or the ZIP of them.
void check_inf(const std::vector<fs::path>& images) {
  const Package& p = *find_package("startrek");
  // Each disk on its own, by the md5 that makes it disk 1 or 2.
  std::map<int, std::unique_ptr<SourceFs>> disks;
  auto add = [&](std::unique_ptr<SourceFs> fs, const std::string& md5) {
    for (const KnownImage& k : p.images)
      if (md5 == k.md5) disks[k.disk] = std::move(fs);
  };
  for (const fs::path& image : images) {
    auto zipped = floppy_images_in_zip(image);
    if (zipped.empty()) {
      add(open_image(image), md5_file_hex(image));
      continue;
    }
    for (auto& z : zipped) add(open_fat_image(z.bytes, z.name), md5_hex(z.bytes->data(), z.bytes->size()));
  }
  CHECK(disks.size() == 2 && disks.count(1) && disks.count(2));
  if (disks.size() != 2 || !disks.count(1)) return;
  auto on_disk = [&](const std::string& name) {
    for (auto& [n, fs] : disks)
      if (fs->find(name)) return n;
    return 0;
  };
  auto inf_node = disks[1]->find("ST_NSTLL.IN_");
  CHECK(inf_node.has_value());
  if (!inf_node) return;
  const std::vector<uint8_t> packed = disks[1]->read_all(*inf_node, 1 << 20);
  std::string text;
  kwaj_expand(packed, "ST_NSTLL.IN_", 1 << 20, [&](const uint8_t* d, size_t n) { text.append((const char*)d, n); });
  struct Line {
    std::string section, name;  // the installed name, upper case
    int disk = 0;
    uint64_t size = 0;
  };
  std::vector<Line> lines;
  std::vector<std::string> tags;
  std::string section;
  auto trim = [](std::string v) {
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t' || v.front() == '"')) v.erase(0, 1);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r' || v.back() == '"')) v.pop_back();
    return v;
  };
  for (size_t i = 0; i < text.size();) {
    size_t eol = text.find('\n', i);
    std::string line = trim(text.substr(i, eol == std::string::npos ? std::string::npos : eol - i));
    i = eol == std::string::npos ? text.size() : eol + 1;
    if (line.empty()) continue;
    if (line.front() == '[') {
      section = line.substr(1, line.find(']') - 1);
      continue;
    }
    std::vector<std::string> f;
    std::string rest = line.substr(line.find('=') == std::string::npos ? 0 : line.find('=') + 1);
    for (size_t a = 0; a <= rest.size();) {
      size_t b = rest.find(',', a);
      if (b == std::string::npos) b = rest.size();
      f.push_back(trim(rest.substr(a, b - a)));
      a = b + 1;
    }
    if (section == "Source Media Descriptions") {
      if (f.size() >= 3) tags.push_back(ascii_upper(f[2]));
      continue;
    }
    if (section == "Default File Settings" || f.size() < 16) continue;
    lines.push_back({section, ascii_upper(f[1]), std::stoi(f[0]), std::stoull(f[15])});
  }
  fprintf(stderr, "  ST_NSTLL.INF: %zu file lines, source media %zu\n", lines.size(), tags.size());
  CHECK_EQ(lines.size(), size_t(61));  // every file of both disks but the INF itself
  CHECK((tags == std::vector<std::string>(p.required_archives.begin(), p.required_archives.end())));
  for (size_t k = 0; k < tags.size(); k++) CHECK_EQ(on_disk(tags[k]), int(k + 1));
  // COMPRESS's name for an installed name: the extension's last character
  // made '_' (appended to a shorter extension).
  auto compressed = [](const std::string& name) {
    const size_t dot = name.rfind('.');
    return name.size() - dot - 1 < 3 ? name + "_" : name.substr(0, name.size() - 1) + "_";
  };
  std::map<std::string, const KnownFile*> manifest;
  for (const KnownFile& k : p.manifest) manifest[k.path] = &k;
  std::set<std::string> taken;
  for (const LooseFile& lf : p.loose_files) {
    const std::string to = lf.to, name = to.substr(to.rfind('/') + 1);
    const Line* l = nullptr;
    for (const Line& x : lines)
      if (x.name == name) l = &x;
    CHECK(l != nullptr);
    if (!l) continue;
    taken.insert(name);
    fprintf(stderr, "  [%s] %-13s disk %d, %7llu bytes -> %s\n", l->section.c_str(), l->name.c_str(), l->disk,
            (unsigned long long)l->size, to.c_str());
    CHECK(lf.codec == Codec::kwaj);
    CHECK_EQ(std::string(lf.from), compressed(name));
    CHECK_EQ(on_disk(lf.from), l->disk);
    auto m = manifest.find(std::string(p.root) + "/" + to);
    CHECK(m != manifest.end() && m->second->size == l->size);
    std::string want;
    if (l->section == "ADModules") want = "AFTERDRK/" + name;
    else if (l->section == "ADExecutables") want = name == "AD.EXE" ? "ENGINE/AD.EXE" : "AFTERDRK/" + name;
    else if (l->section == "ResFiles" || l->section == "Sounds") want = "AFTERDRK/ST_RES/" + name;
    else if (l->section == "Noises") want = "AFTERDRK/SOUNDS/" + name;
    else if (l->section == "WinDirectoryFiles") want = name == "AD_SND.DLL" ? "ENGINE/AD_SND.DLL" : "AFTERDRK/" + name;
    CHECK_EQ(want, to);
  }
  // Everything else the installer could copy, never read by the recipe (I5).
  static const std::set<std::string> kNeverRead = {
      "ADINIT.EXE",   "AD_AILAN.DLL", "AD_MPT.DRV",   "AD_NVLNW.DLL", "AFTERDRK.NSS", "AD_LIB.DLL",   "AD_SB.DRV",
      "AD_NET.EXE",   "NWCORE.DLL",   "NWCONN.DLL",   "NWMISC.DLL",   "AD_WRAP.COM",  "SPALETTE.DLL", "AD.HLP",
      "AD_MESG.ADS",  "AD_PREFS.INI", "SPLASH1.BMP",  "BMPRSRC.DLL",  "AD_NSTLL.INI", "AD.386",       "SETUP.LST",
      "MSDETECT.INC", "MSUILSTF.DLL", "VER.DLL",      "AD_NSTLL.MST", "MSSHLSTF.DLL", "MSCUISTF.DLL", "SETUPAPI.INC",
      "MSDETSTF.DLL", "AD_NSTLL.DLL", "SETUP.EXE",    "MSINSSTF.DLL", "MSCOMSTF.DLL", "_MSTEST.EXE"};
  for (const Line& l : lines)
    if (!taken.count(l.name) && !kNeverRead.count(l.name)) {
      test::g_failures++;
      fprintf(stderr, "  [%s] %s: neither in the table nor listed as never read\n", l.section.c_str(), l.name.c_str());
    }
  CHECK_EQ(taken.size(), p.loose_files.size());
}

// Star Trek: The Screen Saver's two disk images in disk order, from the loose
// images or the ZIP they came in (by the md5 that makes each disk 1 or 2).
std::vector<std::vector<uint8_t>> startrek_disks(const std::vector<fs::path>& images) {
  const Package& p = *find_package("startrek");
  std::map<int, std::vector<uint8_t>> by_disk;
  auto add = [&](const std::vector<uint8_t>& bytes) {
    const std::string md5 = md5_hex(bytes.data(), bytes.size());
    for (const KnownImage& k : p.images)
      if (md5 == k.md5) by_disk[k.disk] = bytes;
  };
  for (const fs::path& image : images) {
    auto zipped = floppy_images_in_zip(image);
    if (zipped.empty()) add(test::read_bytes(image));
    for (auto& z : zipped) add(*z.bytes);
  }
  std::vector<std::vector<uint8_t>> out;
  for (auto& [disk, bytes] : by_disk) out.push_back(std::move(bytes));
  return out;
}

// The two disks in a ZIP under code page 437 names, as Explorer or 7-Zip on
// an English Windows zips them when a name fits it (general-purpose bit 11
// clear): the known disk set, verified "image", the files of the images' own
// import, and an import.json that is UTF-8 with the names decoded; two names
// that differ only in a letter outside ASCII (u-umlaut, o-umlaut) are two
// names.
void check_cp437_zips(const std::wstring& exe, const fs::path& scratch, const std::vector<fs::path>& images) {
  const std::vector<std::vector<uint8_t>> disks = startrek_disks(images);
  CHECK_EQ(disks.size(), size_t(2));
  if (disks.size() != 2) return;
  const Tree want = snapshot(scratch / L"alone-startrek" / L"win" / L"packages" / L"startrek", true);
  auto ends_with = [](const std::string& s, const std::string& tail) {
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
  };
  for (const auto& [tag, one, two] : std::vector<std::tuple<std::wstring, std::string, std::string>>{
           {L"cp437", "DISK\x81\x94" "1.IMG", "DISK\x81\x94" "2.IMG"}, {L"cp437-pair", "TREK\x81.IMG", "TREK\x94.IMG"}}) {
    test::ZipBuilder z;
    z.password = "";
    z.add(one, disks[0], /*deflate=*/false, /*encrypt=*/false);
    z.add(two, disks[1], /*deflate=*/false, /*encrypt=*/false);
    const fs::path zip = scratch / (L"startrek-" + tag + L".zip"), root = scratch / (L"zip-" + tag);
    test::write_bytes(zip, z.build());
    const int code = adimport(exe, {L"--no-cover-download", L"--image", zip.wstring(), L"--dest", root.wstring()},
                              ("Star Trek's disks in a ZIP, code page 437 names: " + to_utf8(tag)).c_str());
    CHECK_EQ(code, 0);
    if (code) continue;
    const fs::path pkg = root / L"win" / L"packages" / L"startrek";
    const std::string text = test::read_text(pkg / L"import.json");
    CHECK(test::strict_utf8(text));
    phosg::JSON j = phosg::JSON::parse(text);
    CHECK_EQ(j.get_string("verified"), std::string("image"));
    CHECK_EQ(j.at("source").get_bool("imageMd5Known"), true);
    const auto& parts = j.at("source").at("parts").as_list();
    CHECK(parts.size() == 2 && ends_with(parts[0]->get_string("path"), "!" + oem437_to_utf8(one)) &&
          ends_with(parts[1]->get_string("path"), "!" + oem437_to_utf8(two)));
    CHECK(snapshot(pkg, true) == want);
  }
}

// InstallShield's package list SETUP.PKG, parsed here on its own (the format
// survey's tools/setuppkg.py layout): each library's logical name with its
// members' names and sizes, in order. Throws std::runtime_error when it does
// not add up.
std::vector<std::pair<std::string, std::vector<std::pair<std::string, uint32_t>>>> parse_package_list(
    const std::vector<uint8_t>& d) {
  size_t p = 0;
  auto need = [&](size_t n) {
    if (d.size() - p < n) throw std::runtime_error("SETUP.PKG ends early");
  };
  auto u8 = [&] {
    need(1);
    return uint32_t(d[p++]);
  };
  auto u16 = [&] {
    need(2);
    p += 2;
    return uint32_t(d[p - 2] | d[p - 1] << 8);
  };
  auto u32 = [&] {
    const uint32_t lo = u16();
    return lo | u16() << 16;
  };
  auto text = [&](size_t n) {
    need(n);
    p += n;
    return std::string(d.begin() + ptrdiff_t(p - n), d.begin() + ptrdiff_t(p));
  };
  if (u16() != 0xA34A) throw std::runtime_error("SETUP.PKG: no magic");
  const uint32_t table = u32();
  u32();
  std::map<uint32_t, std::vector<std::pair<std::string, uint32_t>>> groups;
  while (p < table) {
    const uint32_t at = uint32_t(p), size = u32(), end = uint32_t(p) + size;
    std::vector<std::pair<std::string, uint32_t>> files;
    for (uint32_t dirs = u16(); dirs; dirs--) {
      text(u16());
      if (u8()) throw std::runtime_error("SETUP.PKG: a directory name without its NUL");
      for (uint32_t n = u16(); n; n--) {
        u16();
        const uint32_t usize = u32();
        std::string name = text(u8());
        if (u8()) throw std::runtime_error("SETUP.PKG: a file name without its NUL");
        files.push_back({name, usize});
      }
    }
    if (p != end) throw std::runtime_error("SETUP.PKG: a group of another size");
    groups[at] = files;
  }
  std::vector<std::pair<std::string, std::vector<std::pair<std::string, uint32_t>>>> out;
  while (p < d.size()) {
    u16();
    u16();
    for (uint32_t n = u16(); n; n--) {
      std::string name = text(u16());
      u16();
      const uint32_t group = u32();
      if (!groups.count(group)) throw std::runtime_error("SETUP.PKG: a library pointing at no group");
      out.push_back({name, groups[group]});
    }
  }
  return out;
}

// What Marvel Comics Screen Posters' libraries hold besides the table's
// members, which the recipe never installs: the rest of engine.lib (the
// drivers, AD_LIB, ADINIT, the manual and readme texts) and of win.lib (the
// help file, the DOS wrapper, the PC-speaker palette library, the disk's
// AD_PREFS.INI).
const std::set<std::string> kIslibNeverInstalled = {"ADINIT.EXE",   "AD_LIB.DLL",  "MRVLREAD.TXT", "MARVEL.TXT",
                                                    "AD_MPT.DRV",   "AD_SB.DRV",   "AD_MME.DRV",   "MRVL.WRI",
                                                    "EDITFILE.TXT", "MARVELAD.TXT", "AD.HLP",      "AD_WRAP.COM",
                                                    "SPALETTE.DLL", "AD_PREFS.INI"};

// The previous owners' notes in the user's Marvel and Snoopy ZIPs: never
// extracted, read or hashed (only their names, to skip them).
const std::set<std::string> kIslibNotes = {"SERIAL#.DOC", "REG#.TXT", "SERIAL.NFO", "ADNEWS.TXT"};

// 1g. The islib packages' tables against their disks (the user's ZIP, read
// as the union of its disk folders): every row's member is in its library
// with the manifest's size; every member the table leaves is one the recipe
// never installs; SETUP.PKG lists each library the table reads with exactly
// its members and sizes, and the tag member in the tag library.
void check_islib_tables(const std::string& id, const fs::path& zip) {
  const Package& p = *find_package(id);
  auto src = open_image(zip);
  auto pkg_node = src->find("SETUP.PKG");
  CHECK(pkg_node.has_value());
  if (!pkg_node) return;
  const auto list = parse_package_list(src->read_all(*pkg_node, 64 * 1024));
  std::map<std::string, uint64_t> manifest;
  for (const KnownFile& k : p.manifest) manifest[k.path] = k.size;
  std::map<std::string, std::unique_ptr<IszLibrary>> libs;
  std::set<std::string> taken;  // library!member
  for (const LibraryMember& row : p.library_members) {
    auto& lib = libs[row.library];
    if (!lib) {
      std::vector<IszVolume> volumes;
      for (unsigned k = 1;; k++) {
        const std::string name = k == 1 ? std::string(row.library) : isz_volume_name(row.library, k).value_or("");
        auto node = name.empty() ? std::nullopt : src->find(name);
        if (!node) break;
        volumes.push_back({name, std::make_shared<const std::vector<uint8_t>>(src->read_all(*node))});
        const IszHeader h = isz_header(*volumes.front().data, volumes.front().name);
        if (!h.split || k >= h.volumes) break;
      }
      lib = std::make_unique<IszLibrary>(std::move(volumes));
      fprintf(stderr, "  %s: %s, %zu members over %zu volume(s)\n", id.c_str(), row.library, lib->members().size(),
              lib->volumes().size());
    }
    const IszMember* m = lib->find(row.member);
    CHECK(m != nullptr);
    if (!m) continue;
    CHECK_EQ(uint64_t(m->size), manifest[std::string(p.root) + "/" + row.to]);
    taken.insert(std::string(row.library) + "!" + ascii_upper(m->name));
  }
  for (const auto& [name, lib] : libs) {
    for (const IszMember& m : lib->members())
      if (!taken.count(name + "!" + ascii_upper(m.name)) && !kIslibNeverInstalled.count(ascii_upper(m.name))) {
        test::g_failures++;
        fprintf(stderr, "  %s!%s: neither in the table nor listed as never installed\n", name.c_str(), m.name.c_str());
      }
    // The package list's entry for this library: the same members, sizes and order.
    std::vector<std::pair<std::string, uint32_t>> members;
    for (const IszMember& m : lib->members()) members.push_back({m.name, m.size});
    bool listed = false;
    for (const auto& [logical, files] : list) listed = listed || files == members;
    CHECK(listed);
  }
  bool tag = false;
  for (const auto& [logical, files] : list)
    if (iequals(logical, p.tag_library))
      for (const auto& [name, size] : files) tag = tag || iequals(name, p.tag_member);
  CHECK(tag);
  fprintf(stderr, "  %s: SETUP.PKG lists %zu libraries; the table's %zu rows checked\n", id.c_str(), list.size(),
          p.library_members.size());
}

// 1h. The islib packages' disks copied out of the user's ZIP (every file but
// the previous owners' notes, which are skipped by name and never read) as a
// flat folder, DISK1/DISK2 folders and a flat ZIP (verified "files", the
// ZIP's files), and each disk alone (disk 1: every install disk is needed;
// disk 2: no release).
void check_islib_forms(const std::wstring& exe, const fs::path& scratch, const std::string& id, const fs::path& zip) {
  const Package& p = *find_package(id);
  auto bytes = std::make_shared<std::vector<uint8_t>>(test::read_bytes(zip));
  ZipArchive z(bytes, to_utf8(zip.filename().wstring()), ZipNames::disk_folders);
  std::map<int, Tree> disks;
  Tree flat;
  for (const ZipMember& m : z.members()) {
    if (m.directory) continue;
    const std::string name = m.file_name();
    if (kIslibNotes.count(ascii_upper(name))) continue;  // never extracted
    std::vector<uint8_t> data;
    z.extract(m, "", [&](const uint8_t* d, size_t n) { data.insert(data.end(), d, d + n); });
    disks[int(m.disk)][name] = data;
    flat[name] = data;
  }
  CHECK_EQ(disks.size(), size_t(2));
  const fs::path base = scratch / to_wide("forms-" + id);
  for (const auto& [rel, d] : flat) test::write_bytes(base / L"flat" / to_wide(rel), d);
  for (const auto& [n, files] : disks)
    for (const auto& [rel, d] : files) {
      test::write_bytes(base / L"disks" / (L"DISK" + std::to_wstring(n)) / to_wide(rel), d);
      test::write_bytes(base / (L"disk" + std::to_wstring(n)) / to_wide(rel), d);
    }
  test::ZipBuilder zb;
  zb.password = "";
  for (const auto& [rel, d] : flat) zb.add(rel, d, /*deflate=*/true, /*encrypt=*/false);
  test::write_bytes(base / L"flat.zip", zb.build());
  const Tree want = snapshot(scratch / to_wide("alone-" + id) / L"win" / L"packages" / to_wide(id), true);
  for (const auto& [what, args] : std::vector<std::pair<std::string, std::vector<std::wstring>>>{
           {"a flat folder", {L"--from", (base / L"flat").wstring()}},
           {"DISK1/DISK2 folders", {L"--from", (base / L"disks").wstring()}},
           {"a flat ZIP", {L"--image", (base / L"flat.zip").wstring()}}}) {
    const fs::path root = base / to_wide("root-" + what.substr(what.find(' ') + 1));
    std::vector<std::wstring> a = {L"--no-cover-download"};
    a.insert(a.end(), args.begin(), args.end());
    a.insert(a.end(), {L"--dest", root.wstring()});
    const int code = adimport(exe, a, (std::string(p.title) + " as " + what).c_str());
    CHECK_EQ(code, 0);
    if (code) continue;
    const fs::path pkg = root / L"win" / L"packages" / to_wide(id);
    phosg::JSON j = load(pkg / L"import.json");
    CHECK_EQ(j.get_string("verified"), std::string("files"));
    CHECK(j.at("missingKnown").as_list().empty());
    CHECK(snapshot(pkg, true) == want);
  }
  const int d1 = adimport(exe, {L"--no-cover-download", L"--from", (base / L"disk1").wstring(), L"--dest",
                                (base / L"root-disk1").wstring()},
                          (std::string(p.title) + ", disk 1 alone").c_str());
  CHECK_EQ(d1, 2);
  const int d2 = adimport(exe, {L"--no-cover-download", L"--from", (base / L"disk2").wstring(), L"--dest",
                                (base / L"root-disk2").wstring()},
                          (std::string(p.title) + ", disk 2 alone").c_str());
  CHECK_EQ(d2, 2);
  CHECK(!fs::exists(base / L"root-disk1" / L"win" / L"packages" / to_wide(id)));
}

}  // namespace

int main(int argc, char** argv) {
  const char* on = getenv("AD_E2E_PKG");
  if (!on || std::string(on) != "1") {
    fprintf(stderr, "import.pkg_real: skipped (set AD_E2E_PKG=1 to import the real package sources)\n");
    return 77;
  }
  if (argc < 3) {
    fprintf(stderr, "usage: test_import_pkg_real <adimport.exe> <scratch> [<image dir>]\n");
    return 2;
  }
  std::wstring exe = fs::absolute(argv[1]).wstring();
  // The installed assets (read only: a folder source for step 2), found
  // before the sandbox hides them; every adimport run here has its own --dest.
  const fs::path installed_root = test::installed_assets_root();
  fs::path scratch = test::scratch(argc - 1, argv + 1, "adw-import-pkg-real");
  test::sandbox_data_root(scratch / L"localappdata");
  std::vector<fs::path> dirs;
  for (const fs::path& d : test::image_dirs(argc > 3 ? fs::path(argv[3]) : fs::path()))
    dirs.push_back(fs::absolute(d).make_preferred());  // Mount-DiskImage needs an absolute path

  // ---- find the images by size and md5 --------------------------------------------------
  // One image per package; every install disk's (or the ZIP of them) for a
  // release on several.
  std::map<std::string, std::vector<fs::path>> images_of;
  for (const Package& p : builtin_packages()) {
    if (p.is_deluxe()) continue;
    std::vector<fs::path> set = test::find_disk_set(dirs, p);
    if (!set.empty()) {
      images_of[p.id] = set;
      continue;
    }
    for (const KnownImage& k : p.images) {
      if (k.disk) continue;
      fs::path f = test::find_image(dirs, k.size, k.md5);
      if (!f.empty()) {
        images_of[p.id] = {f};
        break;
      }
    }
  }
  for (const Package& p : builtin_packages()) {
    if (p.is_deluxe()) continue;
    std::string where;
    for (const fs::path& f : images_of[p.id]) where += (where.empty() ? "" : " + ") + to_utf8(f.wstring());
    fprintf(stderr, "%-9s %s\n", p.id, where.empty() ? "(image not found)" : where.c_str());
    if (where.empty()) images_of.erase(p.id);
  }
  const size_t sources = builtin_packages().size() - 1;  // every package but Deluxe (from the installed assets)
  if (images_of.size() != sources) {
    std::string where;
    for (const fs::path& d : dirs) where += (where.empty() ? "" : "; ") + to_utf8(d.wstring());
    fprintf(stderr, "SKIP: needs all %zu package sources in %s\n", sources, where.c_str());
    return 77;
  }
  // "--image <each image>" for a package.
  auto image_args = [&](const std::string& id) {
    std::vector<std::wstring> a;
    for (const fs::path& f : images_of[id]) a.insert(a.end(), {L"--image", f.wstring()});
    return a;
  };
  auto with = [](std::vector<std::wstring> a, const std::vector<std::wstring>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
  };

  // ---- 1. each package alone ----------------------------------------------------------------
  for (const Package& p : builtin_packages()) {
    if (p.is_deluxe()) continue;
    fs::path root = scratch / (L"alone-" + to_wide(p.id));
    int code = adimport(exe, with(with({L"--no-cover-download"}, image_args(p.id)), {L"--dest", root.wstring()}), p.title);
    CHECK_EQ(code, 0);
    if (code) continue;
    fs::path win = root / L"win";
    CHECK(!fs::exists(win / L"FILES"));
    check_package_root(win, p);
    phosg::JSON cat = load(win / L"catalog-win.json");
    CHECK_EQ(cat.at("modules").as_list().size(), kExpect.at(p.id).modules);
    CHECK_EQ(cat.at("packages").as_list().size(), size_t(1));
    check_catalog_of(cat, p);
    // A second import of the same image into the same root: the same files.
    Tree before = snapshot(win / L"packages" / to_wide(p.id), true);
    CHECK_EQ(adimport(exe,
                      with(with({L"--no-cover-download"}, image_args(p.id)), {L"--dest", root.wstring(), L"--quiet"}),
                      "again"),
             0);
    CHECK(snapshot(win / L"packages" / to_wide(p.id), true) == before);
  }

  // ---- 1b. the CDs mounted by Windows, through --from the drive ------------------------------------
  // Windows lists the 10th Anniversary disc by its Joliet names ("Toaster
  // 2k.ad"); a CD drive root is read as the disc, so the result must be the
  // image import's, file for file. Mounting may be refused by policy; that
  // skips this step rather than failing it.
  for (const char* id : {"ad10", "tt", "swse"}) {
    const fs::path& iso = images_of[id].front();
    if (!ends_with_i(to_utf8(iso.extension().wstring()), ".iso")) {
      fprintf(stderr, "---- %s mounted: %s is no .iso Windows can mount; step skipped\n", id,
              to_utf8(iso.filename().wstring()).c_str());
      continue;
    }
    std::wstring q = L"'" + iso.wstring() + L"'";
    std::wstring ps = L"$i = Get-DiskImage -ImagePath " + q + L"; $was = $i.Attached; "
                      L"if (-not $was) { $i = Mount-DiskImage -ImagePath " + q + L" -PassThru }; $l = $null; "
                      L"for ($k = 0; $k -lt 40 -and -not $l; $k++) { $l = ($i | Get-Volume).DriveLetter; "
                      L"if (-not $l) { Start-Sleep -Milliseconds 250 } }; "
                      L"Write-Output ('DRIVE=' + $l + ' WAS=' + $was)";
    const wchar_t* powershell = L"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
    test::ProcessResult m = test::run_process(powershell, {L"-NoProfile", L"-NonInteractive", L"-Command", ps}, 120000);
    size_t at = m.output.find("DRIVE=");
    char letter = at != std::string::npos && at + 6 < m.output.size() ? m.output[at + 6] : 0;
    if (!isalpha((unsigned char)letter)) {
      fprintf(stderr, "---- %s mounted: could not mount the image; step skipped\n%s", id, m.output.c_str());
      continue;
    }
    fs::path root = scratch / (L"drive-" + to_wide(id));
    std::wstring drive = std::wstring(1, wchar_t(letter)) + L":\\";
    int code = adimport(exe, {L"--no-cover-download", L"--from", drive, L"--dest", root.wstring()},
                        (std::string(id) + " --from the mounted disc").c_str());
    if (m.output.find("WAS=False") != std::string::npos)
      test::run_process(
          powershell, {L"-NoProfile", L"-NonInteractive", L"-Command", L"Dismount-DiskImage -ImagePath " + q}, 120000);
    CHECK_EQ(code, 0);
    if (code) continue;
    fs::path pkg_dir = root / L"win" / L"packages" / to_wide(id);
    phosg::JSON j = load(pkg_dir / L"import.json");
    CHECK_EQ(j.get_string("verified"), std::string("files"));
    CHECK_EQ(j.at("source").get_string("kind"), std::string("folder"));
    CHECK(j.at("missingKnown").as_list().empty());
    CHECK(snapshot(pkg_dir, true) == snapshot(scratch / (L"alone-" + to_wide(id)) / L"win" / L"packages" / to_wide(id), true));
    CHECK_EQ(load(root / L"win" / L"catalog-win.json").at("modules").as_list().size(), kExpect.at(id).modules);
  }

  // ---- 1c. Star Wars Screen Entertainment's installer script against the recipe ---------------------
  check_install_dat(images_of["swse"].front());

  // ---- 1d. Star Trek: The Screen Saver's Setup script against the recipe ------------------------
  check_inf(images_of["startrek"]);

  // ---- 1e. Star Trek: The Screen Saver's disks in a ZIP under code page 437 names ---------------
  check_cp437_zips(exe, scratch, images_of["startrek"]);

  // ---- 1f. the releases known by a ZIP, and the Looney Tunes' CD ---------------------------------
  // The user's ZIPs of the Looney Tunes, ScreamSavers and the Disney
  // Collection are their known images (step 1 imported them: verified
  // "image"). The Looney Tunes' CD (LOONEY_T), when one of the image folders
  // holds it, is its other known image: the same files.
  for (const char* id : {"looney", "screams", "disney"}) {
    const Package& p = *find_package(id);
    CHECK(images_of[id].size() == 1 && md5_file_hex(images_of[id].front()) == p.images[0].md5);
    const fs::path record = scratch / (L"alone-" + to_wide(id)) / L"win" / L"packages" / to_wide(id) / L"import.json";
    if (fs::exists(record)) {
      phosg::JSON j = load(record);
      CHECK_EQ(j.at("source").get_string("format"), std::string("zip"));
      CHECK_EQ(j.at("source").get_string("imageMd5"), std::string(p.images[0].md5));
    }
  }
  {
    const Package& lt = *find_package("looney");
    const fs::path cd = lt.images.size() > 1 ? test::find_image(dirs, lt.images[1].size, lt.images[1].md5) : fs::path();
    if (cd.empty()) {
      fprintf(stderr, "---- the Looney Tunes' CD: not in the image folders; step skipped\n");
    } else {
      const fs::path root = scratch / L"looney-cd";
      CHECK_EQ(adimport(exe, {L"--no-cover-download", L"--image", cd.wstring(), L"--dest", root.wstring()},
                        "the Looney Tunes' CD (LOONEY_T)"),
               0);
      const fs::path pkg = root / L"win" / L"packages" / L"looney";
      if (fs::exists(pkg / L"import.json")) {
        phosg::JSON j = load(pkg / L"import.json");
        CHECK_EQ(j.get_string("verified"), std::string("image"));
        CHECK_EQ(j.at("source").get_string("volumeId"), std::string("LOONEY_T"));
        CHECK(snapshot(pkg, true) == snapshot(scratch / L"alone-looney" / L"win" / L"packages" / L"looney", true));
      }
    }
  }

  // ---- 1g, 1h. the islib packages: their tables against their disks, and every form -----------------
  for (const char* id : {"marvel", "snoopy"}) {
    const Package& p = *find_package(id);
    const fs::path zip = images_of[id].front();
    // The user's ZIP is one of the package's known images (step 1: verified "image").
    bool known = false;
    for (const KnownImage& k : p.images) known = known || md5_file_hex(zip) == k.md5;
    CHECK(known);
    try {
      check_islib_tables(id, zip);
      check_islib_forms(exe, scratch, id, zip);
    } catch (const std::exception& e) {
      test::g_failures++;
      fprintf(stderr, "  %s: %s\n", id, e.what());
    }
  }

  // ---- 1i. Dilbert's other copy --------------------------------------------------------------
  // Whichever copy step 1 imported (the four ZIPs of its disks' files are
  // looked for first), the other one, when an image folder holds it, is a
  // known copy too: verified "image", the same files.
  {
    const Package& d = *find_package("dilbert");
    std::vector<fs::path> other;
    if (images_of["dilbert"].size() == 1) {
      other = test::find_disk_set(dirs, d);
    } else {
      for (const KnownImage& k : d.images)
        if (!k.disk) {
          if (fs::path f = test::find_image(dirs, k.size, k.md5); !f.empty()) other = {f};
          break;
        }
    }
    if (other.empty()) {
      fprintf(stderr, "---- Dilbert's other copy: not in the image folders; step skipped\n");
    } else {
      const fs::path root = scratch / L"dilbert-other";
      std::vector<std::wstring> a = {L"--no-cover-download"};
      for (const fs::path& f : other) a.insert(a.end(), {L"--image", f.wstring()});
      a.insert(a.end(), {L"--dest", root.wstring()});
      CHECK_EQ(adimport(exe, a, "Dilbert's other copy"), 0);
      const fs::path pkg = root / L"win" / L"packages" / L"dilbert";
      if (fs::exists(pkg / L"import.json")) {
        CHECK_EQ(load(pkg / L"import.json").get_string("verified"), std::string("image"));
        CHECK(snapshot(pkg, true) == snapshot(scratch / L"alone-dilbert" / L"win" / L"packages" / L"dilbert", true));
      }
    }
  }

  // ---- 2. every package in one root ------------------------------------------------------------
  fs::path installed = installed_root.empty() ? fs::path() : win_assets_dir(installed_root);
  if (installed.empty() || !fs::is_directory(installed / L"FILES" / L"AD40") ||
      !fs::exists(installed / L"catalog-win.json")) {
    fprintf(stderr, "no installed Deluxe assets at %s; the combined check is skipped\n", to_utf8(installed.wstring()).c_str());
    return test::finish("import.pkg_real");
  }
  fprintf(stderr, "Deluxe from %s (read only)\n", to_utf8(installed.wstring()).c_str());
  fs::path all = scratch / L"all";
  // The installed Deluxe tree is a folder source: read, never written.
  CHECK_EQ(adimport(exe, {L"--no-cover-download", L"--from", installed.wstring(), L"--dest", all.wstring()},
                    "Deluxe from the installed assets"),
           0);
  // Every other package, out of registry order on purpose.
  const auto reg = builtin_packages();
  for (size_t i = reg.size(); i-- > 0;)
    if (!reg[i].is_deluxe())
      CHECK_EQ(adimport(exe,
                        with(with({L"--no-cover-download"}, image_args(reg[i].id)), {L"--dest", all.wstring(), L"--quiet"}),
                        reg[i].id),
               0);
  fs::path win = all / L"win";
  phosg::JSON cat = load(win / L"catalog-win.json");
  const auto& mods = cat.at("modules").as_list();
  fprintf(stderr, "combined catalog: %zu modules\n", mods.size());
  CHECK_EQ(mods.size(), combined_modules());
  CHECK_EQ(cat.at("packages").as_list().size(), reg.size());
  for (const Package& p : builtin_packages())
    if (!p.is_deluxe()) {
      check_package_root(win, p);
      check_catalog_of(cat, p);
    }
  // The packages list: oldest release first (Star Trek: The Screen Saver,
  // 1992-11, then Johnny Castaway, 1992-12, the Opus 'n Bill Screen Saver,
  // 1993-09, Intermission 4.0, 1993-11, Marvel Comics Screen Posters,
  // 1993-12, The Flintstones, 1994-05, then The Far Side, 1994-06); swse
  // ties with the Simpsons (1994-08) and follows it, as in the registry; On
  // the Road Again (1994-09) comes next; Snoopy's Screen Savers, Dilbert and
  // Star Trek: The Next Generation Screen Saver (1994-10) follow them, in
  // registry order too; ScreamSavers ties with the Looney Tunes (1995-04)
  // the same way; the Disney Collection (1995-09) comes after Totally Twisted.
  {
    std::vector<std::string> order;
    for (auto& pk : cat.at("packages").as_list()) order.push_back(pk->get_string("id"));
    CHECK((order == std::vector<std::string>{"startrek",    "castaway", "opus",   "intermission", "marvel",
                                             "flintstones", "farside",  "simpsons", "swse",       "opusroad",
                                             "snoopy",      "dilbert",  "tng",    "looney",       "screams",
                                             "ad32",        "tt",       "disney", "deluxe",       "ad10"}));
  }
  // The Looney Tunes' Messages comes after 3.2's: it is told apart by its
  // short title.
  if (const phosg::JSON* m = [&]() -> const phosg::JSON* {
        for (auto& x : mods)
          if (x->get_string("id") == "looney.ltmessgs") return x.get();
        return nullptr;
      }())
    CHECK_EQ(m->get_string("displayName"), std::string("Messages (Looney Tunes)"));
  // Display names unique per lane; sameAs names an earlier entry with the same md5.
  std::set<std::string> names;
  std::map<std::string, std::string> md5_of;
  size_t same = 0;
  for (auto& m : mods) {
    std::string key = m->get_string("lane") + "\n" + ascii_lower(m->get_string("displayName"));
    if (!names.insert(key).second) {
      test::g_failures++;
      fprintf(stderr, "  display name repeated in its lane: %s\n", m->get_string("displayName").c_str());
    }
    if (m->contains("sameAs")) {
      same++;
      auto it = md5_of.find(m->get_string("sameAs"));
      CHECK(it != md5_of.end() && it->second == m->get_string("md5"));
    }
    md5_of[m->get_string("id")] = m->get_string("md5");
  }
  fprintf(stderr, "  %zu entries point at an identical earlier module (sameAs)\n", same);
  CHECK_EQ(same, size_t(73));
  // Deluxe's entries: every existing field as the installed catalog has it.
  // The installed catalog may cover other releases too (any the user has
  // imported); only its Deluxe entries are the reference here (a catalog
  // from before packages has no "package" field: all Deluxe).
  phosg::JSON ref = load(installed / L"catalog-win.json");
  std::map<std::string, const phosg::JSON*> ours;
  for (auto& m : mods) ours[m->get_string("id")] = m.get();
  size_t deluxe = 0;
  for (auto& m : ref.at("modules").as_list()) {
    if (m->contains("package") && m->get_string("package") != "deluxe") continue;
    auto it = ours.find(m->get_string("id"));
    CHECK(it != ours.end());
    if (it == ours.end()) continue;
    deluxe++;
    CHECK_EQ(it->second->get_string("package"), std::string("deluxe"));
    for (auto& [k, v] : m->as_dict()) {
      if (!it->second->contains(k) || it->second->at(k).serialize() != v->serialize()) {
        test::g_failures++;
        fprintf(stderr, "  %s.%s differs from the installed catalog\n", m->get_string("id").c_str(), k.c_str());
      }
    }
  }
  CHECK_EQ(deluxe, size_t(84));

  // ---- 3. re-imports are isolated -----------------------------------------------------------
  auto everything_but = [&](const std::string& id) {
    Tree t;
    for (auto& [rel, d] : snapshot(win))
      if (rel.rfind("packages/" + id + "/", 0) != 0 && rel != "catalog-win.json") t[rel] = d;
    return t;
  };
  for (const Package& p : builtin_packages()) {
    if (p.is_deluxe()) continue;
    const std::string id = p.id;
    Tree others = everything_but(id);
    Tree mine = snapshot(win / L"packages" / to_wide(id), true);
    CHECK_EQ(adimport(exe, with(with({L"--no-cover-download"}, image_args(id)), {L"--dest", all.wstring(), L"--quiet"}),
                      ("re-import " + id).c_str()),
             0);
    CHECK(everything_but(id) == others);
    CHECK(snapshot(win / L"packages" / to_wide(id), true) == mine);
  }
  CHECK_EQ(load(win / L"catalog-win.json").at("modules").as_list().size(), combined_modules());
  return test::finish("import.pkg_real");
}
