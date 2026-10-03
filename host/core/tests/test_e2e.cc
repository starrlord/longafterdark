// End-to-end: spawn adhostwin.exe the way a front-end does (CreateProcess,
// anonymous pipes on stdin/stdout/stderr, a kill-on-close job) and speak the
// protocol to it. Frames are parsed with a strict reference reader of the
// frame grammar (parse_header/take_frame below), so anything this test
// accepts is a well-formed frame to any front-end's reader.
//
//   adw_core_e2e <adhostwin.exe>            protocol tests on --test-pattern
//   adw_core_e2e <adhostwin.exe> --assets   lane detection on the real modules
//                                           (exit 77 = skipped, assets absent)
//   adw_core_e2e <adhostwin.exe> --audio-assets
//                                           real modules' sound (AUDIO.md §10.3):
//                                           opt-in with AD_AUDIO_ASSETS=1 (or a
//                                           comma list of case names); 77 = skipped
//
// No test here opens a sound device: sound-on runs are headless (captures)
// or say ADAUDIOLIVE=0.
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "adw/core/audio.h"
#include "adw/core/env.h"
#include "adw/core/fnv.h"
#include "adw/core/lane.h"
#include "adw/core/status.h"
#include "adw/core/text.h"
#include "audio_internal.h"
#include "check.h"
#include "test_paths.h"

using namespace adw;

// Defined by core's CMakeLists when a lane component registered and was linked
// into adhostwin; without the lane a real module exits 3 ("not built in").
#ifndef ADW_HAVE_LANE_PE32
#define ADW_HAVE_LANE_PE32 0
#endif
#ifndef ADW_HAVE_LANE_NE16
#define ADW_HAVE_LANE_NE16 0
#endif

namespace {

std::wstring g_exe;
// Every child's AD_LOCALAPPDATA unless a test sets its own: a scratch base
// (never created by us), so no host this test starts resolves a default
// location under the real %LOCALAPPDATA%.
std::wstring g_scratch_lad;

// ---- reference frame reader ------------------------------------------------

struct Frame {
  std::vector<uint8_t> body;
  int w = 0, h = 0, tag = 0;  // tag 8 = P8 (768 palette + w*h), 6 = P6 (w*h*3)
};

// parse_header: tokens separated by whitespace, '#' comments to end of line;
// a token only counts once its delimiter is buffered; exactly one whitespace
// byte separates the last token from the body. nullopt = need more bytes (or
// not a frame header).
struct Header {
  size_t body_start;
  int w, h, tag;
};
std::optional<Header> parse_header(const std::vector<uint8_t>& b) {
  size_t n = b.size(), i = 0;
  auto is_ws = [](uint8_t c) { return c == 0x20 || c == 0x09 || c == 0x0A || c == 0x0D; };
  auto skip_ws = [&]() {
    while (i < n) {
      uint8_t c = b[i];
      if (c == 0x23) {
        while (i < n && b[i] != 0x0A) i++;
      } else if (is_ws(c)) {
        i++;
      } else {
        return true;
      }
    }
    return false;
  };
  auto token = [&]() -> std::optional<std::string> {
    if (!skip_ws()) return std::nullopt;
    size_t start = i;
    while (i < n) {
      uint8_t c = b[i];
      if (is_ws(c) || c == 0x23) break;
      i++;
    }
    if (i >= n) return std::nullopt;
    return std::string(b.begin() + long(start), b.begin() + long(i));
  };
  // An integer token: an optional single '+'/'-', then one or more ASCII
  // digits; nullopt on anything else or on Int64 overflow. The range checks
  // below then reject what the reader would reject.
  auto to_int = [](const std::optional<std::string>& s) -> std::optional<int64_t> {
    if (!s) return std::nullopt;
    size_t i = 0;
    bool neg = false;
    if (i < s->size() && ((*s)[i] == '+' || (*s)[i] == '-')) neg = (*s)[i++] == '-';
    if (i == s->size()) return std::nullopt;
    uint64_t mag = 0;
    const uint64_t limit = neg ? uint64_t(INT64_MAX) + 1 : uint64_t(INT64_MAX);
    for (; i < s->size(); i++) {
      char c = (*s)[i];
      if (c < '0' || c > '9') return std::nullopt;
      if (mag > (limit - uint64_t(c - '0')) / 10) return std::nullopt;
      mag = mag * 10 + uint64_t(c - '0');
    }
    return neg ? int64_t(0 - mag) : int64_t(mag);
  };
  auto magic = token();
  if (!magic || (*magic != "P6" && *magic != "P8")) return std::nullopt;
  int tag = *magic == "P8" ? 8 : 6;
  auto w = to_int(token());
  if (!w || *w <= 0 || *w >= 20000) return std::nullopt;
  auto h = to_int(token());
  if (!h || *h <= 0 || *h >= 20000) return std::nullopt;
  if (tag == 6) {
    auto m = to_int(token());
    if (!m || *m <= 0 || *m >= 256) return std::nullopt;
  }
  return Header{i + 1, int(*w), int(*h), tag};
}

// take_frame: removes and returns the leading complete frame, else leaves the
// buffer untouched.
std::optional<Frame> take_frame(std::vector<uint8_t>& buf) {
  auto hdr = parse_header(buf);
  if (!hdr) return std::nullopt;
  size_t body_len = hdr->tag == 8 ? 768 + size_t(hdr->w) * size_t(hdr->h) : size_t(hdr->w) * size_t(hdr->h) * 3;
  size_t end = hdr->body_start + body_len;
  if (buf.size() < end) return std::nullopt;
  Frame f;
  f.body.assign(buf.begin() + long(hdr->body_start), buf.begin() + long(end));
  f.w = hdr->w;
  f.h = hdr->h;
  f.tag = hdr->tag;
  buf.erase(buf.begin(), buf.begin() + long(end));
  return f;
}

// ---- child process -----------------------------------------------------------

struct ChildOptions {
  std::vector<std::string> args;
  std::map<std::string, std::string> env;
  bool stdin_pipe = true;           // else NUL
  HANDLE stdout_override = nullptr; // e.g. a disk file (inheritable)
  // NULL handles in STARTUPINFO. Under CREATE_NO_WINDOW the child has a hidden
  // console and Windows substitutes that console's handles for NULL ones, so
  // these reach the host's console branches without ever showing a window.
  bool null_stdin = false, null_stdout = false, null_stderr = false;
  bool stderr_is_stdout = false;    // one pipe passed as both stdout and stderr
  bool stderr_dup_of_stdout = false;// stderr = a duplicate handle of stdout (a shell's 2>&1)
  bool detached = false;            // DETACHED_PROCESS: no console, NULLs stay NULL
};

class Child {
 public:
  ~Child() {
    if (pi_.hProcess) {
      TerminateProcess(pi_.hProcess, 99);
      WaitForSingleObject(pi_.hProcess, 2000);
    }
    close_stdin();
    close_stdout();
    if (err_thread_.joinable()) err_thread_.join();
    if (err_r_) CloseHandle(err_r_);
    if (pi_.hProcess) CloseHandle(pi_.hProcess);
    if (job_) CloseHandle(job_);
  }

  bool start(const ChildOptions& o) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE in_r = nullptr, out_w = nullptr, err_w = nullptr;
    if (o.null_stdin) {
      // leave NULL
    } else if (o.stdin_pipe) {
      if (!CreatePipe(&in_r, &in_w_, &sa, 0)) return false;
      SetHandleInformation(in_w_, HANDLE_FLAG_INHERIT, 0);
    } else {
      in_r = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    }
    if (o.stdout_override) {
      out_w = o.stdout_override;
    } else if (!o.null_stdout) {
      if (!CreatePipe(&out_r_, &out_w, &sa, 1 << 16)) return false;
      SetHandleInformation(out_r_, HANDLE_FLAG_INHERIT, 0);
    }
    if (o.stderr_is_stdout) {
      err_w = out_w;
    } else if (o.stderr_dup_of_stdout) {
      if (!DuplicateHandle(GetCurrentProcess(), out_w, GetCurrentProcess(), &err_w, 0, TRUE,
                           DUPLICATE_SAME_ACCESS))
        return false;
    } else if (!o.null_stderr) {
      if (!CreatePipe(&err_r_, &err_w, &sa, 0)) return false;
      SetHandleInformation(err_r_, HANDLE_FLAG_INHERIT, 0);
    }

    // Environment: ours minus any AD* variable (a developer's shell must not
    // leak ADSTREAM etc. into the test), plus the scratch AD_LOCALAPPDATA,
    // plus the test's own.
    auto ci_less = [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; };
    std::map<std::wstring, std::wstring, decltype(ci_less)> vars(ci_less);
    if (wchar_t* block = GetEnvironmentStringsW()) {
      for (const wchar_t* p = block; *p; p += wcslen(p) + 1) {
        std::wstring e(p);
        size_t eq = e.find(L'=', 1);
        if (eq == std::wstring::npos || e[0] == L'=') continue;
        std::wstring name = e.substr(0, eq);
        if (_wcsnicmp(name.c_str(), L"AD", 2) == 0) continue;
        vars[name] = e.substr(eq + 1);
      }
      FreeEnvironmentStringsW(block);
    }
    if (!g_scratch_lad.empty()) vars[L"AD_LOCALAPPDATA"] = g_scratch_lad;
    for (auto& [k, v] : o.env) vars[widen(k)] = widen(v);
    std::wstring env_block;
    for (auto& [k, v] : vars) env_block += k + L"=" + v + L'\0';
    env_block += L'\0';

    std::wstring cmd = L"\"" + g_exe + L"\"";
    for (auto& a : o.args) cmd += L" \"" + widen(a) + L"\"";

