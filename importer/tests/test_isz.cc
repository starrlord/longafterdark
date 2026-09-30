// InstallShield 2 compressed libraries and PKWARE DCL explode (isz.h;
// research/win/pkg/installshield/SURVEY_REPORT.md, "Test strategy"). Nothing
// here compresses anything:
//   vectors      the survey's libraries made by InstallShield's own ICOMP.EXE
//                (tests/isz_vectors.h: dictionary bits 4, 5 and 6, and a
//                stored member, which is refused by name) decode to the bytes
//                the importer tests' generators make; the survey's crafted
//                and damaged streams, written again token by token by
//                tests/isz_builder.h (the same bytes as its tools/dclwrite.py
//                wrote), decode as its reference did — but for the coded
//                literals and the one-bits after an end code, which this
//                reader refuses by name
//   explode      every DCL rule, crafted: the header bytes, every length,
//                every distance at each dictionary size, overlapping copies, a
//                copy reaching exactly the first byte and one byte further,
//                output past the recorded size (a literal, a copy) and short
//                of it at the end code, truncation at every byte, bytes and
//                one-bits after the end code, chunks of at most 64 KiB
//   container    libraries and split sets laid out by isz_builder.h
//                (volumes in every order, a boundary at every byte of a
//                member): members(), find(), extract(), isz_header(),
//                isz_volume_name(); every container rule broken, and every
//                refusal by name (password, stored member, named or second
//                directory, a member over three volumes, a boundary between
//                members, a missing, doubled or misnamed volume)
//   bit flips    every single-bit flip of the vectors of 1 KB or less, and
//                of a small split set: an IszError, or exactly the recorded
//                size, never a crash
//   real         (opt-in, AD_E2E_PKG=1) the user's Marvel Comics Screen
//                Posters and Snoopy's Screen Savers ZIPs, found by size and
//                md5 and read as DISK<n> sources (the union lists every
//                disk's files once): every member of every library against
//                the survey's md5s (tests/isz_real.h), from the ZIP and from
//                a folder copy of the libraries' volumes in DISK folders
//
//   test_import_isz <scratch>
//   test_import_isz real <scratch> <source_iso>
#include <algorithm>
#include <functional>
#include <iterator>
#include <memory>
#include <numeric>
#include <set>
#include <tuple>

#include "arj_builder.h"
#include "isz.h"
#include "isz_builder.h"
#include "isz_real.h"
#include "isz_vectors.h"
#include "md5.h"
#include "names.h"
#include "source.h"
#include "test_util.h"
#include "zip.h"

using namespace adw::import;
namespace fs = std::filesystem;

