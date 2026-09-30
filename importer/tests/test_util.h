// Tiny test harness shared by the importer tests: CHECK macros that count
// failures instead of aborting (one run reports every broken expectation),
// deterministic data, and file helpers.
#pragma once

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <type_traits>
#include <vector>

#include "adw/core/data_root.h"
#include "md5.h"
#include "packages.h"
#include "source.h"
#include "winutil.h"

namespace test {

// ---- the data folder -------------------------------------------------------------
// adimport's defaults live in the data folder, %LOCALAPPDATA%\LongAfterDark
// (importer.h data_folder()). No test may write to the user's real folder:
// every suite calls sandbox_data_root() first thing, so the defaults of this
// process and of every adimport it starts resolve under a scratch base. A
// suite that reads the user's installed data finds it with
// installed_data_root() before that, read only.

// The data folder that holds the user's data: %LOCALAPPDATA%\LongAfterDark
// (AD_LOCALAPPDATA stands in for LOCALAPPDATA as usual); "" without either.
// Nothing is looked at or created.
inline std::filesystem::path installed_data_root() {
  return adw::data_root_path(adw::data_root_base());
}

// <installed data root>\assets, or AD_ASSETS_DIR when that is set (as adimport
// would take it); "" when neither is known.
inline std::filesystem::path installed_assets_root() {
  if (const wchar_t* e = _wgetenv(L"AD_ASSETS_DIR")) {
    std::wstring v = adw::data_root_detail::trim(e);
    if (!v.empty()) return v;
  }
  const std::filesystem::path d = installed_data_root();
  return d.empty() ? d : d / L"assets";
}

// An environment variable for this process (the CRT's copy, which _wgetenv
// reads, and the process's, which data_root.h and child processes read);
// "" removes it.
inline void set_env(const wchar_t* name, const std::wstring& value) {
  _wputenv_s(name, value.c_str());
  SetEnvironmentVariableW(name, value.empty() ? nullptr : value.c_str());
}

inline std::wstring get_env(const wchar_t* name) {
  const wchar_t* v = _wgetenv(name);
  return v ? v : L"";
}

// From now on the data folder of this process and of the processes it starts
// is under `base` (AD_LOCALAPPDATA; `base` is not created). Returns `base`.
inline std::filesystem::path sandbox_data_root(const std::filesystem::path& base) {
  set_env(adw::kDataRootBaseVar, base.wstring());
  return base;
}

inline int g_failures = 0;

#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      ::test::g_failures++;                                                      \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
    }                                                                            \
  } while (0)

#define CHECK_EQ(a, b)                                                                     \
  do {                                                                                     \
    auto _va = (a);                                                                        \
    auto _vb = (b);                                                                        \
    if (!(_va == _vb)) {                                                                   \
      ::test::g_failures++;                                                                \
      fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %s\n  left:  %s\n  right: %s\n",      \
              __FILE__, __LINE__, #a, #b, ::test::show(_va).c_str(), ::test::show(_vb).c_str()); \
    }                                                                                      \
  } while (0)

inline std::string show(const std::string& s) { return "\"" + s + "\""; }
inline std::string show(const char* s) { return s ? show(std::string(s)) : "(null)"; }
inline std::string show(const std::filesystem::path& p) { return show(adw::import::to_utf8(p.wstring())); }
template <typename T>
std::string show(const T& v) {
  if constexpr (std::is_enum_v<T>) {
    return std::to_string(int(v));
  } else if constexpr (std::is_arithmetic_v<T>) {
    return std::to_string(v);
  } else {
    return "(value)";
  }
}

inline int finish(const char* name) {
  if (g_failures) {
    fprintf(stderr, "%s: %d failure(s)\n", name, g_failures);
    return 1;
  }
  fprintf(stderr, "%s: ok\n", name);
  return 0;
}

// xorshift-filled buffer: reproducible, incompressible-looking test content.
inline std::vector<uint8_t> pattern(size_t n, uint32_t seed) {
  std::vector<uint8_t> v(n);
  uint32_t x = seed * 2654435761u + 1;
  for (size_t i = 0; i < n; i++) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    v[i] = uint8_t(x >> 24);
  }
  return v;
}

inline void write_bytes(const std::filesystem::path& p, const std::vector<uint8_t>& data) {
  std::filesystem::create_directories(p.parent_path());
  FILE* f = _wfopen(p.c_str(), L"wb");
  if (!f) {
    fprintf(stderr, "cannot write %s\n", adw::import::to_utf8(p.wstring()).c_str());
    exit(2);
  }
  if (!data.empty()) fwrite(data.data(), 1, data.size(), f);
  fclose(f);
}

inline std::vector<uint8_t> read_bytes(const std::filesystem::path& p) {
  std::vector<uint8_t> out;
  FILE* f = _wfopen(p.c_str(), L"rb");
  if (!f) return out;
  uint8_t buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
  fclose(f);
  return out;
}

inline std::string read_text(const std::filesystem::path& p) {
  auto b = read_bytes(p);
  return std::string(b.begin(), b.end());
}

// Whether `s` is UTF-8 to Windows' own strict decoder (MB_ERR_INVALID_CHARS):
// the tests' check of what the importer writes, independent of the
// importer's own utf8.h utf8_sequence_length.
inline bool strict_utf8(const std::string& s) {
  return s.empty() || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()), nullptr, 0) > 0;
}