    // A front-end puts hosts in a kill-on-close job; so do we, so a failed
    // check can never leave a host running.
    job_ = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim{};
    lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &lim, sizeof(lim));

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_r;
    si.hStdOutput = out_w;
    si.hStdError = err_w;
    DWORD console = o.detached ? DETACHED_PROCESS : CREATE_NO_WINDOW;
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                             CREATE_UNICODE_ENVIRONMENT | console | CREATE_SUSPENDED, env_block.data(),
                             nullptr, &si, &pi_);
    // Our copies of the child's ends must go, or EOF never propagates.
    if (in_r && in_r != INVALID_HANDLE_VALUE) CloseHandle(in_r);
    if (out_w && !o.stdout_override) CloseHandle(out_w);
    if (err_w && !o.stderr_is_stdout) CloseHandle(err_w);
    if (!ok) {
      fprintf(stderr, "  CreateProcess failed: %lu\n", GetLastError());
      pi_ = {};
      return false;
    }
    AssignProcessToJobObject(job_, pi_.hProcess);
    ResumeThread(pi_.hThread);
    CloseHandle(pi_.hThread);
    pi_.hThread = nullptr;
    if (err_r_) {
      err_thread_ = std::thread([this] {
        char buf[4096];
        DWORD n;
        while (ReadFile(err_r_, buf, sizeof(buf), &n, nullptr) && n > 0) {
          std::lock_guard<std::mutex> lock(err_mu_);
          err_.append(buf, n);
        }
      });
    }
    return true;
  }

  bool send(const std::string& s) {
    DWORD n = 0;
    return in_w_ && WriteFile(in_w_, s.data(), DWORD(s.size()), &n, nullptr) && n == s.size();
  }
  void close_stdin() {
    if (in_w_) CloseHandle(in_w_);
    in_w_ = nullptr;
  }
  void close_stdout() {
    if (out_r_) CloseHandle(out_r_);
    out_r_ = nullptr;
  }

  // Pull whatever stdout has, without blocking. False once the pipe is closed.
  bool pump() {
    if (!out_r_ || out_eof_) return false;
    DWORD avail = 0;
    if (!PeekNamedPipe(out_r_, nullptr, 0, nullptr, &avail, nullptr)) {
      out_eof_ = true;
      return false;
    }
    if (avail) {
      size_t old = buf_.size();
      buf_.resize(old + avail);
      DWORD n = 0;
      ReadFile(out_r_, buf_.data() + old, avail, &n, nullptr);
      buf_.resize(old + n);
    }
    return true;
  }

  std::optional<Frame> read_frame(DWORD timeout_ms = 5000) {
    DWORD t0 = GetTickCount();
    for (;;) {
      if (auto f = take_frame(buf_)) return f;
      bool open = pump();
      if (auto f = take_frame(buf_)) return f;
      if (!open || GetTickCount() - t0 > timeout_ms) return std::nullopt;
      Sleep(1);
    }
  }

  // Bytes that arrived and are not part of any frame taken so far.
  size_t pending_bytes(DWORD settle_ms) {
    Sleep(settle_ms);
    pump();
    return buf_.size();
  }
  bool stdout_eof(DWORD timeout_ms = 5000) {
    DWORD t0 = GetTickCount();
    while (pump()) {
      if (GetTickCount() - t0 > timeout_ms) return false;
      Sleep(1);
    }
    return true;
  }

  // Exit code, or -1 on timeout (the process is then killed).
  int wait_exit(DWORD timeout_ms = 10000) {
    if (!pi_.hProcess) return -2;
    if (WaitForSingleObject(pi_.hProcess, timeout_ms) != WAIT_OBJECT_0) {
      TerminateProcess(pi_.hProcess, 99);
      WaitForSingleObject(pi_.hProcess, 2000);
      return -1;
    }
    DWORD code = 0;
    GetExitCodeProcess(pi_.hProcess, &code);
    return int(code);
  }

  // stderr (complete once the process has exited).
  std::string err() {
    if (err_thread_.joinable() && pi_.hProcess && WaitForSingleObject(pi_.hProcess, 0) == WAIT_OBJECT_0)
      err_thread_.join();
    std::lock_guard<std::mutex> lock(err_mu_);
    return err_;
  }

  size_t stdout_bytes_seen() {
    pump();
    return buf_.size();
  }
  // Everything stdout carried (complete once the process has exited and the
  // pipe has drained).
  std::string stdout_text() {
    stdout_eof(2000);
    return std::string(buf_.begin(), buf_.end());
  }

 private:
  PROCESS_INFORMATION pi_{};
  HANDLE job_ = nullptr, in_w_ = nullptr, out_r_ = nullptr, err_r_ = nullptr;
  std::thread err_thread_;
  std::mutex err_mu_;
  std::string err_;
  std::vector<uint8_t> buf_;
  bool out_eof_ = false;
};

// "FBHASH <frame> <hex64>" lines, in order.
std::vector<std::pair<uint64_t, uint64_t>> fbhashes(const std::string& err) {
  std::vector<std::pair<uint64_t, uint64_t>> out;
  size_t pos = 0;
  while ((pos = err.find("FBHASH ", pos)) != std::string::npos) {
    unsigned long long f = 0, h = 0;
    if (sscanf(err.c_str() + pos, "FBHASH %llu %llx", &f, &h) == 2) out.emplace_back(f, h);
    pos += 7;
  }
  return out;
}

std::vector<uint64_t> hash_values(const std::vector<std::pair<uint64_t, uint64_t>>& v) {
  std::vector<uint64_t> out;
  for (auto& p : v) out.push_back(p.second);
  return out;
}

uint64_t frame_hash(const Frame& f) { return fnv1a64(f.body.data(), f.body.size()); }

std::string temp_dir() {
  wchar_t buf[MAX_PATH];
  GetTempPathW(MAX_PATH, buf);
  return narrow(buf);
}

std::string write_temp(const std::string& name, const std::vector<uint8_t>& bytes) {
  std::string path = temp_dir() + "adw_e2e_" + std::to_string(GetCurrentProcessId()) + "_" + name;
  FILE* f = _wfopen(widen(path).c_str(), L"wb");
  if (f) {
    fwrite(bytes.data(), 1, bytes.size(), f);
    fclose(f);
  }
  return path;
}

const std::map<std::string, std::string> kSmall = {{"ADSCREENW", "160"}, {"ADSCREENH", "120"}};

std::map<std::string, std::string> with(std::map<std::string, std::string> base,
                                        const std::map<std::string, std::string>& extra) {
  for (auto& [k, v] : extra) base[k] = v;
  return base;
}

// Headless reference hashes for the small test pattern.
std::vector<uint64_t> headless_reference(int frames) {
  Child c;
  ChildOptions o;
  o.args = {"--test-pattern"};
  o.env = with(kSmall, {{"ADFRAMES", std::to_string(frames)}, {"ADFBHASH", "1"}});
  o.stdin_pipe = false;
  if (!c.start(o)) return {};
  c.wait_exit();
  return hash_values(fbhashes(c.err()));
}

// One scripted lockstep session: each entry is sent as-is, and every "GO" in
// it is answered by exactly one frame before the next entry goes out.
struct Session {
  std::vector<Frame> frames;
  std::vector<uint64_t> stderr_hashes;
  int exit_code = -3;
  size_t stray_bytes = 0;
  std::string err;
};
Session run_session(const std::vector<std::string>& script, const std::map<std::string, std::string>& env) {
  Session s;
  Child c;
  ChildOptions o;
  o.args = {"--test-pattern"};
  o.env = with(env, {{"ADSTREAM", "1"}, {"ADFBHASH", "1"}});
  if (!c.start(o)) return s;
  for (const std::string& line : script) {
    c.send(line);
    size_t gos = 0;
    for (size_t p = 0; (p = line.find("GO\n", p)) != std::string::npos; p += 3) gos++;
    for (size_t k = 0; k < gos; k++) {
      auto f = c.read_frame();
      if (!f) {
        fprintf(stderr, "  no frame for GO #%zu\n", s.frames.size());
        return s;
      }
      s.frames.push_back(std::move(*f));
    }
  }
  s.stray_bytes = c.pending_bytes(150);  // lockstep: nothing unrequested
  c.send("QUIT\n");
  s.exit_code = c.wait_exit();
  s.err = c.err();
  s.stderr_hashes = hash_values(fbhashes(s.err));
  return s;
}

}  // namespace

// ---- tests -------------------------------------------------------------------

TEST(reference_frame_reader) {
  // The reader itself, on the header shapes a front-end must handle:
  // what it accepts, what it rejects, and "need more bytes" on a partial frame.
  auto bytes = [](const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); };
  auto hdr = [&](const std::string& s) { return parse_header(bytes(s)); };
  auto h = hdr("P8\n2 3\n");
  CHECK(h && h->tag == 8 && h->w == 2 && h->h == 3 && h->body_start == 7);
  h = hdr("P6\n2 3\n255\n");
  CHECK(h && h->tag == 6 && h->body_start == 11);
  h = hdr("# c\nP8 # c\n +2\t3\r");  // comments, any whitespace, a '+' sign (Int("+2") == 2)
  CHECK(h && h->w == 2 && h->h == 3 && h->body_start == 17);
  CHECK(!hdr("P8\n2 3"));            // last token has no delimiter yet: need more
  CHECK(!hdr("P8\n0 3\n"));          // w must be > 0
  CHECK(!hdr("P8\n-2 3\n"));
  CHECK(!hdr("P8\n20000 3\n"));      // and < 20000
  CHECK(!hdr("P8\n0x2 3\n"));
  CHECK(!hdr("P8\n99999999999999999999 3\n"));  // Int overflow -> nil
  CHECK(!hdr("P6\n2 3\n256\n"));     // maxval < 256
  CHECK(!hdr("P5\n2 3\n255\n"));
  std::vector<uint8_t> buf = bytes("P8\n1 1\n");
  buf.resize(buf.size() + 768, 0);  // palette, but the one index byte is missing
  CHECK(!take_frame(buf));
  CHECK_EQ(buf.size(), size_t(7 + 768));  // left untouched
  buf.push_back(42);
  buf.push_back('P');  // the start of the next frame stays buffered
  auto f = take_frame(buf);
  CHECK(f && f->body.size() == 769 && f->body.back() == 42);
  CHECK_EQ(buf.size(), size_t(1));
}

