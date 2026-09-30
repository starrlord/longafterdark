// Where a Classic module's support files come from (PACKAGES.md §7.1/§7.3,
// DESIGN.md §7): the package rule, the module's kind, the bridge and reader
// choices and the AD palettes.
//
//   module dir   = the folder of the module file
//   package root = its parent; engine dir = <package root>\ENGINE
//   packaged     = the package root's parent is named "packages" (any case)
//   windows dir  = <package root>\WINDOWS, when that folder exists (packaged
//                  only): what the original installer put in C:\WINDOWS
//                  (SWSE.INI), the read-only lower layer of the guest's
//                  C:\WINDOWS whatever the module's protocol — a folder
//                  rule, never a package-id rule. Never a module folder.
//
// Packaged modules resolve DLLs from the module dir, then the engine dir, and
// nothing outside their package; C:\WINDOWS\SYSTEM is the engine dir. Every
// other module is legacy and keeps the lane's original rule: the engine dir
// is <win>\FILES\ENGINE (or the module's own folder when only that holds
// OLDMOD16.DLL), and DLLs come from the module dir, <win>\FILES\CLASSIC, then
// the engine dir.
//
// Kind (ADNE16KIND=auto|ad3|imx; lane.hh "Module protocols"): auto reads the
// module's exports, resident or non-resident, in any case — MODULE is an
// After Dark 2.x/3.x module (ad3; it wins when both are there); SAVERINIT
// and SAVERDRAW an Intermission module (imx), unless it also exports
// SETCURRSAVER or its file name starts IMXX_, both of which Intermission's
// IMX reader refuses (IMIMXPLY 2:03c7, 2:044d); SAVERMAIN alone is an
// Intermission reader, not a module; anything else is not a module at all.
//
// Bridge (ADNE16BRIDGE=auto|oldmod16|native, ad3): auto runs the real
// OLDMOD16.DLL when the engine dir holds one, else the host-native AD3 bridge
// (bridge.hh) over the engine dir's AD_SND.DLL — or, when the engine dir
// holds no AD_SND.DLL (a rule by file: Snoopy's Screen Savers, eight modules
// that used the user's own After Dark), over the host's own AD_SND
// (win16/adsnd16.cc), which the AD3 protocol registers first.
//
// Palettes: AFTERDAR.SCR's AD_PALETTE 101..104 for OLDMOD16; for the native
// bridge ADTASK.DLL's 5000/1..4 when the engine dir holds it (hpal[0..3] =
// 5000/3, 5000/1, 5000/4, 5000/2, so palette request 10+k selects
// 5000/(k+1)), else AFTERDAR.SCR's, else — neither file in the engine dir,
// again a rule by file: Star Trek: The Screen Saver (After Dark 2.0), Marvel
// Comics Screen Posters, Snoopy's Screen Savers — After Dark 2.0's four as
// its AD.EXE 2.0b computed them in code
// (ABI.md §3.9): the same algorithm, run here, which gives the bytes
// ADTASK's and AFTERDAR.SCR's palettes hold. No palette is ever copied from
// Berkeley's files into the host: what the host has is that algorithm,
// never the data (PACKAGES.md §7.4). AD.EXE built a palette when a module
// asked for one, so these reach the bridge at the first palette request
// (AdPalettes::computed, bridge.hh defer_palettes), still through
// SETADPALETTE16 in its order; a package whose modules ask for none runs as
// it did without them (no Star Trek or Marvel module asks). Collage, one of
// Snoopy's modules, asks for palette 12 (the grey ramp) at INITIALIZE.
//
// After Dark 2.0 (ad3): a module folder that holds AD_MOD.DLL, After Dark
// 2.0's module library (Star Trek: The Screen Saver's; no other release has
// one), is After Dark 2.0's — a rule by file, never a package-id rule. Its
// AD3 protocol seeds AD_PREFS.INI (win16/dos16.hh seed_after_dark2) and takes
// DRAWFRAME's result 5, which AD.EXE 2.0 took as its wake, as the module's
// wake (lane.hh "The AD3 protocol").
//
// After Dark 3.x (ad3): an engine dir that holds ADW30.EXE, the After Dark
// 3.x host (After Dark 3.2, Totally Twisted, the Simpsons, the Disney
// Collection and the other AD 3.x collections install it), is After Dark
// 3.x's — a rule by file again. Its AD3 protocol seeds the keys ADW30 wrote
// into AD_PREFS.INI at every start (win16/dos16.hh seed_after_dark3), which
// ADXPL100, the Disney Collection's module library, needs; After Dark 2.0's
// rule wins when both hold.
//
// Reader (ADNE16READER=auto|imq|native, imx): auto runs Intermission's own
// IMX reader, IMIMXPLY.IMQ, from the engine dir (the guest's
// C:\WINDOWS\SYSTEM), else from the module dir, when either holds it; else
// the host-native reader (imreader.hh).
#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace adw::loader::ne {
class Image;
}

