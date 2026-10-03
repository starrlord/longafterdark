// adimport's windows (COVERS.md §4.3):
//
//   test_import_gui model [scratch]              import.gui_model
//   test_import_gui shots <adimport.exe> [scratch]  import.gui_shots
//   test_import_gui flow  <adimport.exe> [scratch]  import.gui_flow (label gui)
//
// model: the wording and data the windows show, over a scratch assets tree
//   (no windows).
// shots: every page x light/dark/hc x 100/150/200% through the screenshot
//   hook (AD_IMPORT_TEST_SCREENSHOT): parked, cloaked windows that never show
//   on the desktop; each PNG exists and its body margin is pal.base. Every
//   release of the registry is imported (fake records), so the Sources page
//   lists them all; on work areas too short for the page its list shows as
//   many whole rows as fit. The Downloads page's list shows nine whole cards
//   at the most and scrolls, the page no taller than Sources.
// flow: real windows, pressed with TDM_CLICK_BUTTON as import.cli does:
//   Sources -> 103 -> Back -> Cancel is exit 5 and writes nothing (on the
//   way the Downloads list scrolls with the wheel, and the Down arrow brings
//   each card it focuses into view); then "Change cover" on a scratch
//   install with a synthetic picture is exit 0 and leaves a new tile.png
//   (skipped, 77, while set_cover is C's stub).
#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <oleacc.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <cmath>
#include <map>

#include "adw/ui/capture.h"
#include "adw/ui/image.h"
#include "adw/ui/theme.h"
#include "fixture.h"
#include "importer.h"
#include "md5.h"
#include "model.h"
#include "pages.h"
#include "run_process.h"
#include "test_util.h"

using namespace adw::import;
namespace fs = std::filesystem;
namespace gui = adw::import::gui;

namespace {

void write_text(const fs::path& p, const std::string& s) { test::write_bytes(p, std::vector<uint8_t>(s.begin(), s.end())); }

// An assets tree that lists as installed without a real import: Deluxe's
// FILES and import.json, the other packages' import.json (with the md5 of
// the image or ZIP it came from, when `image_md5` names one), and a catalog
// with each package's module count (what the windows read).
void fake_install(const fs::path& assets, const std::vector<std::string>& ids,
                  const std::map<std::string, std::string>& image_md5 = {}) {
  const fs::path win = assets / L"win";
  std::string packages;
  static const std::map<std::string, int> modules = {{"deluxe", 84},   {"ad10", 46},     {"ad32", 44},
                                                      {"tt", 13},       {"simpsons", 15}, {"swse", 14},
                                                      {"startrek", 16}, {"marvel", 1},    {"snoopy", 8},
                                                      {"looney", 12},   {"screams", 15},  {"disney", 16},
                                                      {"farside", 14},  {"dilbert", 16},  {"tng", 13},
                                                      {"castaway", 1},  {"opus", 16},     {"opusroad", 16},
                                                      {"flintstones", 15}, {"intermission", 54}};
  for (const std::string& id : ids) {
    auto md5 = image_md5.find(id);
    const std::string source = md5 == image_md5.end() ? "" : ", \"source\": {\"imageMd5\": \"" + md5->second + "\"}";
    const std::string record = "{\"version\": " + std::string(id == "deluxe" ? "1" : "2") + source +
                               ", \"verified\": \"image\", \"importedUtc\": \"2026-09-26T08:00:00Z\", \"fileCount\": 26}";
    if (id == "deluxe") {
      fs::create_directories(win / L"FILES" / L"AD40");
      write_text(win / L"import.json", record);
    } else {
      write_text(win / L"packages" / to_wide(id) / L"import.json", record);
    }
    packages += std::string(packages.empty() ? "" : ",\n") + "  {\"id\": \"" + id + "\", \"modules\": " +
                std::to_string(modules.at(id)) + "}";
  }
  write_text(win / L"catalog-win.json",
             "{\"version\": 1, \"generator\": \"test\", \"packages\": [\n" + packages + "\n], \"modules\": []}\n");
}

// A picture of our own: a gradient with a band, `w` x `h`, as a PNG.
bool synthetic_picture(const fs::path& png, int w, int h, uint8_t hue) {
  std::vector<uint8_t> bgr((size_t)w * h * 3);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      uint8_t* p = &bgr[((size_t)y * w + x) * 3];
      const bool band = y > h / 3 && y < h / 2;
      p[0] = band ? 240 : (uint8_t)(hue + x * 80 / w);
      p[1] = band ? 240 : (uint8_t)(40 + y * 120 / h);
      p[2] = band ? 240 : (uint8_t)(200 - hue / 2);
    }
  std::string err;
  return adw::ui::save_png_bgr(png.wstring(), w, h, bgr, &err);
}

std::map<std::string, std::string> read_report(const fs::path& p) {
  std::map<std::string, std::string> kv;
  std::string text = test::read_text(p);
  size_t pos = 0;
  while (pos < text.size()) {
    size_t nl = text.find('\n', pos);
    std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    size_t eq = line.find('=');
    if (eq != std::string::npos) kv[line.substr(0, eq)] = line.substr(eq + 1);
    if (nl == std::string::npos) break;
    pos = nl + 1;
  }
  return kv;
}

// The Downloads list in a report (list=, cards=): the height that shows, the
// whole list's and where each card ends, in px, and how many cards show whole
// (0 when the list ends inside one).
struct DownloadsList {
  int shown = -1, whole = -1, cards = 0;
  std::vector<int> ends;
};
DownloadsList downloads_list(std::map<std::string, std::string>& kv) {
  DownloadsList l;
  sscanf(kv["list"].c_str(), "%d,%d", &l.shown, &l.whole);
  const std::string& s = kv["cards"];
  for (size_t p = 0; p < s.size();) {
    l.ends.push_back(atoi(s.c_str() + p));
    const size_t comma = s.find(',', p);
    if (comma == std::string::npos) break;
    p = comma + 1;
  }
  for (size_t i = 0; i < l.ends.size(); ++i)
    if (l.ends[i] == l.shown) l.cards = int(i + 1);
  return l;
}

// The client area's height in a report (client=x,y,w,h), or -1.
int client_height(std::map<std::string, std::string>& kv) {
  int x = 0, y = 0, w = 0, h = -1;
  sscanf(kv["client"].c_str(), "%d,%d,%d,%d", &x, &y, &w, &h);
  return h;
}

// ---- import.gui_model ---------------------------------------------------------------

