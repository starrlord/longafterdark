// adimport — puts the original Windows modules of the known releases (the
// After Dark releases of the registry, packages.h, and Star Wars Screen
// Entertainment) in place (DESIGN.md §6, PACKAGES.md) and keeps each
// release's box cover (COVERS.md §2).
//
//   adimport --image <image> [--image <image2> …] | --iso <image> | --from <drive or folder>
//            | --download [<id> | all]
//            [--package <id>] [--dest <assets root>] [--gui] [--no-verify] [--quiet]
//            [--download-dir <dir>] [--url <url> [--md5 <hex>]] [--no-cover-download]
//   adimport --catalog-only [--dest <assets root>] [--quiet]
//   adimport --list-packages [--dest <assets root>]
//   adimport --remove <id> [--dest <assets root>] [--quiet]
//   adimport --set-cover <id> <picture> [--dest <assets root>] [--quiet]
//   adimport --clear-cover <id> [--dest <assets root>] [--quiet]
//   adimport --refresh-covers [<id> | all] [--force] [--dest <assets root>] [--download-dir <dir>] [--quiet]
//   adimport --gui --change-cover <id> [--dest <assets root>] [--download-dir <dir>] [--no-cover-download]
//   adimport --gui --refresh-covers [<id> | all] [--force] [--dest <assets root>] [--download-dir <dir>]
//
// The source is identified as one of the known releases (packages.h) and
// only that release's directory is replaced. Every import ends by writing
// catalog-win.json (DESIGN.md §6a) over every installed package;
// --catalog-only rewrites it from the files already there.
//
// Exit codes are adw::import::Status: 0 ok, 1 error, 2 source invalid,
// 3 verify failed, 4 network, 5 cancelled.
//
// --download fetches a package's Internet Archive copy (packages.h
// `downloads`; Deluxe when no package is named), checks it against its
// published size and md5 and imports it; `--download all` does that for
// every package in turn.
//
// Covers (covers.h): an import captures the package's box cover from the
// registry's cover sources (downloads unless --no-cover-download, and art on
// the disc being imported); --set-cover stores the user's own picture,
// --clear-cover goes back to the original, --refresh-covers tries the
// downloads again. The cover commands are modes, like --catalog-only.
//
// --gui is how LongAfterDark.scr's settings dialog runs it: with no source it
// first offers the three sources (image file(s), disc/folder, Internet Archive
// — a list of the releases, their download sizes and whether each is already
// imported), then shows progress with a Cancel button; --change-cover <id>
// opens only the cover window, and --refresh-covers only a progress window
// that tries the cover downloads (the settings dialog's "Get the covers").
// The windows live in gui/ (gui.h, COVERS.md
// §4). It is a console program so scripted runs get output and an exit code;
// launched with no arguments from Explorer (no parent console) it behaves as
// --gui. On Windows 11 24H2+ the manifest keeps a GUI parent from giving it a
// console window at all; on earlier systems the parent must start it with
// CREATE_NO_WINDOW so no console flashes (the .scr's dialog does, and reads
// exit 5 as "cancelled, nothing changed").
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "adw_version.h"
#include "cancel.h"
#include "catalog.h"
#include "covers.h"
#include "gui.h"
#include "importer.h"
#include "winutil.h"

using namespace adw::import;
namespace fs = std::filesystem;