TEST(headless_deterministic_fbhash) {
  Child a, b;
  ChildOptions o;
  o.args = {"--test-pattern"};
  o.env = with(kSmall, {{"ADFRAMES", "48"}, {"ADFBHASH", "1"}});
  o.stdin_pipe = false;
  CHECK(a.start(o));
  CHECK_EQ(a.wait_exit(), 0);
  CHECK(b.start(o));
  CHECK_EQ(b.wait_exit(), 0);
  auto ha = fbhashes(a.err()), hb = fbhashes(b.err());
  CHECK_EQ(ha.size(), size_t(48));
  for (size_t i = 0; i < ha.size(); i++) CHECK_EQ(ha[i].first, uint64_t(i));
  CHECK(ha == hb);
  std::set<uint64_t> distinct;
  for (auto& p : ha) distinct.insert(p.second);
  CHECK_EQ(distinct.size(), size_t(48));
  CHECK_EQ(a.stdout_bytes_seen(), size_t(0));  // headless: stdout stays silent
}

TEST(headless_adout_ppm) {
  std::string dir = temp_dir() + "adw_e2e_out_" + std::to_string(GetCurrentProcessId());
  Child c;
  ChildOptions o;
  o.args = {"--test-pattern"};
  o.env = with(kSmall, {{"ADFRAMES", "3"}, {"ADOUT", dir}});
  o.stdin_pipe = false;
  CHECK(c.start(o));
  CHECK_EQ(c.wait_exit(), 0);
  for (int i = 0; i < 4; i++) {
    char name[32];
    snprintf(name, sizeof(name), "\\frame_%05d.ppm", i);
    std::wstring path = widen(dir + name);
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (i == 3) {
      CHECK(f == nullptr);
    } else {
      CHECK(f != nullptr);
      if (f) {
        std::vector<uint8_t> bytes(64 * 1024 + 64);
        size_t n = fread(bytes.data(), 1, bytes.size(), f);
        fclose(f);
        const char* hdr = "P6\n160 120\n255\n";
        CHECK_EQ(n, strlen(hdr) + 160 * 120 * 3);
        CHECK(memcmp(bytes.data(), hdr, strlen(hdr)) == 0);
      }
    }
    if (f || i < 3) DeleteFileW(path.c_str());
  }
  RemoveDirectoryW(widen(dir).c_str());
}

// ---- audio (AUDIO.md §10.1, §10.3) ------------------------------------------------

namespace {

std::vector<uint8_t> read_all(const std::string& path) {
  std::vector<uint8_t> out;
  FILE* f = _wfopen(widen(path).c_str(), L"rb");
  if (!f) return out;
  uint8_t buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
  fclose(f);
  return out;
}

bool exists(const std::string& path) { return GetFileAttributesW(widen(path).c_str()) != INVALID_FILE_ATTRIBUTES; }

// A capture: interleaved int16 stereo at `rate`.
struct Capture {
  uint32_t rate = 0;
  std::vector<int16_t> s;
  bool ok() const { return rate != 0; }
  size_t frames() const { return s.size() / 2; }
  // RMS of both channels over output frames [a, b), in dBFS (-999 = digital silence).
  double rms_db(size_t a, size_t b) const {
    b = std::min(b, frames());
    if (a >= b) return -999;
    double sum = 0;
    for (size_t i = a * 2; i < b * 2; i++) sum += double(s[i]) * double(s[i]);
    double v = std::sqrt(sum / double((b - a) * 2));
    return v > 0 ? 20 * std::log10(v / 32768.0) : -999;
  }
};
Capture read_capture(const std::string& path) {
  Capture c;
  std::vector<uint8_t> b = read_all(path);
  audio::Wave w;
  if (!audio::parse_wave(b, w) || w.format.channels != 2 || w.format.bits != 16) return c;
  c.rate = w.format.rate;
  c.s.resize(w.data.size() / 2);
  memcpy(c.s.data(), w.data.data(), c.s.size() * 2);
  return c;
}

// Note-on times (ms) in a .mid event log.
std::vector<uint64_t> note_on_ms(const std::string& path) {
  std::vector<uint64_t> out;
  audio::SmfSong song;
  if (!audio::parse_smf(read_all(path), song)) return out;
  for (const audio::SmfEvent& e : song.events)
    if (e.is_channel() && e.type() == 0x90 && e.d2 > 0) out.push_back(e.us / 1000);
  return out;
}

// "[audio] voices=… chunks=… songs=… midi_events=… underruns=… drops=… frames=…"
std::map<std::string, uint64_t> audio_summary(const std::string& err) {
  std::map<std::string, uint64_t> kv;
  size_t pos = err.find("[audio] voices=");
  if (pos == std::string::npos) return kv;
  size_t end = err.find('\n', pos);
  std::string line = err.substr(pos + 8, end == std::string::npos ? std::string::npos : end - pos - 8);
  size_t p = 0;
  while (p < line.size()) {
    size_t sp = line.find(' ', p);
    std::string tok = line.substr(p, sp == std::string::npos ? std::string::npos : sp - p);
    size_t eq = tok.find('=');
    if (eq != std::string::npos) kv[tok.substr(0, eq)] = strtoull(tok.c_str() + eq + 1, nullptr, 10);
    p = sp == std::string::npos ? line.size() : sp + 1;
  }
  return kv;
}

// Virtual µs of each "[audio] @<t> <what> <id> <verb>" line with that what/verb.
std::vector<uint64_t> trace_times(const std::string& err, const char* what, const char* verb) {
  std::vector<uint64_t> out;
  size_t pos = 0;
  while ((pos = err.find("[audio] @", pos)) != std::string::npos) {
    unsigned long long t = 0;
    char w[32] = {}, v[32] = {};
    unsigned id = 0;
    if (sscanf(err.c_str() + pos, "[audio] @%llu %31s %u %31s", &t, w, &id, v) == 4 && !strcmp(w, what) &&
        !strcmp(v, verb))
      out.push_back(t);
    pos += 9;
  }
  return out;
}

std::string e2e_temp(const std::string& name) {
  return temp_dir() + "adw_e2e_" + std::to_string(GetCurrentProcessId()) + "_" + name;
}

void remove_capture(const std::string& wav) {
  DeleteFileW(widen(wav).c_str());
  DeleteFileW(widen(wav.substr(0, wav.size() - 4) + ".mid").c_str());
}

}  // namespace

TEST(test_pattern_audio_capture) {
  // AUDIO.md §10.1: 6 s at the pattern's 30 fps; a blip at every whole second,
  // a MIDI note every second second.
  std::string wav = e2e_temp("tp.wav"), mid = e2e_temp("tp.mid");
  Child c;
  ChildOptions o;
  o.args = {"--test-pattern"};
  o.env = with(kSmall, {{"ADFRAMES", "180"}, {"ADTESTAUDIO", "1"}, {"ADAUDIOOUT", wav}, {"ADFBHASH", "1"}});
  o.stdin_pipe = false;
  CHECK(c.start(o));
  CHECK_EQ(c.wait_exit(), 0);
  std::string err = c.err();
  Capture cap = read_capture(wav);
  CHECK(cap.ok());
  CHECK_EQ(cap.rate, 44100u);
  // Ends at the last frame's time: floor(179 * 33333 µs * 44100 / 10^6).
  CHECK_EQ(cap.frames(), size_t(uint64_t(179) * 33333 * 44100 / 1000000));
  for (int n = 0; n < 6; n++) {
    size_t a = size_t(n) * 44100, b = a + 4410;
    double on = cap.rms_db(a, b), off = cap.rms_db(b, a + 44100);
    if (!(on > -30 && off < -80)) fprintf(stderr, "  second %d: %.1f dBFS in the blip, %.1f after\n", n, on, off);
    CHECK(on > -30);
    CHECK(off < -80);
  }
  std::vector<uint64_t> notes = note_on_ms(mid);
  CHECK(notes == std::vector<uint64_t>({0, 2000, 4000}));
  auto sum = audio_summary(err);
  CHECK_EQ(sum["voices"], uint64_t(6));
  CHECK_EQ(sum["songs"], uint64_t(3));
  CHECK_EQ(sum["underruns"], uint64_t(0));
  CHECK_EQ(sum["frames"], uint64_t(cap.frames()));
  // The pixels do not change with sound on.
  CHECK(hash_values(fbhashes(err)) == headless_reference(180));
  remove_capture(wav);
}

TEST(sound_off_is_unchanged_and_headless_sound_on_matches_capture) {
  const int kFrames = 48;
  std::vector<uint64_t> ref = headless_reference(kFrames);
  CHECK_EQ(ref.size(), size_t(kFrames));
  auto run = [&](const std::map<std::string, std::string>& extra, std::string* err_out) {
    Child c;
    ChildOptions o;
    o.args = {"--test-pattern"};
    o.env = with(with(kSmall, {{"ADFRAMES", std::to_string(kFrames)}, {"ADFBHASH", "1"}, {"ADTESTAUDIO", "1"}}), extra);
    o.stdin_pipe = false;
    if (!c.start(o)) return std::vector<uint64_t>{};
    CHECK_EQ(c.wait_exit(), 0);
    *err_out = c.err();
    return hash_values(fbhashes(*err_out));
  };
  // Sound off (ADTESTAUDIO alone does nothing): today's stream, no audio line.
  std::string err;
  CHECK(run({}, &err) == ref);
  CHECK(err.find("[audio]") == std::string::npos);
  CHECK(err.find("sound on") == std::string::npos);
  // ADSOUND=1 headless: sound on, no device, no files; the same frames as a capture run.
  std::string err_on, err_cap;
  std::vector<uint64_t> on = run({{"ADSOUND", "1"}}, &err_on);
  CHECK(on == ref);
  CHECK(err_on.find("[audio] voices=") != std::string::npos);
  CHECK(err_on.find("no output") != std::string::npos);
  std::string wav = e2e_temp("same.wav");
  std::vector<uint64_t> cap = run({{"ADAUDIOOUT", wav}}, &err_cap);
  CHECK(cap == on);
  CHECK(exists(wav));
  auto a = audio_summary(err_on), b = audio_summary(err_cap);
  CHECK(!a.empty() && a == b);
  remove_capture(wav);
  // A streamed run with sound on but ADAUDIOLIVE=0 opens no device either.
  Session s = run_session({"GO\nGO\nGO\n"}, with(kSmall, {{"ADSOUND", "1"}, {"ADAUDIOLIVE", "0"}, {"ADTESTAUDIO", "1"}}));
  CHECK_EQ(s.exit_code, 0);
  CHECK_EQ(s.frames.size(), size_t(3));
  CHECK(s.err.find("live PCM") == std::string::npos);
  CHECK(s.err.find(", no output") != std::string::npos);
}

