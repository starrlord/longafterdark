// PKZIP + ZipCrypto + raw inflate (zip.h) and the archive password's
// derivation from an InstallShield script (PACKAGES.md §8.3, §8.4), on
// archives from tests/zip_builder.h: round trips, the header check byte,
// wrong passwords, damaged and foreign archives, hostile names, member names
// as UTF-8 (bit 11, UTF-8 without it, else code page 437), the candidate
// order of the password search, and the DISK<n> folders a ZIP source may
// keep its install disks in (ZipNames::disk_folders: one level, any case,
// the folders' own entries; any other folder, anything deeper, a folder
// entry with data refused; the installers' archives keep bare names).
#include <memory>

#include "test_util.h"
#include "zip.h"
#include "zip_builder.h"

using namespace adw::import;

namespace {

// The whole member (the library streams only; fixtures are small).
std::vector<uint8_t> extract_all(const ZipArchive& z, const ZipMember& m, std::string_view pw) {
  std::vector<uint8_t> v;
  z.extract(m, pw, [&](const uint8_t* p, size_t n) { v.insert(v.end(), p, p + n); });
  return v;
}

std::shared_ptr<const std::vector<uint8_t>> own(std::vector<uint8_t> v) {
  return std::make_shared<const std::vector<uint8_t>>(std::move(v));
}

bool refused(const std::vector<uint8_t>& archive, const char* what, ZipNames names = ZipNames::bare,
             const char* expect = "") {
  try {
    ZipArchive z(own(archive), "T.ZIP", names);
    fprintf(stderr, "  %s: accepted, should have been refused\n", what);
    return false;
  } catch (const ZipError& e) {
    fprintf(stderr, "  %s -> %s\n", what, e.what());
    if (!strstr(e.what(), expect)) {
      fprintf(stderr, "  %s: expected \"%s\"\n", what, expect);
      return false;
    }
    return true;
  }
}

// An archive of unencrypted stored members with these names (and bytes).
std::vector<uint8_t> named(const std::vector<std::pair<std::string, std::vector<uint8_t>>>& members) {
  test::ZipBuilder b;
  b.password = "";
  for (const auto& [n, d] : members) b.add(n, d, /*deflate=*/false, /*encrypt=*/false);
  return b.build();
}

bool extract_fails(const ZipArchive& z, const ZipMember& m, const std::string& pw, const char* what) {
  try {
    extract_all(z, m, pw);
    fprintf(stderr, "  %s: extracted, should have failed\n", what);
    return false;
  } catch (const ZipError& e) {
    fprintf(stderr, "  %s -> %s\n", what, e.what());
    return true;
  }
}

std::vector<uint8_t> compressible(size_t n) {
  std::vector<uint8_t> v(n);
  for (size_t i = 0; i < n; i++) v[i] = uint8_t("After Dark "[i % 11] + (i / 4096) % 3);
  return v;
}

// An InstallShield-like script: length-prefixed strings among opcodes.
std::vector<uint8_t> script(const std::vector<std::string>& strings_before, const std::string& marker_follow,
                            const std::vector<std::string>& strings_after) {
  std::vector<uint8_t> s = {0xFF, 0xFF, 0x0C, 0x00, 0x01, 0x02};
  auto lp = [&](const std::string& t) {
    s.push_back(uint8_t(t.size()));
    s.push_back(uint8_t(t.size() >> 8));
    s.insert(s.end(), t.begin(), t.end());
    s.push_back(0x13);  // an "opcode" byte, printable-adjacent on purpose
  };
  for (auto& t : strings_before) lp(t);
  lp("Cannot initialize for unzip!");
  s.insert(s.end(), {0x03, 0x00, 0xFF, 0xFF});
  lp(marker_follow);
  for (auto& t : strings_after) lp(t);
  return s;
}

}  // namespace

