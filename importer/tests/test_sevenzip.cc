// 7z archives (sevenzip.h, PACKAGES.md §8.10): 7-Zip's own archives of
// made-up files (tests/sevenzip_vectors.h: LZMA with and without the end
// marker, LZMA2 in several chunks, in blocks that reset the dictionary and
// as an uncompressed chunk, Copy; the header plain and packed; solid and
// not; folders and an empty file), every
// file against its md5 and CRC-32, in any order; the refusals (7zAES on the
// data and on the header, BCJ, Delta, PPMd, BZip2, the first volume of a
// split archive); every truncation and every single-bit flip of two small
// ones; archives the test writes (tests/sevenzip_builder.h: stored blocks,
// hostile and duplicate names, the caps, unknown methods); and 7z sources:
// install files flat and in DISK<n> folders, any other folder refused, and
// floppy images (floppy_images_in_zip), in a folder, solid or not.
//
//   test_import_sevenzip <scratch>
//   test_import_sevenzip real <scratch> <source_iso>
//                (opt-in, AD_E2E_PKG=1) the user's 7z of Screen Antics:
//                Johnny Castaway's floppy image, found by size and md5 in
//                AD_SOURCE_ISO_DIR or <source_iso>: its one image, decoded,
//                against the image's md5, then read as a floppy.
#include <cstring>
#include <memory>
#include <set>

#include "fat_builder.h"
#include "names.h"
#include "sevenzip.h"
#include "sevenzip_builder.h"
#include "sevenzip_vectors.h"
#include "test_util.h"

using namespace adw::import;
namespace fs = std::filesystem;