namespace {

struct Args {
  std::optional<Source> source;
  fs::path dest;
  bool gui = false, quiet = false, no_verify = false, help = false, version = false, catalog_only = false,
       list_packages = false;
  bool download_all = false;  // --download all
  std::string remove;   // --remove <id>
  std::string package;  // --package <id>, also for a source the GUI asks for
  // Covers (COVERS.md §2.8).
  std::string set_cover;         // --set-cover <id> <picture>
  fs::path set_cover_picture;
  std::string clear_cover;       // --clear-cover <id>
  bool refresh_covers = false;   // --refresh-covers [<id> | all]
  std::string refresh_id;        // "" = every installed package
  bool force = false;            // --refresh-covers --force
  bool no_cover_download = false;
  std::string change_cover;      // --gui --change-cover <id>
  fs::path download_dir;         // --download-dir (imports and --refresh-covers)
  std::string error;
};

void usage(FILE* f) {
  std::string ids, titles;
  for (const Package& p : builtin_packages()) {
    ids += std::string(ids.empty() ? "" : ", ") + p.id;
    titles += std::string("  ") + p.title + "\n";
  }
  fprintf(f,
          "usage: adimport --image <image> [--image <image2> ...] | --from <drive or folder>\n"
          "                | --download [<id> | all]\n"
          "                [--package <id>] [--dest <assets root>] [--gui] [--no-verify] [--quiet]\n"
          "                [--download-dir <dir>] [--url <url> [--md5 <hex>]] [--no-cover-download]\n"
          "       adimport --catalog-only [--dest <assets root>] [--quiet]\n"
          "       adimport --list-packages [--dest <assets root>]\n"
          "       adimport --remove <id> [--dest <assets root>] [--quiet]\n"
          "       adimport --set-cover <id> <picture> [--dest <assets root>] [--quiet]\n"
          "       adimport --clear-cover <id> [--dest <assets root>] [--quiet]\n"
          "       adimport --refresh-covers [<id> | all] [--force] [--dest <assets root>]\n"
          "                [--download-dir <dir>] [--quiet]\n"
          "       adimport --gui --change-cover <id> [--dest <assets root>] [--download-dir <dir>]\n"
          "                [--no-cover-download]\n"
          "       adimport --gui --refresh-covers [<id> | all] [--force] [--dest <assets root>]\n"
          "                [--download-dir <dir>]\n"
          "\n"
          "Identifies which of these releases the source is,\n"
          "%s"
          "copies its Windows files into <assets root>\\win (After Dark 4.0 Deluxe: FILES; every\n"
          "other release: packages\\<id>), verifies them, and rewrites the module catalog,\n"
          "<assets root>\\win\\catalog-win.json, over every imported release.\n"
          "  --image <path>      an image of a CD (.iso, or a raw 2352-byte-sector .bin) or of a\n"
          "                      floppy disk (.img/.ima/.vfd/.flp, FAT12/16), or a ZIP of the\n"
          "                      install files or of floppy images; repeat it for every disk of\n"
          "                      a set. --iso is the same option\n"
          "  --from <dir>        a CD drive (E:\\) or any folder holding a copy of the disc or\n"
          "                      floppies\n"
          "  --download [<id>]   fetch the release's copy from the Internet Archive (After Dark\n"
          "                      4.0 Deluxe when no id or --package is given; see --list-packages\n"
          "                      for the ids and sizes), check it against its published md5 and\n"
          "                      import it. Kept in %%LOCALAPPDATA%%\\LongAfterDark\\downloads (or\n"
          "                      --download-dir); an interrupted download resumes, and a file\n"
          "                      already there is reused\n"
          "  --download all      the same for every release, one after another\n"
          "  --download-dir <dir>\n"
          "                      where downloads are kept; with any import, also the cover\n"
          "                      pictures it downloads (in <dir>\\covers)\n"
          "  --package <id>      accept the source only as this release (ids: %s)\n"
          "  --dest <dir>        assets root (default %%AD_ASSETS_DIR%% or\n"
          "                      %%LOCALAPPDATA%%\\LongAfterDark\\assets)\n"
          "  --gui               progress window; with no source, ask for one\n"
          "  --no-verify         import files that differ from the known release\n"
          "  --no-cover-download take the box cover only from the disc being imported (else the\n"
          "                      cover stays generated); by default an import also fetches the\n"
          "                      release's cover picture, checked against its published md5\n"
          "  --catalog-only      import nothing: rewrite catalog-win.json from the files\n"
          "                      already imported\n"
          "  --list-packages     the known releases, which are imported, their covers and\n"
          "                      their downloads\n"
          "  --remove <id>       delete an imported release and rewrite the catalog (its box\n"
          "                      cover is kept for a later import)\n"
          "  --version           print the version and exit\n"
          "Ctrl+C cancels an import, a download or a cover fetch (exit 5); a second one\n"
          "ends adimport at once.\n"
          "\n"
          "Box covers. Each imported release has a cover, kept as PNG files in\n"
          "<assets root>\\win\\covers\\<id>: your own picture when you set one, else the best\n"
          "picture found so far (a download checked against its md5, or art from the disc),\n"
          "else a generated cover. Nothing fetches covers in the background.\n"
          "  --set-cover <id> <picture>\n"
          "                      use your own picture (PNG, JPEG, GIF, BMP, TIFF, or any format\n"
          "                      Windows can read) as the release's cover; it stays on this\n"
          "                      computer and imports keep it\n"
          "  --clear-cover <id>  go back to the original cover\n"
          "  --refresh-covers [<id> | all]\n"
          "                      for installed releases (all when no id is given): fetch the\n"
          "                      cover downloads better than the current original, and repair\n"
          "                      missing or damaged cover files. Pictures are kept in\n"
          "                      <download dir>\\covers (--download-dir)\n"
          "  --force             (--refresh-covers) try every cover download; the current\n"
          "                      cover stays when none can be used\n"
          "  --change-cover <id> (implies --gui) only the cover window for that release\n"
          "  --gui --refresh-covers [<id> | all]\n"
          "                      --refresh-covers in a progress window; exit 0 when a cover\n"
          "                      changed, else the first failure, or 5 when nothing changed\n"
          "\n"
          "exit: 0 ok, 1 error, 2 source invalid, 3 verify failed, 4 network, 5 cancelled\n"
          "      --set-cover: 2 when the picture can't be read. --refresh-covers: 4 when a\n"
          "      download failed (the previous cover is kept)\n"
          "\n"
          "Data folder: %%LOCALAPPDATA%%\\LongAfterDark, shared with the screen saver\n",
          titles.c_str(), ids.c_str());
}

Args parse_args(int argc, wchar_t** argv) {
  Args a;
  Source s;
  int sources = 0;
  bool have_image = false, have_folder = false, have_download = false, have_quiet = false;
  std::string url, md5;
  auto need = [&](int& i) -> const wchar_t* {
    if (i + 1 >= argc) {
      a.error = "missing value after " + to_utf8(argv[i]);
      return nullptr;
    }
    return argv[++i];
  };
  // An optional value: the next argument, when it is not an option.
  auto optional_value = [&](int i) { return i + 1 < argc && argv[i + 1][0] != L'-' && argv[i + 1][0] != L'/'; };
  auto known_id = [&](const std::string& id, const char* after) {
    if (find_package(id)) return true;
    a.error = "unknown package \"" + id + "\" after " + after + " (see --list-packages)";
    return false;
  };
  for (int i = 1; i < argc && a.error.empty(); i++) {
    std::wstring k = argv[i];
    if (k == L"--iso" || k == L"--image") {
      if (auto v = need(i)) {
        if (!have_image) {
          s.path = v;
          have_image = true;
          sources++;
        } else {
          s.more_images.push_back(v);
        }
      }
    } else if (k == L"--from") {
      if (auto v = need(i)) {
        s.path = v;
        have_folder = true;
        sources++;
      }
    } else if (k == L"--download") {
      have_download = true;
      sources++;
      // An optional release: "--download ad32", "--download all".
      if (optional_value(i)) {
        std::string v = to_utf8(argv[++i]);
        if (v == "all") {
          a.download_all = true;
        } else if (!find_package(v)) {
          a.error = "unknown package \"" + v + "\" after --download (see --list-packages, or use all)";
        } else if (!s.package.empty() && s.package != v) {
          a.error = "--download " + v + " and --package " + s.package + " name different releases";
        } else {
          s.package = v;
        }
      }
    } else if (k == L"--package") {
      if (auto v = need(i)) {
        std::string id = to_utf8(v);
        if (!find_package(id)) a.error = "unknown package \"" + id + "\" (see --list-packages)";
        else if (!s.package.empty() && s.package != id)
          a.error = "--download " + s.package + " and --package " + id + " name different releases";
        s.package = id;
      }
    } else if (k == L"--list-packages") {
      a.list_packages = true;
    } else if (k == L"--remove") {
      if (auto v = need(i)) a.remove = to_utf8(v);
    } else if (k == L"--set-cover") {
      if (auto v = need(i)) {
        a.set_cover = to_utf8(v);
        if (auto pic = need(i)) {
          a.set_cover_picture = fs::path(pic).make_preferred();
          known_id(a.set_cover, "--set-cover");
        }
      }
    } else if (k == L"--clear-cover") {
      if (auto v = need(i)) {
        a.clear_cover = to_utf8(v);
        known_id(a.clear_cover, "--clear-cover");
      }
    } else if (k == L"--refresh-covers") {
      a.refresh_covers = true;
      if (optional_value(i)) {
        std::string v = to_utf8(argv[++i]);
        if (v != "all" && known_id(v, "--refresh-covers")) a.refresh_id = v;
      }
    } else if (k == L"--force") {
      a.force = true;
    } else if (k == L"--no-cover-download") {
      a.no_cover_download = true;
    } else if (k == L"--change-cover") {
      if (auto v = need(i)) {
        a.change_cover = to_utf8(v);
        known_id(a.change_cover, "--change-cover");
      }
    } else if (k == L"--dest") {
      // make_preferred: "D:/x" would otherwise print as "D:/x\win" everywhere.
      if (auto v = need(i)) a.dest = fs::path(v).make_preferred();
    } else if (k == L"--download-dir") {
      if (auto v = need(i)) a.download_dir = fs::path(v).make_preferred();
    } else if (k == L"--url") {
      if (auto v = need(i)) url = to_utf8(v);
    } else if (k == L"--md5") {
      if (auto v = need(i)) md5 = to_utf8(v);
    } else if (k == L"--gui") {
      a.gui = true;
    } else if (k == L"--quiet" || k == L"-q") {
      a.quiet = true;
      have_quiet = true;
    } else if (k == L"--no-verify") {
      a.no_verify = true;
    } else if (k == L"--catalog-only") {
      a.catalog_only = true;
    } else if (k == L"--help" || k == L"-h" || k == L"/?") {
      a.help = true;
    } else if (k == L"--version") {
      a.version = true;
    } else {
      a.error = "unknown argument " + to_utf8(k);
    }
  }
  if (!a.error.empty()) return a;
  // What only an import (or the GUI) takes.
  const bool import_args = sources || a.gui || a.no_verify || !url.empty() || !md5.empty() || !s.package.empty();
  const bool dl_dir = !a.download_dir.empty();
  const int modes = int(a.catalog_only) + int(a.list_packages) + int(!a.remove.empty()) + int(!a.set_cover.empty()) +
                    int(!a.clear_cover.empty()) + int(a.refresh_covers) + int(!a.change_cover.empty());
  if (modes > 1) {
    a.error = "give only one of --catalog-only, --list-packages, --remove, --set-cover, --clear-cover, "
              "--refresh-covers, --change-cover";
    return a;
  }
  if (a.force && !a.refresh_covers) {
    a.error = "--force only applies to --refresh-covers";
    return a;
  }
  // A mode takes only the options listed for it (COVERS.md §2.8).
  if (a.catalog_only) {
    // Nothing is imported, so nothing about a source or a transfer applies.
    if (import_args || dl_dir || a.no_cover_download) a.error = "--catalog-only takes only --dest and --quiet";
    return a;
  }
  if (a.list_packages) {
    if (import_args || dl_dir || a.no_cover_download || have_quiet) a.error = "--list-packages takes only --dest";
    return a;
  }
  if (!a.remove.empty()) {
    if (import_args || dl_dir || a.no_cover_download) a.error = "--remove takes only --dest and --quiet";
    else if (!find_package(a.remove)) a.error = "unknown package \"" + a.remove + "\" (see --list-packages)";
    return a;
  }
  if (!a.set_cover.empty()) {
    if (import_args || dl_dir || a.no_cover_download) a.error = "--set-cover takes only --dest and --quiet";
    return a;
  }
  if (!a.clear_cover.empty()) {
    if (import_args || dl_dir || a.no_cover_download) a.error = "--clear-cover takes only --dest and --quiet";
    return a;
  }
  if (a.refresh_covers) {
    // In a window too (--gui): the settings dialog's "Get the covers".
    const bool other = sources || a.no_verify || !url.empty() || !md5.empty() || !s.package.empty();
    if (other || a.no_cover_download || (a.gui && have_quiet))
      a.error = "--refresh-covers takes only a package id or all, --force, --gui, --dest, --download-dir and --quiet";
    return a;
  }
  if (!a.change_cover.empty()) {
    // The cover window alone: --gui is implied, and no source is read
    // (--download-dir: where "Download the original cover" keeps the picture).
    if (sources || a.no_verify || !url.empty() || !md5.empty() || !s.package.empty() || have_quiet)
      a.error = "--change-cover takes only --gui, --dest, --download-dir and --no-cover-download";
    a.gui = true;
    return a;
  }
  if (a.no_cover_download && !sources && !a.gui) {
    a.error = "--no-cover-download only applies to an import (--image, --from, --download or --gui)";
    return a;
  }
  if (sources > 1) {
    a.error = "give only one of --image/--iso, --from, --download";
    return a;
  }
  s.kind = have_download ? Source::Kind::download : have_folder ? Source::Kind::folder : Source::Kind::image;
  if (a.download_all && (!s.package.empty() || !url.empty() || !md5.empty())) {
    a.error = "--download all fetches every release: it takes no --package, --url or --md5";
    return a;
  }
  if (sources == 1) {
    if (s.kind == Source::Kind::download) {
      s.path = a.download_dir;
      s.url = url;
      for (char& c : md5)
        if (c >= 'A' && c <= 'F') c = char(c - 'A' + 'a');
      // A typo here would otherwise download 400 MB and then fail verify.
      if (!md5.empty() && (md5.size() != 32 || md5.find_first_not_of("0123456789abcdef") != std::string::npos)) {
        a.error = "--md5 needs 32 hex digits";
        return a;
      }
      s.expected_md5 = md5;
    } else if (!url.empty() || !md5.empty()) {
      // (--download-dir also says where an image or folder import keeps the
      // cover pictures it downloads.)
      a.error = "--url/--md5 only apply to --download";
      return a;
    }
    a.source = s;
  }
  a.package = s.package;
  return a;
}

std::string mb(uint64_t n) {
  char b[32];
  snprintf(b, sizeof(b), "%.1f MB", double(n) / (1024.0 * 1024.0));
  return b;
}

// ---- console front-end ------------------------------------------------------

// What the console calls a phase: phase_name, with the cover phase (whose
// name is its enum's, "cover") told as what is happening.
const char* phase_label(Progress::Phase p) {
  return p == Progress::Phase::cover ? "Getting the cover art" : phase_name(p);
}

// Ctrl+C (or Ctrl+Break) during an import, a download or a cover fetch: the
// first one cancels it the way the GUI's Cancel does (at once, even while a
// download waits on the network), so the stage is cleaned up and the exit
// code is 5; a second one ends the process as usual.
CancelToken g_cancel;

BOOL WINAPI on_console_ctrl(DWORD event) {
  if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) return FALSE;
  if (g_cancel.cancelled()) return FALSE;
  g_cancel.cancel();
  return TRUE;
}