void test_model(const fs::path& dir) {
  using gui::Outcome;
  CHECK(gui::phase_instruction(Progress::Phase::cover, "The Simpsons Screen Saver") == L"Getting the cover art");
  CHECK(gui::phase_instruction(Progress::Phase::cover, "x", 1, 4) == L"Getting the cover art (2 of 4)");
  CHECK(gui::phase_instruction(Progress::Phase::download, "After Dark 3.2") ==
        L"Downloading After Dark 3.2 from the Internet Archive");
  CHECK(gui::phase_instruction(Progress::Phase::download, "") == L"Downloading the disc image");
  CHECK(gui::phase_instruction(Progress::Phase::check_image, "") == L"Checking the disc image");
  CHECK(gui::phase_instruction(Progress::Phase::copy, "Totally Twisted After Dark", 0, 2) ==
        L"Copying Totally Twisted After Dark (1 of 2)");
  CHECK(gui::amount_line(129394278, 419430400, "5.0 MB/s") == L"123.4 MB of 400.0 MB (5.0 MB/s)");
  CHECK(gui::amount_line(1048576, 0, "") == L"1.0 MB");

  // The exit code: 0 once anything changed, else the first failure, else 5.
  {
    gui::Tally t;
    CHECK_EQ(t.exit_code(), 5);
    t.import_result(Status::verify_failed);
    t.import_result(Status::network);
    CHECK_EQ(t.exit_code(), 3);
    t.cover_changed();
    CHECK_EQ(t.exit_code(), 0);
    gui::Tally c;
    c.import_result(Status::cancelled);
    CHECK_EQ(c.exit_code(), 5);
    gui::Tally ok;
    ok.import_result(Status::network);
    ok.import_result(Status::ok);
    CHECK_EQ(ok.exit_code(), 0);
  }

  // The Sources page's first line: every release by name before any is
  // imported; once some are (the list under it names them), how many.
  {
    const std::wstring first = gui::sources_intro(false);
    CHECK(first.find(L"Long After Dark runs the original Windows modules of After Dark 4.0 Deluxe, After Dark 10th "
                     L"Anniversary, ") == 0);
    for (const Package& p : builtin_packages()) CHECK(first.find(to_wide(p.title)) != std::wstring::npos);
    CHECK(first.find(L", The Disney Collection Screen Saver, The Far Side Screen Saver Collection, Scott Adams' "
                     L"Dilbert Screen Saver Collection, Star Trek: The Next Generation Screen Saver, Screen Antics: "
                     L"Johnny Castaway, Opus 'n Bill Screen Saver, Opus 'n Bill: On the Road Again!, The Flintstones "
                     L"Screen Saver Collection and Intermission 4.0. Choose where to copy them from.") !=
          std::wstring::npos);
    const std::wstring later = gui::sources_intro(true);
    CHECK(later.find(L"After Dark 4.0 Deluxe") == std::wstring::npos);
    CHECK(later.find(L" releases. Choose where to copy them from.") != std::wstring::npos);
    const auto reg = builtin_packages();
    CHECK(gui::sources_intro(true, std::span<const Package>(reg.data(), 3)) ==
          L"Long After Dark runs the original Windows modules of three releases. Choose where to copy them from.");
    CHECK(gui::sources_intro(true, std::span<const Package>(reg.data(), 1)).find(L" of one release. ") !=
          std::wstring::npos);
    CHECK(gui::sources_intro(false, std::span<const Package>(reg.data(), 2)) ==
          L"Long After Dark runs the original Windows modules of After Dark 4.0 Deluxe and After Dark 10th "
          L"Anniversary. Choose where to copy them from.");
    if (reg.size() == 10) CHECK(later.find(L" of ten releases. ") != std::wstring::npos);
    if (reg.size() == 12) CHECK(later.find(L" of twelve releases. ") != std::wstring::npos);
    if (reg.size() == 14) CHECK(later.find(L" of fourteen releases. ") != std::wstring::npos);
    if (reg.size() == 15) CHECK(later.find(L" of fifteen releases. ") != std::wstring::npos);
    if (reg.size() == 16) CHECK(later.find(L" of sixteen releases. ") != std::wstring::npos);
    if (reg.size() == 20) CHECK(later.find(L" of twenty releases. ") != std::wstring::npos);
    CHECK_EQ(reg.size(), size_t(20));
  }

  // Nothing installed, and an assets folder that doesn't exist stays that way.
  const fs::path none = dir / L"none";
  CHECK(gui::installed_rows(none).empty());
  CHECK(!fs::exists(none));

  const fs::path assets = dir / L"assets";
  fake_install(assets, {"deluxe", "tt"});
  auto rows = gui::installed_rows(assets);
  CHECK_EQ(rows.size(), size_t(2));
  if (rows.size() == 2) {
    CHECK(rows[0].id == "deluxe" && rows[1].id == "tt");
    CHECK(rows[0].title == L"After Dark 4.0 Deluxe");
    CHECK(rows[0].detail == L"84 modules \u00b7 verified against the original disc");
    CHECK(rows[1].detail == L"13 modules \u00b7 verified against the original disc");
    CHECK(rows[1].cover.package == "tt");
    // Both still have generated covers, and the registry can download one for each.
    CHECK((gui::missing_cover_ids(rows) == std::vector<std::string>{"deluxe", "tt"}));
    CHECK(gui::missing_covers_note(2) == L"2 releases have no cover picture yet.");
    CHECK(gui::missing_covers_note(1) == L"One release has no cover picture yet.");
    CHECK(gui::missing_covers_note(0).empty());
    // Their covers on Sources: the caption (the short title and the module
    // count), what screen readers call each, and its tooltip.
    CHECK(rows[0].short_title == L"Deluxe" && rows[1].short_title == L"Totally Twisted");
    CHECK(rows[0].modules == L"84 modules" && rows[1].modules == L"13 modules");
    CHECK(gui::installed_tile_name(rows[0]) == L"After Dark 4.0 Deluxe, 84 modules · verified against the original disc");
    CHECK(gui::installed_tile_tip(rows[1]) == L"Totally Twisted After Dark\r\n13 modules · verified against the original "
                                               L"disc\r\nClick to change its cover or remove it.");
    // The Remove window's words.
    CHECK(gui::remove_question(rows[0].title) == L"Remove After Dark 4.0 Deluxe?");
    CHECK(gui::remove_text(rows[0]) == L"Its 84 modules are deleted from this computer, and the screen saver stops showing "
                                       L"them. Its cover is kept, and you can import the release again at any time.");
  }
  {
    gui::InstalledRow one;
    one.title = L"Tom & Jerry";
    one.modules = one.detail = L"1 module";
    CHECK(gui::installed_tile_name(one) == L"Tom && Jerry, 1 module");   // a button's text is read for mnemonics
    CHECK(gui::installed_tile_tip(one) == L"Tom & Jerry\r\n1 module\r\nClick to change its cover or remove it.");
    CHECK(gui::remove_text(one) == L"Its 1 module is deleted from this computer, and the screen saver stops showing it. "
                                   L"Its cover is kept, and you can import the release again at any time.");
    one.modules = one.detail = L"";
    CHECK(gui::installed_tile_name(one) == L"Tom && Jerry");
    CHECK(gui::remove_text(one).rfind(L"Its modules are deleted from this computer, and the screen saver stops showing "
                                      L"them.",
                                      0) == 0);
    CHECK(gui::not_installed_text(L"After Dark 3.2") == L"After Dark 3.2 is not installed, so there is nothing to remove.");
    CHECK(gui::remove_failed_text("cannot remove X (a file in it is in use)") ==
          L"Nothing was removed. Cannot remove X (a file in it is in use).");
    CHECK(gui::remove_failed_text("") == L"Nothing was removed.");
    CHECK(gui::removed_note(L"The Simpsons Screen Saver") == L"Removed The Simpsons Screen Saver.");
    // A removal counts as a change: the session's exit code is 0.
    gui::Tally t;
    CHECK_EQ(t.exit_code(), 5);
    t.import_result(Status::error);
    CHECK_EQ(t.exit_code(), 1);
    t.release_removed();
    CHECK_EQ(t.exit_code(), 0);
  }
  CHECK(gui::verified_words("image", "simpsons") == L"verified against the original disks");
  CHECK(gui::verified_words("image", "startrek") == L"verified against the original disks");
  // One floppy, from its image, the KryoFlux dump's ZIP or the 7z (each by the image's md5).
  CHECK(gui::verified_words("image", "castaway") == L"verified against the original disk");
  CHECK(gui::verified_words("image", "castaway", "81087ea7cc6a304896e81c722b0a85ec") ==
        L"verified against the original disk");
  CHECK(gui::verified_words("image", "swse") == L"verified against the original disc");
  CHECK(gui::verified_words("files", "simpsons") == L"every file verified");
  CHECK(gui::verified_words("none", "deluxe") == L"not verified");
  // The releases known by the ZIP of their install files; the Looney Tunes
  // also by its CD, which the import's own md5 names.
  CHECK(gui::verified_words("image", "disney") == L"verified against the known ZIP");
  CHECK(gui::verified_words("image", "screams") == L"verified against the known ZIP");
  CHECK(gui::verified_words("image", "looney") == L"verified against the known ZIP");
  CHECK(gui::verified_words("image", "looney", "642b358a4854c481fe99984b8452ceb5") == L"verified against the known ZIP");
  CHECK(gui::verified_words("image", "looney", "6ad72e19b2cf6fcb9e67427f8e600449") ==
        L"verified against the original disc");
  CHECK(gui::verified_words("image", "swse", "ce51614a3484b9269b5ed9e61510e971") == L"verified against the original disc");
  CHECK(gui::verified_words("files", "disney", "2f38df15494728b5bc20d26c36ba84c7") == L"every file verified");
  // Marvel Comics Screen Posters by either of its two ZIPs, Snoopy's Screen
  // Savers by its one.
  CHECK(gui::verified_words("image", "marvel") == L"verified against the known ZIP");
  CHECK(gui::verified_words("image", "marvel", "6981b36abb04779a076466fabad3721c") == L"verified against the known ZIP");
  CHECK(gui::verified_words("image", "marvel", "4c608dbbeb34108b30ede88304912c94") == L"verified against the known ZIP");
  CHECK(gui::verified_words("image", "snoopy", "a712447e1c957767bdbca884cead02dc") == L"verified against the known ZIP");
  CHECK_EQ(gui::catalog_module_counts(assets / L"win").size(), size_t(2));
  {
    // The installed list words each import by the image or ZIP it came from.
    const fs::path three = dir / L"assets-three";
    fake_install(three, {"looney", "screams", "disney"},
                 {{"looney", "6ad72e19b2cf6fcb9e67427f8e600449"}, {"disney", "2f38df15494728b5bc20d26c36ba84c7"}});
    auto r3 = gui::installed_rows(three);
    CHECK_EQ(r3.size(), size_t(3));
    if (r3.size() == 3) {
      CHECK(r3[0].id == "looney" && r3[0].detail == L"12 modules · verified against the original disc");
      CHECK(r3[1].id == "screams" && r3[1].detail == L"15 modules · verified against the known ZIP");
      CHECK(r3[2].id == "disney" && r3[2].detail == L"16 modules · verified against the known ZIP");
      CHECK(r3[2].title == L"The Disney Collection Screen Saver");
    }
  }

  // The Internet Archive list: one card per release, "already downloaded" by
  // size, and the "every release not imported yet" card.
  const fs::path downloads = dir / L"downloads";
  const Package* simpsons = find_package("simpsons");
  CHECK(simpsons && !simpsons->downloads.empty());
  if (simpsons && !simpsons->downloads.empty()) {
    fs::create_directories(downloads);
    { FILE* f = _wfopen((downloads / simpsons->downloads.front().file_name).c_str(), L"wb"); if (f) fclose(f); }
    fs::resize_file(downloads / simpsons->downloads.front().file_name, simpsons->downloads.front().size);
  }
  // Star Trek: The Screen Saver's first copy is two images: only disk 1's is
  // here, so it is not downloaded yet.
  const Package* startrek = find_package("startrek");
  CHECK(startrek && !startrek->downloads.empty() && !startrek->downloads.front().more_images.empty());
  if (startrek && !startrek->downloads.empty()) {
    const Download& d = startrek->downloads.front();
    { FILE* f = _wfopen((downloads / d.file_name).c_str(), L"wb"); if (f) fclose(f); }
    fs::resize_file(downloads / d.file_name, d.size);
  }
  auto dl = gui::download_rows(assets, "", downloads);
  CHECK_EQ(dl.size(), size_t(20));
  for (const auto& r : dl) {
    CHECK(r.text.find(r.title + L"\n") == 0);
    if (r.id == "deluxe" || r.id == "tt")
      CHECK(r.text.find(L"Imported \u00b7 verified against the original disc") != std::wstring::npos);
    else CHECK(r.text.find(L"Not imported yet") != std::wstring::npos);
    CHECK((r.text.find(L"already downloaded") != std::wstring::npos) == (r.id == "simpsons"));
    if (r.id == "simpsons") CHECK(r.text.find(L"Install files (ZIP)") != std::wstring::npos);
    if (r.id == "deluxe") CHECK(r.text.find(L"CD image") != std::wstring::npos);
    if (r.id == "swse") CHECK(r.text.find(L"Star Wars Screen Entertainment\nCD image \u00b7 6.9 MB\nNot imported yet") == 0);
    if (r.id == "startrek") {
      CHECK(r.text == L"Star Trek: The Screen Saver\n2 floppy disk images \u00b7 2.8 MB\nNot imported yet");
      CHECK_EQ(r.size, uint64_t(2 * 1474560));
    }
    if (r.id == "disney") CHECK(r.text == L"The Disney Collection Screen Saver\nInstall files (ZIP) \u00b7 3.4 MB\nNot imported yet");
    if (r.id == "looney") CHECK(r.text == L"The Looney Tunes Screen Saver\nInstall files (ZIP) \u00b7 2.8 MB\nNot imported yet");
    if (r.id == "screams") CHECK(r.text == L"ScreamSavers\nInstall files (ZIP) \u00b7 3.3 MB\nNot imported yet");
    if (r.id == "tng")
      CHECK(r.text == L"Star Trek: The Next Generation Screen Saver\nCD image \u00b7 5.8 MB\nNot imported yet");
    if (r.id == "marvel") CHECK(r.text == L"Marvel Comics Screen Posters\nInstall files (ZIP) \u00b7 1.9 MB\nNot imported yet");
    if (r.id == "snoopy") CHECK(r.text == L"Snoopy's Screen Savers\nInstall files (ZIP) \u00b7 1.9 MB\nNot imported yet");
    // A ZIP of each install disk's files (The Far Side's five), and a ZIP of them all (Dilbert's first copy).
    if (r.id == "farside")
      CHECK(r.text == L"The Far Side Screen Saver Collection\nInstall files (5 ZIPs) \u00b7 5.5 MB\nNot imported yet");
    if (r.id == "dilbert")
      CHECK(r.text == L"Scott Adams' Dilbert Screen Saver Collection\nInstall files (ZIP) \u00b7 4.3 MB\nNot imported yet");
    // One floppy's image, in the KryoFlux dump's ZIP.
    if (r.id == "castaway")
      CHECK(r.text == L"Screen Antics: Johnny Castaway\nFloppy disk image \u00b7 1.3 MB\nNot imported yet");
    // A ZIP of each install disk's files (Opus 'n Bill's three), a ZIP of a
    // release's files in one folder, three ZIPs in a tar (The Flintstones':
    // the tar's size), the ZIP of three floppy images (Intermission 4.0's).
    if (r.id == "opus")
      CHECK(r.text == L"Opus 'n Bill Screen Saver\nInstall files (3 ZIPs) \u00b7 2.8 MB\nNot imported yet");
    if (r.id == "opusroad")
      CHECK(r.text == L"Opus 'n Bill: On the Road Again!\nInstall files (ZIP) \u00b7 4.4 MB\nNot imported yet");
    if (r.id == "flintstones")
      CHECK(r.text == L"The Flintstones Screen Saver Collection\nInstall files (3 ZIPs, in a tar) \u00b7 129.3 MB\n"
                      L"Not imported yet");
    if (r.id == "intermission")
      CHECK(r.text == L"Intermission 4.0\n3 floppy disk images (ZIP) \u00b7 3.3 MB\nNot imported yet");
  }
  // With disk 2's image too, the pair is downloaded.
  if (startrek && !startrek->downloads.empty() && !startrek->downloads.front().more_images.empty()) {
    const DownloadPart& q = startrek->downloads.front().more_images.front();
    { FILE* f = _wfopen((downloads / q.file_name).c_str(), L"wb"); if (f) fclose(f); }
    fs::resize_file(downloads / q.file_name, q.size);
    for (const auto& r : gui::download_rows(assets, "startrek", downloads))
      CHECK(r.text == L"Star Trek: The Screen Saver\n2 floppy disk images \u00b7 2.8 MB\nNot imported yet \u00b7 already "
                      L"downloaded");
  }
  auto all = gui::all_missing_row(dl);
  CHECK(all.has_value());
  if (all) {
    CHECK((all->ids == std::vector<std::string>{"ad10", "ad32", "simpsons", "swse", "startrek", "marvel", "snoopy",
                                                "looney", "screams", "disney", "farside", "dilbert", "tng",
                                                "castaway", "opus", "opusroad", "flintstones", "intermission"}));
    CHECK(all->text.find(L"Every release not imported yet\n18 releases") == 0);
  }
  auto one = gui::download_rows(assets, "tt", downloads);
  CHECK_EQ(one.size(), size_t(1));
  CHECK(!gui::all_missing_row(one));
  CHECK(gui::download_card_text("").find(L"Download from the Internet Archive\u2026\nFrom ") == 0);
  CHECK(gui::download_card_text("tt").find(L"\nAbout ") != std::wstring::npos);

  // Results.
  {
    ImportResult r;
    r.status = Status::ok;
    r.package_title = "The Simpsons Screen Saver";
    r.package_modules = 15;
    r.catalog_modules = 202;
    r.verified = "files";
    r.files.resize(2);
    r.files_dir = L"C:\\a\\win\\packages\\simpsons";
    r.installed = {"After Dark 4.0 Deluxe", "The Simpsons Screen Saver"};
    auto t = gui::single_result(r);
    CHECK(t.kind == Outcome::success);
    CHECK(t.heading == L"Imported The Simpsons Screen Saver: 15 modules");
    CHECK(t.body.find(L"2 files were copied to\nC:\\a\\win\\packages\\simpsons") == 0);
    CHECK(t.body.find(L"Every file matched the release of The Simpsons Screen Saver.") != std::wstring::npos);
    CHECK(t.body.find(L"Installed: After Dark 4.0 Deluxe, The Simpsons Screen Saver. 202 modules are ready to use.") !=
          std::wstring::npos);
    ImportResult bad;
    bad.status = Status::verify_failed;
    bad.message = "1 file differs";
    bad.files.push_back(ImportedFile{"FILES/AD40/X.AD", 1, "00ff", ImportedFile::Known::mismatch, ""});
    auto f = gui::single_result(bad);
    CHECK(f.kind == Outcome::failure);
    CHECK(f.heading == L"The files did not verify");
    CHECK(f.body == L"1 file differs\n\nNothing was changed.");
    CHECK(f.details.find(L"differs: FILES/AD40/X.AD (md5 00ff)") != std::wstring::npos);
    auto several = gui::several_result({"ad10", "tt"}, {r, bad});
    CHECK(several.kind == Outcome::partial);
    CHECK(several.heading == L"Imported 1 of 2 releases");
    CHECK(several.body.find(L": 15 modules (every file verified)") != std::wstring::npos);
    CHECK(several.body.find(L"Totally Twisted After Dark: The files did not verify \u2014 1 file differs") != std::wstring::npos);
    // A release known by the ZIP of its install files: its ZIP matched that
    // one; the Looney Tunes' CD is an image, as any disc's.
    ImportResult z;
    z.status = Status::ok;
    z.package_id = "disney";
    z.package_title = "The Disney Collection Screen Saver";
    z.package_modules = 16;
    z.catalog_modules = 16;
    z.verified = "image";
    z.iso_md5 = "2f38df15494728b5bc20d26c36ba84c7";
    z.format = "zip";
    z.files.resize(31);
    z.installed = {"The Disney Collection Screen Saver"};
    CHECK(gui::single_result(z).body.find(L"The ZIP matched the known ZIP of The Disney Collection Screen Saver.") !=
          std::wstring::npos);
    ImportResult cd = z;
    cd.package_id = "looney";
    cd.package_title = "The Looney Tunes Screen Saver";
    cd.iso_md5 = "6ad72e19b2cf6fcb9e67427f8e600449";
    cd.format = "iso9660";
    CHECK(gui::single_result(cd).body.find(L"The image matched the known image of The Looney Tunes Screen Saver.") !=
          std::wstring::npos);
    auto both = gui::several_result({"disney", "looney"}, {z, cd});
    CHECK(both.body.find(L"The Disney Collection Screen Saver: 16 modules (verified against the known ZIP)") !=
          std::wstring::npos);
    CHECK(both.body.find(L"The Looney Tunes Screen Saver: 16 modules (verified against the original disc)") !=
          std::wstring::npos);
    auto none_ok = gui::several_result({"ad10", "tt"}, {bad});
    CHECK(none_ok.kind == Outcome::failure);
    CHECK(none_ok.body.find(L"not started (cancelled)") != std::wstring::npos);
    CHECK(none_ok.body.find(L"Nothing was changed.") != std::wstring::npos);
  }

  // The cover window's origin line.
  {
    CoverInfo c;
    CHECK(gui::origin_line(c) == L"No picture yet");
    c.origin = CoverOrigin::user;
    CHECK(gui::origin_line(c) == L"Your own picture");
    c.origin = CoverOrigin::download;
    c.label = "Box front";
    c.credit = "Wikisimpsons";
    CHECK(gui::origin_line(c) == L"Box front \u00b7 Wikisimpsons");
    c.origin = CoverOrigin::disc;
    c.label = "Installer art";
    c.credit = "your disc";
    CHECK(gui::origin_line(c) == L"Installer art from your disc");
    // What "Use the original cover" and "Download the original cover" would bring.
    CHECK(gui::original_note(c).empty());   // no picture of yours
    c.has_user = true;
    c.original = CoverOrigin::download;
    c.original_label = "Disc label";
    c.original_credit = "Internet Archive";
    CHECK(gui::original_note(c) == L"Original: Disc label \u00b7 Internet Archive");
    c.original = CoverOrigin::generated;
    CHECK(gui::original_note(c) == L"No original picture yet: a generated cover");
    CHECK(gui::download_note(c, true).empty());   // nothing better to download
    c.can_download = true;
    c.download_label = "Box front";
    c.download_credit = "Wayback Machine";
    CHECK(gui::download_note(c, false).empty());   // downloads are off
    CHECK(gui::download_note(c, true) == L"Gets the Box front \u00b7 Wayback Machine; your picture stays the cover");
    c.has_user = false;
    CHECK(gui::download_note(c, true) == L"Gets the Box front \u00b7 Wayback Machine");
  }

  // "Get the covers": what refresh_covers did, per release.
  {
    auto res = [](const char* id, Status st, bool changed) {
      CoverResult r;
      r.info.package = id;
      r.info.origin = CoverOrigin::download;
      r.info.label = "Box front";
      r.info.credit = "Wayback Machine";
      r.status = st;
      r.changed = changed;
      if (st != Status::ok) r.message = "could not connect";
      return r;
    };
    auto all_got = gui::covers_result({res("deluxe", Status::ok, true), res("tt", Status::ok, true)});
    CHECK(all_got.kind == Outcome::success);
    CHECK(all_got.heading == L"Got 2 covers");
    CHECK(all_got.body.find(L"After Dark 4.0 Deluxe: Box front \u00b7 Wayback Machine") == 0);
    auto some = gui::covers_result({res("deluxe", Status::ok, true), res("ad10", Status::network, false)});
    CHECK(some.kind == Outcome::partial);
    CHECK(some.heading == L"Got 1 of 2 covers");
    CHECK(some.body.find(L"After Dark 10th Anniversary: the download failed") != std::wstring::npos);
    CHECK(some.body.find(L"Try again when this computer is online.") != std::wstring::npos);
    auto none_got = gui::covers_result({res("ad10", Status::network, false)});
    CHECK(none_got.kind == Outcome::failure);
    CHECK(none_got.heading == L"The cover downloads failed");
    auto nothing = gui::covers_result({res("tt", Status::ok, false)});
    CHECK(nothing.kind == Outcome::success);
    CHECK(nothing.heading == L"The covers are already the best available");
    CHECK(gui::covers_result({res("tt", Status::ok, true)}).heading == L"Got the cover art");
  }
}

