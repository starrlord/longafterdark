// One running adhostwin.exe (DESIGN.md §1): spawned with pipes and no
// console, placed in the caller's kill-on-close Job, fed GO/SET/QUIT lines on
// stdin by a writer thread, and drained by a reader thread that parses P8/P6
// frames and hands the newest one to the UI thread (latest wins — a slow
// present drops frames instead of queueing them).
#pragma once

#include <windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "adw/core/status.h"
#include "frame_parser.h"

namespace adw::scr {

using Clock = std::chrono::steady_clock;

// A frame ready for StretchDIBits: DWORD-aligned rows, top-down.
struct Frame {
  int width = 0, height = 0;
  int bpp = 8;                          // 8 (palettized) or 32 (BGRX, from P6)
  int stride = 0;                       // bytes per row
  std::vector<uint8_t> bits;
  std::array<RGBQUAD, 256> palette{};   // bpp 8 only
};

// P8 keeps its indices (the colour table rides in the BITMAPINFO); P6 becomes
// 32-bit BGRX. Rows are re-padded only when the width needs it.
void convert_frame(const RawFrame& raw, Frame& out);

struct HostSpec {
  std::wstring exe;
  std::wstring module_path;
  std::wstring working_dir;             // optional
  // Environment changes on top of ours; an empty value removes the variable.
  std::vector<std::pair<std::wstring, std::wstring>> env;
  std::wstring stderr_path;             // optional: append the host's log here (else NUL)
  DWORD priority_class = 0;             // e.g. BELOW_NORMAL_PRIORITY_CLASS for previews
  // Create the host's status record (INTERACTION.md §3.4): a one-page
  // section inherited by the host and named in ADSTATUSHANDLE.
  bool status_record = true;
};

// The environment every host gets on top of the spec's (unless the spec sets
// them itself): ADSTATE = state_dir() (INTERACTION.md §7.1), and no sound --
// ADSOUND=0, ADAUDIOOUT and ADVOLUME removed (sound.h) -- unless the spec
// says ADSOUND itself.
void add_host_defaults(std::vector<std::pair<std::wstring, std::wstring>>& env);

// A module's host controls' variables (catalog.h: HostControlValues::env),
// put in front of `env`, the start's own changes: every start of a module's
// host passes them (the saver's windows, Preview and /p among them, the
// dialog's live preview, its thumbnails and its module buttons). A start's
// own variable wins over a control's of the same name (the catalog never has
// one: host_variable_ok).
void add_host_control_env(std::vector<std::pair<std::wstring, std::wstring>>& env,
                          const std::vector<std::pair<std::wstring, std::wstring>>& controls);

// CreateProcess environment block: ours with `changes` applied.
std::wstring build_environment_block(const std::vector<std::pair<std::wstring, std::wstring>>& changes);

class HostProcess {
 public:
  // The reader posts `frame_msg` when a new frame is ready and `exit_msg`
  // when the host's stdout closes, both with wParam = `cookie` so the window
  // can ignore messages from a host it has already replaced.
  struct Notify {
    HWND hwnd = nullptr;
    UINT frame_msg = 0, exit_msg = 0;
    WPARAM cookie = 0;
  };

  explicit HostProcess(Notify n);
  ~HostProcess();
  HostProcess(const HostProcess&) = delete;
  HostProcess& operator=(const HostProcess&) = delete;

  bool start(const HostSpec& spec, HANDLE job, std::wstring* error);
  void request_quit();                  // queue QUIT; returns at once
  void stop(DWORD grace_ms = 150);      // QUIT, wait up to grace, terminate, join threads
  // The host was started with ADSOUND=1 (sound.h): it is stopped with the
  // longer grace, so it can silence its device and send MIDI all-notes-off.
  bool sound() const { return sound_; }
  // stop() with the grace this host needs (sound.h kHostStopGraceMs /
  // kSoundHostStopGraceMs).
  void stop_gracefully();

