// One running adhostwin.exe under Wine (DESIGN.md §1), the Linux
// counterpart of the Windows saver's HostProcess (scr/src/host_process.*):
//
//  * spawned with fork/exec (no shell) in a process group of its own (so a
//    Ctrl+C in the terminal reaches the player, which stops the host with
//    QUIT, and never the host directly), with PR_SET_PDEATHSIG so it can't
//    outlive a player that is killed outright, with every descriptor but
//    its three pipes closed, and at a lower priority when asked (a
//    preview's, as the Windows saver's /p host runs below normal);
//  * stdout read without blocking into the frame parser; stderr read the
//    same way, its STATUS lines (ADSTATUSLOG=1) parsed into the status
//    record and everything else passed to the host log;
//  * stdin written without blocking: input lines numbered 1, 2, 3 ... in the
//    order they are queued, exactly as the host numbers them, MOUSE moves
//    coalesced until the next GO, and at most one GO in flight, paced by the
//    player's clock;
//  * the end of the stream is stdout's end (read() == 0, POLLHUP or POLLERR)
//    or the process having exited, whichever is seen first. (Not stderr's:
//    Wine's service processes inherit the first host's stderr and keep it
//    open after the host is gone.)
//  * stopped with QUIT (and stdin closed) first, a grace period for its
//    sound shutdown, then SIGTERM and SIGKILL, and always reaped.
#pragma once

#include <poll.h>
#include <sys/types.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "frame_parser.h"
#include "sound.h"
#include "status.h"

namespace lad {

using Clock = std::chrono::steady_clock;

// A child process with pipes, for the host and the --capabilities probe.
struct SpawnSpec {
  std::string program;               // absolute path (the Wine loader)
  std::vector<std::string> argv;     // argv[0] included
  std::string cwd;                   // "" = the player's
  EnvChanges env;                    // on top of the player's; "" removes (names compared case-insensitively)
  bool pipe_stdin = true;            // else /dev/null
  bool pipe_stderr = true;           // else /dev/null
  // At least this nice value (0: the player's) for the child, and so for
  // every process it starts (Wine's): a child of a player already niced as
  // far keeps the player's, and none is given a higher priority.
  int nice = 0;
};
struct Child {
  pid_t pid = -1;
  int in = -1, out = -1, err = -1;   // the player's ends, non-blocking and close-on-exec; -1 when not piped
};
// False with *error when the pipes can't be made, fork fails, or exec
// fails (reported by the child through a close-on-exec pipe, so a missing
// Wine is an error here and not a host that silently never answers).
bool spawn_child(const SpawnSpec& spec, Child& out, std::string* error);

// waitpid(pid, WNOHANG), tolerant of EINTR. True when the child has ended
// (*status set; -1 when it was already reaped elsewhere).
bool reap_child(pid_t pid, int* status);
// "exit code 3", "signal 9", "unknown".
std::string describe_exit(int status);
// For the "could not be started" message: "host exit code 3", "host killed
// by signal 9", "host ended".
std::string host_exit_text(int status);

class HostProcess {
 public:
  struct Spec {
    std::string wine;                // the Wine loader
    std::string exe;                 // adhostwin.exe (a Unix path; Wine takes one)
    std::vector<std::string> args;   // after the exe: the module's Windows path, or --test-pattern
    std::string cwd;
    EnvChanges env;                  // the host environment (ADSTREAM, ADSCREENW, ...)
    bool sound = false;              // started with ADSOUND=1: the longer stop grace
    int nice = 0;                    // SpawnSpec::nice (a preview's host: 10)
  };

  HostProcess() = default;
  ~HostProcess();
  HostProcess(const HostProcess&) = delete;
  HostProcess& operator=(const HostProcess&) = delete;

  // Spawns the host and sends the opening GO at once (DESIGN.md §1: the
  // host waits only 250 ms for it before frame 0).
  bool start(const Spec& spec, Clock::duration period, std::string* error);

