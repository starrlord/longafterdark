// The Win16 module table: NE images loaded with adw::loader::ne onto LDT
// selectors, plus pseudo modules for the system DLLs we emulate.
//
// Loading an NE DLL (LoadLibrary, or an import of one), in the order the
// Win16 KERNEL used:
//   1. place(): one global block (own selector) per segment; the automatic
//      data segment is reserved at 64 KiB so its local heap can grow in place;
//   2. a module database block (hModule): the NE header, 'NE' at offset 0;
//   3. the modules it imports, loaded (and initialized) first;
//   4. load_segments(): relocations resolved through the table — internal
//      references to our selectors, imports of system DLLs to far thunks
//      (constant exports such as __AHINCR to their values), imports of NE
//      DLLs to their entry points (by ordinal, or by name case-insensitively:
//      EINSTEIN imports AD_SND's "adwPlaySound" while AD_SND exports
//      ADWPLAYSOUND, ABI.md §3.6); OSFIXUP records keep the real x87 opcodes;
//   5. prolog patching: exported entries of a SINGLEDATA DLL that begin
//      `push ds; pop ax; nop` (1E 58 90) or `mov ax,ds; nop` (8C D8 90) are
//      rewritten to `mov ax, DGROUP` (ABI.md §3.6), which is what makes a
//      far call into them load the DLL's own DS;
//   6. initialization: the NE entry point (LibEntry: DI = hInstance, DS =
//      DGROUP, CX = heap size, ES:SI = command line; AX = 0 fails the load),
//      then — for a Windows 4.0 DLL that exports it — DLLENTRYPOINT(1, hInst,
//      DS, heap, 0, 0). OLDMOD16 has no NE entry point and needs exactly that
//      explicit call (ABI.md §3.2/§8). Each image is initialized once.
//
// hInstance is the DGROUP selector (or hModule for a DLL without data);
// both find the module (by_handle).
//
// Tasks: a Win16 application (an NE file that is no library: a Windows 3.1
// screen saver's .SCR, the ne16 lane's scrnsave protocol) runs as this
// runtime's one task, started as Windows 3.1's loader started one
// (load_task, then run_task on the lane's guest fiber):
//   * its segments and imports as a DLL's (steps 1–4), its exported entries'
//     prologs patched as a SINGLEDATA DLL's (step 5: one instance, so
//     `mov ax, DGROUP` is what MakeProcInstance's thunk loaded), and no
//     LibEntry: the NE entry point is the task's start (Borland's C0W, …);
//   * DGROUP as Windows laid it out: the static data, the stack (the
//     header's stack size) above it, the local heap (the header's heap size)
//     above that, which KERNEL's InitTask makes (kernel16.cc);
//   * SS:SP the header's (SP 0: the top of the stack area);
//   * the PSP (dos16.hh dos16_psp) holds the command tail; hInstance is the
//     DGROUP selector, hPrevInstance 0;
//   * the start's registers are the loader's: BX the stack size, CX the heap
//     size, DI hInstance, SI hPrevInstance, BP 0, DS DGROUP, ES the PSP.
// run_task returns only if the start returns far (C0W never does: it ends
// with INT 21h AH=4Ch, GuestError16 exit); while it runs, the runtime's
// task budget is on (Runtime16::set_task_budget).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "loader/ne.hh"

namespace adw::win16 {

class Runtime16;
struct Shim16Entry;
struct GlobalBlock;

struct Module16 {
  std::string name;        // resident module name, upper case ("TOASTPRO", "KERNEL")
  std::string file;        // file name, upper case ("TOAST3.AD")
  std::string host_path;   // "" for system modules
  std::string guest_path;  // "C:\\AFTERDRK\\TOAST3.AD"
  bool system = false;     // one of the DLLs we emulate (no image)
  std::shared_ptr<loader::ne::Image> image;
  std::vector<uint16_t> seg_sel;   // per segment (index 0 = segment 1)
  std::vector<uint32_t> seg_base;  // linear
  uint16_t hmodule = 0;
  uint16_t hinstance = 0;
  uint16_t dgroup = 0;             // 0 when the module has no automatic data segment
  int refs = 0;
  bool initialized = false;
  uint32_t dll_entry = 0;          // DLLENTRYPOINT, when it was called with reason 1 (called with 0 at unload)
  std::vector<Module16*> deps;     // what it imports (one reference each)

