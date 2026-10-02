// adimport.exe as the .scr and scripts use it: argument handling and the
// exit-code contract (0 ok, 1 usage, 2 source invalid, 3 verify failed,
// 4 network), --catalog-only, and --download pointed at a loopback server
// that serves the synthetic image. With AD_GUI_TESTS=1 it also drives --gui
// end to end (AD_GUI_AUTOCLOSE=1 closes the progress window and skips the
// result box), and the source chooser's Internet Archive list.
#include "http_server.h"

#include <commctrl.h>

#include "fixture.h"
#include "importer.h"
#include "md5.h"
#include "run_process.h"

using namespace adw::import;
namespace fs = std::filesystem;

namespace {

int run(const std::wstring& exe, const std::vector<std::wstring>& args, const char* what,
        std::string* out = nullptr) {
  test::ProcessResult r = test::run_process(exe, args, 120000);
  fprintf(stderr, "[%s] exit %d\n%s", what, r.exit_code, r.output.c_str());
  if (out) *out = r.output;
  return r.exit_code;
}

// Starts adimport with `flags`; for each button id in `clicks`, waits for
// its next task dialog (a window titled "Long After Dark" that replaced
// the previous one) and presses that button. TDM_CLICK_BUTTON carries no
// pointers, so it can be sent across processes. Returns the exit code, or -1
// when a dialog did not appear.
int click_through(const std::wstring& exe, const std::vector<std::wstring>& args, DWORD flags,
                  const std::vector<int>& clicks, const char* what) {
  std::wstring cmd = test::command_line(exe, args);
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, flags, nullptr, nullptr, &si, &pi)) return -1;
  struct Find {
    DWORD pid;
    HWND hwnd = nullptr;
  };
  size_t shown = 0;
  HWND prev = nullptr;
  for (int button : clicks) {
    // The previous dialog must be gone first, so its handle is never taken for the next.
    for (int i = 0; i < 100 && prev && IsWindow(prev); i++) Sleep(50);
    Find find{pi.dwProcessId};
    for (int i = 0; i < 200 && !find.hwnd; i++) {
      if (WaitForSingleObject(pi.hProcess, 100) == WAIT_OBJECT_0) break;
      EnumWindows(
          [](HWND h, LPARAM lp) -> BOOL {
            auto* f = reinterpret_cast<Find*>(lp);
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            wchar_t title[64] = {};
            GetWindowTextW(h, title, 64);
            if (pid == f->pid && IsWindowVisible(h) && wcscmp(title, L"Long After Dark") == 0) {
              f->hwnd = h;
              return FALSE;
            }
            return TRUE;
          },
          reinterpret_cast<LPARAM>(&find));
    }
    if (!find.hwnd) break;
    shown++;
    Sleep(200);  // let it finish drawing: a click during creation can be lost
    SendMessageW(find.hwnd, TDM_CLICK_BUTTON, WPARAM(button), 0);
    prev = find.hwnd;
  }
  if (WaitForSingleObject(pi.hProcess, 20000) == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, DWORD(-2));
  DWORD code = 0;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  fprintf(stderr, "[%s] %zu of %zu dialogs shown, exit %d\n", what, shown, clicks.size(), int(code));
  return shown == clicks.size() ? int(code) : -1;
}

