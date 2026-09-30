// KERNEL — memory (global + local heaps), modules and resources, files,
// profile strings, strings, time-independent process services, Catch/Throw,
// SwitchStackTo/Back, DOS3Call (API_SURFACE.md §2 "KERNEL", 80 imports).
//
// Behaviour notes:
//   * GetVersion: Windows "3.95" on DOS 7.00 — what Win16 code saw on Windows
//     95; GetWinFlags: protected + enhanced mode, 486, x87.
//   * Profiles go through the shared store (win32/ini_store.hh, INTERACTION.md
//     §7.3): the seeds register_dos made (MODULES.INI's per-install settings,
//     WIN.INI's [Berkeley Systems], which points the AD data/INI directories
//     at the guest install directory, as the installer wrote it) under the
//     file; writes land in the upper layer of the file's overlay — the
//     per-user state with ADSTATE, else memory, so a headless run starts from
//     the same settings every time.
//   * MakeProcInstance returns the procedure itself: every callback a Classic
//     DLL hands out is an exported entry whose prolog loads DGROUP (prolog
//     patching, modules16.hh), which is also what Windows did for DLLs.
//   * Files go through DosFiles (dos16.hh) over the Vfs overlays: reads from
//     the upper or lower layer, writes copied up.
//   * GetCurrentTask: DX is the first task of the task list, this one
//     (NONSENSE walks the list for the Notepad it started). The synthetic
//     desktop's Program Manager belongs to another task (kernel16_shell_task,
//     GetWindowTask), which the list never shows.
//   * Resources are counted as Win16 counted them: LoadResource of a loaded
//     resource returns the same block and counts one more use, FreeResource
//     counts one less and frees the block at zero, and the next LoadResource
//     reads a fresh copy from the image (POSTERS appends to its locked
//     caption every frame and relies on that). AccessResource opens the
//     module file at the resource's data.
//   * GetTempFileName with uUnique 0 creates the (empty) file, as Windows did;
//     C:\WINDOWS\TEMP is an in-memory overlay.
//   * GlobalWire/GlobalUnWire are GlobalLock/GlobalUnlock (the arena never
//     moves a block, so there is nothing to move low).
//   * GetModuleHandle finds the system DLLs Windows 95 always has loaded
//     (KERNEL, USER, GDI, SYSTEM, KEYBOARD, DISPLAY, SOUND and MMSYSTEM, which
//     SYSTEM.INI's [boot] drivers= line loads) before anything imports them.
//   * GetHeapSpaces answers a fixed, healthy local heap (90% free, what
//     GetFreeSystemResources reports). The selector calls (AllocSelector,
//     FreeSelector, AllocCStoDSAlias, AllocDStoCSAlias, Get/SetSelectorBase
//     and Get/SetSelectorLimit) do what KRNL386's did, but free and change
//     only the selectors they made themselves.
//   * A call that frees a selector the caller holds in DS, ES, FS or GS
//     returns with that register null (the freed-selector rule, one place:
//     Runtime16::null_freed_segments).
//
// Known gaps, deliberately left (no module of the supported releases needs
// more; API_SURFACE.md §2 KERNEL):
//   * WinExec refuses (error 2), except "notepad <file>" in configure mode
//     (dialogs16.cc).
//   * SetHandleCount reports the task's file handle table size, but the DOS
//     handle table (dos16.hh) holds 250 files whatever it says.
#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <map>
#include <vector>

#include "adw/core/log.h"
#include "adw/core/text.h"
#include "win16/dos16.hh"
#include "win16/modules16.hh"
#include "win16/shim_families16.hh"
#include "win32/ini_store.hh"
#include "win32/vfs.hh"