  pid_t pid() const { return child_.pid; }
  bool sound() const { return sound_; }
  bool quitting() const { return quitting_; }
  // The stream has ended: stdout closed, or the process was reaped.
  bool ended() const { return stdout_closed_ || reaped_; }
  bool reaped() const { return reaped_; }
  int exit_status() const { return status_; }   // valid once reaped(); -1 unknown
  // stop() had to end it with SIGTERM or SIGKILL (its status is then the
  // player's doing, not its own).
  bool signalled() const { return signalled_; }

  // ---- poll integration
  // Appends this host's descriptors; `base` receives the index of the first.
  void add_pollfds(std::vector<pollfd>& fds, size_t* base) const;
  // Handles what poll() reported for them (reads, writes, hang-ups).
  void on_poll(const std::vector<pollfd>& fds, size_t base);
  // waitpid without blocking; true when it has just been reaped.
  bool check_exit();

  // ---- frames
  // The newest complete frame since the last take (older ones are dropped:
  // with one GO in flight there is normally just one).
  bool take_frame(RawFrame& out);
  uint64_t frames() const { return frames_; }
  uint64_t resyncs() const { return parser_.resyncs(); }
  Clock::time_point started_at() const { return started_at_; }
  Clock::time_point last_frame_at() const { return last_frame_at_; }

  // ---- status (from STATUS lines)
  bool status(HostStatus* out) const;   // false until the first STATUS line

  // ---- input (INTERACTION.md §3.2)
  // KEY, CAPS, NUMLOCK and MOUSE lines: numbered, and the number returned
  // (0 = nothing queued: the host is quitting or gone). A MOUSE line with
  // the same buttons as the MOUSE line still waiting (and last in the
  // queue) replaces its text and keeps its number; MOUSE lines go out with
  // the next GO or the next other line, the rest at once.
  uint64_t send_input(const std::string& line);
  void send_line(const std::string& line);   // SET ...; not numbered
  uint64_t input_seq() const { return input_seq_; }

  // ---- pacing
  // When the next GO may go out: Clock::time_point::max() while one is in
  // flight (or the host is quitting); otherwise the later of the frame's
  // arrival and the previous GO's time plus one period, so GOs never come
  // faster than one per period and never catch up in a burst.
  Clock::time_point next_go_at(Clock::time_point now);
  void maybe_send_go(Clock::time_point now);
  uint64_t gos_sent() const { return gos_sent_; }

  // ---- lifetime
  // QUIT now (and stdin closed after it); returns at once. stop() follows.
  void request_quit();
  // QUIT (stdin closed after it), up to `grace_ms` for it to end on its
  // own, then SIGTERM, then SIGKILL; reaped; descriptors closed. Frames
  // and log lines that arrive meanwhile are drained, so the host never
  // blocks on a full pipe while it shuts down.
  void stop(int grace_ms);
  // stop() with the grace this host needs (sound.h).
  void stop_gracefully();

 private:
  void read_stdout();
  void read_stderr();
  void stderr_line(std::string_view line);
  void write_some();
  void flush_mouse();
  void close_fd(int& fd);

  Child child_;
  bool sound_ = false;
  bool quitting_ = false;
  bool stdout_closed_ = false;
  bool stdin_broken_ = false;
  bool reaped_ = false;
  bool signalled_ = false;
  int status_ = -1;
  Clock::time_point started_at_{}, last_frame_at_{};

  FrameParser parser_;
  RawFrame latest_;
  bool have_frame_ = false;
  uint64_t frames_ = 0;

  std::string errbuf_;
  HostStatus status_rec_{};
  bool have_status_ = false;

  std::string outq_;
  uint64_t input_seq_ = 0;
  std::string mouse_text_;          // the MOUSE line still waiting ("" = none)
  std::string mouse_buttons_;
  uint64_t mouse_seq_ = 0;

  Clock::duration period_{};
  uint64_t gos_sent_ = 0;
  int64_t written_off_ = 0;         // GOs whose frames the parser discarded
  Clock::time_point last_go_{};     // when the last GO was written
  Clock::time_point last_sched_{};  // when it was due
  uint64_t resyncs_at_go_ = 0;
};

}  // namespace lad
