// adw::win16::Runtime16 — one emulated Win16 "task" for the Classic lane: the
// CPU in 16-bit protected mode over a host-owned LDT (ldt.hh), the global and
// local heaps, the far-call import thunks and their registry (shims16.hh), the
// NE module table (modules16.hh), the machine underneath Windows that the
// Classic binaries reach directly (INT 21h/1Ah/2Fh, the BIOS data area at
// selector 0x40, the VGA DAC and retrace ports — ABI.md §3.6/§4), and, once
// attached, the display (win32/display.hh, the host-level palette model the
// Win32 lane uses too).
//
// Control flow between host and guest:
//   * guest → host: `int 0xFE` far thunks (shims16.hh); `int n` for the DOS/
//     BIOS services (dos16.cc); `in`/`out` for the VGA ports.
//   * host → guest: call_far() pushes Pascal arguments on the 16-bit task
//     stack and a far return address pointing at the sentinel thunk (thunk
//     segment offset 0), and runs a nested emulation until the callee returns
//     there — for LibEntry/DLLENTRYPOINT, OLDMOD16's exports, the AD3 MODULE
//     entry (OLDMOD16 calls it itself), window procedures and enum callbacks.
//     Nesting is unlimited; each level saves and restores every register.
//   * fatal guest conditions (a CPU fault — Win16 had no SEH, a GP fault
//     killed the task —, FatalExit/FatalAppExit, INT 21h AH=4Ch, a hung call)
//     throw GuestError16, which unwinds every run loop at once.
//   * a call (or software interrupt) that frees a selector the guest holds in
//     DS, ES, FS or GS returns with that register null: the freed-selector
//     rule (null_freed_segments), what Windows 3.1's GlobalFree did for DS,
//     DPMI 1.0 for any register, and Wine's relay does on every return.
//
// Shim families keep their own state in state<T>() (a struct deriving from
// RuntimeState16, created on first use).
#pragma once

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <typeindex>
#include <unordered_map>
#include <vector>

#include "X86Emulator.hh"
#include "adw/core/clock.h"
#include "adw/core/protocol.h"
#include "adw/core/screen.h"
#include "win16/global_heap.hh"
#include "win16/layout16.hh"
#include "win16/ldt.hh"
#include "win16/local_heap.hh"
#include "win16/shims16.hh"

namespace adw::win32 {
class Display;
class GuestHeap;
class Vfs;
}  // namespace adw::win32

namespace adw::win16 {

class ModuleTable16;
class Runtime16;

// Errors that end the emulation (they propagate out of every nested run loop).
class GuestError16 : public std::runtime_error {
 public:
  enum class Kind {
    exit,   // FatalExit / FatalAppExit / INT 21h AH=4Ch
    fault,  // a CPU fault in guest code, or a bad far pointer handed to a shim
    hang,   // a guest call exceeded its instruction budget
    fatal,  // anything else the runtime cannot continue from
  };
  GuestError16(Kind kind, const std::string& what) : std::runtime_error(what), kind_(kind) {}
  Kind kind() const { return kind_; }