namespace {

using Bytes = std::vector<uint8_t>;

std::shared_ptr<const Bytes> own(Bytes v) { return std::make_shared<const Bytes>(std::move(v)); }

Bytes text(const char* s) { return Bytes(s, s + strlen(s)); }

Bytes extract_all(const SevenZipArchive& a, const SevenZipMember& m) {
  Bytes v;
  a.extract(m, [&](const uint8_t* p, size_t n) { v.insert(v.end(), p, p + n); });
  return v;
}

std::string md5_of(const Bytes& v) { return md5_hex(v.data(), v.size()); }

bool refused(const Bytes& archive, const char* what, const char* expect) {
  try {
    SevenZipArchive a(own(archive), "T.7z");
    fprintf(stderr, "  %s: accepted, should have been refused\n", what);
    return false;
  } catch (const SevenZipError& e) {
    fprintf(stderr, "  %s -> %s\n", what, e.what());
    if (!strstr(e.what(), expect)) {
      fprintf(stderr, "  %s: expected \"%s\"\n", what, expect);
      return false;
    }
    return true;
  }
}

template <size_t N>
Bytes bytes_of(const uint8_t (&a)[N]) {
  return Bytes(a, a + N);
}

struct Vector {
  const char* name;
  Bytes bytes;
  const char* set;
  const char* method;
  bool solid;
};

// 2026-01-01 00:00:00 UTC as a FILETIME: the made-up files' time.
constexpr uint64_t kFileTime = (1767225600ull + 11644473600ull) * 10000000ull;

// Every file of `set` out of `a`, against the generator's md5s and CRCs, in
// archive order and then backwards (a solid block decodes once, and serves
// any order). The number of files checked.
size_t check_set(const SevenZipArchive& a, const char* set, const char* method, bool solid) {
  size_t files = 0;
  for (int pass = 0; pass < 2; pass++) {
    std::vector<const test::SevenZipVectorFile*> want;
    for (const test::SevenZipVectorFile& f : test::kSevenZipFiles)
      if (!strcmp(f.set, set)) want.push_back(&f);
    if (pass) std::reverse(want.begin(), want.end());
    for (const test::SevenZipVectorFile* f : want) {
      const SevenZipMember* m = a.find(f->name);
      CHECK(m != nullptr);
      if (!m) continue;
      CHECK(!m->directory);
      CHECK_EQ(m->size, uint64_t(f->size));
      CHECK(m->crc && *m->crc == f->crc);
      CHECK(m->mtime && *m->mtime == kFileTime);
      CHECK_EQ(md5_of(extract_all(a, *m)), std::string(f->md5));
      if (f->size) {
        CHECK_EQ(a.method(*m), std::string(method));
        CHECK_EQ(a.solid(*m), solid);
      }
      if (!pass) files++;
    }
  }
  return files;
}

std::string expected_md5(const char* set, const std::string& name) {
  for (const test::SevenZipVectorFile& f : test::kSevenZipFiles)
    if (!strcmp(f.set, set) && name == f.name) return f.md5;
  return "";
}

// Every single-bit flip of a small archive: refused, or every file still
// its own bytes (each file's and block's CRC-32, the header's, the start
// header's). Only the format's minor version byte (offset 7) is free.
void flip_every_bit(const Vector& v) {
  size_t refused_open = 0, refused_read = 0, silent = 0;
  for (size_t i = 0; i < v.bytes.size(); i++) {
    for (int bit = 0; bit < 8; bit++) {
      Bytes b = v.bytes;
      b[i] ^= uint8_t(1 << bit);
      std::unique_ptr<SevenZipArchive> a;
      try {
        a = std::make_unique<SevenZipArchive>(own(b), "F.7z");
      } catch (const SevenZipError&) {
        refused_open++;
        continue;
      }
      bool failed = false;
      for (const SevenZipMember& m : a->members()) {
        if (m.directory) continue;
        try {
          const Bytes got = extract_all(*a, m);
          if (md5_of(got) != expected_md5(v.set, m.name)) {
            fprintf(stderr, "  %s: flipping bit %d of byte %zu changed %s unnoticed\n", v.name, bit, i, m.name.c_str());
            test::g_failures++;
          }
        } catch (const SevenZipError&) {
          failed = true;
        }
      }
      if (failed) {
        refused_read++;
      } else if (i != 7) {
        fprintf(stderr, "  %s: flipping bit %d of byte %zu went unnoticed\n", v.name, bit, i);
        silent++;
      }
    }
  }
  fprintf(stderr, "  %s: %zu flips refused at open, %zu when read, %zu unnoticed\n", v.name, refused_open,
          refused_read, silent);
  CHECK_EQ(silent, size_t(0));
}

int real_test(int argc, char** argv);

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 && std::string(argv[1]) == "real") return real_test(argc - 1, argv + 1);
  // No default may reach the real data folder.
  const fs::path dir = test::scratch(argc, argv, "adw-import-sevenzip");
  test::sandbox_data_root(dir / L"localappdata");

  const std::vector<Vector> vectors = {
      {"kLzmaFlat", bytes_of(test::kLzmaFlat), "flat", "LZMA", false},
      {"kLzmaFlatSolid", bytes_of(test::kLzmaFlatSolid), "flat", "LZMA", true},
      {"kLzma2Disks", bytes_of(test::kLzma2Disks), "disks", "LZMA2", true},
      {"kLzma2DisksSplit", bytes_of(test::kLzma2DisksSplit), "disks", "LZMA2", false},
      {"kLzma2Long", bytes_of(test::kLzma2Long), "long", "LZMA2", true},
      {"kLzmaLong", bytes_of(test::kLzmaLong), "long", "LZMA", false},
      {"kLzma2Random", bytes_of(test::kLzma2Random), "random", "LZMA2", false},
      {"kLzma2Blocks", bytes_of(test::kLzma2Blocks), "blocks", "LZMA2", false},  // dictionary resets mid-stream
      {"kCopyFlat", bytes_of(test::kCopyFlat), "flat", "Copy", false},
      {"kCopyDisksEncodedHeader", bytes_of(test::kCopyDisksEncodedHeader), "disks", "Copy", false},  // 7-Zip stores each file alone
  };

  // ---- 7-Zip's archives ----------------------------------------------------------------
  for (const Vector& v : vectors) {
    fprintf(stderr, "  %s\n", v.name);
    try {
      SevenZipArchive a(own(v.bytes), std::string(v.name) + ".7z");
      const size_t files = check_set(a, v.set, v.method, v.solid);
      size_t dirs = 0;
      for (const SevenZipMember& m : a.members()) {
        if (!m.directory) continue;
        dirs++;
        CHECK(m.size == 0 && m.block == SIZE_MAX && (m.attributes & 0x10));
        bool threw = false;
        try {
          extract_all(a, m);
        } catch (const SevenZipError&) {
          threw = true;
        }
        CHECK(threw);
      }
      CHECK_EQ(dirs, size_t(strcmp(v.set, "disks") == 0 ? 2 : 0));
      CHECK_EQ(a.members().size(), files + dirs);
    } catch (const SevenZipError& e) {
      test::g_failures++;
      fprintf(stderr, "  %s: %s\n", v.name, e.what());
    }
  }
  {
    // Lookups: any case, '\' or '/'; the folders' own entries; no trailing '/'.
    SevenZipArchive a(own(bytes_of(test::kLzma2Disks)), "D.7z");
    CHECK(a.find("disk1\\readme.txt") && a.find("disk1\\readme.txt")->name == "DISK1/README.TXT");
    CHECK(a.find("DISK2") && a.find("DISK2")->directory);
    CHECK(!a.find("DISK2/") && !a.find("README.TXT"));
    const SevenZipMember* empty = a.find("DISK2/EMPTY.TXT");
    CHECK(empty && !empty->directory && empty->size == 0 && extract_all(a, *empty).empty());
    // Streaming: chunks of at most 64 KiB, in order, over several LZMA2 chunks.
    SevenZipArchive l(own(bytes_of(test::kLzma2Long)), "L.7z");
    size_t chunks = 0, biggest = 0;
    Bytes joined;
    l.extract(*l.find("LONG.TXT"), [&](const uint8_t* p, size_t n) {
      chunks++;
      biggest = std::max(biggest, n);
      joined.insert(joined.end(), p, p + n);
    });
    CHECK(chunks >= 36 && biggest == 65536);
    CHECK_EQ(md5_of(joined), expected_md5("long", "LONG.TXT"));
    // A sink that stops early (floppy_images_in_zip's boot-sector check):
    // the next read of the block goes on from what was decoded.
    struct Stop {};
    try {
      l.extract(*l.find("README.TXT"), [&](const uint8_t*, size_t) { throw Stop{}; });
    } catch (const Stop&) {
    }
    CHECK_EQ(md5_of(extract_all(l, *l.find("README.TXT"))), expected_md5("long", "README.TXT"));
    // A sink that reads the archive again, the same block and another.
    SevenZipArchive s(own(bytes_of(test::kLzma2DisksSplit)), "S.7z");
    Bytes inner;
    Bytes outer;
    s.extract(*s.find("DISK1/NOTES.TXT"), [&](const uint8_t* p, size_t n) {
      outer.insert(outer.end(), p, p + n);
      inner = extract_all(s, *s.find("DISK2/RANDOM.BIN"));
    });
    CHECK_EQ(md5_of(outer), expected_md5("disks", "DISK1/NOTES.TXT"));
    CHECK_EQ(md5_of(inner), expected_md5("disks", "DISK2/RANDOM.BIN"));
  }

  // ---- what is refused ----------------------------------------------------------------
  CHECK(refused(bytes_of(test::kAes), "7zAES on the data", "encrypted (7zAES)"));
  CHECK(refused(bytes_of(test::kAesHeader), "7zAES on the header too", "encrypted (7zAES)"));
  CHECK(refused(bytes_of(test::kBcj), "BCJ + LZMA", "a block packed with BCJ (x86) + LZMA is not supported"));
  CHECK(refused(bytes_of(test::kDelta), "Delta", "the method Delta is not supported"));
  CHECK(refused(bytes_of(test::kPpmd), "PPMd", "the method PPMd is not supported"));
  CHECK(refused(bytes_of(test::kBzip2), "BZip2", "the method BZip2 is not supported"));
  CHECK(refused(bytes_of(test::kSplitFirstVolume), "the first volume of three", "first volume of a split archive"));
  CHECK(refused(text("PK\3\4 not a 7z"), "a ZIP", "not a 7z archive"));

  // ---- damage --------------------------------------------------------------------------
  {
    const Bytes good = bytes_of(test::kLzmaFlat);
    // Every truncation.
    size_t cut_refused = 0;
    for (size_t n = 0; n < good.size(); n++) {
      try {
        SevenZipArchive a(own(Bytes(good.begin(), good.begin() + ptrdiff_t(n))), "C.7z");
      } catch (const SevenZipError&) {
        cut_refused++;
      }
    }
    CHECK_EQ(cut_refused, good.size());
    CHECK(refused(Bytes(good.begin(), good.end() - 1), "one byte short", "truncated"));
    // The start header's CRC, the header's, a file's data.
    Bytes b = good;
    b[13] ^= 1;
    CHECK(refused(b, "next header offset", "damaged signature header (CRC-32 mismatch)"));
    b = good;
    b[b.size() - 3] ^= 0x10;
    CHECK(refused(b, "the header", "damaged header (CRC-32 mismatch)"));
    b = good;
    Bytes tail = b;
    tail.push_back(0);
    CHECK(refused(tail, "a byte after the header", "data after the end of the archive"));
    b[40] ^= 0x01;
    SevenZipArchive a(own(b), "B.7z");
    size_t bad = 0;
    for (const SevenZipMember& m : a.members()) {
      try {
        extract_all(a, m);
      } catch (const SevenZipError& e) {
        bad++;
        fprintf(stderr, "  a flipped data bit -> %s\n", e.what());
      }
    }
    CHECK_EQ(bad, size_t(1));
    flip_every_bit(vectors[0]);  // LZMA, plain header, a block per file
    flip_every_bit(vectors[2]);  // LZMA2, packed header, one solid block
  }

  // ---- archives written here (stored blocks) -------------------------------------------
  for (bool solid : {false, true}) {
    test::SevenZipBuilder b;
    b.solid = solid;
    b.add("A.TXT", text("first file"));
    b.add_dir("SUB");
    b.add("SUB\\B.TXT", test::pattern(70000, 1));  // 7-Zip's own separator on Windows
    b.add("EMPTY.TXT", {});
    b.add("CAF\xC3\x89.TXT", text("non-ASCII"));          // U+00C9
    b.add("NOTE\xF0\x9F\x8C\x99.TXT", text("astral"));    // U+1F319, a surrogate pair
    SevenZipArchive a(own(b.build()), "W.7z");
    CHECK_EQ(a.members().size(), size_t(6));
    CHECK(a.find("SUB/B.TXT") && extract_all(a, *a.find("SUB/B.TXT")) == test::pattern(70000, 1));
    CHECK(a.find("A.TXT") && extract_all(a, *a.find("A.TXT")) == text("first file"));
    CHECK(a.find("CAF\xC3\x89.TXT") && extract_all(a, *a.find("CAF\xC3\x89.TXT")) == text("non-ASCII"));
    CHECK(a.find("NOTE\xF0\x9F\x8C\x99.TXT") != nullptr);
    CHECK(a.find("SUB") && a.find("SUB")->directory);
    CHECK(a.find("A.TXT") && a.method(*a.find("A.TXT")) == "Copy" && a.solid(*a.find("A.TXT")) == solid);
    CHECK(a.find("A.TXT") && !a.find("A.TXT")->mtime);
  }
  {
    auto one = [](const std::string& name) {
      test::SevenZipBuilder b;
      b.add(name, text("x"));
      return b.build();
    };
    for (const char* bad : {"..", "../X.TXT", "A/../B.TXT", "C:X.TXT", "CON", "NUL.TXT", "/X.TXT", "A//B.TXT",
                            "X.", "X ", "A/", "TAB\tNAME"})
      CHECK(refused(one(bad), bad, "unusable file name"));
    test::SevenZipBuilder dup;
    dup.add("a.txt", text("1"));
    dup.add("A.TXT", text("2"));
    CHECK(refused(dup.build(), "two names, one file", "two entries are named A.TXT"));
    test::SevenZipBuilder dup2;
    dup2.add("CAF\xC3\xA9.TXT", text("1"));  // e-acute and E-acute: one file to Windows
    dup2.add("CAF\xC3\x89.TXT", text("2"));
    CHECK(refused(dup2.build(), "non-ASCII case", "two entries are named"));
    // The caps, and methods by id.
    test::SevenZipBuilder big;
    big.coder = {0x03, 0x01, 0x01};
    big.props = {0x5D, 0, 0, 1, 0};
    big.declared_block_size = SevenZipArchive::kMaxBlockBytes + 1;
    big.add("BIG.BIN", text("not really"));
    CHECK(refused(big.build(), "a block over the cap", "is more than this reader holds (256 MB)"));
    test::SevenZipBuilder aes;
    aes.coder = {0x06, 0xF1, 0x07, 0x01};
    aes.add("X.TXT", text("x"));
    CHECK(refused(aes.build(), "7zAES alone", "encrypted (7zAES)"));
    test::SevenZipBuilder zstd;
    zstd.coder = {0x04, 0xF7, 0x11, 0x01};
    zstd.add("X.TXT", text("x"));
    CHECK(refused(zstd.build(), "an unknown method", "the method method 04F71101 is not supported"));
    test::SevenZipBuilder lzma_props;
    lzma_props.coder = {0x03, 0x01, 0x01};
    lzma_props.props = {225, 0, 0, 1, 0};
    lzma_props.add("X.TXT", text("x"));
    CHECK(refused(lzma_props.build(), "lc/lp/pb out of range", "LZMA with bad properties"));
    test::SevenZipBuilder sizes;
    sizes.declared_block_size = 3;
    sizes.add("X.TXT", text("four"));
    CHECK(refused(sizes.build(), "a stored block of another size", "a stored block whose sizes differ"));
    test::SevenZipBuilder after;
    after.add("X.TXT", text("x"));
    after.trailing = {0};
    CHECK(refused(after.build(), "data after the header", "data after the end of the archive"));
    // A damaged stored file: its CRC-32.
    test::SevenZipBuilder crc;
    crc.add("X.TXT", text("abcd"));
    Bytes c = crc.build();
    c[32] ^= 1;
    SevenZipArchive ca(own(c), "C.7z");
    bool crc_threw = false;
    try {
      extract_all(ca, ca.members().front());
    } catch (const SevenZipError& e) {
      crc_threw = strstr(e.what(), "C.7z!X.TXT: CRC-32 mismatch") != nullptr;
      fprintf(stderr, "  a damaged stored file -> %s\n", e.what());
    }
    CHECK(crc_threw);
    // Empty archives: 7-Zip's (the signature header alone), and a header
    // with nothing in it.
    Bytes empty(32);
    const uint8_t sig[8] = {'7', 'z', 0xBC, 0xAF, 0x27, 0x1C, 0, 4};
    std::copy(sig, sig + 8, empty.begin());
    const uint32_t zcrc = uint32_t(crc32(crc32(0, nullptr, 0), empty.data() + 12, 20));
    for (int i = 0; i < 4; i++) empty[8 + i] = uint8_t(zcrc >> (8 * i));
    CHECK(SevenZipArchive(own(empty), "E.7z").members().empty());
    CHECK(SevenZipArchive(own(test::SevenZipBuilder().build()), "E2.7z").members().empty());
  }

  // ---- 7z sources ----------------------------------------------------------------------
  {
    // Install files at the root: format "7z", upper-cased names, read and checked.
    test::SevenZipBuilder flat;
    flat.add("setup.exe", test::pattern(5000, 2));
    flat.add("DATA.BIN", test::pattern(1474560, 3));  // floppy-sized, no FAT volume: still an install file
    flat.add("EMPTY.TXT", {});
    test::write_bytes(dir / L"flat.7z", flat.build());
    std::vector<std::string> ignored;
    CHECK(floppy_images_in_zip(dir / L"flat.7z", &ignored).empty());
    std::string note = "stale";
    auto src = open_image(dir / L"flat.7z", &note);
    CHECK_EQ(src->format(), std::string("7z"));
    CHECK(note.empty());
    auto setup = src->find("SETUP.EXE");
    CHECK(setup && setup->name == "SETUP.EXE" && setup->size == 5000 && !setup->mtime);
    CHECK(setup && src->read_all(*setup) == test::pattern(5000, 2));
    CHECK(src->find("data.bin") && src->read_all(*src->find("data.bin")) == test::pattern(1474560, 3));
    CHECK(src->find("EMPTY.TXT") && src->read_all(*src->find("EMPTY.TXT")).empty());
    CHECK_EQ(src->list(src->root()).size(), size_t(3));
    // DISK<n> folders (7-Zip's own archive): the union of the disks.
    test::write_bytes(dir / L"disks.7z", bytes_of(test::kLzma2Disks));
    auto disks = open_image(dir / L"disks.7z", &note);
    CHECK_EQ(note, std::string("reading disks.7z as the union of its folders DISK1 and DISK2 (one install disk each)"));
    for (const char* f : {"DISK1/README.TXT", "DISK1/NOTES.TXT", "DISK2/EMPTY.TXT", "DISK2/RANDOM.BIN"}) {
      auto n = disks->find(std::string(f).substr(6));
      CHECK(n && n->mtime.has_value());
      CHECK(n && md5_of(disks->read_all(*n)) == expected_md5("disks", f));
    }
    // Any other folder, a deeper one, a file beside the disks: refused.
    auto source_refused = [&](const wchar_t* name, test::SevenZipBuilder b, const char* expect) {
      test::write_bytes(dir / name, b.build());
      try {
        open_image(dir / name);
        fprintf(stderr, "  %ls: accepted, should have been refused\n", name);
        return false;
      } catch (const ImportError& e) {
        fprintf(stderr, "  %ls -> %s\n", name, e.what());
        return e.status() == Status::source_invalid && strstr(e.what(), expect) != nullptr;
      }
    };
    // One folder that holds everything is read as the root, as a ZIP's is;
    // two folders, or a file beside one, are refused.
    {
      test::SevenZipBuilder one;
      one.add_dir("SUB");
      one.add("SUB/A.TXT", text("a"));
      test::write_bytes(dir / L"one.7z", one.build());
      auto top = open_image(dir / L"one.7z", &note);
      CHECK_EQ(note, std::string("reading one.7z's folder SUB as the source"));
      CHECK(top->find("A.TXT") && top->read_all(*top->find("A.TXT")) == text("a"));
    }
    test::SevenZipBuilder other;
    other.add_dir("SUB");
    other.add("SUB/A.TXT", text("a"));
    other.add("OTHER/B.TXT", text("b"));
    CHECK(source_refused(L"other.7z", other,
                         "(a 7z source holds the install files, or only DISK<n> folders of them, at its root or in "
                         "one folder)"));
    test::SevenZipBuilder deeper;
    deeper.add_dir("DISK1");
    deeper.add("DISK1/SUB/A.TXT", text("a"));
    CHECK(source_refused(L"deeper.7z", deeper, "is not a bare file name or a file in a DISK<n> folder"));
    test::SevenZipBuilder beside;
    beside.add("README.TXT", text("r"));
    beside.add_dir("DISK1");
    beside.add("DISK1/A.TXT", text("a"));
    CHECK(source_refused(L"beside.7z", beside, "holds files at its root (README.TXT) beside DISK<n> folders (DISK1)"));
    test::SevenZipBuilder encrypted;
    encrypted.coder = {0x06, 0xF1, 0x07, 0x01};
    encrypted.add("A.TXT", text("a"));
    CHECK(source_refused(L"encrypted.7z", encrypted, "encrypted (7zAES)"));
  }
  {
    // Floppy images in a folder, stored solid and not, beside a scan and a
    // floppy-sized member that is no FAT volume.
    test::FatBuilder fa = test::FatBuilder::floppy144(), fb = test::FatBuilder::floppy144();
    fa.file("SETUP.LST", text("disk one"));
    fb.file("DISK2.TAG", text("disk two"));
    const Bytes one = fa.build(), two = fb.build();
    for (bool solid : {false, true}) {
      test::SevenZipBuilder b;
      b.solid = solid;
      b.add_dir("images");
      b.add("images/disk2.img", two);
      b.add("images/disk1.img", one);
      b.add("images/scan.jpg", test::pattern(3000, 4));
      b.add("notfat.img", test::pattern(1474560, 5));
      const fs::path p = dir / (solid ? L"images-solid.7z" : L"images.7z");
      test::write_bytes(p, b.build());
      std::vector<std::string> ignored;
      auto got = floppy_images_in_zip(p, &ignored);
      CHECK_EQ(got.size(), size_t(2));
      if (got.size() == 2) {
        CHECK(got[0].name == "images/disk2.img" && *got[0].bytes == two);
        CHECK(got[1].name == "images/disk1.img" && *got[1].bytes == one);
        auto fsrc = open_fat_image(got[1].bytes, "images.7z!images/disk1.img");
        auto n = fsrc->find("SETUP.LST");
        CHECK(n && fsrc->read_all(*n) == text("disk one"));
      }
      CHECK((ignored == std::vector<std::string>{"images/scan.jpg", "notfat.img"}));
      // The bound on the images held together.
      bool threw = false;
      try {
        floppy_images_in_zip(p, nullptr, 2u << 20);
      } catch (const ImportError& e) {
        threw = strstr(e.what(), "holds more than 2 MB of disk images") != nullptr;
      }
      CHECK(threw);
    }
    // A damaged image is refused, not skipped: it may be one of the disks.
    test::SevenZipBuilder d;
    d.add("disk1.img", one);
    Bytes damaged = d.build();
    damaged[32 + 600] ^= 1;
    test::write_bytes(dir / L"damaged.7z", damaged);
    bool threw = false;
    try {
      floppy_images_in_zip(dir / L"damaged.7z");
    } catch (const ImportError& e) {
      threw = e.status() == Status::source_invalid && strstr(e.what(), "damaged.7z!disk1.img: CRC-32 mismatch");
      fprintf(stderr, "  a damaged image -> %s\n", e.what());
    }
    CHECK(threw);
  }
  return test::finish("import.sevenzip");
}