// For the length of a run that honours the token.
struct CtrlCCancels {
  CtrlCCancels() { SetConsoleCtrlHandler(on_console_ctrl, TRUE); }
  ~CtrlCCancels() { SetConsoleCtrlHandler(on_console_ctrl, FALSE); }
  CtrlCCancels(const CtrlCCancels&) = delete;
  CtrlCCancels& operator=(const CtrlCCancels&) = delete;
};

class ConsoleProgress {
 public:
  explicit ConsoleProgress(bool quiet) : quiet_(quiet) {
    DWORD mode;
    tty_ = GetConsoleMode(GetStdHandle(STD_ERROR_HANDLE), &mode) != 0;
  }
  ConsoleProgress(const ConsoleProgress&) = delete;
  ConsoleProgress& operator=(const ConsoleProgress&) = delete;

  bool on(const Progress& p) {
    if (quiet_) return true;
    ULONGLONG now = GetTickCount64();
    bool new_phase = !started_ || p.phase != phase_;
    if (new_phase) {
      end_line();
      phase_ = p.phase;
      started_ = true;
      decile_ = -1;
      speed_tick_ = now;
      speed_bytes_ = p.done;
      if (!tty_) fprintf(stderr, "adimport: %s\n", phase_label(p.phase));
    }
    // A download that restarts from byte 0 (server ignored Range) goes
    // backwards; restart the rate window rather than underflow it.
    if (p.done < speed_bytes_) speed_tick_ = now, speed_bytes_ = p.done;
    int pct = p.total ? int(p.done * 100 / p.total) : -1;
    if (tty_) {
      if (!new_phase && now - last_tick_ < 200 && p.done != p.total) return true;
      last_tick_ = now;
      std::string line = std::string(phase_label(p.phase)) + "  " + mb(p.done);
      if (p.total) line += " / " + mb(p.total) + "  " + std::to_string(pct) + "%";
      if ((p.phase == Progress::Phase::download || p.phase == Progress::Phase::cover) && now > speed_tick_ + 1000) {
        double rate = double(p.done - speed_bytes_) / (double(now - speed_tick_) / 1000.0);
        line += "  " + mb(uint64_t(rate)) + "/s";
      }
      if (!p.item.empty()) line += "  " + p.item;
      if (line.size() < width_) line.append(width_ - line.size(), ' ');
      width_ = std::max<size_t>(width_, line.size());
      fprintf(stderr, "\r%s", line.c_str());
      open_ = true;
    } else if (pct >= 0 && pct / 10 > decile_) {
      decile_ = pct / 10;
      fprintf(stderr, "adimport:   %d%% (%s)\n", pct, mb(p.done).c_str());
    }
    return true;
  }

