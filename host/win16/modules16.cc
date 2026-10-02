#include "win16/modules16.hh"

#include <windows.h>

#include <algorithm>
#include <cctype>

#include "adw/core/log.h"
#include "adw/core/text.h"
#include "win16/dos16.hh"
#include "win16/runtime16.hh"
#include "win16/shim_families16.hh"
#include "win32/vfs.hh"

namespace adw::win16 {

namespace ne = loader::ne;

namespace {

std::string upper(std::string_view s) {
  std::string u(s);
  for (char& c : u) c = char(toupper(uint8_t(c)));
  return u;
}

std::string file_part(std::string_view p) {
  size_t s = p.find_last_of("\\/:");
  return std::string(s == std::string_view::npos ? p : p.substr(s + 1));
}

std::string strip_ext(std::string_view f) {
  size_t d = f.find('.');
  return std::string(d == std::string_view::npos ? f : f.substr(0, d));
}

bool host_file_exists(const std::string& p) {
  DWORD a = GetFileAttributesW(widen(p).c_str());
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// GetWinFlags(): protected mode, enhanced mode, a 486 with its FPU — what
// OLDMOD16 turns into AD_SYSTEM's CPU (4) and FPU (1) fields.
constexpr uint16_t kWinFlags = 0x0001 | 0x0020 | 0x0008 | 0x0400;

// place() reserves every segment through this sink, so each gets its own
// global block (and selector) as it is placed.
class SegmentSink : public loader::ImageSink {
 public:
  SegmentSink(Runtime16& rt, Module16& m) : rt_(rt), m_(m) {}
  uint32_t reserve(uint32_t, uint32_t size) override {
    const ne::Image& img = *m_.image;
    size_t i = m_.seg_sel.size();
    const ne::Segment& s = img.segments().at(i);
    bool dgroup = img.header().autodata_segment == s.index;
    // The automatic data segment is reserved at 64 KiB: its local heap grows
    // in place (local_heap.hh), as Win16 grew DGROUP by reallocating it.
    GlobalBlock* b = rt_.global().alloc_block(size, !s.is_data(), s.is_moveable() ? GlobalHeap16::kMoveable : 0,
                                              m_.hmodule, dgroup ? 0x10000 : size);
    if (!b) throw loader::LoaderError(loader::LoaderError::Kind::placement, "out of global memory for a segment");
    m_.seg_sel.push_back(b->sel);
    m_.seg_base.push_back(b->base);
    rt_.ldt().set_tag(b->sel, m_.name + " seg " + std::to_string(s.index));
    return b->base;
  }
  void write(uint32_t addr, const void* data, size_t size) override { rt_.mem().memcpy(addr, data, size); }

 private:
  Runtime16& rt_;
  Module16& m_;
};

}  // namespace

ModuleTable16::ModuleTable16(Runtime16& rt) : rt_(rt) {}
ModuleTable16::~ModuleTable16() = default;

void ModuleTable16::add_search_dir(const std::string& host_dir) { search_dirs_.push_back(host_dir); }

Module16* ModuleTable16::system_module(const std::string& name) {
  for (auto& m : modules_) {
    if (m->system && m->name == name) return m.get();
  }
  auto m = std::make_unique<Module16>();
  m->name = name;
  // MCISEQ is the MCI sequencer driver (sound16.cc registers it with the audio engine on).
  m->file = name + (name == "KERNEL" || name == "USER" || name == "GDI" ? ".EXE" : name == "MCISEQ" ? ".DRV" : ".DLL");
  m->guest_path = rt_.options().system_dir + "\\" + m->file;
  m->system = true;
  m->initialized = true;
  m->refs = 1;
  // A module database with its 'NE' signature, so hModule is a real selector.
  if (GlobalBlock* b = rt_.global().alloc_block(64, false, 0, 0)) {
    b->owner = b->sel;
    rt_.mem().write_u16l(b->base, 0x454E);
    m->hmodule = b->sel;
    rt_.ldt().set_tag(b->sel, name + " module database");
  }
  m->hinstance = m->hmodule;
  modules_.push_back(std::move(m));
  return modules_.back().get();
}

Module16* ModuleTable16::by_handle(uint16_t h) {
  if (!h) return nullptr;
  for (auto& m : modules_) {
    if ((m->hmodule | 3) == (h | 3) || (m->hinstance | 3) == (h | 3)) return m.get();
  }
  return nullptr;
}

Module16* ModuleTable16::by_name(std::string_view name) {
  std::string f = upper(file_part(name));
  std::string base = strip_ext(f);
  for (auto& m : modules_) {
    if (m->file == f || m->name == f || m->name == base || strip_ext(m->file) == base) return m.get();
  }
  return nullptr;
}

Module16* ModuleTable16::containing(uint16_t sel) {
  for (auto& m : modules_) {
    if (segment_index(*m, sel)) return m.get();
  }
  return nullptr;
}

int ModuleTable16::segment_index(const Module16& m, uint16_t sel) const {
  for (size_t i = 0; i < m.seg_sel.size(); i++) {
    if ((m.seg_sel[i] | 3) == (sel | 3)) return int(i + 1);
  }
  return 0;
}

std::string ModuleTable16::find_file(std::string_view name, std::string* guest_path) {
  std::string n(name);
  if (file_part(n).find('.') == std::string::npos) n += ".DLL";
  win32::Vfs& vfs = rt_.vfs();
  if (n.find_first_of("\\/:") != std::string::npos) {
    std::string g = vfs.full_path(n);
    std::string h = vfs.to_host(g);
    if (!h.empty() && host_file_exists(h)) {
      *guest_path = g;
      return h;
    }
    n = file_part(n);  // a path that leads nowhere: fall back to the search, as a lenient loader would
  }
  const Runtime16Options& o = rt_.options();
  for (const std::string& dir : {vfs.cwd(), o.windows_dir, o.system_dir, o.guest_dir}) {
    std::string g = vfs.full_path(dir + "\\" + n);
    std::string h = vfs.to_host(g);
    if (!h.empty() && host_file_exists(h)) {
      *guest_path = g;
      return h;
    }
  }
  for (const std::string& dir : search_dirs_) {
    std::string h = dir + "\\" + n;
    if (host_file_exists(h)) {
      std::string g = vfs.to_guest(h);
      *guest_path = g.empty() ? o.guest_dir + "\\" + upper(n) : g;
      return h;
    }
  }
  return {};
}

Module16* ModuleTable16::load(std::string_view name, uint16_t* err) {
  if (err) *err = 0;
  std::string f = upper(file_part(name));
  std::string base = strip_ext(f);
  for (auto& m : modules_) {
    if (m->file == f || (!m->system && m->name == base && f.find('.') == std::string::npos) ||
        (m->system && m->name == base)) {
      m->refs++;
      return m.get();
    }
  }
  if (rt_.shims().has_module(base)) {
    Module16* m = system_module(base);
    return m;
  }
  std::string guest;
  std::string host = find_file(name, &guest);
  if (host.empty()) {
    trace("mod16", "LoadLibrary(%.*s): not found", int(name.size()), name.data());
    if (err) *err = 2;
    return nullptr;
  }
  return load_file(host, guest, err);
}

Module16* ModuleTable16::load_host(const std::string& host_path, uint16_t* err) {
  for (auto& m : modules_) {
    if (!m->system && CompareStringOrdinal(widen(m->host_path).c_str(), -1, widen(host_path).c_str(), -1, TRUE) ==
                          CSTR_EQUAL) {
      m->refs++;
      return m.get();
    }
  }
  std::string guest = rt_.vfs().to_guest(host_path);
  if (guest.empty()) guest = rt_.options().guest_dir + "\\" + upper(file_part(host_path));
  return load_file(host_path, guest, err);
}

Module16* ModuleTable16::load_task(const std::string& host_path, const std::string& guest_path,
                                   const std::string& command_tail, uint16_t* err) {
  if (err) *err = 0;
  if (task_.module) {
    log("win16: %s: a task is loaded already (%s)", host_path.c_str(), task_.module->file.c_str());
    if (err) *err = 11;
    return nullptr;
  }
  if (!host_file_exists(host_path)) {
    if (err) *err = 2;
    return nullptr;
  }
  task_.command_tail = command_tail.substr(0, 126);
  return load_file(host_path, guest_path, err, /*task=*/true);
}

uint32_t ModuleTable16::run_task() {
  if (!task_.module) throw GuestError16(GuestError16::Kind::fatal, "run_task: no task is loaded");
  Module16* m = task_.module;
  const ne::Header& h = m->image->header();
  if (!h.cs || h.cs > m->seg_sel.size()) throw GuestError16(GuestError16::Kind::fatal, m->file + " has no entry point");
  Regs16In in;
  in.ax = 0;
  in.bx = task_.stack_size;
  in.cx = task_.heap_size;
  in.dx = 0;
  in.si = 0;  // hPrevInstance
  in.di = m->hinstance;
  in.bp = 0;
  in.ds = m->dgroup;
  in.es = task_.psp;
  in.ss = task_.ss;
  in.sp = task_.sp;
  trace("mod16", "%s: the task starts at %u:%04X, SS:SP %04X:%04X, stack %u, heap %u, PSP %04X, command tail \"%s\"",
        m->file.c_str(), h.cs, h.ip, task_.ss, task_.sp, task_.stack_size, task_.heap_size, task_.psp,
        task_.command_tail.c_str());
  const bool was = rt_.task_budget();
  rt_.set_task_budget(true);
  try {
    uint32_t r = rt_.call_far((uint32_t(m->seg_sel[h.cs - 1]) << 16) | h.ip, {}, &in);
    rt_.set_task_budget(was);
    return r;
  } catch (...) {
    rt_.set_task_budget(was);
    throw;
  }
}

Module16* ModuleTable16::load_file(const std::string& host_path, const std::string& guest_path, uint16_t* err,
                                   bool task) {
  for (auto& m : modules_) {
    if (!m->system && CompareStringOrdinal(widen(m->host_path).c_str(), -1, widen(host_path).c_str(), -1, TRUE) ==
                          CSTR_EQUAL) {
      m->refs++;
      return m.get();
    }
  }
  std::shared_ptr<ne::Image> img;
  try {
    img = std::make_shared<ne::Image>(ne::Image::from_file(host_path));
  } catch (const loader::LoaderError& e) {
    log("win16: %s: not a loadable NE image: %s", host_path.c_str(), e.what());
    if (err) *err = 11;
    return nullptr;
  } catch (const std::exception& e) {
    log("win16: %s: %s", host_path.c_str(), e.what());
    if (err) *err = 2;
    return nullptr;
  }
  if (!img->header().is_dll() && !task) {
    // Libraries only, but the one application the runtime runs as its task (load_task).
    log("win16: %s is a Win16 application, not a DLL", host_path.c_str());
    if (err) *err = 11;
    return nullptr;
  }
  if (task && img->header().is_dll()) {
    log("win16: %s is a Win16 library, not an application", host_path.c_str());
    if (err) *err = 11;
    return nullptr;
  }

  auto owned = std::make_unique<Module16>();
  Module16* m = owned.get();
  m->name = upper(img->module_name());
  m->file = upper(file_part(host_path));
  m->host_path = host_path;
  m->guest_path = guest_path;
  m->image = img;
  m->refs = 1;
  // The task's start is its entry point (run_task), never a LibEntry.
  if (task) m->initialized = true;

  // The module database: the NE header as the file has it ('NE' at 0).
  const ne::Header& h = img->header();
  std::string_view file = img->file();
  uint32_t mdb_size = uint32_t(std::min<size_t>(file.size() - h.ne_offset, 0x2000));
  GlobalBlock* mdb = rt_.global().alloc_block(mdb_size, false, 0, 0);
  if (!mdb) {
    if (err) *err = 14;
    return nullptr;
  }
  mdb->owner = mdb->sel;
  m->hmodule = mdb->sel;
  rt_.mem().memcpy(mdb->base, file.data() + h.ne_offset, mdb_size);
  rt_.ldt().set_tag(mdb->sel, m->name + " module database");
  modules_.push_back(std::move(owned));

  try {
    ne::LoadOptions opts;
    opts.os_fixups = ne::OsFixupMode::leave;  // real x87 opcodes: the CPU has an FPU
    SegmentSink sink(rt_, *m);
    ne::Placement pl = ne::place(*img, sink, opts);
    if (h.autodata_segment && h.autodata_segment <= m->seg_sel.size()) m->dgroup = m->seg_sel[h.autodata_segment - 1];
    m->hinstance = m->dgroup ? m->dgroup : m->hmodule;

    // What it imports, loaded and initialized first.
    for (const std::string& ref : img->module_refs()) {
      uint16_t e = 0;
      Module16* dep = load(ref, &e);
      if (!dep) {
        log("win16: %s imports %s, which cannot be loaded (error %u)", m->file.c_str(), ref.c_str(), e);
        if (err) *err = e ? e : 2;
        unload(m);
        return nullptr;
      }
      m->deps.push_back(dep);
    }

    ne::LoadStats st = ne::load_segments(*img, pl, sink, [&](const ne::Target& t) { return resolve(m, t); }, opts);
    trace("mod16", "%s: %zu segments, %zu relocations (%zu os fixups left as x87)", m->file.c_str(),
          m->seg_sel.size(), st.locations, st.os_fixups);

    if (m->dgroup) {
      if (GlobalBlock* dg = rt_.global().find(m->dgroup)) rt_.global().set_limit(*dg, 0x10000);
      const ne::Segment& ds = img->segment(h.autodata_segment);
      uint32_t static_end = (ds.flags & ne::seg_iterated) ? ds.min_alloc : std::max(ds.file_size, ds.min_alloc);
      static_end = std::min<uint32_t>(static_end, 0xFFF0);
      if (task) {
        // The task's DGROUP (Tasks): the stack above the static data, the
        // local heap above the stack (InitTask makes it); SP 0 is the stack's top.
        const uint32_t top = std::min<uint32_t>(static_end + h.stack_size, 0xFFF0);
        task_.module = m;
        task_.stack_size = h.stack_size;
        task_.heap_size = h.heap_size;
        task_.stack_low = uint16_t(static_end);
        task_.ss = h.ss && h.ss <= m->seg_sel.size() ? m->seg_sel[h.ss - 1] : m->dgroup;
        task_.sp = uint16_t(h.sp ? h.sp : top & ~1u);
        dos16_set_command_tail(rt_, task_.command_tail);
        task_.psp = dos16_psp(rt_);
        rt_.local().note_dgroup(m->dgroup, uint16_t(top));
      } else {
        rt_.local().note_dgroup(m->dgroup, uint16_t(static_end));
      }
    }

    // Prolog patching (ABI.md §3.6): exported entries of a SINGLEDATA DLL,
    // and of the task (one instance: what MakeProcInstance's thunk loaded).
    if (m->dgroup && ((h.flags & ne::mod_singledata) || task)) {
      int patched = 0;
      for (const ne::Entry& e : img->entries()) {
        if (!e.exported() || e.segment == 0 || e.segment >= 0xFE || e.segment > m->seg_sel.size()) continue;
        if (img->segment(e.segment).is_data()) continue;
        uint32_t a = m->seg_base[e.segment - 1] + e.offset;
        if (!rt_.mem().exists(a, 3)) continue;
        uint8_t b0 = rt_.mem().read_u8(a), b1 = rt_.mem().read_u8(a + 1), b2 = rt_.mem().read_u8(a + 2);
        if ((b0 == 0x1E && b1 == 0x58 && b2 == 0x90) || (b0 == 0x8C && b1 == 0xD8 && b2 == 0x90)) {
          const uint8_t mov[3] = {0xB8, uint8_t(m->dgroup), uint8_t(m->dgroup >> 8)};
          rt_.mem().memcpy(a, mov, 3);
          patched++;
        }
      }
      if (patched) trace("mod16", "%s: %d exported prologs patched to mov ax,%04X", m->file.c_str(), patched, m->dgroup);
    }
  } catch (const loader::LoaderError& e) {
    log("win16: %s: %s", host_path.c_str(), e.what());
    if (err) *err = 11;
    unload(m);
    return nullptr;
  }

  if (!initialize(m)) {
    log("win16: %s failed to initialize", m->file.c_str());
    if (err) *err = 20;
    unload(m);
    return nullptr;
  }
  return m;
}

bool ModuleTable16::initialize(Module16* m) {
  if (m->initialized) return true;
  m->initialized = true;
  const ne::Header& h = m->image->header();
  if (h.cs && h.cs <= m->seg_sel.size()) {
    // LibEntry: DI = hInstance, DS = DGROUP (hInstance), CX = heap size,
    // ES:SI = the command line.
    uint32_t cmd = (uint32_t(rt_.sys_sel()) << 16) | uint16_t(layout::kSysPsp + 0x81);
    Regs16In in;
    in.di = m->hinstance;
    in.ds = m->dgroup ? m->dgroup : uint16_t(0);
    in.cx = h.heap_size;
    in.es = uint16_t(cmd >> 16);
    in.si = uint16_t(cmd);
    uint32_t proc = (uint32_t(m->seg_sel[h.cs - 1]) << 16) | h.ip;
    uint32_t r = rt_.call_far(proc, {}, &in);
    trace("mod16", "%s: LibEntry -> %04X", m->file.c_str(), r & 0xFFFF);
    if (!(r & 0xFFFF)) return false;
  }
  // A Windows 4.0 DLL's DllEntryPoint. Windows 95's KERNEL called it for
  // such DLLs; OLDMOD16 relies on it (it has no LibEntry at all).
  if (h.expected_version >= 0x0400) {
    if (auto ord = m->image->find_ordinal("DLLENTRYPOINT")) {
      uint32_t proc = entry_far(m, *ord);
      if (proc) {
        uint32_t r = rt_.call_far(proc, {l16(1), w16(m->hinstance), w16(m->dgroup), w16(h.heap_size), l16(0), w16(0)});
        trace("mod16", "%s: DllEntryPoint(DLL_PROCESS_ATTACH) -> %04X", m->file.c_str(), r & 0xFFFF);
        m->dll_entry = proc;
        if (!(r & 0xFFFF)) return false;
      }
    }
  }
  return true;
}

bool ModuleTable16::free(Module16* m) {
  if (!m) return false;
  if (m->system) return true;
  if (--m->refs > 0) return true;
  unload(m);
  return true;
}

void ModuleTable16::free_all() {
  // A module goes before what it imports, so a DLLENTRYPOINT(0) still finds
  // its imports loaded. Load order does not give that: a module is entered
  // in the table before its imports are loaded (they come after it), so pick
  // the newest module nothing else still imports; a cycle falls back to the
  // newest.
  while (true) {
    Module16* victim = nullptr;
    Module16* newest = nullptr;
    for (auto it = modules_.rbegin(); it != modules_.rend() && !victim; ++it) {
      Module16* m = it->get();
      if (m->system) continue;
      if (!newest) newest = m;
      bool imported = std::any_of(modules_.begin(), modules_.end(), [&](const auto& o) {
        return o.get() != m && std::find(o->deps.begin(), o->deps.end(), m) != o->deps.end();
      });
      if (!imported) victim = m;
    }
    if (!victim) victim = newest;
    if (!victim) break;
    victim->refs = 0;
    unload(victim);
  }
}

void ModuleTable16::unload(Module16* m) {
  if (m->dll_entry) {
    uint32_t proc = m->dll_entry;
    m->dll_entry = 0;
    try {
      rt_.call_far(proc, {l16(0), w16(m->hinstance), w16(m->dgroup), w16(m->image->header().heap_size), l16(0), w16(0)});
    } catch (const std::exception& e) {
      log("win16: %s: DllEntryPoint(DLL_PROCESS_DETACH): %s", m->file.c_str(), e.what());
    }
  }
  std::vector<Module16*> deps = std::move(m->deps);
  m->deps.clear();
  if (task_.module == m) task_ = Task16{};
  if (m->dgroup) rt_.local().forget(m->dgroup);
  rt_.global().free_owned(m->hmodule);
  auto it = std::find_if(modules_.begin(), modules_.end(), [&](auto& p) { return p.get() == m; });
  if (it != modules_.end()) modules_.erase(it);
  for (Module16* d : deps) {
    if (std::any_of(modules_.begin(), modules_.end(), [&](auto& p) { return p.get() == d; })) free(d);
  }
}

uint32_t ModuleTable16::entry_far(Module16* m, uint16_t ordinal) {
  const ne::Entry* e = m->image->find_entry(ordinal);
  if (!e || !e->segment) return 0;
  if (e->segment == 0xFE) return e->offset;  // a constant
  if (e->segment > m->seg_sel.size()) return 0;
  return (uint32_t(m->seg_sel[e->segment - 1]) << 16) | e->offset;
}

uint32_t ModuleTable16::proc_address(Module16* m, std::string_view name) {
  if (!m) return 0;
  if (m->system) {
    Shim16Entry* e = rt_.shims().find_name(m->name, name);
    if (!e || e->conv == Conv16::variable || !e->by_name) return 0;
    // A constant export comes back as its value in AX: ADXPL300 fetches
    // __AHINCR with GetProcAddress(GetModuleHandle("KERNEL"), "__AHINCR").
    if (e->conv == Conv16::equate) return equate_value(*e) & 0xFFFF;
    return rt_.thunk_far(*e);
  }
  auto ord = m->image->find_ordinal(name);
  return ord ? entry_far(m, *ord) : 0;
}

uint32_t ModuleTable16::proc_address(Module16* m, uint16_t ordinal) {
  if (!m) return 0;
  if (m->system) {
    Shim16Entry* e = rt_.shims().find(m->name, ordinal);
    if (!e || e->conv == Conv16::variable) return 0;
    if (e->conv == Conv16::equate) return equate_value(*e) & 0xFFFF;
    return rt_.thunk_far(*e);
  }
  return entry_far(m, ordinal);
}

const ne::Resource* ModuleTable16::find_resource(Module16* m, const loader::ResId& type, const loader::ResId& name) {
  if (!m || !m->image) return nullptr;
  if (const ne::Resource* r = m->image->find_resource(type, name)) return r;
  if (!type.is_string && !name.is_string) return nullptr;
  // Windows 3.0 resource compilers stored string types and names as integers
  // and recorded the strings in a NAMETABLE resource (type 15): entries of
  // WORD size, WORD type, WORD id, then the type's and the name's strings —
  // a field with its high bit set is the one whose string is given (DOMINOES,
  // BORIS, LUNATIC, MOWIN: "DIB", "PAL", "RESINFO", "CLICKSND"…).
  const ne::Resource* nt = nullptr;
  for (const ne::Resource& r : m->image->resources()) {
    if (!r.type.is_string && r.type.num == loader::rt::nametable) nt = &r;
  }
  if (!nt) return nullptr;
  std::string_view d = m->image->resource_data(*nt);
  auto u16 = [&](size_t off) { return off + 2 <= d.size() ? uint16_t(uint8_t(d[off]) | (uint8_t(d[off + 1]) << 8)) : 0; };
  auto cstr = [&](size_t& off) {
    size_t e = d.find('\0', off);
    if (e == std::string_view::npos) e = d.size();
    std::string s(d.substr(off, e - off));
    off = e + 1;
    return s;
  };
  loader::ResId t = type, n = name;
  for (int pass = 0; pass < 2; pass++) {
    for (size_t p = 0; p + 6 < d.size();) {
      uint16_t size = u16(p);
      if (size < 6) break;
      uint16_t ty = u16(p + 2), id = u16(p + 4);
      size_t q = p + 6;
      std::string ts = cstr(q), ns = cstr(q);
      p += size;
      // Pass 0 resolves the type (its string names it), pass 1 the name
      // within that type.
      if (pass == 0) {
        if (t.is_string && (ty & 0x8000) && ieq16(ts, t.str)) t = loader::ResId::of(uint16_t(ty & 0x7FFF));
      } else if (n.is_string && (id & 0x8000) && ieq16(ns, n.str)) {
        bool type_ok = t.is_string ? ((ty & 0x8000) && ieq16(ts, t.str)) : (uint16_t(ty & 0x7FFF) == t.num);
        if (type_ok) n = loader::ResId::of(uint16_t(id & 0x7FFF));
      }
    }
  }
  return m->image->find_resource(t, n);
}

uint16_t ModuleTable16::fixed_selector(uint32_t linear, const char* tag) {
  for (auto& [lin, sel] : fixed_sels_) {
    if (lin == linear) return sel;
  }
  // The real-mode windows (A000h, B800h, F000h, …) have no hardware behind
  // them here: a zeroed 64 KiB block keeps code that peeks at them harmless.
  GlobalBlock* b = rt_.global().alloc_block(0x10000, false, 0, 0);
  if (!b) return 0;
  b->owner = 0xFFFF;
  rt_.ldt().set_tag(b->sel, tag);
  fixed_sels_.push_back({linear, b->sel});
  return b->sel;
}

ne::FarPtr ModuleTable16::resolve_import(std::string_view module, uint16_t ordinal, std::string_view name) {
  Shim16Registry& shims = rt_.shims();
  Shim16Entry* e = !name.empty() ? shims.find_name(module, name) : shims.find(module, ordinal);
  if (!e) {
    if (!name.empty()) {
      e = &shims.add(module, uint16_t(0xF000 + (std::hash<std::string_view>{}(name) & 0x0FFF)), name, Conv16::stub,
                     true, -1);
    } else {
      e = &shims.get(module, ordinal);
    }
  }
  if (e->conv == Conv16::equate) {
    // A constant serves as a selector (SELECTOR fixups: __0040H) and as an
    // offset (OFFSET fixups: `mov ax, __AHINCR`).
    uint32_t v = equate_value(*e);
    return ne::FarPtr{uint16_t(v), v & 0xFFFF};
  }
  return ne::FarPtr{rt_.thunk_sel(), rt_.shims().thunk_offset(*e)};
}

uint32_t ModuleTable16::equate_value(const Shim16Entry& e) {
  const std::string& n = e.name;
  if (n == "__0040H") return Ldt::kBiosSelector;
  if (n == "__WINFLAGS") return kWinFlags;
  if (n == "__A000H") return fixed_selector(0xA0000, "__A000H");
  if (n == "__B000H") return fixed_selector(0xB0000, "__B000H");
  if (n == "__B800H") return fixed_selector(0xB8000, "__B800H");
  if (n == "__C000H") return fixed_selector(0xC0000, "__C000H");
  if (n == "__D000H") return fixed_selector(0xD0000, "__D000H");
  if (n == "__E000H") return fixed_selector(0xE0000, "__E000H");
  if (n == "__F000H" || n == "__ROMBIOS") return fixed_selector(0xF0000, "__F000H");
  if (n == "__0000H") return fixed_selector(0x00000, "__0000H");
  return uint32_t(e.value);
}

ne::FarPtr ModuleTable16::resolve(Module16* m, const ne::Target& t) {
  if (t.kind == ne::TargetKind::internal) {
    if (t.segment == 0 || t.segment > m->seg_sel.size()) return ne::FarPtr{};
    return ne::FarPtr{m->seg_sel[t.segment - 1], t.offset};
  }
  std::string mod = upper(t.module);
  std::string_view name = t.kind == ne::TargetKind::import_name ? t.name : std::string_view{};
  if (rt_.shims().has_module(mod)) return resolve_import(mod, t.ordinal, name);
  Module16* dep = by_name(mod);
  uint32_t fp = 0;
  if (dep && !dep->system) {
    if (!name.empty()) {
      if (auto ord = dep->image->find_ordinal(name)) fp = entry_far(dep, *ord);
    } else {
      fp = entry_far(dep, t.ordinal);
    }
  }
  if (fp) return ne::FarPtr{uint16_t(fp >> 16), fp & 0xFFFF};
  // Unresolvable: bind a thunk that stops the lane with the name when it is
  // called (the import's argument list is unknown, so it cannot return).
  std::string label = mod + "." + (name.empty() ? std::to_string(t.ordinal) : std::string(name));
  log("win16: %s imports %s, which does not exist", m->file.c_str(), label.c_str());
  uint16_t off = rt_.shims().internal_thunk("unresolved " + label, [label](Call16&) {
    throw GuestError16(GuestError16::Kind::fatal, "call to unresolved import " + label);
  });
  return ne::FarPtr{rt_.thunk_sel(), off};
}

}  // namespace adw::win16
