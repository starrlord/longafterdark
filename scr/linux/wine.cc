#include "wine.h"

#include <dirent.h>
#include <limits.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace lad {

namespace {

std::string env(const char* name) {
  const char* v = getenv(name);
  return v ? std::string(v) : std::string();
}

std::string join(const std::string& a, const std::string& b) {
  if (a.empty()) return b;
  if (a.back() == '/') return a + b;
  return a + "/" + b;
}

std::string parent_of(const std::string& p) {
  size_t s = p.find_last_of('/');
  if (s == std::string::npos) return ".";
  if (s == 0) return "/";
  return p.substr(0, s);
}

// "/a/./b/../c/" -> "/a/c": lexical, for the part of a path that doesn't
// exist (realpath handles the rest).
std::string normalize(const std::string& p) {
  std::vector<std::string> parts;
  size_t i = 0;
  while (i <= p.size()) {
    size_t j = p.find('/', i);
    if (j == std::string::npos) j = p.size();
    std::string c = p.substr(i, j - i);
    if (c == "..") {
      if (!parts.empty()) parts.pop_back();
    } else if (!c.empty() && c != ".") {
      parts.push_back(c);
    }
    i = j + 1;
  }
  std::string out;
  for (const auto& c : parts) out += "/" + c;
  return out.empty() ? "/" : out;
}

std::string real(const std::string& p) {
  char buf[PATH_MAX];
  if (realpath(p.c_str(), buf)) return buf;
  return {};
}

// The real location of `p`, which may not exist: its deepest existing
// ancestor resolved, the rest appended.
std::string resolve(const std::string& p) {
  std::string abs = p;
  if (abs.empty() || abs[0] != '/') {
    char cwd[PATH_MAX];
    abs = join(getcwd(cwd, sizeof(cwd)) ? cwd : "/", abs);
  }
  abs = normalize(abs);
  std::string head = abs, tail;
  for (;;) {
    std::string r = real(head);
    if (!r.empty()) return tail.empty() ? r : normalize(join(r, tail));
    if (head == "/") return abs;
    size_t s = head.find_last_of('/');
    std::string last = head.substr(s + 1);
    tail = tail.empty() ? last : last + "/" + tail;
    head = s == 0 ? "/" : head.substr(0, s);
  }
}

bool under(const std::string& path, const std::string& root) {
  if (root == "/") return true;
  return path == root || (path.size() > root.size() && path.compare(0, root.size(), root) == 0 && path[root.size()] == '/');
}

std::string backslashes(std::string s) {
  for (char& c : s) {
    if (c == '/') c = '\\';
  }
  return s;
}

}  // namespace