 private:
  Kind kind_;
};

// KERNEL.Throw to a Catch made at a shallower host call level (Catch in the
// DRAWFRAME path, Throw from a window procedure or timer callback the host
// called back into): not an error but a non-local exit through the host's own
// frames. Every call_far in between unwinds (restoring its registers), and the
// thunk dispatcher running at `depth` — the shim whose callback threw — calls
// `resume` to load the CATCHBUF state and continues the guest there.
// Deliberately not a std::exception, so no generic handler swallows it.
struct GuestUnwind16 {
  int depth = 0;
  std::function<void(Runtime16&)> resume;
};

struct RuntimeState16 {
  virtual ~RuntimeState16() = default;
};

struct Runtime16Options {
  uint32_t arena_size = layout::kDefaultArenaSize;
  // Instructions a top-level call_far may run before it counts as hung.
  uint64_t call_budget = 2'000'000'000ull;
  // Win16 GetTickCount granularity: Windows 3.x/95 advanced it once per
  // 55 ms timer tick, and AD_RSRC/EINSTEIN calibrate their delays on that
  // (ABI.md §4). 1 gives a millisecond clock.
  uint32_t tick_quantum_ms = 55;
  // Virtual time a clock read sees (fixed-step clock only): every read nudges
  // it read_step_us (the lane's ADREADSTEPUS), and every insns_per_us
  // instructions executed since the previous read add a microsecond — a
  // notional 1996 CPU, so loops that draw until the tick count changes
  // (SATORI) end after as much work as they did on real hardware. 0 = off.
  // With insns_per_us set, that CPU's work runs INSIDE the frame period: a
  // read sees max(the time already reached, the frame's start) plus its
  // nudge, so a frame that does less than a period's work costs no extra
  // time and the lane can fill the period with DRAWFRAMEs (ne16/lane.hh
  // "Pacing"); only work beyond the period pushes the clock past the frame
  // grid. With insns_per_us 0 the read nudges accumulate on the core clock
  // instead (the lane's original model).
  uint32_t read_step_us = 5;
  uint32_t insns_per_us = 100;
  // What one API call costs in that time, in instructions: a Win16 call into
  // USER/GDI went through the Win95 thunk layer and took microseconds (500 =
  // 5 µs at 100 MIPS).
  uint32_t api_cost_insns = 500;
  // What one pixel a GDI blit or fill writes costs, in instructions — for
  // the lane's DRAWFRAME budget only (work_insns), not the clock: a 1995
  // display card moved some 10–40 MB/s, so blits bounded how often a module
  // could draw (2 = 12.5 Mpixel/s against a 25-MIPS budget).
  uint32_t pixel_cost_insns = 2;
  // MMSYSTEM reports one (silent) wave-out device (system16.cc).
  bool sound_device = true;
  // The hardware palette the display starts with: false = the 20 static
  // colours with black between them (a freshly booted 8-bit display, what the
  // Classic lane has always shown); true = the statics around 236 distinct
  // colours (a 6×6×6 colour cube and 20 greys), as a Windows 256-colour
  // desktop left the system palette when a saver started. ADXPL310 builds an
  // "identity" palette from GetSystemPaletteEntries and maps its canvas
  // through it, which only works when those entries differ (SIMPCLOK).
  bool desktop_palette = false;
  // Debugging: log every change of the byte at this far address (sel:off,
  // "0" = off) with the instruction that made it. Slow; ADWATCH16 in the lane.
  uint32_t watch = 0;
  // Debugging: log every instruction executed at CS = step_cs, step_lo <= IP < step_hi.
  uint16_t step_cs = 0, step_lo = 0, step_hi = 0;
  // What the guest sees.
  std::string guest_dir = "C:\\AFTERDRK";         // where the modules live (the current directory)
  std::string windows_dir = "C:\\WINDOWS";
  std::string system_dir = "C:\\WINDOWS\\SYSTEM";  // OLDMOD16.DLL, AD_SND.DLL
};

// An argument for call_far: a WORD, or a DWORD pushed high word first (a far
// pointer or a LONG, as PASCAL pushes them).
struct Arg16 {
  uint32_t v = 0;
  bool dword = false;
};
inline Arg16 w16(uint32_t v) { return Arg16{v & 0xFFFF, false}; }
inline Arg16 l16(uint32_t v) { return Arg16{v, true}; }

// Register inputs for call_far (the NE DLL entry takes DI/DS/CX/ES:SI). With
// ss and sp the call runs on that stack instead of the current one (a task's
// start: its own stack in its DGROUP, modules16.hh "Tasks"); bp is BP at the
// callee's first instruction.
struct Regs16In {
  std::optional<uint16_t> ax, bx, cx, dx, si, di, ds, es;
  std::optional<uint16_t> ss, sp, bp;
};

class Runtime16 {
 public:
  Runtime16(const Runtime16Options& opts, VirtualClock& clock, const InputState* input = nullptr);
  ~Runtime16();
  Runtime16(const Runtime16&) = delete;
  Runtime16& operator=(const Runtime16&) = delete;

