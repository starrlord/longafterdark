#include "host_process.h"

#include <dwmapi.h>

#include <algorithm>
#include <cstring>

#include "log.h"
#include "paths.h"
#include "sound.h"

namespace adw::scr {

using namespace std::chrono_literals;

// At most one GO in flight: the host renders a frame per GO, so this is the
// lockstep that keeps it paced by our clock instead of racing ahead.
constexpr int64_t kMaxOutstandingGo = 1;
// How long an unanswered GO waits before it is re-sent -- and it only is when
// the parser threw bytes away meanwhile (see clock_tick).
constexpr auto kGoRetry = 1s;
constexpr size_t kPoolMax = 3;

void convert_frame(const RawFrame& raw, Frame& out) {
  out.width = raw.width;
  out.height = raw.height;
  if (raw.format == 8) {
    out.bpp = 8;
    out.stride = (raw.width + 3) & ~3;
    for (int i = 0; i < 256; ++i) {
      out.palette[i] = RGBQUAD{raw.palette[i * 3 + 2], raw.palette[i * 3 + 1], raw.palette[i * 3], 0};
    }
    if (out.stride == raw.width) {
      out.bits.assign(raw.pixels.begin(), raw.pixels.end());
    } else {
      out.bits.assign((size_t)out.stride * raw.height, 0);
      for (int y = 0; y < raw.height; ++y) {
        memcpy(out.bits.data() + (size_t)y * out.stride, raw.pixels.data() + (size_t)y * raw.width, raw.width);
      }
    }
  } else {
    out.bpp = 32;
    out.stride = raw.width * 4;
    out.bits.resize((size_t)out.stride * raw.height);
    const uint8_t* s = raw.pixels.data();
    uint8_t* d = out.bits.data();
    for (size_t i = 0, n = (size_t)raw.width * raw.height; i < n; ++i, s += 3, d += 4) {
      d[0] = s[2];
      d[1] = s[1];
      d[2] = s[0];
      d[3] = 0;
    }
  }
}

std::wstring build_environment_block(const std::vector<std::pair<std::wstring, std::wstring>>& changes) {
  std::vector<std::wstring> vars;
  if (LPWCH base = GetEnvironmentStringsW()) {
    for (LPWCH p = base; *p; p += wcslen(p) + 1) vars.emplace_back(p);
    FreeEnvironmentStringsW(base);
  }
  for (const auto& [k, v] : changes) {
    std::erase_if(vars, [&](const std::wstring& e) {
      // Search from 1: the per-drive "=C:=C:\dir" entries start with '='.
      size_t eq = e.find(L'=', 1);
      return eq == k.size() &&
             CompareStringOrdinal(e.c_str(), (int)eq, k.c_str(), (int)k.size(), TRUE) == CSTR_EQUAL;
    });
    if (!v.empty()) vars.push_back(k + L"=" + v);
  }
  // CreateProcess documents the block as sorted (case-insensitively).
  std::sort(vars.begin(), vars.end(), [](const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), (int)a.size(), b.c_str(), (int)b.size(), TRUE) == CSTR_LESS_THAN;
  });
  std::wstring block;
  for (const auto& e : vars) {
    block += e;
    block.push_back(L'\0');
  }
  if (vars.empty()) block.push_back(L'\0');
  block.push_back(L'\0');
  return block;
}

void add_host_defaults(std::vector<std::pair<std::wstring, std::wstring>>& env) {
  auto has = [&](const wchar_t* k) {
    return std::any_of(env.begin(), env.end(), [&](const auto& kv) { return _wcsicmp(kv.first.c_str(), k) == 0; });
  };
  if (!has(L"ADSTATE")) env.emplace_back(L"ADSTATE", state_dir());
  // Silent unless the spawn says otherwise: only the saver's primary host
  // plays (sound.h), and an inherited ADSOUND or ADAUDIOOUT must never turn
  // any other on. (A spawn that only removes ADSOUND has said nothing.)
  const bool says_sound = std::any_of(env.begin(), env.end(), [](const auto& kv) {
    return _wcsicmp(kv.first.c_str(), L"ADSOUND") == 0 && !kv.second.empty();
  });
  if (!says_sound) add_sound_env(env, SoundChoice{});
}

