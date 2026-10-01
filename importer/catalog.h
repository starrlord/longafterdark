// adw::import catalog — <assets>\win\catalog-win.json from the module
// binaries (DESIGN.md §6a, PACKAGES.md §6), without executing anything,
// merged over every installed package (packages.h).
//
// This is the C++ port of the prototype research/win/make_catalog.py and must
// stay semantically identical to it over the real Deluxe corpus
// (import.catalog_real compares the two, the PACKAGES.md §6 fields aside).
// What is read, per ABI.md §2.10 — the lane comes from the file header; the
// Deluxe folders are named here (*.AD), every other package scans its own
// module dirs and ENGINE (*.AD and *.IMX):
//
//   AD4 lane (PE32: FILES/AD40/*.AD, then FILES/ENGINE/STARRYNI.AD)
//     displayName  VERSIONINFO FileDescription (fallback: STRINGLIST 128[0], file name)
//     about        type 2000 name 40: RTF, reduced to plain text
//     controls     type 1000 names 1..4 (slot = name-1)
//     entry        "_Module@4" when exported (STARRYNI, MSVC), else "Module"
//   Classic lane (NE: FILES/CLASSIC/*.AD, and every package's NE *.AD — After
//   Dark 2.0b's Star Trek modules too): an NE exporting MODULE
//     displayName  type 2000 name 20 (C string)
//     about        type 2000 name 30 (plain text); credits: name 10. For a
//                  package whose About texts are After Dark 2.0's (the
//                  registry's Package::About::ad20): the last line "Berkeley
//                  Systems Authorized User." (the stand-in for the registered
//                  owner) is dropped, and a line break after a space and
//                  before a lower-case letter (a sentence wrapped by hand) is
//                  joined into that space. Every other package's texts are as
//                  written: four of their texts have such a break, kept.
//     controls     type 1000 names 1..4
//     entry        "MODULE"
//   Intermission IMX modules (NE, lane ne16 too; Star Wars Screen
//   Entertainment's SAVER/*.IMX): an NE exporting SAVERINIT and SAVERDRAW
//   but not MODULE nor SETCURRSAVER, whose file name does not start with
//   IMXX_ — the rules Intermission's IMX reader IMIMXPLY.IMQ applies. An NE
//   is told apart exactly as the ne16 lane's detect_kind does
//   (host/ne16/package.cc), by its exports' names, never by the extension:
//   MODULE wins when both kinds' exports are there, and an NE the lane
//   would refuse (IMXX_*, SETCURRSAVER, one of SAVERINIT/SAVERDRAW alone, a
//   reader exporting SAVERMAIN, none of these) is left out, its reason
//   logged.
//     displayName  the file stem (no resource holds a name: the registry's
//                  name overrides give moduleName)
//     about        "" (no text resource; no credits)
//     controls     one button, {0, "Configure...", button}, when SAVERDLGPROC
//                  is exported (the module's own dialog, IMIMXPLY's message 8)
//     entry        "SAVERDRAW"
//     abi          "intermission" (written last, only for Intermission
//                  entries; absent means the After Dark module ABI)
//   Intermission's other two module forms (lane ne16; The Far Side's and
//   Dilbert's SAVER, the folders of a package Delrina's installer installed,
//   Package::delrina_installer — only those folders list *.ASA and *.IMQ):
//     an ASA animation, *.ASA starting "AniN" or "AniM" (the ne16 lane's
//     rule: data, which Intermission's ASA reader IMASAPLY.IMQ plays from
//     ENGINE); an IMQ module, an NE *.IMQ exporting SAVERMAIN but neither
//     MODULE nor SAVERINIT and SAVERDRAW, its own reader, unless its name is
//     one of Intermission's readers' (is_intermission_reader: left out,
//     logged, as an NE exporting SAVERMAIN under any other extension is)
//     displayName  the file stem (the registry's name overrides give moduleName)
//     about        ""
//     controls     one button, {0, "Configure...", button}: an ASA's reader's
//                  dialog (IMASAPLY exports SAVERDLGPROC), an IMQ module's own
//                  when it exports SAVERDLGPROC
//     entry        "SAVERMAIN" (the reader's: the module's own for an IMQ)
//     needs        an IMQ module's imports; none for an ASA (its reader's
//                  are ENGINE's business)
//     abi          "intermission"
//   Any lane, from the registry: "screen" ("640x480"), the fixed screen every
//   module of a package is shown at (Package::screen: Star Trek: The Screen
//   Saver's, several of whose modules compose a fixed scene for it), written
//   last, and only for such a package's entries.
//
// PE resources with several languages (STARRYNI ships de/en/fr/ja/es) are
// read as an English Windows loads them: US English, then neutral, then the
// lowest language id. Text is Windows-1252 (the modules' ANSI code page),
// written out as UTF-8.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "packages.h"
#include "status.h"