namespace {

// The user's copy (the Internet Archive's 000580-ScreenAnticsJohnnyCastaway,
// the same file) and the floppy image inside it, with the image's files:
// names, sizes and md5s only.
constexpr uint64_t kRealSize = 1355520;
constexpr const char* kRealMd5 = "edf027407e258f73d8056ae8dc87215f";
constexpr const char* kImageMd5 = "81087ea7cc6a304896e81c722b0a85ec";
struct RealFile {
  const char* name;
  uint64_t size;
  const char* md5;
};
constexpr RealFile kRealFiles[] = {
    {"INSTALL.EX$", 102948, "80dce2cf84c8dc83ea0af3c165685be2"},
    {"INSTALL.INS", 2910, "7072f786a9fa42fb639c6624d17467ee"},
    {"LOGO.BMP", 630, "3398cecac28f936734ef01af1ebba530"},
    {"RESOURCE.00$", 1072707, "35a3e2ea7135986c6522ae2a999d5320"},
    {"RESOURCE.001", 35, "472d9346500fbce21a98bd199cf95506"},
    {"RESOURCE.MAP", 1461, "374e6d05c5e0acd88fb5af748948c899"},
    {"SCRANTIC.SC$", 158517, "3990fa5688017eeb3fd11c0c9e725dc6"},
    {"SETUP.EXE", 47616, "7bd67393b022efc1a006cd8dceebdd13"},
    {"SLOGO.BMP", 7318, "aa15cae239244854e6f966fed79b1fca"},
};

int real_test(int argc, char** argv) {
  if (test::get_env(L"AD_E2E_PKG") != L"1") {
    fprintf(stderr, "import.sevenzip_real: skipped (set AD_E2E_PKG=1)\n");
    return 77;
  }
  const fs::path scratch = test::scratch(argc, argv, "adw-import-sevenzip-real");
  test::sandbox_data_root(scratch / L"localappdata");
  const fs::path p = test::find_image(test::image_dirs(argc > 2 ? fs::path(argv[2]) : fs::path()), kRealSize, kRealMd5);
  if (p.empty()) {
    fprintf(stderr, "import.sevenzip_real: skipped (the 7z of Johnny Castaway, md5 %s, is not in the source folders)\n",
            kRealMd5);
    return 77;
  }
  fprintf(stderr, "  %s\n", to_utf8(p.wstring()).c_str());
  SevenZipArchive a(own(test::read_bytes(p)), to_utf8(p.filename().wstring()));
  std::vector<const SevenZipMember*> files;
  for (const SevenZipMember& m : a.members()) {
    fprintf(stderr, "  %s%s, %llu bytes, %s\n", m.name.c_str(), m.directory ? " (folder)" : "",
            (unsigned long long)m.size, a.method(m).c_str());
    if (!m.directory) files.push_back(&m);
  }
  CHECK_EQ(files.size(), size_t(1));
  if (files.size() == 1) {
    CHECK(ends_with_i(files[0]->name, "/disk1.img"));
    CHECK_EQ(files[0]->size, uint64_t(1474560));
    CHECK_EQ(md5_of(extract_all(a, *files[0])), std::string(kImageMd5));
  }
  // As the importer reads it: its one floppy image, then that floppy's files.
  std::vector<std::string> ignored;
  auto images = floppy_images_in_zip(p, &ignored);
  CHECK_EQ(images.size(), size_t(1));
  CHECK(ignored.empty());
  if (images.size() == 1) {
    CHECK_EQ(md5_hex(images[0].bytes->data(), images[0].bytes->size()), std::string(kImageMd5));
    auto fat = open_fat_image(images[0].bytes, images[0].name);
    CHECK_EQ(fat->format(), std::string("fat12"));
    std::set<std::string> listed;
    for (const SourceNode& n : fat->list(fat->root())) listed.insert(n.name);
    CHECK_EQ(listed.size(), std::size(kRealFiles));
    for (const RealFile& f : kRealFiles) {
      auto n = fat->find(f.name);
      CHECK(n && n->size == f.size);
      if (n) CHECK_EQ(md5_of(fat->read_all(*n)), std::string(f.md5));
    }
  }
  return test::finish("import.sevenzip_real");
}

}  // namespace
