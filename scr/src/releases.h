// Releases (installed packages) as the settings dialog and the saver see them
// (COVERS.md §1): the box-cover strip's filter and how Collections applies,
// what Random plays under it, and the module list grouped by release. Pure,
// window-free code, so the tests can pin every rule.
#pragma once

#include <functional>
#include <set>
#include <string>
#include <vector>

#include "catalog.h"
#include "settings.h"

namespace adw::scr {

// ---- the filter (COVERS.md §1.1, §1.6, §1.9) ------------------------------------------

// The strip shows with two or more releases installed.
bool strip_shown(const Catalog& c);

// Collections as it applies: the saved ids that are installed releases, in
// the catalog's release order. Empty (= every release) when none of them is,
// or when every installed release is, which also covers a catalog with one
// release (the strip hidden).
std::vector<std::string> effective_collections(const std::vector<std::string>& saved, const Catalog& c);

// Module `m` of `c` belongs to the (effective) filter `f`; an empty filter holds everything.
bool in_filter(const Catalog& c, const Module& m, const std::vector<std::string>& f);

// ---- what Random plays (COVERS.md §1.8) ------------------------------------------------

// Modules with the same bytes share a key: `sameAs` when set, else the id.
const std::string& same_as_key(const Module& m);

struct RotationPlan {
  std::vector<std::string> ids;       // the shuffle bag, in catalog order
  std::string lead;                   // a named Module that plays first ("" when none)
  bool collections_ignored = false;   // nothing checked in the selected releases: all of Randomize plays
};

// For a saver that rotates (Settings::rotates()):
//  1. Randomize, or every catalog id when it names none the catalog has;
//  2. only the ids in the effective Collections;
//  3. one id per `sameAs` key, the first in catalog order (a byte-identical
//     copy checked in two releases plays once per pass);
//  4. when that leaves nothing (a hand-edited file), step 1 alone, and
//     `collections_ignored` says so;
//  5. a Module leading a Randomize list still plays first, whatever the
//     filter; it stands in for a copy of itself in the bag.
// `available` (optional) drops ids whose file is missing before step 3, so a
// present copy stands in for a missing one.
RotationPlan effective_rotation(const Settings& s, const Catalog& c,
                                const std::function<bool(const std::string&)>& available = nullptr);

// Whether what Random plays for `s` depends on the host's --capabilities:
// the saver rotates, and its rotation (effective_rotation, over `available`)
// holds a module of another module ABI than After Dark's, which a host too
// old for it can't run. The saver then waits for the host's answer before
// its first host starts, and leaves out what that host can't run (saver.cc,
// rotation_for_host); any other rotation never waits.
bool rotation_needs_capabilities(const Settings& s, const Catalog& c,
                                 const std::function<bool(const std::string&)>& available = nullptr);

// What Random plays on a host that may not run every module (the saver, with
// the host's answer: `runs` says whether it lists a module's lane and ABI).
// `plan` is effective_rotation over the modules both `available` and `runs`:
// a Randomize list of only modules the host can't run gives way to every
// module it can, and a lead it can't run is left out. `left_out` counts the
// modules the rotation would have held without `runs` (effective_rotation
// over `available`: its ids and its lead) that `runs` rejects, which is what
// the saver's log says it left out. An empty plan means the host can run
// none of the modules available (then `left_out` is never 0).
struct HostRotation {
  RotationPlan plan;
  size_t left_out = 0;
};
HostRotation rotation_for_host(const Settings& s, const Catalog& c,
                               const std::function<bool(const std::string&)>& available,
                               const std::function<bool(const std::string&)>& runs);

// The own screens (geometry.h: own_screen; {0, 0} for a module that follows
// the display) the first module a /s window plays may have, known before its
// rotation is built (the saver takes the desktop seed at their screens, three
// at most, geometry.h: module_screen, plan_seed_shots): the chosen module's when it
// plays alone (every available module's when it is gone: the saver picks
// one); a rotation's modules' (and its lead's); every available module's
// when the host's answer may change what Random plays
// (rotation_needs_capabilities: a list of only modules a host can't run
// gives way to every one it can). Modules of one own screen share it: an
// Intermission and a Star Trek module (640x480 both) are one screen.
// Empty when nothing is available.
std::set<SizeI> first_module_screens(const Settings& s, const Catalog& c,
                                     const std::function<bool(const std::string&)>& available = nullptr);

// ---- the module list, by release (COVERS.md §1.7) ------------------------------------

struct ListRow {
  int module = -1;          // catalog index
  std::string label;        // what the row says
};
struct ListGroup {
  int release = -1;         // catalog release index; the ListView group id is 1 + this
  std::vector<ListRow> rows;
};
struct ListModel {
  std::vector<ListGroup> groups;     // shown releases with modules, in release order
  size_t shown = 0, total = 0;       // rows listed, modules in the catalog
  std::vector<int> order() const;    // catalog indices, top to bottom
  bool shows(int module) const;
  const ListGroup* group(int release) const;
};

// The rows under `filter` (effective; empty = everything): one group per
// release, rows sorted case-insensitively (digits as numbers) by name. Each
// row shows the module's name; two rows of a group that would read the same
// tell themselves apart: the Classic (ne16) one gets " (Classic)" and, if
// they still read the same, each gets its upper-case file stem, " (BADDOG3)".
ListModel build_list(const Catalog& c, const std::vector<std::string>& filter);

// The module the details card shows once the list is rebuilt under a new
// filter (catalog indices; -1 = none): the chosen module while its row is
// listed (it is the row selected), else the module shown before while its
// row is listed, else the list's first row, else -1 (the filter leaves no
// rows: the details show that). The details never show a module the list
// hides. Showing a module is not choosing it: the chosen module (what
// Single saves) changes only when the user picks a row.
int details_after_filter(const ListModel& list, int chosen, int shown);

// Titles of the other releases that ship this module's bytes (sameAs either
// way), in release order: the details chip's "Also on:" tooltip. Empty when none.
std::vector<std::string> also_on(const Catalog& c, int module);

// ---- words ----------------------------------------------------------------------------

// The strip's status box: "Click covers to filter the list" (nothing
// selected), "Showing 2 of 5 releases", "Showing all 5 releases".
std::wstring strip_status(size_t selected, size_t releases);
// A filter is active (the "Show all" link shows, unselected covers dim)
// while some, but not all, of the releases are selected.
inline bool filter_active(size_t selected, size_t releases) { return selected > 0 && selected < releases; }
// The count above the list: "15 of 202" while filtered, else "202" ("" with none).
std::wstring modules_count_label(size_t shown, size_t total);
// A tile's accessible name, "The Simpsons Screen Saver, 15 screen savers",
// with any '&' doubled (a button's text is read for mnemonics).
std::wstring tile_name(const Release& r, size_t modules);
// ...and its tooltip: "<title> — 15 screen savers", then how to use it (a
// click filters; a right-click offers "Change cover…" and "Remove …").
std::wstring tile_tip(const Release& r, size_t modules);
// "Also on: After Dark 10th Anniversary, Totally Twisted After Dark" ("" with none).
std::wstring also_on_tip(const std::vector<std::string>& titles);

} // namespace adw::scr