// A fresh, empty scratch directory (argv[1] of the test, or %TEMP%).
inline std::filesystem::path scratch(int argc, char** argv, const char* name) {
  std::filesystem::path p = argc > 1 ? std::filesystem::path(argv[1])
                                     : std::filesystem::temp_directory_path() / name;
  adw::import::remove_tree(p);  // not std::filesystem::remove_all: see winutil.h
  std::filesystem::create_directories(p);
  return p;
}

// Where the real-image tests look for images, which they identify by size
// and md5, never by name: every folder of AD_SOURCE_ISO_DIR (';'-separated),
// else `fallback` (<repo>\source_iso), and the folders directly inside each
// (so <repo>\source_iso\Implemented is searched too). Only read.
inline std::vector<std::filesystem::path> image_dirs(const std::filesystem::path& fallback) {
  std::vector<std::filesystem::path> roots, out;
  const std::wstring env = get_env(L"AD_SOURCE_ISO_DIR");
  if (!env.empty()) {
    size_t i = 0;
    while (i <= env.size()) {
      size_t j = env.find(L';', i);
      if (j == std::wstring::npos) j = env.size();
      if (j > i) roots.push_back(env.substr(i, j - i));
      i = j + 1;
    }
  } else if (!fallback.empty()) {
    roots.push_back(fallback);
  }
  std::error_code ec;
  for (const auto& r : roots) {
    out.push_back(r);
    std::vector<std::filesystem::path> subs;
    for (auto it = std::filesystem::directory_iterator(r, ec); !ec && it != std::filesystem::directory_iterator();
         it.increment(ec))
      if (it->is_directory(ec)) subs.push_back(it->path());
    ec.clear();
    std::sort(subs.begin(), subs.end());
    out.insert(out.end(), subs.begin(), subs.end());
  }
  return out;
}

// The first file directly in one of `dirs` with this size and md5 (only
// files of that size are hashed); "" when there is none.
inline std::filesystem::path find_image(const std::vector<std::filesystem::path>& dirs, uint64_t size,
                                        const std::string& md5) {
  std::error_code ec;
  for (const auto& d : dirs) {
    for (auto it = std::filesystem::directory_iterator(d, ec); !ec && it != std::filesystem::directory_iterator();
         it.increment(ec)) {
      if (!it->is_regular_file(ec) || it->file_size(ec) != size) continue;
      if (adw::import::md5_file_hex(it->path()) == md5) return it->path();
    }
    ec.clear();
  }
  return {};
}

// The images of every install disk of a release on several (packages.h
// KnownImage::disk), identified by size and md5, never by name: a loose
// image of each disk directly in one of `dirs` (from either known copy of
// it), in disk order; else one ZIP of 256 MB or less whose floppy images
// cover every disk (the Internet Archive's ZIP of the images, as
// source_iso/afterdark-20b_startrek.zip is). Empty when neither is there, or
// the package has no install disks. Only read.
inline std::vector<std::filesystem::path> find_disk_set(const std::vector<std::filesystem::path>& dirs,
                                                        const adw::import::Package& p) {
  int n = 0;
  for (const adw::import::KnownImage& k : p.images) n = std::max(n, k.disk);
  if (!n) return {};
  std::vector<std::filesystem::path> loose(static_cast<size_t>(n));
  for (const adw::import::KnownImage& k : p.images)
    if (k.disk && loose[size_t(k.disk - 1)].empty()) loose[size_t(k.disk - 1)] = find_image(dirs, k.size, k.md5);
  if (std::all_of(loose.begin(), loose.end(), [](const std::filesystem::path& x) { return !x.empty(); })) return loose;
  std::error_code ec;
  for (const auto& d : dirs) {
    for (auto it = std::filesystem::directory_iterator(d, ec); !ec && it != std::filesystem::directory_iterator();
         it.increment(ec)) {
      const std::string name = adw::import::to_utf8(it->path().filename().wstring());
      if (!it->is_regular_file(ec) || it->file_size(ec) > (256ull << 20) || name.size() < 4 ||
          _stricmp(name.c_str() + name.size() - 4, ".zip") != 0)
        continue;
      std::vector<bool> have(static_cast<size_t>(n), false);
      try {
        for (const adw::import::ZippedImage& z : adw::import::floppy_images_in_zip(it->path())) {
          const std::string md5 = adw::import::md5_hex(z.bytes->data(), z.bytes->size());
          for (const adw::import::KnownImage& k : p.images)
            if (k.disk && md5 == k.md5) have[size_t(k.disk - 1)] = true;
        }
      } catch (const std::exception&) {
        continue;
      }
      if (std::all_of(have.begin(), have.end(), [](bool b) { return b; })) return {it->path()};
    }
    ec.clear();
  }
  return {};
}

// Every regular file under `root`, as '/'-separated relative paths.
inline std::vector<std::string> list_tree(const std::filesystem::path& root) {
  std::vector<std::string> out;
  std::error_code ec;
  for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
       it != std::filesystem::recursive_directory_iterator(); ++it) {
    if (!it->is_regular_file()) continue;
    std::string rel = adw::import::to_utf8(std::filesystem::relative(it->path(), root).wstring());
    for (char& c : rel)
      if (c == '\\') c = '/';
    out.push_back(rel);
  }
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace test