namespace adw::win16 {

namespace {

constexpr const char* K = "KERNEL";
using SegReg = cpu::X86Emulator::SegReg;

constexpr uint16_t kWinFlags = 0x0001 | 0x0020 | 0x0008 | 0x0400;  // PMODE | ENHANCED | CPU486 | 80x87
constexpr uint16_t kHfileError = 0xFFFF;

// ---- profiles ---------------------------------------------------------------------------------------

std::string trim(std::string s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  size_t e = s.find_last_not_of(" \t\r\n");
  return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

struct KernelState : RuntimeState16 {
  explicit KernelState(Runtime16& rt) : rt(rt) {}
  Runtime16& rt;
  uint16_t error_mode = 0;
  uint16_t task = 0;
  uint16_t shell_task = 0;  // the synthetic desktop's (kernel16_shell_task)
  // SetHandleCount: the size of the task's file handle table (the PDB's
  // JFN length): Windows' default 20, grown up to 255.
  uint16_t handle_count = 20;
  // Resources: HRSRC = index + 1 into this table. Each row keeps its image
  // alive: the module may be freed (and another loaded at the same address)
  // while the guest still holds an HRSRC or a loaded copy.
  struct Res {
    std::shared_ptr<loader::ne::Image> image;
    std::string module;      // for traces
    std::string guest_path;  // the module's file (AccessResource opens it)
    const loader::ne::Resource* res;
    uint16_t hglobal = 0;    // the loaded copy, 0 while not loaded
    uint16_t usage = 0;      // LoadResource count (Win16's NE_NAMEINFO usage)
  };
  std::vector<Res> resources;
  // SwitchStackTo/Back.
  struct StackSwitch {
    uint16_t ss = 0, sp = 0, bp = 0, frame = 0;
  };
  std::vector<StackSwitch> stack_switches;

};

KernelState& ks(Runtime16& rt) { return rt.state<KernelState>(); }

// ---- selectors -----------------------------------------------------------------------------------------

// `n` new selectors with d's attributes, tiled as a huge block's are (tile i
// based i * 64 KiB on, its limit running to the end: KRNL386's
// Fill_In_Selector_Array), marked as the guest's own run (Ldt::mark_guest_run).
// 0 when the LDT has no room.
uint16_t alloc_own(Runtime16& rt, const SegDesc& d, uint32_t n, const char* tag) {
  if (!n || n >= Ldt::kEntries) return 0;
  uint16_t first = rt.ldt().alloc(uint16_t(n));
  if (!first) return 0;
  for (uint32_t i = 0; i < n; i++) {
    SegDesc t = d;
    t.base = d.base + (i << 16);
    t.limit = d.limit - (i << 16);
    uint16_t s = uint16_t(first + i * Ldt::kAhIncr);
    rt.ldt().set(s, t);
    rt.ldt().set_tag(s, tag);
  }
  rt.ldt().mark_guest_run(first, uint16_t(n));
  return first;
}

// A task database block: the handle only has to be a stable, valid selector.
uint16_t make_tdb(Runtime16& rt, const char* tag) {
  GlobalBlock* b = rt.global().alloc_block(0x200, false, 0, 0);
  if (!b) return 0;
  b->owner = 0xFFFF;
  rt.ldt().set_tag(b->sel, tag);
  rt.mem().write_u16l(b->base + 0xFA, 0x4454);  // 'TD' signature at TDB+0xFA
  return b->sel;
}

std::string profile_path(Runtime16& rt, const std::string& name) {
  if (name.find_first_of("\\/:") == std::string::npos) return upper16(rt.options().windows_dir + "\\" + name);
  return upper16(rt.vfs().full_path(name));
}

// Writes a NUL-separated, double-NUL-terminated list; returns the characters
// copied excluding the final NUL (size-2 when truncated).
uint16_t write_list(Runtime16& rt, uint32_t buf, uint16_t size, const std::vector<std::string>& items) {
  if (!buf || size < 2) {
    if (buf && size) rt.wr8(buf, 0);
    return 0;
  }
  std::string out;
  for (const std::string& s : items) {
    out += s;
    out.push_back('\0');
  }
  if (out.size() + 1 > size) {
    out.resize(size - 2);
    out.push_back('\0');
    rt.write_bytes(buf, out.data(), out.size());
    rt.wr8(buf + uint32_t(out.size()), 0);
    return uint16_t(size - 2);
  }
  if (out.empty()) out.push_back('\0');
  rt.write_bytes(buf, out.data(), out.size());
  rt.wr8(buf + uint32_t(out.size()), 0);
  return uint16_t(out.size() == 1 && items.empty() ? 0 : out.size() - 1);
}

// Profiles go through the shared store (win32/ini_store.hh): the seeds
// register_dos made (MODULES.INI's per-install settings, WIN.INI's [Berkeley
// Systems]) under the file, the file wins per key, writes land in the upper
// layer of the file's overlay (the per-user state, or memory).
uint16_t get_profile_string(Runtime16& rt, const std::string& file, uint32_t app, uint32_t key, uint32_t def,
                            uint32_t buf, uint16_t size) {
  win32::IniStore& ini = profiles16(rt);
  std::string path = profile_path(rt, file);
  if (!app) return write_list(rt, buf, size, ini.sections(path));
  std::string section = rt.read_str(app);
  if (!key) return write_list(rt, buf, size, ini.keys(path, section));
  std::string k = rt.read_str(key);
  std::optional<std::string> v = ini.get(path, section, k);
  bool found = v.has_value();
  std::string value = found ? *v : trim(rt.read_str(def));
  if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front()) {
    value = value.substr(1, value.size() - 2);
  }
  trace("file16", "GetProfileString %s [%s] %s -> \"%s\"%s", file.c_str(), section.c_str(), k.c_str(), value.c_str(),
        found ? "" : " (default)");
  if (!buf || !size) return 0;
  return uint16_t(rt.write_str(buf, value, size));
}

int32_t get_profile_int(Runtime16& rt, const std::string& file, uint32_t app, uint32_t key, int16_t def) {
  std::string section = rt.read_str(app), k = rt.read_str(key);
  trace("file16", "GetProfileInt %s [%s] %s (default %d)", file.c_str(), section.c_str(), k.c_str(), def);
  std::optional<std::string> v = profiles16(rt).get(profile_path(rt, file), section, k);
  if (!v) return def;
  // Leading digits only, as Windows parses it; a non-number reads as 0.
  int32_t n = 0;
  size_t i = 0;
  bool neg = false;
  if (i < v->size() && ((*v)[i] == '-' || (*v)[i] == '+')) neg = (*v)[i++] == '-';
  while (i < v->size() && isdigit(uint8_t((*v)[i]))) n = n * 10 + ((*v)[i++] - '0');
  return neg ? -n : n;
}

bool write_profile_string(Runtime16& rt, const std::string& file, uint32_t app, uint32_t key, uint32_t value) {
  if (!app) return true;  // Win16: a NULL section flushes the cache (nothing to do)
  std::string path = profile_path(rt, file);
  std::string section = rt.read_str(app);
  std::optional<std::string> k, v;
  if (key) k = rt.read_str(key);
  if (key && value) v = rt.read_str(value);
  bool ok = profiles16(rt).set(path, section, k ? std::optional<std::string_view>(*k) : std::nullopt,
                               v ? std::optional<std::string_view>(*v) : std::nullopt);
  trace("file16", "WriteProfileString %s [%s] %s%s", path.c_str(), section.c_str(), k ? k->c_str() : "(section)",
        ok ? "" : ": not written");
  return ok;
}

// ---- files ---------------------------------------------------------------------------------------------

// OpenFile's search for a bare name: current, Windows and System directories,
// then where the modules live.
std::string search_file(Runtime16& rt, const std::string& name) {
  DosFiles& d = rt.state<DosFiles>();
  if (name.find_first_of("\\/:") != std::string::npos) return rt.vfs().full_path(name);
  const Runtime16Options& o = rt.options();
  for (const std::string& dir : {rt.vfs().cwd(), o.windows_dir, o.system_dir, o.guest_dir}) {
    std::string p = rt.vfs().full_path(dir + "\\" + name);
    if (d.exists(p)) return p;
  }
  return rt.vfs().full_path(name);
}

uint16_t open_file(Call16& c) {
  Runtime16& rt = c.rt;
  uint32_t name_p = c.ptr();
  uint32_t of = c.ptr();
  uint16_t style = c.w();
  DosFiles& d = rt.state<DosFiles>();
  std::string name = (style & 0x8000) && of ? rt.read_str(of + 8) : rt.read_str(name_p);
  std::string path = (style & 0x1000) ? rt.vfs().full_path(name) : search_file(rt, name);
  auto set_of = [&](uint16_t err) {
    if (!of) return;
    rt.wr8(of, 136);
    rt.wr8(of + 1, 1);
    rt.wr16(of + 2, err);
    rt.wr32(of + 4, 0);
    rt.write_str(of + 8, upper16(path), 128);
  };
  if (style & 0x0100) {  // OF_PARSE
    set_of(0);
    return 0;
  }
  if (style & 0x0200) {  // OF_DELETE
    bool ok = d.remove(path) == 0;
    set_of(ok ? 0 : doserr::kAccessDenied);
    return ok ? 1 : kHfileError;
  }
  int h;
  if (style & 0x1000) h = d.open(path, 2, true);  // OF_CREATE
  else h = d.open(path, style & 3, false);
  if (h < 0) {
    set_of(uint16_t(-h));
    return kHfileError;
  }
  set_of(0);
  if (style & 0x4000) {  // OF_EXIST: open and close again
    d.close(uint16_t(h));
    return 1;
  }
  return uint16_t(h);
}

// Huge copies (hmemcpy): tile by tile.
void huge_copy(Runtime16& rt, uint32_t dst, uint32_t src, uint32_t n) {
  std::vector<uint8_t> buf;
  while (n) {
    uint32_t chunk = std::min({n, 0x10000 - (dst & 0xFFFF), 0x10000 - (src & 0xFFFF)});
    buf.resize(chunk);
    rt.read_bytes(src, buf.data(), chunk);
    rt.write_bytes(dst, buf.data(), chunk);
    dst = Runtime16::huge_add(dst, chunk);
    src = Runtime16::huge_add(src, chunk);
    n -= chunk;
  }
}

Module16* caller_module(Call16& c) { return c.rt.modules().containing(c.ret_cs()); }

// The system DLLs Windows 95 always had loaded, whether or not anything had
// imported them yet: KRNL386's boot modules, and MMSYSTEM (SYSTEM.INI [boot]
// drivers=mmsystem.dll). Here a system DLL is a pseudo module made when an
// import or LoadLibrary first names it, so GetModuleHandle makes these on
// demand: INTRMLIB's and ANTSW's LibEntry ask for MMSYSTEM before the Star
// Wars modules' later imports (SWSE) bring it in, and would otherwise pick
// the PC speaker (INTRMLIB 4:0044..4:0075).
bool resident_system_module(std::string_view name) {
  size_t slash = name.find_last_of("\\/:");
  std::string base = upper16(slash == std::string_view::npos ? name : name.substr(slash + 1));
  base = base.substr(0, base.find('.'));
  for (const char* m : {"KERNEL", "USER", "GDI", "SYSTEM", "KEYBOARD", "DISPLAY", "SOUND", "MMSYSTEM"}) {
    if (base == m) return true;
  }
  return false;
}

}  // namespace

uint16_t kernel16_current_task(Runtime16& rt) {
  KernelState& s = ks(rt);
  if (!s.task) s.task = make_tdb(rt, "task database");
  return s.task;
}

uint16_t kernel16_shell_task(Runtime16& rt) {
  KernelState& s = ks(rt);
  if (!s.shell_task) s.shell_task = make_tdb(rt, "shell task database");
  return s.shell_task;
}

// ---- shared helpers ------------------------------------------------------------------------------------

uint16_t caller_ds(Call16& c) { return c.rt.cpu().get_segment(SegReg::DS); }

std::string upper16(std::string_view s) {
  std::string u(s);
  for (char& ch : u) ch = char(toupper(uint8_t(ch)));
  return u;
}

bool ieq16(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); i++) {
    if (toupper(uint8_t(a[i])) != toupper(uint8_t(b[i]))) return false;
  }
  return true;
}

