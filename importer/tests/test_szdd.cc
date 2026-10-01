// SZDD (szdd.h; research/win/pkg/swse/survey/importer_design.md §3, §6.4), on
// files from tests/szdd_builder.h:
//   round trips    random data, leading spaces (the window's initial spaces),
//                  overlapping runs, 18-byte matches, a 1-byte and an empty
//                  file, 70 KB (more than one 64 KiB chunk), literals only
//   the header     the magic, KWAJ, mode 'B', a short header, the missing
//                  character
//   errors         data cut short, a match past the size, bytes left over
//   fixed vectors  LEADSP.TX_ and RUN.BI_ from the research encoder, which
//                  Windows' EXPAND.EXE expands to the same bytes (they pin
//                  the window rule: spaces, written from 0xFF0)
//
//   test_import_szdd <scratch>
#include <functional>

#include "arj_builder.h"  // unhex
#include "md5.h"
#include "szdd.h"
#include "szdd_builder.h"
#include "test_util.h"

using namespace adw::import;

namespace {

std::vector<uint8_t> expand(const std::vector<uint8_t>& f, size_t* max_chunk = nullptr, size_t* chunks = nullptr) {
  std::vector<uint8_t> out;
  szdd_expand(f, "T.XX_", [&](const uint8_t* p, size_t n) {
    out.insert(out.end(), p, p + n);
    if (max_chunk) *max_chunk = std::max(*max_chunk, n);
    if (chunks) ++*chunks;
  });
  return out;
}

bool fails(const char* what, const std::function<void()>& f, const std::string& want) {
  try {
    f();
    fprintf(stderr, "  %s: accepted, should have failed\n", what);
    return false;
  } catch (const SzddError& e) {
    const bool ok = std::string(e.what()).find(want) != std::string::npos;
    fprintf(stderr, "  %s -> %s\n", what, e.what());
    if (!ok) fprintf(stderr, "    expected the message to hold \"%s\"\n", want.c_str());
    return ok;
  }
}

std::vector<uint8_t> bytes(std::string_view s) { return std::vector<uint8_t>(s.begin(), s.end()); }

}  // namespace