namespace adw::ne16 {

enum class BridgeKind { oldmod16, native };
const char* bridge_name(BridgeKind k);

struct Ne16Layout {
  std::string module_path;  // full host path
  std::string module_dir;
  std::string package_root;  // packaged only
  std::string package_id;    // packaged only: the root's folder name (logs)
  bool packaged = false;
  std::string engine_dir;
  std::string windows_dir;  // packaged only: <package root>\WINDOWS when it exists, else ""
  // Host directories the module table searches after the guest's directories.
  std::vector<std::string> search_dirs;
};

using FileExists = std::function<bool(const std::string&)>;

// The rule above. `win` is the assets' win dir (legacy modules only use it);
// `dir_exists` answers for the package root's WINDOWS folder (none: no
// windows dir).
Ne16Layout resolve_layout(const std::string& module_full_path, const std::string& win, const FileExists& exists,
                          const FileExists& dir_exists = {});

// ---- the module's kind ----------------------------------------------------------------------------------------

enum class ModuleKind { ad3, imx };
const char* kind_name(ModuleKind k);  // "ad3", "imx"
// What the exports say (the rule above). !ok: the lane refuses the module,
// and `why` says what it is instead.
struct KindProbe {
  bool ok = false;
  ModuleKind kind = ModuleKind::ad3;
  std::string why;
};
// `file_name` is the module file's name (the IMXX_ rule).
KindProbe detect_kind(const loader::ne::Image& img, const std::string& file_name);
// ADNE16KIND's value ("" = auto). False when it is not auto, ad3 or imx.
bool parse_kind_choice(const std::string& value, bool* is_auto, ModuleKind* forced);

// ---- the IMX reader -------------------------------------------------------------------------------------------

enum class ReaderKind { imq, native };
const char* reader_name(ReaderKind k);  // "imq", "native"
// ADNE16READER's value ("" = auto). False when it is not auto, imq or native.
bool parse_reader_choice(const std::string& value, bool* is_auto, ReaderKind* forced);
// IMIMXPLY.IMQ in the engine dir, else in the module dir (host = "" when neither has it).
struct ReaderFile {
  std::string host;
  bool in_engine_dir = false;  // else the module dir's
};
constexpr const char* kImxReader = "IMIMXPLY.IMQ";
ReaderFile find_reader(const Ne16Layout& layout, const FileExists& exists);

// ADNE16BRIDGE's value ("" = auto). False when the value is not one of the three.
bool parse_bridge_choice(const std::string& value, bool* is_auto, BridgeKind* forced);
// auto: OLDMOD16 when the engine dir has OLDMOD16.DLL, else native.
BridgeKind choose_bridge(const Ne16Layout& layout, const FileExists& exists);

// After Dark 2.0's module library; a module folder holding it is After Dark 2.0's (the rule above).
constexpr const char* kAfterDark2Library = "AD_MOD.DLL";
bool after_dark2(const Ne16Layout& layout, const FileExists& exists);
// After Dark 3.x's host; an engine dir holding it is After Dark 3.x's (the rule above).
constexpr const char* kAfterDark3Host = "ADW30.EXE";
bool after_dark3_host(const Ne16Layout& layout, const FileExists& exists);

// The sound library the native bridge loads from the engine dir; an engine
// dir without it (or no engine dir at all) gets the host's own AD_SND (the
// rule above; the native bridge only).
constexpr const char* kAdSndLibrary = "AD_SND.DLL";
bool host_ad_snd(const Ne16Layout& layout, const FileExists& exists);

// The four palettes handed to SETADPALETTE16(hpal[i], i), and where they came from.
struct AdPalettes {
  std::vector<std::vector<PALETTEENTRY>> pal;  // 4 entries, or empty when unavailable
  std::string source;                          // "…\AFTERDAR.SCR AD_PALETTE 101..104", "…\ADTASK.DLL 5000/1..4", …
  std::string error;                           // why they are unavailable
  // After Dark 2.0's, computed (palettes_after_dark2): AD.EXE 2.0b built a
  // palette when a module asked for one, so the lane hands these to the
  // bridge at the first palette request, not at load (bridge.hh
  // defer_palettes) — a module that asks for none sees no call at all.
  bool computed = false;
};
// AFTERDAR.SCR (PE) AD_PALETTE 101..104 in that order.
AdPalettes palettes_from_scr(const std::string& scr);
// ADTASK.DLL (NE) resources 5000/1..4, ordered for SETADPALETTE16: 5000/3, 5000/1, 5000/4, 5000/2.
AdPalettes palettes_from_adtask(const std::string& adtask);
// After Dark 2.0's four, computed as AD.EXE 2.0b computed them (ABI.md §3.9),
// ordered for SETADPALETTE16: the palettes of requests 12, 10, 13, 11. Each
// has 235 PC_RESERVED entries: 10 a hue sweep (h = 0x217 + 0x11D·i, full
// saturation and value), 11 a 6×6×6 cube of 255, 204, 153, 102, 51 and 0
// (blue fastest) and 19 greys (255, then 12 upward in steps of 13), 12 a
// grey ramp (i, i, i), 13 seven ramps (white, red, orange, yellow, green,
// blue, magenta) of 34 entries, the last cut to 31.
AdPalettes palettes_after_dark2();
// The bridge's palette source (see the header comment).
AdPalettes load_palettes(const Ne16Layout& layout, BridgeKind bridge, const FileExists& exists);

}  // namespace adw::ne16
