// The package registry (PACKAGES.md §2): every release the importer knows,
// compiled in: the After Dark releases, seven Delrina Intermission products —
// Star Wars Screen Entertainment (LucasArts'), The Far Side Screen Saver
// Collection, Scott Adams' Dilbert Screen Saver Collection, the Opus 'n Bill
// Screen Saver, Opus 'n Bill: On the Road Again!, The Flintstones Screen
// Saver Collection and Intermission 4.0 itself — and Sierra's Screen Antics:
// Johnny Castaway, a Windows 3.1 screen-saver program.
// Registry order is the catalog's module order and the precedence order for
// display-name disambiguation (§6); the catalog's packages list goes by
// `released` instead.
//
//   deluxe    After Dark 4.0 Deluxe               tree          -> <win>\FILES\{AD40,CLASSIC,ENGINE,AFI}
//   ad10      After Dark 10th Anniversary         tree          -> <win>\packages\ad10\{AD10TH,ENGINE,AFI}
//   ad32      After Dark 3.2                      ad3zip        -> <win>\packages\ad32\{AD32,ENGINE}
//   tt        Totally Twisted After Dark          ad3zip        -> <win>\packages\tt\{TWISTED,ENGINE}
//   simpsons  The Simpsons Screen Saver           ad3zip        -> <win>\packages\simpsons\{SIMPSONS,ENGINE}
//   swse      Star Wars Screen Entertainment      intermission  -> <win>\packages\swse\{SAVER,ENGINE,WINDOWS}
//   startrek  Star Trek: The Screen Saver         ad2kwaj       -> <win>\packages\startrek\{AFTERDRK,ENGINE}
//   marvel    Marvel Comics Screen Posters        islib         -> <win>\packages\marvel\{AFTERDRK,ENGINE}
//   snoopy    Snoopy's Screen Savers              islib         -> <win>\packages\snoopy\AFTERDRK
//   looney    The Looney Tunes Screen Saver       ad3zip        -> <win>\packages\looney\{LNYTUNES,ENGINE}
//   screams   ScreamSavers                        ad3zip        -> <win>\packages\screams\{SCREAMS,ENGINE}
//   disney    The Disney Collection Screen Saver  ad3zip        -> <win>\packages\disney\{DISNEY,ENGINE}
//   farside   The Far Side Screen Saver Collection intermission -> <win>\packages\farside\{SAVER,ENGINE}
//   dilbert   Scott Adams' Dilbert Screen Saver Collection
//                                                 intermission  -> <win>\packages\dilbert\{SAVER,ENGINE}
//   tng       Star Trek: The Next Generation Screen Saver
//                                                 ad3zip        -> <win>\packages\tng\{ST-TNG,ENGINE}
//   castaway  Screen Antics: Johnny Castaway      is1           -> <win>\packages\castaway\SCRANTIC
//   opus      Opus 'n Bill Screen Saver           intermission  -> <win>\packages\opus\{SAVER,ENGINE}
//   opusroad  Opus 'n Bill: On the Road Again!    intermission  -> <win>\packages\opusroad\{SAVER,ENGINE}
//   flintstones The Flintstones Screen Saver Collection
//                                                 intermission  -> <win>\packages\flintstones\{SAVER,ENGINE}
//   intermission Intermission 4.0                 intermission  -> <win>\packages\intermission\{SAVER,ENGINE}
//
// Each entry carries what identifies the release (known image md5s — of one
// image, or of every install disk of a set — and fingerprints), how to
// extract it (the recipe and its parameters), what an import must contain
// (`required`), the install-time fix-ups, catalog name overrides, the
// manifest (path, size, md5 of every installed file — never bytes of the
// release) that a folder source is verified against, the Internet Archive
// copies `--download` fetches, and where its box cover comes from.
//
// A package root holds its module folders and ENGINE (never a module
// folder; snoopy has none: Snoopy's Screen Savers are modules for an After
// Dark already installed, and ship no engine; nor has castaway, a program
// that is its own engine). An optional WINDOWS folder
// holds the files the original installer put in C:\WINDOWS (swse:
// SWSE.INI); it is never a module folder either, and the 16-bit lane lays it
// under the guest's C:\WINDOWS.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace adw::import {

// A file of a verified import: path relative to <assets>\win ('/'-separated:
// "FILES/AD40/ADXPL510.DLL", "packages/ad32/AD32/GUTS.AD"), size and md5.
struct KnownFile {
  const char* path;
  uint64_t size;
  const char* md5;
};

