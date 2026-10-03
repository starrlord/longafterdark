// FAT12/FAT16 image reader (fat.h) and the SourceFs views over it
// (source.h): the 1.44 MB and 2.88 MB floppy geometries, FAT16, a fragmented
// chain, subdirectories, the entries the reader must skip (long names,
// deleted, volume label, "." and ".."), the volume label decoded from code
// page 437 as the names are, and every damaged shape PACKAGES.md
// §8.2 says to refuse — each read both from the image's file and from its
// bytes in memory (a floppy image a ZIP holds), which must agree. Then
// content sniffing (ISO first, FAT second), the union of two floppy images,
// and the floppy images a ZIP holds (floppy_images_in_zip: floppy-sized FAT
// members only, a member with no boot sector never inflated past its first
// 64 KiB, at most max_bytes of images; a flat ZIP of install files stays one).
#include <functional>
#include <map>

#include "fat.h"
#include "fat_builder.h"
#include "iso_builder.h"
#include "source.h"
#include "test_util.h"
#include "zip_builder.h"

using namespace adw::import;
namespace fs = std::filesystem;

namespace {

std::vector<uint8_t> text(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }

std::vector<uint8_t> read(const FatImage& img, const FatEntry& e) {
  std::vector<uint8_t> out;
  img.read(e, [&](const uint8_t* p, size_t n) { out.insert(out.end(), p, p + n); });
  return out;
}

const FatEntry* find(const std::vector<FatEntry>& v, const std::string& name) {
  for (const FatEntry& e : v)
    if (e.name == name) return &e;
  return nullptr;
}

// The image's bytes, as a ZIP member's are held.
std::shared_ptr<const std::vector<uint8_t>> bytes_of(const fs::path& p) {
  return std::make_shared<const std::vector<uint8_t>>(test::read_bytes(p));
}

// Both ways an image is read — its file, and its bytes in memory — refuse it
// with the same message.
bool refused(const fs::path& p, const char* what) {
  std::string messages[2];
  for (int mem = 0; mem < 2; mem++) {
    try {
      std::unique_ptr<FatImage> img = mem ? std::make_unique<FatImage>(bytes_of(p)) : std::make_unique<FatImage>(p);
      fprintf(stderr, "  %s (%s): opened, should have been refused\n", what, mem ? "memory" : "file");
      return false;
    } catch (const FatError& e) {
      messages[mem] = e.what();
    }
  }
  fprintf(stderr, "  %s -> %s\n", what, messages[0].c_str());
  CHECK_EQ(messages[1], messages[0]);
  return true;
}

bool read_refused(const fs::path& p, const std::string& file, const char* what) {
  std::string messages[2];
  for (int mem = 0; mem < 2; mem++) {
    try {
      std::unique_ptr<FatImage> img = mem ? std::make_unique<FatImage>(bytes_of(p)) : std::make_unique<FatImage>(p);
      auto root = img->list(img->root());
      const FatEntry* e = find(root, file);
      if (!e) {
        fprintf(stderr, "  %s: %s not listed\n", what, file.c_str());
        return false;
      }
      read(*img, *e);
      fprintf(stderr, "  %s (%s): read, should have been refused\n", what, mem ? "memory" : "file");
      return false;
    } catch (const FatError& e) {
      messages[mem] = e.what();
    }
  }
  fprintf(stderr, "  %s -> %s\n", what, messages[0].c_str());
  CHECK_EQ(messages[1], messages[0]);
  return true;
}

// The standard test volume: files of several sizes, one fragmented, a
// subdirectory tree, and the raw entries a reader must skip.
test::FatBuilder standard(test::FatBuilder b) {
  b.label = "TESTVOL";
  b.file("INSTALL.INS", test::pattern(5000, 1));
  b.file("SETUP.PKG", text("disk list"));
  b.file("EMPTY.TXT", {});
  b.file("BIG.ZIP", test::pattern(40000, 2), /*fragment=*/true);
  b.file("AFTER.ZIP", test::pattern(3000, 3));
  b.file("SUB/INNER.DAT", test::pattern(1500, 4));
  b.file("SUB/DEEPER/LEAF.BIN", test::pattern(700, 5));
  b.file("\xE5" "LEAD.TXT", text("leading E5"));  // stored with the 0x05 escape
  b.root_raw.push_back(test::FatBuilder::raw_entry("DELETED TXT", 0x20, 0xE5));
  b.root_raw.push_back(test::FatBuilder::raw_entry("Aa long nam", 0x0F));
  b.node("AFTER.ZIP").raw_before.push_back(test::FatBuilder::raw_entry("Bb long nam", 0x0F));
  return b;
}

void check_standard(const FatImage& img, int bits) {
  CHECK_EQ(img.fat_bits(), bits);
  CHECK_EQ(img.volume_label(), std::string("TESTVOL"));
  auto root = img.list(img.root());
  std::vector<std::string> names;
  for (auto& e : root) names.push_back(e.name);
  // Label, deleted and LFN entries are skipped; the rest in on-disk order.
  CHECK((names == std::vector<std::string>{"INSTALL.INS", "SETUP.PKG", "EMPTY.TXT", "BIG.ZIP", "AFTER.ZIP", "SUB",
                                          "\xCF\x83LEAD.TXT"}));
  CHECK(read(img, *find(root, "INSTALL.INS")) == test::pattern(5000, 1));
  CHECK(read(img, *find(root, "EMPTY.TXT")).empty());
  CHECK(read(img, *find(root, "BIG.ZIP")) == test::pattern(40000, 2));
  CHECK(read(img, *find(root, "AFTER.ZIP")) == test::pattern(3000, 3));
  CHECK(read(img, *find(root, "\xCF\x83LEAD.TXT")) == text("leading E5"));
  const FatEntry* sub = find(root, "SUB");
  CHECK(sub && sub->is_dir);
  if (sub) {
    auto inner = img.list(*sub);  // "." and ".." skipped
    CHECK_EQ(inner.size(), size_t(2));
    const FatEntry* f = find(inner, "INNER.DAT");
    CHECK(f && read(img, *f) == test::pattern(1500, 4));
    const FatEntry* d = find(inner, "DEEPER");
    if (d) {
      auto deeper = img.list(*d);
      CHECK(deeper.size() == 1 && deeper[0].name == "LEAF.BIN" && read(img, deeper[0]) == test::pattern(700, 5));
    } else {
      CHECK(d != nullptr);
    }
  }
  // DOS date and time come through.
  const FatEntry* ins = find(root, "INSTALL.INS");
  CHECK(ins && ins->dos_date == 0x1CF1 && ins->dos_time == 0x7A00);
}

// From the file, and from the same bytes in memory: the same volume.
void check_standard(const fs::path& p, int bits) {
  FatImage file(p);
  check_standard(file, bits);
  FatImage mem(bytes_of(p));
  check_standard(mem, bits);
  CHECK(mem.file_size() == file.file_size() && mem.cluster_count() == file.cluster_count() &&
        mem.volume_label() == file.volume_label());
}

}  // namespace

