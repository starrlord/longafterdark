#include "win16/dos16.hh"

#include <windows.h>

#include <algorithm>
#include <cctype>

#include "adw/core/log.h"
#include "adw/core/text.h"
#include "win16/modules16.hh"
#include "win32/display.hh"
#include "win32/ini_store.hh"
#include "win32/vfs.hh"

namespace adw::win16 {

namespace {

using SegReg = cpu::X86Emulator::SegReg;
using Regs = cpu::X86Emulator::Regs;

std::string upper(std::string_view s) {
  std::string u(s);
  for (char& c : u) c = char(toupper(uint8_t(c)));
  return u;
}

uint16_t seg(Runtime16& rt, SegReg s) { return rt.cpu().get_segment(s); }
uint32_t farp(uint16_t sel, uint16_t off) { return (uint32_t(sel) << 16) | off; }

// A DOS wildcard match ("*.AD", "FOO?.BMP"), case-insensitive, with the DOS
// rule that "*.*" matches names without an extension too.
bool wild_match(std::string_view pat, std::string_view name) {
  size_t p = 0, n = 0, star_p = std::string_view::npos, star_n = 0;
  while (n < name.size()) {
    if (p < pat.size() && (pat[p] == '?' || toupper(uint8_t(pat[p])) == toupper(uint8_t(name[n])))) {
      p++;
      n++;
    } else if (p < pat.size() && pat[p] == '*') {
      star_p = p++;
      star_n = n;
    } else if (star_p != std::string_view::npos) {
      p = star_p + 1;
      n = ++star_n;
    } else {
      return false;
    }
  }
  while (p < pat.size() && (pat[p] == '*' || pat[p] == '.' || pat[p] == '?')) p++;
  return p == pat.size();
}

struct DosState : RuntimeState16 {
  uint32_t dta = 0;
  uint16_t psp_sel = 0;
  uint16_t last_error = 0;
  std::vector<std::vector<DosFiles::Found>> finds;
  std::map<uint8_t, uint32_t> vectors;  // INT 21h AH=25h, for AH=35h to hand back
  uint16_t iret_off = 0;
  bool reported[256] = {};
};

DosState& ds_state(Runtime16& rt) { return rt.state<DosState>(); }

}  // namespace

uint16_t dos16_psp(Runtime16& rt) {
  DosState& st = ds_state(rt);
  if (!st.psp_sel) {
    st.psp_sel = rt.ldt().alloc(1);
    rt.ldt().set(st.psp_sel, cpu::SegDesc{layout::kSysBase + layout::kSysPsp, 0xFF, true, false, true, false, 3});
    rt.ldt().set_tag(st.psp_sel, "PSP");
  }
  return st.psp_sel;
}

void dos16_set_command_tail(Runtime16& rt, std::string_view tail) {
  const uint32_t psp = layout::kSysBase + layout::kSysPsp;
  const size_t n = std::min<size_t>(tail.size(), 126);
  rt.mem().write_u8(psp + 0x80, uint8_t(n));
  if (n) rt.mem().memcpy(psp + 0x81, tail.data(), n);
  rt.mem().write_u8(psp + 0x81 + uint32_t(n), 0x0D);
}

namespace {

SYSTEMTIME now_local(Runtime16& rt) {
  uint64_t ft = rt.local_filetime();
  FILETIME f{DWORD(ft), DWORD(ft >> 32)};
  SYSTEMTIME st{};
  FileTimeToSystemTime(&f, &st);
  return st;
}

uint8_t bcd(unsigned v) { return uint8_t(((v / 10) % 10) << 4 | (v % 10)); }

}  // namespace

// ---- DosFiles ------------------------------------------------------------------------------------------

DosFiles::~DosFiles() = default;

uint16_t DosFiles::dos_error(uint32_t e) {
  switch (e) {
    case ERROR_FILE_NOT_FOUND:
      return doserr::kFileNotFound;
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME:
    case ERROR_BAD_PATHNAME:
      return doserr::kPathNotFound;
    case ERROR_TOO_MANY_OPEN_FILES:
      return doserr::kTooManyFiles;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
      return doserr::kFileExists;
    case ERROR_NO_MORE_FILES:
      return doserr::kNoMoreFiles;
    default:
      return doserr::kAccessDenied;
  }
}

int DosFiles::add(File f) {
  for (uint16_t h = 5; h < 255; h++) {
    if (!files_.count(h)) {
      files_[h] = std::move(f);
      return h;
    }
  }
  return -int(doserr::kTooManyFiles);
}

int DosFiles::open(const std::string& guest_path, int mode, bool create, bool exclusive) {
  win32::Vfs& vfs = rt_.vfs();
  std::string full = vfs.full_path(guest_path);
  using A = win32::Vfs::Access;
  using D = win32::Vfs::Disposition;
  A access = create ? A::read_write : (mode & 3) == 0 ? A::read : (mode & 3) == 1 ? A::write : A::read_write;
  D disp = !create ? D::open_existing : exclusive ? D::create_new : D::create_always;
  if (!create && vfs.is_dir(full)) return -int(doserr::kAccessDenied);
  uint32_t err = 0;
  std::unique_ptr<win32::VfsFile> f = vfs.open(full, access, disp, &err);
  if (!f) {
    trace("file16", "open %s (%s) failed: error %u", full.c_str(), access == A::read ? "read" : "write", err);
    return -int(dos_error(err));
  }
  int h = add(File{full, std::shared_ptr<win32::VfsFile>(std::move(f))});
  trace("file16", "open %s%s -> handle %d", full.c_str(), access == A::read ? "" : create ? " (created)" : " for writing", h);
  return h;
}

int DosFiles::close(uint16_t h) {
  if (h < 5) return 0;
  auto it = files_.find(h);
  if (it == files_.end()) return -int(doserr::kInvalidHandle);
  // The last handle on a file written to flushes it (a persistent upper: the atomic replace).
  if (it->second.f.use_count() == 1 && it->second.f->writable()) it->second.f->flush();
  files_.erase(it);
  return 0;
}

int DosFiles::dup(uint16_t h) {
  auto it = files_.find(h);
  if (it == files_.end()) return h < 5 ? h : -int(doserr::kInvalidHandle);
  File f = it->second;
  return add(std::move(f));
}

uint32_t DosFiles::size_of(uint16_t h) {
  auto it = files_.find(h);
  return it == files_.end() ? 0 : uint32_t(it->second.f->size());
}

// Reads and writes move one 64 KiB tile of the (possibly huge) guest buffer
// at a time, through one tile-sized host buffer: the count is the guest's
// (_hread takes 32 bits), so it never sizes a host allocation, and a read
// stops at the end of the file.
int32_t DosFiles::read(uint16_t h, uint32_t fp, uint32_t n) {
  if (h < 5) return 0;  // console input: nothing typed
  auto it = files_.find(h);
  if (it == files_.end()) return -int(doserr::kInvalidHandle);
  win32::VfsFile& f = *it->second.f;
  std::vector<uint8_t> buf(std::min<uint32_t>(n, 0x10000));
  uint32_t dst = fp, done = 0;
  while (done < n) {
    uint32_t room = 0x10000 - (dst & 0xFFFF);
    uint32_t chunk = std::min<uint32_t>(room, n - done);
    int64_t got = f.read(buf.data(), chunk);
    if (got < 0) return done ? int32_t(done) : -int(doserr::kAccessDenied);
    if (got) rt_.write_bytes(dst, buf.data(), uint32_t(got));
    done += uint32_t(got);
    dst = Runtime16::huge_add(dst, uint32_t(got));
    if (uint32_t(got) < chunk) break;  // the end of the file
  }
  return int32_t(done);
}

int32_t DosFiles::write(uint16_t h, uint32_t fp, uint32_t n) {
  win32::VfsFile* f = nullptr;
  if (h >= 5) {
    auto it = files_.find(h);
    if (it == files_.end()) return -int(doserr::kInvalidHandle);
    f = it->second.f.get();
    if (!f->writable()) return -int(doserr::kAccessDenied);
    if (n == 0) return f->truncate() ? 0 : -int(doserr::kAccessDenied);  // a zero-length write truncates here
  }
  std::vector<uint8_t> buf(std::min<uint32_t>(n, 0x10000));
  uint32_t src = fp, done = 0;
  while (done < n) {
    uint32_t room = 0x10000 - (src & 0xFFFF);
    uint32_t chunk = std::min(room, n - done);
    rt_.read_bytes(src, buf.data(), chunk);
    if (h == 1 || h == 2) {
      dos_write_console(rt_, std::string(buf.begin(), buf.begin() + chunk));
    } else if (f) {
      int64_t put = f->write(buf.data(), chunk);
      if (put < 0) return done ? int32_t(done) : -int(doserr::kAccessDenied);
      done += uint32_t(put);
      if (uint32_t(put) < chunk) break;  // the disk is full (Vfs::kMaxFileSize): fewer bytes, no error
      src = Runtime16::huge_add(src, chunk);
      continue;
    }
    done += chunk;
    src = Runtime16::huge_add(src, chunk);
  }
  return int32_t(done);
}

int64_t DosFiles::seek(uint16_t h, int32_t off, int whence) {
  auto it = files_.find(h);
  if (it == files_.end()) return h < 5 ? 0 : -int(doserr::kInvalidHandle);
  if (whence < 0 || whence > 2) return -int(doserr::kInvalidFunction);
  int64_t p = it->second.f->seek(off, whence);
  return p < 0 ? -int(doserr::kInvalidFunction) : p;
}

bool DosFiles::exists(const std::string& guest_path) {
  win32::Vfs::Stat st;
  return rt_.vfs().stat(guest_path, &st) && !st.dir;
}

bool DosFiles::is_dir(const std::string& guest_path) { return rt_.vfs().is_dir(guest_path); }

int DosFiles::remove(const std::string& guest_path) {
  uint32_t err = 0;
  if (rt_.vfs().remove(guest_path, &err)) return 0;
  trace("file16", "delete %s refused: error %u", rt_.vfs().full_path(guest_path).c_str(), err);
  return -int(dos_error(err));
}

int DosFiles::rename(const std::string& from, const std::string& to) {
  uint32_t err = 0;
  return rt_.vfs().rename(from, to, &err) ? 0 : -int(dos_error(err));
}

int DosFiles::make_dir(const std::string& guest_path) {
  uint32_t err = 0;
  return rt_.vfs().make_dir(guest_path, &err) ? 0 : -int(dos_error(err));
}

int DosFiles::remove_dir(const std::string& guest_path) {
  uint32_t err = 0;
  return rt_.vfs().remove_dir(guest_path, &err) ? 0 : -int(dos_error(err));
}

std::vector<DosFiles::Found> DosFiles::list(const std::string& guest_pattern, uint8_t attr_mask) {
  std::vector<Found> out;
  std::string full = rt_.vfs().full_path(guest_pattern);
  size_t slash = full.find_last_of('\\');
  std::string dir = slash == std::string::npos ? full : full.substr(0, slash);
  std::string pat = slash == std::string::npos ? "*.*" : full.substr(slash + 1);
  if (dir.size() == 2) dir += '\\';
  for (const win32::Vfs::DirEntry& e : rt_.vfs().list(dir, "*")) {
    std::string name = upper(e.short_name.empty() ? e.name : e.short_name);
    if (name == "." || name == "..") continue;
    bool is_dir = e.attributes & FILE_ATTRIBUTE_DIRECTORY;
    if (is_dir && !(attr_mask & 0x10)) continue;
    if (!wild_match(pat, name)) continue;
    out.push_back(Found{name, uint32_t(e.size), uint8_t((is_dir ? 0x10 : 0) | 0x20)});
  }
  std::sort(out.begin(), out.end(), [](const Found& a, const Found& b) { return a.name < b.name; });
  return out;
}

win32::IniStore& profiles16(Runtime16& rt) {
  struct Profiles16 : RuntimeState16 {
    explicit Profiles16(Runtime16& rt) : store(rt.vfs()) {}
    win32::IniStore store;
  };
  return rt.state<Profiles16>().store;
}

int dos_chdir(Runtime16& rt, const std::string& path, bool select_drive) {
  win32::Vfs& vfs = rt.vfs();
  std::string full = path.empty() ? std::string() : vfs.full_path(path);
  const char* why = path.empty()                                     ? "an empty path"
                    : path.find_first_of("*?") != std::string::npos ? "a wildcard"
                    : full.size() > kMaxCurDir                      ? "longer than DOS's current directory"
                    : !vfs.is_dir(full)                             ? "not a directory"
                                                                    : nullptr;
  if (!why && (select_drive ? vfs.set_cwd(full) : vfs.set_drive_cwd(full))) return 0;
  trace("dos", "chdir %s refused: %s", full.empty() ? path.c_str() : full.c_str(), why ? why : "not a directory");
  return -int(doserr::kPathNotFound);
}

void dos_write_console(Runtime16& rt, const std::string& text) {
  DosFiles& d = rt.state<DosFiles>();
  for (char c : text) {
    if (c == '\n' || d.line_.size() >= 1024) {
      log("win16 guest console: %s", d.line_.c_str());
      d.line_.clear();
    }
    if (c != '\n' && c != '\r') d.line_.push_back(c);
  }
}

// ---- INT 21h -------------------------------------------------------------------------------------------

namespace {

void fill_dta(Runtime16& rt, uint32_t dta, uint16_t slot, uint16_t next, const DosFiles::Found& f) {
  rt.wr16(dta + 0, slot);
  rt.wr16(dta + 2, next);
  rt.wr8(dta + 0x15, f.attr);
  rt.wr16(dta + 0x16, 0x6000);  // 12:00:00
  rt.wr16(dta + 0x18, uint16_t(((1996 - 1980) << 9) | (9 << 5) | 12));
  rt.wr32(dta + 0x1A, f.size);
  char name[13] = {};
  memcpy(name, f.name.data(), std::min<size_t>(f.name.size(), 12));
  rt.write_bytes(dta + 0x1E, name, 13);
}

}  // namespace

void dos_int21(Runtime16& rt) {
  auto& cpu = rt.cpu();
  auto& r = cpu.registers();
  DosState& st = ds_state(rt);
  DosFiles& files = rt.state<DosFiles>();
  uint8_t ah = r.r_ah(), al = r.r_al();
  auto ds_dx = [&] { return farp(seg(rt, SegReg::DS), r.r_dx()); };
  auto ok = [&] { rt.set_carry(false); };
  auto fail = [&](uint16_t code) {
    st.last_error = code;
    r.w_ax(code);
    rt.set_carry(true);
  };
  if (!st.dta) st.dta = farp(rt.sys_sel(), layout::kSysDta);
  if (tracing("dos")) {
    std::string path;
    if (ah == 0x3C || ah == 0x3D || ah == 0x41 || ah == 0x43 || ah == 0x4E || ah == 0x5B || ah == 0x3B || ah == 0x39 ||
        ah == 0x3A || ah == 0x56)
      path = rt.read_str(ds_dx());
    trace("dos", "INT 21h AX=%04X BX=%04X CX=%04X%s%s", r.r_ax(), r.r_bx(), r.r_cx(), path.empty() ? "" : " ",
          path.c_str());
  }

  switch (ah) {
    case 0x02:  // display character
      dos_write_console(rt, std::string(1, char(r.r_dl())));
      break;
    case 0x06:  // direct console I/O: no input waiting
    case 0x07:
    case 0x08:
      r.w_al(0);
      r.replace_flag(Regs::ZF, true);
      break;
    case 0x09: {  // display $-terminated string
      std::string s;
      uint32_t p = ds_dx();
      for (int i = 0; i < 4096; i++) {
        char c = char(rt.rd8(p + uint32_t(i)));
        if (c == '$') break;
        s.push_back(c);
      }
      dos_write_console(rt, s);
      break;
    }
    case 0x0B:  // check stdin: nothing
      r.w_al(0);
      break;
    case 0x0E:  // select disk DL (0 = A:) when the guest's disk has it; AL = the drive letters, A: to H:
      if (r.r_dl() < 26) rt.vfs().set_drive(char('A' + r.r_dl()));
      r.w_al(kLastDrive);
      break;
    case 0x19:  // current disk (0 = A:): the current directory's
      r.w_al(uint8_t(rt.vfs().cwd()[0] - 'A'));
      break;
    case 0x1A:
      st.dta = ds_dx();
      break;
    case 0x25:  // set vector: recorded (our INT handlers stay in charge)
      st.vectors[al] = ds_dx();
      trace("dos", "INT 21h AH=25h: vector %02Xh -> %04X:%04X (recorded)", al, seg(rt, SegReg::DS), r.r_dx());
      break;
    case 0x2A: {
      SYSTEMTIME t = now_local(rt);
      r.w_cx(t.wYear);
      r.w_dh(uint8_t(t.wMonth));
      r.w_dl(uint8_t(t.wDay));
      r.w_al(uint8_t(t.wDayOfWeek));
      break;
    }
    case 0x2C: {
      SYSTEMTIME t = now_local(rt);
      r.w_ch(uint8_t(t.wHour));
      r.w_cl(uint8_t(t.wMinute));
      r.w_dh(uint8_t(t.wSecond));
      r.w_dl(uint8_t(t.wMilliseconds / 10));
      break;
    }
    case 0x2B:  // set date / time: refused (AL = FF)
    case 0x2D:
      r.w_al(0xFF);
      break;
    case 0x2F: {  // get DTA
      cpu.load_segment(SegReg::ES, uint16_t(st.dta >> 16));
      r.w_bx(uint16_t(st.dta));
      break;
    }
    case 0x30:  // DOS 7.00 (Windows 95)
      r.w_ax(0x0007);
      r.w_bx(0xFF00);
      r.w_cx(0);
      break;
    case 0x33:  // Ctrl-Break state
      if (al == 0) r.w_dl(0);
      else if (al == 5) r.w_dl(3);  // boot drive C:
      break;
    case 0x35: {  // get vector
      auto it = st.vectors.find(al);
      uint32_t v = it != st.vectors.end() ? it->second : 0;
      if (!v) {
        if (!st.iret_off) {
          // An IRET: chaining to the "previous" handler returns to the caller.
          st.iret_off = rt.shims().internal_thunk("iret", [](Call16& c) {
            auto& rr = c.rt.cpu().registers();
            uint16_t ip = c.ret_ip(), cs = c.ret_cs();
            uint16_t fl = c.stack16(0);
            rr.w_sp(uint16_t(c.sp + 6));
            rr.write_eflags((rr.read_eflags() & ~0x0CD5u) | (fl & 0x0CD5u));
            c.rt.cpu().set_cs_eip(cs, ip);
            c.take_over();
          });
        }
        v = farp(rt.thunk_sel(), st.iret_off);
      }
      cpu.load_segment(SegReg::ES, uint16_t(v >> 16));
      r.w_bx(uint16_t(v));
      break;
    }
    case 0x36:  // free disk space: plenty
      r.w_ax(8);
      r.w_bx(0x7FFF);
      r.w_cx(512);
      r.w_dx(0xFFFF);
      break;
    case 0x3B: {  // chdir: its drive's current directory (the current drive stays)
      int e = dos_chdir(rt, rt.read_str(ds_dx()), /*select_drive=*/false);
      if (e < 0) fail(uint16_t(-e));
      else ok();
      break;
    }
    case 0x39:  // mkdir
    case 0x3A: {  // rmdir
      std::string path = rt.read_str(ds_dx());
      int e = ah == 0x39 ? files.make_dir(path) : files.remove_dir(path);
      if (e < 0) fail(uint16_t(-e));
      else ok();
      break;
    }
    case 0x3C:  // create
    case 0x5B: {  // create new
      int h = files.open(rt.read_str(ds_dx()), 2, true, ah == 0x5B);
      if (h < 0) fail(uint16_t(-h));
      else {
        r.w_ax(uint16_t(h));
        ok();
      }
      break;
    }
    case 0x3D: {  // open
      int h = files.open(rt.read_str(ds_dx()), al & 3, false);
      if (h < 0) fail(uint16_t(-h));
      else {
        r.w_ax(uint16_t(h));
        ok();
      }
      break;
    }
    case 0x3E: {
      int e = files.close(r.r_bx());
      if (e < 0) fail(uint16_t(-e));
      else ok();
      break;
    }
    case 0x3F: {
      int32_t n = files.read(r.r_bx(), ds_dx(), r.r_cx());
      if (n < 0) fail(uint16_t(-n));
      else {
        r.w_ax(uint16_t(n));
        ok();
      }
      break;
    }
    case 0x40: {
      int32_t n = files.write(r.r_bx(), ds_dx(), r.r_cx());
      if (n < 0) fail(uint16_t(-n));
      else {
        r.w_ax(uint16_t(n));
        ok();
      }
      break;
    }
    case 0x41: {
      int e = files.remove(rt.read_str(ds_dx()));
      if (e < 0) fail(uint16_t(-e));
      else ok();
      break;
    }
    case 0x42: {
      int32_t off = int32_t((uint32_t(r.r_cx()) << 16) | r.r_dx());
      int64_t p = files.seek(r.r_bx(), off, al);
      if (p < 0) fail(uint16_t(-p));
      else {
        r.w_ax(uint16_t(p));
        r.w_dx(uint16_t(p >> 16));
        ok();
      }
      break;
    }
    case 0x43: {  // attributes
      std::string path = rt.read_str(ds_dx());
      win32::Vfs::Stat stt;
      if (!rt.vfs().stat(path, &stt)) {
        fail(doserr::kFileNotFound);
      } else if (al == 0) {
        r.w_cx(stt.dir ? 0x10 : uint16_t(0x20 | (stt.writable ? 0 : 0x01)));
        ok();
      } else if (al == 1 && !stt.dir && stt.writable) {
        ok();  // set: the overlay keeps no attributes; accepted where it could write
      } else {
        fail(doserr::kAccessDenied);
      }
      break;
    }
    case 0x44:  // IOCTL
      switch (al) {
        case 0x00:
          if (!files.valid(r.r_bx())) {
            fail(doserr::kInvalidHandle);
            return;
          }
          r.w_dx(files.is_device(r.r_bx()) ? 0x80D3 : 0x0002);
          ok();
          break;
        case 0x01:
          ok();
          break;
        case 0x08:  // removable? fixed
          r.w_ax(1);
          ok();
          break;
        case 0x09:  // remote? local
          r.w_dx(0);
          ok();
          break;
        case 0x0E:
          r.w_al(0);
          ok();
          break;
        default:
          fail(doserr::kInvalidFunction);
      }
      break;
    case 0x45: {
      int h = files.dup(r.r_bx());
      if (h < 0) fail(uint16_t(-h));
      else {
        r.w_ax(uint16_t(h));
        ok();
      }
      break;
    }
    case 0x47: {  // current directory of drive DL (0: the current drive, 1: A:), without "X:\"
      win32::Vfs& vfs = rt.vfs();
      char letter = r.r_dl() ? char('A' + r.r_dl() - 1) : vfs.cwd()[0];
      std::string dir = r.r_dl() <= 26 ? vfs.drive_cwd(letter) : std::string();
      // Whole, never cut: dos_chdir kept it within DOS's 64 bytes.
      if (dir.size() < 3 || dir.size() > kMaxCurDir || !vfs.is_dir(dir.substr(0, 3))) {
        fail(doserr::kInvalidDrive);
        break;
      }
      std::string rel = dir.substr(3);
      rt.write_str(farp(seg(rt, SegReg::DS), r.r_si()), rel, rel.size() + 1);
      r.w_ax(0x0100);
      ok();
      break;
    }
    case 0x48:  // DOS memory: none in protected mode
      r.w_bx(0);
      fail(8);
      break;
    case 0x49:
    case 0x4A:
      fail(9);
      break;
    case 0x4C:
      throw GuestError16(GuestError16::Kind::exit, "INT 21h AH=4Ch: the guest exited with code " + std::to_string(al));
    case 0x4E: {  // find first
      std::string pat = rt.read_str(ds_dx());
      auto found = files.list(pat, uint8_t(r.r_cx()));
      if (found.empty()) {
        fail(doserr::kNoMoreFiles);
        break;
      }
      st.finds.push_back(std::move(found));
      uint16_t slot = uint16_t(st.finds.size() - 1);
      fill_dta(rt, st.dta, slot, 1, st.finds[slot][0]);
      r.w_ax(0);
      ok();
      break;
    }
    case 0x4F: {  // find next
      uint16_t slot = rt.rd16(st.dta), next = rt.rd16(st.dta + 2);
      if (slot >= st.finds.size() || next >= st.finds[slot].size()) {
        fail(doserr::kNoMoreFiles);
        break;
      }
      fill_dta(rt, st.dta, slot, uint16_t(next + 1), st.finds[slot][next]);
      r.w_ax(0);
      ok();
      break;
    }
    case 0x50:  // set PSP
      break;
    case 0x51:  // get PSP
    case 0x62:
      r.w_bx(dos16_psp(rt));
      break;
    case 0x56: {  // rename DS:DX → ES:DI
      std::string from = rt.read_str(ds_dx());
      std::string to = rt.read_str(farp(seg(rt, SegReg::ES), r.r_di()));
      int e = files.rename(from, to);
      if (e < 0) fail(uint16_t(-e));
      else ok();
      break;
    }
    case 0x57:  // file date/time
      if (al == 0) {
        r.w_cx(0x6000);
        r.w_dx(uint16_t(((1996 - 1980) << 9) | (9 << 5) | 12));
      }
      ok();
      break;
    case 0x59:  // extended error
      r.w_ax(st.last_error);
      r.w_bx(0x0101);
      r.w_ch(1);
      break;
    case 0x67:  // set handle count
      ok();
      break;
    case 0x6C: {  // extended open/create
      uint32_t name = farp(seg(rt, SegReg::DS), r.r_si());
      uint16_t action = r.r_dx();
      std::string path = rt.read_str(name);
      bool exists = files.exists(path);
      int h;
      uint16_t result;
      if (exists && (action & 0x0F) == 0) {
        fail(80);  // file exists
        break;
      }
      if (!exists && (action & 0xF0) == 0) {
        fail(doserr::kFileNotFound);
        break;
      }
      if (exists && (action & 0x0F) == 2) {
        h = files.open(path, 2, true);  // replace
        result = 3;
      } else if (exists) {
        h = files.open(path, r.r_bx() & 3, false);
        result = 1;
      } else {
        h = files.open(path, 2, true);
        result = 2;
      }
      if (h < 0) fail(uint16_t(-h));
      else {
        r.w_ax(uint16_t(h));
        r.w_cx(result);
        ok();
      }
      break;
    }
    case 0x71:  // long file names: not supported (DOS 7 without the LFN API)
      fail(0x7100);
      break;
    default:
      if (!st.reported[ah]) {
        st.reported[ah] = true;
        log("win16: INT 21h AH=%02Xh (AX=%04X) not supported", ah, r.r_ax());
      }
      fail(doserr::kInvalidFunction);
      break;
  }
}

// ---- the other interrupts ---------------------------------------------------------------------------

namespace {

void bios_int1a(Runtime16& rt) {
  auto& r = rt.cpu().registers();
  switch (r.r_ah()) {
    case 0x00: {  // ticks since midnight
      rt.update_bios_ticks();
      uint32_t t = rt.mem().read_u32l(layout::kBdaBase + 0x6C);
      r.w_cx(uint16_t(t >> 16));
      r.w_dx(uint16_t(t));
      r.w_al(0);
      break;
    }
    case 0x02: {  // RTC time, BCD
      SYSTEMTIME t = now_local(rt);
      r.w_ch(bcd(t.wHour));
      r.w_cl(bcd(t.wMinute));
      r.w_dh(bcd(t.wSecond));
      r.w_dl(0);
      rt.set_carry(false);
      break;
    }
    case 0x04: {  // RTC date, BCD
      SYSTEMTIME t = now_local(rt);
      r.w_ch(bcd(t.wYear / 100));
      r.w_cl(bcd(t.wYear % 100));
      r.w_dh(bcd(t.wMonth));
      r.w_dl(bcd(t.wDay));
      rt.set_carry(false);
      break;
    }
    default:
      rt.set_carry(true);
      break;
  }
}

void multiplex_int2f(Runtime16& rt) {
  auto& r = rt.cpu().registers();
  uint16_t ax = r.r_ax();
  switch (ax) {
    case 0x1600:  // enhanced-mode Windows installed: 4.00
      r.w_ax(0x0004);
      break;
    case 0x160A:  // Windows version: 4.00, enhanced mode
      r.w_ax(0);
      r.w_bx(0x0400);
      r.w_cx(3);
      break;
    case 0x1500:  // MSCDEX: no CD-ROM drives (SLIDE.AD's probe)
      r.w_bx(0);
      break;
    case 0x150B:  // MSCDEX drive check: not a CD-ROM, MSCDEX absent
      r.w_ax(0);
      r.w_bx(0);
      break;
    case 0x1686:  // DPMI: in protected mode
      r.w_ax(0);
      break;
    case 0x1684:  // a VxD's API entry point (BX = its id): none here, ES:DI = 0:0 —
      // what Windows answered for a VxD that is not loaded. INTRMLIB's LibEntry
      // keeps three of them (INTRMLIB 6:0092..6:00F5) for its sound engines.
      rt.cpu().set_segment_null(SegReg::ES);
      r.w_di(0);
      break;
    default:
      break;  // not installed: AL unchanged
  }
}

// INT 25h/26h: absolute disk read/write — refused. DOS returns with the
// caller's flags still pushed; the caller pops them.
void disk_int25(Runtime16& rt) {
  auto& cpu = rt.cpu();
  auto& r = cpu.registers();
  rt.set_carry(true);
  r.w_ax(0x0802);  // sector not found
  const cpu::SegDesc& ss = cpu.get_segment_desc(SegReg::SS);
  uint16_t sp = uint16_t(r.r_sp() - 2);
  rt.mem().write_u16l(ss.base + sp, uint16_t(r.read_eflags()));
  r.w_sp(sp);
}

void video_int10(Runtime16& rt) {
  auto& cpu = rt.cpu();
  auto& r = cpu.registers();
  win32::Display* d = rt.display();
  auto es_dx = [&] { return farp(seg(rt, SegReg::ES), r.r_dx()); };
  auto six = [](uint8_t c) { return uint8_t(c >> 2); };
  auto eight = [](uint8_t c) { return BYTE(((c & 0x3F) << 2) | ((c & 0x3F) >> 4)); };
  switch (r.r_ah()) {
    case 0x0F:  // current mode: 640x480x256 (a VESA-ish mode number; nobody checks)
      r.w_al(0x13);
      r.w_ah(80);
      r.w_bh(0);
      return;
    case 0x10:
      switch (r.r_al()) {
        case 0x09: {  // read all 16 palette registers + overscan
          uint8_t regs[17];
          for (int i = 0; i < 16; i++) regs[i] = uint8_t(i);
          regs[16] = 0;
          rt.write_bytes(es_dx(), regs, sizeof(regs));
          return;
        }
        case 0x10:  // set one DAC register
          if (d) {
            PALETTEENTRY e{eight(r.r_dh()), eight(r.r_ch()), eight(r.r_cl()), 0};
            d->set_system_entries(r.r_bx() & 0xFF, 1, &e);
          }
          return;
        case 0x12: {  // set a block of DAC registers
          uint16_t first = r.r_bx(), n = r.r_cx();
          for (uint16_t i = 0; i < n && first + i < 256 && d; i++) {
            uint32_t p = es_dx() + 3u * i;
            PALETTEENTRY e{eight(rt.rd8(p)), eight(rt.rd8(p + 1)), eight(rt.rd8(p + 2)), 0};
            d->set_system_entries(first + i, 1, &e);
          }
          return;
        }
        case 0x15:  // read one DAC register
          if (d) {
            const PALETTEENTRY& e = d->system_palette()[r.r_bx() & 0xFF];
            r.w_dh(six(e.peRed));
            r.w_ch(six(e.peGreen));
            r.w_cl(six(e.peBlue));
          }
          return;
        case 0x17: {  // read a block of DAC registers
          uint16_t first = r.r_bx(), n = r.r_cx();
          for (uint16_t i = 0; i < n && first + i < 256 && d; i++) {
            const PALETTEENTRY& e = d->system_palette()[first + i];
            uint32_t p = es_dx() + 3u * i;
            rt.wr8(p, six(e.peRed));
            rt.wr8(p + 1, six(e.peGreen));
            rt.wr8(p + 2, six(e.peBlue));
          }
          return;
        }
        default:
          return;
      }
    default:
      return;
  }
}

void keyboard_int16(Runtime16& rt) {
  auto& r = rt.cpu().registers();
  switch (r.r_ah()) {
    case 0x01:
    case 0x11:
      r.replace_flag(Regs::ZF, true);  // no key waiting
      break;
    case 0x02:
    case 0x12:
      r.w_al(0);
      break;
    default:
      r.w_ax(0);
      break;
  }
}

// DPMI descriptor services (INT 31h) over our LDT.
void dpmi_int31(Runtime16& rt) {
  auto& cpu = rt.cpu();
  auto& r = cpu.registers();
  Ldt& ldt = rt.ldt();
  auto ok = [&] { rt.set_carry(false); };
  auto fail = [&](uint16_t code) {
    r.w_ax(code);
    rt.set_carry(true);
  };
  uint16_t bx = r.r_bx();
  switch (r.r_ax()) {
    case 0x0000: {
      uint16_t n = std::max<uint16_t>(r.r_cx(), 1);
      uint16_t s = ldt.alloc(n);
      if (!s) return fail(0x8011);
      r.w_ax(s);
      return ok();
    }
    case 0x0001:
      if (!ldt.in_use(bx)) return fail(0x8022);
      ldt.free(bx, 1);
      return ok();
    case 0x0002:
      if (bx == 0x40) {
        r.w_ax(Ldt::kBiosSelector);
        return ok();
      }
      return fail(0x8011);
    case 0x0003:
      r.w_ax(Ldt::kAhIncr);
      return ok();
    case 0x0006: {
      const cpu::SegDesc* d = ldt.get(bx);
      if (!d) return fail(0x8022);
      r.w_cx(uint16_t(d->base >> 16));
      r.w_dx(uint16_t(d->base));
      return ok();
    }
    case 0x0007:
    case 0x0008:
    case 0x0009: {
      const cpu::SegDesc* d = ldt.get(bx);
      if (!d || !Ldt::is_ldt(bx)) return fail(0x8022);
      cpu::SegDesc nd = *d;
      uint32_t v = (uint32_t(r.r_cx()) << 16) | r.r_dx();
      if (r.r_ax() == 0x0007) nd.base = v;
      else if (r.r_ax() == 0x0008) nd.limit = v;
      else {
        uint8_t acc = r.r_cl(), ext = r.r_ch();
        nd.present = acc & 0x80;
        nd.code = acc & 0x08;
        nd.readable_or_writable = acc & 0x02;
        nd.big = ext & 0x40;
      }
      ldt.set(bx, nd);
      cpu.reload_segments();
      return ok();
    }
    case 0x000A: {
      const cpu::SegDesc* d = ldt.get(bx);
      if (!d) return fail(0x8022);
      cpu::SegDesc nd = *d;
      nd.code = false;
      nd.readable_or_writable = true;
      uint16_t s = ldt.alloc(1);
      if (!s) return fail(0x8011);
      ldt.set(s, nd);
      r.w_ax(s);
      return ok();
    }
    case 0x000B: {
      const cpu::SegDesc* d = ldt.get(bx);
      if (!d) return fail(0x8022);
      uint32_t lim = d->limit;
      bool gran = lim > 0xFFFFF;
      if (gran) lim >>= 12;
      uint8_t raw[8];
      raw[0] = uint8_t(lim);
      raw[1] = uint8_t(lim >> 8);
      raw[2] = uint8_t(d->base);
      raw[3] = uint8_t(d->base >> 8);
      raw[4] = uint8_t(d->base >> 16);
      raw[5] = uint8_t((d->present ? 0x80 : 0) | 0x60 | 0x10 | (d->code ? 0x08 : 0) | (d->readable_or_writable ? 2 : 0));
      raw[6] = uint8_t(((lim >> 16) & 0x0F) | (d->big ? 0x40 : 0) | (gran ? 0x80 : 0));
      raw[7] = uint8_t(d->base >> 24);
      rt.write_bytes(farp(seg(rt, SegReg::ES), r.r_di()), raw, 8);
      return ok();
    }
    case 0x000C: {
      if (!ldt.in_use(bx)) return fail(0x8022);
      uint8_t raw[8];
      rt.read_bytes(farp(seg(rt, SegReg::ES), r.r_di()), raw, 8);
      cpu::SegDesc nd;
      nd.limit = raw[0] | (raw[1] << 8) | ((raw[6] & 0x0F) << 16);
      if (raw[6] & 0x80) nd.limit = (nd.limit << 12) | 0xFFF;
      nd.base = raw[2] | (raw[3] << 8) | (raw[4] << 16) | (uint32_t(raw[7]) << 24);
      nd.present = raw[5] & 0x80;
      nd.code = raw[5] & 0x08;
      nd.readable_or_writable = raw[5] & 0x02;
      nd.big = raw[6] & 0x40;
      nd.dpl = 3;
      ldt.set(bx, nd);
      cpu.reload_segments();
      return ok();
    }
    case 0x0400:  // DPMI 0.90, 386
      r.w_ax(0x005A);
      r.w_bx(0x0005);
      r.w_cl(4);
      r.w_dx(0x0870);
      return ok();
    case 0x0500: {  // free memory information: a comfortable machine
      uint32_t info = farp(seg(rt, SegReg::ES), r.r_di());
      for (int i = 0; i < 12; i++) rt.wr32(info + 4u * i, 0xFFFFFFFF);
      rt.wr32(info, 16u << 20);
      return ok();
    }
    case 0x0600:
    case 0x0601:
    case 0x0702:
    case 0x0703:
      return ok();
    default:
      return fail(0x8001);
  }
}

}  // namespace

void seed_modules_ini(Runtime16& rt) {
  const Runtime16Options& o = rt.options();
  DosFiles& files = rt.state<DosFiles>();
  win32::IniStore& ini = profiles16(rt);
  const std::string& ad = o.guest_dir;
  std::string ray = ad + "\\TRACES\\ROTCUBE.TRC";
  for (const char* scene : {"ROTCUBE.TRC", "DIAMOND.TRC", "ROTPYRA.TRC"}) {
    std::string p = ad + "\\TRACES\\" + scene;
    if (files.exists(p)) {
      ray = p;
      break;
    }
  }
  std::string path = o.windows_dir + "\\MODULES.INI";
  ini.add_seed(path, "The Artist", "Image", ad + "\\BITMAPS\\ADLOGO.BMP");
  ini.add_seed(path, "Ray", "RaySceneFile", ray);
  ini.add_seed(path, "Slide Show", "CatalogName", "BITMAPS");
  ini.add_seed(path, "Logo Section", "LogoFile", ad + "\\BITMAPS\\ADLOGO.BMP");
}

void seed_intermission(Runtime16& rt, const IntermissionSeeds& seeds) {
  const Runtime16Options& o = rt.options();
  win32::IniStore& ini = profiles16(rt);
  const char* kDisplayDriver = "pnpdrvr.drv";  // Windows 95's Plug and Play display driver
  ini.add_seed(o.windows_dir + "\\SYSTEM.INI", "boot", "display.drv", kDisplayDriver);
  if (seeds.swse_gdi) {
    std::string swse = o.windows_dir + "\\SWSE.INI";
    ini.add_seed(swse, "technology", "display.drv", kDisplayDriver);
    ini.add_seed(swse, "technology", "WinGFound", "1");
    ini.add_seed(swse, "technology", "DibBlit", "GDI");
  }
  std::string antsw = o.windows_dir + "\\ANTSW.INI";
  ini.add_seed(antsw, "Intermission", "Volume", std::to_string(std::clamp(seeds.volume, 0, 100)));
  ini.add_seed(antsw, "Intermission", "Saver Path", seeds.saver_path.empty() ? o.guest_dir : seeds.saver_path);
}

void seed_scrnsave(Runtime16& rt, const ScrnsaveSeeds& seeds) {
  const Runtime16Options& o = rt.options();
  win32::IniStore& ini = profiles16(rt);
  const std::string win = o.windows_dir + "\\WIN.INI";
  ini.add_seed(win, "Windows", "ScreenSaveActive", "1");
  ini.add_seed(win, "Windows", "ScreenSaveTimeOut", "120");
  if (!seeds.program.empty()) ini.add_seed(o.windows_dir + "\\SYSTEM.INI", "boot", "SCRNSAVE.EXE", seeds.program);
  if (!seeds.source_dir.empty()) {
    ini.add_seed(o.windows_dir + "\\SCRANTIC.INI", "ScreenSaver.ScreenAntics", "SourceDir", seeds.source_dir);
  }
}

void seed_after_dark2(Runtime16& rt) {
  const Runtime16Options& o = rt.options();
  win32::IniStore& ini = profiles16(rt);
  std::string prefs = o.windows_dir + "\\AD_PREFS.INI";
  // The installer's own spelling of the directory: with its backslash (the
  // modules append ST_RES\ and SOUNDS\ to it, AD_SND the driver's name).
  ini.add_seed(prefs, "After Dark", "Path", o.guest_dir + "\\");
  ini.add_seed(prefs, "Sound", "SoundDriver", "AD_MME.DRV");
}

void seed_after_dark3(Runtime16& rt) {
  const Runtime16Options& o = rt.options();
  win32::IniStore& ini = profiles16(rt);
  std::string prefs = o.windows_dir + "\\AD_PREFS.INI";
  // ADW30's own spelling of its directory: no backslash (ADXPL100 adds one,
  // 4:014E, before it appends DIS_SND.DLL and music\).
  ini.add_seed(prefs, "After Dark", "Path", o.guest_dir);
  ini.add_seed(prefs, "Sound", "SoundDriver", "AD_MME.DRV");
}

std::string progman_group_file(const std::string& name) {
  // GROUPHEADER (Windows 3.1): "PMCC", wCheckSum, cbGroup, nCmdShow,
  // rcNormal, ptMin, pName (0x16), wLogPixelsX/Y, bBitsPerPixel, bPlanes,
  // wReserved, cItems (0x20); the name follows (no items).
  std::string g(0x22, '\0');
  auto put = [&](size_t at, uint16_t v) {
    g[at] = char(v);
    g[at + 1] = char(v >> 8);
  };
  memcpy(g.data(), "PMCC", 4);
  put(0x08, 1);                                    // SW_SHOWNORMAL
  put(0x0A, 20), put(0x0C, 40), put(0x0E, 420), put(0x10, 280);  // rcNormal
  put(0x12, 0xFFFF), put(0x14, 0xFFFF);            // ptMin: never minimized
  put(0x16, 0x22);                                 // pName
  put(0x18, 96), put(0x1A, 96);                    // logical pixels per inch
  g[0x1C] = 8, g[0x1D] = 1;                        // 8 bits per pixel, 1 plane
  g += name;
  g.push_back('\0');
  if (g.size() & 1) g.push_back('\0');
  put(0x06, uint16_t(g.size()));  // cbGroup
  uint16_t sum = 0;
  for (size_t i = 0; i < g.size(); i += 2) sum = uint16_t(sum + (uint8_t(g[i]) | (uint8_t(g[i + 1]) << 8)));
  put(0x04, uint16_t(-sum));  // the words of the file sum to 0
  return g;
}

void seed_program_manager(Runtime16& rt) {
  const Runtime16Options& o = rt.options();
  win32::Vfs& vfs = rt.vfs();
  std::string ini_path = o.windows_dir + "\\PROGMAN.INI";
  if (vfs.exists(ini_path)) return;
  static const struct {
    const char* file;
    const char* name;
  } kGroups[] = {{"MAIN.GRP", "Main"},
                 {"ACCESSOR.GRP", "Accessories"},
                 {"GAMES.GRP", "Games"},
                 {"STARTUP.GRP", "StartUp"},
                 {"AFTERDRK.GRP", "After Dark"}};
  std::string ini = "[Settings]\r\nWindow=40 30 440 330 1\r\nSaveSettings=1\r\nMinOnRun=0\r\nAutoArrange=1\r\n[Groups]\r\n";
  int n = 0;
  auto bytes = [](const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); };
  for (const auto& g : kGroups) {
    std::string path = o.windows_dir + "\\" + g.file;
    vfs.add_virtual_file(path, bytes(progman_group_file(g.name)));
    ini += "Group" + std::to_string(++n) + "=" + path + "\r\n";
  }
  vfs.add_virtual_file(ini_path, bytes(ini));
}

