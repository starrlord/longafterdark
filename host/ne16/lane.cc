#include "ne16/lane.hh"

#include <windows.h>

#include <algorithm>
#include <cinttypes>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "adw/core/audio.h"
#include "adw/core/log.h"
#include "adw/core/text.h"
#include "loader/ne.hh"
#include "win16/dialogs16.hh"
#include "win16/gdi16.hh"
#include "win16/input16.hh"
#include "win16/modules16.hh"
#include "win16/runtime16.hh"
#include "win16/shim_families16.hh"
#include "win16/sound16.hh"
#include "win32/config_script.hh"
#include "win32/display.hh"
#include "win32/vfs.hh"

namespace adw::ne16 {

using win16::GuestError16;
using win16::Runtime16;
using win16::l16;
using win16::w16;

namespace {

std::string full_path(const std::string& p) {
  std::wstring w = widen(p);
  DWORD n = GetFullPathNameW(w.c_str(), 0, nullptr, nullptr);
  if (!n) return p;
  std::wstring out(n, L'\0');
  n = GetFullPathNameW(w.c_str(), n, out.data(), nullptr);
  out.resize(n);
  return narrow(out);
}

std::string file_of(const std::string& p) {
  size_t s = p.find_last_of("\\/");
  return s == std::string::npos ? p : p.substr(s + 1);
}

uint64_t env_u64(const Env& env, const char* name, uint64_t def) {
  const std::string* v = env.get(name);
  if (!v || v->empty()) return def;
  char* end = nullptr;
  double d = strtod(v->c_str(), &end);  // accepts 1e9 as well as plain integers
  if (end == v->c_str() || d < 0) {
    log("%s='%s' is not a number; using %" PRIu64, name, v->c_str(), def);
    return def;
  }
  return uint64_t(d);
}

bool file_exists(const std::string& p) {
  DWORD a = GetFileAttributesW(widen(p).c_str());
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool dir_exists(const std::string& p) {
  DWORD a = GetFileAttributesW(widen(p).c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// The file starts with an Intermission ASA animation's header (package.hh "Form").
bool asa_file(const std::string& p) {
  HANDLE h = CreateFileW(widen(p).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  char head[4] = {};
  DWORD got = 0;
  const bool ok = ReadFile(h, head, sizeof(head), &got, nullptr) && got == sizeof(head);
  CloseHandle(h);
  return ok && asa_header(head);
}

}  // namespace

std::unique_ptr<Protocol16> Ne16Lane::choose_protocol(const Ne16Layout& layout, const Env& env, std::string* why) {
  // An ASA animation is data, so no export can say what it is: its header does.
  if (asa_file(layout.module_path)) {
    if (env.get("ADNE16KIND")) {
      log("%s: ADNE16KIND is ignored: an ASA animation is an Intermission module", file_of(layout.module_path).c_str());
    }
    return make_imx_protocol(layout, ImxForm::asa);
  }
  bool is_auto = true;
  ModuleKind kind = ModuleKind::ad3;
  ImxForm form = ImxForm::imx;
  if (const std::string* k = env.get("ADNE16KIND")) {
    if (!parse_kind_choice(*k, &is_auto, &kind)) {
      log("ADNE16KIND='%s' is not auto, ad3, imx or scr; using auto", k->c_str());
      is_auto = true;
    }
  }
  std::unique_ptr<loader::ne::Image> img;
  if (is_auto || kind == ModuleKind::imx) {
    try {
      img = std::make_unique<loader::ne::Image>(loader::ne::Image::from_file(layout.module_path));
    } catch (const std::exception&) {
      // Not a readable NE image: the AD3 protocol says so, as before (forced, the IMX protocol's reader does).
      if (is_auto) return make_ad3_protocol(layout);
    }
  }
  if (is_auto) {
    KindProbe p = detect_kind(*img, file_of(layout.module_path));
    if (!p.ok) {
      *why = p.why;
      return nullptr;
    }
    kind = p.kind;
    form = p.form;
  } else {
    // Forced: the form still follows the exports (an .IMQ is its own reader).
    if (kind == ModuleKind::imx && img) {
      KindProbe p = detect_kind(*img, file_of(layout.module_path));
      if (p.ok && p.kind == ModuleKind::imx) form = p.form;
    }
    trace("lane", "%s: kind %s (ADNE16KIND)", file_of(layout.module_path).c_str(), kind_name(kind));
  }
  if (kind == ModuleKind::scr) return make_scr_protocol(layout);
  return kind == ModuleKind::imx ? make_imx_protocol(layout, form) : make_ad3_protocol(layout);
}

Ne16Lane::Ne16Lane() : Ne16Lane(&Ne16Lane::choose_protocol) {}

Ne16Lane::Ne16Lane(ProtocolFactory make_protocol) : make_protocol_(std::move(make_protocol)) {}

Ne16Lane::~Ne16Lane() {
  proto_.reset();
  rt_.reset();
  free_fibers();
}

// A failed init drops the runtime at once: it refers to ctx's screen, clock
// and input, which run_host destroys before the lane (lane.h).
bool Ne16Lane::init(const std::string& module_path, LaneContext& ctx) {
  if (init_impl(module_path, ctx)) return true;
  proto_.reset();
  rt_.reset();
  free_fibers();
  loaded_ = false;
  ctx_ = nullptr;
  return false;
}

bool Ne16Lane::init_impl(const std::string& module_path, LaneContext& ctx) {
  ctx_ = &ctx;
  const Env& env = ctx.env;
  std::string path = full_path(module_path);
  module_name_ = file_of(path);
  // Where the engine files come from (package.hh, PACKAGES.md §7.1/§7.3).
  layout_ = resolve_layout(path, env.win_assets_dir(), file_exists, dir_exists);
  // The module's protocol (protocol.hh) and its choices, before anything
  // else: AD3's bridge, volume and mute, IMX's reader and volume, and the
  // guest dir and the display's starting palette.
  std::string refused;
  proto_ = make_protocol_(layout_, env, &refused);
  if (!proto_) {
    log("%s: %s", module_name_.c_str(), refused.c_str());
    return false;
  }
  win16::Runtime16Options opts;
  proto_->configure_runtime(opts, ctx);

  opts.arena_size = uint32_t(std::clamp<uint64_t>(env_u64(env, "ADHEAPMB", 64), 16, 512) << 20);
  opts.call_budget = env_u64(env, "ADCALLBUDGET", 1'000'000'000ull);
  opts.tick_quantum_ms = uint32_t(std::clamp<uint64_t>(env_u64(env, "ADTICKMS", 55), 1, 1000));
  if (env.get("ADSOUNDDEV")) opts.sound_device = env.flag("ADSOUNDDEV");
  // The display's starting palette is the protocol's
  // (Runtime16Options::desktop_palette); ADDESKTOPPAL=0/1 overrides it.
  if (env.get("ADDESKTOPPAL")) opts.desktop_palette = env.flag("ADDESKTOPPAL");
  if (const std::string* w = env.get("ADSTEP16")) {
    unsigned cs = 0, lo = 0, hi = 0;
    if (sscanf(w->c_str(), "%x:%x-%x", &cs, &lo, &hi) == 3) {
      opts.step_cs = uint16_t(cs);
      opts.step_lo = uint16_t(lo);
      opts.step_hi = uint16_t(hi);
    }
  }
  if (const std::string* w = env.get("ADWATCH16")) {
    unsigned sel = 0, off = 0;
    if (sscanf(w->c_str(), "%x:%x", &sel, &off) == 2) opts.watch = (sel << 16) | (off & 0xFFFF);
  }
  // Deadline loops and CPU-speed calibrations inside one call must see time
  // move (ABI.md §4): per clock read, and per instruction executed.
  opts.read_step_us = uint32_t(env_u64(env, "ADREADSTEPUS", 5));
  opts.insns_per_us = uint32_t(env_u64(env, "ADMIPS", 100));
  opts.api_cost_insns = uint32_t(env_u64(env, "ADAPICOST", 500));
  // What a blit's or fill's pixel costs the DRAWFRAME budget is the
  // protocol's (Protocol16::pixel_cost, lane.hh "Pacing"): After Dark's
  // ADPIXCOST, an Intermission module's ADNE16IMXPIXCOST; neither knob
  // changes the other's cost.
  const Protocol16::PixelCost pixel = proto_->pixel_cost();
  opts.pixel_cost_insns = uint32_t(env_u64(env, pixel.knob, pixel.def));
  if (strcmp(pixel.knob, "ADPIXCOST") != 0) {
    if (env.get("ADPIXCOST")) {
      log("%s: ADPIXCOST is ignored: it is the After Dark modules' pixel cost; this module's is %s",
          module_name_.c_str(), pixel.knob);
    }
    // The knob's text as given: a value env_u64 refused shows beside the
    // default it fell back to.
    const std::string* raw = env.get(pixel.knob);
    trace("lane", "%s: a pixel a blit or fill writes costs %" PRIu32 " (%s%s%s)", module_name_.c_str(),
          opts.pixel_cost_insns, raw ? pixel.knob : "the protocol's default", raw ? "=" : "", raw ? raw->c_str() : "");
  }
  // Pacing (lane.hh): with ADMIPS set, headless time is Runtime16's
  // frame-bounded model, which applies the read step itself, and streamed
  // time is the wall clock as it is (a run of DRAWFRAMEs reads the clock
  // many times a frame; nudging each read would make it outrun the wall).
  // With ADMIPS=0 the core clock nudges every read, as the lane always did.
  ctx.clock.set_read_step_us(opts.insns_per_us ? 0 : opts.read_step_us);
  // The DRAWFRAME budget: ADDRAWMIPS (default 25) of the ADMIPS machine's
  // frame period — a 486-class share, what the emulator sustains in real
  // time (a 100-MIPS budget kept busy costs more than a frame of host time).
  draw_mips_ = std::min<uint64_t>(env_u64(env, "ADDRAWMIPS", 25), opts.insns_per_us);
  max_draws_ = opts.insns_per_us ? uint32_t(std::clamp<uint64_t>(env_u64(env, "ADMAXDRAWS", 64), 1, 100000)) : 1;
  // Long calls (lane.hh): on with the virtual CPU; ADNE16LONGCALLS=0 turns them off.
  long_calls_ = opts.insns_per_us && !(env.get("ADNE16LONGCALLS") && !env.flag("ADNE16LONGCALLS"));
  // A protocol whose one call lasts the whole run (scr: the program's task)
  // has its frames end inside it, so it cannot run without them.
  if (proto_->runs_as_task() && !long_calls_) {
    log("%s: this module runs only with long calls (its program's task is one call that lasts the run); "
        "ADMIPS=0 and ADNE16LONGCALLS=0 turn them off",
        module_name_.c_str());
    return false;
  }
  // Carried overruns (lane.hh "Pacing"): the protocol's choice, with the
  // DRAWFRAME budget on; ADNE16IMXCARRY=0 turns them off.
  carry_ = proto_->carries_overruns() && draw_mips_ && !(env.get("ADNE16IMXCARRY") && !env.flag("ADNE16IMXCARRY"));
  if (proto_->carries_overruns()) {
    const std::string carried = "carried into the next frames (below " + std::to_string(kMaxOwedBudgets) + " budgets owed)";
    trace("lane", "%s: overruns %s", module_name_.c_str(),
          carry_       ? carried.c_str()
          : draw_mips_ ? "not carried (ADNE16IMXCARRY=0)"
                       : "not carried (no DRAWFRAME budget)");
  }
  // Small screens (lane.hh): the guest gets a display k times the output.
  guest_scale_ = auto_guest_scale(ctx.screen.width(), ctx.screen.height());
  if (const std::string* s = env.get("ADNE16SCALE"); s && !s->empty() && *s != "auto") {
    char* end = nullptr;
    long k = strtol(s->c_str(), &end, 10);
    if (end == s->c_str() || *end || k < 1 || k > 8) {
      log("ADNE16SCALE='%s' is not auto or 1..8; using auto (%d)", s->c_str(), guest_scale_);
    } else {
      guest_scale_ = int(k);
    }
  }
  if (int64_t(ctx.screen.width()) * guest_scale_ > Screen::kMaxDim ||
      int64_t(ctx.screen.height()) * guest_scale_ > Screen::kMaxDim) {
    guest_scale_ = 1;
  }
  if (guest_scale_ > 1) {
    guest_screen_ = std::make_unique<Screen>(ctx.screen.width() * guest_scale_, ctx.screen.height() * guest_scale_);
    sync_input();
    trace("lane", "%dx%d output: the guest display is %dx%d (x%d), averaged down each frame", ctx.screen.width(),
          ctx.screen.height(), guest_screen_->width(), guest_screen_->height(), guest_scale_);
  }

  try {
    rt_ = std::make_unique<Runtime16>(opts, ctx.clock, guest_screen_ ? &guest_input_ : &ctx.input);
    Runtime16& rt = *rt_;
    win16::register_all16(rt);
    // MMSYSTEM's sound half over the host audio engine (win16/sound16.hh);
    // without an enabled one, the silent device the lane always had.
    win16::attach_audio16(rt, ctx.audio);
    proto_->mount(rt, env);
    win32::Display& display = rt.attach_display(guest_screen_ ? *guest_screen_ : ctx.screen);
    // The desktop the saver started over (win32/display.hh "desktop seed"),
    // as in the pe32 lane. AFTERDAR.SCR's full-screen saver window never
    // erases itself (WM_ERASEBKGND 0x401466 returns 1 without painting unless
    // it is the /p preview), so the screen held the desktop when OLDMOD16 sent
    // BLANK, and each module's own BLANK decides what survives — Puzzle, Punch
    // Out, Spotlight, Down the Drain and Mowin' Man work on it; Zooommm! and
    // Mowin' Boris blank it (PACKAGES.md §12 has the census list). Off unless asked
    // for, so unseeded runs stay byte-identical.
    if (const std::string* s = env.get("ADSEEDIMG"); s && !s->empty() && !env.flag("ADNOSEED")) {
      std::string what;
      if (display.seed(*s, &what)) {
        trace("lane", "desktop seed: %s", what.c_str());
      } else {
        log("ADSEEDIMG: %s; the screen starts black", what.c_str());
      }
    }
    rt.set_scanout_hook([this] { on_scanout(); });

    hwnd_ = win16::user16_saver_window(rt);
    hdc_ = win16::gdi16_screen_dc(rt, hwnd_);
    if (!hdc_) throw std::runtime_error("no screen DC");

    // The module, through its protocol (AD3: the bridge, the AD palettes, the
    // controls, LOADADMODULE16); it has logged why when it fails.
    if (!proto_->load(rt, hwnd_, hdc_, ctx)) {
      census();
      return false;
    }
    loaded_ = true;
    // The AD 3.x engines (ADXPL40, ADXPL310) hook the keyboard as they load.
    hooked_ = win16::user16_has_keyboard_hook(rt);
    // Long calls (lane.hh): the frame's DRAWFRAMEs run on a fiber of their own.
    if (long_calls_) {
      if (IsThreadAFiber()) {
        host_fiber_ = GetCurrentFiber();
      } else {
        host_fiber_ = ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH);
        converted_thread_ = host_fiber_ != nullptr;
      }
      // The host's own 8 MB (the top-level CMakeLists.txt): shims and nested guest
      // calls recurse on it as they do on the main thread.
      if (host_fiber_) guest_fiber_ = CreateFiberEx(0, 8u << 20, FIBER_FLAG_FLOAT_SWITCH, &Ne16Lane::fiber_main, this);
      if (!guest_fiber_) {
        log("%s: no fiber for long calls (error %lu); every DRAWFRAME ends its frame", module_name_.c_str(), GetLastError());
        long_calls_ = false;
      }
    }
    if (proto_->runs_as_task() && !long_calls_) {
      census();
      return false;
    }
    // A synchronous sndPlaySound lasts its sound's duration (lane.hh "Sound"):
    // the frames go on meanwhile, as in a long call.
    if (long_calls_) rt.set_yield_hook([this] { return suspend_frame(); });
    present();
    return true;
  } catch (const GuestError16& e) {
    log("%s: %s", module_name_.c_str(), e.what());
    if (rt_) rt_->log_state("guest state");
  } catch (const loader::LoaderError& e) {
    log("%s: cannot load: %s", module_name_.c_str(), e.what());
  } catch (const std::exception& e) {
    log("%s: %s", module_name_.c_str(), e.what());
    if (rt_) rt_->log_state("guest state");
  }
  census();
  return false;
}

// Transient content. The host shows the screen as each DRAWFRAME leaves it,
// but a 1996 monitor showed it at every refresh during the call too: a
// module that draws, waits in a CPU delay loop and erases within one
// DRAWFRAME (ZOT's lightning bolts) was seen for a refresh or so, and would
// never be seen here. So the runtime reports every virtual 70 Hz refresh
// (Runtime16::set_scanout_hook); when a DRAWFRAME ends exactly where it
// began but a refresh during it showed something else, the frame presented
// is that refresh — and the real screen goes back before the guest runs
// again. Steps that change the screen are presented as they end, as before.
void Ne16Lane::on_scanout() {
  if (!in_step_) return;
  win32::Display* d = rt_->display();
  if (!d || start_bits_.empty()) return;
  GdiFlush();
  if (memcmp(d->bits(), start_bits_.data(), start_bits_.size()) != 0) {
    latched_bits_.assign(d->bits(), d->bits() + start_bits_.size());
    latched_ = true;
  }
}

void Ne16Lane::settle_screen() {
  if (!showing_latched_) return;
  showing_latched_ = false;
  if (win32::Display* d = rt_->display()) memcpy(d->bits(), start_bits_.data(), start_bits_.size());
}

void Ne16Lane::on_command(const Command& c) {
  // A module that woke the saver is called no more (step()): input and SET go nowhere.
  if (!rt_ || !loaded_ || woke_) return;
  sync_input();
  settle_screen();
  if (c.kind == Command::Kind::key || c.kind == Command::Kind::caps || c.kind == Command::Kind::numlock ||
      c.kind == Command::Kind::mouse) {
    // An interactive module takes every input line as its own (INTERACTION.md §5.2).
    if (wants_events_ && c.seq > eaten_) eaten_ = c.seq;
    queue_input(c);
    return;
  }
  // SET: the protocol's to take (AD3: control values 0..3).
  if (c.kind == Command::Kind::set && proto_->set_control(c.a, c.b)) {
    // Mid-DRAWFRAME (a long call the last frame ended inside) the guest cannot
    // be called: the values go to the module before its next DRAWFRAME.
    if (suspended_) {
      controls_pending_ = true;
      return;
    }
    try {
      proto_->send_controls();
    } catch (const std::exception& e) {
      log("%s: SET %d %d: %s", module_name_.c_str(), c.a, c.b, e.what());
    }
  }
}

// ---- input (lane.hh "Input and status") ----

void Ne16Lane::queue_input(const Command& c) {
  PendingInput p;
  p.kind = c.kind;
  p.seq = c.seq;
  if (c.kind == Command::Kind::key) {
    // A protocol that takes no key messages (IMX): the key state alone,
    // which run_host has updated; nothing goes to the guest's queue.
    if (!proto_->takes_key_messages()) return;
    p.vk = uint8_t(c.a & 0xFF);
    p.down = c.b != 0;
    p.was_down = key_down_[p.vk];
    key_down_[p.vk] = p.down;
  } else if (c.kind == Command::Kind::mouse) {
    // A protocol that takes no mouse messages (scr): the mouse state alone,
    // which run_host has updated (GetCursorPos).
    if (!proto_->takes_mouse_messages()) return;
    // Guest coordinates: the output's, scaled up with the guest display (sync_input).
    const int k = guest_scale_;
    const int w = guest_screen_ ? guest_screen_->width() : ctx_->screen.width();
    const int h = guest_screen_ ? guest_screen_->height() : ctx_->screen.height();
    p.x = std::clamp<int32_t>(c.a * k + (k > 1 ? k / 2 : 0), 0, std::max(w - 1, 0));
    p.y = std::clamp<int32_t>(c.b * k + (k > 1 ? k / 2 : 0), 0, std::max(h - 1, 0));
    p.buttons = uint32_t(c.c) & kMouseButtonMask;
    p.prev_buttons = mouse_buttons_;
    p.moved = !mouse_known_ || p.x != mouse_x_ || p.y != mouse_y_;
    mouse_known_ = true;
    mouse_x_ = p.x;
    mouse_y_ = p.y;
    mouse_buttons_ = p.buttons;
  } else {
    return;  // CAPS, NUMLOCK: the toggle state only (the KEY 20 or 144 line before it is the key)
  }
  pending_input_.push_back(p);
}

// At a point the guest can be called (the start of a frame's DRAWFRAME run,
// or where a suspended long call resumes, on_deadline): each KEY
// through the WH_KEYBOARD chain, then — unless a hook consumed it — into the
// saver window's queue (a protocol that takes key messages: queue_input);
// each MOUSE as WM_MOUSEMOVE and button messages.
void Ne16Lane::deliver_input() {
  std::vector<PendingInput> batch;
  batch.swap(pending_input_);
  Runtime16& rt = *rt_;
  const InputState& in = rt.input();
  for (const PendingInput& p : batch) {
    if (p.kind == Command::Kind::key) {
      uint32_t lp = win16::key_lparam(p.vk, p.down, p.was_down);
      if (win16::user16_has_keyboard_hook(rt) && win16::user16_keyboard_hooks(rt, p.vk, lp, p.seq)) {
        if (p.seq > eaten_) eaten_ = p.seq;
        continue;
      }
      bool sys = p.vk == VK_MENU || p.vk == VK_F10;
      uint16_t msg = p.down ? (sys ? WM_SYSKEYDOWN : WM_KEYDOWN) : (sys ? WM_SYSKEYUP : WM_KEYUP);
      win16::user16_post_input(rt, msg, p.vk, lp, p.seq);
    } else {
      uint16_t mk = uint16_t(((p.buttons & kMouseLeft) ? MK_LBUTTON : 0) | ((p.buttons & kMouseRight) ? MK_RBUTTON : 0) |
                             ((p.buttons & kMouseMiddle) ? MK_MBUTTON : 0) | (in.keys.test(VK_SHIFT) ? MK_SHIFT : 0) |
                             (in.keys.test(VK_CONTROL) ? MK_CONTROL : 0));
      uint32_t lp = (uint32_t(uint16_t(p.y)) << 16) | uint16_t(p.x);
      if (p.moved) win16::user16_post_input(rt, WM_MOUSEMOVE, mk, lp, p.seq);
      static const struct {
        uint32_t bit;
        uint16_t down, up;
      } kButtons[] = {{kMouseLeft, WM_LBUTTONDOWN, WM_LBUTTONUP},
                      {kMouseRight, WM_RBUTTONDOWN, WM_RBUTTONUP},
                      {kMouseMiddle, WM_MBUTTONDOWN, WM_MBUTTONUP}};
      for (const auto& b : kButtons) {
        if ((p.buttons ^ p.prev_buttons) & b.bit) win16::user16_post_input(rt, (p.buttons & b.bit) ? b.down : b.up, mk, lp, p.seq);
      }
    }
  }
}

// After a step: what the guest consumed from the saver window's queue, whether
// it reads it, whether it asked the saver to close; input nobody took is
// dropped unless a DRAWFRAME that may still take it is suspended.
void Ne16Lane::end_step_input() {
  // Input nobody took: dropped after its step (the 1996 host pumped its queue
  // when the module's call returned, and the saver window had it), unless a
  // DRAWFRAME that may still take it is suspended: one more step, or, while
  // that call reads the saver window's queue itself, until it does (bounded).
  // Lunatic Fringe's game is one such call, polling the queue once per game
  // tick, 2 to 3 frames apart under emulation; the original host could not
  // pump before the call returned, so every key was the game's.
  const bool reader = read_queue_ && frames_ - last_read_frame_ < kReaderSteps;
  const uint32_t keep = !suspended_ ? 0 : reader ? kReaderKeepSteps : 1;
  win16::StepReport16 r = win16::user16_end_step(*rt_, keep);
  if (r.consumed > eaten_) eaten_ = r.consumed;
  if (r.queue_reads != queue_reads_) {
    queue_reads_ = r.queue_reads;
    last_read_frame_ = frames_;
    read_queue_ = true;
  }
  wake_ = wake_ || r.wake;
  queued_seq_ = r.pending;
  hooked_ = win16::user16_has_keyboard_hook(*rt_);
  // What an Intermission module posted to its own task (SWSE's FORCETOWAKE,
  // lane.hh "Intermission (IMX)"), taken by the protocol's message pump: said,
  // never a wake.
  if (r.task_posts != task_posts_) {
    trace("lane", "%s: frame %" PRIu64 ": %u message(s) posted to the guest's task so far (the latest %04X): not a wake",
          module_name_.c_str(), frames_, r.task_posts, r.last_task_msg);
    task_posts_ = r.task_posts;
  }
}

LaneStatus Ne16Lane::status() const {
  LaneStatus s;
  s.interactive = wants_events_;
  s.cursor = cursor_;
  s.source = wants_events_ ? kStatusSourceAd3 : 0;  // the toggle is AD3's 0x0E (Protocol16::Call)
  // Within the last 120 steps it read the saver window's queue, or a keyboard
  // hook is in — unless the protocol takes neither key nor mouse messages
  // (scr): then nothing it reads or hooks is the saver's input.
  const bool takes_input = proto_ && (proto_->takes_key_messages() || proto_->takes_mouse_messages());
  s.key_filter = takes_input && (hooked_ || (read_queue_ && frames_ - last_read_frame_ < kReaderSteps));
  s.wake = wake_;
  s.eaten = eaten_;
  // Input still in the saver window's queue (kept while a suspended DRAWFRAME
  // may read it) or not yet delivered: the guest has not had its say.
  s.unsettled = queued_seq_;
  for (const PendingInput& p : pending_input_) {
    if (p.seq && (!s.unsettled || p.seq < s.unsettled)) s.unsettled = p.seq;
  }
  return s;
}

// The frame's run of DRAWFRAMEs (lane.hh "Pacing"): until the work budget
// (less what the frame pays back of a carried overrun), ADMAXDRAWS calls, or
// - with long calls on - the frame's deadline. False when the module stopped
// the run.
bool Ne16Lane::draw_run() {
  for (;;) {
    if (controls_pending_) {
      controls_pending_ = false;
      proto_->send_controls();
    }
    if (!pending_input_.empty()) deliver_input();
    // The host's message loop ran between DRAWFRAMEs: MM_MCINOTIFY and
    // MM_WOM_* due by now reach their windows (lane.hh "Sound").
    win16::audio16_pump(*rt_);
    // One call of the protocol (AD3: SetWindowOrgEx, MODULEMESSAGE16(DRAWFRAME)).
    mid_call_ = true;
    Protocol16::Call r = proto_->call();
    mid_call_ = false;
    draws_++;
    frame_draws_++;
    if (r.kind == Protocol16::Call::Kind::toggle_events) {
      wants_events_ = !wants_events_;
    } else if (r.kind == Protocol16::Call::Kind::cursor_on || r.kind == Protocol16::Call::Kind::cursor_off) {
      cursor_ = r.kind == Protocol16::Call::Kind::cursor_on;
    } else if (r.kind == Protocol16::Call::Kind::stop) {
      if (winding_down_) {
        trace("lane", "%s: closing: the module ended (result %d): %s", module_name_.c_str(), r.code,
              proto_->error_text().c_str());
      } else {
        log("%s: frame %" PRIu64 ": the module stopped (result %d): %s", module_name_.c_str(), frames_, r.code,
            proto_->error_text().c_str());
      }
      return false;
    } else if (r.kind == Protocol16::Call::Kind::wake) {
      // The module asked the saver to end, as the user's input would (AD.EXE
      // 2.0 took result 5 as its wake): the status says wake, so the saver
      // ends as when the user wakes it; the frame is presented as the call
      // left it, and the module is called no more (step()).
      woke_ = wake_ = true;
      log("%s: frame %" PRIu64 ": the module woke the saver (result %d)%s", module_name_.c_str(), frames_, r.code,
          ctx_->env.stream ? "; it is called no more" : "; the run ends");
      return true;
    }
    proto_->after_call();
    if (frame_draws_ >= max_draws_ || rt_->work_insns() - frame_w0_ >= frame_allow_) return true;
    if (long_calls_ && rt_->peek_us() >= frame_deadline_) return true;
  }
}

namespace {
// Thrown from the deadline hook into a suspended DRAWFRAME to abandon it (the
// run is closing): call_far's levels put the machine back on the way out.
struct AbandonCall {};
}  // namespace

void CALLBACK Ne16Lane::fiber_main(void* self) { static_cast<Ne16Lane*>(self)->fiber_body(); }

void Ne16Lane::fiber_body() {
  for (;;) {
    try {
      run_result_ = draw_run() ? Run::frame_done : Run::stopped;
    } catch (const AbandonCall&) {
      run_result_ = Run::abandoned;
    } catch (...) {
      fiber_error_ = std::current_exception();
      run_result_ = Run::error;
    }
    mid_call_ = false;
    SwitchToFiber(host_fiber_);
  }
}

// The deadline hook (Runtime16::set_deadline), on the guest fiber at an API
// call inside a DRAWFRAME: the frame period is over, so the frame ends here
// and the call carries on when the next frame begins.
void Ne16Lane::on_deadline() { suspend_frame(); }

// Ends the presented frame inside the DRAWFRAME in progress and returns when
// the next frame resumes it; false (nothing happens) outside a DRAWFRAME on
// the guest fiber. The deadline hook, and a synchronous sndPlaySound waiting
// out its sound (Runtime16::wait_until_us), end frames here.
bool Ne16Lane::suspend_frame() {
  if (!mid_call_ || !guest_fiber_ || GetCurrentFiber() != guest_fiber_) return false;
  suspended_ = true;
  run_result_ = Run::suspended;
  SwitchToFiber(host_fiber_);
  suspended_ = false;
  if (abandon_) throw AbandonCall{};
  // The frame resumes the call here, inside an API call the guest made (as
  // Windows ran keyboard hooks inside GetMessage/PeekMessage): input that
  // arrived meanwhile goes in now — a module whose game loop is one long
  // DRAWFRAME (LUNATIC) reads it before its next frame.
  if (!pending_input_.empty()) deliver_input();
  return true;
}

StepResult Ne16Lane::step() {
  if (!rt_ || !loaded_) return StepResult::failed;
  // After the module's wake (draw_run) it is called no more. A headless run
  // ends here (exit 0): nobody reads its status, and a saver would have
  // ended. Streamed, the front end has the wake in the status of the frame
  // before; until it ends the run, the frames repeat the last picture (and
  // sound already playing plays on).
  if (woke_) {
    if (!ctx_->env.stream) return StepResult::finished;
    frames_++;
    return StepResult::ok;
  }
  try {
    trace("lane", "frame %" PRIu64, frames_);
    settle_screen();
    rt_->start_frames();
    win32::Display* disp = rt_->display();
    start_bits_.assign(disp->bits(), disp->bits() + size_t(disp->pitch()) * size_t(disp->height()));
    latched_ = false;
    rt_->update_bios_ticks();
    // One presented frame = as many back-to-back DRAWFRAMEs as the virtual
    // CPU fits into a frame period (lane.hh "Pacing"): AFTERDAR.SCR sent one
    // per pass of its idle loop, so a module that counts calls (LOGO moves its
    // picture every 100th call at Medium) ran at thousands of calls a second.
    // Work counts are deterministic, so so is the number of calls.
    // (The frame period is the core's once the lane runs: ADPACEMS or ours.)
    const uint64_t period = ctx_->clock.step_us();
    frame_budget_ = draw_mips_ * period;
    frame_w0_ = rt_->work_insns();
    frame_draws_ = 0;
    const uint64_t i0 = rt_->instructions(), d0 = draws_, t0 = rt_->peek_us();
    const bool resumed = suspended_;
    // Carried overruns (lane.hh "Pacing"): the frame's budget first pays back
    // what the last frames' calls did beyond theirs, and a frame it all goes
    // to makes no call — the modeled machine is still busy with them.
    const uint64_t paid = carry_ ? std::min(owed_, frame_budget_) : 0;
    frame_allow_ = frame_budget_ - paid;
    const bool idle = carry_ && !resumed && !frame_allow_;
    in_step_ = true;
    if (long_calls_ && !idle) {
      // Long calls (lane.hh): the frame ends at the deadline even inside a
      // DRAWFRAME. Headless, the deadline is the frame grid's next line;
      // streamed, 90% of a period of wall time from now (the rest is the
      // host's, to present).
      frame_deadline_ = rt_->modeled_time() ? ctx_->clock.now_us() + period : rt_->peek_us() + period * 9 / 10;
      rt_->set_deadline(frame_deadline_, [this] { on_deadline(); });
      // A task's frame (lane.hh "Windows 3.1 screen savers") also ends at the
      // first API call past the frame's work budget: the 486-class machine's
      // share of the period, as a run of DRAWFRAMEs gets (Pacing).
      if (proto_->runs_as_task() && frame_budget_) rt_->set_work_deadline(frame_w0_ + frame_budget_);
      run_result_ = Run::none;
      SwitchToFiber(guest_fiber_);
      rt_->clear_deadline();
    } else {
      try {
        if (idle) {
          // No call, but what goes on between calls does: the input, the
          // audio (MEMMIDI's timer is delivered at the pump and at API
          // calls), the protocol's message loop.
          if (!pending_input_.empty()) deliver_input();
          win16::audio16_pump(*rt_);
          proto_->after_call();
          run_result_ = Run::frame_done;
        } else {
          run_result_ = draw_run() ? Run::frame_done : Run::stopped;
        }
      } catch (...) {
        in_step_ = false;
        throw;
      }
    }
    in_step_ = false;
    if (run_result_ == Run::error) {
      std::exception_ptr e = fiber_error_;
      fiber_error_ = nullptr;
      std::rethrow_exception(e);
    }
    if (run_result_ != Run::frame_done && run_result_ != Run::suspended) {
      census();
      return StepResult::failed;
    }
    const uint64_t work = rt_->work_insns() - frame_w0_;
    if (carry_) {
      // What the frame's passes did beyond what it was allowed is owed. A
      // pass that runs past its frame's deadline owes nothing, neither in the
      // frames that end inside it nor in the one it returns in: it is paced
      // by the deadline (Long calls), not by the budget. The passes that
      // frame runs after it returns are owed as any frame's are: one only
      // starts while the frame's work is below its allowance (draw_run), so
      // what they did beyond it is the frame's work less the allowance (all
      // of the budget: the frame before ended inside a call, so nothing was
      // owed or paid back). What is owed stays below kMaxOwedBudgets budgets,
      // and each frame without a call pays a whole budget back, so at most
      // kMaxOwedBudgets - 1 frames in a row make none.
      if (suspended_) {
        owed_ = 0;  // it ended inside a call
      } else if (resumed && draws_ - d0 <= 1) {
        owed_ = 0;  // it only finished the call it resumed
      } else if (idle) {
        // A frame without a call pays a whole budget back, whatever its pumps
        // did (MEMMIDI's timer procedures, the message loop): owing that work
        // again would let a frame whose pumps cost a budget stall the module
        // for good, and the bound above would not hold.
        owed_ -= paid;
      } else {
        const uint64_t over = work > frame_allow_ ? work - frame_allow_ : 0;
        owed_ = std::min(owed_ - paid + over, kMaxOwedBudgets * frame_budget_ - 1);
      }
      if (idle) idle_frames_++;
    }
    if (tracing("pace")) {
      char carried[64] = "";
      if (carry_) snprintf(carried, sizeof(carried), ", %" PRIu64 " paid back, %" PRIu64 " owed", paid, owed_);
      trace("pace", "frame %" PRIu64 ": %" PRIu64 " DRAWFRAME(s)%s%s, work %" PRIu64 " of %" PRIu64 " (%" PRIu64
            " instructions)%s, time %" PRIu64 " us (grid %" PRIu64 ", began %" PRIu64 ")",
            frames_, draws_ - d0, resumed ? ", resumed" : "", suspended_ ? ", ended inside one" : "", work, frame_budget_,
            rt_->instructions() - i0, carried, rt_->peek_us(), ctx_->clock.now_us(), t0);
    }
    if (suspended_) long_frames_++;
    rt_->settle_time();
    // Real GDI batches drawing per thread: finish it before the host reads the pixels.
    GdiFlush();
    if (latched_ && memcmp(disp->bits(), start_bits_.data(), start_bits_.size()) == 0) {
      // Nothing changed net, but a refresh showed something (see on_scanout).
      memcpy(disp->bits(), latched_bits_.data(), latched_bits_.size());
      showing_latched_ = true;
      transient_frames_++;
      trace("lane", "frame %" PRIu64 ": presenting content a refresh showed during DRAWFRAME", frames_);
    }
    frames_++;
    end_step_input();
    present();
    ctx_->screen.mark_dirty();
    // The engine renders up to the guest's own time (lane.hh "Sound"): it runs
    // ahead of the core clock run_host advances with (by the frame's work
    // headless, by the modeled init time streamed), and the engine takes an
    // earlier time as the latest it has seen — so without this a streamed run
    // would render only at the guest's audio calls. But never past an audio
    // event not yet delivered (Runtime16::audio_due): a timer procedure's
    // calls are dated at its period's due time (win16/sound16.hh), and a frame
    // can end with periods due and undelivered — inside a call, or after one
    // that made no API call — whose notes would otherwise sound at the
    // engine's time, bunched at the frame's end.
    if (ctx_->audio && ctx_->audio->enabled()) ctx_->audio->advance(std::min(rt_->peek_us(), rt_->audio_due()));
    return StepResult::ok;
  } catch (const GuestError16& e) {
    log("%s: frame %" PRIu64 ": %s", module_name_.c_str(), frames_, e.what());
    rt_->log_state("guest state");
  } catch (const std::exception& e) {
    log("%s: frame %" PRIu64 ": %s", module_name_.c_str(), frames_, e.what());
    rt_->log_state("guest state");
  }
  census();
  return StepResult::failed;
}

// Before the guest can be called on the host fiber again (UNLOAD at close): a
// DRAWFRAME the last frame ended inside is abandoned - the deadline hook
// throws, and every call level restores the machine on the way out.
void Ne16Lane::abandon_long_call() {
  if (!suspended_ || !guest_fiber_) return;
  abandon_ = true;
  SwitchToFiber(guest_fiber_);
  abandon_ = false;
  trace("lane", "%s: abandoned the DRAWFRAME in progress", module_name_.c_str());
}

// Shutdown (Protocol16::close_suspended): the suspended call resumes, a
// frame period of virtual time at a time (the frame clock stands still), for
// at most kWindDownSteps resumptions, until it returns; nothing is presented.
void Ne16Lane::wind_down() {
  winding_down_ = true;
  const uint64_t period = ctx_->clock.step_us();
  int steps = 0;
  for (; steps < kWindDownSteps && suspended_; steps++) {
    rt_->set_deadline(rt_->peek_us() + period, [this] { on_deadline(); });
    run_result_ = Run::none;
    SwitchToFiber(guest_fiber_);
    rt_->clear_deadline();
    if (run_result_ == Run::error) {
      std::exception_ptr e = fiber_error_;
      fiber_error_ = nullptr;
      try {
        std::rethrow_exception(e);
      } catch (const std::exception& x) {
        log("%s: while closing: %s", module_name_.c_str(), x.what());
      }
      break;
    }
  }
  winding_down_ = false;
  trace("lane", "%s: closing took %d resumption(s)%s", module_name_.c_str(), steps,
        suspended_ ? "; still running: abandoned" : "");
}

void Ne16Lane::free_fibers() {
  if (guest_fiber_) {
    DeleteFiber(guest_fiber_);
    guest_fiber_ = nullptr;
  }
  if (converted_thread_) {
    ConvertFiberToThread();
    converted_thread_ = false;
  }
  host_fiber_ = nullptr;
}

void Ne16Lane::shutdown() {
  if (!rt_) return;
  settle_screen();
  if (transient_frames_) trace("lane", "%" PRIu64 " frame(s) presented transient content", transient_frames_);
  trace("lane", "%s: %" PRIu64 " DRAWFRAME calls over %" PRIu64 " frames (%" PRIu64 " ended inside one%s)",
        module_name_.c_str(), draws_, frames_, long_frames_,
        carry_ ? (", " + std::to_string(idle_frames_) + " made none").c_str() : "");
  try {
    // A suspended call the protocol ends as its host ended it (scr: the
    // program closes, as on the input that woke a Windows 3.1 saver) runs on
    // to that end first; what is still suspended after it is abandoned.
    if (suspended_ && loaded_ && proto_ && proto_->close_suspended()) wind_down();
    abandon_long_call();
    if (loaded_ && proto_ && !suspended_) proto_->unload();
    loaded_ = false;
    if (proto_ && !suspended_) proto_->close();
    if (!suspended_) rt_->modules().free_all();
    // What the guest left playing or open (win16/sound16.hh).
    win16::audio16_close(*rt_);
  } catch (const std::exception& e) {
    log("%s: while closing: %s", module_name_.c_str(), e.what());
  }
  census();
  proto_.reset();
  rt_.reset();
  free_fibers();
}

int Ne16Lane::auto_guest_scale(int w, int h) {
  if (w <= 0 || h <= 0) return 1;
  int k = 1;
  while (k < 8 && (int64_t(w) * k < 640 || int64_t(h) * k < 480)) k++;
  return k;
}

// The guest sees the output's mouse scaled up to its own display.
void Ne16Lane::sync_input() {
  if (!guest_screen_ || !ctx_) return;
  guest_input_ = ctx_->input;
  const int k = guest_scale_;
  guest_input_.mouse_x = std::clamp<int32_t>(ctx_->input.mouse_x * k + k / 2, 0, guest_screen_->width() - 1);
  guest_input_.mouse_y = std::clamp<int32_t>(ctx_->input.mouse_y * k + k / 2, 0, guest_screen_->height() - 1);
}

// Small screens (lane.hh): the guest display averaged k×k → 1 into the output,
// each average matched to the nearest hardware palette entry (squared RGB
// distance, lowest index on ties) at 6 bits per channel, the VGA DAC's
// precision. A pure function of the guest's pixels and palette.
void Ne16Lane::present() {
  if (!guest_screen_ || !ctx_) return;
  GdiFlush();  // real GDI batches drawing per thread
  Screen& out = ctx_->screen;
  const Screen& in = *guest_screen_;
  const auto& pal = in.palette();
  if (near_cache_.empty() || memcmp(pal.data(), near_pal_.data(), sizeof(near_pal_)) != 0) {
    near_pal_ = pal;
    if (near_cache_.empty()) near_cache_.assign(size_t(1) << 18, 0);
    if (++near_gen_ >= (1u << 24)) {
      std::fill(near_cache_.begin(), near_cache_.end(), 0u);
      near_gen_ = 1;
    }
  }
  if (memcmp(out.palette().data(), pal.data(), sizeof(near_pal_)) != 0) out.set_entries(0, 256, pal.data());
  const int k = guest_scale_, n = k * k;
  const int w = out.width(), h = out.height();
  for (int y = 0; y < h; y++) {
    uint8_t* dst = out.row(y);
    for (int x = 0; x < w; x++) {
      int r = 0, g = 0, b = 0;
      const uint8_t first = in.row(y * k)[size_t(x) * size_t(k)];
      bool uniform = true;
      for (int j = 0; j < k; j++) {
        const uint8_t* src = in.row(y * k + j) + size_t(x) * size_t(k);
        for (int i = 0; i < k; i++) {
          const RGBQUAD& q = pal[src[i]];
          r += q.rgbRed;
          g += q.rgbGreen;
          b += q.rgbBlue;
          uniform &= src[i] == first;
        }
      }
      if (uniform) {  // one index: itself, exactly
        dst[x] = first;
        continue;
      }
      r = (r + n / 2) / n;
      g = (g + n / 2) / n;
      b = (b + n / 2) / n;
      uint32_t key = (uint32_t(r >> 2) << 12) | (uint32_t(g >> 2) << 6) | uint32_t(b >> 2);
      uint32_t& slot = near_cache_[key];
      if ((slot >> 8) != near_gen_) {
        // The cell's centre colour, matched against every hardware entry.
        int cr = int(key >> 12) * 4 + 2, cg = int((key >> 6) & 63) * 4 + 2, cb = int(key & 63) * 4 + 2;
        int best = 0, best_d = INT32_MAX;
        for (int e = 0; e < 256; e++) {
          int dr = pal[size_t(e)].rgbRed - cr, dg = pal[size_t(e)].rgbGreen - cg, db = pal[size_t(e)].rgbBlue - cb;
          int dd = dr * dr + dg * dg + db * db;
          if (dd < best_d) best_d = dd, best = e;
        }
        slot = (near_gen_ << 8) | uint32_t(best);
      }
      dst[x] = uint8_t(slot & 0xFF);
    }
  }
}

// Configure mode (lane.hh "Configure"): the module's button handler, run by
// its protocol (Protocol16::button; AD3: as AFTERDAR.SCR's property page ran
// it through OLDMOD32's ButtonPushed3216 — BUTTONPUSHED16 of the real
// OLDMOD16, or the native bridge's same sequence) with the module's dialogs
// real (win16/dialogs16.hh) and its disk persistent (ADSTATE, or the
// --configure default).
ConfigureResult Ne16Lane::configure(const std::string& module_path, LaneContext& ctx, const ConfigureRequest& req,
                                    std::string* json_out) {
  ctx_ = &ctx;
  const Env& env = ctx.env;
  std::string path = full_path(module_path);
  module_name_ = file_of(path);
  layout_ = resolve_layout(path, env.win_assets_dir(), file_exists, dir_exists);
  auto finish = [&](ConfigureResult r, int shown, const std::string& message, const std::vector<std::string>& written) {
    if (json_out) *json_out = configure_json(r, shown, message, written);
    log("%s: configure button %d: %s (%d shown)%s%s", module_name_.c_str(), req.slot,
        r == ConfigureResult::shown ? "shown" : r == ConfigureResult::nothing ? "nothing shown" : "failed", shown,
        message.empty() ? "" : ": ", message.c_str());
    return r;
  };
  // Whether the module has this button, before anything is loaded (AD3: the
  // slot's control record is a button; IMX: slot 0).
  std::string why;
  proto_ = make_protocol_(layout_, env, &why);
  if (!proto_) return finish(ConfigureResult::failed, 0, why, {});
  if (!proto_->check_button(req.slot, &why)) return finish(ConfigureResult::failed, 0, why, {});

  win32::ConfigScript script;
  std::string script_error;
  if (!script.load_env(env, &script_error)) return finish(ConfigureResult::failed, 0, script_error, {});
  win16::Configure16 cfg;
  cfg.script = &script;
  cfg.owner = req.owner && IsWindow(reinterpret_cast<HWND>(uintptr_t(req.owner))) ? reinterpret_cast<HWND>(uintptr_t(req.owner))
                                                                                  : nullptr;
  if (req.owner && !cfg.owner) log("%s: --owner 0x%llx is not a window; the dialogs have no owner", module_name_.c_str(),
                                   (unsigned long long)req.owner);

  win16::Runtime16Options opts;
  opts.arena_size = uint32_t(std::clamp<uint64_t>(env_u64(env, "ADHEAPMB", 64), 16, 512) << 20);
  // No budget: the user may keep the dialog open as long as they like.
  opts.call_budget = UINT64_MAX / 2;
  // The protocol's choices and runtime options (AD3: the bridge, the palette).
  proto_->configure_button_runtime(opts, env);
  if (env.get("ADDESKTOPPAL")) opts.desktop_palette = env.flag("ADDESKTOPPAL");
  ctx.clock.set_read_step_us(0);
  std::vector<std::string> written;
  try {
    rt_ = std::make_unique<Runtime16>(opts, ctx.clock, &ctx.input);
    Runtime16& rt = *rt_;
    win16::register_all16(rt);
    proto_->mount(rt, env);
    rt.attach_display(ctx.screen);
    // Dialogs run on the wall clock (a module's timers tick while the user
    // looks); the core's realtime clock starts with its first frame.
    ctx.clock.begin_frame();
    rt.start_frames();
    win16::enable_real_dialogs16(rt, &cfg);
    uint16_t owner16 = win16::real_hwnd16(rt, cfg.owner);

    Protocol16::Button b = proto_->button(rt, req.slot, owner16, ctx);
    if (!b.ran) {
      proto_.reset();
      rt_.reset();
      return finish(ConfigureResult::failed, 0, b.message, {});
    }
    proto_->close();
    rt.modules().free_all();
    written = rt.vfs().written();
    proto_.reset();
    rt_.reset();
    if (!b.failure.empty()) return finish(ConfigureResult::failed, cfg.shown, b.failure, written);
    std::string message = b.message;
    for (const std::string& n : cfg.notes) message += (message.empty() ? "" : "; ") + n;
    if (cfg.failed) return finish(ConfigureResult::failed, cfg.shown, message.empty() ? "a dialog failed" : message, written);
    return finish(cfg.shown ? ConfigureResult::shown : ConfigureResult::nothing, cfg.shown, message, written);
  } catch (const GuestError16& e) {
    log("%s: configure: %s", module_name_.c_str(), e.what());
    if (rt_) rt_->log_state("guest state");
    if (rt_) written = rt_->vfs().written();
    std::string what = e.what();
    proto_.reset();
    rt_.reset();
    return finish(ConfigureResult::failed, cfg.shown, what, written);
  } catch (const std::exception& e) {
    if (rt_) written = rt_->vfs().written();
    std::string what = e.what();
    proto_.reset();
    rt_.reset();
    return finish(ConfigureResult::failed, cfg.shown, what, written);
  }
}

void Ne16Lane::census() {
  if (!rt_ || census_done_) return;
  census_done_ = true;
  rt_->shims().print_census(module_name_);
}

}  // namespace adw::ne16

namespace adw {
std::unique_ptr<Lane> make_ne16_lane() { return std::make_unique<ne16::Ne16Lane>(); }
}  // namespace adw