namespace {

using Bytes = std::vector<uint8_t>;
using test::DclToken;

std::shared_ptr<const Bytes> own(Bytes v) { return std::make_shared<const Bytes>(std::move(v)); }

std::string md5_of(const Bytes& b) { return md5_hex(b.data(), b.size()); }

Bytes cat(Bytes a, const Bytes& b) {
  a.insert(a.end(), b.begin(), b.end());
  return a;
}

std::vector<DclToken> cat(std::vector<DclToken> a, const std::vector<DclToken>& b) {
  a.insert(a.end(), b.begin(), b.end());
  return a;
}

// What the tokens say the output is: literals and copies applied in order
// (the tests' own account of the format, independent of the decoder).
Bytes expand(const std::vector<DclToken>& tokens) {
  Bytes out;
  for (const DclToken& t : tokens) {
    if (t.kind == DclToken::Kind::literal) out.push_back(uint8_t(t.a));
    if (t.kind == DclToken::Kind::copy)
      for (uint32_t i = 0; i < t.a; i++) out.push_back(out[out.size() - t.b]);
    if (t.kind == DclToken::Kind::end) break;
  }
  return out;
}

std::vector<DclToken> with_end(std::vector<DclToken> t) {
  t.push_back(DclToken::end());
  return t;
}

// The whole output of explode, or the IszError's message in `why`.
std::optional<Bytes> explode(const Bytes& in, uint32_t size, std::string* why = nullptr, size_t* chunks = nullptr,
                             size_t* biggest = nullptr) {
  Bytes out;
  try {
    isz_detail::explode(in, size, [&](const uint8_t* p, size_t n) {
      if (chunks) ++*chunks;
      if (biggest) *biggest = std::max(*biggest, n);
      out.insert(out.end(), p, p + n);
    });
  } catch (const IszError& e) {
    if (why) *why = e.what();
    return std::nullopt;
  }
  return out;
}

// explode refuses `in` with a message holding `expect`.
bool explode_refuses(const Bytes& in, uint32_t size, const std::string& what, const std::string& expect) {
  std::string why;
  if (explode(in, size, &why)) {
    fprintf(stderr, "  %s: decoded, should have been refused\n", what.c_str());
    return false;
  }
  if (why.find(expect) == std::string::npos) {
    fprintf(stderr, "  %s: refused with \"%s\", expected \"%s\"\n", what.c_str(), why.c_str(), expect.c_str());
    return false;
  }
  return true;
}

// The tokens decode to what they say.
bool round_trip(const std::vector<DclToken>& tokens, int dict_bits, const std::string& what) {
  const Bytes want = expand(tokens);
  std::string why;
  auto got = explode(test::dcl_write(tokens, 0, dict_bits), uint32_t(want.size()), &why);
  if (!got) {
    fprintf(stderr, "  %s: refused: %s\n", what.c_str(), why.c_str());
    return false;
  }
  if (*got != want) {
    fprintf(stderr, "  %s: decoded to other bytes\n", what.c_str());
    return false;
  }
  return true;
}

std::vector<IszVolume> as_volumes(const test::IszBuilt& b, const std::string& stem = "T") {
  std::vector<IszVolume> v;
  if (b.volumes.size() == 1) {
    v.push_back({stem + ".LIB", own(b.volumes[0])});
    return v;
  }
  for (size_t i = 0; i < b.volumes.size(); i++) v.push_back({stem + "." + std::to_string(i + 1), own(b.volumes[i])});
  return v;
}

Bytes extract_all(const IszLibrary& lib, const IszMember& m) {
  Bytes out;
  lib.extract(m, [&](const uint8_t* p, size_t n) { out.insert(out.end(), p, p + n); });
  return out;
}

// IszLibrary refuses the volumes with a message holding `expect`.
bool refused(const std::vector<IszVolume>& vols, const std::string& what, const std::string& expect) {
  try {
    IszLibrary lib(vols);
    fprintf(stderr, "  %s: accepted, should have been refused\n", what.c_str());
    return false;
  } catch (const IszError& e) {
    if (std::string(e.what()).find(expect) == std::string::npos) {
      fprintf(stderr, "  %s: refused with \"%s\", expected \"%s\"\n", what.c_str(), e.what(), expect.c_str());
      return false;
    }
    fprintf(stderr, "  %s -> %s\n", what.c_str(), e.what());
    return true;
  }
}

bool refused(const test::IszBuilt& b, const std::string& what, const std::string& expect) {
  return refused(as_volumes(b), what, expect);
}

// Extraction of member `name` fails with a message holding `expect`.
bool extract_refused(const test::IszBuilt& b, const std::string& name, const std::string& what,
                     const std::string& expect) {
  try {
    IszLibrary lib(as_volumes(b));
    const IszMember* m = lib.find(name);
    if (!m) {
      fprintf(stderr, "  %s: no member %s\n", what.c_str(), name.c_str());
      return false;
    }
    extract_all(lib, *m);
    fprintf(stderr, "  %s: extracted, should have failed\n", what.c_str());
    return false;
  } catch (const IszError& e) {
    if (std::string(e.what()).find(expect) == std::string::npos) {
      fprintf(stderr, "  %s: failed with \"%s\", expected \"%s\"\n", what.c_str(), e.what(), expect.c_str());
      return false;
    }
    fprintf(stderr, "  %s -> %s\n", what.c_str(), e.what());
    return true;
  }
}

// ---- the survey's plain files and streams --------------------------------------------------

// A vector's plain file, made the way research/win/pkg/installshield/vectors/
// make_vectors.py made it (the importer tests' own generators).
Bytes plain_file(const std::string& name) {
  auto rep = [](uint8_t b, size_t n) { return Bytes(n, b); };
  if (name == "TEXT700.BIN") return test::arj_vector_text(700, 7);
  if (name == "TXT5000.BIN") return test::arj_vector_text(5000, 3);
  if (name == "PAT2000.BIN") return test::pattern(2000, 1);
  if (name == "RUN1100.BIN") return rep('X', 1100);
  if (name == "FAR6.BIN") {
    const Bytes p = test::pattern(4096, 5);
    return cat(p, Bytes(p.begin(), p.begin() + 700));
  }
  if (name == "FAR4.BIN") {
    const Bytes p = test::pattern(1024, 6);
    return cat(p, Bytes(p.begin(), p.begin() + 300));
  }
  if (name == "ALLBYTES.BIN") {
    Bytes v;
    for (int k = 0; k < 2; k++)
      for (int b = 0; b < 256; b++) v.push_back(uint8_t(b));
    return v;
  }
  if (name == "ONE.BIN") return {'A'};
  if (name == "EMPTY.BIN") return {};
  if (name == "BIG70K.BIN") {
    const Bytes unit = test::arj_vector_text(1500, 21);
    Bytes v;
    for (size_t k = 0; v.size() < 70000; k++) {
      v.insert(v.end(), unit.begin(), unit.end());
      v[v.size() - 1 - (k * 263) % 1500] ^= 0x20;
    }
    v.resize(70000);
    return v;
  }
  abort();
}

// The survey's crafted and damaged streams (make_vectors.py crafted() and
// raw_damage()), written again, with the recorded size each is decoded
// against here.
struct Stream {
  Bytes bytes;
  uint32_t size = 0;
};

Stream survey_stream(const std::string& name) {
  using T = DclToken;
  auto lits = [](const std::string& s) { return test::dcl_literals(s); };
  auto w = [](const std::vector<T>& t, int mode = 0, int k = 6, bool pad = false) {
    return test::dcl_write(t, mode, k, pad);
  };
  if (name == "c_every_length") {
    auto t = test::dcl_literals(test::pattern(300, 9));
    for (uint32_t n : {2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 15, 16, 23, 24, 39, 40, 71, 72, 135, 136, 263, 264, 518})
      t.push_back(T::copy(n, n == 2 ? 3 : 277));
    t = with_end(t);
    return {w(t), uint32_t(expand(t).size())};
  }
  if (name == "c_len2_distances") {
    auto t = test::dcl_literals(test::pattern(256, 10));
    for (uint32_t d : {1, 2, 3, 4, 5, 63, 64, 65, 128, 200, 255, 256}) t.push_back(T::copy(2, d));
    t = with_end(t);
    return {w(t), uint32_t(expand(t).size())};
  }
  for (int k : {4, 5, 6})
    if (name == "c_dist_max_k" + std::to_string(k)) {
      const uint32_t win = 64u << k;
      auto t = test::dcl_literals(test::pattern(win, uint32_t(11 + k)));
      for (const T& c : {T::copy(518, win), T::copy(100, win - 1), T::copy(3, 1), T::copy(9, 64), T::copy(9, 65)})
        t.push_back(c);
      t = with_end(t);
      return {w(t, 0, k), uint32_t(expand(t).size())};
    }
  if (name == "c_overlap_run") {
    auto t = with_end(cat(lits("ab"), {T::copy(518, 1), T::copy(518, 2), T::copy(7, 3)}));
    return {w(t), uint32_t(expand(t).size())};
  }
  if (name == "c_ascii_mode") {
    auto t = with_end(cat(test::dcl_literals(test::arj_vector_text(400, 5)), {T::copy(20, 100)}));
    return {w(t, 1, 6), uint32_t(expand(t).size())};
  }
  if (name == "c_ascii_all_bytes") {
    Bytes all(256);
    std::iota(all.begin(), all.end(), uint8_t(0));
    return {w(with_end(test::dcl_literals(all)), 1, 5), 256};
  }
  if (name == "c_empty") return {w({T::end()}), 0};
  if (name == "c_pad_ones") return {w(with_end(lits("pad")), 0, 6, true), 3};
  if (name == "e_no_end") return {w(lits("no end code here")), 16};
  if (name == "e_too_far") return {w(with_end(cat(lits("abc"), {T::copy(5, 4)}))), 8};
  if (name == "e_too_far_len2") return {w(with_end(cat(lits("a"), {T::copy(2, 2)}))), 3};
  const Bytes ok = w(with_end(lits("trailing")));
  auto head = [&](uint8_t mode, uint8_t k) { return cat({mode, k}, Bytes(ok.begin() + 2, ok.end())); };
  if (name == "e_trailing_byte") return {cat(ok, {0}), 8};
  if (name == "e_mode2") return {head(2, 6), 8};
  if (name == "e_dict3") return {head(0, 3), 8};
  if (name == "e_dict7") return {head(0, 7), 8};
  if (name == "e_one_byte") return {{0}, 0};
  if (name == "e_header_only") return {{0, 6}, 0};
  if (name == "e_cut_in_copy") {
    Bytes b = w(with_end(cat(lits("abcdef"), {T::copy(300, 6)})));
    b.resize(b.size() - 3);
    return {b, 306};
  }
  abort();
}

// What this reader says of each survey stream: "" = decodes as the survey's
// reference did; else the refusal it gives (its own rules: no coded
// literals, zero bits after the end code).
std::string expected_refusal(const std::string& name) {
  if (name == "c_ascii_mode" || name == "c_ascii_all_bytes") return "coded-literal (ASCII) mode is not supported";
  if (name == "c_pad_ones") return "the bits after the end code are not zero";
  if (name == "e_no_end" || name == "e_header_only" || name == "e_cut_in_copy") return "ends before its end code";
  if (name == "e_too_far") return "a copy reaches 4 bytes back with only 3 written";
  if (name == "e_too_far_len2") return "a copy reaches 2 bytes back with only 1 written";
  if (name == "e_trailing_byte") return "1 byte(s) follow the end code";
  if (name == "e_mode2") return "unknown literal mode 2";
  if (name == "e_dict3") return "dictionary bits 3 (only 4, 5 and 6 exist)";
  if (name == "e_dict7") return "dictionary bits 7 (only 4, 5 and 6 exist)";
  if (name == "e_one_byte") return "shorter than its 2-byte header";
  return "";
}

// ---- made-up members for the container tests -------------------------------------------------

struct Member {
  test::IszSpec spec;
  Bytes plain;
};

// A member whose stream is `tokens` (and the end code).
Member member(const std::string& name, const std::vector<DclToken>& tokens, int dict_bits = 6, uint32_t attrs = 0x20,
              uint16_t date = 0x1B8D, uint16_t time = 0xB9A0) {
  Member m;
  m.plain = expand(tokens);
  m.spec.name = name;
  m.spec.size = uint32_t(m.plain.size());
  m.spec.stream = test::dcl_write(with_end(tokens), 0, dict_bits);
  m.spec.attrs = attrs;
  m.spec.date = date;
  m.spec.time = time;
  return m;
}

std::vector<test::IszSpec> specs(const std::vector<Member>& ms) {
  std::vector<test::IszSpec> v;
  for (const Member& m : ms) v.push_back(m.spec);
  return v;
}

// Four members of several shapes: literals and copies (overlapping ones
// too), every dictionary size, attributes 0x00-0x27, '#' and '-' in names
// (as the releases' AVENGE#4.FIF, SHE-HULK.FIF).
std::vector<Member> four_members() {
  using T = DclToken;
  std::vector<Member> v;
  v.push_back(
      member("FIRST.AD", cat(test::dcl_literals(test::pattern(3000, 1)), {T::copy(518, 3000), T::copy(40, 7)})));
  v.push_back(member("AVENGE#4.FIF",
                     cat(test::dcl_literals(test::arj_vector_text(2500, 4)), {T::copy(2, 1), T::copy(300, 1024)}), 4,
                     0x00));
  v.push_back(member("SHE-HULK.FIF",
                     cat(test::dcl_literals(test::pattern(2100, 3)), {T::copy(100, 1700), T::copy(3, 2048)}), 5, 0x27,
                     0x1B6C, 0x7000));
  v.push_back(member("LAST.TXT", test::dcl_literals("the last member\r\n"), 6, 0x01));
  return v;
}

// Every member of the library decodes to its plain bytes.
bool all_extract(const IszLibrary& lib, const std::vector<Member>& ms, const std::string& what) {
  bool ok = lib.members().size() == ms.size();
  for (size_t i = 0; ok && i < ms.size(); i++) {
    const IszMember& m = lib.members()[i];
    try {
      ok = m.name == ms[i].spec.name && m.size == ms[i].plain.size() && extract_all(lib, m) == ms[i].plain;
    } catch (const IszError& e) {
      fprintf(stderr, "  %s: %s\n", what.c_str(), e.what());
      ok = false;
    }
  }
  if (!ok) fprintf(stderr, "  %s: the members are not what was laid out\n", what.c_str());
  return ok;
}

// An IszError, or exactly the recorded size of every member; anything else
// (another exception, a size mismatch without an error) is a failure.
struct FlipCount {
  size_t tried = 0, refused = 0, decoded = 0, bad = 0;
};

void flip_library(const std::vector<IszVolume>& vols, FlipCount& c, const std::string& what) {
  c.tried++;
  try {
    IszLibrary lib(vols);
    for (const IszMember& m : lib.members()) {
      uint64_t got = 0;
      lib.extract(m, [&](const uint8_t*, size_t n) { got += n; });
      if (got != m.size) {
        if (c.bad++ < 10)
          fprintf(stderr, "  %s: %s gave %llu bytes, %u recorded\n", what.c_str(), m.name.c_str(),
                  (unsigned long long)got, unsigned(m.size));
      }
    }
    c.decoded++;
  } catch (const IszError&) {
    c.refused++;
  } catch (const std::exception& e) {
    if (c.bad++ < 10) fprintf(stderr, "  %s: %s\n", what.c_str(), e.what());
  }
}

void flip_stream(const Bytes& in, uint32_t size, FlipCount& c, const std::string& what) {
  c.tried++;
  try {
    uint64_t got = 0;
    isz_detail::explode(in, size, [&](const uint8_t*, size_t n) { got += n; });
    if (got != size && c.bad++ < 10)
      fprintf(stderr, "  %s: %llu bytes, %u recorded\n", what.c_str(), (unsigned long long)got, unsigned(size));
    c.decoded++;
  } catch (const IszError&) {
    c.refused++;
  } catch (const std::exception& e) {
    if (c.bad++ < 10) fprintf(stderr, "  %s: %s\n", what.c_str(), e.what());
  }
}

int real_test(int argc, char** argv);

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 && std::string(argv[1]) == "real") return real_test(argc - 1, argv + 1);
  // No default may reach the real data folder.
  test::sandbox_data_root(test::scratch(argc, argv, "adw-import-isz") / L"localappdata");

  // ---- the survey's ICOMP libraries ------------------------------------------------------------
  size_t icomp = 0;
  for (const test::IszLibraryVector& v : test::kIszLibraryVectors) {
    const Bytes lib = test::unhex(v.hex);
    const Bytes plain = plain_file(v.plain);
    CHECK_EQ(md5_of(lib), std::string(v.library_md5));
    CHECK_EQ(md5_of(plain), std::string(v.plain_md5));  // the generators are the survey's
    CHECK_EQ(plain.size(), size_t(v.plain_size));
    const std::vector<IszVolume> vols = {{v.library, own(lib)}};
    if (!v.dict_bits) {
      CHECK(refused(vols, v.library, "stored (uncompressed) members are not supported"));
      continue;
    }
    try {
      IszLibrary z(vols);
      CHECK_EQ(z.members().size(), size_t(1));
      const IszMember& m = z.members().at(0);
      CHECK_EQ(m.name, std::string(v.plain));
      CHECK_EQ(m.size, v.plain_size);
      CHECK_EQ(m.csize, v.stream_size);
      CHECK_EQ(m.dos_datetime, (0x1D53u << 16) | 0x5E6Cu);  // 1994-10-19 11:51:24, make_vectors.py's time
      CHECK_EQ(m.attributes, uint32_t(0x20));
      CHECK_EQ(m.volumes, std::string(v.library));
      CHECK(m.segments.size() == 1 && m.segments[0].volume == 0 && m.segments[0].offset == 255 &&
            m.segments[0].csize == v.stream_size);
      const Bytes stream(lib.begin() + 255, lib.begin() + 255 + v.stream_size);
      CHECK_EQ(md5_of(stream), std::string(v.stream_md5));
      CHECK_EQ(int(stream.at(1)), v.dict_bits);
      size_t chunks = 0, biggest = 0;
      Bytes got;
      z.extract(m, [&](const uint8_t* p, size_t n) {
        chunks++;
        biggest = std::max(biggest, n);
        got.insert(got.end(), p, p + n);
      });
      CHECK(got == plain);
      CHECK(biggest <= 65536);
      if (plain.size() > 65536) CHECK(chunks >= 2);
      CHECK(explode(stream, v.plain_size) == plain);
      const IszHeader h = isz_header(lib, v.library);
      CHECK(!h.split && h.volume == 0 && h.volumes == 0 && h.files == 1);
      CHECK_EQ(h.library_size, uint32_t(lib.size()));
      CHECK_EQ(h.total_size, v.plain_size);
      icomp++;
    } catch (const IszError& e) {
      test::g_failures++;
      fprintf(stderr, "  %s: %s\n", v.library, e.what());
    }
  }
  fprintf(stderr, "  ICOMP libraries: %zu decoded\n", icomp);
  CHECK_EQ(icomp, size_t(19));

  // ---- the survey's crafted and damaged streams --------------------------------------------------
  for (const test::DclStreamVector& v : test::kDclStreamVectors) {
    const Stream s = survey_stream(v.name);
    // The port writes what tools/dclwrite.py wrote, byte for byte.
    CHECK_EQ(s.bytes.size(), size_t(v.stream_size));
    CHECK_EQ(md5_of(s.bytes), std::string(v.stream_md5));
    const std::string refusal = expected_refusal(v.name);
    if (refusal.empty()) {
      CHECK(v.decodes);
      CHECK_EQ(s.size, v.size);
      std::string why;
      auto got = explode(s.bytes, s.size, &why);
      if (!got) fprintf(stderr, "  %s: %s\n", v.name, why.c_str());
      CHECK(got && md5_of(*got) == v.md5);
    } else {
      CHECK(explode_refuses(s.bytes, s.size, v.name, refusal));
    }
  }

  // ---- explode: the header ----------------------------------------------------------------------------
  {
    const Bytes body = test::dcl_write(with_end(test::dcl_literals("header")));
    for (int mode = 0; mode < 256; mode++) {
      Bytes b = body;
      b[0] = uint8_t(mode);
      if (mode == 0)
        CHECK(explode(b, 6) == Bytes({'h', 'e', 'a', 'd', 'e', 'r'}));
      else if (mode == 1)
        CHECK(explode_refuses(b, 6, "mode 1", "coded-literal (ASCII) mode is not supported"));
      else
        CHECK(explode_refuses(b, 6, "mode " + std::to_string(mode), "unknown literal mode " + std::to_string(mode)));
    }
    for (int k = 0; k < 256; k++) {
      Bytes b = body;
      b[1] = uint8_t(k);
      if (k >= 4 && k <= 6)
        CHECK(explode(b, 6).has_value());
      else
        CHECK(explode_refuses(b, 6, "dictionary " + std::to_string(k),
                              "dictionary bits " + std::to_string(k) + " (only"));
    }
    CHECK(explode_refuses({}, 0, "no bytes", "shorter than its 2-byte header"));
    CHECK(explode_refuses({0}, 0, "one byte", "shorter than its 2-byte header"));
    CHECK(explode_refuses({0, 6}, 0, "the header alone", "ends before its end code"));
  }

  // ---- explode: lengths, distances, overlaps --------------------------------------------------------
  {
    using T = DclToken;
    // Every length, each as a copy of distance 1 (overlapping itself) and
    // of a distance longer than the length.
    for (int k : {4, 5, 6}) {
      auto t = test::dcl_literals(test::pattern(600, 21));
      for (uint32_t len = 2; len <= 518; len++) t.push_back(T::copy(len, len == 2 ? 1 : 1 + len % 200));
      for (uint32_t len = 3; len <= 518; len++) t.push_back(T::copy(len, 1 + len));
      CHECK(round_trip(with_end(t), k, "every length, dictionary " + std::to_string(k)));
    }
    // Every distance each dictionary size can write (length 3), and every
    // distance of a length-2 copy (1..256).
    for (int k : {4, 5, 6}) {
      const uint32_t win = 64u << k;
      auto t = test::dcl_literals(test::pattern(win, uint32_t(30 + k)));
      for (uint32_t d = 1; d <= win; d++) t.push_back(T::copy(3, d));
      for (uint32_t d = 1; d <= 256; d++) t.push_back(T::copy(2, d));
      CHECK(round_trip(with_end(t), k, "every distance, dictionary " + std::to_string(k)));
    }
    // A copy may reach exactly the first byte written, not one byte further.
    for (int k : {4, 5, 6}) {
      for (uint32_t written : {1u, 2u, 3u, 64u, 65u, 1024u}) {
        if (written > (64u << k)) continue;
        auto lits = test::dcl_literals(test::pattern(written, written));
        CHECK(round_trip(with_end(cat(lits, {T::copy(4, written)})), k, "a copy of the first byte"));
        if (written + 1 <= (64u << k)) {  // a distance the dictionary size can write
          const auto beyond = with_end(cat(lits, {T::copy(4, written + 1)}));
          CHECK(explode_refuses(test::dcl_write(beyond, 0, k), written + 4, "a copy before the first byte",
                                "a copy reaches " + std::to_string(written + 1) + " bytes back with only " +
                                    std::to_string(written) + " written"));
        }
        if (written < 256) {
          const auto beyond2 = with_end(cat(lits, {T::copy(2, written + 1)}));
          CHECK(explode_refuses(test::dcl_write(beyond2, 0, k), written + 2, "a length-2 copy before the first byte",
                                "a copy reaches"));
        }
      }
    }
    // A copy at the very start.
    CHECK(explode_refuses(test::dcl_write(with_end({T::copy(3, 1)})), 3, "a copy of nothing",
                          "a copy reaches 1 bytes back with only 0 written"));
  }

  // ---- explode: the recorded size and the end code -------------------------------------------------------
  {
    using T = DclToken;
    const auto t = with_end(cat(test::dcl_literals("recorded"), {T::copy(10, 4)}));  // 18 bytes
    const Bytes s = test::dcl_write(t);
    CHECK(explode(s, 18) == expand(t));
    // A literal past the size, a copy past it, the end code before it.
    CHECK(explode_refuses(s, 7, "a literal past the size", "expands past its recorded size, 7 bytes"));
    CHECK(explode_refuses(s, 8, "a copy past the size", "expands past its recorded size, 8 bytes"));
    CHECK(explode_refuses(s, 17, "a copy one byte past the size", "expands past its recorded size, 17 bytes"));
    CHECK(explode_refuses(s, 19, "the end code early", "the data ends after 18 bytes, 19 recorded"));
    CHECK(explode_refuses(s, 0, "size 0", "expands past its recorded size, 0 bytes"));
    // No byte past the size ever reaches the sink.
    {
      uint64_t got = 0;
      try {
        isz_detail::explode(s, 12, [&](const uint8_t*, size_t n) { got += n; });
      } catch (const IszError&) {
      }
      CHECK(got <= 12);
    }
    // Cut at every byte: every prefix ends before the end code.
    for (size_t cut = 2; cut < s.size(); cut++)
      CHECK(explode_refuses(Bytes(s.begin(), s.begin() + ptrdiff_t(cut)), 18, "cut at " + std::to_string(cut),
                            "ends before its end code"));
    // Any byte after the end code.
    for (int b = 0; b < 256; b++)
      CHECK(explode_refuses(cat(s, {uint8_t(b)}), 18, "a byte after", "1 byte(s) follow the end code"));
    CHECK(explode_refuses(cat(s, {0, 0}), 18, "two bytes after", "2 byte(s) follow the end code"));
    // The end code leaves 0..7 bits of its last byte (9 bits a literal, 16
    // the end code): each of those bits set is refused, zero is decoded.
    for (size_t c = 0; c < 8; c++) {
      const Bytes plain = test::pattern(c, uint32_t(c + 40));
      const Bytes zero = test::dcl_write(with_end(test::dcl_literals(plain)));
      const size_t pad = (8 - c % 8) % 8;
      CHECK(explode(zero, uint32_t(c)) == plain);
      for (size_t bit = 0; bit < pad; bit++) {
        Bytes one = zero;
        one.back() = uint8_t(one.back() | (0x80 >> bit));
        CHECK(explode_refuses(one, uint32_t(c), "padding bit " + std::to_string(bit) + " of " + std::to_string(pad),
                              "the bits after the end code are not zero"));
      }
    }
    // The empty member: the end code alone.
    CHECK(explode(test::dcl_write({T::end()}), 0) == Bytes());
  }

  // ---- explode: chunks ---------------------------------------------------------------------------------
  {
    using T = DclToken;
    auto t = test::dcl_literals(test::pattern(4096, 77));
    for (int i = 0; i < 600; i++) t.push_back(T::copy(518, 4096 - uint32_t(i)));
    t = with_end(t);
    const Bytes want = expand(t);
    size_t chunks = 0, biggest = 0;
    auto got = explode(test::dcl_write(t), uint32_t(want.size()), nullptr, &chunks, &biggest);
    CHECK(got && *got == want);
    CHECK(biggest <= 65536 && chunks >= want.size() / 65536 + 1);
    fprintf(stderr, "  %zu bytes in %zu chunks, the largest %zu\n", want.size(), chunks, biggest);
    // Whatever the sink throws reaches the caller as it is.
    struct Stop {};
    bool stopped = false;
    try {
      isz_detail::explode(test::dcl_write(t), uint32_t(want.size()), [](const uint8_t*, size_t) { throw Stop{}; });
    } catch (const Stop&) {
      stopped = true;
    }
    CHECK(stopped);
  }

  // ---- the container: an unsplit library ------------------------------------------------------------
  const std::vector<Member> four = four_members();
  {
    test::IszBuilt b = test::isz_library(specs(four));
    IszLibrary lib(as_volumes(b, "MODULES"));
    CHECK(all_extract(lib, four, "unsplit library"));
    CHECK_EQ(lib.volumes().size(), size_t(1));
    size_t off = 255;
    for (size_t i = 0; i < four.size(); i++) {
      const IszMember& m = lib.members()[i];
      CHECK_EQ(m.csize, uint32_t(four[i].spec.stream.size()));
      CHECK_EQ(m.attributes, four[i].spec.attrs);
      CHECK_EQ(m.dos_datetime, uint32_t(four[i].spec.date) << 16 | four[i].spec.time);
      CHECK_EQ(m.volumes, std::string("MODULES.LIB"));
      CHECK(m.segments.size() == 1 && m.segments[0].volume == 0 && m.segments[0].offset == off);
      off += m.csize;
    }
    // Case-insensitive lookup; members keep their stored names.
    CHECK(lib.find("she-hulk.fif") && lib.find("she-hulk.fif")->name == "SHE-HULK.FIF");
    CHECK(lib.find("Avenge#4.Fif") == &lib.members()[1]);
    CHECK(!lib.find("NOPE.AD"));
    // The header alone.
    const IszHeader h = isz_header(b.volumes[0], "MODULES.LIB");
    CHECK(!h.split && h.volume == 0 && h.volumes == 0 && h.files == 4);
    CHECK_EQ(h.dos_datetime, (0x1B8Du << 16) | 0xBABDu);
    CHECK_EQ(h.library_size, uint32_t(b.volumes[0].size()));
    CHECK_EQ(h.total_size,
             uint32_t(four[0].plain.size() + four[1].plain.size() + four[2].plain.size() + four[3].plain.size()));
    // ICOMP writes 0 at +0x1B, the releases 1: both are read.
    test::IszBuilt zero = test::isz_library(specs(four), {}, 0x1B8D, 0xBABD, 0);
    CHECK(all_extract(IszLibrary(as_volumes(zero)), four, "byte 0x1B zero"));
    // One member, and an empty one.
    test::IszBuilt one = test::isz_library({member("EMPTY.BIN", {}).spec});
    IszLibrary lone(as_volumes(one));
    CHECK(lone.members().size() == 1 && extract_all(lone, lone.members()[0]).empty());
    // A member name in code page 437 is UTF-8 afterwards (0x81: u-umlaut).
    test::IszBuilt cp = test::isz_library({member("M\x81SIK.AD", test::dcl_literals("x")).spec});
    CHECK_EQ(IszLibrary(as_volumes(cp)).members()[0].name, std::string("M\xC3\xBCSIK.AD"));
  }

  // ---- the container: split sets ----------------------------------------------------------------------
  {
    // Two volumes: the boundary at every byte of the second member (its
    // first byte and its last excepted: the boundary would lie between two
    // members), so the cut falls inside the 2-byte header, a literal, a
    // copy's codes and the end code.
    const size_t start = four[0].spec.stream.size(), len = four[1].spec.stream.size();
    size_t cuts = 0;
    for (size_t c = start + 1; c < start + len; c++) {
      test::IszBuilt b = test::isz_split(specs(four), {c});
      try {
        IszLibrary lib(as_volumes(b, "IMAGES"));
        if (!all_extract(lib, four, "cut at " + std::to_string(c))) continue;
        const IszMember& m = lib.members()[1];
        CHECK(m.segments.size() == 2 && m.segments[0].volume == 0 && m.segments[0].csize == c - start &&
              m.segments[1].volume == 1 && m.segments[1].offset == 255 && m.segments[1].csize == start + len - c);
        CHECK_EQ(m.volumes, std::string("IMAGES.1+IMAGES.2"));
        CHECK_EQ(lib.members()[0].volumes, std::string("IMAGES.1"));
        CHECK_EQ(lib.members()[3].volumes, std::string("IMAGES.2"));
        cuts++;
      } catch (const IszError& e) {
        test::g_failures++;
        fprintf(stderr, "  cut at %zu: %s\n", c, e.what());
      }
    }
    CHECK_EQ(cuts, len - 1);
    // Three volumes, a boundary inside members 1 and 2, given in every order.
    const size_t s1 = start + 5, s2 = (four[1].spec.stream.size() - 5) + (four[2].spec.stream.size() - 3);
    test::IszBuilt three = test::isz_split(specs(four), {s1, s2});
    CHECK_EQ(three.volumes.size(), size_t(3));
    std::vector<IszVolume> vols = as_volumes(three, "AD_MODS");
    std::vector<size_t> order = {0, 1, 2};
    size_t orders = 0;
    do {
      std::vector<IszVolume> given;
      for (size_t i : order) given.push_back(vols[i]);
      try {
        IszLibrary lib(given);
        CHECK(lib.volumes()[0].name == "AD_MODS.1" && lib.volumes()[2].name == "AD_MODS.3");
        CHECK(all_extract(lib, four, "three volumes"));
        CHECK_EQ(lib.members()[1].volumes, std::string("AD_MODS.1+AD_MODS.2"));
        CHECK_EQ(lib.members()[2].volumes, std::string("AD_MODS.2+AD_MODS.3"));
        orders++;
      } catch (const IszError& e) {
        test::g_failures++;
        fprintf(stderr, "  three volumes: %s\n", e.what());
      }
    } while (std::next_permutation(order.begin(), order.end()));
    CHECK_EQ(orders, size_t(6));
    // The headers of a set.
    const IszHeader h1 = isz_header(three.volumes[0], "AD_MODS.1"), h3 = isz_header(three.volumes[2], "AD_MODS.3");
    CHECK(h1.split && h1.volume == 1 && h1.volumes == 3 && h1.files == 4);
    CHECK(h3.split && h3.volume == 3 && h3.volumes == 0 && h3.library_size == h1.library_size);
    // Volume names.
    CHECK(isz_volume_name("IMAGES.1", 2) == std::optional<std::string>("IMAGES.2"));
    CHECK(isz_volume_name("ad_mods.1", 12) == std::optional<std::string>("ad_mods.12"));
    CHECK(!isz_volume_name("MODULES.LIB", 2) && !isz_volume_name("NOEXT", 2) && !isz_volume_name("X.", 2));
  }

  // ---- refused by name -----------------------------------------------------------------------------
  const test::IszBuilt base = test::isz_library(specs(four));
  const test::IszBuilt split = test::isz_split(specs(four), {four[0].spec.stream.size() + 100});
  auto patched = [](test::IszBuilt b, const std::function<void(test::IszBuilt&)>& f) {
    f(b);
    return b;
  };
  auto header = [&](const test::IszBuilt& from, size_t v, size_t at, uint32_t x, int width) {
    return patched(from, [&](test::IszBuilt& b) { test::isz_set(b.volumes[v], at, x, width); });
  };
  auto entry = [&](const test::IszBuilt& from, size_t i, size_t field, uint32_t x, int width) {
    return patched(from, [&](test::IszBuilt& b) { test::isz_set_entry(b, i, field, x, width); });
  };
  {
    CHECK(refused(header(base, 0, 0x09, 1, 1), "a password", "password-protected libraries are not supported"));
    CHECK(refused(header(split, 1, 0x09, 1, 1), "a password in volume 2",
                  "password-protected libraries are not supported"));
    CHECK(refused(entry(base, 2, 0x19, 0x0010, 2), "a stored member",
                  "SHE-HULK.FIF: stored (uncompressed) members are not supported"));
    auto named = specs(four);
    CHECK(refused(test::isz_library(named, {{"SUB", 4}}), "a named directory", "named directories are not supported"));
    named[3].dir = 1;
    CHECK(refused(test::isz_library(named, {{"", 3}, {"", 1}}), "two directories",
                  "2 directories (only a library of one unnamed directory is supported)"));
    // A member spanning three volumes: the cuts both inside member 1.
    const size_t s = four[0].spec.stream.size();
    CHECK(refused(test::isz_split(specs(four), {s + 10, 20}), "a member over three volumes",
                  "spans 3 volumes (a member spanning more than two is not supported)"));
    // A boundary between two members (in either volume's view).
    CHECK(refused(test::isz_split(specs(four), {s}), "a boundary between members",
                  "the volume ends between two members"));
    CHECK(refused(test::isz_split(specs(four), {s + four[1].spec.stream.size()}), "a boundary between members 2 and 3",
                  "the volume ends between two members"));
    // Coded literals inside a library: refused when extracted.
    Member m = member("CODED.TXT", test::dcl_literals("coded"));
    m.spec.stream = test::dcl_write(with_end(test::dcl_literals("coded")), 1, 6);
    CHECK(extract_refused(test::isz_library({m.spec}), "CODED.TXT", "coded literals",
                          "T.LIB!CODED.TXT: PKWARE's coded-literal (ASCII) mode is not supported"));
    // A member damaged past its end code, extracted from a split set: the
    // message names both volumes.
    std::vector<Member> damaged = four;
    damaged[1].spec.size += 1;
    damaged[2].spec.size -= 1;
    CHECK(extract_refused(test::isz_split(specs(damaged), {four[0].spec.stream.size() + 100}), "AVENGE#4.FIF",
                          "a short member", "T.1+T.2!AVENGE#4.FIF: the data ends after"));
  }

  // ---- the header's rules ----------------------------------------------------------------------------
  {
    for (size_t i = 0; i < 8; i++)
      CHECK(refused(header(base, 0, i, base.volumes[0][i] ^ 0x40, 1), "signature byte " + std::to_string(i),
                    "no signature"));
    CHECK(refused(test::IszBuilt{{Bytes(base.volumes[0].begin(), base.volumes[0].begin() + 254)}, {}}, "254 bytes",
                  "not an InstallShield compressed library (too small)"));
    CHECK(refused(test::IszBuilt{{test::pattern(4000, 3)}, {}}, "not a library", "no signature"));
    CHECK(refused(header(base, 0, 0x08, 1, 1), "byte 8", "damaged header (byte 0x08"));
    for (size_t at = 0x3F; at < 0xFF; at++)
      CHECK(refused(header(base, 0, at, 0x01, 1), "padding byte " + std::to_string(at), "is not zero)"));
    CHECK(refused(header(base, 0, 0x0A, 2, 2), "flags 2", "unknown library flags 0x0002"));
    CHECK(refused(header(base, 0, 0x0A, 0x100, 2), "flags 0x100", "unknown library flags 0x0100"));
    // Dates: month 0 and 13, day 0, hour 24, minute 60, second 60.
    const uint16_t date = 0x1B8D, time = 0xBABD;
    for (uint32_t d : {uint32_t(date & ~(15u << 5)), uint32_t((date & ~(15u << 5)) | 13u << 5), uint32_t(date & ~31u)})
      CHECK(refused(header(base, 0, 0x0E, d, 2), "library date " + std::to_string(d),
                    "damaged header (an invalid date)"));
    for (uint32_t t : {uint32_t((time & 0x07FF) | 24u << 11), uint32_t((time & ~(63u << 5)) | 60u << 5),
                       uint32_t((time & ~31u) | 30u)})
      CHECK(refused(header(base, 0, 0x10, t, 2), "library time " + std::to_string(t),
                    "damaged header (an invalid date)"));
    // An unsplit library's split-set fields.
    for (const auto& [at, x, width] : std::vector<std::tuple<size_t, uint32_t, int>>{
             {0x1E, 2, 1}, {0x1F, 1, 1}, {0x20, 1, 1}, {0x3B, 1, 4}, {0x25, 1, 4}, {0x21, 0, 4}, {0x1A, 0, 4}})
      CHECK(refused(header(base, 0, at, x, width), "unsplit, field " + std::to_string(at),
                    "split-set fields in an unsplit library"));
    // Its size.
    CHECK(refused(header(base, 0, 0x12, uint32_t(base.volumes[0].size() + 1), 4), "library size",
                  "the header records " + std::to_string(base.volumes[0].size() + 1) + " (truncated or padded)"));
    CHECK(refused(patched(base, [](test::IszBuilt& b) { b.volumes[0].push_back(0); }), "a byte appended",
                  "(truncated or padded)"));
    CHECK(refused(patched(base, [](test::IszBuilt& b) { b.volumes[0].pop_back(); }), "a byte cut off",
                  "(truncated or padded)"));
    // The tables where the header says.
    const uint32_t dir_off = test::isz_get(base.volumes[0], 0x29, 4);
    CHECK(
        refused(header(base, 0, 0x29, dir_off + 1, 4), "directory offset", "the tables are not where the header says"));
    CHECK(refused(header(base, 0, 0x2D, 12, 4), "directory size", "the tables are not where the header says"));
    CHECK(refused(header(base, 0, 0x33, test::isz_get(base.volumes[0], 0x33, 4) - 1, 4), "file table offset",
                  "the tables are not where the header says"));
    CHECK(refused(header(base, 0, 0x37, test::isz_get(base.volumes[0], 0x37, 4) + 1, 4), "file table size",
                  "the tables are not where the header says"));
    CHECK(refused(header(header(header(base, 0, 0x29, 254, 4), 0, 0x2D, 11 + dir_off - 254, 4), 0, 0x12,
                         test::isz_get(base.volumes[0], 0x12, 4), 4),
                  "a directory table inside the header", "the tables are not where the header says"));
    // A split set's header.
    CHECK(refused(header(split, 1, 0x1F, 0, 1), "volume 0 of a set", "volume 0 of a split set"));
    CHECK(refused(header(split, 0, 0x1E, 1, 1), "volume 1 of a set of 1", "volume 1 of a set of 1"));
    CHECK(refused(header(split, 1, 0x1E, 2, 1), "a volume count in volume 2", "a volume count in volume 2"));
    CHECK(refused(header(split, 0, 0x20, test::isz_get(split.volumes[0], 0x20, 1) + 1, 1), "check byte", "check byte"));
    CHECK(refused(header(split, 0, 0x1A, 256, 4), "volume 1's field 0x1A", "field 0x1A is 256"));
    CHECK(refused(header(split, 1, 0x1A, test::isz_get(split.volumes[1], 0x1A, 4) + 1, 4), "volume 2's field 0x1A",
                  "not the continued part and the tables"));
    CHECK(refused(header(split, 1, 0x3B, test::isz_get(split.volumes[1], 0x3B, 4) + 1, 4), "volume size",
                  "(truncated or padded)"));
  }

  // ---- the tables' rules ----------------------------------------------------------------------------
  {
    const uint32_t dir_off = test::isz_get(base.volumes[0], 0x29, 4);
    CHECK(refused(header(base, 0, dir_off + 2, 12, 2), "directory entry size", "damaged directory table (entry size)"));
    CHECK(refused(header(base, 0, dir_off + 6, 'X', 1), "directory name's end",
                  "damaged directory table (the name's end)"));
    for (size_t k = 7; k < 11; k++)
      CHECK(refused(header(base, 0, dir_off + k, 1, 1), "directory reserved byte",
                    "damaged directory table (the name's end)"));
    CHECK(refused(header(base, 0, dir_off, 5, 2), "directory file count",
                  "the directory holds 5 files, the header records 4"));
    CHECK(refused(header(base, 0, 0x31, 0, 2), "no directory", "0 directories"));
    // A byte between the directory entry and the file table.
    CHECK(refused(patched(base,
                          [&](test::IszBuilt& b) {
                            Bytes& v = b.volumes[0];
                            v.insert(v.begin() + ptrdiff_t(dir_off + 11), 0);
                            test::isz_set(v, 0x2D, 12, 4);
                            test::isz_set(v, 0x33, test::isz_get(v, 0x33, 4) + 1, 4);
                            test::isz_set(v, 0x12, test::isz_get(v, 0x12, 4) + 1, 4);
                          }),
                  "a byte after the directory entry", "damaged directory table (1 byte(s) after its entry)"));
    // A byte after the last file entry.
    CHECK(refused(patched(base,
                          [&](test::IszBuilt& b) {
                            Bytes& v = b.volumes[0];
                            v.push_back(0);
                            test::isz_set(v, 0x37, test::isz_get(v, 0x37, 4) + 1, 4);
                            test::isz_set(v, 0x12, test::isz_get(v, 0x12, 4) + 1, 4);
                          }),
                  "a byte after the file entries", "damaged file table (1 byte(s) after its entries)"));
    // Entries.
    CHECK(refused(entry(base, 1, 0x17, 43 + 12 + 1, 2), "entry size", "damaged file table (entry 1) (entry size)"));
    CHECK(refused(entry(base, 1, 0x1E + 12, 'X', 1), "entry name's end",
                  "damaged file table (entry 1) (the name's end)"));
    for (size_t k = 0; k < 12; k++)
      CHECK(refused(entry(base, 3, 0x1F + 8 + k, 1, 1), "version byte " + std::to_string(k),
                    "damaged file table (entry 3) (version fields not zero)"));
    CHECK(refused(entry(base, 0, 0x01, 1, 2), "directory 1", "FIRST.AD: damaged entry (directory 1 of 1)"));
    for (uint32_t f : {0x0001u, 0x0020u, 0x0040u, 0x0200u, 0x8000u})
      CHECK(refused(entry(base, 0, 0x19, f, 2), "flags " + std::to_string(f), "FIRST.AD: unsupported flags"));
    for (uint32_t a : {0x08u, 0x10u, 0x40u, 0x80u, 0x100u})
      CHECK(refused(entry(base, 0, 0x13, a, 4), "attributes " + std::to_string(a), "FIRST.AD: unsupported attributes"));
    for (uint32_t x : {2u, 0xFFu})
      CHECK(refused(entry(base, 0, 0x1B, x, 1), "byte 0x1B", "damaged entry (byte 0x1B is"));
    CHECK(refused(entry(base, 2, 0x0F, 0x1B8D & ~(15u << 5), 2), "member date",
                  "SHE-HULK.FIF: damaged entry (an invalid date)"));
    CHECK(refused(entry(base, 2, 0x11, 24u << 11, 2), "member time", "SHE-HULK.FIF: damaged entry (an invalid date)"));
    // Names.
    auto named = [&](const std::string& name) {
      auto s = specs(four);
      s[2].name = name;
      return test::isz_library(s);
    };
    CHECK(refused(named(""), "an empty name", "(an empty name, or a NUL inside it)"));
    CHECK(refused(named(std::string("A\0B", 3)), "a NUL inside a name", "(an empty name, or a NUL inside it)"));
    for (const char* hostile : {"..", ".", "CON", "NUL.AD", "COM1.DLL", "TRAIL.", "SPACE ", "TAB\t.AD"})
      CHECK(refused(named(hostile), hostile, "unusable file name"));
    for (const char* path : {"SUB/X.AD", "SUB\\X.AD", "../X.AD", "A:B", "C:EVIL.AD"})
      CHECK(refused(named(path), path, "is not a bare file name"));
    CHECK(refused(named("first.ad"), "two names", "two members are named first.ad"));
    // Code page 437's u-umlaut and U-umlaut are one name to Windows.
    auto two = specs(four);
    two[0].name = "\x81.AD";
    two[1].name = "\x9A.AD";
    CHECK(refused(test::isz_library(two), "u-umlaut and U-umlaut", "two members are named \xC3\x9C.AD"));
  }

  // ---- the layout's rules ------------------------------------------------------------------------------
  {
    CHECK(refused(entry(base, 1, 0x0B, test::isz_get(base.volumes[0], test::isz_entry_at(base, 0, 1) + 0x0B, 4) + 1, 4),
                  "an offset", "AVENGE#4.FIF: its data is at"));
    CHECK(
        refused(entry(base, 0, 0x03, uint32_t(four[0].plain.size() + 1), 4), "a size", "the members' sizes add up to"));
    CHECK(refused(entry(base, 0, 0x07, uint32_t(four[0].spec.stream.size() + 1), 4), "a compressed size",
                  "the members' data and the tables add up to"));
    CHECK(refused(entry(base, 3, 0x1C, 1, 1), "a first volume in an unsplit library",
                  "volume fields in an unsplit library"));
    CHECK(refused(entry(base, 3, 0x00, 1, 1), "a last volume in an unsplit library",
                  "volume fields in an unsplit library"));
    CHECK(refused(entry(base, 3, 0x19, 0x0100, 2), "continued in an unsplit library",
                  "volume fields in an unsplit library"));
    // Split sets.
    const size_t head = 100;  // member 1's bytes in volume 1
    CHECK(refused(entry(split, 0, 0x1C, 0, 1), "first volume 0", "damaged entry (volumes 0 to 1 of a set of 2)"));
    CHECK(refused(entry(split, 3, 0x00, 3, 1), "last volume 3", "damaged entry (volumes 2 to 3 of a set of 2)"));
    CHECK(refused(entry(split, 3, 0x1C, 3, 1), "first after last", "damaged entry (volumes 3 to 2 of a set of 2)"));
    CHECK(refused(entry(split, 1, 0x19, 0, 2), "a continuing member without the flag",
                  "(volumes 1 to 2, not continued)"));
    CHECK(refused(entry(entry(split, 0, 0x19, 0x0100, 2), 0, 0x00, 1, 1), "the flag on a member in one volume",
                  "(volumes 1 to 1, continued)"));
    CHECK(refused(entry(entry(split, 0, 0x1C, 2, 1), 0, 0x00, 2, 1), "a member of volume 2 before volume 1's",
                  "unused byte(s) before its tables"));
    CHECK(refused(entry(entry(split, 2, 0x1C, 1, 1), 2, 0x00, 1, 1), "a member of volume 1 after the boundary",
                  "out of order"));
    CHECK(refused(header(split, 0, 0x21, test::isz_get(split.volumes[0], 0x21, 4) + 1, 4), "volume 1's split start",
                  "its header says the member continuing into the next volume starts at"));
    CHECK(refused(header(header(split, 1, 0x25, test::isz_get(split.volumes[1], 0x25, 4) + 1, 4), 1, 0x1A,
                         test::isz_get(split.volumes[1], 0x1A, 4) + 1, 4),
                  "volume 2's continued end", "its header says the part continued from T.1 ends at"));
    CHECK(refused(header(split, 1, 0x21, 300, 4), "the last volume's split start",
                  "a member continues past the last volume"));
    // Volume 1's field 0x25 is left over in the releases: never read.
    CHECK(all_extract(IszLibrary(as_volumes(header(split, 0, 0x25, 0xEA4, 4))), four, "volume 1's field 0x25"));
    // A member of volume 1 reaching into its tables (the sums still right).
    CHECK(refused(entry(entry(split, 0, 0x07, uint32_t(four[0].spec.stream.size() + head + 1), 4), 2, 0x07,
                        uint32_t(four[2].spec.stream.size() - head - 1), 4),
                  "a member into volume 1's tables", "FIRST.AD: its data runs into the tables"));
    // A byte in volume 2 that no member holds.
    CHECK(refused(patched(split,
                          [&](test::IszBuilt& b) {
                            Bytes& v = b.volumes[1];
                            const uint32_t d = test::isz_get(v, 0x29, 4);
                            v.insert(v.begin() + ptrdiff_t(d), 0);
                            test::isz_set(v, 0x29, d + 1, 4);
                            test::isz_set(v, 0x33, test::isz_get(v, 0x33, 4) + 1, 4);
                            test::isz_set(v, 0x3B, test::isz_get(v, 0x3B, 4) + 1, 4);
                          }),
                  "a stray byte in volume 2", "T.2: 1 unused byte(s) before its tables"));
  }

  // ---- the volumes of a set -----------------------------------------------------------------------
  {
    const std::vector<IszVolume> vols = as_volumes(split);
    CHECK(refused(std::vector<IszVolume>{}, "no volumes", "no InstallShield library volumes"));
    CHECK(refused(std::vector<IszVolume>{vols[0]}, "volume 1 alone",
                  "T.1: the library continues on T.2 (volume 2 of 2), which is missing"));
    CHECK(refused(std::vector<IszVolume>{vols[1]}, "volume 2 alone", "the set's volume 1 (T.1) is missing"));
    // A file named as the missing volume would be holds another: it is
    // there, so it is never the one said to be missing.
    CHECK(refused(std::vector<IszVolume>{{"T.1", vols[1].data}}, "volume 2 named T.1",
                  "the set's volume 1 is missing (T.1 is volume 2)"));
    CHECK(refused(std::vector<IszVolume>{{"t.2", vols[0].data}}, "volume 1 named t.2",
                  "t.2: the library continues on volume 2 of 2, which is missing (t.2 is volume 1)"));
    // Both files swapped: the headers give the order, the names are labels
    // (a recipe that finds a set by its first volume's name checks it).
    try {
      IszLibrary swapped(std::vector<IszVolume>{{"T.1", vols[1].data}, {"T.2", vols[0].data}});
      CHECK(swapped.volumes()[0].name == "T.2" && swapped.volumes()[1].name == "T.1");
      CHECK(all_extract(swapped, four, "the volumes' names swapped"));
      CHECK_EQ(swapped.members()[0].volumes, std::string("T.2"));
      CHECK_EQ(swapped.members()[1].volumes, std::string("T.2+T.1"));
    } catch (const IszError& e) {
      test::g_failures++;
      fprintf(stderr, "  the volumes' names swapped: %s\n", e.what());
    }
    CHECK(refused(std::vector<IszVolume>{vols[0], vols[1], vols[1]}, "volume 2 twice", "are both volume 2 of the set"));
    CHECK(refused(std::vector<IszVolume>{vols[0], vols[1], {"T.LIB", own(base.volumes[0])}}, "an unsplit library too",
                  "T.LIB is an unsplit library, not a volume of a set with T.1"));
    CHECK(refused(std::vector<IszVolume>{{"A.LIB", own(base.volumes[0])}, {"B.LIB", own(base.volumes[0])}},
                  "two libraries", "A.LIB is an unsplit library, not a volume of a set with B.LIB"));
    CHECK(refused(std::vector<IszVolume>{{"T.1", nullptr}}, "no data", "T.1: no data"));
    test::IszBuilt three = test::isz_split(specs(four), {four[0].spec.stream.size() + 5, 1000});
    CHECK(refused(std::vector<IszVolume>{vols[0], {"T.3", own(three.volumes[2])}}, "volume 3 of another set",
                  "volume 3 of a set of 2"));
    CHECK(refused(std::vector<IszVolume>{{"T.1", own(three.volumes[0])}, {"T.2", own(three.volumes[2])}},
                  "volume 3 named T.2",
                  "T.1: the library continues on volume 2 of 3, which is missing (T.2 is volume 3)"));
    CHECK(refused(std::vector<IszVolume>{{"T.1", own(three.volumes[0])}, {"T.2", own(three.volumes[1])}},
                  "volume 3 of 3 missing", "T.1: the library continues on T.3 (volume 3 of 3), which is missing"));
    // A volume of another set of the same shape: its header or its tables differ.
    test::IszBuilt other = test::isz_split(specs(four), {four[0].spec.stream.size() + 100}, 0x1B8E);
    CHECK(refused(std::vector<IszVolume>{vols[0], {"U.2", own(other.volumes[1])}}, "another set's volume 2 (date)",
                  "U.2: not a volume of the same set as T.1 (its header differs)"));
    auto renamed = specs(four);
    renamed[3].name = "LAST.TXU";
    test::IszBuilt other2 = test::isz_split(renamed, {four[0].spec.stream.size() + 100});
    CHECK(refused(std::vector<IszVolume>{vols[0], {"U.2", own(other2.volumes[1])}}, "another set's volume 2 (tables)",
                  "U.2: not a volume of the same set as T.1 (its tables differ)"));
    // Every volume is checked on its own too.
    CHECK(refused(std::vector<IszVolume>{vols[0], {"T.2", own(header(split, 1, 0x0A, 3, 2).volumes[1])}},
                  "volume 2's flags", "T.2: unknown library flags"));
  }

  // ---- bit flips -------------------------------------------------------------------------------------
  {
    FlipCount lib_flips, stream_flips;
    for (const test::IszLibraryVector& v : test::kIszLibraryVectors) {
      const Bytes lib = test::unhex(v.hex);
      if (lib.size() > 1024) continue;
      for (size_t bit = 0; bit < lib.size() * 8; bit++) {
        Bytes f = lib;
        f[bit / 8] ^= uint8_t(1 << (bit % 8));
        flip_library({{v.library, own(f)}}, lib_flips, v.library);
      }
      if (!v.dict_bits) continue;
      const Bytes stream(lib.begin() + 255, lib.begin() + 255 + v.stream_size);
      for (size_t bit = 0; bit < stream.size() * 8; bit++) {
        Bytes f = stream;
        f[bit / 8] ^= uint8_t(1 << (bit % 8));
        flip_stream(f, v.plain_size, stream_flips, v.library);
      }
    }
    for (const test::DclStreamVector& v : test::kDclStreamVectors) {
      const Stream s = survey_stream(v.name);
      if (s.bytes.size() > 1024) continue;
      for (size_t bit = 0; bit < s.bytes.size() * 8; bit++) {
        Bytes f = s.bytes;
        f[bit / 8] ^= uint8_t(1 << (bit % 8));
        flip_stream(f, s.size, stream_flips, v.name);
      }
    }
    // A small split set: every bit of both volumes.
    std::vector<Member> small = {
        member("A.DAT", test::dcl_literals("a small first member, ")),
        member("B.DAT", cat(test::dcl_literals("then the one across"), {DclToken::copy(20, 7)})),
        member("C.DAT", test::dcl_literals("and the last"))};
    test::IszBuilt ss = test::isz_split(specs(small), {small[0].spec.stream.size() + 9});
    CHECK(all_extract(IszLibrary(as_volumes(ss)), small, "the small split set"));
    for (size_t v = 0; v < 2; v++)
      for (size_t bit = 0; bit < ss.volumes[v].size() * 8; bit++) {
        test::IszBuilt f = ss;
        f.volumes[v][bit / 8] ^= uint8_t(1 << (bit % 8));
        flip_library(as_volumes(f), lib_flips, "split set");
      }
    fprintf(stderr, "  bit flips of libraries: %zu tried, %zu refused, %zu decoded to their sizes, %zu wrong\n",
            lib_flips.tried, lib_flips.refused, lib_flips.decoded, lib_flips.bad);
    fprintf(stderr, "  bit flips of streams: %zu tried, %zu refused, %zu decoded to their sizes, %zu wrong\n",
            stream_flips.tried, stream_flips.refused, stream_flips.decoded, stream_flips.bad);
    CHECK_EQ(lib_flips.bad, size_t(0));
    CHECK_EQ(stream_flips.bad, size_t(0));
    CHECK(lib_flips.tried > 50000 && stream_flips.tried > 20000);
  }
  return test::finish("import.isz");
}

