// settings.ini (DESIGN.md §6a): UTF-8 INI owned by the front-end.
//   [Saver]  Module=<id>|random  Randomize=<id>,<id>,…  DurationMin=<n>|0
//            Scale=1.0|1.5  Monitors=all|primary
//            DifferentPerMonitor=1|0  (a saver that rotates, on several monitors:
//                                      1 = a different module on each, each
//                                      switching on its own; 0 or missing = the
//                                      same module on all, switching together)
//            RandomizeSaved=<id>,<id>,…|-  (the dialog's checklist while a single
//                                          module is chosen, "-" = nothing
//                                          checked; the saver ignores it)
//            Collections=<package id>,…   (the box-cover strip's filter; empty or
//                                          missing = every release; COVERS.md §1.9)
//            Sound=1|0  Volume=0..100  SoundMonitor=primary   (AUDIO.md §9)
//   [Module.<id>]  <index>=<value>   one line per control the user has set
// Saving edits the existing file in place (IniFile keeps unknown keys,
// sections and comments), so a hand-edited or newer-version file survives a
// round trip through an older dialog.
#pragma once

#include <cstdint>
#include <map>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace adw::scr {

inline constexpr int kDefaultVolume = 50;   // After Dark's own default (AUDIO.md §4)

class IniFile {
 public:
  void parse(std::string_view text);
  std::string serialize() const;     // CRLF line endings, no BOM

  // Section and key lookups are case-insensitive, like GetPrivateProfileString.
  const std::string* get(std::string_view section, std::string_view key) const;
  void set(std::string_view section, std::string_view key, std::string_view value);
  void remove_key(std::string_view section, std::string_view key);
  void remove_section(std::string_view section);
  void clear_entries(std::string_view section);   // keeps the section, its place and its comments
  std::vector<std::string> section_names() const;
  std::vector<std::pair<std::string, std::string>> entries(std::string_view section) const;

 private:
  struct Line {
    std::string raw;                 // verbatim text for comments/blank/unparsed lines
    std::string key, value;
    bool entry = false;
  };
  struct Section {
    std::string name;                // "" = lines before the first [section]
    std::vector<Line> lines;
  };
  Section* find(std::string_view name);
  const Section* find(std::string_view name) const;

  std::vector<Section> sections_;
};

struct Settings {
  std::string module = "random";              // catalog id, or "random"
  std::vector<std::string> randomize;         // rotation subset; empty = every module
  int duration_min = 5;                       // rotation interval; 0 = never rotate
  double scale = 1.0;                         // 1.0 (480-line) or 1.5 (720-line)
  bool all_monitors = true;                   // false = primary only, others black
  // [Saver] DifferentPerMonitor: when the saver rotates (Random, or a
  // Randomize list) on several monitors, each monitor plays a different
  // module, on a rotation of its own. False, the default (and a file without
  // the key, written before there was one): they all play the same module
  // and switch together, following one rotation (SharedRotation, saver.cc).
  bool different_per_monitor = false;
  // [Saver] StartFromDesktop=0: /s starts every module on black instead of
  // on a capture of the desktop (INTERACTION.md §8). No UI; default 1.
  bool start_from_desktop = true;
  std::map<std::string, std::map<int, int>> controls;   // module id -> index -> value
  // The Random checklist while "Show the selected module" is chosen: Randomize
  // must then be empty (a list makes the saver rotate), so the dialog keeps
  // the user's checks here. Empty = every module checked, unless
  // randomize_saved_none says nothing was (RandomizeSaved=-).
  std::vector<std::string> randomize_saved;
  bool randomize_saved_none = false;
  // [Saver] Collections: the releases (package ids) the settings dialog's
  // strip selected. Empty = every release. Read by the dialog and the saver,
  // which both apply it through effective_collections() (releases.h): ids of
  // releases not installed are ignored there, but kept here, so they stay in
  // the file until the next OK. Serializing leaves the file's key exactly as
  // written when it already says this list.
  std::vector<std::string> collections;
  // [Saver] Sound, Volume, SoundMonitor (AUDIO.md §9): the modules' sound, on
  // by default, played by the primary monitor's screen saver alone; Volume is
  // After Dark's own volume slider (ADVOLUME, 50 as in the original).
  // SoundMonitor is reserved: "primary" is the only value there is; another
  // one (a later version's) is kept as written and played as primary.
  bool sound = true;
  int volume = kDefaultVolume;
  std::string sound_monitor = "primary";

  bool is_random() const;
  // The saver rotates when Module=random or a Randomize list is set; a named
  // Module with a list plays first, then the list takes over.
  bool rotates() const { return is_random() || !randomize.empty(); }
  // A named Module in front of a Randomize list (hand-written, or kept from
  // an earlier version): it plays first, then the list rotates.
  bool has_lead() const { return !is_random() && !randomize.empty(); }
  bool operator==(const Settings&) const = default;
};

// ---- the settings dialog's choices ---------------------------------------------------
// Kept free of windows so the rules can be tested: which modules the dialog
// shows checked, and what its two modes save.

struct DialogChoice {
  bool random = true;                   // "Random" rather than "Show the selected module"
  // Checked ids, in list order. The dialog keeps one check per catalog id,
  // for every release, including releases its filter hides (COVERS.md §1.8).
  std::vector<std::string> checked;
  size_t total = 0;                     // modules in the catalog (0 = no catalog)
  std::string selected;                 // the chosen module id, "" when none
  // The box-cover strip (COVERS.md §1.9). While it is hidden (fewer than two
  // releases) OK leaves Collections as it was.
  bool strip = false;
  std::vector<std::string> collections; // the selected releases' ids, in packages[] order
  size_t releases = 0;                  // releases installed: every one selected is saved as "all"
  size_t shown_checked = 0;             // checked rows among those the list shows (Random needs one)
};

