// FILE_ID_INFO (Windows 8 and later; the importer needs Windows 10) for the
// folder reader's directory ids. Before any header.
#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0602
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#include "source.h"

#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <map>

#include "fat.h"
#include "iso9660.h"
#include "md5.h"
#include "names.h"
#include "sevenzip.h"
#include "winutil.h"
#include "zip.h"

namespace adw::import {

namespace fs = std::filesystem;

// ---- common ----------------------------------------------------------------------

std::optional<SourceNode> SourceFs::child(const SourceNode& dir, std::string_view name) const {
  if (!dir.is_dir) return std::nullopt;
  for (SourceNode& n : list(dir))
    if (iequals(n.name, name) || (!n.alt_name.empty() && iequals(n.alt_name, name))) return std::move(n);
  return std::nullopt;
}

std::optional<SourceNode> SourceFs::find(std::string_view path) const {
  SourceNode cur = root();
  size_t i = 0;
  while (i <= path.size()) {
    size_t j = path.find_first_of("/\\", i);
    if (j == std::string_view::npos) j = path.size();
    std::string_view comp = path.substr(i, j - i);
    i = j + 1;
    if (comp.empty() || comp == ".") continue;
    auto next = child(cur, comp);
    if (!next) return std::nullopt;
    cur = std::move(*next);
  }
  return cur;
}

std::vector<uint8_t> SourceFs::read_all(const SourceNode& file, uint64_t max_bytes) const {
  if (file.size > max_bytes)
    throw ImportError(Status::source_invalid, file.name + " is implausibly large (" + std::to_string(file.size) + " bytes)");
  std::vector<uint8_t> out;
  out.reserve(size_t(file.size));
  read(file, [&](const uint8_t* p, size_t n) {
    if (out.size() + n > max_bytes) throw ImportError(Status::source_invalid, file.name + " is implausibly large");
    out.insert(out.end(), p, p + n);
  });
  return out;
}

namespace {

[[noreturn]] void invalid(const std::string& what) { throw ImportError(Status::source_invalid, what); }

// ---- ISO-9660 -------------------------------------------------------------------------

// The recording time as a UTC FILETIME, or nothing when the record holds a
// time Windows cannot represent (hour 25, 30 February…): a damaged timestamp
// should leave the copy with its import time, not 1601-01-01.
std::optional<FILETIME> iso_filetime(const IsoTime& t) {
  SYSTEMTIME st{};
  st.wYear = WORD(t.year);
  st.wMonth = WORD(t.month);
  st.wDay = WORD(t.day);
  st.wHour = WORD(t.hour);
  st.wMinute = WORD(t.minute);
  st.wSecond = WORD(t.second);
  FILETIME ft{};
  if (!SystemTimeToFileTime(&st, &ft)) return std::nullopt;
  // The recorded time is local to the mastering site; subtract its GMT offset
  // (ECMA-119 allows -48..+52 quarter hours).
  if (t.gmt_offset_15min < -48 || t.gmt_offset_15min > 52) return std::nullopt;
  ULARGE_INTEGER u{};
  u.LowPart = ft.dwLowDateTime;
  u.HighPart = ft.dwHighDateTime;
  u.QuadPart -= int64_t(t.gmt_offset_15min) * 15 * 60 * 10000000LL;
  ft.dwLowDateTime = u.LowPart;
  ft.dwHighDateTime = u.HighPart;
  return ft;
}

class IsoFs : public SourceFs {
 public:
  explicit IsoFs(std::unique_ptr<IsoImage> iso) : iso_(std::move(iso)) {}

  SourceNode root() const override { return node(iso_->root(), true); }

  std::vector<SourceNode> list(const SourceNode& dir) const override {
    try {
      std::vector<SourceNode> out;
      for (IsoEntry& e : iso_->list(entry(dir))) out.push_back(node(std::move(e), false));
      return out;
    } catch (const IsoError& e) {
      invalid(e.what());
    }
  }

  void read(const SourceNode& file, const Sink& sink) const override {
    try {
      iso_->read(entry(file), sink);
    } catch (const IsoError& e) {
      invalid(e.what());
    }
  }

  std::string format() const override { return iso_->joliet() ? "iso9660+joliet" : "iso9660"; }
  std::string volume_id() const override { return iso_->volume_id(); }
  // The first extent: where the directory's records are. The tree matters
  // too (a Joliet directory and its primary twin are different records).
  std::string dir_key(const SourceNode& dir) const override {
    const IsoEntry& e = entry(dir);
    if (!e.is_dir || e.extents.empty()) return {};
    return std::string(e.in_joliet ? "j:" : "p:") + std::to_string(e.extents[0].lba);
  }

 private:
  std::unique_ptr<IsoImage> iso_;

  static const IsoEntry& entry(const SourceNode& n) { return *static_cast<const IsoEntry*>(n.impl.get()); }