TEST(command_line_overrides) {
  Child c;
  ChildOptions o;
  o.args = {"--test-pattern", "ADFRAMES=2", "ADFBHASH=1", "ADSCREENW=64", "ADSCREENH=48"};
  o.stdin_pipe = false;
  CHECK(c.start(o));
  CHECK_EQ(c.wait_exit(), 0);
  CHECK_EQ(fbhashes(c.err()).size(), size_t(2));
}

TEST(stream_lockstep_matches_headless_and_inputs_visible) {
  const int kPlain = 40;
  std::vector<uint64_t> ref = headless_reference(kPlain + 3);
  CHECK_EQ(ref.size(), size_t(kPlain + 3));
  std::vector<std::string> script;
  for (int i = 0; i < kPlain; i++) script.push_back("GO\n");
  script.push_back("SET 0 60\nSET 5 100\nGO\n");       // frame 40
  script.push_back("KEY 65 1\nCAPS 1\nGO\n");          // frame 41
  script.push_back("MOUSE 20 30 1\nGO\n");             // frame 42
  std::vector<std::string> script_no_input = script;  // same, minus KEY/CAPS/MOUSE
  script_no_input[size_t(kPlain) + 1] = "GO\n";
  script_no_input[size_t(kPlain) + 2] = "GO\n";

  Session a = run_session(script, kSmall);
  Session b = run_session(script, kSmall);
  Session n = run_session(script_no_input, kSmall);
  CHECK_EQ(a.frames.size(), size_t(kPlain + 3));
  CHECK_EQ(a.exit_code, 0);
  CHECK_EQ(a.stray_bytes, size_t(0));
  CHECK(a.err.find("QUIT") != std::string::npos);
  if (a.frames.size() != size_t(kPlain + 3) || b.frames.size() != a.frames.size() ||
      n.frames.size() != a.frames.size() || ref.size() != a.frames.size())
    return;
  std::vector<uint64_t> ha, hb, hn;
  for (auto& f : a.frames) {
    CHECK_EQ(f.tag, 8);
    CHECK_EQ(f.w, 160);
    CHECK_EQ(f.h, 120);
    CHECK_EQ(f.body.size(), size_t(768 + 160 * 120));
    ha.push_back(frame_hash(f));
  }
  for (auto& f : b.frames) hb.push_back(frame_hash(f));
  for (auto& f : n.frames) hn.push_back(frame_hash(f));
  CHECK(ha == hb);                     // deterministic across runs
  CHECK(ha == a.stderr_hashes);        // FBHASH == FNV-1a of the P8 body on the wire
  for (int i = 0; i < kPlain; i++) CHECK_EQ(ha[size_t(i)], ref[size_t(i)]);  // == headless
  CHECK(ha[kPlain] != ref[kPlain]);    // SET is visible on the frame after it
  CHECK_EQ(ha[kPlain], hn[kPlain]);
  CHECK(ha[kPlain + 1] != hn[kPlain + 1]);  // KEY/CAPS visible
  CHECK(ha[kPlain + 2] != hn[kPlain + 2]);  // MOUSE visible
}

TEST(stream_opening_set_then_go_in_separate_writes) {
  // The SET and the first GO arrive 50 ms apart; frame 0 must still be the
  // reply to that GO (no unrequested frame first) and must show the SET.
  std::vector<uint64_t> ref = headless_reference(1);
  Child c;
  ChildOptions o;
  o.args = {"--test-pattern"};
  // A wide window so a loaded machine stretching the 50 ms gap cannot turn
  // this into a timing test; what is checked is that the wait keeps going
  // past a non-GO opening line.
  o.env = with(kSmall, {{"ADSTREAM", "1"}, {"ADGOWAITMS", "3000"}});
  CHECK(c.start(o));
  c.send("SET 5 100\n");
  Sleep(50);
  c.send("GO\n");
  auto f = c.read_frame();
  CHECK(f.has_value());
  CHECK_EQ(c.pending_bytes(150), size_t(0));
  if (f && !ref.empty()) CHECK(frame_hash(*f) != ref[0]);
  c.send("GO\n");
  CHECK(c.read_frame().has_value());
  CHECK_EQ(c.pending_bytes(100), size_t(0));
  c.close_stdin();
  CHECK_EQ(c.wait_exit(5000), 0);
}

TEST(stream_default_size) {
  Session s = run_session({"GO\n"}, {});
  CHECK_EQ(s.frames.size(), size_t(1));
  if (!s.frames.empty()) {
    CHECK_EQ(s.frames[0].w, 640);
    CHECK_EQ(s.frames[0].h, 480);
    CHECK_EQ(s.frames[0].body.size(), size_t(768 + 640 * 480));
  }
  CHECK_EQ(s.exit_code, 0);
}

TEST(stream_p6_fallback) {
  Session s = run_session({"GO\nGO\n"}, with(kSmall, {{"ADSTREAMP6", "1"}}));
  CHECK_EQ(s.frames.size(), size_t(2));
  for (auto& f : s.frames) {
    CHECK_EQ(f.tag, 6);
    CHECK_EQ(f.body.size(), size_t(160 * 120 * 3));
  }
}

TEST(stream_exits_on_stdin_eof_after_go) {
  Child c;
  ChildOptions o;
  o.args = {"--test-pattern"};
  o.env = with(kSmall, {{"ADSTREAM", "1"}});
  CHECK(c.start(o));
  c.send("GO\nGO\nGO\n");
  for (int i = 0; i < 3; i++) CHECK(c.read_frame().has_value());
  c.close_stdin();
  CHECK_EQ(c.wait_exit(5000), 0);
  CHECK(c.stdout_eof());
  CHECK(c.err().find("stdin closed after GO") != std::string::npos);
}

TEST(stream_free_running_then_reader_closes) {
  // No GO: the host free-runs at its own pace. Closing our end of its stdout
  // must end it promptly and cleanly (exit 0), whether it was mid-write or not.
  Child c;
  ChildOptions o;
  o.args = {"--test-pattern"};
  o.env = with(kSmall, {{"ADSTREAM", "1"}});
  CHECK(c.start(o));
  auto f0 = c.read_frame();
  // Timed from frame 0, not from the spawn: the pre-frame-0 GO wait (250 ms on
  // a silent pipe) would satisfy any bound measured across it on its own.
  auto t0 = std::chrono::steady_clock::now();
  auto f1 = c.read_frame(), f2 = c.read_frame();
  double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  CHECK(f0 && f1 && f2);
  // Paced at 30 fps: frame 2 comes two periods (66.7 ms) after frame 0; the
  // margin covers the poll loop's Sleep(1) granularity. A flood takes ~1 ms.
  if (ms < 45) fprintf(stderr, "  frames 0..2 took %.1f ms\n", ms);
  CHECK(ms >= 45);
  if (f0 && f1) CHECK(frame_hash(*f0) != frame_hash(*f1));
  c.close_stdout();
  CHECK_EQ(c.wait_exit(5000), 0);
  CHECK(c.err().find("stdout closed") != std::string::npos);
}

TEST(stream_stdin_eof_before_go_keeps_running) {
  Child c;
  ChildOptions o;
  o.args = {"--test-pattern"};
  o.env = with(kSmall, {{"ADSTREAM", "1"}, {"ADFRAMES", "5"}, {"ADNOPACE", "1"}});
  CHECK(c.start(o));
  c.close_stdin();
  int frames = 0;
  while (c.read_frame(3000)) frames++;
  CHECK_EQ(frames, 5);
  CHECK_EQ(c.wait_exit(5000), 0);
}

TEST(stream_quit_while_waiting_for_go) {
  Child c;
  ChildOptions o;
  o.args = {"--test-pattern"};
  o.env = with(kSmall, {{"ADSTREAM", "1"}});
  CHECK(c.start(o));
  c.send("GO\n");
  CHECK(c.read_frame().has_value());
  Sleep(100);  // host is now blocked waiting for the next GO
  c.send("QUIT\n");
  CHECK_EQ(c.wait_exit(5000), 0);
}

TEST(stream_refuses_disk_file_stdout) {
  std::string path = temp_dir() + "adw_e2e_" + std::to_string(GetCurrentProcessId()) + "_stdout.bin";
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE f = CreateFileW(widen(path).c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, 0, nullptr);
  CHECK(f != INVALID_HANDLE_VALUE);
  {
    Child c;
    ChildOptions o;
    o.args = {"--test-pattern"};
    o.env = with(kSmall, {{"ADSTREAM", "1"}, {"ADFRAMES", "1"}});
    o.stdout_override = f;
    CHECK(c.start(o));
    CHECK_EQ(c.wait_exit(), 2);
    CHECK(c.err().find("disk file") != std::string::npos);
  }
  {
    Child c;
    ChildOptions o;
    o.args = {"--test-pattern"};
    o.env = with(kSmall, {{"ADSTREAM", "1"}, {"ADFRAMES", "2"}, {"ADSTREAMFORCE", "1"}, {"ADNOPACE", "1"}});
    o.stdout_override = f;
    CHECK(c.start(o));
    CHECK_EQ(c.wait_exit(), 0);
  }
  CloseHandle(f);
  WIN32_FILE_ATTRIBUTE_DATA fa{};
  GetFileAttributesExW(widen(path).c_str(), GetFileExInfoStandard, &fa);
  CHECK_EQ(uint64_t(fa.nFileSizeLow), uint64_t(2 * (std::string("P8\n160 120\n").size() + 768 + 160 * 120)));
  DeleteFileW(widen(path).c_str());
}

