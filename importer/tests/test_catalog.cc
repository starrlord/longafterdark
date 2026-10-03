// catalog-win.json generation (catalog.h).
//
//   test_import_catalog unit <scratch>
//     the text and record readers against hand-made inputs, whole synthetic
//     PE32 / NE modules (tests/module_builder.h), NE modules told apart by
//     their exports as the ne16 lane does (MODULE first; what it refuses
//     left out, with its reason), a scan of a synthetic FILES tree and of a
//     package tree with *.AD and *.IMX, the JSON layout (abi last, IMX
//     entries only), the Speed control (a made-up Intermission 4.0 tree: on
//     the registry's speed modules alone, after the Configure... button,
//     "host" last; the same files under another package as before; the
//     registry's list against the package's modules), After Dark 2.0's
//     About rules and "screen" (the startrek package's entries only), and
//     regenerate_catalog (adimport --catalog-only)
//   test_import_catalog real <scratch> <adimport.exe> <reference.json>
//     semantic identity with the prototype's output (research/win/
//     make_catalog.py -> research/win/catalog-win.json) over the real
//     corpus, both through the library and through adimport --catalog-only
//     on a scratch copy of the modules. Exit 77 (skipped) when the imported
//     assets or the reference are not on this machine.
#include <phosg/JSON.hh>

#include <functional>
#include <map>
#include <set>
#include <tuple>

#include "catalog.h"
#include "importer.h"
#include "module_builder.h"
#include "names.h"
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

// ---- the Speed control (Package::speed_modules) ---------------------------------------------

// The catalog's Speed control as the JSON lays it out inside "controls" (a
// module's), starting at `stop` (its value the default): the exact text the
// front-ends read.
std::string speed_json(int stop) {
  static const int kValues[] = {6, 12, 25, 50, 100};
  return "    {\n"
         "     \"index\": 1,\n"
         "     \"name\": \"Speed:\",\n"
         "     \"kind\": \"stringslider\",\n"
         "     \"type\": \"slider\",\n"
         "     \"items\": [\n"
         "      \"Slowest\",\n"
         "      \"Slow\",\n"
         "      \"Normal\",\n"
         "      \"Fast\",\n"
         "      \"Fastest\"\n"
         "     ],\n"
         "     \"values\": [\n"
         "      6,\n"
         "      12,\n"
         "      25,\n"
         "      50,\n"
         "      100\n"
         "     ],\n"
         "     \"default\": " +
         std::to_string(kValues[stop]) + ",\n     \"defaultStop\": " + std::to_string(stop) +
         ",\n"
         "     \"host\": \"ADNE16IMXSPEED\"\n"
         "    }\n";
}

// Two controls alike, field for field.
bool same_control(const CatalogControl& a, const CatalogControl& b) {
  return std::tie(a.index, a.name, a.kind, a.type, a.items, a.values, a.def, a.default_stop, a.bold_stop, a.min, a.max,
                  a.raw_default, a.unit, a.unit_pos, a.host) ==
         std::tie(b.index, b.name, b.kind, b.type, b.items, b.values, b.def, b.default_stop, b.bold_stop, b.min, b.max,
                  b.raw_default, b.unit, b.unit_pos, b.host);
}