namespace adw::import {

// One type-1000 control record as the catalog lists it. Which optional
// fields are present depends on `kind` (ABI.md §2.10.5):
//   stringslider  type "slider", items, values, default (= values[defaultStop]),
//                 defaultStop, boldStop when the host appends a stop
//   numslider     type "slider", min, max, default (clamped), rawDefault,
//                 unit + unitPos when the record names a unit
//   popup         type "popup", items, default (clamped index)
//   checkbox      type "checkbox", default 0/1
//   button        type "button" (no value: the saver never presses buttons)
struct CatalogControl {
  int index = 0;              // slot: what SET <idx> / ADCVSET address
  std::string name;
  std::string kind;           // stringslider | numslider | popup | button | checkbox
  std::string type;           // slider | popup | button | checkbox
  std::vector<std::string> items;
  std::vector<int> values;    // stringslider: the value sent for each stop
  std::optional<int> def;     // "default"
  std::optional<int> default_stop, bold_stop;
  std::optional<int> min, max, raw_default;
  std::string unit;           // non-empty only when the record has one
  std::string unit_pos;       // "none" | "prefix" | "suffix" (with unit)
};

struct CatalogModule {
  std::string id;             // "ad40.toasters" / "classic.toast3" / "ad32.toilet"
  std::string display_name;   // unique within the lane over the whole catalog (PACKAGES.md §6)
  std::string lane;           // "pe32" | "ne16" (from the file header, never the folder)
  std::string path;           // relative to <assets>\win, '/'-separated, on-disk case
  std::string about;
  std::optional<std::string> credits;   // Classic only, when the module has 2000/10
  std::vector<CatalogControl> controls;
  std::string entry;
  std::vector<std::string> needs;       // non-system DLLs it imports (sorted, upper case)
  std::vector<std::string> system;      // system DLLs it imports (sorted, upper case)
  // PACKAGES.md §6 (optional for front-ends; every entry has them):
  std::string package, package_title;  // "deluxe", "After Dark 4.0 Deluxe"
  std::string module_name;              // the name before disambiguation (trimmed, overrides applied)
  std::string md5;                      // of the module file
  std::string same_as;                  // id of the first entry with the same md5 ("" = none)
  // The module ABI when it is not After Dark's: "intermission" for an
  // Intermission module, IMX, ASA or IMQ ("" = After Dark; the JSON then has
  // no "abi").
  std::string abi;
  // The fixed screen the module is shown at, "WxH", from its package's
  // registry entry ("" = any screen; the JSON then has no "screen").
  std::string screen;
};

// An installed package's box cover as the catalog lists it (COVERS.md §2.7).
// Paths are relative to <win>, '/'-separated. A generated cover (the
// front-ends draw it) is just { "origin": "generated" }.
struct CatalogCover {
  std::string origin = "generated";  // "user" | "download" | "disc" | "generated"
  std::string tile, tile_md5;        // the 640x800 tile.png and its md5 (changes whenever the tile does)
  std::string image;                 // the picture it shows: user.png or original.png
  int width = 0, height = 0;         // of `image`
  std::string art;                   // the original's "box" | "disc" | "splash" | "panel" (none for a user picture)
  std::string label, credit;         // for people: "Box front", "Wikisimpsons"; "Your own picture", ""
  std::string original = "generated";  // the origin of the original (under a user picture too)