TEST(stream_refuses_shared_stdout_stderr) {
  // One pipe as both stdout and stderr: log lines would be spliced between
  // frames, so the host refuses (and says why on that same pipe).
  Child c;
  ChildOptions o;
  o.args = {"--test-pattern"};
  o.env = with(kSmall, {{"ADSTREAM", "1"}, {"ADFRAMES", "1"}});
  o.stdin_pipe = false;
  o.stderr_is_stdout = true;
  CHECK(c.start(o));
  CHECK_EQ(c.wait_exit(), 2);
  std::string out = c.stdout_text();
  CHECK(out.find("same handle") != std::string::npos);
  CHECK(out.find("P8\n") == std::string::npos);  // no frame went out
}

TEST(stream_refuses_duplicated_stdout_stderr) {
  // A shell's 2>&1 gives the child two different handle VALUES for one pipe;
  // log lines would still be spliced between frames, so this is refused too.
  Child c;
  ChildOptions o;
  o.args = {"--test-pattern"};
  o.env = with(kSmall, {{"ADSTREAM", "1"}, {"ADFRAMES", "1"}});
  o.stdin_pipe = false;
  o.stderr_dup_of_stdout = true;
  CHECK(c.start(o));
  CHECK_EQ(c.wait_exit(), 2);
  std::string out = c.stdout_text();
  CHECK(out.find("same handle") != std::string::npos);
  CHECK(out.find("P8\n") == std::string::npos);
}

TEST(console_stdout_refused) {
  // ADSTREAM into a console (the hidden one CREATE_NO_WINDOW gives the host;
  // a NULL stdout becomes its handle): binary frames would scribble on a
  // terminal, so it is refused with exit 2.
  Child c;
  ChildOptions o;
  o.args = {"--test-pattern"};
  o.env = with(kSmall, {{"ADSTREAM", "1"}, {"ADFRAMES", "1"}});
  o.stdin_pipe = false;
  o.null_stdout = true;
  CHECK(c.start(o));
  CHECK_EQ(c.wait_exit(), 2);
  std::string err = c.err();
  if (err.find("stdout is a console") == std::string::npos) fprintf(stderr, "  stderr: %s\n", err.c_str());
  CHECK(err.find("stdout is a console") != std::string::npos);
}

TEST(console_stdin_free_runs_and_exits_promptly) {
  // stdin is a (hidden, silent) console: no pre-frame-0 wait, frames free-run,
  // and the reader thread parked in a console ReadFile does not hold up the
  // exit when the frame reader goes away.
  Child c;
  ChildOptions o;
  o.args = {"--test-pattern"};
  o.env = with(kSmall, {{"ADSTREAM", "1"}, {"ADPACEMS", "5"}});
  o.null_stdin = true;
  CHECK(c.start(o));
  CHECK(c.read_frame().has_value());
  CHECK(c.read_frame().has_value());
  auto t0 = std::chrono::steady_clock::now();
  c.close_stdout();
  CHECK_EQ(c.wait_exit(5000), 0);
  double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  if (ms >= 2000) fprintf(stderr, "  exit took %.0f ms after stdout closed\n", ms);
  CHECK(ms < 2000);
  std::string err = c.err();
  CHECK(err.find("stdin console") != std::string::npos);
  CHECK(err.find("stdout closed") != std::string::npos);
}

TEST(no_console_no_stdin_no_stderr) {
  // A GUI parent may start the host with no console and only a stdout pipe:
  // stdin absent (never a command, never EOF), stderr absent (logging must
  // fail quietly, not fault in the CRT). It must still stream.
  Child c;
  ChildOptions o;
  o.args = {"--test-pattern"};
  o.env = with(kSmall, {{"ADSTREAM", "1"}, {"ADFRAMES", "3"}, {"ADNOPACE", "1"}});
  o.null_stdin = true;
  o.null_stderr = true;
  o.detached = true;
  CHECK(c.start(o));
  int frames = 0;
  while (c.read_frame(3000)) frames++;
  CHECK_EQ(frames, 3);
  CHECK_EQ(c.wait_exit(5000), 0);
}

TEST(lane_detection_exit_codes) {
  auto image = [](const char* sig, size_t sig_len) {
    std::vector<uint8_t> b(0x80, 0);
    b[0] = 'M';
    b[1] = 'Z';
    b[0x3C] = 0x40;
    memcpy(&b[0x40], sig, sig_len);
    return b;
  };
  std::string pe = write_temp("fake_pe.ad", image("PE\0\0\x4c\x01", 6));
  std::string ne = write_temp("fake_ne.ad", image("NE", 2));
  std::string junk = write_temp("junk.ad", {'n', 'o', 'p', 'e'});
  // An Intermission ASA animation is data: routed to the ne16 lane by its
  // header, where no ASA reader (IMASAPLY.IMQ) is installed for it.
  std::string asa = write_temp("fake.asa", {'A', 'n', 'i', 'N', 0x20, 0, 0x20, 3});
  // So is an .FLI animation, by its extension: no IMFLIPLY.IMQ is installed for it.
  std::string fli = write_temp("fake.fli", {0x10, 0, 0, 0, 0x12, 0xAF});
  // Without its lane a header-only stub is refused as "not built in" (3); with
  // the lane linked it is routed there, and the lane's init must reject a
  // 128-byte image (1) rather than crash on it.
  struct Case {
    std::vector<std::string> args;
    int code;
    const char* needle;
  } cases[] = {
      {{pe}, ADW_HAVE_LANE_PE32 ? 1 : 3, ADW_HAVE_LANE_PE32 ? "lane init failed" : "was built without the pe32 lane"},
      {{ne}, ADW_HAVE_LANE_NE16 ? 1 : 3, ADW_HAVE_LANE_NE16 ? "lane init failed" : "was built without the ne16 lane"},
      {{junk}, 2, "not a module this host can run"},
      {{asa}, ADW_HAVE_LANE_NE16 ? 1 : 3, ADW_HAVE_LANE_NE16 ? "IMASAPLY.IMQ" : "was built without the ne16 lane"},
      {{fli}, ADW_HAVE_LANE_NE16 ? 1 : 3, ADW_HAVE_LANE_NE16 ? "IMFLIPLY.IMQ" : "was built without the ne16 lane"},
      {{temp_dir() + "adw_e2e_missing.ad"}, 2, "cannot open"},
      {{}, 2, "usage"},
      {{pe, "--test-pattern"}, 2, "--test-pattern takes no module"},
      {{"--test-pattern", pe}, 2, "--test-pattern takes no module"},
  };
  for (auto& k : cases) {
    Child c;
    ChildOptions o;
    o.args = k.args;
    o.stdin_pipe = false;
    CHECK(c.start(o));
    int code = c.wait_exit();
    std::string err = c.err();
    if (code != k.code || err.find(k.needle) == std::string::npos)
      fprintf(stderr, "  args[0]=%s exit %d stderr: %s\n", k.args.empty() ? "" : k.args[0].c_str(), code,
              err.c_str());
    CHECK_EQ(code, k.code);
    CHECK(err.find(k.needle) != std::string::npos);
  }
  for (auto& p : {pe, ne, junk, asa, fli}) DeleteFileW(widen(p).c_str());
}

// ---- interaction (INTERACTION.md §3) ------------------------------------------

namespace {

// The script every status test runs on the test pattern with
// ADTESTINTERACTIVE=1 (CAPS 1 = interactive, keys eaten while it is).
const char* kStatusScript = "GO\nCAPS 1\nGO\nKEY 65 1\nGO\nCAPS 0\nGO\nQUIT\n";

std::vector<std::string> status_lines(const std::string& err) {
  std::vector<std::string> out;
  size_t pos = 0;
  while ((pos = err.find("STATUS ", pos)) != std::string::npos) {
    size_t end = err.find('\n', pos);
    std::string line = err.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    while (!line.empty() && line.back() == '\r') line.pop_back();
    out.push_back(line);
    pos = end == std::string::npos ? err.size() : end;
  }
  return out;
}

}  // namespace

TEST(status_log_lines) {
  Child c;
  ChildOptions o;
  o.args = {"--test-pattern"};
  o.env = {{"ADSTREAM", "1"}, {"ADSCREENW", "64"}, {"ADSCREENH", "48"}, {"ADTESTINTERACTIVE", "1"},
           {"ADSTATUSLOG", "1"}};
  CHECK(c.start(o));
  CHECK(c.send(kStatusScript));
  for (int i = 0; i < 4; i++) CHECK(c.read_frame().has_value());
  CHECK_EQ(c.wait_exit(), 0);
  std::vector<std::string> lines = status_lines(c.err());
  std::vector<std::string> want = {
      "STATUS 0 flags=0x20 applied=0 eaten=0 src=0",
      "STATUS 2 flags=0x21 applied=1 eaten=0 src=1",
      "STATUS 3 flags=0x21 applied=2 eaten=2 src=1",
      "STATUS 4 flags=0x20 applied=3 eaten=2 src=0",
  };
  if (lines != want) fprintf(stderr, "  stderr: %s\n", c.err().c_str());
  CHECK(lines == want);
}

