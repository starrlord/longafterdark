// adw_win16 tests: the LDT (selectors, huge-block tiling), the global and
// local heaps, KERNEL's selector calls and the freed-selector rule, Pascal
// far thunks (argument order, callee pops, DX:AX), far
// callbacks from a shim into guest code (nested), Catch/Throw, INT 21h/1Ah
// basics (the current drive and each drive's current directory, DOS's
// limit on one), the VGA ports, a small NE DLL built in memory (imports, prolog
// patching, LibEntry), and — with the imported assets — OLDMOD16.DLL loaded
// through the module table with its DLLENTRYPOINT, and AD_SND.DLL.
//
//   adw_win16_tests            unit tests
//   adw_win16_tests --assets   the asset tests (exit 77 when the assets are absent)
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "adw/core/clock.h"
#include "adw/core/screen.h"
#include "test_paths.h"
#include "win16/dialogs16.hh"
#include "win16/dos16.hh"
#include "win16/gdi16.hh"
#include "win16/input16.hh"
#include "win16/modules16.hh"
#include "win16/runtime16.hh"
#include "win16/shim_families16.hh"
#include "win32/config_script.hh"
#include "win32/display.hh"
#include "win32/ini_store.hh"
#include "win32/vfs.hh"

using namespace adw;
using namespace adw::win16;
using SegReg = cpu::X86Emulator::SegReg;

namespace {

int failures = 0, checks = 0;

#define CHECK(cond, ...)                          \
  do {                                            \
    checks++;                                     \
    if (!(cond)) {                                \
      printf("FAIL %s:%d: ", __FILE__, __LINE__); \
      printf(__VA_ARGS__);                        \
      printf("\n");                               \
      failures++;                                 \
    }                                             \
  } while (0)

struct Machine {
  VirtualClock clock{VirtualClock::Mode::fixed_step, 16667};
  Runtime16 rt{Runtime16Options{}, clock};
  Machine() {
    clock.set_read_step_us(5);
    register_all16(rt);
  }
  // A code segment holding `bytes`; returns its selector.
  uint16_t code(const std::vector<uint8_t>& bytes) {
    GlobalBlock* b = rt.global().alloc_block(uint32_t(bytes.size() + 16), true, 0, 0);
    rt.mem().memcpy(b->base, bytes.data(), bytes.size());
    return b->sel;
  }
  uint16_t data(uint32_t size) {
    GlobalBlock* b = rt.global().alloc_block(size, false, 0, 0);
    return b->sel;
  }
  // `lcall thunk` bytes (9A off seg) for a shim.
  std::vector<uint8_t> lcall(const char* module, const char* name) {
    Shim16Entry* e = rt.shims().find_name(module, name);
    uint32_t fp = rt.thunk_far(*e);
    return {0x9A, uint8_t(fp), uint8_t(fp >> 8), uint8_t(fp >> 16), uint8_t(fp >> 24)};
  }
};

void append(std::vector<uint8_t>& v, const std::vector<uint8_t>& w) { v.insert(v.end(), w.begin(), w.end()); }

// ---- LDT ----------------------------------------------------------------------------------------------

void test_ldt() {
  Ldt ldt;
  uint16_t a = ldt.alloc(1);
  uint16_t b = ldt.alloc(3);
  CHECK((a & 7) == 7 && (b & 7) == 7, "selectors are LDT/RPL3 (%04X %04X)", a, b);
  CHECK(Ldt::index_of(b) == Ldt::index_of(a) + 1, "first fit, consecutive");
  ldt.set_block(b, 0x100000, 0x28000, false);  // 160 KiB: three tiles
  CHECK(ldt.base_of(b) == 0x100000 && ldt.base_of(b + 8) == 0x110000 && ldt.base_of(b + 16) == 0x120000,
        "tiles are __AHINCR apart and 64K apart in memory");
  CHECK(ldt.limit_of(b) == 0x27FFF && ldt.limit_of(b + 16) == 0x7FFF, "tile limits run to the block end (%X %X)",
        ldt.limit_of(b), ldt.limit_of(b + 16));
  cpu::SegDesc d;
  CHECK(ldt.lookup(uint16_t(b & ~1), d) && d.base == 0x100000, "a handle (RPL 2) finds the same descriptor");
  CHECK(!ldt.lookup(0x40, d), "no BIOS selector until set");
  ldt.set_bios_area(0x10000, 0xFFFF);
  CHECK(ldt.lookup(0x40, d) && d.base == 0x10000, "selector 0x40 = BIOS data area");
  ldt.free(b, 3);
  CHECK(ldt.alloc(2) == b, "freed selectors are reused first-fit");
  // Runs the guest's selector calls made (mark_guest_run): the first carries
  // the length, its tiles belong to it; a freed or reallocated entry is never
  // taken for one of them.
  uint16_t g = ldt.alloc(3), h = ldt.alloc(1);
  ldt.mark_guest_run(g, 3);
  ldt.mark_guest_run(h, 1);
  CHECK(ldt.guest_run(g) == 3 && !ldt.guest_run(g + 8) && ldt.in_guest_run(g + 16) && ldt.guest_run(h) == 1 &&
            !ldt.in_guest_run(a),
        "a marked run: its first has the length, its tiles are in it, others are not");
  CHECK(ldt.free_guest_run(uint16_t(g + 8)) == 0 && ldt.in_use(g + 8), "free_guest_run of a tile: nothing");
  ldt.free(uint16_t(g + 8), 1);
  uint16_t reuse = ldt.alloc(1);
  CHECK(reuse == uint16_t(g + 8) && !ldt.in_guest_run(reuse), "a tile freed and handed out again is no longer marked");
  CHECK(ldt.free_guest_run(g) == 3 && !ldt.in_use(g) && ldt.in_use(reuse) && !ldt.in_use(g + 16) && ldt.in_use(h),
        "free_guest_run frees the run's own tiles only");
  CHECK(ldt.alloc(1) == g && !ldt.guest_run(g), "a freed run's first, reallocated, is unmarked");
}

// ---- global heap ---------------------------------------------------------------------------------------

void test_global() {
  Machine m;
  GlobalHeap16& g = m.rt.global();
  uint16_t f = g.alloc(0, 100);
  uint16_t mv = g.alloc(GlobalHeap16::kMoveable | GlobalHeap16::kZeroInit, 0x3C);
  CHECK(f && (f & 1), "fixed handle == selector (%04X)", f);
  CHECK(mv && !(mv & 1), "moveable handle has bit 0 clear (%04X)", mv);
  CHECK(g.size(f) == 128 && g.size(mv) == 0x40, "sizes rounded to the 32-byte granule (%u %u)", g.size(f), g.size(mv));
  CHECK(m.rt.ldt().limit_of(f) == 127, "the selector limit covers the rounded size (%X)", m.rt.ldt().limit_of(f));
  uint32_t p = g.lock(mv);
  CHECK(p == (uint32_t(mv | 1) << 16), "GlobalLock = sel:0 (%08X)", p);
  CHECK(g.flags(mv) == 1, "lock count in GlobalFlags");
  CHECK(g.handle(uint16_t(mv | 1)) == ((uint32_t(mv | 1) << 16) | mv), "GlobalHandle(sel) = sel:handle");
  m.rt.wr32(p + 0x3C, 0x12345678);
  uint16_t h2 = g.realloc(mv, 0x30000, GlobalHeap16::kMoveable);
  CHECK(h2 == mv, "a moveable realloc keeps its handle when its selectors can grow (%04X)", h2);
  CHECK(m.rt.rd32(p + 0x3C) == 0x12345678, "realloc keeps the contents");
  CHECK(g.size(mv) == 0x30000, "grown to 192K");
  uint32_t tile2 = (uint32_t((mv | 1) + 2 * Ldt::kAhIncr) << 16) | 0xFFF0;
  m.rt.wr16(tile2, 0xBEEF);
  CHECK(m.rt.mem().read_u16l(m.rt.ldt().base_of(mv | 1) + 0x2FFF0) == 0xBEEF, "third tile addresses 128K..192K");
  CHECK(m.rt.rd16(Runtime16::huge_add(p, 0x2FFF0)) == 0xBEEF, "huge pointer arithmetic");
  CHECK(!g.unlock(mv), "unlock to 0");
  CHECK(g.free(mv) == 0 && g.free(f) == 0, "GlobalFree returns 0");
  CHECK(g.free(f) == f, "double free fails");
  CHECK(!g.find(f), "freed");
  // A zero-size moveable block is discarded: handle, no memory.
  uint16_t z = g.alloc(GlobalHeap16::kMoveable, 0);
  CHECK(z && !g.lock(z) && (g.flags(z) & GlobalHeap16::kFlagDiscarded), "discarded block");
}

// ---- local heap ----------------------------------------------------------------------------------------

void test_local() {
  Machine m;
  uint16_t ds = m.data(0x10000);
  LocalHeaps16& lh = m.rt.local();
  lh.note_dgroup(ds, 0x200);
  CHECK(lh.init(ds, 0, 0x400), "LocalInit(ds, 0, 0x400)");
  CHECK(m.rt.rd16((uint32_t(ds) << 16) | 6) == 0x200, "pLocalHeap at DS:[6]");
  uint16_t a = lh.alloc(ds, 0, 10);
  uint16_t b = lh.alloc(ds, LocalHeaps16::kMoveable | LocalHeaps16::kZeroInit, 20);
  CHECK(a >= 0x200 && (a & 3) == 0, "fixed block: 4-aligned pointer (%04X)", a);
  CHECK((b & 3) == 2, "moveable handle ≡ 2 mod 4 (%04X)", b);
  uint16_t pb = lh.lock(ds, b);
  CHECK(pb && m.rt.rd16((uint32_t(ds) << 16) | b) == pb, "*(WORD*)handle is the block (%04X)", pb);
  CHECK(lh.flags(ds, b) == 1, "lock count");
  CHECK(lh.handle(ds, pb) == b, "LocalHandle(ptr)");
  CHECK(lh.size(ds, a) == 12, "LocalSize rounds to 4 (%u)", lh.size(ds, a));
  // Growth past the initial 1K: the DGROUP is 64K.
  uint16_t big = lh.alloc(ds, 0, 0x2000);
  CHECK(big != 0, "the heap grows past LocalInit's size");
  uint16_t b2 = lh.realloc(ds, b, 400, LocalHeaps16::kMoveable);
  CHECK(b2 == b && lh.size(ds, b) >= 400, "moveable realloc keeps the handle");
  CHECK(lh.unlock(ds, b) == false, "unlocked");
  CHECK(lh.free(ds, b) == 0 && lh.free(ds, a) == 0 && lh.free(ds, big) == 0, "LocalFree");
  CHECK(lh.free(ds, a) == a, "double free fails");
}

// ---- thunks: Pascal convention -------------------------------------------------------------------------

void test_thunks() {
  Machine m;
  int called = 0;
  uint16_t got_a = 0;
  uint32_t got_b = 0;
  uint16_t got_c = 0;
  m.rt.shims().add("TESTDLL", 1, "Pascal3", Conv16::pascal_, false, 8, [&](Call16& c) {
    got_a = c.w();
    got_b = c.l();
    got_c = c.w();
    called++;
    c.ret32(0xCAFE0000u | got_a);
  });
  m.rt.shims().add("TESTDLL", 2, "Cdecl", Conv16::cdecl_, true, 4, [&](Call16& c) {
    uint16_t x = c.w(), y = c.w();
    c.ret(uint16_t(x - y));
  });
  // push 0x1111; push 0x2222; push 0x3333 (the long: high first); push 0x4444;
  // lcall Pascal3; mov bx,ax; mov si,dx; push 5; push 9 (cdecl: y first, x last); lcall Cdecl; add sp,4; retf
  uint32_t p3 = m.rt.thunk_far(*m.rt.shims().find("TESTDLL", 1));
  uint32_t cd = m.rt.thunk_far(*m.rt.shims().find("TESTDLL", 2));
  std::vector<uint8_t> code = {0x68, 0x11, 0x11, 0x68, 0x22, 0x22, 0x68, 0x33, 0x33, 0x68, 0x44, 0x44};
  append(code, {0x9A, uint8_t(p3), uint8_t(p3 >> 8), uint8_t(p3 >> 16), uint8_t(p3 >> 24)});
  append(code, {0x89, 0xC3, 0x89, 0xD6});
  append(code, {0x6A, 0x05, 0x6A, 0x09});
  append(code, {0x9A, uint8_t(cd), uint8_t(cd >> 8), uint8_t(cd >> 16), uint8_t(cd >> 24)});
  append(code, {0x83, 0xC4, 0x04});
  // Result in DX:AX = SI:BX of the first call, CX = the cdecl result.
  append(code, {0x89, 0xC1, 0x89, 0xD8, 0x89, 0xF2, 0xCB});
  uint16_t cs = m.code(code);
  uint16_t sp0 = m.rt.cpu().registers().r_sp();
  uint32_t r = m.rt.call_far(uint32_t(cs) << 16, {});
  CHECK(called == 1, "shim called");
  CHECK(got_a == 0x1111 && got_b == 0x22223333 && got_c == 0x4444, "Pascal args in declaration order (%04X %08X %04X)",
        got_a, got_b, got_c);
  CHECK(r == 0xCAFE1111, "DX:AX result (%08X)", r);
  CHECK(m.rt.cpu().registers().r_sp() == sp0, "call_far restores SP");
  // The callee popped exactly 8 bytes: otherwise the cdecl result would be garbage.
  CHECK(m.rt.last_sp_after() == sp0, "the stack balanced across both calls (%04X vs %04X)", m.rt.last_sp_after(), sp0);
}

// ---- callbacks: a shim calling guest code, nested -------------------------------------------------------

void test_callbacks() {
  Machine m;
  // Guest callback: int FAR PASCAL cb(int a, int b) { return a*10+b; } → retf 4.
  std::vector<uint8_t> cb = {0x55, 0x89, 0xE5, 0x8B, 0x46, 0x08, 0x6B, 0xC0, 0x0A, 0x03, 0x46, 0x06, 0x5D, 0xCA, 0x04, 0x00};
  uint16_t cbs = m.code(cb);
  uint32_t seen = 0;
  m.rt.shims().add("TESTDLL", 3, "Enum", Conv16::pascal_, true, 4, [&](Call16& c) {
    uint32_t proc = c.ptr();
    uint32_t sum = 0;
    for (uint16_t i = 1; i <= 3; i++) sum += m.rt.call_far(proc, {w16(i), w16(7)}) & 0xFFFF;
    seen = sum;
    c.ret(uint16_t(sum));
  });
  uint32_t en = m.rt.thunk_far(*m.rt.shims().find("TESTDLL", 3));
  // push cbs; push 0; lcall Enum; retf
  std::vector<uint8_t> code = {0x68, uint8_t(cbs), uint8_t(cbs >> 8), 0x6A, 0x00};
  append(code, {0x9A, uint8_t(en), uint8_t(en >> 8), uint8_t(en >> 16), uint8_t(en >> 24), 0xCB});
  uint16_t cs = m.code(code);
  // Mark SI/DI/BP/DS: callbacks must preserve the caller's registers.
  Regs16In in;
  in.si = 0x5151;
  uint32_t r = m.rt.call_far(uint32_t(cs) << 16, {}, &in);
  CHECK(seen == 17 + 27 + 37, "three nested callbacks (%u)", seen);
  CHECK((r & 0xFFFF) == 81, "result through the thunk (%u)", r & 0xFFFF);
}

// ---- Catch / Throw -------------------------------------------------------------------------------------

void test_catch_throw() {
  Machine m;
  uint16_t buf = m.data(64);
  auto catch_ = m.lcall("KERNEL", "Catch");
  auto throw_ = m.lcall("KERNEL", "Throw");
  // mov ax,buf; mov es,ax  (unused) ; push buf; push 0; lcall Catch; or ax,ax; jnz caught;
  // push buf; push 0; push 42; lcall Throw; (never returns) caught: retf
  std::vector<uint8_t> code = {0x68, uint8_t(buf), uint8_t(buf >> 8), 0x6A, 0x00};
  append(code, catch_);
  append(code, {0x09, 0xC0, 0x75, 0x00});  // or ax,ax; jnz rel8 (patched below)
  size_t jnz = code.size() - 1;
  append(code, {0x68, uint8_t(buf), uint8_t(buf >> 8), 0x6A, 0x00, 0x6A, 0x2A});
  append(code, throw_);
  append(code, {0xB8, 0xFF, 0xFF, 0xCB});  // mov ax,-1; retf (not reached)
  code[jnz] = uint8_t(code.size() - (jnz + 1));
  append(code, {0xCB});  // caught: retf with AX = 42
  uint16_t cs = m.code(code);
  uint16_t sp0 = m.rt.cpu().registers().r_sp();
  uint32_t r = m.rt.call_far(uint32_t(cs) << 16, {});
  CHECK((r & 0xFFFF) == 42, "Throw returns through Catch with its value (%d)", int16_t(r));
  CHECK(m.rt.last_sp_after() == sp0, "stack restored by Throw");
}

// A Throw from inside a callback the host made (a window procedure called by
// a shim) to a Catch in the code that called that shim: the host's frames in
// between must unwind and the guest resume at the Catch.
void test_throw_across_host_levels() {
  Machine m;
  uint16_t buf = m.data(64);
  auto catch_ = m.lcall("KERNEL", "Catch");
  auto throw_ = m.lcall("KERNEL", "Throw");
  bool shim_returned = false;
  m.rt.shims().add("TESTDLL", 4, "CallBack", Conv16::pascal_, true, 4, [&](Call16& c) {
    uint32_t proc = c.ptr();
    m.rt.call_far(proc, {w16(1), w16(2)});
    shim_returned = true;  // must not happen: the callback Threw past us
    c.ret(0xEEEE);
  });
  // The callback: push buf; push 0; push 7; lcall Throw; retf 4 (not reached)
  std::vector<uint8_t> cb = {0x68, uint8_t(buf), uint8_t(buf >> 8), 0x6A, 0x00, 0x6A, 0x07};
  append(cb, throw_);
  append(cb, {0xCA, 0x04, 0x00});
  uint16_t cbs = m.code(cb);
  uint32_t shim = m.rt.thunk_far(*m.rt.shims().find("TESTDLL", 4));
  // push buf; push 0; lcall Catch; or ax,ax; jnz done; push cbs; push 0; lcall CallBack; mov ax,-1; done: retf
  std::vector<uint8_t> code = {0x68, uint8_t(buf), uint8_t(buf >> 8), 0x6A, 0x00};
  append(code, catch_);
  append(code, {0x09, 0xC0, 0x75, 0x00});
  size_t jnz = code.size() - 1;
  append(code, {0x68, uint8_t(cbs), uint8_t(cbs >> 8), 0x6A, 0x00});
  append(code, {0x9A, uint8_t(shim), uint8_t(shim >> 8), uint8_t(shim >> 16), uint8_t(shim >> 24)});
  append(code, {0xB8, 0xFF, 0xFF});
  code[jnz] = uint8_t(code.size() - (jnz + 1));
  append(code, {0xCB});
  uint16_t cs = m.code(code);
  uint16_t sp0 = m.rt.cpu().registers().r_sp();
  uint32_t r = 0;
  try {
    r = m.rt.call_far(uint32_t(cs) << 16, {});
  } catch (const std::exception& e) {
    CHECK(false, "cross-level Throw: %s", e.what());
  }
  CHECK((r & 0xFFFF) == 7, "Throw from a host-called callback lands at the outer Catch (%d)", int16_t(r));
  CHECK(!shim_returned, "the shim between Catch and Throw was unwound, not returned from");
  CHECK(m.rt.call_depth() == 0 && m.rt.last_sp_after() == sp0, "call levels and stack unwound (depth %d, sp %04X/%04X)",
        m.rt.call_depth(), m.rt.last_sp_after(), sp0);
}

// A guest fault inside a call leaves the machine as the call found it, so the
// host can call into the guest again (the lane's UNLOADADMODULE16 after a
// failed DRAWFRAME).
void test_fault_restores_state() {
  Machine m;
  uint16_t small = m.data(16);
  auto& r = m.rt.cpu().registers();
  uint16_t sp0 = r.r_sp(), ds0 = m.rt.cpu().get_segment(SegReg::DS);
  r.w_si(0x5151);
  // mov ax,small; mov ds,ax; mov si,1234h; push ax; push ax; mov ax,[100h] (past the limit); retf
  std::vector<uint8_t> code = {0xB8, uint8_t(small), uint8_t(small >> 8), 0x8E, 0xD8, 0xBE, 0x34, 0x12,
                               0x50, 0x50, 0xA1, 0x00, 0x01, 0xCB};
  bool faulted = false;
  try {
    m.rt.call_far(uint32_t(m.code(code)) << 16, {});
  } catch (const GuestError16& e) {
    faulted = e.kind() == GuestError16::Kind::fault;
  }
  CHECK(faulted, "the out-of-limit read faults");
  CHECK(r.r_sp() == sp0 && r.r_si() == 0x5151 && m.rt.cpu().get_segment(SegReg::DS) == ds0 && m.rt.call_depth() == 0,
        "registers restored after the fault (sp %04X si %04X)", r.r_sp(), r.r_si());
  std::vector<uint8_t> ok = {0xB8, 0x05, 0x00, 0xCB};
  CHECK((m.rt.call_far(uint32_t(m.code(ok)) << 16, {}) & 0xFFFF) == 5, "the next call runs normally");
}

// WM_CREATE's CREATESTRUCT is the whole 34-byte Win16 structure (dwExStyle
// last): a window procedure reading it must not fault.
void test_create_window() {
  Machine m;
  Screen screen(64, 32);
  m.rt.attach_display(screen);
  // Window procedure: WM_CREATE → -1 unless lpcs->dwExStyle's high word is 1234h.
  // push bp; mov bp,sp; cmp word [bp+12],1; jne other; les bx,[bp+6]; mov ax,es:[bx+32];
  // cmp ax,1234h; je ok; mov ax,-1; jmp out; ok/other: xor ax,ax; out: cwd; pop bp; retf 10
  std::vector<uint8_t> wp = {0x55, 0x89, 0xE5, 0x83, 0x7E, 0x0C, 0x01, 0x75, 0x11, 0xC4, 0x5E,
                             0x06, 0x26, 0x8B, 0x47, 0x20, 0x3D, 0x34, 0x12, 0x74, 0x05, 0xB8,
                             0xFF, 0xFF, 0xEB, 0x02, 0x31, 0xC0, 0x99, 0x5D, 0xCA, 0x0A, 0x00};
  uint16_t wps = m.code(wp);
  uint16_t ds = m.data(256);
  uint32_t d = uint32_t(ds) << 16;
  m.rt.write_str(d, "TESTCLS", 16);
  // WNDCLASS: style, lpfnWndProc, cbClsExtra, cbWndExtra, hInstance, hIcon, hCursor, hbrBackground, menu, class.
  m.rt.wr32(d + 0x20 + 2, uint32_t(wps) << 16);
  m.rt.wr32(d + 0x20 + 22, d);
  auto shim = [&](const char* mod, const char* name) { return m.rt.thunk_far(*m.rt.shims().find_name(mod, name)); };
  CHECK(m.rt.call_far(shim("USER", "RegisterClass"), {l16(d + 0x20)}) & 0xFFFF, "RegisterClass");
  uint32_t hwnd = 0;
  try {
    hwnd = m.rt.call_far(shim("USER", "CreateWindowEx"),
                         {l16(0x12345678), l16(d), l16(d), l16(0), w16(0), w16(0), w16(10), w16(10), w16(0), w16(0),
                          w16(0), l16(0)}) &
           0xFFFF;
  } catch (const std::exception& e) {
    CHECK(false, "CreateWindowEx: %s", e.what());
  }
  CHECK(hwnd != 0, "WM_CREATE saw dwExStyle in its CREATESTRUCT");
}

// ---- DOS / BIOS / ports ---------------------------------------------------------------------------------

void test_dos() {
  Machine m;
  // mov ah,30h; int 21h; mov bx,ax; mov ah,2Ch; int 21h; mov ax,bx; retf  → AX = version, CX:DX time
  std::vector<uint8_t> code = {0xB4, 0x30, 0xCD, 0x21, 0x89, 0xC3, 0xB4, 0x2C, 0xCD, 0x21, 0x89, 0xD8, 0x89, 0xCA,
                               0xCB};
  uint16_t cs = m.code(code);
  uint32_t r = m.rt.call_far(uint32_t(cs) << 16, {});
  CHECK((r & 0xFFFF) == 0x0007, "DOS 7.00 (%04X)", r & 0xFFFF);
  // The Win32 lane's headless epoch (FILETIME 125132976000000000) reads as 20:00.
  CHECK((r >> 16) == 0x1400, "headless clock: 20:00 (CX=%04X)", r >> 16);
  // INT 1Ah AH=0: ticks since midnight, 20h * 18.2/s.
  std::vector<uint8_t> c2 = {0xB4, 0x00, 0xCD, 0x1A, 0x89, 0xD0, 0x89, 0xCA, 0xCB};
  uint32_t t = m.rt.call_far(uint32_t(m.code(c2)) << 16, {});
  CHECK(t >= 1310000 && t <= 1311000, "BIOS ticks at 20:00 (%u)", t);
  // Selector 0x40: mov ax,40h; mov es,ax; mov ax,es:[6Ch]; retf
  std::vector<uint8_t> c3 = {0xB8, 0x40, 0x00, 0x8E, 0xC0, 0x26, 0xA1, 0x6C, 0x00, 0xCB};
  uint32_t lo = m.rt.call_far(uint32_t(m.code(c3)) << 16, {});
  CHECK((lo & 0xFFFF) == (t & 0xFFFF) || (lo & 0xFFFF) == ((t + 1) & 0xFFFF), "0040:006C holds the ticks");
  // File I/O through the Vfs: open this test's own directory listing is host-specific, so
  // create a file in an overlay's memory upper, write, seek, read back.
  m.rt.vfs().mount_overlay("C:\\AFTERDRK", ".", "");
  uint16_t ds = m.data(256);
  m.rt.write_str(uint32_t(ds) << 16, "C:\\AFTERDRK\\SAVE.DAT", 64);
  m.rt.write_str((uint32_t(ds) << 16) | 0x40, "hello", 16);
  // mov ax,ds_sel; mov ds,ax; mov ah,3Ch; xor cx,cx; xor dx,dx; int 21h; mov bx,ax;
  // mov ah,40h; mov cx,5; mov dx,40h; int 21h; mov ax,4200h; xor cx,cx; xor dx,dx; int 21h;
  // mov ah,3Fh; mov cx,5; mov dx,80h; int 21h; push ax; mov ah,3Eh; int 21h; pop ax; retf
  std::vector<uint8_t> c4 = {0xB8, uint8_t(ds), uint8_t(ds >> 8), 0x8E, 0xD8, 0xB4, 0x3C, 0x31, 0xC9, 0x31, 0xD2,
                             0xCD, 0x21, 0x89, 0xC3, 0xB4, 0x40, 0xB9, 0x05, 0x00, 0xBA, 0x40, 0x00, 0xCD, 0x21,
                             0xB8, 0x00, 0x42, 0x31, 0xC9, 0x31, 0xD2, 0xCD, 0x21, 0xB4, 0x3F, 0xB9, 0x05, 0x00,
                             0xBA, 0x80, 0x00, 0xCD, 0x21, 0x50, 0xB4, 0x3E, 0xCD, 0x21, 0x58, 0xCB};
  uint32_t n = m.rt.call_far(uint32_t(m.code(c4)) << 16, {});
  CHECK((n & 0xFFFF) == 5 && m.rt.read_str((uint32_t(ds) << 16) | 0x80) == "hello",
        "create/write/seek/read through INT 21h (%u, \"%s\")", n & 0xFFFF,
        m.rt.read_str((uint32_t(ds) << 16) | 0x80).c_str());
  // VGA retrace (port 0x3DA) must toggle while polled: wait for set, then clear.
  // mov dx,3DAh; l1: in al,dx; test al,8; jz l1; l2: in al,dx; test al,8; jnz l2; retf
  std::vector<uint8_t> c5 = {0xBA, 0xDA, 0x03, 0xEC, 0xA8, 0x08, 0x74, 0xFB, 0xEC, 0xA8, 0x08, 0x75, 0xFB, 0xCB};
  m.rt.call_far(uint32_t(m.code(c5)) << 16, {});
  CHECK(true, "the retrace wait loop terminates");
}

// ---- a tiny NE DLL built in memory ------------------------------------------------------------------------

// The RT_RCDATA resource 7 build_ne(true) carries (16 bytes at file offset 0x310).
constexpr char kTestResource[16] = "hello, resource";

// Writes an NE DLL with a code segment and a data segment. The code segment
// has LibEntry at 0 (returns AX=1 after calling KERNEL.GetVersion through an
// import) and an exported function at 0x20 with the MSVC prolog
// `push ds; pop ax; nop; …` that returns DS. With `resource`, a resource
// table holding one resource follows the segment table: RT_RCDATA 7
// (kTestResource), or `type` `id` holding `data`.
std::string build_ne(bool resource = false, uint16_t type = 10, uint16_t id = 7,
                     std::string_view data = std::string_view(kTestResource, sizeof(kTestResource))) {
  std::string f(0x40, '\0');
  f[0] = 'M';
  f[1] = 'Z';
  uint32_t ne = 0x40;
  f[0x3C] = char(ne);
  std::string h(0x40, '\0');
  auto w16 = [](std::string& s, size_t at, uint16_t v) {
    if (s.size() < at + 2) s.resize(at + 2, '\0');
    s[at] = char(v);
    s[at + 1] = char(v >> 8);
  };
  h[0] = 'N';
  h[1] = 'E';
  // Tables after the header: segment table (2 x 8), resource table (none),
  // resident names, module refs, imported names, entry table.
  std::string seg, res, resident, modref, imp, entry;
  resident += char(6) + std::string("TESTNE") + std::string("\0\0", 2);
  resident += char(4) + std::string("FUNC") + std::string("\x01\x00", 2);
  resident += '\0';
  imp += '\0';
  imp += char(6) + std::string("KERNEL");
  w16(modref, 0, 1);  // KERNEL at imported-names offset 1
  // Entry table: one bundle of 1 moveable entry: ordinal 1 → seg 1:0x20.
  entry += char(1);
  entry += char(0xFF);
  entry += char(0x03);  // exported | shared data
  entry += char(0xCD);
  entry += char(0x3F);
  entry += char(1);
  entry += char(0x20);
  entry += char(0x00);
  entry += '\0';
  // Resource table: alignment shift 4; one type (RT_RCDATA) of one resource
  // (id 7, 16 bytes at 0x310 = 0x31 << 4; or the caller's, its length in
  // 16-byte units); the type list's end; no names.
  if (resource) {
    w16(res, 0, 4);
    w16(res, 2, 0x8000 | type);
    w16(res, 4, 1);
    w16(res, 6, 0), w16(res, 8, 0);
    w16(res, 10, 0x31), w16(res, 12, uint16_t((data.size() + 15) >> 4)), w16(res, 14, 0x30), w16(res, 16, 0x8000 | id);
    w16(res, 18, 0), w16(res, 20, 0);
    w16(res, 22, 0);
    res.push_back('\0');
  }
  uint16_t off = 0x40;
  uint16_t seg_off = off;
  off += 16;
  uint16_t res_off = off;
  off += uint16_t(res.size());
  uint16_t resident_off = off;
  off += uint16_t(resident.size());
  uint16_t modref_off = off;
  off += uint16_t(modref.size());
  uint16_t imp_off = off;
  off += uint16_t(imp.size());
  uint16_t entry_off = off;
  off += uint16_t(entry.size());
  w16(h, 0x04, entry_off);
  w16(h, 0x06, uint16_t(entry.size()));
  w16(h, 0x0C, 0x8001);  // LIBRARY | SINGLEDATA
  w16(h, 0x0E, 2);       // autodata = segment 2
  w16(h, 0x10, 0x100);   // heap
  w16(h, 0x14, 0);       // IP
  w16(h, 0x16, 1);       // CS = segment 1
  w16(h, 0x1C, 2);       // segments
  w16(h, 0x1E, 1);       // module refs
  w16(h, 0x22, seg_off);
  w16(h, 0x24, res_off);
  w16(h, 0x26, resident_off);
  w16(h, 0x28, modref_off);
  w16(h, 0x2A, imp_off);
  w16(h, 0x32, 4);  // alignment shift 4
  h[0x36] = 2;      // Windows
  w16(h, 0x3E, 0x030A);
  std::string tables = h;
  tables.resize(0x40);
  // Code segment at file 0x200, data at 0x300 (shift 4 → sectors 0x20, 0x30).
  std::string code(0x40, '\x90');
  // LibEntry: lcall KERNEL.3 (GetVersion) [fixup at 1]; mov ax,1; retf
  const uint8_t le[] = {0x9A, 0xFF, 0xFF, 0x00, 0x00, 0xB8, 0x01, 0x00, 0xCB};
  memcpy(code.data(), le, sizeof(le));
  // FUNC at 0x20: push ds; pop ax; nop; mov ax, ax(=patched DS); retf
  const uint8_t fn[] = {0x1E, 0x58, 0x90, 0xCB};
  memcpy(code.data() + 0x20, fn, sizeof(fn));
  // Relocations: 1 record: import ordinal KERNEL.3 at offset 1, pointer32.
  std::string rel;
  w16(rel, 0, 1);
  rel += char(3);  // pointer32
  rel += char(1);  // import ordinal
  w16(rel, 4, 1);  // offset
  w16(rel, 6, 1);  // module 1
  w16(rel, 8, 3);  // ordinal 3
  std::string segt;
  w16(segt, 0, 0x20);                            // sector
  w16(segt, 2, uint16_t(code.size()));           // length
  w16(segt, 4, 0x0100 | 0x0010);                 // RELOCINFO | MOVEABLE (code)
  w16(segt, 6, uint16_t(code.size()));
  w16(segt, 8, 0x30);
  w16(segt, 10, 0x10);
  w16(segt, 12, 0x0001);  // data
  w16(segt, 14, 0x10);
  std::string all = f;
  all.resize(ne);
  all += tables;
  all += segt;
  all += res;
  all += resident;
  all += modref;
  all += imp;
  all += entry;
  all.resize(0x200, '\0');
  all += code;
  all += rel;
  all.resize(0x300, '\0');
  all += std::string(0x10, '\0');
  if (resource) {
    all += std::string(data);
    all.resize((all.size() + 15) & ~size_t(15), '\0');
  }
  return all;
}

void test_ne_module() {
  Machine m;
  char tmp[MAX_PATH], dir[MAX_PATH];
  GetTempPathA(MAX_PATH, dir);
  snprintf(tmp, sizeof(tmp), "%sadw_win16_test_%lu.dll", dir, GetCurrentProcessId());
  std::string img = build_ne();
  FILE* fh = fopen(tmp, "wb");
  fwrite(img.data(), 1, img.size(), fh);
  fclose(fh);
  uint16_t err = 0;
  Module16* mod = m.rt.modules().load_host(tmp, &err);
  CHECK(mod != nullptr, "the synthetic NE DLL loads (error %u)", err);
  if (mod) {
    CHECK(mod->name == "TESTNE", "module name %s", mod->name.c_str());
    CHECK(mod->initialized, "LibEntry ran");
    CHECK(m.rt.shims().find("KERNEL", 3)->calls == 1, "the import reached KERNEL.GetVersion");
    uint32_t fn = m.rt.modules().proc_address(mod, "func");
    CHECK(fn, "GetProcAddress by name, any case");
    uint32_t lin = m.rt.linear(fn, 3);
    CHECK(m.rt.mem().read_u8(lin) == 0xB8 && m.rt.mem().read_u16l(lin + 1) == mod->dgroup,
          "prolog patched to mov ax,DGROUP");
    CHECK((m.rt.call_far(fn, {}) & 0xFFFF) == mod->dgroup, "the exported function sees its DGROUP");
    CHECK(m.rt.modules().by_handle(mod->hinstance) == mod && m.rt.modules().by_name("testne") == mod, "lookups");
    CHECK(m.rt.rd16(uint32_t(mod->hmodule) << 16) == 0x454E, "module database starts with NE");
    m.rt.modules().free(mod);
    CHECK(!m.rt.modules().by_name("TESTNE"), "FreeLibrary unloads");
  }
  DeleteFileA(tmp);
}

// ---- GDI basics on an attached display ---------------------------------------------------------------------

void test_gdi() {
  Machine m;
  Screen screen(64, 32);
  m.rt.attach_display(screen);
  Gdi16& g = m.rt.state<Gdi16>();
  uint16_t hdc = g.create_screen_dc(0);
  CHECK(hdc, "screen DC");
  // A palette with one reserved red entry, realized: it lands on index 10.
  std::vector<PALETTEENTRY> pe = {{255, 0, 0, PC_RESERVED}};
  uint16_t pal = g.create_palette(pe);
  Dc16* d = g.dc(hdc);
  d->s.palette = pal;
  g.display().realize(*g.get(pal, G16::palette)->pal, false);
  g.sync(hdc);
  uint16_t br = g.create_brush(0x01000000);  // PALETTEINDEX(0)
  g.select(hdc, br);
  PatBlt(g.host_dc(hdc), 0, 0, 8, 8, PATCOPY);
  GdiFlush();
  CHECK(screen.at(3, 3) == 10, "PALETTEINDEX(0) drew hardware index 10 (%u)", screen.at(3, 3));
  CHECK(screen.palette()[10].rgbRed == 255, "hardware palette entry 10 is red");
  g.release_dc(hdc);
}

// ---- the synthetic desktop, icons, and the GDI/INI additions (PACKAGES.md §7.3) -----------------------------

uint32_t api(Machine& m, const char* mod, const char* name, std::initializer_list<Arg16> args) {
  Shim16Entry* e = m.rt.shims().find_name(mod, name);
  if (!e) throw std::runtime_error(std::string("no shim ") + mod + "." + name);
  return m.rt.call_far(m.rt.thunk_far(*e), args);
}

void test_desktop() {
  Machine m;
  Screen screen(64, 48);
  m.rt.attach_display(screen);
  uint16_t saver = user16_saver_window(m.rt);
  uint16_t hdc = gdi16_screen_dc(m.rt, saver);
  uint32_t progman_cls = m.rt.static_bytes("t Progman", "Progman");
  // Before anything enumerates windows there is no Program Manager (BADDOG3 looks for one).
  CHECK((api(m, "USER", "FindWindow", {l16(progman_cls), l16(0)}) & 0xFFFF) == 0, "no Program Manager yet");
  CHECK(!m.rt.vfs().exists("C:\\WINDOWS\\PROGMAN.INI"), "no PROGMAN.INI yet");
  // An EnumWindows callback in host code: records each HWND; stops when told.
  std::vector<uint16_t> seen;
  size_t stop_after = 100;
  uint32_t lp_seen = 0;
  m.rt.shims().add("TESTCB", 1, "ENUMPROC", Conv16::pascal_, true, 6, [&](Call16& c) {
    seen.push_back(c.w());
    lp_seen = c.l();
    c.ret(seen.size() < stop_after ? 1 : 0);
  });
  uint32_t cb = m.rt.thunk_far(*m.rt.shims().find_name("TESTCB", "ENUMPROC"));
  uint32_t r = api(m, "USER", "EnumWindows", {l16(cb), l16(0x12345678)});
  uint16_t pm = uint16_t(api(m, "USER", "FindWindow", {l16(progman_cls), l16(0)}));
  CHECK((r & 0xFFFF) == 1 && seen.size() == 2 && seen[0] == saver && seen[1] == pm && pm && lp_seen == 0x12345678,
        "EnumWindows: the saver, then Program Manager (%zu windows)", seen.size());
  seen.clear();
  stop_after = 1;
  api(m, "USER", "EnumWindows", {l16(cb), l16(0)});
  CHECK(seen.size() == 1, "a callback returning 0 stops the enumeration");
  // What the gatherers ask of each window.
  uint16_t icon = uint16_t(api(m, "USER", "GetClassWord", {w16(pm), w16(uint16_t(-14))}));
  CHECK(icon != 0 && (api(m, "USER", "GetClassWord", {w16(saver), w16(uint16_t(-14))}) & 0xFFFF) == 0,
        "GCW_HICON: Program Manager has a class icon, the saver window none");
  CHECK((api(m, "USER", "IsWindowVisible", {w16(pm)}) & 0xFFFF) == 1, "Program Manager is visible");
  uint16_t ds = m.data(256);
  uint32_t buf = uint32_t(ds) << 16;
  api(m, "USER", "GetWindowText", {w16(pm), l16(buf), w16(40)});
  CHECK(m.rt.read_str(buf) == "Program Manager", "its title");
  api(m, "USER", "GetClassName", {w16(pm), l16(buf), w16(40)});
  CHECK(m.rt.read_str(buf) == "Progman", "its class");
  m.rt.wr16(buf, 22);
  api(m, "USER", "GetWindowPlacement", {w16(pm), l16(buf)});
  CHECK(m.rt.rd16(buf + 4) == SW_SHOWNORMAL, "shown normally, not iconic");
  // GetWindow: Z order among the top-level windows.
  uint16_t desk = uint16_t(api(m, "USER", "GetDesktopWindow", {}));
  CHECK((api(m, "USER", "GetWindow", {w16(desk), w16(5)}) & 0xFFFF) == saver, "GW_CHILD of the desktop: the saver");
  CHECK((api(m, "USER", "GetWindow", {w16(saver), w16(2)}) & 0xFFFF) == pm, "GW_HWNDNEXT: Program Manager");
  CHECK((api(m, "USER", "GetWindow", {w16(pm), w16(2)}) & 0xFFFF) == 0, "Program Manager is last");
  CHECK((api(m, "USER", "GetWindow", {w16(pm), w16(0)}) & 0xFFFF) == saver &&
            (api(m, "USER", "GetWindow", {w16(saver), w16(1)}) & 0xFFFF) == pm,
        "GW_HWNDFIRST / GW_HWNDLAST");
  // PROGMAN.INI and its groups, as ADXPL40/ADXPL310 read them.
  uint32_t groups = m.rt.static_bytes("t Groups", "Groups"), progman_ini = m.rt.static_bytes("t ini", "Progman.ini");
  uint32_t empty = m.rt.static_bytes("t empty", "");
  uint16_t n = uint16_t(api(m, "KERNEL", "GetPrivateProfileString", {l16(groups), l16(0), l16(empty), l16(buf), w16(200), l16(progman_ini)}));
  CHECK(n > 0 && m.rt.read_str(buf) == "Group1", "[Groups] keys (%u)", n);
  uint32_t g1 = m.rt.static_bytes("t Group5", "Group5");
  api(m, "KERNEL", "GetPrivateProfileString", {l16(groups), l16(g1), l16(empty), l16(buf), w16(80), l16(progman_ini)});
  std::string grp_path = m.rt.read_str(buf);
  CHECK(grp_path == "C:\\WINDOWS\\AFTERDRK.GRP", "Group5 = %s", grp_path.c_str());
  uint16_t hf = uint16_t(api(m, "KERNEL", "_lopen", {l16(buf), w16(0)}));
  CHECK(hf != 0xFFFF, "the group file opens");
  api(m, "KERNEL", "_llseek", {w16(hf), l16(0x16), w16(0)});
  api(m, "KERNEL", "_lread", {w16(hf), l16(buf + 0x80), w16(2)});
  api(m, "KERNEL", "_llseek", {w16(hf), l16(m.rt.rd16(buf + 0x80)), w16(0)});
  api(m, "KERNEL", "_lread", {w16(hf), l16(buf + 0x80), w16(40)});
  api(m, "KERNEL", "_lclose", {w16(hf)});
  CHECK(m.rt.read_str(buf + 0x80) == "After Dark", "the group's name through pName at 0x16 (%s)",
        m.rt.read_str(buf + 0x80).c_str());
  // Guest counts and offsets never size a host allocation (dos16.cc
  // DosFiles::read/write): a 4 GB _hread of a 5-byte file reads its 5 bytes,
  // and a write 2 GB out is a full disk (0 bytes written), not a 2 GB buffer.
  {
    uint32_t path = m.rt.static_bytes("t big", "C:\\WINDOWS\\BIG.DAT"), text = m.rt.static_bytes("t hello", "hello");
    uint16_t hb = uint16_t(api(m, "KERNEL", "_lcreat", {l16(path), w16(0)}));
    CHECK(hb != 0xFFFF, "_lcreat C:\\WINDOWS\\BIG.DAT");
    CHECK((api(m, "KERNEL", "_lwrite", {w16(hb), l16(text), w16(5)}) & 0xFFFF) == 5, "_lwrite 5 bytes");
    api(m, "KERNEL", "_llseek", {w16(hb), l16(0), w16(0)});
    uint32_t got = api(m, "KERNEL", "_hread", {w16(hb), l16(buf + 0xC0), l16(0xFFFFFFF0u)});
    CHECK(got == 5 && m.rt.read_str(buf + 0xC0).compare(0, 5, "hello") == 0, "_hread of 4 GB reads the 5 bytes (%u)",
          got);
    CHECK(api(m, "KERNEL", "_llseek", {w16(hb), l16(0x7FFFFF00u), w16(0)}) == 0x7FFFFF00u, "_llseek 2 GB out");
    CHECK((api(m, "KERNEL", "_lwrite", {w16(hb), l16(text), w16(1)}) & 0xFFFF) == 0, "a write there: disk full");
    api(m, "KERNEL", "_lclose", {w16(hb)});
  }
  std::string gf = progman_group_file("Games");
  uint16_t sum = 0;
  for (size_t i = 0; i + 1 < gf.size(); i += 2) sum = uint16_t(sum + (uint8_t(gf[i]) | (uint8_t(gf[i + 1]) << 8)));
  CHECK(gf.compare(0, 4, "PMCC") == 0 && sum == 0 && gf.size() % 2 == 0, "GROUPHEADER: PMCC, checksum, even size");
  // Icons: CopyIcon makes a new handle, DestroyIcon ends it; DrawIcon paints the opaque pixels.
  uint16_t copy = uint16_t(api(m, "USER", "CopyIcon", {w16(0), w16(icon)}));
  CHECK(copy && copy != icon, "CopyIcon: a new handle");
  CHECK((api(m, "USER", "DestroyIcon", {w16(copy)}) & 0xFFFF) == 1, "DestroyIcon");
  CHECK((api(m, "USER", "DestroyCursor", {w16(0x0F00)}) & 0xFFFF) == 1, "DestroyCursor");
  api(m, "USER", "DrawIcon", {w16(hdc), w16(10), w16(8), w16(icon)});
  GdiFlush();
  const auto& pal = screen.palette();
  auto rgb_at = [&](int x, int y) {
    const RGBQUAD& q = pal[screen.at(x, y)];
    return RGB(q.rgbRed, q.rgbGreen, q.rgbBlue);
  };
  CHECK(screen.at(10, 8) == 0 && screen.at(10 + 31, 8 + 31) == 0, "transparent corners stay as they were");
  CHECK(rgb_at(10 + 10, 8 + 5) == RGB(0, 0, 128), "the title bar is navy (%06lX)", rgb_at(10 + 10, 8 + 5));
  CHECK(rgb_at(10 + 1, 8 + 3) == RGB(0, 0, 0) && rgb_at(10 + 7, 8 + 13) == RGB(255, 0, 0),
        "the frame black, a program icon red");
  // CreateIcon: a 16×16 1-bpp image, WORD-aligned rows: a white box on a clear border.
  uint16_t bits = m.data(256);
  uint32_t andp = uint32_t(bits) << 16, xorp = andp + 64;
  for (uint32_t y = 0; y < 16; y++) {
    bool inner = y >= 4 && y < 12;
    m.rt.wr16(andp + 2 * y, inner ? 0xF00F : 0xFFFF);  // bytes F0 0F: x 4..11 opaque (MSB first)
    m.rt.wr16(xorp + 2 * y, inner ? 0xF00F : 0x0000);
  }
  // Rows are bytes: set them explicitly so byte order is unambiguous.
  for (uint32_t y = 4; y < 12; y++) {
    m.rt.wr8(andp + 2 * y, 0xF0);
    m.rt.wr8(andp + 2 * y + 1, 0x0F);
    m.rt.wr8(xorp + 2 * y, 0x0F);
    m.rt.wr8(xorp + 2 * y + 1, 0xF0);
  }
  uint16_t made = uint16_t(api(m, "USER", "CreateIcon", {w16(0), w16(16), w16(16), w16(1), w16(1), l16(andp), l16(xorp)}));
  CHECK(made != 0, "CreateIcon");
  api(m, "USER", "DrawIcon", {w16(hdc), w16(40), w16(20), w16(made)});
  GdiFlush();
  CHECK(rgb_at(40 + 6, 20 + 6) == RGB(255, 255, 255) && screen.at(40 + 1, 20 + 1) == 0 && screen.at(40 + 6, 20 + 1) == 0,
        "CreateIcon + DrawIcon: the white box drawn, the clear border left");
  // LoadIcon of a system icon keeps its image-less handle; ExtractIcon of a file that is not there, 0.
  CHECK((api(m, "USER", "LoadIcon", {w16(0), l16(32512)}) & 0xFFFF) == 0x0F04, "LoadIcon(NULL, IDI_APPLICATION)");
  CHECK((api(m, "SHELL", "ExtractIcon", {w16(0), l16(m.rt.static_bytes("t x", "C:\\NOPE.EXE")), w16(0)}) & 0xFFFF) == 0,
        "ExtractIcon of a missing file");
}

void test_gdi_extras() {
  Machine m;
  Screen screen(64, 48);
  m.rt.attach_display(screen);
  uint16_t saver = user16_saver_window(m.rt);
  uint16_t hdc = gdi16_screen_dc(m.rt, saver);
  CHECK(api(m, "GDI", "GetDCOrg", {w16(hdc)}) == 0, "GetDCOrg of the saver window's DC: (0, 0)");
  // A child window at (10, 20): GetDC gives a DC whose origin is its corner.
  uint32_t cls = m.rt.static_bytes("t STATIC", "STATIC");
  uint16_t child = uint16_t(api(m, "USER", "CreateWindow", {l16(cls), l16(cls), l16(WS_CHILD | WS_VISIBLE), w16(10),
                                                            w16(20), w16(16), w16(8), w16(saver), w16(0), w16(0),
                                                            l16(0)}));
  uint16_t cdc = uint16_t(api(m, "USER", "GetDC", {w16(child)}));
  CHECK(api(m, "GDI", "GetDCOrg", {w16(cdc)}) == ((20u << 16) | 10u), "GetDCOrg of a child's DC: its corner (%08X)",
        api(m, "GDI", "GetDCOrg", {w16(cdc)}));
  api(m, "USER", "ReleaseDC", {w16(child), w16(cdc)});
  // CreateBrushIndirect: solid, hollow, hatched; a solid one paints its colour.
  uint16_t ds = m.data(64);
  uint32_t lb = uint32_t(ds) << 16;
  auto brush = [&](uint16_t style, uint32_t color, uint16_t hatch) {
    m.rt.wr16(lb, style);
    m.rt.wr32(lb + 2, color);
    m.rt.wr16(lb + 6, hatch);
    return uint16_t(api(m, "GDI", "CreateBrushIndirect", {l16(lb)}));
  };
  Gdi16& g = m.rt.state<Gdi16>();
  uint16_t solid = brush(BS_SOLID, RGB(255, 0, 0), 0);
  uint16_t hollow = brush(BS_NULL, 0, 0);
  uint16_t hatched = brush(BS_HATCHED, RGB(0, 0, 255), HS_CROSS);
  CHECK(g.get(solid, G16::brush) && g.get(hollow, G16::brush) && g.get(hatched, G16::brush), "three brushes");
  CHECK(g.get(hollow, G16::brush)->style == BS_NULL && g.get(hatched, G16::brush)->hatch == HS_CROSS, "styles kept");
  g.select(hdc, solid);
  g.sync(hdc);
  PatBlt(g.host_dc(hdc), 0, 0, 4, 4, PATCOPY);
  GdiFlush();
  const RGBQUAD& q = screen.palette()[screen.at(1, 1)];
  CHECK(q.rgbRed == 255 && q.rgbGreen == 0 && q.rgbBlue == 0, "the solid brush paints red");
  uint16_t bmp = g.create_device_bitmap(8, 8, 8);
  uint16_t pat = brush(BS_PATTERN, 0, bmp);
  CHECK(g.get(pat, G16::brush) && g.get(pat, G16::brush)->pattern == bmp, "BS_PATTERN keeps its bitmap");
}

// A module that swaps palettes with its pen and brush selected (ARTIST calls
// USER.SelectPalette ~90 times a frame between two palettes while it strokes)
// re-keys them on every swap. Each key colour's real object is made once and
// reused: the process's GDI object count stays flat, where every swap used to
// make two new ones (a host at the 10,000-object quota within seconds).
void test_gdi_palette_swap() {
  Machine m;
  Screen screen(64, 48);
  m.rt.attach_display(screen);
  uint16_t saver = user16_saver_window(m.rt);
  uint16_t hdc = gdi16_screen_dc(m.rt, saver);
  Gdi16& g = m.rt.state<Gdi16>();
  uint16_t red = g.create_palette({{255, 0, 0, 0}, {0, 255, 0, 0}});
  uint16_t blue = g.create_palette({{0, 0, 255, 0}, {255, 255, 0, 0}});
  uint16_t pen = g.create_pen(PS_SOLID, 1, 0x01000000);  // PALETTEINDEX(0)
  uint16_t brush = g.create_brush(0x01000001);           // PALETTEINDEX(1)
  g.select(hdc, pen);
  g.select(hdc, brush);
  auto select_palette = [&](uint16_t pal) { api(m, "USER", "SelectPalette", {w16(hdc), w16(pal), w16(0)}); };
  select_palette(red);
  COLORREF pen_red = g.get(pen, G16::pen)->made_for;
  select_palette(blue);
  COLORREF pen_blue = g.get(pen, G16::pen)->made_for;
  CHECK(pen_red != pen_blue, "the two palettes key PALETTEINDEX(0) differently (%06lX, %06lX)", pen_red, pen_blue);
  select_palette(red);
  HGDIOBJ real_red = g.host(pen);
  const DWORD before = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
  for (int i = 0; i < 5000; i++) {
    select_palette(blue);
    select_palette(red);
  }
  const DWORD after = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
  CHECK(after <= before + 2, "10,000 palette swaps: GDI objects %lu -> %lu", before, after);
  CHECK(g.host(pen) == real_red && g.get(pen, G16::pen)->made_for == pen_red, "the pen is its first red object again");
  CHECK(GetCurrentObject(g.host_dc(hdc), OBJ_PEN) == real_red, "and that object is selected into the real DC");
  // The pen draws the colour of the palette selected now.
  PatBlt(g.host_dc(hdc), 0, 0, 64, 48, BLACKNESS);
  MoveToEx(g.host_dc(hdc), 0, 5, nullptr);
  LineTo(g.host_dc(hdc), 20, 5);
  select_palette(blue);
  MoveToEx(g.host_dc(hdc), 0, 9, nullptr);
  LineTo(g.host_dc(hdc), 20, 9);
  GdiFlush();
  const RGBQUAD& r = screen.palette()[screen.at(4, 5)];
  const RGBQUAD& b = screen.palette()[screen.at(4, 9)];
  CHECK(r.rgbRed > 128 && r.rgbBlue < 128, "drawn under the red palette: red (%u,%u,%u)", r.rgbRed, r.rgbGreen, r.rgbBlue);
  CHECK(b.rgbBlue > 128 && b.rgbRed < 128, "drawn under the blue palette: blue (%u,%u,%u)", b.rgbRed, b.rgbGreen, b.rgbBlue);
  // Deleting the guest objects frees every realization.
  api(m, "GDI", "SelectObject", {w16(hdc), w16(g.stock(BLACK_PEN))});
  api(m, "GDI", "SelectObject", {w16(hdc), w16(g.stock(WHITE_BRUSH))});
  const DWORD selected_out = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
  api(m, "GDI", "DeleteObject", {w16(pen)});
  api(m, "GDI", "DeleteObject", {w16(brush)});
  // (Solid brushes may stay in GDI's per-process brush cache: count the pens.)
  const DWORD freed = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
  CHECK(freed + 2 <= selected_out, "deleting the pen frees both its real objects (%lu -> %lu)", selected_out, freed);
}

void test_seeds() {
  Machine m;
  auto ray = [&]() {
    return profiles16(m.rt).get("C:\\WINDOWS\\MODULES.INI", "Ray", "RaySceneFile").value_or(std::string());
  };
  CHECK(ray() == "C:\\AFTERDRK\\TRACES\\ROTCUBE.TRC", "nothing mounted: ROTCUBE (%s)", ray().c_str());
  CHECK(profiles16(m.rt).get("C:\\WINDOWS\\MODULES.INI", "Logo Section", "LogoFile").value_or("") ==
            "C:\\AFTERDRK\\BITMAPS\\ADLOGO.BMP",
        "[Logo Section] LogoFile");
  // The seeds are profile seeds: the file itself exists (OF_EXIST finds it) and is empty.
  win32::Vfs::Stat st;
  CHECK(m.rt.vfs().stat("C:\\WINDOWS\\MODULES.INI", &st) && !st.dir && st.size == 0, "MODULES.INI: an empty virtual file");
  char base[MAX_PATH];
  GetTempPathA(MAX_PATH, base);
  std::string dir = std::string(base) + "adw_win16_seed_" + std::to_string(GetCurrentProcessId());
  CreateDirectoryA(dir.c_str(), nullptr);
  CreateDirectoryA((dir + "\\TRACES").c_str(), nullptr);
  auto touch = [&](const char* f) {
    FILE* fh = fopen((dir + "\\TRACES\\" + f).c_str(), "wb");
    fclose(fh);
  };
  touch("ROTPYRA.TRC");
  touch("DIAMOND.TRC");
  m.rt.vfs().mount("C:\\AFTERDRK", dir, false);
  seed_modules_ini(m.rt);
  CHECK(ray() == "C:\\AFTERDRK\\TRACES\\DIAMOND.TRC", "AD 3.2's scenes: DIAMOND first (%s)", ray().c_str());
  touch("ROTCUBE.TRC");
  seed_modules_ini(m.rt);
  CHECK(ray() == "C:\\AFTERDRK\\TRACES\\ROTCUBE.TRC", "Deluxe's: ROTCUBE (%s)", ray().c_str());
  for (const char* f : {"ROTPYRA.TRC", "DIAMOND.TRC", "ROTCUBE.TRC"}) DeleteFileA((dir + "\\TRACES\\" + f).c_str());
  RemoveDirectoryA((dir + "\\TRACES").c_str());
  RemoveDirectoryA(dir.c_str());
}

// LoadBitmap(NULL, OBM_*) and the caption metrics: INS draws a Windows 3.1
// window's chrome with them (system-menu box, minimize/maximize, scroll
// arrows) and stopped with "Out of memory" when OBM_CLOSE was 0.
void test_system_bitmaps() {
  Machine m;
  Screen screen(64, 48);
  m.rt.attach_display(screen);
  auto metric = [&](int i) { return api(m, "USER", "GetSystemMetrics", {w16(uint16_t(i))}) & 0xFFFF; };
  CHECK(metric(SM_CXSIZE) == 18 && metric(SM_CYSIZE) == 18, "SM_CXSIZE/SM_CYSIZE 18 (%u %u)", metric(SM_CXSIZE),
        metric(SM_CYSIZE));
  CHECK(metric(SM_CYCAPTION) == metric(SM_CYSIZE) + 2, "the caption holds its bitmaps and two border lines");
  CHECK(metric(SM_CYVTHUMB) == metric(SM_CYVSCROLL) && metric(SM_CXHTHUMB) == metric(SM_CXHSCROLL), "thumbs");
  CHECK(metric(SM_CXICONSPACING) == 75 && metric(SM_CYICONSPACING) == 75, "icon spacing");
  Gdi16& g = m.rt.state<Gdi16>();
  auto obm = [&](uint16_t id) { return uint16_t(api(m, "USER", "LoadBitmap", {w16(0), l16(id)})); };
  for (uint16_t id : {uint16_t(32754), uint16_t(32749), uint16_t(32748), uint16_t(32747)}) {
    uint16_t b = obm(id);
    Obj16* o = g.get(b, G16::bitmap);
    CHECK(o && o->bmp.w == 18 && o->bmp.h == 18, "OBM %u: an 18x18 bitmap (%04X)", id, b);
  }
  for (uint16_t id : {uint16_t(32753), uint16_t(32752), uint16_t(32751), uint16_t(32750)}) {
    Obj16* o = g.get(obm(id), G16::bitmap);
    CHECK(o && o->bmp.w == 16 && o->bmp.h == 16, "scroll arrow OBM %u: 16x16", id);
  }
  CHECK(obm(32767) == 0, "an OBM this host does not draw stays 0");
  // OBM_REDUCE: a raised grey face (white top-left, black bottom-right edge) with a black glyph.
  Obj16* o = g.get(obm(32749), G16::bitmap);
  if (o && o->bmp.bits) {
    auto rgb = [&](int x, int y) { return g.display().hardware_color(o->bmp.bits[size_t(y) * o->bmp.stride + size_t(x)]); };
    CHECK(rgb(0, 0) == RGB(255, 255, 255) && rgb(17, 17) == RGB(0, 0, 0) && rgb(3, 3) == RGB(192, 192, 192),
          "OBM_REDUCE edges and face (%06lX %06lX %06lX)", rgb(0, 0), rgb(17, 17), rgb(3, 3));
    int black = 0;
    for (int y = 4; y < 14; y++)
      for (int x = 4; x < 14; x++) black += rgb(x, y) == RGB(0, 0, 0);
    CHECK(black >= 12, "OBM_REDUCE has its triangle (%d black pixels)", black);
  }
}

// Runtime16's frame-bounded time (runtime16.hh): with insns_per_us set, the
// work of a frame runs inside its period — reads within a frame advance by
// their nudges, the next frame starts at its grid time again, and only work
// past the period pushes the clock on. insns_per_us 0 keeps the accumulating
// nudges on the core clock.
void test_frame_time() {
  {
    Machine m;  // read_step_us 5, 100 MIPS, 500 per API call; frames of 16667 µs
    m.clock.begin_frame();
    uint64_t a = m.rt.clock_us(), b = m.rt.clock_us();
    CHECK(a == 5 && b == 10, "reads in frame 0 nudge 5 µs each (%llu %llu)", (unsigned long long)a,
          (unsigned long long)b);
    for (int i = 0; i < 20; i++) m.rt.clock_us();
    m.clock.begin_frame();
    uint64_t c = m.rt.clock_us();
    CHECK(c == 16667 + 5, "frame 1 starts on the grid: earlier nudges do not carry over (%llu)", (unsigned long long)c);
    // An API call's cost reaches the next read: 500 instructions = 5 µs.
    api(m, "USER", "GetTickCount", {});
    uint64_t d = m.rt.clock_us();
    CHECK(d >= c + 10, "an API call's cost moves the clock (%llu -> %llu)", (unsigned long long)c,
          (unsigned long long)d);
    // Work past the period pushes the clock beyond the grid, and the next frame continues from there.
    while (m.rt.clock_us() < 2 * 16667 + 1000) {
    }
    uint64_t e = m.rt.peek_us();
    m.clock.begin_frame();
    uint64_t f = m.rt.clock_us();
    CHECK(f > e && f < 2 * 16667 + 1100, "an overrun frame's time carries on (%llu after %llu)", (unsigned long long)f,
          (unsigned long long)e);
    CHECK(m.rt.modeled_time(), "headless with insns_per_us: modeled");
  }
  {
    VirtualClock clock{VirtualClock::Mode::fixed_step, 16667};
    Runtime16Options o;
    o.insns_per_us = 0;
    Runtime16 rt{o, clock};
    clock.set_read_step_us(5);
    clock.begin_frame();
    for (int i = 0; i < 20; i++) rt.clock_us();
    clock.begin_frame();
    uint64_t c = rt.clock_us();
    CHECK(!rt.modeled_time() && c == 16667 + 21 * 5, "insns_per_us 0: the nudges accumulate (%llu)",
          (unsigned long long)c);
  }
  // settle_time(): a frame's work without a clock read is charged inside
  // that frame, not on top of the next frame's grid line at the next read.
  {
    Machine m;
    m.clock.begin_frame();
    // mov cx, 0x4000; loop $; retf — some 16K instructions (~164 µs at 100 MIPS), no clock read.
    uint16_t cs = m.code({0xB9, 0x00, 0x40, 0xE2, 0xFE, 0xCB});
    m.rt.call_far(uint32_t(cs) << 16, {});
    m.rt.settle_time();
    m.clock.begin_frame();
    uint64_t t = m.rt.clock_us();
    CHECK(t == 16667 + 5, "settled: frame 1's first read is on its grid line (%llu)", (unsigned long long)t);
    m.rt.call_far(uint32_t(cs) << 16, {});
    m.clock.begin_frame();
    uint64_t u = m.rt.clock_us();
    CHECK(u > 2 * 16667 + 100, "unsettled: the previous frame's work lands on this frame's grid line (%llu)",
          (unsigned long long)u);
  }
  // A streamed (realtime) clock only runs from frame 0: until start_frames()
  // clock reads are modeled (init's calibration loops must see time move —
  // EINSTEIN, GLOBE and Om Appliances spun forever), then they follow the wall
  // clock plus the time init took, never going back.
  {
    uint64_t wall = 1'000'000;
    VirtualClock clock{VirtualClock::Mode::realtime, 16667, [&] { return wall; }};
    Runtime16 rt{Runtime16Options{}, clock};
    clock.set_read_step_us(0);  // what the lane sets with insns_per_us
    CHECK(rt.modeled_time(), "realtime before the first frame: modeled");
    uint64_t a = rt.clock_us(), b = rt.clock_us();
    CHECK(b > a && a > 0, "reads during init advance (%llu %llu)", (unsigned long long)a, (unsigned long long)b);
    for (int i = 0; i < 1000; i++) rt.clock_us();
    uint64_t c = rt.clock_us();
    clock.begin_frame();
    rt.start_frames();
    CHECK(!rt.modeled_time(), "realtime after start_frames: the wall clock");
    uint64_t d = rt.clock_us();
    wall += 20000;
    uint64_t e = rt.clock_us();
    CHECK(d >= c && d < c + 100, "no jump back at the first frame (%llu after %llu)", (unsigned long long)d,
          (unsigned long long)c);
    CHECK(e == d + 20000, "then it follows the wall (%llu -> %llu)", (unsigned long long)d, (unsigned long long)e);
  }
  // set_deadline(): the hook runs once, at the first API call at or after the deadline.
  {
    Machine m;
    m.clock.begin_frame();
    int fired = 0;
    m.rt.set_deadline(1000, [&] { fired++; });
    api(m, "USER", "GetTickCount", {});
    CHECK(fired == 0, "before the deadline: no hook");
    while (m.rt.clock_us() < 1000) {
    }
    api(m, "USER", "GetTickCount", {});
    api(m, "USER", "GetTickCount", {});
    CHECK(fired == 1, "at the deadline: the hook ran once (%d)", fired);
  }
}

// adw::cpu regression, found by this lane: `inc`/`dec` must leave DF alone.
// X86Exec.cc's inc/dec r16/r32 and FE/FF forms once passed ~Regs::CF as the
// flag mask, which rewrote every other EFLAGS bit — DF included — so a
// backward memmove (`std; … dec si; dec di; rep movsw`, ADXPL300/AD_RSRC) ran
// forward and faulted past its source block (fixed with
// Regs::default_int_flags & ~Regs::CF; this pins it).
int run_cpu_df() {
  Machine m;
  // std; mov si,5; dec si; inc di; pushf; pop ax; cld; retf
  std::vector<uint8_t> code = {0xFD, 0xBE, 0x05, 0x00, 0x4E, 0x47, 0x9C, 0x58, 0xFC, 0xCB};
  uint32_t r = m.rt.call_far(uint32_t(m.code(code)) << 16, {});
  CHECK((r & 0x0400) != 0, "DF survives dec si / inc di (flags %04X)", r & 0xFFFF);
  // FE/FF forms: std; mov bx,1; dec bx (FF CB); pushf; pop ax; cld; retf
  std::vector<uint8_t> code2 = {0xFD, 0xBB, 0x01, 0x00, 0xFF, 0xCB, 0x9C, 0x58, 0xFC, 0xCB};
  r = m.rt.call_far(uint32_t(m.code(code2)) << 16, {});
  CHECK((r & 0x0400) != 0, "DF survives dec r/m16 (flags %04X)", r & 0xFFFF);
  printf("%d/%d checks passed\n", checks - failures, checks);
  return failures ? 1 : 0;
}

// ---- interaction (INTERACTION.md §5.2, §6.2 Win16, §7) ------------------------------------------------------

void put16(std::string& s, uint16_t v) {
  s.push_back(char(v));
  s.push_back(char(v >> 8));
}
void put32(std::string& s, uint32_t v) {
  put16(s, uint16_t(v));
  put16(s, uint16_t(v >> 16));
}
uint16_t le16(const std::vector<uint8_t>& b, size_t o) { return uint16_t(b[o] | (b[o + 1] << 8)); }
uint32_t le32(const std::vector<uint8_t>& b, size_t o) { return le16(b, o) | (uint32_t(le16(b, o + 2)) << 16); }
std::wstring wstr_at(const std::vector<uint8_t>& b, size_t* o) {
  std::wstring w;
  while (*o + 1 < b.size() && le16(b, *o)) {
    w.push_back(wchar_t(le16(b, *o)));
    *o += 2;
  }
  *o += 2;
  return w;
}

// A Win16 DLGTEMPLATE, byte by byte: DS_SETFONT, a named menu (dropped), a
// custom dialog class (dropped), a caption with a 1252 character, 8 pt Helv;
// a predefined button, a custom-class item, an SS_ICON static (its icon
// ordinal is the guest's: dropped) and an item with two extra bytes.
std::string win16_template() {
  std::string t;
  put32(t, WS_POPUP | WS_CAPTION | DS_MODALFRAME | DS_SETFONT | DS_SYSMODAL);
  t.push_back(4);
  put16(t, 10), put16(t, 20), put16(t, 200), put16(t, 100);
  t += "MYMENU";
  t.push_back('\0');
  t += "MyDlgClass";
  t.push_back('\0');
  t += "Caf\xE9 Settings";
  t.push_back('\0');
  put16(t, 8);
  t += "Helv";
  t.push_back('\0');
  auto item = [&](int16_t x, uint16_t id, uint32_t style, const std::string& cls, bool atom, const std::string& text,
                  bool ordinal, std::string extra) {
    put16(t, uint16_t(x)), put16(t, 5), put16(t, 50), put16(t, 14), put16(t, id);
    put32(t, style);
    if (atom) {
      t.push_back(cls[0]);
    } else {
      t += cls;
      t.push_back('\0');
    }
    if (ordinal) {
      t.push_back(char(0xFF));
      put16(t, uint16_t(atoi(text.c_str())));
    } else {
      t += text;
      t.push_back('\0');
    }
    t.push_back(char(extra.size()));
    t += extra;
  };
  item(1, IDOK, WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, "\x80", true, "&OK", false, "");
  item(2, 102, WS_CHILD | WS_VISIBLE, "LunaticKey", false, "", false, "");
  item(3, 103, WS_CHILD | WS_VISIBLE | SS_ICON, "\x82", true, "100", true, "");
  item(4, 104, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, "EDIT", false, "abc", false, std::string("\x01\x02", 2));
  return t;
}

void test_template_converter() {
  std::string t16 = win16_template();
  DialogTemplate32 t = convert_dialog_template16(t16, WS_EX_TOOLWINDOW);
  CHECK(t.ok, "the template converts (%s)", t.error.c_str());
  if (!t.ok) return;
  const std::vector<uint8_t>& b = t.bytes;
  CHECK(le32(b, 0) == (WS_POPUP | WS_CAPTION | DS_MODALFRAME | DS_SETFONT), "style kept, DS_SYSMODAL dropped (%08X)",
        le32(b, 0));
  CHECK(le32(b, 4) == WS_EX_TOOLWINDOW && le16(b, 8) == 4, "extended style, 4 items");
  CHECK(int16_t(le16(b, 10)) == 10 && le16(b, 12) == 20 && le16(b, 14) == 200 && le16(b, 16) == 100, "x/y/cx/cy");
  CHECK(le16(b, 18) == 0 && le16(b, 20) == 0, "no menu, no dialog class");
  size_t o = 22;
  std::wstring cap = wstr_at(b, &o);
  CHECK(cap == L"Caf\u00E9 Settings", "the caption from code page 1252");
  uint16_t pt = le16(b, o);
  o += 2;
  CHECK(pt == 8 && wstr_at(b, &o) == L"Helv", "8 pt Helv");
  // Items: DWORD aligned; style, exstyle, x, y, cx, cy, id, class, title, creation data.
  struct Item {
    uint32_t style;
    uint16_t id, atom;
    std::wstring cls, title;
    uint16_t title_ord = 0, extra = 0;
  };
  std::vector<Item> items;
  for (int i = 0; i < 4; i++) {
    o = (o + 3) & ~size_t(3);
    Item it;
    it.style = le32(b, o);
    CHECK(le32(b, o + 4) == 0, "item %d: no extended style", i);
    it.id = le16(b, o + 16);
    o += 18;
    if (le16(b, o) == 0xFFFF) {
      it.atom = le16(b, o + 2);
      o += 4;
    } else {
      it.atom = 0;
      it.cls = wstr_at(b, &o);
    }
    if (le16(b, o) == 0xFFFF) {
      it.title_ord = le16(b, o + 2);
      o += 4;
    } else {
      it.title = wstr_at(b, &o);
    }
    it.extra = le16(b, o);
    o += 2;
    items.push_back(it);
  }
  CHECK(o == b.size(), "the whole template was walked (%zu of %zu)", o, b.size());
  CHECK(items[0].atom == 0x80 && items[0].id == IDOK && items[0].title == L"&OK", "a predefined Button");
  CHECK(items[1].atom == 0 && items[1].cls == L"LunaticKey", "a custom class by name");
  CHECK(items[2].atom == 0x82 && items[2].title.empty() && items[2].title_ord == 0, "SS_ICON loses the guest's icon");
  CHECK(items[3].atom == 0x81 && items[3].title == L"abc" && items[3].extra == 0, "\"EDIT\" by name is the Edit atom; "
        "the guest's extra bytes are dropped");
  CHECK(t.classes.size() == 1 && t.classes[0] == "LunaticKey", "custom classes reported");
  // Windows takes it: a real (invisible) dialog from the converted template.
  WNDCLASSEXW wc{sizeof(wc)};
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"LunaticKey";
  RegisterClassExW(&wc);
  HWND dlg = CreateDialogIndirectParamW(GetModuleHandleW(nullptr), reinterpret_cast<LPCDLGTEMPLATEW>(b.data()), nullptr,
                                        [](HWND, UINT, WPARAM, LPARAM) -> INT_PTR { return FALSE; }, 0);
  CHECK(dlg != nullptr, "CreateDialogIndirectParamW accepts it (error %lu)", GetLastError());
  if (dlg) {
    wchar_t buf[32] = {};
    GetDlgItemTextW(dlg, 104, buf, 32);
    CHECK(GetDlgItem(dlg, IDOK) && GetDlgItem(dlg, 102) && std::wstring(buf) == L"abc", "its items exist");
    DestroyWindow(dlg);
  }
  UnregisterClassW(L"LunaticKey", GetModuleHandleW(nullptr));
  // Truncated input fails cleanly.
  CHECK(!convert_dialog_template16(t16.substr(0, 20)).ok, "a truncated header is refused");
  CHECK(!convert_dialog_template16(t16.substr(0, t16.size() - 3)).ok, "a truncated item is refused");
}

void test_message_table() {
  // Win16's WM_USER-based control messages, by class (§6.2).
  CHECK(msg16_to_32(Ctl16::edit, WM_USER + 1) == EM_SETSEL && msg16_to_32(Ctl16::edit, WM_USER + 0x15) == EM_LIMITTEXT,
        "EM_SETSEL, EM_LIMITTEXT");
  CHECK(msg16_to_32(Ctl16::edit, WM_USER + 0x14) == EM_GETLINE && msg16_to_32(Ctl16::edit, WM_USER + 0x12) == EM_REPLACESEL,
        "EM_GETLINE, EM_REPLACESEL");
  CHECK(msg16_to_32(Ctl16::edit, WM_USER + 0x0C) == 0 && msg16_to_32(Ctl16::edit, WM_USER + 0x0D) == 0,
        "EM_SETHANDLE/EM_GETHANDLE: no Win32 form");
  CHECK(msg16_to_32(Ctl16::button, WM_USER + 1) == BM_SETCHECK && msg16_to_32(Ctl16::button, WM_USER + 0) == BM_GETCHECK,
        "BM_SETCHECK, BM_GETCHECK");
  CHECK(msg16_to_32(Ctl16::listbox, WM_USER + 1) == LB_ADDSTRING && msg16_to_32(Ctl16::listbox, WM_USER + 0x0A) == LB_GETTEXT &&
            msg16_to_32(Ctl16::listbox, WM_USER + 0x0E) == LB_DIR && msg16_to_32(Ctl16::listbox, WM_USER + 0x23) == LB_FINDSTRINGEXACT,
        "LB_ADDSTRING, LB_GETTEXT, LB_DIR, LB_FINDSTRINGEXACT");
  CHECK(msg16_to_32(Ctl16::listbox, WM_USER) == 0, "WM_USER itself is no list-box message");
  CHECK(msg16_to_32(Ctl16::combobox, WM_USER + 3) == CB_ADDSTRING && msg16_to_32(Ctl16::combobox, WM_USER + 0x0E) == CB_SETCURSEL &&
            msg16_to_32(Ctl16::combobox, WM_USER + 8) == CB_GETLBTEXT,
        "CB_ADDSTRING, CB_SETCURSEL, CB_GETLBTEXT");
  CHECK(msg16_to_32(Ctl16::statik, WM_USER) == 0 && msg16_to_32(Ctl16::scrollbar, WM_USER + 1) == 0,
        "static icons and scroll bars: none");
  CHECK(msg16_to_32(Ctl16::other, WM_USER + 7) == WM_USER + 7 && msg16_to_32(Ctl16::edit, WM_SETTEXT) == WM_SETTEXT,
        "dialogs' and custom controls' own messages, and standard ones, pass");
  for (Ctl16 k : {Ctl16::button, Ctl16::edit, Ctl16::listbox, Ctl16::combobox}) {
    for (uint16_t m = WM_USER; m < WM_USER + 0x30; m++) {
      uint32_t w = msg16_to_32(k, m);
      if (w) CHECK(msg32_to_16(k, w) == m, "round trip %04X", m);
    }
  }
  CHECK(control_kind16(L"ComboBox") == Ctl16::combobox && control_kind16(L"COMBOLBOX") == Ctl16::listbox &&
            control_kind16(L"button") == Ctl16::button && control_kind16(L"LunaticKey") == Ctl16::other,
        "classes by real name");
}

void test_keyboard_tables() {
  CHECK(vk_scan_code('A') == 0x1E && vk_scan_code('1') == 0x02 && vk_scan_code(VK_SPACE) == 0x39 &&
            vk_scan_code(VK_LEFT) == 0x4B,
        "US scan codes");
  CHECK(key_lparam(VK_LEFT, true, false) == (1u | (0x4Bu << 16) | (1u << 24)), "key down: repeat 1, scan, extended");
  CHECK(key_lparam('A', false, true) == (1u | (0x1Eu << 16) | (1u << 30) | (1u << 31)), "key up: previous + transition");
  CHECK(key_lparam('A', true, true) & (1u << 30), "auto-repeat: previous state");
  CHECK(vk_to_char('A', false, false) == 'a' && vk_to_char('A', true, false) == 'A' && vk_to_char('A', false, true) == 'A' &&
            vk_to_char('A', true, true) == 'a',
        "letters with Shift and Caps Lock");
  CHECK(vk_to_char('1', true, false) == '!' && vk_to_char(VK_OEM_2, true, false) == '?' && vk_to_char(VK_F1, false, false) == -1,
        "shifted symbols; F1 has none");
}

// Hook procedures as host thunks: each records its call, and either chains
// (DefHookProc / CallNextHookEx) or answers.
void test_hooks() {
  Machine m;
  std::vector<std::string> calls;
  uint32_t token_old = 0, token_new = 0;
  uint16_t ds = m.data(64);
  uint32_t token_slot = uint32_t(ds) << 16;
  m.rt.shims().add("TESTHOOK", 1, "OLDHOOK", Conv16::pascal_, false, 8, [&](Call16& c) {
    int16_t code = c.sw();
    uint16_t vk = c.w();
    c.l();
    calls.push_back("old(" + std::to_string(code) + "," + std::to_string(vk) + ")");
    c.ret32(vk == 'X' ? 1 : 0);  // the older hook eats X
  });
  m.rt.shims().add("TESTHOOK", 2, "NEWHOOK", Conv16::pascal_, false, 8, [&](Call16& c) {
    int16_t code = c.sw();
    uint16_t vk = c.w();
    uint32_t lp = c.l();
    calls.push_back("new(" + std::to_string(vk) + ")");
    // Win 3.0 style: DefHookProc(code, wParam, lParam, &lpfnNext).
    uint32_t r = api(m, "USER", "DefHookProc", {w16(uint16_t(code)), w16(vk), l16(lp), l16(token_slot)});
    c.ret32(r);
  });
  uint32_t old_proc = m.rt.thunk_far(*m.rt.shims().find_name("TESTHOOK", "OLDHOOK"));
  uint32_t new_proc = m.rt.thunk_far(*m.rt.shims().find_name("TESTHOOK", "NEWHOOK"));
  CHECK(!user16_has_keyboard_hook(m.rt), "no hook yet");
  CHECK(api(m, "USER", "SetWindowsHook", {w16(WH_MOUSE), l16(old_proc)}) == 0, "other hook kinds are refused");
  token_old = api(m, "USER", "SetWindowsHook", {w16(WH_KEYBOARD), l16(old_proc)});
  token_new = api(m, "USER", "SetWindowsHookEx", {w16(WH_KEYBOARD), l16(new_proc), w16(0), w16(0)});
  m.rt.wr32(token_slot, token_new);
  CHECK(token_old && token_new && token_old != token_new && user16_has_keyboard_hook(m.rt), "two keyboard hooks");
  CHECK(!user16_keyboard_hooks(m.rt, 'A', key_lparam('A', true, false), 1), "A goes through (0)");
  CHECK(calls.size() == 2 && calls[0] == "new(65)" && calls[1] == "old(0,65)",
        "most recent first, DefHookProc reaches the older one (%s %s)", calls.size() > 0 ? calls[0].c_str() : "",
        calls.size() > 1 ? calls[1].c_str() : "");
  calls.clear();
  CHECK(user16_keyboard_hooks(m.rt, 'X', key_lparam('X', true, false), 2), "X is consumed (the older hook's 1)");
  // CallNextHookEx from the older hook: nothing after it.
  CHECK(api(m, "USER", "CallNextHookEx", {l16(token_old), w16(0), w16('Q'), l16(0)}) == 0, "the end of the chain: 0");
  // Unhook the newer by token, the older by (kind, proc).
  CHECK((api(m, "USER", "UnhookWindowsHookEx", {l16(token_new)}) & 0xFFFF) == 1, "UnhookWindowsHookEx");
  calls.clear();
  user16_keyboard_hooks(m.rt, 'B', key_lparam('B', true, false), 3);
  CHECK(calls.size() == 1 && calls[0] == "old(0,66)", "only the older one is left");
  CHECK((api(m, "USER", "UnhookWindowsHook", {w16(WH_KEYBOARD), l16(old_proc)}) & 0xFFFF) == 1 &&
            !user16_has_keyboard_hook(m.rt),
        "UnhookWindowsHook");
}

// The saver window's queue: tagged input, filters, consumption, drops.
void test_saver_queue() {
  Machine m;
  Screen screen(64, 48);
  m.rt.attach_display(screen);
  InputState in;
  m.rt.set_input(&in);
  uint16_t saver = user16_saver_window(m.rt);
  uint16_t ds = m.data(256);
  uint32_t msg = uint32_t(ds) << 16;
  auto peek = [&](uint16_t hwnd, uint16_t lo, uint16_t hi, uint16_t flags) {
    return api(m, "USER", "PeekMessage", {l16(msg), w16(hwnd), w16(lo), w16(hi), w16(flags)}) & 0xFFFF;
  };
  // FindWindow("Sleep", NULL): the blanker LUNATIC looks for.
  uint32_t sleep_cls = m.rt.static_bytes("t Sleep", "Sleep");
  CHECK((api(m, "USER", "FindWindow", {l16(sleep_cls), l16(0)}) & 0xFFFF) == saver, "FindWindow(\"Sleep\") = the saver");
  StepReport16 r0 = user16_end_step(m.rt);
  user16_post_input(m.rt, WM_KEYDOWN, 'A', key_lparam('A', true, false), 7);
  user16_post_input(m.rt, WM_KEYUP, 'A', key_lparam('A', false, true), 8);
  // A module's own posted message comes first.
  api(m, "USER", "PostMessage", {w16(saver), w16(WM_USER + 5), w16(1), l16(2)});
  CHECK(peek(0, WM_KEYFIRST, WM_KEYLAST, PM_NOREMOVE) == 1 && m.rt.rd16(msg + 2) == WM_KEYDOWN, "a key range skips the "
        "posted message");
  CHECK(peek(0, 0, 0, PM_NOREMOVE) == 1 && m.rt.rd16(msg + 2) == WM_USER + 5, "posted before input");
  CHECK(peek(0x7777, 0, 0, PM_NOREMOVE) == 0, "another window's filter finds nothing");
  CHECK(peek(saver, WM_MOUSEFIRST, WM_MOUSELAST, PM_REMOVE) == 0, "a mouse range finds no key");
  // Remove the key down, translate it, keep it; dispatch the key up back.
  CHECK(peek(saver, WM_KEYDOWN, WM_KEYDOWN, PM_REMOVE) == 1 && m.rt.rd16(msg + 4) == 'A', "WM_KEYDOWN removed");
  in.keys.set(VK_SHIFT);
  CHECK((api(m, "USER", "TranslateMessage", {l16(msg)}) & 0xFFFF) == 1, "TranslateMessage");
  CHECK(peek(saver, WM_CHAR, WM_CHAR, PM_REMOVE) == 1 && m.rt.rd16(msg + 4) == 'A', "WM_CHAR 'A' (Shift) next");
  CHECK(peek(saver, WM_KEYUP, WM_KEYUP, PM_REMOVE) == 1, "WM_KEYUP removed");
  api(m, "USER", "DispatchMessage", {l16(msg)});
  StepReport16 r1 = user16_end_step(m.rt);
  CHECK(r1.consumed == 7, "the down (and its char) consumed, the dispatched up not (%llu)", (unsigned long long)r1.consumed);
  CHECK(r1.queue_reads > r0.queue_reads, "reading the saver's queue with removal for keys counts (key-filter)");
  // Untaken input is dropped after the step; the module's own message stays.
  user16_post_input(m.rt, WM_KEYDOWN, 'B', key_lparam('B', true, false), 9);
  StepReport16 r2 = user16_end_step(m.rt);
  CHECK(r2.dropped == 1 && r2.consumed == 0 && peek(0, WM_KEYFIRST, WM_KEYLAST, PM_NOREMOVE) == 0, "the untaken key dropped");
  CHECK(peek(0, 0, 0, PM_REMOVE) == 1 && m.rt.rd16(msg + 2) == WM_USER + 5, "the module's own message stays");
  // Kept for a suspended call that reads the queue: pending until taken.
  user16_post_input(m.rt, WM_KEYDOWN, 'C', key_lparam('C', true, false), 10);
  StepReport16 k1 = user16_end_step(m.rt, 3);
  CHECK(k1.dropped == 0 && k1.pending == 10, "kept: pending %llu", (unsigned long long)k1.pending);
  StepReport16 k2 = user16_end_step(m.rt, 3);
  CHECK(k2.dropped == 0 && k2.pending == 10, "still kept a step later");
  CHECK(peek(saver, WM_KEYFIRST, WM_KEYLAST, PM_REMOVE) == 1 && m.rt.rd16(msg + 4) == 'C', "taken late");
  StepReport16 k3 = user16_end_step(m.rt, 3);
  CHECK(k3.consumed == 10 && k3.pending == 0, "taken late is consumed, nothing pending");
  user16_post_input(m.rt, WM_KEYDOWN, 'D', key_lparam('D', true, false), 11);
  for (int i = 0; i < 3; i++) user16_end_step(m.rt, 3);
  StepReport16 k4 = user16_end_step(m.rt, 3);
  CHECK(k4.dropped == 1 && k4.pending == 0, "dropped once kept keep_steps steps");
  // A mouse-range read with removal is no key read.
  uint64_t reads = user16_end_step(m.rt).queue_reads;
  CHECK(reads > r2.queue_reads, "the all-messages read above was one");
  peek(0, WM_MOUSEFIRST, WM_MOUSELAST, PM_REMOVE);
  CHECK(user16_end_step(m.rt).queue_reads == reads, "a mouse-only read is no key read");
  // WM_CLOSE posted to the saver: wake.
  CHECK(!user16_end_step(m.rt).wake, "no wake yet");
  api(m, "USER", "PostMessage", {w16(saver), w16(WM_SYSCOMMAND), w16(SC_CLOSE), l16(0)});
  CHECK(user16_end_step(m.rt).wake, "SC_CLOSE to the saver window wakes");
  // The mouse buttons from the MOUSE bitmask.
  in.mouse_buttons = 2;
  CHECK((api(m, "USER", "GetAsyncKeyState", {w16(VK_RBUTTON)}) & 0x8000) && !(api(m, "USER", "GetAsyncKeyState", {w16(VK_LBUTTON)}) & 0x8000),
        "VK_RBUTTON from the bitmask");
  in.mouse_buttons = 4;
  CHECK((api(m, "USER", "GetKeyState", {w16(VK_MBUTTON)}) & 0x8000) != 0, "VK_MBUTTON from the bitmask");
  m.rt.set_input(nullptr);
}

// The overlay (INTERACTION.md §7.3) under the DOS and profile calls: copy-up
// into a persistent upper, the lower untouched; seeds under the file; the
// upper-only file deleted and renamed; directories made.
void test_overlay16() {
  char base[MAX_PATH];
  GetTempPathA(MAX_PATH, base);
  std::string root = std::string(base) + "adw_win16_ovl_" + std::to_string(GetCurrentProcessId());
  std::string lower = root + "\\lower", upper = root + "\\upper";
  CreateDirectoryA(root.c_str(), nullptr);
  CreateDirectoryA(lower.c_str(), nullptr);
  {
    FILE* f = fopen((lower + "\\DATA.TXT").c_str(), "wb");
    fputs("lower", f);
    fclose(f);
    f = fopen((lower + "\\MINE.INI").c_str(), "wb");
    fputs("[A]\r\nx=1\r\n", f);
    fclose(f);
  }
  {
    Machine m;
    m.rt.vfs().mount_overlay("C:\\AFTERDRK", lower, upper);
    m.rt.vfs().mount_overlay("C:\\WINDOWS", "", root + "\\win");
    DosFiles& d = m.rt.state<DosFiles>();
    int h = d.open("C:\\AFTERDRK\\DATA.TXT", 2, false);
    uint16_t ds = m.data(64);
    m.rt.write_str(uint32_t(ds) << 16, "upper", 16);
    CHECK(h >= 5 && d.write(uint16_t(h), uint32_t(ds) << 16, 5) == 5 && d.close(uint16_t(h)) == 0, "write through a handle");
    std::string text;
    FILE* f = fopen((upper + "\\DATA.TXT").c_str(), "rb");
    char buf[16] = {};
    if (f) {
      fread(buf, 1, 15, f);
      fclose(f);
    }
    text = buf;
    CHECK(text == "upper", "copied up into the upper layer (%s)", text.c_str());
    f = fopen((lower + "\\DATA.TXT").c_str(), "rb");
    memset(buf, 0, sizeof(buf));
    fread(buf, 1, 15, f);
    fclose(f);
    CHECK(std::string(buf) == "lower", "the lower file is untouched");
    CHECK(d.remove("C:\\AFTERDRK\\MINE.INI") < 0, "a lower file cannot be deleted");
    CHECK(d.make_dir("C:\\AFTERDRK\\NEWDIR") == 0 && d.is_dir("C:\\AFTERDRK\\NEWDIR"), "mkdir in the upper layer");
    int h2 = d.open("C:\\AFTERDRK\\NEWDIR\\T.DAT", 2, true);
    d.close(uint16_t(h2));
    CHECK(d.rename("C:\\AFTERDRK\\NEWDIR\\T.DAT", "C:\\AFTERDRK\\NEWDIR\\U.DAT") == 0 && d.exists("C:\\AFTERDRK\\NEWDIR\\U.DAT"),
          "rename in the upper layer");
    CHECK(d.remove("C:\\AFTERDRK\\NEWDIR\\U.DAT") == 0 && !d.exists("C:\\AFTERDRK\\NEWDIR\\U.DAT"), "delete an upper-only file");
    CHECK(d.open("C:\\AFTERDRK\\DATA.TXT", 2, true, true) < 0, "create-new of an existing file fails");
    // Profiles: seeds under the file, writes to the upper file only.
    uint32_t sec = m.rt.static_bytes("t sec A", "A"), key_x = m.rt.static_bytes("t key x", "x"),
             key_y = m.rt.static_bytes("t key y", "y"), val = m.rt.static_bytes("t val", "2"),
             file = m.rt.static_bytes("t file", "C:\\AFTERDRK\\MINE.INI"), def = m.rt.static_bytes("t def", "d");
    profiles16(m.rt).add_seed("C:\\AFTERDRK\\MINE.INI", "A", "y", "seed");
    uint32_t out = uint32_t(ds) << 16;
    api(m, "KERNEL", "GetPrivateProfileString", {l16(sec), l16(key_y), l16(def), l16(out), w16(32), l16(file)});
    CHECK(m.rt.read_str(out) == "seed", "a seed shows under the file (%s)", m.rt.read_str(out).c_str());
    CHECK((api(m, "KERNEL", "GetPrivateProfileInt", {l16(sec), l16(key_x), w16(7), l16(file)}) & 0xFFFF) == 1,
          "the file's own key");
    CHECK((api(m, "KERNEL", "WritePrivateProfileString", {l16(sec), l16(key_x), l16(val), l16(file)}) & 0xFFFF) == 1,
          "WritePrivateProfileString");
    CHECK((api(m, "KERNEL", "GetPrivateProfileInt", {l16(sec), l16(key_x), w16(7), l16(file)}) & 0xFFFF) == 2, "read back");
  }
  std::string ini;
  if (FILE* f = fopen((upper + "\\MINE.INI").c_str(), "rb")) {
    char buf[256] = {};
    fread(buf, 1, 255, f);
    fclose(f);
    ini = buf;
  }
  CHECK(ini.find("x=2") != std::string::npos && ini.find("seed") == std::string::npos, "the upper INI has the write, "
        "not the seed (%s)", ini.c_str());
  for (const char* p : {"\\upper\\DATA.TXT", "\\upper\\MINE.INI", "\\lower\\DATA.TXT", "\\lower\\MINE.INI"})
    DeleteFileA((root + p).c_str());
  for (const char* p : {"\\upper\\NEWDIR", "\\upper", "\\lower", "\\win"}) RemoveDirectoryA((root + p).c_str());
  RemoveDirectoryA(root.c_str());
}

// ---- Star Wars Screen Entertainment's Win16 surface (README "Intermission modules") -------------------------

// GetTempFileName with uUnique 0 makes the file (STRESS.DLL counts file
// handles by opening it until the handles run out; every Star Wars module
// wants 10); SetHandleCount reports the task's table size.
void test_temp_files() {
  Machine m;
  uint16_t ds = m.data(512);
  uint32_t buf = uint32_t(ds) << 16, pfx = m.rt.static_bytes("t pfx", "str");
  DosFiles& d = m.rt.state<DosFiles>();
  uint16_t n1 = uint16_t(api(m, "KERNEL", "GetTempFileName", {w16(0), l16(pfx), w16(0), l16(buf)}));
  std::string p1 = m.rt.read_str(buf);
  CHECK(n1 == 0x1234 && p1 == "C:\\WINDOWS\\TEMP\\~str1234.TMP", "uUnique 0: %04X %s", n1, p1.c_str());
  win32::Vfs::Stat st;
  CHECK(m.rt.vfs().stat(p1, &st) && !st.dir && st.size == 0, "the file exists, empty");
  uint16_t n2 = uint16_t(api(m, "KERNEL", "GetTempFileName", {w16(0), l16(pfx), w16(0), l16(buf + 0x100)}));
  CHECK(n2 == 0x1235 && m.rt.read_str(buf + 0x100) == "C:\\WINDOWS\\TEMP\\~str1235.TMP", "the next number is free (%04X)", n2);
  // STRESS's GETFREEFILEHANDLES: _lopen until it fails, close them all, OF_DELETE.
  std::vector<uint16_t> handles;
  for (int i = 0; i < 300; i++) {
    uint16_t h = uint16_t(api(m, "KERNEL", "_lopen", {l16(buf), w16(0x40)}));
    if (h == 0xFFFF) break;
    handles.push_back(h);
  }
  CHECK(handles.size() == 250, "250 handles on the temp file (%zu): at least 10, below STRESS's 256", handles.size());
  for (uint16_t h : handles) api(m, "KERNEL", "_lclose", {w16(h)});
  uint16_t of = m.data(160);
  CHECK((api(m, "KERNEL", "OpenFile", {l16(buf), l16(uint32_t(of) << 16), w16(0x0200)}) & 0xFFFF) == 1 && !d.exists(p1),
        "OF_DELETE removes it");
  // A nonzero uUnique names a file and makes none.
  CHECK((api(m, "KERNEL", "GetTempFileName", {w16(0), l16(pfx), w16(0x42), l16(buf)}) & 0xFFFF) == 0x42 &&
            m.rt.read_str(buf) == "C:\\WINDOWS\\TEMP\\~str0042.TMP" && !d.exists(m.rt.read_str(buf)),
        "uUnique 42h: %s, no file", m.rt.read_str(buf).c_str());
  // TF_FORCEDRIVE: the current directory of that drive (the root while it is
  // the current directory), TEMP or no TEMP.
  api(m, "KERNEL", "GetTempFileName", {w16(0x80 | 'C'), l16(pfx), w16(0x10), l16(buf)});
  CHECK(m.rt.read_str(buf) == "C:\\~str0010.TMP", "TF_FORCEDRIVE, C:\\ current: %s", m.rt.read_str(buf).c_str());
  // The module folder current, as the lanes make it (mount_imx_disk: C:\SAVER
  // over a writable upper layer): the file is made there, and opens.
  m.rt.vfs().mount_overlay("C:\\SAVER", "", "");
  CHECK(m.rt.vfs().set_cwd("C:\\SAVER"), "C:\\SAVER current");
  uint16_t nf = uint16_t(api(m, "KERNEL", "GetTempFileName", {w16(0x80 | 'c'), l16(pfx), w16(0), l16(buf)}));
  std::string pf = m.rt.read_str(buf);
  CHECK(nf == 0x1234 && pf == "C:\\SAVER\\~str1234.TMP" && d.exists(pf), "TF_FORCEDRIVE, uUnique 0: %04X %s, made",
        nf, pf.c_str());
  uint16_t hf = uint16_t(api(m, "KERNEL", "_lopen", {l16(buf), w16(2)}));
  CHECK(hf != 0xFFFF, "and the module's _lopen of it succeeds");
  api(m, "KERNEL", "_lclose", {w16(hf)});
  CHECK((api(m, "KERNEL", "GetTempFileName", {w16(0x80), l16(pfx), w16(0), l16(buf)}) & 0xFFFF) == 0x1235 &&
            m.rt.read_str(buf) == "C:\\SAVER\\~str1235.TMP",
        "TF_FORCEDRIVE with drive 0: the current drive's (%s)", m.rt.read_str(buf).c_str());
  // A drive the guest's disk does not have: TEMP (Wine's GetTempFileName16 too).
  api(m, "KERNEL", "GetTempFileName", {w16(0x80 | 'D'), l16(pfx), w16(0x11), l16(buf)});
  CHECK(m.rt.read_str(buf) == "C:\\WINDOWS\\TEMP\\~str0011.TMP", "TF_FORCEDRIVE, no D: drive: %s",
        m.rt.read_str(buf).c_str());
  m.rt.vfs().set_cwd("C:\\");
  // SetHandleCount: Windows' 20, grown to at most 255, never shrunk.
  CHECK((api(m, "KERNEL", "SetHandleCount", {w16(10)}) & 0xFFFF) == 20, "SetHandleCount(10): 20");
  CHECK((api(m, "KERNEL", "SetHandleCount", {w16(300)}) & 0xFFFF) == 255, "SetHandleCount(300): 255");
  CHECK((api(m, "KERNEL", "SetHandleCount", {w16(30)}) & 0xFFFF) == 255, "SetHandleCount(30): still 255");
  // _hwrite: a huge write, as _hread reads.
  uint32_t text = m.rt.static_bytes("t hw", "abcdef");
  uint16_t h = uint16_t(api(m, "KERNEL", "_lcreat", {l16(m.rt.static_bytes("t hwf", "C:\\WINDOWS\\TEMP\\HW.DAT")), w16(0)}));
  CHECK(api(m, "KERNEL", "_hwrite", {w16(h), l16(text), l16(6)}) == 6, "_hwrite 6 bytes");
  api(m, "KERNEL", "_lclose", {w16(h)});
  CHECK(m.rt.vfs().stat("C:\\WINDOWS\\TEMP\\HW.DAT", &st) && st.size == 6, "written");
}

// FindResource → AccessResource (a DOS handle at the resource's data in the
// module file) and LoadResource/FreeResource's usage count: a resource freed
// to zero uses is read afresh (POSTERS edits its locked caption every frame).
void test_resources() {
  Machine m;
  char dir[MAX_PATH];
  GetTempPathA(MAX_PATH, dir);
  std::string base = std::string(dir) + "adw_win16_res_" + std::to_string(GetCurrentProcessId());
  CreateDirectoryA(base.c_str(), nullptr);
  std::string file = base + "\\RESTEST.DLL";
  {
    std::string img = build_ne(true);
    FILE* fh = fopen(file.c_str(), "wb");
    fwrite(img.data(), 1, img.size(), fh);
    fclose(fh);
  }
  m.rt.vfs().mount("C:\\AFTERDRK", base, false);
  uint16_t err = 0;
  Module16* mod = m.rt.modules().load("C:\\AFTERDRK\\RESTEST.DLL", &err);
  CHECK(mod != nullptr, "the NE DLL with a resource loads (error %u)", err);
  if (mod) {
    uint16_t hi = mod->hinstance;
    uint16_t hr = uint16_t(api(m, "KERNEL", "FindResource", {w16(hi), l16(7), l16(10)}));
    CHECK(hr != 0, "FindResource(RT_RCDATA 7)");
    CHECK(api(m, "KERNEL", "SizeofResource", {w16(hi), w16(hr)}) == 16, "SizeofResource: 16");
    uint16_t ds = m.data(128);
    uint32_t buf = uint32_t(ds) << 16;
    uint16_t hf = uint16_t(api(m, "KERNEL", "AccessResource", {w16(hi), w16(hr)}));
    CHECK(hf != 0xFFFF && hf >= 5, "AccessResource: a DOS file handle (%04X)", hf);
    uint32_t got = api(m, "KERNEL", "_hread", {w16(hf), l16(buf), l16(16)});
    CHECK(got == 16 && m.rt.read_str(buf) == kTestResource, "_hread reads the resource (%u, \"%s\")", got,
          m.rt.read_str(buf).c_str());
    CHECK((api(m, "KERNEL", "_lclose", {w16(hf)}) & 0xFFFF) == 0, "_lclose");
    CHECK((api(m, "KERNEL", "AccessResource", {w16(hi), w16(0x7777)}) & 0xFFFF) == 0xFFFF, "an unknown HRSRC: HFILE_ERROR");
    // One copy while it is used; the count goes down with FreeResource.
    uint16_t h1 = uint16_t(api(m, "KERNEL", "LoadResource", {w16(hi), w16(hr)}));
    uint16_t h2 = uint16_t(api(m, "KERNEL", "LoadResource", {w16(hi), w16(hr)}));
    CHECK(h1 && h1 == h2, "a loaded resource comes back as the same block (%04X %04X)", h1, h2);
    uint32_t p = api(m, "KERNEL", "LockResource", {w16(h1)});
    m.rt.wr8(p, 'J');  // POSTERS appends to its caption in place
    api(m, "KERNEL", "GlobalUnlock", {w16(h1)});
    CHECK((api(m, "KERNEL", "FreeResource", {w16(h1)}) & 0xFFFF) == 0 && m.rt.global().find(h1) &&
              m.rt.read_str(p) == "Jello, resource",
          "FreeResource with a use left: still loaded, the edit still there");
    CHECK((api(m, "KERNEL", "FreeResource", {w16(h2)}) & 0xFFFF) == 0 && !m.rt.global().find(h1),
          "the last FreeResource frees the block");
    uint16_t h3 = uint16_t(api(m, "KERNEL", "LoadResource", {w16(hi), w16(hr)}));
    uint32_t p3 = api(m, "KERNEL", "LockResource", {w16(h3)});
    CHECK(h3 && p3 && m.rt.read_str(p3) == kTestResource, "the next LoadResource reads it afresh (\"%s\")",
          p3 ? m.rt.read_str(p3).c_str() : "");
    api(m, "KERNEL", "GlobalUnlock", {w16(h3)});
    api(m, "KERNEL", "FreeResource", {w16(h3)});
    // A block that is no resource: FreeResource frees it (USER's DestroyIcon32 did).
    uint16_t g = uint16_t(api(m, "KERNEL", "GlobalAlloc", {w16(GlobalHeap16::kMoveable), l16(32)}));
    CHECK((api(m, "KERNEL", "FreeResource", {w16(g)}) & 0xFFFF) == 0 && !m.rt.global().find(g), "FreeResource of a "
          "plain block frees it");
    m.rt.modules().free_all();
  }
  DeleteFileA(file.c_str());
  RemoveDirectoryA(base.c_str());
}

// A packed 8-bit DIB in a fresh global block: header, a 1024-byte colour
// table (SWSE's identity WORD table unless rgb_table), then the bits (0x428).
uint32_t packed_dib(Machine& m, int w, int h, bool rgb_table = false) {
  uint32_t stride = uint32_t((w + 3) & ~3);
  uint16_t hb = m.rt.global().alloc(GlobalHeap16::kMoveable | GlobalHeap16::kZeroInit, 0x428 + stride * uint32_t(std::abs(h)));
  uint32_t p = m.rt.global().lock(hb);
  BITMAPINFOHEADER bi{sizeof(BITMAPINFOHEADER), w, h, 1, 8, BI_RGB, stride * uint32_t(std::abs(h)), 0, 0, 0, 0};
  m.rt.write_bytes(p, &bi, sizeof(bi));
  for (uint32_t i = 0; i < 256; i++) {
    if (rgb_table) m.rt.wr32(p + 40 + 4 * i, 0);  // black everywhere …
    else m.rt.wr16(p + 40 + 2 * i, uint16_t(i));
  }
  if (rgb_table) m.rt.wr32(p + 40 + 4 * 42, 0x0000FF00);  // … but entry 42: green (B, G, R, 0)
  return p;
}

// The DIB driver (gdi16.hh): CreateDC("DIB", NULL, NULL, lpPackedDIB) draws
// into the packed DIB's own bits, as DIB.DRV did, with its colour matching.
void test_dib_driver() {
  Machine m;
  Screen screen(64, 48);
  m.rt.attach_display(screen);
  Gdi16& g = m.rt.state<Gdi16>();
  uint16_t saver = user16_saver_window(m.rt);
  uint16_t sdc = gdi16_screen_dc(m.rt, saver);
  const int W = 20, H = 10;
  const uint32_t stride = 20;
  uint32_t p = packed_dib(m, W, H);
  uint32_t drv = m.rt.static_bytes("t DIB", "DIB");
  uint16_t dc = uint16_t(api(m, "GDI", "CreateDC", {l16(drv), l16(0), l16(0), l16(p)}));
  CHECK(dc != 0 && g.dc(dc) && g.dc(dc)->dib_device, "CreateDC(\"DIB\") over the packed DIB (%04X)", dc);
  if (!dc) return;
  // Bottom-up: row y is at (H-1-y) in memory.
  auto px = [&](int x, int y) { return m.rt.rd8(Runtime16::huge_add(p, 0x428 + uint32_t(H - 1 - y) * stride + uint32_t(x))); };
  uint16_t rs = m.data(64);
  uint32_t rc = uint32_t(rs) << 16;
  auto fill = [&](int16_t l, int16_t t, int16_t r, int16_t b, uint32_t color) {
    write16(m.rt, rc, RECT16{l, t, r, b});
    uint16_t br = uint16_t(api(m, "GDI", "CreateSolidBrush", {l16(color)}));
    api(m, "USER", "FillRect", {w16(dc), l16(rc), w16(br)});
    api(m, "GDI", "DeleteObject", {w16(br)});
  };
  fill(0, 0, 4, 4, 0x10FF0037);   // DIBINDEX(37h)
  fill(4, 0, 8, 4, 0x0100005A);   // PALETTEINDEX(5Ah)
  fill(8, 0, 12, 4, RGB(255, 255, 255));
  fill(12, 0, 16, 4, RGB(0x80, 0, 0));
  fill(16, 0, 20, 4, 0x0200C0C0 | (0xC0 << 16));  // PALETTERGB(C0, C0, C0)
  CHECK(px(1, 1) == 0x37 && px(5, 1) == 0x5A, "DIBINDEX and PALETTEINDEX are the pixel value itself (%02X %02X)", px(1, 1),
        px(5, 1));
  CHECK(px(9, 1) == 15 && px(13, 1) == 1 && px(17, 1) == 7,
        "RGB and PALETTERGB on an index table: the 16 VGA colours (white %u, dark red %u, light grey %u)", px(9, 1),
        px(13, 1), px(17, 1));
  CHECK(m.rt.rd8(p + 0x428 + uint32_t(H - 1) * stride + 1) == 0x37, "row 0 is the last in memory (bottom-up)");
  // The guest writes pixels itself; a blit reads them, indices unchanged.
  m.rt.wr8(Runtime16::huge_add(p, 0x428 + uint32_t(H - 1 - 5) * stride + 15), 0x77);
  api(m, "GDI", "BitBlt", {w16(sdc), w16(0), w16(0), w16(W), w16(H), w16(dc), w16(0), w16(0), l16(SRCCOPY)});
  GdiFlush();
  CHECK(screen.at(15, 5) == 0x77 && screen.at(1, 1) == 0x37, "a blit to the screen copies the indices (%02X %02X)",
        screen.at(15, 5), screen.at(1, 1));
  // Text and lines land in the bits too (a pen of DIBINDEX(9)).
  uint16_t pen = uint16_t(api(m, "GDI", "CreatePen", {w16(PS_SOLID), w16(1), l16(0x10FF0009)}));
  uint16_t old_pen = uint16_t(api(m, "GDI", "SelectObject", {w16(dc), w16(pen)}));
  api(m, "GDI", "MoveTo", {w16(dc), w16(0), w16(8)});
  api(m, "GDI", "LineTo", {w16(dc), w16(W), w16(8)});
  CHECK(px(3, 8) == 9 && px(18, 8) == 9, "LineTo with a DIBINDEX pen (%u)", px(3, 8));
  api(m, "GDI", "SelectObject", {w16(dc), w16(old_pen)});
  // Not a palette device: the driver's own caps; RealizePalette maps nothing.
  CHECK(!(api(m, "GDI", "GetDeviceCaps", {w16(dc), w16(RASTERCAPS)}) & RC_PALETTE) &&
            (api(m, "GDI", "GetDeviceCaps", {w16(dc), w16(HORZRES)}) & 0xFFFF) == W &&
            (api(m, "GDI", "GetDeviceCaps", {w16(dc), w16(NUMCOLORS)}) & 0xFFFF) == 256,
        "GetDeviceCaps: the DIB driver's");
  uint16_t ls = m.data(16);
  m.rt.wr16(uint32_t(ls) << 16, 0x300);
  m.rt.wr16((uint32_t(ls) << 16) + 2, 1);
  m.rt.wr32((uint32_t(ls) << 16) + 4, 0x000000FF);
  uint16_t pal = uint16_t(api(m, "GDI", "CreatePalette", {l16(uint32_t(ls) << 16)}));
  api(m, "USER", "SelectPalette", {w16(dc), w16(pal), w16(0)});
  CHECK((api(m, "USER", "RealizePalette", {w16(dc)}) & 0xFFFF) == 0, "RealizePalette on a DIB DC: nothing");
  fill(0, 4, 4, 8, 0x01000003);
  CHECK(px(1, 5) == 3, "PALETTEINDEX ignores the selected palette (%u)", px(1, 5));
  // A device DC: no bitmap selects into it; GetPixel reads the table's colour.
  uint16_t bmp = uint16_t(api(m, "GDI", "CreateCompatibleBitmap", {w16(dc), w16(4), w16(4)}));
  CHECK((api(m, "GDI", "SelectObject", {w16(dc), w16(bmp)}) & 0xFFFF) == 0, "no bitmap selects into a DIB DC");
  CHECK(api(m, "GDI", "GetPixel", {w16(dc), w16(9), w16(1)}) == RGB(255, 255, 255), "GetPixel: VGA white for 15");
  // DeleteDC leaves the DIB as it was.
  CHECK((api(m, "GDI", "DeleteDC", {w16(dc)}) & 0xFFFF) == 1 && px(1, 1) == 0x37 && m.rt.global().find(uint16_t(p >> 16)),
        "DeleteDC: the bits and their block stay");
  // An RGB colour table (SWSE's SETDIBUSAGEBI usage 0): the nearest entry.
  uint32_t q = packed_dib(m, 8, 4, true);
  uint16_t dc2 = uint16_t(api(m, "GDI", "CreateDC", {l16(drv), l16(0), l16(0), l16(q)}));
  uint16_t green = uint16_t(api(m, "GDI", "CreateSolidBrush", {l16(RGB(0, 250, 10))}));
  write16(m.rt, rc, RECT16{0, 0, 8, 4});
  api(m, "USER", "FillRect", {w16(dc2), l16(rc), w16(green)});
  CHECK(m.rt.rd8(q + 0x428) == 42, "an RGB table: the nearest entry (%u)", m.rt.rd8(q + 0x428));
  api(m, "GDI", "DeleteDC", {w16(dc2)});
  // Top-down (negative biHeight): row 0 first in memory.
  uint32_t t = packed_dib(m, 8, -4);
  uint16_t dc3 = uint16_t(api(m, "GDI", "CreateDC", {l16(drv), l16(0), l16(0), l16(t)}));
  CHECK(dc3 != 0, "a top-down DIB");
  uint16_t b9 = uint16_t(api(m, "GDI", "CreateSolidBrush", {l16(0x10FF0009)}));
  write16(m.rt, rc, RECT16{0, 0, 1, 1});
  api(m, "USER", "FillRect", {w16(dc3), l16(rc), w16(b9)});
  CHECK(m.rt.rd8(t + 0x428) == 9 && m.rt.rd8(t + 0x428 + 3 * 8) == 0, "top-down: row 0 first");
  api(m, "GDI", "DeleteDC", {w16(dc3)});
  // What the driver refuses (and 4-bit DIBs, which it drew but this does not): 0.
  uint32_t bad = packed_dib(m, 8, 4);
  m.rt.wr16(bad + 14, 4);
  CHECK((api(m, "GDI", "CreateDC", {l16(drv), l16(0), l16(0), l16(bad)}) & 0xFFFF) == 0, "a 4-bit DIB: 0");
  m.rt.wr16(bad + 14, 8);
  m.rt.wr32(bad + 16, BI_RLE8);
  CHECK((api(m, "GDI", "CreateDC", {l16(drv), l16(0), l16(0), l16(bad)}) & 0xFFFF) == 0, "a compressed DIB: 0");
  CHECK((api(m, "GDI", "CreateDC", {l16(drv), l16(0), l16(0), l16(0)}) & 0xFFFF) == 0, "no DIB: 0");
  CHECK((api(m, "GDI", "CreateDC", {l16(m.rt.static_bytes("t WINGDIB", "WINGDIB")), l16(0), l16(0), l16(p)}) & 0xFFFF) == 0,
        "another driver: 0");
}

// DIB bits drawn onto a DC with DIB colour semantics (gdi16.cc dib_xlate,
// dib_to_indices; DIB.DRV's translation between two colour tables, 1:0653):
// an index table keeps the source's indices; an RGB table takes each source
// colour's nearest entry — unless the source's DIB_PAL_COLORS table is the
// identity, which keeps them there too — and a DIB_PAL_COLORS source's colours
// are its entries in the DC's palette; deep pixels are matched as colours are
// (dib_index). A memory DC made compatible with a DIB DC has its colour
// semantics; GetNearestColor answers the table's colours.
void test_dib_translation() {
  Machine m;
  Screen screen(64, 48);
  m.rt.attach_display(screen);
  Gdi16& g = m.rt.state<Gdi16>();
  uint32_t drv = m.rt.static_bytes("t DIB", "DIB");
  // Two 8x4 DIB DCs: over an index table, and over an RGB table (black, but entry 42 green).
  uint32_t ip = packed_dib(m, 8, 4), rp = packed_dib(m, 8, 4, true);
  uint16_t idc = uint16_t(api(m, "GDI", "CreateDC", {l16(drv), l16(0), l16(0), l16(ip)}));
  uint16_t rdc = uint16_t(api(m, "GDI", "CreateDC", {l16(drv), l16(0), l16(0), l16(rp)}));
  CHECK(idc && rdc, "two DIB DCs (%04X %04X)", idc, rdc);
  if (!idc || !rdc) return;
  // The destinations' pixels; memory row 0 is their bottom row, where an 8x4
  // source's row 0 lands (all bottom-up).
  auto px = [&](uint32_t p, uint32_t i) { return int(m.rt.rd8(p + 0x428 + i)); };
  // The source: 8x4, 8 bits, pixel values 0..31; an RGB table of bright red
  // but entry 5 (blue) and entry 7 (a green, not quite pure).
  uint16_t sh = m.rt.global().alloc(GlobalHeap16::kMoveable | GlobalHeap16::kZeroInit, 40 + 1024 + 32);
  uint32_t sp = m.rt.global().lock(sh), sbits = sp + 40 + 1024;
  BITMAPINFOHEADER bi{sizeof(BITMAPINFOHEADER), 8, 4, 1, 8, BI_RGB, 32, 0, 0, 0, 0};
  m.rt.write_bytes(sp, &bi, sizeof(bi));
  for (uint32_t i = 0; i < 256; i++) m.rt.wr32(sp + 40 + 4 * i, 0x00FF0000);  // (B, G, R, 0)
  m.rt.wr32(sp + 40 + 4 * 5, 0x000000FF);
  m.rt.wr32(sp + 40 + 4 * 7, 0x0000F000);
  for (uint32_t i = 0; i < 32; i++) m.rt.wr8(sbits + i, uint8_t(i));
  auto stretch = [&](uint16_t dc, uint16_t usage) {
    return int16_t(api(m, "GDI", "StretchDIBits", {w16(dc), w16(0), w16(0), w16(8), w16(4), w16(0), w16(0), w16(8),
                                                   w16(4), l16(sbits), l16(sp), w16(usage), l16(SRCCOPY)}));
  };
  auto kept = [&](uint32_t p) {
    int k = 0;
    for (uint32_t i = 0; i < 32; i++) k += px(p, i) == int(i);
    return k;
  };
  CHECK(stretch(idc, DIB_RGB_COLORS) == 4 && kept(ip) == 32, "an index-table destination keeps the indices (%d of 32)",
        kept(ip));
  CHECK(stretch(rdc, DIB_RGB_COLORS) == 4 && px(rp, 7) == 42 && px(rp, 5) == 0 && px(rp, 3) == 0,
        "an RGB-table destination: each colour's nearest entry (green-ish %d, blue %d, red %d)", px(rp, 7), px(rp, 5),
        px(rp, 3));
  // DIB_PAL_COLORS: an identity table keeps the indices on the RGB table too …
  for (uint32_t i = 0; i < 256; i++) m.rt.wr16(sp + 40 + 2 * i, uint16_t(i));
  CHECK(stretch(rdc, DIB_PAL_COLORS) == 4 && kept(rp) == 32, "an identity DIB_PAL_COLORS source keeps its indices (%d)",
        kept(rp));
  // … any other goes through the DC's palette (DEFAULT_PALETTE: 14 is green,
  // 19 white — nearer the green entry than black — and none past 19).
  m.rt.wr16(sp + 40 + 2 * 3, 14);
  stretch(rdc, DIB_PAL_COLORS);
  CHECK(px(rp, 3) == 42 && px(rp, 0) == 0 && px(rp, 19) == 42 && px(rp, 20) == 0,
        "a DIB_PAL_COLORS source through the DC's palette (%d %d %d %d)", px(rp, 3), px(rp, 0), px(rp, 19), px(rp, 20));
  // 24-bit pixels (SetDIBitsToDevice, one row at the top): matched as colours
  // are — on the index table, the VGA colours white 15, dark red 1, green 10, blue 12.
  const uint8_t deep[12] = {0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x80, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0x00};  // B, G, R
  uint32_t dp = uint32_t(m.data(64)) << 16;
  BITMAPINFOHEADER di{sizeof(BITMAPINFOHEADER), 4, 1, 1, 24, BI_RGB, 12, 0, 0, 0, 0};
  m.rt.write_bytes(dp, &di, sizeof(di));
  m.rt.write_bytes(dp + 40, deep, sizeof(deep));
  CHECK((api(m, "GDI", "SetDIBitsToDevice", {w16(idc), w16(0), w16(0), w16(4), w16(1), w16(0), w16(0), w16(0), w16(1),
                                             l16(dp + 40), l16(dp), w16(DIB_RGB_COLORS)}) &
         0xFFFF) == 1,
        "SetDIBitsToDevice of a 24-bit row");
  const uint32_t top = 3 * 8;  // y 0: the last row in memory
  CHECK(px(ip, top) == 15 && px(ip, top + 1) == 1 && px(ip, top + 2) == 10 && px(ip, top + 3) == 12,
        "24-bit pixels on an index table (%d %d %d %d)", px(ip, top), px(ip, top + 1), px(ip, top + 2), px(ip, top + 3));
  // A memory DC made compatible with the DIB DC: RGB white is 15 there too,
  // and DIB bits keep their indices.
  uint16_t mdc = uint16_t(api(m, "GDI", "CreateCompatibleDC", {w16(idc)}));
  uint16_t bmp = uint16_t(api(m, "GDI", "CreateCompatibleBitmap", {w16(idc), w16(8), w16(4)}));
  api(m, "GDI", "SelectObject", {w16(mdc), w16(bmp)});
  CHECK(g.dc(mdc) && g.dc(mdc)->dib_header == ip, "the memory DC has the DIB's colour semantics");
  uint32_t rc = uint32_t(m.data(16)) << 16;
  write16(m.rt, rc, RECT16{0, 0, 8, 4});
  uint16_t white = uint16_t(api(m, "GDI", "CreateSolidBrush", {l16(RGB(255, 255, 255))}));
  api(m, "USER", "FillRect", {w16(mdc), l16(rc), w16(white)});
  GdiFlush();
  Obj16* bo = g.get(bmp, G16::bitmap);
  CHECK(bo && bo->bmp.bits && bo->bmp.bits[0] == 15, "RGB white is 15 on it (%d)", bo && bo->bmp.bits ? bo->bmp.bits[0] : -1);
  for (uint32_t i = 0; i < 256; i++) m.rt.wr32(sp + 40 + 4 * i, 0x00FF0000);
  stretch(mdc, DIB_RGB_COLORS);
  GdiFlush();
  std::vector<int> mem_px;
  for (int y = 0; bo && bo->bmp.bits && y < 4; y++) {
    for (int x = 0; x < 8; x++) mem_px.push_back(bo->bmp.bits[size_t(y) * bo->bmp.stride + size_t(x)]);
  }
  std::sort(mem_px.begin(), mem_px.end());
  bool all_kept = mem_px.size() == 32;
  for (size_t i = 0; i < mem_px.size() && all_kept; i++) all_kept = mem_px[i] == int(i);
  CHECK(all_kept, "DIB bits onto it keep their indices, 0..31 (%zu pixels)", mem_px.size());
  // GetNearestColor: the table's colours (the VGA colours for an index table).
  CHECK(api(m, "GDI", "GetNearestColor", {w16(idc), l16(RGB(250, 250, 250))}) == RGB(255, 255, 255) &&
            api(m, "GDI", "GetNearestColor", {w16(rdc), l16(RGB(10, 200, 10))}) == RGB(0, 255, 0) &&
            api(m, "GDI", "GetNearestColor", {w16(rdc), l16(RGB(200, 0, 0))}) == RGB(0, 0, 0),
        "GetNearestColor on DIB DCs");
  api(m, "GDI", "DeleteDC", {w16(mdc)});
  api(m, "GDI", "DeleteDC", {w16(idc)});
  api(m, "GDI", "DeleteDC", {w16(rdc)});
}

// DIB bits into a monochrome bitmap: real GDI gets the DIB's own colours
// (gdi16.cc), so white is 1 and black 0 whatever palette the DC holds —
// StretchDIBits and SetDIBitsToDevice on a memory DC holding one, as SetDIBits
// always did. Star Trek's AD_MOD.DLL makes Scotty's Files' blueprint masks
// so: 1-bpp DIBs, white on black, into monochrome bitmaps of a memory DC whose
// palette holds its white at a slot of its own (PC_RESERVED); as a key colour
// that white was a dark grey, black in monochrome, and every mask came out
// empty.
void test_mono_dib_targets() {
  Machine m;
  Screen screen(64, 48);
  m.rt.attach_display(screen);
  Gdi16& g = m.rt.state<Gdi16>();
  uint16_t hwnd = user16_saver_window(m.rt);
  uint16_t sdc = gdi16_screen_dc(m.rt, hwnd);
  // A palette of black and a reserved white, realized: the white gets slot 10.
  uint32_t lp = uint32_t(m.data(64)) << 16;
  m.rt.wr16(lp, 0x300);
  m.rt.wr16(lp + 2, 2);
  m.rt.wr32(lp + 4, 0x00000000);
  m.rt.wr32(lp + 8, 0x01FFFFFF);  // R, G, B = FF, flags PC_RESERVED
  uint16_t pal = uint16_t(api(m, "GDI", "CreatePalette", {l16(lp)}));
  api(m, "USER", "SelectPalette", {w16(sdc), w16(pal), w16(0)});
  api(m, "USER", "RealizePalette", {w16(sdc)});
  Obj16* po = g.get(pal, G16::palette);
  CHECK(po && po->pal->map.size() == 2 && po->pal->map[1] >= 10, "the reserved white has a slot of its own (%d)",
        po && po->pal->map.size() == 2 ? po->pal->map[1] : -1);
  uint16_t mdc = uint16_t(api(m, "GDI", "CreateCompatibleDC", {w16(sdc)}));
  api(m, "USER", "SelectPalette", {w16(mdc), w16(pal), w16(0)});
  uint16_t mono = uint16_t(api(m, "GDI", "CreateBitmap", {w16(16), w16(2), w16(1), w16(1), l16(0)}));
  api(m, "GDI", "SelectObject", {w16(mdc), w16(mono)});
  // The DIB: 16x2, 1 bpp, colour 0 white and 1 black; each row black, white, white, black by fours.
  uint32_t dp = uint32_t(m.data(128)) << 16, bits = dp + 48;
  BITMAPINFOHEADER bi{sizeof(BITMAPINFOHEADER), 16, 2, 1, 1, BI_RGB, 8, 0, 0, 2, 0};
  m.rt.write_bytes(dp, &bi, sizeof(bi));
  m.rt.wr32(dp + 40, 0x00FFFFFF);
  m.rt.wr32(dp + 44, 0x00000000);
  for (uint32_t row = 0; row < 2; row++) {
    m.rt.wr8(bits + 4 * row, 0xF0);
    m.rt.wr8(bits + 4 * row + 1, 0x0F);
  }
  auto rows = [&]() {
    GdiFlush();
    uint8_t b[4] = {};
    Obj16* bo = g.get(mono, G16::bitmap);
    if (bo && bo->host) GetBitmapBits(static_cast<HBITMAP>(bo->host), 4, b);
    char s[16];
    snprintf(s, sizeof(s), "%02X%02X %02X%02X", b[0], b[1], b[2], b[3]);
    return std::string(s);
  };
  // White (1) where the DIB is white: 0000 1111 1111 0000 on both rows.
  api(m, "GDI", "PatBlt", {w16(mdc), w16(0), w16(0), w16(16), w16(2), l16(BLACKNESS)});
  CHECK(int16_t(api(m, "GDI", "StretchDIBits", {w16(mdc), w16(0), w16(0), w16(16), w16(2), w16(0), w16(0), w16(16),
                                                w16(2), l16(bits), l16(dp), w16(DIB_RGB_COLORS), l16(SRCCOPY)})) == 2 &&
            rows() == "0FF0 0FF0",
        "StretchDIBits into a monochrome bitmap: white is 1 (%s)", rows().c_str());
  api(m, "GDI", "PatBlt", {w16(mdc), w16(0), w16(0), w16(16), w16(2), l16(BLACKNESS)});
  CHECK((api(m, "GDI", "SetDIBitsToDevice", {w16(mdc), w16(0), w16(0), w16(16), w16(2), w16(0), w16(0), w16(0), w16(2),
                                             l16(bits), l16(dp), w16(DIB_RGB_COLORS)}) &
         0xFFFF) == 2 &&
            rows() == "0FF0 0FF0",
        "SetDIBitsToDevice into a monochrome bitmap: white is 1 (%s)", rows().c_str());
  // Source origins reach real GDI as given. A second DIB whose two rows differ
  // (bottom-up: first in memory the bottom row, white 0-3 and black 4-15;
  // then the top row, black 0-7, white 8-11, black 12-15): its top row's
  // x 8..15 — XSrc 8 and YSrc 1 counted from the bottom — onto x 4..11 of the
  // bitmap's row 1 by StretchDIBits, and onto row 0 by SetDIBitsToDevice from
  // a band holding the top row alone (start scan 1, one line). A swapped or
  // dropped origin or start scan draws nothing or the wrong row.
  uint32_t bits2 = dp + 56;
  m.rt.wr8(bits2, 0x0F);
  m.rt.wr8(bits2 + 1, 0xFF);
  m.rt.wr8(bits2 + 4, 0xFF);
  m.rt.wr8(bits2 + 5, 0x0F);
  api(m, "GDI", "PatBlt", {w16(mdc), w16(0), w16(0), w16(16), w16(2), l16(BLACKNESS)});
  int16_t so = int16_t(api(m, "GDI", "StretchDIBits", {w16(mdc), w16(4), w16(1), w16(8), w16(1), w16(8), w16(1),
                                                       w16(8), w16(1), l16(bits2), l16(dp), w16(DIB_RGB_COLORS),
                                                       l16(SRCCOPY)}));
  CHECK(rows() == "0000 0F00", "StretchDIBits into a monochrome bitmap from source (8, 1): %s (%d)", rows().c_str(),
        so);
  api(m, "GDI", "PatBlt", {w16(mdc), w16(0), w16(0), w16(16), w16(2), l16(BLACKNESS)});
  uint32_t sd = api(m, "GDI", "SetDIBitsToDevice", {w16(mdc), w16(4), w16(0), w16(8), w16(1), w16(8), w16(1), w16(1),
                                                    w16(1), l16(bits2 + 4), l16(dp), w16(DIB_RGB_COLORS)}) &
                0xFFFF;
  CHECK(rows() == "0F00 0000", "SetDIBitsToDevice into a monochrome bitmap from (8, 1), band of scan 1: %s (%u)",
        rows().c_str(), sd);
  // SetDIBits into the bitmap itself, as before.
  api(m, "GDI", "PatBlt", {w16(mdc), w16(0), w16(0), w16(16), w16(2), l16(BLACKNESS)});
  CHECK((api(m, "GDI", "SetDIBits", {w16(mdc), w16(mono), w16(0), w16(2), l16(bits), l16(dp), w16(DIB_RGB_COLORS)}) &
         0xFFFF) == 2 &&
            rows() == "0FF0 0FF0",
        "SetDIBits: the same (%s)", rows().c_str());
  // The path's bounds: into a colour (8-bit) bitmap of a memory DC, both
  // calls keep the key table — the DIB's colours become hardware indices as
  // ever (colour 0 red: the static red; with the monochrome path's real
  // colours on the key table it would be some grey level's index instead).
  uint16_t cdc = uint16_t(api(m, "GDI", "CreateCompatibleDC", {w16(sdc)}));
  uint16_t col = uint16_t(api(m, "GDI", "CreateCompatibleBitmap", {w16(sdc), w16(16), w16(2)}));
  api(m, "GDI", "SelectObject", {w16(cdc), w16(col)});
  m.rt.wr32(dp + 40, 0x00FF0000);
  auto at4 = [&]() {
    GdiFlush();
    Obj16* co = g.get(col, G16::bitmap);
    return co && co->bmp.bpp == 8 && co->bmp.bits ? g.index_rgb(co->bmp.bits[4]) : COLORREF(0xFFFFFFFF);
  };
  api(m, "GDI", "PatBlt", {w16(cdc), w16(0), w16(0), w16(16), w16(2), l16(BLACKNESS)});
  api(m, "GDI", "StretchDIBits", {w16(cdc), w16(0), w16(0), w16(16), w16(2), w16(0), w16(0), w16(16), w16(2), l16(bits),
                                  l16(dp), w16(DIB_RGB_COLORS), l16(SRCCOPY)});
  CHECK(at4() == RGB(255, 0, 0), "StretchDIBits into a colour bitmap: the key table, red stays red (%06lX)",
        (unsigned long)at4());
  api(m, "GDI", "PatBlt", {w16(cdc), w16(0), w16(0), w16(16), w16(2), l16(BLACKNESS)});
  api(m, "GDI", "SetDIBitsToDevice", {w16(cdc), w16(0), w16(0), w16(16), w16(2), w16(0), w16(0), w16(0), w16(2),
                                      l16(bits), l16(dp), w16(DIB_RGB_COLORS)});
  CHECK(at4() == RGB(255, 0, 0), "SetDIBitsToDevice into a colour bitmap: the key table, red stays red (%06lX)",
        (unsigned long)at4());
  // Which of a DIB's colours become 1: the rule of real GDI, which the path
  // hands the DIB to (its colour table; a DIB_PAL_COLORS table as the DC's
  // palette maps it). Only the table's entry nearest white is 1 — the first
  // of equal ones — and every other entry is 0, however light: beside a
  // white, yellow, light grey and FFFBF0 are 0 (by nearness each would be 1);
  // without one, light grey, the nearest, is 1. An 8-bit DIB 16x2 whose
  // pixel i is colour i; the monochrome bitmap's first row, a character per
  // colour, through each of the three calls.
  uint32_t dp8 = uint32_t(m.data(1024)) << 16;
  auto mono_of = [&](const std::vector<uint32_t>& table, uint16_t usage, int call) {
    BITMAPINFOHEADER h8{sizeof(BITMAPINFOHEADER), 16, 2, 1, 8, BI_RGB, 32, 0, 0, DWORD(table.size()), 0};
    m.rt.write_bytes(dp8, &h8, sizeof(h8));
    uint32_t at = dp8 + sizeof(h8);
    for (uint32_t c : table) {
      if (usage == DIB_PAL_COLORS) {
        m.rt.wr16(at, uint16_t(c));
        at += 2;
      } else {
        m.rt.wr32(at, c);  // 0xRRGGBB, little-endian: the RGBQUAD blue, green, red, 0
        at += 4;
      }
    }
    uint32_t px = dp8 + 0x200;
    for (uint32_t row = 0; row < 2; row++) {
      for (uint32_t i = 0; i < 16; i++) m.rt.wr8(px + 16 * row + i, uint8_t(i < table.size() ? i : 0));
    }
    api(m, "GDI", "PatBlt", {w16(mdc), w16(0), w16(0), w16(16), w16(2), l16(BLACKNESS)});
    if (call == 0) {
      api(m, "GDI", "StretchDIBits", {w16(mdc), w16(0), w16(0), w16(16), w16(2), w16(0), w16(0), w16(16), w16(2), l16(px),
                                      l16(dp8), w16(usage), l16(SRCCOPY)});
    } else if (call == 1) {
      api(m, "GDI", "SetDIBitsToDevice", {w16(mdc), w16(0), w16(0), w16(16), w16(2), w16(0), w16(0), w16(0), w16(2),
                                          l16(px), l16(dp8), w16(usage)});
    } else {
      api(m, "GDI", "SetDIBits", {w16(mdc), w16(mono), w16(0), w16(2), l16(px), l16(dp8), w16(usage)});
    }
    GdiFlush();
    uint8_t b[4] = {};
    Obj16* bo = g.get(mono, G16::bitmap);
    if (bo && bo->host) GetBitmapBits(static_cast<HBITMAP>(bo->host), 4, b);
    std::string s;
    for (size_t i = 0; i < table.size(); i++) s += ((b[i >> 3] >> (7 - (i & 7))) & 1) ? '1' : '0';
    return s;
  };
  const char* calls[] = {"StretchDIBits", "SetDIBitsToDevice", "SetDIBits"};
  for (int call = 0; call < 3; call++) {
    std::string mixed = mono_of({0x000000, 0xFFFF00, 0xC0C0C0, 0xFFFFFF, 0x808080, 0xFFFBF0, 0x00FFFF, 0x404040},
                                DIB_RGB_COLORS, call);
    CHECK(mixed == "00010000", "%s: black, yellow, light grey, WHITE, grey, FFFBF0, cyan, dark grey -> %s (white alone 1)",
          calls[call], mixed.c_str());
    std::string no_white = mono_of({0x808080, 0xC0C0C0, 0x000000, 0xFFFF00}, DIB_RGB_COLORS, call);
    CHECK(no_white == "0100", "%s: grey, LIGHT GREY, black, yellow -> %s (the nearest white alone 1)", calls[call],
          no_white.c_str());
    std::string two = mono_of({0x000000, 0xFFFFFF, 0x808080, 0xFFFFFF}, DIB_RGB_COLORS, call);
    CHECK(two == "0100", "%s: black, WHITE, grey, white -> %s (the first of two whites alone 1)", calls[call], two.c_str());
  }
  // DIB_PAL_COLORS: the entries as the DC's palette maps them. A palette of
  // white, yellow, light grey and black; the table 1, 2, 0, 3 (the palette's
  // indices themselves, as colours, would make the last entry the nearest white).
  m.rt.wr16(lp, 0x300);
  m.rt.wr16(lp + 2, 4);
  m.rt.wr32(lp + 4, 0x00FFFFFF);   // white
  m.rt.wr32(lp + 8, 0x0000FFFF);   // yellow: R, G, B = FF, FF, 00
  m.rt.wr32(lp + 12, 0x00C0C0C0);  // light grey
  m.rt.wr32(lp + 16, 0x00000000);  // black
  uint16_t pal4 = uint16_t(api(m, "GDI", "CreatePalette", {l16(lp)}));
  api(m, "USER", "SelectPalette", {w16(sdc), w16(pal4), w16(0)});
  api(m, "USER", "RealizePalette", {w16(sdc)});
  api(m, "USER", "SelectPalette", {w16(mdc), w16(pal4), w16(0)});
  for (int call = 0; call < 3; call++) {
    std::string pc = mono_of({1, 2, 0, 3}, DIB_PAL_COLORS, call);
    CHECK(pc == "0010", "%s, DIB_PAL_COLORS: yellow, light grey, WHITE, black through the palette -> %s", calls[call],
          pc.c_str());
  }
  api(m, "USER", "SelectPalette", {w16(mdc), w16(pal), w16(0)});
  api(m, "GDI", "DeleteDC", {w16(cdc)});
  api(m, "GDI", "DeleteObject", {w16(col)});
  api(m, "GDI", "DeleteDC", {w16(mdc)});
  api(m, "GDI", "DeleteObject", {w16(mono)});
}

// GetSystemPaletteUse, MulDiv, PaintRgn, CreateHatchBrush,
// CreateDIBPatternBrush (on the screen and on a DIB DC), GlobalWire/UnWire.
void test_gdi_additions() {
  Machine m;
  Screen screen(64, 48);
  m.rt.attach_display(screen);
  Gdi16& g = m.rt.state<Gdi16>();
  uint16_t saver = user16_saver_window(m.rt);
  uint16_t hdc = gdi16_screen_dc(m.rt, saver);
  CHECK((api(m, "GDI", "GetSystemPaletteUse", {w16(hdc)}) & 0xFFFF) == SYSPAL_STATIC, "SYSPAL_STATIC");
  api(m, "GDI", "SetSystemPaletteUse", {w16(hdc), w16(SYSPAL_NOSTATIC)});
  CHECK((api(m, "GDI", "GetSystemPaletteUse", {w16(hdc)}) & 0xFFFF) == SYSPAL_NOSTATIC, "after SetSystemPaletteUse");
  api(m, "GDI", "SetSystemPaletteUse", {w16(hdc), w16(SYSPAL_STATIC)});
  // MulDiv: rounded, halves away from zero; -32768 for 0 and overflow.
  struct {
    int16_t a, b, c, r;
  } md[] = {{10, 10, 3, 33},    {10, 20, 3, 67},       {-10, 20, 3, -67},     {10, -20, 3, -67},  {-10, -20, 3, 67},
            {5, 1, 2, 3},       {-5, 1, 2, -3},        {1, 1, -2, -1},        {300, 200, -7, -8571}, {7, 1, 0, -32768},
            {32767, 2, 1, -32768}, {-32768, 1, 1, -32768}, {32767, 1, 1, 32767}, {0, -5, 3, 0},      {250, 480, 640, 188}};
  for (const auto& t : md) {
    CHECK(gdi16_muldiv(t.a, t.b, t.c) == t.r, "MulDiv(%d, %d, %d) = %d, not %d", t.a, t.b, t.c, t.r, gdi16_muldiv(t.a, t.b, t.c));
  }
  CHECK(int16_t(api(m, "GDI", "MulDiv", {w16(uint16_t(-10)), w16(20), w16(3)})) == -67, "MulDiv through the thunk");
  // PaintRgn: the DC's brush.
  uint16_t rgn = uint16_t(api(m, "GDI", "CreateRectRgn", {w16(2), w16(2), w16(6), w16(6)}));
  uint16_t red = uint16_t(api(m, "GDI", "CreateSolidBrush", {l16(RGB(255, 0, 0))}));
  api(m, "GDI", "SelectObject", {w16(hdc), w16(red)});
  CHECK((api(m, "GDI", "PaintRgn", {w16(hdc), w16(rgn)}) & 0xFFFF) == 1, "PaintRgn");
  GdiFlush();
  const RGBQUAD& q = screen.palette()[screen.at(3, 3)];
  CHECK(q.rgbRed == 255 && q.rgbGreen == 0 && screen.at(7, 7) == 0, "the region is red, the rest untouched");
  // CreateHatchBrush: a hatch in the colour over the background.
  uint16_t hb = uint16_t(api(m, "GDI", "CreateHatchBrush", {w16(HS_CROSS), l16(RGB(0, 0, 255))}));
  Obj16* ho = g.get(hb, G16::brush);
  CHECK(ho && ho->style == BS_HATCHED && ho->hatch == HS_CROSS, "a hatched brush");
  api(m, "GDI", "SetBkColor", {w16(hdc), l16(RGB(255, 255, 255))});
  api(m, "GDI", "SelectObject", {w16(hdc), w16(hb)});
  api(m, "GDI", "PatBlt", {w16(hdc), w16(16), w16(16), w16(16), w16(16), l16(PATCOPY)});
  GdiFlush();
  int blue = 0, white = 0;
  for (int y = 16; y < 32; y++) {
    for (int x = 16; x < 32; x++) {
      const RGBQUAD& c = screen.palette()[screen.at(x, y)];
      blue += c.rgbBlue == 255 && c.rgbRed == 0;
      white += c.rgbBlue == 255 && c.rgbRed == 255;
    }
  }
  CHECK(blue > 16 && white > 16, "the cross hatch: %d blue, %d white", blue, white);
  // CreateDIBPatternBrush: an 8x8 checker of indices 21h and 0, entry 21h red.
  uint16_t hd = m.rt.global().alloc(GlobalHeap16::kMoveable | GlobalHeap16::kZeroInit, 40 + 1024 + 64);
  uint32_t dp = m.rt.global().lock(hd);
  BITMAPINFOHEADER bi{sizeof(BITMAPINFOHEADER), 8, 8, 1, 8, BI_RGB, 64, 0, 0, 0, 0};
  m.rt.write_bytes(dp, &bi, sizeof(bi));
  m.rt.wr32(dp + 40 + 4 * 0x21, 0x00FF0000);  // red (B, G, R, 0)
  for (uint32_t i = 0; i < 64; i++) m.rt.wr8(dp + 40 + 1024 + i, ((i / 8 + i % 8) & 1) ? 0x21 : 0);
  m.rt.global().unlock(hd);
  uint16_t pb = uint16_t(api(m, "GDI", "CreateDIBPatternBrush", {w16(hd), w16(DIB_RGB_COLORS)}));
  CHECK(pb && g.get(pb, G16::brush) && !g.get(pb, G16::brush)->dib_pattern.empty(), "CreateDIBPatternBrush keeps a copy");
  api(m, "KERNEL", "GlobalFree", {w16(hd)});
  api(m, "GDI", "SelectObject", {w16(hdc), w16(pb)});
  api(m, "GDI", "PatBlt", {w16(hdc), w16(32), w16(0), w16(8), w16(8), l16(PATCOPY)});
  GdiFlush();
  auto rgb = [&](int x, int y) {
    const RGBQUAD& c = screen.palette()[screen.at(x, y)];
    return RGB(c.rgbRed, c.rgbGreen, c.rgbBlue);
  };
  CHECK((rgb(32, 0) == RGB(0, 0, 0) || rgb(32, 0) == RGB(255, 0, 0)) && rgb(32, 0) != rgb(33, 0),
        "on the screen: the pattern's colours (%06lX %06lX)", rgb(32, 0), rgb(33, 0));
  // On a DIB DC (index table): the pattern's own indices.
  uint32_t p = packed_dib(m, 8, 8);
  uint16_t dc = uint16_t(api(m, "GDI", "CreateDC", {l16(m.rt.static_bytes("t DIB", "DIB")), l16(0), l16(0), l16(p)}));
  api(m, "GDI", "SelectObject", {w16(dc), w16(pb)});
  api(m, "GDI", "PatBlt", {w16(dc), w16(0), w16(0), w16(8), w16(8), l16(PATCOPY)});
  std::set<uint8_t> seen;
  for (uint32_t i = 0; i < 64; i++) seen.insert(m.rt.rd8(p + 0x428 + i));
  CHECK(seen == std::set<uint8_t>({0, 0x21}), "on a DIB DC: indices 0 and 21h (%zu values)", seen.size());
  // R2_MASKPEN with it (RCLOCK's fills): dest AND pattern, on the indices.
  for (uint32_t i = 0; i < 64; i++) m.rt.wr8(p + 0x428 + i, 0x33);
  api(m, "GDI", "SetROP2", {w16(dc), w16(R2_MASKPEN)});
  uint16_t rgn2 = uint16_t(api(m, "GDI", "CreateRectRgn", {w16(0), w16(0), w16(8), w16(8)}));
  api(m, "GDI", "FillRgn", {w16(dc), w16(rgn2), w16(pb)});
  seen.clear();
  for (uint32_t i = 0; i < 64; i++) seen.insert(m.rt.rd8(p + 0x428 + i));
  CHECK(seen == std::set<uint8_t>({0, 0x21 & 0x33}), "FillRgn under R2_MASKPEN: 33h AND the pattern (%zu values)", seen.size());
  api(m, "GDI", "DeleteDC", {w16(dc)});
  // A DIB pattern brush's own bitmap (made from the DIB, never the guest's)
  // is deleted with it: a brush made and deleted every frame never fills the
  // handle table (6,144 slots), and each pair gets the same handles back —
  // CreateBrushIndirect's BS_DIBPATTERN alike. BS_PATTERN's bitmap is the
  // guest's and stays.
  uint16_t hd2 = m.rt.global().alloc(GlobalHeap16::kMoveable | GlobalHeap16::kZeroInit, 40 + 1024 + 64);
  m.rt.write_bytes(m.rt.global().lock(hd2), &bi, sizeof(bi));
  m.rt.global().unlock(hd2);
  uint32_t lb = uint32_t(m.data(16)) << 16;
  m.rt.wr16(lb, BS_DIBPATTERN);
  m.rt.wr32(lb + 2, DIB_RGB_COLORS);
  m.rt.wr16(lb + 6, hd2);
  for (int indirect = 0; indirect < 2; indirect++) {
    uint16_t first = 0;
    int made = 0;
    bool same = true;
    for (int i = 0; i < 7000; i++) {
      uint16_t b = uint16_t(indirect ? api(m, "GDI", "CreateBrushIndirect", {l16(lb)})
                                     : api(m, "GDI", "CreateDIBPatternBrush", {w16(hd2), w16(DIB_RGB_COLORS)}));
      if (!b) break;
      made++;
      if (!first) first = b;
      same &= b == first;
      api(m, "GDI", "DeleteObject", {w16(b)});
    }
    CHECK(made == 7000 && same, "%s: 7000 brushes made and deleted, one handle (%d, %04X)",
          indirect ? "CreateBrushIndirect(BS_DIBPATTERN)" : "CreateDIBPatternBrush", made, first);
  }
  CHECK((api(m, "GDI", "CreateSolidBrush", {l16(RGB(1, 2, 3))}) & 0xFFFF) != 0, "the table has room afterwards");
  // A stale handle the guest deletes twice may name the brush's bitmap (its
  // freed slot was reused for it); the object that takes the slot next is
  // not deleted with the brush.
  uint16_t stale = uint16_t(api(m, "GDI", "CreateCompatibleBitmap", {w16(hdc), w16(8), w16(8)}));
  api(m, "GDI", "DeleteObject", {w16(stale)});
  uint16_t sb = uint16_t(api(m, "GDI", "CreateDIBPatternBrush", {w16(hd2), w16(DIB_RGB_COLORS)}));
  api(m, "GDI", "DeleteObject", {w16(stale)});
  uint16_t other = uint16_t(api(m, "GDI", "CreateSolidBrush", {l16(RGB(4, 5, 6))}));
  CHECK(other == stale && (api(m, "GDI", "DeleteObject", {w16(sb)}) & 0xFFFF) == 1 && g.get(other, G16::brush),
        "a brush in the stale slot (%04X, %04X) outlives the pattern brush deleted after it", other, stale);
  uint16_t pat = uint16_t(api(m, "GDI", "CreateCompatibleBitmap", {w16(hdc), w16(8), w16(8)}));
  m.rt.wr16(lb, BS_PATTERN);
  m.rt.wr16(lb + 6, pat);
  uint16_t patb = uint16_t(api(m, "GDI", "CreateBrushIndirect", {l16(lb)}));
  CHECK(patb && (api(m, "GDI", "DeleteObject", {w16(patb)}) & 0xFFFF) == 1 && g.get(pat, G16::bitmap),
        "BS_PATTERN: deleting the brush leaves the guest's bitmap");
  // GlobalWire: a lock; GlobalUnWire TRUE once unlocked.
  uint16_t gw = uint16_t(api(m, "KERNEL", "GlobalAlloc", {w16(GlobalHeap16::kMoveable), l16(64)}));
  uint32_t wp = api(m, "KERNEL", "GlobalWire", {w16(gw)});
  CHECK(wp == (uint32_t(gw | 1) << 16) && (m.rt.global().flags(gw) & 0xFF) == 1, "GlobalWire locks (%08X)", wp);
  CHECK((api(m, "KERNEL", "GlobalUnWire", {w16(gw)}) & 0xFFFF) == 1 && (m.rt.global().flags(gw) & 0xFF) == 0,
        "GlobalUnWire unlocks");
}

// GetMapMode: DCs start in MM_TEXT; SetMapMode's mode comes back; the
// ScreamSavers idiom, SetMapMode(mem, GetMapMode(screen)), keeps MM_TEXT.
void test_map_mode() {
  Machine m;
  Screen screen(64, 48);
  m.rt.attach_display(screen);
  uint16_t sdc = gdi16_screen_dc(m.rt, user16_saver_window(m.rt));
  uint16_t mdc = uint16_t(api(m, "GDI", "CreateCompatibleDC", {w16(sdc)}));
  auto mode = [&](uint16_t dc) { return int(api(m, "GDI", "GetMapMode", {w16(dc)}) & 0xFFFF); };
  CHECK(mode(sdc) == MM_TEXT && mode(mdc) == MM_TEXT, "screen and memory DCs start in MM_TEXT (%d %d)", mode(sdc),
        mode(mdc));
  api(m, "GDI", "SetMapMode", {w16(mdc), w16(MM_ANISOTROPIC)});
  CHECK(mode(mdc) == MM_ANISOTROPIC, "SetMapMode(MM_ANISOTROPIC) comes back (%d)", mode(mdc));
  api(m, "GDI", "SetMapMode", {w16(mdc), w16(uint16_t(mode(sdc)))});
  CHECK(mode(mdc) == MM_TEXT, "SetMapMode(mem, GetMapMode(screen)): MM_TEXT again (%d)", mode(mdc));
  CHECK(mode(0x1234) == 0, "no DC: 0");
  api(m, "GDI", "DeleteDC", {w16(mdc)});
}

// IntersectClipRect (GDI.22; Intermission's ASA reader through ANTSW): the
// clip region cut to the rectangle, in logical units, right and bottom
// excluded; the result the region's type (SIMPLEREGION, NULLREGION once
// nothing is left), 0 for no DC; SaveDC/RestoreDC bring the old region back.
void test_intersect_clip_rect() {
  Machine m;
  Screen screen(64, 48);
  m.rt.attach_display(screen);
  uint16_t sdc = gdi16_screen_dc(m.rt, user16_saver_window(m.rt));
  uint16_t mdc = uint16_t(api(m, "GDI", "CreateCompatibleDC", {w16(sdc)}));
  uint16_t bmp = uint16_t(api(m, "GDI", "CreateCompatibleBitmap", {w16(sdc), w16(32), w16(32)}));
  api(m, "GDI", "SelectObject", {w16(mdc), w16(bmp)});
  api(m, "GDI", "PatBlt", {w16(mdc), w16(0), w16(0), w16(32), w16(32), l16(BLACKNESS)});
  const uint32_t rc = uint32_t(m.data(16)) << 16;
  auto box = [&](uint16_t dc) {
    int t = int(api(m, "GDI", "GetClipBox", {w16(dc), l16(rc)}) & 0xFFFF);
    char b[64];
    snprintf(b, sizeof(b), "%d:%d,%d,%d,%d", t, int16_t(m.rt.rd16(rc)), int16_t(m.rt.rd16(rc + 2)),
             int16_t(m.rt.rd16(rc + 4)), int16_t(m.rt.rd16(rc + 6)));
    return std::string(b);
  };
  auto pixel = [&](int x, int y) {
    return uint32_t(api(m, "GDI", "GetPixel", {w16(mdc), w16(uint16_t(x)), w16(uint16_t(y))}));
  };
  api(m, "GDI", "SaveDC", {w16(mdc)});
  uint16_t t = uint16_t(api(m, "GDI", "IntersectClipRect", {w16(mdc), w16(4), w16(4), w16(12), w16(10)}));
  CHECK(t == SIMPLEREGION && box(mdc) == "2:4,4,12,10", "IntersectClipRect: a simple region, the box (%u, %s)", t,
        box(mdc).c_str());
  api(m, "GDI", "PatBlt", {w16(mdc), w16(0), w16(0), w16(32), w16(32), l16(WHITENESS)});
  CHECK(pixel(3, 4) == CLR_INVALID && pixel(4, 4) == RGB(255, 255, 255),
        "GetPixel outside the clip region: CLR_INVALID (%08X %08X)", pixel(3, 4), pixel(4, 4));
  t = uint16_t(api(m, "GDI", "IntersectClipRect", {w16(mdc), w16(8), w16(6), w16(30), w16(30)}));
  CHECK(t == SIMPLEREGION && box(mdc) == "2:8,6,12,10", "a second rectangle cuts the region again (%u, %s)", t,
        box(mdc).c_str());
  t = uint16_t(api(m, "GDI", "IntersectClipRect", {w16(mdc), w16(20), w16(20), w16(30), w16(30)}));
  CHECK(t == NULLREGION, "a rectangle outside the region: nothing left, NULLREGION (%u)", t);
  api(m, "GDI", "RestoreDC", {w16(mdc), w16(0xFFFF)});
  CHECK(box(mdc) == "2:0,0,32,32", "RestoreDC: the whole bitmap again (%s)", box(mdc).c_str());
  CHECK(pixel(4, 4) == RGB(255, 255, 255) && pixel(11, 9) == RGB(255, 255, 255) && pixel(3, 4) == 0 &&
            pixel(12, 9) == 0 && pixel(11, 10) == 0 && pixel(20, 20) == 0,
        "the fill reached the rectangle alone, right and bottom excluded (%06X %06X %06X %06X %06X %06X)", pixel(4, 4),
        pixel(11, 9), pixel(3, 4), pixel(12, 9), pixel(11, 10), pixel(20, 20));
  CHECK(api(m, "GDI", "IntersectClipRect", {w16(0x1234), w16(0), w16(0), w16(1), w16(1)}) == 0, "no DC: 0 (ERROR)");
  api(m, "GDI", "DeleteDC", {w16(mdc)});
  api(m, "GDI", "DeleteObject", {w16(bmp)});
  CHECK(m.rt.shims().find_name("GDI", "IntersectClipRect")->calls == 4, "GDI.22 is implemented (no stub)");
}

// FloodFill and ExtFloodFill (Snoopy's sprite masks): the colour keyed as
// SetPixel's, so pixel indices are compared (PALETTEINDEX and PALETTERGB
// through the DC's palette, a plain RGB to the statics); real GDI's fill,
// 4-connected, with the DC's brush, in logical coordinates, within the clip
// region, FALSE when the point is not in the area; on a monochrome bitmap
// the colour's nearest of black and white; on a DIB DC its pixel values, in
// its own row order. Every fill charges the pixels it painted: the host's
// count is held here to what real GDI painted.
void test_flood_fill() {
  Machine m;
  Screen screen(64, 48);
  m.rt.attach_display(screen);
  Gdi16& g = m.rt.state<Gdi16>();
  uint16_t sdc = gdi16_screen_dc(m.rt, user16_saver_window(m.rt));
  uint16_t mdc = uint16_t(api(m, "GDI", "CreateCompatibleDC", {w16(sdc)}));
  const int W = 8, H = 5;
  uint16_t bmp = uint16_t(api(m, "GDI", "CreateCompatibleBitmap", {w16(sdc), w16(W), w16(H)}));
  api(m, "GDI", "SelectObject", {w16(mdc), w16(bmp)});
  Obj16* bo = g.get(bmp, G16::bitmap);
  CHECK(bo && bo->bmp.bpp == 8 && bo->bmp.bits, "an 8-bit bitmap");
  if (!bo || !bo->bmp.bits) return;
  // Pictures in hardware indices: '.' light grey (7), '#' black (0), 'o'
  // yellow (251), 'w' white (255), 'p' a palette's white (slot 10, below),
  // 'r' red (249), the brush's.
  const std::string key = ".#owpr";
  const uint8_t idx[] = {7, 0, 251, 255, 10, 249};
  auto load = [&](const std::vector<std::string>& rows) {
    GdiFlush();
    for (int y = 0; y < H; y++) {
      for (int x = 0; x < W; x++) bo->bmp.bits[size_t(y) * bo->bmp.stride + size_t(x)] = idx[key.find(rows[size_t(y)][size_t(x)])];
    }
  };
  auto show = [&]() {
    GdiFlush();
    std::string s;
    for (int y = 0; y < H; y++) {
      if (y) s += '/';
      for (int x = 0; x < W; x++) {
        size_t k = size_t(std::find(idx, idx + 6, bo->bmp.bits[size_t(y) * bo->bmp.stride + size_t(x)]) - idx);
        s += k < 6 ? key[k] : '?';
      }
    }
    return s;
  };
  // A call's result, the pixels it charged (its work beyond a call that
  // fills nothing, over the pixel cost) and the 8-bit bitmap after it.
  auto work = [&](bool ext, uint16_t dc, int x, int y, uint32_t col, uint16_t type, int* ok) {
    uint64_t before = m.rt.work_insns();
    uint32_t r = ext ? api(m, "GDI", "ExtFloodFill", {w16(dc), w16(uint16_t(x)), w16(uint16_t(y)), l16(col), w16(type)})
                     : api(m, "GDI", "FloodFill", {w16(dc), w16(uint16_t(x)), w16(uint16_t(y)), l16(col)});
    if (ok) *ok = int(r & 0xFFFF);
    return m.rt.work_insns() - before;
  };
  const uint64_t idle_ext = work(true, mdc, -1, 0, RGB(0, 0, 0), FLOODFILLBORDER, nullptr);
  const uint64_t idle_ff = work(false, mdc, -1, 0, RGB(0, 0, 0), 0, nullptr);
  struct Fill {
    int ok = 0;
    int64_t charged = 0;
    std::string after;
  };
  auto fill = [&](bool ext, uint16_t dc, int x, int y, uint32_t col, uint16_t type) {
    Fill f;
    uint64_t wk = work(ext, dc, x, y, col, type, &f.ok);
    f.charged = int64_t(wk - (ext ? idle_ext : idle_ff)) / int64_t(Runtime16Options{}.pixel_cost_insns);
    f.after = show();
    return f;
  };
  uint16_t red = uint16_t(api(m, "GDI", "CreateSolidBrush", {l16(RGB(255, 0, 0))}));
  api(m, "GDI", "SelectObject", {w16(mdc), w16(red)});
  // FLOODFILLSURFACE is 4-connected: a diagonal of black stops it.
  load({"...#....", "..#.....", ".#......", "#.......", "........"});
  Fill a = fill(true, mdc, 0, 0, RGB(0xC0, 0xC0, 0xC0), FLOODFILLSURFACE);
  CHECK(a.ok == 1 && a.after == "rrr#..../rr#...../r#....../#......./........" && a.charged == 6,
        "surface fill: the corner alone (%d, %s, %lld pixels)", a.ok, a.after.c_str(), (long long)a.charged);
  // FLOODFILLBORDER, and FloodFill: everything up to the colour, whatever it holds.
  const std::vector<std::string> box = {"########", "#..o...#", "#.o..o.#", "########", "........"};
  load(box);
  Fill b = fill(true, mdc, 1, 1, RGB(0, 0, 0), FLOODFILLBORDER);
  CHECK(b.ok == 1 && b.after == "########/#rrrrrr#/#rrrrrr#/########/........" && b.charged == 12,
        "border fill: the box's inside (%d, %s, %lld pixels)", b.ok, b.after.c_str(), (long long)b.charged);
  load(box);
  Fill b2 = fill(false, mdc, 6, 2, RGB(0, 0, 0), 0);
  CHECK(b2.ok == 1 && b2.after == b.after && b2.charged == 12, "FloodFill: the same (%d, %s, %lld pixels)", b2.ok,
        b2.after.c_str(), (long long)b2.charged);
  // No fill: the point on the border colour, not of the surface's, or off the bitmap.
  load(box);
  Fill n1 = fill(true, mdc, 0, 0, RGB(0, 0, 0), FLOODFILLBORDER);
  Fill n2 = fill(true, mdc, 3, 1, RGB(0xC0, 0xC0, 0xC0), FLOODFILLSURFACE);
  Fill n3 = fill(true, mdc, W, 0, RGB(0, 0, 0), FLOODFILLBORDER);
  CHECK(n1.ok == 0 && n2.ok == 0 && n3.ok == 0 && n1.charged == 0 && n2.charged == 0 && n3.charged == 0 &&
            n3.after == "########/#..o...#/#.o..o.#/########/........",
        "no fill: FALSE, nothing painted or charged (%d %d %d, %lld %lld %lld)", n1.ok, n2.ok, n3.ok,
        (long long)n1.charged, (long long)n2.charged, (long long)n3.charged);
  // Palette-mapped colours: PALETTEINDEX and PALETTERGB name the palette's
  // white, realized at slot 10; a plain RGB white is the static at 255.
  uint32_t lp = uint32_t(m.data(16)) << 16;
  m.rt.wr16(lp, 0x300);
  m.rt.wr16(lp + 2, 1);
  m.rt.wr32(lp + 4, 0x01FFFFFF);  // white, PC_RESERVED
  uint16_t pal = uint16_t(api(m, "GDI", "CreatePalette", {l16(lp)}));
  api(m, "USER", "SelectPalette", {w16(sdc), w16(pal), w16(0)});
  api(m, "USER", "RealizePalette", {w16(sdc)});
  api(m, "USER", "SelectPalette", {w16(mdc), w16(pal), w16(0)});
  Obj16* po = g.get(pal, G16::palette);
  CHECK(po && po->pal->map.size() == 1 && po->pal->map[0] == 10, "the palette's white is at slot 10");
  const std::vector<std::string> whites = {"pppp####", "pppp#www", "####wwww", "wwwwwwww", "wwwwwwww"};
  load(whites);
  Fill p1 = fill(true, mdc, 0, 0, 0x01000000, FLOODFILLSURFACE);  // PALETTEINDEX(0)
  CHECK(p1.ok == 1 && p1.after == "rrrr####/rrrr#www/####wwww/wwwwwwww/wwwwwwww" && p1.charged == 8,
        "PALETTEINDEX(0): the slot's area (%d, %s, %lld pixels)", p1.ok, p1.after.c_str(), (long long)p1.charged);
  load(whites);
  Fill p2 = fill(true, mdc, 1, 1, 0x02FFFFFF, FLOODFILLSURFACE);  // PALETTERGB(255, 255, 255)
  CHECK(p2.ok == 1 && p2.after == p1.after && p2.charged == 8, "PALETTERGB white: the same (%d, %s)", p2.ok,
        p2.after.c_str());
  load(whites);
  Fill p3 = fill(true, mdc, 0, 0, RGB(255, 255, 255), FLOODFILLSURFACE);
  Fill p4 = fill(true, mdc, 7, 4, RGB(255, 255, 255), FLOODFILLSURFACE);
  CHECK(p3.ok == 0 && p3.charged == 0 && p4.ok == 1 &&
            p4.after == "pppp####/pppp#rrr/####rrrr/rrrrrrrr/rrrrrrrr" && p4.charged == 23,
        "plain RGB white: the static's area, not the slot's (%d; %d, %s, %lld pixels)", p3.ok, p4.ok, p4.after.c_str(),
        (long long)p4.charged);
  api(m, "USER", "SelectPalette", {w16(mdc), w16(g.stock(DEFAULT_PALETTE)), w16(0)});
  // Logical coordinates: with the viewport origin at (4, 0), logical (0, 0) is pixel (4, 0).
  load({"....oooo", "....oooo", "....oooo", "....oooo", "....oooo"});
  api(m, "GDI", "SetViewportOrg", {w16(mdc), w16(4), w16(0)});
  Fill v = fill(true, mdc, 0, 0, RGB(255, 255, 0), FLOODFILLSURFACE);
  api(m, "GDI", "SetViewportOrg", {w16(mdc), w16(0), w16(0)});
  CHECK(v.ok == 1 && v.after == "....rrrr/....rrrr/....rrrr/....rrrr/....rrrr" && v.charged == 20,
        "viewport origin (4, 0): the right half (%d, %s, %lld pixels)", v.ok, v.after.c_str(), (long long)v.charged);
  // A screen DC: the display's pixels.
  api(m, "GDI", "PatBlt", {w16(sdc), w16(0), w16(0), w16(64), w16(48), l16(BLACKNESS)});
  api(m, "GDI", "PatBlt", {w16(sdc), w16(10), w16(10), w16(20), w16(5), l16(WHITENESS)});
  uint16_t was = uint16_t(api(m, "GDI", "SelectObject", {w16(sdc), w16(red)}));
  Fill sc = fill(true, sdc, 12, 12, RGB(255, 255, 255), FLOODFILLSURFACE);
  api(m, "GDI", "SelectObject", {w16(sdc), w16(was)});
  CHECK(sc.ok == 1 && sc.charged == 100 && screen.at(10, 10) == 249 && screen.at(29, 14) == 249 && screen.at(9, 10) == 0 &&
            screen.at(30, 14) == 0 && screen.at(10, 15) == 0,
        "the screen: the white rectangle (%d, %lld pixels, %u %u %u)", sc.ok, (long long)sc.charged, screen.at(10, 10),
        screen.at(9, 10), screen.at(10, 15));
  // The clip region bounds the fill: column 3 is clipped away but for the
  // bottom row, so the right half is reached through that row alone, and not
  // its top block, which touches the rest only through clipped pixels; a
  // point outside the region fills nothing.
  auto rgn = [&](int l, int t, int r, int b) {
    return uint16_t(api(m, "GDI", "CreateRectRgn", {w16(uint16_t(l)), w16(uint16_t(t)), w16(uint16_t(r)), w16(uint16_t(b))}));
  };
  uint16_t clip = rgn(0, 0, 3, 5), top = rgn(4, 0, 8, 2), bottom = rgn(3, 4, 8, 5);
  api(m, "GDI", "CombineRgn", {w16(clip), w16(clip), w16(top), w16(RGN_OR)});
  api(m, "GDI", "CombineRgn", {w16(clip), w16(clip), w16(bottom), w16(RGN_OR)});
  api(m, "GDI", "SelectClipRgn", {w16(mdc), w16(clip)});
  const std::vector<std::string> grey(size_t(H), "........");
  load(grey);
  Fill k1 = fill(true, mdc, 0, 0, RGB(0xC0, 0xC0, 0xC0), FLOODFILLSURFACE);
  CHECK(k1.ok == 1 && k1.after == "rrr...../rrr...../rrr...../rrr...../rrrrrrrr" && k1.charged == 20,
        "clipped: the left columns and the bottom row (%d, %s, %lld pixels)", k1.ok, k1.after.c_str(),
        (long long)k1.charged);
  load(grey);
  Fill k2 = fill(true, mdc, 3, 1, RGB(0xC0, 0xC0, 0xC0), FLOODFILLSURFACE);
  CHECK(k2.ok == 0 && k2.charged == 0 && k2.after == "......../......../......../......../........",
        "a point outside the clip region: FALSE (%d, %lld)", k2.ok, (long long)k2.charged);
  api(m, "GDI", "SelectClipRgn", {w16(mdc), w16(0)});
  // A monochrome bitmap, a black column at x 8: light grey is white there,
  // a dark grey black (the colour keyed as everywhere: its static first).
  uint16_t mono = uint16_t(api(m, "GDI", "CreateBitmap", {w16(16), w16(2), w16(1), w16(1), l16(0)}));
  uint16_t mmdc = uint16_t(api(m, "GDI", "CreateCompatibleDC", {w16(sdc)}));
  api(m, "GDI", "SelectObject", {w16(mmdc), w16(mono)});
  api(m, "GDI", "SelectObject", {w16(mmdc), w16(g.stock(BLACK_BRUSH))});
  auto mono_load = [&]() {
    api(m, "GDI", "PatBlt", {w16(mmdc), w16(0), w16(0), w16(16), w16(2), l16(WHITENESS)});
    api(m, "GDI", "PatBlt", {w16(mmdc), w16(8), w16(0), w16(1), w16(2), l16(BLACKNESS)});
  };
  auto mono_rows = [&]() {
    GdiFlush();
    uint8_t mb[4] = {};
    GetBitmapBits(static_cast<HBITMAP>(g.host(mono)), 4, mb);
    char s[16];
    snprintf(s, sizeof(s), "%02X%02X %02X%02X", mb[0], mb[1], mb[2], mb[3]);
    return std::string(s);
  };
  mono_load();
  Fill q1 = fill(true, mmdc, 0, 0, RGB(200, 200, 200), FLOODFILLSURFACE);
  std::string q1_rows = mono_rows();
  CHECK(q1.ok == 1 && q1_rows == "007F 007F" && q1.charged == 16, "monochrome, light grey's surface: the left half (%d, %s, %lld)",
        q1.ok, q1_rows.c_str(), (long long)q1.charged);
  mono_load();
  Fill q2 = fill(true, mmdc, 15, 1, RGB(40, 40, 40), FLOODFILLBORDER);
  std::string q2_rows = mono_rows();
  CHECK(q2.ok == 1 && q2_rows == "FF00 FF00" && q2.charged == 14, "monochrome, up to dark grey: the right side (%d, %s, %lld)",
        q2.ok, q2_rows.c_str(), (long long)q2.charged);
  // A DIB DC, bottom-up (its top row last in memory): DIBINDEX(5) from the
  // top-left reaches the top row and three of the next, not the bottom row.
  uint32_t p = packed_dib(m, 8, 4);
  uint16_t ddc = uint16_t(api(m, "GDI", "CreateDC", {l16(m.rt.static_bytes("t DIB", "DIB")), l16(0), l16(0), l16(p)}));
  const char* drows[4] = {"55555555", "55599999", "99999999", "55555555"};
  for (uint32_t y = 0; y < 4; y++) {
    for (uint32_t x = 0; x < 8; x++) m.rt.wr8(p + 0x428 + (3 - y) * 8 + x, uint8_t(drows[y][x] - '0'));
  }
  api(m, "GDI", "SelectObject", {w16(ddc), w16(uint16_t(api(m, "GDI", "CreateSolidBrush", {l16(0x10FF0007)})))});
  Fill d = fill(true, ddc, 0, 0, 0x10FF0005, FLOODFILLSURFACE);
  std::string dib_after;
  for (uint32_t y = 0; y < 4; y++) {
    if (y) dib_after += '/';
    for (uint32_t x = 0; x < 8; x++) dib_after += char('0' + m.rt.rd8(p + 0x428 + (3 - y) * 8 + x));
  }
  CHECK(d.ok == 1 && dib_after == "77777777/77799999/99999999/55555555" && d.charged == 11,
        "a bottom-up DIB DC: its pixel values (%d, %s, %lld pixels)", d.ok, dib_after.c_str(), (long long)d.charged);
  api(m, "GDI", "DeleteDC", {w16(ddc)});
  api(m, "GDI", "DeleteDC", {w16(mmdc)});
  api(m, "GDI", "DeleteDC", {w16(mdc)});
}

// GetDIBits(DIB_PAL_COLORS): the bits stay hardware indices (never logical
// ones) and the colour table describes them, entry h the DC palette's
// logical entry nearest hardware colour h, so ADXPL41's label canvas keeps
// its colours through its round trips, SetDIBits → GetDIBits → SetDIBits,
// through a 255-entry palette holding the low statics at 0..9 and the high
// ones at 245..254. The identity table written before sent hardware 255 to
// logical 255, past the palette's end (black), and 246..254 one static on.
void test_dib_pal_colors() {
  Machine m;
  Screen screen(64, 48);
  m.rt.attach_display(screen);
  Gdi16& g = m.rt.state<Gdi16>();
  uint16_t sdc = gdi16_screen_dc(m.rt, user16_saver_window(m.rt));
  // ADXPL41's kind of palette: the statics at 0..9 and 245..254, colours of
  // its own (PC_NOCOLLAPSE) between, 10 a second black.
  win32::LogicalPalette st = win32::Display::default_palette();
  uint32_t lp = uint32_t(m.data(4 + 4 * 255)) << 16;
  m.rt.wr16(lp, 0x300);
  m.rt.wr16(lp + 2, 255);
  for (uint32_t i = 0; i < 255; i++) {
    PALETTEENTRY e{BYTE(i), BYTE(255 - i), BYTE(i * 7), PC_NOCOLLAPSE};
    if (i < 10) e = st.entries[i];
    if (i >= 245) e = st.entries[i - 235];
    if (i == 10) e = PALETTEENTRY{0, 0, 0, PC_NOCOLLAPSE};
    write16(m.rt, lp + 4 + 4 * i, e);
  }
  uint16_t pal = uint16_t(api(m, "GDI", "CreatePalette", {l16(lp)}));
  api(m, "USER", "SelectPalette", {w16(sdc), w16(pal), w16(0)});
  api(m, "USER", "RealizePalette", {w16(sdc)});
  Obj16* po = g.get(pal, G16::palette);
  CHECK(po && po->pal->map.size() == 255 && po->pal->map[10] == 10 && po->pal->map[244] == 244 &&
            po->pal->map[245] == 246 && po->pal->map[254] == 255,
        "realized: 0..244 in place, the high statics at 246..255");
  // The canvas: a 16x16 DIB, pixel i of value i, with ADXPL41's own table
  // (hardware → logical: 10 → 0, 246..255 → 245..254).
  const uint32_t hsize = 40 + 4 * 256;
  uint32_t dib = uint32_t(m.data(hsize + 256)) << 16, bits = dib + hsize;
  auto header = [&]() {
    BITMAPINFOHEADER bi{sizeof(BITMAPINFOHEADER), 16, 16, 1, 8, BI_RGB, 256, 0, 0, 256, 0};
    m.rt.write_bytes(dib, &bi, sizeof(bi));
  };
  header();
  for (uint32_t i = 0; i < 256; i++) {
    m.rt.wr16(dib + 40 + 2 * i, uint16_t(i == 10 ? 0 : i >= 246 ? i - 1 : i));
    m.rt.wr8(bits + i, uint8_t(i));
  }
  uint16_t b1 = uint16_t(api(m, "GDI", "CreateCompatibleBitmap", {w16(sdc), w16(16), w16(16)}));
  uint16_t b2 = uint16_t(api(m, "GDI", "CreateCompatibleBitmap", {w16(sdc), w16(16), w16(16)}));
  auto set = [&](uint16_t b) {
    return int(api(m, "GDI", "SetDIBits", {w16(sdc), w16(b), w16(0), w16(16), l16(bits), l16(dib), w16(DIB_PAL_COLORS)}) & 0xFFFF);
  };
  auto get = [&](uint16_t dc, uint16_t usage) {
    return int(api(m, "GDI", "GetDIBits", {w16(dc), w16(b1), w16(0), w16(16), l16(bits), l16(dib), w16(usage)}) & 0xFFFF);
  };
  CHECK(set(b1) == 16, "SetDIBits through the palette");
  GdiFlush();
  Obj16* o1 = g.get(b1, G16::bitmap);
  Obj16* o2 = g.get(b2, G16::bitmap);
  // Bottom-up: value i's pixel is on row 15 - i / 16.
  auto hw = [&](Obj16* o, uint32_t i) { return int(o->bmp.bits[size_t(15 - i / 16) * o->bmp.stride + i % 16]); };
  CHECK(hw(o1, 255) == 255 && hw(o1, 250) == 250 && hw(o1, 246) == 246 && hw(o1, 10) == 0 && hw(o1, 100) == 100,
        "the canvas in hardware indices: white 255, 250, 246, a black 0, 100 (%d %d %d %d %d)", hw(o1, 255), hw(o1, 250),
        hw(o1, 246), hw(o1, 10), hw(o1, 100));
  // GetDIBits through the same DC: the table describes the hardware indices.
  for (uint32_t i = 0; i < 256; i++) {
    m.rt.wr16(dib + 40 + 2 * i, 0xEEEE);
    m.rt.wr8(bits + i, 0);
  }
  CHECK(get(sdc, DIB_PAL_COLORS) == 16, "GetDIBits(DIB_PAL_COLORS)");
  auto tab = [&](uint32_t h) { return int(m.rt.rd16(dib + 40 + 2 * h)); };
  CHECK(tab(255) == 254 && tab(246) == 245 && tab(250) == 249 && tab(10) == 0 && tab(0) == 0 && tab(7) == 7 &&
            tab(100) == 100 && tab(244) == 244,
        "the table: 255 -> 254, 246 -> 245, 250 -> 249, 10 -> 0, 100 -> 100 (%d %d %d %d %d)", tab(255), tab(246),
        tab(250), tab(10), tab(100));
  CHECK(m.rt.rd8(bits + 255) == 255 && m.rt.rd8(bits + 250) == 250 && m.rt.rd8(bits + 10) == 0 && m.rt.rd8(bits + 100) == 100,
        "the bits stay hardware indices (%u %u %u %u)", m.rt.rd8(bits + 255), m.rt.rd8(bits + 250), m.rt.rd8(bits + 10),
        m.rt.rd8(bits + 100));
  // … and back through the table GetDIBits wrote: every colour kept, white
  // and the high statics included.
  CHECK(set(b2) == 16, "SetDIBits again");
  GdiFlush();
  int same = 0;
  for (uint32_t i = 0; i < 256; i++) same += g.index_rgb(hw(o1, i)) == g.index_rgb(hw(o2, i));
  CHECK(same == 256 && hw(o2, 255) == 255 && hw(o2, 246) == 246 && hw(o2, 254) == 254,
        "the round trip keeps every colour (%d of 256): 255, 246, 254 stay (%d %d %d)", same, hw(o2, 255), hw(o2, 246),
        hw(o2, 254));
  // A DC with no palette of its own names DEFAULT_PALETTE's entries (white its 20th).
  uint16_t mdc = uint16_t(api(m, "GDI", "CreateCompatibleDC", {w16(sdc)}));
  header();
  CHECK(get(mdc, DIB_PAL_COLORS) == 16 && tab(255) == 19 && tab(249) == 13 && tab(0) == 0 && tab(7) == 7,
        "DEFAULT_PALETTE: 255 -> 19, 249 -> 13 (%d %d)", tab(255), tab(249));
  // DIB_RGB_COLORS: the hardware colours, as ever (100: the palette's 100, 9B, BC).
  header();
  CHECK(get(sdc, DIB_RGB_COLORS) == 16 && m.rt.rd32(dib + 40 + 4 * 255) == 0x00FFFFFF &&
            m.rt.rd32(dib + 40 + 4 * 249) == 0x00FF0000 && m.rt.rd32(dib + 40 + 4 * 100) == 0x00649BBC,
        "DIB_RGB_COLORS: the colours (%08X)", m.rt.rd32(dib + 40 + 4 * 100));
  api(m, "GDI", "DeleteDC", {w16(mdc)});
}

// GetDIBits to 4-bit rows (Haunted captures the desktop a line at a time
// so): real GDI's table of 16 colours — dark grey at 7, light grey at 8 —
// each pixel its nearest entry, biClrUsed 0; with DIB_PAL_COLORS the table
// names the DC palette's entries nearest those colours. 1-, 16- and 32-bit
// rows stay refused.
void test_getdibits_4bpp() {
  Machine m;
  Screen screen(64, 48);
  m.rt.attach_display(screen);
  Gdi16& g = m.rt.state<Gdi16>();
  uint16_t sdc = gdi16_screen_dc(m.rt, user16_saver_window(m.rt));
  uint16_t bmp = uint16_t(api(m, "GDI", "CreateCompatibleBitmap", {w16(sdc), w16(5), w16(2)}));
  Obj16* o = g.get(bmp, G16::bitmap);
  CHECK(o && o->bmp.bits, "an 8-bit bitmap");
  if (!o || !o->bmp.bits) return;
  // Row 0: black, light grey, dark grey, red, white; row 1: the four statics
  // outside the 16 (C0DCC0, A6CAF0, FFFBF0, A0A0A4), then dark cyan.
  const uint8_t px[2][5] = {{0, 7, 248, 249, 255}, {8, 9, 246, 247, 6}};
  GdiFlush();
  for (int y = 0; y < 2; y++) {
    for (int x = 0; x < 5; x++) o->bmp.bits[size_t(y) * o->bmp.stride + size_t(x)] = px[y][x];
  }
  uint32_t dib = uint32_t(m.data(40 + 64 + 16)) << 16, bits = dib + 40 + 64;
  auto get = [&](uint16_t dc, uint16_t bpp, uint16_t usage) {
    BITMAPINFOHEADER bi{sizeof(BITMAPINFOHEADER), 5, 2, 1, bpp, BI_RGB, 0, 0, 0, 0, 0};
    m.rt.write_bytes(dib, &bi, sizeof(bi));
    for (uint32_t i = 0; i < 16; i++) m.rt.wr32(dib + 40 + 4 * i, 0x44332211);
    for (uint32_t i = 0; i < 16; i++) m.rt.wr8(bits + i, 0xAA);
    return int(api(m, "GDI", "GetDIBits", {w16(dc), w16(bmp), w16(0), w16(2), l16(bits), l16(dib), w16(usage)}) & 0xFFFF);
  };
  CHECK(get(sdc, 4, DIB_RGB_COLORS) == 2, "GetDIBits: two 4-bit lines");
  BITMAPINFOHEADER got = read16<BITMAPINFOHEADER>(m.rt, dib);
  CHECK(got.biBitCount == 4 && got.biClrUsed == 0 && got.biSizeImage == 8 && got.biWidth == 5 && got.biHeight == 2,
        "the header: 4 bits, biClrUsed 0, 8 bytes (%u %u %u)", got.biBitCount, unsigned(got.biClrUsed),
        unsigned(got.biSizeImage));
  const uint32_t want[16] = {0x000000, 0x800000, 0x008000, 0x808000, 0x000080, 0x800080, 0x008080, 0x808080,
                             0xC0C0C0, 0xFF0000, 0x00FF00, 0xFFFF00, 0x0000FF, 0xFF00FF, 0x00FFFF, 0xFFFFFF};
  int table_ok = 0;
  for (uint32_t i = 0; i < 16; i++) table_ok += m.rt.rd32(dib + 40 + 4 * i) == want[i];  // RGBQUAD: 0x00RRGGBB
  CHECK(table_ok == 16, "real GDI's 16 colours, dark grey at 7 (%d of 16; 7 = %06X, 8 = %06X)", table_ok,
        m.rt.rd32(dib + 40 + 4 * 7), m.rt.rd32(dib + 40 + 4 * 8));
  // Bottom-up: row 1 first — 8, 8, 15, 8, 6 — then row 0 — 0, 8, 7, 9, 15.
  const uint8_t rows[8] = {0x88, 0xF8, 0x60, 0x00, 0x08, 0x79, 0xF0, 0x00};
  uint8_t b[8] = {};
  m.rt.read_bytes(bits, b, 8);
  CHECK(memcmp(b, rows, 8) == 0, "each pixel its nearest colour (%02X%02X%02X%02X %02X%02X%02X%02X)", b[0], b[1], b[2], b[3],
        b[4], b[5], b[6], b[7]);
  // DIB_PAL_COLORS through a DC without a palette: DEFAULT_PALETTE's entries
  // nearest the 16 colours (dark grey its 12, light grey its 7); the same bits.
  uint16_t mdc = uint16_t(api(m, "GDI", "CreateCompatibleDC", {w16(sdc)}));
  CHECK(get(mdc, 4, DIB_PAL_COLORS) == 2, "GetDIBits(DIB_PAL_COLORS): two 4-bit lines");
  const int pal_want[16] = {0, 1, 2, 3, 4, 5, 6, 12, 7, 13, 14, 15, 16, 17, 18, 19};
  int pal_ok = 0;
  for (uint32_t i = 0; i < 16; i++) pal_ok += m.rt.rd16(dib + 40 + 2 * i) == pal_want[i];
  m.rt.read_bytes(bits, b, 8);
  CHECK(pal_ok == 16 && memcmp(b, rows, 8) == 0, "the palette's entries (%d of 16), the same bits", pal_ok);
  for (uint16_t bpp : {1, 16, 32}) CHECK(get(sdc, bpp, DIB_RGB_COLORS) == 0, "%u-bit rows: refused", bpp);
  api(m, "GDI", "DeleteDC", {w16(mdc)});
}

// GetDIBits of a monochrome bitmap: 1-bit rows are real GDI's, asked in a
// header of the host's own — 40 bytes, the bitmap's width and height —
// whatever the guest's says (real GDI writes rows as wide as its header's
// biWidth and reads biSize bytes of it: a wider header or a biSize over 40
// ran it past the host's buffers); 4-, 8- and 24-bit rows are the bitmap's
// black and white as hardware indices 0 and 255, described as for an 8-bit
// bitmap (real GDI's values: 0 and 15, 0 and 255, 000000 and FFFFFF; each
// request went to real GDI with room for 1-bit rows and two colours, and
// crashed the host); 16- and 32-bit rows and the stock 1x1 bitmap: refused.
void test_getdibits_mono() {
  Machine m;
  Screen screen(64, 48);
  m.rt.attach_display(screen);
  uint16_t sdc = gdi16_screen_dc(m.rt, user16_saver_window(m.rt));
  // 37x3 (a WORD row of 6 bytes, DIB rows of 8, 20, 40 and 112): white on
  // row 0 at x 0..7, on row 1 at 8..15, on row 2 at 32..36, black elsewhere.
  uint16_t mono = uint16_t(api(m, "GDI", "CreateBitmap", {w16(37), w16(3), w16(1), w16(1), l16(0)}));
  uint32_t seg = uint32_t(m.data(0x800)) << 16, dib = seg, bits = seg + 0x500, src = seg + 0x7C0;
  const uint8_t ddb[18] = {0xFF, 0, 0, 0, 0, 0, 0, 0xFF, 0, 0, 0, 0, 0, 0, 0, 0, 0xF8, 0};
  m.rt.write_bytes(src, ddb, sizeof(ddb));
  api(m, "GDI", "SetBitmapBits", {w16(mono), l16(sizeof(ddb)), l16(src)});
  auto white = [](int x, int y) { return y == 0 ? x < 8 : y == 1 ? (x >= 8 && x < 16) : x >= 32; };
  auto get = [&](uint16_t dc, uint16_t hb, uint16_t bpp, uint32_t size, int32_t w, int32_t h, uint16_t lines,
                 uint16_t usage) {
    for (uint32_t i = 0; i < 0x7C0; i += 4) m.rt.wr32(seg + i, 0xAAAAAAAA);
    BITMAPINFOHEADER bi{size, w, h, 1, bpp, BI_RGB, 0, 0, 0, 0, 0};
    m.rt.write_bytes(dib, &bi, sizeof(bi));
    uint32_t r = api(m, "GDI", "GetDIBits", {w16(dc), w16(hb), w16(0), w16(lines), l16(bits), l16(dib), w16(usage)});
    return int(r & 0xFFFF);
  };
  // The pixels that differ from white() in the three rows written (bottom-up
  // unless top_down), plus 1 when the byte after them was touched.
  auto bad_pixels = [&](uint16_t bpp, bool top_down) {
    uint32_t stride = ((37u * bpp + 31) / 32) * 4;
    int bad = 0;
    for (int r = 0; r < 3; r++) {
      uint32_t at = bits + uint32_t(r) * stride;
      for (int x = 0; x < 37; x++) {
        uint32_t v = bpp == 1   ? (m.rt.rd8(at + x / 8) >> (7 - x % 8)) & 1
                     : bpp == 4 ? (m.rt.rd8(at + x / 2) >> (x % 2 ? 0 : 4)) & 15
                     : bpp == 8 ? m.rt.rd8(at + x)
                                : m.rt.rd32(at + 3 * x) & 0xFFFFFF;
        uint32_t want = bpp == 1 ? 1 : bpp == 4 ? 15 : bpp == 8 ? 255 : 0xFFFFFF;
        bad += v != (white(x, top_down ? r : 2 - r) ? want : 0);
      }
    }
    return bad + (m.rt.rd8(bits + 3 * stride) != 0xAA);
  };
  auto header = [&]() { return read16<BITMAPINFOHEADER>(m.rt, dib); };
  // 1-bit rows, the bitmap's own header: real GDI's answer, as before.
  CHECK(get(sdc, mono, 1, 40, 37, 3, 3, DIB_RGB_COLORS) == 3 && header().biSizeImage == 24 &&
            m.rt.rd32(dib + 40) == 0 && m.rt.rd32(dib + 44) == 0x00FFFFFF && bad_pixels(1, false) == 0,
        "1-bit rows: black and white, 24 bytes (%d bad)", bad_pixels(1, false));
  // A header wider and taller than the bitmap, of 108 bytes, for 10 lines:
  // the bitmap's own rows, 8 bytes each, three of them; the header keeps its
  // size and its table follows it.
  CHECK(get(sdc, mono, 1, 108, 4096, 4096, 10, DIB_RGB_COLORS) == 3, "1-bit rows for a larger header: three lines");
  BITMAPINFOHEADER got = header();
  CHECK(got.biSize == 108 && got.biWidth == 37 && got.biHeight == 3 && got.biSizeImage == 24 &&
            m.rt.rd32(dib + 108) == 0 && m.rt.rd32(dib + 112) == 0x00FFFFFF && bad_pixels(1, false) == 0,
        "the bitmap's own 37x3 rows, the table after the 108 bytes (%u %dx%d, %d bad)", unsigned(got.biSize),
        int(got.biWidth), int(got.biHeight), bad_pixels(1, false));
  CHECK(get(sdc, mono, 1, 40, 37, -3, 3, DIB_RGB_COLORS) == 3 && header().biHeight == -3 && bad_pixels(1, true) == 0,
        "top-down 1-bit rows (%d bad)", bad_pixels(1, true));
  // 4-, 8- and 24-bit rows: black and white as the colour tables hold them.
  CHECK(get(sdc, mono, 4, 40, 37, 3, 3, DIB_RGB_COLORS) == 3, "4-bit rows");
  got = header();
  CHECK(got.biBitCount == 4 && got.biClrUsed == 0 && got.biSizeImage == 60 && m.rt.rd32(dib + 40) == 0 &&
            m.rt.rd32(dib + 40 + 4 * 7) == 0x00808080 && m.rt.rd32(dib + 40 + 4 * 15) == 0x00FFFFFF &&
            bad_pixels(4, false) == 0,
        "4-bit rows: black 0 and white 15 of real GDI's table (%u bytes, %d bad)", unsigned(got.biSizeImage),
        bad_pixels(4, false));
  CHECK(get(sdc, mono, 8, 40, 37, 3, 3, DIB_RGB_COLORS) == 3, "8-bit rows");
  got = header();
  CHECK(got.biClrUsed == 256 && got.biSizeImage == 120 && m.rt.rd32(dib + 40) == 0 &&
            m.rt.rd32(dib + 40 + 4 * 255) == 0x00FFFFFF && bad_pixels(8, false) == 0,
        "8-bit rows: hardware black 0 and white 255 (%u bytes, %d bad)", unsigned(got.biSizeImage),
        bad_pixels(8, false));
  CHECK(get(sdc, mono, 24, 40, 37, 3, 3, DIB_RGB_COLORS) == 3 && header().biSizeImage == 336 &&
            bad_pixels(24, false) == 0,
        "24-bit rows: 000000 and FFFFFF (%d bad)", bad_pixels(24, false));
  // DIB_PAL_COLORS: the table names the DC palette's entries nearest black
  // and white (a palette of red, white, black: 2 and 1); the bits the same.
  uint32_t lp = uint32_t(m.data(16)) << 16;
  m.rt.wr16(lp, 0x300);
  m.rt.wr16(lp + 2, 3);
  m.rt.wr32(lp + 4, 0x000000FF);   // red
  m.rt.wr32(lp + 8, 0x00FFFFFF);   // white
  m.rt.wr32(lp + 12, 0x00000000);  // black
  uint16_t pal = uint16_t(api(m, "GDI", "CreatePalette", {l16(lp)}));
  uint16_t mdc = uint16_t(api(m, "GDI", "CreateCompatibleDC", {w16(sdc)}));
  api(m, "USER", "SelectPalette", {w16(mdc), w16(pal), w16(0)});
  CHECK(get(mdc, mono, 8, 40, 37, 3, 3, DIB_PAL_COLORS) == 3 && m.rt.rd16(dib + 40) == 2 &&
            m.rt.rd16(dib + 40 + 2 * 255) == 1 && bad_pixels(8, false) == 0,
        "8-bit rows, DIB_PAL_COLORS: black -> 2, white -> 1 (%u %u, %d bad)", m.rt.rd16(dib + 40),
        m.rt.rd16(dib + 40 + 2 * 255), bad_pixels(8, false));
  CHECK(get(mdc, mono, 4, 40, 37, 3, 3, DIB_PAL_COLORS) == 3 && m.rt.rd16(dib + 40) == 2 &&
            m.rt.rd16(dib + 40 + 2 * 15) == 1 && bad_pixels(4, false) == 0,
        "4-bit rows, DIB_PAL_COLORS: black -> 2, white -> 1 (%u %u)", m.rt.rd16(dib + 40),
        m.rt.rd16(dib + 40 + 2 * 15));
  // 16- and 32-bit rows: refused, nothing written.
  for (uint16_t bpp : {16, 32}) {
    CHECK(get(sdc, mono, bpp, 40, 37, 3, 3, DIB_RGB_COLORS) == 0 && header().biBitCount == bpp &&
              m.rt.rd32(dib + 40) == 0xAAAAAAAA && m.rt.rd8(bits) == 0xAA,
          "%u-bit rows: refused", bpp);
  }
  // The stock 1x1 bitmap a memory DC starts with has no pixels here.
  uint16_t other = uint16_t(api(m, "GDI", "CreateBitmap", {w16(8), w16(8), w16(1), w16(1), l16(0)}));
  uint16_t stock = uint16_t(api(m, "GDI", "SelectObject", {w16(mdc), w16(other)}));
  CHECK(stock && get(sdc, stock, 8, 40, 1, 1, 1, DIB_RGB_COLORS) == 0, "the stock bitmap %04X: refused", stock);
  // No host buffer ran over.
  CHECK(HeapValidate(GetProcessHeap(), 0, nullptr), "the host heap is intact");
  api(m, "GDI", "SelectObject", {w16(mdc), w16(stock)});
  api(m, "GDI", "DeleteDC", {w16(mdc)});
  api(m, "GDI", "DeleteObject", {w16(other)});
  api(m, "GDI", "DeleteObject", {w16(pal)});
  api(m, "GDI", "DeleteObject", {w16(mono)});
}

// CreateBitmapIndirect and CreatePatternBrush (Little Mermaid's "Plain"
// sea): an 8x8 monochrome BITMAP made a brush and deleted before the brush
// is used — the brush has its own copy —, which paints its 0 bits in the
// text colour and its 1 bits in the background colour; of a larger colour
// bitmap (selected into a DC, at that) the top-left 8x8 alone; the brush's
// copy goes with it.
void test_pattern_brush() {
  Machine m;
  Screen screen(64, 48);
  m.rt.attach_display(screen);
  Gdi16& g = m.rt.state<Gdi16>();
  uint16_t sdc = gdi16_screen_dc(m.rt, user16_saver_window(m.rt));
  uint16_t mdc = uint16_t(api(m, "GDI", "CreateCompatibleDC", {w16(sdc)}));
  uint16_t canvas = uint16_t(api(m, "GDI", "CreateCompatibleBitmap", {w16(sdc), w16(16), w16(16)}));
  api(m, "GDI", "SelectObject", {w16(mdc), w16(canvas)});
  Obj16* co = g.get(canvas, G16::bitmap);
  auto at = [&](int x, int y) {
    GdiFlush();
    return int(co->bmp.bits[size_t(y) * co->bmp.stride + size_t(x)]);
  };
  uint32_t rc = uint32_t(m.data(16)) << 16;
  write16(m.rt, rc, RECT16{0, 0, 16, 16});
  // The BITMAP: 8x8, one plane, one bit, WORD rows alternately 0F and F0.
  uint32_t bm = uint32_t(m.data(512)) << 16, bits = bm + 16;
  write16(m.rt, bm, BITMAP16{0, 8, 8, 2, 1, 1, bits});
  for (uint32_t y = 0; y < 8; y++) {
    m.rt.wr8(bits + 2 * y, (y & 1) ? 0xF0 : 0x0F);
    m.rt.wr8(bits + 2 * y + 1, 0);
  }
  uint16_t hbm = uint16_t(api(m, "GDI", "CreateBitmapIndirect", {l16(bm)}));
  Obj16* bo = g.get(hbm, G16::bitmap);
  CHECK(bo && bo->bmp.bpp == 1 && bo->bmp.w == 8 && bo->bmp.h == 8, "CreateBitmapIndirect: an 8x8 monochrome bitmap");
  uint16_t hbr = uint16_t(api(m, "GDI", "CreatePatternBrush", {w16(hbm)}));
  Obj16* br = g.get(hbr, G16::brush);
  uint16_t own = br ? br->pattern : 0;
  CHECK(br && br->style == BS_PATTERN && own && own != hbm && g.get(own, G16::bitmap),
        "CreatePatternBrush: a brush with a bitmap of its own");
  CHECK((api(m, "GDI", "DeleteObject", {w16(hbm)}) & 0xFFFF) == 1 && !g.get(hbm), "the bitmap deleted before the brush is used");
  api(m, "GDI", "SetTextColor", {w16(mdc), l16(RGB(255, 0, 0))});
  api(m, "GDI", "SetBkColor", {w16(mdc), l16(RGB(0, 0, 255))});
  CHECK((api(m, "USER", "FillRect", {w16(mdc), l16(rc), w16(hbr)}) & 0xFFFF) == 1, "FillRect with it");
  CHECK(at(0, 0) == 249 && at(3, 0) == 249 && at(4, 0) == 252 && at(7, 0) == 252 && at(0, 1) == 252 && at(4, 1) == 249 &&
            at(8, 0) == 249 && at(12, 2) == 252 && at(15, 15) == 249,
        "0 bits red (the text colour), 1 bits blue (the background), tiled (%d %d %d %d %d)", at(0, 0), at(4, 0), at(0, 1),
        at(12, 2), at(15, 15));
  CHECK((api(m, "GDI", "DeleteObject", {w16(hbr)}) & 0xFFFF) == 1 && !g.get(own), "DeleteObject takes the brush's copy with it");
  // An 8-bit BITMAP keeps its bits as given (WORD rows).
  write16(m.rt, bm, BITMAP16{0, 4, 2, 4, 1, 8, bits});
  for (uint32_t i = 0; i < 8; i++) m.rt.wr8(bits + i, uint8_t(0x31 + i));
  uint16_t h8 = uint16_t(api(m, "GDI", "CreateBitmapIndirect", {l16(bm)}));
  uint32_t buf = bits + 64;
  CHECK(h8 && api(m, "GDI", "GetBitmapBits", {w16(h8), l16(8), l16(buf)}) == 8 && m.rt.rd8(buf) == 0x31 &&
            m.rt.rd8(buf + 3) == 0x34 && m.rt.rd8(buf + 4) == 0x35 && m.rt.rd8(buf + 7) == 0x38,
        "an 8-bit CreateBitmapIndirect: its bits");
  // A 16x16 colour bitmap, selected into a DC: its top-left 8x8 — a checker
  // of 21h and 22h — alone; the other quarters (23h) never show.
  uint32_t big = bits + 128;
  for (uint32_t y = 0; y < 16; y++) {
    for (uint32_t x = 0; x < 16; x++) m.rt.wr8(big + y * 16 + x, uint8_t(y < 8 && x < 8 ? ((x + y) & 1 ? 0x22 : 0x21) : 0x23));
  }
  uint16_t hbig = uint16_t(api(m, "GDI", "CreateBitmap", {w16(16), w16(16), w16(1), w16(8), l16(big)}));
  uint16_t bdc = uint16_t(api(m, "GDI", "CreateCompatibleDC", {w16(sdc)}));
  api(m, "GDI", "SelectObject", {w16(bdc), w16(hbig)});
  uint16_t hbr2 = uint16_t(api(m, "GDI", "CreatePatternBrush", {w16(hbig)}));
  api(m, "USER", "FillRect", {w16(mdc), l16(rc), w16(hbr2)});
  std::set<int> seen;
  for (int y = 0; y < 16; y++) {
    for (int x = 0; x < 16; x++) seen.insert(at(x, y));
  }
  CHECK(seen == std::set<int>({0x21, 0x22}) && at(0, 0) == 0x21 && at(1, 0) == 0x22 && at(8, 8) == 0x21 && at(9, 8) == 0x22,
        "a 16x16 bitmap: its top-left 8x8 alone (%zu values; %02X %02X %02X)", seen.size(), at(0, 0), at(1, 0), at(8, 8));
  CHECK((api(m, "GDI", "CreatePatternBrush", {w16(0x1234)}) & 0xFFFF) == 0, "no bitmap: 0");
  api(m, "GDI", "DeleteObject", {w16(hbr2)});
  api(m, "GDI", "DeleteDC", {w16(bdc)});
  api(m, "GDI", "DeleteDC", {w16(mdc)});
}

// GetMenu, GetWindowTask, GetNextWindow, EnumChildWindows — with the
// synthetic desktop JAWAS walks (EnumWindows, EnumChildWindows).
void test_window_queries() {
  Machine m;
  Screen screen(64, 48);
  m.rt.attach_display(screen);
  uint16_t saver = user16_saver_window(m.rt);
  uint16_t task = uint16_t(api(m, "KERNEL", "GetCurrentTask", {}));
  CHECK((api(m, "USER", "GetWindowTask", {w16(saver)}) & 0xFFFF) == task && task, "the saver window is this task's");
  CHECK((api(m, "USER", "GetMenu", {w16(saver)}) & 0xFFFF) == 0, "no menu on the saver window");
  CHECK((api(m, "USER", "GetWindowTask", {w16(0x1234)}) & 0xFFFF) == 0, "no window: no task");
  std::vector<uint16_t> seen;
  m.rt.shims().add("TESTCB", 2, "CHILDPROC", Conv16::pascal_, true, 6, [&](Call16& c) {
    seen.push_back(c.w());
    c.l();
    c.ret(1);
  });
  uint32_t cb = m.rt.thunk_far(*m.rt.shims().find_name("TESTCB", "CHILDPROC"));
  uint32_t cls = m.rt.static_bytes("t STATIC", "STATIC");
  auto create = [&](uint32_t style, uint16_t parent, uint16_t menu) {
    return uint16_t(api(m, "USER", "CreateWindow", {l16(cls), l16(cls), l16(style), w16(0), w16(0), w16(8), w16(8),
                                                    w16(parent), w16(menu), w16(0), l16(0)}));
  };
  uint16_t top = create(WS_POPUP | WS_VISIBLE, 0, 0x0F44);
  uint16_t k1 = create(WS_CHILD | WS_VISIBLE, top, 1), k11 = create(WS_CHILD, k1, 2), k2 = create(WS_CHILD, top, 3);
  CHECK((api(m, "USER", "GetMenu", {w16(top)}) & 0xFFFF) == 0x0F44 && (api(m, "USER", "GetMenu", {w16(k1)}) & 0xFFFF) == 0,
        "a top-level window's menu; none for a child");
  CHECK((api(m, "USER", "EnumChildWindows", {w16(top), l16(cb), l16(0)}) & 0xFFFF) == 1 && seen.size() == 3 &&
            seen[0] == k1 && seen[1] == k11 && seen[2] == k2,
        "EnumChildWindows: each child, then its own (%zu)", seen.size());
  seen.clear();
  CHECK((api(m, "USER", "EnumChildWindows", {w16(0), l16(cb), l16(0)}) & 0xFFFF) == 0 && seen.empty(),
        "EnumChildWindows(NULL): nothing, FALSE");
  // The synthetic desktop: Program Manager is another task's and has a menu bar.
  m.rt.shims().add("TESTCB", 3, "TOPPROC", Conv16::pascal_, true, 6, [&](Call16& c) {
    c.w();
    c.l();
    c.ret(1);
  });
  api(m, "USER", "EnumWindows", {l16(m.rt.thunk_far(*m.rt.shims().find_name("TESTCB", "TOPPROC"))), l16(0)});
  uint16_t pm = uint16_t(api(m, "USER", "FindWindow", {l16(m.rt.static_bytes("t Progman", "Progman")), l16(0)}));
  uint16_t pm_task = uint16_t(api(m, "USER", "GetWindowTask", {w16(pm)}));
  CHECK(pm && pm_task && pm_task != task && (api(m, "KERNEL", "IsTask", {w16(pm_task)}) & 0xFFFF) &&
            (api(m, "KERNEL", "IsTask", {w16(task)}) & 0xFFFF),
        "Program Manager: another (valid) task");
  CHECK((api(m, "USER", "GetMenu", {w16(pm)}) & 0xFFFF) != 0, "Program Manager has a menu bar");
  // GetNextWindow: GetWindow's two sibling steps.
  uint16_t next = uint16_t(api(m, "USER", "GetNextWindow", {w16(saver), w16(GW_HWNDNEXT)}));
  CHECK(next && next == uint16_t(api(m, "USER", "GetWindow", {w16(saver), w16(GW_HWNDNEXT)})) &&
            uint16_t(api(m, "USER", "GetNextWindow", {w16(next), w16(GW_HWNDPREV)})) == saver,
        "GetNextWindow NEXT/PREV");
  CHECK((api(m, "USER", "GetNextWindow", {w16(saver), w16(GW_CHILD)}) & 0xFFFF) == 0, "GetNextWindow takes no GW_CHILD");
  // EnumChildWindows of the desktop: the top-level windows and theirs.
  seen.clear();
  uint16_t desk = uint16_t(api(m, "USER", "GetDesktopWindow", {}));
  api(m, "USER", "EnumChildWindows", {w16(desk), l16(cb), l16(0)});
  std::vector<uint16_t> want = {saver, top, k1, k11, k2, pm};
  CHECK(seen == want, "the desktop's children: %zu windows", seen.size());
}

// wsprintf: a %s far pointer to nothing reads as "" (SWTEXT's configure
// dialog passes a near pointer).
void test_wsprintf_bad_pointer() {
  Machine m;
  uint16_t ds = m.data(256);
  uint32_t buf = uint32_t(ds) << 16, fmt = m.rt.static_bytes("t fmt", "[%s|%d]");
  // cdecl: the last argument is pushed first.
  uint16_t n = uint16_t(api(m, "USER", "_wsprintf", {w16(7), l16(0xC0000E24), l16(fmt), l16(buf)}));
  CHECK(m.rt.read_str(buf) == "[|7]" && n == 4, "an unreadable %%s: \"\" (\"%s\", %u)", m.rt.read_str(buf).c_str(), n);
  uint32_t ok = m.rt.static_bytes("t ok", "ok");
  api(m, "USER", "_wsprintf", {w16(8), l16(ok), l16(fmt), l16(buf)});
  CHECK(m.rt.read_str(buf) == "[ok|8]", "a good one still reads (%s)", m.rt.read_str(buf).c_str());
}

// ChooseFont: cancelled in the saver; in configure mode a real dialog, which
// a hidden (scripted) run cannot answer: cancelled, counted as shown.
void test_choosefont() {
  Machine m;
  Screen screen(64, 48);
  m.rt.attach_display(screen);
  uint16_t ds = m.data(256);
  uint32_t cf = uint32_t(ds) << 16;
  m.rt.wr32(cf, 0x2E);
  m.rt.wr32(cf + 8, cf + 0x80);  // lpLogFont
  m.rt.wr32(cf + 0x0E, 0x2041);  // CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_LIMITSIZE (SWTEXT's)
  m.rt.write_str(cf + 0x80 + 18, "Arial", 32);
  CHECK((api(m, "COMMDLG", "ChooseFont", {l16(cf)}) & 0xFFFF) == 0, "the saver: cancelled");
  win32::ConfigScript script;
  script.set_hidden(true);
  Configure16 cfg;
  cfg.script = &script;
  enable_real_dialogs16(m.rt, &cfg);
  CHECK((api(m, "COMMDLG", "ChooseFont", {l16(cf)}) & 0xFFFF) == 0 && cfg.shown == 1,
        "configure mode, hidden: cancelled (shown %d)", cfg.shown);
  CHECK(m.rt.read_str(cf + 0x80 + 18) == "Arial", "the LOGFONT untouched");
}

// WING.1008 takes 8 argument bytes (WING.DLL's retf 8), not the table's 6:
// a call leaves the caller's stack balanced.
void test_wing_signature() {
  Machine m;
  Shim16Entry* e = m.rt.shims().find_name("WING", "WinGCreateHalftoneBrush");
  CHECK(e && e->arg_bytes == 8 && e->conv == Conv16::pascal_ && e->ordinal == 1008, "WinGCreateHalftoneBrush: 8 bytes");
  // push hdc; push colour (dword); push dither; lcall; retf
  uint32_t fp = e ? m.rt.thunk_far(*e) : 0;
  std::vector<uint8_t> code = {0x6A, 0x01, 0x68, 0x00, 0x00, 0x68, 0xFF, 0x00, 0x6A, 0x02};
  append(code, {0x9A, uint8_t(fp), uint8_t(fp >> 8), uint8_t(fp >> 16), uint8_t(fp >> 24), 0xCB});
  uint16_t sp0 = m.rt.cpu().registers().r_sp();
  m.rt.call_far(uint32_t(m.code(code)) << 16, {});
  CHECK(m.rt.last_sp_after() == sp0, "the stack is balanced after the call (%04X vs %04X)", m.rt.last_sp_after(), sp0);
}

// GetModuleHandle finds the system DLLs Windows 95 always has loaded before
// anything imports them (INTRMLIB's and ANTSW's LibEntry ask for MMSYSTEM).
void test_resident_modules() {
  Machine m;
  uint32_t mm = api(m, "KERNEL", "GetModuleHandle", {l16(m.rt.static_bytes("t mm", "MMSYSTEM"))});
  CHECK((mm & 0xFFFF) != 0, "GetModuleHandle(\"MMSYSTEM\") before any import (%08X)", mm);
  CHECK(api(m, "KERNEL", "GetModuleHandle", {l16(m.rt.static_bytes("t mmd", "mmsystem.dll"))}) == mm, "by file name too");
  CHECK((api(m, "KERNEL", "GetModuleHandle", {l16(m.rt.static_bytes("t snd", "SOUND"))}) & 0xFFFF) != 0, "SOUND");
  CHECK((api(m, "KERNEL", "GetModuleHandle", {l16(m.rt.static_bytes("t fp", "WIN87EM"))}) & 0xFFFF) == 0,
        "WIN87EM is loaded on demand only: 0");
  CHECK(api(m, "KERNEL", "GetProcAddress", {w16(uint16_t(mm)), l16(m.rt.static_bytes("t wo", "waveOutGetNumDevs"))}) != 0,
        "GetProcAddress into it");
}

// INT 2Fh AX=1684h (a VxD's API entry point): none, ES:DI = 0:0, as Windows
// answered for a VxD that is not there (INTRMLIB's LibEntry asks for three).
void test_int2f_vxd() {
  Machine m;
  uint16_t ds = m.data(16);
  // mov ax,ds_sel; mov es,ax; mov di,5555h; mov bx,0028h; mov ax,1684h; int 2Fh; mov ax,es; mov dx,di; retf
  std::vector<uint8_t> code = {0xB8, uint8_t(ds), uint8_t(ds >> 8), 0x8E, 0xC0, 0xBF, 0x55, 0x55, 0xBB, 0x28, 0x00,
                               0xB8, 0x84, 0x16, 0xCD, 0x2F, 0x8C, 0xC0, 0x89, 0xFA, 0xCB};
  uint32_t r = m.rt.call_far(uint32_t(m.code(code)) << 16, {});
  CHECK(r == 0, "ES:DI = 0:0 (%08X)", r);
}

// user16_dispatch_guest (INTERMIS's loop between saver calls): the guest's
// own posted messages and due timers go to their procedures; task messages
// are counted; host-posted and input messages stay for the lane.
void test_dispatch_guest() {
  Machine m;
  m.clock.begin_frame();  // frame 0
  Screen screen(64, 48);
  m.rt.attach_display(screen);
  InputState in;
  m.rt.set_input(&in);
  struct Got {
    uint16_t hwnd, msg, wp;
    uint32_t lp;
  };
  std::vector<Got> got, timer_calls;
  m.rt.shims().add("TESTWP", 1, "PUMPPROC", Conv16::pascal_, false, 10, [&](Call16& c) {
    Got g{c.w(), c.w(), c.w(), c.l()};
    if (g.msg != WM_CREATE) got.push_back(g);
    c.ret32(0);
  });
  m.rt.shims().add("TESTWP", 2, "TIMERPROC", Conv16::pascal_, true, 10, [&](Call16& c) {
    Got g{c.w(), c.w(), c.w(), c.l()};
    timer_calls.push_back(g);
    c.ret(0);
  });
  uint16_t ds = m.data(256);
  uint32_t d = uint32_t(ds) << 16;
  m.rt.write_str(d, "PUMPCLS", 16);
  m.rt.wr32(d + 0x20 + 2, m.rt.thunk_far(*m.rt.shims().find_name("TESTWP", "PUMPPROC")));
  m.rt.wr32(d + 0x20 + 22, d);
  api(m, "USER", "RegisterClass", {l16(d + 0x20)});
  uint16_t hwnd = uint16_t(api(m, "USER", "CreateWindow", {l16(d), l16(d), l16(WS_POPUP), w16(0), w16(0), w16(8), w16(8),
                                                             w16(0), w16(0), w16(0), l16(0)}));
  uint16_t task = uint16_t(api(m, "KERNEL", "GetCurrentTask", {}));
  api(m, "USER", "PostMessage", {w16(hwnd), w16(WM_USER + 1), w16(2), l16(3)});
  user16_post_host(m.rt, hwnd, 0x3B9, 5, 6);  // MM_WOM_DONE: the lane's (user16_dispatch_host)
  api(m, "USER", "PostAppMessage", {w16(task), w16(WM_MOUSEMOVE), w16(0xFFFF), l16(0)});
  user16_post_input(m.rt, WM_KEYDOWN, 'A', key_lparam('A', true, false), 9);
  api(m, "USER", "PostMessage", {w16(0x7770), w16(WM_USER + 7), w16(0), l16(0)});
  api(m, "USER", "PostMessage", {w16(hwnd), w16(WM_USER + 2), w16(4), l16(5)});
  int n = user16_dispatch_guest(m.rt);
  CHECK(n == 4, "four messages handled (%d)", n);
  CHECK(got.size() == 2 && got[0].msg == WM_USER + 1 && got[0].wp == 2 && got[0].lp == 3 && got[1].msg == WM_USER + 2,
        "the window's own messages, in order (%zu)", got.size());
  uint32_t msg = d + 0x80;
  CHECK((api(m, "USER", "PeekMessage", {l16(msg), w16(0), w16(WM_KEYFIRST), w16(WM_KEYLAST), w16(PM_NOREMOVE)}) & 0xFFFF) &&
            m.rt.rd16(msg + 2) == WM_KEYDOWN,
        "the tagged input is still there");
  got.clear();
  CHECK(user16_dispatch_host(m.rt) == 1 && got.size() == 1 && got[0].msg == 0x3B9, "the host's message is still the host's");
  StepReport16 r = user16_end_step(m.rt);
  CHECK(r.task_posts == 1 && r.last_task_msg == WM_MOUSEMOVE, "the task message counted (%u, %04X)", r.task_posts,
        r.last_task_msg);
  // Timers: WM_TIMER to the window, the TIMERPROC's call, once each when due.
  got.clear();
  api(m, "USER", "SetTimer", {w16(hwnd), w16(5), w16(10), l16(0)});
  uint16_t tid = uint16_t(api(m, "USER", "SetTimer", {w16(0), w16(0), w16(10),
                                                      l16(m.rt.thunk_far(*m.rt.shims().find_name("TESTWP", "TIMERPROC")))}));
  CHECK(user16_dispatch_guest(m.rt) == 0 && got.empty(), "not due yet");
  m.clock.begin_frame();  // frame 1: 16.7 ms on
  CHECK(user16_dispatch_guest(m.rt) == 2, "both timers fire");
  CHECK(got.size() == 1 && got[0].msg == WM_TIMER && got[0].wp == 5 && timer_calls.size() == 1 &&
            timer_calls[0].msg == WM_TIMER && timer_calls[0].wp == tid,
        "WM_TIMER 5 to the window, the TIMERPROC for timer %u", tid);
  CHECK(user16_dispatch_guest(m.rt) == 0, "and not again until due");
  // A due timer that an earlier callback of the same pump sets again (a 5 s
  // timeout restarted: SetTimer on its hwnd and id resets it, as on Windows)
  // waits for its new time, as GetMessage's own timer check has it.
  api(m, "USER", "KillTimer", {w16(hwnd), w16(5)});
  api(m, "USER", "KillTimer", {w16(0), w16(tid)});
  int resets = 0;
  m.rt.shims().add("TESTWP", 3, "RESETPROC", Conv16::pascal_, true, 10, [&](Call16& c) {
    if (!resets++) api(m, "USER", "SetTimer", {w16(hwnd), w16(7), w16(5000), l16(0)});
    c.ret(0);
  });
  api(m, "USER", "SetTimer",
      {w16(hwnd), w16(6), w16(10), l16(m.rt.thunk_far(*m.rt.shims().find_name("TESTWP", "RESETPROC")))});
  api(m, "USER", "SetTimer", {w16(hwnd), w16(7), w16(10), l16(0)});
  got.clear();
  auto timer7 = [&] {
    int k = 0;
    for (const Got& x : got) k += x.msg == WM_TIMER && x.wp == 7;
    return k;
  };
  m.clock.begin_frame();  // both due
  CHECK(user16_dispatch_guest(m.rt) == 1 && resets == 1 && timer7() == 0,
        "timer 6's procedure ran and set timer 7 again: 7 does not fire (%d)", timer7());
  for (int f = 0; f < 60; f++) {
    m.clock.begin_frame();
    user16_dispatch_guest(m.rt);
  }
  CHECK(timer7() == 0, "nor within a second");
  for (int f = 0; f < 250; f++) {
    m.clock.begin_frame();
    user16_dispatch_guest(m.rt);
  }
  CHECK(timer7() == 1, "it fires at its new time, 5 s on (%d)", timer7());
  m.rt.set_input(nullptr);
}

// seed_intermission: the SYSTEM.INI / SWSE.INI / ANTSW.INI seeds, read as
// seed ⊕ file, never written out.
void test_intermission_seeds() {
  char base[MAX_PATH];
  GetTempPathA(MAX_PATH, base);
  std::string upper = std::string(base) + "adw_win16_imseed_" + std::to_string(GetCurrentProcessId());
  {
    Machine m;
    m.rt.vfs().mount_overlay("C:\\WINDOWS", "", upper);
    IntermissionSeeds s;
    s.volume = 150;
    s.swse_gdi = true;
    seed_intermission(m.rt, s);
    win32::IniStore& ini = profiles16(m.rt);
    auto get = [&](const char* f, const char* sec, const char* k) {
      return ini.get(std::string("C:\\WINDOWS\\") + f, sec, k).value_or("(none)");
    };
    CHECK(get("SYSTEM.INI", "boot", "display.drv") == "pnpdrvr.drv", "SYSTEM.INI [boot] display.drv");
    CHECK(get("SWSE.INI", "technology", "display.drv") == "pnpdrvr.drv" && get("SWSE.INI", "technology", "WinGFound") == "1" &&
              get("SWSE.INI", "technology", "DibBlit") == "GDI",
          "SWSE.INI [technology]: GDI");
    CHECK(get("ANTSW.INI", "Intermission", "Volume") == "100" && get("ANTSW.INI", "Intermission", "Saver Path") == "C:\\AFTERDRK",
          "ANTSW.INI [Intermission]: Volume clamped to 100, Saver Path the guest directory");
    // The guest reads them through its own calls; its writes never carry them.
    uint16_t ds = m.data(128);
    uint32_t buf = uint32_t(ds) << 16;
    uint32_t tech = m.rt.static_bytes("t tech", "technology"), blit = m.rt.static_bytes("t blit", "DibBlit"),
             swse = m.rt.static_bytes("t swse", "SWSE.INI"), def = m.rt.static_bytes("t def", "WinG");
    api(m, "KERNEL", "GetPrivateProfileString", {l16(tech), l16(blit), l16(def), l16(buf), w16(16), l16(swse)});
    CHECK(m.rt.read_str(buf) == "GDI", "GetPrivateProfileString sees the seed (%s)", m.rt.read_str(buf).c_str());
    uint32_t sec = m.rt.static_bytes("t sec", "Darth Vader"), key = m.rt.static_bytes("t key", "Delay"),
             val = m.rt.static_bytes("t val", "30");
    api(m, "KERNEL", "WritePrivateProfileString", {l16(sec), l16(key), l16(val), l16(swse)});
  }
  std::string ini;
  if (FILE* f = fopen((upper + "\\SWSE.INI").c_str(), "rb")) {
    char b[256] = {};
    fread(b, 1, 255, f);
    fclose(f);
    ini = b;
  }
  CHECK(ini.find("Delay=30") != std::string::npos && ini.find("technology") == std::string::npos,
        "the written SWSE.INI has the module's key and no seeds (%s)", ini.c_str());
  DeleteFileA((upper + "\\SWSE.INI").c_str());
  RemoveDirectoryA(upper.c_str());
  Machine m2;
  seed_intermission(m2.rt, IntermissionSeeds{});
  CHECK(!profiles16(m2.rt).get("C:\\WINDOWS\\SWSE.INI", "technology", "DibBlit") &&
            profiles16(m2.rt).get("C:\\WINDOWS\\ANTSW.INI", "Intermission", "Volume").value_or("") == "0",
        "without SWSE.DLL: no [technology]; sound off: Volume 0");
}

// After Dark 2.0's profile seeds (seed_after_dark2): AD_PREFS.INI [After Dark]
// Path and [Sound] SoundDriver under the empty virtual file, read through the
// guest's own calls as AD_MOD.DLL and AD_SND make them; a module's write
// (AD_SND's [Sound] Mute) never carries them.
void test_after_dark2_seeds() {
  char base[MAX_PATH];
  GetTempPathA(MAX_PATH, base);
  std::string upper = std::string(base) + "adw_win16_ad2seed_" + std::to_string(GetCurrentProcessId());
  {
    Machine m;
    m.rt.vfs().mount_overlay("C:\\WINDOWS", "", upper);
    CHECK(!profiles16(m.rt).get("C:\\WINDOWS\\AD_PREFS.INI", "After Dark", "Path"), "no Path before the seeds");
    seed_after_dark2(m.rt);
    win32::IniStore& ini = profiles16(m.rt);
    CHECK(ini.get("C:\\WINDOWS\\AD_PREFS.INI", "After Dark", "Path").value_or("") == "C:\\AFTERDRK\\" &&
              ini.get("C:\\WINDOWS\\AD_PREFS.INI", "Sound", "SoundDriver").value_or("") == "AD_MME.DRV",
          "[After Dark] Path=C:\\AFTERDRK\\, [Sound] SoundDriver=AD_MME.DRV");
    uint16_t ds = m.data(256);
    uint32_t buf = uint32_t(ds) << 16;
    uint32_t ad = m.rt.static_bytes("t ad", "After Dark"), path = m.rt.static_bytes("t path", "PATH"),
             empty = m.rt.static_bytes("t empty", ""), prefs = m.rt.static_bytes("t prefs", "ad_prefs.ini"),
             sound = m.rt.static_bytes("t sound", "Sound"), drv = m.rt.static_bytes("t drv", "SoundDriver"),
             mute = m.rt.static_bytes("t mute", "Mute"), no = m.rt.static_bytes("t no", "NO");
    api(m, "KERNEL", "GetPrivateProfileString", {l16(ad), l16(path), l16(empty), l16(buf), w16(0xA0), l16(prefs)});
    CHECK(m.rt.read_str(buf) == "C:\\AFTERDRK\\", "AD_MOD's read: %s", m.rt.read_str(buf).c_str());
    CHECK((api(m, "KERNEL", "WritePrivateProfileString", {l16(sound), l16(mute), l16(no), l16(prefs)}) & 0xFFFF) == 1,
          "AD_SND's [Sound] Mute write");
    api(m, "KERNEL", "GetPrivateProfileString", {l16(sound), l16(drv), l16(empty), l16(buf), w16(0xA0), l16(prefs)});
    CHECK(m.rt.read_str(buf) == "AD_MME.DRV", "the seed still shows under the written file (%s)", m.rt.read_str(buf).c_str());
  }
  std::string ini;
  if (FILE* f = fopen((upper + "\\AD_PREFS.INI").c_str(), "rb")) {
    char b[256] = {};
    fread(b, 1, 255, f);
    fclose(f);
    ini = b;
  }
  CHECK(ini.find("Mute=NO") != std::string::npos && ini.find("Path") == std::string::npos &&
            ini.find("SoundDriver") == std::string::npos,
        "the written AD_PREFS.INI has the module's key and no seeds (%s)", ini.c_str());
  DeleteFileA((upper + "\\AD_PREFS.INI").c_str());
  RemoveDirectoryA(upper.c_str());
}

// After Dark 3.x's profile seeds (seed_after_dark3): the keys ADW30.EXE wrote
// into AD_PREFS.INI at every start — [After Dark] Path without a backslash,
// [Sound] SoundDriver — read through the guest's own calls as ADXPL100 makes
// them (4:00FB); a module's write never carries them.
void test_after_dark3_seeds() {
  char base[MAX_PATH];
  GetTempPathA(MAX_PATH, base);
  std::string upper = std::string(base) + "adw_win16_ad3seed_" + std::to_string(GetCurrentProcessId());
  {
    Machine m;
    m.rt.vfs().mount_overlay("C:\\WINDOWS", "", upper);
    CHECK(!profiles16(m.rt).get("C:\\WINDOWS\\AD_PREFS.INI", "After Dark", "Path"), "no Path before the seeds");
    seed_after_dark3(m.rt);
    win32::IniStore& ini = profiles16(m.rt);
    CHECK(ini.get("C:\\WINDOWS\\AD_PREFS.INI", "After Dark", "Path").value_or("") == "C:\\AFTERDRK" &&
              ini.get("C:\\WINDOWS\\AD_PREFS.INI", "Sound", "SoundDriver").value_or("") == "AD_MME.DRV",
          "[After Dark] Path=C:\\AFTERDRK (ADW30's spelling), [Sound] SoundDriver=AD_MME.DRV");
    uint16_t ds = m.data(256);
    uint32_t buf = uint32_t(ds) << 16;
    uint32_t ad = m.rt.static_bytes("t ad", "After Dark"), path = m.rt.static_bytes("t path", "PATH"),
             empty = m.rt.static_bytes("t empty", ""), prefs = m.rt.static_bytes("t prefs", "ad_prefs.ini"),
             sound = m.rt.static_bytes("t sound", "Sound"), mute = m.rt.static_bytes("t mute", "Mute"),
             yes = m.rt.static_bytes("t yes", "1");
    uint32_t n =
        api(m, "KERNEL", "GetPrivateProfileString", {l16(ad), l16(path), l16(empty), l16(buf), w16(0xA0), l16(prefs)});
    CHECK((n & 0xFFFF) == 11 && m.rt.read_str(buf) == "C:\\AFTERDRK", "ADXPL100's read: %s (%u)",
          m.rt.read_str(buf).c_str(), n & 0xFFFF);
    CHECK((api(m, "KERNEL", "WritePrivateProfileString", {l16(sound), l16(mute), l16(yes), l16(prefs)}) & 0xFFFF) == 1,
          "a module's [Sound] Mute write");
  }
  std::string ini;
  if (FILE* f = fopen((upper + "\\AD_PREFS.INI").c_str(), "rb")) {
    char b[256] = {};
    fread(b, 1, 255, f);
    fclose(f);
    ini = b;
  }
  CHECK(ini.find("Mute=1") != std::string::npos && ini.find("Path") == std::string::npos &&
            ini.find("SoundDriver") == std::string::npos,
        "the written AD_PREFS.INI has the module's key and no seeds (%s)", ini.c_str());
  DeleteFileA((upper + "\\AD_PREFS.INI").c_str());
  RemoveDirectoryA(upper.c_str());
}

// ---- KERNEL: heap spaces, code handles, selectors, the freed-selector rule; TOOLHELP's heap walk -----------

// Guest code for the tests below: 16-bit instructions.
std::vector<uint8_t> mov_ax(uint16_t v) { return {0xB8, uint8_t(v), uint8_t(v >> 8)}; }
std::vector<uint8_t> push_imm(uint16_t v) { return {0x68, uint8_t(v), uint8_t(v >> 8)}; }
constexpr uint8_t kMovEsAx[] = {0x8E, 0xC0}, kMovDsAx[] = {0x8E, 0xD8}, kMovFsAx[] = {0x8E, 0xE0},
                  kMovGsAx[] = {0x8E, 0xE8}, kMovAxEs[] = {0x8C, 0xC0}, kMovAxDs[] = {0x8C, 0xD8};
std::vector<uint8_t> ops(std::initializer_list<std::vector<uint8_t>> parts) {
  std::vector<uint8_t> v;
  for (const auto& p : parts) append(v, p);
  return v;
}
std::vector<uint8_t> op(const uint8_t (&b)[2]) { return {b[0], b[1]}; }

// Runs guest code (a far procedure) and returns DX:AX; *fault (when given)
// says whether it ended in a guest fault instead.
uint32_t run_guest(Machine& m, const std::vector<uint8_t>& code, bool* fault = nullptr) {
  if (fault) *fault = false;
  try {
    return m.rt.call_far(uint32_t(m.code(code)) << 16, {});
  } catch (const GuestError16& e) {
    if (!fault || e.kind() != GuestError16::Kind::fault) throw;
    *fault = true;
    return 0;
  }
}

// KERNEL.138 GetHeapSpaces (MARVEL.AD's INITIALIZE divides by the size and
// refuses below 20% free) and KERNEL.93 GetCodeHandle (DECO.DLL locks its
// decoder with LockSegment(GetCodeHandle(fn))).
void test_heap_spaces_code_handle() {
  Machine m;
  for (const char* name : {"USER", "GDI"}) {
    uint16_t h = uint16_t(api(m, "KERNEL", "GetModuleHandle", {l16(m.rt.static_bytes(name, name))}));
    uint32_t hs = api(m, "KERNEL", "GetHeapSpaces", {w16(h)});
    CHECK(h && hs == 0xFA00E100 && (hs & 0xFFFF) * 100 / (hs >> 16) == 90,
          "GetHeapSpaces(%s): %u of %u bytes free, the 90%% GetFreeSystemResources reports", name, hs & 0xFFFF, hs >> 16);
  }
  CHECK(api(m, "KERNEL", "GetHeapSpaces", {w16(0)}) == 0 && api(m, "KERNEL", "GetHeapSpaces", {w16(m.data(16))}) == 0,
        "GetHeapSpaces of no module: 0");
  char tmp[MAX_PATH], dir[MAX_PATH];
  GetTempPathA(MAX_PATH, dir);
  snprintf(tmp, sizeof(tmp), "%sadw_win16_codehandle_%lu.dll", dir, GetCurrentProcessId());
  std::string img = build_ne();
  FILE* fh = fopen(tmp, "wb");
  fwrite(img.data(), 1, img.size(), fh);
  fclose(fh);
  uint16_t err = 0;
  Module16* mod = m.rt.modules().load_host(tmp, &err);
  CHECK(mod != nullptr, "the synthetic NE DLL loads (error %u)", err);
  if (mod) {
    uint32_t fn = m.rt.modules().proc_address(mod, "func");
    uint16_t sel = uint16_t(fn >> 16);
    uint32_t ch = api(m, "KERNEL", "GetCodeHandle", {l16(fn)});
    CHECK(ch && ch == m.rt.global().handle(sel) && (ch >> 16) == sel && m.rt.modules().segment_index(*mod, uint16_t(ch)),
          "GetCodeHandle(func): DX:AX = %04X:%04X, its segment's selector and handle", ch >> 16, ch & 0xFFFF);
    CHECK(uint16_t(api(m, "KERNEL", "LockSegment", {w16(uint16_t(ch))})) != 0, "LockSegment(GetCodeHandle(func))");
    m.rt.modules().free(mod);
  }
  DeleteFileA(tmp);
  CHECK(api(m, "KERNEL", "GetCodeHandle", {l16(uint32_t(m.code({0xCB})) << 16)}) == 0 &&
            api(m, "KERNEL", "GetCodeHandle", {l16(0)}) == 0,
        "GetCodeHandle of code that is no module's: 0");
}

// The selector calls (AllocSelector, FreeSelector, AllocCStoDSAlias,
// AllocDStoCSAlias, Get/SetSelectorBase, Get/SetSelectorLimit), limited to
// the selectors they make themselves.
void test_selector_calls() {
  Machine m;
  Ldt& ldt = m.rt.ldt();
  auto k = [&](const char* fn, std::initializer_list<Arg16> args) { return api(m, "KERNEL", fn, args); };
  auto k16 = [&](const char* fn, std::initializer_list<Arg16> args) { return uint16_t(api(m, "KERNEL", fn, args)); };
  uint16_t blk = m.data(0x100);  // a global block: not the selector calls' own
  uint32_t base = ldt.base_of(blk);
  m.rt.wr16((uint32_t(blk) << 16) | 0x20, 0x5A5A);
  // AllocSelector: an exact copy.
  uint16_t s = k16("AllocSelector", {w16(blk)});
  CHECK(s && s != blk && (s & 7) == 7 && ldt.base_of(s) == base && ldt.limit_of(s) == ldt.limit_of(blk) &&
            ldt.get(s)->present && !ldt.get(s)->code && ldt.get(s)->readable_or_writable,
        "AllocSelector(block): a copy of its descriptor (%04X, base %X, limit %X)", s, ldt.base_of(s), ldt.limit_of(s));
  m.rt.wr16((uint32_t(s) << 16) | 0x10, 0xBEEF);
  CHECK(m.rt.rd16((uint32_t(blk) << 16) | 0x10) == 0xBEEF, "a write through the copy lands in the block");
  // Get/SetSelectorBase, Get/SetSelectorLimit on it.
  CHECK(k16("SetSelectorBase", {w16(s), l16(base + 0x10)}) == s && k("GetSelectorBase", {w16(s)}) == base + 0x10 &&
            m.rt.rd16(uint32_t(s) << 16) == 0xBEEF && m.rt.rd16((uint32_t(s) << 16) | 0x10) == 0x5A5A,
        "SetSelectorBase: the copy now starts 16 bytes on, and returns the selector");
  CHECK(k16("SetSelectorLimit", {w16(s), l16(0x1F)}) == 0 && k("GetSelectorLimit", {w16(s)}) == 0x1F,
        "SetSelectorLimit returns 0 (always); GetSelectorLimit reads the new limit");
  k16("SetSelectorLimit", {w16(s), l16(0x12345678)});
  CHECK(ldt.limit_of(s) == 0x45678, "a limit's 20 bits, byte-granular, as KRNL386 wrote them (%X)", ldt.limit_of(s));
  // A segment register holding it sees a new base at once:
  // mov ax,s; mov es,ax; push ax; push hi; push lo; lcall SetSelectorBase; mov ax,es:[0]; retf
  uint32_t nb = base + 0x20;
  uint32_t r = run_guest(m, ops({mov_ax(s), op(kMovEsAx), {0x50}, push_imm(uint16_t(nb >> 16)), push_imm(uint16_t(nb)),
                                 m.lcall("KERNEL", "SetSelectorBase"), {0x26, 0xA1, 0x00, 0x00, 0xCB}}));
  CHECK((r & 0xFFFF) == 0x5A5A, "ES holding the selector reads through the new base at once (%04X)", r & 0xFFFF);
  // The Sets leave a selector the calls did not make as it is; the Gets read any.
  CHECK(k16("SetSelectorBase", {w16(blk), l16(0x1234)}) == 0 && ldt.base_of(blk) == base, "SetSelectorBase(block) refused");
  k16("SetSelectorLimit", {w16(blk), l16(7)});
  CHECK(ldt.limit_of(blk) == 0xFF, "SetSelectorLimit(block) refused (%X)", ldt.limit_of(blk));
  CHECK(k("GetSelectorBase", {w16(blk)}) == base && k("GetSelectorLimit", {w16(blk)}) == 0xFF &&
            k("GetSelectorBase", {w16(0)}) == 0 && k("GetSelectorLimit", {w16(0x1234)}) == 0,
        "the Gets read any selector, 0 for one that is no good");
  // AllocSelector(0), or of a selector that is no good: one uninitialized
  // selector, not present, so a load of it faults.
  uint16_t raw = k16("AllocSelector", {w16(0)}), raw2 = k16("AllocSelector", {w16(0x7FF7)});
  CHECK(raw && raw2 && ldt.in_use(raw) && !ldt.get(raw)->present && !ldt.get(raw2)->present,
        "AllocSelector(0) and of a bad selector: uninitialized (not present) selectors");
  bool fault = false;
  run_guest(m, ops({mov_ax(raw), op(kMovEsAx), {0xCB}}), &fault);
  CHECK(fault, "loading an uninitialized selector faults");
  // A copy of a huge block's selector: one tile per 64 KiB, as the block's.
  uint16_t big = m.data(0x28000);
  uint16_t t = k16("AllocSelector", {w16(big)});
  const uint16_t t1 = uint16_t(t + Ldt::kAhIncr), t2 = uint16_t(t + 2 * Ldt::kAhIncr);
  CHECK(t && ldt.base_of(t) == ldt.base_of(big) && ldt.base_of(t1) == ldt.base_of(big) + 0x10000 &&
            ldt.base_of(t2) == ldt.base_of(big) + 0x20000 && ldt.limit_of(t) == ldt.limit_of(big) &&
            ldt.limit_of(t2) == ldt.limit_of(uint16_t(big + 2 * Ldt::kAhIncr)),
        "AllocSelector(160 KiB block): three tiles, 64 KiB apart, limits running to the end");
  CHECK(k16("FreeSelector", {w16(t1)}) == t1 && ldt.in_use(t1), "a tile alone: refused");
  CHECK(k16("SetSelectorBase", {w16(t1), l16(ldt.base_of(t1))}) == t1, "SetSelectorBase of a tile of theirs: allowed");
  CHECK(k16("FreeSelector", {w16(t)}) == 0 && !ldt.in_use(t) && !ldt.in_use(t1) && !ldt.in_use(t2),
        "FreeSelector frees every tile");
  // AllocCStoDSAlias: a writable data selector over a code segment; code
  // written through it runs (the CPU reads live memory).
  uint16_t cs = m.code({0x90, 0x90, 0x90, 0x90, 0xCB});
  uint16_t alias = k16("AllocCStoDSAlias", {w16(cs)});
  CHECK(alias && !ldt.get(alias)->code && ldt.get(alias)->readable_or_writable && ldt.base_of(alias) == ldt.base_of(cs) &&
            ldt.limit_of(alias) == ldt.limit_of(cs),
        "AllocCStoDSAlias: a data selector over the code segment");
  const uint8_t mov_ret[] = {0xB8, 0x34, 0x12, 0xCB};  // mov ax,1234h; retf
  m.rt.write_bytes(uint32_t(alias) << 16, mov_ret, sizeof(mov_ret));
  CHECK((m.rt.call_far(uint32_t(cs) << 16, {}) & 0xFFFF) == 0x1234, "code written through the alias runs");
  // AllocDStoCSAlias: code built in a data block runs through a code selector.
  uint16_t data = m.data(16);
  const uint8_t mov_ret2[] = {0xB8, 0x78, 0x56, 0xCB};
  m.rt.write_bytes(uint32_t(data) << 16, mov_ret2, sizeof(mov_ret2));
  uint16_t code = k16("AllocDStoCSAlias", {w16(data)});
  CHECK(code && ldt.get(code)->code && ldt.base_of(code) == ldt.base_of(data) &&
            (m.rt.call_far(uint32_t(code) << 16, {}) & 0xFFFF) == 0x5678,
        "AllocDStoCSAlias: the data block's code runs through it");
  CHECK(k16("AllocCStoDSAlias", {w16(0x7FF7)}) == 0 && k16("AllocDStoCSAlias", {w16(0)}) == 0, "an alias of no selector: 0");
  // FreeSelector: its own selectors only.
  CHECK(k16("FreeSelector", {w16(s)}) == 0 && !ldt.in_use(s), "FreeSelector(copy): 0, freed");
  CHECK(k16("FreeSelector", {w16(s)}) == s, "the same selector again: refused");
  CHECK(k16("FreeSelector", {w16(alias)}) == 0 && k16("FreeSelector", {w16(code)}) == 0 &&
            k16("FreeSelector", {w16(raw)}) == 0,
        "the aliases and an uninitialized selector are the caller's to free");
  uint16_t sys = m.rt.sys_sel();
  CHECK(k16("FreeSelector", {w16(blk)}) == blk && k16("FreeSelector", {w16(cs)}) == cs &&
            k16("FreeSelector", {w16(sys)}) == sys && k16("FreeSelector", {w16(0x40)}) == 0x40 &&
            k16("FreeSelector", {w16(0)}) == 0,
        "a global block's, the host's, the BIOS selector: refused (0 itself answers 0)");
  CHECK(ldt.in_use(blk) && m.rt.global().size(blk) == 0x100 && m.rt.rd16((uint32_t(blk) << 16) | 0x10) == 0xBEEF &&
            ldt.in_use(sys),
        "the refused selectors are intact");
  // A selector of theirs freed another way (as INT 31h AX=0001h frees one)
  // and handed out again, here to a global block, is theirs no longer.
  Machine m2;
  uint16_t s2 = uint16_t(api(m2, "KERNEL", "AllocSelector", {w16(m2.data(16))}));
  m2.rt.ldt().free(s2, 1);
  uint16_t h2 = uint16_t(api(m2, "KERNEL", "GlobalAlloc", {w16(0), l16(64)}));
  CHECK(h2 == s2 && uint16_t(api(m2, "KERNEL", "FreeSelector", {w16(s2)})) == s2 &&
            uint16_t(api(m2, "KERNEL", "SetSelectorBase", {w16(s2), l16(0)})) == 0 && m2.rt.global().size(h2) == 64,
        "its index reused for a global block: FreeSelector and SetSelectorBase refuse it (%04X %04X)", s2, h2);
}

// The freed-selector rule (Runtime16::null_freed_segments): a call that
// frees a selector held in DS, ES, FS or GS returns with that register null;
// nothing else changes.
void test_freed_selector_rule() {
  Machine m;
  auto block = [&](uint16_t* h) {
    *h = uint16_t(api(m, "KERNEL", "GlobalAlloc", {w16(GlobalHeap16::kMoveable), l16(64)}));
    return uint16_t(api(m, "KERNEL", "GlobalLock", {w16(*h)}) >> 16);
  };
  uint16_t h1 = 0, h2 = 0, h3 = 0;
  // Borland C++'s far-heap free (IS_FLY 1:6B71, then 1:6B16..1:6B20): ES
  // holds the block GlobalFree frees; push es / pop es follows.
  uint16_t s1 = block(&h1);
  bool fault = false;
  uint32_t r = run_guest(m, ops({mov_ax(s1), op(kMovEsAx), push_imm(h1), m.lcall("KERNEL", "GlobalFree"), {0x06, 0x07},
                                 op(kMovAxEs), {0xCB}}),
                         &fault);
  CHECK(!fault && (r & 0xFFFF) == 0, "GlobalFree of ES's block: ES comes back null, push es/pop es loads it (%04X)",
        r & 0xFFFF);
  // DECO.DLL's work buffer (GlobalFree at 6:A0F2 with DS holding it; push ds
  // at 7:002D, pop ds at 7:008A).
  uint16_t s2 = block(&h2);
  r = run_guest(m, ops({mov_ax(s2), op(kMovDsAx), push_imm(h2), m.lcall("KERNEL", "GlobalFree"), {0x1E, 0x1F},
                        op(kMovAxDs), {0xCB}}),
                &fault);
  CHECK(!fault && (r & 0xFFFF) == 0, "GlobalFree of DS's block: DS comes back null (%04X)", r & 0xFFFF);
  // FreeSelector of a selector held in ES, FS and GS: all three null.
  uint16_t blk = m.data(64);
  uint16_t s3 = uint16_t(api(m, "KERNEL", "AllocSelector", {w16(blk)}));
  // mov ax,s3; mov es,ax; mov fs,ax; mov gs,ax; push ax; lcall FreeSelector;
  // mov ax,es; mov dx,fs; or ax,dx; mov dx,gs; or ax,dx; retf
  r = run_guest(m, ops({mov_ax(s3), op(kMovEsAx), op(kMovFsAx), op(kMovGsAx), {0x50}, m.lcall("KERNEL", "FreeSelector"),
                        op(kMovAxEs), {0x8C, 0xE2, 0x09, 0xD0, 0x8C, 0xEA, 0x09, 0xD0, 0xCB}}),
                &fault);
  CHECK(!fault && (r & 0xFFFF) == 0 && !m.rt.ldt().in_use(s3), "FreeSelector of a selector in ES, FS and GS: all null");
  // A selector FreeSelector refuses (a global block's) stays in ES, usable.
  // mov ax,blk; mov es,ax; push ax; lcall FreeSelector; mov dx,es; mov bx,es:[0]; retf
  m.rt.wr16(uint32_t(blk) << 16, 0x7777);
  r = run_guest(m, ops({mov_ax(blk), op(kMovEsAx), {0x50}, m.lcall("KERNEL", "FreeSelector"),
                        {0x8C, 0xC2, 0x26, 0x8B, 0x1E, 0x00, 0x00, 0xCB}}),
                &fault);
  CHECK(!fault && (r & 0xFFFF) == blk && (r >> 16) == blk, "a foreign selector: refused, ES keeps it (%08X)", r);
  // A register holding a selector the call did not free keeps it.
  uint16_t s4 = block(&h3);
  r = run_guest(m, ops({mov_ax(blk), op(kMovEsAx), push_imm(h3), m.lcall("KERNEL", "GlobalFree"), op(kMovAxEs), {0xCB}}),
                &fault);
  CHECK(!fault && (r & 0xFFFF) == blk && !m.rt.global().find(s4), "GlobalFree of another block: ES keeps its selector");
  // The rule fixes no load: a selector pushed before the free and popped
  // after it still faults, as it did on Windows.
  uint16_t h5 = 0, s5 = block(&h5);
  run_guest(m, ops({mov_ax(s5), op(kMovEsAx), {0x06}, push_imm(h5), m.lcall("KERNEL", "GlobalFree"), {0x07, 0xCB}}), &fault);
  CHECK(fault, "push es, GlobalFree, pop es: the pop faults");
  // DPMI 1.0's Free LDT Descriptor (INT 31h AX=0001h) zeroes a segment register holding it.
  uint16_t d = m.rt.ldt().alloc(1);
  r = run_guest(m, ops({mov_ax(d), op(kMovEsAx), {0x89, 0xC3}, mov_ax(1), {0xCD, 0x31}, op(kMovAxEs), {0xCB}}), &fault);
  CHECK(!fault && (r & 0xFFFF) == 0 && !m.rt.ldt().in_use(d), "INT 31h 0001h of ES's selector: ES null");
}

// TOOLHELP's GlobalFirst/GlobalNext answer an empty walk (ADXPL100's
// lock_sequencer_down_hard_now notes the error and plays on).
void test_toolhelp_walk() {
  Machine m;
  uint16_t ds = m.data(64);
  uint32_t ge = uint32_t(ds) << 16;
  m.rt.wr32(ge, 0x24);  // dwSize = sizeof(GLOBALENTRY)
  m.rt.wr32(ge + 4, 0xDEADBEEF);
  CHECK((api(m, "TOOLHELP", "GlobalFirst", {l16(ge), w16(0)}) & 0xFFFF) == 0 &&
            (api(m, "TOOLHELP", "GlobalNext", {l16(ge), w16(0)}) & 0xFFFF) == 0,
        "GlobalFirst/GlobalNext(GLOBAL_ALL): FALSE");
  CHECK(m.rt.rd32(ge) == 0x24 && m.rt.rd32(ge + 4) == 0xDEADBEEF, "the GLOBALENTRY is left as it was");
}

// GetKeyState's toggle bit (bit 0): Caps Lock's from the CAPS line, Num
// Lock's from the NUMLOCK line (Final Exam starts its exam when it changes);
// GetAsyncKeyState's bit 0 stays "pressed since the last call".
void test_toggle_keys() {
  VirtualClock clock{VirtualClock::Mode::fixed_step, 16667};
  InputState in;
  Runtime16 rt{Runtime16Options{}, clock, &in};
  register_all16(rt);
  auto call = [&](const char* fn, int vk) {
    return uint16_t(rt.call_far(rt.thunk_far(*rt.shims().find_name("USER", fn)), {w16(uint16_t(vk))}));
  };
  CHECK(call("GetKeyState", VK_NUMLOCK) == 0 && call("GetKeyState", VK_CAPITAL) == 0, "both toggles off");
  in.numlock = true;
  CHECK(call("GetKeyState", VK_NUMLOCK) == 1 && call("GetKeyState", VK_CAPITAL) == 0, "NUMLOCK 1: Num Lock's toggle alone");
  in.numlock = false;
  in.caps = true;
  CHECK(call("GetKeyState", VK_NUMLOCK) == 0 && call("GetKeyState", VK_CAPITAL) == 1, "CAPS 1: Caps Lock's alone");
  in.numlock = true;
  in.keys.set(VK_NUMLOCK);
  CHECK(call("GetKeyState", VK_NUMLOCK) == 0x8001, "held and toggled: 8001 (%04X)", call("GetKeyState", VK_NUMLOCK));
  CHECK(call("GetAsyncKeyState", VK_NUMLOCK) == 0x8001 && call("GetAsyncKeyState", VK_NUMLOCK) == 0x8000,
        "GetAsyncKeyState: pressed since the last call, then down only");
  CHECK(call("GetKeyState", VK_SCROLL) == 0, "no other key has a toggle");
}

// INT 21h with these registers, as the guest's `int 21h` and KERNEL.DOS3Call
// reach it: AX, DX, DS (when given) and SI; the AX and carry it leaves.
struct Dos21 {
  uint16_t ax;
  bool cf;
};
Dos21 int21(Machine& m, uint16_t ax, uint16_t dx, uint16_t ds = 0, uint16_t si = 0) {
  auto& r = m.rt.cpu().registers();
  if (ds) m.rt.cpu().load_segment(SegReg::DS, ds);
  r.w_ax(ax);
  r.w_dx(dx);
  r.w_si(si);
  m.rt.set_carry(false);
  dos_int21(m.rt);
  return {r.r_ax(), (r.read_eflags() & 1) != 0};
}

// Directories for the drive tests, below `dir` mounted as C:\AFTERDRK (11
// characters): five D1234567s (56), then D123456.8 — C:\AFTERDRK\…\D123456.8
// is 66 characters, all a DOS current directory held — and D1234567.9 beside
// it, 67.
std::string deep_dir(const std::string& dir) {
  std::string d = dir;
  for (int i = 0; i < 5; i++) {
    d += "\\D1234567";
    CreateDirectoryA(d.c_str(), nullptr);
  }
  CreateDirectoryA((d + "\\D123456.8").c_str(), nullptr);
  CreateDirectoryA((d + "\\D1234567.9").c_str(), nullptr);
  return d;
}

void remove_deep_dir(const std::string& dir) {
  std::string d = dir + "\\D1234567\\D1234567\\D1234567\\D1234567\\D1234567";
  RemoveDirectoryA((d + "\\D123456.8").c_str());
  RemoveDirectoryA((d + "\\D1234567.9").c_str());
  for (int i = 0; i < 5; i++) {
    RemoveDirectoryA(d.c_str());
    d = d.substr(0, d.find_last_of('\\'));
  }
}

// DlgDirList and LB_DIR with DDL_DRIVES (configure mode, on real, never
// shown windows): "[-c-]", and "[-h-]" once the host's drives are mounted
// as H:; DlgDirSelect makes "[-h-]" "h:", and listing it lists H:\ (the
// drives) — Sounder's "Sounds.." folder dialog reaching the user's .WAVs.
// The list moves the guest's DOS to the drive and directory it lists, each
// drive keeping its own, and does not enter a folder deeper than a DOS
// current directory could be.
void test_dir_list_drives() {
  char base[MAX_PATH];
  GetTempPathA(MAX_PATH, base);
  std::string dir = std::string(base) + "adw_win16_ddl_" + std::to_string(GetCurrentProcessId());
  CreateDirectoryA(dir.c_str(), nullptr);
  CreateDirectoryA((dir + "\\SUB").c_str(), nullptr);
  if (FILE* f = fopen((dir + "\\JIM.WAV").c_str(), "wb")) fclose(f);
  // A folder of the host's own (under no mount), which the guest reaches
  // through H:; its name is its own 8.3 name, so no alias of it can shift.
  char hname[16];
  snprintf(hname, sizeof(hname), "H%07lX", (unsigned long)(GetCurrentProcessId() & 0xFFFFFFF));
  std::string hdir = std::string(base) + hname;
  CreateDirectoryA(hdir.c_str(), nullptr);
  if (FILE* f = fopen((hdir + "\\HOST.WAV").c_str(), "wb")) fclose(f);
  {
    Machine m;
    m.rt.vfs().mount("C:\\AFTERDRK", dir, false);
    win32::ConfigScript script;
    script.set_hidden(true);
    Configure16 cfg;
    cfg.script = &script;
    enable_real_dialogs16(m.rt, &cfg);
    HWND dlg = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 200, 200, nullptr, nullptr, nullptr, nullptr);
    HWND list = CreateWindowExW(0, L"LISTBOX", L"", WS_CHILD | LBS_HASSTRINGS, 0, 0, 100, 100, dlg,
                                reinterpret_cast<HMENU>(uintptr_t(204)), nullptr, nullptr);
    CreateWindowExW(0, L"STATIC", L"", WS_CHILD, 0, 120, 100, 20, dlg, reinterpret_cast<HMENU>(uintptr_t(203)), nullptr, nullptr);
    CHECK(dlg && list, "the real windows (error %lu)", GetLastError());
    uint16_t h16 = real_hwnd16(m.rt, dlg);
    uint16_t ds = m.data(256);
    uint32_t spec = uint32_t(ds) << 16, out = spec + 128;
    auto items = [&]() {
      std::string all;
      LRESULT n = SendMessageW(list, LB_GETCOUNT, 0, 0);
      for (LRESULT i = 0; i < n; i++) {
        wchar_t t[64] = {};
        SendMessageW(list, LB_GETTEXT, WPARAM(i), LPARAM(t));
        std::string s;
        for (const wchar_t* p = t; *p; p++) s += char(*p);
        all += (all.empty() ? "" : " ") + s;
      }
      return all;
    };
    auto dir_list = [&](const std::string& s) {
      m.rt.write_str(spec, s, 128);
      return api(m, "USER", "DlgDirList", {w16(h16), l16(spec), w16(204), w16(203), w16(DDL_EXCLUSIVE | DDL_DRIVES | DDL_DIRECTORY)}) & 0xFFFF;
    };
    CHECK(dir_list("C:\\AFTERDRK\\*.WAV") == 1 && items() == "[..] [SUB] [-c-]", "no H:: [-c-] alone (%s)", items().c_str());
    m.rt.vfs().mount_host_drives(/*short_names=*/true);
    CHECK(dir_list("C:\\AFTERDRK\\*.WAV") == 1 && items() == "[..] [SUB] [-c-] [-h-]", "H: mounted: [-h-] after [-c-] (%s)",
          items().c_str());
    // LB_DIR (Win16 WM_USER + 0x0E) lists the same.
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    m.rt.write_str(spec, "C:\\AFTERDRK\\*.*", 64);
    api(m, "USER", "SendMessage", {w16(real_hwnd16(m.rt, list)), w16(WM_USER + 0x0E), w16(DDL_DRIVES | DDL_DIRECTORY), l16(spec)});
    CHECK(items() == "JIM.WAV [..] [SUB] [-c-] [-h-]", "LB_DIR: files, directories, both drives (%s)", items().c_str());
    SendMessageW(list, LB_SETCURSEL, 4, 0);
    CHECK((api(m, "USER", "DlgDirSelect", {w16(h16), l16(out), w16(204)}) & 0xFFFF) == 1 && m.rt.read_str(out) == "h:",
          "DlgDirSelect on [-h-]: h: (%s)", m.rt.read_str(out).c_str());
    wchar_t shown[64] = {};
    CHECK(dir_list("h:*.WAV") == 1 && items().find("[-c-] [-h-]") != std::string::npos && items().rfind("[", 0) == 0 &&
              GetDlgItemTextW(dlg, 203, shown, 64) && std::wstring(shown) == L"H:\\",
          "h: lists H:\\, the host's drives (%s)", items().c_str());
    // The drive and directory the list moves to are the guest's DOS's: walked
    // down from H:\ as a module does ("C\*.WAV", …) to this test's folder on
    // the host, the guest's getcwd() (INT 21h AH=19h, then AH=47h) names it,
    // drive and all (it said C: whatever the list showed, and a module saved
    // SoundPath=C:\C\…); "c:" then lists C:'s own directory, where the list
    // left it, and "h:" H:'s.
    auto static_text = [&]() {
      wchar_t t[128] = {};
      GetDlgItemTextW(dlg, 203, t, 128);
      std::string s;
      for (const wchar_t* p = t; *p; p++) s += char(*p);
      return s;
    };
    uint16_t ds2 = m.data(128);
    auto getcwd = [&]() {
      auto& r = m.rt.cpu().registers();
      m.rt.cpu().load_segment(SegReg::DS, ds2);
      r.w_ax(0x1900);
      dos_int21(m.rt);
      char letter = char('A' + (r.r_ax() & 0xFF));
      r.w_ax(0x4700);
      r.w_dx(0);
      r.w_si(0);
      dos_int21(m.rt);
      return std::string(1, letter) + ":\\" + m.rt.read_str(uint32_t(ds2) << 16);
    };
    const std::string g = m.rt.vfs().host_to_guest(hdir);
    bool walked = g.rfind("H:\\", 0) == 0;
    for (size_t i = 3; walked && i < g.size();) {
      size_t j = g.find('\\', i);
      if (j == std::string::npos) j = g.size();
      walked = dir_list(g.substr(i, j - i) + "\\*.WAV") == 1;
      i = j + 1;
    }
    if (g.size() <= kMaxCurDir) {
      CHECK(walked && items() == "[..] [-c-] [-h-]" && static_text() == g && getcwd() == g &&
                m.rt.read_str(spec) == "*.WAV",
            "walked to %s: getcwd() %s, static %s (%s)", g.c_str(), getcwd().c_str(), static_text().c_str(), items().c_str());
    } else {
      CHECK(!walked, "%s is deeper than DOS's current directory: not entered", g.c_str());
    }
    std::string h_dir = getcwd();
    CHECK(dir_list("c:*.WAV") == 1 && static_text() == "C:\\AFTERDRK" && getcwd() == "C:\\AFTERDRK",
          "c: lists C:'s own directory (%s, getcwd() %s)", static_text().c_str(), getcwd().c_str());
    CHECK(dir_list("h:*.WAV") == 1 && static_text() == h_dir && getcwd() == h_dir,
          "h: lists H:'s own directory again (%s)", static_text().c_str());
    // A folder deeper than DOS's current directory could be (66 characters
    // with the drive) is not entered: 0, and the list, the static, the spec
    // and the current directory stay.
    deep_dir(dir);
    const std::string d56 = "C:\\AFTERDRK\\D1234567\\D1234567\\D1234567\\D1234567\\D1234567";
    CHECK(dir_list(d56 + "\\*.WAV") == 1 && items() == "[..] [D123456.8] [D1234567.9] [-c-] [-h-]",
          "C:\\AFTERDRK\\…\\D1234567 (%s)", items().c_str());
    CHECK(dir_list("D1234567.9\\*.WAV") == 0 && items() == "[..] [D123456.8] [D1234567.9] [-c-] [-h-]" &&
              static_text() == d56 && m.rt.read_str(spec) == "D1234567.9\\*.WAV" && getcwd() == d56,
          "D1234567.9 (67 characters) refused: %s, static %s, spec %s", items().c_str(), static_text().c_str(),
          m.rt.read_str(spec).c_str());
    CHECK(dir_list("D123456.8\\*.WAV") == 1 && items() == "[..] [-c-] [-h-]" && static_text() == d56 + "\\D123456.8" &&
              getcwd() == d56 + "\\D123456.8",
          "D123456.8 (66 characters) entered: %s (%s)", getcwd().c_str(), items().c_str());
    DestroyWindow(dlg);
  }
  remove_deep_dir(dir);
  DeleteFileA((dir + "\\JIM.WAV").c_str());
  RemoveDirectoryA((dir + "\\SUB").c_str());
  RemoveDirectoryA(dir.c_str());
  DeleteFileA((hdir + "\\HOST.WAV").c_str());
  RemoveDirectoryA(hdir.c_str());
}

// Configure mode keeps the guest's screen coordinates and lays its emulated
// desktop over the owner window (guest_screen_origin16): a top-level real
// window the guest places goes to origin + (x, y), a child stays in its
// parent's client area, and window rectangles, ClientToScreen/ScreenToClient,
// CB_GETDROPPEDCONTROLRECT and a top-level window's WM_MOVE come back on the
// guest's screen. Before, Marvel's Saver.. and Lunatic Fringe's Keys...
// centred themselves on 640 × 480 at the primary monitor's top left, far
// from the settings window. No owner, or a hidden run: (0, 0). Real windows
// only, never shown.
void test_configure_placement() {
  auto origin = [](RECT o, RECT wa, int w, int h) { return guest_screen_origin16(o, wa, w, h); };
  auto at = [](POINT p, LONG x, LONG y) { return p.x == x && p.y == y; };
  POINT p = origin({1000, 500, 1800, 1100}, {0, 0, 2560, 1392}, 640, 480);
  CHECK(at(p, 1080, 560), "centred on the owner (%ld,%ld)", p.x, p.y);
  p = origin({0, 0, 400, 300}, {0, 0, 2560, 1392}, 640, 480);
  CHECK(at(p, 0, 0), "an owner at the top left: the work area's corner (%ld,%ld)", p.x, p.y);
  p = origin({2400, 1300, 2560, 1392}, {0, 0, 2560, 1392}, 640, 480);
  CHECK(at(p, 1920, 912), "an owner at the bottom right: inside the work area (%ld,%ld)", p.x, p.y);
  p = origin({-1500, 200, -700, 800}, {-1920, 0, 0, 1040}, 640, 480);
  CHECK(at(p, -1420, 260), "a monitor left of the primary (%ld,%ld)", p.x, p.y);
  p = origin({100, 100, 300, 300}, {0, 0, 512, 384}, 640, 480);
  CHECK(at(p, -64, -48), "a work area smaller than the desktop: centred on it (%ld,%ld)", p.x, p.y);
  p = origin({1000, 500, 1800, 1100}, {0, 0, 2560, 1392}, 800, 600);
  CHECK(at(p, 1000, 500), "another desktop size (%ld,%ld)", p.x, p.y);

  // The wraps: an owner in the middle of the primary monitor's work area, a
  // top-level window it owns and a child of that.
  MONITORINFO pmi{};
  pmi.cbSize = sizeof(pmi);
  GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &pmi);
  const RECT pwa = pmi.rcWork;
  HWND owner = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, (pwa.left + pwa.right) / 2 - 350, (pwa.top + pwa.bottom) / 2 - 250,
                               700, 500, nullptr, nullptr, nullptr, nullptr);
  CHECK(owner != nullptr, "the owner window (error %lu)", GetLastError());
  auto real_rect = [](HWND h) {
    RECT r{};
    GetWindowRect(h, &r);
    return r;
  };
  {
    Machine m;
    Screen screen(640, 480);
    m.rt.attach_display(screen);
    win32::ConfigScript script;
    Configure16 cfg;
    cfg.script = &script;
    cfg.owner = owner;
    enable_real_dialogs16(m.rt, &cfg);
    // What enable_real_dialogs16 measured, in this thread's coordinates.
    const RECT orc = real_rect(owner);
    MONITORINFO omi{};
    omi.cbSize = sizeof(omi);
    GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &omi);
    const POINT o = guest_screen_origin16(orc, omi.rcWork, 640, 480);
    const RECT& wa = omi.rcWork;
    if (wa.right - wa.left >= 700 && wa.bottom - wa.top >= 500) {
      CHECK(at(o, (orc.left + orc.right) / 2 - 320, (orc.top + orc.bottom) / 2 - 240),
            "the guest's screen centred on the owner (%ld,%ld)", o.x, o.y);
    }
    HWND top = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 200, 150, owner, nullptr, nullptr, nullptr);
    HWND kid = CreateWindowExW(0, L"STATIC", L"", WS_CHILD, 0, 0, 50, 20, top, reinterpret_cast<HMENU>(uintptr_t(101)), nullptr,
                               nullptr);
    CHECK(top && kid, "the real windows (error %lu)", GetLastError());
    uint16_t t16 = real_hwnd16(m.rt, top), k16 = real_hwnd16(m.rt, kid);
    uint16_t ds = m.data(64);
    uint32_t buf = uint32_t(ds) << 16;
    auto guest_rect = [&](uint16_t h16) {
      api(m, "USER", "GetWindowRect", {w16(h16), l16(buf)});
      return read16<RECT16>(m.rt, buf);
    };
    auto guest_point = [&](const char* fn, uint16_t h16, int16_t x, int16_t y) {
      write16(m.rt, buf, POINT16{x, y});
      api(m, "USER", fn, {w16(h16), l16(buf)});
      return read16<POINT16>(m.rt, buf);
    };
    api(m, "USER", "MoveWindow", {w16(t16), w16(10), w16(20), w16(200), w16(150), w16(0)});
    RECT r = real_rect(top);
    CHECK(r.left == o.x + 10 && r.top == o.y + 20 && r.right == o.x + 210 && r.bottom == o.y + 170,
          "MoveWindow of a top-level window: origin + (10,20) (%ld,%ld)-(%ld,%ld)", r.left, r.top, r.right, r.bottom);
    RECT16 g = guest_rect(t16);
    CHECK(g.left == 10 && g.top == 20 && g.right == 210 && g.bottom == 170, "its GetWindowRect: on the guest's screen (%d,%d)-(%d,%d)",
          g.left, g.top, g.right, g.bottom);
    api(m, "USER", "SetWindowPos", {w16(t16), w16(0), w16(30), w16(40), w16(0), w16(0), w16(SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE)});
    r = real_rect(top);
    CHECK(r.left == o.x + 30 && r.top == o.y + 40 && r.right - r.left == 200, "SetWindowPos: origin + (30,40) (%ld,%ld)", r.left,
          r.top);
    api(m, "USER", "SetWindowPos",
        {w16(t16), w16(0), w16(500), w16(500), w16(220), w16(160), w16(SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE)});
    r = real_rect(top);
    CHECK(r.left == o.x + 30 && r.top == o.y + 40 && r.right - r.left == 220 && r.bottom - r.top == 160,
          "SetWindowPos with SWP_NOMOVE: resized where it was (%ld,%ld)-(%ld,%ld)", r.left, r.top, r.right, r.bottom);
    // A child: its position is in its parent's client area.
    api(m, "USER", "MoveWindow", {w16(k16), w16(5), w16(6), w16(50), w16(20), w16(0)});
    POINT co{0, 0};
    ClientToScreen(top, &co);
    RECT kr = real_rect(kid);
    CHECK(kr.left - co.x == 5 && kr.top - co.y == 6, "MoveWindow of a child: in its parent's client area (%ld,%ld)", kr.left - co.x,
          kr.top - co.y);
    g = guest_rect(k16);
    CHECK(g.left == 35 && g.top == 46 && g.right == 85 && g.bottom == 66, "a child's GetWindowRect: on the guest's screen (%d,%d)-(%d,%d)",
          g.left, g.top, g.right, g.bottom);
    POINT16 q = guest_point("ClientToScreen", t16, 1, 2);
    CHECK(q.x == 31 && q.y == 42, "ClientToScreen: on the guest's screen (%d,%d)", q.x, q.y);
    q = guest_point("ScreenToClient", t16, 31, 42);
    CHECK(q.x == 1 && q.y == 2, "ScreenToClient: from the guest's screen (%d,%d)", q.x, q.y);
    q = guest_point("ScreenToClient", k16, 35, 46);
    CHECK(q.x == 0 && q.y == 0, "ScreenToClient of a child's corner (%d,%d)", q.x, q.y);
    // CB_GETDROPPEDCONTROLRECT (Win16 WM_USER + 18) is on the screen too.
    HWND combo = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | CBS_DROPDOWNLIST, 10, 10, 80, 100, top,
                                 reinterpret_cast<HMENU>(uintptr_t(103)), nullptr, nullptr);
    RECT dr{};
    SendMessageW(combo, CB_GETDROPPEDCONTROLRECT, 0, LPARAM(&dr));
    api(m, "USER", "SendMessage", {w16(real_hwnd16(m.rt, combo)), w16(WM_USER + 18), w16(0), l16(buf)});
    g = read16<RECT16>(m.rt, buf);
    CHECK(combo && g.left == dr.left - o.x && g.top == dr.top - o.y && g.right == dr.right - o.x && g.bottom == dr.bottom - o.y,
          "CB_GETDROPPEDCONTROLRECT: on the guest's screen (%d,%d)-(%d,%d)", g.left, g.top, g.right, g.bottom);
    // WM_MOVE to a guest procedure (the guest subclassed both): a top-level
    // window's client origin on the guest's screen; a child's as it is.
    std::vector<std::pair<uint16_t, uint32_t>> moves;
    m.rt.shims().add("TESTCB", 4, "WNDPROC", Conv16::pascal_, false, 10, [&](Call16& c) {
      uint16_t h = c.w(), msg = c.w();
      c.w();
      uint32_t lp = c.l();
      if (msg == WM_MOVE) moves.emplace_back(h, lp);
      c.ret32(0);
    });
    uint32_t proc = m.rt.thunk_far(*m.rt.shims().find_name("TESTCB", "WNDPROC"));
    uint32_t old_t = api(m, "USER", "SetWindowLong", {w16(t16), w16(uint16_t(-4)), l16(proc)});
    uint32_t old_k = api(m, "USER", "SetWindowLong", {w16(k16), w16(uint16_t(-4)), l16(proc)});
    api(m, "USER", "MoveWindow", {w16(t16), w16(70), w16(80), w16(220), w16(160), w16(0)});
    api(m, "USER", "MoveWindow", {w16(k16), w16(7), w16(8), w16(50), w16(20), w16(0)});
    CHECK(moves.size() == 2 && moves[0] == std::make_pair(t16, uint32_t((80u << 16) | 70u)) &&
              moves[1] == std::make_pair(k16, uint32_t((8u << 16) | 7u)),
          "WM_MOVE: (70,80) for the top-level window, (7,8) for the child (%zu moves, %08X %08X)", moves.size(),
          moves.empty() ? 0u : moves[0].second, moves.size() < 2 ? 0u : moves[1].second);
    api(m, "USER", "SetWindowLong", {w16(t16), w16(uint16_t(-4)), l16(old_t)});
    api(m, "USER", "SetWindowLong", {w16(k16), w16(uint16_t(-4)), l16(old_k)});
    // CreateWindow on a real parent: an owned popup at a place on the guest's
    // screen; a child in its parent's client area.
    uint32_t cls = m.rt.static_bytes("t STATIC", "STATIC");
    uint16_t pop16 = uint16_t(api(m, "USER", "CreateWindow", {l16(cls), l16(cls), l16(WS_POPUP), w16(12), w16(34), w16(40), w16(30),
                                                              w16(t16), w16(0), w16(0), l16(0)}));
    uint16_t kid16 = uint16_t(api(m, "USER", "CreateWindow", {l16(cls), l16(cls), l16(WS_CHILD), w16(3), w16(4), w16(10), w16(10),
                                                              w16(t16), w16(102), w16(0), l16(0)}));
    HWND pop = real_window16(m.rt, pop16), kid2 = real_window16(m.rt, kid16);
    RECT pr = pop ? real_rect(pop) : RECT{};
    CHECK(pop && pr.left == o.x + 12 && pr.top == o.y + 34, "CreateWindow of an owned popup: origin + (12,34) (%ld,%ld)", pr.left,
          pr.top);
    co = POINT{0, 0};
    ClientToScreen(top, &co);
    RECT k2 = kid2 ? real_rect(kid2) : RECT{};
    CHECK(kid2 && k2.left - co.x == 3 && k2.top - co.y == 4, "CreateWindow of a child: in its parent's client area (%ld,%ld)",
          k2.left - co.x, k2.top - co.y);
    if (pop) DestroyWindow(pop);
    DestroyWindow(top);
  }
  // No owner, or a hidden run (its dialogs are parked): the guest's screen at (0, 0).
  for (int hidden = 0; hidden < 2; hidden++) {
    Machine m;
    Screen screen(640, 480);
    m.rt.attach_display(screen);
    win32::ConfigScript script;
    script.set_hidden(hidden != 0);
    Configure16 cfg;
    cfg.script = &script;
    cfg.owner = hidden ? owner : nullptr;
    enable_real_dialogs16(m.rt, &cfg);
    HWND top = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 100, 100, cfg.owner, nullptr, nullptr, nullptr);
    uint16_t t16 = real_hwnd16(m.rt, top);
    api(m, "USER", "MoveWindow", {w16(t16), w16(10), w16(20), w16(100), w16(100), w16(0)});
    RECT r = real_rect(top);
    CHECK(top && r.left == 10 && r.top == 20, "%s: MoveWindow to (10,20) as given (%ld,%ld)", hidden ? "hidden, owner" : "no owner",
          r.left, r.top);
    DestroyWindow(top);
  }
  DestroyWindow(owner);
}

// Registers a guest window class `name` (WNDCLASS at at + 0x10, the name at
// at) whose procedure is `proc`.
uint16_t register_guest_class(Machine& m, uint32_t at, const char* name, uint32_t proc, uint16_t hinst) {
  m.rt.write_str(at, name, 16);
  m.rt.wr32(at + 0x10 + 2, proc);
  m.rt.wr16(at + 0x10 + 10, hinst);
  m.rt.wr32(at + 0x10 + 22, at);
  return uint16_t(api(m, "USER", "RegisterClass", {l16(at + 0x10)}));
}

// Configure mode: what a guest's control answers of itself is the real
// answer, and what the real window manager asks of it arrives as Win16 asked.
// WM_NCHITTEST reaches a window procedure of the guest's with the point on the
// guest's screen, and its answer is an int: HTTRANSPARENT in AX alone is -1
// (Intermission's frames answer so, and the click goes on to the control under
// them); one it leaves to DefWindowProc is answered from the real point, and
// the guest's own WM_NCHITTEST to a real window has its point moved to the
// real screen. A real BM_SETCHECK/BM_GETCHECK (the real CheckDlgButton,
// IsDlgButtonChecked and CheckRadioButton, the guest's, which call them, the
// configure script's CHECK) reaches a guest class's procedure, or the guest's
// subclass of a real button, as Win16's WM_USER + 1 / WM_USER, and its answer
// is the WORD in AX; the guest's subclass of another real control is not
// asked. GetCursorPos is the real cursor on the guest's screen, or the
// emulated one where the real one cannot be read (not the input desktop).
// Before, WM_NCHITTEST and the BM_* messages stayed with the real default
// procedure (HTCLIENT, 0), and GetCursorPos answered the saver's (320, 240).
// Real windows only, never shown.
void test_configure_guest_controls() {
  MONITORINFO pmi{};
  pmi.cbSize = sizeof(pmi);
  GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &pmi);
  const RECT pwa = pmi.rcWork;
  HWND owner = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, (pwa.left + pwa.right) / 2 - 350, (pwa.top + pwa.bottom) / 2 - 250,
                               700, 500, nullptr, nullptr, nullptr, nullptr);
  CHECK(owner != nullptr, "the owner window (error %lu)", GetLastError());
  {
    Machine m;
    Screen screen(640, 480);
    m.rt.attach_display(screen);
    win32::ConfigScript script;
    Configure16 cfg;
    cfg.script = &script;
    cfg.owner = owner;
    enable_real_dialogs16(m.rt, &cfg);
    RECT orc{};
    GetWindowRect(owner, &orc);
    MONITORINFO omi{};
    omi.cbSize = sizeof(omi);
    GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &omi);
    const POINT o = guest_screen_origin16(orc, omi.rcWork, 640, 480);
    auto pack = [](LONG x, LONG y) { return (uint32_t(uint16_t(y)) << 16) | uint16_t(x); };
    struct Seen {
      uint16_t msg, wp;
      uint32_t lp;
    };
    std::vector<Seen> frame, check, sub_button, sub_edit;
    uint16_t state = 0, b16 = 0;
    std::map<uint16_t, uint32_t> old_proc;
    auto def = [&](uint16_t h, uint16_t msg, uint16_t wp, uint32_t lp) {
      return api(m, "USER", "DefWindowProc", {w16(h), w16(msg), w16(wp), l16(lp)});
    };
    // A frame (HTTRANSPARENT, in AX alone) and a check box (WM_USER, WM_USER +
    // 1; its state's WORD with junk above it), the rest DefWindowProc's.
    m.rt.shims().add("TESTGC", 1, "FRAMEPROC", Conv16::pascal_, false, 10, [&](Call16& c) {
      uint16_t h = c.w(), msg = c.w(), wp = c.w();
      uint32_t lp = c.l();
      frame.push_back({msg, wp, lp});
      c.ret32(msg == WM_NCHITTEST ? 0x0000FFFFu : def(h, msg, wp, lp));
    });
    m.rt.shims().add("TESTGC", 2, "CHECKPROC", Conv16::pascal_, false, 10, [&](Call16& c) {
      uint16_t h = c.w(), msg = c.w(), wp = c.w();
      uint32_t lp = c.l();
      check.push_back({msg, wp, lp});
      if (msg == WM_USER) return c.ret32(0xBEEF0000u | state);
      if (msg == WM_USER + 1) {
        state = wp;
        return c.ret32(0);
      }
      c.ret32(def(h, msg, wp, lp));
    });
    // The guest's subclass of a real button and of a real edit.
    m.rt.shims().add("TESTGC", 3, "SUBPROC", Conv16::pascal_, false, 10, [&](Call16& c) {
      uint16_t h = c.w(), msg = c.w(), wp = c.w();
      uint32_t lp = c.l();
      (h == b16 ? sub_button : sub_edit).push_back({msg, wp, lp});
      c.ret32(api(m, "USER", "CallWindowProc", {l16(old_proc[h]), w16(h), w16(msg), w16(wp), l16(lp)}));
    });
    auto thunk = [&](const char* name) { return m.rt.thunk_far(*m.rt.shims().find_name("TESTGC", name)); };
    uint16_t ds = m.data(512);
    const uint32_t mem = uint32_t(ds) << 16;
    CHECK(register_guest_class(m, mem, "TESTFRAME", thunk("FRAMEPROC"), 0) &&
              register_guest_class(m, mem + 0x40, "TESTCHECK", thunk("CHECKPROC"), 0),
          "the guest's classes");
    HWND top = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 300, 200, owner, nullptr, nullptr, nullptr);
    uint16_t t16 = real_hwnd16(m.rt, top);
    auto create = [&](uint32_t cls, int16_t x, int16_t y, uint16_t id) {
      return uint16_t(api(m, "USER", "CreateWindow", {l16(cls), l16(cls), l16(WS_CHILD | WS_VISIBLE), w16(x), w16(y), w16(100),
                                                      w16(30), w16(t16), w16(id), w16(0), l16(0)}));
    };
    uint16_t f16 = create(mem, 10, 10, 100), c16 = create(mem + 0x40, 10, 50, 101);
    HWND button = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_CHECKBOX, 150, 10, 100, 30, top,
                                  reinterpret_cast<HMENU>(uintptr_t(102)), nullptr, nullptr);
    HWND edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE, 150, 50, 100, 30, top, reinterpret_cast<HMENU>(uintptr_t(103)),
                                nullptr, nullptr);
    HWND fr = real_window16(m.rt, f16), ck = real_window16(m.rt, c16);
    CHECK(top && fr && ck && button && edit, "the real windows (error %lu)", GetLastError());
    RECT r{};
    // WM_NCHITTEST to the frame: its HTTRANSPARENT; the point it saw is on the guest's screen.
    GetWindowRect(fr, &r);
    frame.clear();
    LRESULT ht = SendMessageW(fr, WM_NCHITTEST, 0, MAKELPARAM(r.left + 5, r.top + 6));
    CHECK(ht == HTTRANSPARENT, "WM_NCHITTEST: the guest's HTTRANSPARENT (AX = FFFF, DX = 0) is -1 (%lld)", (long long)ht);
    const uint32_t want = pack(r.left + 5 - o.x, r.top + 6 - o.y);
    CHECK(frame.size() == 1 && frame[0].msg == WM_NCHITTEST && frame[0].lp == want,
          "the guest saw WM_NCHITTEST at the point on its screen (%zu, %08X; want %08X, origin %ld,%ld)", frame.size(),
          frame.empty() ? 0u : frame[0].lp, want, o.x, o.y);
    // To the check box, which leaves it to DefWindowProc: answered from the real point.
    GetWindowRect(ck, &r);
    check.clear();
    LRESULT in = SendMessageW(ck, WM_NCHITTEST, 0, MAKELPARAM(r.left + 2, r.top + 2));
    LRESULT out = SendMessageW(ck, WM_NCHITTEST, 0, MAKELPARAM(r.right + 40, r.top + 2));
    CHECK(in == HTCLIENT && out == HTNOWHERE && check.size() == 2 && check[0].msg == WM_NCHITTEST,
          "WM_NCHITTEST the guest leaves to DefWindowProc: HTCLIENT inside, HTNOWHERE outside (%lld, %lld; %zu seen)", (long long)in,
          (long long)out, check.size());
    // The guest's own WM_NCHITTEST to a real button: the point from its screen.
    b16 = real_hwnd16(m.rt, button);
    GetWindowRect(button, &r);
    uint32_t gh = api(m, "USER", "SendMessage", {w16(b16), w16(WM_NCHITTEST), w16(0), l16(pack(r.left + 3 - o.x, r.top + 3 - o.y))});
    CHECK(int16_t(gh) == HTCLIENT, "the guest's WM_NCHITTEST to a real button at a point of it on the guest's screen: HTCLIENT (%d)",
          int16_t(gh));
    // BM_SETCHECK / BM_GETCHECK to the guest's check box: WM_USER + 1 / WM_USER.
    check.clear();
    CheckDlgButton(top, 101, BST_CHECKED);
    CHECK(state == 1 && check.size() == 1 && check[0].msg == WM_USER + 1 && check[0].wp == 1,
          "the real CheckDlgButton: the guest's WM_USER + 1 (state %u, %zu seen)", state, check.size());
    UINT got = IsDlgButtonChecked(top, 101);
    CHECK(got == BST_CHECKED, "the real IsDlgButtonChecked: the WORD of the guest's WM_USER answer (%u)", got);
    api(m, "USER", "CheckDlgButton", {w16(t16), w16(101), w16(0)});
    CHECK(state == 0 && (api(m, "USER", "IsDlgButtonChecked", {w16(t16), w16(101)}) & 0xFFFF) == 0,
          "the guest's CheckDlgButton and IsDlgButtonChecked (state %u)", state);
    CheckRadioButton(top, 101, 101, 101);
    CHECK(state == 1, "the real CheckRadioButton (state %u)", state);
    win32::ConfigScript::Action a;
    a.kind = win32::ConfigScript::Action::Kind::check;
    a.id = 101;
    a.value = 0;
    CHECK(win32::ConfigScript::apply(top, a) && state == 0, "the configure script's CHECK (state %u)", state);
    // The guest's subclass of a real button sees them as WM_USER + n (and
    // passes them on to the button); its subclass of an edit is not asked.
    uint16_t e16 = real_hwnd16(m.rt, edit);
    old_proc[b16] = api(m, "USER", "SetWindowLong", {w16(b16), w16(uint16_t(-4)), l16(thunk("SUBPROC"))});
    old_proc[e16] = api(m, "USER", "SetWindowLong", {w16(e16), w16(uint16_t(-4)), l16(thunk("SUBPROC"))});
    sub_button.clear();
    sub_edit.clear();
    CheckDlgButton(top, 102, BST_CHECKED);
    got = IsDlgButtonChecked(top, 102);
    bool set = false, get = false;
    for (const Seen& s : sub_button) {
      set |= s.msg == WM_USER + 1 && s.wp == 1;
      get |= s.msg == WM_USER;
    }
    CHECK(set && get && got == BST_CHECKED, "a real button the guest subclassed: WM_USER + 1 and WM_USER, on to the button (%u)", got);
    SendMessageW(edit, BM_GETCHECK, 0, 0);
    CHECK(sub_edit.empty(), "a real edit the guest subclassed is not asked a button's question (%zu)", sub_edit.size());
    api(m, "USER", "SetWindowLong", {w16(b16), w16(uint16_t(-4)), l16(old_proc[b16])});
    api(m, "USER", "SetWindowLong", {w16(e16), w16(uint16_t(-4)), l16(old_proc[e16])});
    // GetCursorPos: the real cursor, on the guest's screen.
    POINT c0{}, c1{};
    BOOL r0 = GetCursorPos(&c0);
    api(m, "USER", "GetCursorPos", {l16(mem + 0x100)});
    BOOL r1 = GetCursorPos(&c1);
    POINT16 g = read16<POINT16>(m.rt, mem + 0x100);
    if (r0 && r1) {
      CHECK((g.x == c0.x - o.x && g.y == c0.y - o.y) || (g.x == c1.x - o.x && g.y == c1.y - o.y),
            "GetCursorPos: the real cursor less the origin (%d,%d; real %ld,%ld, origin %ld,%ld)", g.x, g.y, c0.x, c0.y, o.x, o.y);
    } else {
      CHECK(g.x == 320 && g.y == 240, "GetCursorPos where the real cursor cannot be read: the emulated one (%d,%d)", g.x, g.y);
    }
    DestroyWindow(top);
  }
  DestroyWindow(owner);
}

// Configure mode: a click lands where a user's would, in a real dialog made
// from a module's template. A made-up NE module (CLICKS.DLL) holds the dialog:
// a frame of a class the guest registered lies over a check box of another of
// its classes, as Intermission's ANT3DBOX over ANT3DCHECK (first in the
// template, so on top), and over a plain BS_CHECKBOX and two plain
// BS_RADIOBUTTONs, which its DLGPROC checks itself on BN_CLICKED
// (CheckDlgButton, CheckRadioButton); a list box and OK lie beside it. The
// configure script's PRESS clicks each where a user would (the dialog hidden):
// the frame answers WM_NCHITTEST with HTTRANSPARENT (AX alone), so every click
// goes on to the control under it. The guest's check box takes the click and
// posts its BN_CLICKED, which arrives before the next action; the real buttons
// send theirs; the list box selects the item under the point. On OK the
// DLGPROC reads them back with IsDlgButtonChecked (the guest's check box
// through the real BM_GETCHECK, as Win16's WM_USER) and LB_GETCURSEL. Then the
// script's CHECK sets the guest's check box (the real BM_SETCHECK, as Win16's
// WM_USER + 1). Before, the frame answered HTCLIENT (the real default
// procedure's answer) and took every click under it: nothing changed there,
// and CHECK did not reach the guest's check box.
void test_configure_dialog_clicks() {
  char dir[MAX_PATH];
  GetTempPathA(MAX_PATH, dir);
  std::string file = std::string(dir) + "adw_win16_clicks_" + std::to_string(GetCurrentProcessId()) + ".dll";
  // The dialog (Win16 DLGTEMPLATE), its items top of the z-order first.
  std::string t;
  put32(t, WS_POPUP | WS_CAPTION | DS_MODALFRAME | DS_SETFONT);
  t.push_back(8);
  put16(t, 0), put16(t, 0), put16(t, 200), put16(t, 110);
  t.push_back('\0');  // no menu
  t.push_back('\0');  // the dialog class
  t += "Clicks";
  t.push_back('\0');
  put16(t, 8);
  t += "Helv";
  t.push_back('\0');
  auto item = [&](int16_t x, int16_t y, int16_t cx, int16_t cy, uint16_t id, uint32_t style, const std::string& cls,
                  const std::string& text) {
    put16(t, uint16_t(x)), put16(t, uint16_t(y)), put16(t, uint16_t(cx)), put16(t, uint16_t(cy)), put16(t, id);
    put32(t, WS_CHILD | WS_VISIBLE | style);
    t += cls;  // a predefined class's atom byte, or a name
    if (uint8_t(cls[0]) < 0x80) t.push_back('\0');
    t += text;
    t.push_back('\0');
    t.push_back('\0');  // no extra bytes
  };
  item(5, 5, 120, 75, 100, 0, "TESTFRAME", "");
  item(10, 10, 100, 12, 201, WS_TABSTOP, "TESTCHECK", "&Guest");
  item(10, 25, 100, 12, 202, BS_CHECKBOX | WS_TABSTOP, "\x80", "&Plain");
  item(10, 40, 100, 12, 203, BS_RADIOBUTTON | WS_GROUP | WS_TABSTOP, "\x80", "&One");
  item(10, 55, 100, 12, 204, BS_RADIOBUTTON, "\x80", "&Two");
  item(135, 5, 60, 60, 205, LBS_NOTIFY | WS_BORDER | WS_VSCROLL | WS_GROUP | WS_TABSTOP, "\x83", "");
  item(130, 90, 30, 14, IDOK, BS_DEFPUSHBUTTON | WS_GROUP | WS_TABSTOP, "\x80", "OK");
  item(165, 90, 30, 14, IDCANCEL, BS_PUSHBUTTON | WS_TABSTOP, "\x80", "Cancel");
  {
    std::string img = build_ne(true, 5, 100, t);  // RT_DIALOG 100
    FILE* fh = fopen(file.c_str(), "wb");
    fwrite(img.data(), 1, img.size(), fh);
    fclose(fh);
  }
  {
    Machine m;
    Screen screen(640, 480);
    m.rt.attach_display(screen);
    uint16_t err = 0;
    Module16* mod = m.rt.modules().load_host(file, &err);
    CHECK(mod != nullptr, "the module holding the dialog loads (error %u)", err);
    win32::ConfigScript script;
    script.set_hidden(true);
    script.set_timeout_ms(8000);
    Configure16 cfg;
    cfg.script = &script;
    enable_real_dialogs16(m.rt, &cfg);
    struct State {
      std::vector<uint32_t> frame_hits;  // the WM_NCHITTEST points the frame saw
      int frame_clicks = 0;              // mouse buttons that reached the frame
      uint16_t check = 0;                // the guest's check box
      bool pressed = false;
      int check_sets = 0;  // its WM_USER + 1
      int notified = 0;    // its BN_CLICKED at the DLGPROC
      int selchange = 0;
      int dlg_hittests = 0;  // WM_NCHITTEST at the DLGPROC
      int ok = 0;
      uint16_t got[5] = {};  // on OK: 201, 202, 203, 204, the list's selection
    } s;
    auto user = [&](const char* fn, std::initializer_list<Arg16> a) { return api(m, "USER", fn, a); };
    uint16_t ds = m.data(512);
    const uint32_t mem = uint32_t(ds) << 16;
    m.rt.write_str(mem + 0x100, "One", 8);
    m.rt.write_str(mem + 0x108, "Two", 8);
    m.rt.write_str(mem + 0x110, "Three", 8);
    m.rt.shims().add("TESTDLG", 1, "DLGPROC", Conv16::pascal_, true, 10, [&](Call16& c) {
      uint16_t h = c.w(), msg = c.w(), wp = c.w();
      uint32_t lp = c.l();
      if (msg == WM_NCHITTEST) s.dlg_hittests++;
      if (msg == WM_INITDIALOG) {
        user("CheckRadioButton", {w16(h), w16(203), w16(204), w16(203)});
        user("CheckDlgButton", {w16(h), w16(201), w16(0)});
        for (uint32_t str : {0x100u, 0x108u, 0x110u})
          user("SendDlgItemMessage", {w16(h), w16(205), w16(WM_USER + 1), w16(0), l16(mem + str)});  // LB_ADDSTRING
        user("SendDlgItemMessage", {w16(h), w16(205), w16(WM_USER + 0x21), w16(0), l16(16)});       // LB_SETITEMHEIGHT
        return c.ret(1);
      }
      if (msg != WM_COMMAND) return c.ret(0);
      const uint16_t code = uint16_t(lp >> 16);
      switch (wp) {
        case 201:
          if (code == BN_CLICKED) s.notified++;
          break;
        case 202:
          if (code == BN_CLICKED) user("CheckDlgButton", {w16(h), w16(202), w16(!(user("IsDlgButtonChecked", {w16(h), w16(202)}) & 0xFFFF))});
          break;
        case 203:
        case 204:
          if (code == BN_CLICKED) user("CheckRadioButton", {w16(h), w16(203), w16(204), w16(wp)});
          break;
        case 205:
          if (code == LBN_SELCHANGE) s.selchange++;
          break;
        case IDOK:
          for (uint16_t i = 0; i < 4; i++) s.got[i] = uint16_t(user("IsDlgButtonChecked", {w16(h), w16(uint16_t(201 + i))}));
          s.got[4] = uint16_t(user("SendDlgItemMessage", {w16(h), w16(205), w16(WM_USER + 9), w16(0), l16(0)}));  // LB_GETCURSEL
          s.ok++;
          user("EndDialog", {w16(h), w16(1)});
          break;
        case IDCANCEL:
          user("EndDialog", {w16(h), w16(2)});
          break;
        default:
          return c.ret(0);
      }
      c.ret(1);
    });
    // The frame: transparent to the hit test (AX alone), and nothing else of its own.
    m.rt.shims().add("TESTDLG", 2, "FRAMEPROC", Conv16::pascal_, false, 10, [&](Call16& c) {
      uint16_t h = c.w(), msg = c.w(), wp = c.w();
      uint32_t lp = c.l();
      if (msg == WM_NCHITTEST) {
        s.frame_hits.push_back(lp);
        return c.ret32(0x0000FFFF);
      }
      if (msg >= WM_LBUTTONDOWN && msg <= WM_MBUTTONDBLCLK) s.frame_clicks++;
      c.ret32(user("DefWindowProc", {w16(h), w16(msg), w16(wp), l16(lp)}));
    });
    // The check box: a click (down, then up inside) toggles it and posts
    // BN_CLICKED to its parent; WM_USER / WM_USER + 1 read and set it.
    m.rt.shims().add("TESTDLG", 3, "CHECKPROC", Conv16::pascal_, false, 10, [&](Call16& c) {
      uint16_t h = c.w(), msg = c.w(), wp = c.w();
      uint32_t lp = c.l();
      switch (msg) {
        case WM_GETDLGCODE:
          return c.ret32(DLGC_BUTTON);
        case WM_LBUTTONDOWN:
          s.pressed = true;
          return c.ret32(0);
        case WM_LBUTTONUP: {
          RECT cr{};
          GetClientRect(real_window16(m.rt, h), &cr);
          const int16_t x = int16_t(lp & 0xFFFF), y = int16_t(lp >> 16);
          if (s.pressed && x >= 0 && y >= 0 && x < cr.right && y < cr.bottom) {
            s.check ^= 1;
            user("PostMessage", {w16(uint16_t(user("GetParent", {w16(h)}))), w16(WM_COMMAND), w16(201), l16((uint32_t(BN_CLICKED) << 16) | h)});
          }
          s.pressed = false;
          return c.ret32(0);
        }
        case WM_USER:
          return c.ret32(s.check);
        case WM_USER + 1:
          s.check = wp != 0;
          s.check_sets++;
          return c.ret32(0);
        default:
          c.ret32(user("DefWindowProc", {w16(h), w16(msg), w16(wp), l16(lp)}));
      }
    });
    auto thunk = [&](const char* name) { return m.rt.thunk_far(*m.rt.shims().find_name("TESTDLG", name)); };
    const uint16_t hinst = mod ? mod->hinstance : 0;
    CHECK(register_guest_class(m, mem, "TESTFRAME", thunk("FRAMEPROC"), hinst) &&
              register_guest_class(m, mem + 0x40, "TESTCHECK", thunk("CHECKPROC"), hinst),
          "the module's classes");
    // Clicks: the guest's check box, the plain check box and the second radio
    // button under the frame; the list's second item (16 pixels high).
    std::string why;
    CHECK(script.parse("PRESS 201\nPRESS 202\nPRESS 204\nPRESS 205 10 24\nCLICK 1\n", &why), "the script (%s)", why.c_str());
    uint32_t r = mod ? api(m, "USER", "DialogBox", {w16(hinst), l16(100), w16(0), l16(thunk("DLGPROC"))}) : 0;
    CHECK(int16_t(r) == 1 && s.ok == 1 && !cfg.failed, "the dialog ran and ended on OK (%d, %d)", int16_t(r), s.ok);
    CHECK(!s.frame_hits.empty() && s.frame_clicks == 0, "the frame was asked (%zu times) and took no click (%d)", s.frame_hits.size(),
          s.frame_clicks);
    CHECK(s.got[0] == 1 && s.notified == 1 && s.check_sets == 1,
          "the guest's check box under the frame: clicked, checked, its BN_CLICKED before OK; CheckDlgButton reached it "
          "(%u, %d, %d)",
          s.got[0], s.notified, s.check_sets);
    CHECK(s.got[1] == 1, "the plain BS_CHECKBOX under the frame: checked by the DLGPROC on its BN_CLICKED (%u)", s.got[1]);
    CHECK(s.got[2] == 0 && s.got[3] == 1, "the plain BS_RADIOBUTTONs under the frame: the second checked by the DLGPROC (%u %u)",
          s.got[2], s.got[3]);
    CHECK(s.got[4] == 1 && s.selchange == 1, "the list box: the item under the click (%u), LBN_SELCHANGE (%d)", s.got[4], s.selchange);
    CHECK(s.dlg_hittests == 0, "the DLGPROC is never asked WM_NCHITTEST (%d)", s.dlg_hittests);
    // The script's CHECK on the guest's check box: the real BM_SETCHECK as WM_USER + 1.
    s.ok = 0;
    CHECK(script.parse("CHECK 201 1\nCLICK 1\n", &why), "the second script (%s)", why.c_str());
    r = mod ? api(m, "USER", "DialogBox", {w16(hinst), l16(100), w16(0), l16(thunk("DLGPROC"))}) : 0;
    CHECK(int16_t(r) == 1 && s.ok == 1 && s.got[0] == 1 && s.got[1] == 0 && s.got[2] == 1,
          "CHECK 201 1: the guest's check box set (%u; plain %u, first radio %u)", s.got[0], s.got[1], s.got[2]);
    m.rt.modules().free_all();
  }
  DeleteFileA(file.c_str());
}

// The current drive and directories as DOS kept them (dos16.hh): AH=19h
// reports the current directory's drive, AH=0Eh selects a drive the guest's
// disk has (C:, and H: with the host's drives mounted) and reports the letters
// to H:, AH=3Bh sets the directory of its path's drive and leaves the current
// drive, AH=47h reports any drive's own directory (DL 0 the current one, 3 C:,
// 8 H:), whole; "C:name" resolves against C:'s own directory in every file
// call, and TF_FORCEDRIVE's temp file goes there. A directory deeper than a
// DOS current directory could be (66 characters with its drive) is refused
// with error 3 and nothing changes — before, AH=19h said C: whatever the
// current directory's drive, AH=0Eh selected nothing, and AH=47h returned the
// current directory, cut to 63 characters, whichever drive DL named.
void test_dos_drives() {
  char base[MAX_PATH];
  GetTempPathA(MAX_PATH, base);
  std::string dir = std::string(base) + "adw_win16_drv_" + std::to_string(GetCurrentProcessId());
  CreateDirectoryA(dir.c_str(), nullptr);
  CreateDirectoryA((dir + "\\SUB").c_str(), nullptr);
  if (FILE* f = fopen((dir + "\\SUB\\X.TXT").c_str(), "wb")) {
    fputs("sub", f);
    fclose(f);
  }
  deep_dir(dir);
  const std::string d66 = "C:\\AFTERDRK\\D1234567\\D1234567\\D1234567\\D1234567\\D1234567\\D123456.8";
  const std::string d67 = "C:\\AFTERDRK\\D1234567\\D1234567\\D1234567\\D1234567\\D1234567\\D1234567.9";
  CHECK(d66.size() == kMaxCurDir && d67.size() == kMaxCurDir + 1, "the test's paths: %zu and %zu characters", d66.size(),
        d67.size());
  {
    Machine m;
    win32::Vfs& vfs = m.rt.vfs();
    vfs.mount("C:\\AFTERDRK", dir, false);
    vfs.set_cwd("C:\\AFTERDRK");
    uint16_t ds = m.data(512);
    uint32_t buf = uint32_t(ds) << 16, path = buf + 0x100;
    auto getcwd = [&](uint8_t drive, Dos21* res = nullptr) {
      m.rt.write_str(buf, "(untouched)", 64);
      Dos21 d = int21(m, 0x4700, drive, ds, 0);
      if (res) *res = d;
      return d.cf ? std::string("(error)") : m.rt.read_str(buf);
    };
    auto chdir = [&](const std::string& p) {
      m.rt.write_str(path, p, 128);
      return int21(m, 0x3B00, uint16_t(path), ds);
    };
    auto drive = [&]() { return int(int21(m, 0x1900, 0).ax & 0xFF); };
    Dos21 r{};
    CHECK(drive() == 2 && getcwd(0, &r) == "AFTERDRK" && r.ax == 0x0100 && getcwd(3) == "AFTERDRK",
          "C:\\AFTERDRK current: AH=19h 2, AH=47h \"%s\" (DL 0) and \"%s\" (DL 3)", getcwd(0).c_str(), getcwd(3).c_str());
    CHECK(getcwd(8, &r) == "(error)" && r.ax == doserr::kInvalidDrive, "no H: before the host's drives are mounted (%04X)",
          r.ax);
    vfs.mount_host_drives(/*short_names=*/true);
    CHECK(getcwd(8) == "", "H: mounted, never visited: its root (\"%s\")", getcwd(8).c_str());
    CHECK(getcwd(4, &r) == "(error)" && r.cf && r.ax == doserr::kInvalidDrive && getcwd(27, &r) == "(error)",
          "no D: (or drive 27): error 15 (%04X)", r.ax);
    // AH=0Eh: an absent drive changes nothing; H: becomes the current drive at its root.
    CHECK((int21(m, 0x0E00, 3).ax & 0xFF) == kLastDrive && drive() == 2 && vfs.cwd() == "C:\\AFTERDRK",
          "AH=0Eh D:: 8 letters, still C: (%s)", vfs.cwd().c_str());
    CHECK((int21(m, 0x0E00, 7).ax & 0xFF) == 8 && drive() == 7 && vfs.cwd() == "H:\\" && getcwd(0) == "" &&
              getcwd(3) == "AFTERDRK",
          "AH=0Eh H:: H: current at its root, C: keeps C:\\AFTERDRK (%s, \"%s\")", vfs.cwd().c_str(), getcwd(3).c_str());
    // AH=3Bh on C: from H:: C:'s directory moves, the current drive stays H:.
    r = chdir("C:\\AFTERDRK\\SUB");
    CHECK(!r.cf && drive() == 7 && getcwd(3) == "AFTERDRK\\SUB" && getcwd(0) == "",
          "AH=3Bh C:\\AFTERDRK\\SUB with H: current: C:'s own, H: stays (%d, \"%s\")", drive(), getcwd(3).c_str());
    // "C:name" is C:'s own directory's, in the file calls too.
    DosFiles& files = m.rt.state<DosFiles>();
    CHECK(vfs.full_path("C:X.TXT") == "C:\\AFTERDRK\\SUB\\X.TXT" && files.exists("C:X.TXT") &&
              vfs.full_path("c:") == "C:\\AFTERDRK\\SUB" && vfs.full_path("\\X") == "H:\\X",
          "C:X.TXT = %s, c: = %s", vfs.full_path("C:X.TXT").c_str(), vfs.full_path("c:").c_str());
    uint16_t h = uint16_t(api(m, "KERNEL", "_lopen", {l16(m.rt.static_bytes("t cx", "C:X.TXT")), w16(0)}));
    char got[4] = {};
    if (h != 0xFFFF) {
      files.read(h, buf, 3);
      m.rt.read_bytes(buf, got, 3);
      api(m, "KERNEL", "_lclose", {w16(h)});
    }
    CHECK(h != 0xFFFF && std::string(got, 3) == "sub", "_lopen(\"C:X.TXT\") reads C:\\AFTERDRK\\SUB\\X.TXT (%s)",
          std::string(got, 3).c_str());
    uint32_t tmp = uint32_t(m.data(160)) << 16, pfx = m.rt.static_bytes("t pfx", "str");
    api(m, "KERNEL", "GetTempFileName", {w16(0x80 | 'C'), l16(pfx), w16(0x10), l16(tmp)});
    std::string t1 = m.rt.read_str(tmp);
    api(m, "KERNEL", "GetTempFileName", {w16(0x80), l16(pfx), w16(0x11), l16(tmp)});
    std::string t2 = m.rt.read_str(tmp);
    CHECK(t1 == "C:\\AFTERDRK\\SUB\\~str0010.TMP" && t2 == "H:\\~str0011.TMP",
          "TF_FORCEDRIVE: C:'s own directory (%s), the current drive's (%s)", t1.c_str(), t2.c_str());
    // Back to C: (AH=0Eh): its own directory again.
    int21(m, 0x0E00, 2);
    CHECK(drive() == 2 && vfs.cwd() == "C:\\AFTERDRK\\SUB" && getcwd(8) == "", "AH=0Eh C:: at C:\\AFTERDRK\\SUB (%s)",
          vfs.cwd().c_str());
    // DOS's limit: 66 characters with the drive; AH=47h hands out all 63 after "C:\".
    r = chdir(d66);
    std::string w66 = getcwd(0);
    CHECK(!r.cf && vfs.cwd() == d66 && w66 == d66.substr(3) && w66.size() == 63,
          "AH=3Bh to 66 characters: current; AH=47h whole (%zu: %s)", w66.size(), w66.c_str());
    r = chdir(d67);
    CHECK(r.cf && r.ax == doserr::kPathNotFound && vfs.cwd() == d66 && getcwd(0) == w66,
          "AH=3Bh to 67 characters: error 3 (%04X), nothing changes (%s)", r.ax, vfs.cwd().c_str());
    CHECK(dos_chdir(m.rt, d67, /*select_drive=*/true) == -int(doserr::kPathNotFound) && vfs.cwd() == d66,
          "dos_chdir (DlgDirList's) refuses it too");
    r = chdir("..");
    CHECK(!r.cf && vfs.cwd() == d66.substr(0, d66.find_last_of('\\')), "AH=3Bh ..: %s", vfs.cwd().c_str());
    for (const char* bad : {"C:\\AFTERDRK\\NOSUCH", "C:\\AFTERDRK\\SUB\\X.TXT", "C:\\AFTERDRK\\S*", ""}) {
      std::string before = vfs.cwd();
      r = chdir(bad);
      CHECK(r.cf && r.ax == doserr::kPathNotFound && vfs.cwd() == before, "AH=3Bh \"%s\": error 3 (%04X)", bad, r.ax);
    }
    // Vfs::set_cwd (Win32's SetCurrentDirectory) takes existing directories
    // only (it took any path below a mount that was not a file).
    std::string before = vfs.cwd();
    CHECK(!vfs.set_cwd("C:\\AFTERDRK\\NOSUCH") && !vfs.set_cwd("C:\\AFTERDRK\\SUB\\X.TXT") && vfs.cwd() == before &&
              vfs.set_cwd("C:\\AFTERDRK") && vfs.cwd() == "C:\\AFTERDRK",
          "set_cwd: only an existing directory (%s)", vfs.cwd().c_str());
  }
  remove_deep_dir(dir);
  DeleteFileA((dir + "\\SUB\\X.TXT").c_str());
  RemoveDirectoryA((dir + "\\SUB").c_str());
  RemoveDirectoryA(dir.c_str());
}

// ---- tasks: a Win16 application run as the runtime's task (modules16.hh "Tasks") ---------------------------

// Writes an NE application (no LIBRARY bit, MULTIPLEDATA): segment 1 is
// `code`, segment 2 DGROUP (0x40 bytes of static data), heap 0x200, stack
// 0x400, CS:IP 1:0, SS:SP 2:0; `imports` are pointer32 import-ordinal
// fixups (module 1 KERNEL, 2 USER) at their code offsets.
struct AppImport {
  uint16_t at, module, ordinal;
};
std::string build_ne_app(const std::vector<uint8_t>& code_bytes, const std::vector<AppImport>& imports) {
  auto w16 = [](std::string& s, size_t at, uint16_t v) {
    if (s.size() < at + 2) s.resize(at + 2, '\0');
    s[at] = char(v);
    s[at + 1] = char(v >> 8);
  };
  std::string f(0x40, '\0');
  f[0] = 'M';
  f[1] = 'Z';
  f[0x3C] = 0x40;
  std::string h(0x40, '\0');
  h[0] = 'N';
  h[1] = 'E';
  std::string resident, modref, imp, entry;
  resident += char(7) + std::string("TESTAPP") + std::string("\0\0", 2);
  resident += '\0';
  imp += '\0';
  imp += char(6) + std::string("KERNEL");
  imp += char(4) + std::string("USER");
  w16(modref, 0, 1);
  w16(modref, 2, 8);
  entry += '\0';
  uint16_t off = 0x40;
  const uint16_t seg_off = off;
  off += 16;
  const uint16_t res_off = off, resident_off = off;
  off += uint16_t(resident.size());
  const uint16_t modref_off = off;
  off += uint16_t(modref.size());
  const uint16_t imp_off = off;
  off += uint16_t(imp.size());
  const uint16_t entry_off = off;
  off += uint16_t(entry.size());
  w16(h, 0x04, entry_off);
  w16(h, 0x06, uint16_t(entry.size()));
  w16(h, 0x0C, 0x0002);  // MULTIPLEDATA, an application
  w16(h, 0x0E, 2);       // autodata = segment 2
  w16(h, 0x10, 0x200);   // heap
  w16(h, 0x12, 0x400);   // stack
  w16(h, 0x14, 0);       // IP
  w16(h, 0x16, 1);       // CS
  w16(h, 0x18, 0);       // SP 0: the top of the stack area
  w16(h, 0x1A, 2);       // SS = DGROUP
  w16(h, 0x1C, 2);
  w16(h, 0x1E, 2);
  w16(h, 0x22, seg_off);
  w16(h, 0x24, res_off);
  w16(h, 0x26, resident_off);
  w16(h, 0x28, modref_off);
  w16(h, 0x2A, imp_off);
  w16(h, 0x32, 4);
  h[0x36] = 2;
  w16(h, 0x3E, 0x030A);
  std::string code(code_bytes.begin(), code_bytes.end());
  code.resize((code.size() + 15) & ~size_t(15), '\x90');
  std::string rel;
  w16(rel, 0, uint16_t(imports.size()));
  for (size_t i = 0; i < imports.size(); i++) {
    const size_t at = 2 + i * 8;
    rel.resize(at + 8, '\0');
    rel[at] = 3;      // pointer32
    rel[at + 1] = 1;  // import ordinal
    w16(rel, at + 2, imports[i].at);
    w16(rel, at + 4, imports[i].module);
    w16(rel, at + 6, imports[i].ordinal);
  }
  // The code at 0x200 (sector 0x20), its relocations after it, the data in the next sector.
  const uint16_t data_sector = uint16_t((0x200 + code.size() + rel.size() + 15) >> 4);
  std::string segt;
  w16(segt, 0, 0x20);
  w16(segt, 2, uint16_t(code.size()));
  w16(segt, 4, 0x0100 | 0x0010);
  w16(segt, 6, uint16_t(code.size()));
  w16(segt, 8, data_sector);
  w16(segt, 10, 0x40);
  w16(segt, 12, 0x0001);
  w16(segt, 14, 0x40);
  std::string all = f + h + segt + resident + modref + imp + entry;
  all.resize(0x200, '\0');
  all += code;
  all += rel;
  all.resize(size_t(data_sector) << 4, '\0');
  all.resize(all.size() + 0x40, '\0');
  return all;
}

// A made-up program: InitTask, its registers stored in DGROUP (AX 10h, BX
// 12h, CX 14h, DX 16h, SI 18h, DI 1Ah, ES 1Ch, SP 1Eh, SS 20h), then
// GetMessage(DS:30h, 0, 0, 0) (its result at 22h, the message at 24h), then
// INT 21h AH=4Ch with code 7.
std::string task_program() {
  const std::vector<uint8_t> code = {
      0x9A, 0xFF, 0xFF, 0x00, 0x00,  // 00 lcall InitTask
      0xA3, 0x10, 0x00,              // 05 mov [10h], ax
      0x89, 0x1E, 0x12, 0x00,        // 08 mov [12h], bx
      0x89, 0x0E, 0x14, 0x00,        // 0C mov [14h], cx
      0x89, 0x16, 0x16, 0x00,        // 10 mov [16h], dx
      0x89, 0x36, 0x18, 0x00,        // 14 mov [18h], si
      0x89, 0x3E, 0x1A, 0x00,        // 18 mov [1Ah], di
      0x8C, 0x06, 0x1C, 0x00,        // 1C mov [1Ch], es
      0x89, 0x26, 0x1E, 0x00,        // 20 mov [1Eh], sp
      0x8C, 0x16, 0x20, 0x00,        // 24 mov [20h], ss
      0x1E,                          // 28 push ds
      0xB8, 0x30, 0x00, 0x50,        // 29 mov ax, 30h; push ax
      0x31, 0xC0, 0x50, 0x50, 0x50,  // 2D xor ax, ax; push ax (x3)
      0x9A, 0xFF, 0xFF, 0x00, 0x00,  // 32 lcall GetMessage
      0xA3, 0x22, 0x00,              // 37 mov [22h], ax
      0xA1, 0x32, 0x00,              // 3A mov ax, [32h]
      0xA3, 0x24, 0x00,              // 3D mov [24h], ax
      0xB8, 0x07, 0x4C, 0xCD, 0x21,  // 40 mov ax, 4C07h; int 21h
  };
  return build_ne_app(code, {{0x01, 1, 91}, {0x33, 2, 108}});
}

std::string write_temp(const std::string& name, const std::string& bytes) {
  char dir[MAX_PATH];
  GetTempPathA(MAX_PATH, dir);
  std::string p = std::string(dir) + "adw_win16_" + std::to_string(GetCurrentProcessId()) + "_" + name;
  FILE* fh = fopen(p.c_str(), "wb");
  fwrite(bytes.data(), 1, bytes.size(), fh);
  fclose(fh);
  return p;
}

void test_app_task() {
  const std::string path = write_temp("task.exe", task_program());
  {
    // The start and InitTask's contract; GetMessage waits (the frame ends
    // through the yield hook, which posts a message meanwhile).
    Machine m;
    uint16_t err = 0;
    CHECK(!m.rt.modules().load_host(path, &err) && err == 11, "an application is no DLL for LoadLibrary (%u)", err);
    Module16* mod = m.rt.modules().load_task(path, "C:\\APP\\TASK.EXE", " /s", &err);
    const Task16* t = m.rt.modules().task();
    CHECK(mod && t && t->module == mod, "the application loads as the task (error %u)", err);
    if (!mod || !t) return;
    CHECK(!m.rt.modules().load_task(path, "C:\\APP\\TASK.EXE", " /s", &err), "one task per runtime");
    CHECK(t->ss == mod->dgroup && t->sp == 0x440 && t->stack_size == 0x400 && t->heap_size == 0x200,
          "SS:SP is DGROUP's stack top: the static data, then the stack (%04X:%04X)", t->ss, t->sp);
    const uint32_t psp = uint32_t(t->psp) << 16;
    CHECK(t->psp == dos16_psp(m.rt) && m.rt.rd8(psp + 0x80) == 3 && m.rt.read_str(psp + 0x81, 3) == " /s" &&
              m.rt.rd8(psp + 0x84) == 0x0D,
          "the PSP's command tail");
    user16_set_app_task(m.rt, true);
    int yields = 0;
    m.rt.set_yield_hook([&] {
      yields++;
      user16_post_message(m.rt, 0, WM_USER + 1, 0, 0);
      return true;
    });
    std::string why;
    try {
      m.rt.modules().run_task();
    } catch (const GuestError16& e) {
      CHECK(e.kind() == GuestError16::Kind::exit, "the program exits through INT 21h AH=4Ch (%s)", e.what());
      why = e.what();
    }
    CHECK(why.find("code 7") != std::string::npos, "with its code (%s)", why.c_str());
    const uint32_t ds = uint32_t(mod->dgroup) << 16;
    CHECK(m.rt.rd16(ds + 0x10) == 1 && m.rt.rd16(ds + 0x12) == 0x82 && m.rt.rd16(ds + 0x14) == 0x40 + 150 &&
              m.rt.rd16(ds + 0x16) == 1 && m.rt.rd16(ds + 0x18) == 0 && m.rt.rd16(ds + 0x1A) == mod->hinstance &&
              m.rt.rd16(ds + 0x1C) == t->psp,
          "InitTask: AX 1, BX the command line past its blank (%04X), CX pStackTop (%04X), DX nCmdShow, SI 0, DI "
          "hInstance, ES the PSP",
          m.rt.rd16(ds + 0x12), m.rt.rd16(ds + 0x14));
    CHECK(m.rt.rd16(ds + 0x20) == mod->dgroup && m.rt.rd16(ds + 0x1E) == 0x440 - 4,
          "the program runs on its own stack (SS %04X SP %04X)", m.rt.rd16(ds + 0x20), m.rt.rd16(ds + 0x1E));
    CHECK(m.rt.rd16(ds + 0x0A) == 0x40 + 150 && m.rt.rd16(ds + 0x0C) == 0x440 && m.rt.rd16(ds + 0x0E) == 0x440 &&
              m.rt.rd16(ds + 6) == 0x440 && m.rt.local().has_heap(mod->dgroup),
          "the instance data's stack words and the local heap above the stack (pLocalHeap %04X)", m.rt.rd16(ds + 6));
    CHECK(yields == 1 && m.rt.rd16(ds + 0x22) == 1 && m.rt.rd16(ds + 0x24) == WM_USER + 1 && user16_app_waits(m.rt) == 1,
          "GetMessage waited once (the frame ended) and took what came meanwhile (%d, %04X)", yields,
          m.rt.rd16(ds + 0x24));
    CHECK(!m.rt.task_budget(), "the task budget is off once the task has ended");
  }
  {
    // Without an application task: GetMessage's WM_NULL at once, as before.
    Machine m;
    Module16* mod = m.rt.modules().load_task(path, "C:\\APP\\TASK.EXE", "", nullptr);
    CHECK(mod != nullptr, "loads");
    if (!mod) return;
    try {
      m.rt.modules().run_task();
    } catch (const GuestError16&) {
    }
    const uint32_t ds = uint32_t(mod->dgroup) << 16;
    CHECK(m.rt.rd16(ds + 0x12) == 0x80 && m.rt.rd16(ds + 0x22) == 1 && m.rt.rd16(ds + 0x24) == WM_NULL,
          "no task wait: WM_NULL (BX %04X, message %04X)", m.rt.rd16(ds + 0x12), m.rt.rd16(ds + 0x24));
  }
  {
    // An application task with nothing to wait for and no frame to end: an error, never a hang.
    Machine m;
    Module16* mod = m.rt.modules().load_task(path, "C:\\APP\\TASK.EXE", " /s", nullptr);
    CHECK(mod != nullptr, "loads");
    if (!mod) return;
    user16_set_app_task(m.rt, true);
    std::string why;
    try {
      m.rt.modules().run_task();
    } catch (const GuestError16& e) {
      why = e.what();
    }
    CHECK(why.find("nothing can come") != std::string::npos, "a wait for nothing fails (%s)", why.c_str());
  }
  {
    // ... and with a timer, virtual time moves on to it.
    Machine m;
    Module16* mod = m.rt.modules().load_task(path, "C:\\APP\\TASK.EXE", " /s", nullptr);
    CHECK(mod != nullptr, "loads");
    if (!mod) return;
    user16_set_app_task(m.rt, true);
    api(m, "USER", "SetTimer", {w16(0), w16(0), w16(500), l16(0)});
    const uint64_t t0 = m.rt.peek_us();
    try {
      m.rt.modules().run_task();
    } catch (const GuestError16&) {
    }
    const uint32_t ds = uint32_t(mod->dgroup) << 16;
    CHECK(m.rt.rd16(ds + 0x24) == WM_TIMER && m.rt.peek_us() >= t0 + 500000,
          "no frame to end: time moves on to the timer (%04X after %llu us)", m.rt.rd16(ds + 0x24),
          (unsigned long long)(m.rt.peek_us() - t0));
  }
  DeleteFileA(path.c_str());
}

// An application task's windows and the calls Johnny Castaway added.
void test_app_task_calls() {
  Machine m;
  Screen screen(64, 48);
  m.rt.attach_display(screen);
  struct Got {
    uint16_t hwnd, msg, wp;
    uint32_t lp;
  };
  std::vector<Got> got;
  uint32_t erase_result = 1;
  m.rt.shims().add("TESTWP", 1, "APPPROC", Conv16::pascal_, false, 10, [&](Call16& c) {
    Got g{c.w(), c.w(), c.w(), c.l()};
    got.push_back(g);
    if (g.msg == WM_ERASEBKGND) return c.ret32(erase_result);
    if (g.msg == WM_CLOSE || g.msg == WM_PAINT) {
      // DefWindowProc's, as SCRNSAVE.LIB's DefScreenSaverProc passed them on.
      Shim16Entry* d = c.rt.shims().find_name("USER", "DefWindowProc");
      return c.ret32(c.rt.call_far(c.rt.thunk_far(*d), {w16(g.hwnd), w16(g.msg), w16(g.wp), l16(g.lp)}));
    }
    c.ret32(0);
  });
  user16_set_app_task(m.rt, true);
  uint16_t ds = m.data(256);
  uint32_t d = uint32_t(ds) << 16;
  m.rt.write_str(d, "APPCLS", 16);
  m.rt.wr32(d + 0x20 + 2, m.rt.thunk_far(*m.rt.shims().find_name("TESTWP", "APPPROC")));
  m.rt.wr32(d + 0x20 + 22, d);
  api(m, "USER", "RegisterClass", {l16(d + 0x20)});
  uint16_t hwnd = uint16_t(api(m, "USER", "CreateWindow", {l16(d), l16(d), l16(WS_POPUP | WS_VISIBLE), w16(2), w16(3),
                                                             w16(40), w16(30), w16(0), w16(0), w16(0x1234), l16(0)}));
  CHECK(hwnd && got.size() == 3 && got[0].msg == WM_CREATE && got[1].msg == WM_SIZE && got[1].wp == SIZE_RESTORED &&
            got[1].lp == ((30u << 16) | 40) && got[2].msg == WM_MOVE && got[2].lp == ((3u << 16) | 2),
        "an application task's CreateWindow: WM_CREATE, WM_SIZE, WM_MOVE (%zu)", got.size());
  CHECK(user16_main_window(m.rt, 0x1234) == hwnd, "the instance's main window");
  const uint32_t msg = d + 0x80;
  CHECK((api(m, "USER", "GetMessage", {l16(msg), w16(0), w16(0), w16(0)}) & 0xFFFF) && m.rt.rd16(msg) == hwnd &&
            m.rt.rd16(msg + 2) == WM_PAINT,
        "a shown window is invalid: GetMessage brings its WM_PAINT");
  got.clear();
  erase_result = 0;
  const uint32_t ps = d + 0xA0;
  const uint16_t hdc = uint16_t(api(m, "USER", "BeginPaint", {w16(hwnd), l16(ps)}));
  CHECK(hdc && got.size() == 1 && got[0].msg == WM_ERASEBKGND && got[0].wp == hdc && m.rt.rd16(ps + 2) == 1,
        "BeginPaint sends WM_ERASEBKGND with its DC first; left undone, fErase is set");
  api(m, "USER", "EndPaint", {w16(hwnd), l16(ps)});
  got.clear();
  api(m, "USER", "InvalidateRect", {w16(hwnd), l16(0), w16(0)});
  CHECK((api(m, "USER", "PeekMessage", {l16(msg), w16(0), w16(0), w16(0), w16(PM_REMOVE)}) & 0xFFFF) &&
            m.rt.rd16(msg + 2) == WM_PAINT,
        "InvalidateRect: WM_PAINT again");
  api(m, "USER", "DispatchMessage", {l16(msg)});
  CHECK(!(api(m, "USER", "PeekMessage", {l16(msg), w16(0), w16(0), w16(0), w16(PM_REMOVE)}) & 0xFFFF),
        "DefWindowProc's WM_PAINT validates the window");
  api(m, "USER", "SetTimer", {w16(hwnd), w16(1), w16(50), l16(0)});
  got.clear();
  CHECK(!(api(m, "USER", "SendMessage", {w16(hwnd), w16(WM_CLOSE), w16(0), l16(0)}) & 0xFFFF) && !user16_window_exists(m.rt, hwnd),
        "DefWindowProc's WM_CLOSE destroys the window");
  CHECK(got.size() == 2 && got[1].msg == WM_DESTROY, "with its WM_DESTROY (%zu)", got.size());
  m.clock.begin_frame();
  m.clock.begin_frame();
  m.clock.begin_frame();
  m.clock.begin_frame();
  CHECK(!(api(m, "USER", "PeekMessage", {l16(msg), w16(0), w16(0), w16(0), w16(PM_REMOVE)}) & 0xFFFF),
        "its timer died with it");
  // The calls Johnny Castaway's census added.
  CHECK((api(m, "KERNEL", "WaitEvent", {w16(0)}) & 0xFFFF) == 0, "WaitEvent: the start event is there: FALSE at once");
  CHECK((api(m, "USER", "InitApp", {w16(0x1234)}) & 0xFFFF) == 1, "InitApp: the queue is there");
  CHECK((api(m, "USER", "SetCursor", {w16(0x0F00)}) & 0xFFFF) == 0 &&
            (api(m, "USER", "SetCursor", {w16(0)}) & 0xFFFF) == 0x0F00,
        "SetCursor returns the cursor before it");
  CHECK((api(m, "USER", "ShowCursor", {w16(0)}) & 0xFFFF) == 0xFFFF &&
            (api(m, "USER", "ShowCursor", {w16(1)}) & 0xFFFF) == 0,
        "ShowCursor counts");
  const uint32_t rc = d + 0xC0;
  api(m, "USER", "GetClipCursor", {l16(rc)});
  CHECK(m.rt.rd16(rc) == 0 && m.rt.rd16(rc + 4) == 64 && m.rt.rd16(rc + 6) == 48, "no clip: the whole screen");
  m.rt.wr16(rc, 1), m.rt.wr16(rc + 2, 2), m.rt.wr16(rc + 4, 3), m.rt.wr16(rc + 6, 4);
  api(m, "USER", "ClipCursor", {l16(rc)});
  m.rt.wr32(rc, 0), m.rt.wr32(rc + 4, 0);
  api(m, "USER", "GetClipCursor", {l16(rc)});
  CHECK(m.rt.rd16(rc) == 1 && m.rt.rd16(rc + 6) == 4, "ClipCursor's rectangle, emulated state only");
  api(m, "USER", "ClipCursor", {l16(0)});
  api(m, "USER", "GetClipCursor", {l16(rc)});
  CHECK(m.rt.rd16(rc + 4) == 64, "ClipCursor(NULL): the whole screen again");
  m.rt.write_str(d + 0xE0, "WILLY.FON", 16);
  CHECK((api(m, "GDI", "AddFontResource", {l16(d + 0xE0)}) & 0xFFFF) == 0,
        "AddFontResource of a file that is not there: 0");
  CHECK(api(m, "MMSYSTEM", "mciSendCommand", {w16(5), w16(0x0804), l16(0), l16(0)}) == 257,
        "mciSendCommand(MCI_CLOSE) of a device that is not open: MCIERR_INVALID_DEVICE_ID");
  CHECK(api(m, "MMSYSTEM", "mciSendCommand", {w16(5), w16(0x0803), l16(0), l16(0)}) == 257,
        "any command to a device ID that is not open: MCIERR_INVALID_DEVICE_ID");
  user16_set_app_task(m.rt, false);
  api(m, "USER", "WaitMessage", {});
  CHECK(true, "WaitMessage outside an application task returns at once");
  user16_post_message(m.rt, 0, WM_USER, 0, 0);
  user16_set_app_task(m.rt, true);
  api(m, "USER", "WaitMessage", {});
  CHECK(user16_app_waits(m.rt) == 0, "WaitMessage with a message there: no wait");
}

int run_unit() {
  test_app_task();
  test_app_task_calls();
  test_template_converter();
  test_message_table();
  test_keyboard_tables();
  test_hooks();
  test_saver_queue();
  test_overlay16();
  test_ldt();
  test_global();
  test_local();
  test_thunks();
  test_callbacks();
  test_catch_throw();
  test_throw_across_host_levels();
  test_fault_restores_state();
  test_create_window();
  test_dos();
  test_ne_module();
  test_gdi();
  test_desktop();
  test_gdi_extras();
  test_gdi_palette_swap();
  test_seeds();
  test_system_bitmaps();
  test_frame_time();
  test_temp_files();
  test_resources();
  test_dib_driver();
  test_dib_translation();
  test_mono_dib_targets();
  test_gdi_additions();
  test_map_mode();
  test_intersect_clip_rect();
  test_flood_fill();
  test_dib_pal_colors();
  test_getdibits_4bpp();
  test_getdibits_mono();
  test_pattern_brush();
  test_window_queries();
  test_wsprintf_bad_pointer();
  test_choosefont();
  test_wing_signature();
  test_resident_modules();
  test_int2f_vxd();
  test_dispatch_guest();
  test_intermission_seeds();
  test_after_dark2_seeds();
  test_after_dark3_seeds();
  test_heap_spaces_code_handle();
  test_selector_calls();
  test_freed_selector_rule();
  test_toolhelp_walk();
  test_toggle_keys();
  test_dir_list_drives();
  test_configure_placement();
  test_configure_guest_controls();
  test_configure_dialog_clicks();
  test_dos_drives();
  printf("%d/%d checks passed\n", checks - failures, checks);
  return failures ? 1 : 0;
}

// ---- assets: OLDMOD16 + AD_SND --------------------------------------------------------------------------

// The installed assets' win dir: AD_ASSETS_DIR, else the data folder's
// (read-only; core/tests/test_paths.h).
std::string assets_win() {
  const char* a = getenv("AD_ASSETS_DIR");
  std::string root;
  if (a && *a) {
    root = a;
  } else {
    root = adw_test::installed_assets_root();
    if (root.empty()) return {};
  }
  if (GetFileAttributesA((root + "\\win\\FILES").c_str()) != INVALID_FILE_ATTRIBUTES) return root + "\\win";
  if (GetFileAttributesA((root + "\\FILES").c_str()) != INVALID_FILE_ATTRIBUTES) return root;
  return {};
}

int run_assets() {
  std::string win = assets_win();
  if (win.empty()) {
    printf("assets not found: skipped\n");
    return 77;
  }
  std::string engine = win + "\\FILES\\ENGINE", classic = win + "\\FILES\\CLASSIC";
  Machine m;
  Screen screen(640, 480);
  m.rt.attach_display(screen);
  m.rt.vfs().mount("C:\\AFTERDRK", classic, false);
  m.rt.vfs().mount("C:\\WINDOWS\\SYSTEM", engine, false);
  m.rt.vfs().set_cwd("C:\\AFTERDRK");
  m.rt.modules().add_search_dir(engine);
  m.rt.modules().add_search_dir(classic);
  try {
    uint16_t err = 0;
    Module16* om = m.rt.modules().load_host(engine + "\\OLDMOD16.DLL", &err);
    CHECK(om != nullptr, "OLDMOD16.DLL loads (error %u)", err);
    if (om) {
      CHECK(om->dll_entry != 0, "DLLENTRYPOINT was called (reason 1)");
      // Its two blocks were allocated and locked: far pointers at DGROUP:11EC / 11F0.
      uint32_t sys = m.rt.rd32((uint32_t(om->dgroup) << 16) | 0x11EC);
      uint32_t mod = m.rt.rd32((uint32_t(om->dgroup) << 16) | 0x11F0);
      CHECK(sys && mod, "AD_SYSTEM/AD_MODULE locked (%08X %08X)", sys, mod);
      CHECK(m.rt.global().size(uint16_t(sys >> 16)) >= 0x3C && m.rt.global().size(uint16_t(mod >> 16)) >= 0x30,
            "block sizes");
      CHECK(m.rt.modules().proc_address(om, "LOADADMODULE16") && m.rt.modules().proc_address(om, "ModuleMessage16"),
            "exports resolve by name");
    }
    Module16* snd = m.rt.modules().load("ad_snd.dll", &err);
    CHECK(snd != nullptr, "AD_SND.DLL loads and its LibEntry succeeds (error %u)", err);
    if (snd) {
      CHECK(m.rt.modules().proc_address(snd, "adwSoundInit") != 0, "mixed-case GetProcAddress into AD_SND");
    }
    // LoadIcon of a module's own icon (BUGS.AD's RT_GROUP_ICON 42): one
    // shared handle however often it is loaded, as Win16 did, which
    // DestroyIcon leaves alone; CopyIcon makes one of the caller's own.
    Module16* bugs = m.rt.modules().load_host(classic + "\\BUGS.AD", &err);
    CHECK(bugs != nullptr, "BUGS.AD loads (error %u)", err);
    if (bugs) {
      uint16_t first = uint16_t(api(m, "USER", "LoadIcon", {w16(bugs->hinstance), l16(42)}));
      CHECK(first != 0 && first != 0x0F04, "LoadIcon(BUGS, 42): its own icon (%04X)", first);
      bool same = true;
      for (int i = 0; i < 5000; i++) same &= uint16_t(api(m, "USER", "LoadIcon", {w16(bugs->hinstance), l16(42)})) == first;
      CHECK(same, "5,000 more LoadIcons: the same handle");
      uint32_t name = m.rt.static_bytes("t #42", "#42");
      CHECK(uint16_t(api(m, "USER", "LoadIcon", {w16(bugs->hinstance), l16(name)})) == first, "\"#42\" names it too");
      api(m, "USER", "DestroyIcon", {w16(first)});
      CHECK(uint16_t(api(m, "USER", "LoadIcon", {w16(bugs->hinstance), l16(42)})) == first,
            "DestroyIcon leaves the shared icon");
      uint16_t copy = uint16_t(api(m, "USER", "CopyIcon", {w16(bugs->hinstance), w16(first)}));
      CHECK(copy && copy != first, "CopyIcon: a new handle (%04X)", copy);
      api(m, "USER", "DestroyIcon", {w16(copy)});
      uint16_t again = uint16_t(api(m, "USER", "CopyIcon", {w16(bugs->hinstance), w16(first)}));
      CHECK(again == copy, "a destroyed copy's handle is free again (%04X)", again);
    }
    m.rt.modules().free_all();
    CHECK(m.rt.modules().by_name("OLDMOD16") == nullptr, "unloaded (DLLENTRYPOINT(0) ran)");
  } catch (const std::exception& e) {
    CHECK(false, "exception: %s", e.what());
    m.rt.log_state("assets");
  }
  m.rt.shims().print_census("win16 assets test");
  printf("%d/%d checks passed\n", checks - failures, checks);
  return failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 && std::string(argv[1]) == "--assets") return run_assets();
  if (argc > 1 && std::string(argv[1]) == "--cpu-df") return run_cpu_df();
  try {
    return run_unit();
  } catch (const std::exception& e) {
    printf("FAIL: exception %s\n", e.what());
    return 1;
  }
}
