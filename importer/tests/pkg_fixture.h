// Synthetic sources shaped like the known releases (PACKAGES.md §2–§4), for
// the package tests: the Deluxe and 10th Anniversary CDs' plain FILES trees;
// the AD 3.x InstallShield installs (AD 3.2, Totally Twisted, the Simpsons
// floppies, the Looney Tunes, ScreamSavers, the Disney Collection and Star
// Trek: The Next Generation Screen Saver) with
// encrypted PKZIP archives under the test-only password and an
// INSTALL.INS-like script that holds it among decoys, their MODMISC.ZIP and
// AFI.ZIP holding what the real ones do (the names the fingerprint reads), and
// the owners' notes that must never be read; a Presage install shaped like
// Star Wars Screen Entertainment's (INSTALL.DAT, multi-volume ARJ archives
// with split members, SZDD loose files, decoys that must never be read); a
// Microsoft Setup install shaped like Star Trek: The Screen Saver's
// (SETUP.LST, KWAJ files written by kwaj_builder.h, two install disks,
// decoys); and InstallShield 2 installs of compressed libraries shaped like
// Marvel Comics Screen Posters' and Snoopy's Screen Savers' (SETUP.PKG, a
// library split over the two install disks and unsplit ones, written by
// isz_builder.h, the installer's files, the owners' notes and a disk copier's
// leftovers); and Delrina Intermission Installer installs shaped like The Far
// Side Screen Saver Collection's and Dilbert's (every file loose, ASA
// animations and IMQ modules, SZDD with Delrina's version stamps, every
// disk's tag file, decoys); and an InstallShield 1 floppy shaped like Screen
// Antics: Johnny Castaway's ("$" files written by isz_builder.h, a Windows
// 3.1 screen-saver program, the installer's files and a placeholder that must
// never be read). Each fixture is a tree (path -> bytes) that can be written as a
// folder, an ISO image, a FAT floppy image (or several), a flat ZIP or a ZIP
// of floppy images, plus exactly what an import must install and the catalog
// ids it must list. Modules come from module_builder.h: made-up resources, no
// bytes of any release.
#pragma once

#include <algorithm>
#include <cctype>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "arj_builder.h"
#include "fat_builder.h"
#include "importer.h"
#include "iso_builder.h"
#include "isz_builder.h"
#include "kwaj_builder.h"
#include "md5.h"
#include "module_builder.h"
#include "szdd_builder.h"
#include "test_util.h"
#include "zip_builder.h"

namespace test {

using Tree = std::map<std::string, std::vector<uint8_t>>;

inline std::vector<uint8_t> vec(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }

// An NE (Classic) module named `name` importing `refs`; it exports MODULE,
// as every real one does (the catalog lists an NE only as its lane runs it).
inline std::vector<uint8_t> ne_module(const std::string& name, const std::vector<std::string>& refs) {
  NeSpec ne;
  ne.module_refs = refs;
  ne.exports = {"MODULE"};
  ne.resources = {{2000, "", 20, name + '\0'},
                  {2000, "", 30, "About " + name + "\r\n"},
                  {1000, "", 1, checkbox_record("Sound", 1)}};
  return vec(build_ne(ne));
}

// A PE32 (AD4) module described as `desc`, importing `imports`.
inline std::vector<uint8_t> pe_module(const std::string& desc, const std::vector<std::string>& imports,
                                      bool msvc = false) {
  PeSpec pe;
  pe.imports = imports;
  pe.exports = {msvc ? "_Module@4" : "Module"};
  pe.resources = {{16, "", 1, 0x409, version_resource({{"FileDescription", desc}})},
                  {1000, "", 1, 0x409, checkbox_record("Clear Screen", 1)}};
  return vec(build_pe(pe));
}

inline std::vector<uint8_t> blob(const std::string& tag, size_t n = 600) {
  std::vector<uint8_t> v = vec(tag + ":");
  auto p = pattern(n, uint32_t(std::hash<std::string>{}(tag)));
  v.insert(v.end(), p.begin(), p.end());
  return v;
}

// Modules two releases share byte for byte (catalog sameAs).
inline const std::vector<uint8_t>& shared_classic() {
  static const auto v = ne_module("Same Module", {"KERNEL"});
  return v;
}
inline const std::vector<uint8_t>& shared_starry() {
  static const auto v = pe_module("Starry Night Display", {"KERNEL32.DLL"}, true);
  return v;
}
inline const std::vector<uint8_t>& shared_toilet40() {
  static const auto v = ne_module("Flying Toilets", {"KERNEL", "ADXPL40"});
  return v;
}

// An InstallShield-like script: length-prefixed strings among opcode bytes,
// the password (when given) right after the unzip failure message.
inline std::vector<uint8_t> install_ins(const std::string& password) {
  std::vector<uint8_t> s = {0xFF, 0xFF, 0x0C, 0x00};
  auto lp = [&](const std::string& t) {
    s.push_back(uint8_t(t.size()));
    s.push_back(uint8_t(t.size() >> 8));
    s.insert(s.end(), t.begin(), t.end());
    s.push_back(0x2E);  // a printable opcode right after, as the real scripts have
  };
  for (const char* d : {"SRCDIR", "TARGETDIR", "Welcome to setup", "C:\\AFTERDRK", "Decompressing engine..."}) lp(d);
  lp("Cannot initialize for unzip!");
  s.insert(s.end(), {0x03, 0x00, 0xFF, 0xFF, 0x0E, 0x00});
  if (!password.empty()) lp(password);
  for (const char* d : {"engine.zip", "Creating module folders", "folder.afi", "MUSICG.ZIP"}) lp(d);
  return s;
}

struct PkgFixture {
  Tree source;                        // as on the medium ('/'-separated)
  std::map<std::string, std::string> joliet;  // source path -> Joliet name (ISO only)
  Tree expect;                        // installed files, relative to <win> (import.json excluded)
  std::vector<std::string> ids;       // catalog ids, in catalog order
};

// ---- the plain CDs ----------------------------------------------------------------

inline PkgFixture deluxe_fixture() {
  PkgFixture f;
  auto both = [&](const std::string& rel, std::vector<uint8_t> d) {
    f.source["ADE/FILES/" + rel] = d;
    f.expect["FILES/" + rel] = std::move(d);
  };
  both("AD40/ADXPL510.DLL", blob("deluxe ADXPL510"));
  both("AD40/BADDOG.AD", pe_module("Bad Dog!", {"ADXPL510.DLL", "KERNEL32.DLL"}));
  both("AD40/TOASTERS.AD", pe_module("Flying Toasters!", {"ADXPL510.DLL", "USER32.DLL"}));
  both("AD40/TOASTERS.MID", blob("deluxe toasters mid"));
  both("CLASSIC/ADXPL300.DLL", blob("deluxe ADXPL300"));
  both("CLASSIC/TOILETS.AD", ne_module("Flying Toilets", {"KERNEL", "ADXPL300"}));
  both("CLASSIC/SAMEMOD.AD", shared_classic());
  both("ENGINE/OLDMOD16.DLL", blob("deluxe OLDMOD16"));
  both("ENGINE/AD_SND.DLL", blob("deluxe AD_SND 4.0"));
  both("ENGINE/AFTERDAR.SCR", blob("deluxe AFTERDAR"));
  both("ENGINE/STARRYNI.AD", shared_starry());
  both("AFI/AD2.AFI", blob("deluxe AD2.AFI"));
  f.source["ADE/FILES/WALLPAPR/W.BMP"] = blob("wallpaper");
  f.source["ADE/SETUP.INF"] = blob("setup inf");
  f.ids = {"ad40.baddog", "ad40.toasters", "ad40.starryni", "classic.samemod", "classic.toilets"};
  return f;
}

// `with_fixups`: the four §4.3 copies are expected (their sources matched).
inline PkgFixture ad10_fixture(bool with_fixups = true) {
  PkgFixture f;
  auto both = [&](const std::string& rel, std::vector<uint8_t> d) {
    f.source["ADE/FILES/" + rel] = d;
    f.expect["packages/ad10/" + rel] = std::move(d);
  };
  both("AD10TH/ADXPL510.DLL", blob("ad10 ADXPL510 5.2"));
  both("AD10TH/ADXPL300.DLL", blob("ad10 ADXPL300"));
  both("AD10TH/ADXPL40.DLL", blob("ad10 ADXPL40"));
  both("AD10TH/ADTOOL.DLL", blob("ad10 ADTOOL"));
  both("AD10TH/BADDOG.AD", pe_module("Bad Dog!", {"ADXPL510.DLL", "KERNEL32.DLL", "SHELL32.DLL"}));
  both("AD10TH/BADDOG3.AD", ne_module("Bad Dog!", {"KERNEL", "ADXPL300", "ADTOOL"}));
  both("AD10TH/TOAST2K.AD", pe_module("Toasters 2k", {"KERNEL32.DLL", "WINMM.DLL"}, true));
  both("AD10TH/TOASTER2.AD", pe_module("Toasters 2k", {"KERNEL32.DLL"}, true));
  both("AD10TH/TOASTERS.AD", pe_module("Flying Toasters!", {"ADXPL510.DLL"}));
  both("AD10TH/TOILET.AD", shared_toilet40());
  both("AD10TH/TOASTER1.MID", blob("toasters 2k music"));
  both("AD10TH/TOASTERS.MID", blob("flying toasters music"));
  both("AD10TH/BABY.MID", blob("baby toasters music"));
  both("AD10TH/MUSIC/TT_SND.DLL", blob("tt shared sounds"));
  both("AD10TH/MUSIC/DAWN.MID", blob("dawn"));
  both("AD10TH/PICTURES/SOMEPICT.BMP", blob("a picture"));
  f.joliet["ADE/FILES/AD10TH/PICTURES/SOMEPICT.BMP"] = "Some Picture.bmp";
  f.joliet["ADE/FILES/AD10TH/TOASTER2.AD"] = "Toaster 2k.ad";
  both("ENGINE/OLDMOD16.DLL", blob("ad10 OLDMOD16"));
  both("ENGINE/AD_SND.DLL", blob("ad10 AD_SND 4.0"));
  both("ENGINE/AFTERDAR.SCR", blob("ad10 AFTERDAR"));
  both("ENGINE/STARRYNI.AD", shared_starry());
  both("AFI/AD2.AFI", blob("ad10 AD2.AFI"));
  f.source["ADE/FILES/GAMES/BAD_DOG.EXE"] = blob("a game");
  f.source["ADE/FILES/WALLPAPR/W.BMP"] = blob("wallpaper 10");
  f.source["ADE/SETUP.INF"] = blob("setup inf 10");
  f.source["DIRECTX6/DX.EXE"] = blob("directx");
  if (with_fixups) {
    f.expect["packages/ad10/AD10TH/TT_SND.DLL"] = f.expect["packages/ad10/AD10TH/MUSIC/TT_SND.DLL"];
    f.expect["packages/ad10/AD10TH/MUSIC/Toasters2k.mid"] = f.expect["packages/ad10/AD10TH/TOASTER1.MID"];
    f.expect["packages/ad10/AD10TH/MUSIC/Flying Toasters.mid"] = f.expect["packages/ad10/AD10TH/TOASTERS.MID"];
    f.expect["packages/ad10/AD10TH/MUSIC/Baby Toasters.mid"] = f.expect["packages/ad10/AD10TH/BABY.MID"];
  }
  f.ids = {"ad10.baddog", "ad10.baddog3", "ad10.toast2k", "ad10.toaster2", "ad10.toasters", "ad10.toilet",
           "ad10.starryni"};
  return f;
}

// ---- the AD 3.x InstallShield installs --------------------------------------------------

struct Ad3Parts {
  std::string install;  // "INSTALL" or "" (floppy root)
  std::string root;     // "packages/<id>"
  std::string mdir;     // module dir
  std::string password = kTestZipPassword;
  PkgFixture f;