void register_dos(Runtime16& rt) {
  // The guest's disk: the directories After Dark's installer made and the
  // INI files it left in the AD INI directory (WIN.INI [Berkeley Systems]
  // "AD Ini Files" = the Windows directory): modules look for MODULES.INI
  // before they read their settings from it. The settings are what a fresh
  // install pointed the modules that show a picture or a scene at — the
  // sample data shipped beside them (our choice of samples: without one
  // each of these modules refuses to start) — as profile seeds (never
  // written out); the files themselves are empty virtual files. C:\WINDOWS
  // and its TEMP are in-memory overlays until the lane mounts its own
  // C:\WINDOWS (INTERACTION.md §7.2); TEMP stays in memory.
  const Runtime16Options& o = rt.options();
  win32::Vfs& vfs = rt.vfs();
  vfs.mount_overlay(o.windows_dir, "", "");
  vfs.mount_overlay(o.windows_dir + "\\TEMP", "", "");
  for (const char* f : {"MODULES.INI", "AD_PREFS.INI", "AFTERDRK.INI"}) vfs.add_virtual_file(o.windows_dir + "\\" + f, {});
  const std::string& ad = o.guest_dir;
  seed_modules_ini(rt);
  win32::IniStore& ini = profiles16(rt);
  ini.add_seed(o.windows_dir + "\\AD_PREFS.INI", "After Dark", "GlobeFile", ad + "\\BITMAPS\\EARTH.BMP");
  ini.add_seed(o.windows_dir + "\\WIN.INI", "Berkeley Systems", "AD Ini Files", o.windows_dir);
  ini.add_seed(o.windows_dir + "\\WIN.INI", "Berkeley Systems", "AD Data Files", ad);
  ini.add_seed(o.windows_dir + "\\WIN.INI", "Berkeley Systems", "After Dark", ad);

  rt.set_int_handler(0x21, dos_int21);
  rt.set_int_handler(0x1A, bios_int1a);
  rt.set_int_handler(0x2F, multiplex_int2f);
  rt.set_int_handler(0x25, disk_int25);
  rt.set_int_handler(0x26, disk_int25);
  rt.set_int_handler(0x10, video_int10);
  rt.set_int_handler(0x16, keyboard_int16);
  rt.set_int_handler(0x31, dpmi_int31);
  // INT 3 (DebugBreak builds, stray int3s): ignored, as a retail Windows did.
  rt.set_int_handler(0x03, [](Runtime16&) {});
  // The FP emulator vectors: only reachable with OSFIXUPs applied, which the
  // lane never does (the CPU runs the x87 opcodes, ABI.md §3.6).
  for (uint8_t v = 0x34; v <= 0x3D; v++) {
    rt.set_int_handler(v, [v](Runtime16&) {
      throw GuestError16(GuestError16::Kind::fatal,
                         "INT " + std::to_string(v) + "h: floating-point emulator call (OSFIXUPs were applied?)");
    });
  }
}

}  // namespace adw::win16
