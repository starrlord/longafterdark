// Lane — the seam between the protocol host and whatever produces frames.
// The PE32 lane (AD 4 modules on ADXPL510.DLL) and the NE lane (Classic
// modules on ADXPL300.DLL + OLDMOD16.DLL, and Intermission modules through
// IMIMXPLY.IMQ) plug in here; so does the built-in test pattern.
//
// Lifecycle, driven by run_host():
//   init(module, ctx)            once; ctx.input already holds ADCVSET values
//   loop:
//     on_command(c)              each SET/KEY/CAPS/NUMLOCK/MOUSE, in arrival order,
//                                after ctx.input has been updated with it
//     ctx.clock.begin_frame()    (host)
//     step()                     produce the next frame into ctx.screen
//     ctx.audio->advance(now)    (host: the audio engine renders up to the step's time,
//                                unless the lane advanced it during the step: audio.h)
//     (host presents ctx.screen)
//   ctx.audio->shutdown(now)     (host: live sound stops, captures are finalized)
//   shutdown()                   once, on every exit path after a successful init
//
// A lane should turn its emulator's exceptions into false / failed itself; one
// that escapes init/on_command/step is contained by run_host (logged, exit 1,
// shutdown() still run after a successful init).
//
// Interaction (INTERACTION.md §3, §5, §10): after init and after every step
// run_host reads status() and publishes it (ADSTATUSHANDLE / ADSTATUSLOG).
// Input commands carry Command::seq; a lane that consumes an input line as the
// module's own reports the highest such seq in LaneStatus::eaten.
//
// Configure mode (adhostwin --configure, §6.1) is a separate entry point:
// configure() is called on a fresh lane object instead of init()/step(); it
// loads what it needs, runs the module's button handler, unloads, and
// returns. shutdown() is not called afterwards (init never ran).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "adw/core/clock.h"
#include "adw/core/env.h"
#include "adw/core/protocol.h"
#include "adw/core/screen.h"
#include "adw/core/status.h"

namespace adw {
namespace audio {
class Engine;  // adw/core/audio.h
}

// What a lane reports after each step (published as AdwHostStatusV1, §3.4).
struct LaneStatus {
  bool interactive = false;  // the module takes keys, clicks and moves as its own (ADWS_INTERACTIVE)
  bool cursor = false;       // it wants a visible cursor (ADWS_CURSOR)
  bool rotate_ok = false;    // it may be rotated away while interactive (ADWS_ROTATE_OK)
  bool key_filter = false;   // it may consume input without being interactive (ADWS_KEY_FILTER)
  bool wake = false;         // it asked the saver to end, as the user's input would (ADWS_WAKE): it posted
                             // WM_CLOSE/SC_CLOSE to the saver window, or (ne16) an After Dark 2.0 module
                             // returned its wake result, 5
  uint32_t source = 0;       // 1 AD4 WantEvents, 2 AD3 0x0E (kStatusSource*)
  uint64_t eaten = 0;        // highest input seq consumed
  // The lowest input seq the guest may still take: posted to a queue it reads
  // (or waiting to be posted) and neither taken nor dropped yet; 0 = none.
  // The published input_applied stays below it, so the front-end waits for
  // the guest's verdict on that input rather than reading "applied, not eaten".
  uint64_t unsettled = 0;
};

// adhostwin --configure <module> --button <slot> [--owner <hwnd>] (§6.1).
struct ConfigureRequest {
  int slot = -1;             // the catalog control index of the button
  uint64_t owner = 0;        // real HWND value of the owner, 0 = none
};
// Exit codes 0, 4, 5, 1.
enum class ConfigureResult { shown, nothing, unsupported, failed };

// The one JSON line --configure prints (§6.1):
//   {"result":"ok"|"nothing"|"error","dialogs":<n>,"message":"…","written":["<guest path>",…]}
// Lanes build theirs with this so the escaping is uniform. `result` follows r
// (shown = ok, nothing = nothing, unsupported/failed = error).
std::string configure_json(ConfigureResult r, int dialogs, const std::string& message,
                           const std::vector<std::string>& written);
// Process exit code for a configure result (0, 4, 5, 1).
int configure_exit_code(ConfigureResult r);

struct LaneContext {
  const Env& env;
  Screen& screen;
  VirtualClock& clock;
  const InputState& input;
  // The host audio engine (audio.h, AUDIO.md §3, §5). run_host sets it for
  // every module and test-pattern run (a disabled engine while sound is off),
  // configure_module to audio::null_engine(); a context built elsewhere (unit
  // tests) may leave it null, and a lane then uses audio::null_engine(). The
  // engine outlives the lane: after run_host's shutdown() of it, calls are
  // still accepted (and silent), so a lane may release its voices in its
  // destructor.
  audio::Engine* audio = nullptr;
};

enum class StepResult {
  ok,        // a frame is ready (mark the screen dirty if anything changed)
  finished,  // the module ended on its own; the host exits 0
  failed,    // unrecoverable; the host exits 1 (the lane has logged why)
};

class Lane {
 public:
  virtual ~Lane() = default;
  virtual const char* name() const = 0;
  // False = could not start (the lane has logged why); the host exits 1.
  // A failed init must release everything that refers to ctx (its screen,
  // clock and input live in run_host's frame and are gone before the lane is
  // destroyed; shutdown() is not called after a failed init).
  virtual bool init(const std::string& module_path, LaneContext& ctx) = 0;
  // The lane's natural frame period: free-running pacing and the headless
  // fixed clock step both use it (ADPACEMS overrides). Read after init().
  virtual uint32_t frame_interval_us() const { return 33333; }
  virtual void on_command(const Command&) {}
  virtual StepResult step() = 0;
  virtual void shutdown() {}

