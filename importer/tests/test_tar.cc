// Tar members (tar.h), on made-up tars (tests/tar_builder.h):
//   find      members by name, in the order asked, a ustar prefix joined to
//             the name with '/', V7 headers, a name not there; directories,
//             links and GNU/pax extension entries are no members; the walk
//             stops once every name is found (a damaged header after them is
//             never reached) and at the two zero blocks
//   read      a member's bytes exactly, over more than one 1 MiB chunk
//   damage    a header's checksum, a size that is no octal number or runs
//             past the file, a file cut inside a member
//
//   test_import_tar <scratch>
#include <functional>

#include "tar.h"
#include "tar_builder.h"
#include "test_util.h"

using namespace adw::import;
namespace fs = std::filesystem;

namespace {

std::vector<uint8_t> read(const fs::path& tar, const TarEntry& e) {
  std::vector<uint8_t> out;
  read_tar_entry(tar, e, [&](const uint8_t* p, size_t n) { out.insert(out.end(), p, p + n); }, "t.tar");
  return out;
}

// Made-up bytes: the tag, then a pattern of its own.
std::vector<uint8_t> blob(const std::string& tag, size_t n) {
  std::vector<uint8_t> v(tag.begin(), tag.end());
  const auto p = test::pattern(n, uint32_t(tag.size() * 7919 + n));
  v.insert(v.end(), p.begin(), p.end());
  return v;
}

bool fails(const char* what, const std::function<void()>& f, const std::string& want) {
  try {
    f();
    fprintf(stderr, "  %s: accepted, should have failed\n", what);
    return false;
  } catch (const TarError& e) {
    const bool ok = std::string(e.what()).find(want) != std::string::npos;
    fprintf(stderr, "  %s -> %s\n", what, e.what());
    if (!ok) fprintf(stderr, "    expected the message to hold \"%s\"\n", want.c_str());
    return ok;
  }
}

}  // namespace