int main(int argc, char** argv) {
  // No default may reach the real data folder.
  test::sandbox_data_root(test::scratch(argc, argv, "adw-import-szdd") / L"localappdata");

  // ---- round trips -----------------------------------------------------------------------
  {
    auto roundtrip = [](const char* what, const std::vector<uint8_t>& data, bool literal_only = false) {
      const auto f = test::szdd_encode(data, 0, literal_only);
      size_t chunk = 0;
      const bool ok = expand(f, &chunk) == data;
      fprintf(stderr, "  %-26s %6zu -> %6zu bytes: %s\n", what, data.size(), f.size(), ok ? "ok" : "MISMATCH");
      CHECK(ok);
      CHECK(chunk <= 64 * 1024);
      CHECK_EQ(szdd_header(f, "T.XX_").size, uint32_t(data.size()));
    };
    roundtrip("empty", {});
    roundtrip("one byte", {0x7F});
    roundtrip("random 5 KB", test::pattern(5000, 3));
    roundtrip("leading spaces", bytes("        eight spaces the window already holds, and more:        "));
    roundtrip("a run of 1000", std::vector<uint8_t>(1000, 'Z'));  // overlapping 18-byte matches
    roundtrip("text", test::arj_vector_text(3000, 4));
    roundtrip("literals only", test::pattern(300, 5), true);
    {
      // More than one 64 KiB chunk: 70 KB, expanded in two pieces.
      auto big = test::arj_vector_text(70000, 7);
      auto f = test::szdd_encode(big);
      size_t chunk = 0, chunks = 0;
      CHECK(expand(f, &chunk, &chunks) == big);
      CHECK_EQ(chunk, size_t(64 * 1024));
      CHECK_EQ(chunks, size_t(2));
    }
  }

  // ---- the fixed vectors: what EXPAND.EXE gives -------------------------------------------------
  {
    const auto leadsp = test::unhex(
        "535a444488f027334100b4000000fe0001666f7572206c65ff6164696e67207370ff6163657320726575ff7365207468652077ff69"
        "6e646f772773207f696e697469616c000500eeff000f120f240f360f0c0f1e0b");
    std::string unit = "    four leading spaces reuse the window's initial spaces   ";
    CHECK(expand(leadsp) == bytes(unit + unit + unit));
    CHECK_EQ(md5_hex(expand(leadsp).data(), 180), std::string("11eff481cd3ea8d2db003d78eef1edc6"));
    const auto run = test::unhex(
        "535a444488f027334100e8030000015af0ff000f000f000f000f000f000f00000f000f000f000f000f000f000f000f00000f000f000f"
        "000f000f000f000f000f00000f000f000f000f000f000f000f000f00000f000f000f000f000f000f000f000f00000f000f000f000f"
        "000f000f000f000f00000f000f000f000f000f000f000f000f000006");
    CHECK(expand(run) == std::vector<uint8_t>(1000, 'Z'));
    // The encoder's own output for the same text: EXPAND.EXE agreed with the
    // research encoder, whose output this is.
    CHECK(test::szdd_encode(bytes(unit + unit + unit)) == leadsp);
  }

  // ---- the header -------------------------------------------------------------------------------
  {
    auto f = test::szdd_encode(bytes("hello"), 'L');
    SzddHeader h = szdd_header(f, "HELLO.TX_");
    CHECK_EQ(h.size, uint32_t(5));
    CHECK_EQ(h.missing, 'L');
    CHECK_EQ(szdd_header(test::szdd_encode(bytes("x")), "X").missing, '\0');
    CHECK(fails("not SZDD", [] { szdd_header(test::pattern(40, 1), "JUNK.DL_"); }, "JUNK.DL_: not an SZDD file"));
    CHECK(fails("empty", [] { szdd_header({}, "E.DL_"); }, "not an SZDD file"));
    auto kwaj = f;
    kwaj[0] = 'K', kwaj[1] = 'W', kwaj[2] = 'A', kwaj[3] = 'J';
    CHECK(fails("KWAJ", [&] { szdd_header(kwaj, "K.DL_"); }, "K.DL_: a KWAJ file, not SZDD"));
    auto mode_b = f;
    mode_b[8] = 'B';
    CHECK(fails("mode B", [&] { szdd_header(mode_b, "B.DL_"); }, "SZDD mode 'B' is not supported"));
    mode_b[8] = 0x01;
    CHECK(fails("mode 0x01", [&] { szdd_header(mode_b, "B.DL_"); }, "SZDD mode 0x01 is not supported"));
    std::vector<uint8_t> cut(f.begin(), f.begin() + 12);
    CHECK(fails("a short header", [&] { szdd_header(cut, "S.DL_"); }, "the SZDD header is cut short"));
    CHECK(fails("expand checks the header too", [&] { expand(mode_b); }, "is not supported"));
  }

  // ---- errors -----------------------------------------------------------------------------------
  {
    const auto data = test::arj_vector_text(2000, 9);
    const auto f = test::szdd_encode(data);
    // Cut anywhere in the data: it ends early.
    for (size_t cut : {size_t(1), size_t(2), size_t(7), f.size() / 2, f.size() - 15}) {
      std::vector<uint8_t> c(f.begin(), f.end() - cut);
      CHECK(fails(("cut by " + std::to_string(cut)).c_str(), [&] { expand(c); }, "ends early"));
    }
    // A match that would pass the size: the header claims one byte less
    // than a run of 18-byte matches produces.
    auto run = test::szdd_encode(std::vector<uint8_t>(100, 'A'));
    run[10] = 99;  // the size field (little-endian DWORD at 10)
    CHECK(fails("a match past the size", [&] { expand(run); }, "a match passes the expanded size"));
    // Bytes left over after the last token.
    auto extra = f;
    extra.push_back(0x00);
    CHECK(fails("a byte left over", [&] { expand(extra); }, "1 byte(s) left after the compressed data"));
    // An empty file with a stray flag byte.
    auto e = test::szdd_encode(std::vector<uint8_t>{});
    e.push_back(0xFF);
    CHECK(fails("an empty file with a flag byte", [&] { expand(e); }, "left after"));
    // Delrina's version stamps after the data ("DLL " + four digits, one or
    // two records) are not data; anything else of eight bytes still is.
    auto stamped = [&](const std::string& tail) {
      auto s = f;
      s.insert(s.end(), tail.begin(), tail.end());
      return s;
    };
    CHECK(expand(stamped("DLL 0401")) == data);
    CHECK(expand(stamped("DLL 1001DLL 0501")) == data);
    CHECK(fails("a stamp with a letter", [&] { expand(stamped("DLL 04X1")); }, "8 byte(s) left after"));
    CHECK(fails("a stamp cut short", [&] { expand(stamped("DLL 040")); }, "7 byte(s) left after"));
    CHECK(fails("a stamp and a byte", [&] { expand(stamped("DLL 0401 ")); }, "9 byte(s) left after"));
    CHECK(fails("another tag", [&] { expand(stamped("EXE 0401")); }, "8 byte(s) left after"));
    // A literal byte changed: still exactly the size (SZDD has no checksum;
    // only the manifest can tell).
    auto lit = test::szdd_encode(bytes("ABCDEFGHIJ"), 0, true);
    lit[15] ^= 0x20;
    auto got = expand(lit);
    CHECK(got.size() == 10 && got != bytes("ABCDEFGHIJ"));
  }
  return test::finish("import.szdd");
}