// A disc or floppy image whose md5 names the package (and makes an import
// from it "verified": "image"). A ZIP of a release's install files whose
// bytes never change (the Internet Archive's stored file, the user's own copy
// byte for byte) is a known image the same way where no image of the original
// disks exists, or none can be listed (marvel, snoopy, looney, screams,
// disney; PACKAGES.md §3), as the Simpsons' is the owner's own merge of both
// floppies.
struct KnownImage {
  const char* md5;
  uint64_t size;
  // Human-readable. The windows word a verification by it (gui/model.cc):
  // "floppy" in it, the original disks; starting "ZIP", the known ZIP of the
  // install files; else the original disc.
  const char* medium;
  const char* volume_id;  // "" when the medium has none
  // 0: the whole release on this one image. n: install disk n of a set that
  // is the release only together (PACKAGES.md §3): an import is identified by
  // the set's md5s, and verified "image", when every disk 1..N of the package
  // is given exactly once (from either of two copies of a disk alike).
  int disk = 0;
};

// An install-time copy (§4.3): `create` is made as a copy of `copy_of`, both
// relative to the package root, when the source matched the release.
struct Fixup {
  const char* create;
  const char* copy_of;
};

// A catalog moduleName replacement (§6), keyed by the module's path relative
// to the package root ("AD10TH/TOAST2K.AD", compared without case, as
// Package::speed_modules is); for Intermission modules the name itself
// ("SAVER/VADER.IMX": the one SAVERINIT returns, which no resource holds);
// for Johnny Castaway's program the name it is known by (its description
// names it Screen Antics).
struct NameOverride {
  const char* module;
  const char* name;
};

// The stops of the Speed control (catalog.h speed_control): Slowest 6,
// Slow 12, Normal 25, Fast 50 and Fastest 100, in percent of the host's
// own speed.
enum class SpeedStop { slowest, slow, normal, fast, fastest };

// A module of Package::speed_modules: its path relative to the package
// root, keyed as a name override is ("SAVER/DRAGON.IMX"), and the stop its
// Speed control starts at.
struct SpeedModule {
  const char* module;
  SpeedStop start = SpeedStop::normal;
};

// tree: plain files copied from the disc's FILES dir. ad3zip: the AD 3.x
// InstallShield installs (encrypted PKZIP), their placement baked in.
// intermission: the installers of Intermission products, their placement
// baked in too: Presage's (multi-volume ARJ archives and SZDD-compressed
// loose files, INSTALL.DAT read only to identify the release), or Delrina's
// own Intermission Installer (IMINST2.EXE: every file loose on the install
// floppies, most SZDD-compressed under their installed names; nothing read
// to identify the release but the names of disk 1's files). ad2kwaj: the After Dark
// 2.0 Microsoft Setup installs (KWAJ-compressed files on the install floppies,
// SETUP.LST read only to identify the release), their placement baked in from
// the installer's script (no INF or MS Test interpreter). islib: the
// InstallShield 2 installs of compressed libraries (isz.h: a library, or one
// split over two install floppies; the package list SETUP.PKG read only to
// identify the release), the placement of each member baked in from the
// installer's script (no IS-script interpreter). is1: the InstallShield 1
// installs of single compressed files (isz.h: "$" files on one install
// floppy, beside the installer's own INSTALL.INS, SETUP.EXE and INSTALL.EX$,
// which name the release with its first file; nothing is read to identify
// it), every file it installs a loose file, the placement baked in from the
// installer's script (no IS-script interpreter).
enum class Recipe { tree, ad3zip, intermission, ad2kwaj, islib, is1 };
// "tree", "ad3zip", "intermission", "ad2kwaj", "islib" or "is1" (import.json's
// package.recipe).
const char* recipe_name(Recipe r);

// How a loose file is stored on the install medium: as it is, compressed by
// Microsoft COMPRESS 'A' (SZDD, szdd.h), by the Microsoft Setup Toolkit's
// COMPRESS (KWAJ method 3, kwaj.h) or as an InstallShield 1 "$" file (isz.h);
// a compressed one is expanded on the way.
enum class Codec { plain, szdd, kwaj, is1 };