namespace {

// Every library of `z`, read from `src` (its volumes given last first: the
// reader puts them in order): the volumes' md5s, then every member's name,
// size and md5 against the survey's. Returns the members checked.
size_t check_libraries(const SourceFs& src, const test::IszRealZip& z, const char* form) {
  size_t members = 0;
  for (const test::IszRealLibrary& l : test::kIszRealLibraries) {
    if (std::string(l.zip_md5) != z.md5) continue;
    std::vector<IszVolume> vols;
    for (size_t i = l.volume_count; i-- > 0;) {
      const std::string path = l.volumes[i].name, file = path.substr(path.find('/') + 1);
      auto node = src.find(file);
      CHECK(node && !node->is_dir);
      if (!node) continue;
      Bytes data = src.read_all(*node);
      CHECK_EQ(data.size(), size_t(l.volumes[i].size));
      CHECK_EQ(md5_of(data), std::string(l.volumes[i].md5));
      vols.push_back({file, own(std::move(data))});
    }
    try {
      IszLibrary lib(vols);
      CHECK_EQ(lib.members().size(), l.member_count);
      for (size_t i = 0; i < lib.members().size() && i < l.member_count; i++) {
        const IszMember& m = lib.members()[i];
        CHECK_EQ(m.name, std::string(l.members[i].name));
        CHECK_EQ(m.size, l.members[i].size);
        Md5 h;
        uint64_t n = 0;
        lib.extract(m, [&](const uint8_t* p, size_t k) {
          h.update(p, k);
          n += k;
        });
        const std::string md5 = h.finish_hex();
        if (md5 != l.members[i].md5 || n != m.size) {
          test::g_failures++;
          fprintf(stderr, "  %s!%s: md5 %s, the survey's %s\n", m.volumes.c_str(), m.name.c_str(), md5.c_str(),
                  l.members[i].md5);
        }
        if (m.segments.size() > 1)
          fprintf(stderr, "  %s: %s spans %s\n", l.logical_name, m.name.c_str(), m.volumes.c_str());
        members++;
      }
      fprintf(stderr, "  %s (%s) %s: %zu members\n", z.release, form, l.logical_name, lib.members().size());
    } catch (const IszError& e) {
      test::g_failures++;
      fprintf(stderr, "  %s (%s) %s: %s\n", z.release, form, l.logical_name, e.what());
    }
  }
  return members;
}

// The user's Marvel Comics Screen Posters and Snoopy's Screen Savers ZIPs
// (found by size and md5, never by name), read as the importer reads them —
// DISK<n> sources: the union's root lists every disk's files once — and
// every member of every library of theirs checked against the survey's md5s;
// then the same from a folder copy of the libraries' volumes, each in its
// disk's folder. Only the libraries are read (and copied): never the previous
// owners' notes beside them, which the union's listing only counts (the log
// names none of the root's files).
int real_test(int argc, char** argv) {
  if (test::get_env(L"AD_E2E_PKG") != L"1") {
    fprintf(stderr, "import.isz_real: skipped (set AD_E2E_PKG=1)\n");
    return 77;
  }
  const fs::path scratch = test::scratch(argc, argv, "adw-import-isz-real");
  test::sandbox_data_root(scratch / L"localappdata");
  const auto dirs = test::image_dirs(argc > 2 ? fs::path(argv[2]) : fs::path());
  size_t found = 0, members = 0, folder_members = 0;
  for (const test::IszRealZip& z : test::kIszRealZips) {
    const fs::path zip = test::find_image(dirs, z.size, z.md5);
    if (zip.empty()) {
      fprintf(stderr, "  %s: %s not found (size %llu, md5 %s)\n", z.release, z.name, (unsigned long long)z.size, z.md5);
      continue;
    }
    found++;
    std::string note;
    std::unique_ptr<SourceFs> src;
    try {
      src = open_image(zip, &note);
    } catch (const std::exception& e) {
      test::g_failures++;
      fprintf(stderr, "  %s: %s\n", z.release, e.what());
      continue;
    }
    fprintf(stderr, "  %s: %s\n", z.release, note.c_str());
    CHECK(note.find("as the union of its folders Disk1 and Disk2") != std::string::npos);
    // The union: each name once, as many as the ZIP's files have distinct names.
    std::set<std::string> names, distinct;
    for (const SourceNode& n : src->list(src->root())) {
      CHECK(!n.is_dir);
      CHECK(names.insert(n.name).second);
    }
    fprintf(stderr, "  %s: %zu files at the union's root\n", z.release, names.size());
    ZipArchive za(std::make_shared<const Bytes>(test::read_bytes(zip)), z.name, ZipNames::disk_folders);
    for (const ZipMember& m : za.members())
      if (!m.directory) distinct.insert(name_key(m.file_name()));
    CHECK_EQ(names.size(), distinct.size());
    members += check_libraries(*src, z, "zip");
    // A folder copy of the volumes, each in its disk's folder (Disk1\IMAGES.1,
    // Disk2\IMAGES.2, ...): read as the union of DISK1 and DISK2 too.
    const fs::path copy = scratch / to_wide(z.release);
    for (const test::IszRealLibrary& l : test::kIszRealLibraries)
      if (std::string(l.zip_md5) == z.md5)
        for (size_t i = 0; i < l.volume_count; i++) {
          const std::string path = l.volumes[i].name;
          auto node = src->find(path.substr(path.find('/') + 1));
          if (node)
            test::write_bytes(copy / to_wide(path.substr(0, path.find('/'))) / to_wide(node->name),
                              src->read_all(*node));
        }
    std::string folder_note;
    auto folder = open_folder(copy, &folder_note);
    fprintf(stderr, "  %s: %s\n", z.release, folder_note.c_str());
    CHECK(folder_note.find("as the union of its folders DISK1 and DISK2") != std::string::npos);
    folder_members += check_libraries(*folder, z, "folder");
  }
  if (!found) {
    fprintf(stderr, "import.isz_real: skipped (neither ZIP is in the source folders)\n");
    return 77;
  }
  fprintf(stderr, "  %zu members checked from the ZIPs, %zu from the folder copies\n", members, folder_members);
  CHECK_EQ(folder_members, members);
  if (found == std::size(test::kIszRealZips)) CHECK_EQ(members, size_t(88));
  return test::finish("import.isz_real");
}

}  // namespace