  void log(const std::string& s) {
    if (quiet_) return;
    end_line();
    fprintf(stderr, "adimport: %s\n", s.c_str());
  }

  void end_line() {
    if (open_) fprintf(stderr, "\n");
    open_ = false;
    width_ = 0;
  }

 private:
  bool quiet_, tty_ = false, open_ = false, started_ = false;
  Progress::Phase phase_ = Progress::Phase::copy;
  int decile_ = -1;
  ULONGLONG last_tick_ = 0, speed_tick_ = 0;
  uint64_t speed_bytes_ = 0;
  size_t width_ = 0;
};

void print_result(const ImportResult& r, bool quiet) {
  if (r.status == Status::ok) {
    if (!quiet) {
      printf("%s\n", r.message.c_str());
      printf("  into    %s\n", to_utf8(r.files_dir.wstring()).c_str());
      printf("  record  %s\n", to_utf8(r.import_json.wstring()).c_str());
      printf("  catalog %s\n", to_utf8(r.catalog.wstring()).c_str());
      if (!r.url.empty()) printf("  from    %s\n", r.url.c_str());
      if (!r.iso_md5.empty()) {
        const bool zip = r.format == "zip";
        printf("  %s md5 %s (%s)\n", zip ? "zip  " : "image", r.iso_md5.c_str(),
               r.iso_md5_known           ? ((zip ? "the known ZIP of " : "the known image of ") + r.package_title).c_str()
               : r.download_md5_checked  ? "the expected md5 of the download"
                                         : "not a known image");
      }
      std::string installed;
      for (const std::string& t : r.installed) installed += (installed.empty() ? "" : ", ") + t;
      printf("  installed %s\n", installed.c_str());
    }
  } else {
    fprintf(stderr, "adimport: %s: %s\n", status_name(r.status), r.message.c_str());
    for (const ImportedFile& f : r.files)
      if (f.known == ImportedFile::Known::mismatch) fprintf(stderr, "  differs: %s (md5 %s)\n", f.path.c_str(), f.md5.c_str());
  }
}