// A file the intermission, ad2kwaj and is1 recipes install from outside any
// archive, from the install dir: `from` as the source lists it, `to`
// relative to the package root. Presage's and Microsoft's installers gave a
// compressed file its installed name (INSTALL.DAT, or ST_NSTLL.INF), so it is
// never installed under its own; Delrina's compressed its files under their
// installed names; InstallShield 1's script names each one's (the name an
// "$" file records need not be it: SCRANTIC.SC$ holds SCRANTIC.EXE,
// installed as SCRANTIC.SCR).
struct LooseFile {
  const char* from;
  const char* to;
  Codec codec;
};

// A member of an InstallShield library the islib recipe installs (the baked
// placement table): `library` names the library by its file, or a split set
// by its first volume ("MODULES.LIB", "IMAGES.1"; the set's other volumes are
// found from that one's header), `member` as the library lists it (compared
// without ASCII case), `to` relative to the package root. A library member no
// row names is never decoded.
struct LibraryMember {
  const char* library;
  const char* member;
  const char* to;
};

// A file a download is taken out of instead of used as it is: a member of a
// tar the Internet Archive serves (The Flintstones' three ZIPs, in a BBS
// collection's tar of 160 files): `path` as the tar names it, saved as
// <downloads dir>\<file_name>, checked against `size` and `md5`. Nothing
// else in the tar is ever read.
struct DownloadMember {
  const char* path;
  const wchar_t* file_name;
  uint64_t size;
  const char* md5;
};

// A further file of a copy made of several (another install disk's image).
struct DownloadPart {
  const char* url;
  const wchar_t* file_name;
  uint64_t size;
  const char* md5;
};

// A copy of the package on the Internet Archive that `adimport --download`
// fetches (verified 2026-09-26: research/win/pkg/sources/sources.json).
// A package lists its copies in the order they are tried; the next one is
// tried when a copy cannot be fetched (network) or is not the published file
// (size or md5). Copies with the same bytes share a file name, so a transfer
// interrupted on one resumes from the next.
struct Download {
  const char* url;           // archive.org/download/… as published, percent-encoded; redirects are followed
  const wchar_t* file_name;  // saved as <downloads dir>\<file_name>
  uint64_t size;
  const char* md5;           // what the Internet Archive publishes for the file; checked before use
  // "image": a disc image, identified by its md5 like one the user gives
  // (verified: image). "zip": a ZIP of the install files (no image of the
  // original medium exists online); read as a folder and verified file by
  // file against the manifest (verified: files), unless its md5 is one of
  // the package's known images (the ZIP is the release's known copy:
  // verified: image).
  const char* kind;
  // A copy of a release on several install disks: the images of the other
  // disks (the first one's is the fields above), or for a "zip" copy made of
  // one ZIP per install disk, the other disks' ZIPs. The copy is used only
  // when every part is fetched and verifies; the parts are then read as one,
  // as several --image are.
  std::span<const DownloadPart> more_images = {};
  // Set: the fields above are a tar (ustar) that holds the copy; these
  // members are taken out of it, each checked, and read as the copy's
  // files (the images or ZIPs of `kind`), in this order.
  std::span<const DownloadMember> members = {};
};
// Every part of a copy together, in bytes (what the lists show; for a tar,
// the tar).
uint64_t download_size(const Download& d);

// A rectangle of a decoded picture, in its pixels (after EXIF orientation);
// w == 0: no crop.
struct Crop {
  int x = 0, y = 0, w = 0, h = 0;
};

// One place a package's cover can come from (COVERS.md §2.2, §2.3), in the
// package's preference order. Only URLs, md5s, sizes, paths and crops live
// here: no picture bytes (covers are fetched or read onto the user's machine
// at import time).
struct CoverSource {
  enum class Kind { download, disc };
  Kind kind;
  const char* art;         // "box" | "disc" | "splash" | "panel" — how the tile is rendered (§2.6)
  const char* label;       // for people: "Box front", "Disc label", "Installer art"
  const char* credit;      // for people: "Internet Archive", "Wikisimpsons", "your disc"
  // download
  const char* url;         // https only; redirects followed; the URL as published (never a mirror node)
  const char* md5;         // of the downloaded file (not of the crop); required for downloads
  uint64_t size;           // published size; required for downloads
  const wchar_t* file_name;  // saved as <download dir>\covers\<file_name>
  // disc
  const char* path;        // in the source, as identification located it: "INSTALL/SETUP.BMP", "ADE/PAGE1.BMP", "SETUP.EXE"
  const char* path_md5;    // md5 of that file: a file with other bytes (another pressing) is skipped; "" = any bytes
  uint16_t resource_type;  // 0: the file itself is the picture; 2 (RT_BITMAP): a bitmap resource of an NE/PE file
  uint16_t resource_id;
  Crop crop;               // applied after decoding (and after EXIF orientation)
};