void add_host_control_env(std::vector<std::pair<std::wstring, std::wstring>>& env,
                          const std::vector<std::pair<std::wstring, std::wstring>>& controls) {
  // build_environment_block applies changes in order: the later one wins.
  env.insert(env.begin(), controls.begin(), controls.end());
}

HANDLE create_kill_on_close_job() {
  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (!job) return nullptr;
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
  info.BasicLimitInformation.LimitFlags =
      JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
  SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info));
  return job;
}

// ---- HostProcess ---------------------------------------------------------------

HostProcess::HostProcess(Notify n) : notify_(n) {}

HostProcess::~HostProcess() { stop(0); }

bool HostProcess::start(const HostSpec& spec, HANDLE job, std::wstring* error) {
  auto fail = [&](const wchar_t* what) {
    if (error) *error = std::wstring(what) + L" failed (error " + std::to_wstring(GetLastError()) + L")";
    return false;
  };
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE in_r = nullptr, in_w = nullptr, out_r = nullptr, out_w = nullptr;
  if (!CreatePipe(&in_r, &in_w, &sa, 4096)) return fail(L"CreatePipe(stdin)");
  // A roomy stdout pipe lets the host hand over a whole frame per write.
  if (!CreatePipe(&out_r, &out_w, &sa, 1 << 20)) {
    CloseHandle(in_r);
    CloseHandle(in_w);
    return fail(L"CreatePipe(stdout)");
  }
  SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
  HANDLE err_h = INVALID_HANDLE_VALUE;
  if (!spec.stderr_path.empty()) {
    err_h = CreateFileW(spec.stderr_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  }
  if (err_h == INVALID_HANDLE_VALUE) {
    err_h = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
  }

  // The status record (INTERACTION.md §3.4): an unnamed pagefile section the
  // host maps read/write; we map it read-only.
  HANDLE section = nullptr;
  if (spec.status_record) {
    section = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, adw::kStatusSectionBytes, nullptr);
    if (!section) log_line("warning: status section failed (error %lu)", GetLastError());
  }

  // Inherit exactly these handles. Several hosts are spawned from one
  // process; without the list each would inherit the others' pipe ends (and
  // status records) and a dead host's stdout would never report EOF.
  HANDLE inherit[4];
  size_t n_inherit = 0;
  inherit[n_inherit++] = in_r;
  inherit[n_inherit++] = out_w;
  if (err_h != INVALID_HANDLE_VALUE) inherit[n_inherit++] = err_h;
  if (section) inherit[n_inherit++] = section;
  SIZE_T attr_size = 0;
  InitializeProcThreadAttributeList(nullptr, 2, 0, &attr_size);
  std::vector<uint8_t> attr_buf(attr_size);
  auto attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
  const bool attrs_init = !attr_buf.empty() && InitializeProcThreadAttributeList(attrs, 2, 0, &attr_size);
  const bool have_attrs = attrs_init &&
                          UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit,
                                                    n_inherit * sizeof(HANDLE), nullptr, nullptr);
  // Created inside the Job, so there is no moment (between creating it and
  // assigning it) when our own exit would leave it behind.
  const bool in_job = have_attrs && job &&
                      UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, &job, sizeof(job), nullptr, nullptr);

  STARTUPINFOEXW si{};
  si.StartupInfo.cb = sizeof(si);
  si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  si.StartupInfo.hStdInput = in_r;
  si.StartupInfo.hStdOutput = out_w;
  si.StartupInfo.hStdError = err_h;
  si.lpAttributeList = have_attrs ? attrs : nullptr;

  std::wstring cmd = quote_arg(spec.exe) + L" " + quote_arg(spec.module_path);
  std::vector<std::pair<std::wstring, std::wstring>> env_changes = spec.env;
  add_host_defaults(env_changes);
  sound_ = false;
  for (const auto& [k, v] : env_changes) {
    if (_wcsicmp(k.c_str(), L"ADSOUND") == 0) sound_ = v == L"1";
  }
  // Always set (or removed): a stale inherited value must never name one of
  // our handles that this host does not have.
  env_changes.emplace_back(L"ADSTATUSHANDLE", section ? std::to_wstring((uintptr_t)section) : L"");
  std::wstring env = build_environment_block(env_changes);
  DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | spec.priority_class;
  if (have_attrs) flags |= EXTENDED_STARTUPINFO_PRESENT;
  PROCESS_INFORMATION pi{};
  BOOL ok = CreateProcessW(spec.exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, flags, env.data(),
                           spec.working_dir.empty() || !dir_exists(spec.working_dir) ? nullptr
                                                                                     : spec.working_dir.c_str(),
                           &si.StartupInfo, &pi);
  DWORD create_err = GetLastError();
  if (attrs_init) DeleteProcThreadAttributeList(attrs);
  CloseHandle(in_r);
  CloseHandle(out_w);
  if (err_h != INVALID_HANDLE_VALUE) CloseHandle(err_h);
  if (!ok) {
    CloseHandle(in_w);
    CloseHandle(out_r);
    if (section) CloseHandle(section);
    SetLastError(create_err);
    return fail(L"CreateProcess");
  }
  if (section) {
    // Ours from here on: no later child may inherit it by accident.
    SetHandleInformation(section, HANDLE_FLAG_INHERIT, 0);
    status_section_ = section;
    status_view_ = MapViewOfFile(section, FILE_MAP_READ, 0, 0, sizeof(adw::AdwHostStatusV1));
  }
  // Join the Job before the first instruction runs, so even a host that
  // spawns helpers can't outlive the saver (already in it when created there).
  if (job && !in_job && !AssignProcessToJobObject(job, pi.hProcess)) {
    log_line("warning: AssignProcessToJobObject failed (error %lu)", GetLastError());
  }
  ResumeThread(pi.hThread);
  CloseHandle(pi.hThread);

  process_ = pi.hProcess;
  pid_ = pi.dwProcessId;
  stdin_w_ = in_w;
  stdout_r_ = out_r;
  started_at_ = Clock::now();
  // The opening GO goes out with the spawn: the host waits only briefly
  // (ADGOWAITMS, 250 ms) for it before frame 0, and a frame 0 it renders
  // unasked would leave us a frame behind for the whole session.
  go_pending_ = true;
  gos_sent_ = 1;
  last_go_ = started_at_;
  resyncs_at_go_ = 0;
  written_off_ = 0;
  reader_ = std::thread([this] { reader_main(); });
  writer_ = std::thread([this] { writer_main(); });
  return true;
}

