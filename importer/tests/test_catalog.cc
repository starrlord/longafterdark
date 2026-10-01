// catalog-win.json generation (catalog.h).
//
//   test_import_catalog unit <scratch>
//     the text and record readers against hand-made inputs, whole synthetic
//     PE32 / NE modules (tests/module_builder.h), NE modules told apart by
//     their exports as the ne16 lane does (MODULE first; what it refuses
//     left out, with its reason), a scan of a synthetic FILES tree and of a
//     package tree with *.AD and *.IMX, the JSON layout (abi last, IMX
//     entries only), After Dark 2.0's About rules and "screen" (the startrek
//     package's entries only), and regenerate_catalog (adimport
//     --catalog-only)
//   test_import_catalog real <scratch> <adimport.exe> <reference.json>
//     semantic identity with the prototype's output (research/win/
//     make_catalog.py -> research/win/catalog-win.json) over the real
//     corpus, both through the library and through adimport --catalog-only
//     on a scratch copy of the modules. Exit 77 (skipped) when the imported
//     assets or the reference are not on this machine.
#include <phosg/JSON.hh>

#include <functional>
#include <set>
#include <tuple>

#include "catalog.h"
#include "importer.h"
#include "module_builder.h"
#include "run_process.h"
#include "test_util.h"

using namespace adw::import;
namespace fs = std::filesystem;