TEST(numlock_line_and_env) {
  // NUMLOCK is an input line of its own (numbered, applied; not interactive
  // for the test pattern, which knows nothing of it); ADNUMLOCK is logged at
  // start, and nothing is refused.
  Child c;
  ChildOptions o;
  o.args = {"--test-pattern"};
  o.env = {{"ADSTREAM", "1"}, {"ADSCREENW", "64"}, {"ADSCREENH", "48"}, {"ADTESTINTERACTIVE", "1"},
           {"ADSTATUSLOG", "1"}, {"ADNUMLOCK", "1"}};
  CHECK(c.start(o));
  CHECK(c.send("GO\nKEY 144 1\nNUMLOCK 0\nGO\nKEY 144 0\nGO\nQUIT\n"));
  for (int i = 0; i < 3; i++) CHECK(c.read_frame().has_value());
  CHECK_EQ(c.wait_exit(), 0);
  std::vector<std::string> lines = status_lines(c.err());
  std::vector<std::string> want = {
      "STATUS 0 flags=0x20 applied=0 eaten=0 src=0",
      "STATUS 2 flags=0x20 applied=2 eaten=0 src=0",
      "STATUS 3 flags=0x20 applied=3 eaten=0 src=0",
  };
  if (lines != want) fprintf(stderr, "  stderr: %s\n", c.err().c_str());
  CHECK(lines == want);
  CHECK(c.err().find(", num lock on") != std::string::npos);
  CHECK(c.err().find("unrecognized") == std::string::npos);
}

TEST(status_record_through_inherited_handle) {
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE sec = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, 4096, nullptr);
  CHECK(sec != nullptr);
  if (!sec) return;
  const void* view = MapViewOfFile(sec, FILE_MAP_READ, 0, 0, 64);
  Child c;
  ChildOptions o;
  o.args = {"--test-pattern"};
  o.env = {{"ADSTREAM", "1"}, {"ADSCREENW", "64"}, {"ADSCREENH", "48"}, {"ADTESTINTERACTIVE", "1"},
           {"ADSTATUSHANDLE", std::to_string(uintptr_t(sec))}};
  CHECK(c.start(o));
  CHECK(c.send("GO\nCAPS 1\nGO\n"));
  CHECK(c.read_frame().has_value());
  CHECK(c.read_frame().has_value());
  // The record for a step is published before its frame is written.
  AdwHostStatusV1 s{};
  CHECK(read_status(view, &s));
  CHECK_EQ(s.frames, uint64_t(2));
  CHECK_EQ(s.flags, ADWS_READY | ADWS_INTERACTIVE);
  CHECK_EQ(s.input_applied, uint64_t(1));
  CHECK_EQ(s.lane, kStatusLaneTest);
  CHECK(c.send("KEY 65 1\nGO\nCAPS 0\nGO\nQUIT\n"));
  CHECK(c.read_frame().has_value());
  CHECK(c.read_frame().has_value());
  CHECK_EQ(c.wait_exit(), 0);
  CHECK(read_status(view, &s));
  CHECK_EQ(s.frames, uint64_t(4));
  CHECK_EQ(s.flags, ADWS_READY);
  CHECK_EQ(s.input_applied, uint64_t(3));
  CHECK_EQ(s.input_eaten, uint64_t(2));
  CHECK(c.err().find("status record") != std::string::npos);
  UnmapViewOfFile(view);
  CloseHandle(sec);

  // A value that is not a handle is reported and ignored: the run goes on.
  Child bad;
  ChildOptions ob;
  ob.args = {"--test-pattern"};
  ob.env = {{"ADFRAMES", "2"}, {"ADSCREENW", "32"}, {"ADSCREENH", "32"}, {"ADSTATUSHANDLE", "0x7ffffff0"}};
  ob.stdin_pipe = false;
  CHECK(bad.start(ob));
  CHECK_EQ(bad.wait_exit(), 0);
  CHECK(bad.err().find("does not map") != std::string::npos);
}

TEST(capabilities_line) {
  Child c;
  ChildOptions o;
  o.args = {"--capabilities"};
  o.stdin_pipe = false;
  CHECK(c.start(o));
  CHECK_EQ(c.wait_exit(), 0);
  std::string out = c.stdout_text();
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
  std::map<std::string, std::string> kv;
  size_t pos = 0;
  while (pos < out.size()) {
    size_t sp = out.find(' ', pos);
    std::string tok = out.substr(pos, sp == std::string::npos ? std::string::npos : sp - pos);
    size_t eq = tok.find('=');
    if (eq != std::string::npos) kv[tok.substr(0, eq)] = tok.substr(eq + 1);
    pos = sp == std::string::npos ? out.size() : sp + 1;
  }
  std::string lanes;
  if (ADW_HAVE_LANE_PE32) lanes += "pe32";
  if (ADW_HAVE_LANE_NE16) lanes += std::string(lanes.empty() ? "" : ",") + "ne16";
  CHECK_EQ(kv["lanes"], lanes);
  CHECK(kv.count("configure"));
  // configure lists only linked lanes.
  for (const char* l : {"pe32", "ne16"})
    if (kv["configure"].find(l) != std::string::npos) CHECK(lanes.find(l) != std::string::npos);
  // abis: the union of the linked lanes' module ABIs, in lane order (pe32
  // runs After Dark's; ne16 After Dark's, Intermission's and Windows 3.1's
  // screen-saver programs').
  CHECK(kv.count("abis"));
  std::string abis = (ADW_HAVE_LANE_PE32 || ADW_HAVE_LANE_NE16) ? "afterdark" : "";
  if (ADW_HAVE_LANE_NE16) abis += ",intermission,scrnsave";
  CHECK_EQ(kv["abis"], abis);
  CHECK(out.find(" abis=") > out.find("configure="));  // after configure=, before the core features
  CHECK_EQ(kv["status"], std::string("1"));
  CHECK_EQ(kv["state"], std::string("1"));
  CHECK_EQ(kv["seed"], std::string("1"));
  CHECK_EQ(kv["audio"], std::string("1"));
  CHECK_EQ(kv["numlock"], std::string("1"));  // the NUMLOCK line and ADNUMLOCK are understood
  CHECK(out.find('\n') == std::string::npos);  // one line

  Child extra;
  ChildOptions oe;
  oe.args = {"--capabilities", "--test-pattern"};
  oe.stdin_pipe = false;
  CHECK(extra.start(oe));
  CHECK_EQ(extra.wait_exit(), 2);
}

TEST(configure_exit_codes) {
  // What --capabilities says decides what a real lane answers.
  Child cap;
  ChildOptions oc;
  oc.args = {"--capabilities"};
  oc.stdin_pipe = false;
  CHECK(cap.start(oc));
  cap.wait_exit();
  const std::string caps = cap.stdout_text();
  const size_t cfg_at = caps.find("configure=");
  const std::string configure = cfg_at == std::string::npos ? "" : caps.substr(cfg_at + 10, caps.find(' ', cfg_at) - cfg_at - 10);

  auto image = [](const char* sig, size_t sig_len) {
    std::vector<uint8_t> b(0x80, 0);
    b[0] = 'M';
    b[1] = 'Z';
    b[0x3C] = 0x40;
    memcpy(&b[0x40], sig, sig_len);
    return b;
  };
  std::string pe = write_temp("cfg_pe.ad", image("PE\0\0\x4c\x01", 6));
  std::string ne = write_temp("cfg_ne.ad", image("NE", 2));
  std::string junk = write_temp("cfg_junk.ad", {'n', 'o', 'p', 'e'});
  auto lane_code = [&](bool linked, const char* lane) {
    if (!linked) return 3;                                          // lane missing
    if (configure.find(lane) == std::string::npos) return 5;        // no configure support
    return 1;                                                       // a 128-byte stub cannot load
  };
  struct Case {
    std::vector<std::string> args;
    int code;
  } cases[] = {
      {{"--configure"}, 2},
      {{"--configure", pe}, 2},                                 // no --button
      {{"--configure", pe, "--button", "x"}, 2},
      {{"--configure", pe, "--button", "-1"}, 2},
      {{"--configure", pe, "--button", "1", "--owner", "zz"}, 2},
      {{"--configure", pe, "--button"}, 2},
      {{"--configure", "--test-pattern", "--button", "1"}, 2},
      {{pe, "--button", "1"}, 2},                               // --button without --configure
      {{"--configure", junk, "--button", "0"}, 2},
      {{"--configure", pe, "--button", "0", "--owner", "0x10"}, lane_code(ADW_HAVE_LANE_PE32, "pe32")},
      {{"--configure", ne, "--button", "3"}, lane_code(ADW_HAVE_LANE_NE16, "ne16")},
  };
  for (auto& k : cases) {
    Child c;
    ChildOptions o;
    o.args = k.args;
    o.stdin_pipe = false;
    // Never the user's state folder (the --configure default).
    o.env = {{"ADSTATE", ":memory:"}, {"ADCONFIGHIDDEN", "1"}};
    CHECK(c.start(o));
    int code = c.wait_exit(30000);
    std::string out = c.stdout_text();
    if (code != k.code) {
      std::string all;
      for (auto& a : k.args) all += a + " ";
      fprintf(stderr, "  %s-> exit %d (want %d) stdout: %s stderr: %s\n", all.c_str(), code, k.code, out.c_str(),
              c.err().c_str());
    }
    CHECK_EQ(code, k.code);
    // --configure always answers with one JSON line (usage errors included).
    if (!k.args.empty() && k.args[0] == "--configure")
      CHECK(out.rfind("{\"result\":", 0) == 0 && out.find('\n') == out.size() - 1);
  }
  for (auto& p : {pe, ne, junk}) DeleteFileW(widen(p).c_str());
}

// ---- the data folder (data_root.h) -------------------------------------------

namespace {

bool path_exists(const std::string& p) { return GetFileAttributesW(widen(p).c_str()) != INVALID_FILE_ATTRIBUTES; }

void remove_tree(const std::string& p) {
  std::wstring w = widen(p);
  DWORD a = GetFileAttributesW(w.c_str());
  if (a == INVALID_FILE_ATTRIBUTES) return;
  if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
    DeleteFileW(w.c_str());
    return;
  }
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((w + L"\\*").c_str(), &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      std::wstring n = fd.cFileName;
      if (n != L"." && n != L"..") remove_tree(p + "\\" + narrow(n));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  RemoveDirectoryW(w.c_str());
}

struct HostRun {
  int code;
  std::string err;
};
HostRun run_host(const std::vector<std::string>& args, const std::map<std::string, std::string>& env) {
  Child c;
  ChildOptions o;
  o.args = args;
  o.env = env;
  o.stdin_pipe = false;
  if (!c.start(o)) return {-1, ""};
  int code = c.wait_exit(30000);
  return {code, c.err()};
}

}  // namespace