// ---- import.gui_shots -------------------------------------------------------------------

// true when set_cover is still C's C-M1 stub.
bool covers_are_stubs(const std::wstring& exe, const fs::path& dir) {
  const fs::path assets = dir / L"probe-assets";
  fake_install(assets, {"deluxe"});
  const fs::path pic = dir / L"probe.png";
  synthetic_picture(pic, 64, 80, 20);
  test::ProcessResult r = test::run_process(exe, {L"--set-cover", L"deluxe", pic.wstring(), L"--dest", assets.wstring()}, 60000);
  return r.output.find("not implemented") != std::string::npos;
}

void test_shots(const std::wstring& exe, const fs::path& dir) {
  const fs::path assets = dir / L"assets";
  // Every release of the registry imported (ten and more): the Sources page's
  // list of them must fit, or scroll inside its card.
  std::vector<std::string> all_ids;
  for (const Package& p : builtin_packages()) all_ids.push_back(p.id);
  CHECK(all_ids.size() >= 10);
  fake_install(assets, all_ids);
  // Synthetic covers for two releases when C's covers are in (the others stay generated).
  if (!covers_are_stubs(exe, dir)) {
    const fs::path a = dir / L"cover-a.png", b = dir / L"cover-b.png";
    CHECK(synthetic_picture(a, 320, 400, 30));
    CHECK(synthetic_picture(b, 500, 400, 120));
    test::run_process(exe, {L"--set-cover", L"deluxe", a.wstring(), L"--dest", assets.wstring(), L"--quiet"}, 60000);
    test::run_process(exe, {L"--set-cover", L"simpsons", b.wstring(), L"--dest", assets.wstring(), L"--quiet"}, 60000);
    fake_install(assets, all_ids);   // the module counts again, over the catalog set_cover wrote
  }
  const fs::path out = dir / L"png";
  fs::create_directories(out);
  struct Shot {
    std::wstring page, extra;
  };
  const std::vector<Shot> pages = {{L"sources", L""},  {L"downloads", L""}, {L"progress", L"progress=0.4"},
                                   {L"result", L""},   {L"error", L""},     {L"cover", L"package=simpsons"},
                                   {L"remove", L"package=simpsons"}};
  size_t n = 0;
  for (const Shot& shot : pages) {
    for (const wchar_t* theme : {L"light", L"dark", L"hc"}) {
      for (int dpi : {96, 144, 192}) {
        const std::wstring name = shot.page + L"_" + theme + L"_" + std::to_wstring(dpi * 100 / 96);
        const fs::path png = out / (name + L".png"), report = out / (name + L".txt");
        std::wstring state = L"page=" + shot.page + L";theme=" + theme + L";dpi=" + std::to_wstring(dpi) +
                             L";report=" + report.wstring();
        if (!shot.extra.empty()) state += L";" + shot.extra;
        SetEnvironmentVariableW(L"AD_IMPORT_TEST_SCREENSHOT", png.wstring().c_str());
        SetEnvironmentVariableW(L"AD_IMPORT_TEST_SCREENSHOT_STATE", state.c_str());
        test::ProcessResult r = test::run_process(exe, {L"--gui", L"--dest", assets.wstring()}, 60000);
        if (r.exit_code != 0) fprintf(stderr, "[%s] exit %d\n%s", to_utf8(name).c_str(), r.exit_code, r.output.c_str());
        CHECK_EQ(r.exit_code, 0);
        CHECK(fs::exists(png));
        auto kv = read_report(report);
        adw::ui::Image img;
        std::string err;
        CHECK(adw::ui::load_image(png.wstring(), img, &err));
        int px = -1, py = -1;
        sscanf(kv["probe"].c_str(), "%d,%d", &px, &py);
        CHECK(px >= 0 && py >= 0 && px < img.w && py < img.h);
        if (px >= 0 && py >= 0 && px < img.w && py < img.h) {
          const uint8_t* p = &img.pbgra[((size_t)py * img.w + px) * 4];
          char got[8];
          snprintf(got, sizeof(got), "%02X%02X%02X", p[2], p[1], p[0]);
          if (kv["base"] != got)
            fprintf(stderr, "%s: base pixel %s, want %s\n", to_utf8(name).c_str(), got, kv["base"].c_str());
          CHECK(kv["base"] == got);
        }
        CHECK(kv["dpi"] == std::to_string(dpi));
        if (shot.page == L"downloads") {
          // A card per release, every one imported: nine show whole at the
          // most (eight when the note under them runs to a third line, as the
          // scratch folder's long path may make it), and the list scrolls.
          const DownloadsList l = downloads_list(kv);
          CHECK(l.ends.size() == all_ids.size() && l.shown < l.whole);
          CHECK(l.cards == 8 || l.cards == 9);
        }
        n++;
      }
    }
  }
  // A few more states, at 100% and 150%.
  const std::vector<Shot> extras = {{L"sources", L"focus=101"},        {L"sources", L"caution=1"},
                                    {L"progress", L"progress=marquee;phase=cover"}, {L"result", L"result=partial"},
                                    {L"result", L"result=several"},    {L"cover", L"status=error;package=deluxe"},
                                    {L"cover", L"status=running;package=simpsons"}, {L"downloads", L"focus=200"},
                                    {L"sources", L"workarea=640x520"},  {L"sources", L"focus=300"},
                                    {L"sources", L"notice=1"},          {L"remove", L"status=error;package=deluxe"},
                                    {L"remove", L"status=running;package=startrek"}, {L"remove", L"focus=701"}};
  for (const Shot& shot : extras) {
    for (const wchar_t* theme : {L"light", L"dark"}) {
      std::wstring tag = shot.extra;
      for (wchar_t& c : tag)
        if (c == L'=' || c == L';' || c == L'.') c = L'-';
      const std::wstring name = shot.page + L"_" + theme + L"_" + tag;
      const fs::path png = out / (name + L".png");
      SetEnvironmentVariableW(L"AD_IMPORT_TEST_SCREENSHOT", png.wstring().c_str());
      SetEnvironmentVariableW(L"AD_IMPORT_TEST_SCREENSHOT_STATE",
                              (L"page=" + shot.page + L";theme=" + theme + L";dpi=144;" + shot.extra).c_str());
      test::ProcessResult r = test::run_process(exe, {L"--gui", L"--dest", assets.wstring()}, 60000);
      CHECK_EQ(r.exit_code, 0);
      CHECK(fs::exists(png));
      n++;
    }
  }
  // The Sources page on common screens, every release installed: their
  // covers are a grid, seven to a row in the 816-DIP window (twenty on three
  // rows), which shows whole at 150% on a 2560x1440 monitor (a 2560x1392 DIP
  // work area) and on a 1080-line screen at 100%. A work area too narrow and
  // short for it (640x900 at 100%: five to a row, four rows) shows as many
  // whole rows as fit (two), and the grid scrolls in its card. On a very
  // short one (640x520) the grid keeps its fallback: fewer than two rows, or
  // its full height with the whole body scrolling. With cover downloads off
  // there is no "Get the covers" line (most of these releases have no
  // picture), so the page is the one of a root whose covers are all there.
  struct Rows {
    std::wstring state;
    int at_least;   // whole rows that must show (-1: every one; 0: the fallback)
    int cols;       // covers to a row
  };
  const std::vector<Rows> rows = {{L"workarea=2560x1392;dpi=144", -1, 7},
                                  {L"workarea=1920x1032;dpi=96", -1, 7},
                                  {L"workarea=640x900;dpi=96", 2, 5},
                                  {L"workarea=640x520;dpi=144", 0, 5}};
  std::map<std::wstring, int> sources_h;   // the page's client height at each of them
  for (const Rows& t : rows) {
    std::wstring tag = t.state;
    for (wchar_t& c : tag)
      if (c == L'=' || c == L';') c = L'-';
    const fs::path png = out / (L"sources_rows_" + tag + L".png"), report = out / (L"sources_rows_" + tag + L".txt");
    SetEnvironmentVariableW(L"AD_IMPORT_TEST_SCREENSHOT", png.wstring().c_str());
    SetEnvironmentVariableW(L"AD_IMPORT_TEST_SCREENSHOT_STATE",
                            (L"page=sources;theme=light;" + t.state + L";report=" + report.wstring()).c_str());
    test::ProcessResult r =
        test::run_process(exe, {L"--gui", L"--dest", assets.wstring(), L"--no-cover-download"}, 60000);
    CHECK_EQ(r.exit_code, 0);
    auto kv = read_report(report);
    sources_h[t.state] = client_height(kv);
    int shown = -1, whole = -1, row = -1, grid_rows = -1, cols = -1;
    sscanf(kv["list"].c_str(), "%d,%d,%d,%d,%d", &shown, &whole, &row, &grid_rows, &cols);
    fprintf(stderr, "sources covers at %s: grid %d of %d px, %d rows of %d px, %d to a row\n", to_utf8(t.state).c_str(),
            shown, whole, grid_rows, row, cols);
    CHECK(cols == t.cols && grid_rows == (int(all_ids.size()) + t.cols - 1) / t.cols);
    // The grid's margins: 8 DIP above and below it, less the gap a row's height counts.
    const int frame = whole - grid_rows * row;
    CHECK(row > 0 && frame > 0 && frame < row);
    if (row <= 0) continue;
    if (t.at_least < 0) {
      CHECK(shown == whole);
    } else if (t.at_least) {
      CHECK(shown >= t.at_least * row + frame);
      CHECK(shown < whole && (shown - frame) % row == 0);
    } else {
      CHECK(shown < 2 * row + frame || shown == whole);
    }
    n++;
  }
  // The Downloads page at the same work areas (a card per release, every one
  // imported), with a short downloads folder so the note under the list keeps
  // to two lines: the list shows nine whole cards, its most, and scrolls for
  // the rest, and the page is no taller than Sources there, so the window
  // keeps its height from one page to the other (unlimited at 200% too). On
  // a short work area it shows as many whole cards as fit, two at the least;
  // on a very short one (640x340 at 150%) what fits, never less than 80 DIP.
  struct Cards {
    std::wstring state;
    int fewest, most;   // whole cards that show (0, 0: fewer than two fit)
    int dpi;
  };
  const std::vector<Cards> cards = {{L"workarea=2560x1392;dpi=144", 9, 9, 144},
                                    {L"workarea=1920x1032;dpi=96", 9, 9, 96},
                                    {L"dpi=192", 9, 9, 192},
                                    {L"workarea=640x900;dpi=96", 2, 8, 96},
                                    {L"workarea=640x520;dpi=144", 2, 8, 144},
                                    {L"workarea=640x340;dpi=144", 0, 0, 144}};
  for (const Cards& t : cards) {
    std::wstring tag = t.state;
    for (wchar_t& c : tag)
      if (c == L'=' || c == L';') c = L'-';
    const fs::path png = out / (L"downloads_cards_" + tag + L".png"),
                   report = out / (L"downloads_cards_" + tag + L".txt");
    SetEnvironmentVariableW(L"AD_IMPORT_TEST_SCREENSHOT", png.wstring().c_str());
    SetEnvironmentVariableW(L"AD_IMPORT_TEST_SCREENSHOT_STATE",
                            (L"page=downloads;theme=light;" + t.state + L";report=" + report.wstring()).c_str());
    // "dl": a folder name short enough for two lines (nothing is read there but whether a download is).
    test::ProcessResult r =
        test::run_process(exe, {L"--gui", L"--dest", assets.wstring(), L"--download-dir", L"dl"}, 60000);
    CHECK_EQ(r.exit_code, 0);
    auto kv = read_report(report);
    const DownloadsList l = downloads_list(kv);
    const int h = client_height(kv);
    const int beside = sources_h.count(t.state) ? sources_h[t.state] : -1;
    fprintf(stderr, "downloads cards at %s: list %d of %d px, %d cards whole of %zu, page %d px (sources %d)\n",
            to_utf8(t.state).c_str(), l.shown, l.whole, l.cards, l.ends.size(), h, beside);
    CHECK(l.ends.size() == all_ids.size() && l.shown < l.whole);
    if (t.most) {
      CHECK(l.cards >= t.fewest && l.cards <= t.most);
    } else {
      CHECK(l.cards < 2 && l.ends.size() >= 2 && l.shown < l.ends[1] && l.shown >= 80 * t.dpi / 96);
    }
    if (t.fewest == 9 && beside > 0) CHECK(h > 0 && h <= beside);
    n++;
  }
  // Live changes: the theme switched after opening, and a move to a monitor of
  // another scale (the page is laid out again at the new DPI).
  struct Live {
    std::wstring name, state;
    std::string base, dpi;
  };
  const std::vector<Live> live = {
      {L"live_theme_to_dark", L"page=sources;theme=light;dpi=96;themechange=dark", "202020", "96"},
      {L"live_theme_to_light", L"page=downloads;theme=dark;dpi=144;themechange=light", "F3F3F3", "144"},
      {L"live_dpi_to_150", L"page=cover;theme=light;dpichange=144;package=deluxe", "F3F3F3", "144"},
      {L"live_dpi_to_200", L"page=progress;theme=dark;dpichange=192", "202020", "192"}};
  for (const Live& l : live) {
    const fs::path png = out / (l.name + L".png"), report = out / (l.name + L".txt");
    SetEnvironmentVariableW(L"AD_IMPORT_TEST_SCREENSHOT", png.wstring().c_str());
    SetEnvironmentVariableW(L"AD_IMPORT_TEST_SCREENSHOT_STATE", (l.state + L";report=" + report.wstring()).c_str());
    test::ProcessResult r = test::run_process(exe, {L"--gui", L"--dest", assets.wstring()}, 60000);
    CHECK_EQ(r.exit_code, 0);
    auto kv = read_report(report);
    CHECK_EQ(kv["base"], l.base);
    CHECK_EQ(kv["dpi"], l.dpi);
    adw::ui::Image img;
    std::string err;
    CHECK(adw::ui::load_image(png.wstring(), img, &err));
    int px = -1, py = -1;
    sscanf(kv["probe"].c_str(), "%d,%d", &px, &py);
    const bool inside = px >= 0 && py >= 0 && px < img.w && py < img.h;
    CHECK(inside);
    if (inside) {
      const uint8_t* p = &img.pbgra[((size_t)py * img.w + px) * 4];
      char got[8];
      snprintf(got, sizeof(got), "%02X%02X%02X", p[2], p[1], p[0]);
      CHECK_EQ(std::string(got), l.base);
    }
    n++;
  }
  SetEnvironmentVariableW(L"AD_IMPORT_TEST_SCREENSHOT", nullptr);
  SetEnvironmentVariableW(L"AD_IMPORT_TEST_SCREENSHOT_STATE", nullptr);
  fprintf(stderr, "%zu screenshots in %s\n", n, to_utf8(out.wstring()).c_str());
}