  const Runtime16Options& options() const { return opts_; }
  cpu::MemoryContext& mem() { return *mem_; }
  const cpu::MemoryContext& mem() const { return *mem_; }
  cpu::X86Emulator& cpu() { return *cpu_; }
  VirtualClock& clock() { return clock_; }
  const InputState& input() const { return input_ ? *input_ : empty_input_; }
  void set_input(const InputState* input) { input_ = input; }

  Ldt& ldt() { return ldt_; }
  GlobalHeap16& global() { return *global_; }
  LocalHeaps16& local() { return *local_; }
  win32::GuestHeap& arena() { return *arena_; }
  Shim16Registry& shims() { return *shims_; }
  ModuleTable16& modules() { return *modules_; }
  win32::Vfs& vfs() { return *vfs_; }

  // The emulated screen: attach_display() allocates the screen DIB's bits in
  // the arena and builds the Display over them; until then display() is null.
  win32::Display* display() { return display_.get(); }
  win32::Display& attach_display(Screen& screen);

  template <typename T>
  T& state() {
    static_assert(std::is_base_of_v<RuntimeState16, T>);
    auto key = std::type_index(typeid(T));
    auto it = states_.find(key);
    if (it == states_.end()) {
      std::unique_ptr<RuntimeState16> p;
      if constexpr (std::is_constructible_v<T, Runtime16&>) p = std::make_unique<T>(*this);
      else p = std::make_unique<T>();
      it = states_.emplace(key, std::move(p)).first;
    }
    return static_cast<T&>(*it->second);
  }

  // ---- host segments ----
  uint16_t thunk_sel() const { return thunk_sel_; }
  uint16_t stack_sel() const { return stack_sel_; }
  uint16_t sys_sel() const { return sys_sel_; }
  // The far address (sel:off) whose call reaches shim `e`.
  uint32_t thunk_far(Shim16Entry& e);
  uint32_t sentinel_far() const { return uint32_t(thunk_sel_) << 16; }
  // A NUL-terminated copy of `bytes` in the system segment (once per key).
  uint32_t static_bytes(const std::string& key, std::string_view bytes);

  // ---- guest memory through far pointers ----
  // Linear address of sel:off for a `size`-byte access, checked against the
  // descriptor (throws GuestError16(fault) for a null/unknown selector or a
  // limit overrun: the GP fault Windows would have taken in the API).
  uint32_t linear(uint16_t sel, uint32_t off, uint32_t size = 1) const;
  uint32_t linear(uint32_t farptr, uint32_t size = 1) const { return linear(uint16_t(farptr >> 16), farptr & 0xFFFF, size); }
  uint8_t rd8(uint32_t fp) const { return mem_->read_u8(linear(fp, 1)); }
  uint16_t rd16(uint32_t fp) const { return mem_->read_u16l(linear(fp, 2)); }
  uint32_t rd32(uint32_t fp) const { return mem_->read_u32l(linear(fp, 4)); }
  void wr8(uint32_t fp, uint8_t v) { mem_->write_u8(linear(fp, 1), v); }
  void wr16(uint32_t fp, uint16_t v) { mem_->write_u16l(linear(fp, 2), v); }
  void wr32(uint32_t fp, uint32_t v) { mem_->write_u32l(linear(fp, 4), v); }
  void read_bytes(uint32_t fp, void* out, size_t n) const;
  void write_bytes(uint32_t fp, const void* data, size_t n);
  // NUL-terminated string (fp 0 → ""); stops at the segment limit or `max`.
  std::string read_str(uint32_t fp, size_t max = 0x10000) const;
  // Copies s (truncated to cap-1) plus a NUL; returns the characters copied.
  size_t write_str(uint32_t fp, std::string_view s, size_t cap);
  // A huge pointer advanced by n bytes (selector tiles __AHINCR apart).
  static uint32_t huge_add(uint32_t fp, uint32_t n);

