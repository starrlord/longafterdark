// Wine and the file system: which Wine runs the hosts, where its prefix is,
// how a Unix path is handed to a Windows program, and where the imported
// releases, the modules' saved state and the Windows programs are.
#pragma once

#include <string>
#include <vector>

namespace lad {

std::string home_dir();

// The directory of the player's own executable, symlinks resolved, so a
// link in /usr/local/bin to an unpacked release still finds adhostwin.exe
// beside the real file.
std::string self_dir();

// The Wine loader: $AD_WINE_BIN (a path, or a name looked up on PATH), else
// "wine" or "wine64" on PATH, else Wine's usual install places. An absolute
// path, or "" when there is none.
std::string find_wine();

// The Wine prefix the hosts run in: $WINEPREFIX, else ~/.wine.
std::string wine_prefix();

// True when the prefix exists and is a 32-bit one (system.reg says
// "#arch=win32"): adhostwin.exe is a 64-bit program and can't run there.
bool prefix_is_32bit(const std::string& prefix);

// A Unix path as a Windows program under Wine must be given it: the drive
// whose link in <prefix>/dosdevices is the longest prefix of the path's
// real location (Wine's own rule; Z:\ for / in a standard prefix), with
// backslashes; \\?\unix\... when no drive maps it. `path` may name a file
// that doesn't exist yet (its existing parent is resolved).
std::string to_windows_path(const std::string& path, const std::string& prefix);

// The win dir of an assets root (host/core/README.md, AD_ASSETS_DIR):
// <root>/win when that holds FILES, packages or catalog-win.json; else
// <root> itself when it holds one of them; else <root>/win.
std::string win_assets_dir(const std::string& root);

struct AssetsSearch {
  std::string root;                 // the assets root found ("" when none)
  bool has_catalog = false;         // <win dir>/catalog-win.json exists
  bool explicit_root = false;       // named by --assets-dir or AD_ASSETS_DIR
  std::vector<std::string> tried;   // the places looked at, for the message
};
// Where the imported releases are: `override` (--assets-dir), else
// $AD_ASSETS_DIR, else where adimport puts them when it runs under Wine: the
// prefix's %LOCALAPPDATA%\LongAfterDark\assets
// (<prefix>/drive_c/users/<user>/AppData/Local/LongAfterDark/assets, the
// user's own folder first).
AssetsSearch find_assets(const std::string& override, const std::string& prefix);

// ADSTATE's folder (INTERACTION.md §7): $AD_SCR_STATE, else
// $XDG_DATA_HOME/longafterdark/state ($XDG_DATA_HOME defaults to
// ~/.local/share). Created (0700) when missing; "" with *error when it
// can't be.
std::string state_dir(std::string* error);

// A Windows program of the release: $<env_var> when it names a file, else
// <self_dir>/<name>, else the build trees' copies (relative to the current
// directory, for a developer's checkout). An absolute path, or "".
std::string find_program(const char* env_var, const char* name, const std::vector<std::string>& dev_paths);

// PATH lookup of an executable; "" when not found.
std::string which(const std::string& name);

bool is_dir(const std::string& p);
bool is_file(const std::string& p);
// mkdir -p with `mode` for the directories it creates.
bool make_dirs(const std::string& p, unsigned mode);

}  // namespace lad