void test_speed(const fs::path& dir) {
  // The control: a host control (ADNE16IMXSPEED, never ADCVSET or SET), the
  // same five stops for every module, starting at the module's own (Normal
  // unless its row says otherwise).
  const std::vector<std::string> stops = {"Slowest", "Slow", "Normal", "Fast", "Fastest"};
  const std::vector<int> values = {6, 12, 25, 50, 100};
  const CatalogControl s = speed_control();
  CHECK(s.index == 1 && s.name == "Speed:" && s.kind == "stringslider" && s.type == "slider");
  CHECK(s.items == stops && s.values == values);
  CHECK(s.def == 25 && s.default_stop == 2 && !s.bold_stop && !s.min && !s.max && !s.raw_default && s.unit.empty());
  CHECK_EQ(s.host, std::string("ADNE16IMXSPEED"));
  CHECK_EQ(std::string(kSpeedHost), std::string("ADNE16IMXSPEED"));
  for (int k = 0; k < 5; k++) {
    const CatalogControl at = speed_control(SpeedStop(k));
    CHECK(at.index == 1 && at.items == stops && at.values == values && at.host == s.host && !at.bold_stop);
    CHECK(at.default_stop == k && at.def == values[size_t(k)]);
  }

  // The registry: Intermission 4.0 alone lists modules; each is one of its
  // modules (its table installs it, its manifest has it, a name override
  // names it), none twice. Its IMX, IMQ, MRF and MSV modules are listed but
  // for the ten its measurement found the same at any speed (their own
  // clocks, or their readers': research/speed/host/FACTS.md); no ASA or FLI
  // animation is (their readers' clocks). Six start elsewhere than Normal:
  // Dragon Kites, Ping and Bricks at Slowest, Wriggly and Snow Flakes at
  // Slow, Space Shark at Fast.
  for (const Package& p : builtin_packages())
    CHECK_EQ(p.speed_modules.empty(), std::string_view(p.id) != "intermission");
  const Package* im = find_package("intermission");
  CHECK(im != nullptr);
  if (!im) return;
  std::set<std::string> listed, expected;
  std::map<std::string, SpeedStop> not_normal;
  for (const SpeedModule& m : im->speed_modules) {
    CHECK(listed.insert(m.module).second);
    if (m.start != SpeedStop::normal) not_normal[m.module] = m.start;
  }
  for (const std::string& m : listed) {
    CHECK(m.rfind("SAVER/", 0) == 0 && !ends_with_i(m, ".ASA") && !ends_with_i(m, ".FLI"));
    bool installed = false, known = false, named = false;
    for (const LooseFile& lf : im->loose_files) installed = installed || m == lf.to;
    for (const KnownFile& k : im->manifest) known = known || std::string(im->root) + "/" + m == k.path;
    for (const NameOverride& o : im->name_overrides) named = named || m == o.module;
    if (!installed || !known || !named) fprintf(stderr, "  speed module %s: not one of the package's\n", m.c_str());
    CHECK(installed && known && named);
  }
  const std::set<std::string> same_at_any_speed = {
      "SAVER/BIGFOOT.IMX", "SAVER/COMMNQUE.IMX", "SAVER/CONUND.IMX", "SAVER/FADE.IMX",     "SAVER/FLEX.IMX",
      "SAVER/MAZE.IMX",    "SAVER/ORBS.IMX",     "SAVER/PHOTO.IMX",  "SAVER/TIMEPIEC.IMX", "SAVER/PARADISE.MRF"};
  for (const LooseFile& lf : im->loose_files) {
    const std::string to = lf.to;
    if (to.rfind("SAVER/", 0) == 0 && !is_intermission_reader(lf.from) && !same_at_any_speed.count(to) &&
        (ends_with_i(to, ".IMX") || ends_with_i(to, ".IMQ") || ends_with_i(to, ".MRF") || ends_with_i(to, ".MSV")))
      expected.insert(to);
  }
  for (const std::string& m : expected)
    if (!listed.count(m)) fprintf(stderr, "  speed module %s: not listed\n", m.c_str());
  CHECK(listed == expected);
  CHECK_EQ(listed.size(), size_t(36));  // 34 IMX, IMSHARK.IMQ, MACHINE.MSV
  CHECK((not_normal == std::map<std::string, SpeedStop>{{"SAVER/DRAGON.IMX", SpeedStop::slowest},
                                                        {"SAVER/PING.IMX", SpeedStop::slowest},
                                                        {"SAVER/BRICKS.IMX", SpeedStop::slowest},
                                                        {"SAVER/WORMS.IMX", SpeedStop::slow},
                                                        {"SAVER/SNOW.IMX", SpeedStop::slow},
                                                        {"SAVER/IMSHARK.IMQ", SpeedStop::fast}}));

  // A made-up Intermission 4.0 tree. A listed module gets the control after
  // its Configure... button (its key matched without case: worms.imx), at
  // its own starting stop; one without a dialog gets it alone, still at
  // index 1; an unlisted module of the release (the clock, the morph, the
  // ASA and FLI animations), a module it does not have and an After Dark
  // module at a listed path (its slot 1 is its own) do not.
  const fs::path root = dir / L"im40-tree";
  auto put = [&](const std::wstring& rel, const std::string& data) {
    test::write_bytes(root / rel, std::vector<uint8_t>(data.begin(), data.end()));
  };
  const std::vector<std::string> imq = {"WEP", "SAVERMAIN", "SAVERDLGPROC"};
  put(L"SAVER\\DRAGON.IMX", synth_imx(kImxExports, {"INTRMLIB", "KERNEL", "GDI"}));
  put(L"SAVER\\worms.imx", synth_imx(kImxExports, {"INTRMLIB", "USER"}));
  put(L"SAVER\\SNOW.IMX", synth_imx({"WEP", "SAVERINIT", "SAVERDRAW"}, {"INTRMLIB"}));
  put(L"SAVER\\TIMEPIEC.IMX", synth_imx(kImxExports, {"INTRMLIB"}));
  put(L"SAVER\\NEWONE.IMX", synth_imx(kImxExports, {"INTRMLIB"}));
  put(L"SAVER\\IMSHARK.IMQ", synth_imx(imq, {"INTRMLIB", "ANTSW"}));
  put(L"SAVER\\PARADISE.MRF", "a made-up morph");
  put(L"SAVER\\MACHINE.MSV", "a made-up mix");
  put(L"SAVER\\FACE.ASA", "AniN a made-up face");
  put(L"SAVER\\EINSTEIN.FLI", std::string("\x00\x04\x00\x00\x12\xAF", 6) + std::string(200, '\x01'));
  put(L"SAVER\\MOSAIC.IMX", synth_ne());
  CatalogTree t;
  t.package = im;
  t.dir = root;
  std::vector<std::string> logged;
  const CatalogDoc doc = build_catalog({t}, [&](const std::string& l) { logged.push_back(l); });
  for (const auto& l : logged) fprintf(stderr, "  %s\n", l.c_str());
  CHECK(logged.empty());
  CHECK_EQ(doc.modules.size(), size_t(11));
  auto entry = [&](const CatalogDoc& d, const std::string& id) -> const CatalogModule* {
    for (const CatalogModule& m : d.modules)
      if (m.id == id) return &m;
    fprintf(stderr, "  no entry %s\n", id.c_str());
    return nullptr;
  };
  // Configure... then Speed: / Configure... alone / Speed: alone; the stop
  // Speed: starts at.
  enum class Has { both, button, speed };
  struct Want {
    const char* id;
    Has has;
    SpeedStop start;
  };
  for (const Want& w : {Want{"intermission.dragon", Has::both, SpeedStop::slowest},
                        Want{"intermission.worms", Has::both, SpeedStop::slow},
                        Want{"intermission.imshark", Has::both, SpeedStop::fast},
                        Want{"intermission.machine", Has::both, SpeedStop::normal},
                        Want{"intermission.snow", Has::speed, SpeedStop::slow},
                        Want{"intermission.timepiec", Has::button, SpeedStop::normal},
                        Want{"intermission.newone", Has::button, SpeedStop::normal},
                        Want{"intermission.paradise", Has::button, SpeedStop::normal},
                        Want{"intermission.face", Has::button, SpeedStop::normal},
                        Want{"intermission.einstein", Has::button, SpeedStop::normal}}) {
    const CatalogModule* m = entry(doc, w.id);
    CHECK(m != nullptr);
    if (!m) continue;
    CHECK_EQ(m->abi, std::string("intermission"));
    const std::vector<CatalogControl>& c = m->controls;
    CHECK_EQ(c.size(), size_t(w.has == Has::both ? 2 : 1));
    if (c.size() != (w.has == Has::both ? 2u : 1u)) continue;
    if (w.has != Has::speed) CHECK(c[0].index == 0 && c[0].name == "Configure..." && c[0].host.empty());
    if (w.has != Has::button) CHECK(same_control(c.back(), speed_control(w.start)));
    else CHECK(c[0].kind == "button");
  }
  CHECK_EQ(entry(doc, "intermission.worms") ? entry(doc, "intermission.worms")->module_name : "", std::string("Wriggly"));
  // The After Dark module keeps its own controls (slots 1, 2 and 4) and no
  // more.
  if (const CatalogModule* ad = entry(doc, "intermission.mosaic")) {
    CHECK(ad->abi.empty() && ad->entry == "MODULE");
    CHECK(ad->controls.size() == 3 && ad->controls[1].index == 1 && ad->controls[1].kind == "popup");
    for (const CatalogControl& c : ad->controls) CHECK(c.host.empty());
  }
  // The JSON: the Configure... button, then the Speed control, exactly (at
  // Slowest for Dragon Kites, at Normal for the mix; Snow Flakes' alone, at
  // Slow); no other control has "host".
  const std::string text = render_catalog(doc);
  auto both = [](int stop, const char* entry_point) {
    return "   \"controls\": [\n    {\n     \"index\": 0,\n     \"name\": \"Configure...\",\n"
           "     \"kind\": \"button\",\n     \"type\": \"button\"\n    },\n" +
           speed_json(stop) + "   ],\n   \"entry\": \"" + entry_point + "\",\n";
  };
  auto in_entry = [&](const std::string& id, const std::string& part) {
    const size_t at = text.find("\"id\": \"" + id + "\""), end = text.find("\n  }", at);
    return at != std::string::npos && text.find(part, at) < end;
  };
  CHECK(in_entry("intermission.dragon", both(0, "SAVERDRAW")));
  CHECK(in_entry("intermission.machine", both(2, "SAVERMAIN")));
  CHECK(in_entry("intermission.snow", "   \"controls\": [\n" + speed_json(1) + "   ],\n   \"entry\": \"SAVERDRAW\",\n"));
  size_t hosts = 0;
  for (size_t at = text.find("\"host\""); at != std::string::npos; at = text.find("\"host\"", at + 1)) hosts++;
  CHECK_EQ(hosts, size_t(5));
  // Read back independently: the same control everywhere but its default.
  const phosg::JSON parsed = phosg::JSON::parse(text);
  std::map<std::string, int64_t> defaults;
  for (const auto& m : parsed.at("modules").as_list()) {
    const auto& ctl = m->at("controls").as_list();
    for (const auto& c : ctl) {
      if (!c->contains("host")) continue;
      CHECK(c->get_int("index") == 1 && c->get_string("name") == "Speed:" && c->get_string("kind") == "stringslider");
      CHECK_EQ(c->get_string("type"), std::string("slider"));
      CHECK_EQ(c->get_string("host"), std::string("ADNE16IMXSPEED"));
      CHECK(c->at("items").as_list().size() == 5 && c->at("items").as_list()[4]->as_string() == "Fastest");
      CHECK(c->at("values").as_list().size() == 5 && c->at("values").as_list()[0]->as_int() == 6 &&
            c->at("values").as_list()[4]->as_int() == 100);
      const int64_t stop = c->get_int("defaultStop");
      CHECK(stop >= 0 && stop < 5 && c->get_int("default") == values[size_t(stop)]);
      CHECK(!c->contains("boldStop"));
      defaults[m->get_string("id")] = c->get_int("default");
    }
  }
  CHECK((defaults == std::map<std::string, int64_t>{{"intermission.dragon", 6},
                                                    {"intermission.worms", 12},
                                                    {"intermission.snow", 12},
                                                    {"intermission.imshark", 50},
                                                    {"intermission.machine", 25}}));

  // Another package, the same files: no Speed control anywhere, every entry's
  // controls the file's own (catalog_module's), and the JSON has no "host" —
  // The Flintstones' (IMX and IMQ modules, Delrina's installer, as
  // Intermission 4.0's) and Star Wars Screen Entertainment's (IMX only).
  for (const char* other : {"flintstones", "swse"}) {
    const Package* p = find_package(other);
    CHECK(p != nullptr && p->speed_modules.empty());
    if (!p) continue;
    CatalogTree o;
    o.package = p;
    o.dir = root;
    const CatalogDoc od = build_catalog({o});
    CHECK_EQ(od.modules.size(), size_t(std::string_view(other) == "swse" ? 6 : 11));
    for (const CatalogModule& m : od.modules) {
      const std::string rel = m.path.substr(std::string(p->root).size() + 1);
      const CatalogModule alone = catalog_module(root / to_wide(rel), m.path, p);
      CHECK_EQ(m.controls.size(), alone.controls.size());
      for (size_t k = 0; k < std::min(m.controls.size(), alone.controls.size()); k++)
        CHECK(same_control(m.controls[k], alone.controls[k]) && m.controls[k].host.empty());
    }
    const std::string otext = render_catalog(od);
    CHECK(otext.find("\"host\"") == std::string::npos && otext.find("Speed:") == std::string::npos);
  }

  // The list is the registry's data: a made-up release whose list names
  // NEWONE.IMX (in lower case, starting at Fastest) and a file the tree
  // lacks gives NEWONE.IMX the control and DRAGON.IMX none.
  Package made = *find_package("flintstones");
  const SpeedModule made_speed[] = {{"saver/newone.imx", SpeedStop::fastest}, {"SAVER/ABSENT.IMX"}};
  made.speed_modules = made_speed;
  CatalogTree mt;
  mt.package = &made;
  mt.dir = root;
  const CatalogDoc md = build_catalog({mt});
  const CatalogModule* newone = entry(md, "flintstones.newone");
  const CatalogModule* dragon = entry(md, "flintstones.dragon");
  CHECK(newone && newone->controls.size() == 2 &&
        same_control(newone->controls[1], speed_control(SpeedStop::fastest)) && newone->controls[1].def == 100);
  CHECK(dragon && dragon->controls.size() == 1 && dragon->controls[0].name == "Configure...");
}