loader::ResId res_id(Runtime16& rt, uint32_t fp) {
  if ((fp >> 16) == 0) return loader::ResId::of(uint16_t(fp));
  std::string s = rt.read_str(fp);
  if (!s.empty() && s[0] == '#') return loader::ResId::of(uint16_t(atoi(s.c_str() + 1)));
  return loader::ResId::of(s);
}

// ---- registration ------------------------------------------------------------------------------------

void register_kernel16(Runtime16& rt) {
  Shim16Registry& r = rt.shims();

  // ---- process ----
  r.add(K, 1, "FatalExit", Conv16::pascal_, true, 2, [](Call16& c) {
    uint16_t code = c.w();
    throw GuestError16(GuestError16::Kind::exit, "FatalExit(" + std::to_string(code) + ") from " +
                                                     c.rt.describe(c.ret_cs(), c.ret_ip()));
  });
  r.impl(K, "FatalAppExit", [](Call16& c) {
    c.w();
    std::string msg = c.rt.read_str(c.ptr());
    throw GuestError16(GuestError16::Kind::exit, "FatalAppExit: " + msg);
  });
  // Windows 3.95 on DOS 7.00: AX = 0x5F03, DX = 0x0700.
  r.impl(K, "GetVersion", [](Call16& c) { c.ret32(0x07005F03); });
  r.impl(K, "GetWinFlags", [](Call16& c) { c.ret32(kWinFlags); });
  r.impl(K, "InitTask", [](Call16& c) { c.rt.cpu().registers().w_ax(1); });
  r.impl(K, "GetCurrentTask", [](Call16& c) {
    uint16_t task = kernel16_current_task(c.rt);
    // DX: the first task of the task list (TDB+0 links the next, +1Ch is the
    // task's hInstance): NONSENSE walks it to find the Notepad it started.
    // This task is the only one listed; nothing follows it.
    c.ret32((uint32_t(task) << 16) | task);
  });
  r.impl(K, "IsTask", [](Call16& c) {
    uint16_t h = c.w();
    KernelState& s = ks(c.rt);
    c.ret_bool(h && (h == s.task || h == s.shell_task));
  });
  r.impl(K, "SetErrorMode", [](Call16& c) {
    uint16_t m = c.w();
    uint16_t old = ks(c.rt).error_mode;
    ks(c.rt).error_mode = m;
    c.ret(old);
  });
  r.impl(K, "OutputDebugString", [](Call16& c) {
    std::string s = c.rt.read_str(c.ptr());
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    trace("debug16", "OutputDebugString: %s", s.c_str());
  });
  r.impl(K, "DebugBreak", [](Call16&) {});
  r.impl(K, "WinExec", [](Call16& c) {
    std::string cmd = c.rt.read_str(c.ptr());
    c.w();
    log("win16: WinExec(\"%s\") refused", cmd.c_str());
    c.ret(2);
  });
  r.impl(K, "GetDOSEnvironment", [](Call16& c) { c.ret32(uint32_t(c.rt.sys_sel()) << 16 | layout::kSysEnvironment); });
  r.impl(K, "DOS3Call", [](Call16& c) { dos_int21(c.rt); });
  // OLDMOD16's DLLENTRYPOINT returns whatever this does; the flat-thunk
  // plumbing it connects is replaced by the lane (ABI.md §3.2).
  r.impl(K, "ThunkConnect16", [](Call16& c) { c.ret32(1); });

  // ---- global memory ----
  r.impl(K, "GlobalAlloc", [](Call16& c) {
    uint16_t flags = c.w();
    uint32_t size = c.l();
    c.ret(c.rt.global().alloc(flags, size));
  });
  r.impl(K, "GlobalReAlloc", [](Call16& c) {
    uint16_t h = c.w();
    uint32_t size = c.l();
    uint16_t flags = c.w();
    c.ret(c.rt.global().realloc(h, size, flags));
  });
  r.impl(K, "GlobalFree", [](Call16& c) { c.ret(c.rt.global().free(c.w())); });
  r.impl(K, "GlobalLock", [](Call16& c) { c.ret32(c.rt.global().lock(c.w())); });
  r.impl(K, "GlobalUnlock", [](Call16& c) { c.ret_bool(c.rt.global().unlock(c.w())); });
  r.impl(K, "GlobalSize", [](Call16& c) { c.ret32(c.rt.global().size(c.w())); });
  r.impl(K, "GlobalHandle", [](Call16& c) { c.ret32(c.rt.global().handle(c.w())); });
  r.impl(K, "GlobalFlags", [](Call16& c) { c.ret(c.rt.global().flags(c.w())); });
  r.impl(K, "GlobalCompact", [](Call16& c) {
    c.l();
    c.ret32(std::min<uint32_t>(c.rt.arena().largest_free(), 0x00FF0000));
  });
  r.impl(K, "GetFreeSpace", [](Call16& c) {
    c.w();
    c.ret32(c.rt.arena().bytes_free());
  });
  // GetHeapSpaces(hModule), KERNEL.138 (undocumented; Undocumented Windows
  // ch. 1): HIWORD the size of the module's default local heap, LOWORD its
  // free bytes. A fixed, healthy heap for every module: 57,600 of 64,000
  // bytes free, the same 90% USER's GetFreeSystemResources reports; 0 for a
  // handle that is no module's (as Wine answers). MARVEL.AD's INITIALIZE
  // finds it with GetProcAddress, asks it of USER and GDI (3:3CAC), divides
  // by the size (5:0678) and refuses to start below 20% free.
  r.impl(K, "GetHeapSpaces", [](Call16& c) {
    constexpr uint32_t kSize = 0xFA00, kFree = 0xE100;
    c.ret32(c.rt.modules().by_handle(c.w()) ? kSize << 16 | kFree : 0);
  });
  r.impl(K, "LockSegment", [](Call16& c) {
    uint16_t s = c.w();
    c.ret(s == 0xFFFF ? caller_ds(c) : s);
  });
  r.impl(K, "UnlockSegment", [](Call16& c) {
    uint16_t s = c.w();
    c.ret(s == 0xFFFF ? caller_ds(c) : s);
  });
  r.impl(K, "GlobalPageLock", [](Call16& c) {
    c.w();
    c.ret(1);
  });
  r.impl(K, "GlobalPageUnlock", [](Call16& c) {
    c.w();
    c.ret(0);
  });
  // GlobalWire: move the block low and lock it — nothing moves here, so a
  // GlobalLock (SWSE PLAYMIDIFILE wires the song image, 1:642C);
  // GlobalUnWire: GlobalUnlock, TRUE once the block is unlocked.
  r.impl(K, "GlobalWire", [](Call16& c) { c.ret32(c.rt.global().lock(c.w())); });
  r.impl(K, "GlobalUnWire", [](Call16& c) {
    uint16_t h = c.w();
    c.ret_bool(c.rt.global().find(h) && !c.rt.global().unlock(h));
  });
  r.impl(K, "GlobalLRUOldest", [](Call16& c) { c.ret(c.w()); });
  r.impl(K, "hmemcpy", [](Call16& c) {
    uint32_t dst = c.ptr(), src = c.ptr(), n = c.l();
    huge_copy(c.rt, dst, src, n);
  });

  // ---- segments and selectors ----
  // GetCodeHandle(lpfn): DX:AX = selector:handle of the module segment lpfn
  // points into, as GlobalHandle gives them (Windows loaded the segment first
  // and marked it recently used; every segment is loaded here), 0 when it is
  // no module's (Wine's GetCodeHandle16). DECO.DLL, Marvel Comics Screen
  // Posters' Iterated Systems decoder, locks its decoder around each decode
  // with LockSegment(GetCodeHandle(6:18B2)) and gives up (error 0x6A) on 0
  // (DECO 5:0B85).
  r.impl(K, "GetCodeHandle", [](Call16& c) {
    uint16_t sel = uint16_t(c.ptr() >> 16);
    c.ret32(c.rt.modules().containing(sel) ? c.rt.global().handle(sel) : 0);
  });
  // The selector calls, as Windows 3.1's KRNL386 made them (Pietrek, Windows
  // Internals ch. 2, 3PROTECT.OBJ; the SDK's entries and KB Q132005): what
  // DECO.DLL's real-mode-style decoder needs — a writable alias of its code
  // segment, through which it patches its own code (AllocCStoDSAlias,
  // DECO 7:0016), copies of a template selector based at paragraph offsets
  // into its buffers (AllocSelector, Get/SetSelectorBase, 6:3A8E..6:3BAF),
  // each freed after the decode (FreeSelector, 7:0000). The selectors these
  // calls make are marked as the guest's own (Ldt::mark_guest_run);
  // FreeSelector and SetSelectorBase/Limit act on those only — KRNL386 would
  // bash any LDT entry — so no guest can free or move a global block's, a
  // module segment's or the host's selectors. A segment register that holds
  // a selector FreeSelector frees comes back null (the freed-selector rule,
  // Runtime16::null_freed_segments).
  //
  // AllocSelector(sel): a copy of sel's descriptor (base, limit, rights) —
  // one selector per 64 KiB of its limit, tiled as a huge block's
  // (KB Q132005) —; for 0 or a selector that is no good (KRNL386's LSL
  // fails) one uninitialized selector, not present until its rights are set
  // (DPMI 0009h), as KRNL386's raw Get_Sel(1) is unusable. 0 when the LDT
  // has no room.
  r.impl(K, "AllocSelector", [](Call16& c) {
    uint16_t sel = c.w();
    const SegDesc* d = sel ? c.rt.ldt().get(sel) : nullptr;
    const SegDesc raw{0, 0, false, false, true, false, 3};
    uint16_t s = d ? alloc_own(c.rt, *d, (d->limit >> 16) + 1, "selector (AllocSelector)")
                   : alloc_own(c.rt, raw, 1, "selector (AllocSelector, uninitialized)");
    trace("mem16", "AllocSelector(%04X) -> %04X", sel, s);
    c.ret(s);
  });
  // FreeSelector(sel): 0 when freed (every tile of it), else sel — also for
  // a selector these calls did not make, which stays as it is.
  r.impl(K, "FreeSelector", [](Call16& c) {
    uint16_t sel = c.w();
    uint16_t n = c.rt.ldt().free_guest_run(sel);
    trace("mem16", "FreeSelector(%04X)%s", sel, n ? "" : ": not one AllocSelector & co. made; refused");
    c.ret(n ? 0 : sel);
  });
  // AllocCStoDSAlias / AllocDStoCSAlias(sel): one new selector over sel's
  // descriptor with only its code bit changed (KRNL386's AKA(): base, limit
  // and the R/W bit copied), 0 when sel is no good. Windows fixed a moveable
  // data block first; nothing moves here.
  auto alias = [](Call16& c, bool code) {
    uint16_t sel = c.w();
    const SegDesc* d = c.rt.ldt().get(sel);
    if (!d) return c.ret(0);
    SegDesc a = *d;
    a.code = code;
    uint16_t s = alloc_own(c.rt, a, 1, code ? "selector (AllocDStoCSAlias)" : "selector (AllocCStoDSAlias)");
    trace("mem16", "%s(%04X) -> %04X", code ? "AllocDStoCSAlias" : "AllocCStoDSAlias", sel, s);
    c.ret(s);
  };
  r.impl(K, "AllocCStoDSAlias", [alias](Call16& c) { alias(c, false); });
  r.impl(K, "AllocDStoCSAlias", [alias](Call16& c) { alias(c, true); });
  // Get/SetSelectorBase, Get/SetSelectorLimit. The Gets read any selector
  // (0 for one that is no good: LSL fails). The Sets change the caller's own
  // selectors only; SetSelectorBase returns the selector (0: refused),
  // SetSelectorLimit always 0 (the SDK's entry), taking the limit's 20 bits
  // byte-granular as KRNL386 wrote them. A segment register holding the
  // selector sees the new descriptor at once (the CPU's caches are
  // refreshed, as DPMI's Set Segment Base Address/Limit reload them).
  r.impl(K, "GetSelectorBase", [](Call16& c) { c.ret32(c.rt.ldt().base_of(c.w())); });
  r.impl(K, "GetSelectorLimit", [](Call16& c) { c.ret32(c.rt.ldt().limit_of(c.w())); });
  auto set_selector = [](Call16& c, bool base) {
    uint16_t sel = c.w();
    uint32_t v = c.l();
    const SegDesc* d = c.rt.ldt().get(sel);
    bool own = d && c.rt.ldt().in_guest_run(sel);
    trace("mem16", "SetSelector%s(%04X, %08X)%s", base ? "Base" : "Limit", sel, v,
          own ? "" : ": not one AllocSelector & co. made; refused");
    if (own) {
      SegDesc n = *d;
      if (base) n.base = v;
      else n.limit = v & 0xFFFFF;
      c.rt.ldt().set(sel, n);
      c.rt.cpu().reload_segments();
    }
    c.ret(base && own ? sel : 0);
  };
  r.impl(K, "SetSelectorBase", [set_selector](Call16& c) { set_selector(c, true); });
  r.impl(K, "SetSelectorLimit", [set_selector](Call16& c) { set_selector(c, false); });

  // ---- local memory (the caller's DS) ----
  r.impl(K, "LocalInit", [](Call16& c) {
    uint16_t seg = c.w(), start = c.w(), end = c.w();
    c.ret_bool(c.rt.local().init(seg ? seg : caller_ds(c), start, end));
  });
  r.impl(K, "LocalAlloc", [](Call16& c) {
    uint16_t flags = c.w(), size = c.w();
    uint16_t h = c.rt.local().alloc(caller_ds(c), flags, size);
    c.rt.cpu().registers().w_cx(h);
    c.ret(h);
  });
  r.impl(K, "LocalReAlloc", [](Call16& c) {
    uint16_t h = c.w(), size = c.w(), flags = c.w();
    c.ret(c.rt.local().realloc(caller_ds(c), h, size, flags));
  });
  r.impl(K, "LocalFree", [](Call16& c) { c.ret(c.rt.local().free(caller_ds(c), c.w())); });
  r.impl(K, "LocalLock", [](Call16& c) {
    uint16_t p = c.rt.local().lock(caller_ds(c), c.w());
    c.ret32(p ? (uint32_t(caller_ds(c)) << 16) | p : 0);
  });
  r.impl(K, "LocalUnlock", [](Call16& c) { c.ret_bool(c.rt.local().unlock(caller_ds(c), c.w())); });
  r.impl(K, "LocalSize", [](Call16& c) { c.ret(c.rt.local().size(caller_ds(c), c.w())); });
  r.impl(K, "LocalHandle", [](Call16& c) { c.ret(c.rt.local().handle(caller_ds(c), c.w())); });
  r.impl(K, "LocalFlags", [](Call16& c) { c.ret(c.rt.local().flags(caller_ds(c), c.w())); });
  r.impl(K, "LocalCompact", [](Call16& c) {
    c.w();
    c.ret(c.rt.local().compact(caller_ds(c)));
  });

  // ---- modules ----
  r.impl(K, "LoadLibrary", [](Call16& c) {
    std::string name = c.rt.read_str(c.ptr());
    uint16_t err = 0;
    Module16* m = c.rt.modules().load(name, &err);
    trace("mod16", "LoadLibrary(\"%s\") -> %s", name.c_str(), m ? m->name.c_str() : "failed");
    c.ret(m ? m->hinstance : (err ? err : 2));
  });
  r.impl(K, "FreeLibrary", [](Call16& c) {
    Module16* m = c.rt.modules().by_handle(c.w());
    if (m) c.rt.modules().free(m);
  });
  r.impl(K, "GetModuleHandle", [](Call16& c) {
    uint32_t p = c.ptr();
    Module16* m = nullptr;
    if (p >> 16) {
      std::string name = c.rt.read_str(p);
      m = c.rt.modules().by_name(name);
      if (!m && resident_system_module(name)) {
        m = c.rt.modules().load(name);
        trace("mod16", "GetModuleHandle(\"%s\"): a system module Windows 95 always has loaded", name.c_str());
      }
    } else {
      m = c.rt.modules().by_handle(uint16_t(p));
    }
    // HIWORD = the instance handle as well, as Windows returned it.
    c.ret32(m ? (uint32_t(m->hinstance) << 16) | m->hmodule : 0);
  });
  r.impl(K, "GetModuleUsage", [](Call16& c) {
    Module16* m = c.rt.modules().by_handle(c.w());
    c.ret(m ? uint16_t(std::max(m->refs, 1)) : 0);
  });
  r.impl(K, "GetModuleFileName", [](Call16& c) {
    uint16_t h = c.w();
    uint32_t buf = c.ptr();
    int16_t n = c.sw();
    Module16* m = h ? c.rt.modules().by_handle(h) : caller_module(c);
    std::string path = m ? upper16(m->guest_path) : upper16(c.rt.options().system_dir + "\\KRNL386.EXE");
    c.ret(n > 0 ? uint16_t(c.rt.write_str(buf, path, size_t(n))) : 0);
  });
  r.impl(K, "GetProcAddress", [](Call16& c) {
    uint16_t h = c.w();
    uint32_t name = c.ptr();
    Module16* m = h ? c.rt.modules().by_handle(h) : caller_module(c);
    uint32_t fp = 0;
    if (m) fp = (name >> 16) ? c.rt.modules().proc_address(m, c.rt.read_str(name))
                             : c.rt.modules().proc_address(m, uint16_t(name));
    if (tracing("mod16")) {
      std::string n = (name >> 16) ? c.rt.read_str(name) : "#" + std::to_string(name & 0xFFFF);
      trace("mod16", "GetProcAddress(%s, %s) -> %04X:%04X", m ? m->name.c_str() : "?", n.c_str(), fp >> 16,
            fp & 0xFFFF);
    }
    c.ret32(fp);
  });
  r.impl(K, "MakeProcInstance", [](Call16& c) {
    uint32_t proc = c.ptr();
    c.w();
    c.ret32(proc);
  });
  r.impl(K, "FreeProcInstance", [](Call16& c) { c.ptr(); });

  // ---- resources ----
  r.impl(K, "FindResource", [](Call16& c) {
    uint16_t h = c.w();
    uint32_t name = c.ptr(), type = c.ptr();
    Module16* m = c.rt.modules().by_handle(h);
    loader::ResId t = res_id(c.rt, type), n = res_id(c.rt, name);
    const loader::ne::Resource* res = c.rt.modules().find_resource(m, t, n);
    trace("res16", "FindResource(%s, %s, %s) -> %s", m ? m->name.c_str() : "?", n.to_string().c_str(),
          t.to_string().c_str(), res ? "found" : "none");
    if (!res) return c.ret(0);
    KernelState& s = ks(c.rt);
    for (size_t i = 0; i < s.resources.size(); i++) {
      if (s.resources[i].res == res && s.resources[i].image == m->image) return c.ret(uint16_t(i + 1));
    }
    s.resources.push_back({m->image, m->name, m->guest_path, res, 0, 0});
    c.ret(uint16_t(s.resources.size()));
  });
  // LoadResource counts: a resource already loaded (its block still there,
  // not discarded) comes back as the same block with one more use; else a
  // fresh copy is read from the image (Win16's NE_NAMEINFO usage count).
  r.impl(K, "LoadResource", [](Call16& c) {
    c.w();
    uint16_t hr = c.w();
    KernelState& s = ks(c.rt);
    if (!hr || hr > s.resources.size()) return c.ret(0);
    auto& e = s.resources[hr - 1];
    GlobalBlock* have = e.hglobal ? c.rt.global().find(e.hglobal) : nullptr;
    if (have && have->base) {
      e.usage++;
      trace("res16", "LoadResource(%s %s): loaded, %u uses", e.module.c_str(), e.res->name.to_string().c_str(), e.usage);
      return c.ret(e.hglobal);
    }
    std::string_view data = e.image->resource_data(*e.res);
    uint16_t h = c.rt.global().alloc(GlobalHeap16::kMoveable, uint32_t(std::max<size_t>(data.size(), 1)));
    GlobalBlock* b = c.rt.global().find(h);
    if (!b) return c.ret(0);
    c.rt.mem().memcpy(b->base, data.data(), data.size());
    c.rt.ldt().set_tag(b->sel, "resource of " + e.module);
    e.hglobal = h;
    e.usage = 1;
    trace("res16", "LoadResource(%s %s): read from the image -> %04X", e.module.c_str(), e.res->name.to_string().c_str(), h);
    c.ret(e.hglobal);
  });
  r.impl(K, "LockResource", [](Call16& c) { c.ret32(c.rt.global().lock(c.w())); });
  // FreeResource: one use less; at none the block is freed, so the next
  // LoadResource reads the resource afresh (POSTERS appends a space to its
  // locked caption every frame, SWSE 1:6A63..1:6AE6, and the edits must not
  // pile up). FALSE is success. A block that is no loaded resource is
  // GlobalFree'd, as KERNEL handed it on to USER's DestroyIcon32, which frees it.
  r.impl(K, "FreeResource", [](Call16& c) {
    uint16_t h = c.w();
    if (!h) return c.ret(0);
    KernelState& s = ks(c.rt);
    for (auto& e : s.resources) {
      if (!e.hglobal || (e.hglobal | 1) != (h | 1)) continue;
      if (e.usage) e.usage--;
      if (!e.usage) {
        c.rt.global().free(e.hglobal);
        e.hglobal = 0;
      }
      trace("res16", "FreeResource(%s %s): %u uses left", e.module.c_str(), e.res->name.to_string().c_str(), e.usage);
      return c.ret(0);
    }
    c.ret(c.rt.global().free(h) == 0 ? 0 : 1);
  });
  // AccessResource(hInstance, hResInfo): a DOS handle on the module's file,
  // opened read-only and positioned at the resource's data — SWSE
  // (_CREATEMEMRESOURCE, LOADRESTOMEM) and READJPG read pictures, palettes,
  // shapes and WAVE sounds through it with _hread. HFILE_ERROR (-1) when
  // there is no such resource or the file cannot be opened.
  r.impl(K, "AccessResource", [](Call16& c) {
    c.w();
    uint16_t hr = c.w();
    KernelState& s = ks(c.rt);
    if (!hr || hr > s.resources.size()) return c.ret(kHfileError);
    auto& e = s.resources[hr - 1];
    DosFiles& d = c.rt.state<DosFiles>();
    int h = e.guest_path.empty() ? -1 : d.open(e.guest_path, 0, false);
    if (h >= 0 && d.seek(uint16_t(h), int32_t(e.res->file_offset), 0) < 0) {
      d.close(uint16_t(h));
      h = -1;
    }
    trace("res16", "AccessResource(%s %s) -> %s at %u", e.module.c_str(), e.res->name.to_string().c_str(),
          h < 0 ? "HFILE_ERROR" : std::to_string(h).c_str(), e.res->file_offset);
    c.ret(h < 0 ? kHfileError : uint16_t(h));
  });
  r.impl(K, "SizeofResource", [](Call16& c) {
    c.w();
    uint16_t hr = c.w();
    KernelState& s = ks(c.rt);
    c.ret32(hr && hr <= s.resources.size() ? s.resources[hr - 1].res->size : 0);
  });

  // ---- strings ----
  r.impl(K, "lstrcpy", [](Call16& c) {
    uint32_t dst = c.ptr(), src = c.ptr();
    std::string s = c.rt.read_str(src);
    c.rt.write_bytes(dst, s.c_str(), s.size() + 1);
    c.ret32(dst);
  });
  r.impl(K, "lstrcpyn", [](Call16& c) {
    uint32_t dst = c.ptr(), src = c.ptr();
    uint16_t n = c.w();
    if (n) c.rt.write_str(dst, c.rt.read_str(src, n), n);
    c.ret32(dst);
  });
  r.impl(K, "lstrcat", [](Call16& c) {
    uint32_t dst = c.ptr(), src = c.ptr();
    std::string d = c.rt.read_str(dst), s = c.rt.read_str(src);
    c.rt.write_bytes(dst + uint32_t(d.size()), s.c_str(), s.size() + 1);
    c.ret32(dst);
  });
  r.impl(K, "lstrlen", [](Call16& c) { c.ret(uint16_t(c.rt.read_str(c.ptr()).size())); });

  // ---- profiles ----
  r.impl(K, "GetProfileInt", [](Call16& c) {
    uint32_t app = c.ptr(), key = c.ptr();
    int16_t def = c.sw();
    c.ret(uint16_t(get_profile_int(c.rt, "WIN.INI", app, key, def)));
  });
  r.impl(K, "GetProfileString", [](Call16& c) {
    uint32_t app = c.ptr(), key = c.ptr(), def = c.ptr(), buf = c.ptr();
    uint16_t size = c.w();
    c.ret(get_profile_string(c.rt, "WIN.INI", app, key, def, buf, size));
  });
  r.impl(K, "WriteProfileString", [](Call16& c) {
    uint32_t app = c.ptr(), key = c.ptr(), val = c.ptr();
    c.ret_bool(write_profile_string(c.rt, "WIN.INI", app, key, val));
  });
  r.impl(K, "GetPrivateProfileInt", [](Call16& c) {
    uint32_t app = c.ptr(), key = c.ptr();
    int16_t def = c.sw();
    std::string file = c.rt.read_str(c.ptr());
    c.ret(uint16_t(get_profile_int(c.rt, file, app, key, def)));
  });
  r.impl(K, "GetPrivateProfileString", [](Call16& c) {
    uint32_t app = c.ptr(), key = c.ptr(), def = c.ptr(), buf = c.ptr();
    uint16_t size = c.w();
    std::string file = c.rt.read_str(c.ptr());
    c.ret(get_profile_string(c.rt, file, app, key, def, buf, size));
  });
  r.impl(K, "WritePrivateProfileString", [](Call16& c) {
    uint32_t app = c.ptr(), key = c.ptr(), val = c.ptr();
    std::string file = c.rt.read_str(c.ptr());
    c.ret_bool(write_profile_string(c.rt, file, app, key, val));
  });
  r.impl(K, "GetWindowsDirectory", [](Call16& c) {
    uint32_t buf = c.ptr();
    uint16_t n = c.w();
    const std::string& d = c.rt.options().windows_dir;
    if (n > d.size()) c.rt.write_str(buf, d, n);
    c.ret(uint16_t(d.size()));
  });
  r.impl(K, "GetDriveType", [](Call16& c) {
    uint16_t drive = c.w();
    c.ret(drive == 2 ? 3 : 0);  // C: fixed; nothing else exists
  });

  // ---- files ----
  r.impl(K, "OpenFile", [](Call16& c) { c.ret(open_file(c)); });
  r.impl(K, "_lopen", [](Call16& c) {
    std::string name = c.rt.read_str(c.ptr());
    uint16_t mode = c.w();
    int h = c.rt.state<DosFiles>().open(name, mode & 3, false);
    c.ret(h < 0 ? kHfileError : uint16_t(h));
  });
  r.impl(K, "_lcreat", [](Call16& c) {
    std::string name = c.rt.read_str(c.ptr());
    c.w();
    int h = c.rt.state<DosFiles>().open(name, 2, true);
    c.ret(h < 0 ? kHfileError : uint16_t(h));
  });
  r.impl(K, "_lclose", [](Call16& c) { c.ret(c.rt.state<DosFiles>().close(c.w()) < 0 ? kHfileError : 0); });
  r.impl(K, "_lread", [](Call16& c) {
    uint16_t h = c.w();
    uint32_t buf = c.ptr();
    uint16_t n = c.w();
    int32_t got = c.rt.state<DosFiles>().read(h, buf, n);
    c.ret(got < 0 ? kHfileError : uint16_t(got));
  });
  r.impl(K, "_lwrite", [](Call16& c) {
    uint16_t h = c.w();
    uint32_t buf = c.ptr();
    uint16_t n = c.w();
    int32_t put = c.rt.state<DosFiles>().write(h, buf, n);
    c.ret(put < 0 ? kHfileError : uint16_t(put));
  });
  r.impl(K, "_hread", [](Call16& c) {
    uint16_t h = c.w();
    uint32_t buf = c.ptr(), n = c.l();
    int32_t got = c.rt.state<DosFiles>().read(h, buf, n);
    c.ret32(got < 0 ? 0xFFFFFFFF : uint32_t(got));
  });
  r.impl(K, "_hwrite", [](Call16& c) {
    uint16_t h = c.w();
    uint32_t buf = c.ptr(), n = c.l();
    int32_t put = c.rt.state<DosFiles>().write(h, buf, n);
    c.ret32(put < 0 ? 0xFFFFFFFF : uint32_t(put));
  });
  r.impl(K, "_llseek", [](Call16& c) {
    uint16_t h = c.w();
    int32_t off = c.sl();
    uint16_t whence = c.w();
    int64_t p = c.rt.state<DosFiles>().seek(h, off, whence);
    c.ret32(p < 0 ? 0xFFFFFFFF : uint32_t(p));
  });
  // GetTempFileName(bDriveLetter, lpPrefix, uUnique, lpTempFileName):
  // "<TEMP>\~<prefix, 3 characters><uUnique, 4 hex digits>.TMP". With
  // TF_FORCEDRIVE (0x80) the file goes in the current directory of
  // bDriveLetter's drive instead (0: the current drive), whatever TEMP says —
  // the rule the Windows 3.1 SDK documents for the flag (Wine's
  // GetTempFileName16 ends at the drive's root only because it hands "C:" to
  // Win32's GetTempFileName, which adds a '\'). Here that is that drive's own
  // current directory (win32::Vfs::drive_cwd: on the current drive the
  // module folder, C:\SAVER or C:\AFTERDRK, writable; on another its root
  // until a chdir there); a drive the guest's disk does not have falls back
  // to TEMP, as Wine does for an invalid drive.
  // With uUnique 0 Windows picks a number no file has yet and CREATES the
  // empty file: STRESS (LibEntry, and GETFREEFILEHANDLES at every Star Wars
  // module's start) opens it until the handles run out, and each module
  // refuses to start below 10. Windows took the first number from the clock;
  // here it is 1234h, counting up past taken names, so runs stay
  // deterministic. A nonzero uUnique makes no file. Returns the number used,
  // also when the file could not be made (logged), as Wine does.
  r.impl(K, "GetTempFileName", [](Call16& c) {
    uint16_t drive = c.w();
    std::string prefix = c.rt.read_str(c.ptr());
    uint16_t unique = c.w();
    uint32_t buf = c.ptr();
    std::string dir = c.rt.options().windows_dir + "\\TEMP";
    if (drive & 0x80) {
      win32::Vfs& vfs = c.rt.vfs();
      char letter = char(toupper(drive & 0x7F));
      if (letter < 'A' || letter > 'Z') letter = vfs.cwd()[0];
      std::string forced = vfs.drive_cwd(letter);
      if (vfs.is_dir(forced)) dir = forced.size() == 3 ? forced.substr(0, 2) : forced;  // a root: "C:" (name_for adds the '\')
    }
    auto name_for = [&](uint16_t n) {
      char name[32];
      snprintf(name, sizeof(name), "~%.3s%04X.TMP", prefix.c_str(), n);
      return dir + "\\" + name;
    };
    std::string path;
    if (unique) {
      path = name_for(unique);
    } else {
      DosFiles& d = c.rt.state<DosFiles>();
      uint16_t n = 0x1234;
      for (uint32_t tries = 0; tries < 0xFFFF; tries++, n = uint16_t(n == 0xFFFF ? 1 : n + 1)) {
        path = name_for(n);
        if (!d.exists(path)) break;
      }
      unique = n;
      int h = d.open(path, 2, true, /*exclusive=*/true);
      if (h >= 0) {
        d.close(uint16_t(h));
        trace("file16", "GetTempFileName -> %s (created)", path.c_str());
      } else {
        log("win16: GetTempFileName could not create %s (DOS error %d)", path.c_str(), -h);
      }
    }
    c.rt.write_str(buf, path, 144);
    c.ret(unique);
  });
  // SetHandleCount(n): the task's file handle table grows to n entries (at
  // most 255; it never shrinks below Windows' default 20); the result is its
  // size. STRESS.DLL's LibEntry asks for 255 (STRESS 5:017C).
  r.impl(K, "SetHandleCount", [](Call16& c) {
    uint16_t n = std::min<uint16_t>(c.w(), 255);
    KernelState& s = ks(c.rt);
    if (n > s.handle_count) s.handle_count = n;
    c.ret(s.handle_count);
  });

  // ---- Catch/Throw ----
  // CATCHBUF (9 WORDs, opaque to its users): IP, CS, SP, BP, SI, DI, DS, the
  // host call level Catch ran at (call_depth()), SS. The level lets a Throw
  // from inside a callback the host made (a window or timer procedure called
  // from a shim) unwind the host frames in between (GuestUnwind16) instead of
  // jumping into a guest stack frame some host frame still owns.
  r.impl(K, "Catch", [](Call16& c) {
    uint32_t buf = c.ptr();
    auto& cpu = c.rt.cpu();
    auto& rr = cpu.registers();
    uint16_t words[9] = {c.ret_ip(),  c.ret_cs(),  uint16_t(c.sp + 4 + 4),      rr.r_bp(),
                         rr.r_si(),   rr.r_di(),   cpu.get_segment(SegReg::DS), uint16_t(c.rt.call_depth()),
                         cpu.get_segment(SegReg::SS)};
    for (int i = 0; i < 9; i++) c.rt.wr16(buf + 2u * uint32_t(i), words[i]);
    c.ret(0);
  });
  r.impl(K, "Throw", [](Call16& c) {
    uint32_t buf = c.ptr();
    uint16_t back = c.w();
    if (tracing("throw16")) trace("throw16", "Throw(%d) from %s", int16_t(back), c.rt.backtrace().c_str());
    std::array<uint16_t, 9> w;
    for (int i = 0; i < 9; i++) w[size_t(i)] = c.rt.rd16(buf + 2u * uint32_t(i));
    auto resume = [w, back](Runtime16& rt) {
      auto& cpu = rt.cpu();
      auto& rr = cpu.registers();
      cpu.load_segment(SegReg::SS, w[8]);
      rr.w_sp(w[2]);
      rr.w_bp(w[3]);
      rr.w_si(w[4]);
      rr.w_di(w[5]);
      if (w[6] & ~3) cpu.load_segment(SegReg::DS, w[6]);
      else cpu.set_segment_null(SegReg::DS);
      rr.w_ax(back);
      cpu.set_cs_eip(w[1], w[0]);
    };
    int level = int16_t(w[7]);
    if (level > 0 && level < c.rt.call_depth()) {
      trace("throw16", "Throw unwinds %d host call level(s) to the Catch", c.rt.call_depth() - level);
      throw GuestUnwind16{level, resume};
    }
    // Same level (or a buffer from a level that has returned — undefined on
    // Windows too): jump straight there.
    resume(c.rt);
    c.take_over();
  });

  // ---- SwitchStackTo/Back: run the caller's locals on a stack in its own data segment ----
  r.impl(K, "SwitchStackTo", [](Call16& c) {
    uint16_t new_ss = c.w(), new_sp = c.w();
    c.w();  // stack top (lowest address): only a stack probe's business
    auto& cpu = c.rt.cpu();
    auto& rr = cpu.registers();
    uint16_t rip = c.ret_ip(), rcs = c.ret_cs();
    uint16_t old_ss = cpu.get_segment(SegReg::SS);
    uint16_t sp_after = uint16_t(c.sp + 4 + 6);
    uint16_t bp = rr.r_bp();
    // The caller's locals and saved BP ([sp_after, bp+2)) move to the new
    // stack, so BP-relative code keeps working until SwitchStackBack.
    uint16_t frame = bp >= sp_after ? uint16_t(bp + 2 - sp_after) : 0;
    uint16_t nsp = uint16_t(new_sp - frame);
    std::vector<uint8_t> tmp(frame);
    if (frame) {
      c.rt.read_bytes((uint32_t(old_ss) << 16) | sp_after, tmp.data(), frame);
      c.rt.write_bytes((uint32_t(new_ss) << 16) | nsp, tmp.data(), frame);
    }
    ks(c.rt).stack_switches.push_back({old_ss, sp_after, bp, frame});
    cpu.load_segment(SegReg::SS, new_ss);
    rr.w_sp(nsp);
    rr.w_bp(uint16_t(nsp + (bp - sp_after)));
    cpu.set_cs_eip(rcs, rip);
    c.take_over();
  });
  r.impl(K, "SwitchStackBack", [](Call16& c) {
    auto& ss = ks(c.rt).stack_switches;
    if (ss.empty()) return;
    auto sw = ss.back();
    ss.pop_back();
    auto& cpu = c.rt.cpu();
    auto& rr = cpu.registers();
    uint16_t rip = c.ret_ip(), rcs = c.ret_cs();
    uint16_t cur_ss = cpu.get_segment(SegReg::SS);
    uint16_t sp_after = uint16_t(c.sp + 4);
    // Copy the (possibly changed) frame back to where it came from.
    if (sw.frame) {
      std::vector<uint8_t> tmp(sw.frame);
      c.rt.read_bytes((uint32_t(cur_ss) << 16) | sp_after, tmp.data(), sw.frame);
      c.rt.write_bytes((uint32_t(sw.ss) << 16) | sw.sp, tmp.data(), sw.frame);
    }
    cpu.load_segment(SegReg::SS, sw.ss);
    rr.w_sp(sw.sp);
    rr.w_bp(sw.bp);
    cpu.set_cs_eip(rcs, rip);
    c.take_over();
  });
}

}  // namespace adw::win16