// Another build of a release: a revision of the same product on disks of
// its own (Opus 'n Bill's November 1993 disks, with the toasters censored;
// The Flintstones' May 1994 disks). The package's own fields describe one
// build, the release's (Package::build names it); another has its own known
// images, its own file on install disk 1 that tells it apart, what it
// installs and its manifest. An
// import of it is identified as the package, verified against that build's
// manifest and recorded with its id (import.json package.build). The
// package's root, module folders, required files, name overrides (keyed by
// path), downloads and covers serve every build.
struct Build {
  const char* id;     // [0-9a-z-]+, import.json's package.build: "1993-11"
  const char* label;  // for people: "the November 1993 build"
  // Delrina's installer (Package::delrina_installer): a file of this build's
  // own, on install disk 1 beside the installer, that no other build has.
  // The package is identified by its own fingerprint or by this file; this
  // file, or the build's known images, make the import this build's.
  const char* marker;
  std::span<const KnownImage> images;
  std::span<const LooseFile> loose_files;
  std::span<const KnownFile> manifest;
  // What the log and import.json (package.buildNote) say of this build;
  // nullptr when nothing ("CARS.ASA is damaged in every known copy").
  const char* note = nullptr;
  // The files that tell each install disk is there, disk 1's tag file first
  // (Package::required_archives when empty): the only copy of The
  // Flintstones' May build has no tag file on disks 2 and 3, so a file of
  // its own on each stands for it.
  std::span<const char* const> disk_files = {};
};