// ---- a FILES tree ------------------------------------------------------------------------

// A FILES tree with modules of both lanes, files that are not modules, a
// damaged module, and an id collision.
// A Windows 3.1 screen-saver program (SCRNSAVE.LIB's convention): an NE
// program exporting SCREENSAVERPROC, named by its description; a library
// exporting it is no program; MODULE still wins. Made-up bytes.
std::string synth_scr(const std::string& description, const std::vector<std::string>& exports, bool program = true) {
  test::NeSpec ne;
  ne.module_name = "SYNTHSCR";
  ne.program = program;
  ne.description = description;
  ne.module_refs = {"MMSYSTEM", "GDI", "KERNEL", "USER"};
  ne.exports = exports;
  return test::build_ne(ne);
}

void test_scrnsave(const fs::path& dir) {
  fs::create_directories(dir);
  auto write = [&](const std::string& name, const std::string& data) {
    fs::path p = dir / to_wide(name);
    test::write_bytes(p, std::vector<uint8_t>(data.begin(), data.end()));
    return p;
  };
  const Package* cast = find_package("castaway");
  CHECK(cast && cast->recipe == Recipe::is1);
  const std::vector<std::string> both = {"SCREENSAVERPROC", "SCREENSAVERCONFIGUREDIALOG", "PASSWORDDIALOG"};
  CatalogModule m = catalog_module(write("SCRANTIC.SCR", synth_scr("SCRNSAVE :Made Up Antics ", both)),
                                   "packages/castaway/SCRANTIC/SCRANTIC.SCR", cast);
  CHECK(m.id == "castaway.scrantic" && m.lane == "ne16" && m.abi == "scrnsave" && m.entry == "SCREENSAVERPROC");
  // Its own name, as Windows 3.1's Control Panel read it (trimmed); the
  // registry's override is the merge's business.
  CHECK(m.module_name == "Made Up Antics" && m.display_name == "Made Up Antics" && m.about.empty() && !m.credits);
  CHECK(m.controls.size() == 1 && m.controls[0].index == 0 && m.controls[0].name == "Setup..." &&
        m.controls[0].type == "button" && m.controls[0].kind == "button");
  CHECK(m.needs.empty() && (m.system == std::vector<std::string>{"GDI", "KERNEL", "MMSYSTEM", "USER"}));
  CHECK(m.screen == "640x480");
  // No dialog export, no button; "SCRNSAVE: Name" (no blank before the
  // colon) and lower case read alike; any other description: the file stem.
  CatalogModule plain =
      catalog_module(write("PLAIN.SCR", synth_scr("scrnsave: Plain", {"SCREENSAVERPROC"})), "x", cast);
  CHECK(plain.controls.empty() && plain.module_name == "Plain");
  CHECK_EQ(catalog_module(write("OTHER.SCR", synth_scr("A screen saver", both)), "x", cast).module_name,
           std::string("other"));
  CHECK_EQ(catalog_module(write("NONE.SCR", synth_scr("", both)), "x", cast).module_name, std::string("none"));
  // A library exporting SCREENSAVERPROC is no program: left out, by name.
  try {
    catalog_module(write("LIB.SCR", synth_scr("SCRNSAVE :Lib", both, false)), "x", cast);
    CHECK(false);
  } catch (const ImportError& e) {
    CHECK(std::string(e.what()).find("a library that exports SCREENSAVERPROC") != std::string::npos);
  }
  // MODULE still wins (an After Dark module), whatever else it exports.
  CHECK(
      catalog_module(write("AD.SCR", synth_scr("SCRNSAVE :X", {"MODULE", "SCREENSAVERPROC"})), "x", cast).abi.empty());
  // The JSON: "abi" then "screen", last.
  m.md5 = std::string(32, 'c');
  const std::string j = render_catalog_json({m});
  CHECK(j.find("\"md5\": \"" + std::string(32, 'c') +
               "\",\n   \"abi\": \"scrnsave\",\n   \"screen\": \"640x480\"\n  }") != std::string::npos);
  // The merge: only an InstallShield 1 package's module folders list *.SCR
  // (never ENGINE's, never another package's), and castaway's override
  // names its program Johnny Castaway.
  const fs::path root = dir / L"tree";
  fs::create_directories(root / L"SCRANTIC");
  fs::create_directories(root / L"ENGINE");
  auto put = [](const fs::path& p, const std::string& data) {
    test::write_bytes(p, std::vector<uint8_t>(data.begin(), data.end()));
  };
  put(root / L"SCRANTIC" / L"SCRANTIC.SCR", synth_scr("SCRNSAVE :Made Up Antics", both));
  put(root / L"ENGINE" / L"SPARE.SCR", synth_scr("SCRNSAVE :Spare", both));
  CatalogTree t;
  t.package = cast;
  t.dir = root;
  const CatalogDoc doc = build_catalog({t});
  CHECK(doc.modules.size() == 1 && doc.modules[0].id == "castaway.scrantic" &&
        doc.modules[0].module_name == "Johnny Castaway" && doc.modules[0].display_name == "Johnny Castaway");
  CatalogTree other;
  other.package = find_package("tng");
  other.dir = root;
  CHECK(build_catalog({other}).modules.empty());
}

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
  test_speed(dir / L"speed");
  test_scrnsave(dir / L"scrnsave");
  test_scan(dir / L"scan");
  test_ad20(dir / L"ad20");
  test_json();
  test_regenerate(dir / L"regen");
  return test::finish("import.catalog");
}
