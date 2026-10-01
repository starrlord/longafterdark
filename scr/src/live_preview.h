// The settings dialog's live preview: a child window that runs the selected
// module in its own adhostwin.exe (as the saver does, DESIGN.md §1) at the
// size of a real screen and shows it scaled down, so the dialog previews
// exactly what the saver will show, unsaved settings included.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "catalog.h"

namespace adw::ui {
struct Palette;
}

namespace adw::scr {

using adw::ui::Palette;

// Posted to the parent: wParam = kLiveLaneMissing when the host said the
// module's lane is not built into this adhostwin (exit 3 before a frame,
// host.h: kExitLaneMissing; live_preview_take_cant_run() names the module);
// kLiveThumbSaved when a thumbnail was written to the target's thumb_path.
inline constexpr UINT WM_APP_LIVE_STATUS = WM_APP + 30;
inline constexpr WPARAM kLiveLaneMissing = 1, kLiveThumbSaved = 2;

struct LiveTarget {
  std::string id;                // the module's catalog id (what an exit 3 is reported for)
  // Its catalog ABI and "screen" (catalog.h Module::screen), which size its
  // screen (geometry.h: own_screen, module_screen): an After Dark module's is
  // 480 lines at the preview's aspect, an Intermission, Star Trek,
  // ScreamSavers or Marvel module's its own 640x480.
  std::string abi = kAfterDarkAbi;
  SizeI screen;
  std::wstring host_exe;         // adhostwin.exe
  std::wstring module_path;      // absolute
  std::wstring win_dir;          // <assets>\win (working directory)
  std::string cvset;             // ADCVSET, "0=50,1=1"
  std::wstring name;             // the module's display name (hover caption, messages)
  // Where to keep a thumbnail of the module ("" = don't take one): the most
  // detailed of a few of its frames (thumbnails.h: ThumbTaker).
  std::wstring thumb_path;
};

void register_live_preview_class(HINSTANCE hinst);
HWND create_live_preview(HWND parent, int id, HINSTANCE hinst);

// Colours of the frame around the picture and of the messages.
void live_preview_set_palette(HWND preview, const Palette* pal, int dpi);
// (Re)starts the module after a short pause (so arrowing through the list
// doesn't start a host per row). The same target again is a no-op.
void live_preview_run(HWND preview, const LiveTarget& target);
// Stops any module and shows a message instead ("Coming soon", …).
void live_preview_message(HWND preview, const std::wstring& title, const std::wstring& detail);
// Until anything is imported: no module, but the welcome's picture (the
// night sky, the moon and a couple of flying toasters).
void live_preview_hero(HWND preview, bool on);
// The pointer is over the preview: show the module's name along its foot.
void live_preview_set_hover(HWND preview, bool hover);
// "Stretch to fit" (Settings::stretch_to_fit): a module with a screen of its
// own fills the preview instead of keeping its shape; the host carries on.
void live_preview_set_stretch(HWND preview, bool on);
// While the full-screen Preview runs, the live one stops asking for frames.
void live_preview_pause(HWND preview, bool paused);
// Starts the current module again in a new host (after a module button's
// run: the module reads what it saved at its next start).
void live_preview_restart(HWND preview);
// Frames shown so far by the current module (the screenshot hook waits on it).
unsigned long long live_preview_frames(HWND preview);
// The ids of the modules whose host exited 3 before a frame since the last
// call (each kLiveLaneMissing names one), oldest first; the list is emptied.
std::vector<std::string> live_preview_take_cant_run(HWND preview);

} // namespace adw::scr
