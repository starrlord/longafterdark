// Small Win32 helpers shared by the importer sources (internal header).
//
// The importer speaks UTF-8 std::string at its API edges (names, messages,
// import.json) and UTF-16 to Win32; these are the two conversions, the code
// page 437 names of DOS-era sources, an owning HANDLE and the write/rename
// steps every atomic replacement is built from, kept header-only so the tests
// can use them too. The test and repair of UTF-8 itself are utf8.h's
// (portable, so minijson.cc needs no Win32), included here.
#pragma once

#include <windows.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

#include "utf8.h"

namespace adw::import {

inline std::string to_utf8(std::wstring_view w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string out(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), out.data(), n, nullptr, nullptr);
  return out;
}

inline std::wstring to_wide(std::string_view s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring out(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
  return out;
}

// Names in OEM code page 437 (DOS directory entries and volume labels on FAT
// volumes, member names in ARJ archives, ZIP member names that are not UTF-8)
// as UTF-8; pure ASCII is passed through unchanged.
inline std::string oem437_to_utf8(std::string_view oem) {
  bool ascii = true;
  for (char c : oem) ascii = ascii && static_cast<unsigned char>(c) < 0x80;
  if (ascii) return std::string(oem);
  int w = MultiByteToWideChar(437, 0, oem.data(), int(oem.size()), nullptr, 0);
  std::wstring ws(size_t(w > 0 ? w : 0), L'\0');
  if (w > 0) MultiByteToWideChar(437, 0, oem.data(), int(oem.size()), ws.data(), w);
  return to_utf8(ws);
}

// "The system cannot find the file specified. (2)" — for error messages.
inline std::string win_error_string(DWORD err) {
  wchar_t* buf = nullptr;
  DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                               FORMAT_MESSAGE_IGNORE_INSERTS,
                           nullptr, err, 0, (wchar_t*)&buf, 0, nullptr);
  std::wstring msg = n ? std::wstring(buf, n) : L"error";
  if (buf) LocalFree(buf);
  while (!msg.empty() && (msg.back() == L'\r' || msg.back() == L'\n' || msg.back() == L' ' || msg.back() == L'.'))
    msg.pop_back();
  return to_utf8(msg) + " (" + std::to_string(err) + ")";
}

// Owning Win32 file HANDLE (INVALID_HANDLE_VALUE = empty).
class Handle {
 public:
  Handle() = default;
  explicit Handle(HANDLE h) : h_(h) {}
  ~Handle() { reset(); }
  Handle(Handle&& o) noexcept : h_(std::exchange(o.h_, INVALID_HANDLE_VALUE)) {}
  Handle& operator=(Handle&& o) noexcept {
    if (this != &o) { reset(); h_ = std::exchange(o.h_, INVALID_HANDLE_VALUE); }
    return *this;
  }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;

  HANDLE get() const { return h_; }
  bool valid() const { return h_ != INVALID_HANDLE_VALUE && h_ != nullptr; }
  void reset() {
    if (valid()) CloseHandle(h_);
    h_ = INVALID_HANDLE_VALUE;
  }

 private:
  HANDLE h_ = INVALID_HANDLE_VALUE;
};

// Writes `data` as the whole of `p` (created or truncated) and flushes it to
// disk, so a rename that follows publishes complete contents. On failure the
// Win32 error is in `err`, captured before anything can disturb it.
inline bool write_whole_file(const std::filesystem::path& p, std::string_view data, DWORD& err) {
  Handle w(CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
  DWORD wrote = 0;
  if (!w.valid() || !WriteFile(w.get(), data.data(), DWORD(data.size()), &wrote, nullptr) || wrote != data.size()) {
    err = GetLastError();
    return false;
  }
  FlushFileBuffers(w.get());
  err = 0;
  return true;
}

// Deletes a file or a directory tree (read-only files included; a junction
// or directory symlink is removed, never followed). Best effort: returns
// false when anything could not be deleted. std::filesystem::remove_all is
// not used for this: with this toolchain's libc++ it takes ~170 ms per file
// (15 s for a package tree that DeleteFileW removes in 15 ms).
inline bool remove_tree(const std::filesystem::path& p) {
  DWORD attr = GetFileAttributesW(p.c_str());
  if (attr == INVALID_FILE_ATTRIBUTES) {
    DWORD e = GetLastError();
    return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND;
  }
  if (attr & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(p.c_str(), attr & ~DWORD(FILE_ATTRIBUTE_READONLY));
  if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) return DeleteFileW(p.c_str()) != 0;
  bool ok = true;
  if (!(attr & FILE_ATTRIBUTE_REPARSE_POINT)) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((p / L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h != INVALID_HANDLE_VALUE) {
      do {
        if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) ok = remove_tree(p / fd.cFileName) && ok;
      } while (FindNextFileW(h, &fd));
      FindClose(h);
    }
  }
  return RemoveDirectoryW(p.c_str()) != 0 && ok;
}

// Renames fail while anything inside (or the file itself) is open without
// FILE_SHARE_DELETE (a running host, an Explorer preview, antivirus); those
// holds are usually brief, so retry for a couple of seconds. On failure the
// Win32 error is returned through `err` (0 = success).
inline bool move_with_retry(const std::filesystem::path& from, const std::filesystem::path& to, DWORD flags,
                            DWORD& err) {
  for (int i = 0; i < 20; i++) {
    if (MoveFileExW(from.c_str(), to.c_str(), flags)) {
      err = 0;
      return true;
    }
    err = GetLastError();
    if (err != ERROR_ACCESS_DENIED && err != ERROR_SHARING_VIOLATION) return false;
    Sleep(100);
  }
  return false;
}

}  // namespace adw::import