// ---- import.gui_flow --------------------------------------------------------------------

// adimport with real windows: finds its "Long After Dark" window, presses
// buttons with TDM_CLICK_BUTTON, reads controls, waits for it to exit.
class GuiProcess {
 public:
  bool start(const std::wstring& exe, const std::vector<std::wstring>& args) {
    std::wstring cmd = test::command_line(exe, args);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    return CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi_) != 0;
  }
  ~GuiProcess() {
    if (pi_.hProcess) {
      if (WaitForSingleObject(pi_.hProcess, 0) == WAIT_TIMEOUT) TerminateProcess(pi_.hProcess, DWORD(-2));
      CloseHandle(pi_.hProcess);
      CloseHandle(pi_.hThread);
    }
  }
  // The visible window titled "Long After Dark" other than `not_this`.
  HWND window(HWND not_this = nullptr, DWORD timeout_ms = 20000) {
    struct Find {
      DWORD pid;
      HWND skip, hwnd = nullptr;
    } find{pi_.dwProcessId, not_this};
    const ULONGLONG until = GetTickCount64() + timeout_ms;
    while (!find.hwnd && GetTickCount64() < until) {
      if (WaitForSingleObject(pi_.hProcess, 50) == WAIT_OBJECT_0) return nullptr;
      EnumWindows(
          [](HWND h, LPARAM lp) -> BOOL {
            auto* f = reinterpret_cast<Find*>(lp);
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            wchar_t title[64] = {};
            GetWindowTextW(h, title, 64);
            if (pid == f->pid && h != f->skip && IsWindowVisible(h) && wcscmp(title, L"Long After Dark") == 0) {
              f->hwnd = h;
              return FALSE;
            }
            return TRUE;
          },
          reinterpret_cast<LPARAM>(&find));
    }
    if (find.hwnd) Sleep(200);   // let it finish drawing
    return find.hwnd;
  }
  // A control of the window by id, looking inside its scrolling lists too.
  static HWND control(HWND top, int id) {
    struct Find {
      int id;
      HWND hwnd = nullptr;
    } find{id};
    EnumChildWindows(
        top,
        [](HWND h, LPARAM lp) -> BOOL {
          auto* f = reinterpret_cast<Find*>(lp);
          if (GetDlgCtrlID(h) == f->id) {
            f->hwnd = h;
            return FALSE;
          }
          return TRUE;
        },
        reinterpret_cast<LPARAM>(&find));
    return find.hwnd;
  }
  // The first such window (other than `not_this`) that has a control `id`,
  // waiting past the pages that come and go before it.
  HWND window_with(HWND not_this, int id, DWORD timeout_ms = 20000) {
    const ULONGLONG until = GetTickCount64() + timeout_ms;
    HWND skip = not_this;
    while (GetTickCount64() < until) {
      HWND w = window(skip, 1000);
      if (!w) {
        if (WaitForSingleObject(pi_.hProcess, 0) == WAIT_OBJECT_0) return nullptr;
        continue;
      }
      if (control(w, id)) return w;
      skip = w;
      gone(w, (DWORD)std::max<LONGLONG>(0, (LONGLONG)(until - GetTickCount64())));
    }
    return nullptr;
  }
  static void click(HWND h, int id) { SendMessageW(h, WM_USER + 102 /* TDM_CLICK_BUTTON */, WPARAM(id), 0); }
  // The control with the keyboard focus in `top`'s thread.
  static HWND focus(HWND top) {
    GUITHREADINFO gti{sizeof(gti)};
    return GetGUIThreadInfo(GetWindowThreadProcessId(top, nullptr), &gti) ? gti.hwndFocus : nullptr;
  }
  // A key pressed on the focused control (posted, so the page's dialog keys
  // see it); returns the control focused next, once the focus has moved.
  static HWND press(HWND top, WPARAM vk, DWORD timeout_ms = 3000) {
    HWND f = focus(top);
    PostMessageW(f, WM_KEYDOWN, vk, 1);
    PostMessageW(f, WM_KEYUP, vk, LPARAM(0xC0000001));
    const ULONGLONG until = GetTickCount64() + timeout_ms;
    while (focus(top) == f && GetTickCount64() < until) Sleep(10);
    return focus(top);
  }
  // Waits until `cond` holds (or the time is up); returns whether it does.
  template <typename F>
  static bool settle(F cond, DWORD timeout_ms = 2000) {
    const ULONGLONG until = GetTickCount64() + timeout_ms;
    while (!cond() && GetTickCount64() < until) Sleep(10);
    return cond();
  }
  static bool gone(HWND h, DWORD timeout_ms = 5000) {
    const ULONGLONG until = GetTickCount64() + timeout_ms;
    while (IsWindow(h) && GetTickCount64() < until) Sleep(50);
    return !IsWindow(h);
  }
  int wait(DWORD timeout_ms = 20000) {
    if (WaitForSingleObject(pi_.hProcess, timeout_ms) == WAIT_TIMEOUT) TerminateProcess(pi_.hProcess, DWORD(-2));
    DWORD code = 0;
    GetExitCodeProcess(pi_.hProcess, &code);
    return int(code);
  }

 private:
  PROCESS_INFORMATION pi_{};
};