bool is_dir(const std::string& p) {
  struct stat st;
  return !p.empty() && stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool is_file(const std::string& p) {
  struct stat st;
  return !p.empty() && stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool make_dirs(const std::string& p, unsigned mode) {
  if (p.empty()) return false;
  if (is_dir(p)) return true;
  std::string parent = parent_of(p);
  if (parent != p && !is_dir(parent) && !make_dirs(parent, mode)) return false;
  return mkdir(p.c_str(), mode) == 0 || errno == EEXIST;
}

std::string home_dir() {
  std::string h = env("HOME");
  if (!h.empty()) return h;
  if (struct passwd* pw = getpwuid(getuid()); pw && pw->pw_dir) return pw->pw_dir;
  return "/";
}

std::string self_dir() {
  char buf[PATH_MAX];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) return {};
  buf[n] = '\0';
  std::string r = real(buf);
  return parent_of(r.empty() ? std::string(buf) : r);
}

std::string which(const std::string& name) {
  if (name.find('/') != std::string::npos) return access(name.c_str(), X_OK) == 0 ? resolve(name) : std::string();
  std::string path = env("PATH");
  if (path.empty()) path = "/usr/local/bin:/usr/bin:/bin";
  size_t i = 0;
  while (i <= path.size()) {
    size_t j = path.find(':', i);
    if (j == std::string::npos) j = path.size();
    std::string dir = path.substr(i, j - i);
    if (dir.empty()) dir = ".";
    std::string cand = join(dir, name);
    if (is_file(cand) && access(cand.c_str(), X_OK) == 0) return cand[0] == '/' ? cand : resolve(cand);
    i = j + 1;
  }
  return {};
}

std::string find_wine() {
  if (std::string e = env("AD_WINE_BIN"); !e.empty()) return which(e);
  for (const char* n : {"wine", "wine64"}) {
    if (std::string w = which(n); !w.empty()) return w;
  }
  for (const char* p : {"/usr/lib/wine/wine64", "/usr/lib/wine/wine", "/opt/wine-stable/bin/wine",
                        "/opt/wine-staging/bin/wine", "/usr/lib64/wine/wine64"}) {
    if (access(p, X_OK) == 0) return p;
  }
  return {};
}

std::string wine_prefix() {
  std::string p = env("WINEPREFIX");
  if (!p.empty()) return p;
  return join(home_dir(), ".wine");
}

bool prefix_is_32bit(const std::string& prefix) {
  std::ifstream f(join(prefix, "system.reg"));
  if (!f) return false;
  std::string line;
  for (int i = 0; i < 40 && std::getline(f, line); ++i) {
    if (line.rfind("#arch=", 0) == 0) return line.compare(6, 5, "win32") == 0;
  }
  return false;
}

std::string to_windows_path(const std::string& path, const std::string& prefix) {
  const std::string p = resolve(path);
  const std::string devices = join(prefix, "dosdevices");
  std::string best_root;
  char best_letter = 0;
  if (DIR* d = opendir(devices.c_str())) {
    while (struct dirent* e = readdir(d)) {
      const char* n = e->d_name;
      // Drive links are "c:" .. "z:"; "c::" and the like are raw devices.
      if (strlen(n) != 2 || n[1] != ':') continue;
      char letter = n[0];
      if (letter >= 'A' && letter <= 'Z') letter = char(letter - 'A' + 'a');
      if (letter < 'a' || letter > 'z') continue;
      std::string root = real(join(devices, n));
      if (root.empty() || !under(p, root)) continue;
      if (root.size() > best_root.size() || (root.size() == best_root.size() && letter < best_letter)) {
        best_root = root;
        best_letter = letter;
      }
    }
    closedir(d);
  }
  if (!best_letter) return "\\\\?\\unix" + backslashes(p);
  std::string rest = best_root == "/" ? p.substr(1) : (p.size() > best_root.size() ? p.substr(best_root.size() + 1) : "");
  std::string out;
  out += char(best_letter - 'a' + 'A');
  out += ":\\";
  out += backslashes(rest);
  return out;
}

std::string win_assets_dir(const std::string& root) {
  auto holds = [](const std::string& dir) {
    return is_dir(join(dir, "FILES")) || is_dir(join(dir, "packages")) || is_file(join(dir, "catalog-win.json"));
  };
  const std::string win = join(root, "win");
  if (holds(win)) return win;
  if (holds(root)) return root;
  return win;
}

AssetsSearch find_assets(const std::string& override, const std::string& prefix) {
  AssetsSearch s;
  auto check = [&](const std::string& root) {
    s.tried.push_back(root);
    return is_file(join(win_assets_dir(root), "catalog-win.json"));
  };
  std::string given = !override.empty() ? override : env("AD_ASSETS_DIR");
  if (!given.empty()) {
    s.explicit_root = true;
    s.root = resolve(given);
    s.has_catalog = check(s.root);
    return s;
  }
  // %LOCALAPPDATA% under Wine: the profile folder is the user's name.
  const std::string users = join(prefix, "drive_c/users");
  std::vector<std::string> names;
  for (const char* v : {"USER", "LOGNAME"}) {
    if (std::string n = env(v); !n.empty()) names.push_back(n);
  }
  if (struct passwd* pw = getpwuid(getuid()); pw && pw->pw_name) names.push_back(pw->pw_name);
  if (DIR* d = opendir(users.c_str())) {
    while (struct dirent* e = readdir(d)) {
      std::string n = e->d_name;
      if (n != "." && n != ".." && n != "Public") names.push_back(n);
    }
    closedir(d);
  }
  std::vector<std::string> seen;
  for (const std::string& n : names) {
    bool dup = false;
    for (const auto& x : seen) dup = dup || x == n;
    if (dup) continue;
    seen.push_back(n);
    for (const char* local : {"AppData/Local", "Local Settings/Application Data"}) {
      std::string root = join(join(join(users, n), local), "LongAfterDark/assets");
      if (check(root)) {
        s.root = root;
        s.has_catalog = true;
        return s;
      }
    }
  }
  return s;
}

std::string state_dir(std::string* error) {
  std::string dir = env("AD_SCR_STATE");
  if (dir.empty()) {
    std::string data = env("XDG_DATA_HOME");
    if (data.empty() || data[0] != '/') data = join(home_dir(), ".local/share");
    dir = join(data, "longafterdark/state");
  }
  dir = resolve(dir);
  if (!make_dirs(dir, 0700)) {
    if (error) *error = "cannot create " + dir + ": " + strerror(errno);
    return {};
  }
  return dir;
}

std::string find_program(const char* env_var, const char* name, const std::vector<std::string>& dev_paths) {
  if (std::string e = env(env_var); !e.empty()) return is_file(e) ? resolve(e) : std::string();
  if (std::string d = self_dir(); !d.empty() && is_file(join(d, name))) return join(d, name);
  for (const auto& p : dev_paths) {
    if (is_file(p)) return resolve(p);
  }
  return {};
}

}  // namespace lad