// Random with nothing checked in the releases shown is refused: the saver
// would have nothing of the user's choosing to play (and with a named Module
// leading the file's list, Random would quietly become that single module).
bool random_allowed(const DialogChoice& c);
inline constexpr wchar_t kRandomNeedsChecks[] = L"Check at least one module in the releases shown";

// The strip's selection as Collections saves it: without repeats, and empty
// when nothing is selected or every installed release is, so that releases
// imported later are included.
std::vector<std::string> normalize_collections(const std::vector<std::string>& selected, size_t releases);

// The checklist to show for `s`: the rotation list when the saver rotates,
// otherwise the one kept for it. Empty = every module, unless
// dialog_checklist_none(s): the kept checklist had nothing checked.
const std::vector<std::string>& dialog_checklist(const Settings& s);
bool dialog_checklist_none(const Settings& s);
// How RandomizeSaved spells "nothing checked" (an empty value means "all").
inline constexpr char kRandomizeSavedNone[] = "-";

// `loaded` (the file as the dialog opened it) with the choice applied:
//  - Random: Module=random and the checked ids (all checked saves an empty
//    list, so modules imported later join in) -- unless the file had a named
//    Module leading its list, which keeps leading; its list is then always
//    written out, since a lead with an empty list would be a single module.
//  - Show the selected module: that Module, an empty Randomize (or the saver
//    would rotate) and the checklist kept in RandomizeSaved (all checked
//    keeps nothing, nothing checked keeps "-").
//  - No catalog (total 0): nothing to choose from, so mode and lists stay.
//  - Collections: the strip's selection, normalized, while the strip shows;
//    otherwise as loaded.
Settings apply_dialog_choice(const Settings& loaded, const DialogChoice& c);

Settings parse_settings(std::string_view text);
// Applies `s` onto `base` (the file's current contents) and returns the text.
std::string serialize_settings(const Settings& s, std::string_view base = {});
bool load_settings(const std::wstring& path, Settings& out);   // false if unreadable (out = defaults)
bool save_settings(const std::wstring& path, const Settings& s);

// "0=50,1=1" — the ADCVSET form (DESIGN.md §1), ascending by index.
std::string format_cvset(const std::map<int, int>& values);

// A shuffle bag over the rotation subset: every module plays once per pass,
// and a new pass never starts with the module that just ended. `first`
// (Module=<id> alongside a Randomize list) plays before the bag: it is
// moved to the front when it is in the subset, and otherwise plays once
// ahead of it.
class Rotation {
 public:
  Rotation(std::vector<std::string> ids, uint32_t seed, const std::string& first = {});
  const std::string& current() const;     // "" when there is nothing to play
  const std::string& next();
  size_t size() const { return order_.size() + (lead_.empty() ? 0 : 1); }
  bool empty() const { return size() == 0; }

 private:
  void shuffle();
  std::vector<std::string> order_;
  std::string lead_;                      // plays once, before order_
  size_t pos_ = 0;
  std::mt19937 rng_;
};

// The rotation every monitor follows when the saver rotates without
// DifferentPerMonitor (saver.cc): one shuffle bag (a Rotation) and one clock,
// so they all play the same module and switch together; a monitor that
// joins (plugged in while it runs) plays the module the others play. The
// rules for moving it on, kept free of windows for the tests:
//  * the clock (DurationMin) moves every monitor on to the next module, but
//    not while the primary monitor's module plays a game it may not be
//    switched away from (INTERACTION.md §4.2): the rotation then waits (the
//    saver asks again every second) and moves on once the game ends;
//  * a module a monitor's host fails three times in a row is skipped on
//    every monitor, except while another monitor's module plays such a game
//    (the player keeps it; the failing monitor says it can't start it), and
//    except when that monitor has failed every module in turn without a
//    frame: then it, not the module, is at fault, and it no longer moves the
//    others on (it retries at a relaxed pace until the clock moves them).
class SharedRotation {
 public:
  SharedRotation(std::vector<std::string> ids, uint32_t seed, const std::string& first = {})
      : bag_(std::move(ids), seed, first) {}
  const Rotation& bag() const { return bag_; }
  const std::string& current() const { return bag_.current(); }
  size_t size() const { return bag_.size(); }
  bool empty() const { return bag_.empty(); }
  uint64_t step() const { return step_; }     // how many times it has moved on
  bool waiting() const { return waiting_; }   // the clock ran out during such a game
  // The clock ran out. `owner_plays`: the primary monitor's module plays a
  // game it may not be switched away from. True when every monitor moves on.
  bool tick(bool owner_plays);
  // A monitor's host failed the current module three times in a row. `dead`:
  // the modules that monitor has given up in a row without showing a frame
  // (this one included when it showed none of it); `owner`: it is the
  // primary monitor's. True when the module is skipped on every monitor.
  bool give_up(size_t dead, bool owner, bool owner_plays);

 private:
  void move_on();
  Rotation bag_;
  uint64_t step_ = 0;
  bool waiting_ = false;
};

bool iequals(std::string_view a, std::string_view b);

} // namespace adw::scr