  // ---- host → guest calls ----
  // Far-calls `proc` (sel:off) with PASCAL arguments (pushed in the order
  // given) and returns DX:AX. Every register is restored afterwards — also
  // when an exception (a guest fault, a hang, a cross-level Throw) leaves the
  // call, so the host can still call into the guest after a failed call
  // (UNLOADADMODULE16 at shutdown); log_state() still shows where it failed.
  uint32_t call_far(uint32_t proc, std::span<const Arg16> args, const Regs16In* regs = nullptr);
  uint32_t call_far(uint32_t proc, std::initializer_list<Arg16> args, const Regs16In* regs = nullptr) {
    return call_far(proc, std::span<const Arg16>(args.begin(), args.size()), regs);
  }
  int call_depth() const { return depth_; }
  // SP at the start of the innermost active call (0 when none).
  uint16_t last_sp_after() const { return last_sp_after_; }

  // ---- the machine ----
  // Every time API reads the virtual clock through here (see Runtime16Options).
  uint64_t clock_us();
  uint32_t time_ms() { return uint32_t(VirtualClock::kBootOffsetMs + clock_us() / 1000); }  // timeGetTime
  // Win16 GetTickCount/GetCurrentTime: the virtual clock, quantized.
  uint32_t tick_count();
  // Local time as a FILETIME value (100 ns since 1601), from the virtual clock:
  // headless runs start at a fixed moment, streamed ones at the host's time.
  uint64_t local_filetime();
  // Refreshes the BIOS tick count at 0040:006C (INT 1Ah's view).
  void update_bios_ticks();
  // Virtual time as a clock read would see it now (the instructions run since
  // the last read included), without advancing it.
  uint64_t peek_us() const;
  // True when the frame-bounded model above is on (fixed-step clock and
  // insns_per_us set; with a realtime clock, until start_frames()): the lane
  // then sets the core clock's own read step to 0.
  bool modeled_time() const;
  // The lane's first frame is starting. A realtime (streamed) clock only runs
  // from frame 0, so until this call clock reads use the modeled time (init's
  // calibration loops must see time move); from it on they follow the wall
  // clock, offset by the time init took, so time never goes back.
  void start_frames();
  // The frame is over: the work done since the last clock read belongs to
  // it, so it is charged now, against this frame's grid line, and not on top
  // of the next one's at the next read (a module that reads no clock while
  // drawing would otherwise carry each frame's work into the next frame).
  void settle_time();
  // Instructions executed, plus api_cost_insns per API call and
  // pixel_cost_insns per pixel charged, so far: the work the lane's
  // DRAWFRAME budget counts.
  uint64_t work_insns() const;
  // GDI shims: a blit or fill wrote n pixels (the destination area).
  void charge_pixels(int64_t n) { if (n > 0) pix_charged_ += uint64_t(n); }
  // Real GDI batches drawing per thread. A surface the guest also reads and
  // writes with its own code — a DIB driver DC's bits (gdi16.hh) — must be
  // finished before the guest runs on: a shim that touched one calls this,
  // and the batch is flushed (GdiFlush) as the API call returns.
  void flush_gdi_after_call() { gdi_flush_ = true; }
  // Scanout: the emulated VGA shows the screen once per 70 Hz refresh (the
  // same timing as the 0x3DA retrace bit). At the first API call after each
  // refresh boundary of virtual time the hook runs, so a lane can see what a
  // monitor would have shown between two calls — content a module draws and
  // erases inside one DRAWFRAME (ZOT's lightning: draw, CPU delay loop,
  // erase) is visible only there.
  void set_scanout_hook(std::function<void()> fn) { scanout_ = std::move(fn); }
  // A deadline in virtual time (clock_us()'s scale): at the first API call
  // (or retrace-port read) at or after it, `fn` runs once — after the
  // scanout hook, before the call does anything — and the deadline is
  // cleared. The Classic lane ends a presented frame there when a DRAWFRAME
  // outlasts the frame period (ne16/lane.hh "Long calls"): `fn` may switch
  // to another fiber and come back, or throw to abandon the guest call.
  void set_deadline(uint64_t us, std::function<void()> fn) {
    deadline_us_ = us;
    work_deadline_ = UINT64_MAX;
    deadline_fn_ = std::move(fn);
  }
  // With a deadline set: `fn` also runs at the first API call (or
  // retrace-port read) once work_insns() reaches `work` — the lane's frame
  // budget for a protocol whose one call is a task (ne16/lane.hh "Windows 3.1
  // screen savers"). set_deadline clears it.
  void set_work_deadline(uint64_t work) { work_deadline_ = work; }
  void clear_deadline() {
    deadline_fn_ = nullptr;
    work_deadline_ = UINT64_MAX;
  }
  // Audio delivery (sound16.hh, AUDIO.md §8.6): `fn` runs at every API call
  // at or after virtual time `due` (peek_us()'s scale) — after the scanout
  // and deadline hooks, before the call does anything — until the owner
  // moves `due` on (UINT64_MAX = never). MMSYSTEM posts MM_WOM_*/MM_MCINOTIFY
  // and calls CALLBACK_FUNCTION procedures there, so they reach the guest at
  // the first safe point at or after their time, never mid-instruction.
  void set_audio_hook(std::function<void()> fn) { audio_fn_ = std::move(fn); }
  void set_audio_due(uint64_t us) { audio_due_ = us; }
  uint64_t audio_due() const { return audio_due_; }
  // A call that blocked the guest until virtual time `us` (sndPlaySound
  // without SND_ASYNC: the sound's duration, AUDIO.md §8.2) — never a real
  // wait. While the yield hook (the lane's long-call fiber, ne16/lane.hh)
  // can end the frame here, frames are presented and the call resumes on the
  // next ones until guest time reaches `us`, as the 1996 screen froze while
  // the call blocked. Otherwise (no hook, or it cannot yield: init, ADMIPS=0,
  // long calls off) the time is charged at once: the modeled clock jumps to
  // `us`, a realtime or read-step clock is offset by what is left.
  void set_yield_hook(std::function<bool()> fn) { yield_fn_ = std::move(fn); }
  void wait_until_us(uint64_t us);
  // Ends the presented frame here through the yield hook, and returns when a
  // later frame resumes the guest (true); false when nothing can yield (no
  // hook, or not inside the lane's long call). A Win16 application's message
  // wait (user16 GetMessage/WaitMessage in an application task).
  bool yield_frame() { return yield_fn_ && yield_fn_(); }
  // Runs the audio hook when an audio event is due by now, as every API call
  // does first: what a wait inside an API call delivers before it looks again.
  void deliver_due_audio() {
    if (audio_fn_ && peek_us() >= audio_due_) audio_fn_();
  }
  // An application task's budget (modules16.hh "Tasks"): its one call runs
  // for the whole run, so a call made while this is on is hung only after
  // call_budget instructions without an API call — at every call level —
  // rather than call_budget in all.
  void set_task_budget(bool on) { task_budget_ = on; }
  bool task_budget() const { return task_budget_; }