void HostProcess::request_quit() {
  std::lock_guard lk(mu_);
  if (quitting_ || !process_) return;
  quitting_ = true;
  outq_ += "QUIT\n";
  mouse_tail_pos_ = std::string::npos;
  urgent_ = true;
  cv_.notify_all();
}

void HostProcess::stop(DWORD grace_ms) {
  if (!process_) return;
  request_quit();
  if (WaitForSingleObject(process_, grace_ms) != WAIT_OBJECT_0) TerminateProcess(process_, 0);
  WaitForSingleObject(process_, 2000);
  // The host is gone, so its pipe ends are closed: a blocked ReadFile/WriteFile
  // fails with ERROR_BROKEN_PIPE. CancelSynchronousIo covers a grandchild that
  // inherited a pipe end and kept it open.
  for (std::thread* t : {&reader_, &writer_}) {
    if (!t->joinable()) continue;
    HANDLE th = (HANDLE)t->native_handle();
    while (WaitForSingleObject(th, 50) == WAIT_TIMEOUT) CancelSynchronousIo(th);
    t->join();
  }
  CloseHandle(stdin_w_);
  CloseHandle(stdout_r_);
  CloseHandle(process_);
  stdin_w_ = stdout_r_ = process_ = nullptr;
  if (status_view_) UnmapViewOfFile(status_view_);
  if (status_section_) CloseHandle(status_section_);
  status_view_ = nullptr;
  status_section_ = nullptr;
}

void HostProcess::stop_gracefully() { stop(sound_ ? kSoundHostStopGraceMs : kHostStopGraceMs); }

void HostProcess::send_line(const std::string& line) {
  std::lock_guard lk(mu_);
  if (quitting_) return;
  outq_ += line;
  outq_ += "\n";
  mouse_tail_pos_ = std::string::npos;
  urgent_ = true;
  cv_.notify_all();
}

