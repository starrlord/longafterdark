// The registry's real box covers (opt-in: AD_E2E=1; exit 77 = skipped).
// COVERS.md §2.14.
//
//   test_import_covers_real <adimport.exe> <scratch> <image dir> <covers-sheet.png>
//
//   1. Every registry cover download (archive.org, Wikisimpsons, the Wayback
//      Machine) is fetched into a scratch downloads folder, checked against
//      its md5 and size, decoded, cropped and rendered as a tile.
//   2. With AD_E2E_PKG=1, the real package images (found by size and md5 in
//      AD_SOURCE_ISO_DIR, a ';'-separated list, else <image dir>, and the
//      folders directly in each; the Deluxe ISO also in the downloads folder,
//      which is only read) are imported into a scratch root with
//      --no-cover-download, so every disc source is extracted: the originals
//      must come out 387x183 (ad32), 387x172 (simpsons, disney), 387x204
//      (tt), 387x161 (looney), 350x119 (screams: a bitmap resource of
//      SETUP.EXE), 79x175 (snoopy: the picture beside its installer's readme),
//      63x123 (dilbert: its installer's picture) and 118x226 (ad10, deluxe).
//      A release with no cover source on its disc (Star Wars Screen
//      Entertainment, Star Trek: The Screen Saver, Marvel Comics Screen
//      Posters and The Far Side: every picture is inside their archives,
//      compressed files or libraries) is skipped.
//   3. Every tile is drawn side by side into covers-sheet.png, for a person
//      to look at.
// Nothing is written outside <scratch> and the sheet; the scratch tree is
// deleted afterwards unless AD_E2E_KEEP=1.
#include <objbase.h>

#include <phosg/JSON.hh>

#include <map>

#include "cover_image.h"
#include "download.h"
#include "importer.h"
#include "md5.h"
#include "run_process.h"
#include "test_util.h"

using namespace adw::import;
using cover::Picture;
namespace fs = std::filesystem;

namespace {

bool env_on(const char* name) {
  const char* v = getenv(name);
  return v && *v && std::string(v) != "0";
}

struct Tile {
  std::string name;
  Picture tile;
};

Picture sheet(const std::vector<Tile>& tiles) {
  const int cols = std::min<int>(6, int(tiles.size())), rows = int((tiles.size() + 5) / 6);
  const int tw = cover::kTileW / 2, th = cover::kTileH / 2, gap = 16;
  Picture s = Picture::filled(gap + cols * (tw + gap), gap + rows * (th + gap), 0x30, 0x30, 0x30);
  for (size_t i = 0; i < tiles.size(); i++) {
    Picture small = cover::resize_cubic(tiles[i].tile, tw, th);
    int x0 = gap + int(i % 6) * (tw + gap), y0 = gap + int(i / 6) * (th + gap);
    for (int y = 0; y < th; y++) memcpy(s.px(x0, y0 + y), small.px(0, y), size_t(tw) * 4);
  }
  return s;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 5) {
    fprintf(stderr, "usage: test_import_covers_real <adimport.exe> <scratch> <image dir> <covers-sheet.png>\n");
    return 2;
  }
  if (!env_on("AD_E2E")) {
    fprintf(stderr, "import.covers_real: skipped (set AD_E2E=1; it downloads every registry cover)\n");
    return 77;
  }
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  std::wstring exe = fs::absolute(argv[1]).wstring();
  // The installed data folder (read only: its downloads may hold the images),
  // found before the sandbox hides it.
  const fs::path installed_data = test::installed_data_root();
  fs::path dir = test::scratch(2, argv + 1, "adw-import-covers-real");
  test::sandbox_data_root(dir / L"localappdata");  // no default may reach the real data folder
  fs::path downloads = dir / L"downloads";
  std::vector<Tile> tiles;
  std::vector<std::string> notes;