int main(int argc, char** argv) {
  fs::path dir = test::scratch(argc, argv, "adw-import-fat");
  test::sandbox_data_root(dir / L"localappdata");  // no default may reach the real data folder

  // ---- geometries ----------------------------------------------------------------------
  {
    auto b = standard(test::FatBuilder::floppy144());
    test::write_bytes(dir / L"f144.img", b.build());
    CHECK_EQ(fs::file_size(dir / L"f144.img"), uint64_t(1474560));
    check_standard(dir / L"f144.img", 12);
  }
  {
    auto b = standard(test::FatBuilder::floppy288());
    test::write_bytes(dir / L"f288.img", b.build());
    CHECK_EQ(fs::file_size(dir / L"f288.img"), uint64_t(2949120));
    check_standard(dir / L"f288.img", 12);
    FatImage img(dir / L"f288.img");
    CHECK(img.sectors_per_cluster() == 2 && img.bytes_per_sector() == 512 && img.media() == 0xF0);
  }
  {
    // 16 MB with 4-sector clusters: over 4084 clusters, so FAT16.
    test::FatBuilder b;
    b.spc = 4;
    b.root_entries = 512;
    b.total_sectors = 32768;
    b.spf = 32;
    b.media = 0xF8;
    auto s = standard(std::move(b));
    CHECK(s.fat16());
    test::write_bytes(dir / L"f16.img", s.build());
    check_standard(dir / L"f16.img", 16);
  }
  {
    // Larger sectors: 1024 bytes each.
    test::FatBuilder b;
    b.bps = 1024;
    b.total_sectors = 1440;
    b.spf = 5;
    b.root_entries = 224;
    auto s = standard(std::move(b));
    test::write_bytes(dir / L"f1k.img", s.build());
    check_standard(dir / L"f1k.img", 12);
  }
  // ---- the volume label: code page 437, as the names are ----------------------------------------
  {
    test::FatBuilder b = test::FatBuilder::floppy144();
    b.label = "STAR TR\x81K";  // 0x81: u-umlaut
    b.file("A.TXT", text("a"));
    test::write_bytes(dir / L"label437.img", b.build());
    FatImage file(dir / L"label437.img"), mem(bytes_of(dir / L"label437.img"));
    CHECK_EQ(file.volume_label(), std::string("STAR TR\xC3\xBCK"));
    CHECK_EQ(mem.volume_label(), file.volume_label());
    CHECK_EQ(open_image(dir / L"label437.img")->volume_id(), std::string("STAR TR\xC3\xBCK"));
    // A leading 0xE5 is stored as 0x05, as in a name (sigma); the case is kept.
    test::FatBuilder s = test::FatBuilder::floppy144();
    s.label = "\x05igma disk";
    test::write_bytes(dir / L"label05.img", s.build());
    CHECK_EQ(FatImage(dir / L"label05.img").volume_label(), std::string("\xCF\x83igma disk"));
  }

  // ---- damaged images: refused ------------------------------------------------------------
  auto base = standard(test::FatBuilder::floppy144());
  const auto good = base.build();
  const uint32_t big = base.node("BIG.ZIP").clusters[0];
  const auto& big_chain = base.node("BIG.ZIP").clusters;
  auto variant = [&](const wchar_t* name, const std::function<void(std::vector<uint8_t>&)>& damage) {
    auto img = good;
    damage(img);
    test::write_bytes(dir / name, img);
    return dir / name;
  };
  CHECK(refused(variant(L"no-sig.img", [](auto& v) { v[510] = 0; }), "no 55 AA"));
  CHECK(refused(variant(L"bps0.img", [](auto& v) { v[11] = v[12] = 0; }), "bytes/sector 0"));
  CHECK(refused(variant(L"bps300.img", [](auto& v) { v[11] = 0x2C, v[12] = 0x01; }), "bytes/sector 300"));
  CHECK(refused(variant(L"spc3.img", [](auto& v) { v[13] = 3; }), "sectors/cluster 3"));
  CHECK(refused(variant(L"res0.img", [](auto& v) { v[14] = v[15] = 0; }), "no reserved sectors"));
  CHECK(refused(variant(L"fats3.img", [](auto& v) { v[16] = 3; }), "three FATs"));
  CHECK(refused(variant(L"root0.img", [](auto& v) { v[17] = v[18] = 0; }), "no root entries"));
  CHECK(refused(variant(L"spf0.img", [](auto& v) { v[22] = v[23] = 0; }), "no sectors per FAT"));
  CHECK(refused(variant(L"tot0.img", [](auto& v) { v[19] = v[20] = 0; }), "no sector count"));
  {
    auto img = good;
    img.resize(img.size() - 512);
    test::write_bytes(dir / L"short.img", img);
    CHECK(refused(dir / L"short.img", "shorter than its sectors"));
  }
  CHECK(refused(variant(L"tiny.img", [](auto& v) { v.resize(100); }), "too small"));
  {
    // FAT32-sized: 70000 clusters (a sparse 35 MB file).
    test::FatBuilder b;
    b.total_sectors = 70000;
    b.spf = 540;
    b.root_entries = 512;
    // Only the boot sector matters for this refusal: write it into a file of the right size.
    auto img = b.build();
    HANDLE h = CreateFileW((dir / L"fat32ish.img").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD w = 0;
    WriteFile(h, img.data(), 512 * 64, &w, nullptr);
    LARGE_INTEGER sz;
    sz.QuadPart = int64_t(img.size());
    SetFilePointerEx(h, sz, nullptr, FILE_BEGIN);
    SetEndOfFile(h);
    CloseHandle(h);
    CHECK(refused(dir / L"fat32ish.img", "FAT32-sized"));
  }
  // Chains: the image opens (the root lists), reading the file is refused.
  {
    test::FatBuilder& b = base;
    auto img = good;
    b.set_fat(img, big_chain[2], big_chain[1]);  // loops back
    test::write_bytes(dir / L"loop.img", img);
    CHECK(read_refused(dir / L"loop.img", "BIG.ZIP", "chain loop"));
    img = good;
    b.set_fat(img, big_chain[3], 0);  // free cluster inside the chain
    test::write_bytes(dir / L"free.img", img);
    CHECK(read_refused(dir / L"free.img", "BIG.ZIP", "free cluster in chain"));
    img = good;
    b.set_fat(img, big_chain[3], 0xFF7);  // bad cluster
    test::write_bytes(dir / L"bad.img", img);
    CHECK(read_refused(dir / L"bad.img", "BIG.ZIP", "bad cluster in chain"));
    img = good;
    b.set_fat(img, big_chain[3], 4000);  // beyond the last cluster
    test::write_bytes(dir / L"range.img", img);
    CHECK(read_refused(dir / L"range.img", "BIG.ZIP", "out-of-range cluster"));
    img = good;
    b.set_fat(img, big_chain[5], 0xFFF);  // ends early
    test::write_bytes(dir / L"shortchain.img", img);
    CHECK(read_refused(dir / L"shortchain.img", "BIG.ZIP", "chain shorter than the file"));
    img = good;
    size_t e = base.node("AFTER.ZIP").entry_offset;
    img[e + 26] = img[e + 27] = 0;  // non-empty file without clusters
    test::write_bytes(dir / L"nocluster.img", img);
    CHECK(read_refused(dir / L"nocluster.img", "AFTER.ZIP", "no clusters"));
    // The damage is local: every other file still reads.
    FatImage ok(dir / L"loop.img");
    auto root = ok.list(ok.root());
    CHECK(read(ok, *find(root, "AFTER.ZIP")) == test::pattern(3000, 3));
    (void)big;
  }

  // ---- SourceFs: sniffing, FAT view, union of two floppies -------------------------------------
  {
    auto fat = open_image(dir / L"f288.img");
    CHECK_EQ(fat->format(), std::string("fat12"));
    CHECK_EQ(fat->volume_id(), std::string("TESTVOL"));
    auto n = fat->find("sub/deeper/leaf.bin");
    CHECK(n && fat->read_all(*n) == test::pattern(700, 5));
    CHECK(n && n->mtime.has_value());
    CHECK(!fat->find("SUB/NOPE"));
    test::IsoBuilder ib;
    ib.file("INSTALL/X.TXT", text("iso"));
    test::write_bytes(dir / L"small.iso", ib.build());
    auto iso = open_image(dir / L"small.iso");
    CHECK_EQ(iso->format(), std::string("iso9660+joliet"));
    test::write_bytes(dir / L"junk.img", test::pattern(3000000, 11));
    bool threw = false;
    try {
      open_image(dir / L"junk.img");
    } catch (const ImportError& e) {
      threw = e.status() == Status::source_invalid;
      fprintf(stderr, "  junk image -> %s\n", e.what());
    }
    CHECK(threw);
  }
  {
    test::FatBuilder d1 = test::FatBuilder::floppy144(), d2 = test::FatBuilder::floppy144();
    d1.file("DISK.1", text("1"));
    d1.file("SHARED.PKG", text("same bytes"));
    d1.file("ONE.ZIP", test::pattern(2000, 21));
    d1.file("DIR/A.TXT", text("a"));
    d2.file("DISK.2", text("2"));
    d2.file("SHARED.PKG", text("same bytes"));
    d2.file("TWO.ZIP", test::pattern(2000, 22));
    d2.file("DIR/B.TXT", text("b"));
    d2.file("CLASH.TXT", text("xxxx"));
    d1.file("CLASH.TXT", text("yyyy"));  // same size, different bytes
    d2.file("SIZED.TXT", text("12345"));
    d1.file("SIZED.TXT", text("123"));
    test::write_bytes(dir / L"disk1.img", d1.build());
    test::write_bytes(dir / L"disk2.img", d2.build());
    std::vector<std::unique_ptr<SourceFs>> parts;
    parts.push_back(open_image(dir / L"disk1.img"));
    parts.push_back(open_image(dir / L"disk2.img"));
    auto u = union_of(std::move(parts));
    // Another size is refused when the file is read, never when it is only
    // listed (what no recipe reads, a BBS's notes, never stops an import).
    bool size_threw = false;
    try {
      u->list(u->root());
      u->read_all(*u->find("SIZED.TXT"));
    } catch (const ImportError& e) {
      size_threw = std::string(e.what()).find("SIZED.TXT differs between the images (size)") != std::string::npos;
      fprintf(stderr, "  union, same name different size -> %s\n", e.what());
    }
    CHECK(size_threw);
    CHECK(u->read_all(*u->find("SHARED.PKG")) == text("same bytes"));
    // Without the size clash: the listing merges, the byte clash shows on read.
    test::FatBuilder e1 = test::FatBuilder::floppy144(), e2 = test::FatBuilder::floppy144();
    e1.file("DISK.1", text("1"));
    e1.file("SHARED.PKG", text("same bytes"));
    e1.file("DIR/A.TXT", text("a"));
    e1.file("CLASH.TXT", text("yyyy"));
    e2.file("DISK.2", text("2"));
    e2.file("SHARED.PKG", text("same bytes"));
    e2.file("DIR/B.TXT", text("b"));
    e2.file("CLASH.TXT", text("xxxx"));
    test::write_bytes(dir / L"e1.img", e1.build());
    test::write_bytes(dir / L"e2.img", e2.build());
    std::vector<std::unique_ptr<SourceFs>> p2;
    p2.push_back(open_image(dir / L"e1.img"));
    p2.push_back(open_image(dir / L"e2.img"));
    auto v = union_of(std::move(p2));
    auto root = v->list(v->root());
    CHECK_EQ(root.size(), size_t(5));  // DISK.1, SHARED.PKG, DIR, CLASH.TXT, DISK.2
    CHECK(v->find("DISK.2") && v->find("DIR/A.TXT") && v->find("DIR/B.TXT"));
    CHECK(v->read_all(*v->find("SHARED.PKG")) == text("same bytes"));
    bool byte_threw = false;
    try {
      v->read_all(*v->find("CLASH.TXT"));
    } catch (const ImportError& e) {
      byte_threw = true;
      fprintf(stderr, "  union, same name different bytes -> %s\n", e.what());
    }
    CHECK(byte_threw);
  }
  // ---- the floppy images a ZIP holds ---------------------------------------------------------------
  // The Internet Archive zips an item's disk images (stored, disk 2 first,
  // with its scans and metadata in a whole-item download): the floppy-sized
  // FAT members are the images, in the ZIP's order; nothing else is inflated.
  {
    test::FatBuilder a = test::FatBuilder::floppy144(), b = test::FatBuilder::floppy144();
    a.file("SETUP.LST", text("disk one"));
    b.file("DISK2.TAG", text("disk two"));
    const auto one = a.build(), two = b.build();
    test::FatBuilder small = test::FatBuilder::floppy144();
    small.total_sectors = 1440;  // 720 KB (two sectors a cluster, 112 root entries), a floppy size too
    small.spc = 2;
    small.root_entries = 112;
    small.spf = 3;
    small.file("SMALL.TXT", text("720 KB"));
    const auto seven_twenty = small.build();
    test::ZipBuilder z;
    z.password = "";
    z.add("disk2.img", two, /*deflate=*/false, /*encrypt=*/false);
    z.add("disk1.img", one, /*deflate=*/true, /*encrypt=*/false);
    z.add("disk1.jpg", test::pattern(3000, 9), false, false);
    auto odd = one;
    odd.push_back(0);  // a FAT volume, but not a floppy's size (not a multiple of 512): never inflated
    z.add("odd.img", odd, false, false);
    z.add("notfat.img", test::pattern(1474560, 10), false, false);            // floppy-sized, no boot sector
    auto bad_bpb = one;
    bad_bpb[13] = 3;  // a boot sector (55 AA, 512-byte sectors), but 3 sectors a cluster: no FAT volume
    z.add("badbpb.img", bad_bpb, true, false);
    z.add("small.ima", seven_twenty, false, false);
    test::write_bytes(dir / L"images.zip", z.build());
    std::vector<std::string> ignored;
    auto got = floppy_images_in_zip(dir / L"images.zip", &ignored);
    CHECK_EQ(got.size(), size_t(3));
    if (got.size() == 3) {
      CHECK(got[0].name == "disk2.img" && *got[0].bytes == two);
      CHECK(got[1].name == "disk1.img" && *got[1].bytes == one);
      CHECK(got[2].name == "small.ima" && *got[2].bytes == seven_twenty);
      auto fsrc = open_fat_image(got[1].bytes, "images.zip!disk1.img");
      CHECK_EQ(fsrc->format(), std::string("fat12"));
      auto n = fsrc->find("SETUP.LST");
      CHECK(n && fsrc->read_all(*n) == text("disk one"));
      CHECK(n && n->mtime.has_value());
    }
    CHECK((ignored == std::vector<std::string>{"disk1.jpg", "odd.img", "notfat.img", "badbpb.img"}));
    // A flat ZIP of install files, a floppy-sized member among them: no image,
    // so it stays a ZIP of install files (open_image reads it as before).
    test::ZipBuilder flat;
    flat.password = "";
    flat.add("SETUP.EXE", test::pattern(5000, 12), true, false);
    flat.add("DATA.BIN", test::pattern(1474560, 13), true, false);
    test::write_bytes(dir / L"flat.zip", flat.build());
    ignored.clear();
    CHECK(floppy_images_in_zip(dir / L"flat.zip", &ignored).empty());
    CHECK(ignored.empty());
    auto fz = open_image(dir / L"flat.zip");
    CHECK_EQ(fz->format(), std::string("zip"));
    CHECK(fz->find("DATA.BIN").has_value());
    // No ZIP at all (an image, a text file): nothing.
    CHECK(floppy_images_in_zip(dir / L"f144.img").empty());
    CHECK(floppy_images_in_zip(dir / L"nosuch.zip").empty());
    // One image in a ZIP.
    test::ZipBuilder single;
    single.password = "";
    single.add("only.img", one, true, false);
    test::write_bytes(dir / L"single.zip", single.build());
    auto only = floppy_images_in_zip(dir / L"single.zip");
    CHECK(only.size() == 1 && *only[0].bytes == one);
    // The images in a folder (the Internet Archive's ZIP of Intermission
    // 4.0's floppies, "Intermission 4.0/ITM4W-D1.IMA"), its own entry beside
    // them, as a 7z's may be: named with their folder.
    test::ZipBuilder in_folder;
    in_folder.password = "";
    in_folder.add("A Product 1.0/", {}, false, false);
    in_folder.add("A Product 1.0/DISK-1.IMA", one, true, false);
    in_folder.add("A Product 1.0/DISK-2.IMA", two, true, false);
    in_folder.add("A Product 1.0/scans/label.jpg", text("a scan"), true, false);
    test::write_bytes(dir / L"in-folder.zip", in_folder.build());
    ignored.clear();
    auto folder_images = floppy_images_in_zip(dir / L"in-folder.zip", &ignored);
    CHECK(folder_images.size() == 2 && folder_images[0].name == "A Product 1.0/DISK-1.IMA" &&
          *folder_images[0].bytes == one && folder_images[1].name == "A Product 1.0/DISK-2.IMA" &&
          *folder_images[1].bytes == two);
    CHECK((ignored == std::vector<std::string>{"A Product 1.0/scans/label.jpg"}));
    // A password-protected or damaged floppy-sized member is refused, not
    // skipped: it may be one of the disks.
    auto refused_zip = [&](const wchar_t* name, const std::vector<uint8_t>& zip, const char* want,
                           uint64_t max_bytes = kMaxZippedImageBytes) {
      test::write_bytes(dir / name, zip);
      try {
        floppy_images_in_zip(dir / name, nullptr, max_bytes);
        fprintf(stderr, "  %s: accepted, should have been refused\n", to_utf8(name).c_str());
        test::g_failures++;
      } catch (const ImportError& e) {
        fprintf(stderr, "  %s -> %s\n", to_utf8(name).c_str(), e.what());
        CHECK(e.status() == Status::source_invalid && std::string(e.what()).find(want) != std::string::npos);
      }
    };
    test::ZipBuilder locked;
    locked.add("disk1.img", one, false, true);
    refused_zip(L"locked.zip", locked.build(), "locked.zip!disk1.img is password-protected");
    test::ZipBuilder stored;
    stored.password = "";
    stored.add("only.img", one, /*deflate=*/false, /*encrypt=*/false);
    auto damaged = stored.build();
    damaged[30 + 8 + 40000] ^= 0x01;  // inside the stored image
    refused_zip(L"damaged.zip", damaged, "only.img: CRC-32 mismatch");
    // A floppy-sized member whose first sector is no boot sector is not
    // inflated past its first 64 KiB (the chunk that holds that sector):
    // damage further on is never met (it would be a CRC-32 mismatch), and the
    // member is only named.
    test::ZipBuilder late;
    late.password = "";
    late.add("scan.img", test::pattern(1474560, 15), /*deflate=*/false, /*encrypt=*/false);
    auto late_zip = late.build();
    late_zip[30 + 8 + 1400000] ^= 0x01;
    test::write_bytes(dir / L"late.zip", late_zip);
    bool late_threw = false;
    try {
      CHECK(floppy_images_in_zip(dir / L"late.zip").empty());
    } catch (const ImportError& e) {
      late_threw = true;
      fprintf(stderr, "  late damage in a member with no boot sector -> %s\n", e.what());
    }
    CHECK(!late_threw);
    // Every image is held in memory: members with a boot sector past
    // max_bytes refuse the ZIP; members without one do not count.
    test::ZipBuilder many;
    many.password = "";
    many.add("noise.img", test::pattern(1474560, 16), true, false);
    many.add("a.img", one, true, false);
    many.add("b.img", two, true, false);
    test::write_bytes(dir / L"many.zip", many.build());
    CHECK_EQ(floppy_images_in_zip(dir / L"many.zip", nullptr, 3u << 20).size(), size_t(2));  // 2.8 MB of images
    many.add("c.img", one, true, false);  // 4.2 MB
    refused_zip(L"many3.zip", many.build(), "many3.zip holds more than 3 MB of disk images; no release came on that many disks",
                3u << 20);
    CHECK_EQ(floppy_images_in_zip(dir / L"many3.zip", nullptr, 5u << 20).size(), size_t(3));
    CHECK_EQ(floppy_images_in_zip(dir / L"many3.zip").size(), size_t(3));
    // Bytes that are no FAT volume, opened as one.
    bool threw = false;
    try {
      open_fat_image(std::make_shared<const std::vector<uint8_t>>(test::pattern(1474560, 14)), "x.zip!junk.img");
    } catch (const ImportError& e) {
      threw = e.status() == Status::source_invalid && std::string(e.what()).find("x.zip!junk.img is not a FAT") == 0;
      fprintf(stderr, "  junk in memory -> %s\n", e.what());
    }
    CHECK(threw);
  }

  // ---- directory identity (SourceFs::dir_key) --------------------------------------------------
  // Two entries whose first cluster is the same directory are one directory
  // listed twice (the importer refuses that); distinct ones differ, and a
  // union of images keys a directory by every image's copy.
  {
    test::FatBuilder a = test::FatBuilder::floppy144();
    a.file("ONE/X.TXT", text("x"));
    a.file("TWO/Y.TXT", text("y"));
    a.file("THREE/Z.TXT", text("z"));
    auto img = a.build();
    const uint32_t one = a.node("ONE").clusters.at(0);
    const size_t two = a.node("TWO").entry_offset;
    img[two + 26] = uint8_t(one), img[two + 27] = uint8_t(one >> 8);
    test::write_bytes(dir / L"alias.img", img);
    auto fsrc = open_image(dir / L"alias.img");
    std::map<std::string, std::string> key;
    for (const SourceNode& n : fsrc->list(fsrc->root())) key[n.name] = fsrc->dir_key(n);
    CHECK(!key["ONE"].empty());
    CHECK_EQ(key["ONE"], key["TWO"]);
    CHECK(key["ONE"] != key["THREE"]);
    CHECK(fsrc->dir_key(fsrc->root()) != key["ONE"]);
    auto x = fsrc->find("ONE/X.TXT");
    CHECK(x && fsrc->dir_key(*x).empty());  // a file has no directory key

    std::vector<std::unique_ptr<SourceFs>> parts;
    parts.push_back(open_image(dir / L"alias.img"));
    parts.push_back(open_image(dir / L"f288.img"));
    auto u = union_of(std::move(parts));
    std::map<std::string, std::string> ukey;
    for (const SourceNode& n : u->list(u->root()))
      if (n.is_dir) ukey[n.name] = u->dir_key(n);
    CHECK(!ukey["ONE"].empty());
    CHECK_EQ(ukey["ONE"], ukey["TWO"]);
    CHECK(ukey["ONE"] != ukey["THREE"]);
    CHECK(ukey["SUB"] != ukey["ONE"]);
  }
  return test::finish("import.fat");
}