int run_cli(const Args& a) {
  ConsoleProgress con(a.quiet);
  CtrlCCancels ctrl_c;
  ImportOptions o;
  o.cancel = &g_cancel;
  o.assets_root = a.dest;
  o.check_known = !a.no_verify;
  o.cover_download = !a.no_cover_download;
  o.cover_download_dir = a.download_dir;  // empty: the download's own, else the default
  o.progress = [&](const Progress& p) { return con.on(p); };
  o.log = [&](const std::string& s) { con.log(s); };
  if (a.download_all) {
    // Every release in turn; the exit code is the first failure's (0 when
    // every one was imported).
    std::vector<std::string> ids = downloadable_packages();
    std::vector<ImportResult> rs = import_downloads(ids, *a.source, o);
    con.end_line();
    int code = 0;
    size_t ok = 0;
    for (size_t i = 0; i < rs.size(); i++) {
      const Package* p = find_package(ids[i]);
      if (!a.quiet || rs[i].status != Status::ok) printf("%s:\n", p ? p->title : ids[i].c_str());
      print_result(rs[i], a.quiet);
      ok += rs[i].status == Status::ok;
      if (!code && rs[i].status != Status::ok) code = int(rs[i].status);
    }
    if (!a.quiet)
      printf("imported %zu of %zu releases%s\n", ok, ids.size(),
             rs.size() < ids.size() ? " (cancelled)" : "");
    return code;
  }
  ImportResult r = run_import(*a.source, o);
  con.end_line();
  print_result(r, a.quiet);
  return int(r.status);
}