namespace {

std::string bytes(std::string_view s) { return std::string(s); }

// ---- text -------------------------------------------------------------------------

void test_text() {
  CHECK_EQ(cp1252_to_utf8("A\x80\x81\xA9\xFF\x9D"), std::string("A\xE2\x82\xAC\xEF\xBF\xBD\xC2\xA9\xC3\xBF\xEF\xBF\xBD"));
  CHECK_EQ(cp1252_to_utf8("\x93quoted\x94 \x96 \x85"),
           std::string("\xE2\x80\x9Cquoted\xE2\x80\x9D \xE2\x80\x93 \xE2\x80\xA6"));
  CHECK_EQ(std::string(c_string(bytes(std::string_view("ab\0cd", 5)))), std::string("ab"));
  CHECK_EQ(std::string(c_string("abc")), std::string("abc"));

  auto rtf = [](std::string_view s) { return rtf_to_text(s); };
  // Tables are not text; paragraphs, tabs and \'xx are; the NUL after the
  // closing brace (and anything past it) is not read.
  CHECK_EQ(rtf(std::string("{\\rtf1\\ansi\\deff0{\\fonttbl{\\f0\\fswiss Arial;}}{\\colortbl;\\red0\\green0\\blue0;}\r\n"
                           "\\pard\\plain\\f0\\fs20 Hello\\par World\\tab x\\'a9 1996\\par\\par\\par\\par End}") +
               std::string(1, '\0') + "{junk}"),
           std::string("Hello\nWorld\tx\xC2\xA9 1996\n\nEnd"));
  CHECK_EQ(rtf(std::string("{\\rtf1 A") + std::string(1, '\0') + "B}"), std::string("A"));
  CHECK_EQ(rtf("{\\rtf1 A{\\*\\generator Foo 1.0;}B}"), std::string("AB"));
  CHECK_EQ(rtf("{\\rtf1 A{\\pict\\wmetafile8 0123abcd}B{\\info{\\author me}}C}"), std::string("ABC"));
  CHECK_EQ(rtf("{\\rtf1 \\\\ \\{ \\} a\\~b}"), std::string("\\ { } a b"));
  // A control word eats one following space and its numeric parameter.
  CHECK_EQ(rtf("{\\rtf1\\li-360 X\\fi720  Y}"), std::string("X Y"));
  // Blanks before a line break go; runs of 3+ breaks become 2 — after the
  // blanks went, so "\n\t\n\n" is a run of three.
  CHECK_EQ(rtf("{\\rtf1 A  \\par B}"), std::string("A\nB"));
  CHECK_EQ(rtf("{\\rtf1 A\\par\\tab\\par\\par B}"), std::string("A\n\nB"));
  CHECK_EQ(rtf("{\\rtf1 A\\line\\line B}"), std::string("A\n\nB"));
  // Only lower-case control words are words: "\Foo" is the symbol \F + "oo".
  CHECK_EQ(rtf("{\\rtf1 \\Foo x}"), std::string("oo x"));
  // A backslash before a raw line break (or at the very end) is dropped.
  CHECK_EQ(rtf("{\\rtf1 A\\\nB\\"), std::string("AB"));
  // Raw CR/LF in the source are not text.
  CHECK_EQ(rtf("{\\rtf1 A\r\nB}"), std::string("AB"));
  // The outermost group ends the document; a stray '}' ends it too.
  CHECK_EQ(rtf("{\\rtf1 A}B"), std::string("A"));
  CHECK_EQ(rtf("text} more"), std::string("text"));
  CHECK_EQ(rtf("no braces at all"), std::string("no braces at all"));
  // Trimming covers NO-BREAK SPACE (str.strip does); undefined bytes decode
  // to U+FFFD.
  CHECK_EQ(rtf("{\\rtf1 \\'a0 X \\'a0}"), std::string("X"));
  CHECK_EQ(rtf("{\\rtf1 \\'81\\'zz}"), std::string("\xEF\xBF\xBDzz"));
  CHECK_EQ(rtf(""), std::string());

  std::string sl = test::stringlist({"First", "Caf\xE9", "x"});
  auto items = parse_stringlist(sl);
  CHECK(items.size() == 3 && items[0] == "First" && items[1] == "Caf\xC3\xA9" && items[2] == "x");
  CHECK(parse_stringlist("").empty());
  CHECK(parse_stringlist("\x01").empty());
  items = parse_stringlist(std::string("\x03\x00" "a\0bc", 6));   // count says 3, one unterminated
  CHECK(items.size() == 2 && items[0] == "a" && items[1] == "bc");
}

// ---- control records ------------------------------------------------------------------

void test_records() {
  // Kinds 0 and unknown are not controls; nor is a record without a kind.
  CHECK(!parse_control_record(test::record_head(0, "None", 0, 0), 0));
  CHECK(!parse_control_record(test::record_head(9, "Nine", 0, 0), 0));
  CHECK(!parse_control_record("\x05", 0));

  auto cb = parse_control_record(test::checkbox_record("Clear Screen First", 3), 2);
  CHECK(cb && cb->index == 2 && cb->name == "Clear Screen First" && cb->kind == "checkbox" && cb->type == "checkbox" &&
        cb->def == 1);
  CHECK(parse_control_record(test::checkbox_record("Off", 0), 0)->def == 0);
  auto bt = parse_control_record(test::button_record("Pictures"), 0);
  CHECK(bt && bt->kind == "button" && bt->type == "button" && !bt->def);
  // The name field is 20 bytes of Windows-1252.
  CHECK_EQ(parse_control_record(test::checkbox_record("Caf\xE9", 0), 0)->name, std::string("Caf\xC3\xA9"));
  CHECK_EQ(parse_control_record(test::checkbox_record("ABCDEFGHIJKLMNOPQRSTUVWXYZ", 0), 0)->name,
           std::string("ABCDEFGHIJKLMNOPQRST"));
  // A record cut short after the name still has a kind and a name.
  auto shortcb = parse_control_record(test::checkbox_record("Short", 1).substr(0, 0x10), 1);
  CHECK(shortcb && shortcb->name == "Short" && shortcb->def == 0);

  // String slider, values from 0 (AD4 shape): no extra stop; the default
  // stop is the last whose value does not exceed the default.
  auto ss = parse_control_record(
      test::string_slider_record("Critic Appears:", {"Never", "Rarely", "Often", "Always"}, {0, 33, 66, 100}, 50), 1);
  CHECK(ss && ss->kind == "stringslider" && ss->type == "slider");
  CHECK((ss->items == std::vector<std::string>{"Never", "Rarely", "Often", "Always"}));
  CHECK((ss->values == std::vector<int>{0, 33, 66, 100}));
  CHECK(ss->def == 33 && ss->default_stop == 1 && !ss->bold_stop);
  ss = parse_control_record(test::string_slider_record("S", {"a", "b"}, {0, 50}, 1000), 0);
  CHECK(ss->default_stop == 1 && ss->def == 50);
  // Classic shape: stored lower bounds start above 0, so the host prepends a
  // 0 value and repeats the last label, flagged bold.
  ss = parse_control_record(
      test::string_slider_record("Objects", {"Flight", "Squadron", "Air Wing", "Swarm"}, {25, 50, 75, 100}, 60), 0);
  CHECK((ss->items == std::vector<std::string>{"Flight", "Squadron", "Air Wing", "Swarm", "Swarm"}));
  CHECK((ss->values == std::vector<int>{0, 25, 50, 75, 100}));
  CHECK(ss->default_stop == 2 && ss->def == 50 && ss->bold_stop == 4);
  // A value table cut off by the record's end reads as all zeros.
  std::string cut = test::string_slider_record("Cut", {"x", "y", "z"}, {10, 20, 30}, 0);
  ss = parse_control_record(cut.substr(0, cut.size() - 1), 0);
  CHECK((ss->values == std::vector<int>{0, 0, 0}) && ss->items.size() == 3 && !ss->bold_stop && ss->default_stop == 2);
  // No stops at all: the prototype's defaultStop -1 and default 0.
  ss = parse_control_record(test::string_slider_record("Empty", {}, {}, 5), 0);
  CHECK(ss->items.empty() && ss->values.empty() && ss->def == 0 && ss->default_stop == -1);
  // The host keeps at most 101 stops; an appended stop past that is dropped
  // with its bold flag.
  std::vector<std::string> many;
  std::vector<uint16_t> many_values;
  for (int i = 0; i < 101; i++) many.push_back("s" + std::to_string(i)), many_values.push_back(uint16_t(i + 1));
  ss = parse_control_record(test::string_slider_record("Many", many, many_values, 3), 0);
  CHECK(ss->items.size() == 101 && ss->values.size() == 101 && !ss->bold_stop);
  CHECK(ss->values[0] == 0 && ss->values[100] == 100 && ss->default_stop == 3);

  // Numeric slider: the default is clamped, the raw one kept; a unit only
  // when the record has one.
  auto ns = parse_control_record(test::num_slider_record("# of Drops", 1, 9, 22), 0);
  CHECK(ns && ns->kind == "numslider" && ns->type == "slider" && ns->min == 1 && ns->max == 9 && ns->def == 9 &&
        ns->raw_default == 22 && ns->unit.empty());
  ns = parse_control_record(test::num_slider_record("Height:", -5, 95, -40, "%", 2), 0);
  CHECK(ns->def == -5 && ns->raw_default == -40 && ns->unit == "%" && ns->unit_pos == "suffix");
  CHECK(parse_control_record(test::num_slider_record("Cost", 0, 9, 3, "$", 1), 0)->unit_pos == "prefix");
  CHECK(parse_control_record(test::num_slider_record("Speed", 0, 9, 3, "x", 0), 0)->unit_pos == "none");
  ns = parse_control_record(test::record_head(2, "Bare", 0, 7), 0);   // no min/max bytes at all
  CHECK(ns->min == 0 && ns->max == 0 && ns->def == 0 && ns->raw_default == 7);

  // Popup: the default index is clamped (negative -> 0), items capped at 101.
  auto pp = parse_control_record(test::popup_record("Display:", {"Random", "In order", "Reverse"}, 5), 3);
  CHECK(pp && pp->kind == "popup" && pp->type == "popup" && pp->items.size() == 3 && pp->def == 2 && pp->index == 3);
  CHECK(parse_control_record(test::popup_record("P", {"a", "b"}, -1), 0)->def == 0);
  CHECK(parse_control_record(test::popup_record("P", {}, 0), 0)->def == -1);   // the prototype's n-1
}

// ---- synthetic modules ------------------------------------------------------------------

std::string synth_pe(bool msvc_entry = false) {
  test::PeSpec pe;
  pe.imports = {"KERNEL32.dll", "ADXPL510.DLL", "user32.dll", "KERNEL32.dll"};
  pe.exports = {msvc_entry ? "_Module@4" : "Module"};
  pe.resources = {
      {16, "", 1, 0x409, test::version_resource({{"CompanyName", "Nobody"}, {"FileDescription", "Synth Four"}})},
      // German before English in the directory (0x407 < 0x409): English wins.
      {2000, "", 40, 0x407, "{\\rtf1 Deutsch\\par}"},
      {2000, "", 40, 0x409, "{\\rtf1\\ansi{\\fonttbl{\\f0 Arial;}}English\\par about \\'a9 1996}\0"},
      {1000, "", 1, 0x407, test::checkbox_record("Bildschirm", 0)},
      {1000, "", 1, 0x409, test::checkbox_record("Clear Screen", 1)},
      // Only one language: that one.
      {1000, "", 2, 0x40C, test::popup_record("Ordre", {"Un", "Deux"}, 1)},
      // Neutral beats French.
      {1000, "", 3, 0x40C, test::num_slider_record("Vitesse", 0, 10, 5)},
      {1000, "", 3, 0, test::num_slider_record("Speed", 0, 10, 5, "%", 2)},
      // Slot 5 does not exist in the ABI: never read.
      {1000, "", 5, 0x409, test::checkbox_record("Fifth", 1)},
      {0, "STRINGLIST", 128, 0x409, test::stringlist({"Not the name"})},
  };
  return test::build_pe(pe);
}

std::string synth_ne() {
  test::NeSpec ne;
  ne.module_refs = {"KERNEL", "USER", "ADXPL300", "AD_RSRC", "GDI", "USER"};
  ne.exports = {"WEP", "MODULE"};  // what the Classic lane calls
  ne.resources = {
      {2000, "", 20, std::string("Synth Three\0junk", 16)},
      {2000, "", 30, "About \x93this\x94\r\nline 2\r\n  "},
      {2000, "", 10, "Credits\r\n"},
      {1000, "", 1, test::string_slider_record("Density", {"Low", "High"}, {50, 100}, 75)},
      {1000, "", 2, test::popup_record("Mode", {"A", "B", "C"}, 1)},
      {1000, "", 3, test::record_head(0, "Nothing", 0, 0)},
      {1000, "", 4, test::checkbox_record("Sound", 1)},
      {3000, "", 1, "RIFF....WAVE"},
      {0, "STRINGLIST", 128, test::stringlist({"Not the name either"})},
  };
  return test::build_ne(ne);
}

void test_modules(const fs::path& dir) {
  fs::create_directories(dir);
  auto write = [&](const std::string& name, const std::string& data) {
    fs::path p = dir / to_wide(name);
    test::write_bytes(p, std::vector<uint8_t>(data.begin(), data.end()));
    return p;
  };

  CatalogModule m = catalog_module(write("SYNTH4.AD", synth_pe()), "FILES/AD40/SYNTH4.AD");
  CHECK_EQ(m.id, std::string("ad40.synth4"));
  CHECK_EQ(m.lane, std::string("pe32"));
  CHECK_EQ(m.path, std::string("FILES/AD40/SYNTH4.AD"));
  CHECK_EQ(m.display_name, std::string("Synth Four"));
  CHECK_EQ(m.about, std::string("English\nabout \xC2\xA9 1996"));
  CHECK(!m.credits);
  CHECK_EQ(m.entry, std::string("Module"));
  CHECK((m.needs == std::vector<std::string>{"ADXPL510.DLL"}));
  CHECK((m.system == std::vector<std::string>{"KERNEL32.DLL", "USER32.DLL"}));
  CHECK_EQ(m.controls.size(), size_t(3));
  if (m.controls.size() == 3) {
    CHECK(m.controls[0].index == 0 && m.controls[0].name == "Clear Screen" && m.controls[0].def == 1);
    CHECK(m.controls[1].index == 1 && m.controls[1].name == "Ordre" && m.controls[1].kind == "popup");
    CHECK(m.controls[2].index == 2 && m.controls[2].name == "Speed" && m.controls[2].unit == "%");
  }
  CHECK_EQ(catalog_module(write("STARRYNI.AD", synth_pe(true)), "FILES/ENGINE/STARRYNI.AD").entry,
           std::string("_Module@4"));

  // No FileDescription: STRINGLIST 128's first string, else the file name.
  test::PeSpec bare;
  bare.resources = {{0, "STRINGLIST", 128, 0x409, test::stringlist({"From the list", "second"})}};
  CHECK_EQ(catalog_module(write("LISTED.AD", test::build_pe(bare)), "x").display_name, std::string("From the list"));
  bare.resources = {{0, "STRINGLIST", 128, 0x409, test::stringlist({""})},
                    {16, "", 1, 0x409, test::version_resource({{"FileDescription", ""}})}};
  CatalogModule unnamed = catalog_module(write("NoName.AD", test::build_pe(bare)), "x");
  CHECK_EQ(unnamed.display_name, std::string("noname"));
  CHECK_EQ(unnamed.id, std::string("ad40.noname"));
  CHECK(unnamed.about.empty() && unnamed.controls.empty() && unnamed.needs.empty() && unnamed.system.empty());

  CatalogModule c = catalog_module(write("SYNTH3.AD", synth_ne()), "FILES/CLASSIC/SYNTH3.AD");
  CHECK_EQ(c.id, std::string("classic.synth3"));
  CHECK_EQ(c.lane, std::string("ne16"));
  CHECK_EQ(c.display_name, std::string("Synth Three"));
  CHECK_EQ(c.about, std::string("About \xE2\x80\x9Cthis\xE2\x80\x9D\nline 2"));
  CHECK(c.credits && *c.credits == "Credits");
  CHECK_EQ(c.entry, std::string("MODULE"));
  // Sorted by bytes: 'X' (0x58) before '_' (0x5F).
  CHECK((c.needs == std::vector<std::string>{"ADXPL300", "AD_RSRC"}));
  CHECK((c.system == std::vector<std::string>{"GDI", "KERNEL", "USER"}));
  CHECK_EQ(c.controls.size(), size_t(3));
  if (c.controls.size() == 3) {
    const CatalogControl& s = c.controls[0];
    CHECK(s.kind == "stringslider" && (s.values == std::vector<int>{0, 50, 100}) && s.bold_stop == 2 &&
          s.default_stop == 1 && s.def == 50);
    CHECK(c.controls[1].index == 1 && c.controls[1].def == 1);
    CHECK(c.controls[2].index == 3 && c.controls[2].kind == "checkbox");   // slot 2 was kind 0
  }
  // An empty name (just the NUL) falls back like a missing one.
  test::NeSpec ne;
  ne.exports = {"MODULE"};
  ne.resources = {{2000, "", 20, std::string(1, '\0')}, {0, "STRINGLIST", 128, test::stringlist({"Listed"})}};
  CatalogModule listed = catalog_module(write("LISTED3.AD", test::build_ne(ne)), "x");
  CHECK_EQ(listed.display_name, std::string("Listed"));
  CHECK(listed.about.empty() && !listed.credits);
  ne.resources.clear();
  CHECK_EQ(catalog_module(write("PLAIN3.AD", test::build_ne(ne)), "x").display_name, std::string("plain3"));

  // Not a module: a clear error, never a crash.
  bool threw = false;
  try {
    catalog_module(write("JUNK.AD", "MZ not really an executable, just text padding it out to 64 bytes...."), "x");
  } catch (const ImportError& e) {
    threw = e.status() == Status::source_invalid;
  }
  CHECK(threw);
  threw = false;
  try {
    catalog_module(write("CUT.AD", synth_pe().substr(0, 0x100)), "x");   // cut inside the headers
  } catch (const std::exception&) {
    threw = true;
  }
  CHECK(threw);
}

// ---- Intermission IMX modules ---------------------------------------------------------------

const std::vector<std::string> kImxExports = {"WEP", "SAVERINIT", "SAVERDRAW", "SAVERDLGPROC", "LIBMAIN", "SAVERDLGPROC2"};

// An NE with these exports (and, listed after them, names with no entry);
// it also carries an After Dark name resource, which an IMX module's entry
// must not read.
std::string synth_imx(const std::vector<std::string>& exports,
                      const std::vector<std::string>& refs = {"INTRMLIB", "READJPG", "SWSE", "KERNEL", "USER", "GDI",
                                                              "WIN87EM", "STRESS"},
                      const std::vector<std::string>& names_without_entries = {}) {
  test::NeSpec ne;
  ne.module_name = "SYNTHIMX";
  ne.module_refs = refs;
  ne.exports = exports;
  ne.names_without_entries = names_without_entries;
  ne.resources = {{5, "", 1, "a dialog"}, {2000, "", 20, std::string("After Dark Name\0", 16)},
                  {2000, "", 30, "About text\r\n"}};
  return test::build_ne(ne);
}

void test_imx(const fs::path& dir) {
  fs::create_directories(dir);
  auto write = [&](const std::string& name, const std::string& data) {
    fs::path p = dir / to_wide(name);
    test::write_bytes(p, std::vector<uint8_t>(data.begin(), data.end()));
    return p;
  };
  const Package* swse = find_package("swse");
  CHECK(swse && swse->recipe == Recipe::intermission);
  CatalogModule m = catalog_module(write("VADER.IMX", synth_imx(kImxExports)), "packages/swse/SAVER/VADER.IMX", swse);
  CHECK_EQ(m.id, std::string("swse.vader"));
  CHECK_EQ(m.lane, std::string("ne16"));
  CHECK_EQ(m.abi, std::string("intermission"));
  CHECK_EQ(m.entry, std::string("SAVERDRAW"));
  CHECK_EQ(m.display_name, std::string("vader"));  // no resource holds its name: the registry's overrides do
  CHECK_EQ(m.module_name, std::string("vader"));
  CHECK(m.about.empty() && !m.credits);
  CHECK_EQ(m.controls.size(), size_t(1));
  if (m.controls.size() == 1) {
    const CatalogControl& b = m.controls[0];
    CHECK(b.index == 0 && b.name == "Configure..." && b.kind == "button" && b.type == "button" && !b.def);
  }
  CHECK((m.needs == std::vector<std::string>{"INTRMLIB", "READJPG", "STRESS", "SWSE"}));
  CHECK((m.system == std::vector<std::string>{"GDI", "KERNEL", "USER", "WIN87EM"}));
  CHECK_EQ(m.package, std::string("swse"));
  CHECK_EQ(m.package_title, std::string("Star Wars Screen Entertainment"));
  // Export names match without case, as GetProcAddress (IMIMXPLY asks in
  // lower case; the NE stores upper case).
  CatalogModule lower = catalog_module(write("LOWER.IMX", synth_imx({"wep", "saverinit", "saverdraw", "saverdlgproc"})),
                                       "x", swse);
  CHECK(lower.abi == "intermission" && lower.controls.size() == 1);
  // No SAVERDLGPROC: no dialog, no button.
  CatalogModule nodlg = catalog_module(write("NODLG.IMX", synth_imx({"SAVERINIT", "SAVERDRAW"})), "x", swse);
  CHECK(nodlg.abi == "intermission" && nodlg.controls.empty());
  // The ne16 lane's rule, rule for rule (host/ne16/package.cc detect_kind):
  // MODULE makes an After Dark module whatever else it exports — its name,
  // text and controls, no Configure... button.
  const std::vector<std::vector<std::string>> hybrids = {{"MODULE", "SAVERINIT", "SAVERDRAW", "SAVERDLGPROC"},
                                                         {"SAVERINIT", "SAVERDRAW", "SAVERDLGPROC", "module"}};
  for (const auto& exports : hybrids) {
    CatalogModule both = catalog_module(write("BOTH.IMX", synth_imx(exports)), "x", swse);
    CHECK(both.abi.empty());
    CHECK_EQ(both.entry, std::string("MODULE"));
    CHECK_EQ(both.display_name, std::string("After Dark Name"));
    CHECK_EQ(both.about, std::string("About text"));
    CHECK(both.controls.empty());
  }
  // What the lane would refuse is no module: catalog_module says why, in the
  // lane's words (build_catalog logs it and leaves the file out) — a module
  // exporting SETCURRSAVER is another product's, an IMXX_ file an extension,
  // one of SAVERINIT/SAVERDRAW is not enough, SAVERMAIN is a reader, and an
  // NE with none of these exports is nothing the lane runs.
  for (const auto& [file, exports, why] : std::vector<std::tuple<std::string, std::vector<std::string>, std::string>>{
           {"OTHER.IMX", {"SAVERINIT", "SAVERDRAW", "SETCURRSAVER"},
            "an Intermission module that exports SETCURRSAVER, which the IMX reader refuses"},
           {"IMXX_IWR.IMX", kImxExports, "an Intermission module named IMXX_*, which the IMX reader refuses"},
           {"imxx_low.imx", kImxExports, "an Intermission module named IMXX_*, which the IMX reader refuses"},
           {"HALF.IMX", {"SAVERINIT"}, "not an Intermission module: it exports SAVERINIT without SAVERDRAW"},
           {"DRAW.IMX", {"SAVERDRAW", "SAVERDLGPROC"},
            "not an Intermission module: it exports SAVERDRAW without SAVERINIT"},
           {"READER.IMX", {"WEP", "SAVERMAIN"}, "an Intermission reader (it exports SAVERMAIN), not a module"},
           {"NONE.AD", {"WEP"}, "not an After Dark or Intermission module (no MODULE, SAVERINIT or SAVERDRAW export)"},
           {"EMPTY.IMX", {}, "not an After Dark or Intermission module (no MODULE, SAVERINIT or SAVERDRAW export)"}}) {
    std::string got = "listed";
    try {
      catalog_module(write(file, synth_imx(exports)), "x", swse);
    } catch (const ImportError& e) {
      got = e.status() == Status::source_invalid ? e.what() : "another status";
    }
    if (got != why) fprintf(stderr, "  %s: \"%s\", expected \"%s\"\n", file.c_str(), got.c_str(), why.c_str());
    CHECK_EQ(got, why);
  }
  // Exports are found by name, as the lane finds them: a name with no
  // entry-table entry still counts (the Configure... button needs a
  // callable SAVERDLGPROC, as IMIMXPLY's GetProcAddress does).
  {
    CatalogModule named = catalog_module(write("NAMED.IMX", synth_imx({"WEP"}, {"KERNEL"}, {"SAVERINIT", "SAVERDRAW",
                                                                                              "SAVERDLGPROC"})),
                                         "x", swse);
    CHECK(named.abi == "intermission" && named.entry == "SAVERDRAW" && named.controls.empty());
    CatalogModule ad = catalog_module(write("NAMED.AD", synth_imx(kImxExports, {"KERNEL"}, {"MODULE"})), "x", swse);
    CHECK(ad.abi.empty() && ad.entry == "MODULE");
  }
  // The exports decide, never the extension.
  CHECK_EQ(catalog_module(write("SHAPED.AD", synth_imx(kImxExports)), "x", swse).abi, std::string("intermission"));

  // JSON: "abi" only on IMX entries, last in the object; an After Dark entry
  // is laid out exactly as before.
  CatalogModule ad = catalog_module(write("AD.AD", synth_ne()), "x", swse);
  m.md5 = std::string(32, 'a');
  ad.md5 = std::string(32, 'b');
  std::string j = render_catalog_json({ad, m});
  CHECK(j.find("\"md5\": \"" + std::string(32, 'a') + "\",\n   \"abi\": \"intermission\"\n  }") != std::string::npos);
  CHECK(j.find("\"md5\": \"" + std::string(32, 'b') + "\"\n  }") != std::string::npos);
  CHECK(j.find("\"entry\": \"SAVERDRAW\"") != std::string::npos);
  CHECK(j.find("\"abi\"") == j.rfind("\"abi\""));  // once
  phosg::JSON doc = phosg::JSON::parse(j);
  CHECK(!doc.at("modules").as_list().at(0)->contains("abi"));
  CHECK_EQ(doc.at("modules").as_list().at(1)->get_string("abi"), std::string("intermission"));
  // A sameAs goes before it.
  m.same_as = "swse.other";
  j = render_catalog_json({m});
  CHECK(j.find("\"sameAs\": \"swse.other\",\n   \"abi\": \"intermission\"\n  }") != std::string::npos);
  CHECK(is_system_dll("KERNEL") && is_system_dll("WIN87EM") && !is_system_dll("TOOLHELP") && !is_system_dll("INTRMLIB"));

  // Scanning: a package's folders list *.AD and *.IMX together, sorted;
  // ENGINE is scanned too; WINDOWS never is; the registry's name overrides
  // name the Intermission modules; a file the lane would refuse is left out
  // and logged, and one exporting MODULE too is an After Dark entry.
  fs::path root = dir / L"swse-tree";
  auto put = [&](const std::wstring& rel, const std::string& data) {
    test::write_bytes(root / rel, std::vector<uint8_t>(data.begin(), data.end()));
  };
  put(L"SAVER\\C.imx", synth_imx(kImxExports));
  put(L"SAVER\\A.IMX", synth_imx(kImxExports, {"KERNEL"}));
  put(L"SAVER\\B.AD", synth_ne());
  put(L"SAVER\\VADER.IMX", synth_imx(kImxExports, {"USER"}));
  put(L"SAVER\\READJPG.DLL", synth_imx(kImxExports, {"GDI"}));  // not a module file name
  put(L"SAVER\\IMIMXPLY.IMQ", synth_imx(kImxExports, {"GDI", "USER"}));
  put(L"SAVER\\IMXX_EXT.IMX", synth_imx(kImxExports));
  put(L"SAVER\\SETCUR.IMX", synth_imx({"SAVERINIT", "SAVERDRAW", "SETCURRSAVER"}));
  put(L"SAVER\\BOTH.IMX", synth_imx({"MODULE", "SAVERINIT", "SAVERDRAW", "SAVERDLGPROC"}));
  put(L"ENGINE\\E.IMX", synth_imx(kImxExports, {"COMMDLG"}));
  put(L"WINDOWS\\W.IMX", synth_imx(kImxExports, {"SHELL"}));
  CatalogTree t;
  t.package = swse;
  t.dir = root;
  std::vector<std::string> logged;
  CatalogDoc cat = build_catalog({t}, [&](const std::string& s) { logged.push_back(s); });
  std::vector<std::string> ids, names;
  for (auto& mod : cat.modules) ids.push_back(mod.id), names.push_back(mod.module_name);
  CHECK((ids == std::vector<std::string>{"swse.a", "swse.b", "swse.both", "swse.c", "swse.vader", "swse.e"}));
  CHECK_EQ(cat.modules.size() > 4 ? cat.modules[4].module_name : std::string(), std::string("Darth Vader"));
  CHECK_EQ(cat.modules.size() > 4 ? cat.modules[4].display_name : std::string(), std::string("Darth Vader"));
  CHECK_EQ(cat.modules.size() > 4 ? cat.modules[4].path : std::string(), std::string("packages/swse/SAVER/VADER.IMX"));
  CHECK(cat.modules.size() > 2 && cat.modules[2].abi.empty() && cat.modules[2].entry == "MODULE");
  CHECK_EQ(cat.packages.size() == 1 ? cat.packages[0].modules : 0, size_t(6));
  CHECK((logged == std::vector<std::string>{
                       "catalog: skipped packages/swse/SAVER/IMXX_EXT.IMX: an Intermission module named IMXX_*, which "
                       "the IMX reader refuses",
                       "catalog: skipped packages/swse/SAVER/SETCUR.IMX: an Intermission module that exports "
                       "SETCURRSAVER, which the IMX reader refuses"}));
  for (const auto& l : logged) fprintf(stderr, "  %s\n", l.c_str());
  // Intermission's other two forms (The Far Side's, Dilbert's): an ASA
  // animation, by its header ("AniN", or "AniM"), whatever its name; an
  // .IMQ exporting SAVERMAIN, its own reader, unless it is named as
  // Intermission's readers are.
  {
    const Package* fs_pkg = find_package("farside");
    CHECK(fs_pkg && fs_pkg->delrina_installer());
    for (const char* magic : {"AniN", "AniM"}) {
      CatalogModule a = catalog_module(write("HELL.ASA", std::string(magic) + std::string(600, '\x07')),
                                       "packages/farside/SAVER/HELL.ASA", fs_pkg);
      CHECK(a.id == "farside.hell" && a.lane == "ne16" && a.abi == "intermission" && a.entry == "SAVERMAIN");
      CHECK(a.display_name == "hell" && a.module_name == "hell" && a.about.empty() && !a.credits);
      CHECK(a.needs.empty() && a.system.empty() && a.package == "farside");
      CHECK(a.controls.size() == 1 && a.controls[0].name == "Configure..." && a.controls[0].kind == "button");
    }
    std::string why = "listed";
    try {
      catalog_module(write("FAKE.ASA", "Anim and more"), "x", fs_pkg);
    } catch (const ImportError& e) {
      why = e.what();
    }
    CHECK(why.find("not a PE32 or NE image") != std::string::npos);
    const std::vector<std::string> imq = {"WEP", "SAVERMAIN", "SAVERDLGPROC", "SAVERDLGPROC2"};
    CatalogModule q = catalog_module(write("PTERY.IMQ", synth_imx(imq, {"INTRMLIB", "ANTSW", "DIBDLL", "MMSYSTEM"})),
                                     "packages/farside/SAVER/PTERY.IMQ", fs_pkg);
    CHECK(q.id == "farside.ptery" && q.lane == "ne16" && q.abi == "intermission" && q.entry == "SAVERMAIN");
    CHECK(q.display_name == "ptery" && q.about.empty() && q.controls.size() == 1);
    CHECK((q.needs == std::vector<std::string>{"ANTSW", "DIBDLL", "INTRMLIB"}) &&
          (q.system == std::vector<std::string>{"MMSYSTEM"}));
    CHECK(catalog_module(write("NODLG.IMQ", synth_imx({"WEP", "SAVERMAIN"})), "x", fs_pkg).controls.empty());
    // MODULE, or SAVERINIT and SAVERDRAW, win over SAVERMAIN, as everywhere.
    CHECK(catalog_module(write("BOTH.IMQ", synth_imx({"SAVERMAIN", "SAVERINIT", "SAVERDRAW"})), "x", fs_pkg).entry ==
          "SAVERDRAW");
    for (const char* reader : {"IMASAPLY.IMQ", "imimxply.imq", "IMAD_PLY.IMQ", "READER.IMX", "READER.DLL"}) {
      std::string got = "listed";
      try {
        catalog_module(write(reader, synth_imx({"WEP", "SAVERMAIN", "SAVERDLGPROC", "MTDLGPROC"})), "x", fs_pkg);
      } catch (const ImportError& e) {
        got = e.what();
      }
      CHECK_EQ(got, std::string("an Intermission reader (it exports SAVERMAIN), not a module"));
    }
    // A Delrina release's module folder lists *.ASA and *.IMQ with *.AD and
    // *.IMX, sorted together; its ENGINE (the ASA reader's place) lists no
    // ASA or IMQ, and a reader in the module folder is left out and logged.
    // Star Wars Screen Entertainment's folders list no ASA or IMQ at all.
    fs::path droot = dir / L"farside-tree";
    auto dput = [&](const std::wstring& rel, const std::string& data) {
      test::write_bytes(droot / rel, std::vector<uint8_t>(data.begin(), data.end()));
    };
    dput(L"SAVER\\OCEAN.ASA", "AniN ocean");
    dput(L"SAVER\\eggfight.asa", "AniM eggfight");
    dput(L"SAVER\\PTERY.IMQ", synth_imx(imq, {"INTRMLIB", "ANTSW"}));
    dput(L"SAVER\\IMASAPLY.IMQ", synth_imx(imq, {"INTRMLIB", "ANTSW"}));
    dput(L"SAVER\\INTRMLIB.DLL", synth_imx({"WEP"}));
    dput(L"SAVER\\NOTES.TXT", "AniN, but not a module file name");
    dput(L"ENGINE\\IMASAPLY.IMQ", synth_imx(imq, {"INTRMLIB", "ANTSW"}));
    dput(L"ENGINE\\HIDDEN.ASA", "AniN in ENGINE");
    CatalogTree ft;
    ft.package = fs_pkg;
    ft.dir = droot;
    std::vector<std::string> flog;
    CatalogDoc fcat = build_catalog({ft}, [&](const std::string& s) { flog.push_back(s); });
    std::vector<std::string> fids, fnames;
    for (auto& mod : fcat.modules) fids.push_back(mod.id), fnames.push_back(mod.module_name);
    CHECK((fids == std::vector<std::string>{"farside.ocean", "farside.ptery", "farside.eggfight"}));
    CHECK((fnames == std::vector<std::string>{"Ocean", "Pterodactyl", "Eggfight"}));
    CHECK((flog == std::vector<std::string>{"catalog: skipped packages/farside/SAVER/IMASAPLY.IMQ: an Intermission "
                                            "reader (it exports SAVERMAIN), not a module"}));
    for (const auto& l : flog) fprintf(stderr, "  %s\n", l.c_str());
    t.package = swse;
    t.dir = droot;
    CHECK(build_catalog({t}).modules.empty());
  }

  // Deluxe's fixed places hold *.AD only: an IMX file there is not listed.
  fs::path files = dir / L"FILES.deluxe";
  const std::string imx = synth_imx(kImxExports), ne = synth_ne();
  test::write_bytes(files / L"CLASSIC" / L"X.IMX", std::vector<uint8_t>(imx.begin(), imx.end()));
  test::write_bytes(files / L"CLASSIC" / L"Y.AD", std::vector<uint8_t>(ne.begin(), ne.end()));
  test::write_bytes(files / L"AD40" / L"Z.IMX", std::vector<uint8_t>(imx.begin(), imx.end()));
  auto deluxe = scan_catalog(files);
  CHECK_EQ(deluxe.size(), size_t(1));
  CHECK(!deluxe.empty() && deluxe[0].id == "classic.y");
}

// ---- a FILES tree ------------------------------------------------------------------------

// A FILES tree with modules of both lanes, files that are not modules, a
// damaged module, and an id collision.
void write_tree(const fs::path& files) {
  auto put = [&](const std::wstring& rel, const std::string& data) {
    test::write_bytes(files / rel, std::vector<uint8_t>(data.begin(), data.end()));
  };
  put(L"AD40\\B.AD", synth_pe());
  put(L"AD40\\A.ad", synth_pe());
  put(L"AD40\\ADXPL510.DLL", synth_pe());
  put(L"AD40\\JUNK.AD", "not a module");
  put(L"AD40\\STARRYNI.AD", synth_pe());   // takes the id the ENGINE copy would have
  put(L"ENGINE\\STARRYNI.AD", synth_pe(true));
  put(L"CLASSIC\\Z.AD", synth_ne());
  put(L"CLASSIC\\M.AD", synth_ne());
  fs::create_directories(files / L"CLASSIC" / L"SUB.AD");
}

void test_scan(const fs::path& dir) {
  fs::path files = dir / L"FILES.importing-1";   // any name: the paths still say FILES/
  write_tree(files);
  std::vector<std::string> logged;
  auto mods = scan_catalog(files, [&](const std::string& s) { logged.push_back(s); });
  std::vector<std::string> ids, paths;
  for (auto& m : mods) ids.push_back(m.id), paths.push_back(m.path);
  CHECK((ids == std::vector<std::string>{"ad40.a", "ad40.b", "ad40.starryni", "classic.m", "classic.z"}));
  CHECK((paths == std::vector<std::string>{"FILES/AD40/A.ad", "FILES/AD40/B.AD", "FILES/AD40/STARRYNI.AD",
                                           "FILES/CLASSIC/M.AD", "FILES/CLASSIC/Z.AD"}));
  CHECK_EQ(logged.size(), size_t(2));
  if (logged.size() == 2) {
    CHECK(logged[0].find("FILES/AD40/JUNK.AD") != std::string::npos);
    CHECK(logged[1].find("FILES/ENGINE/STARRYNI.AD") != std::string::npos &&
          logged[1].find("duplicate id") != std::string::npos);
  }
  bool threw = false;
  try {
    scan_catalog(dir / L"nowhere");
  } catch (const ImportError& e) {
    threw = e.status() == Status::source_invalid;
  }
  CHECK(threw);
}

// ---- After Dark 2.0: the About rules and the fixed screen (startrek) ----------------------------

void test_ad20(const fs::path& dir) {
  // The text rules alone.
  CHECK_EQ(ad20_about("TITLE\n\nWrapped by \nhand.\nTM 1992\nBerkeley Systems Authorized User."),
           std::string("TITLE\n\nWrapped by hand.\nTM 1992"));
  CHECK_EQ(ad20_about("x   \n  Berkeley Systems Authorized User.  "), std::string("x"));
  CHECK_EQ(ad20_about("Berkeley Systems Authorized User."), std::string());
  // Only the last line, only that text; a break joins only after a space and
  // before a lower-case letter.
  CHECK_EQ(ad20_about("Berkeley Systems Authorized User.\nmore"), std::string("Berkeley Systems Authorized User.\nmore"));
  CHECK_EQ(ad20_about("x\nBerkeley Systems, Inc."), std::string("x\nBerkeley Systems, Inc."));
  CHECK_EQ(ad20_about("a \nB, a\nb, a \n\nb, a \n1, burn-in. \n"), std::string("a \nB, a\nb, a \n\nb, a \n1, burn-in. \n"));
  CHECK_EQ(ad20_about(" \nx"), std::string(" x"));
  CHECK_EQ(ad20_about(""), std::string());

  // In the catalog: a Classic module of the After Dark 2.0 package has them
  // and the package's screen; the same file anywhere else is as written
  // (four texts of the other releases have such a break).
  fs::create_directories(dir);
  test::NeSpec ne;
  ne.module_refs = {"KERNEL", "AD_MOD"};
  ne.exports = {"MODULE"};
  ne.resources = {{2000, "", 20, std::string(" Two Oh") + '\0'},
                  {2000, "", 30, "TWO OH\r\n\r\nA sentence wrapped by \r\nhand.\r\nBerkeley Systems Authorized User."}};
  const std::string bytes = test::build_ne(ne);
  const fs::path file = dir / L"TWOOH.AD";
  test::write_bytes(file, std::vector<uint8_t>(bytes.begin(), bytes.end()));
  const Package* st = find_package("startrek");
  CatalogModule a = catalog_module(file, "packages/startrek/AFTERDRK/TWOOH.AD", st);
  CHECK_EQ(a.id, std::string("startrek.twooh"));
  CHECK_EQ(a.about, std::string("TWO OH\n\nA sentence wrapped by hand."));
  CHECK_EQ(a.module_name, std::string("Two Oh"));
  CHECK_EQ(a.screen, std::string("640x480"));
  for (const char* id : {"ad32", "simpsons", "deluxe"}) {
    CatalogModule o = catalog_module(file, "x/TWOOH.AD", find_package(id));
    CHECK_EQ(o.about, std::string("TWO OH\n\nA sentence wrapped by \nhand.\nBerkeley Systems Authorized User."));
    CHECK(o.screen.empty());
  }
  CHECK(catalog_module(file, "FILES/CLASSIC/TWOOH.AD").screen.empty());
  // "screen" is written last, and only when there is one.
  CatalogModule plain = a;
  plain.screen.clear();
  const std::string with = render_catalog_json({a}), without = render_catalog_json({plain});
  CHECK(with.find("   \"md5\": \"" + a.md5 + "\",\n   \"screen\": \"640x480\"\n  }\n") != std::string::npos);
  CHECK(without.find("screen") == std::string::npos);
}

// ---- JSON ----------------------------------------------------------------------------------

void test_json() {
  // PACKAGES.md §6, COVERS.md §2.7: the top-level packages list; generator
  // adimport 1.3 since Intermission modules (the "abi" field, the
  // intermission recipe).
  CHECK_EQ(render_catalog_json({}),
           std::string("{\n \"version\": 1,\n \"generator\": \"adimport 1.3\",\n \"packages\": [],\n \"modules\": []\n}\n"));
  CatalogModule m;
  m.id = "ad40.x";
  m.display_name = "X \"quoted\" \\ \x01";
  m.lane = "pe32";
  m.path = "FILES/AD40/X.AD";
  m.about = "line\n\ttab \xC2\xA9";
  m.entry = "Module";
  m.needs = {"ADXPL510.DLL"};
  CatalogControl b;
  b.index = 0;
  b.name = "Pictures";
  b.kind = b.type = "button";
  m.controls.push_back(b);
  std::string j = render_catalog_json({m});
  // Python's json.dump(indent=1, ensure_ascii=False) layout and escapes.
  CHECK(j.find("\n   \"displayName\": \"X \\\"quoted\\\" \\\\ \\u0001\",\n") != std::string::npos);
  CHECK(j.find("\"about\": \"line\\n\\ttab \xC2\xA9\"") != std::string::npos);
  CHECK(j.find("   \"controls\": [\n    {\n     \"index\": 0,\n     \"name\": \"Pictures\",\n     \"kind\": \"button\",\n"
               "     \"type\": \"button\"\n    }\n   ],\n") != std::string::npos);
  CHECK(j.find("   \"needs\": [\n    \"ADXPL510.DLL\"\n   ],\n   \"system\": []\n  }\n ]\n}\n") != std::string::npos);
  CHECK(j.find("credits") == std::string::npos);

  // Every field of every kind, read back independently.
  CatalogModule c;
  c.id = "classic.y";
  c.display_name = "Y";
  c.lane = "ne16";
  c.path = "FILES/CLASSIC/Y.AD";
  c.credits = "someone";
  c.entry = "MODULE";
  c.controls.push_back(*parse_control_record(
      test::string_slider_record("S", {"a", "b"}, {10, 20}, 15), 0));
  c.controls.push_back(*parse_control_record(test::num_slider_record("N", 1, 9, 22, "%", 1), 1));
  c.controls.push_back(*parse_control_record(test::popup_record("P", {"x", "y"}, 1), 2));
  c.controls.push_back(*parse_control_record(test::checkbox_record("C", 1), 3));
  phosg::JSON doc = phosg::JSON::parse(render_catalog_json({c}));
  const phosg::JSON& y = *doc.at("modules").as_list().at(0);
  CHECK_EQ(y.get_string("credits"), std::string("someone"));
  CHECK_EQ(y.get_string("about"), std::string());
  const auto& ctl = y.at("controls").as_list();
  CHECK_EQ(ctl.size(), size_t(4));
  if (ctl.size() == 4) {
    const phosg::JSON& s = *ctl[0];
    CHECK(s.get_string("kind") == "stringslider" && s.get_string("type") == "slider");
    CHECK(s.at("items").as_list().size() == 3 && s.at("values").as_list().size() == 3);
    CHECK(s.get_int("default") == 10 && s.get_int("defaultStop") == 1 && s.get_int("boldStop") == 2);
    const phosg::JSON& n = *ctl[1];
    CHECK(n.get_int("min") == 1 && n.get_int("max") == 9 && n.get_int("default") == 9 && n.get_int("rawDefault") == 22);
    CHECK(n.get_string("unit") == "%" && n.get_string("unitPos") == "prefix");
    const phosg::JSON& p = *ctl[2];
    CHECK(p.get_int("default") == 1 && p.at("items").as_list().size() == 2 && !p.contains("values"));
    const phosg::JSON& k = *ctl[3];
    CHECK(k.get_int("default") == 1 && !k.contains("items"));
  }

  // COVERS.md §2.7: every package gains "cover" after "modules"; a generated
  // cover is its origin alone, a picture carries the rest.
  CatalogPackage gen, pic, mine;
  gen.id = "deluxe";
  pic.id = "simpsons";
  pic.cover.origin = pic.cover.original = "download";
  pic.cover.tile = "covers/simpsons/tile.png";
  pic.cover.tile_md5 = std::string(32, 'a');
  pic.cover.image = "covers/simpsons/original.png";
  pic.cover.width = 600;
  pic.cover.height = 776;
  pic.cover.art = "box";
  pic.cover.label = "Box front";
  pic.cover.credit = "Wikisimpsons";
  mine.id = "tt";
  mine.cover = pic.cover;
  mine.cover.origin = "user";
  mine.cover.original = "disc";
  mine.cover.art.clear();
  mine.cover.label = "Your own picture";
  mine.cover.credit.clear();
  // Given in registry order; written oldest release first: simpsons (1994), tt (1995), deluxe (1996).
  std::string text = render_catalog_json({}, {gen, pic, mine});
  CHECK(text.find("   \"modules\": 0,\n   \"cover\": {\n    \"origin\": \"generated\"\n   }\n  }") !=
        std::string::npos);
  CHECK(text.find("   \"cover\": {\n    \"origin\": \"download\",\n    \"tile\": \"covers/simpsons/tile.png\",\n"
                  "    \"tileMd5\": \"" + std::string(32, 'a') + "\",\n    \"image\": \"covers/simpsons/original.png\",\n"
                  "    \"width\": 600,\n    \"height\": 776,\n    \"art\": \"box\",\n    \"label\": \"Box front\",\n"
                  "    \"credit\": \"Wikisimpsons\",\n    \"original\": \"download\"\n   }") != std::string::npos);
  phosg::JSON packages = phosg::JSON::parse(text).at("packages");
  CHECK_EQ(packages.as_list().at(0)->get_string("id"), std::string("simpsons"));
  CHECK_EQ(packages.as_list().at(1)->get_string("id"), std::string("tt"));
  CHECK_EQ(packages.as_list().at(2)->get_string("id"), std::string("deluxe"));
  CHECK_EQ(packages.as_list().at(2)->at("cover").as_dict().size(), size_t(1));
  const phosg::JSON& u = packages.as_list().at(1)->at("cover");
  CHECK_EQ(u.get_string("origin"), std::string("user"));
  CHECK_EQ(u.get_string("original"), std::string("disc"));
  CHECK(!u.contains("art"));
}

// ---- adimport --catalog-only, as a library call ------------------------------------------

void test_regenerate(const fs::path& dir) {
  // Nothing imported: nothing to catalogue, and nothing written.
  CatalogResult r = regenerate_catalog(dir / L"empty");
  CHECK_EQ(r.status, Status::source_invalid);
  CHECK(!fs::exists(dir / L"empty" / L"win" / L"catalog-win.json"));

  fs::path root = dir / L"assets";
  write_tree(root / L"win" / L"FILES");
  std::vector<std::string> logged;
  r = regenerate_catalog(root, [&](const std::string& s) { logged.push_back(s); });
  CHECK_EQ(r.status, Status::ok);
  CHECK_EQ(r.modules, size_t(5));
  CHECK_EQ(r.controls, size_t(3 + 3 + 3 + 3 + 3));
  CHECK_EQ(r.path, root / L"win" / L"catalog-win.json");
  CHECK_EQ(logged.size(), size_t(2));
  std::string text = test::read_text(r.path);
  // PACKAGES.md §6: the installed packages (Deluxe, with no import record) are listed too.
  CatalogTree tree;
  tree.package = find_package("deluxe");
  tree.dir = root / L"win" / L"FILES";
  tree.verified = "unknown";
  CHECK_EQ(text, render_catalog(build_catalog({tree})));
  CHECK_EQ(scan_catalog(root / L"win" / L"FILES").size(), size_t(5));
  phosg::JSON doc = phosg::JSON::parse(text);
  CHECK_EQ(doc.at("modules").as_list().size(), size_t(5));
  for (auto& e : fs::directory_iterator(root / L"win"))
    CHECK(e.path().filename().wstring().find(L".tmp-") == std::wstring::npos);

  // A root that is itself the win directory is updated in place.
  r = regenerate_catalog(root / L"win");
  CHECK_EQ(r.status, Status::ok);
  CHECK_EQ(r.path, root / L"win" / L"catalog-win.json");

  // An import holding the lock: refused, the old catalog left alone.
  {
    Handle lock(CreateFileW((root / L"win" / L"import.lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                            OPEN_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, nullptr));
    CHECK(lock.valid());
    test::write_bytes(r.path, {'o', 'l', 'd'});
    r = regenerate_catalog(root);
    CHECK_EQ(r.status, Status::error);
    CHECK(r.message.find("running") != std::string::npos);
    CHECK_EQ(test::read_text(r.path), std::string("old"));
  }
  CHECK_EQ(regenerate_catalog(root).status, Status::ok);
}

// ---- the real corpus ----------------------------------------------------------------------

struct Diff {
  int count = 0;
  void report(const std::string& path, const std::string& what) {
    if (++count <= 40) fprintf(stderr, "  %s: %s\n", path.c_str(), what.c_str());
  }
};

std::string brief(const phosg::JSON& j) {
  std::string s = j.serialize();
  return s.size() > 120 ? s.substr(0, 117) + "..." : s;
}

// Same value, member order aside.
void compare(const phosg::JSON& a, const phosg::JSON& b, const std::string& path, Diff& d) {
  if (a.is_dict() && b.is_dict()) {
    std::set<std::string> keys;
    for (auto& [k, v] : a.as_dict()) keys.insert(k);
    for (auto& [k, v] : b.as_dict()) keys.insert(k);
    for (const auto& k : keys) {
      if (!a.contains(k)) d.report(path + "." + k, "only in the reference: " + brief(b.at(k)));
      else if (!b.contains(k)) d.report(path + "." + k, "only in ours: " + brief(a.at(k)));
      else compare(a.at(k), b.at(k), path + "." + k, d);
    }
  } else if (a.is_list() && b.is_list()) {
    const auto &la = a.as_list(), &lb = b.as_list();
    if (la.size() != lb.size())
      d.report(path, "length " + std::to_string(la.size()) + " vs " + std::to_string(lb.size()));
    for (size_t i = 0; i < std::min(la.size(), lb.size()); i++) {
      std::string sub = path + "[" + std::to_string(i) + "]";
      // Name list entries by id where there is one: easier to read.
      if (la[i]->is_dict() && la[i]->contains("id") && la[i]->at("id").is_string())
        sub = path + "[" + la[i]->at("id").as_string() + "]";
      compare(*la[i], *lb[i], sub, d);
    }
  } else if (a.is_int() && b.is_int()) {
    if (a.as_int() != b.as_int()) d.report(path, brief(a) + " vs " + brief(b));
  } else if (a.is_string() && b.is_string()) {
    if (a.as_string() != b.as_string()) d.report(path, brief(a) + " vs " + brief(b));
  } else if (!(a.is_null() && b.is_null())) {
    d.report(path, "type differs: " + brief(a) + " vs " + brief(b));
  }
}

size_t count_controls(const phosg::JSON& doc) {
  size_t n = 0;
  for (auto& m : doc.at("modules").as_list()) n += m->at("controls").as_list().size();
  return n;
}

// The generator field names the tool, so it is the one expected difference;
// the fields PACKAGES.md §6 added (per module: package, packageTitle,
// moduleName, md5, sameAs; top level: packages) are not the prototype's and
// are ignored here (import.pkg_real checks them).
void check_same_as_reference(const std::string& ours_text, const phosg::JSON& ref, const char* what) {
  phosg::JSON ours = phosg::JSON::parse(ours_text);
  CHECK_EQ(ours.get_string("generator"), std::string(kCatalogGenerator));
  ours.as_dict().erase("generator");
  CHECK(ours.contains("packages"));
  ours.as_dict().erase("packages");
  for (auto& m : ours.at("modules").as_list()) {
    CHECK(m->contains("package") && m->contains("moduleName") && m->contains("md5"));
    for (const char* k : {"package", "packageTitle", "moduleName", "md5", "sameAs"}) m->as_dict().erase(k);
  }
  Diff d;
  compare(ours, ref, "$", d);
  if (d.count) {
    test::g_failures++;
    fprintf(stderr, "%s: %d difference(s) from the prototype's catalog\n", what, d.count);
  }
  size_t ours_modules = ours.at("modules").as_list().size(), ours_controls = count_controls(ours);
  fprintf(stderr, "%s: %zu modules, %zu controls (reference %zu, %zu)\n", what, ours_modules, ours_controls,
          ref.at("modules").as_list().size(), count_controls(ref));
}

// `installed`: the user's assets root (test::installed_assets_root()), read only.
int test_real(const fs::path& scratch, const fs::path& adimport, const fs::path& reference, const fs::path& installed) {
  fs::path win = win_assets_dir(installed);
  std::error_code ec;
  if (installed.empty() || !fs::is_directory(win / L"FILES" / L"AD40", ec) || !fs::is_regular_file(reference, ec)) {
    fprintf(stderr, "SKIP: needs imported assets (%s) and the prototype's %s\n", to_utf8(win.wstring()).c_str(),
            to_utf8(reference.wstring()).c_str());
    return 77;
  }
  phosg::JSON ref = phosg::JSON::parse(test::read_text(reference));
  ref.as_dict().erase("generator");
  // Known prototype errata, corrected in the reference before comparing so
  // that nothing else may differ. make_catalog.py asks pefile for the import
  // directory only (parse_data_directories(directories=[IMPORT])), so its
  // export list is always empty and every module gets entry "Module"; the
  // MSVC-built STARRYNI.AD exports "_Module@4" (ABI.md §1, §2.14, VERIFIED),
  // which is what the C++ generator reports.
  struct Erratum {
    const char *id, *field, *prototype, *actual;
  };
  for (const Erratum& e : {Erratum{"ad40.starryni", "entry", "Module", "_Module@4"}}) {
    for (auto& m : ref.at("modules").as_list()) {
      if (m->get_string("id") != e.id || m->get_string(e.field) != e.prototype) continue;
      m->as_dict()[e.field] = std::make_unique<phosg::JSON>(std::string(e.actual));
      fprintf(stderr, "reference erratum applied: %s.%s \"%s\" -> \"%s\"\n", e.id, e.field, e.prototype, e.actual);
    }
  }
  // The prototype's run over this corpus: 84 modules, 248 controls (ABI.md
  // §2.10.5). A different reference means a different corpus or prototype.
  CHECK_EQ(ref.at("modules").as_list().size(), size_t(84));
  CHECK_EQ(count_controls(ref), size_t(248));

  std::vector<std::string> skipped;
  auto mods = scan_catalog(win / L"FILES", [&](const std::string& s) { skipped.push_back(s); });
  for (auto& s : skipped) fprintf(stderr, "  %s\n", s.c_str());
  CHECK(skipped.empty());
  check_same_as_reference(render_catalog_json(mods), ref, "scan_catalog");

  // The same through adimport --catalog-only, on a scratch copy of just the
  // module files (the user's own catalog is never touched by a test).
  fs::path root = scratch / L"real-assets";
  fs::remove_all(root, ec);
  for (const auto& m : mods) {
    fs::path from = win / to_wide(m.path), to = root / L"win" / to_wide(m.path);
    fs::create_directories(to.parent_path());
    fs::copy_file(from, to, fs::copy_options::overwrite_existing);
  }
  test::ProcessResult pr = test::run_process(adimport.wstring(), {L"--catalog-only", L"--dest", root.wstring()}, 120000);
  fprintf(stderr, "[adimport --catalog-only] exit %d\n%s", pr.exit_code, pr.output.c_str());
  CHECK_EQ(pr.exit_code, 0);
  check_same_as_reference(test::read_text(root / L"win" / L"catalog-win.json"), ref, "adimport --catalog-only");
  fs::remove_all(root, ec);   // copies of the user's files: never left lying around
  return test::finish("import.catalog_real");
}

}  // namespace

int main(int argc, char** argv) {
  std::string suite = argc > 1 ? argv[1] : "unit";
  if (suite == "real") {
    if (argc < 5) {
      fprintf(stderr, "usage: test_import_catalog real <scratch> <adimport.exe> <reference.json>\n");
      return 2;
    }
    // The installed assets, found read only before the sandbox hides them.
    const fs::path installed = test::installed_assets_root();
    fs::path scratch = test::scratch(argc - 1, argv + 1, "adw-import-catalog-real");
    test::sandbox_data_root(scratch / L"localappdata");
    return test_real(scratch, fs::absolute(argv[3]), fs::absolute(argv[4]), installed);
  }
  fs::path dir = test::scratch(argc - 1, argv + 1, "adw-import-catalog");
  test::sandbox_data_root(dir / L"localappdata");  // no default may reach the real data folder
  test_text();
  test_records();
  test_modules(dir / L"modules");
  test_imx(dir / L"imx");
  test_scan(dir / L"scan");
  test_ad20(dir / L"ad20");
  test_json();
  test_regenerate(dir / L"regen");
  return test::finish("import.catalog");
}