  // Software-interrupt services (dos16.cc registers 21h, 1Ah, 2Fh, …).
  using IntHandler = std::function<void(Runtime16&)>;
  void set_int_handler(uint8_t vector, IntHandler fn) { ints_[vector] = std::move(fn); }
  // Carry flag helpers for DOS-style returns.
  void set_carry(bool on);

  // ---- diagnostics ----
  std::string describe(uint16_t cs, uint32_t ip) const;  // "TOASTPRO 1:0123", "thunk USER.13 GetTickCount"
  void log_state(const char* why) const;
  // The far return addresses up the BP chain of the current stack (Win16
  // frames push an odd BP for far calls), innermost first.
  std::string backtrace(int depth = 8) const;
  uint64_t instructions() const;

 private:
  struct SavedRegs {
    uint32_t gpr[8];
    uint32_t eip, eflags;
    uint16_t sel[6];
    cpu::SegDesc desc[6];
    bool null_seg[6];
  };
  SavedRegs save() const;
  void restore(const SavedRegs& s);
  std::string state_text(const char* why, bool frames) const;  // registers, stack words, BP chain

  void build_machine();
  void on_interrupt(cpu::X86Emulator& cpu, uint8_t vector);
  void on_fault(cpu::X86Emulator& cpu, const cpu::X86Emulator::Fault& f);
  uint32_t on_port(uint16_t port, uint8_t size, bool is_write, uint32_t value);
  void dispatch_thunk(uint16_t id);
  // The freed-selector rule (runtime16.cc), as an API call (`by`) or a
  // software interrupt (`by` null, `vector`) returns: DS, ES, FS or GS
  // holding a selector that no longer exists becomes the null selector.
  void null_freed_segments(const Shim16Entry* by, uint8_t vector);

