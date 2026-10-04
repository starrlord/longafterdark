// Screen saver command line, per the Windows convention:
//   /s          run full screen            /p <HWND>  live preview in that window
//   /c[:HWND]   settings (owned by HWND)   (none)     settings
//   /a <HWND>   change password (Win9x; ignored on NT)
// Case-insensitive; '-' works like '/'; the HWND may follow a ':' / '=' or be
// the next argument ("/c:1234", "/p 1234", "/P1234"). The first of these
// decides; anything else on such a command line is skipped.
//
// And Long After Dark's own (scr/README.md, "Window mode"), which a user
// types (Windows gives a .scr nothing but /S, so they are for the same
// program named LongAfterDark.exe), in any order:
//   /window          an ordinary window of its own instead of the full screen
//   /size WxH        its client area in physical pixels (default 1280x720)
//   /module <m>      that module: its id or its name (catalog.h resolve_module)
//   /random          the modules in turn, as Random in the settings (/module first)
//   /help, /?        what every switch does
// '-' and "--" work like '/' for these; a value may follow ':' / '=' or be the
// next argument ("/size:1920x1080", "/module ad40.toasters", "/module
// \"Flying Toasters!\""). /size, /module and /random go with /window; with
// /window, an unknown switch or a stray word is an error, as is a size
// outside kWindowMinSize..kWindowMaxSize.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace adw::scr {

enum class Mode { settings, run, preview, password, window, help };

// /size's default and its limits, in physical pixels.
inline constexpr int kWindowDefaultW = 1280, kWindowDefaultH = 720;
inline constexpr int kWindowMinW = 160, kWindowMinH = 120;
inline constexpr int kWindowMaxW = 7680, kWindowMaxH = 4320;

struct Args {
  Mode mode = Mode::settings;
  uintptr_t hwnd = 0;         // parent (/p) or owner (/c); 0 when absent
  bool has_hwnd = false;
  bool valid = true;          // false: /p without a window to draw into
  // /window's: the client area, the module named ("" for none: the settings
  // choose) and /random.
  int width = kWindowDefaultW, height = kWindowDefaultH;
  std::wstring module;
  bool random = false;
  // What is wrong with the command line, for the user ("" when nothing is):
  // the program shows it with the usage (usage_message) and exits.
  std::wstring error;
};

// `argv` excludes the program name.
Args parse_args(const std::vector<std::wstring>& argv);

// What every switch does: /help's message box.
std::wstring usage_text();
// `problem` (Args::error, or a module /module can't name), then how the
// switches go in short and where /help says more.
std::wstring usage_message(const std::wstring& problem);

} // namespace adw::scr