  static SourceNode node(IsoEntry e, bool is_root) {
    SourceNode n;
    if (!is_root) {
      n.name = ascii_upper(e.short_name.empty() ? e.name : e.short_name);
      if (!e.short_name.empty() && !iequals(e.short_name, e.name)) n.alt_name = e.name;
    }
    n.is_dir = e.is_dir;
    n.size = e.size;
    if (e.mtime.valid) n.mtime = iso_filetime(e.mtime);
    n.impl = std::make_shared<IsoEntry>(std::move(e));
    return n;
  }
};

// ---- FAT --------------------------------------------------------------------------------

class FatFs : public SourceFs {
 public:
  explicit FatFs(std::unique_ptr<FatImage> fat) : fat_(std::move(fat)) {}

  SourceNode root() const override { return node(fat_->root()); }

  std::vector<SourceNode> list(const SourceNode& dir) const override {
    try {
      std::vector<SourceNode> out;
      for (FatEntry& e : fat_->list(entry(dir))) out.push_back(node(std::move(e)));
      return out;
    } catch (const FatError& e) {
      invalid(e.what());
    }
  }

  void read(const SourceNode& file, const Sink& sink) const override {
    try {
      fat_->read(entry(file), sink);
    } catch (const FatError& e) {
      invalid(e.what());
    }
  }

  std::string format() const override { return fat_->fat_bits() == 12 ? "fat12" : "fat16"; }
  std::string volume_id() const override { return fat_->volume_label(); }
  // A subdirectory is its cluster chain, which starts at its first cluster.
  std::string dir_key(const SourceNode& dir) const override {
    const FatEntry& e = entry(dir);
    if (!e.is_dir) return {};
    return e.root ? std::string("root") : std::to_string(e.first_cluster);
  }

 private:
  std::unique_ptr<FatImage> fat_;

  static const FatEntry& entry(const SourceNode& n) { return *static_cast<const FatEntry*>(n.impl.get()); }

  static SourceNode node(FatEntry e) {
    SourceNode n;
    n.name = e.root ? std::string() : e.name;
    n.is_dir = e.is_dir;
    n.size = e.size;
    if (!e.root) n.mtime = dos_filetime(e.dos_date, e.dos_time);
    n.impl = std::make_shared<FatEntry>(std::move(e));
    return n;
  }
};

// ---- folder -----------------------------------------------------------------------------

class FolderFs : public SourceFs {
 public:
  explicit FolderFs(fs::path dir) : dir_(std::move(dir)) {}

  SourceNode root() const override {
    SourceNode n;
    n.is_dir = true;
    n.impl = std::make_shared<fs::path>(dir_);
    return n;
  }