TEST(data_folder_default_assets_root) {
  // With AD_ASSETS_DIR unset, a catalog path resolves under
  // <base>\LongAfterDark\assets\win. A junk module put there is found and
  // refused (exit 2) by its full path, which shows where the host looked.
  const std::string base = temp_dir() + "adw_e2e_dataroot_" + std::to_string(GetCurrentProcessId());
  const std::string win = base + "\\LongAfterDark\\assets\\win";
  remove_tree(base);
  for (const std::string& d : {base, base + "\\LongAfterDark", base + "\\LongAfterDark\\assets", win,
                               win + "\\FILES", win + "\\FILES\\AD40"})
    CreateDirectoryW(widen(d).c_str(), nullptr);
  if (FILE* f = _wfopen(widen(win + "\\FILES\\AD40\\JUNK.AD").c_str(), L"wb")) {
    fputs("nope", f);
    fclose(f);
  }
  const std::vector<std::string> module = {"FILES/AD40/JUNK.AD"};
  const std::string found = win + "\\FILES/AD40/JUNK.AD: ";
  HostRun a = run_host(module, {{"AD_LOCALAPPDATA", base}});
  CHECK_EQ(a.code, 2);
  CHECK(a.err.find(found) != std::string::npos);
  if (a.err.find(found) == std::string::npos) fprintf(stderr, "  stderr: %s\n", a.err.c_str());
  // A blank AD_LOCALAPPDATA gives way to LOCALAPPDATA (the child's, not ours).
  HostRun l = run_host(module, {{"AD_LOCALAPPDATA", " "}, {"LOCALAPPDATA", base}});
  CHECK_EQ(l.code, 2);
  CHECK(l.err.find(found) != std::string::npos);
  remove_tree(base);

  // Resolving the data folder creates nothing: neither a module run on the
  // default assets root nor the test pattern makes <base>\LongAfterDark.
  CreateDirectoryW(widen(base).c_str(), nullptr);
  HostRun m = run_host(module, {{"AD_LOCALAPPDATA", base}});
  CHECK_EQ(m.code, 2);
  HostRun t = run_host({"--test-pattern"}, {{"ADFRAMES", "1"}, {"AD_LOCALAPPDATA", base}});
  CHECK_EQ(t.code, 0);
  CHECK(path_exists(base) && !path_exists(base + "\\LongAfterDark"));
  remove_tree(base);
}

// ---- --assets ----------------------------------------------------------------

static int run_assets_mode() {
  Env env = Env::from_process();
  // Without AD_ASSETS_DIR: the installed assets in the data folder
  // (read-only).
  if (!env.get("AD_ASSETS_DIR") || env.get("AD_ASSETS_DIR")->find_first_not_of(" \t") == std::string::npos)
    env.assets_root = adw_test::installed_assets_root();
  std::string win = env.win_assets_dir();
  std::string ad40 = win + "\\FILES\\AD40", classic = win + "\\FILES\\CLASSIC";
  DWORD a = GetFileAttributesW(widen(ad40).c_str());
  if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) {
    fprintf(stderr, "SKIP: no Windows After Dark assets at %s (set AD_ASSETS_DIR)\n", win.c_str());
    return 77;
  }
  auto list = [](const std::string& dir, const wchar_t* pattern) {
    std::vector<std::string> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(widen(dir + "\\").append(pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do out.push_back(dir + "\\" + narrow(fd.cFileName));
    while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end());
    return out;
  };
  auto pe = list(ad40, L"*.AD"), ne = list(classic, L"*.AD");
  fprintf(stderr, "assets: %zu AD40 modules, %zu Classic modules under %s\n", pe.size(), ne.size(), win.c_str());
  CHECK(pe.size() >= 20);
  CHECK(ne.size() >= 50);
  for (auto& p : pe) {
    ModuleProbe m = probe_module(p);
    if (m.kind != LaneKind::pe32) fprintf(stderr, "  %s: %s (%s)\n", p.c_str(), lane_kind_name(m.kind), m.detail.c_str());
    CHECK(m.kind == LaneKind::pe32);
  }
  for (auto& p : ne) {
    ModuleProbe m = probe_module(p);
    if (m.kind != LaneKind::ne16) fprintf(stderr, "  %s: %s (%s)\n", p.c_str(), lane_kind_name(m.kind), m.detail.c_str());
    CHECK(m.kind == LaneKind::ne16);
  }
  for (auto& [path, want] : {std::pair<std::string, LaneKind>{ad40 + "\\ADXPL510.DLL", LaneKind::pe32},
                             std::pair<std::string, LaneKind>{classic + "\\ADXPL300.DLL", LaneKind::ne16}}) {
    ModuleProbe m = probe_module(path);
    fprintf(stderr, "  %s: %s (%s)\n", path.c_str(), lane_kind_name(m.kind), m.detail.c_str());
    CHECK(m.kind == want);
  }
  // adhostwin recognizes a real module of each lane, including by the
  // catalog-relative path the front-end passes. Without the lane that is
  // exit 3 exactly; with it linked the run must have been ROUTED to the lane
  // (0, or 1 if the lane cannot run this module yet) — never 2 (not
  // recognized) or 3 (a registered lane that failed to link).
  struct AssetRun {
    std::string arg;
    bool lane_linked;
  };
  std::vector<AssetRun> runs;
  if (!pe.empty()) runs.push_back({pe.front(), bool(ADW_HAVE_LANE_PE32)});
  if (!ne.empty()) runs.push_back({ne.front(), bool(ADW_HAVE_LANE_NE16)});
  runs.push_back({"FILES/AD40/TOASTERS.AD", bool(ADW_HAVE_LANE_PE32)});
  // Star Wars Screen Entertainment's Intermission modules, when imported, are
  // NE too: the ne16 lane's (which tells the two protocols apart by exports).
  auto imx = list(win + "\\packages\\swse\\SAVER", L"*.IMX");
  if (!imx.empty()) fprintf(stderr, "assets: %zu Intermission modules\n", imx.size());
  for (auto& p : imx) {
    ModuleProbe m = probe_module(p);
    if (m.kind != LaneKind::ne16) fprintf(stderr, "  %s: %s (%s)\n", p.c_str(), lane_kind_name(m.kind), m.detail.c_str());
    CHECK(m.kind == LaneKind::ne16);
  }
  if (!imx.empty()) runs.push_back({"packages/swse/SAVER/VADER.IMX", bool(ADW_HAVE_LANE_NE16)});
  for (auto& run : runs) {
    Child c;
    ChildOptions o;
    o.args = {run.arg};
    o.env = {{"ADFRAMES", "1"}, {"AD_ASSETS_DIR", env.assets_root}};
    o.stdin_pipe = false;
    CHECK(c.start(o));
    int code = c.wait_exit(60000);
    fprintf(stderr, "  adhostwin %s -> exit %d\n", run.arg.c_str(), code);
    if (run.lane_linked) CHECK(code == 0 || code == 1);
    else CHECK_EQ(code, 3);
  }
  return adw_test::failures() ? 1 : 0;
}

// ---- --audio-assets (AUDIO.md §10.3) -------------------------------------------

namespace {

struct AudioCase {
  const char* name;
  const char* path;      // catalog-relative, under the win dir
  bool pe32;             // its lane (a case whose lane is not linked is skipped)
  uint64_t frames;       // ADFRAMES
  int min_note_ons;      // .mid note-ons (0 = not checked)
  bool voices;           // [audio] voices > 0
  bool nonsilent;        // some 100 ms window above -40 dBFS
  int speech_windows;    // voice starts with a 100 ms window above -40 dBFS within 20 ms
  int min_song_starts;   // "song … start" trace lines
  const char* cvset;     // ADCVSET, or nullptr for the module's defaults
};

// The table of §10.3. TOAST3's loop needs 5 minutes: 18000 frames at 60 fps,
// and its Music control at "Always" (control 2 = 80): the default "Once"
// plays OMTW.MID a single time (AUDIO.md §8.5).
const AudioCase kAudioCases[] = {
    {"toasters", "FILES/AD40/TOASTERS.AD", true, 1800, 100, true, false, 0, 0, nullptr},
    {"toaster2", "packages/ad10/AD10TH/TOASTER2.AD", true, 1800, 100, false, false, 0, 0, nullptr},
    {"burns", "packages/simpsons/SIMPSONS/BURNS.AD", false, 1800, 0, true, true, 4, 0, nullptr},
    {"fish", "FILES/AD40/FISH.AD", true, 1800, 0, true, true, 0, 0, nullptr},
    {"bungee", "packages/tt/TWISTED/BUNGEE.AD", false, 1800, 0, false, true, 0, 0, nullptr},
    {"toast3", "FILES/CLASSIC/TOAST3.AD", false, 18000, 1, false, false, 0, 2, "2=80"},
    {"halloffa", "packages/ad10/AD10TH/HALLOFFA.AD", true, 1800, 0, false, true, 0, 0, nullptr},
};

struct AudioRun {
  int exit_code = -3;
  std::string err;
  Capture cap;
  std::vector<uint8_t> wav, mid;
  std::vector<uint64_t> notes, hashes;
};

AudioRun run_audio_case(const AudioCase& c, const std::string& assets_root, const std::string& tag) {
  AudioRun r;
  std::string wav = e2e_temp(std::string("aa_") + c.name + tag + ".wav");
  std::string mid = wav.substr(0, wav.size() - 4) + ".mid";
  Child ch;
  ChildOptions o;
  o.args = {c.path};
  o.env = {{"ADFRAMES", std::to_string(c.frames)}, {"ADGOWAITMS", "0"}, {"ADAUDIOOUT", wav},
           {"ADTRACE", "audio"}, {"ADFBHASH", "1"}, {"AD_ASSETS_DIR", assets_root}};
  if (c.cvset) o.env["ADCVSET"] = c.cvset;
  o.stdin_pipe = false;
  if (!ch.start(o)) return r;
  r.exit_code = ch.wait_exit(1800000);
  r.err = ch.err();
  r.cap = read_capture(wav);
  r.wav = read_all(wav);
  r.mid = read_all(mid);
  r.notes = note_on_ms(mid);
  r.hashes = hash_values(fbhashes(r.err));
  remove_capture(wav);
  return r;
}

// The loudest 100 ms window starting within ±20 ms of t (µs), in dBFS.
double window_near(const Capture& cap, uint64_t t) {
  double best = -999;
  for (int d = -20; d <= 20; d += 5) {
    int64_t start_us = int64_t(t) + d * 1000;
    if (start_us < 0) continue;
    size_t a = size_t(uint64_t(start_us) * cap.rate / 1000000);
    best = std::max(best, cap.rms_db(a, a + cap.rate / 10));
  }
  return best;
}

}  // namespace