uint64_t HostProcess::send_input(const std::string& line) {
  std::lock_guard lk(mu_);
  if (quitting_ || !process_) return 0;
  const bool mouse = line.compare(0, 6, "MOUSE ") == 0;
  std::string buttons;
  if (mouse) {
    size_t sp = line.find_last_of(' ');
    buttons = line.substr(sp + 1);
    if (mouse_tail_pos_ != std::string::npos && buttons == mouse_tail_buttons_) {
      // Still queued and last: the newer position replaces it.
      outq_.resize(mouse_tail_pos_);
      outq_ += line;
      outq_ += "\n";
      return mouse_tail_seq_;
    }
  }
  const uint64_t seq = ++input_seq_;
  if (mouse) {
    mouse_tail_pos_ = outq_.size();
    mouse_tail_seq_ = seq;
    mouse_tail_buttons_ = buttons;
  } else {
    mouse_tail_pos_ = std::string::npos;
    urgent_ = true;
  }
  outq_ += line;
  outq_ += "\n";
  cv_.notify_all();
  return seq;
}

uint64_t HostProcess::input_seq() const {
  std::lock_guard lk(mu_);
  return input_seq_;
}

bool HostProcess::read_status(adw::AdwHostStatusV1* out) const {
  return status_view_ && adw::read_status(status_view_, out);
}

void HostProcess::clock_tick(Clock::time_point now) {
  std::lock_guard lk(mu_);
  if (quitting_ || writer_done_ || go_pending_ || stdout_closed_) return;
  // Backpressure: while the UI hasn't taken the last frame, asking for the
  // next would only render a frame nobody sees.
  if (latest_) return;
  int64_t outstanding = (int64_t)gos_sent_.load() - (int64_t)frames_.load() - written_off_;
  if (outstanding < 0 && written_off_ > 0) {
    // Answers we had written off arrived after all (the step was merely slow
    // and the garbage was elsewhere): take them back rather than let every
    // such episode leave one more GO permanently in flight.
    int64_t back = std::min(written_off_, -outstanding);
    written_off_ -= back;
    outstanding += back;
  }
  if (outstanding >= kMaxOutstandingGo) {
    // A pipe loses nothing, so an unanswered GO normally means a slow step or
    // a slow init (seconds, for some modules): re-sending would only queue
    // frames the host then renders back to back. The one way an answer goes
    // missing is a frame the parser had to discard as garbage; then, and
    // only then, the GO is repeated so the lockstep can't wedge.
    if (resyncs_.load() == resyncs_at_go_ || now - last_go_ < kGoRetry) return;
    // The frames that would have balanced the count are gone for good. Left
    // counted, `outstanding` could never fall below 1 again: every later tick
    // would take this branch, and with no fresh garbage it would never send.
    written_off_ += outstanding;
  }
  if (now - last_go_ < min_go_interval_.load()) return;
  go_pending_ = true;
  ++gos_sent_;
  last_go_ = now;
  resyncs_at_go_ = resyncs_.load();
  cv_.notify_all();
}

std::unique_ptr<Frame> HostProcess::take_frame() {
  // Clear the flag before taking, so a frame published after this point
  // always produces a fresh notification.
  notify_pending_ = false;
  std::lock_guard lk(mu_);
  return std::move(latest_);
}

void HostProcess::recycle(std::unique_ptr<Frame> f) {
  if (!f) return;
  std::lock_guard lk(mu_);
  if (pool_.size() < kPoolMax) pool_.push_back(std::move(f));
}

std::unique_ptr<Frame> HostProcess::pooled() {
  std::lock_guard lk(mu_);
  if (pool_.empty()) return std::make_unique<Frame>();
  auto f = std::move(pool_.back());
  pool_.pop_back();
  return f;
}

DWORD HostProcess::exit_code() const {
  if (!process_) return STILL_ACTIVE;
  // Once stdout has closed the process is on its way out; give it a moment
  // to finish so the code is real rather than STILL_ACTIVE.
  if (stdout_closed_) WaitForSingleObject(process_, 200);
  DWORD code = STILL_ACTIVE;
  GetExitCodeProcess(process_, &code);
  return code;
}

bool HostProcess::exited() const {
  if (stdout_closed_) return true;
  return process_ && WaitForSingleObject(process_, 0) == WAIT_OBJECT_0;
}

Clock::time_point HostProcess::last_frame_at() const {
  std::lock_guard lk(mu_);
  return last_frame_at_;
}

