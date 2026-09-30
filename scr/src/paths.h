// Paths and small file helpers shared by the saver, the settings dialog and
// the tests. Every location has an environment override so the smoke tests
// (and anyone debugging a real install) can point the saver at fixtures
// without touching %LOCALAPPDATA%.
#pragma once

#include <string>
#include <string_view>

namespace adw::scr {

std::wstring widen(std::string_view utf8);
std::string narrow(std::wstring_view wide);

// Value of an environment variable, or "" when unset.
std::wstring env_w(const wchar_t* name);
bool env_set(const wchar_t* name);
// Integer environment variable, or `fallback` when unset/unparseable.
long long env_int(const wchar_t* name, long long fallback);

std::wstring exe_path();                  // this process's image
std::wstring dir_of(const std::wstring& path);
std::wstring join_path(const std::wstring& a, const std::wstring& b);

// The data folder, %LOCALAPPDATA%\LongAfterDark (host/core's data_root.h,
// shared with adhostwin and adimport, so all three agree on it).
// AD_LOCALAPPDATA stands in for %LOCALAPPDATA%; SHGetKnownFolderPath answers
// when neither is set. Only a path: nothing is looked at or created. It is
// reached only through the defaults below, so a process with AD_SETTINGS and
// AD_ASSETS_DIR set keeps nothing in the real folder.
std::wstring app_data_root();
// AD_ASSETS_DIR, else <app_data_root>\assets (the same root the importer
// uses; the modules live under its "win" subdirectory).
std::wstring assets_root();
// <assets_root>\win. Tolerates AD_ASSETS_DIR pointing straight at the win
// directory (i.e. one that already holds catalog-win.json).
std::wstring win_assets_dir();
std::wstring catalog_path();              // <win_assets_dir>\catalog-win.json
// AD_SETTINGS, else <app_data_root>\settings.ini
std::wstring settings_path();
// AD_SCR_THUMBS, else "thumbs" next to settings_path(): the module
// thumbnails the settings dialog's live preview takes (module_icons.h).
std::wstring thumbs_dir();
// AD_SCR_STATE, else "state" next to settings_path(): the modules' own
// settings and data files (INTERACTION.md §7.1), passed to every host as
// ADSTATE.
std::wstring state_dir();
// AD_SCR_LASTLOG, else logs\saver-last.log next to settings_path(): the
// always-on log of the last /s run (INTERACTION.md §9.1).
std::wstring last_exit_log_path();
// %TEMP%\LongAfterDark-seed-<pid>-<window index>.ppm (INTERACTION.md §8), or
// with `size` ("640x480": the capture for a module's own screen, an
// Intermission, Star Trek, ScreamSavers or Marvel module's, saver.cc)
// ...-<window index>-<size>.ppm.
std::wstring seed_file_path(unsigned long pid, int window_index, const std::wstring& size = L"");
inline constexpr wchar_t kSeedFilePrefix[] = L"LongAfterDark-seed-";
// AD_HOST_EXE, else adhostwin.exe next to LongAfterDark.scr
std::wstring host_exe_path();
// AD_IMPORT_EXE, else adimport.exe next to LongAfterDark.scr
std::wstring import_exe_path();

// A catalog "path" (relative to <assets>\win, forward slashes) as an absolute
// Windows path.
std::wstring resolve_module_path(const std::wstring& win_dir, const std::string& rel);

bool file_exists(const std::wstring& path);
bool dir_exists(const std::wstring& path);
bool read_file(const std::wstring& path, std::string& out);
// Writes via a temp file + MoveFileEx so a crash mid-save never leaves a
// truncated settings.ini behind. Creates the parent directory if needed.
bool write_file_atomic(const std::wstring& path, const std::string& data);
// Creates `path` and its missing parents.
bool ensure_dir(const std::wstring& path);

// Quote one argument for CreateProcess per the MSVCRT argv rules.
std::wstring quote_arg(const std::wstring& arg);

} // namespace adw::scr