int run_list_packages(const Args& a) {
  fs::path root = a.dest.empty() ? default_assets_root() : a.dest;
  printf("packages in %s\n", to_utf8(win_assets_dir(root).wstring()).c_str());
  // The title column fits the longest title ("The Disney Collection Screen Saver").
  int title_w = 0;
  for (const Package& p : builtin_packages()) title_w = std::max(title_w, int(strlen(p.title)));
  for (const PackageState& s : list_packages(root)) {
    std::string state = s.installed ? "installed, " + std::to_string(s.file_count) + " files, verified: " + s.verified +
                                          (s.imported_utc.empty() ? "" : ", " + s.imported_utc)
                                    : "not installed";
    if (s.installed) {
      // COVERS.md §2.8: "; cover: <origin> (<label>)".
      CoverInfo c = cover_info(s.package->id, root);
      if (!c.error.empty()) fprintf(stderr, "adimport: warning: %s\n", c.error.c_str());
      state += std::string("; cover: ") + cover_origin_name(c.origin);
      std::string label = c.label;
      if (!c.credit.empty() && c.origin != CoverOrigin::user) label += (label.empty() ? "" : ", ") + c.credit;
      if (!label.empty()) state += " (" + label + ")";
    }
    std::string dl = "no download";
    if (!s.package->downloads.empty()) {
      const Download& d = s.package->downloads.front();
      const bool zip = std::string_view(d.kind) == "zip";
      const std::string parts = std::to_string(1 + d.more_images.size());
      dl = "download " + mb(download_size(d)) +
           (zip && d.more_images.empty() ? " (ZIP of the install files)"
            : zip                        ? " (" + parts + " ZIPs of the install disks' files)"
            : d.more_images.empty()      ? " (disc image)"
                                         : " (" + parts + " floppy images)");
    }
    printf("  %-9s %-*s %s; %s\n", s.package->id, title_w, s.package->title, state.c_str(), dl.c_str());
  }
  return 0;
}

