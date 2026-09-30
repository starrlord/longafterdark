// catalog-win.json (DESIGN.md §6a): the only description of the modules the
// front-end reads. Parsing is lenient about optional fields (a hand-written
// fixture and the generated catalog must both load) and strict about the
// ones the saver cannot run without (id, path). The release fields
// (`package`, `packageTitle`, `moduleName`, `sameAs`, the top-level `packages`
// with each release's `cover`) are all optional (COVERS.md §1.10), and so are
// `abi`, the module ABI when it is not After Dark's ("intermission" for Star
// Wars Screen Entertainment's IMX modules; absent = "afterdark"), and
// `screen`, the fixed screen of a module that composes a scene of that size
// ("640x480" for Star Trek: The Screen Saver's, ScreamSavers' and Marvel's;
// absent = none).
//
// Control shapes, as the generator (adimport, importer/catalog.h;
// ABI.md §2.10)
// writes them on top of the §6a core:
//   slider   numeric: min/max (+ optional unit/unitPos)
//            string:  items + values — discrete stops; the value sent is
//                     values[stop], a lower bound on a 0..100 scale
//                     (items without values: the value is the stop index)
//   popup    items; the value is the item index (+ min, if given)
//   checkbox 0/1
//   button   no value; the saver never presses buttons (ABI.md §2.10.4)
#pragma once

#include <string>
#include <vector>

#include "geometry.h"

namespace adw::scr {

// The module ABI a catalog entry without "abi" has, and the one every host
// runs (a host whose --capabilities lists no "abis=" runs this one alone).
inline constexpr char kAfterDarkAbi[] = "afterdark";
// Star Wars Screen Entertainment's IMX modules (Delrina's Intermission
// engine, lane ne16). Their emulated screen is their own 640x480
// (geometry.h: module_screen).
inline constexpr char kIntermissionAbi[] = "intermission";

enum class ControlType { slider, checkbox, popup, button, unknown };

struct Control {
  int index = 0;                   // SET <idx> / ADCVSET slot
  std::string name;
  ControlType type = ControlType::unknown;
  std::string type_name;           // as written, for unknown types
  int min = 0, max = 100;          // numeric slider range; popup: value = min + item
  int def = 0;
  std::vector<std::string> items;  // popup entries / string-slider stop labels
  std::vector<int> values;         // string slider: value of each stop (ascending)
  // String slider: the value of the stop the original control panel appended
  // (catalog `boldStop`, ABI.md §2.10.5); has_bold is false when there is none.
  bool has_bold = false;
  int bold_value = 0;
  std::string unit;                // numeric slider label unit ("%")
  bool unit_prefix = false;        // "$5" rather than "5%"

  // A control the user can set and the host receives a value for.
  bool settable() const;
  // A slider that moves between labelled stops rather than over min..max.
  bool stepped() const { return type == ControlType::slider && !items.empty(); }
  int stop_count() const;
  // The stop showing `value`: the last one whose value is <= it (how the
  // original control panel picks a string slider's default stop).
  int stop_of(int value) const;
  int value_of_stop(int stop) const;
  // Into the control's valid value range (stepped sliders snap to a stop).
  int clamp(int v) const;
  // What the settings panel shows next to a slider: the stop's label, or the
  // number with its unit.
  std::string value_label(int value) const;
};

struct Module {
  std::string id;
  std::string display_name;
  std::string lane;                // "pe32" | "ne16"
  // The module ABI, which a host must list (--capabilities abis=) to run it:
  // "afterdark", or "intermission" (an IMX module; lane ne16). Never empty.
  std::string abi = kAfterDarkAbi;
  // The screen the catalog gives it ("screen": "WxH", PACKAGES.md §6): a
  // module shown at a fixed size (Star Trek: The Screen Saver's, ScreamSavers'
  // and Marvel's, 640x480, some of which compose a fixed scene) gets that
  // screen whatever the display and the Resolution setting, scaled to fit, as
  // an Intermission module gets its 640x480 by its ABI (geometry.h:
  // own_screen, module_screen). {0, 0} when the entry has none, or one this
  // saver can't use: not "<w>x<h>" with 1 to 5 decimal digits either side of
  // the x (or X; leading zeros count, so "000640x480" is none, and no axis can
  // overflow), an axis outside 1..8192 or more than 4096x4096 pixels in all
  // (no frame the stream parser reads back, frame_parser.h).
  SizeI screen;
  std::string path;                // relative to <assets>\win, forward slashes
  std::string about;
  std::string credits;             // Classic modules: the credits line (AD4 ones carry theirs in `about`)
  std::vector<Control> controls;
  // The release it came from (COVERS.md §1.10; PACKAGES.md §6). Optional in
  // the file: a module without `package` is "deluxe" when its id starts
  // "ad40." or "classic.", else "other".
  std::string package, package_title;
  std::string module_name;         // catalog `moduleName` as written ("" when absent)
  // What the settings dialog shows for it: `moduleName` (else `displayName`)
  // with the names the Windows 3.x control panel cut short shown whole.
  // No " (<short title>)" suffix: the list groups by release instead.
  std::string name;
  std::string same_as;             // `sameAs`: the id of the first module with the same bytes, or ""
  int release = -1;                // index into Catalog::releases (always set by parse_catalog)

  const Control* control(int index) const;
};

// A release's box cover as the catalog describes it (COVERS.md §2.7).
struct Cover {
  std::string origin = "generated";   // user | download | disc | generated
  std::string tile;                   // 640x800 PNG relative to <assets>\win ("" when generated)
  std::string tile_md5;               // changes whenever the tile does
  std::string image, art, label, credit, original;
  int width = 0, height = 0;
  bool generated() const { return origin == "generated" || tile.empty(); }
};

// An installed release: one entry of the catalog's top-level `packages`
// array (or, for a catalog written before there was one, made up from the
// modules' `package` fields).
struct Release {
  std::string id;                  // registry id: deluxe, ad10, ad32, tt, simpsons, ...
  std::string title;               // "After Dark 4.0 Deluxe"
  std::string short_title;         // "Deluxe" (the title when the catalog has none)
  int modules = 0;                 // as the catalog counts them (0 when it doesn't say)
  Cover cover;
};

struct Catalog {
  int version = 0;
  std::vector<Module> modules;
  // Installed releases in the catalog's (registry) order. Every module's
  // `release` indexes it: a module whose package the array doesn't list gets
  // an entry of its own at the end.
  std::vector<Release> releases;
  bool has_packages = false;       // the file had a `packages` array

  const Module* find(const std::string& id) const;
  int release_index(const std::string& id) const;   // -1 when not installed
  // Modules listed for release `index`.
  size_t modules_in(int index) const;
};

bool parse_catalog(const std::string& json_text, Catalog& out, std::string* error);
bool load_catalog(const std::wstring& path, Catalog& out, std::string* error);

} // namespace adw::scr
