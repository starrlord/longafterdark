// The Windows 3.1 screen-saver protocol (protocol.hh, lane.hh "Windows 3.1
// screen savers"): a .SCR built on Microsoft's SCRNSAVE.LIB — an NE
// application exporting SCREENSAVERPROC, Johnny Castaway's SCRANTIC.SCR — run
// as Windows 3.1 ran it: the program itself, unchanged, as the runtime's task
// (win16/modules16.hh "Tasks") with the command line Control Panel gave it,
// "/s" for the saver and "/c" for its Setup... button. The program owns its
// message loop (SCRNSAVE.LIB's WinMain: RegisterClass, a full-screen
// WS_POPUP window, GetMessage/TranslateMessage/DispatchMessage), so the
// protocol's one call is the task: it starts on the lane's guest fiber and
// never returns while the program runs; every frame ends inside it — at the
// frame's deadline in an API call (Long calls), or where its GetMessage
// finds nothing to deliver (win16 user16_set_app_task) — and the next frame
// resumes it there.
#include <windows.h>

#include <cinttypes>
#include <cstdio>

#include "adw/core/log.h"
#include "adw/core/text.h"
#include "ne16/protocol.hh"
#include "win16/dos16.hh"
#include "win16/modules16.hh"
#include "win16/runtime16.hh"
#include "win16/shim_families16.hh"
#include "win32/vfs.hh"

namespace adw::ne16 {

using win16::GuestError16;
using win16::Runtime16;

namespace {

std::string file_of(const std::string& p) {
  size_t s = p.find_last_of("\\/");
  return s == std::string::npos ? p : p.substr(s + 1);
}

// SCRNSAVE.LIB's command lines (Windows 3.1's Control Panel ran the saver
// "<file> /s" and its Setup... "<file> /c"), as the PSP's command tail.
constexpr const char* kSaveTail = " /s";
constexpr const char* kConfigureTail = " /c";

}  // namespace

std::string scr_install_dir(const Ne16Layout& layout) {
  const std::string file = win16::upper16(file_of(layout.module_path));
  // Johnny Castaway's installer (INSTALL.INS): "\SIERRA\SCRANTIC" on the drive the user picked.
  if (file == "SCRANTIC.SCR") return "C:\\SIERRA\\SCRANTIC";
  return "C:\\" + win16::upper16(file_of(layout.module_dir));
}

// The guest's disk (INTERACTION.md §7.2), one Windows 3.1 machine with the
// saver installed:
//   <install dir>      the module's folder (C:\SIERRA\SCRANTIC, the
//                      installer's default destination: SCRANTIC.SCR,
//                      RESOURCE.MAP, RESOURCE.001) and the current
//                      directory, under a copy-on-write upper layer
//                      <state>\<package>\<MODDIR>
//   C:\WINDOWS         over the package's windows dir when it has one and
//                      the runtime's virtual seed files, under
//                      <state>\<package>\WINDOWS: SCRANTIC.INI, which the
//                      program reads at WM_CREATE and writes at WM_DESTROY
//                      (its story's day and date, the introduction shown)
//                      and from Setup..., lands there
//   C:\WINDOWS\SYSTEM  the engine dir (none here), read-only
//   H:\<L>\…           the host's drives, read-only, 8.3 names
// Without ADSTATE every upper layer is memory: a headless run starts from
// the installer's state every time. The profile seeds (win16/dos16.hh
// seed_scrnsave): WIN.INI's and SYSTEM.INI's screen-saver lines, and
// SCRANTIC.INI's SourceDir for SCRANTIC.SCR.
void mount_scr_disk(Runtime16& rt, const Env& env, const Ne16Layout& layout) {
  const win16::Runtime16Options& o = rt.options();
  win32::Vfs& vfs = rt.vfs();
  std::string pkg = package_state_dir(env, layout.module_path);
  std::string moddir = file_of(layout.module_dir);
  if (env.state_persistent()) vfs.set_state_root(env.state_root);
  vfs.mount_overlay(o.windows_dir, layout.windows_dir, pkg.empty() ? "" : pkg + "\\WINDOWS");
  vfs.mount_overlay(o.guest_dir, layout.module_dir, pkg.empty() ? "" : pkg + "\\" + moddir);
  vfs.mount(o.system_dir, layout.engine_dir, /*writable=*/false);
  vfs.mount_host_drives(/*short_names=*/true);
  vfs.set_cwd(o.guest_dir);
  for (const std::string& d : layout.search_dirs) rt.modules().add_search_dir(d);
  win16::ScrnsaveSeeds seeds;
  const std::string file = win16::upper16(file_of(layout.module_path));
  seeds.program = o.guest_dir + "\\" + file;
  if (file == "SCRANTIC.SCR") seeds.source_dir = o.guest_dir;
  win16::seed_scrnsave(rt, seeds);
  trace("lane", "disk: %s and C:\\WINDOWS over %s%s; seeds: SYSTEM.INI SCRNSAVE.EXE=%s%s", o.guest_dir.c_str(),
        pkg.empty() ? "memory (no ADSTATE)" : (pkg + " (" + moddir + ")").c_str(),
        layout.windows_dir.empty() ? "" : (", C:\\WINDOWS over " + layout.windows_dir).c_str(), seeds.program.c_str(),
        seeds.source_dir.empty() ? "" : (", SCRANTIC.INI SourceDir=" + seeds.source_dir).c_str());
}

namespace {

class ScrProtocol : public Protocol16 {
 public:
  explicit ScrProtocol(const Ne16Layout& layout) : layout_(layout), module_name_(file_of(layout.module_path)) {}