int run_remove(const Args& a) {
  RemoveResult r = remove_package(a.remove, a.dest, [&](const std::string& s) {
    if (!a.quiet) fprintf(stderr, "adimport: %s\n", s.c_str());
  });
  if (r.status != Status::ok) {
    fprintf(stderr, "adimport: %s: %s\n", status_name(r.status), r.message.c_str());
  } else if (!a.quiet) {
    printf("%s\n", r.message.c_str());
  }
  return int(r.status);
}

// ---- covers (COVERS.md §2.8) -------------------------------------------------------

CoverOptions cover_options(const Args& a, ConsoleProgress& con) {
  CoverOptions o;
  o.assets_root = a.dest;
  o.download_dir = a.download_dir;
  o.force = a.force;
  o.cancel = &g_cancel;
  o.progress = [&con](const Progress& p) { return con.on(p); };
  o.log = [&con](const std::string& s) { con.log(s); };
  return o;
}

std::string describe(const CoverInfo& c) {
  if (c.origin == CoverOrigin::generated) return "a generated cover";
  std::string s = c.origin == CoverOrigin::user ? "your own picture" : c.label.empty() ? "the original" : c.label;
  if (c.origin != CoverOrigin::user && !c.credit.empty()) s += " (" + c.credit + ")";
  if (c.width && c.height) s += ", " + std::to_string(c.width) + "x" + std::to_string(c.height);
  return s;
}

void print_cover(const CoverResult& r, const Args& a) {
  if (r.status != Status::ok) {
    fprintf(stderr, "adimport: %s: %s\n", status_name(r.status), r.message.c_str());
    return;
  }
  if (a.quiet) return;
  printf("%s\n", r.message.c_str());
  if (!r.info.tile.empty()) printf("  tile    %s\n", to_utf8(r.info.tile.wstring()).c_str());
  printf("  cover   %s\n", describe(r.info).c_str());
}

int run_set_cover(const Args& a) {
  ConsoleProgress con(a.quiet);
  CoverResult r = set_cover(a.set_cover, a.set_cover_picture, cover_options(a, con));
  con.end_line();
  print_cover(r, a);
  return int(r.status);
}

int run_clear_cover(const Args& a) {
  ConsoleProgress con(a.quiet);
  CoverResult r = clear_cover(a.clear_cover, cover_options(a, con));
  con.end_line();
  print_cover(r, a);
  return int(r.status);
}

