#include "win16/runtime16.hh"

#include <windows.h>

#include <algorithm>
#include <cinttypes>
#include <cstdio>

#include "adw/core/log.h"
#include "win16/modules16.hh"
#include "win32/display.hh"
#include "win32/heap.hh"
#include "win32/vfs.hh"

namespace adw::win16 {

namespace {

using X86 = cpu::X86Emulator;
using SegReg = X86::SegReg;

constexpr uint8_t kThunkVector = 0xFE;

// The headless clock's wall time: the same fixed FILETIME the Win32 lane uses
// (win32/kernel32.cc kHeadlessEpoch), so both lanes show one moment
// (20:00:00 as FileTimeToSystemTime reads it).
constexpr uint64_t kHeadlessEpoch = 125132976000000000ull;

// VGA timing for the 0x3DA retrace bit: a 70 Hz frame, the last ~1.4 ms of
// which is vertical retrace (640x480 mode's figures, rounded).
constexpr uint64_t kVgaFrameUs = 14286;
constexpr uint64_t kVgaRetraceUs = 1400;

std::string hex16(uint16_t v) {
  char b[8];
  snprintf(b, sizeof(b), "%04X", v);
  return b;
}

// What EFLAGS a restored snapshot may set: the arithmetic flags and DF.
constexpr uint32_t kUserFlags = 0x0CD5;

}  // namespace

Runtime16::Runtime16(const Runtime16Options& opts, VirtualClock& clock, const InputState* input)
    : opts_(opts), clock_(clock), input_(input) {
  if (opts_.arena_size < (4u << 20) || opts_.arena_size > layout::kMaxArenaSize) {
    throw std::invalid_argument("win16 arena size out of range");
  }
  if (opts_.tick_quantum_ms == 0) opts_.tick_quantum_ms = 1;
  mem_ = std::make_shared<cpu::MemoryContext>();
  cpu_ = std::make_unique<X86>(mem_);
  build_machine();
  arena_ = std::make_unique<win32::GuestHeap>(*mem_, layout::kArenaBase, opts_.arena_size);
  global_ = std::make_unique<GlobalHeap16>(*mem_, *arena_, ldt_);
  // A block that moved (GlobalReAlloc) has new descriptors: segment
  // registers holding its selectors must see them, as reloading them would.
  global_->set_on_moved([this] { cpu_->reload_segments(); });
  local_ = std::make_unique<LocalHeaps16>(*mem_, ldt_);
  shims_ = std::make_unique<Shim16Registry>(*mem_, layout::kThunkBase);
  vfs_ = std::make_unique<win32::Vfs>();
  modules_ = std::make_unique<ModuleTable16>(*this);

  cpu_->set_descriptor_provider(&ldt_);
  cpu_->set_interrupt_handler([this](X86& c, uint8_t v) { on_interrupt(c, v); });
  cpu_->set_fault_handler([this](X86& c, const X86::Fault& f) { on_fault(c, f); });
  cpu_->set_port_handler([this](X86&, uint16_t port, uint8_t size, bool w, uint32_t v) {
    return on_port(port, size, w, v);
  });
  cpu_->load_segment(SegReg::SS, stack_sel_);
  cpu_->load_segment(SegReg::DS, sys_sel_);
  cpu_->load_segment(SegReg::ES, sys_sel_);
  cpu_->set_segment_null(SegReg::FS);
  cpu_->set_segment_null(SegReg::GS);
  cpu_->set_cs_eip(thunk_sel_, 0);
  cpu_->registers().w_esp(layout::kInitialSp);
  prof_ = tracing("prof16");
  if (opts_.step_cs) {
    cpu_->set_debug_hook([this](X86& c) {
      auto& r = c.registers();
      if ((c.get_segment(SegReg::CS) | 3) != (opts_.step_cs | 3) || r.eip < opts_.step_lo || r.eip >= opts_.step_hi) return;
      log("step %04X:%04X ax=%04X bx=%04X cx=%04X dx=%04X si=%04X di=%04X bp=%04X sp=%04X ds=%04X es=%04X fl=%04X",
          c.get_segment(SegReg::CS), r.eip, r.r_ax(), r.r_bx(), r.r_cx(), r.r_dx(), r.r_si(), r.r_di(), r.r_bp(),
          r.r_sp(), c.get_segment(SegReg::DS), c.get_segment(SegReg::ES), r.read_eflags() & 0xFFFF);
    });
  }
  if (opts_.watch) {
    // A software watchpoint: compare the watched byte before every
    // instruction and blame the previous one for a change.
    auto last = std::make_shared<int>(-1);
    auto prev_cs = std::make_shared<uint16_t>(0);
    auto prev_ip = std::make_shared<uint32_t>(0);
    cpu_->set_debug_hook([this, last, prev_cs, prev_ip](X86& c) {
      const cpu::SegDesc* d = ldt_.get(uint16_t(opts_.watch >> 16));
      int v = -1;
      if (d && d->present && (opts_.watch & 0xFFFF) <= d->limit) v = mem_->read_u8(d->base + (opts_.watch & 0xFFFF));
      if (v != *last) {
        log("watch %04X:%04X: %d -> %d by %s", opts_.watch >> 16, opts_.watch & 0xFFFF, *last, v,
            describe(*prev_cs, *prev_ip).c_str());
        *last = v;
      }
      *prev_cs = c.get_segment(SegReg::CS);
      *prev_ip = c.registers().eip;
    });
  }
}

Runtime16::~Runtime16() {
  // Family state first (it may own real GDI objects selected into DCs over the
  // display's surface), then the display, then the address space.
  states_.clear();
  display_.reset();
}

void Runtime16::build_machine() {
  auto& mem = *mem_;
  auto seg = [&](uint32_t base, bool code) {
    uint16_t sel = ldt_.alloc(1);
    ldt_.set(sel, cpu::SegDesc{base, 0xFFFF, true, code, true, false, 3});
    return sel;
  };

  // BIOS data area, selector 0x40 (GDT).
  mem.allocate_at(layout::kBdaBase, layout::kBdaSize);
  mem.memset(layout::kBdaBase, 0, layout::kBdaSize);
  ldt_.set_bios_area(layout::kBdaBase, 0xFFFF);
  mem.write_u16l(layout::kBdaBase + 0x10, 0x0027);  // equipment: FPU present, VGA, 2 floppies… (bit 1 = x87)
  mem.write_u16l(layout::kBdaBase + 0x13, 640);     // KB of base memory
  mem.write_u8(layout::kBdaBase + 0x49, 0x03);      // video mode (text; Windows owns the adapter)
  mem.write_u16l(layout::kBdaBase + 0x4A, 80);      // columns
  mem.write_u8(layout::kBdaBase + 0x84, 24);        // rows - 1

  // System segment: environment, PSP, DTA, host strings.
  mem.allocate_at(layout::kSysBase, layout::kSysSize);
  mem.memset(layout::kSysBase, 0, layout::kSysSize);
  sys_sel_ = seg(layout::kSysBase, false);
  ldt_.set_tag(sys_sel_, "system segment");
  {
    // The DOS environment block, then (DOS 3+) a word count and the program
    // path — C runtimes read argv[0] from there.
    std::string env;
    env += "COMSPEC=C:\\COMMAND.COM";
    env.push_back('\0');
    env += "PATH=" + opts_.windows_dir + ";" + opts_.system_dir + ";" + opts_.guest_dir;
    env.push_back('\0');
    env += "TEMP=" + opts_.windows_dir + "\\TEMP";
    env.push_back('\0');
    env += "windir=" + opts_.windows_dir;
    env.push_back('\0');
    env.push_back('\0');
    env.push_back('\1');
    env.push_back('\0');
    env += opts_.system_dir + "\\KRNL386.EXE";
    env.push_back('\0');
    mem.memcpy(layout::kSysBase + layout::kSysEnvironment, env.data(), env.size());
    // A PSP: INT 20h at 0, the environment selector at 2Ch, an empty command tail.
    uint32_t psp = layout::kSysBase + layout::kSysPsp;
    mem.write_u16l(psp + 0x00, 0x20CD);
    mem.write_u16l(psp + 0x02, 0xA000);
    mem.write_u16l(psp + 0x2C, sys_sel_);
    mem.write_u8(psp + 0x80, 0);
    mem.write_u8(psp + 0x81, 0x0D);
  }

  // Thunk segment: `int3` everywhere, the call_far sentinel at offset 0.
  mem.allocate_at(layout::kThunkBase, layout::kThunkSize);
  mem.memset(layout::kThunkBase, 0xCC, layout::kThunkSize);
  const uint8_t sentinel[4] = {0xCD, kThunkVector, 0x00, 0x00};
  mem.memcpy(layout::kThunkBase, sentinel, sizeof(sentinel));
  thunk_sel_ = seg(layout::kThunkBase, true);
  ldt_.set_tag(thunk_sel_, "thunk segment");

  // Task stack, with the instance-data words compiler stack probes read.
  mem.allocate_at(layout::kStackBase, layout::kStackSize);
  mem.memset(layout::kStackBase, 0, layout::kStackSize);
  stack_sel_ = seg(layout::kStackBase, false);
  ldt_.set_tag(stack_sel_, "task stack");
  mem.write_u16l(layout::kStackBase + 0x0A, layout::kStackTop);     // pStackTop
  mem.write_u16l(layout::kStackBase + 0x0C, layout::kStackTop);     // pStackMin
  mem.write_u16l(layout::kStackBase + 0x0E, layout::kStackBottom);  // pStackBottom
}

win32::Display& Runtime16::attach_display(Screen& screen) {
  if (display_) throw std::logic_error("display already attached");
  uint32_t size = win32::Display::bits_size(screen.width(), screen.height());
  uint32_t bits = arena_->alloc(size, /*zero=*/true, 0x10000);
  if (!bits) throw std::runtime_error("the win16 arena cannot hold the screen surface");
  display_ = std::make_unique<win32::Display>(screen, *mem_, bits);
  if (opts_.desktop_palette) {
    // 216 colour-cube entries (0, 51, …, 255 per channel) and 20 greys
    // between the cube's own greys: 236 distinct non-static colours.
    std::vector<PALETTEENTRY> pe;
    for (int r = 0; r < 6; r++)
      for (int g = 0; g < 6; g++)
        for (int b = 0; b < 6; b++) pe.push_back(PALETTEENTRY{BYTE(51 * r), BYTE(51 * g), BYTE(51 * b), 0});
    for (int v = 8; pe.size() < 236; v += 12) pe.push_back(PALETTEENTRY{BYTE(v), BYTE(v), BYTE(v), 0});
    display_->set_system_entries(10, int(pe.size()), pe.data());
  }
  return *display_;
}

uint32_t Runtime16::thunk_far(Shim16Entry& e) { return (uint32_t(thunk_sel_) << 16) | shims_->thunk_offset(e); }

uint32_t Runtime16::static_bytes(const std::string& key, std::string_view bytes) {
  auto it = strings_.find(key);
  if (it != strings_.end()) return it->second;
  uint32_t size = (uint32_t(bytes.size()) + 2 + 3) & ~3u;
  if (uint32_t(strings_next_) + size > layout::kSysSize) throw std::runtime_error("win16 system segment is full");
  uint32_t fp = (uint32_t(sys_sel_) << 16) | strings_next_;
  mem_->memcpy(layout::kSysBase + strings_next_, bytes.data(), bytes.size());
  mem_->write_u8(layout::kSysBase + strings_next_ + uint32_t(bytes.size()), 0);
  strings_next_ = uint16_t(strings_next_ + size);
  strings_[key] = fp;
  return fp;
}

// ---- guest memory --------------------------------------------------------------------------------

uint32_t Runtime16::linear(uint16_t sel, uint32_t off, uint32_t size) const {
  const cpu::SegDesc* d = ldt_.get(sel);
  if (!d || !d->present) {
    throw GuestError16(GuestError16::Kind::fault,
                       "invalid far pointer " + hex16(sel) + ":" + hex16(uint16_t(off)) +
                           (sel ? " (no such selector)" : " (NULL)"));
  }
  if (size && uint64_t(off) + size - 1 > d->limit) {
    throw GuestError16(GuestError16::Kind::fault, "far pointer " + hex16(sel) + ":" + hex16(uint16_t(off)) + " + " +
                                                     std::to_string(size) + " runs past its segment's limit");
  }
  return d->base + off;
}

void Runtime16::read_bytes(uint32_t fp, void* out, size_t n) const {
  if (!n) return;
  mem_->memcpy(out, linear(fp, uint32_t(n)), n);
}

void Runtime16::write_bytes(uint32_t fp, const void* data, size_t n) {
  if (!n) return;
  mem_->memcpy(linear(fp, uint32_t(n)), data, n);
}

std::string Runtime16::read_str(uint32_t fp, size_t max) const {
  std::string s;
  if (!fp) return s;
  uint16_t sel = uint16_t(fp >> 16);
  uint32_t off = fp & 0xFFFF;
  const cpu::SegDesc* d = ldt_.get(sel);
  if (!d || !d->present) linear(fp);  // throws
  for (size_t i = 0; i < max && off + i <= d->limit; i++) {
    char c = char(mem_->read_u8(d->base + off + uint32_t(i)));
    if (!c) break;
    s.push_back(c);
  }
  return s;
}

size_t Runtime16::write_str(uint32_t fp, std::string_view s, size_t cap) {
  if (!cap || !fp) return 0;
  size_t n = std::min(s.size(), cap - 1);
  if (n) write_bytes(fp, s.data(), n);
  wr8(fp + uint32_t(n), 0);
  return n;
}

uint32_t Runtime16::huge_add(uint32_t fp, uint32_t n) {
  uint32_t off = (fp & 0xFFFF) + n;
  uint16_t sel = uint16_t((fp >> 16) + ((off >> 16) << Ldt::kAhShift));
  return (uint32_t(sel) << 16) | (off & 0xFFFF);
}

// ---- time ------------------------------------------------------------------------------------------

bool Runtime16::modeled_time() const {
  // Streamed runs follow the wall clock, which already includes the time the
  // emulation took; headless ones model it. Before the first frame a
  // streamed run's clock does not run yet (it follows the wall from frame 0
  // on), so init — where AD_RSRC, EINSTEIN, GLOBE and Om Appliances
  // calibrate their delay loops on the tick count — is modeled too.
  return opts_.insns_per_us && (clock_.mode() == VirtualClock::Mode::fixed_step || !frames_started_);
}

void Runtime16::start_frames() {
  if (frames_started_) return;
  // Streamed: from here on the wall clock, continuing from the time init took.
  if (opts_.insns_per_us && clock_.mode() == VirtualClock::Mode::realtime) rt_offset_ = peek_us();
  frames_started_ = true;
}

uint64_t Runtime16::work_insns() const {
  return cpu_->cycles() + api_charged_ * opts_.api_cost_insns + pix_charged_ * opts_.pixel_cost_insns;
}

uint64_t Runtime16::clock_us() {
  if (!modeled_time()) return rt_offset_ + clock_.read_us();
  uint64_t now = cpu_->cycles();
  insn_carry_ += now - clock_insns_;
  clock_insns_ = now;
  uint64_t extra = std::min<uint64_t>(insn_carry_ / opts_.insns_per_us, 1000000);
  insn_carry_ -= extra * opts_.insns_per_us;
  // The frame grid (the core clock, whose own read step the lane sets to 0)
  // is a floor, not a base: work inside a frame period does not move it.
  vt_ = std::max(vt_, clock_.now_us()) + opts_.read_step_us + extra;
  return vt_;
}

void Runtime16::settle_time() {
  if (!modeled_time()) return;
  uint64_t now = cpu_->cycles();
  insn_carry_ += now - clock_insns_;
  clock_insns_ = now;
  uint64_t extra = std::min<uint64_t>(insn_carry_ / opts_.insns_per_us, 1000000);
  insn_carry_ -= extra * opts_.insns_per_us;
  vt_ = std::max(vt_, clock_.now_us()) + extra;
}

uint64_t Runtime16::peek_us() const {
  if (!modeled_time()) return rt_offset_ + clock_.now_us();
  uint64_t pending = insn_carry_ + (cpu_->cycles() - clock_insns_);
  return std::max(vt_, clock_.now_us()) + std::min<uint64_t>(pending / opts_.insns_per_us, 1000000);
}

void Runtime16::wait_until_us(uint64_t us) {
  while (peek_us() < us) {
    // The lane ends the frame here and resumes the call on a later one.
    if (yield_fn_ && yield_fn_()) continue;
    if (modeled_time()) {
      // The work done so far first (as a clock read would see it), then the jump.
      uint64_t now = cpu_->cycles();
      insn_carry_ += now - clock_insns_;
      clock_insns_ = now;
      uint64_t extra = std::min<uint64_t>(insn_carry_ / opts_.insns_per_us, 1000000);
      insn_carry_ -= extra * opts_.insns_per_us;
      vt_ = std::max(std::max(vt_, clock_.now_us()) + extra, us);
    } else {
      rt_offset_ += us - peek_us();
    }
    break;
  }
}

uint32_t Runtime16::tick_count() {
  uint64_t ms = clock_us() / 1000;
  uint64_t q = opts_.tick_quantum_ms;
  return uint32_t(VirtualClock::kBootOffsetMs + (ms / q) * q);
}

uint64_t Runtime16::local_filetime() {
  if (!time_base_set_) {
    time_base_set_ = true;
    if (clock_.mode() == VirtualClock::Mode::fixed_step) {
      time_base_ = kHeadlessEpoch;
    } else {
      SYSTEMTIME st;
      ::GetLocalTime(&st);
      FILETIME f;
      SystemTimeToFileTime(&st, &f);
      time_base_ = ((uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime) - peek_us() * 10;
    }
  }
  return time_base_ + clock_us() * 10;
}

void Runtime16::update_bios_ticks() {
  uint64_t ft = local_filetime();
  uint64_t ms_today = (ft / 10000) % 86400000ull;
  // 1193180 / 65536 ticks per second.
  uint32_t ticks = uint32_t(ms_today * 1193180ull / 65536ull / 1000ull);
  mem_->write_u32l(layout::kBdaBase + 0x6C, ticks);
}

void Runtime16::set_carry(bool on) { cpu_->registers().replace_flag(X86::Regs::CF, on); }

// ---- host → guest ------------------------------------------------------------------------------------

Runtime16::SavedRegs Runtime16::save() const {
  SavedRegs s{};
  const auto& r = cpu_->registers();
  for (uint8_t i = 0; i < 8; i++) s.gpr[i] = r.read32(i);
  s.eip = r.eip;
  s.eflags = r.read_eflags();
  for (uint8_t i = 0; i < 6; i++) {
    auto seg = static_cast<SegReg>(i);
    s.sel[i] = cpu_->get_segment(seg);
    s.desc[i] = cpu_->get_segment_desc(seg);
    s.null_seg[i] = (s.sel[i] & ~3) == 0 && seg != SegReg::CS && seg != SegReg::SS;
  }
  return s;
}

void Runtime16::restore(const SavedRegs& s) {
  auto& r = cpu_->registers();
  for (uint8_t i = 0; i < 8; i++) r.write32(i, s.gpr[i]);
  r.eip = s.eip;
  r.write_eflags((r.read_eflags() & ~kUserFlags) | (s.eflags & kUserFlags));
  // Raw loads of the saved selectors AND their cached descriptors: a
  // selector freed during the callback (FreeLibrary in a window procedure)
  // must not fault the interrupted caller, whose cache still holds it.
  for (uint8_t i = 0; i < 6; i++) {
    auto seg = static_cast<SegReg>(i);
    if (s.null_seg[i]) cpu_->set_segment_null(seg);
    else cpu_->set_segment(seg, s.sel[i], s.desc[i]);
  }
}

uint32_t Runtime16::call_far(uint32_t proc, std::span<const Arg16> args, const Regs16In* in) {
  if (!proc) throw GuestError16(GuestError16::Kind::fatal, "far call to a NULL procedure");
  SavedRegs saved = save();
  auto& r = cpu_->registers();
  const cpu::SegDesc& ss = cpu_->get_segment_desc(SegReg::SS);
  uint16_t sp = r.r_sp();
  auto push = [&](uint16_t v) {
    sp = uint16_t(sp - 2);
    if (uint32_t(sp) + 1 > ss.limit) throw GuestError16(GuestError16::Kind::fatal, "task stack overflow in call_far");
    mem_->write_u16l(ss.base + sp, v);
  };
  for (const Arg16& a : args) {
    if (a.dword) {
      push(uint16_t(a.v >> 16));
      push(uint16_t(a.v));
    } else {
      push(uint16_t(a.v));
    }
  }
  push(thunk_sel_);  // far return address: the sentinel
  push(0);
  r.w_esp(sp);
  if (in) {
    if (in->ax) r.w_ax(*in->ax);
    if (in->bx) r.w_bx(*in->bx);
    if (in->cx) r.w_cx(*in->cx);
    if (in->dx) r.w_dx(*in->dx);
    if (in->si) r.w_si(*in->si);
    if (in->di) r.w_di(*in->di);
  }
  try {
    if (in && in->ds) {
      if (*in->ds & ~3) cpu_->load_segment(SegReg::DS, *in->ds);
      else cpu_->set_segment_null(SegReg::DS);
    }
    if (in && in->es) {
      if (*in->es & ~3) cpu_->load_segment(SegReg::ES, *in->es);
      else cpu_->set_segment_null(SegReg::ES);
    }
    cpu_->set_cs_eip(uint16_t(proc >> 16), proc & 0xFFFF);
  } catch (const X86::fault_error& e) {
    restore(saved);
    throw GuestError16(GuestError16::Kind::fault, std::string("call_far setup: ") + e.what());
  }

  if (depth_ == 0) {
    budget_end_ = cpu_->cycles() + opts_.call_budget;
    fault_state_.clear();
  }
  uint64_t now = cpu_->cycles();
  uint64_t budget = budget_end_ > now ? budget_end_ - now : 0;
  depth_++;
  X86::StopReason why;
  try {
    why = cpu_->run_until(thunk_sel_, 0, budget);
  } catch (const GuestUnwind16&) {
    // A Throw to a Catch further out: this level's registers go back to the
    // caller's (the shim that called us), which the target level then
    // overwrites from the CATCHBUF.
    depth_--;
    restore(saved);
    if (depth_ == 0) {
      throw GuestError16(GuestError16::Kind::fatal, "Throw to a Catch buffer whose call level no longer exists");
    }
    throw;
  } catch (...) {
    // Every level puts the machine back as it found it, so the host can call
    // into the guest again (the lane's UNLOADADMODULE16 after a fault); the
    // innermost level first records where the guest was, for log_state().
    if (fault_state_.empty()) fault_state_ = state_text("at the failure", true);
    depth_--;
    restore(saved);
    throw;
  }
  depth_--;
  if (why != X86::StopReason::ADDRESS) {
    std::string where = describe(cpu_->get_segment(SegReg::CS), r.eip);
    if (fault_state_.empty()) fault_state_ = state_text("at the stop", true);
    restore(saved);
    throw GuestError16(GuestError16::Kind::hang, "far call to " + describe(uint16_t(proc >> 16), proc & 0xFFFF) +
                                                     " did not return within " + std::to_string(opts_.call_budget) +
                                                     " instructions (stopped at " + where + ")");
  }
  uint32_t result = (uint32_t(r.r_dx()) << 16) | r.r_ax();
  last_sp_after_ = r.r_sp();
  restore(saved);
  return result;
}

// ---- traps ---------------------------------------------------------------------------------------

void Runtime16::on_interrupt(X86& cpu, uint8_t vector) {
  if (vector == kThunkVector) {
    uint32_t lin = cpu.get_segment_desc(SegReg::CS).base + cpu.registers().eip;
    dispatch_thunk(mem_->read_u16l(lin));
    return;
  }
  auto it = ints_.find(vector);
  if (it != ints_.end()) {
    it->second(*this);
    null_freed_segments(nullptr, vector);  // DPMI's Free LDT Descriptor (INT 31h AX=0001h)
    return;
  }
  if (!unknown_int_reported_[vector]) {
    unknown_int_reported_[vector] = true;
    log("win16: INT %02Xh (AX=%04X) at %s ignored", vector, cpu.registers().r_ax(),
        describe(cpu.get_segment(SegReg::CS), cpu.registers().eip - 2).c_str());
  }
}

void Runtime16::check_deadline() {
  if (!deadline_fn_ || peek_us() < deadline_us_) return;
  std::function<void()> fn = std::move(deadline_fn_);
  deadline_fn_ = nullptr;
  fn();
}

// The freed-selector rule. A call that frees a selector the caller holds in
// DS, ES, FS or GS returns with that register null, so the caller's own later
// push/pop of the register loads the null selector instead of faulting. One
// check, here, as every API call (dispatch_thunk) and every software
// interrupt (on_interrupt) returns, whatever freed the selector: GlobalFree,
// FreeSelector, FreeResource, FreeLibrary, a GlobalReAlloc that gave a block
// new selectors, DPMI's Free LDT Descriptor, or a callback the call made.
// Nothing else changes: null registers, the GDT's 0x40 and live selectors
// stay, and a selector pushed before the call and popped after it still
// faults — the rule nulls registers as a call returns, it fixes no load.
//
// What Windows did (research/win/pkg/more/l2/FREED_SELECTOR_RULE.md):
//   * Windows 3.1's GlobalFree (KRNL386 3GINTERF.OBJ, pseudocode in Pietrek,
//     Windows Internals, 1993, ch. 2) zeroes the caller's DS it saved on
//     entry when that DS is the block being freed, "so that we don't GP fault
//     when we restore the value from the stack": it returns with DS = 0.
//     ES is scratch in the Win16 convention and KERNEL's heap code used it
//     for its own selectors; Borland C++'s far-heap free, which pushes and
//     pops ES right after GlobalFree, ran on Windows only because ES never
//     came back holding the freed selector (the next point).
//   * KERNEL's fault handler recovered faults only in the system DLLs' __GP
//     ranges (Undocumented Windows ch. 5): a freed selector loaded by an
//     application's own code was an Unrecoverable Application Error. Hence
//     no fault-time fixup here.
//   * DPMI 1.0's Free LDT Descriptor (INT 31h 0001h; 0101h and 0102h for DOS
//     blocks): "any segment registers which contain the selector being freed
//     are zeroed by this function".
//   * Wine nulls a freed DS/ES/FS/GS its 16-bit relay restores as every API
//     call returns (dlls/krnl386.exe16/wowthunk.c fix_selector; in 2000
//     memory/instr.c: "Saved selector may have become invalid when the relay
//     code tries to restore it. We simply clear it.") and fixes no load in
//     the application's code but selector 0x40.
// It serves DECO.DLL (Marvel Comics Screen Posters), which frees its work
// buffer with DS still holding it (GlobalFree at DECO 6:A0F2, DS loaded at
// 6:9FB0) and later saves and restores DS (push ds 7:002D .. pop ds 7:008A),
// and Borland C++'s far-heap free (the Snoopy modules, IS_FLY 1:6B71), which
// frees the segment in ES and then pushes and pops ES (1:6B16..1:6B20).
void Runtime16::null_freed_segments(const Shim16Entry* by, uint8_t vector) {
  for (SegReg s : {SegReg::DS, SegReg::ES, SegReg::FS, SegReg::GS}) {
    uint16_t sel = cpu_->get_segment(s);
    if (!(sel & ~3) || !Ldt::is_ldt(sel) || ldt_.in_use(sel)) continue;
    cpu_->set_segment_null(s);
    if (tracing("mem16")) {
      static const char* const kNames[6] = {"ES", "CS", "SS", "DS", "FS", "GS"};
      char intr[16];
      snprintf(intr, sizeof(intr), "INT %02Xh", vector);
      trace("mem16", "%s %04X was freed by %s: null on return", kNames[uint8_t(s)], sel,
            by ? by->label().c_str() : intr);
    }
  }
}

void Runtime16::dispatch_thunk(uint16_t id) {
  Shim16Entry* e = shims_->by_thunk(id);
  if (!e) {
    throw GuestError16(GuestError16::Kind::fatal, id == 0 ? "guest code jumped to the host-call sentinel"
                                                          : "call through an unknown win16 thunk id " + std::to_string(id));
  }
  if (scanout_) {
    // Before the call draws anything: the screen as it stands is what every
    // refresh since the previous API call showed.
    uint64_t t = peek_us();
    if (t >= next_scanout_us_) {
      next_scanout_us_ = (t / kVgaFrameUs + 1) * kVgaFrameUs;
      scanout_();
    }
  }
  check_deadline();
  // Audio callbacks due by now (sound16.cc), before the call does anything.
  if (audio_fn_ && peek_us() >= audio_due_) audio_fn_();
  auto& r = cpu_->registers();
  uint16_t sp = r.r_sp();
  Call16 c(*this, *e, sp);
  e->calls++;
  insn_carry_ += opts_.api_cost_insns;
  api_charged_++;
  uint16_t rip = c.ret_ip(), rcs = c.ret_cs();

  bool traced = tracing("api16");
  std::string args;
  if (traced) {
    int n = std::max(e->arg_bytes, 0) / 2;
    for (int i = n; i-- > 0;) {
      char b[12];
      snprintf(b, sizeof(b), "%s%04X", args.empty() ? "" : " ", c.stack16(uint32_t(2 * i)));
      args += b;
    }
  }

  if (e->fn) {
    try {
      if (prof_) {
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        e->fn(c);
        QueryPerformanceCounter(&t1);
        e->host_ticks += uint64_t(t1.QuadPart - t0.QuadPart);
      } else {
        e->fn(c);
      }
    } catch (const GuestUnwind16& u) {
      // A callback this shim made Threw to a Catch at this level: resume
      // there instead of returning from the shim (GuestUnwind16).
      if (u.depth != depth_) throw;
      u.resume(*this);
      c.take_over();
    }
  } else {
    if (!e->unimpl_reported) {
      e->unimpl_reported = true;
      write_stderr("[unimpl16] " + e->label() + (e->arg_bytes < 0 ? " (unknown signature)" : "") + "\n");
    }
    if (e->conv != Conv16::register_) c.ret(0);
  }
  if (gdi_flush_) {
    // The call drew on a surface the guest reads itself (a DIB driver DC).
    gdi_flush_ = false;
    GdiFlush();
  }

  if (traced) {
    trace("api16", "%s(%s) -> %0*X%s  [from %s]", e->label().c_str(), args.c_str(), e->ret16 ? 4 : 8, c.result(),
          c.took_over() ? " (no return)" : "", describe(rcs, rip).c_str());
  }
  // The BP chain behind every call (who called the caller): slow, for debugging.
  if (tracing("bt16") && !c.took_over()) trace("bt16", "%s  [from %s <- %s]", e->label().c_str(), describe(rcs, rip).c_str(), backtrace(12).c_str());
  null_freed_segments(e, 0);
  if (!c.took_over()) {
    int pop = e->conv == Conv16::cdecl_ ? 0 : std::max(e->arg_bytes, 0);
    r.w_sp(uint16_t(sp + 4 + pop));
    cpu_->set_cs_eip(rcs, rip);
  }
}

void Runtime16::on_fault(X86& cpu, const X86::Fault& f) {
  std::string where = describe(f.cs, f.eip);
  std::string bytes;
  try {
    uint32_t lin = cpu.get_segment_desc(SegReg::CS).base + f.eip;
    for (uint32_t i = 0; i < 6 && mem_->exists(lin + i); i++) {
      char b[4];
      snprintf(b, sizeof(b), "%s%02X", i ? " " : "", mem_->read_u8(lin + i));
      bytes += b;
    }
  } catch (const std::exception&) {
  }
  // The data segments' limits: most faults are an offset past one of them.
  std::string segs;
  for (auto [name, reg] : {std::pair{"ds", SegReg::DS}, {"es", SegReg::ES}, {"ss", SegReg::SS}}) {
    char b[48];
    snprintf(b, sizeof(b), " %s=%04X/lim %X", name, cpu.get_segment(reg), cpu.get_segment_desc(reg).limit);
    segs += b;
  }
  throw GuestError16(GuestError16::Kind::fault,
                     f.str() + " in " + where + (bytes.empty() ? "" : " [" + bytes + "]") + segs);
}

uint32_t Runtime16::on_port(uint16_t port, uint8_t size, bool is_write, uint32_t value) {
  win32::Display* d = display_.get();
  switch (port) {
    case 0x3DA:  // input status 1: bit 3 = vertical retrace, bit 0 = display disabled
    case 0x3BA:
      if (!is_write) {
        check_deadline();
        uint64_t phase = clock_us() % kVgaFrameUs;
        return phase >= kVgaFrameUs - kVgaRetraceUs ? 0x09 : 0x00;
      }
      return 0;
    case 0x3C7:  // DAC read index (write) / DAC state (read)
      if (is_write) {
        dac_read_index_ = uint8_t(value);
        dac_read_comp_ = 0;
        return 0;
      }
      return 0x03;
    case 0x3C8:  // DAC write index
      if (is_write) {
        dac_write_index_ = uint8_t(value);
        dac_write_comp_ = 0;
        return 0;
      }
      return dac_write_index_;
    case 0x3C9:  // DAC data: R, G, B, 6 bits each, auto-incrementing
      if (is_write) {
        dac_latch_[dac_write_comp_++] = uint8_t(value & 0x3F);
        if (dac_write_comp_ == 3) {
          dac_write_comp_ = 0;
          if (d) {
            PALETTEENTRY e{BYTE((dac_latch_[0] << 2) | (dac_latch_[0] >> 4)),
                           BYTE((dac_latch_[1] << 2) | (dac_latch_[1] >> 4)),
                           BYTE((dac_latch_[2] << 2) | (dac_latch_[2] >> 4)), 0};
            d->set_system_entries(dac_write_index_, 1, &e);
          }
          dac_write_index_++;
        }
        return 0;
      } else {
        uint8_t v = 0;
        if (d) {
          const PALETTEENTRY& e = d->system_palette()[dac_read_index_];
          uint8_t c = dac_read_comp_ == 0 ? e.peRed : dac_read_comp_ == 1 ? e.peGreen : e.peBlue;
          v = uint8_t(c >> 2);
        }
        if (++dac_read_comp_ == 3) {
          dac_read_comp_ = 0;
          dac_read_index_++;
        }
        return v;
      }
    case 0x3C6:  // PEL mask
      return is_write ? 0 : 0xFF;
    default:
      break;
  }
  if (!reported_port_) {
    reported_port_ = true;
    log("win16: %s port 0x%03X (size %u) at %s: no device (reads 0xFF)", is_write ? "write to" : "read from", port,
        size, describe(cpu_->get_segment(SegReg::CS), cpu_->registers().eip).c_str());
  }
  return is_write ? 0 : (size == 1 ? 0xFF : size == 2 ? 0xFFFF : 0xFFFFFFFF);
}

// ---- diagnostics ---------------------------------------------------------------------------------

std::string Runtime16::describe(uint16_t cs, uint32_t ip) const {
  char b[128];
  if ((cs | 3) == (thunk_sel_ | 3)) {
    if (ip < 4) return "the host-call sentinel";
    uint16_t id = uint16_t(ip / Shim16Registry::kThunkStride);
    const Shim16Entry* e = shims_ ? const_cast<Shim16Registry&>(*shims_).by_thunk(id) : nullptr;
    return e ? "thunk " + e->label() : "thunk #" + std::to_string(id);
  }
  if (modules_) {
    if (Module16* m = modules_->containing(cs)) {
      snprintf(b, sizeof(b), "%s %d:%04X", m->name.c_str(), modules_->segment_index(*m, cs), ip);
      return b;
    }
  }
  const std::string& tag = ldt_.tag(cs);
  snprintf(b, sizeof(b), "%04X:%04X%s%s", cs, ip, tag.empty() ? "" : " ", tag.c_str());
  return b;
}

std::string Runtime16::state_text(const char* why, bool frames) const {
  const auto& r = cpu_->registers();
  auto sel = [&](SegReg s) { return cpu_->get_segment(s); };
  char b[320];
  snprintf(b, sizeof(b),
           "%s: cs:ip=%04X:%04X (%s) ax=%04X bx=%04X cx=%04X dx=%04X si=%04X di=%04X bp=%04X sp=%04X ds=%04X "
           "es=%04X ss=%04X eflags=%08X insns=%" PRIu64,
           why, sel(SegReg::CS), r.eip, describe(sel(SegReg::CS), r.eip).c_str(), r.r_ax(), r.r_bx(), r.r_cx(),
           r.r_dx(), r.r_si(), r.r_di(), r.r_bp(), r.r_sp(), sel(SegReg::DS), sel(SegReg::ES), sel(SegReg::SS),
           r.read_eflags(), cpu_->cycles());
  std::string out = b;
  const cpu::SegDesc& ss = cpu_->get_segment_desc(SegReg::SS);
  out += "\n" + std::string(why) + ": stack:";
  for (uint32_t i = 0; i < 12; i++) {
    uint32_t off = uint32_t(r.r_sp()) + 2 * i;
    if (off + 1 > ss.limit || !mem_->exists(ss.base + off, 2)) break;
    snprintf(b, sizeof(b), " %04X", mem_->read_u16l(ss.base + off));
    out += b;
  }
  if (depth_ == 0) return out;
  out += "\n" + std::string(why) + ": frames: " + backtrace();
  return out;
}

void Runtime16::log_state(const char* why) const {
  // After a failed call the registers are the caller's again (call_far
  // restores them); where the guest actually was is in fault_state_.
  if (depth_ == 0 && !fault_state_.empty()) log("%s %s", why, fault_state_.c_str());
  log("%s", state_text(why, depth_ > 0).c_str());
}

std::string Runtime16::backtrace(int depth) const {
  const auto& r = cpu_->registers();
  const cpu::SegDesc& ss = cpu_->get_segment_desc(SegReg::SS);
  uint16_t bp = r.r_bp();
  std::string out;
  for (int i = 0; i < depth && bp && uint32_t(bp) + 5 <= ss.limit; i++) {
    uint16_t saved = mem_->read_u16l(ss.base + (bp & ~1));
    uint16_t ip = mem_->read_u16l(ss.base + (bp & ~1) + 2);
    uint16_t cs = (bp & 1) ? mem_->read_u16l(ss.base + (bp & ~1) + 4) : cpu_->get_segment(SegReg::CS);
    out += (out.empty() ? "" : " <- ") + describe(cs, ip);
    if ((saved & ~1) <= (bp & ~1)) break;
    bp = saved;
  }
  return out;
}

uint64_t Runtime16::instructions() const { return cpu_->cycles(); }

}  // namespace adw::win16