  bool is_dll() const { return image && image->header().is_dll(); }
};

// The task (see Tasks above): its module and what its start and InitTask hand it.
struct Task16 {
  Module16* module = nullptr;
  uint16_t psp = 0;                  // the PSP's selector (ES)
  uint16_t ss = 0, sp = 0;           // the start's stack
  uint16_t stack_size = 0, heap_size = 0;
  uint16_t stack_low = 0;            // the stack area's lowest offset (the static data's end)
  uint16_t cmd_show = 1;             // nCmdShow: SW_SHOWNORMAL
  std::string command_tail;          // as the PSP holds it (" /s")
};

class ModuleTable16 {
 public:
  explicit ModuleTable16(Runtime16& rt);
  ~ModuleTable16();

  // Loads the application at host_path (its guest path `guest_path`) as the
  // task, `command_tail` its PSP's command tail (DOS style, " /s"). Null on
  // failure (*err: 2 not found, 11 not an application or not loadable, 14
  // out of memory), or when a task is loaded already.
  Module16* load_task(const std::string& host_path, const std::string& guest_path, const std::string& command_tail,
                      uint16_t* err = nullptr);
  const Task16* task() const { return task_.module ? &task_ : nullptr; }
  // Starts the task (see Tasks above) and returns DX:AX if its start returns.
  uint32_t run_task();

  // Host directories searched for a bare DLL name after the guest's
  // directories (current, Windows, System) resolve nowhere.
  void add_search_dir(const std::string& host_dir);

  // LoadLibrary: a bare name ("ad_snd.dll", "AD_RSRC"), or a guest path.
  // Loads and initializes the module and what it imports; a module already
  // loaded just gains a reference. On failure returns null with *err set to
  // the Win16 LoadLibrary error (2 file not found, 11 invalid exe, 14 out of
  // memory, 20 a DLL failed to initialize).
  Module16* load(std::string_view name, uint16_t* err = nullptr);
  // Same, from a host path (the lane loads OLDMOD16.DLL this way).
  Module16* load_host(const std::string& host_path, uint16_t* err = nullptr);
  // FreeLibrary: drops a reference; the last one unloads (and releases the
  // modules it imported). DLLENTRYPOINT(0) runs for the modules that got 1.
  bool free(Module16* m);
  void free_all();

  Module16* by_handle(uint16_t h);          // hModule or hInstance
  Module16* by_name(std::string_view name);  // module or file name, any case, with or without extension
  Module16* containing(uint16_t sel);        // the module one of whose segments sel is
  int segment_index(const Module16& m, uint16_t sel) const;  // 1-based, 0 if not its segment

  // GetProcAddress: sel:off, 0 if none. Names compare case-insensitively.
  uint32_t proc_address(Module16* m, std::string_view name);
  uint32_t proc_address(Module16* m, uint16_t ordinal);

  const loader::ne::Resource* find_resource(Module16* m, const loader::ResId& type, const loader::ResId& name);

  const std::vector<std::unique_ptr<Module16>>& all() const { return modules_; }

 private:
  Module16* system_module(const std::string& name);
  std::string find_file(std::string_view name, std::string* guest_path);
  Module16* load_file(const std::string& host_path, const std::string& guest_path, uint16_t* err, bool task = false);
  bool initialize(Module16* m);
  loader::ne::FarPtr resolve(Module16* m, const loader::ne::Target& t);
  loader::ne::FarPtr resolve_import(std::string_view module, uint16_t ordinal, std::string_view name);
  // The value of a system DLL's constant export (__AHINCR, __0040H, …).
  uint32_t equate_value(const Shim16Entry& e);
  uint32_t entry_far(Module16* m, uint16_t ordinal);
  uint16_t fixed_selector(uint32_t linear, const char* tag);
  void unload(Module16* m);

  Runtime16& rt_;
  std::vector<std::string> search_dirs_;
  std::vector<std::unique_ptr<Module16>> modules_;
  std::vector<std::pair<uint32_t, uint16_t>> fixed_sels_;  // __A000H & co.
  Task16 task_;
};

}  // namespace adw::win16