void HostProcess::reader_main() {
  FrameParser parser;
  RawFrame raw;
  std::vector<uint8_t> buf(256 * 1024);
  for (;;) {
    DWORD got = 0;
    if (!ReadFile(stdout_r_, buf.data(), (DWORD)buf.size(), &got, nullptr)) break;   // EOF = broken pipe
    if (got == 0) continue;                                                         // zero-length write
    parser.feed(buf.data(), got);
    while (parser.take(raw)) {
      auto f = pooled();
      convert_frame(raw, *f);
      {
        std::lock_guard lk(mu_);
        if (latest_ && pool_.size() < kPoolMax) pool_.push_back(std::move(latest_));   // dropped, never shown
        latest_ = std::move(f);
        last_frame_at_ = Clock::now();
      }
      ++frames_;
      if (!notify_pending_.exchange(true)) {
        PostMessageW(notify_.hwnd, notify_.frame_msg, notify_.cookie, 0);
      }
    }
    resyncs_ = parser.resyncs();
  }
  stdout_closed_ = true;
  {
    std::lock_guard lk(mu_);
    cv_.notify_all();   // let the writer notice
  }
  PostMessageW(notify_.hwnd, notify_.exit_msg, notify_.cookie, 0);
}

void HostProcess::writer_main() {
  std::unique_lock lk(mu_);
  for (;;) {
    // Queued MOUSE moves alone wait for the next GO (the host applies input
    // per step anyway), so a burst of moves between two steps is one line.
    cv_.wait(lk, [&] { return quitting_ || stdout_closed_ || go_pending_ || urgent_; });
    std::string out = std::move(outq_);
    outq_.clear();
    urgent_ = false;
    mouse_tail_pos_ = std::string::npos;   // written: nothing left to coalesce with
    if (go_pending_ && !quitting_) out += "GO\n";   // never a GO after QUIT
    go_pending_ = false;
    bool last = quitting_ || stdout_closed_;
    lk.unlock();
    bool ok = true;
    if (!out.empty()) {
      DWORD put = 0;
      ok = WriteFile(stdin_w_, out.data(), (DWORD)out.size(), &put, nullptr) && put == out.size();
    }
    lk.lock();
    if (!ok || (last && outq_.empty())) break;
  }
  writer_done_ = true;
}

// ---- Pacer ---------------------------------------------------------------------

Pacer::~Pacer() { stop(); }

void Pacer::start() {
  if (thread_.joinable()) return;
  stop_ = false;
  thread_ = std::thread([this] { run(); });
}

void Pacer::stop() {
  stop_ = true;
  if (thread_.joinable()) thread_.join();
}

void Pacer::add(HostProcess* h) {
  std::lock_guard lk(mu_);
  hosts_.push_back(h);
}

void Pacer::remove(HostProcess* h) {
  std::lock_guard lk(mu_);
  std::erase(hosts_, h);
}

void Pacer::run() {
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
  const bool dwm_allowed = !env_set(L"AD_SCR_NO_DWM");
  bool use_dwm = dwm_allowed;
  int dwm_misses = 0;
  while (!stop_) {
    if (paused_) {
      // DWM stops composing with the display off; ask it again afterwards.
      use_dwm = dwm_allowed;
      dwm_misses = 0;
      Sleep(50);
      continue;
    }
    bool waited = false;
    if (use_dwm) {
      // DwmFlush returns at the next composition pass: vsync-aligned GOs. On a
      // desktop DWM isn't composing (or can't, e.g. the secure desktop) it
      // fails or returns at once; after a run of those, stop asking.
      auto t0 = Clock::now();
      HRESULT hr = DwmFlush();
      if (SUCCEEDED(hr) && Clock::now() - t0 >= 2ms) {
        waited = true;
        dwm_misses = 0;
      } else if (++dwm_misses > 120) {
        use_dwm = false;
      }
    }
    if (!waited) {
      LARGE_INTEGER due;
      due.QuadPart = -166667;   // 16.67 ms, relative, in 100 ns units
      if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, 100);
      else Sleep(16);
    }
    auto now = Clock::now();
    std::lock_guard lk(mu_);
    for (HostProcess* h : hosts_) h->clock_tick(now);
  }
  if (timer) CloseHandle(timer);
}

} // namespace adw::scr
