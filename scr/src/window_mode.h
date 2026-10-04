// Window mode (args.h /window; scr/README.md "Window mode"): the saver in an
// ordinary window of its own, for a streamer's "be right back" screen that
// OBS captures by its title. The parts that need no window, so the tests
// can pin them: its class, title and styles, its outer size for the client
// area /size asks for, which module /module names, and what it plays.
#pragma once

#include <windows.h>

#include <functional>
#include <string>

#include "catalog.h"
#include "geometry.h"
#include "settings.h"

namespace adw::scr {

// What OBS's Window Capture matches it by: "[LongAfterDark.exe]: Long After
// Dark". The title never changes (not when Random moves on); the class is
// the window mode's own, not the full-screen saver's (LongAfterDarkSaver) or
// the settings window's.
inline constexpr wchar_t kWindowModeClass[] = L"LongAfterDarkWindow";
inline constexpr wchar_t kWindowModeTitle[] = L"Long After Dark";
// An ordinary top-level window: a sizing frame, a caption with the system
// menu and the minimize and maximize boxes, a taskbar button; never topmost.
inline constexpr DWORD kWindowModeStyle = WS_OVERLAPPEDWINDOW;
inline constexpr DWORD kWindowModeExStyle = 0;

// The window's outer size for a client area of `client` physical pixels at
// `dpi` (96 = 100%): the frame and caption scale with the DPI, the client
// area never does, so OBS captures exactly the pixels /size asked for.
SizeI window_size_for_client(SizeI client, UINT dpi);

// What a /window run plays, from the settings it read (`s`):
//  * neither /module nor /random: what /s would (`s` as it is);
//  * /module alone: that module (`module`, a catalog id), and nothing else;
//  * /random: the settings window's Random, whatever it shows now: the
//    modules checked in its list (dialog_checklist: the rotation's list, or
//    the one kept while a single module is chosen; nothing checked: every
//    module), under its Collections, changing as often as it says
//    (DurationMin); with /module, that module first.
Settings window_settings(const Settings& s, const Catalog& c, const std::string& module, bool random);

// /module's argument as a module (resolve_module), or what is wrong with it,
// for the user: no catalog (nothing imported), no module of that name, a
// name several modules share (their ids listed), or a module whose file is
// missing (`available` says whether it is there).
struct WindowModule {
  const Module* module = nullptr;
  std::wstring error;   // "" when `module` is set
};
WindowModule window_module(const Catalog& c, const std::wstring& name,
                           const std::function<bool(const std::string&)>& available);

} // namespace adw::scr