// What screen readers call `h` (MSAA, which UIA's proxy reads too).
std::wstring accessible_name(HWND h) {
  IAccessible* acc = nullptr;
  if (FAILED(AccessibleObjectFromWindow(h, (DWORD)OBJID_CLIENT, IID_IAccessible, reinterpret_cast<void**>(&acc))) || !acc)
    return L"";
  VARIANT self;
  VariantInit(&self);
  self.vt = VT_I4;
  self.lVal = CHILDID_SELF;
  BSTR name = nullptr;
  std::wstring out;
  if (SUCCEEDED(acc->get_accName(self, &name)) && name) out = name;
  if (name) SysFreeString(name);
  acc->Release();
  return out;
}

// The file and folder dialogs' own code (pages.cc), with no window: the
// options each dialog gets are accepted by the shell's IFileOpenDialog as
// set, and a result array of real files and a folder comes back as their
// paths, in order.
void test_dialog_code(const fs::path& dir) {
  const fs::path a = dir / L"dialogs" / L"disk1.img", b = dir / L"dialogs" / L"disk 2.img",
                 folder = dir / L"dialogs" / L"cd copy";
  test::write_bytes(a, test::pattern(100, 1));
  test::write_bytes(b, test::pattern(100, 2));
  fs::create_directories(folder);
  const DWORD files = gui::dialog_options(0, false, true), one = gui::dialog_options(0, false, false),
              dirs = gui::dialog_options(0, true, false);
  CHECK((files & (FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_ALLOWMULTISELECT)) ==
        (FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_ALLOWMULTISELECT));
  CHECK(!(files & FOS_PICKFOLDERS));
  CHECK(!(one & FOS_ALLOWMULTISELECT) && (one & FOS_FILEMUSTEXIST));
  CHECK((dirs & (FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM)) == (FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM));
  CHECK(!(dirs & FOS_ALLOWMULTISELECT));
  for (DWORD want : {files, one, dirs}) {
    IFileOpenDialog* d = nullptr;
    CHECK(SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog, (void**)&d)));
    if (!d) continue;
    DWORD base = 0, got = 0;
    d->GetOptions(&base);
    CHECK(SUCCEEDED(d->SetOptions(gui::dialog_options(base, want & FOS_PICKFOLDERS, want & FOS_ALLOWMULTISELECT))));
    d->GetOptions(&got);
    CHECK((got & want) == want);
    d->Release();
  }
  std::vector<PIDLIST_ABSOLUTE> ids;
  for (const fs::path& p : {a, b, folder}) {
    PIDLIST_ABSOLUTE id = nullptr;
    CHECK(SUCCEEDED(SHParseDisplayName(fs::path(p).make_preferred().wstring().c_str(), nullptr, &id, 0, nullptr)));
    if (id) ids.push_back(id);
  }
  IShellItemArray* items = nullptr;
  std::vector<PCIDLIST_ABSOLUTE> cids(ids.begin(), ids.end());
  CHECK(SUCCEEDED(SHCreateShellItemArrayFromIDLists(UINT(cids.size()), cids.data(), &items)));
  std::vector<fs::path> got = gui::dialog_paths(items);
  CHECK_EQ(got.size(), size_t(3));
  for (size_t i = 0; i < got.size() && i < 3; i++) {
    std::error_code ec;
    CHECK(fs::equivalent(got[i], std::vector<fs::path>{a, b, folder}[i], ec));
  }
  if (items) items->Release();
  for (PIDLIST_ABSOLUTE id : ids) CoTaskMemFree(id);
  CHECK(gui::dialog_paths(nullptr).empty());
}