  void send_line(const std::string& line);   // "SET 0 5" etc.; newline added
  // Input lines (INTERACTION.md §3.2): KEY, CAPS and MOUSE are numbered 1, 2,
  // 3 ... in the order they are queued, exactly as the host numbers them, and
  // the line's number is returned (0 = nothing queued: the host is quitting).
  // A MOUSE line with the same buttons as a MOUSE line still waiting in the
  // queue (and last in it) replaces that line's text and keeps its number:
  // moves are coalesced. MOUSE lines go out with the next GO (or the next
  // other line); KEY and CAPS go out at once.
  uint64_t send_input(const std::string& line);
  uint64_t input_seq() const;
  // The host's status record (§3.4). False when there is none, or nothing
  // consistent has been published yet (`out` is then untouched).
  bool read_status(adw::AdwHostStatusV1* out) const;
  bool has_status_record() const { return status_view_ != nullptr; }
  // Pacing (called by the Pacer on every vsync-ish tick, any thread): sends
  // GO when the previous GO's frame has arrived *and been taken by the UI*
  // and the minimum interval has passed, so the host renders at the rate we
  // can present. The first GO goes out with the spawn (DESIGN.md §1). A GO
  // still unanswered after a second is re-sent only if the parser discarded
  // garbage meanwhile (the frame it answered with may have been lost).
  void clock_tick(Clock::time_point now);
  void set_min_go_interval(std::chrono::microseconds us) { min_go_interval_ = us; }

  std::unique_ptr<Frame> take_frame();        // UI thread
  void recycle(std::unique_ptr<Frame> f);     // hand a presented frame back for reuse

  bool exited() const;
  DWORD exit_code() const;              // STILL_ACTIVE while running
  uint64_t frames() const { return frames_.load(); }
  uint64_t gos_sent() const { return gos_sent_.load(); }
  Clock::time_point started_at() const { return started_at_; }
  Clock::time_point last_frame_at() const;
  DWORD pid() const { return pid_; }
  uint64_t resyncs() const { return resyncs_.load(); }

 private:
  void reader_main();
  void writer_main();
  std::unique_ptr<Frame> pooled();

  Notify notify_;
  HANDLE process_ = nullptr;
  HANDLE stdin_w_ = nullptr;
  HANDLE stdout_r_ = nullptr;
  DWORD pid_ = 0;
  bool sound_ = false;
  std::thread reader_, writer_;
  Clock::time_point started_at_{};

  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::string outq_;
  bool go_pending_ = false;
  bool quitting_ = false;
  bool writer_done_ = false;
  Clock::time_point last_go_{};
  uint64_t resyncs_at_go_ = 0;
  int64_t written_off_ = 0;             // GOs whose frames the parser discarded (see clock_tick)
  Clock::time_point last_frame_at_{};
  bool urgent_ = false;                 // queued lines that must not wait for the next GO
  uint64_t input_seq_ = 0;
  size_t mouse_tail_pos_ = std::string::npos;   // offset of the MOUSE line ending outq_, if it does
  uint64_t mouse_tail_seq_ = 0;
  std::string mouse_tail_buttons_;
  HANDLE status_section_ = nullptr;
  void* status_view_ = nullptr;
  std::unique_ptr<Frame> latest_;
  std::vector<std::unique_ptr<Frame>> pool_;

  std::atomic<std::chrono::microseconds> min_go_interval_{std::chrono::microseconds(12000)};
  std::atomic<uint64_t> frames_{0}, gos_sent_{0}, resyncs_{0};
  std::atomic<bool> stdout_closed_{false};
  std::atomic<bool> notify_pending_{false};
};

// The frame clock: one thread that wakes once per display refresh (DwmFlush)
// or, where composition can't pace us (secure desktop, no DWM), on a 60 Hz
// high-resolution waitable timer, and ticks every registered host.
class Pacer {
 public:
  ~Pacer();
  void start();
  void stop();
  void add(HostProcess* h);
  void remove(HostProcess* h);          // blocks until no tick is using `h`
  // While paused no GOs go out, so lockstep hosts sit blocked on stdin and
  // cost nothing (the saver pauses while the display is powered off).
  void set_paused(bool paused) { paused_ = paused; }
  bool paused() const { return paused_; }

 private:
  void run();
  std::mutex mu_;
  std::vector<HostProcess*> hosts_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> paused_{false};
};

// A Job that kills every host when the saver goes away (however it goes), and
// suppresses the crash dialog of a host that faults.
HANDLE create_kill_on_close_job();

} // namespace adw::scr