  const char* name() const override { return "scr"; }
  void configure_runtime(win16::Runtime16Options& opts, LaneContext& ctx) override;
  void mount(Runtime16& rt, const Env& env) override { mount_scr_disk(rt, env, layout_); }
  bool load(Runtime16& rt, uint16_t hwnd, uint16_t hdc, LaneContext& ctx) override;
  Call call() override;
  // A Windows 3.1 saver closed itself on a key or a mouse move
  // (DefScreenSaverProc); the host's saver ends the run on input itself, so
  // the program gets neither (lane.hh "Windows 3.1 screen savers").
  bool takes_key_messages() const override { return false; }
  bool takes_mouse_messages() const override { return false; }
  bool runs_as_task() const override { return true; }
  bool close_suspended() override;
  void unload() override;
  std::string error_text() const override { return stop_text_; }
  bool check_button(int slot, std::string* why) override;
  void configure_button_runtime(win16::Runtime16Options& opts, const Env& env) override;
  Button button(Runtime16& rt, int slot, uint16_t owner16, LaneContext& ctx) override;

 private:
  bool load_task(Runtime16& rt, const char* tail, std::string* why);
  // Runs the task to its end: true when it ended (stop_text_ says how).
  bool run();

  Ne16Layout layout_;
  std::string module_name_;
  Runtime16* rt_ = nullptr;
  uint16_t hinstance_ = 0;
  bool started_ = false, ended_ = false, closing_ = false;
  int exit_code_ = 0;
  std::string stop_text_;
};

void ScrProtocol::configure_runtime(win16::Runtime16Options& opts, LaneContext& ctx) {
  (void)ctx;
  opts.guest_dir = scr_install_dir(layout_);
}

bool ScrProtocol::load_task(Runtime16& rt, const char* tail, std::string* why) {
  rt_ = &rt;
  std::string guest = rt.vfs().to_guest(layout_.module_path);
  if (guest.empty()) guest = rt.options().guest_dir + "\\" + win16::upper16(module_name_);
  uint16_t err = 0;
  win16::Module16* m = rt.modules().load_task(layout_.module_path, guest, tail, &err);
  if (!m || !rt.modules().task()) {
    *why = "cannot load the program as a task (error " + std::to_string(err) + ")";
    return false;
  }
  hinstance_ = m->hinstance;
  // Its message loop waits as Windows 3.1's did (win16 user16_set_app_task).
  win16::user16_set_app_task(rt, true);
  trace("lane", "%s: package %s, module dir %s, kind scr, \"%s\" (%s) as %s, the task's command tail \"%s\"",
        module_name_.c_str(), layout_.packaged ? layout_.package_id.c_str() : "legacy", layout_.module_dir.c_str(),
        m->name.c_str(), m->image->description().c_str(), guest.c_str(), tail);
  return true;
}

bool ScrProtocol::load(Runtime16& rt, uint16_t hwnd, uint16_t hdc, LaneContext& ctx) {
  (void)hwnd;  // the program makes its own full-screen window
  (void)hdc;
  (void)ctx;
  std::string why;
  if (!load_task(rt, kSaveTail, &why)) {
    log("%s: %s", module_name_.c_str(), why.c_str());
    return false;
  }
  return true;
}

bool ScrProtocol::run() {
  try {
    uint32_t r = rt_->modules().run_task();
    // C0W never returns far; a start that does is over all the same.
    exit_code_ = int(r & 0xFFFF);
    stop_text_ = "the program's start returned (" + std::to_string(exit_code_) + ")";
  } catch (const GuestError16& e) {
    // INT 21h AH=4Ch (the C runtime's exit), FatalExit, FatalAppExit: the end.
    if (e.kind() != GuestError16::Kind::exit) throw;
    stop_text_ = std::string("the program ended: ") + e.what();
  }
  ended_ = true;
  const std::string box = win16::user16_last_message_box(*rt_);
  if (!box.empty()) stop_text_ += "; its last message box: " + box;
  return true;
}

// The task, started at the first call; it returns only when the program has
// ended (WM_QUIT, WinMain returned, INT 21h AH=4Ch, its MessageBox for
// missing data files first), which stops the run with what it said.
Protocol16::Call ScrProtocol::call() {
  if (!started_) {
    started_ = true;
    run();
  }
  Call c;
  c.kind = Call::Kind::stop;
  c.code = exit_code_;
  return c;
}

// At shutdown: the program closes as on the input that woke a Windows 3.1
// saver — SCRNSAVE.LIB's close routine (SCRANTIC 5:019c) sent its window
// WM_CLOSE, so DefWindowProc destroyed it, its WM_DESTROY saved the story to
// SCRANTIC.INI and posted WM_QUIT, and the program exited —; here it is
// posted, and the lane resumes the task to that end (Ne16Lane::wind_down).
bool ScrProtocol::close_suspended() {
  if (!rt_ || !started_ || ended_) return false;
  const uint16_t main = win16::user16_main_window(*rt_, hinstance_);
  if (!main) return false;
  closing_ = true;
  win16::user16_post_message(*rt_, main, WM_CLOSE, 0, 0);
  trace("lane", "%s: closing: WM_CLOSE posted to the program's window %04X", module_name_.c_str(), main);
  return true;
}

void ScrProtocol::unload() {
  if (!rt_) return;
  trace("lane", "%s: %s; %" PRIu64 " message waits", module_name_.c_str(),
        ended_ ? stop_text_.c_str() : started_ ? "the program was still running" : "the program never started",
        win16::user16_app_waits(*rt_));
}

// Configure mode (lane.hh "Configure"): one button, slot 0 — Windows 3.1's
// Control Panel's Setup..., which ran the program with "/c".
bool ScrProtocol::check_button(int slot, std::string* why) {
  if (slot == 0) return true;
  *why = "control " + std::to_string(slot) + " is not a button";
  return false;
}

void ScrProtocol::configure_button_runtime(win16::Runtime16Options& opts, const Env& env) {
  (void)env;
  opts.guest_dir = scr_install_dir(layout_);
}

// SCRNSAVE.LIB's WinMain with "/c": its SCREENSAVERCONFIGURE dialog
// (DialogBox with no owner: the real dialog's owner is the --owner), then
// WinMain returns and the C runtime exits. The whole program runs inside
// this call, on the host's thread (no frames: its message waits, if any,
// move the wall clock's time on).
Protocol16::Button ScrProtocol::button(Runtime16& rt, int slot, uint16_t owner16, LaneContext& ctx) {
  (void)slot;
  (void)ctx;
  Button out;
  std::string why;
  if (!load_task(rt, kConfigureTail, &why)) {
    out.message = why;
    return out;
  }
  out.ran = true;
  started_ = true;
  run();
  trace("lane", "%s: Setup... (owner %04X): %s", module_name_.c_str(), owner16, stop_text_.c_str());
  if (exit_code_) out.message = stop_text_;
  return out;
}

}  // namespace

std::unique_ptr<Protocol16> make_scr_protocol(const Ne16Layout& layout) { return std::make_unique<ScrProtocol>(layout); }

}  // namespace adw::ne16