int main(int argc, char** argv) {
  const fs::path dir = test::scratch(argc, argv, "adw-import-tar");
  test::sandbox_data_root(dir / L"localappdata");  // no default may reach the real data folder

  const std::vector<uint8_t> one = blob("a made-up disk one", 1253), two = blob("disk two", 512),
                             big = test::pattern((3u << 20) + 77, 5);
  // ---- find and read --------------------------------------------------------------------------
  {
    test::TarBuilder b;
    b.add("collection/", {}, '5');
    b.add("collection/NOTES.TXT", blob("a note", 90));
    b.add("././@LongLink", blob("a long name", 40), 'L');
    b.add("FLINTST1.ZIP", one, '0', "collection");
    b.add("collection/LINK.ZIP", {}, '2');
    b.add("collection/FLINTST2.ZIP", two);
    b.add("collection/BIG.BIN", big);
    test::write_bytes(dir / L"t.tar", b.build());
    auto got = find_tar_entries(dir / L"t.tar",
                                {"collection/BIG.BIN", "collection/FLINTST1.ZIP", "collection/FLINTST2.ZIP",
                                 "collection/NOSUCH.ZIP", "collection/LINK.ZIP", "collection"},
                                "t.tar");
    CHECK_EQ(got.size(), size_t(6));
    if (got.size() == 6) {
      CHECK(got[0] && got[0]->size == big.size() && read(dir / L"t.tar", *got[0]) == big);
      CHECK(got[1] && got[1]->name == "collection/FLINTST1.ZIP" && read(dir / L"t.tar", *got[1]) == one);
      CHECK(got[2] && read(dir / L"t.tar", *got[2]) == two);
      CHECK(!got[3] && !got[4] && !got[5]);  // not there; a link; a directory
    }
    // V7 headers (no magic, no prefix).
    test::TarBuilder v7;
    v7.ustar = false;
    v7.add("A.ZIP", one);
    test::write_bytes(dir / L"v7.tar", v7.build());
    auto a = find_tar_entries(dir / L"v7.tar", {"A.ZIP"}, "v7.tar");
    CHECK(a.size() == 1 && a[0] && read(dir / L"v7.tar", *a[0]) == one);
  }
  // ---- the walk stops once every name is found, and at the end of the archive -----------------------
  {
    test::TarBuilder b;
    b.add("FIRST.ZIP", one);
    auto bytes = b.build();
    bytes.resize(bytes.size() - 1024);  // no end blocks yet
    auto bad = test::TarBuilder::header({"LATER.ZIP", "", two, '0'}, true);
    bad[148] = '9';  // a checksum that is no octal number
    bytes.insert(bytes.end(), bad.begin(), bad.end());
    bytes.insert(bytes.end(), two.begin(), two.end());
    bytes.resize((bytes.size() + 511) / 512 * 512 + 1024, 0);
    test::write_bytes(dir / L"stop.tar", bytes);
    auto first = find_tar_entries(dir / L"stop.tar", {"FIRST.ZIP"}, "stop.tar");
    CHECK(first.size() == 1 && first[0] && read(dir / L"stop.tar", *first[0]) == one);
    CHECK(fails("a damaged header reached", [&] { find_tar_entries(dir / L"stop.tar", {"LATER.ZIP"}, "stop.tar"); },
                "stop.tar: a header at byte 2048 fails its checksum"));
    // After the two zero blocks: no more members.
    test::TarBuilder e;
    e.add("A.ZIP", one);
    auto ended = e.build();
    test::TarBuilder f;
    f.add("AFTER.ZIP", two);
    const auto more = f.build();
    ended.insert(ended.end(), more.begin(), more.end());
    test::write_bytes(dir / L"ended.tar", ended);
    auto after = find_tar_entries(dir / L"ended.tar", {"AFTER.ZIP", "A.ZIP"}, "ended.tar");
    CHECK(after.size() == 2 && !after[0] && after[1]);
  }
  // ---- damage ---------------------------------------------------------------------------------------------
  {
    test::TarBuilder b;
    b.add("A.ZIP", one);
    const auto good = b.build();
    auto sum = good;
    sum[0] ^= 1;  // the name changed: the checksum no longer holds
    test::write_bytes(dir / L"sum.tar", sum);
    CHECK(fails("a checksum", [&] { find_tar_entries(dir / L"sum.tar", {"A.ZIP"}, "sum.tar"); },
                "sum.tar: a header at byte 0 fails its checksum (not a tar, or a damaged one)"));
    // A size past the end of the file, its checksum right.
    test::TarBuilder big_size;
    big_size.add("A.ZIP", one);
    auto past = big_size.build();
    past.resize(512 + 512);  // the header and half the member
    test::write_bytes(dir / L"past.tar", past);
    CHECK(fails("a member past the end", [&] { find_tar_entries(dir / L"past.tar", {"A.ZIP"}, "past.tar"); },
                "runs past its end"));
    // A size that is no octal number (the checksum made to hold).
    auto h = test::TarBuilder::header({"A.ZIP", "", one, '0'}, true);
    h[124] = 'x';
    memset(h.data() + 148, ' ', 8);
    unsigned s = 0;
    for (uint8_t c : h) s += c;
    char buf[16];
    snprintf(buf, sizeof(buf), "%06o", s);
    memcpy(h.data() + 148, buf, 6);
    h[154] = 0;
    std::vector<uint8_t> nosize = h;
    nosize.resize(4096, 0);
    test::write_bytes(dir / L"nosize.tar", nosize);
    CHECK(fails("a size that is no number", [&] { find_tar_entries(dir / L"nosize.tar", {"A.ZIP"}, "nosize.tar"); },
                "has no size"));
    // Not a tar at all.
    test::write_bytes(dir / L"text.tar", blob("just text", 4000));
    CHECK(fails("not a tar", [&] { find_tar_entries(dir / L"text.tar", {"A.ZIP"}, "text.tar"); },
                "fails its checksum"));
    // A file cut inside a member found earlier.
    auto found = find_tar_entries(dir / L"past.tar", {}, "past.tar");
    CHECK(found.empty());
    TarEntry cut{"A.ZIP", 512, one.size()};
    CHECK(fails("a cut member", [&] { read(dir / L"past.tar", cut); }, "ends inside an entry"));
  }
  return test::finish("import.tar");
}