// Sound-on baselines (tests/audio_baselines.txt): per case, the frame count
// and FNV-1a 64 digests of its FBHASH stream, WAV capture and MIDI log — a
// sound-on run is as deterministic as a silent one, so any change to what a
// module draws or plays with sound on shows here. One line per case:
//   <case> <frames> <fbhash digest> <wav digest> <mid digest>
// AD_AUDIO_BASELINE_WRITE=1 records the cases that ran (keeping the others).
std::map<std::string, std::string> read_baselines() {
  std::map<std::string, std::string> out;
  std::string text;
  std::vector<uint8_t> raw = read_all(ADW_AUDIO_BASELINES);
  text.assign(raw.begin(), raw.end());
  size_t pos = 0;
  while (pos < text.size()) {
    size_t eol = text.find('\n', pos);
    std::string line = text.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
    pos = eol == std::string::npos ? text.size() : eol + 1;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    size_t sp = line.find(' ');
    if (sp != std::string::npos) out[line.substr(0, sp)] = line.substr(sp + 1);
  }
  return out;
}

std::string baseline_of(const AudioRun& r) {
  uint64_t h = kFnvOffset;
  for (uint64_t v : r.hashes) h = fnv1a64(&v, sizeof(v), h);
  char buf[128];
  snprintf(buf, sizeof(buf), "%zu %016llx %016llx %016llx", r.hashes.size(), (unsigned long long)h,
           (unsigned long long)fnv1a64(r.wav.data(), r.wav.size()), (unsigned long long)fnv1a64(r.mid.data(), r.mid.size()));
  return buf;
}

void write_baselines(const std::map<std::string, std::string>& b) {
  std::string text =
      "# Sound-on baselines for core.audio_assets (test_e2e.cc read_baselines): <case> <frames> <FBHASH stream\n"
      "# digest> <WAV capture digest> <MIDI log digest>, FNV-1a 64. Record anew with AD_AUDIO_BASELINE_WRITE=1 after\n"
      "# an intended change to pacing, drawing or sound.\n";
  for (const auto& [k, v] : b) text += k + " " + v + "\n";
  FILE* f = _wfopen(widen(ADW_AUDIO_BASELINES).c_str(), L"wb");
  if (!f) {
    fprintf(stderr, "cannot write %s\n", ADW_AUDIO_BASELINES);
    return;
  }
  fwrite(text.data(), 1, text.size(), f);
  fclose(f);
}

static int run_audio_assets_mode() {
  const char* sel = getenv("AD_AUDIO_ASSETS");
  if (!sel || !*sel || !strcmp(sel, "0")) {
    fprintf(stderr, "SKIP: opt-in; set AD_AUDIO_ASSETS=1 (every case) or a comma list of cases\n");
    return 77;
  }
  std::string want = sel;
  bool all = want == "1" || want == "all";
  // The assets: AD_E2E_ASSETS, else AD_ASSETS_DIR, else the installed ones (read-only).
  Env env = Env::from_process();
  if (const char* e = getenv("AD_E2E_ASSETS"); e && *e) env.assets_root = e;
  else if (!env.get("AD_ASSETS_DIR") || env.get("AD_ASSETS_DIR")->find_first_not_of(" \t") == std::string::npos)
    env.assets_root = adw_test::installed_assets_root();
  std::string win = env.win_assets_dir();
  DWORD a = GetFileAttributesW(widen(win + "\\FILES\\AD40").c_str());
  if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) {
    fprintf(stderr, "SKIP: no Windows After Dark assets at %s (set AD_E2E_ASSETS or AD_ASSETS_DIR)\n", win.c_str());
    return 77;
  }
  int ran = 0;
  const AudioCase* repeat = nullptr;
  std::map<std::string, std::string> baselines = read_baselines();
  const bool record = getenv("AD_AUDIO_BASELINE_WRITE") && !strcmp(getenv("AD_AUDIO_BASELINE_WRITE"), "1");
  for (const AudioCase& c : kAudioCases) {
    if (!all && ("," + want + ",").find(std::string(",") + c.name + ",") == std::string::npos) continue;
    if (!(c.pe32 ? ADW_HAVE_LANE_PE32 : ADW_HAVE_LANE_NE16)) {
      fprintf(stderr, "  %s: skipped (the %s lane is not linked)\n", c.name, c.pe32 ? "pe32" : "ne16");
      continue;
    }
    if (!exists(win + "\\" + c.path)) {
      fprintf(stderr, "  %s: skipped (%s absent)\n", c.name, c.path);
      continue;
    }
    ran++;
    int before = adw_test::failures();
    AudioRun r = run_audio_case(c, env.assets_root, "");
    auto sum = audio_summary(r.err);
    auto starts = trace_times(r.err, "voice", "start");
    auto songs = trace_times(r.err, "song", "start");
    double loudest = -999;
    for (size_t f = 0; r.cap.ok() && f + r.cap.rate / 10 <= r.cap.frames(); f += r.cap.rate / 10)
      loudest = std::max(loudest, r.cap.rms_db(f, f + r.cap.rate / 10));
    int speech = 0;
    for (uint64_t t : starts) speech += window_near(r.cap, t) > -40;
    fprintf(stderr, "  %s: exit %d, %zu frames, voices %llu, note-ons %zu, song starts %zu, loudest %.1f dBFS, "
                    "voice starts heard %d of %zu\n",
            c.name, r.exit_code, r.hashes.size(), (unsigned long long)sum["voices"], r.notes.size(), songs.size(),
            loudest, speech, starts.size());
    CHECK_EQ(r.exit_code, 0);
    CHECK(r.cap.ok());
    if (c.min_note_ons) CHECK(r.notes.size() >= size_t(c.min_note_ons));
    if (c.voices) CHECK(sum["voices"] > 0);
    if (c.nonsilent) CHECK(loudest > -40);
    if (c.speech_windows) CHECK(speech >= c.speech_windows);
    if (c.min_song_starts) CHECK(songs.size() >= size_t(c.min_song_starts));
    if (!repeat && c.frames <= 1800) repeat = &c;
    std::string got = baseline_of(r);
    if (record) {
      baselines[c.name] = got;
    } else if (auto it = baselines.find(c.name); it != baselines.end()) {
      CHECK_EQ(got, it->second);
      if (got != it->second) fprintf(stderr, "  %s: sound-on baseline %s, this run %s\n", c.name, it->second.c_str(), got.c_str());
    } else {
      fprintf(stderr, "  %s: no sound-on baseline recorded (AD_AUDIO_BASELINE_WRITE=1)\n", c.name);
    }
    fprintf(stderr, "%s audio %s\n", adw_test::failures() == before ? "PASS" : "FAIL", c.name);
  }
  // Any of the above twice: byte-identical captures and FBHASH streams.
  if (repeat) {
    AudioRun x = run_audio_case(*repeat, env.assets_root, "_a"), y = run_audio_case(*repeat, env.assets_root, "_b");
    CHECK(!x.wav.empty());
    CHECK(x.wav == y.wav);
    CHECK(x.mid == y.mid);
    CHECK(!x.hashes.empty() && x.hashes == y.hashes);
    fprintf(stderr, "%s audio repeat (%s twice)\n", x.wav == y.wav && x.mid == y.mid && x.hashes == y.hashes ? "PASS" : "FAIL",
            repeat->name);
  }
  if (ran == 0) {
    fprintf(stderr, "SKIP: no audio case can run in this build\n");
    return 77;
  }
  if (record) {
    write_baselines(baselines);
    fprintf(stderr, "recorded sound-on baselines in %s\n", ADW_AUDIO_BASELINES);
  }
  return adw_test::failures() ? 1 : 0;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: adw_core_e2e <adhostwin.exe> [--assets | --audio-assets | <test-filter>]\n");
    return 2;
  }
  // Absolute, backslashed: CreateProcess does not search a relative
  // forward-slash path the way a shell does.
  wchar_t full[MAX_PATH];
  DWORD n = GetFullPathNameW(widen(argv[1]).c_str(), MAX_PATH, full, nullptr);
  g_exe = (n > 0 && n < MAX_PATH) ? std::wstring(full, n) : widen(argv[1]);
  if (GetFileAttributesW(g_exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
    fprintf(stderr, "adhostwin not found: %s\n", narrow(g_exe).c_str());
    return 2;
  }
  const std::string scratch_lad = temp_dir() + "adw_e2e_lad_" + std::to_string(GetCurrentProcessId());
  g_scratch_lad = widen(scratch_lad);
  std::string mode = argc > 2 ? argv[2] : "";
  int rc = mode == "--assets"         ? run_assets_mode()
           : mode == "--audio-assets" ? run_audio_assets_mode()
                                      : adw_test::run_all(argc > 2 ? argv[2] : nullptr);
  remove_tree(scratch_lad);  // anything a host put under the scratch base
  return rc;
}