int main(int argc, char** argv) {
  // No default may reach the real data folder.
  test::sandbox_data_root(test::scratch(argc, argv, "adw-import-zip") / L"localappdata");

  // ---- round trips -----------------------------------------------------------------
  {
    test::ZipBuilder b;
    b.add("DEFLATE.AD", compressible(300000));  // several 64 KiB chunks each way
    b.add("RANDOM.DLL", test::pattern(70000, 5));
    b.add("STORED.AM3", test::pattern(16, 6), /*deflate=*/false);
    b.add("PLAIN.TXT", compressible(1000), true, /*encrypt=*/false);
    b.add("EMPTY.TXT", {}, false);
    b.add("EMPTYZ.TXT", {}, true);
    b.add("I&SSHOW.MID", test::pattern(3000, 7));
    ZipArchive z(own(b.build()), "ROUND.ZIP");
    CHECK_EQ(z.members().size(), size_t(7));
    for (const test::ZipSpec& s : b.members) {
      const ZipMember* m = z.find(s.name);
      CHECK(m != nullptr);
      if (!m) continue;
      CHECK_EQ(m->usize, uint32_t(s.data.size()));
      CHECK_EQ(m->encrypted(), s.encrypt);
      CHECK(extract_all(z, *m, test::kTestZipPassword) == s.data);
      if (s.encrypt) CHECK(z.header_check(*m, test::kTestZipPassword));
    }
    // Case-insensitive lookup; members keep their stored name.
    CHECK(z.find("deflate.ad") && z.find("deflate.ad")->name == "DEFLATE.AD");
    CHECK(!z.find("NOPE.AD"));
    // Streaming: chunks of at most 64 KiB, in order.
    size_t chunks = 0, biggest = 0;
    std::vector<uint8_t> joined;
    z.extract(*z.find("DEFLATE.AD"), test::kTestZipPassword, [&](const uint8_t* p, size_t n) {
      chunks++;
      biggest = std::max(biggest, n);
      joined.insert(joined.end(), p, p + n);
    });
    CHECK(chunks >= 5 && biggest <= 65536 && joined == compressible(300000));
    // An unencrypted member needs no password at all.
    CHECK(extract_all(z, *z.find("PLAIN.TXT"), "") == compressible(1000));
  }

  // ---- wrong password, header check byte ---------------------------------------------------
  {
    test::ZipBuilder b;
    b.add("A.AD", test::pattern(5000, 1));
    b.add("B.AM3", test::pattern(16, 2), false);
    ZipArchive z(own(b.build()), "PW.ZIP");
    const ZipMember& a = *z.find("A.AD");
    CHECK(z.password_opens(a, test::kTestZipPassword));
    CHECK(!z.password_opens(a, "not-the-password"));
    CHECK(extract_fails(z, a, "not-the-password", "wrong password"));
    // 1 in 256 wrong passwords pass the check byte; the CRC-32 catches them.
    int header_passes = 0, opened = 0;
    for (int i = 0; i < 2000; i++) {
      std::string pw = "wrong" + std::to_string(i);
      header_passes += z.header_check(a, pw);
      opened += z.password_opens(a, pw);
    }
    fprintf(stderr, "  2000 wrong passwords: %d pass the check byte, %d open the member\n", header_passes, opened);
    CHECK(header_passes > 0 && header_passes < 30);
    CHECK_EQ(opened, 0);
    // A stored member decrypts to the wrong bytes under a check-byte false
    // positive; the CRC-32 refuses it too.
    const ZipMember& s = *z.find("B.AM3");
    for (int i = 0; i < 5000; i++) {
      std::string pw = "stored" + std::to_string(i);
      if (z.header_check(s, pw)) {
        CHECK(!z.password_opens(s, pw));
        CHECK(extract_fails(z, s, pw, "check-byte false positive on a stored member"));
        break;
      }
    }
  }
  {
    // Flag bit 3 (data descriptor): the check byte is the time's high byte.
    test::ZipBuilder b;
    b.add("T.AD", test::pattern(700, 3));
    b.members[0].extra_flags = 0x08;
    ZipArchive z(own(b.build()), "BIT3.ZIP");
    CHECK(z.header_check(z.members()[0], test::kTestZipPassword));
    CHECK(extract_all(z, z.members()[0], test::kTestZipPassword) == test::pattern(700, 3));
  }

  // ---- damaged archives: corrupt, never a verify failure --------------------------------------
  {
    test::ZipBuilder b;
    b.add("C.AD", test::pattern(9000, 4));
    b.add("D.AD", compressible(20000));
    auto good = b.build();
    {
      auto bad = good;  // central CRC changed
      bad[b.central_offsets[0] + 16] ^= 0x5A;
      ZipArchive z(own(bad), "CRC.ZIP");
      // The check byte comes from the CRC too, so a changed CRC is refused
      // either as a wrong password or as a CRC mismatch; both are errors.
      CHECK(extract_fails(z, *z.find("C.AD"), test::kTestZipPassword, "bad CRC"));
    }
    {
      auto bad = good;  // one byte of compressed data flipped
      bad[30 + 4 + 12 + 100] ^= 0xFF;
      ZipArchive z(own(bad), "DATA.ZIP");
      CHECK(extract_fails(z, *z.find("C.AD"), test::kTestZipPassword, "damaged data"));
    }
    {
      auto bad = good;  // recorded size larger than the data
      bad[b.central_offsets[1] + 24] ^= 0x01;
      ZipArchive z(own(bad), "SIZE.ZIP");
      CHECK(extract_fails(z, *z.find("D.AD"), test::kTestZipPassword, "size mismatch"));
    }
    CHECK(refused(std::vector<uint8_t>(good.begin(), good.begin() + good.size() / 2), "truncated archive"));
    CHECK(refused(std::vector<uint8_t>(good.begin(), good.end() - 1), "end record cut"));
    CHECK(refused(test::pattern(5000, 9), "not a ZIP"));
    CHECK(refused({}, "empty file"));
    {
      // Central directory intact, but the member's data runs past the end.
      auto bad = good;
      bad[b.central_offsets[0] + 20] = 0xFF, bad[b.central_offsets[0] + 21] = 0xFF, bad[b.central_offsets[0] + 22] = 0x0F;
      ZipArchive z(own(bad), "TRUNC.ZIP");
      CHECK(extract_fails(z, *z.find("C.AD"), test::kTestZipPassword, "member past the end"));
    }
  }

  // ---- foreign shapes: refused ---------------------------------------------------------------
  {
    test::ZipBuilder b;
    b.add("A.AD", {1, 2, 3});
    b.zip64_marker = true;
    CHECK(refused(b.build(), "ZIP64 end record"));
  }
  {
    test::ZipBuilder b;
    b.add("A.AD", {1, 2, 3});
    auto img = b.build();
    img[b.central_offsets[0] + 20] = img[b.central_offsets[0] + 21] = img[b.central_offsets[0] + 22] =
        img[b.central_offsets[0] + 23] = 0xFF;
    CHECK(refused(img, "ZIP64 member sizes"));
  }
  {
    test::ZipBuilder b;
    b.add("A.AD", {1, 2, 3});
    b.disk_number = 1;
    CHECK(refused(b.build(), "multi-disk"));
  }
  {
    test::ZipBuilder b;
    b.add("A.AD", {1, 2, 3});
    b.members[0].extra_flags = 0x40;
    CHECK(refused(b.build(), "strong encryption"));
  }
  {
    test::ZipBuilder b;
    b.add("A.AD", {1, 2, 3});
    b.members[0].method_override = 12;  // bzip2
    CHECK(refused(b.build(), "unsupported method"));
  }
  for (const char* hostile : {"../ESCAPE.AD", "SUB/X.AD", "SUB\\X.AD", "C:EVIL.AD", "CON", "NUL.AD", "COM1.DLL",
                              "TRAIL.", "SPACE ", ""}) {
    test::ZipBuilder b;
    b.add(hostile, {1, 2, 3});
    CHECK(refused(b.build(), hostile));
  }
  {
    test::ZipBuilder b;
    b.add("TWICE.AD", {1});
    b.add("twice.ad", {2});
    CHECK(refused(b.build(), "duplicate names"));
    // Case outside ASCII too: u-umlaut and U-umlaut are one name to Windows.
    test::ZipBuilder u;
    u.add("M\xC3\xBCSIK.AD", {1});
    u.add("M\xC3\x9CSIK.AD", {2});
    CHECK(refused(u.build(), "duplicate names, u-umlaut and U-umlaut"));
    test::ZipBuilder two;
    two.add("M\xC3\xBCSIK.AD", {1});
    two.add("M\xC3\xB6SIK.AD", {2});  // o-umlaut: another name
    CHECK_EQ(ZipArchive(own(two.build()), "T.ZIP").members().size(), size_t(2));
  }

  // ---- member names: UTF-8, or code page 437 ---------------------------------------------------
  // Without general-purpose bit 11 a name that is UTF-8 stays UTF-8 (above:
  // M\xC3\xBCSIK.AD, as ZipBuilder never sets the bit), and any other is code
  // page 437, as Explorer and 7-Zip on an English Windows write a name that
  // fits it (0x81 u-umlaut, 0x94 o-umlaut, 0x9A U-umlaut, 0xE0 alpha, 0xA0
  // a-acute). With the bit, a byte that is not UTF-8 is U+FFFD. Every name
  // is UTF-8 afterwards.
  {
    test::ZipBuilder b;
    b.password = "";
    b.add("TREK\x81.IMG", {1}, false, false);
    b.add("TREK\x94.IMG", {2}, true, false);  // another letter: another name
    b.add("DISK\x81\x94" "1.IMG", {3}, false, false);
    b.add("\xE0\xA0.AD", {4}, false, false);  // a UTF-8 sequence cut short: not UTF-8
    b.add("M\xC3\x9CSIK.AD", {5}, false, false);
    b.add("FLAG\xC3\xA9.AD", {6}, false, false);
    b.members.back().extra_flags = 0x800;
    b.add("FLAG\xFF\x81.AD", {7}, false, false);
    b.members.back().extra_flags = 0x800;
    std::unique_ptr<ZipArchive> z;
    try {
      z = std::make_unique<ZipArchive>(own(b.build()), "NAMES.ZIP");
    } catch (const ZipError& e) {
      fprintf(stderr, "  names: refused, should have been read -> %s\n", e.what());
    }
    CHECK(z != nullptr);
    std::vector<std::string> names;
    if (z)
      for (const ZipMember& m : z->members()) {
        names.push_back(m.name);
        CHECK(test::strict_utf8(m.name));
      }
    CHECK((names == std::vector<std::string>{"TREK\xC3\xBC.IMG", "TREK\xC3\xB6.IMG", "DISK\xC3\xBC\xC3\xB6" "1.IMG",
                                            "\xCE\xB1\xC3\xA1.AD", "M\xC3\x9CSIK.AD", "FLAG\xC3\xA9.AD",
                                            "FLAG\xEF\xBF\xBD\xEF\xBF\xBD.AD"}));
    // The data is found as before: the local header's own name is not read.
    const ZipMember* o = z ? z->find("TREK\xC3\xB6.IMG") : nullptr;
    CHECK(o && extract_all(*z, *o, "") == std::vector<uint8_t>{2});
    // Code page 437's u-umlaut and U-umlaut are one name to Windows; the
    // message names the second as UTF-8.
    auto refusal = [&](const std::vector<uint8_t>& archive) {
      try {
        ZipArchive(own(archive), "T.ZIP");
      } catch (const ZipError& e) {
        fprintf(stderr, "  -> %s\n", e.what());
        return std::string(e.what());
      }
      return std::string("accepted");
    };
    test::ZipBuilder pair;
    pair.add("\x81.AD", {1});
    pair.add("\x9A.AD", {2});
    CHECK_EQ(refusal(pair.build()), std::string("T.ZIP: two members are named \xC3\x9C.AD"));
    // Two flagged names whose bytes differ only where they are not UTF-8 read
    // alike (U+FFFD), as Windows reads them: one name.
    test::ZipBuilder bad;
    bad.add("X\xFF.AD", {1});
    bad.add("X\xFE.AD", {2});
    for (auto& m : bad.members) m.extra_flags = 0x800;
    CHECK_EQ(refusal(bad.build()), std::string("T.ZIP: two members are named X\xEF\xBF\xBD.AD"));
  }

  // ---- DISK<n> folders (ZipNames::disk_folders) ------------------------------------------------
  {
    // The folder names: DISK1..DISK99, any case, no leading zero.
    const std::pair<const char*, unsigned> folders[] = {
        {"DISK1", 1},  {"Disk2", 2},  {"disk3", 3},   {"dIsK9", 9}, {"DISK10", 10}, {"DISK99", 99},
        {"DISK0", 0},  {"DISK01", 0}, {"DISK100", 0}, {"DISK", 0},  {"DISKA", 0},   {"DISK 1", 0},
        {"DISK1 ", 0}, {"DISK_1", 0}, {"XDISK1", 0},  {"DISC1", 0}, {"DISK-1", 0},  {"", 0}};
    for (const auto& [name, n] : folders)
      if (disk_folder_number(name) != n) {
        test::g_failures++;
        fprintf(stderr, "  disk_folder_number(\"%s\") = %u, not %u\n", name, disk_folder_number(name), n);
      }
    // The shape of the Internet Archive's ScreamSavers, Marvel and Snoopy
    // ZIPs: files in DISK<n>/, each folder's own entry after its files.
    const auto a = test::pattern(3000, 21), b2 = test::pattern(40, 22), c = test::pattern(7, 23);
    test::ZipBuilder zb;
    zb.password = "";
    zb.add("Disk1/SETUP.PKG", a, true, false);
    zb.add("Disk1/IMAGES.1", b2, false, false);
    zb.add("Disk1/", {}, false, false);
    zb.add("disk2/IMAGES.2", c, false, false);
    zb.add("DISK2/DREAM.ON", {1, 2}, false, false);  // the same name in another disk: the source's business
    zb.add("Disk1/DREAM.ON", {1, 2}, false, false);
    zb.add("DISK12/X#1-A.AD", {9}, false, false);
    zb.add("ROOT.TXT", {3}, false, false);  // a bare name reads as ever (the source refuses the mix)
    const auto bytes = zb.build();
    std::unique_ptr<ZipArchive> z;
    try {
      z = std::make_unique<ZipArchive>(own(bytes), "DISKS.ZIP", ZipNames::disk_folders);
    } catch (const ZipError& e) {
      fprintf(stderr, "  disk folders: refused, should have been read -> %s\n", e.what());
    }
    CHECK(z != nullptr);
    if (z) {
      struct Want {
        const char* name;
        unsigned disk;
        bool directory;
        const char* file;
      };
      const std::vector<Want> want = {{"Disk1/SETUP.PKG", 1, false, "SETUP.PKG"},
                                      {"Disk1/IMAGES.1", 1, false, "IMAGES.1"},
                                      {"Disk1/", 1, true, ""},
                                      {"disk2/IMAGES.2", 2, false, "IMAGES.2"},
                                      {"DISK2/DREAM.ON", 2, false, "DREAM.ON"},
                                      {"Disk1/DREAM.ON", 1, false, "DREAM.ON"},
                                      {"DISK12/X#1-A.AD", 12, false, "X#1-A.AD"},
                                      {"ROOT.TXT", 0, false, "ROOT.TXT"}};
      CHECK_EQ(z->members().size(), want.size());
      for (size_t i = 0; i < want.size() && i < z->members().size(); i++) {
        const ZipMember& m = z->members()[i];
        CHECK_EQ(m.name, std::string(want[i].name));
        CHECK_EQ(m.disk, want[i].disk);
        CHECK_EQ(m.directory, want[i].directory);
        CHECK_EQ(m.file_name(), std::string(want[i].file));
      }
      // Found by the stored name, case-insensitively; extracted as any member.
      CHECK(z->find("DISK1/setup.pkg") && extract_all(*z, *z->find("DISK1/setup.pkg"), "") == a);
      CHECK(z->find("DISK2/images.2") && extract_all(*z, *z->find("DISK2/images.2"), "") == c);
      CHECK(!z->find("SETUP.PKG"));
    }
    // The installers' own archives, and a flat ZIP of install files, keep the
    // bare names: the same archive is refused without disk_folders.
    CHECK(refused(bytes, "DISK<n>/ in a bare-name archive", ZipNames::bare,
                  "member \"Disk1/SETUP.PKG\" is not a bare file name"));
    // Any other folder, anything deeper, other separators, hostile names in a
    // disk, a folder entry holding data: refused.
    for (const char* bad : {"SUB/X.AD", "DISK1/SUB/X.AD", "DISK1/SUB/", "DISK0/X.AD", "DISK01/X.AD", "DISK100/X.AD",
                            "DISK/X.AD", "DISKA/X.AD", "DISK1\\X.AD", "/X.AD", "DISK1//X.AD", "DISK1/C:X.AD",
                            "__MACOSX/DISK1/._X.AD", "DISK1/../X.AD"})
      CHECK(refused(named({{"DISK2/OK.AD", {1}}, {bad, {1}}}), bad, ZipNames::disk_folders,
                    "is not a bare file name or a file in a DISK<n> folder"));
    for (const char* hostile : {"DISK1/..", "DISK1/.", "DISK1/CON", "DISK1/NUL.AD", "DISK1/TRAIL.", "DISK1/SPACE "})
      CHECK(refused(named({{hostile, {1}}}), hostile, ZipNames::disk_folders, "unusable file name"));
    CHECK(refused(named({{"DISK1/", {1}}}), "a folder entry with data", ZipNames::disk_folders,
                  "member \"DISK1/\" is a folder entry that holds data"));
    // One name, as Windows compares them: the folder's case does not make two.
    CHECK(refused(named({{"DISK1/A.AD", {1}}, {"disk1/a.ad", {2}}}), "one file twice", ZipNames::disk_folders,
                  "two members are named disk1/a.ad"));
    CHECK(refused(named({{"DISK1/", {}}, {"Disk1/", {}}}), "one folder entry twice", ZipNames::disk_folders,
                  "two members are named Disk1/"));
    // Code page 437 names in a disk are UTF-8 afterwards, and fold as Windows folds them.
    test::ZipBuilder cp;
    cp.password = "";
    cp.add("DISK1/M\x81SIK.AD", {1}, false, false);
    ZipArchive zc(own(cp.build()), "CP.ZIP", ZipNames::disk_folders);
    CHECK(zc.members()[0].file_name() == "M\xC3\xBCSIK.AD" && zc.members()[0].disk == 1);
    CHECK(refused(named({{"DISK1/\x81.AD", {1}}, {"DISK1/\x9A.AD", {2}}}), "u-umlaut and U-umlaut in a disk",
                  ZipNames::disk_folders, "two members are named DISK1/\xC3\x9C.AD"));
  }

  // ---- the password's candidates (§8.4) --------------------------------------------------------
  {
    auto s = script({"First decoy", "Another one"}, "follow-me", {"later string", "tail"});
    auto c = password_candidates(s);
    CHECK(!c.empty() && c[0] == "follow-me");
    // Then everything else in file order, once each.
    auto pos = [&](const std::string& t) { return std::find(c.begin(), c.end(), t) - c.begin(); };
    CHECK(pos("First decoy") < pos("Another one") && pos("Another one") < pos("later string"));
    CHECK(std::count(c.begin(), c.end(), "follow-me") == 1);
    // Too short and too long strings are not candidates.
    auto t = password_candidates(script({"abc", std::string(33, 'x')}, "okay", {}));
    CHECK(std::find(t.begin(), t.end(), "abc") == t.end());
    CHECK(std::find(t.begin(), t.end(), std::string(33, 'x')) == t.end());
    // A plain run of printable bytes counts too (no length prefix).
    std::vector<uint8_t> raw = {0, 0, 'p', 'l', 'a', 'i', 'n', 'r', 'u', 'n', 0, 1};
    auto u = password_candidates(raw);
    CHECK(std::find(u.begin(), u.end(), "plainrun") != u.end());
  }

  // ---- derive_password -------------------------------------------------------------------------
  {
    const std::string pw = "derive-me-42";
    auto a = test::zip_of({{"PLACE.TXT", test::pattern(30, 1)}, {"A.AD", test::pattern(4000, 2)}}, pw);
    auto bz = test::zip_of({{"MULTI.AM3", test::pattern(16, 3)}}, pw);
    ZipArchive za(own(a), "A.ZIP"), zb(own(bz), "B.ZIP");
    std::vector<const ZipArchive*> zips = {&za, &zb};
    // The preferred position: right after the marker, among decoys.
    auto got = derive_password(script({"decoy-one", "decoy-two"}, pw, {"decoy-three"}), zips);
    CHECK(got && *got == pw);
    // Elsewhere in the script, after a decoy that follows the marker.
    got = derive_password(script({"decoy-one"}, "not-it", {"x-decoy", pw, "tail-decoy"}), zips);
    CHECK(got && *got == pw);
    // A decoy that passes the check byte of the smallest member is still refused.
    std::string trap;
    const ZipMember* smallest = zb.find("MULTI.AM3");
    for (int i = 0; i < 100000 && trap.empty(); i++) {
      std::string c = "trap" + std::to_string(i);
      if (zb.header_check(*smallest, c)) trap = c;
    }
    CHECK(!trap.empty());
    got = derive_password(script({}, trap, {pw}), zips);
    CHECK(got && *got == pw);
    // No candidate works.
    CHECK(!derive_password(script({"a-decoy"}, "nothing-here", {}), zips));
    CHECK(!derive_password({}, zips));
    // Two archives under different passwords: nothing opens both.
    auto c2 = test::zip_of({{"OTHER.AD", test::pattern(500, 9)}}, "second-password");
    ZipArchive zc(own(c2), "C.ZIP");
    std::vector<const ZipArchive*> mixed = {&za, &zc};
    CHECK(!derive_password(script({}, pw, {"second-password"}), mixed));
    // Nothing encrypted: no password needed.
    test::ZipBuilder plain;
    plain.add("A.AD", {1, 2}, true, false);
    ZipArchive zp(own(plain.build()), "P.ZIP");
    got = derive_password(script({}, "whatever", {}), {&zp});
    CHECK(got && got->empty());
  }
  return test::finish("import.zip");
}