// Presses Cancel on the source chooser.
int cancel_chooser(const std::wstring& exe, const std::vector<std::wstring>& args, DWORD flags, const char* what) {
  return click_through(exe, args, flags, {IDCANCEL}, what);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: test_import_cli <adimport.exe> [scratch]\n");
    return 2;
  }
  std::wstring exe = fs::absolute(argv[1]).wstring();
  fs::path dir = test::scratch(argc - 1, argv + 1, "adw-import-cli");
  // No default may reach the real data folder: this process and every adimport it
  // starts (some without --dest) resolve theirs under the scratch tree.
  const fs::path lad = test::sandbox_data_root(dir / L"localappdata");
  test::IsoBuilder b;
  auto fixture = test::build_fixture(b);
  auto image = b.build();
  fs::path iso = dir / L"synthetic.iso";
  test::write_bytes(iso, image);
  fs::path dest = dir / L"assets";
  const bool manifest = !known_files().empty();

  std::string out;
  CHECK_EQ(run(exe, {L"--help"}, "help", &out), 0);
  CHECK(out.find("usage: adimport") != std::string::npos);
  // Every release by title, and every id for --package, the sixteenth last.
  const size_t tng_at = out.find("\n  Star Trek: The Next Generation Screen Saver"),
               castaway_at = out.find("\n  Screen Antics: Johnny Castaway");
  CHECK(tng_at != std::string::npos && castaway_at != std::string::npos && tng_at < castaway_at);
  CHECK(out.find(", farside, dilbert, tng, castaway)") != std::string::npos);
  CHECK_EQ(run(exe, {}, "no arguments, no console"), 1);
  CHECK_EQ(run(exe, {L"--bogus"}, "unknown flag"), 1);
  CHECK_EQ(run(exe, {L"--iso", iso.wstring(), L"--from", dir.wstring()}, "two sources"), 1);
  CHECK_EQ(run(exe, {L"--iso"}, "missing value"), 1);
  CHECK_EQ(run(exe, {L"--from", dir.wstring(), L"--url", L"http://x/"}, "--url without --download"), 1);

  CHECK_EQ(run(exe, {L"--no-cover-download", L"--from", (dir / L"nowhere").wstring(), L"--dest", dest.wstring()},
               "missing folder"),
           2);
  test::write_bytes(dir / L"garbage.iso", test::pattern(50000, 1));
  CHECK_EQ(run(exe, {L"--no-cover-download", L"--iso", (dir / L"garbage.iso").wstring(), L"--dest", dest.wstring()},
               "not an image"),
           2);
  CHECK(!fs::exists(dest / L"win" / L"FILES"));

  // The synthetic image is not the Deluxe release: verification fails when
  // the built-in manifest knows the paths, and --no-verify imports it.
  CHECK_EQ(
      run(exe, {L"--no-cover-download", L"--iso", iso.wstring(), L"--dest", dest.wstring()}, "verify against manifest"),
      manifest ? 3 : 0);
  CHECK_EQ(run(exe, {L"--no-cover-download", L"--iso", iso.wstring(), L"--dest", dest.wstring(), L"--no-verify"},
               "iso import", &out),
           0);
  CHECK(out.find("imported " + std::to_string(test::imported_count(fixture)) + " files") != std::string::npos);
  CHECK(out.find("catalog: 0 modules") != std::string::npos);
  CHECK(fs::exists(dest / L"win" / L"import.json"));
  CHECK(fs::exists(dest / L"win" / L"catalog-win.json"));
  for (auto& f : fixture)
    if (!f.out.empty()) CHECK(test::read_bytes(dest / L"win" / to_wide(f.out)) == f.data);

  // --catalog-only rewrites the catalog from what is already imported, and
  // takes nothing that belongs to an import.
  CHECK_EQ(run(exe, {L"--catalog-only", L"--iso", iso.wstring()}, "--catalog-only with a source"), 1);
  CHECK_EQ(run(exe, {L"--catalog-only", L"--gui"}, "--catalog-only --gui"), 1);
  CHECK_EQ(run(exe, {L"--catalog-only", L"--no-verify"}, "--catalog-only --no-verify"), 1);
  CHECK_EQ(run(exe, {L"--catalog-only", L"--dest", (dir / L"never-imported").wstring()}, "--catalog-only, nothing there"),
           2);
  CHECK(!fs::exists(dir / L"never-imported" / L"win" / L"catalog-win.json"));
  test::write_bytes(dest / L"win" / L"catalog-win.json", {'s', 't', 'a', 'l', 'e'});
  CHECK_EQ(run(exe, {L"--catalog-only", L"--dest", dest.wstring()}, "--catalog-only", &out), 0);
  CHECK(out.find("0 modules, 0 controls") != std::string::npos);
  // The generator: adimport 1.3 since the Intermission modules (the catalog's
  // "abi" field and the intermission recipe; 1.2 was the covers).
  CHECK(test::read_text(dest / L"win" / L"catalog-win.json").find("\"generator\": \"adimport 1.3\"") !=
        std::string::npos);
  CHECK_EQ(run(exe, {L"--catalog-only", L"--dest", dest.wstring(), L"--quiet"}, "--catalog-only --quiet", &out), 0);
  CHECK(out.empty());

  // COVERS.md §2.8: the cover commands are modes like --catalog-only: one at a
  // time, each with only its own options; --no-cover-download belongs to
  // imports; --change-cover implies --gui and takes no source. None of these
  // command lines gets as far as a window or the network.
  CHECK_EQ(run(exe, {L"--help"}, "help", &out), 0);
  for (const char* opt : {"--set-cover <id> <picture>", "--clear-cover <id>", "--refresh-covers [<id> | all]",
                          "--no-cover-download", "--change-cover <id>", "--force"})
    CHECK(out.find(opt) != std::string::npos);
  const std::wstring pic = (dir / L"picture.png").wstring();
  CHECK_EQ(run(exe, {L"--set-cover", L"deluxe"}, "--set-cover without a picture"), 1);
  CHECK_EQ(run(exe, {L"--set-cover", L"nosuch", pic}, "--set-cover of an unknown package", &out), 1);
  CHECK(out.find("(see --list-packages)") != std::string::npos);
  CHECK_EQ(run(exe, {L"--set-cover", L"deluxe", pic, L"--clear-cover", L"deluxe"}, "two cover modes"), 1);
  CHECK_EQ(run(exe, {L"--refresh-covers", L"--catalog-only"}, "--refresh-covers + --catalog-only"), 1);
  CHECK_EQ(run(exe, {L"--clear-cover", L"deluxe", L"--remove", L"deluxe"}, "--clear-cover + --remove"), 1);
  CHECK_EQ(run(exe, {L"--list-packages", L"--refresh-covers"}, "--list-packages + --refresh-covers"), 1);
  CHECK_EQ(run(exe, {L"--set-cover", L"deluxe", pic, L"--iso", iso.wstring()}, "--set-cover + a source"), 1);
  CHECK_EQ(run(exe, {L"--set-cover", L"deluxe", pic, L"--gui"}, "--set-cover --gui"), 1);
  CHECK_EQ(run(exe, {L"--set-cover", L"deluxe", pic, L"--no-cover-download"}, "--set-cover --no-cover-download"), 1);
  CHECK_EQ(run(exe, {L"--clear-cover", L"deluxe", L"--download-dir", dir.wstring()}, "--clear-cover --download-dir"),
           1);
  CHECK_EQ(run(exe, {L"--clear-cover", L"nosuch"}, "--clear-cover of an unknown package"), 1);
  CHECK_EQ(run(exe, {L"--refresh-covers", L"--no-verify"}, "--refresh-covers --no-verify"), 1);
  CHECK_EQ(run(exe, {L"--refresh-covers", L"--no-cover-download"}, "--refresh-covers --no-cover-download"), 1);
  CHECK_EQ(run(exe, {L"--refresh-covers", L"--package", L"tt"}, "--refresh-covers --package"), 1);
  CHECK_EQ(run(exe, {L"--refresh-covers", L"nosuch"}, "--refresh-covers of an unknown package"), 1);
  // In a window (the settings dialog's "Get the covers"): no source, no --quiet.
  CHECK_EQ(run(exe, {L"--gui", L"--refresh-covers", L"--quiet"}, "--gui --refresh-covers --quiet"), 1);
  CHECK_EQ(run(exe, {L"--gui", L"--refresh-covers", L"--iso", iso.wstring()}, "--gui --refresh-covers + a source"), 1);
  CHECK_EQ(run(exe, {L"--gui", L"--refresh-covers", L"nosuch"}, "--gui --refresh-covers of an unknown package"), 1);
  CHECK_EQ(run(exe, {L"--force"}, "--force alone"), 1);
  CHECK_EQ(run(exe, {L"--force", L"--iso", iso.wstring(), L"--dest", dest.wstring()}, "--force with an import"), 1);
  CHECK_EQ(run(exe, {L"--no-cover-download"}, "--no-cover-download alone"), 1);
  CHECK_EQ(run(exe, {L"--no-cover-download", L"--catalog-only"}, "--no-cover-download --catalog-only"), 1);
  CHECK_EQ(run(exe, {L"--no-cover-download", L"--list-packages"}, "--no-cover-download --list-packages"), 1);
  CHECK_EQ(run(exe, {L"--no-cover-download", L"--remove", L"tt"}, "--no-cover-download --remove"), 1);
  // In a window (the settings dialog's "Remove …"): no --quiet, no source, a known id.
  {
    std::string help;
    CHECK_EQ(run(exe, {L"--help"}, "help", &help), 0);
    CHECK(help.find("adimport --gui --remove <id>") != std::string::npos);
  }
  CHECK_EQ(run(exe, {L"--gui", L"--remove", L"tt", L"--quiet"}, "--gui --remove --quiet"), 1);
  CHECK_EQ(run(exe, {L"--gui", L"--remove", L"nosuch"}, "--gui --remove of an unknown package"), 1);
  CHECK_EQ(run(exe, {L"--gui", L"--remove", L"tt", L"--iso", iso.wstring()}, "--gui --remove + a source"), 1);
  CHECK_EQ(run(exe, {L"--remove", L"tt", L"--package", L"tt"}, "--remove --package"), 1);
  CHECK_EQ(run(exe, {L"--remove", L"tt", L"--download-dir", dir.wstring()}, "--remove --download-dir"), 1);
  CHECK_EQ(run(exe, {L"--change-cover"}, "--change-cover without an id"), 1);
  CHECK_EQ(run(exe, {L"--change-cover", L"nosuch"}, "--change-cover of an unknown package"), 1);
  CHECK_EQ(run(exe, {L"--change-cover", L"deluxe", L"--iso", iso.wstring()}, "--change-cover + a source"), 1);
  CHECK_EQ(run(exe, {L"--change-cover", L"deluxe", L"--download"}, "--change-cover --download"), 1);
  CHECK_EQ(run(exe, {L"--change-cover", L"deluxe", L"--catalog-only"}, "--change-cover + --catalog-only"), 1);
  CHECK_EQ(run(exe, {L"--change-cover", L"deluxe", L"--set-cover", L"deluxe", pic}, "--change-cover + --set-cover"), 1);
  CHECK_EQ(run(exe, {L"--change-cover", L"deluxe", L"--no-verify"}, "--change-cover --no-verify"), 1);
  CHECK_EQ(run(exe, {L"--change-cover", L"deluxe", L"--package", L"deluxe"}, "--change-cover --package"), 1);
  CHECK_EQ(run(exe, {L"--change-cover", L"deluxe", L"--quiet"}, "--change-cover --quiet"), 1);
  // The cover column, and the cover commands on what the import above left:
  // Deluxe with a generated cover (the synthetic disc has no setup art).
  CHECK_EQ(run(exe, {L"--list-packages", L"--dest", dest.wstring()}, "--list-packages", &out), 0);
  CHECK(out.find("; cover: generated") != std::string::npos);
  // Every release; the title column fits the longest title (Scott Adams'
  // Dilbert Screen Saver Collection's), so every state starts in the same
  // column.
  CHECK(out.find("\n  swse      Star Wars Screen Entertainment               "
                 "not installed; download 6.9 MB (disc image)") != std::string::npos);
  CHECK(out.find("\n  startrek  Star Trek: The Screen Saver                  "
                 "not installed; download 2.8 MB (2 floppy images)") != std::string::npos);
  CHECK(out.find("\n  tt        Totally Twisted After Dark                   not installed;") != std::string::npos);
  CHECK(out.find("\n  marvel    Marvel Comics Screen Posters                 "
                 "not installed; download 1.9 MB (ZIP of the install files)") != std::string::npos);
  CHECK(out.find("\n  snoopy    Snoopy's Screen Savers                       "
                 "not installed; download 1.9 MB (ZIP of the install files)") != std::string::npos);
  CHECK(out.find("\n  disney    The Disney Collection Screen Saver           "
                 "not installed; download 3.4 MB (ZIP of the install files)") != std::string::npos);
  CHECK(out.find("\n  farside   The Far Side Screen Saver Collection         "
                 "not installed; download 5.5 MB (5 ZIPs of the install disks' files)") != std::string::npos);
  CHECK(out.find("\n  dilbert   Scott Adams' Dilbert Screen Saver Collection "
                 "not installed; download 4.3 MB (ZIP of the install files)") != std::string::npos);
  CHECK(out.find("\n  tng       Star Trek: The Next Generation Screen Saver  "
                 "not installed; download 5.8 MB (disc image)") != std::string::npos);
  CHECK(out.find("\n  castaway  Screen Antics: Johnny Castaway               "
                 "not installed; download 1.3 MB (floppy image)") != std::string::npos);
  CHECK_EQ(run(exe, {L"--set-cover", L"tt", pic, L"--dest", dest.wstring()}, "--set-cover, not imported", &out), 1);
  CHECK(out.find("Totally Twisted After Dark isn't imported") != std::string::npos);
  CHECK_EQ(run(exe, {L"--set-cover", L"deluxe", pic, L"--dest", dest.wstring()}, "--set-cover, no picture"), 2);
  CHECK_EQ(run(exe, {L"--clear-cover", L"deluxe", L"--dest", dest.wstring()}, "--clear-cover, nothing set", &out), 0);
  CHECK(out.find("nothing to clear") != std::string::npos);
  CHECK_EQ(run(exe, {L"--refresh-covers", L"--dest", (dir / L"never-imported").wstring()}, "--refresh-covers, nothing",
               &out),
           0);
  CHECK(out.find("nothing is imported") != std::string::npos);
  CHECK(!fs::exists(dir / L"never-imported"));

  fs::path cd = dir / L"cd";
  test::write_fixture_folder(cd, fixture, false);
  // Trailing separator, the shape of a drive root ("E:\").
  CHECK_EQ(run(exe, {L"--no-cover-download", L"--from", cd.wstring() + L"\\", L"--dest", (dir / L"assets2").wstring(),
                     L"--no-verify", L"--quiet"},
               "folder import"),
           0);
  CHECK(test::read_text(dir / L"assets2" / L"win" / L"import.json").find("\"kind\": \"folder\"") != std::string::npos);

  // --download against the loopback server serving the synthetic image.
  test::Server srv(image);
  std::wstring md5 = to_wide(md5_hex(image.data(), image.size()));
  fs::path dl = dir / L"downloads";
  CHECK_EQ(run(exe,
               {L"--no-cover-download", L"--download", L"--url", to_wide(srv.url("/missing.iso")), L"--download-dir",
                dl.wstring(), L"--dest", (dir / L"assets3").wstring()},
               "download 404"),
           4);
  // A malformed --md5 is a usage error up front, not a failed verify after
  // the whole transfer.
  srv.clear();
  CHECK_EQ(run(exe, {L"--download", L"--url", to_wide(srv.url("/file.bin")), L"--md5", L"d875a603",
                     L"--download-dir", dl.wstring(), L"--dest", (dir / L"assets3").wstring()},
               "malformed --md5"),
           1);
  CHECK(srv.requests().empty());
  // A stray '%' in the URL's file name is kept literally (it used to throw
  // out of the name decoding as a generic error); the server's 404 decides.
  CHECK_EQ(run(exe,
               {L"--no-cover-download", L"--download", L"--url", to_wide(srv.url("/missing%zz")), L"--download-dir",
                dl.wstring(), L"--dest", (dir / L"assets3").wstring()},
               "undecodable %-escape"),
           4);
  CHECK_EQ(run(exe,
               {L"--no-cover-download", L"--download", L"--url", to_wide(srv.url("/redirect1")), L"--md5",
                std::wstring(32, L'0'), L"--download-dir", dl.wstring(), L"--dest", (dir / L"assets3").wstring()},
               "download md5 mismatch"),
           3);
  CHECK_EQ(run(exe,
               {L"--no-cover-download", L"--download", L"--url", to_wide(srv.url("/redirect1")), L"--md5", md5,
                L"--download-dir", dl.wstring(), L"--dest", (dir / L"assets3").wstring(), L"--no-verify"},
               "download + import", &out),
           0);
  CHECK(fs::exists(dl / L"redirect1"));  // a custom URL keeps its own file name
  std::string json = test::read_text(dir / L"assets3" / L"win" / L"import.json");
  CHECK(json.find("\"kind\": \"download\"") != std::string::npos);
  CHECK(json.find("\"finalUrl\": \"" + srv.url("/file.bin") + "\"") != std::string::npos);
  // A URL whose file name is a DOS device (NUL.iso: on Windows 10
  // <downloads>\NUL.iso is the null device) is saved as download.iso.
  srv.serve("/NUL.iso", image);
  CHECK_EQ(run(exe,
               {L"--no-cover-download", L"--download", L"--url", to_wide(srv.url("/NUL.iso")), L"--md5", md5,
                L"--download-dir", dl.wstring(), L"--dest", (dir / L"assets4").wstring(), L"--no-verify"},
               "download named after a device"),
           0);
  CHECK(fs::is_regular_file(dl / L"download.iso"));
  CHECK_EQ(md5_file_hex(dl / L"download.iso"), to_utf8(md5));

  // ---- the data folder (importer.h data_folder(), host/core data_root.h) ------------
  // A base of its own (AD_LOCALAPPDATA) and no AD_ASSETS_DIR, so the defaults
  // come from <base>\LongAfterDark. Explicit locations never use it, and a
  // listing creates nothing there.
  {
    const std::wstring assets_env = test::get_env(L"AD_ASSETS_DIR");
    test::set_env(L"AD_ASSETS_DIR", L"");
    const fs::path data = test::sandbox_data_root(fs::path(dir / L"data-folder").make_preferred()) / L"LongAfterDark";
    const size_t npos = std::string::npos;
    CHECK_EQ(run(exe, {L"--list-packages"}, "data folder: the default assets root", &out), 0);
    CHECK(out.find("packages in " + to_utf8((data / L"assets" / L"win").wstring())) != npos);
    CHECK(!fs::exists(data));
    // The cover commands take the downloads folder only when they download,
    // and an import with --dest and no cover download never does.
    CHECK_EQ(run(exe, {L"--list-packages", L"--dest", dest.wstring()}, "data folder: --dest", &out), 0);
    CHECK_EQ(run(exe, {L"--clear-cover", L"deluxe", L"--dest", dest.wstring()}, "data folder: a cover command with --dest",
                 &out),
             0);
    CHECK_EQ(run(exe, {L"--set-cover", L"deluxe", pic, L"--dest", dest.wstring()}, "data folder: --set-cover with --dest",
                 &out),
             2);
    CHECK_EQ(run(exe, {L"--no-cover-download", L"--iso", iso.wstring(), L"--dest", (dir / L"assets-df").wstring(),
                       L"--no-verify"},
                 "data folder: an import with --dest", &out),
             0);
    test::set_env(L"AD_ASSETS_DIR", dest.wstring());
    CHECK_EQ(run(exe, {L"--list-packages"}, "data folder: AD_ASSETS_DIR", &out), 0);
    CHECK(out.find("packages in " + to_utf8(win_assets_dir(dest).wstring())) != npos);
    test::set_env(L"AD_ASSETS_DIR", L"");
    CHECK(!fs::exists(data));
    // A download without --download-dir goes to <data folder>\downloads.
    CHECK_EQ(run(exe,
                 {L"--no-cover-download", L"--download", L"--url", to_wide(srv.url("/file.bin")), L"--md5", md5,
                  L"--dest", (dir / L"assets-df2").wstring(), L"--no-verify"},
                 "data folder: a download into the default folder", &out),
             0);
    CHECK(fs::is_regular_file(data / L"downloads" / L"file.bin"));
    test::sandbox_data_root(lad);
    test::set_env(L"AD_ASSETS_DIR", assets_env);
  }

  // Offline: the GUI runs below import with the built-in registry, whose
  // Deluxe cover is a download (COVERS.md §8.3); their command lines are the
  // GUI's contract, so the environment turns cover downloads off instead.
  SetEnvironmentVariableW(L"AD_COVER_DOWNLOAD", L"0");
  if (const char* g = getenv("AD_GUI_TESTS"); g && *g == '1') {
    SetEnvironmentVariableW(L"AD_GUI_AUTOCLOSE", L"1");
    CHECK_EQ(run(exe, {L"--gui", L"--iso", iso.wstring(), L"--dest", (dir / L"assets-gui").wstring(), L"--no-verify"},
                 "gui import"),
             0);
    CHECK(fs::exists(dir / L"assets-gui" / L"win" / L"import.json"));
    CHECK_EQ(run(exe, {L"--gui", L"--download", L"--url", to_wide(srv.url("/file.bin")), L"--md5", md5,
                       L"--download-dir", (dir / L"downloads-gui").wstring(), L"--dest",
                       (dir / L"assets-gui2").wstring(), L"--no-verify"},
                 "gui download"),
             0);
    // --gui with no source asks for one; Cancel there is exit 5 (cancelled).
    CHECK_EQ(cancel_chooser(exe, {L"--gui", L"--dest", (dir / L"assets-gui3").wstring()}, CREATE_NO_WINDOW,
                            "gui chooser"),
             5);
    // No arguments and no console at all — how Explorer (or the .scr, under
    // the manifest's detached console policy) starts it — is --gui too.
    CHECK_EQ(cancel_chooser(exe, {}, DETACHED_PROCESS, "no console, no arguments"), 5);
    CHECK(!fs::exists(dir / L"assets-gui3"));
    // "Download from the Internet Archive" opens the list of releases; Cancel
    // there goes back to the sources, where Cancel is exit 5. Nothing is
    // fetched or written (the list only reads the assets and downloads).
    CHECK_EQ(click_through(exe, {L"--gui", L"--dest", (dir / L"assets-gui4").wstring()}, CREATE_NO_WINDOW,
                           {103, IDCANCEL, IDCANCEL}, "gui download list, back, cancel"),
             5);
    CHECK_EQ(click_through(exe, {L"--gui", L"--package", L"tt", L"--dest", (dir / L"assets-gui4").wstring()},
                           CREATE_NO_WINDOW, {103, IDCANCEL, IDCANCEL}, "gui download list of one release"),
             5);
    CHECK(!fs::exists(dir / L"assets-gui4"));
  } else {
    fprintf(stderr, "(GUI runs skipped; set AD_GUI_TESTS=1 on an interactive desktop)\n");
  }
  return test::finish("import.cli");
}