  // 1. Every download source.
  for (const Package& p : builtin_packages())
    for (const CoverSource& s : p.covers) {
      if (s.kind != CoverSource::Kind::download) continue;
      DownloadOptions d;
      d.url = s.url;
      d.dest = downloads / L"covers" / s.file_name;
      d.expected_md5 = s.md5;
      d.expected_size = s.size;
      d.max_attempts = 3;
      d.timeout_ms = 30000;
      d.log = [](const std::string& m) { fprintf(stderr, "  %s\n", m.c_str()); };
      std::string what = std::string(p.id) + ": " + s.url;
      try {
        DownloadResult r = download(d);
        CHECK_EQ(r.md5, std::string(s.md5));
        CHECK_EQ(r.size, s.size);
        Picture pic = cover::normalize(cover::read_picture_file(d.dest), s.crop);
        Picture tile = cover::render_tile(pic, s.art);
        CHECK_EQ(tile.w, 640);
        fprintf(stderr, "ok   %s -> %dx%d (%s)\n", what.c_str(), pic.w, pic.h, s.art);
        if (s.crop.w) {
          CHECK_EQ(pic.w, s.crop.w);
          CHECK_EQ(pic.h, s.crop.h);
        }
        tiles.push_back({std::string(p.id) + " " + s.label + " (" + s.credit + ")", std::move(tile)});
      } catch (const std::exception& e) {
        test::g_failures++;
        fprintf(stderr, "FAIL %s: %s\n", what.c_str(), e.what());
      }
    }

  // 2. The disc sources, through real imports.
  if (env_on("AD_E2E_PKG")) {
    std::vector<fs::path> dirs = test::image_dirs(argv[3]);
    if (!installed_data.empty()) dirs.push_back(installed_data / L"downloads");
    const std::map<std::string, std::pair<int, int>> want = {
        {"deluxe", {118, 226}},  {"ad10", {118, 226}},    {"ad32", {387, 183}},  {"tt", {387, 204}},
        {"simpsons", {387, 172}}, {"looney", {387, 161}}, {"screams", {350, 119}}, {"disney", {387, 172}},
        {"snoopy", {79, 175}},    {"dilbert", {63, 123}}};
    fs::path root = dir / L"root";
    for (const Package& p : builtin_packages()) {
      if (p.images.empty()) continue;
      if (std::none_of(p.covers.begin(), p.covers.end(),
                       [](const CoverSource& c) { return c.kind == CoverSource::Kind::disc; })) {
        fprintf(stderr, "skip %s: no cover source on its disc\n", p.id);
        continue;
      }
      const fs::path image = test::find_image(dirs, p.images[0].size, p.images[0].md5);
      if (image.empty()) {
        fprintf(stderr, "skip %s: no image with md5 %s\n", p.id, p.images[0].md5);
        notes.push_back(std::string("no image of ") + p.id);
        continue;
      }
      fprintf(stderr, "---- %s from %s\n", p.id, to_utf8(image.wstring()).c_str());
      test::ProcessResult pr = test::run_process(
          exe, {L"--no-cover-download", L"--image", image.wstring(), L"--dest", root.wstring(), L"--quiet"}, 1800000);
      fprintf(stderr, "%s", pr.output.c_str());
      CHECK_EQ(pr.exit_code, 0);
      fs::path cj = root / L"win" / L"covers" / to_wide(p.id) / L"cover.json";
      if (!fs::exists(cj)) {
        test::g_failures++;
        fprintf(stderr, "FAIL %s: no cover.json\n", p.id);
        continue;
      }
      phosg::JSON j = phosg::JSON::parse(test::read_text(cj));
      CHECK_EQ(j.at("original").get_string("origin"), std::string("disc"));
      auto [w, h] = want.at(p.id);
      CHECK_EQ(j.at("original").get_int("width"), int64_t(w));
      CHECK_EQ(j.at("original").get_int("height"), int64_t(h));
      fprintf(stderr, "ok   %s disc art %lldx%lld from %s\n", p.id, (long long)j.at("original").get_int("width"),
              (long long)j.at("original").get_int("height"), j.at("original").get_string("path").c_str());
      Picture tile = cover::decode_picture(test::read_bytes(root / L"win" / L"covers" / to_wide(p.id) / L"tile.png"));
      tiles.push_back({std::string(p.id) + " disc", std::move(tile)});
    }
  } else {
    fprintf(stderr, "(disc sources skipped; set AD_E2E_PKG=1 to import the real images)\n");
  }

  // 3. The sheet.
  if (!tiles.empty()) {
    std::vector<uint8_t> png = cover::encode_png(sheet(tiles), false);
    test::write_bytes(fs::absolute(argv[4]), png);
    fprintf(stderr, "wrote %s (%zu tiles):\n", argv[4], tiles.size());
    for (size_t i = 0; i < tiles.size(); i++) fprintf(stderr, "  %zu. %s\n", i + 1, tiles[i].name.c_str());
  }
  for (const std::string& n : notes) fprintf(stderr, "note: %s\n", n.c_str());
  if (!env_on("AD_E2E_KEEP")) remove_tree(dir);
  CoUninitialize();
  return test::finish("import.covers_real");
}