  // The name as listed, upper-cased. Not the file system's short alias:
  // "LEVEL2~1.AFI" is never the disc's own name, and whether a volume makes
  // aliases at all is a per-volume setting (8dot3name), so the same copy
  // would import under different names from different drives.
  std::vector<SourceNode> list(const SourceNode& dir) const override {
    const fs::path& d = path(dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((d / L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) invalid("cannot list " + to_utf8(d.wstring()) + ": " + win_error_string(GetLastError()));
    std::vector<SourceNode> out;
    do {
      if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
      SourceNode n;
      n.name = ascii_upper(to_utf8(fd.cFileName));
      n.is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
      n.size = n.is_dir ? 0 : (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
      n.mtime = fd.ftLastWriteTime;
      n.impl = std::make_shared<fs::path>(d / fd.cFileName);
      out.push_back(std::move(n));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
  }

  void read(const SourceNode& file, const Sink& sink) const override {
    const fs::path& p = path(file);
    Handle in(CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN,
                          nullptr));
    if (!in.valid()) invalid("cannot read " + to_utf8(p.wstring()) + ": " + win_error_string(GetLastError()));
    std::vector<uint8_t> buf(1 << 20);
    for (;;) {
      DWORD got = 0;
      if (!ReadFile(in.get(), buf.data(), DWORD(buf.size()), &got, nullptr))
        invalid("read error on " + to_utf8(p.wstring()) + ": " + win_error_string(GetLastError()));
      if (!got) break;
      sink(buf.data(), got);
    }
  }

  std::string format() const override { return "folder"; }

  // The volume and file id: a junction or directory symlink back up the tree
  // (or across it) reaches a directory already seen under another name. A
  // file system that reports no id (all zero) gives no key.
  std::string dir_key(const SourceNode& dir) const override {
    if (!dir.is_dir) return {};
    Handle h(CreateFileW(path(dir).c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!h.valid()) return {};
    char buf[64];
    FILE_ID_INFO id{};
    if (GetFileInformationByHandleEx(h.get(), FileIdInfo, &id, sizeof(id))) {
      bool zero = true;
      for (BYTE b : id.FileId.Identifier) zero = zero && !b;
      if (zero) return {};
      std::string key = std::to_string(id.VolumeSerialNumber) + ":";
      for (BYTE b : id.FileId.Identifier) {
        snprintf(buf, sizeof(buf), "%02x", b);
        key += buf;
      }
      return key;
    }
    BY_HANDLE_FILE_INFORMATION bh{};
    if (!GetFileInformationByHandle(h.get(), &bh) || (!bh.nFileIndexHigh && !bh.nFileIndexLow)) return {};
    snprintf(buf, sizeof(buf), "%lu:%08lx%08lx", (unsigned long)bh.dwVolumeSerialNumber, (unsigned long)bh.nFileIndexHigh,
             (unsigned long)bh.nFileIndexLow);
    return buf;
  }

 private:
  fs::path dir_;
  static const fs::path& path(const SourceNode& n) { return *static_cast<const fs::path*>(n.impl.get()); }
};

// ---- ZIP and 7z: where an archive source's files are -------------------------------------

// "DISK1, DISK2 and DISK3".
std::string disks_named(const std::vector<std::string>& names) {
  std::string s;
  for (size_t i = 0; i < names.size(); i++) s += (i == 0 ? "" : i + 1 == names.size() ? " and " : ", ") + names[i];
  return s;
}

// One entry of an archive source: its path, '/'-separated (a folder's own
// entry without a trailing '/'), and whether it is a folder's own entry.
struct ArchiveEntry {
  std::string path;
  bool directory = false;
};

// Where an archive source's files are (source.h): at its root, or in the
// one folder that holds everything else (the Internet Archive's ZIPs of a
// release's folder: "Opus n Bill - On the Road Again/"); there, the files
// themselves, or only DISK<n> folders of them. The same rule for a ZIP and a
// 7z.
struct ArchiveLayout {
  std::string top;                         // the one folder everything is in; "" = the archive's root
  std::map<unsigned, std::string> disks;   // each disk once, under the folder name it first appears as
  std::map<unsigned, std::vector<size_t>> files;  // the files of the root (0) and of each disk, by entry index
};

// `kind` names the archive in messages ("ZIP", "7z"), `word` its entries
// ("member", "entry"). Throws ImportError(source_invalid) for anything else
// the archive holds: a folder besides a DISK<n> one, anything deeper, files
// beside DISK<n> folders.
ArchiveLayout archive_layout(const std::vector<ArchiveEntry>& entries, const std::string& name, const char* kind,
                             const char* word) {
  const std::string rule = std::string(" (a ") + kind +
                           " source holds the install files, or only DISK<n> folders of them, at its root or in one "
                           "folder)";
  ArchiveLayout l;
  // One folder that holds everything — and is no DISK<n> folder: a disk set
  // of one disk is that disk.
  if (!entries.empty()) {
    const std::string& p = entries.front().path;
    const std::string top = p.substr(0, p.find('/'));
    bool inside = false, all = !disk_folder_number(top);
    for (const ArchiveEntry& e : entries) {
      if (e.path.size() > top.size() && e.path.compare(0, top.size() + 1, top + "/") == 0) inside = true;
      else if (!(e.directory && e.path == top)) all = false;
    }
    if (all && inside) l.top = top;
  }
  std::string at_root;
  for (size_t i = 0; i < entries.size(); i++) {
    const ArchiveEntry& e = entries[i];
    if (!l.top.empty() && e.path == l.top) continue;  // the folder's own entry
    const std::string rel = l.top.empty() ? e.path : e.path.substr(l.top.size() + 1);
    const size_t slash = rel.find('/');
    const std::string folder = slash != std::string::npos ? rel.substr(0, slash) : e.directory ? rel : "";
    const unsigned disk = folder.empty() ? 0 : disk_folder_number(folder);
    // A folder's own entry is a DISK<n> folder; a file is at the root or
    // directly in one.
    const bool flat = slash == std::string::npos || (!e.directory && rel.find('/', slash + 1) == std::string::npos);
    if (!folder.empty() && (!disk || !flat))
      invalid(name + ": " + word + " \"" + e.path + "\" is not a bare file name or a file in a DISK<n> folder" + rule);
    if (disk) l.disks.emplace(disk, folder);
    if (e.directory) continue;
    l.files[disk].push_back(i);
    if (!disk && at_root.empty()) at_root = rel;
  }
  if (!l.disks.empty() && !at_root.empty()) {
    std::vector<std::string> labels;
    for (const auto& [n, folder] : l.disks) labels.push_back(folder);
    invalid(name + " holds files " + (l.top.empty() ? std::string("at its root") : "in " + l.top) + " (" + at_root +
            ") beside DISK<n> folders (" + disks_named(labels) + ")" + rule);
  }
  return l;
}

// What `note` says of an archive source read from one folder or as a disk set.
std::string layout_note(const std::string& name, const ArchiveLayout& l) {
  std::string s;
  if (!l.top.empty()) s = "reading " + name + "'s folder " + l.top + " as the source";
  if (l.disks.empty()) return s;
  std::vector<std::string> labels;
  for (const auto& [n, folder] : l.disks) labels.push_back(folder);
  return (s.empty() ? "reading " + name : s + ",") + " as the union of its folders " + disks_named(labels) +
         " (one install disk each)";
}

// ---- ZIP ---------------------------------------------------------------------------------

// A ZIP of an install folder (the Internet Archive's Simpsons copies): an
// archive whose members are the files at the source's root (or in its one
// folder: archive_layout). It is held in memory (the known ones are under
// 5 MB), and a member is inflated and its size and CRC-32 checked as it is
// read. The outer archive is only a container, so a password-protected
// member is refused rather than guessed at; the installer's own encrypted
// archives are members like any other file. One view shows the files of the
// archive's root, or of one of its DISK<n> folders (a disk set: one view per
// disk, unioned).
class ZipFs : public SourceFs {
 public:
  ZipFs(std::shared_ptr<const ZipArchive> zip, unsigned disk, std::vector<const ZipMember*> files)
      : zip_(std::move(zip)), disk_(disk), files_(std::move(files)) {}

  SourceNode root() const override {
    SourceNode n;
    n.is_dir = true;
    return n;
  }

  std::vector<SourceNode> list(const SourceNode& dir) const override {
    std::vector<SourceNode> out;
    if (!dir.is_dir) return out;
    for (const ZipMember* m : files_) {
      SourceNode n;
      n.name = ascii_upper(m->name.substr(m->name.rfind('/') + 1));  // without its folders
      n.size = m->usize;
      n.mtime = dos_filetime(m->mod_date, m->mod_time);
      n.impl = std::make_shared<ZipMember>(*m);
      out.push_back(std::move(n));
    }
    return out;
  }

  void read(const SourceNode& file, const Sink& sink) const override {
    try {
      zip_->extract(*static_cast<const ZipMember*>(file.impl.get()), "", sink);
    } catch (const ZipError& e) {
      invalid(e.what());
    }
  }

  std::string format() const override { return "zip"; }
  // Flat: the root (or the disk's folder) is the only directory.
  std::string dir_key(const SourceNode& dir) const override {
    return !dir.is_dir ? "" : disk_ ? "disk" + std::to_string(disk_) : "root";
  }

 private:
  std::shared_ptr<const ZipArchive> zip_;
  unsigned disk_;
  std::vector<const ZipMember*> files_;  // the members it shows, in archive order (zip_ keeps them)
};

bool starts_with_zip_signature(const fs::path& path) {
  Handle in(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
  uint8_t sig[4] = {};
  DWORD got = 0;
  return in.valid() && ReadFile(in.get(), sig, 4, &got, nullptr) && got == 4 && sig[0] == 'P' && sig[1] == 'K' &&
         sig[2] == 3 && sig[3] == 4;
}

// A ZIP source is held in memory, at most this large.
constexpr uint64_t kMaxZip = 256ull << 20;

// The whole file, or nothing when it is larger than kMaxZip.
std::shared_ptr<std::vector<uint8_t>> read_zip_bytes(const fs::path& path) {
  Handle in(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN,
                        nullptr));
  if (!in.valid()) invalid("cannot read " + to_utf8(path.wstring()) + ": " + win_error_string(GetLastError()));
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(in.get(), &size) || uint64_t(size.QuadPart) > kMaxZip) return nullptr;
  auto data = std::make_shared<std::vector<uint8_t>>(size_t(size.QuadPart));
  size_t have = 0;
  while (have < data->size()) {
    DWORD got = 0;
    DWORD want = DWORD(std::min<size_t>(data->size() - have, 1 << 20));
    if (!ReadFile(in.get(), data->data() + have, want, &got, nullptr) || !got)
      invalid("read error on " + to_utf8(path.wstring()) + ": " + win_error_string(GetLastError()));
    have += got;
  }
  return data;
}

// A ZIP of install files: flat, or a disk set of flat DISK<n> folders, at
// its root or in its one folder (archive_layout); a disk set is read as the
// union of one view per disk.
std::unique_ptr<SourceFs> open_zip(const fs::path& path, std::string* note) {
  const std::string name = to_utf8(path.filename().wstring());
  auto data = read_zip_bytes(path);
  if (!data) invalid(name + " is too large for a ZIP of install files");
  std::shared_ptr<ZipArchive> zip;
  try {
    zip = std::make_shared<ZipArchive>(std::move(data), name, ZipNames::paths);
  } catch (const ZipError& e) {
    invalid(e.what());
  }
  std::vector<ArchiveEntry> entries;
  for (const ZipMember& m : zip->members())
    entries.push_back({m.directory ? m.name.substr(0, m.name.size() - 1) : m.name, m.directory});
  const ArchiveLayout l = archive_layout(entries, name, "ZIP", "member");
  for (const ZipMember& m : zip->members())
    if (m.encrypted()) invalid(name + "!" + m.name + " is password-protected");
  auto files = [&](unsigned disk) {
    std::vector<const ZipMember*> out;
    if (auto it = l.files.find(disk); it != l.files.end())
      for (size_t i : it->second) out.push_back(&zip->members()[i]);
    return out;
  };
  if (note) *note = layout_note(name, l);
  if (l.disks.empty()) return std::make_unique<ZipFs>(std::move(zip), 0, files(0));
  std::vector<std::string> labels;
  std::vector<std::unique_ptr<SourceFs>> parts;
  for (const auto& [n, folder] : l.disks) {
    labels.push_back(folder);
    parts.push_back(std::make_unique<ZipFs>(zip, n, files(n)));
  }
  return union_of(std::move(parts), std::move(labels));
}

// ---- 7z ----------------------------------------------------------------------------------

// A 7z of an install folder, read as a ZIP of one is (above): its members
// are the files at the source's root (or in its one folder), or a disk
// set's DISK<n>/<bare name>. It is held in memory, and a member is decoded
// (its block once, however many files share it: sevenzip.h) and its size
// and CRC-32 checked as it is read. One view shows the root's files, or one
// DISK<n> folder's.
class SevenZipFs : public SourceFs {
 public:
  SevenZipFs(std::shared_ptr<const SevenZipArchive> archive, unsigned disk, std::vector<const SevenZipMember*> files)
      : archive_(std::move(archive)), disk_(disk), files_(std::move(files)) {}

  SourceNode root() const override {
    SourceNode n;
    n.is_dir = true;
    return n;
  }

  std::vector<SourceNode> list(const SourceNode& dir) const override {
    std::vector<SourceNode> out;
    if (!dir.is_dir) return out;
    for (const SevenZipMember* m : files_) {
      SourceNode n;
      n.name = ascii_upper(m->name.substr(m->name.rfind('/') + 1));  // without its folders
      n.size = m->size;
      if (m->mtime) n.mtime = FILETIME{DWORD(*m->mtime), DWORD(*m->mtime >> 32)};
      n.impl = std::shared_ptr<const void>(archive_, m);  // the member lives as long as its archive
      out.push_back(std::move(n));
    }
    return out;
  }

  void read(const SourceNode& file, const Sink& sink) const override {
    try {
      archive_->extract(*static_cast<const SevenZipMember*>(file.impl.get()), sink);
    } catch (const SevenZipError& e) {
      invalid(e.what());
    }
  }

  std::string format() const override { return "7z"; }
  // Flat: the root (or the disk's folder) is the only directory.
  std::string dir_key(const SourceNode& dir) const override {
    return !dir.is_dir ? "" : disk_ ? "disk" + std::to_string(disk_) : "root";
  }

 private:
  std::shared_ptr<const SevenZipArchive> archive_;
  unsigned disk_;
  std::vector<const SevenZipMember*> files_;  // the members it shows, in archive order
};

bool starts_with_7z_signature(const fs::path& path) {
  Handle in(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
  uint8_t sig[6] = {};
  DWORD got = 0;
  return in.valid() && ReadFile(in.get(), sig, 6, &got, nullptr) && got == 6 && is_7z_signature(sig);
}

// A 7z of install files, by the rule a ZIP source follows (archive_layout):
// flat, or a disk set of flat DISK<n> folders, at its root or in its one
// folder; a disk set is read as the union of one view per disk.
std::unique_ptr<SourceFs> open_7z(const fs::path& path, std::string* note) {
  const std::string name = to_utf8(path.filename().wstring());
  auto data = read_zip_bytes(path);
  if (!data) invalid(name + " is too large for a 7z of install files");
  std::shared_ptr<SevenZipArchive> archive;
  try {
    archive = std::make_shared<SevenZipArchive>(std::move(data), name);
  } catch (const SevenZipError& e) {
    invalid(e.what());
  }
  std::vector<ArchiveEntry> entries;
  for (const SevenZipMember& m : archive->members()) entries.push_back({m.name, m.directory});
  const ArchiveLayout l = archive_layout(entries, name, "7z", "entry");
  auto files = [&](unsigned disk) {
    std::vector<const SevenZipMember*> out;
    if (auto it = l.files.find(disk); it != l.files.end())
      for (size_t i : it->second) out.push_back(&archive->members()[i]);
    return out;
  };
  if (note) *note = layout_note(name, l);
  if (l.disks.empty()) return std::make_unique<SevenZipFs>(std::move(archive), 0, files(0));
  std::vector<std::string> labels;
  std::vector<std::unique_ptr<SourceFs>> parts;
  for (const auto& [n, folder] : l.disks) {
    labels.push_back(folder);
    parts.push_back(std::make_unique<SevenZipFs>(archive, n, files(n)));
  }
  return union_of(std::move(parts), std::move(labels));
}

// ---- disk sets ----------------------------------------------------------------------------

// One folder of another source, seen as a source of its own: a disk of a disk
// set. Its entries are the other source's, read through it.
class SubtreeFs : public SourceFs {
 public:
  SubtreeFs(std::shared_ptr<const SourceFs> base, SourceNode dir) : base_(std::move(base)), dir_(std::move(dir)) {}

  SourceNode root() const override {
    SourceNode n = dir_;
    n.name.clear();  // a root has no name
    n.alt_name.clear();
    return n;
  }
  std::vector<SourceNode> list(const SourceNode& dir) const override { return base_->list(dir); }
  void read(const SourceNode& file, const Sink& sink) const override { base_->read(file, sink); }
  std::string format() const override { return base_->format(); }
  std::string volume_id() const override { return base_->volume_id(); }
  std::string dir_key(const SourceNode& dir) const override { return base_->dir_key(dir); }

 private:
  std::shared_ptr<const SourceFs> base_;
  SourceNode dir_;
};

// A root that holds nothing but DISK<n> folders is read as their union;
// any other root as it is (source.h). `where` names the source in `note`.
std::unique_ptr<SourceFs> disk_set(std::unique_ptr<SourceFs> fs, const std::string& where, std::string* note) {
  std::map<unsigned, SourceNode> disks;
  std::vector<std::string> others;
  for (SourceNode& e : fs->list(fs->root())) {
    const unsigned n = e.is_dir ? disk_folder_number(e.name) : 0;
    if (!n) {
      others.push_back(e.name);
      continue;
    }
    // Two folders for one disk ("DISK1" and "disk1" on a case-sensitive
    // volume, or a crafted image): no release's disks.
    if (disks.count(n))
      invalid(where + " holds two folders for disk " + std::to_string(n) + " (" + disks[n].name + " and " + e.name +
              ")");
    disks.emplace(n, std::move(e));
  }
  if (disks.empty()) return fs;
  std::vector<std::string> labels;
  for (const auto& [n, node] : disks) labels.push_back(node.name);
  auto add_note = [&](const std::string& s) {
    if (note) *note += (note->empty() ? "" : "; ") + s;
  };
  if (!others.empty()) {
    add_note(where + " holds " + disks_named(labels) + " beside other files or folders (" + others.front() +
             (others.size() > 1 ? ", …" : "") +
             "): reading it as it is (install disks kept apart are read together "
             "only from a folder that holds nothing else)");
    return fs;
  }
  std::shared_ptr<const SourceFs> base(std::move(fs));
  std::vector<std::unique_ptr<SourceFs>> parts;
  for (auto& [n, node] : disks) parts.push_back(std::make_unique<SubtreeFs>(base, std::move(node)));
  add_note("reading " + where + " as the union of its folders " + disks_named(labels) + " (one install disk each)");
  return union_of(std::move(parts), std::move(labels));
}

// ---- union ------------------------------------------------------------------------------

class UnionFs : public SourceFs {
 public:
  UnionFs(std::vector<std::unique_ptr<SourceFs>> parts, std::vector<std::string> labels)
      : parts_(std::move(parts)), labels_(std::move(labels)) {}

  SourceNode root() const override {
    auto entry = std::make_shared<Entry>();
    for (size_t i = 0; i < parts_.size(); i++) entry->copies.push_back({i, parts_[i]->root()});
    SourceNode n;
    n.is_dir = true;
    n.impl = entry;
    return n;
  }

  std::vector<SourceNode> list(const SourceNode& dir) const override {
    std::vector<SourceNode> out;
    std::map<std::string, size_t> index;  // upper-case name -> out[]
    for (const auto& [part, node] : copies(dir)) {
      for (SourceNode& c : parts_[part]->list(node)) {
        auto it = index.find(c.name);
        if (it == index.end()) {
          index[c.name] = out.size();
          SourceNode u = c;
          u.impl = std::make_shared<Entry>(Entry{{{part, std::move(c)}}, {}});
          out.push_back(std::move(u));
          continue;
        }
        SourceNode& u = out[it->second];
        Entry& e = *static_cast<Entry*>(const_cast<void*>(u.impl.get()));
        const size_t first = e.copies.front().first;
        if (u.is_dir != c.is_dir) {
          auto kind = [](bool dir) { return std::string(dir ? "a folder in " : "a file in "); };
          invalid(c.name + " is " +
                  (labels_.empty() ? std::string("a file in one image and a folder in another")
                                   : kind(u.is_dir) + labels_[first] + " and " + kind(c.is_dir) + labels_[part]) +
                  "; they are not the disks of one release");
        }
        // Another size: refused when the file is read, never when it is only
        // listed (what the recipe never opens — a BBS's notes, which differ
        // from disk to disk — never stops an import).
        if (!u.is_dir && u.size != c.size && e.clash.empty())
          e.clash = c.name + " differs between " + between(first, part) +
                    " (size); they are not the disks of one release";
        e.copies.push_back({part, std::move(c)});
      }
    }
    return out;
  }

  // Every other copy is hashed first, then the first one streams while it
  // is hashed too: the same bytes everywhere, or the source is invalid.
  void read(const SourceNode& file, const Sink& sink) const override {
    const Entry& e = *static_cast<const Entry*>(file.impl.get());
    if (!e.clash.empty()) invalid(e.clash);
    const Copies& c = e.copies;
    std::string other;
    for (size_t i = 1; i < c.size(); i++) {
      Md5 h;
      parts_[c[i].first]->read(c[i].second, [&](const uint8_t* p, size_t n) { h.update(p, n); });
      std::string m = h.finish_hex();
      if (!other.empty() && m != other)
        invalid(file.name + " differs between " + between(c[1].first, c[i].first) +
                "; they are not the disks of one release");
      other = m;
    }
    if (other.empty()) {
      parts_[c[0].first]->read(c[0].second, sink);
      return;
    }
    Md5 h;
    parts_[c[0].first]->read(c[0].second, [&](const uint8_t* p, size_t n) {
      h.update(p, n);
      sink(p, n);
    });
    if (h.finish_hex() != other)
      invalid(file.name + " differs between " + between(c[0].first, c[1].first) +
              "; they are not the disks of one release");
  }

  std::string format() const override {
    std::string f = parts_.front()->format();
    for (auto& p : parts_)
      if (p->format() != f) return "mixed";
    return f;
  }
  std::string volume_id() const override { return parts_.front()->volume_id(); }
  // Every copy's key, with its image: the same directory only when it is the
  // same one in each image that has it.
  std::string dir_key(const SourceNode& dir) const override {
    std::string key;
    for (const auto& [part, node] : copies(dir)) {
      std::string k = parts_[part]->dir_key(node);
      if (k.empty()) return {};
      key += std::to_string(part) + "=" + k + ";";
    }
    return key;
  }

 private:
  using Copies = std::vector<std::pair<size_t, SourceNode>>;
  // A name's copies, one per part that has it, and why it cannot be read
  // ("" = it can): the first other size another part lists.
  struct Entry {
    Copies copies;
    std::string clash;
  };
  std::vector<std::unique_ptr<SourceFs>> parts_;
  std::vector<std::string> labels_;  // one per part, or none
  static const Copies& copies(const SourceNode& n) { return static_cast<const Entry*>(n.impl.get())->copies; }

  // "DISK1 and DISK3", or "the images" without labels.
  std::string between(size_t a, size_t b) const {
    return labels_.empty() ? std::string("the images") : labels_[a] + " and " + labels_[b];
  }
};

}  // namespace

std::optional<FILETIME> dos_filetime(uint16_t date, uint16_t time) {
  if (!date) return std::nullopt;
  SYSTEMTIME local{};
  local.wYear = WORD(1980 + (date >> 9));
  local.wMonth = WORD((date >> 5) & 15);
  local.wDay = WORD(date & 31);
  local.wHour = WORD(time >> 11);
  local.wMinute = WORD((time >> 5) & 63);
  local.wSecond = WORD((time & 31) * 2);
  SYSTEMTIME utc{};
  FILETIME ft{};
  if (!TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc) || !SystemTimeToFileTime(&utc, &ft)) return std::nullopt;
  return ft;
}

std::unique_ptr<SourceFs> open_image(const fs::path& path, std::string* note) {
  std::error_code ec;
  if (note) note->clear();
  if (!fs::is_regular_file(path, ec)) invalid("no such image file: " + to_utf8(path.wstring()));
  const std::string name = to_utf8(path.filename().wstring());
  std::string iso_why;
  std::unique_ptr<SourceFs> image;
  try {
    image = std::make_unique<IsoFs>(std::make_unique<IsoImage>(path));
  } catch (const IsoError& e) {
    iso_why = e.what();
  }
  if (image) return disk_set(std::move(image), name, note);
  // A local file header at byte 0: a ZIP of install files, never a floppy
  // (whose boot sector starts with a jump). The same for 7z's signature.
  if (starts_with_zip_signature(path)) return open_zip(path, note);
  if (starts_with_7z_signature(path)) return open_7z(path, note);
  try {
    image = std::make_unique<FatFs>(std::make_unique<FatImage>(path));
  } catch (const FatError& e) {
    invalid(name + " is neither an ISO-9660 disc image (" + iso_why + "), a FAT floppy image (" + e.what() +
            ") nor a ZIP or 7z of install files");
  }
  return disk_set(std::move(image), name, note);
}

std::unique_ptr<SourceFs> open_folder(const fs::path& dir, std::string* note) {
  std::error_code ec;
  if (note) note->clear();
  if (!fs::is_directory(dir, ec)) invalid("no such folder: " + to_utf8(dir.wstring()));
  // The root of a CD drive (a disc, or an image Windows mounted) is read as
  // the disc itself: Windows lists a Joliet disc by its long names
  // ("Toaster 2k.ad"), while the ISO reader pairs each with the 8.3 name the
  // release, its manifest and the catalog ids use (TOASTER2.AD).
  std::wstring s = dir.wstring();
  while (s.size() > 2 && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
  if (s.size() == 2 && s[1] == L':' && iswalpha(s[0])) {
    std::wstring root = s + L"\\";
    if (GetDriveTypeW(root.c_str()) == DRIVE_CDROM) {
      std::unique_ptr<SourceFs> iso;
      try {
        iso = std::make_unique<IsoFs>(std::make_unique<IsoImage>(L"\\\\.\\" + s));
        if (note) *note = "reading " + to_utf8(root) + " as a disc (" + iso->format() + ")";
      } catch (const IsoError& e) {
        if (note) *note = "cannot read " + to_utf8(root) + " as a disc (" + e.what() + "); reading it as a folder";
      }
      if (iso) return disk_set(std::move(iso), to_utf8(root), note);
    }
  }
  return disk_set(std::make_unique<FolderFs>(dir), to_utf8(dir.wstring()), note);
}

std::unique_ptr<SourceFs> union_of(std::vector<std::unique_ptr<SourceFs>> parts, std::vector<std::string> labels) {
  if (parts.size() == 1) return std::move(parts.front());
  if (!labels.empty() && labels.size() != parts.size()) labels.clear();  // one per part, or none
  return std::make_unique<UnionFs>(std::move(parts), std::move(labels));
}

std::vector<ZippedImage> floppy_images_in_zip(const fs::path& path, std::vector<std::string>* ignored,
                                              uint64_t max_bytes) {
  std::vector<ZippedImage> out;
  std::error_code ec;
  if (!fs::is_regular_file(path, ec)) return out;
  const bool seven = starts_with_7z_signature(path);
  if (!seven && !starts_with_zip_signature(path)) return out;
  // Too large, or no ZIP or 7z open_image would read: open_image says why.
  auto data = read_zip_bytes(path);
  if (!data) return out;
  const std::string name = to_utf8(path.filename().wstring());
  std::unique_ptr<ZipArchive> zip;
  std::shared_ptr<SevenZipArchive> sz;
  try {
    if (seven) sz = std::make_shared<SevenZipArchive>(data, name);
    else zip = std::make_unique<ZipArchive>(data, name, ZipNames::paths);
  } catch (const ZipError&) {
    return out;
  } catch (const SevenZipError&) {
    return out;
  }
  std::vector<std::string> others;
  struct NoBootSector {};
  uint64_t inflated = 0;  // the members with a boot sector, inflated whole
  // One member: its name, size, and how to stream it (the archive's errors
  // made source_invalid).
  auto take = [&](const std::string& member, uint64_t size, const std::function<void(const Sink&)>& extract) {
    // The sizes of DOS floppies, 160 KB to 2.88 MB: nothing else is
    // inflated (a whole item's scans and metadata are only named).
    const bool floppy_sized = size % 512 == 0 && size >= 163840 && size <= 2949120;
    if (!floppy_sized) {
      others.push_back(member);
      return;
    }
    auto bytes = std::make_shared<std::vector<uint8_t>>();
    try {
      extract([&](const uint8_t* p, size_t n) {
        const bool first = bytes->size() < 512;
        bytes->insert(bytes->end(), p, p + n);
        if (!first || bytes->size() < 512) return;
        // The first sector decides whether the rest is worth inflating: a
        // FAT volume's boot sector ends in 55 AA and names a sector size
        // FatImage takes (a crafted ZIP of thousands of floppy-sized members
        // costs a chunk each). Every image is held in memory together.
        const uint8_t* bs = bytes->data();
        const unsigned bps = unsigned(bs[11] | bs[12] << 8);
        if (bs[510] != 0x55 || bs[511] != 0xAA || (bps != 512 && bps != 1024 && bps != 2048 && bps != 4096))
          throw NoBootSector{};
        if (size > max_bytes - std::min(inflated, max_bytes))
          invalid(name + " holds more than " + std::to_string(max_bytes >> 20) +
                  " MB of disk images; no release came on that many disks");
        bytes->reserve(size_t(size));
      });
    } catch (const NoBootSector&) {
      others.push_back(member);  // floppy-sized, but no FAT volume
      return;
    }
    inflated += size;
    try {
      FatImage probe{std::shared_ptr<const std::vector<uint8_t>>(bytes)};
    } catch (const FatError&) {
      others.push_back(member);  // floppy-sized, but no FAT volume
      return;
    }
    out.push_back({member, std::move(bytes)});
  };
  // The images may sit in a folder of either ("<folder>/DISK1.IMG", the
  // Internet Archive's "Intermission 4.0/ITM4W-D1.IMA"): only its name in
  // messages and in import.json; folders' own entries are skipped.
  if (zip) {
    for (const ZipMember& m : zip->members()) {
      if (m.directory) continue;
      const bool floppy_sized = m.usize % 512 == 0 && m.usize >= 163840 && m.usize <= 2949120;
      if (floppy_sized && m.encrypted()) invalid(name + "!" + m.name + " is password-protected");
      take(m.name, m.usize, [&](const Sink& sink) {
        try {
          zip->extract(m, "", sink);
        } catch (const ZipError& e) {
          invalid(e.what());
        }
      });
    }
  } else {
    // An encrypted 7z never opens (sevenzip.h).
    for (const SevenZipMember& m : sz->members()) {
      if (m.directory) continue;
      take(m.name, m.size, [&](const Sink& sink) {
        try {
          sz->extract(m, sink);
        } catch (const SevenZipError& e) {
          invalid(e.what());
        }
      });
    }
  }
  if (ignored && !out.empty()) *ignored = std::move(others);
  return out;
}

std::unique_ptr<SourceFs> open_fat_image(std::shared_ptr<const std::vector<uint8_t>> bytes, const std::string& name,
                                         std::string* note) {
  if (note) note->clear();
  std::unique_ptr<SourceFs> image;
  try {
    image = std::make_unique<FatFs>(std::make_unique<FatImage>(std::move(bytes)));
  } catch (const FatError& e) {
    invalid(name + " is not a FAT floppy image (" + e.what() + ")");
  }
  return disk_set(std::move(image), name, note);
}

}  // namespace adw::import
