// What adimport's windows say and show, as pure functions over the importer's
// data (COVERS.md §4.2): the installed releases and their covers, the
// Internet Archive list, the progress wording, the result texts, and the
// session's exit code. No windows here, so import.gui_model tests it all.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "covers.h"
#include "importer.h"

namespace adw::import::gui {

std::string mb(uint64_t n);   // "381.7 MB"

// ---- progress -----------------------------------------------------------------------

// The progress window's first line: "Downloading <title> from the Internet Archive",
// "Copying <title>", "Getting the cover art" … with " (2 of 4)" when several run.
std::wstring phase_instruction(Progress::Phase p, const std::string& package, size_t step = 0, size_t steps = 1);
// "123.4 MB of 400.0 MB (5.0 MB/s)"; `speed` is "" or "5.0 MB/s".
std::wstring amount_line(uint64_t done, uint64_t total, const std::string& speed);

// ---- the Sources page ---------------------------------------------------------------------

// Its first line: before anything is imported, "Long After Dark runs the original Windows
// modules of A, B, … and L. Choose where to copy them from." (every release of the registry,
// what a newcomer with a disc wants to know); once some are, and listed below it, the count
// in words instead ("… of twelve releases. …"), so the page keeps room for the list.
std::wstring sources_intro(bool any_installed, std::span<const Package> registry = {});

// ---- installed releases ---------------------------------------------------------------

// catalog-win.json's packages[] module counts (id -> modules); empty when the catalog is
// missing or unreadable. Reads only.
std::map<std::string, int> catalog_module_counts(const std::filesystem::path& win_dir);

// How an import was checked, for people: "verified against the original disc" ("disks" for a
// floppy set; "the known ZIP" for a release known by the ZIP of its install files; import.json's
// "image"), "every file verified" ("files"), "partly verified", "not verified". `image_md5`, when
// set, is the image or ZIP the import came from (import.json's imageMd5): the known image with
// that md5 is the one named, else the package's first.
std::wstring verified_words(const std::string& verified, const std::string& package, const std::string& image_md5 = "");

struct InstalledRow {
  std::string id;
  std::wstring title;
  std::wstring short_title;   // the cover's caption: "Deluxe", "10th Anniversary"
  std::wstring modules;       // "84 modules" ("" when the catalog doesn't say)
  std::wstring detail;        // "84 modules · verified against the original disc"
  CoverInfo cover;
};
// The installed releases in registry order, with their covers. Reads only (never creates
// the assets folder).
std::vector<InstalledRow> installed_rows(const std::filesystem::path& assets, std::span<const Package> registry = {});
// The installed releases whose cover is still generated (no picture yet) and that the registry
// can download one for: what Sources' "Get the covers" fetches.
std::vector<std::string> missing_cover_ids(const std::vector<InstalledRow>& rows);
// "One release has no cover picture yet." / "3 releases have no cover picture yet." ("" for 0).
std::wstring missing_covers_note(size_t n);
// An installed release's cover on Sources (a button that opens its menu, "Change cover…" and
// "Remove…"): what screen readers call it, "After Dark 4.0 Deluxe, 84 modules · verified
// against the original disc" (& doubled: a button's text is read for mnemonics), and its
// tooltip, the title and the detail on lines of their own, then what a click offers.
std::wstring installed_tile_name(const InstalledRow& r);
std::wstring installed_tile_tip(const InstalledRow& r);

// ---- removing a release ------------------------------------------------------------------

// The Remove window (--gui --remove <id>, or "Remove…" on a Sources cover): its question,
// "Remove The Simpsons Screen Saver?", and what removing it does: its modules are deleted
// from this computer, its cover is kept, and it can be imported again.
std::wstring remove_question(const std::wstring& title);
std::wstring remove_text(const InstalledRow& r);
// What the window says when the release is not installed (nothing to remove):
// "The Simpsons Screen Saver is not installed, so there is nothing to remove."
std::wstring not_installed_text(const std::wstring& title);
// A removal that failed: what remove_package said, after "Nothing was removed."
std::wstring remove_failed_text(const std::string& message);
// Sources' line after a removal: "Removed The Simpsons Screen Saver."
std::wstring removed_note(const std::wstring& title);

// ---- the Internet Archive list ----------------------------------------------------------

struct DownloadRow {
  std::string id;
  std::wstring title;
  std::wstring text;     // the card: "<title>\n<kind> · <size>\n<state>"
  bool installed = false;
  uint64_t size = 0;
  CoverInfo cover;
};
// One row per downloadable release in registry order (only `package` when it is set).
// `download_dir` is where "already downloaded" is looked for (by size).
std::vector<DownloadRow> download_rows(const std::filesystem::path& assets, const std::string& package,
                                       const std::filesystem::path& download_dir,
                                       std::span<const Package> registry = {});
// The "Every release not imported yet" card: only when two or more are not imported.
struct AllRow {
  std::vector<std::string> ids;
  std::wstring text;
};
std::optional<AllRow> all_missing_row(const std::vector<DownloadRow>& rows);
// The chooser's third card ("Download from the Internet Archive…\n<sizes>…"), or "" when
// nothing (within `package`) can be downloaded.
std::wstring download_card_text(const std::string& package, std::span<const Package> registry = {});

// ---- results ------------------------------------------------------------------------

enum class Outcome { success, partial, failure };
struct ResultText {
  Outcome kind = Outcome::failure;
  std::wstring heading;   // "Imported The Simpsons Screen Saver: 15 modules"
  std::wstring body;      // what today's message boxes said
  std::wstring details;   // for "Copy details": the heading, the body and every mismatched file
};
std::wstring failure_heading(const ImportResult& r);
ResultText single_result(const ImportResult& r);
ResultText several_result(const std::vector<std::string>& ids, const std::vector<ImportResult>& rs);

// ---- covers -------------------------------------------------------------------------

// "Box front · Wikisimpsons", "Installer art from your disc", "Your own picture", "No picture yet".
std::wstring origin_line(const CoverInfo& c);
// "Box front · Wayback Machine" (either part may be empty).
std::wstring source_words(const std::string& label, const std::string& credit);
// The cover window's line under "Use the original cover", while your own picture is in use:
// "Original: Disc label · Internet Archive" ("" without a user picture).
std::wstring original_note(const CoverInfo& c);
// ...and under "Download the original cover": "Gets the Box front · Wayback Machine" ("" when
// there is nothing better to download, or downloads are off: the button is then hidden).
std::wstring download_note(const CoverInfo& c, bool downloads_allowed);
// The result of "Get the covers" (refresh_covers over the releases with no picture):
// "Got 3 covers", "Got 2 of 3 covers", "The cover downloads failed", with a line per release.
ResultText covers_result(const std::vector<CoverResult>& rs);

// ---- the session ----------------------------------------------------------------------

// COVERS.md §4.1: 0 when anything was imported or removed or any cover changed during the
// session, else the first failure, and 5 (cancelled) when nothing changed.
struct Tally {
  bool changed = false;
  int first_failure = 0;
  void import_result(Status s);
  void cover_changed() { changed = true; }
  void release_removed() { changed = true; }
  int exit_code() const;
};

}  // namespace adw::import::gui