  Runtime16Options opts_;
  VirtualClock& clock_;
  const InputState* input_;
  InputState empty_input_;

  std::shared_ptr<cpu::MemoryContext> mem_;
  std::unique_ptr<cpu::X86Emulator> cpu_;
  Ldt ldt_;
  std::unique_ptr<win32::GuestHeap> arena_;
  std::unique_ptr<GlobalHeap16> global_;
  std::unique_ptr<LocalHeaps16> local_;
  std::unique_ptr<Shim16Registry> shims_;
  std::unique_ptr<win32::Vfs> vfs_;
  std::unique_ptr<ModuleTable16> modules_;
  std::unique_ptr<win32::Display> display_;
  std::unordered_map<std::type_index, std::unique_ptr<RuntimeState16>> states_;
  std::unordered_map<uint8_t, IntHandler> ints_;
  std::unordered_map<uint8_t, bool> unknown_int_reported_;

  uint16_t thunk_sel_ = 0, stack_sel_ = 0, sys_sel_ = 0;
  int depth_ = 0;
  // The guest's state where the last failed top-level call failed, captured
  // by the innermost call_far before the unwinding restored registers.
  std::string fault_state_;
  uint64_t budget_end_ = 0;
  uint16_t last_sp_after_ = 0;
  uint16_t strings_next_ = layout::kSysStrings;
  std::unordered_map<std::string, uint32_t> strings_;
  // VGA DAC state (ports 0x3C7-0x3C9).
  uint8_t dac_read_index_ = 0, dac_write_index_ = 0, dac_read_comp_ = 0, dac_write_comp_ = 0;
  uint8_t dac_latch_[3] = {0, 0, 0};
  bool reported_port_ = false;
  bool prof_ = false;
  uint64_t time_base_ = 0;
  uint64_t clock_insns_ = 0, insn_carry_ = 0;
  uint64_t vt_ = 0;          // modeled_time(): the virtual µs the last read saw
  bool frames_started_ = false;  // start_frames()
  uint64_t rt_offset_ = 0;       // realtime clock: the modeled time init took
  uint64_t api_charged_ = 0;  // API calls charged (work_insns)
  uint64_t pix_charged_ = 0;  // pixels charged (work_insns)
  bool gdi_flush_ = false;    // flush_gdi_after_call
  bool time_base_set_ = false;
  std::function<void()> scanout_;
  uint64_t next_scanout_us_ = 0;
  std::function<void()> deadline_fn_;
  uint64_t deadline_us_ = 0;
  uint64_t work_deadline_ = UINT64_MAX;
  void check_deadline();
  std::function<void()> audio_fn_;
  uint64_t audio_due_ = UINT64_MAX;
  std::function<bool()> yield_fn_;
  bool task_budget_ = false;
};

}  // namespace adw::win16
