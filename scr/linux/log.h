// Logging for the Linux player.
//
// Three streams, all line-based:
//  * errors: always on stderr ("longafterdark: ..."), and in the log below;
//  * the diagnostic log (spawns, host exits, respawns, rotations, input
//    decisions, frame rates): appended to $AD_SCR_LOG when set, and printed
//    on stderr with --verbose; like the Windows saver's AD_SCR_LOG;
//  * the hosts' own stderr (adhostwin's log, Wine's messages): appended to
//    $AD_SCR_HOSTLOG when set, and printed on stderr with --verbose;
//    otherwise dropped, as the Windows saver drops it. XScreenSaver shows a
//    hack's stderr over the screen, so nothing goes there by default.
#pragma once

#include <string>
#include <string_view>

namespace lad {

// Seconds since the program started (monotonic), for log stamps.
double elapsed_s();

void log_open(const std::string& path, bool to_stderr);
void hostlog_open(const std::string& path, bool to_stderr);
bool log_enabled();

void log_line(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void err_line(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void hostlog_line(std::string_view line);

}  // namespace lad