  std::string src(const std::string& name) const { return install.empty() ? name : install + "/" + name; }
  void zip(const std::string& name, const std::vector<std::pair<std::string, std::vector<uint8_t>>>& members,
           bool stored_small = true) {
    ZipBuilder b;
    b.password = password;
    for (const auto& [n, d] : members) b.add(n, d, /*deflate=*/!(stored_small && d.size() < 64));
    f.source[src(name)] = b.build();
  }
  void expect(const std::string& rel, const std::vector<uint8_t>& d) { f.expect[root + "/" + rel] = d; }
  void engine_zip() {
    auto snd = blob(root + " AD_SND 3.x"), task = blob(root + " ADTASK"), exe = blob(root + " ADW30.EXE"),
         ini = blob(root + " ADW30.INI", 60), eco = blob(root + " ECOLOGIC");
    zip("ENGINE.ZIP", {{"ADSETUP.DLL", blob("adsetup")},
                       {"AD_SND.DLL", snd},
                       {"ADTASK.DLL", task},
                       {"ADW30.EXE", exe},
                       {"ADW30.INI", ini},
                       {"ECOLOGIC.DLL", eco},
                       {"ADHOOK.DLL", blob("adhook")},
                       {"MULTI.AM3", pattern(16, 77)},
                       {"RANDOM.AR3", pattern(21, 78)}});
    for (auto& [n, d] : std::vector<std::pair<std::string, std::vector<uint8_t>>>{
             {"AD_SND.DLL", snd}, {"ADTASK.DLL", task}, {"ADW30.EXE", exe}, {"ADW30.INI", ini}, {"ECOLOGIC.DLL", eco}})
      expect("ENGINE/" + n, d);
  }
  void installer_files(const std::vector<std::string>& disks) {
    f.source[src("INSTALL.INS")] = install_ins(password);
    f.source[src("SETUP.PKG")] = vec("[Package]\r\nFiles=...\r\n");
    f.source[src("SETUP.EXE")] = blob("installshield launcher");
    for (const auto& d : disks) f.source[src(d)] = vec("After Dark " + d);
  }
  // A module archive: every member goes beside the modules.
  void module(const std::string& zipname, const std::vector<std::pair<std::string, std::vector<uint8_t>>>& members) {
    zip(zipname, members);
    for (const auto& [n, d] : members) expect(mdir + "/" + n, d);
  }
};

// The folder files an AD 3.x AFI.ZIP carries besides the release's own: the
// other products' folders, as on the real disks (every one has DISNEY.AFI,
// three have AD3.AFI), so the fingerprint must tell them from the release's.
// Made-up bytes, one blob per name and `tag`.
inline std::vector<std::pair<std::string, std::vector<uint8_t>>> afi_members(const std::string& tag,
                                                                             const std::vector<std::string>& names) {
  std::vector<std::pair<std::string, std::vector<uint8_t>>> m;
  for (const std::string& n : names) m.push_back({n, blob(tag + " " + n)});
  return m;
}

inline PkgFixture ad32_fixture() {
  Ad3Parts p;
  p.install = "INSTALL";
  p.root = "packages/ad32";
  p.mdir = "AD32";
  p.installer_files({"DISK.1", "DISK.CD"});
  p.engine_zip();
  auto xpl = blob("ad32 ADXPL300"), tool = blob("ad32 ADTOOL"), adc = blob("bitmaps adc"), rsrc = blob("AD_RSRC"),
       rsdb = blob("ad32 AD30RSDB");
  // What 3.2's MODMISC.ZIP holds (by name): AD30RSDB.DLL is its marker
  // beside ADXPL300.DLL (ScreamSavers ships the same engine library).
  std::vector<std::pair<std::string, std::vector<uint8_t>>> modmisc = {{"ADXPL300.DLL", xpl},
                                                                       {"AD30RSDB.DLL", rsdb},
                                                                       {"ADTOOL.DLL", tool},
                                                                       {"BITMAPS.ADC", adc},
                                                                       {"EDITFILE.TXT", blob("edit")}};
  for (const char* n : {"DJPG.DLL", "DTARGA.DLL", "MESG_AD3.DAT", "NONSENSE.TXT", "READBMP.DLL", "READFILE.DLL",
                        "READGIF.DLL", "READMMP.DLL", "READPCX.DLL", "STOIKDTH.DLL"}) {
    modmisc.push_back({n, blob(std::string("ad32 ") + n)});
    p.expect(std::string("AD32/") + n, modmisc.back().second);
  }
  p.zip("MODMISC.ZIP", modmisc);
  p.expect("AD32/ADXPL300.DLL", xpl);
  p.expect("AD32/AD30RSDB.DLL", rsdb);
  p.expect("AD32/ADTOOL.DLL", tool);
  p.expect("AD32/BITMAPS.ADC", adc);
  p.zip("WIN.ZIP", {{"AD_RSRC.DLL", rsrc}, {"UNLINK.EXE", blob("unlink")}});
  p.expect("AD32/AD_RSRC.DLL", rsrc);
  auto logo = blob("adlogo"), trc = blob("diamond trace"), ding = blob("ding wav"), mg = blob("omtw gm"),
       afi = blob("AD3.AFI");
  p.zip("BITMAPS.ZIP", {{"ADLOGO.BMP", logo}});
  p.expect("AD32/BITMAPS/ADLOGO.BMP", logo);
  p.zip("TRACES.ZIP", {{"DIAMOND.TRC", trc}});
  p.expect("AD32/TRACES/DIAMOND.TRC", trc);
  p.zip("SOUNDS.ZIP", {{"DING.WAV", ding}});
  p.expect("AD32/SOUNDS/DING.WAV", ding);
  p.zip("MUSIC.ZIP", {{"OMTW.MID", blob("omtw dual format")}});
  p.zip("MUSICG.ZIP", {{"OMTW.MID", mg}});
  p.expect("AD32/MUSIC/OMTW.MID", mg);
  auto afis = afi_members("ad32", {"AD2.AFI", "DISNEY.AFI", "MAD.AFI", "MARVEL.AFI", "STARTREK.AFI", "STUMP.AFI"});
  afis.push_back({"AD3.AFI", afi});
  p.zip("AFI.ZIP", afis);
  p.expect("AD32/FOLDER.AFI", afi);
  p.zip("HELP.ZIP", {{"ADW30.HLP", blob("help")}});
  p.zip("MULTIS.ZIP", {{"CLOCK_AT.AM3", blob("multi")}});
  p.zip("WINSYS.ZIP", {{"PLACE.TXT", pattern(30, 5)}});
  p.zip("EXTRA.ZIP", {{"README.TXT", blob("not part of the recipe")}});
  p.module("TOILET.ZIP", {{"TOILET.AD", ne_module("Flying Toilets", {"KERNEL", "ADXPL300", "ADTOOL"})}});
  p.module("BORIS.ZIP", {{"BORIS.AD", ne_module("Boris", {"KERNEL", "AD_RSRC", "AD_SND"})}});
  p.module("BORISB.ZIP", {{"BORISB.AD", ne_module("Boris", {"KERNEL"})}});
  p.module("GUTS.ZIP", {{"GUTS.AD", ne_module("Guts", {"KERNEL"})}});
  p.module("GUTS2.ZIP", {{"GUTS2.AD", ne_module("guts", {"KERNEL", "USER"})}});
  p.module("GUTS3.ZIP", {{"GUTS3.AD", ne_module("Guts", {"KERNEL", "GDI"})}});
  p.module("SAME.ZIP", {{"SAME.AD", shared_classic()}});
  p.module("WMORPH.ZIP",
           {{"WMORPH.AD", ne_module("Draw Morph", {"KERNEL"})}, {"MORPH1.DAT", blob("m1")}, {"MORPH2.DAT", blob("m2")}});
  // A name the Looney Tunes' Messages repeats (it becomes "Messages (Looney Tunes)").
  p.module("MESSAGES.ZIP", {{"MESSAGES.AD", ne_module("Messages", {"KERNEL", "ADXPL300"})}});
  p.f.source["DEMOS/JACK/SETUP.EXE"] = blob("a demo");
  p.f.ids = {"ad32.boris", "ad32.borisb", "ad32.guts",  "ad32.guts2", "ad32.guts3",
             "ad32.messages", "ad32.same", "ad32.toilet", "ad32.wmorph"};
  return p.f;
}

inline PkgFixture tt_fixture() {
  Ad3Parts p;
  p.install = "INSTALL";
  p.root = "packages/tt";
  p.mdir = "TWISTED";
  p.installer_files({"DISK.1", "DISK.2", "DISK.3", "DISK.CD"});
  p.engine_zip();
  auto xpl = blob("tt ADXPL40"), snd = blob("TT_SND"), mg = blob("dawn gm"), rsrc = blob("AD_RSRC"), afi = blob("PHLEM");
  p.zip("MODMISC.ZIP", {{"ADXPL40.DLL", xpl}});
  p.expect("TWISTED/ADXPL40.DLL", xpl);
  p.zip("MUSIC.ZIP", {{"DAWN.MID", blob("dawn dual")}, {"TT_SND.DLL", snd}});
  p.zip("MUSICG.ZIP", {{"DAWN.MID", mg}, {"TT_SND.DLL", snd}});
  p.expect("TWISTED/MUSIC/DAWN.MID", mg);
  p.expect("TWISTED/TT_SND.DLL", snd);
  p.zip("WIN.ZIP", {{"AD_RSRC.DLL", rsrc}, {"UNLINK.EXE", blob("unlink")}});
  p.expect("TWISTED/AD_RSRC.DLL", rsrc);
  auto afis = afi_members("tt", {"AD2.AFI", "DISNEY.AFI", "MAD.AFI", "MARVEL.AFI", "STARTREK.AFI", "STUMP.AFI"});
  afis.push_back({"PHLEM.AFI", afi});
  p.zip("AFI.ZIP", afis);
  p.expect("TWISTED/FOLDER.AFI", afi);
  p.zip("WAVEMIX.ZIP", {{"MSACM.DLL", blob("msacm")}});
  p.zip("WINSYS.ZIP", {{"PLACE.TXT", pattern(30, 6)}});
  p.module("TOILET.ZIP", {{"TOILET.AD", shared_toilet40()}});
  p.module("MESY.ZIP", {{"MESSYGES.AD", ne_module("Message Mayhem", {"KERNEL", "ADXPL40"})}});
  p.module("CHAM.ZIP", {{"CHAM.AD", ne_module("Chameleon", {"ADXPL40", "KERNEL", "USER"})}});
  p.f.ids = {"tt.cham", "tt.messyges", "tt.toilet"};
  return p.f;
}

// The Simpsons: both floppies' files in one root (as the merged image has
// them), plus the original owner's notes, which the importer must never read.
struct SimpsonsModule {
  const char* zip;
  const char* file;
  const char* name;
};
inline const std::vector<SimpsonsModule>& simpsons_modules() {
  static const std::vector<SimpsonsModule> v = {
      {"BURNS.ZIP", "BURNS.AD", "Mr. Burns"},          {"CHALKBRD.ZIP", "CHALKBRD.AD", "Chalkboard"},
      {"CLOCKS.ZIP", "SIMPCLOK.AD", "Simpsons Clocks"}, {"GRAFFITI.ZIP", "OBJETS.AD", "Objets B'art"},
      {"GRANDPA.ZIP", "GRAMPA.AD", "Grampa's Wisdom "}, {"GRASSKRT.ZIP", "GRASSKRT.AD", "Grass Skirts"},
      {"HOMEREAT.ZIP", "HOMEREAT.AD", "Homer Eats"},    {"HOW2DRAW.ZIP", "HOW2DRAW.AD", "How To Draw"},
      {"INS.ZIP", "INS.AD", "Itchy & Scratchy"},        {"KRUSTY.ZIP", "KRUSTY.AD", "Krusty"},
      {"MLISA.ZIP", "LISA.AD", "Lisa's Mood Swings"},   {"PHYSICS.ZIP", "PHYSICS.AD", "Physics"},
      {"SFILES.ZIP", "SIMPFILE.AD", "Files"},           {"SNOWBALL.ZIP", "SNOWBALL.AD", "Snowball I "},
      {"STRIVIA.ZIP", "SIMPTRIV.AD", "Simpsons Trivia"},
  };
  return v;
}

// Which of the two install floppies each root file was on (the real split).
inline int simpsons_disk(const std::string& name) {
  for (const char* d2 : {"SETUP.PKG", "DISK.2", "MODMISC.ZIP", "MUSIC.ZIP", "HELP.ZIP", "WINSYS.ZIP", "BURNS.ZIP",
                         "CHALKBRD.ZIP", "CLOCKS.ZIP", "GRANDPA.ZIP", "GRASSKRT.ZIP", "KRUSTY.ZIP", "SFILES.ZIP",
                         "SERIAL.TXT"})
    if (name == d2) return 2;
  return 1;
}

inline PkgFixture simpsons_fixture() {
  Ad3Parts p;
  p.install = "";
  p.root = "packages/simpsons";
  p.mdir = "SIMPSONS";
  p.installer_files({"DISK.1", "DISK.2"});
  p.engine_zip();
  auto xpl = blob("ADXPL310"), snd = blob("SIMP_SND", 3000), rsrc = blob("AD_RSRC"), afi = blob("SAX.AFI"),
       m1 = blob("simpsons mid"), m2 = blob("i&s show");
  p.zip("MODMISC.ZIP", {{"ADXPL310.DLL", xpl}, {"EDITFILE.TXT", blob("edit")}});
  p.expect("SIMPSONS/ADXPL310.DLL", xpl);
  p.zip("MUSIC.ZIP", {{"SIMPSONS.MID", m1}, {"I&SSHOW.MID", m2}, {"SIMP_SND.DLL", snd}});
  p.expect("SIMPSONS/MUSIC/SIMPSONS.MID", m1);
  p.expect("SIMPSONS/MUSIC/I&SSHOW.MID", m2);
  p.expect("SIMPSONS/SIMP_SND.DLL", snd);
  p.zip("WIN.ZIP", {{"SPALETTE.DLL", blob("spalette")}, {"AD_RSRC.DLL", rsrc}});
  p.expect("SIMPSONS/AD_RSRC.DLL", rsrc);
  p.zip("WINSYS.ZIP", {{"SPMME.DRV", blob("spmme")}});
  auto afis = afi_members("simpsons", {"AD2.AFI", "AD3.AFI", "DISNEY.AFI", "MAD.AFI", "MARVEL.AFI", "STARTREK.AFI",
                                       "STUMP.AFI"});
  afis.push_back({"SAX.AFI", afi});
  p.zip("AFI.ZIP", afis);
  p.expect("SIMPSONS/FOLDER.AFI", afi);
  p.zip("HELP.ZIP", {{"SIMPSONS.HLP", blob("help")}});
  for (const SimpsonsModule& m : simpsons_modules()) p.module(m.zip, {{m.file, ne_module(m.name, {"KERNEL", "ADXPL310"})}});
  p.f.source["CEREAL.TXT"] = blob("the owner's notes");
  p.f.source["SERIAL.TXT"] = blob("a serial number", 3000);
  p.f.ids = {"simpsons.burns",    "simpsons.chalkbrd", "simpsons.grampa",   "simpsons.grasskrt", "simpsons.homereat",
             "simpsons.how2draw", "simpsons.ins",      "simpsons.krusty",   "simpsons.lisa",     "simpsons.objets",
             "simpsons.physics",  "simpsons.simpclok", "simpsons.simpfile", "simpsons.simptriv", "simpsons.snowball"};
  return p.f;
}

// ---- the AD 3.x releases known by the ZIP of their install files ----------------------------
//
// The Looney Tunes, ScreamSavers and the Disney Collection: the same
// installer as the three above, each with its own engine library in
// MODMISC.ZIP and folder file in AFI.ZIP (ScreamSavers' engine library is 3.2's
// ADXPL300.DLL, and its AFI.ZIP holds an AD3.AFI too), every module archive
// the registry lists, and the files a copy of the disks holds besides: the
// installer's, and the owner's note the user's copy has, which the importer
// must never read. The ids of a fixture's modules follow its module list.

struct Ad3Module {
  const char* zip;
  const char* file;
  const char* name;  // as the name resource holds it (spaces kept: the catalog trims them)
};

inline std::vector<std::string> module_ids(const char* package, const std::vector<Ad3Module>& modules) {
  std::vector<std::string> ids;
  for (const Ad3Module& m : modules) {
    std::string stem = m.file;
    stem = stem.substr(0, stem.find('.'));
    for (char& c : stem) c = char(tolower((unsigned char)c));
    ids.push_back(std::string(package) + "." + stem);
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

// The Looney Tunes: 12 modules on ADXPL41, a sound database named LT_SOUND.DLL
// in MUSIC.ZIP with the General MIDI files (no MUSICG.ZIP), both floppies'
// files at the root, as the user's ZIP and the CD hold them.
inline const std::vector<Ad3Module>& looney_modules() {
  static const std::vector<Ad3Module> v = {
      {"ACMESHOP.ZIP", "ACMESHOP.AD", "ACME Home Shopping"}, {"CART101.ZIP", "CART101.AD", "Cartoons 101"},
      {"CONDUCKT.ZIP", "CONDUCKT.AD", "Conducktor"},        {"FROG.ZIP", "FROG.AD", "Michigan J. Frog "},
      {"LTMESSGS.ZIP", "LTMESSGS.AD", "Messages"},          {"MARVIN.ZIP", "MARVIN.AD", "Marvin's Invasion"},
      {"PEPE.ZIP", "PEPE.AD", "Desquetoppe D\x92" "amour"}, {"PUTTYTAT.ZIP", "PUTTYTAT.AD", "Putty Tat Splat!"},
      {"RABBITRN.ZIP", "RABBITRN.AD", "Rabbitron"},         {"SAM.ZIP", "SAM.AD", "Yosemite Sam"},
      {"TAZ.ZIP", "TAZ.AD", "Taz Desktop"},                 {"WOCKETS.ZIP", "WOCKETS.AD", "Wockets' Wed Gware "},
  };
  return v;
}

// Which of the two install floppies each root file was on (SETUP.PKG's split).
inline int looney_disk(const std::string& name) {
  for (const char* d2 : {"PUTTYTAT.ZIP", "MUSIC.ZIP", "MODMISC.ZIP", "ENGINE.ZIP", "DISK.2"})
    if (name == d2) return 2;
  return 1;
}

// The owner's note in the user's copy (a long name: no floppy holds it).
inline constexpr char kLooneyNote[] = "looneyAD_sn.txt";

inline PkgFixture looney_fixture() {
  Ad3Parts p;
  p.install = "";
  p.root = "packages/looney";
  p.mdir = "LNYTUNES";
  p.installer_files({"DISK.1", "DISK.2", "DISK.CD"});
  p.f.source["SETUP.BMP"] = blob("looney setup splash", 2000);
  for (const char* f : {"CHANGES.TXT", "README.TXT", "DUNZIP.DLL", "INS0762.LIB"}) p.f.source[f] = blob(f, 300);
  p.engine_zip();
  auto xpl = blob("ADXPL41"), snd = blob("LT_SOUND", 3000), rsrc = blob("AD_RSRC"), afi = blob("LNYTUNES.AFI"),
       m1 = blob("acme mid"), m2 = blob("gb&u mid");
  p.zip("MODMISC.ZIP", {{"ADXPL41.DLL", xpl}});
  p.expect("LNYTUNES/ADXPL41.DLL", xpl);
  p.zip("MUSIC.ZIP", {{"ACME.MID", m1}, {"GB&U.MID", m2}, {"LT_SOUND.DLL", snd}});
  p.expect("LNYTUNES/MUSIC/ACME.MID", m1);
  p.expect("LNYTUNES/MUSIC/GB&U.MID", m2);
  p.expect("LNYTUNES/LT_SOUND.DLL", snd);
  p.zip("WIN.ZIP", {{"SPALETTE.DLL", blob("spalette")}, {"AD_RSRC.DLL", rsrc}, {"CLEANAD.BAT", blob("cleanad")},
                    {"UNLINK.EXE", blob("unlink")}});
  p.expect("LNYTUNES/AD_RSRC.DLL", rsrc);
  p.zip("WINSYS.ZIP", {{"SPMME.DRV", blob("spmme")}});
  auto afis = afi_members("looney", {"AD2.AFI", "DISNEY.AFI", "MAD.AFI", "MARVEL.AFI", "STARTREK.AFI", "STUMP.AFI"});
  afis.push_back({"LNYTUNES.AFI", afi});
  p.zip("AFI.ZIP", afis);
  p.expect("LNYTUNES/FOLDER.AFI", afi);
  p.zip("HELP.ZIP", {{"LNYTUNES.HLP", blob("help")}});
  for (const Ad3Module& m : looney_modules()) p.module(m.zip, {{m.file, ne_module(m.name, {"KERNEL", "ADXPL41"})}});
  p.f.source[kLooneyNote] = blob("the owner's note", 19);
  p.f.ids = module_ids("looney", looney_modules());
  return p.f;
}

// ScreamSavers: 15 modules that import only AD_SND, 3.2's ADXPL300.DLL in
// MODMISC.ZIP (installed, used by nothing) and an AD3.AFI beside SCREAMS.AFI
// in AFI.ZIP; three floppies (screams_disk), the owner's note on disk 1.
inline const std::vector<Ad3Module>& screams_modules() {
  static const std::vector<Ad3Module> v = {
      {"AMPHIBO.ZIP", "AMPHIBO.AD", "Swamp Lunch"},   {"BELCHO.ZIP", "BELCHO.AD", "Wake Up"},
      {"BUGZAP.ZIP", "BUGZAP.AD", "Bug Out"},         {"GRISTLE.ZIP", "GRISTLE.AD", "Gristle Slam"},
      {"HEADBUTT.ZIP", "HEADBUTT.AD", "Head Butt"},   {"INFECTO.ZIP", "INFECTO.AD", "Infecto"},
      {"LOCKJAW.ZIP", "LOCKJAW.AD", "All Tied Up"},   {"MALIGNO.ZIP", "MALIGNO.AD", "Melt Down"},
      {"MELTICOR.ZIP", "MELTICOR.AD", "Big Mess"},    {"MOONBITE.ZIP", "MOONBITE.AD", "Rip Out"},
      {"PUPPY.ZIP", "PUPPY.AD", "Bone Crunch"},       {"SNAPPY.ZIP", "SNAPPY.AD", "Blooming Chow"},
      {"SPEWER.ZIP", "SPEWER.AD", "Slime Bath"},      {"STICKY.ZIP", "STICKY.AD", "Sticky Tongue"},
      {"TWISTER.ZIP", "TWISTER.AD", "Spin Out"},
  };
  return v;
}

// Which install floppy each file is on: the installer, the shared archives
// and two modules on disk 1, five modules on disk 2, eight on disk 3.
inline int screams_disk(const std::string& name) {
  for (const char* d2 : {"INFECTO.ZIP", "LOCKJAW.ZIP", "MALIGNO.ZIP", "MELTICOR.ZIP", "MOONBITE.ZIP", "DISK.2"})
    if (name == d2) return 2;
  for (const char* d3 : {"AMPHIBO.ZIP", "BUGZAP.ZIP", "GRISTLE.ZIP", "HEADBUTT.ZIP", "SNAPPY.ZIP", "SPEWER.ZIP",
                         "STICKY.ZIP", "TWISTER.ZIP", "DISK.3"})
    if (name == d3) return 3;
  return 1;
}

// The owner's note on the user's disk 1.
inline constexpr char kScreamsNote[] = "REG'D.TXT";

// `ad32_marker`: MODMISC.ZIP also holds 3.2's marker (a crafted source that
// is both releases).
inline PkgFixture screams_fixture(bool ad32_marker = false) {
  Ad3Parts p;
  p.install = "";
  p.root = "packages/screams";
  p.mdir = "SCREAMS";
  p.installer_files({"DISK.1", "DISK.2", "DISK.3"});
  for (const char* f : {"CHANGES.TXT", "~INS0762.LIB"}) p.f.source[f] = blob(f, 300);
  p.engine_zip();
  auto xpl = blob("screams ADXPL300"), rsrc = blob("AD_RSRC"), afi = blob("SCREAMS.AFI");
  std::vector<std::pair<std::string, std::vector<uint8_t>>> modmisc = {{"ADXPL300.DLL", xpl},
                                                                       {"EDITFILE.TXT", blob("edit")}};
  if (ad32_marker) {
    modmisc.push_back({"AD30RSDB.DLL", blob("a marker")});
    p.expect("SCREAMS/AD30RSDB.DLL", modmisc.back().second);
  }
  p.zip("MODMISC.ZIP", modmisc);
  p.expect("SCREAMS/ADXPL300.DLL", xpl);
  p.zip("WIN.ZIP", {{"AD_RSRC.DLL", rsrc}, {"SPALETTE.DLL", blob("spalette")}, {"AD_GRAPH.TXT", blob("ad graph")}});
  p.expect("SCREAMS/AD_RSRC.DLL", rsrc);
  p.zip("WINSYS.ZIP", {{"SPMME.DRV", blob("spmme")}});
  auto afis = afi_members("screams", {"AD2.AFI", "AD3.AFI", "DISNEY.AFI", "MAD.AFI", "MARVEL.AFI", "STARTREK.AFI",
                                      "STUMP.AFI"});
  afis.push_back({"SCREAMS.AFI", afi});
  p.zip("AFI.ZIP", afis);
  p.expect("SCREAMS/FOLDER.AFI", afi);
  p.zip("HELP.ZIP", {{"SCREAMS.HLP", blob("help")}});
  for (const Ad3Module& m : screams_modules())
    p.module(m.zip, {{m.file, ne_module(m.name, {"KERNEL", "GDI", "USER", "AD_SND"})}});
  p.f.source[kScreamsNote] = blob("the owner's note", 16);
  p.f.ids = module_ids("screams", screams_modules());
  return p.f;
}

// The Disney Collection: 16 modules on ADXPL100 and AD_RSRC, whose name
// resources hold two leading spaces (five of them squeezed a space out),
// the sound library DIS_SND.DLL in MODMISC.ZIP, MUSIC.ZIP and MUSICG.ZIP, the
// 1993 build of a module in BEAUTYOL.ZIP (never opened: were it read, its
// BEAUTY.AD would be a second file for one path), and the copy's files in
// mixed case, as the user's ZIP holds them.
inline const std::vector<Ad3Module>& disney_modules() {
  static const std::vector<Ad3Module> v = {
      {"Beauty.zip", "BEAUTY.AD", "  Beauty"},          {"Checat.zip", "CHECAT.AD", "  Cheshire Cat"},
      {"Dalm.zip", "DALM.AD", "  101Dalmatians"},       {"Donduk.zip", "DONDUK.AD", "  Donald Paints"},
      {"Dsclocks.zip", "DSCLOCKS.AD", "  DisneyClocks"}, {"Falling.zip", "FALLING.AD", "  FallingFlower"},
      {"Firewrk.zip", "FIREWRK.AD", "  MagicKingdom"},  {"Goofy.zip", "GOOFY.AD", "  Goofy"},
      {"Haunted.zip", "HAUNTED.AD", "  Haunted"},       {"Hook.zip", "HOOK.AD", "  Captain Hook"},
      {"Inkwell.zip", "INKWELL.AD", "  Digital Ink"},   {"Jungle.zip", "JUNGLE.AD", "  Jungle Book"},
      {"Mermaid.zip", "MERMAID.AD", "  LittleMermaid"}, {"Pinocchi.zip", "PINOCCHI.AD", "  Pinocchio"},
      {"Scrooge.zip", "SCROOGE.AD", "  Scrooge"},       {"Sorcerer.zip", "SORCERER.AD", "  The Sorcerer"},
  };
  return v;
}

// Which of the three install floppies each file was on (SETUP.PKG's split;
// names compared without case).
inline int disney_disk(const std::string& name) {
  std::string n = name;
  for (char& c : n) c = char(toupper((unsigned char)c));
  for (const char* d2 : {"HELP.ZIP", "MODMISC.ZIP", "MUSIC.ZIP", "MUSICG.ZIP", "AFI.ZIP", "WIN.ZIP", "ENGINE.ZIP",
                         "WINSYS.ZIP", "BEAUTY.ZIP", "INKWELL.ZIP", "SORCERER.ZIP", "DISK.2"})
    if (n == d2) return 2;
  for (const char* d3 : {"CHECAT.ZIP", "GOOFY.ZIP", "HAUNTED.ZIP", "MERMAID.ZIP", "BEAUTYOL.ZIP", "DISK.3"})
    if (n == d3) return 3;
  return 1;
}

// The owner's note in the user's copy.
inline constexpr char kDisneyNote[] = "Dizny_sn.txt";

inline PkgFixture disney_fixture() {
  Ad3Parts p;
  p.install = "";
  p.root = "packages/disney";
  p.mdir = "DISNEY";
  // The installer's files, in the copy's mixed case.
  p.f.source["Install.ins"] = install_ins(p.password);
  p.f.source["Setup.pkg"] = vec("[Package]\r\nFiles=...\r\n");
  p.f.source["Setup.exe"] = blob("installshield launcher");
  p.f.source["Setup.bmp"] = blob("disney setup splash", 2000);
  for (const char* f : {"Changes.txt", "Cheksums.new", "Disk.1", "Disk.2", "Disk.3", "Disk.cd", "Dunzip.dll",
                        "Ins0762.lib", "Readme.txt", "Site.stp"})
    p.f.source[f] = blob(f, 300);
  p.engine_zip();
  // engine_zip() wrote ENGINE.ZIP: as the copy spells it.
  p.f.source["Engine.zip"] = p.f.source["ENGINE.ZIP"];
  p.f.source.erase("ENGINE.ZIP");
  auto xpl = blob("ADXPL100"), snd = blob("DIS_SND", 2000), rsrc = blob("AD_RSRC"), afi = blob("DISNEY.AFI"),
       mg = blob("bandb gm");
  p.zip("Modmisc.zip", {{"EDITFILE.TXT", blob("edit")}, {"ADXPL100.DLL", xpl}, {"DIS_SND.DLL", snd}});
  p.expect("DISNEY/ADXPL100.DLL", xpl);
  p.expect("DISNEY/DIS_SND.DLL", snd);
  p.zip("Music.zip", {{"BANDB.MID", blob("bandb dual")}});
  p.zip("Musicg.zip", {{"BANDB.MID", mg}});
  p.expect("DISNEY/MUSIC/BANDB.MID", mg);
  p.zip("Win.zip", {{"AD_RSRC.DLL", rsrc}, {"UNLINK.EXE", blob("unlink")}});
  p.expect("DISNEY/AD_RSRC.DLL", rsrc);
  p.zip("Winsys.zip", {{"PLACE.TXT", pattern(42, 7)}});
  auto afis = afi_members("disney", {"MAD.AFI", "MARVEL.AFI", "STARTREK.AFI", "STUMP.AFI", "AD2.AFI"});
  afis.push_back({"DISNEY.AFI", afi});
  p.zip("Afi.zip", afis);
  p.expect("DISNEY/FOLDER.AFI", afi);
  p.zip("Help.zip", {{"DISNEY.HLP", blob("help")}});
  for (const Ad3Module& m : disney_modules())
    p.module(m.zip, {{m.file, ne_module(m.name, {"KERNEL", "ADXPL100", "AD_RSRC"})}});
  p.zip("Beautyol.zip", {{"BEAUTY.AD", ne_module("  Beauty", {"KERNEL", "ADXPL100", "AD_RSRC", "USER"})}});
  p.f.source[kDisneyNote] = blob("the owner's note", 40);
  p.f.ids = module_ids("disney", disney_modules());
  return p.f;
}

// Star Trek: The Next Generation Screen Saver: 13 modules on ADXPL320, with an
// art and a sound library beside it in MODMISC.ZIP, MUSIC.ZIP's MIDI files
// (no MUSICG.ZIP), and an AFI.ZIP that also holds six other products' folder
// files; the install files at the CD's root.
inline const std::vector<Ad3Module>& tng_modules() {
  static const std::vector<Ad3Module> v = {
      {"DATA.ZIP", "DATA.AD", "Data Dances"},          {"ENC.ZIP", "ENC.AD", "Encounters"},
      {"NANITES.ZIP", "NANITES.AD", "Nanites"},        {"OFFREV.ZIP", "OFFREV.AD", "Officer's Review"},
      {"PERSFILE.ZIP", "PERSFILE.AD", "Personnel Files"}, {"STARBASE.ZIP", "STARBASE.AD", "Starbase"},
      {"STATIONS.ZIP", "STATIONS.AD", "Science Stations"}, {"TACHYON.ZIP", "TACHYON.AD", "Tachyon Particle Field"},
      {"THEBORG.ZIP", "THEBORG.AD", "The Borg"},       {"TNGMESG.ZIP", "TNGMESG.AD", "Starfleet Messages"},
      {"TROI.ZIP", "TROI.AD", "Counselor Troi"},       {"WARPEFCT.ZIP", "WARPEFCT.AD", "Warp Effect"},
      {"WORF.ZIP", "WORF.AD", "Worf's Weapons"},
  };
  return v;
}

// `engine`: MODMISC.ZIP holds ADXPL320.DLL. Without it the source is a decoy
// (ST-TNG.AFI is in its AFI.ZIP, but nothing else names the release).
inline PkgFixture tng_fixture(bool engine = true) {
  Ad3Parts p;
  p.install = "";
  p.root = "packages/tng";
  p.mdir = "ST-TNG";
  p.installer_files({"DISK.1", "DISK.2", "DISK.CD"});
  p.f.source["SETUP.BMP"] = blob("tng setup splash", 2000);
  for (const char* f : {"CHANGES.TXT", "DUNZIP.DLL", "INS0762.LIB"}) p.f.source[f] = blob(f, 300);
  p.engine_zip();
  auto xpl = blob("ADXPL320"), art = blob("TNG_ART", 1500), snd = blob("TNG_SND", 3000), rsrc = blob("AD_RSRC"),
       afi = blob("ST-TNG.AFI"), m1 = blob("theme mid"), m2 = blob("tap mid");
  std::vector<std::pair<std::string, std::vector<uint8_t>>> misc = {
      {"TNG_ART.DLL", art}, {"TNG_SND.DLL", snd}, {"EDITFILE.TXT", blob("edit")}};
  if (engine) misc.insert(misc.begin(), {"ADXPL320.DLL", xpl});
  p.zip("MODMISC.ZIP", misc);
  if (engine) p.expect("ST-TNG/ADXPL320.DLL", xpl);
  p.expect("ST-TNG/TNG_ART.DLL", art);
  p.expect("ST-TNG/TNG_SND.DLL", snd);
  p.zip("MUSIC.ZIP", {{"THEME.MID", m1}, {"TAP.MID", m2}});
  p.expect("ST-TNG/MUSIC/THEME.MID", m1);
  p.expect("ST-TNG/MUSIC/TAP.MID", m2);
  p.zip("WIN.ZIP", {{"AD_RSRC.DLL", rsrc}, {"UNLINK.EXE", blob("unlink")}});
  p.expect("ST-TNG/AD_RSRC.DLL", rsrc);
  p.zip("WINSYS.ZIP", {{"PLACE.TXT", pattern(42, 9)}});
  auto afis = afi_members("tng", {"AD2.AFI", "DISNEY.AFI", "MAD.AFI", "MARVEL.AFI", "STARTREK.AFI", "STUMP.AFI"});
  afis.push_back({"ST-TNG.AFI", afi});
  p.zip("AFI.ZIP", afis);
  p.expect("ST-TNG/FOLDER.AFI", afi);
  p.zip("HELP.ZIP", {{"ST-TNG.HLP", blob("help")}});
  for (const Ad3Module& m : tng_modules()) p.module(m.zip, {{m.file, ne_module(m.name, {"KERNEL", "ADXPL320"})}});
  p.f.ids = module_ids("tng", tng_modules());
  return p.f;
}

// A tree's files of one install disk (by `disk_of`), at the root.
inline Tree disk_files(const Tree& t, int disk, int (*disk_of)(const std::string&)) {
  Tree out;
  for (const auto& [rel, d] : t)
    if (disk_of(rel) == disk) out[rel] = d;
  return out;
}

// ---- Star Wars Screen Entertainment: a Presage install of Intermission modules --------------

// An Intermission IMX module: an NE exporting what the IMX reader looks for
// (SAVERDLGPROC only with `dialog`), with a dialog template blob.
inline std::vector<uint8_t> imx_module(const std::string& name, const std::vector<std::string>& refs,
                                       bool dialog = true) {
  NeSpec ne;
  ne.module_name = name;
  ne.module_refs = refs;
  ne.exports = {"WEP", "SAVERINIT", "SAVERDRAW"};
  if (dialog) ne.exports.push_back("SAVERDLGPROC");
  ne.exports.push_back("LIBMAIN");
  if (dialog) ne.exports.push_back("SAVERDLGPROC2");
  ne.resources = {{5, "", 1, "a made-up dialog template of " + name}};
  return vec(build_ne(ne));
}

// A plain NE library importing `refs` (the DLLs beside the modules, whose
// own imports the intermission invariants check).
inline std::vector<uint8_t> ne_dll(const std::string& name, const std::vector<std::string>& refs) {
  NeSpec ne;
  ne.module_name = name;
  ne.module_refs = refs;
  ne.exports = {"WEP"};
  return vec(build_ne(ne));
}

// A Presage installer script of our own (the real one is never copied):
// [disks], [data] with the product's short name, a few [install] lines.
inline std::vector<uint8_t> intermission_dat(const std::string& shortname) {
  return vec("[disks]\r\n"
             "1 = \"Made-Up Collection Disk 1\",swse1.arj\r\n"
             "2 = \"Made-Up Collection Disk 2\",swse2.arj\r\n"
             "\r\n"
             "[data]\r\n"
             "neededspace = 0\r\n"
             "  ShortName =  " +
             shortname +
             "  \r\n"
             "longname = A Made-Up Screen Saver Collection\r\n"
             "DestDir = C:\\SAVER\r\n"
             "\r\n"
             "[install]\r\n"
             "; comment lines are skipped\r\n"
             "1 = 1,stress.dl_,stress.dll,A helper library\r\n"
             "2 = 1,gm_battl.mi_,battle.mid,Some music,CHECK=24\r\n"
             "3 = 1,swse1.arj,(DST),The first archive\r\n"
             "4 = 5,swse.ini,(WIN)\\swse.ini,Settings,CHECK=100\r\n");
}

// Which install floppy each file is on (INSTALL.DAT's disk column): disk 1
// has the installer, SWSE1.ARJ and the SZDD files; disks 2-5 one volume of
// SWSE2 each; disk 5 also SWSE.INI, WinG, the VxDs and the control panel.
inline int swse_disk(const std::string& name) {
  if (name == "SWSE2.ARJ") return 2;
  if (name == "SWSE2.A01") return 3;
  if (name == "SWSE2.A02") return 4;
  for (const char* d5 : {"SWSE2.A03", "SWSE.INI", "WING.DL_", "WINGPAL.WN_", "DVA.386", "ANTHOOK.386", "IMCPL.CPL",
                         "SWSESET.EXE", "SYSINI.DAT"})
    if (name == d5) return 5;
  return 1;
}

// The files a Star Wars Screen Entertainment import must never open (I5):
// the installer, readme and notes, the other MIDI sets, WinG, the VxDs, the
// control panel, the chooser.
inline const std::vector<std::string>& swse_decoys() {
  static const std::vector<std::string> v = {"README.TXT",  "SVGA.EXE",    "INSTALL.EXE", "SYSINI.DAT",  "SB6BATTL.MI_",
                                             "FM_TITLE.MI_", "WING.DL_",   "IMCPL.CPL",   "INSTDETL.DAT", "DIB.DR_",
                                             "ANTHOOK.386", "SWSESET.EXE", "DVA.386",     "WINGPAL.WN_"};
  return v;
}

// A Presage install at the source's root (a CD copy of the five floppies):
// SWSE1.ARJ (every DLL, Intermission and its IMX reader, one module, and
// four members the recipe skips — damaged on purpose, so decoding any of them
// would fail the import), SWSE2.ARJ + .A01-.A03 (one archive: stored modules
// cut across volumes, an LZH member cut across volumes, method 1 and 4
// members), the SZDD loose files and SWSE.INI, and decoys. `swse1`, when
// set, changes SWSE1.ARJ's members before the volume is written (the
// expected files stay those of the unchanged release).
inline PkgFixture swse_fixture(const std::function<void(std::vector<ArjEntry>&)>& swse1 = {}) {
  PkgFixture f;
  const std::string root = "packages/swse";
  auto expect = [&](const std::string& rel, const std::vector<uint8_t>& d) { f.expect[root + "/" + rel] = d; };
  const std::vector<std::string> base = {"INTRMLIB", "SWSE", "KERNEL", "USER", "GDI", "STRESS"};
  auto with = [&](std::vector<std::string> extra) {
    std::vector<std::string> r = base;
    r.insert(r.end(), extra.begin(), extra.end());
    return r;
  };
  auto stored = [](const std::string& n, const std::vector<uint8_t>& d, uint8_t flags = 0x10,
                   std::optional<uint32_t> pos = std::nullopt) { return arj_stored_entry(n, d, flags, pos); };

  // SWSE1.ARJ.
  const auto intrmlib = ne_dll("INTRMLIB", {"USER", "KERNEL", "GDI", "ANTSW"});
  const auto antsw = ne_dll("ANTSW", {"KERNEL", "GDI", "USER"});
  const auto swse = ne_dll("SWSE", {"KERNEL", "USER", "GDI", "MMSYSTEM"});
  const auto readjpg = ne_dll("READJPG", {"KERNEL", "USER", "GDI", "WIN87EM"});
  const auto memmidi = arj_vector_plain("m4_text");  // method 4
  const auto intermis = blob("the Intermission control panel");
  const auto imimxply = ne_dll("IMIMXPLY", {"KERNEL", "USER"});
  const auto bluprint = imx_module("BLUPRINT", with({"READJPG"}));
  auto bad_crc = stored("IMAD_PLY.IMQ", blob("the After Dark reader"));
  bad_crc.h.crc ^= 1;
  auto garbage = arj_packed_entry("INTERMSN.HLP", 1, pattern(64, 17), blob("help"));
  auto bad_snd = stored("AD_SND.DLL", blob("Intermission's sound support"));
  bad_snd.h.crc ^= 2;
  std::vector<ArjEntry> members = {
      stored("INTRMLIB.DLL", intrmlib), stored("ANTSW.DLL", antsw), stored("SWSE.DLL", swse),
      stored("READJPG.DLL", readjpg),
      arj_packed_entry("MEMMIDI.DLL", 4, unhex(arj_vector("m4_text").packed), memmidi), stored("INTERMIS.EXE", intermis),
      stored("IMIMXPLY.IMQ", imimxply), stored("BLUPRINT.IMX", bluprint), bad_crc, bad_snd,
      stored("IWLIB.DLL", blob("an extension library")), garbage};
  if (swse1) swse1(members);
  f.source["SWSE1.ARJ"] = arj_volume("SWSE1.ARJ", false, members);
  expect("SAVER/INTRMLIB.DLL", intrmlib);
  expect("SAVER/ANTSW.DLL", antsw);
  expect("SAVER/SWSE.DLL", swse);
  expect("SAVER/READJPG.DLL", readjpg);
  expect("SAVER/MEMMIDI.DLL", memmidi);
  expect("SAVER/BLUPRINT.IMX", bluprint);
  expect("ENGINE/INTERMIS.EXE", intermis);
  expect("ENGINE/IMIMXPLY.IMQ", imimxply);

  // SWSE2.ARJ .. .A03: one archive over four disks.
  const auto battles = imx_module("BATTLES", base), cantina = imx_module("CANTINA", with({"READJPG", "WIN87EM"}));
  const auto jawas = imx_module("JAWAS", base), storybrd = imx_module("STORYBRD", with({"READJPG"}));
  const auto swtext = imx_module("SWTEXT", with({"COMMDLG"})), vader = imx_module("VADER", with({"READJPG"}));
  const auto swtext_txt = arj_vector_plain("m1_all_bytes");  // method 1
  const auto p1 = arj_vector_plain("m1_text"), p2 = arj_vector_plain("m1_two_blocks");
  std::vector<uint8_t> swsfx = p1;  // an LZH member cut across volumes: each segment its own stream
  swsfx.insert(swsfx.end(), p2.begin(), p2.end());
  auto half = [](const std::vector<uint8_t>& d, bool second) {
    const size_t cut = d.size() / 2;
    return second ? std::vector<uint8_t>(d.begin() + cut, d.end()) : std::vector<uint8_t>(d.begin(), d.begin() + cut);
  };
  f.source["SWSE2.ARJ"] = arj_volume("SWSE2.ARJ", true,
                                     {stored("BATTLES.IMX", battles), stored("CANTINA.IMX", cantina),
                                      stored("JAWAS.IMX", half(jawas, false), 0x14)});
  f.source["SWSE2.A01"] = arj_volume("SWSE2.A01", true,
                                     {stored("JAWAS.IMX", half(jawas, true), 0x18, uint32_t(jawas.size() / 2)),
                                      stored("STORYBRD.IMX", half(storybrd, false), 0x14)});
  f.source["SWSE2.A02"] = arj_volume(
      "SWSE2.A02", true,
      {stored("STORYBRD.IMX", half(storybrd, true), 0x18, uint32_t(storybrd.size() / 2)),
       arj_packed_entry("SWSFX.DLL", 1, unhex(arj_vector("m1_text").packed), p1, 0x14)});
  f.source["SWSE2.A03"] = arj_volume(
      "SWSE2.A03", false,
      {arj_packed_entry("SWSFX.DLL", 1, unhex(arj_vector("m1_two_blocks").packed), p2, 0x18, uint32_t(p1.size())),
       stored("SWTEXT.IMX", swtext),
       arj_packed_entry("SWTEXT.TXT", 1, unhex(arj_vector("m1_all_bytes").packed), swtext_txt),
       stored("VADER.IMX", vader)});
  for (auto& [n, d] : std::vector<std::pair<std::string, std::vector<uint8_t>>>{
           {"BATTLES.IMX", battles}, {"CANTINA.IMX", cantina}, {"JAWAS.IMX", jawas}, {"STORYBRD.IMX", storybrd},
           {"SWSFX.DLL", swsfx}, {"SWTEXT.IMX", swtext}, {"SWTEXT.TXT", swtext_txt}, {"VADER.IMX", vader}})
    expect("SAVER/" + n, d);

  // Loose files: SZDD under the installer's names, and the settings.
  const auto stress = ne_dll("STRESS", {"KERNEL", "USER", "TOOLHELP"});
  f.source["STRESS.DL_"] = szdd_encode(stress);
  expect("SAVER/STRESS.DLL", stress);
  for (auto [from, to] : std::vector<std::pair<const char*, const char*>>{{"GM_BATTL.MI_", "BATTLE.MID"},
                                                                          {"GM_CNTNA.MI_", "CANTINA.MID"},
                                                                          {"GM_EMPIR.MI_", "EMPIRE.MID"},
                                                                          {"GM_TITLE.MI_", "SWTHEME.MID"}}) {
    auto music = vec("MThd");
    auto body = blob(std::string("general midi ") + to, 400);
    music.insert(music.end(), body.begin(), body.end());
    f.source[from] = szdd_encode(music);
    expect(std::string("SAVER/") + to, music);
  }
  const auto ini = vec("[Made-Up Module]\r\nSetting=1\r\n");
  f.source["SWSE.INI"] = ini;
  expect("WINDOWS/SWSE.INI", ini);
  f.source["INSTALL.DAT"] = intermission_dat("SWSE");
  for (const std::string& d : swse_decoys()) f.source[d] = blob("decoy " + d, 200);
  f.ids = {"swse.battles", "swse.bluprint", "swse.cantina", "swse.jawas", "swse.storybrd", "swse.swtext", "swse.vader"};
  return f;
}

// ---- Star Trek: The Screen Saver: a Microsoft Setup install of After Dark 2.0 modules ---------

// An After Dark 2.0 module (the Classic lane: an NE exporting MODULE),
// `name` as its name resource holds it (with the leading space After Dark
// 2.0 put in most), importing `refs`, with `controls` (slot, record). Its
// About text ends with the registrant stand-in and wraps a sentence by hand,
// as the real ones do.
inline std::vector<uint8_t> ad20_module(const std::string& name, const std::vector<std::string>& refs,
                                        const std::vector<std::pair<uint16_t, std::string>>& controls = {},
                                        bool stand_in = true) {
  NeSpec ne;
  ne.module_name = "AD20MOD";
  ne.module_refs = refs;
  ne.exports = {"WEP", "MODULE"};
  std::string about = "ABOUT" + name + "\r\n\r\nA made-up module whose sentence is wrapped by \r\nhand.\r\n\r\n"
                      "Made up for the tests.";
  if (stand_in) about += "\r\nBerkeley Systems Authorized User.";
  ne.resources = {{2000, "", 20, name + '\0'}, {2000, "", 30, about + '\0'}, {2000, "", 10, "By nobody.\r\n" + std::string(1, '\0')}};
  for (const auto& [slot, rec] : controls) ne.resources.push_back({1000, "", slot, rec});
  return vec(build_ne(ne));
}

// Which install floppy each file is on (the real split: the setup files and
// the modules on disk 1; the engine, its DLLs and drivers, the art and sound
// databases on disk 2).
inline int startrek_disk(const std::string& name) {
  for (const char* d2 : {"AD.EX_", "AD.HL_", "ADINIT.EX_", "AD_AILAN.DL_", "AD_LIB.DL_", "AD_MME.DR_", "AD_MOD.DL_",
                         "AD_MPT.DR_", "AD_NET.EX_", "AD_NVLNW.DL_", "AD_RSRC.DL_", "AD_SB.DR_", "AD_SND.DL_",
                         "AD_WRAP.CO_", "AFTERDRK.NS_", "NWCONN.DL_", "NWCORE.DL_", "NWMISC.DL_", "SPALETTE.DL_",
                         "ST_MASKS.DL_", "ST_RESDB.DL_", "ST_SND.DL_", "ST_SVGA.DL_", "ST_VGA.DL_"})
    if (name == d2) return 2;
  return 1;
}

// The files a Star Trek: The Screen Saver import must never open (I5): the
// setup program and its scripts, the network, PC-speaker and Sound Blaster
// drivers, the disk's AD_PREFS.INI, the help file, the VxD, AD_MESG and the rest.
inline const std::vector<std::string>& startrek_decoys() {
  static const std::vector<std::string> v = {
      "SETUP.EXE",   "_MSTEST.EX_",  "AD_NSTLL.MS_", "AD_NSTLL.DL_", "ST_NSTLL.IN_", "AD_NSTLL.INI", "AD_MESG.AD_",
      "AD.38_",      "AD.HL_",       "AD_WRAP.CO_",  "NWCONN.DL_",   "AD_NET.EX_",   "AFTERDRK.NS_", "ADINIT.EX_",
      "SPLASH1.BM_", "AD_PREFS.IN_", "AD_MPT.DR_",   "SPALETTE.DL_", "AD_LIB.DL_",   "AD_SB.DR_"};
  return v;
}

// A Setup file list of our own (the real one is never copied): the window
// title that names the release, a command line, a file list, the trailing
// Ctrl-Z. `title` in Windows-1252.
inline std::vector<uint8_t> setup_lst(const std::string& title = "Star Trek\xAE: The Screen Saver") {
  return vec("[Params]\r\n"
             "\tWndTitle            = " + title + "\r\n"
             "\tWndMess             = Setting up a made-up screen saver...\r\n"
             "\tCmdLine             = _mstest made_up.mst  /C \"/S %s %s\"\r\n"
             "\r\n"
             "[Files]\r\n"
             "\tmade_up.in_ = made_up.inf\r\n"
             "\x1A");
}

// Star Trek: The Screen Saver's two install disks' files at the source's
// root (a copy of both, or the disks unioned): SETUP.LST, every file of the
// registry's table KWAJ-compressed under its disk name (made-up modules and
// DLLs as runs of literals; ST_VGA.DL_ is a fixed vector the research
// encoder wrote, ST_MASKS.DL_ the distance-4096 crafted stream), and decoys.
// `change`, when set, edits the sources after they are made (the expected
// files stay those of the unchanged release).
inline PkgFixture startrek_fixture(const std::function<void(Tree&)>& change = {}) {
  PkgFixture f;
  const std::string root = "packages/startrek";
  auto put = [&](const std::string& disk_name, const std::string& rel, const std::vector<uint8_t>& d) {
    f.source[disk_name] = kwaj_literals(d);
    f.expect[root + "/" + rel] = d;
  };
  const std::vector<std::string> base = {"KERNEL", "USER", "GDI", "WIN87EM", "AD_MOD", "AD_RSRC"};
  struct M {
    const char* file;
    const char* name;
  };
  // The 16 names, as the resources hold them: 15 with a leading space.
  const std::vector<M> modules = {{"BRAINCEL", " Brain Cells"},  {"COMMS", " Communications"}, {"FINAL", " Final Exam"},
                                  {"FRONTIER", " Final Frontier"}, {"HORTA", " Horta"},        {"IONSTORM", " Ion Storm"},
                                  {"MISSION", " The Mission"},   {"PANELS", " Ship Panels"},   {"PLANETS", " PlanetaryAtlas"},
                                  {"SCOTTYS", " Scotty's Files"}, {"SICKBAY", " Sickbay"},     {"SOUNDER", "Sounder"},
                                  {"SPACE", " Space"},           {"SPOCK", " Spock"},          {"THOLIAN", " Tholian Web"},
                                  {"TRIBBLE", " Tribbles"}};
  for (const M& m : modules) {
    const std::string file = m.file;
    std::vector<uint8_t> d;
    if (file == "COMMS")  // a button in slot 3; slot 0 empty
      d = ad20_module(m.name, base, {{2, popup_record("Message", {"Custom", "Other"}, 0)}, {4, button_record("Edit Custom...")}});
    else if (file == "SOUNDER")  // AD_SND only, a button in slot 2, no stand-in line
      d = ad20_module(m.name, {"KERNEL", "USER", "GDI", "AD_SND"},
                      {{1, popup_record("Sequence", {"In Order", "Shuffle"}, 0)}, {3, button_record("Sounds..")}}, false);
    else if (file == "MISSION")  // no controls
      d = ad20_module(m.name, base);
    else
      d = ad20_module(m.name, base, {{1, checkbox_record("Clear Screen First", 0)}});
    put(file + ".AD_", "AFTERDRK/" + file + ".AD", d);
  }
  put("AD_MOD.DL_", "AFTERDRK/AD_MOD.DLL", ne_dll("AD_MOD", {"KERNEL", "GDI", "USER", "WIN87EM", "AD_RSRC", "AD_SND"}));
  put("AD_RSRC.DL_", "AFTERDRK/AD_RSRC.DLL", ne_dll("AD_RSRC", {"KERNEL", "USER", "GDI"}));
  put("AD_MME.DR_", "AFTERDRK/AD_MME.DRV", ne_dll("AD_MME", {"WIN87EM", "KERNEL", "USER"}));
  put("ST_RESDB.DL_", "AFTERDRK/ST_RES/ST_RESDB.DLL", ne_dll("ARTDB", {"KERNEL"}));
  put("ST_SVGA.DL_", "AFTERDRK/ST_RES/ST_SVGA.DLL", blob("made-up art, 256 colours", 6000));
  put("ST_SND.DL_", "AFTERDRK/ST_RES/ST_SND.DLL", blob("made-up sounds", 9000));
  auto wav = vec("RIFF");
  auto body = blob("a made-up wave", 700);
  wav.insert(wav.end(), body.begin(), body.end());
  put("JIM.WA_", "AFTERDRK/SOUNDS/JIM.WAV", wav);
  put("AD_SND.DL_", "ENGINE/AD_SND.DLL", ne_dll("AD_SND", {"KERNEL", "USER"}));
  put("AD.EX_", "ENGINE/AD.EXE", blob("a made-up host", 3000));
  // Written by the research encoder, and token by token.
  f.source["ST_VGA.DL_"] = unhex(kKwajVectors[2].packed);
  f.expect[root + "/AFTERDRK/ST_RES/ST_VGA.DLL"] = kwaj_vector_plain(kKwajVectors[2].name);
  const KwajCraftedFile masks = kwaj_dist4096();
  f.source["ST_MASKS.DL_"] = masks.file;
  f.expect[root + "/AFTERDRK/ST_RES/ST_MASKS.DLL"] = masks.plain;
  f.source["SETUP.LST"] = setup_lst();
  for (const std::string& d : startrek_decoys()) f.source[d] = blob("decoy " + d, 200);
  if (change) change(f.source);
  f.ids = {"startrek.braincel", "startrek.comms",   "startrek.final",   "startrek.frontier",
           "startrek.horta",    "startrek.ionstorm", "startrek.mission", "startrek.panels",
           "startrek.planets",  "startrek.scottys",  "startrek.sickbay", "startrek.sounder",
           "startrek.space",    "startrek.spock",    "startrek.tholian", "startrek.tribble"};
  return f;
}

// ---- Delrina's Intermission Installer: The Far Side's and Dilbert's floppies ----------------
//
// Every file loose on the install disks, most SZDD-compressed under their
// installed names, the shared libraries with Delrina's version stamps after
// the data; each disk's tag file (DISK1..DISKn); the installer, the other
// readers, AD_SND and the rest beside them as decoys that must never be
// opened. Made-up bytes throughout.

// An Intermission ASA animation of our own: the header the lane and the
// catalog look for ("AniN", or the older "AniM"), then made-up bytes.
inline std::vector<uint8_t> asa_animation(const std::string& name, bool ani_m = false) {
  std::vector<uint8_t> v = vec(ani_m ? "AniM" : "AniN");
  auto body = blob("a made-up animation " + name, 900);
  v.insert(v.end(), body.begin(), body.end());
  return v;
}

// An IMQ module: an NE exporting SAVERMAIN (and, with `dialog`, its dialog
// procedures), its own reader.
inline std::vector<uint8_t> imq_module(const std::string& name, const std::vector<std::string>& refs,
                                       bool dialog = true) {
  NeSpec ne;
  ne.module_name = name;
  ne.module_refs = refs;
  ne.exports = {"WEP", "SAVERMAIN"};
  if (dialog) ne.exports.insert(ne.exports.end(), {"SAVERDLGPROC", "SAVERDLGPROC2"});
  ne.resources = {{5, "", 1, "a made-up dialog template of " + name}};
  return vec(build_ne(ne));
}

// SZDD with Delrina's version stamp after the data (its shared libraries').
inline std::vector<uint8_t> szdd_stamped(const std::vector<uint8_t>& d, const std::string& stamp = "DLL 0401") {
  std::vector<uint8_t> s = szdd_encode(d);
  s.insert(s.end(), stamp.begin(), stamp.end());
  return s;
}

// One file of a Delrina install: its name on the disks, its disk, and how it
// is stored there.
struct DelrinaFile {
  std::string name;
  int disk;
  std::vector<uint8_t> bytes;  // as installed
  enum class Stored { plain, szdd, stamped } stored = Stored::szdd;
  std::string to;              // relative to the package root; "" = a decoy, never opened
};

// The fixture of a Delrina install from its files (a decoy has no `to`),
// every disk's tag file added, and the catalog ids it must list.
inline PkgFixture delrina_fixture(const std::string& id, const std::vector<DelrinaFile>& files, int disks,
                                  std::vector<std::string> ids) {
  PkgFixture f;
  const std::string root = "packages/" + id;
  for (const DelrinaFile& x : files) {
    f.source[x.name] = x.stored == DelrinaFile::Stored::plain ? x.bytes
                       : x.stored == DelrinaFile::Stored::szdd ? szdd_encode(x.bytes)
                                                               : szdd_stamped(x.bytes);
    if (!x.to.empty()) f.expect[root + "/" + x.to] = x.bytes;
  }
  for (int k = 1; k <= disks; k++) f.source["DISK" + std::to_string(k)] = vec("\r\n");
  f.ids = std::move(ids);
  return f;
}

// The Far Side's disks (the real split): the installer, Intermission and
// its readers, PTERY.IMQ and HELL.ASA on disk 1; the other modules over
// disks 2-5; DIBDLL, the VxD and the control panel on disk 5.
inline int farside_disk(const std::string& name) {
  static const std::map<std::string, int> disk = {
      {"DISK2", 2},        {"NERDCLOK.IMQ", 2}, {"OCEAN.ASA", 2},   {"AMOEBA.ASA", 2},   {"AERIAL.ASA", 2},
      {"DISK3", 3},        {"BIRDS.ASA", 3},    {"BISON.ASA", 3},   {"ISLAND.ASA", 3},   {"DISK4", 4},
      {"FISHBOWL.ASA", 4}, {"FUTURE.ASA", 4},   {"DISK5", 5},       {"EGGFIGHT.ASA", 5}, {"FISH.ASA", 5},
      {"REPTILES.ASA", 5}, {"DIBDLL.DLL", 5},   {"LASTDISK.ASA", 5}, {"ANTHOOK.386", 5}, {"IMCPL.CPL", 5},
      {"USERINST.EXE", 5}};
  auto it = disk.find(name);
  return it == disk.end() ? 1 : it->second;
}

// The files an import of The Far Side must never open (I5): the installer
// and its loader, the other readers, AD_SND, the libraries and programs
// nothing loads, LASTDISK.ASA (an .ASA the table does not list: the
// installer's last-disk check), the VxD, the control panel, the readme, the
// installer's picture, and a BBS's note.
inline const std::vector<std::string>& farside_decoys() {
  static const std::vector<std::string> v = {"SETUP.EXE",    "IMINST2.EXE",  "IMINST3.EXE", "IMIMXPLY.IMQ",
                                             "IMAD_PLY.IMQ", "IMFLCPLY.IMQ", "AD_SND.DLL",  "IWLIB.DLL",
                                             "NETPASS.EXE",  "SSINTERM.SCR", "INTERMIS.TXT", "INSTALL.BMP",
                                             "LASTDISK.ASA", "ANTHOOK.386",  "IMCPL.CPL",   "USERINST.EXE",
                                             "PHOENIX.NFO"};
  return v;
}

// The Far Side Screen Saver Collection's five disks' files at the source's
// root (a copy of all five, or the disks unioned): the 12 ASA animations
// (EGGFIGHT with the older header; HELL stored plain), the two IMQ modules,
// the libraries they load, the ASA reader and Intermission, and decoys.
inline PkgFixture farside_fixture() {
  using S = DelrinaFile::Stored;
  const std::vector<std::string> ui = {"KERNEL", "USER", "GDI"};
  auto with = [&](std::vector<std::string> extra) {
    std::vector<std::string> r = ui;
    r.insert(r.end(), extra.begin(), extra.end());
    return r;
  };
  std::vector<DelrinaFile> files;
  for (const char* m : {"AERIAL", "AMOEBA", "BIRDS", "BISON", "EGGFIGHT", "FISH", "FISHBOWL", "FUTURE", "HELL",
                        "ISLAND", "OCEAN", "REPTILES"}) {
    const std::string n = std::string(m) + ".ASA";
    files.push_back({n, farside_disk(n), asa_animation(m, n == "EGGFIGHT.ASA"),
                     n == "HELL.ASA" ? S::plain : S::szdd, "SAVER/" + n});
  }
  files.push_back({"PTERY.IMQ", 1, imq_module("PTERODACTYL", with({"INTRMLIB", "ANTSW", "DIBDLL", "MMSYSTEM"})),
                   S::szdd, "SAVER/PTERY.IMQ"});
  files.push_back({"NERDCLOK.IMQ", 2, imq_module("NERDCLOK", with({"INTRMLIB", "ANTSW"})), S::szdd,
                   "SAVER/NERDCLOK.IMQ"});
  files.push_back({"INTRMLIB.DLL", 1, ne_dll("INTRMLIB", with({"ANTSW"})), S::stamped, "SAVER/INTRMLIB.DLL"});
  files.push_back({"ANTSW.DLL", 1, ne_dll("ANTSW", with({"MMSYSTEM"})), S::stamped, "SAVER/ANTSW.DLL"});
  files.push_back({"MEMMIDI.DLL", 1, ne_dll("MEMMIDI", {"KERNEL", "MMSYSTEM"}), S::stamped, "SAVER/MEMMIDI.DLL"});
  files.push_back({"DIBDLL.DLL", 5, ne_dll("DIBDLL", ui), S::plain, "SAVER/DIBDLL.DLL"});
  files.push_back({"IMASAPLY.IMQ", 1, imq_module("IMASAPLY", with({"ANTSW", "INTRMLIB"})), S::plain,
                   "ENGINE/IMASAPLY.IMQ"});
  files.push_back({"INTERMIS.EXE", 1, blob("the Intermission control panel"), S::szdd, "ENGINE/INTERMIS.EXE"});
  for (const std::string& d : farside_decoys())
    files.push_back({d, farside_disk(d), blob("decoy " + d, 200), d == "INSTALL.BMP" ? S::szdd : S::plain, ""});
  return delrina_fixture("farside", files, 5,
                         {"farside.aerial", "farside.amoeba", "farside.birds", "farside.bison", "farside.eggfight",
                          "farside.fish", "farside.fishbowl", "farside.future", "farside.hell", "farside.island",
                          "farside.nerdclok", "farside.ocean", "farside.ptery", "farside.reptiles"});
}

// Dilbert's disks (the real split): the installer, Intermission, its readers,
// the module list, DB-CLOCK, DRAW and OPTI on disk 1; the other modules
// over disks 2-4; DIBDLL, IM4_EXP and the last disk's tag on disk 4.
inline int dilbert_disk(const std::string& name) {
  static const std::map<std::string, int> disk = {
      {"DISK2", 2},       {"STDOGB.ASA", 2},   {"LUNCH.ASA", 2},    {"THOR.ASA", 2},    {"SHRED.ASA", 2},
      {"DISK3", 3},       {"DB-BEST.IMQ", 3},  {"SWCROSS.ASA", 3},  {"WEDGIES.ASA", 3}, {"CONOFHO.ASA", 3},
      {"DISK4", 4},       {"CYBER.ASA", 4},    {"PRESENT.ASA", 4},  {"ATWORK.ASA", 4},  {"LAWYER.ASA", 4},
      {"DIL-WHAK.IMQ", 4}, {"DIBDLL.DLL", 4},  {"IM4_EXP.DLL", 4},  {"LASTDISK.ASA", 4}, {"USERINST.EXE", 4}};
  auto it = disk.find(name);
  return it == disk.end() ? 1 : it->second;
}

// As The Far Side's, and the installer's module list (PACKING.LST: never
// read, the names of disk 1's files identify the release), Intermission's
// own icons and the library nothing imports.
inline const std::vector<std::string>& dilbert_decoys() {
  static const std::vector<std::string> v = {"SETUP.EXE",    "IMINST2.EXE",  "IMINST3.EXE",  "IMIMXPLY.IMQ",
                                             "IMSEQPLY.IMQ", "AD_SND.DLL",   "IWLIB.DLL",    "ICONDLL.DLL",
                                             "ANTSW2.DLL",   "MAPI.DLL",     "PACKING.LST",  "INTERMIS.TXT",
                                             "LASTDISK.ASA", "USERINST.EXE", "README.TXT"};
  return v;
}

// Scott Adams' Dilbert Screen Saver Collection's four disks' files at the
// source's root: 13 ASA animations (SHRED stored plain), three IMQ modules
// (DB-BEST and DB-CLOCK stored plain), the libraries, the ASA reader,
// Intermission, and decoys.
inline PkgFixture dilbert_fixture() {
  using S = DelrinaFile::Stored;
  const std::vector<std::string> ui = {"KERNEL", "USER", "GDI"};
  auto with = [&](std::vector<std::string> extra) {
    std::vector<std::string> r = ui;
    r.insert(r.end(), extra.begin(), extra.end());
    return r;
  };
  std::vector<DelrinaFile> files;
  for (const char* m : {"ATWORK", "CONOFHO", "CYBER", "DRAW", "LAWYER", "LUNCH", "OPTI", "PRESENT", "SHRED",
                        "STDOGB", "SWCROSS", "THOR", "WEDGIES"}) {
    const std::string n = std::string(m) + ".ASA";
    files.push_back({n, dilbert_disk(n), asa_animation(m), n == "SHRED.ASA" ? S::plain : S::szdd, "SAVER/" + n});
  }
  files.push_back({"DB-BEST.IMQ", 3, imq_module("DB-BEST", with({"INTRMLIB", "ANTSW", "DIBDLL", "IM4_EXP", "MMSYSTEM"})),
                   S::plain, "SAVER/DB-BEST.IMQ"});
  files.push_back({"DB-CLOCK.IMQ", 1, imq_module("DIL-CLOCK", with({"INTRMLIB", "ANTSW", "DIBDLL", "WIN87EM"})),
                   S::plain, "SAVER/DB-CLOCK.IMQ"});
  files.push_back({"DIL-WHAK.IMQ", 4, imq_module("DB-BUDGT", with({"INTRMLIB", "ANTSW", "DIBDLL"}), false), S::szdd,
                   "SAVER/DIL-WHAK.IMQ"});
  files.push_back({"INTRMLIB.DLL", 1, ne_dll("INTRMLIB", with({"ANTSW"})), S::stamped, "SAVER/INTRMLIB.DLL"});
  files.push_back({"ANTSW.DLL", 1, ne_dll("ANTSW", with({"MMSYSTEM"})), S::stamped, "SAVER/ANTSW.DLL"});
  files.push_back({"MEMMIDI.DLL", 1, ne_dll("MEMMIDI", {"KERNEL", "MMSYSTEM"}), S::stamped, "SAVER/MEMMIDI.DLL"});
  files.push_back({"DIBDLL.DLL", 4, ne_dll("DIBDLL", ui), S::stamped, "SAVER/DIBDLL.DLL"});
  files.push_back({"IM4_EXP.DLL", 4, ne_dll("IM4_EXP", {"KERNEL"}), S::szdd, "SAVER/IM4_EXP.DLL"});
  files.push_back({"IMASAPLY.IMQ", 1, imq_module("IMASAPLY", with({"ANTSW", "INTRMLIB"})), S::plain,
                   "ENGINE/IMASAPLY.IMQ"});
  files.push_back({"INTERMIS.EXE", 1, blob("the Intermission control panel, a later build"), S::stamped,
                   "ENGINE/INTERMIS.EXE"});
  for (const std::string& d : dilbert_decoys())
    files.push_back({d, dilbert_disk(d), blob("decoy " + d, 200), S::plain, ""});
  return delrina_fixture("dilbert", files, 4,
                         {"dilbert.atwork", "dilbert.conofho", "dilbert.cyber", "dilbert.db-best", "dilbert.db-clock",
                          "dilbert.dil-whak", "dilbert.draw", "dilbert.lawyer", "dilbert.lunch", "dilbert.opti",
                          "dilbert.present", "dilbert.shred", "dilbert.stdogb", "dilbert.swcross", "dilbert.thor",
                          "dilbert.wedgies"});
}

// ---- Delrina's Intermission Installer: the twentieth's four releases ------------------------
//
// The Opus 'n Bill Screen Saver (both builds), On the Road Again, The
// Flintstones (both builds) and Intermission 4.0, made up from their
// registry rows' own loose-file tables (packages.cc): every file the table
// installs, stored as the table says (SZDD, the shared libraries with a
// version stamp), with made-up bytes of its kind — ASA animations, IMQ and
// IMX modules, FLI animations (Autodesk's magic, AF12), MRF and MSV data,
// pictures, libraries, readers — on the disks of the real split; every
// disk's tag file; and decoys beside them that must never be opened (the
// installer, the other readers, AD_SND, a BBS's notes...).

// An FLI animation of our own: Autodesk's header shape (size, then the
// magic AF12 of the FLC variant Intermission 4.0's carry), made-up frames.
inline std::vector<uint8_t> fli_animation(const std::string& name) {
  std::vector<uint8_t> v = {0x00, 0x04, 0x00, 0x00, 0x12, 0xAF, 0x02, 0x00, 0x40, 0x01, 0xC8, 0x00};
  auto body = blob("a made-up flic " + name, 1012);
  v.insert(v.end(), body.begin(), body.end());
  return v;
}

// The made-up bytes of one file of a Delrina release's table, by its kind.
inline std::vector<uint8_t> delrina_file_bytes(const std::string& name) {
  const std::vector<std::string> ui = {"KERNEL", "USER", "GDI"};
  auto with = [&](std::vector<std::string> extra) {
    std::vector<std::string> r = ui;
    r.insert(r.end(), extra.begin(), extra.end());
    return r;
  };
  const std::string stem = name.substr(0, name.find('.'));
  auto ext = [&](const char* e) { return name.size() > 4 && name.compare(name.size() - 4, 4, e) == 0; };
  if (ext(".ASA")) return asa_animation(stem);
  if (ext(".FLI")) return fli_animation(stem);
  if (ext(".MRF") || ext(".MSV") || ext(".BMP")) return blob("made-up data " + name, 700);
  if (ext(".IMX")) {
    // A photo viewer imports the fractal decoder beside it; one module the
    // sprite library.
    if (stem == "PHOTO" || stem == "FM-PHOTO") return imx_module(stem, with({"INTRMLIB", "DECO"}));
    if (stem == "PALETTE") return imx_module(stem, with({"ANTSW"}));
    return imx_module(stem, with({"INTRMLIB"}));
  }
  if (ext(".IMQ")) {
    // A reader (IM???PLY.IMQ; the same bytes wherever the table puts it).
    if (name.size() == 12 && name.compare(0, 2, "IM") == 0 && name.compare(5, 7, "PLY.IMQ") == 0)
      return imq_module(stem, with({"ANTSW", "INTRMLIB"}));
    return imq_module(stem, with({"INTRMLIB", "ANTSW", "MMSYSTEM"}));
  }
  if (name == "INTRMLIB.DLL") return ne_dll("INTRMLIB", with({"ANTSW"}));
  if (name == "ANTSW.DLL") return ne_dll("ANTSW", with({"MMSYSTEM"}));
  if (name == "MEMMIDI.DLL") return ne_dll("MEMMIDI", {"KERNEL", "MMSYSTEM"});
  if (ext(".DLL")) return ne_dll(stem, ui);
  return blob("the Intermission control panel of " + name);
}

// A Delrina release made up from its registry row's table (another build's,
// `build`, when set): files on the disks `disk_of` gives, `decoys` beside
// them, every disk's tag file but those in `no_tags`; the catalog ids it must
// list, sorted.
inline PkgFixture delrina_table_fixture(const std::string& id, const char* build, int (*disk_of)(const std::string&),
                                        int disks, const std::vector<std::string>& decoys,
                                        const std::set<int>& no_tags = {}) {
  using S = DelrinaFile::Stored;
  const adw::import::Package* p = adw::import::find_package(id);
  std::span<const adw::import::LooseFile> table = p->loose_files;
  for (const adw::import::Build& b : p->builds)
    if (build && std::string_view(b.id) == build) table = b.loose_files;
  std::vector<DelrinaFile> files;
  std::vector<std::string> ids;
  for (const adw::import::LooseFile& lf : table) {
    const std::string name = lf.from, to = lf.to;
    const bool dll = name.size() > 4 && name.compare(name.size() - 4, 4, ".DLL") == 0;
    const S stored = lf.codec == adw::import::Codec::plain ? S::plain : dll ? S::stamped : S::szdd;
    files.push_back({name, disk_of(name), delrina_file_bytes(name), stored, to});
    const std::string ext = name.substr(name.find('.'));
    const bool reader = name.size() == 12 && name.compare(0, 2, "IM") == 0 && name.compare(5, 7, "PLY.IMQ") == 0;
    if (to.rfind("SAVER/", 0) == 0 && !reader &&
        (ext == ".ASA" || ext == ".IMQ" || ext == ".IMX" || ext == ".FLI" || ext == ".MRF" || ext == ".MSV")) {
      std::string lower = name.substr(0, name.find('.'));
      for (char& c : lower) c = char(tolower((unsigned char)c));
      ids.push_back(id + "." + lower);
    }
  }
  for (const std::string& d : decoys) files.push_back({d, disk_of(d), blob("decoy " + d, 200), S::plain, ""});
  std::sort(ids.begin(), ids.end());
  PkgFixture f = delrina_fixture(id, files, disks, ids);
  for (int k : no_tags) f.source.erase("DISK" + std::to_string(k));
  return f;
}

// The decoys every one of the four ships beside its table's files.
inline std::vector<std::string> delrina_decoys(std::vector<std::string> more) {
  std::vector<std::string> v = {"IMINST2.EXE", "IMINST3.EXE", "IMAD_PLY.IMQ", "IMFLCPLY.IMQ", "IMNSSPLY.IMQ",
                                "AD_SND.DLL",  "IWLIB.DLL",   "NETPASS.EXE",  "SSINTERM.SCR", "INTERMIS.TXT",
                                "INSTALL.BMP", "LASTDISK.ASA", "ANTHOOK.386", "IMCPL.CPL",    "USERINST.EXE"};
  v.insert(v.end(), more.begin(), more.end());
  return v;
}

// The Opus 'n Bill Screen Saver's September 1993 disks (the BBS copies'
// split): the modules over three disks, the libraries and readers on disk 1.
inline int opus_disk(const std::string& name) {
  static const std::map<std::string, int> disk = {
      {"DISK2", 2},        {"OPUSMESS.ASA", 2}, {"PUDDYLUV.ASA", 2}, {"SILIBILL.ASA", 2}, {"SWINGER.ASA", 2},
      {"VELOC.ASA", 2},    {"DISK3", 3},        {"BASSELOP.ASA", 3}, {"BILLFISH.ASA", 3}, {"MALELAM.ASA", 3},
      {"MICROIBM.ASA", 3}, {"NIGHTCAT.ASA", 3}, {"OPUSBATH.ASA", 3}, {"LASTDISK.ASA", 3}, {"ANTHOOK.386", 3},
      {"IMCPL.CPL", 3},    {"USERINST.EXE", 3}, {"AD_SND.DLL", 3}};
  auto it = disk.find(name);
  return it == disk.end() ? 1 : it->second;
}
inline PkgFixture opus_fixture() {
  return delrina_table_fixture("opus", nullptr, opus_disk, 3, delrina_decoys({"INSTALL.EXE", "INTERMSN.HLP"}));
}

// Its November 1993 build's disks (the other BBS copy's split): VELOC2 and
// the clock on disk 1, the censored toasters on disk 2, Opus's Moment on 3.
inline int opus_1993_11_disk(const std::string& name) {
  static const std::map<std::string, int> disk = {
      {"DISK2", 2},        {"BILLFISH.ASA", 2}, {"BUGS.ASA", 2},     {"CTOAST.ASA", 2},   {"OPUSBATH.ASA", 2},
      {"PENGUIN.ASA", 2},  {"SWINGER.ASA", 2},  {"DISK3", 3},        {"BASSELOP.ASA", 3}, {"BERSERK.ASA", 3},
      {"BUNGEE2.ASA", 3},  {"MALELAM.ASA", 3},  {"SILIBILL.ASA", 3}, {"LASTDISK.ASA", 3}, {"ANTHOOK.386", 3},
      {"IMCPL.CPL", 3},    {"USERINST.EXE", 3}, {"AD_SND.DLL", 3}};
  auto it = disk.find(name);
  return it == disk.end() ? 1 : it->second;
}
inline PkgFixture opus_1993_11_fixture() {
  return delrina_table_fixture("opus", "1993-11", opus_1993_11_disk, 3, delrina_decoys({"INSTALL.EXE"}));
}

// On the Road Again's disks: its only copy keeps them together, so a split
// of our own (the trek, the libraries and readers on disk 1).
inline int opusroad_disk(const std::string& name) {
  static const std::map<std::string, int> disk = {
      {"DISK2", 2},        {"ANTS.ASA", 2},     {"BUTTHEAD.ASA", 2}, {"HAIRBALL.ASA", 2}, {"INFOHWY.ASA", 2},
      {"DISK3", 3},        {"JUNGLE.ASA", 3},   {"MIDNITCC.ASA", 3}, {"OPUSFLY2.ASA", 3}, {"PISTACH4.ASA", 3},
      {"BUTTWIPE.IMQ", 3}, {"DISK4", 4},        {"RATRACE2.ASA", 4}, {"SINGIN2.ASA", 4},  {"TAXTHIS.ASA", 4},
      {"UNRIDER.ASA", 4},  {"OB-SKATE.IMQ", 4}, {"OB-SPACE.IMQ", 4}, {"LASTDISK.ASA", 4}, {"DIBDLL.DLL", 4}};
  auto it = disk.find(name);
  return it == disk.end() ? 1 : it->second;
}
inline PkgFixture opusroad_fixture() {
  return delrina_table_fixture("opusroad", nullptr, opusroad_disk, 4,
                               delrina_decoys({"SETUP.EXE", "PACKING.LST", "ICONDLL.DLL", "ANTSW2.DLL", "MAPI.DLL",
                                               "INTERMIS.LIB", "IMSEQPLY.IMQ"}));
}

// The Flintstones' June 1994 disks (the BBS copy's split): CARS and the
// drive-in on disk 1, the logo, DINORDS and the paper boy on disk 2, the
// rest on disk 3.
inline int flintstones_disk(const std::string& name) {
  static const std::map<std::string, int> disk = {
      {"DISK2", 2},        {"DINORDS.ASA", 2},  {"LOGO.ASA", 2},     {"PAPERBOY.IMQ", 2}, {"DISK3", 3},
      {"DIBDLL.DLL", 3},   {"DICTABRD.IMQ", 3}, {"FM-BIRDY.IMQ", 3}, {"FM-BOULD.IMX", 3}, {"FM-BOWL.IMQ", 3},
      {"FM-CLOCK.IMQ", 3}, {"FM-CRANE.IMQ", 3}, {"FM-CRITT.IMX", 3}, {"FM-FEET.IMX", 3},  {"FM-MOBIL.IMQ", 3},
      {"FMPADROK.IMX", 3}, {"LASTDISK.ASA", 3}, {"ANTHOOK.386", 3},  {"IMCPL.CPL", 3},    {"USERINST.EXE", 3},
      {"AD_SND.DLL", 3}};
  auto it = disk.find(name);
  return it == disk.end() ? 1 : it->second;
}
inline PkgFixture flintstones_fixture() {
  return delrina_table_fixture("flintstones", nullptr, flintstones_disk, 3,
                               delrina_decoys({"SETUP.EXE", "SEEME!.COM"}));
}

// Its May 1994 build's disks (the BBS copy's split, which has no tag file
// on disks 2 and 3): the photo shoot on disk 1, the IMQ modules on disk 2,
// the animations on disk 3, its damaged CARS.ASA among them (a decoy: never
// opened).
inline int flintstones_1994_05_disk(const std::string& name) {
  static const std::map<std::string, int> disk = {
      {"DICTABRD.IMQ", 2}, {"DRIVEIN.IMQ", 2}, {"FM-CLOCK.IMQ", 2}, {"FM-CRANE.IMQ", 2}, {"PAPERBOY.IMQ", 2},
      {"IMASAPLY.IMQ", 2}, {"IMIMXPLY.IMQ", 2}, {"IMAD_PLY.IMQ", 2}, {"IMFLCPLY.IMQ", 2}, {"IMNSSPLY.IMQ", 2},
      {"CARS.ASA", 3},     {"DINORDS.ASA", 3}, {"LOGO.ASA", 3},     {"THEME.ASA", 3}};
  auto it = disk.find(name);
  return it == disk.end() ? 1 : it->second;
}
inline PkgFixture flintstones_1994_05_fixture() {
  return delrina_table_fixture("flintstones", "1994-05", flintstones_1994_05_disk, 3,
                               delrina_decoys({"SETUP.EXE", "CARS.ASA", "H3LLO2U.NFO"}), {2, 3});
}

// Intermission 4.0's three floppies (the images' split): the ASA
// animations, the shark, the mix, the pictures, the libraries and every
// reader on disk 1; the FLI animations on disk 2; the IMX modules and the
// morph on disk 3.
inline int intermission_disk(const std::string& name) {
  auto ext = [&](const char* e) { return name.size() > 4 && name.compare(name.size() - 4, 4, e) == 0; };
  if (name == "DISK2" || ext(".FLI")) return 2;
  if (name == "DISK3" || ext(".IMX") || ext(".MRF") || name == "LASTDISK.ASA" || name == "ANTHOOK.386" ||
      name == "IMCPL.CPL" || name == "USERINST.EXE" || name == "AD_SND.DLL")
    return 3;
  return 1;
}
inline PkgFixture intermission_fixture() {
  return delrina_table_fixture("intermission", nullptr, intermission_disk, 3,
                               delrina_decoys({"INSTALL.EXE", "CURTCALL.EXE", "IMIW_PLY.IMQ", "IMSEQPLY.IMQ",
                                               "IMMRFPLY.HLP", "INTERMIS.HLP"}));
}

// ---- InstallShield 2 compressed libraries: Marvel Comics Screen Posters, Snoopy's Screen Savers ----

// A member of a made-up library: its name, the bytes it holds (its recorded
// size) and its DOS date and time; stored as DCL literals written token by
// token (dcl_stream below; isz_builder.h: nothing compresses), or as `stream`
// when that is set.
struct IslibMember {
  std::string name;
  std::vector<uint8_t> data;
  uint16_t date = 0x1B8D, time = 0;  // 1993-12-13 00:00
  std::vector<uint8_t> stream = {};
};

// `data` as a DCL implode stream of literals and the end code (binary
// literals, a 4096-byte window, as the releases' members are).
inline std::vector<uint8_t> dcl_stream(const std::vector<uint8_t>& data) {
  std::vector<DclToken> tokens = dcl_literals(data);
  tokens.push_back(DclToken::end());
  return dcl_write(tokens);
}

// A stream no decoder gets through (its 2-byte header, then no end code): a
// member the import must never decode.
inline std::vector<uint8_t> undecodable_stream() { return {0x00, 0x06}; }

// One library of an install: the script's logical name for it (SETUP.PKG's),
// its file (a split set: its first volume's, "IMAGES.1"; the second is
// "IMAGES.2"), the disk it starts on, its members in table order, and for a
// set split over that disk and the next, the member crossing the boundary.
struct IslibLibrary {
  std::string logical, file;
  int disk = 1;
  std::vector<IslibMember> members;
  std::string crossing;  // "" = one unsplit library
};

inline std::string second_volume(const std::string& first) { return first.substr(0, first.size() - 1) + "2"; }

// InstallShield's package list SETUP.PKG, laid out as the real ones are
// (research/win/pkg/installshield SURVEY_REPORT.md; importer.cc reads it):
// the magic, the disk table's offset and the number of disks, one group per
// library (one unnamed directory with every member's size and name), then the
// disk table: each disk's libraries by logical name, with their groups'
// offsets.
inline std::vector<uint8_t> setup_pkg(const std::vector<IslibLibrary>& libs, int disks) {
  auto put16 = [](std::vector<uint8_t>& o, uint32_t x) {
    o.push_back(uint8_t(x));
    o.push_back(uint8_t(x >> 8));
  };
  auto put32 = [&](std::vector<uint8_t>& o, uint32_t x) {
    put16(o, x & 0xFFFF);
    put16(o, x >> 16);
  };
  std::vector<uint8_t> v = {0x4A, 0xA3};
  put32(v, 0);  // the disk table's offset, set below
  put32(v, uint32_t(disks));
  std::vector<uint32_t> group_at;
  for (const IslibLibrary& lib : libs) {
    std::vector<uint8_t> body;
    put16(body, 1);  // one directory, unnamed
    put16(body, 0);
    body.push_back(0);
    put16(body, uint32_t(lib.members.size()));
    for (const IslibMember& m : lib.members) {
      put16(body, 0);
      put32(body, uint32_t(m.data.size()));
      body.push_back(uint8_t(m.name.size()));
      body.insert(body.end(), m.name.begin(), m.name.end());
      body.push_back(0);
    }
    group_at.push_back(uint32_t(v.size()));
    put32(v, uint32_t(body.size()));
    v.insert(v.end(), body.begin(), body.end());
  }
  const uint32_t table = uint32_t(v.size());
  for (int k = 0; k < 4; k++) v[size_t(2 + k)] = uint8_t(table >> (8 * k));
  for (int disk = 1; disk <= disks; disk++) {
    std::vector<size_t> on;
    for (size_t i = 0; i < libs.size(); i++)
      if (libs[i].disk == disk) on.push_back(i);
    if (on.empty()) continue;
    put16(v, 0);
    put16(v, uint32_t(disk));
    put16(v, uint32_t(on.size()));
    for (size_t i : on) {
      put16(v, uint32_t(libs[i].logical.size()));
      v.insert(v.end(), libs[i].logical.begin(), libs[i].logical.end());
      v.push_back(1);
      v.push_back(1);
      put32(v, group_at[i]);
    }
  }
  return v;
}

// A library's volumes: one, or the two of a set cut inside its crossing member.
inline std::vector<std::vector<uint8_t>> islib_volumes(const IslibLibrary& lib) {
  std::vector<IszSpec> specs;
  size_t cut = 0, pos = 0;
  for (const IslibMember& m : lib.members) {
    IszSpec s;
    s.name = m.name;
    s.size = uint32_t(m.data.size());
    s.stream = m.stream.empty() ? dcl_stream(m.data) : m.stream;
    s.date = m.date;
    s.time = m.time;
    if (m.name == lib.crossing) cut = pos + s.stream.size() / 2;
    pos += s.stream.size();
    specs.push_back(std::move(s));
  }
  if (lib.crossing.empty()) return isz_library(specs).volumes;
  return isz_split(specs, {cut ? cut : pos / 2}).volumes;
}

// An InstallShield 2 install: every disk's files at the root (`source`), the
// disk each is on (0: on every disk), and the names an import must never
// open or name — the installer's files and the libraries the recipe does not
// read (`decoys`), and the previous owners' notes (`notes`).
struct IslibFixture : PkgFixture {
  std::map<std::string, int> disk;
  std::vector<std::string> decoys, notes;

  // The files of install disk `n`.
  Tree disk_files(int n) const {
    Tree t;
    for (const auto& [rel, d] : source)
      if (disk.at(rel) == n || disk.at(rel) == 0) t[rel] = d;
    return t;
  }
  std::map<int, Tree> disks() const {
    std::map<int, Tree> out;
    for (int n = 1; n <= 2; n++) out[n] = disk_files(n);
    return out;
  }
};

// The fixture's libraries laid out, SETUP.PKG, and the expected files: each
// row of the registry's table, its member's bytes.
inline void islib_lay_out(IslibFixture& f, const char* id, const std::vector<IslibLibrary>& libs) {
  for (const IslibLibrary& lib : libs) {
    auto volumes = islib_volumes(lib);
    f.source[lib.file] = volumes[0];
    f.disk[lib.file] = lib.disk;
    if (volumes.size() > 1) {
      f.source[second_volume(lib.file)] = volumes[1];
      f.disk[second_volume(lib.file)] = lib.disk + 1;
    }
  }
  f.source["SETUP.PKG"] = setup_pkg(libs, 2);
  f.disk["SETUP.PKG"] = 1;
  const adw::import::Package* p = adw::import::find_package(id);
  for (const adw::import::LibraryMember& row : p->library_members)
    for (const IslibLibrary& lib : libs)
      if (lib.file == row.library)
        for (const IslibMember& m : lib.members)
          if (m.name == row.member) f.expect[std::string(p->root) + "/" + row.to] = m.data;
}

// A file of an install that the recipe never opens, on `disk`.
inline void islib_decoy(IslibFixture& f, const std::string& name, int disk, size_t n = 300) {
  f.source[name] = blob("decoy " + name, n);
  f.disk[name] = disk;
  f.decoys.push_back(name);
}

// Marvel Comics Screen Posters: images.lib split over both disks (IMAGES.1
// + IMAGES.2, XMEN2099.FIF crossing the boundary, as on the real disks) and
// modules.lib, engine.lib and win.lib on disk 2, holding the registry
// table's members (made-up: the module, its decoder, AD_SND and the host as
// NE files and blobs, the images as blobs) and, in engine.lib and win.lib,
// the members the recipe never decodes, as streams no decoder gets through;
// winsys.lib and the installer's support library as bytes no library reader
// takes; SETUP.PKG listing every library; the installer's files; and the
// previous owners' notes on disk 1. `change`, when set, edits the libraries
// before they are laid out; the expected files follow the edit.
inline IslibFixture marvel_fixture(const std::function<void(std::vector<IslibLibrary>&)>& change = {}) {
  IslibFixture f;
  const adw::import::Package* p = adw::import::find_package("marvel");
  IslibLibrary images{"images.lib", "IMAGES.1", 1, {}, "XMEN2099.FIF"};
  size_t i = 0;
  for (const adw::import::LibraryMember& row : p->library_members)
    if (std::string_view(row.library) == "IMAGES.1")
      images.members.push_back({row.member, blob(std::string("marvel ") + row.member, 150 + (i++ * 37) % 400)});
  // The module (1993-12-13 23:18:44, as the real one's), importing its decoder.
  IslibLibrary modules{"modules.lib", "MODULES.LIB", 2,
                       {{"MARVEL.AD", ne_module("Marvel Comics", {"KERNEL", "USER", "GDI", "WIN87EM", "DECO"}), 0x1B8D,
                         0xBA56},
                        {"DECO.DLL", ne_dll("DECO", {"KERNEL", "GDI", "USER"})}}};
  IslibLibrary engine{"engine.lib", "ENGINE.LIB", 2, {{"AD.EXE", blob("a made-up After Dark 2.0 host", 3000)}}};
  for (const char* n : {"ADINIT.EXE", "AD_LIB.DLL", "MRVLREAD.TXT", "MARVEL.TXT", "AD_MPT.DRV", "AD_SB.DRV",
                        "AD_MME.DRV", "MRVL.WRI", "EDITFILE.TXT", "MARVELAD.TXT"})
    engine.members.push_back({n, blob(std::string("never decoded ") + n, 120), 0x1B8D, 0, undecodable_stream()});
  IslibLibrary win{"win.lib", "WIN.LIB", 2, {}};
  for (const char* n : {"AD.HLP", "AD_SND.DLL", "AD_WRAP.COM", "SPALETTE.DLL", "AD_PREFS.INI"}) {
    if (std::string_view(n) == "AD_SND.DLL") win.members.push_back({n, ne_dll("AD_SND", {"KERNEL", "USER"})});
    else win.members.push_back({n, blob(std::string("never decoded ") + n, 90), 0x1B8D, 0, undecodable_stream()});
  }
  std::vector<IslibLibrary> libs = {images, modules, engine, win};
  if (change) change(libs);
  islib_lay_out(f, "marvel", libs);
  // SETUP.PKG lists winsys.lib too (AD.386); its file is no library at all.
  libs.push_back({"winsys.lib", "WINSYS.LIB", 2, {{"AD.386", blob("a made-up VxD", 100)}}});
  f.source["SETUP.PKG"] = setup_pkg(libs, 2);
  for (const char* n : {"INSTALL.INS", "SETUP.EXE", "SETUP.BIN", "~INS0762.LIB"}) islib_decoy(f, n, 1);
  for (const char* n : {"WINSYS.LIB", "CHANGES.TXT"}) islib_decoy(f, n, 2);
  for (const char* n : {"SERIAL#.DOC", "reg#.txt"}) {
    f.source[n] = blob(std::string("a previous owner's note ") + n, 41);
    f.disk[n] = 1;
    f.notes.push_back(n);
  }
  f.ids = {"marvel.marvel"};
  return f;
}

// Snoopy's Screen Savers: one library, AD_MODS.z, split over both disks
// (AD_MODS.1 + AD_MODS.2, IS_FLY.AD crossing the boundary, as on the real
// disks), holding the eight modules (made-up: six import AD_SND); SETUP.PKG
// listing it; the installer's files and the readme with its picture; and what
// the user's copy holds besides: a previous owner's notes, another release's
// readme and a disk copier's leftovers, DREAM.ON on both disks.
inline IslibFixture snoopy_fixture(const std::function<void(std::vector<IslibLibrary>&)>& change = {}) {
  IslibFixture f;
  struct M {
    const char* file;
    const char* name;
    bool sound;
  };
  const std::vector<M> modules = {{"IS_COLAG.AD", "Collage", false},       {"IS_DANCE.AD", "Dance", true},
                                  {"IS_FACES.AD", "Faces", true},          {"IS_FLY.AD", "Flying Ace", true},
                                  {"IS_LINUS.AD", "Linus & Snoopy", true}, {"IS_LITRY.AD", "Literary Ace", true},
                                  {"IS_SPTLT.AD", "Spotlights", false},    {"IS_THRPY.AD", "Therapy", true}};
  IslibLibrary lib{"AD_MODS.z", "AD_MODS.1", 1, {}, "IS_FLY.AD"};
  for (const M& m : modules) {
    std::vector<std::string> refs = {"KERNEL", "USER", "GDI"};
    if (m.sound) refs.push_back("AD_SND");
    lib.members.push_back({m.file, ne_module(m.name, refs), 0x1D4D, 0x7C00});  // 1994-10-13 15:32
  }
  std::vector<IslibLibrary> libs = {lib};
  if (change) change(libs);
  islib_lay_out(f, "snoopy", libs);
  for (const char* n : {"SETUP.INS", "SETUP.EXE", "AD_MODS.LIS", "AD_MODS.BMP", "AD_Changes.txt", "CMOS.RAM",
                        "TXTSCR.DAT"})
    islib_decoy(f, n, 1);
  islib_decoy(f, "DREAM.ON", 0);  // on both disks, the same bytes
  for (const char* n : {"Serial.nfo", "ADnews.txt"}) {
    f.source[n] = blob(std::string("a previous owner's note ") + n, 60);
    f.disk[n] = 1;
    f.notes.push_back(n);
  }
  f.ids = {"snoopy.is_colag", "snoopy.is_dance", "snoopy.is_faces", "snoopy.is_fly",
           "snoopy.is_linus", "snoopy.is_litry", "snoopy.is_sptlt", "snoopy.is_thrpy"};
  return f;
}

// ---- InstallShield 1: Screen Antics: Johnny Castaway's floppy --------------------------------

// A Windows 3.1 screen-saver program built on SCRNSAVE.LIB: an NE program
// (no library) described "SCRNSAVE :<name>", exporting SCREENSAVERPROC and
// (`dialog`) SCREENSAVERCONFIGUREDIALOG, importing what the real one does.
inline std::vector<uint8_t> scrnsave_program(const std::string& name, bool dialog = true) {
  NeSpec ne;
  ne.module_name = "SCRNMADE";
  ne.program = true;
  ne.description = "SCRNSAVE :" + name;
  ne.module_refs = {"MMSYSTEM", "GDI", "KERNEL", "USER"};
  ne.exports = {"SCREENSAVERPROC"};
  if (dialog) ne.exports.push_back("SCREENSAVERCONFIGUREDIALOG");
  ne.exports.push_back("PASSWORDDIALOG");
  return vec(build_ne(ne));
}

// The files a Johnny Castaway import must never open (I5): the installer
// (its launcher, script and expanded installer), its logos, and the floppy's
// placeholder under RESOURCE.00$'s installed name.
inline const std::vector<std::string>& castaway_decoys() {
  static const std::vector<std::string> v = {"SETUP.EXE", "INSTALL.INS", "INSTALL.EX$",
                                             "LOGO.BMP",  "SLOGO.BMP",   "RESOURCE.001"};
  return v;
}

// Its install floppy's files: the program and its data as "$" files (made-up
// bytes as literals, the program's stored as SCRANTIC.EXE), the map plain,
// and the decoys. `placeholder_only`: no RESOURCE.00$, only the placeholder
// (a decoy source: the placeholder is never installed).
inline PkgFixture castaway_fixture(bool placeholder_only = false) {
  PkgFixture f;
  const std::string root = "packages/castaway/SCRANTIC/";
  const std::vector<uint8_t> scr = scrnsave_program("Made Up Antics"), res = blob("made-up story data", 5000),
                             map = blob("made-up resource map", 300);
  f.source["SCRANTIC.SC$"] = is1_literals("SCRANTIC.EXE", scr);
  if (!placeholder_only) f.source["RESOURCE.00$"] = is1_literals("RESOURCE.001", res);
  f.source["RESOURCE.MAP"] = map;
  for (const std::string& d : castaway_decoys()) f.source[d] = blob("decoy " + d, 200);
  f.expect[root + "SCRANTIC.SCR"] = scr;
  f.expect[root + "RESOURCE.001"] = res;
  f.expect[root + "RESOURCE.MAP"] = map;
  f.ids = {"castaway.scrantic"};
  return f;
}

// An install disk's files as a 1.44 MB floppy image (8.3 names, upper case:
// a copy's long names, which no floppy holds, are left out).
inline std::vector<uint8_t> floppy_of(const Tree& t) {
  FatBuilder b = FatBuilder::floppy144();
  for (const auto& [rel, d] : t) {
    const size_t dot = rel.find('.');
    const bool short_name = rel.find('/') == std::string::npos &&
                            (dot == std::string::npos ? rel.size() <= 8 : dot <= 8 && rel.size() - dot - 1 <= 3);
    if (!short_name) continue;
    std::string upper = rel;
    for (char& c : upper) c = char(toupper((unsigned char)c));
    b.file(upper, d);
  }
  return b.build();
}

// A ZIP of floppy images, as the Internet Archive serves an item's: stored
// members, disk 2 first, and (`with_scan`) a label scan beside them.
inline std::vector<uint8_t> zip_of_images(const std::vector<std::pair<std::string, std::vector<uint8_t>>>& images,
                                          bool with_scan = true) {
  ZipBuilder b;
  b.password = "";
  for (const auto& [name, d] : images) b.add(name, d, /*deflate=*/false, /*encrypt=*/false);
  if (with_scan) b.add("disk1.jpg", pattern(5000, 99), /*deflate=*/false, /*encrypt=*/false);
  return b.build();
}

// ---- writing a fixture as a source -------------------------------------------------------

inline std::filesystem::path path_under(const std::filesystem::path& root, const std::string& rel) {
  std::filesystem::path p = root;
  size_t i = 0;
  while (i < rel.size()) {
    size_t j = rel.find('/', i);
    if (j == std::string::npos) j = rel.size();
    p /= adw::import::to_wide(rel.substr(i, j - i));
    i = j + 1;
  }
  return p;
}

inline void write_tree(const std::filesystem::path& root, const Tree& t) {
  for (const auto& [rel, d] : t) write_bytes(path_under(root, rel), d);
}

inline std::vector<uint8_t> iso_of(const PkgFixture& f, bool joliet = true, const std::string& volume = "AD_TEST") {
  IsoBuilder b;
  b.joliet = joliet;
  b.volume_id = volume;
  for (const auto& [rel, d] : f.source) {
    auto j = f.joliet.find(rel);
    b.file(rel, d, j == f.joliet.end() ? "" : j->second);
  }
  return b.build();
}

// `disk`: 0 = every root file, else only that floppy's files, by `disk_of`
// (the Simpsons' two by default; swse_disk for the five of Star Wars Screen
// Entertainment, with FatBuilder::floppy144()).
inline std::vector<uint8_t> fat_of(const Tree& t, int disk = 0, const std::string& corrupt_chain_of = "",
                                   int (*disk_of)(const std::string&) = simpsons_disk,
                                   FatBuilder b = FatBuilder::floppy288()) {
  for (const auto& [rel, d] : t)
    if (!disk || disk_of(rel) == disk) b.file(rel, d);
  auto img = b.build();
  if (!corrupt_chain_of.empty() && (!disk || disk_of(corrupt_chain_of) == disk)) {
    const auto& c = b.node(corrupt_chain_of).clusters;
    if (c.size() >= 2) b.set_fat(img, c[1], c[0]);  // a loop: reading it would fail
  }
  return img;
}

// A flat ZIP of the tree (every file at the root, none encrypted): the
// Internet Archive's copies of install folders.
inline std::vector<uint8_t> zip_folder(const Tree& t) {
  ZipBuilder b;
  b.password = "";
  for (const auto& [rel, d] : t) b.add(rel, d, /*deflate=*/true, /*encrypt=*/false);
  return b.build();
}

// A ZIP of the tree's root files in one DISK<n> folder per install disk
// ("DISK1/SETUP.PKG", by `disk_of`), each folder's own entry first when
// `folder_entries` (the Internet Archive's ScreamSavers copy has them).
inline std::vector<uint8_t> zip_disk_folders(const Tree& t, int (*disk_of)(const std::string&), int disks,
                                             bool folder_entries = true) {
  ZipBuilder b;
  b.password = "";
  for (int k = 1; k <= disks; k++) {
    const std::string dir = "DISK" + std::to_string(k) + "/";
    if (folder_entries) b.add(dir, {}, /*deflate=*/false, /*encrypt=*/false);
    for (const auto& [rel, d] : t)
      if (disk_of(rel) == k) b.add(dir + rel, d, /*deflate=*/true, /*encrypt=*/false);
  }
  return b.build();
}

// The tree's root files in one DISK<n> folder per install disk, under `root`.
inline void write_disk_folders(const std::filesystem::path& root, const Tree& t, int (*disk_of)(const std::string&),
                               int disks) {
  for (int k = 1; k <= disks; k++)
    for (const auto& [rel, d] : t)
      if (disk_of(rel) == k) write_bytes(path_under(root / (L"DISK" + std::to_wstring(k)), rel), d);
}

// ---- manifests and registries --------------------------------------------------------------

// Stable storage for the KnownFile strings of synthetic manifests.
inline std::deque<std::string>& string_pool() {
  static std::deque<std::string> pool;
  return pool;
}
inline const char* keep(const std::string& s) { return string_pool().emplace_back(s).c_str(); }
inline const wchar_t* keep(const std::wstring& s) {
  static std::deque<std::wstring> pool;
  return pool.emplace_back(s).c_str();
}

// A manifest describing `expect` exactly (paths sorted).
inline std::vector<adw::import::KnownFile> manifest_of(const Tree& expect) {
  std::vector<adw::import::KnownFile> m;
  for (const auto& [rel, d] : expect) m.push_back({keep(rel), d.size(), keep(adw::import::md5_hex(d.data(), d.size()))});
  return m;
}

// The built-in registry with synthetic manifests (and, optionally, known
// image md5s, download copies and cover sources) in place of the real ones —
// no Internet Archive URL and no cover download, so no test reaches the
// network by accident. Storage lives as long as the test.
struct TestRegistry {
  std::vector<adw::import::Package> packages;
  std::deque<std::vector<adw::import::KnownFile>> manifests;
  std::deque<std::vector<adw::import::KnownImage>> images;
  std::deque<std::vector<adw::import::Download>> download_lists;
  std::deque<std::vector<adw::import::DownloadPart>> part_lists;
  std::deque<std::vector<adw::import::DownloadMember>> member_lists;
  std::deque<std::vector<adw::import::CoverSource>> cover_lists;
  std::deque<std::vector<adw::import::Build>> build_lists;

  TestRegistry() {
    auto b = adw::import::builtin_packages();
    packages.assign(b.begin(), b.end());
    for (auto& p : packages) {
      p.manifest = {};
      p.images = {};
      p.downloads = {};
      p.covers = {};
      // Another build's own manifest and images too.
      if (!p.builds.empty()) {
        build_lists.emplace_back(p.builds.begin(), p.builds.end());
        for (auto& x : build_lists.back()) {
          x.manifest = {};
          x.images = {};
        }
        p.builds = build_lists.back();
      }
    }
  }
  // Another build of `id` (Package::builds), to give its manifest and images.
  adw::import::Build& build(const std::string& id, const std::string& build_id) {
    const std::span<const adw::import::Build> builds = get(id).builds;
    for (auto& list : build_lists)
      if (list.data() == builds.data())
        for (auto& b : list)
          if (build_id == b.id) return b;
    abort();
  }
  void build_manifest(const std::string& id, const std::string& build_id, std::vector<adw::import::KnownFile> m) {
    manifests.push_back(std::move(m));
    build(id, build_id).manifest = manifests.back();
  }
  void build_disk_images(const std::string& id, const std::string& build_id,
                         const std::vector<std::pair<std::string, uint64_t>>& disks) {
    std::vector<adw::import::KnownImage> v;
    int k = 0;
    for (const auto& [md5, size] : disks) v.push_back({keep(md5), size, "synthetic floppy", "", ++k});
    images.push_back(std::move(v));
    build(id, build_id).images = images.back();
  }
  // Cover sources for `id` (COVERS.md §2.2), in the order tried.
  void covers(const std::string& id, std::vector<adw::import::CoverSource> sources) {
    cover_lists.push_back(std::move(sources));
    get(id).covers = cover_lists.back();
  }
  adw::import::Package& get(const std::string& id) {
    for (auto& p : packages)
      if (id == p.id) return p;
    abort();
  }
  void manifest(const std::string& id, std::vector<adw::import::KnownFile> m) {
    manifests.push_back(std::move(m));
    get(id).manifest = manifests.back();
  }
  void image(const std::string& id, const std::string& md5, uint64_t size) {
    images.push_back({{keep(md5), size, "synthetic", ""}});
    get(id).images = images.back();
  }
  // Known ZIPs of the install files (several, as Marvel Comics Screen
  // Posters has two): md5 and size each.
  void zip_images(const std::string& id, const std::vector<std::pair<std::string, uint64_t>>& zips) {
    std::vector<adw::import::KnownImage> v;
    for (const auto& [md5, size] : zips) v.push_back({keep(md5), size, "ZIP of synthetic install files", ""});
    images.push_back(std::move(v));
    get(id).images = images.back();
  }
  // Known images of install disks: md5, size, disk number (1..N).
  struct Disk {
    std::string md5;
    uint64_t size;
    int disk;
  };
  void disk_images(const std::string& id, const std::vector<Disk>& disks) {
    std::vector<adw::import::KnownImage> v;
    for (const Disk& d : disks) v.push_back({keep(d.md5), d.size, "synthetic floppy", "", d.disk});
    images.push_back(std::move(v));
    get(id).images = images.back();
  }
  // A download copy: `url`, saved as `file`, published with this size and
  // md5 (and `more`: the images of further install disks).
  struct Part {
    std::string url;
    std::wstring file;
    uint64_t size;
    std::string md5;
  };
  // A member taken out of a tar copy (packages.h DownloadMember).
  struct Member {
    std::string path;
    std::wstring file;
    uint64_t size;
    std::string md5;
  };
  struct Copy {
    std::string url;
    std::wstring file;
    uint64_t size;
    std::string md5;
    std::string kind = "image";
    std::vector<Part> more = {};
    std::vector<Member> members = {};
  };
  void downloads(const std::string& id, const std::vector<Copy>& copies) {
    std::vector<adw::import::Download> d;
    for (const Copy& c : copies) {
      std::span<const adw::import::DownloadPart> more;
      if (!c.more.empty()) {
        std::vector<adw::import::DownloadPart> parts;
        for (const Part& q : c.more) parts.push_back({keep(q.url), keep(q.file), q.size, keep(q.md5)});
        part_lists.push_back(std::move(parts));
        more = part_lists.back();
      }
      std::span<const adw::import::DownloadMember> members;
      if (!c.members.empty()) {
        std::vector<adw::import::DownloadMember> m;
        for (const Member& q : c.members) m.push_back({keep(q.path), keep(q.file), q.size, keep(q.md5)});
        member_lists.push_back(std::move(m));
        members = member_lists.back();
      }
      d.push_back({keep(c.url), keep(c.file), c.size, keep(c.md5), keep(c.kind), more, members});
    }
    download_lists.push_back(std::move(d));
    get(id).downloads = download_lists.back();
  }
  std::span<const adw::import::Package> span() const { return packages; }
};

}  // namespace test