struct Package {
  const char* id;           // [a-z0-9]+; "ad40" and "classic" are reserved (Deluxe's id prefixes)
  const char* title;
  const char* short_title;  // what disambiguates a repeated display name
  Recipe recipe;
  const char* root;         // relative to <win>: "FILES" or "packages/<id>"
  std::span<const char* const> module_dirs;  // catalog scan order (Deluxe: AD40, ENGINE (STARRYNI only), CLASSIC)
  std::span<const KnownImage> images;
  std::span<const char* const> required;     // relative to the root; an import without them fails (2)
  // tree: the folders under the source's FILES dir that are copied. The
  // fingerprint (§3): a FILES dir (ADE\FILES, FILES or the root) holding the
  // first module dir and ENGINE, plus `marker` (relative to FILES) when set,
  // and none of `absent`. ad3zip: `marker`, when set, is a second MODMISC.ZIP
  // member the fingerprint wants beside `engine_dll` (ad32's AD30RSDB.DLL:
  // ScreamSavers ships the same ADXPL300.DLL, and AD3.AFI in its AFI.ZIP).
  std::span<const char* const> copy_dirs;
  const char* marker;
  std::span<const char* const> absent;
  // ad3zip: the module folder, the MODMISC.ZIP member that identifies the
  // package, the AFI.ZIP member installed as <module dir>\FOLDER.AFI (which
  // identifies it too: the fingerprint wants both, and `marker` when set),
  // and the archives that must be present (so a split-floppy source is
  // complete). intermission: the module folder and the archives (every
  // volume: every install disk) too; with Delrina's installer, every install
  // disk's tag file instead (DISK1..DISKn; disk 1's is the fingerprint's),
  // and `marker`, a file of the release's own on disk 1 beside it and the
  // installer, IMINST2.EXE (the fingerprint). ad2kwaj: the module folder, and every
  // install disk's tag file (the Setup script's [Source Media Descriptions];
  // disk 1's is the fingerprint's), which must all be present. islib: the
  // module folder, and every volume of the libraries the recipe reads (every
  // install disk's; disk 1's first, the fingerprint's), which must all be
  // present. is1: the module folder, and every file the recipe reads (the
  // first, the fingerprint's), which must all be present.
  const char* module_dir;
  const char* engine_dll;
  const char* folder_afi;
  std::span<const char* const> required_archives;
  std::span<const Fixup> fixups;
  std::span<const NameOverride> name_overrides;
  std::span<const KnownFile> manifest;
  std::span<const Download> downloads;  // its Internet Archive copies, in the order tried (empty: none known)
  // Where its box cover comes from (COVERS.md §2.3), tried in this order
  // (empty: the front-ends draw a generated cover).
  std::span<const CoverSource> covers;
  // When the release shipped (ISO 8601, YYYY-MM or YYYY-MM-DD). The catalog lists
  // releases oldest first, so the settings dialog's cover strip and its list groups
  // read as a timeline. Empty sorts last, keeping the registry's order.
  const char* released = "";
  // intermission: the INSTALL.DAT [data] shortname that names the package
  // (the fingerprint, with required_archives[0] beside INSTALL.DAT), and the
  // files taken from outside the archives, under their installed names;
  // nullptr for a release Delrina's installer installs (delrina_installer()),
  // every file of which is a loose file. ad2kwaj and is1: every file it
  // installs is a loose file.
  const char* install_name = nullptr;
  std::span<const LooseFile> loose_files = {};
  // ad2kwaj: the SETUP.LST [Params] WndTitle that names the package (the
  // fingerprint, with required_archives[0] beside SETUP.LST), in Windows-1252
  // as the file holds it.
  const char* setup_title = nullptr;
  // The fixed screen every module of the package is shown at, "WxH"
  // ("640x480": Star Trek: The Screen Saver, several of whose modules compose
  // a fixed scene for it), which the catalog gives each of its entries as
  // "screen" (the front-end then gives the module that screen whatever the
  // Resolution setting); nullptr: the modules fit any screen.
  const char* screen = nullptr;
  // How the catalog reads its Classic modules' About texts (catalog.h).
  enum class About { as_is, ad20 };
  About about = About::as_is;
  // ad3zip: archives of the install dir the recipe never opens (I5), by name:
  // the Disney Collection's BEAUTYOL.ZIP, the 1993 build of BEAUTY.AD beside
  // the 1995 one in BEAUTY.ZIP, which the release installs (Berkeley's
  // checksum list names it). The log says it was skipped; it is never read.
  std::span<const char* const> never_opened = {};
  // islib: what names the package, the fingerprint with required_archives[0]
  // beside it: InstallShield's package list SETUP.PKG at the source's root
  // lists `tag_member` in the library `tag_library` (the script's logical
  // name for it, "modules.lib", "AD_MODS.z"; both compared without ASCII
  // case). The tag member is one of `library_members`, the placement table:
  // every file the recipe installs.
  const char* tag_library = nullptr;
  const char* tag_member = nullptr;
  std::span<const LibraryMember> library_members = {};
  // A release with several builds (struct Build): the id of the build the
  // fields above describe ("1993-09"), and the others. nullptr and none for
  // a release with one build (import.json then records no build).
  const char* build = nullptr;
  std::span<const Build> builds = {};
  // The Intermission modules whose motion is the machine's speed: each takes
  // a small step per call, and Intermission called them as fast as the PC
  // let it (ABI.md §3.8.4; Delrina's SDK gave them no clock). The catalog
  // gives each the Speed control (catalog.h speed_control), the emulated
  // machine's speed, which adhostwin takes from ADNE16IMXSPEED, starting at
  // the module's own stop. A module that paces itself by a clock is not
  // listed. Intermission 4.0's alone; empty for every other release, whose
  // entries have no Speed control.
  std::span<const SpeedModule> speed_modules = {};

  bool is_deluxe() const;
  // An Intermission release installed by Delrina's own Intermission
  // Installer (recipe intermission, no INSTALL.DAT shortname): The Far
  // Side's, Dilbert's, both Opus 'n Bill releases', The Flintstones' and
  // Intermission 4.0's. Its module folder holds Intermission's other module
  // forms besides IMX modules: ASA animations (*.ASA, data that the ASA
  // reader IMASAPLY.IMQ plays, from ENGINE), IMQ modules (*.IMQ, each its
  // own reader) and Intermission 4.0's FLI animations, MRF morph and MSV
  // mix (data its readers IMFLIPLY, IMMRFPLY and IMMSVPLY play, from ENGINE).
  bool delrina_installer() const;
};

// The built-in registry, in registry order.
std::span<const Package> builtin_packages();
// `id` in `registry` (the built-in one when empty); nullptr when unknown.
const Package* find_package(std::string_view id, std::span<const Package> registry = {});
// The registry to use: `registry` itself, or the built-in one when it is empty.
std::span<const Package> registry_or_builtin(std::span<const Package> registry);
// "After Dark 4.0 Deluxe, After Dark 10th Anniversary, …" for messages.
std::string known_releases(std::span<const Package> registry = {});
// The ids of the packages with an Internet Archive copy, in registry order
// (what `adimport --download all` fetches).
std::vector<std::string> downloadable_packages(std::span<const Package> registry = {});

}  // namespace adw::import