int test_flow(const std::wstring& exe, const fs::path& dir) {
  if (const char* skip = getenv("AD_IMPORT_SKIP_GUI_TESTS"); skip && *skip == '1') {
    fprintf(stderr, "(skipped: AD_IMPORT_SKIP_GUI_TESTS=1)\n");
    return 77;
  }
  SetEnvironmentVariableW(L"AD_GUI_AUTOCLOSE", nullptr);
  SetEnvironmentVariableW(L"AD_IMPORT_TEST_SCREENSHOT", nullptr);
  // Sources -> "Download from the Internet Archive" -> Back -> Cancel: exit 5,
  // each page a new window, and nothing written.
  {
    const fs::path assets = dir / L"assets-none";
    GuiProcess p;
    CHECK(p.start(exe, {L"--gui", L"--dest", assets.wstring()}));
    HWND sources = p.window();
    CHECK(sources != nullptr);
    if (sources) {
      CHECK(GuiProcess::control(sources, 101) && GuiProcess::control(sources, 102) && GuiProcess::control(sources, 103));
      // A live theme change (as Windows sends it) keeps the window working.
      SendMessageW(sources, WM_THEMECHANGED, 0, 0);
      SendMessageW(sources, WM_SYSCOLORCHANGE, 0, 0);
      CHECK(IsWindow(sources) && IsWindowVisible(sources));
      GuiProcess::click(sources, 103);
      CHECK(GuiProcess::gone(sources));
      HWND list = p.window(sources);
      CHECK(list != nullptr);
      if (list) {
        CHECK(GuiProcess::control(list, 200) != nullptr);
        CHECK(GuiProcess::control(list, 299) != nullptr);   // nothing imported: "every release"
        // Twenty-one cards: nine show at the most, and the list scrolls. A
        // wheel notch over a card scrolls it, and each card the Down arrow
        // focuses (from the first, focused as the page opens, to "every
        // release", the last) is scrolled into view, as Tab's are.
        HWND panel = FindWindowExW(list, nullptr, nullptr, L"Releases"), first = GuiProcess::control(list, 200);
        CHECK(panel != nullptr && first != nullptr);
        if (panel && first) {
          RECT pr{}, before{};
          GetWindowRect(panel, &pr);
          GetWindowRect(first, &before);
          PostMessageW(first, WM_MOUSEWHEEL, MAKEWPARAM(0, WORD(-WHEEL_DELTA)),
                       MAKELPARAM(before.left + 8, before.top + 8));
          auto first_top = [&] {
            RECT r{};
            GetWindowRect(first, &r);
            return r.top;
          };
          CHECK(GuiProcess::settle([&] { return first_top() < before.top; }));
          auto in_view = [&](HWND h) {
            RECT r{};
            GetWindowRect(h, &r);
            return r.top >= pr.top && r.bottom <= pr.bottom;
          };
          CHECK(GuiProcess::focus(list) == first);
          HWND f = first;
          for (int i = 0; i < 30 && GetDlgCtrlID(f) != 299; ++i) {
            HWND next = GuiProcess::press(list, VK_DOWN);
            if (next == f) break;
            f = next;
            CHECK(IsChild(panel, f) && GuiProcess::settle([&] { return in_view(f); }));
          }
          CHECK_EQ(GetDlgCtrlID(f), 299);
          CHECK(first_top() < pr.top);   // the first card scrolled out at the top
        }
        GuiProcess::click(list, IDCANCEL);
        CHECK(GuiProcess::gone(list));
        HWND back = p.window(list);
        CHECK(back != nullptr);
        if (back) GuiProcess::click(back, IDCANCEL);
      }
    }
    CHECK_EQ(p.wait(), 5);
    CHECK(!fs::exists(assets));
  }

  // With no --dest the windows take their folders from the data folder
  // (<base>\LongAfterDark): Sources -> Downloads -> Back -> Cancel creates
  // nothing there.
  {
    const fs::path base = dir / L"lad-default";
    const std::wstring saved_lad = test::get_env(L"AD_LOCALAPPDATA"), saved_assets = test::get_env(L"AD_ASSETS_DIR");
    test::set_env(L"AD_LOCALAPPDATA", base.wstring());
    test::set_env(L"AD_ASSETS_DIR", L"");
    GuiProcess p;
    CHECK(p.start(exe, {L"--gui"}));
    HWND sources = p.window();
    CHECK(sources != nullptr);
    if (sources) {
      GuiProcess::click(sources, 103);
      CHECK(GuiProcess::gone(sources));
      HWND list = p.window(sources);
      CHECK(list != nullptr);
      if (list) {
        GuiProcess::click(list, IDCANCEL);
        CHECK(GuiProcess::gone(list));
        HWND back = p.window(list);
        CHECK(back != nullptr);
        if (back) GuiProcess::click(back, IDCANCEL);
      }
    }
    CHECK_EQ(p.wait(), 5);
    CHECK(!fs::exists(base / L"LongAfterDark"));
    test::set_env(L"AD_LOCALAPPDATA", saved_lad);
    test::set_env(L"AD_ASSETS_DIR", saved_assets);
  }

  // "Change cover" on a scratch install with a synthetic picture.
  if (covers_are_stubs(exe, dir)) {
    fprintf(stderr, "(the cover part is skipped: set_cover is still a stub)\n");
    return test::g_failures ? 1 : 77;
  }
  const fs::path assets = dir / L"assets-cover";
  {
    test::IsoBuilder b;
    auto fixture = test::build_fixture(b);
    const fs::path iso = dir / L"synthetic.iso";
    test::write_bytes(iso, b.build());
    test::ProcessResult r = test::run_process(
        exe, {L"--iso", iso.wstring(), L"--dest", assets.wstring(), L"--no-verify", L"--no-cover-download", L"--quiet"}, 120000);
    CHECK_EQ(r.exit_code, 0);
  }
  // Sources over that install, whose cover is still generated: its cover
  // (a button that opens its menu) names the release and what it holds for
  // screen readers; its menu's "Change cover…" (400) opens the cover window,
  // and Done there comes back to Sources. "Get the covers" (104) runs
  // refresh_covers in a progress window, shows what it got and comes back to
  // Sources. Downloads are off in the environment, so nothing is fetched and
  // nothing changes (exit 5). Then `--gui --refresh-covers` alone.
  SetEnvironmentVariableW(L"AD_COVER_DOWNLOAD", L"0");
  {
    GuiProcess p;
    CHECK(p.start(exe, {L"--gui", L"--dest", assets.wstring()}));
    HWND src = p.window();
    CHECK(src != nullptr);
    if (src) {
      HWND tile = GuiProcess::control(src, 300);   // deluxe: registry index 0
      CHECK(tile != nullptr);
      if (tile) {
        const std::wstring name = accessible_name(tile);
        if (name.rfind(L"After Dark 4.0 Deluxe, ", 0) != 0) fprintf(stderr, "cover's name: %s\n", to_utf8(name).c_str());
        CHECK(name.rfind(L"After Dark 4.0 Deluxe, ", 0) == 0 && name.find(L"module") != std::wstring::npos);
      }
      CHECK(GuiProcess::control(src, 400) == nullptr);   // a menu item, not a control
      GuiProcess::click(src, 400);
      CHECK(GuiProcess::gone(src));
      HWND cover = p.window_with(src, 601);
      CHECK(cover != nullptr);
      if (cover) {
        GuiProcess::click(cover, IDOK);
        CHECK(GuiProcess::gone(cover));
        src = p.window_with(cover, 101);
      }
      CHECK(src != nullptr);
    }
    if (src) {
      CHECK(GuiProcess::control(src, 104) != nullptr);
      GuiProcess::click(src, 104);
      CHECK(GuiProcess::gone(src, 10000));
      HWND result = p.window_with(src, IDOK, 30000);   // after the progress window
      CHECK(result != nullptr);
      if (result) {
        GuiProcess::click(result, IDOK);
        CHECK(GuiProcess::gone(result));
        HWND again = p.window(result);
        CHECK(again != nullptr && GuiProcess::control(again, 101) != nullptr);
        if (again) GuiProcess::click(again, IDCANCEL);
      }
    }
    CHECK_EQ(p.wait(), 5);
  }
  {
    GuiProcess p;
    CHECK(p.start(exe, {L"--gui", L"--refresh-covers", L"--dest", assets.wstring()}));
    HWND result = p.window_with(nullptr, IDOK, 30000);
    CHECK(result != nullptr);
    if (result) GuiProcess::click(result, IDOK);
    CHECK_EQ(p.wait(), 5);
  }
  SetEnvironmentVariableW(L"AD_COVER_DOWNLOAD", nullptr);

  const fs::path tile = assets / L"win" / L"covers" / L"deluxe" / L"tile.png";
  const std::string before = fs::exists(tile) ? md5_hex(test::read_text(tile).data(), test::read_text(tile).size()) : "";
  const fs::path pic = dir / L"my cover.png";
  CHECK(synthetic_picture(pic, 480, 600, 60));
  SetEnvironmentVariableW(L"AD_IMPORT_TEST_PICK", pic.wstring().c_str());
  {
    GuiProcess p;
    CHECK(p.start(exe, {L"--gui", L"--change-cover", L"deluxe", L"--dest", assets.wstring()}));
    HWND w = p.window();
    CHECK(w != nullptr);
    if (w) {
      HWND done = GuiProcess::control(w, IDOK);
      CHECK(done != nullptr);
      GuiProcess::click(w, 601);   // Choose a picture… (AD_IMPORT_TEST_PICK answers the dialog)
      // Done greys while set_cover runs and comes back when it has finished.
      const ULONGLONG until = GetTickCount64() + 30000;
      bool ran = false;
      while (GetTickCount64() < until) {
        const bool enabled = done && IsWindowEnabled(done);
        ran = ran || !enabled;
        if (enabled && (ran || fs::exists(tile))) break;
        Sleep(50);
      }
      Sleep(100);
      GuiProcess::click(w, IDOK);
    }
    CHECK_EQ(p.wait(), 0);
  }
  SetEnvironmentVariableW(L"AD_IMPORT_TEST_PICK", nullptr);
  CHECK(fs::exists(tile));
  if (fs::exists(tile)) {
    const std::string after = md5_hex(test::read_text(tile).data(), test::read_text(tile).size());
    CHECK(after != before);
  }
  // Done with nothing done: exit 5.
  {
    GuiProcess p;
    CHECK(p.start(exe, {L"--gui", L"--change-cover", L"deluxe", L"--dest", assets.wstring()}));
    if (HWND w = p.window()) GuiProcess::click(w, IDOK);
    CHECK_EQ(p.wait(), 5);
  }

  // Removing a release, on that install (Deluxe: FILES and import.json; its
  // cover, the picture set above, is kept).
  const fs::path files = assets / L"win" / L"FILES", record = assets / L"win" / L"import.json";
  // Whether the catalog lists it (the synthetic disc's modules are no real ones: it has none).
  auto listed = [&] { return gui::catalog_module_counts(assets / L"win").count("deluxe") == 1; };
  CHECK(fs::is_directory(files) && fs::exists(record) && listed());
  // `--gui --remove`, then Cancel: nothing changes (exit 5).
  {
    GuiProcess p;
    CHECK(p.start(exe, {L"--gui", L"--remove", L"deluxe", L"--dest", assets.wstring()}));
    HWND w = p.window();
    CHECK(w != nullptr && GuiProcess::control(w, gui::kIdRemove) && IsWindowVisible(GuiProcess::control(w, gui::kIdRemove)));
    if (w) GuiProcess::click(w, IDCANCEL);
    CHECK_EQ(p.wait(), 5);
    CHECK(fs::is_directory(files) && listed());
  }
  // A release that isn't installed: nothing to remove (no Remove button), exit 5.
  {
    GuiProcess p;
    CHECK(p.start(exe, {L"--gui", L"--remove", L"tt", L"--dest", assets.wstring()}));
    HWND w = p.window();
    CHECK(w != nullptr);
    if (w) {
      HWND b = GuiProcess::control(w, gui::kIdRemove);
      CHECK(!b || !IsWindowVisible(b));
      GuiProcess::click(w, gui::kIdRemove);   // not there to press
      GuiProcess::click(w, IDCANCEL);
    }
    CHECK_EQ(p.wait(), 5);
  }
  // A file of it in use (as a running module would hold it): its folder
  // can't be moved aside, so the window says nothing was removed and offers
  // to try again; Cancel then ends with that failure (exit 1), and
  // everything is as it was.
  {
    fs::path held;
    for (const auto& e : fs::recursive_directory_iterator(files))
      if (e.is_regular_file()) {
        held = e.path();
        break;
      }
    CHECK(!held.empty());
    HANDLE h = held.empty() ? INVALID_HANDLE_VALUE
                            : CreateFileW(held.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE);
    GuiProcess p;
    CHECK(p.start(exe, {L"--gui", L"--remove", L"deluxe", L"--dest", assets.wstring()}));
    HWND w = p.window();
    CHECK(w != nullptr);
    if (w) {
      HWND remove = GuiProcess::control(w, gui::kIdRemove), cancel = GuiProcess::control(w, IDCANCEL);
      GuiProcess::click(w, gui::kIdRemove);
      // Cancel greys while it tries (a rename is retried for 2 s), and comes back with "Try again".
      const ULONGLONG until = GetTickCount64() + 30000;
      bool ran = false;
      wchar_t text[32] = {};
      while (GetTickCount64() < until) {
        const bool enabled = cancel && IsWindowEnabled(cancel);
        ran = ran || !enabled;
        GetWindowTextW(remove, text, 32);
        if (enabled && ran && wcscmp(text, L"Try again") == 0) break;
        Sleep(50);
      }
      CHECK(ran && wcscmp(text, L"Try again") == 0);
      GuiProcess::click(w, IDCANCEL);
    }
    CHECK_EQ(p.wait(), 1);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    CHECK(fs::is_directory(files) && fs::exists(record) && listed());
  }
  // From Sources: Deluxe's menu's "Remove…" (450) opens the window that asks;
  // Remove goes back to Sources, which says what was removed and no longer
  // shows it; Close is exit 0. Only its cover stays.
  {
    GuiProcess p;
    CHECK(p.start(exe, {L"--gui", L"--dest", assets.wstring()}));
    HWND src = p.window();
    CHECK(src != nullptr && GuiProcess::control(src, 300) != nullptr);
    if (src) {
      GuiProcess::click(src, 450);
      CHECK(GuiProcess::gone(src));
      HWND ask = p.window_with(src, gui::kIdRemove);
      CHECK(ask != nullptr);
      if (ask) {
        GuiProcess::click(ask, gui::kIdRemove);
        CHECK(GuiProcess::gone(ask, 30000));
        HWND back = p.window_with(ask, 101);
        CHECK(back != nullptr);
        if (back) {
          CHECK(GuiProcess::control(back, 300) == nullptr);   // nothing installed now: no covers
          wchar_t close[16] = {};
          GetWindowTextW(GuiProcess::control(back, IDCANCEL), close, 16);
          CHECK(wcscmp(close, L"Close") == 0);
          GuiProcess::click(back, IDCANCEL);
        }
      }
    }
    CHECK_EQ(p.wait(), 0);
    CHECK(!fs::exists(files) && !fs::exists(record) && !listed());
    CHECK(fs::exists(tile));
  }
  // Imported again, and removed by `--gui --remove` alone: Remove ends it (exit 0).
  {
    test::ProcessResult r = test::run_process(exe,
                                              {L"--iso", (dir / L"synthetic.iso").wstring(), L"--dest", assets.wstring(),
                                               L"--no-verify", L"--no-cover-download", L"--quiet"},
                                              120000);
    CHECK_EQ(r.exit_code, 0);
    CHECK(fs::is_directory(files) && listed());
    GuiProcess p;
    CHECK(p.start(exe, {L"--gui", L"--remove", L"deluxe", L"--dest", assets.wstring()}));
    if (HWND w = p.window()) GuiProcess::click(w, gui::kIdRemove);
    CHECK_EQ(p.wait(30000), 0);
    CHECK(!fs::exists(files) && !fs::exists(record) && !listed());
  }
  return test::g_failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: test_import_gui model [scratch] | shots|flow <adimport.exe> [scratch]\n");
    return 2;
  }
  const std::string mode = argv[1];
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  adw::ui::gdiplus_startup();
  int rc = 0;
  if (mode == "model") {
    fs::path dir = test::scratch(argc - 1, argv + 1, "adw-import-gui-model");
    test::sandbox_data_root(dir / L"localappdata");  // no default may reach the real data folder
    test_model(dir);
    test_dialog_code(dir);
    rc = test::finish("import.gui_model");
  } else if ((mode == "shots" || mode == "flow") && argc >= 3) {
    const std::wstring exe = fs::absolute(argv[2]).wstring();
    fs::path dir = test::scratch(argc - 2, argv + 2, mode == "shots" ? "adw-import-gui-shots" : "adw-import-gui-flow");
    // No default may reach the real data folder: every adimport started here
    // resolves its defaults (the Downloads page's folder, a run without
    // --dest) under the scratch tree.
    test::sandbox_data_root(dir / L"localappdata");
    if (mode == "shots") {
      test_shots(exe, dir);
      rc = test::finish("import.gui_shots");
    } else {
      rc = test_flow(exe, dir);
      if (rc != 77) rc = test::finish("import.gui_flow");
    }
  } else {
    fprintf(stderr, "unknown mode %s\n", mode.c_str());
    rc = 2;
  }
  adw::ui::gdiplus_shutdown();
  CoUninitialize();
  return rc;
}