// 0 when every package now has the best cover this run could reach, 4 when a
// download failed (the previous cover is kept; the output says which), 1 on
// any other error.
int run_refresh_covers(const Args& a) {
  ConsoleProgress con(a.quiet);
  CtrlCCancels ctrl_c;
  std::vector<std::string> ids;
  if (!a.refresh_id.empty()) ids.push_back(a.refresh_id);
  std::vector<CoverResult> rs = refresh_covers(ids, cover_options(a, con));
  con.end_line();
  if (rs.empty()) {
    if (!a.quiet)
      printf("nothing is imported in %s: no cover to refresh\n",
             to_utf8(win_assets_dir(a.dest.empty() ? default_assets_root() : a.dest).wstring()).c_str());
    return int(Status::ok);
  }
  bool error = false, network = false;
  for (const CoverResult& r : rs) {
    const Package* p = find_package(r.info.package);
    std::string title = p ? p->title : r.info.package;
    if (r.status == Status::ok) {
      if (!a.quiet) printf("%s: %s\n", title.c_str(), r.message.c_str());
    } else {
      fprintf(stderr, "adimport: %s%s%s: %s\n", title.c_str(), title.empty() ? "" : ": ", status_name(r.status),
              r.message.c_str());
      if (r.status == Status::network) network = true;
      else error = true;
    }
  }
  return int(error ? Status::error : network ? Status::network : Status::ok);
}

int run_catalog_only(const Args& a) {
  CatalogResult r = regenerate_catalog(a.dest, [&](const std::string& s) {
    if (!a.quiet) fprintf(stderr, "adimport: %s\n", s.c_str());
  });
  if (r.status != Status::ok) {
    fprintf(stderr, "adimport: %s: %s\n", status_name(r.status), r.message.c_str());
  } else if (!a.quiet) {
    printf("%s\n", r.message.c_str());
  }
  return int(r.status);
}

// ---- the windows (gui/, COVERS.md §4, §8.2) ------------------------------------------

int run_windows(const Args& a) {
  gui::Request req;
  req.source = a.source;
  req.download_all = a.download_all;
  req.dest = a.dest;
  req.package = a.package;
  req.no_verify = a.no_verify;
  req.cover_download = !a.no_cover_download;
  req.change_cover = a.change_cover;
  req.download_dir = a.download_dir;
  req.refresh_covers = a.refresh_covers;
  req.refresh_id = a.refresh_id;
  req.force = a.force;
  return gui::run(req);
}

}  // namespace

// UTF-8 console output for paths and names, put back on the way out: the
// output code page belongs to the console, not the process, so leaving
// CP_UTF8 behind would change it for the shell that ran us.
class ConsoleUtf8 {
 public:
  ConsoleUtf8() : old_(GetConsoleOutputCP()) {
    if (old_) SetConsoleOutputCP(CP_UTF8);
  }
  ~ConsoleUtf8() {
    fflush(stdout);
    fflush(stderr);
    if (old_) SetConsoleOutputCP(old_);
  }
  ConsoleUtf8(const ConsoleUtf8&) = delete;
  ConsoleUtf8& operator=(const ConsoleUtf8&) = delete;

 private:
  UINT old_;  // 0 = no console
};

int main() {
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  ConsoleUtf8 utf8;
  Args a = parse_args(argc, argv);
  LocalFree(argv);
  if (a.help) {
    usage(stdout);
    return 0;
  }
  if (a.version) {
    printf("adimport %s (%s)\n", ADW_VERSION_STRING, ADW_PRODUCT_NAME);
    return 0;
  }
  if (!a.error.empty()) {
    fprintf(stderr, "adimport: %s\n\n", a.error.c_str());
    usage(stderr);
    return int(Status::error);
  }
  if (a.catalog_only) return run_catalog_only(a);
  if (!a.set_cover.empty()) return run_set_cover(a);
  if (!a.clear_cover.empty()) return run_clear_cover(a);
  if (a.refresh_covers && !a.gui) return run_refresh_covers(a);
  if (a.list_packages) return run_list_packages(a);
  if (!a.remove.empty()) return run_remove(a);
  // Started from a GUI rather than a shell: either no console at all (the
  // manifest's detached console policy, Windows 11 24H2+), or — on earlier
  // systems — alone on a visible console of its own that we are writing to.
  // A shell shares its console; CREATE_NO_WINDOW (tests) gives a console with
  // no window; scripts redirect stdout. All of those stay on the CLI path.
  DWORD procs[2];
  DWORD attached = GetConsoleProcessList(procs, 2);
  bool from_explorer = attached == 0 ||
                       (attached == 1 && GetConsoleWindow() != nullptr &&
                        GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) == FILE_TYPE_CHAR);
  if (!a.source && !a.gui) {
    if (!from_explorer) {
      usage(stderr);
      return int(Status::error);
    }
    a.gui = true;
  }
  if (a.gui) {
    if (from_explorer) FreeConsole();  // drop the console window Explorer opened for us
    return run_windows(a);
  }
  return run_cli(a);
}