  bool generated() const { return origin == "generated"; }
};

// One installed package as the catalog's top-level "packages" lists it.
struct CatalogPackage {
  std::string id, title, short_title, root, verified, imported_utc;
  size_t modules = 0;
  CatalogCover cover;
};

// ---- building blocks (exposed for the tests) ----------------------------------

// Windows-1252 bytes to UTF-8; the five undefined bytes become U+FFFD (as
// Python's cp1252 codec with errors='replace' does).
std::string cp1252_to_utf8(std::string_view s);
// The bytes before the first NUL (the whole input when there is none).
std::string_view c_string(std::string_view s);

// The About-box RTF reduced to plain text: paragraphs and line breaks become
// '\n', \tab a tab, \'xx the Windows-1252 character, \~ a space; font/colour
// tables, stylesheets, info, pictures, objects and \* destinations are
// dropped; reading stops at the end of the outermost group. Trailing blanks
// on a line are removed, runs of 3+ newlines collapse to 2, and the result is
// trimmed.
std::string rtf_to_text(std::string_view rtf);

// A type-1000 record (ABI.md §2.10.2) for `slot`; nullopt for kind 0 (none),
// an unknown kind, or a record too short to hold its kind.
std::optional<CatalogControl> parse_control_record(std::string_view rec, int slot);

// STRINGLIST: WORD count, then that many NUL-terminated strings (Latin-1).
std::vector<std::string> parse_stringlist(std::string_view data);

// An After Dark 2.0 About text as the catalog lists it (Package::About::ad20,
// above): the trailing "Berkeley Systems Authorized User." line dropped, and
// " \n" before a lower-case ASCII letter joined into " ".
std::string ad20_about(std::string_view about);

// A DLL the host supplies (the catalog's `system` rather than `needs`):
// KERNEL32.DLL, USER32.DLL, ... and KERNEL, USER, GDI, MMSYSTEM, COMMDLG,
// SHELL, KEYBOARD, WIN87EM. `name` in upper case, as the catalog lists it.
bool is_system_dll(std::string_view name);

// The name of the button an Intermission module's configure dialog is behind
// (Intermission's own "Confi&gure...", without the mnemonic).
inline constexpr char kIntermissionConfigure[] = "Configure...";

// One of Intermission's own readers, by its file name: IM???PLY.IMQ, as
// every one is named (IMIMXPLY "IMX Player", IMASAPLY "ASA Player", IMAD_PLY,
// IMFLCPLY, IMFLIPLY, IMNSSPLY, IMSAPPLY, IMSCRPLY, IMSEQPLY, IMSPXPLY),
// without case. Never a module: an IMQ module (The Far Side's PTERY.IMQ) is
// named otherwise. `file_name` without a directory.
bool is_intermission_reader(std::string_view file_name);

// Everything the catalog says about one module file. `rel_path` is the
// catalog "path"; the id comes from the file name and the lane — Deluxe's
// "ad40.<base>" / "classic.<base>" when `package` is null or Deluxe, else
// "<package>.<base>". The package fields and md5 are filled; module_name is
// the trimmed display name (overrides are the merge's job). Throws
// ImportError(source_invalid) for a file that is neither PE32 nor NE nor an
// ASA animation, or an NE the ne16 lane would refuse to run (the message is
// the lane's reason; an IMQ named as Intermission's readers are, a reader),
// and adw::loader::LoaderError for a damaged image.
CatalogModule catalog_module(const std::filesystem::path& file, const std::string& rel_path,
                             const Package* package = nullptr);

// ---- the whole catalog ----------------------------------------------------------

// One installed package for build_catalog: its registry entry, where its
// files are now (its root, or an import's staging copy of it) and what its
// import record says.
struct CatalogTree {
  const Package* package = nullptr;
  std::filesystem::path dir;
  std::string verified, imported_utc;
  CatalogCover cover;  // its box cover (installed_trees and imports fill it in)
};

struct CatalogDoc {
  std::vector<CatalogPackage> packages;
  std::vector<CatalogModule> modules;
};

// The merged catalog (PACKAGES.md §6) over `trees`, which are in registry
// order. Per package, its module dirs in registry order (Deluxe: AD40\*.AD
// sorted, ENGINE\STARRYNI.AD, CLASSIC\*.AD sorted; every other package: each
// module dir's *.AD and *.IMX — a Delrina release's also *.ASA and *.IMQ —
// sorted together, then ENGINE's *.AD and *.IMX; never WINDOWS),
// paths "<package root>/<dir>/<file>" whatever the directory is called now. Ids are unique (a second file with a taken id is skipped
// and logged); name overrides apply; display names are made unique per lane;
// sameAs links byte-identical modules. A module that cannot be read, or that
// its lane would refuse, is left out and reported through `log`, so one
// damaged file never costs the user every other module.
CatalogDoc build_catalog(const std::vector<CatalogTree>& trees,
                         const std::function<void(const std::string&)>& log = {});

// A Deluxe FILES tree alone (<assets>\win\FILES, or an import's staging copy
// of it): build_catalog over that one tree, modules only. Paths come out as
// "FILES/…" whatever the directory is called. Throws
// ImportError(source_invalid) when `files_dir` is not a directory.
std::vector<CatalogModule> scan_catalog(const std::filesystem::path& files_dir,
                                        const std::function<void(const std::string&)>& log = {});

// The JSON document (DESIGN.md §6a, PACKAGES.md §6, plus the prototype's
// extra fields), laid out as Python's json.dump(indent=1,
// ensure_ascii=False) lays out the prototype's, so the two diff cleanly.
std::string render_catalog_json(const std::vector<CatalogModule>& modules,
                                const std::vector<CatalogPackage>& packages = {});
std::string render_catalog(const CatalogDoc& doc);  // the same for a whole document

inline constexpr char kCatalogFileName[] = "catalog-win.json";
inline constexpr char kCatalogGenerator[] = "adimport 1.3";

struct CatalogResult {
  Status status = Status::error;
  std::string message;
  std::filesystem::path path;   // <win>\catalog-win.json
  size_t modules = 0, controls = 0;
};

// adimport --catalog-only: regenerates <win_assets_dir(root)>\catalog-win.json
// from every package already installed there (root empty =
// default_assets_root()); FILES is not required, one installed package is.
// Holds import.lock while it reads, so it never scans a tree an import is
// swapping, repairs what an interrupted import left first, and replaces the
// file atomically. Never throws.
CatalogResult regenerate_catalog(const std::filesystem::path& assets_root,
                                 const std::function<void(const std::string&)>& log = {},
                                 std::span<const Package> registry = {});

}  // namespace adw::import