  // Interaction status after init / the last step (defaults: nothing to say).
  virtual LaneStatus status() const { return {}; }
  // Whether this lane implements configure() (for --capabilities; no module
  // is loaded to answer it).
  virtual bool can_configure() const { return false; }
  // The module ABIs this lane runs, for --capabilities (abis=): "afterdark"
  // (After Dark's module protocols), "intermission" (Delrina Intermission's
  // modules: .IMX modules, .ASA animations, .IMQ modules that are their own
  // readers, and the .FLI, .FLC, .MRF and .MSV files Intermission's other
  // readers play; a catalog entry says "abi":"intermission", and no "abi"
  // means "afterdark") and "scrnsave" (a Windows 3.1 screen saver: a .SCR
  // program built on SCRNSAVE.LIB, run as Windows 3.1 ran it;
  // "abi":"scrnsave"). The pe32 lane runs {afterdark}, the ne16 lane
  // {afterdark, intermission, scrnsave}.
  virtual std::vector<std::string> abis() const { return {}; }
  // Run a module's button handler (§6.1). ctx.input holds ADCVSET, ADCAPS and ADNUMLOCK;
  // ctx.env.state_root is persistent in this mode. The lane may fill
  // *json_out with configure_json(...); when it leaves it empty, adhostwin
  // prints one built from the result alone.
  virtual ConfigureResult configure(const std::string& module_path, LaneContext& ctx,
                                    const ConfigureRequest& req, std::string* json_out) {
    (void)module_path;
    (void)ctx;
    (void)req;
    (void)json_out;
    return ConfigureResult::unsupported;
  }
};

// Module image kind, from the header alone. A Delrina Intermission ASA
// animation (a data file that starts "AniN" or "AniM", played by
// Intermission's ASA reader) is an ne16 module. So is a file of one of
// Intermission's other data types — an .FLI or .FLC animation, an .MRF
// morph, an .MSV MultiSaver group —, which is no executable and which
// Intermission gave the reader whose type is the file's extension (INTRMLIB's
// FINDALLMODULES matched every file's extension against its readers' types):
// here it is the extension too.
enum class LaneKind { pe32, ne16, unsupported, unreadable };
struct ModuleProbe {
  LaneKind kind = LaneKind::unreadable;
  std::string detail;  // e.g. "PE32 i386 DLL", "NE", "not an MZ executable", "Intermission ASA animation"
};
ModuleProbe probe_module(const std::string& path_utf8);
// Whether a file's first four bytes are an Intermission ASA animation's header ("AniN" or "AniM").
bool asa_header(const void* first4);
// The Intermission data type a file name's extension names, upper case
// ("FLI", "FLC", "MRF" or "MSV"; any case in the name), else nullptr. ASA is
// not among them: an ASA animation goes by its header.
const char* intermission_data_type(const std::string& path_utf8);
const char* lane_kind_name(LaneKind k);

// Built-in synthetic lane (adhostwin --test-pattern).
std::unique_ptr<Lane> make_test_pattern_lane();

// Implemented by the lane components; adhostwin links them when they register
// (see host/core/CMakeLists.txt: global properties ADW_LANE_PE32_TARGET
// / ADW_LANE_NE16_TARGET name the library that defines these).
std::unique_ptr<Lane> make_pe32_lane();
std::unique_ptr<Lane> make_ne16_lane();

}  // namespace adw
