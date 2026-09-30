#include "host.h"

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <strings.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>

#include "log.h"

extern char** environ;

namespace lad {

using namespace std::chrono_literals;

namespace {

// A GO still unanswered after this long is re-sent, but only when the
// parser threw bytes away meanwhile (the frame it answered with may have
// been lost); a slow step or a slow module init is simply waited out.
constexpr auto kGoRetry = 1s;
// Longest stderr line kept whole; longer ones are passed on in pieces.
constexpr size_t kMaxLogLine = 8192;
// A host that ignores SIGTERM this long gets SIGKILL.
constexpr auto kTermWait = 200ms;

void set_nonblock(int fd) {
  int fl = fcntl(fd, F_GETFL);
  if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

std::vector<std::string> build_env(const EnvChanges& changes) {
  std::vector<std::string> vars;
  for (char** e = environ; e && *e; ++e) vars.emplace_back(*e);
  for (const auto& [k, v] : changes) {
    // Windows programs read names case-insensitively: an inherited
    // "adsound=1" must not survive an "ADSOUND=0".
    std::erase_if(vars, [&](const std::string& e) {
      size_t eq = e.find('=');
      return eq == k.size() && strncasecmp(e.c_str(), k.c_str(), eq) == 0;
    });
    if (!v.empty()) vars.push_back(k + "=" + v);
  }
  return vars;
}

// Every descriptor from 3 up closes at exec, whatever opened it (Xlib, a
// library, a log file).
void cloexec_from_3() {
#ifdef SYS_close_range
  if (syscall(SYS_close_range, 3u, ~0u, 4u /* CLOSE_RANGE_CLOEXEC */) == 0) return;
#endif
  if (DIR* d = opendir("/proc/self/fd")) {
    int self = dirfd(d);
    while (struct dirent* e = readdir(d)) {
      int fd = atoi(e->d_name);
      if (fd >= 3 && fd != self) fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    closedir(d);
  }
}

}  // namespace

bool spawn_child(const SpawnSpec& spec, Child& out, std::string* error) {
  out = Child{};
  int in[2] = {-1, -1}, o[2] = {-1, -1}, er[2] = {-1, -1}, ex[2] = {-1, -1};
  auto close_all = [&] {
    for (int* p : {in, o, er, ex}) {
      for (int k = 0; k < 2; ++k) {
        if (p[k] >= 0) close(p[k]);
        p[k] = -1;
      }
    }
  };
  if ((spec.pipe_stdin && pipe2(in, O_CLOEXEC) != 0) || pipe2(o, O_CLOEXEC) != 0 ||
      (spec.pipe_stderr && pipe2(er, O_CLOEXEC) != 0) || pipe2(ex, O_CLOEXEC) != 0) {
    if (error) *error = std::string("pipe: ") + strerror(errno);
    close_all();
    return false;
  }
  // A roomy stdout pipe lets the host hand over a whole frame per write.
  fcntl(o[0], F_SETPIPE_SZ, 1 << 20);

  // Everything the child needs is built before fork.
  std::vector<std::string> env = build_env(spec.env);
  std::vector<char*> envp;
  for (auto& e : env) envp.push_back(e.data());
  envp.push_back(nullptr);
  std::vector<std::string> args = spec.argv;
  std::vector<char*> argv;
  for (auto& a : args) argv.push_back(a.data());
  argv.push_back(nullptr);
  const pid_t parent = getpid();
  // The child's nice value, when it is to be lower than the player's.
  int nice_to = -1;
  if (spec.nice > 0) {
    errno = 0;
    const int now = getpriority(PRIO_PROCESS, 0);
    if (errno == 0 && now < spec.nice) nice_to = std::min(spec.nice, 19);
  }

  pid_t pid = fork();
  if (pid < 0) {
    if (error) *error = std::string("fork: ") + strerror(errno);
    close_all();
    return false;
  }
  if (pid == 0) {
    // The child: only async-signal-safe calls from here to exec.
    setpgid(0, 0);
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (getppid() != parent) _exit(127);   // the player died before the line above
    // Inherited by everything it starts: the Wine loader's program, and the
    // Wine server and Wine's own processes when it is the one to start them.
    if (nice_to >= 0) setpriority(PRIO_PROCESS, 0, nice_to);
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, nullptr);
    int devnull = open("/dev/null", O_RDWR | O_CLOEXEC);
    dup2(spec.pipe_stdin ? in[0] : devnull, 0);
    dup2(o[1], 1);
    dup2(spec.pipe_stderr ? er[1] : devnull, 2);
    cloexec_from_3();
    if (!spec.cwd.empty() && chdir(spec.cwd.c_str()) != 0) {
      // Carry on where we are: the host resolves everything it is given.
    }
    execve(spec.program.c_str(), argv.data(), envp.data());
    int e = errno;
    ssize_t w = write(ex[1], &e, sizeof(e));
    (void)w;
    _exit(127);
  }

  // The parent.
  close(ex[1]);
  ex[1] = -1;
  for (int* p : {in + 0, o + 1, er + 1}) {
    if (*p >= 0) close(*p);
    *p = -1;
  }
  int child_errno = 0;
  ssize_t n;
  do {
    n = read(ex[0], &child_errno, sizeof(child_errno));
  } while (n < 0 && errno == EINTR);
  close(ex[0]);
  ex[0] = -1;
  if (n == (ssize_t)sizeof(child_errno)) {
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
    }
    if (error) *error = "cannot run " + spec.program + ": " + strerror(child_errno);
    close_all();
    return false;
  }
  out.pid = pid;
  out.in = in[1];
  out.out = o[0];
  out.err = er[0];
  for (int fd : {out.in, out.out, out.err}) {
    if (fd >= 0) set_nonblock(fd);
  }
  return true;
}

bool reap_child(pid_t pid, int* status) {
  if (pid <= 0) return false;
  int st = 0;
  pid_t r;
  do {
    r = waitpid(pid, &st, WNOHANG);
  } while (r < 0 && errno == EINTR);
  if (r == pid) {
    if (status) *status = st;
    return true;
  }
  if (r < 0 && errno == ECHILD) {
    if (status) *status = -1;
    return true;
  }
  return false;
}

std::string describe_exit(int status) {
  if (status < 0) return "unknown";
  if (WIFEXITED(status)) return "exit code " + std::to_string(WEXITSTATUS(status));
  if (WIFSIGNALED(status)) return "signal " + std::to_string(WTERMSIG(status));
  return "status " + std::to_string(status);
}

std::string host_exit_text(int status) {
  if (status >= 0 && WIFEXITED(status)) return "host exit code " + std::to_string(WEXITSTATUS(status));
  if (status >= 0 && WIFSIGNALED(status)) return "host killed by signal " + std::to_string(WTERMSIG(status));
  return "host ended";
}

// ---- HostProcess ----------------------------------------------------------------

HostProcess::~HostProcess() { stop(0); }

bool HostProcess::start(const Spec& spec, Clock::duration period, std::string* error) {
  SpawnSpec s;
  s.program = spec.wine;
  s.argv = {spec.wine, spec.exe};
  s.argv.insert(s.argv.end(), spec.args.begin(), spec.args.end());
  s.cwd = spec.cwd;
  s.env = spec.env;
  s.nice = spec.nice;
  if (!spawn_child(s, child_, error)) return false;
  sound_ = spec.sound;
  period_ = period;
  started_at_ = Clock::now();
  last_frame_at_ = started_at_;
  // The opening GO goes out with the spawn.
  outq_ = "GO\n";
  gos_sent_ = 1;
  last_go_ = last_sched_ = started_at_;
  resyncs_at_go_ = 0;
  write_some();
  return true;
}

void HostProcess::close_fd(int& fd) {
  if (fd >= 0) close(fd);
  fd = -1;
}

void HostProcess::add_pollfds(std::vector<pollfd>& fds, size_t* base) const {
  *base = fds.size();
  fds.push_back({child_.out, short(child_.out >= 0 ? POLLIN : 0), 0});
  fds.push_back({child_.err, short(child_.err >= 0 ? POLLIN : 0), 0});
  const bool want_out = child_.in >= 0 && !outq_.empty();
  fds.push_back({want_out ? child_.in : -1, short(want_out ? POLLOUT : 0), 0});
}

void HostProcess::on_poll(const std::vector<pollfd>& fds, size_t base) {
  if (base + 3 > fds.size()) return;
  // stderr first: a step's STATUS line is written before its frame.
  if (fds[base + 1].fd >= 0 && fds[base + 1].revents) read_stderr();
  if (fds[base].fd >= 0 && fds[base].revents) read_stdout();
  if (fds[base + 2].fd >= 0 && fds[base + 2].revents) write_some();
}

void HostProcess::read_stdout() {
  if (child_.out < 0) return;
  uint8_t buf[256 * 1024];
  // Bounded, so a flood can't starve the rest of the loop; poll comes back.
  for (int round = 0; round < 32; ++round) {
    ssize_t n = read(child_.out, buf, sizeof(buf));
    if (n > 0) {
      parser_.feed(buf, (size_t)n);
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
    // read() == 0 (every writer gone) or an error: the stream has ended.
    stdout_closed_ = true;
    close_fd(child_.out);
    break;
  }
  RawFrame f;
  while (parser_.take(f)) {
    ++frames_;
    last_frame_at_ = Clock::now();
    std::swap(latest_, f);
    have_frame_ = true;
  }
  // Every STATUS line published before these frames is in the stderr pipe
  // by now; read it so the frame and its verdict are seen together.
  read_stderr();
}

void HostProcess::read_stderr() {
  if (child_.err < 0) return;
  char buf[16384];
  for (int round = 0; round < 16; ++round) {
    ssize_t n = read(child_.err, buf, sizeof(buf));
    if (n > 0) {
      errbuf_.append(buf, (size_t)n);
      size_t start = 0;
      for (;;) {
        size_t nl = errbuf_.find('\n', start);
        if (nl == std::string::npos) break;
        stderr_line(std::string_view(errbuf_).substr(start, nl - start));
        start = nl + 1;
      }
      errbuf_.erase(0, start);
      if (errbuf_.size() > kMaxLogLine) {
        stderr_line(errbuf_);
        errbuf_.clear();
      }
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
    if (!errbuf_.empty()) stderr_line(errbuf_);
    errbuf_.clear();
    close_fd(child_.err);
    break;
  }
}

void HostProcess::stderr_line(std::string_view line) {
  HostStatus s;
  if (parse_status_line(line, s)) {
    status_rec_ = s;
    have_status_ = true;
  }
  hostlog_line(line);
}

bool HostProcess::take_frame(RawFrame& out) {
  if (!have_frame_) return false;
  std::swap(out, latest_);
  have_frame_ = false;
  return true;
}

bool HostProcess::status(HostStatus* out) const {
  if (!have_status_) return false;
  if (out) *out = status_rec_;
  return true;
}

void HostProcess::flush_mouse() {
  if (mouse_text_.empty()) return;
  outq_ += mouse_text_;
  outq_ += "\n";
  mouse_text_.clear();
  mouse_buttons_.clear();
}

uint64_t HostProcess::send_input(const std::string& line) {
  if (quitting_ || child_.in < 0 || stdin_broken_ || ended()) return 0;
  const bool mouse = line.compare(0, 6, "MOUSE ") == 0;
  if (mouse) {
    std::string buttons = line.substr(line.find_last_of(' ') + 1);
    if (!mouse_text_.empty() && buttons == mouse_buttons_) {
      mouse_text_ = line;   // still queued and last: the newer position replaces it
      return mouse_seq_;
    }
    flush_mouse();
    mouse_text_ = line;
    mouse_buttons_ = buttons;
    mouse_seq_ = ++input_seq_;
    return mouse_seq_;
  }
  flush_mouse();
  const uint64_t seq = ++input_seq_;
  outq_ += line;
  outq_ += "\n";
  write_some();
  return seq;
}

void HostProcess::send_line(const std::string& line) {
  if (quitting_ || child_.in < 0 || stdin_broken_) return;
  flush_mouse();
  outq_ += line;
  outq_ += "\n";
  write_some();
}

void HostProcess::write_some() {
  while (child_.in >= 0 && !outq_.empty()) {
    ssize_t n = write(child_.in, outq_.data(), outq_.size());
    if (n > 0) {
      outq_.erase(0, (size_t)n);
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;   // POLLOUT brings us back
    // EPIPE: the host has gone (or closed its stdin); nothing more reaches it.
    stdin_broken_ = true;
    outq_.clear();
    mouse_text_.clear();
    return;
  }
}

Clock::time_point HostProcess::next_go_at(Clock::time_point now) {
  constexpr auto never = Clock::time_point::max();
  if (quitting_ || stdout_closed_ || stdin_broken_ || child_.in < 0) return never;
  // The frame not yet taken: asking for the next would only render a
  // frame nobody sees.
  if (have_frame_) return never;
  int64_t outstanding = (int64_t)gos_sent_ - (int64_t)frames_ - written_off_;
  if (outstanding < 0 && written_off_ > 0) {
    // Answers we had written off arrived after all.
    int64_t back = std::min(written_off_, -outstanding);
    written_off_ -= back;
    outstanding += back;
  }
  if (outstanding >= 1) {
    // A pipe loses nothing, so an unanswered GO normally means a slow step
    // or a slow init: re-sending would only queue frames. The one way an
    // answer goes missing is a frame the parser discarded as garbage; then,
    // and only then, the GO is repeated so the lockstep can't wedge.
    if (parser_.resyncs() == resyncs_at_go_ || now - last_go_ < kGoRetry) {
      return parser_.resyncs() == resyncs_at_go_ ? never : last_go_ + kGoRetry;
    }
    written_off_ += outstanding;
  }
  return std::max(last_sched_ + period_, last_frame_at_);
}

void HostProcess::maybe_send_go(Clock::time_point now) {
  const Clock::time_point due = next_go_at(now);
  if (due == Clock::time_point::max() || now < due) return;
  flush_mouse();
  outq_ += "GO\n";
  ++gos_sent_;
  last_go_ = now;
  last_sched_ = due;
  resyncs_at_go_ = parser_.resyncs();
  write_some();
}

bool HostProcess::check_exit() {
  if (reaped_ || child_.pid <= 0) return false;
  int st = -1;
  if (!reap_child(child_.pid, &st)) return false;
  reaped_ = true;
  status_ = st;
  return true;
}

void HostProcess::request_quit() {
  if (child_.pid <= 0 || quitting_) return;
  quitting_ = true;
  if (child_.in >= 0 && !stdin_broken_) {
    flush_mouse();
    outq_ += "QUIT\n";
    write_some();
  }
  // stdin's end: a host that missed the QUIT exits on it too.
  close_fd(child_.in);
}

void HostProcess::stop(int grace_ms) {
  if (child_.pid <= 0) return;
  if (!reaped_) {
    request_quit();
    const auto deadline = Clock::now() + std::chrono::milliseconds(std::max(grace_ms, 0));
    auto drain_until = [&](Clock::time_point until) {
      while (!check_exit()) {
        const auto now = Clock::now();
        if (now >= until) return;
        pollfd fds[2] = {{child_.out, POLLIN, 0}, {child_.err, POLLIN, 0}};
        int ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(until - now).count();
        poll(fds, 2, std::clamp(ms, 1, 10));
        if (child_.out >= 0 && fds[0].revents) {
          // Frames nobody will see: read and drop, so a write can't block it.
          uint8_t buf[65536];
          ssize_t n;
          while ((n = read(child_.out, buf, sizeof(buf))) > 0) {
          }
          if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
            stdout_closed_ = true;
            close_fd(child_.out);
          }
        }
        if (child_.err >= 0 && fds[1].revents) read_stderr();
      }
    };
    drain_until(deadline);
    if (!reaped_) {
      log_line("host pid=%d: no exit within %d ms of QUIT; SIGTERM", (int)child_.pid, grace_ms);
      signalled_ = true;
      kill(child_.pid, SIGTERM);
      drain_until(Clock::now() + kTermWait);
    }
    if (!reaped_) {
      log_line("host pid=%d: SIGKILL", (int)child_.pid);
      signalled_ = true;
      kill(child_.pid, SIGKILL);
      int st = -1;
      pid_t r;
      do {
        r = waitpid(child_.pid, &st, 0);
      } while (r < 0 && errno == EINTR);
      reaped_ = true;
      status_ = r == child_.pid ? st : -1;
    }
  }
  read_stderr();   // the last lines (the audio summary) for the host log
  close_fd(child_.in);
  close_fd(child_.out);
  close_fd(child_.err);
  child_.pid = -1;
}

void HostProcess::stop_gracefully() { stop(sound_ ? kSoundHostStopGraceMs : kHostStopGraceMs); }

}  // namespace lad
