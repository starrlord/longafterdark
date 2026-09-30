// What the player knows about a host: its status record and its
// --capabilities answer.
//
// The Windows saver reads the status record (INTERACTION.md §3.4) from a
// shared section named in ADSTATUSHANDLE. A native Linux process can't give
// a Windows program such a handle, so the player starts every host with
// ADSTATUSLOG=1 and reads the same record from the lines the host prints on
// stderr (host/core/README.md, "Environment"):
//   STATUS <frame> flags=0x<hex> applied=<n> eaten=<n> src=<s>
// printed at frame 0 and whenever flags, applied or eaten change, and always
// before the frame of that step is written to stdout, so a status read when
// a frame arrives is that step's verdict.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lad {

// The record's flags (host/core/include/adw/core/status.h).
inline constexpr uint32_t ADWS_INTERACTIVE = 0x01;  // the module takes keys, clicks and moves as its own
inline constexpr uint32_t ADWS_CURSOR = 0x02;       // show a cursor
inline constexpr uint32_t ADWS_ROTATE_OK = 0x04;    // may be rotated away while interactive
inline constexpr uint32_t ADWS_KEY_FILTER = 0x08;   // the guest may consume input without being interactive
inline constexpr uint32_t ADWS_WAKE = 0x10;         // the module asked the saver to end
inline constexpr uint32_t ADWS_READY = 0x20;        // lane init done

struct HostStatus {
  uint64_t frames = 0;          // steps completed
  uint32_t flags = 0;           // ADWS_*
  uint64_t input_applied = 0;   // number of the last input line applied before the last completed step
  uint64_t input_eaten = 0;     // highest input line the module consumed
  uint32_t source = 0;          // 0 none, 1 AD4 WantEvents, 2 AD3 0x0E
};

// Parses one stderr line (without its newline; a trailing '\r' is allowed).
// False, with `out` untouched, for anything that is not exactly a STATUS
// line: a module's own log line that happens to start with "STATUS" must
// never be taken for the host's verdict.
bool parse_status_line(std::string_view line, HostStatus& out);

// The host's --capabilities line (host/core/README.md; the Windows saver's
// dialog_support.h HostCapabilities).
struct HostCapabilities {
  bool known = false;                 // the host answered
  std::vector<std::string> lanes;
  // The module ABIs it runs (catalog "abi"). A host that prints no "abis="
  // runs After Dark's alone.
  std::vector<std::string> abis{"afterdark"};
  bool status = false;
  // numlock=1 (exactly): the host takes NUMLOCK lines and ADNUMLOCK.
  bool numlock = false;
  std::string line;                   // as printed, for the log

  bool has_lane(const std::string& lane) const;
  bool has_abi(const std::string& abi) const;
  // Whether it runs a module of `lane` and `abi` ("" = "afterdark"). A host
  // that didn't answer is taken to run everything.
  bool runs(const std::string& lane, const std::string& abi) const;
  // NUMLOCK lines only once it has said numlock=1: a host without the line
  // ignores it without numbering it, and every later input line would carry
  // one number more in the player's count than in the host's.
  bool takes_numlock_lines() const { return known && numlock; }
  // ADNUMLOCK unless it has answered without numlock=1: before the answer it
  // goes too (a host that doesn't know it ignores it; one that does must
  // start with the real toggle, since Final Exam latches it at start).
  bool takes_numlock_env() const { return !known || numlock; }
};

HostCapabilities parse_capabilities(const std::string& text);

}  // namespace lad
