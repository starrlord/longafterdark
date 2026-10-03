// fakehost — a stand-in for adhostwin.exe that speaks the DESIGN.md §1
// protocol with a synthetic palette-cycling image, so LongAfterDark.scr can be
// built and tested before (and independently of) the real emulator host.
//
//   fakehost <module-path> [key=value …]
//   fakehost --capabilities
//   fakehost --configure <module-path> --button <n> [--owner <hwnd>]
//
// Protocol: frames on stdout (P8, or P6 with FAKEHOST_FORMAT=P6) only when
// ADSTREAM=1; GO/SET/KEY/CAPS/MOUSE/QUIT lines on stdin. One frame per GO
// once a GO has been seen; before that, self-paced at 10 fps (the "module's
// own rate"). Exits on QUIT, on stdin EOF after a GO, when a stdout write
// fails, or after ADFRAMES frames.
//
// Interaction (INTERACTION.md §3): KEY/CAPS/NUMLOCK/MOUSE lines are numbered
// 1, 2, 3 … as the real host numbers them and logged ("input" lines). CAPS 1
// makes it interactive (CAPS 0 ends that) and KEY/MOUSE lines while
// interactive are eaten; NUMLOCK only sets its toggle (ADNUMLOCK at start).
// A line it doesn't know is ignored, unnumbered, and logged ("unknown"
// lines), as the real host ignores one ("ignoring unrecognized input line").
// With ADSTATUSHANDLE it publishes the status record after every frame.
// ADSEEDIMG is opened as a host opens it and checked (a P6 of the screen size).
//
// Test levers (env):
//   FAKEHOST_LOG=<file>        append "start …"/"exit …"/"input …" lines (argv + the env the saver passed)
//   FAKEHOST_EXIT_AFTER=<n>    exit with code 3 after n frames (a crashing module)
//   FAKEHOST_FAIL_START=<code> exit with <code> before the first frame (a module
//                              whose lane isn't built: the real host exits 3)
//   FAKEHOST_STALL_AFTER=<n>   stop producing frames after n, stay alive (a hung module)
//   FAKEHOST_FORMAT=P6         emit P6 instead of P8
//   FAKEHOST_GARBAGE=1         write a junk line before every frame (parser resync)
//   FAKEHOST_CORRUPT_EVERY=<n> spoil the magic of every nth frame ("PX"), so the
//                              front-end's parser discards that whole frame: the
//                              GO it answered is never seen to be answered
//   FAKEHOST_LANES=pe32        the lanes this "build" has: a module under a CLASSIC
//                              folder exits 3 at once, as the real host does for a
//                              module whose lane isn't built in (and --capabilities says so)
//   FAKEHOST_CONFIGURE=<lanes> what --capabilities lists under configure= (default: the lanes;
//                              "none" = no lane)
//   FAKEHOST_ABIS=<abis>       what --capabilities lists under abis= (default
//                              "afterdark,intermission", as today's host; "none" leaves the key
//                              out, as a host from before module ABIs). Without "intermission"
//                              an .IMX module exits 1 at once, as such a host fails to load one
//   FAKEHOST_EXIT3_MODULE=<t>  a module whose path contains <t> (any case) exits 3 at once, as
//                              the real host does for a module whose lane isn't built in,
//                              whatever --capabilities listed (the front-end marks that one alone)
//   FAKEHOST_CAPS_DELAY_MS=<ms> --capabilities answers only after this long (a cold or busy
//                              host: the saver's wait for the answer can run out first)
//   FAKEHOST_NUMLOCK=0         a host from before the Num Lock toggle: --capabilities leaves
//                              numlock=1 out (today's host lists it) and a NUMLOCK line is
//                              a line it doesn't know (logged "unknown", never numbered)
//   FAKEHOST_IGNORE_QUIT=1     ignore QUIT (a module stuck in one long step): only
//                              termination ends us, after the front-end's grace
//   FAKEHOST_QUIT_DELAY_MS=<ms> take this long after QUIT before exiting (a host
//                              silencing its audio device, AUDIO.md §9); the exit
//                              line is written only once it has passed
//
// The "start" lines also carry the sound variables the front-end passed
// (ADSOUND, ADVOLUME, ADAUDIOOUT; AUDIO.md §9). fakehost makes no sound.
// They (and the "configure" and "capabilities" lines) carry ADNE16IMXSPEED
// too, Intermission 4.0's Speed: a host control's variable (catalog "host").
//   FAKEHOST_INTERACTIVE=0     CAPS never makes it interactive
//   FAKEHOST_CURSOR=1          raise the cursor flag while interactive
//   FAKEHOST_ROTATE_OK=1       raise the rotate-ok flag while interactive
//   FAKEHOST_KEYFILTER=1       raise the key-filter flag; FAKEHOST_EAT_VKS=<vk,…>
//                              are then eaten even when not interactive
//   FAKEHOST_WAKE_AFTER=<n>    raise the wake flag after n frames
//   FAKEHOST_CONFIGURE_MS=<ms> --configure: how long the "dialog" stays up (default 400)
//   FAKEHOST_CONFIGURE_EXIT=<code>|crash  --configure's exit (default 0; crash = an access violation)
//   FAKEHOST_CONFIGURE_EXIT_FILE=<file>   ...read from this file at each run instead (a test
//                              changes it between runs of one dialog)
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "adw/core/status.h"

namespace {

std::string env(const char* name) {
  const char* v = getenv(name);
  return v ? v : "";
}

long long env_ll(const char* name, long long fallback) {
  std::string v = env(name);
  return v.empty() ? fallback : atoll(v.c_str());
}

void log_event(const std::string& line) {
  std::string path = env("FAKEHOST_LOG");
  if (path.empty()) return;
  HANDLE h = CreateFileA(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return;
  std::string l = line + "\r\n";
  DWORD put;
  WriteFile(h, l.data(), (DWORD)l.size(), &put, nullptr);
  CloseHandle(h);
}

// Who started us: the saver's /s, or the settings dialog (its live preview
// and its probe), which the smoke tests tell apart.
unsigned long parent_pid() {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return 0;
  PROCESSENTRY32W pe{};
  pe.dwSize = sizeof(pe);
  unsigned long ppid = 0;
  for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
    if (pe.th32ProcessID == GetCurrentProcessId()) {
      ppid = pe.th32ParentProcessID;
      break;
    }
  }
  CloseHandle(snap);
  return ppid;
}

std::string upper_path(std::string path) {
  for (char& c : path) c = (char)toupper((unsigned char)(c == '\\' ? '/' : c));
  return path;
}

bool in_classic_folder(const std::string& path) { return upper_path(path).find("/CLASSIC/") != std::string::npos; }

// Star Wars Screen Entertainment's Intermission modules are *.IMX (the real
// host tells them by their exports; the placeholders have none).
bool imx_module(const std::string& path) {
  const std::string p = upper_path(path);
  return p.size() >= 4 && p.compare(p.size() - 4, 4, ".IMX") == 0;
}

// FAKEHOST_ABIS as --capabilities prints it ("" = the key left out).
std::string abis_listed() {
  if (!getenv("FAKEHOST_ABIS")) return "afterdark,intermission";
  const std::string v = env("FAKEHOST_ABIS");
  return v == "none" ? std::string() : v;
}

// This "build" keeps a Num Lock toggle (numlock=1), as today's host does,
// unless FAKEHOST_NUMLOCK=0.
bool keeps_numlock() { return env("FAKEHOST_NUMLOCK") != "0"; }

std::set<int> vk_set(const std::string& list) {
  std::set<int> out;
  size_t p = 0;
  while (p < list.size()) {
    size_t c = list.find(',', p);
    std::string item = list.substr(p, c == std::string::npos ? std::string::npos : c - p);
    if (!item.empty()) out.insert(atoi(item.c_str()));
    p = c == std::string::npos ? list.size() : c + 1;
  }
  return out;
}

struct Input {
  std::mutex mu;
  std::condition_variable cv;
  ULONGLONG started = GetTickCount64();
  long long first_go_ms = -1;     // how long after our start the first GO was read
  int gos = 0;
  bool go_seen = false, quit = false, eof = false;
  std::vector<std::pair<int, int>> sets;
  int caps = 0;
  int numlock = 0;
  bool numlock_lines = true;       // NUMLOCK is a line it knows (keeps_numlock)
  // Interaction.
  uint64_t seq = 0;                // input lines read
  std::deque<uint64_t> go_seq;     // seq as each pending GO was read
  bool interactive = false;
  uint64_t eaten = 0;
  bool interactive_enabled = true;
  bool key_filter = false;
  std::set<int> eat_vks;
};

void stdin_reader(Input* in) {
  const bool ignore_quit = env("FAKEHOST_IGNORE_QUIT") == "1";
  HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
  std::string buf;
  char tmp[512];
  for (;;) {
    DWORD got = 0;
    if (!h || h == INVALID_HANDLE_VALUE || !ReadFile(h, tmp, sizeof(tmp), &got, nullptr) || got == 0) break;
    buf.append(tmp, got);
    size_t nl;
    while ((nl = buf.find('\n')) != std::string::npos) {
      std::string line = buf.substr(0, nl);
      buf.erase(0, nl + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      std::lock_guard lk(in->mu);
      int a, b, c;
      if (line == "GO") {
        ++in->gos;
        in->go_seq.push_back(in->seq);
        if (!in->go_seen) in->first_go_ms = (long long)(GetTickCount64() - in->started);
        in->go_seen = true;
      } else if (line == "QUIT") {
        if (!ignore_quit) in->quit = true;
      } else if (sscanf(line.c_str(), "SET %d %d", &a, &b) == 2) {
        in->sets.emplace_back(a, b);
      } else if (sscanf(line.c_str(), "CAPS %d", &a) == 1) {
        in->caps = a;
        ++in->seq;
        if (in->interactive_enabled) in->interactive = a != 0;
        log_event("input\tpid=" + std::to_string(GetCurrentProcessId()) + "\tseq=" + std::to_string(in->seq) +
                  "\tline=" + line);
      } else if (sscanf(line.c_str(), "KEY %d %d", &a, &b) == 2 || sscanf(line.c_str(), "MOUSE %d %d %d", &a, &b, &c) == 3) {
        ++in->seq;
        const bool key = line.compare(0, 4, "KEY ") == 0;
        if (in->interactive || (key && in->key_filter && in->eat_vks.count(a))) in->eaten = in->seq;
        log_event("input\tpid=" + std::to_string(GetCurrentProcessId()) + "\tseq=" + std::to_string(in->seq) +
                  "\tline=" + line + "\tinteractive=" + (in->interactive ? "1" : "0"));
      } else if (in->numlock_lines && sscanf(line.c_str(), "NUMLOCK %d", &a) == 1) {
        // Numbered like CAPS; it only sets the toggle (a module reads it).
        in->numlock = a != 0;
        ++in->seq;
        log_event("input\tpid=" + std::to_string(GetCurrentProcessId()) + "\tseq=" + std::to_string(in->seq) +
                  "\tline=" + line + "\tinteractive=" + (in->interactive ? "1" : "0"));
      } else if (!line.empty()) {
        // What a host does with a line it doesn't know: nothing, and no number.
        log_event("unknown\tpid=" + std::to_string(GetCurrentProcessId()) + "\tline=" + line);
      }
      in->cv.notify_all();
    }
  }
  std::lock_guard lk(in->mu);
  in->eof = true;
  in->cv.notify_all();
}

bool write_all(HANDLE h, const void* p, size_t n) {
  const char* c = static_cast<const char*>(p);
  while (n) {
    DWORD put = 0;
    if (!WriteFile(h, c, (DWORD)std::min<size_t>(n, 1 << 20), &put, nullptr) || put == 0) return false;
    c += put;
    n -= put;
  }
  return true;
}

// A hue wheel, rotated by `phase`: palette animation is what the real P8
// stream exercises most (Zooommm!, Psycho Deli), so the fake does it too.
void make_palette(uint8_t* pal, int phase) {
  for (int i = 0; i < 256; ++i) {
    double h = ((i + phase) & 255) / 256.0 * 6.0;
    int k = (int)h;
    double f = h - k;
    double r = 0, g = 0, b = 0;
    switch (k % 6) {
      case 0: r = 1; g = f; break;
      case 1: r = 1 - f; g = 1; break;
      case 2: g = 1; b = f; break;
      case 3: g = 1 - f; b = 1; break;
      case 4: r = f; b = 1; break;
      default: r = 1; b = 1 - f; break;
    }
    pal[i * 3] = (uint8_t)(r * 255);
    pal[i * 3 + 1] = (uint8_t)(g * 255);
    pal[i * 3 + 2] = (uint8_t)(b * 255);
  }
  pal[0] = pal[1] = pal[2] = 0;   // index 0 stays black, like the Win95 static colours
}

// ADSEEDIMG, opened as a host opens it (INTERACTION.md §8): share read, write
// and delete, which a delete-on-close file requires. "ok WxH" when it is a
// binary P6 (maxval 255) of exactly the screen size with its whole body.
std::string check_seed(const std::string& path, int w, int h) {
  if (path.empty()) return "none";
  HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return "open-failed-" + std::to_string(GetLastError());
  LARGE_INTEGER size{};
  GetFileSizeEx(f, &size);
  std::string head(64, '\0');
  DWORD got = 0;
  ReadFile(f, head.data(), (DWORD)head.size(), &got, nullptr);
  CloseHandle(f);
  head.resize(got);
  int pw = 0, ph = 0, maxv = 0, n = 0;
  if (sscanf(head.c_str(), "P6 %d %d %d%n", &pw, &ph, &maxv, &n) != 3 || maxv != 255) return "bad-header";
  const long long body = (long long)pw * ph * 3, header = n + 1;
  if (size.QuadPart != header + body) return "bad-size-" + std::to_string(size.QuadPart);
  if (pw != w || ph != h) return "wrong-dims-" + std::to_string(pw) + "x" + std::to_string(ph);
  return "ok " + std::to_string(pw) + "x" + std::to_string(ph);
}

std::string env_fields() {
  std::string s;
  // ADNE16IMXSPEED: Intermission 4.0's Speed, a host control (catalog "host").
  for (const char* k : {"ADSTREAM", "ADSCREENW", "ADSCREENH", "ADCVSET", "AD_ASSETS_DIR", "ADSTATE", "ADCAPS",
                        "ADNUMLOCK", "ADSEEDIMG", "ADSTATUSHANDLE", "ADSOUND", "ADVOLUME", "ADAUDIOOUT",
                        "ADNE16IMXSPEED"}) {
    s += "\t";
    s += k;
    s += "=" + env(k);
  }
  return s;
}

// --capabilities (INTERACTION.md §3.3).
int capabilities() {
  if (const int delay = atoi(env("FAKEHOST_CAPS_DELAY_MS").c_str()); delay > 0) Sleep((DWORD)delay);
  std::string lanes = env("FAKEHOST_LANES");
  if (lanes.empty()) lanes = "pe32,ne16";
  std::string configure = getenv("FAKEHOST_CONFIGURE") ? env("FAKEHOST_CONFIGURE") : lanes;
  if (configure == "none") configure.clear();
  const std::string abis = abis_listed();
  log_event("capabilities\tpid=" + std::to_string(GetCurrentProcessId()) + "\tppid=" + std::to_string(parent_pid()) +
            env_fields());
  printf("lanes=%s configure=%s%s status=1 state=1 seed=1%s\n", lanes.c_str(), configure.c_str(),
         abis.empty() ? "" : (" abis=" + abis).c_str(), keeps_numlock() ? " numlock=1" : "");
  fflush(stdout);
  return 0;
}

// --configure <module> --button <n> [--owner <hwnd>] (INTERACTION.md §6.1):
// no dialog, but everything a front-end can check: the arguments, the
// environment, whether the owner is disabled while "the dialog" is up.
int configure(int argc, char** argv) {
  std::string module, button, owner;
  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--button" && i + 1 < argc) button = argv[++i];
    else if (a == "--owner" && i + 1 < argc) owner = argv[++i];
    else if (module.empty()) module = a;
  }
  HWND owner_hwnd = (HWND)(uintptr_t)strtoull(owner.c_str(), nullptr, 0);
  auto enabled = [&] { return owner_hwnd && IsWindow(owner_hwnd) ? (IsWindowEnabled(owner_hwnd) ? "1" : "0") : "-"; };
  const std::string pid = std::to_string(GetCurrentProcessId());
  log_event("configure\tpid=" + pid + "\tppid=" + std::to_string(parent_pid()) + "\tmodule=" + module + "\tbutton=" +
            button + "\towner=" + owner + "\towner_enabled=" + enabled() + env_fields());
  if (module.empty() || button.empty()) {
    printf("{\"result\":\"error\",\"dialogs\":0,\"message\":\"usage\",\"written\":[]}\n");
    return 2;
  }
  // "The dialog is up": the owner must stay disabled all along.
  const long long ms = env_ll("FAKEHOST_CONFIGURE_MS", 400);
  const ULONGLONG t0 = GetTickCount64();
  bool ever_enabled = false;
  while ((long long)(GetTickCount64() - t0) < ms) {
    if (std::string(enabled()) == "1") ever_enabled = true;
    Sleep(20);
  }
  std::string how = env("FAKEHOST_CONFIGURE_EXIT");
  if (std::string file = env("FAKEHOST_CONFIGURE_EXIT_FILE"); !file.empty()) {
    if (FILE* f = fopen(file.c_str(), "rb")) {
      char buf[32] = {};
      size_t n = fread(buf, 1, sizeof(buf) - 1, f);
      fclose(f);
      how.assign(buf, n);
      while (!how.empty() && (how.back() == '\n' || how.back() == '\r' || how.back() == ' ')) how.pop_back();
    }
  }
  log_event("configure-end\tpid=" + pid + "\towner_enabled=" + enabled() + "\towner_ever_enabled=" +
            (ever_enabled ? "1" : "0") + "\texit=" + (how.empty() ? "0" : how));
  if (how == "crash") {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    fflush(stdout);
    *(volatile int*)nullptr = 1;   // 0xC0000005, as a module that faults in its dialog
  }
  const int code = how.empty() ? 0 : atoi(how.c_str());
  printf("{\"result\":\"%s\",\"dialogs\":%d,\"message\":\"fake\",\"written\":[]}\n",
         code == 0 ? "ok" : code == 4 ? "nothing" : "error", code == 0 ? 1 : 0);
  fflush(stdout);
  return code;
}

} // namespace

int main(int argc, char** argv) {
  if (argc >= 2 && strcmp(argv[1], "--capabilities") == 0) return capabilities();
  if (argc >= 2 && strcmp(argv[1], "--configure") == 0) return configure(argc, argv);
  if (argc < 2) {
    fprintf(stderr, "usage: fakehost <module-path> [key=value ...]\n");
    return 2;
  }
  const bool stream = env("ADSTREAM") == "1";
  int w = (int)env_ll("ADSCREENW", 640), h = (int)env_ll("ADSCREENH", 480);
  if (w <= 0 || w > 8192) w = 640;
  if (h <= 0 || h > 8192) h = 480;
  const long long max_frames = env_ll("ADFRAMES", 0);
  const long long exit_after = env_ll("FAKEHOST_EXIT_AFTER", 0);
  const long long stall_after = env_ll("FAKEHOST_STALL_AFTER", 0);
  const long long fail_start = env_ll("FAKEHOST_FAIL_START", 0);
  const long long wake_after = env_ll("FAKEHOST_WAKE_AFTER", 0);
  const long long quit_delay_ms = std::min<long long>(env_ll("FAKEHOST_QUIT_DELAY_MS", 0), 10000);
  const bool p6 = env("FAKEHOST_FORMAT") == "P6";
  const bool garbage = env("FAKEHOST_GARBAGE") == "1";
  const long long corrupt_every = env_ll("FAKEHOST_CORRUPT_EVERY", 0);
  const bool want_cursor = env("FAKEHOST_CURSOR") == "1";
  const bool rotate_ok = env("FAKEHOST_ROTATE_OK") == "1";

  // The status record, when the front-end passed one.
  void* status = nullptr;
  if (std::string hs = env("ADSTATUSHANDLE"); !hs.empty()) {
    HANDLE sec = (HANDLE)(uintptr_t)strtoull(hs.c_str(), nullptr, 0);
    status = MapViewOfFile(sec, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(adw::AdwHostStatusV1));
  }
  const std::string seed = check_seed(env("ADSEEDIMG"), w, h);

  // Tab-separated key=value fields: paths may contain spaces.
  std::string line = "start\tpid=" + std::to_string(GetCurrentProcessId()) + "\tppid=" + std::to_string(parent_pid()) +
                     "\tmodule=" + argv[1] + env_fields() + "\tstatus_mapped=" + (status ? "1" : "0") +
                     "\tseed_check=" + seed;
  log_event(line);
  fprintf(stderr, "[fakehost] %s\n", line.c_str());
  if (!env("FAKEHOST_LANES").empty() && env("FAKEHOST_LANES").find("ne16") == std::string::npos &&
      in_classic_folder(argv[1])) {
    log_event("exit\tpid=" + std::to_string(GetCurrentProcessId()) + "\tframes=0\treason=lane-missing");
    return 3;
  }
  if (imx_module(argv[1]) && ("," + abis_listed() + ",").find(",intermission,") == std::string::npos) {
    // A host from before the Intermission modules takes one for an After
    // Dark module and fails to load it (exit 1 after 0 frames).
    log_event("exit\tpid=" + std::to_string(GetCurrentProcessId()) + "\tframes=0\treason=abi-missing");
    return 1;
  }
  if (const std::string t = env("FAKEHOST_EXIT3_MODULE");
      !t.empty() && upper_path(argv[1]).find(upper_path(t)) != std::string::npos) {
    log_event("exit\tpid=" + std::to_string(GetCurrentProcessId()) + "\tframes=0\treason=cant-run");
    return 3;
  }
  if (fail_start) {
    log_event("exit\tpid=" + std::to_string(GetCurrentProcessId()) + "\tframes=0\treason=fail-start");
    return (int)fail_start;
  }

  Input in;
  in.interactive_enabled = env("FAKEHOST_INTERACTIVE") != "0";
  in.key_filter = env("FAKEHOST_KEYFILTER") == "1";
  in.eat_vks = vk_set(env("FAKEHOST_EAT_VKS"));
  in.caps = env("ADCAPS") == "1";
  in.numlock_lines = keeps_numlock();
  in.numlock = in.numlock_lines && env("ADNUMLOCK") == "1";   // a host without the toggle never reads it
  std::thread(stdin_reader, &in).detach();
  HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);

  auto publish = [&](uint64_t frames, uint64_t applied) {
    if (!status) return;
    adw::AdwHostStatusV1 rec{};
    {
      std::lock_guard lk(in.mu);
      rec.flags = adw::ADWS_READY;
      if (in.interactive) {
        rec.flags |= adw::ADWS_INTERACTIVE;
        if (want_cursor) rec.flags |= adw::ADWS_CURSOR;
        if (rotate_ok) rec.flags |= adw::ADWS_ROTATE_OK;
        rec.source = adw::kStatusSourceAd4;
      }
      if (in.key_filter) rec.flags |= adw::ADWS_KEY_FILTER;
      rec.input_eaten = in.eaten;
    }
    if (wake_after && (long long)frames >= wake_after) rec.flags |= adw::ADWS_WAKE;
    rec.frames = frames;
    rec.input_applied = applied;
    rec.lane = adw::kStatusLaneTest;
    adw::write_status(status, rec);
  };
  publish(0, 0);

  // Static index field (concentric rings + a diagonal sweep); animation comes
  // from rotating the palette plus a one-index drift per frame.
  std::vector<uint8_t> base((size_t)w * h);
  double cx = w / 2.0, cy = h / 2.0, scale = 256.0 / std::max(w, h);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      double d = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy));
      base[(size_t)y * w + x] = (uint8_t)(1 + ((int)(d * scale * 3) + (x + y) / 8) % 255);
    }
  }
  std::string header = p6 ? "P6\n" + std::to_string(w) + " " + std::to_string(h) + "\n255\n"
                          : "P8\n" + std::to_string(w) + " " + std::to_string(h) + "\n";
  std::vector<uint8_t> frame(header.size() + (p6 ? (size_t)w * h * 3 : 768 + (size_t)w * h));
  memcpy(frame.data(), header.data(), header.size());
  uint8_t pal[768];
  int speed = 3;

  const char* reason = "done";
  int code = 0;
  unsigned long long n = 0;
  for (;;) {
    uint64_t applied = 0;
    {
      std::unique_lock lk(in.mu);
      // Lockstep once the front-end has sent a GO; otherwise our own 10 fps.
      in.cv.wait_for(lk, std::chrono::milliseconds(100), [&] { return in.quit || in.gos > 0 || (in.eof && in.go_seen); });
      if (in.quit) {
        reason = "quit";
        if (quit_delay_ms > 0) {
          lk.unlock();
          Sleep((DWORD)quit_delay_ms);
          lk.lock();
        }
        break;
      }
      if (in.eof && in.go_seen) { reason = "stdin-eof"; break; }
      if (in.go_seen && in.gos == 0) continue;
      applied = in.seq;
      if (in.gos > 0) {
        --in.gos;
        if (!in.go_seq.empty()) {
          applied = in.go_seq.front();
          in.go_seq.pop_front();
        }
      }
      for (auto [idx, val] : in.sets) if (idx == 0) speed = std::max(1, val % 16);
      in.sets.clear();
    }
    if (stall_after && (long long)n >= stall_after) continue;   // hung: alive, reading, silent
    make_palette(pal, (int)(n * speed));
    uint8_t* body = frame.data() + header.size();
    uint8_t drift = (uint8_t)(n / 4);
    if (p6) {
      for (size_t i = 0; i < base.size(); ++i) {
        uint8_t v = (uint8_t)(base[i] + drift);
        memcpy(body + i * 3, pal + v * 3, 3);
      }
    } else {
      memcpy(body, pal, 768);
      for (size_t i = 0; i < base.size(); ++i) body[768 + i] = (uint8_t)(base[i] + drift);
    }
    const bool corrupt = corrupt_every > 0 && (long long)(n + 1) % corrupt_every == 0;
    frame[1] = corrupt ? 'X' : header[1];
    // As the real host: the step's record goes out before its frame.
    publish(n + 1, applied);
    if (stream) {
      if (garbage && !write_all(out, "junk from a stray printf\n", 25)) { reason = "stdout-closed"; break; }
      if (!write_all(out, frame.data(), frame.size())) { reason = "stdout-closed"; break; }
    }
    ++n;
    if (exit_after && (long long)n >= exit_after) { reason = "exit-after"; code = 3; break; }
    if (max_frames && (long long)n >= max_frames) { reason = "adframes"; break; }
  }
  long long first_go_ms;
  uint64_t lines;
  int numlock;
  {
    std::lock_guard lk(in.mu);
    first_go_ms = in.first_go_ms;
    lines = in.seq;
    numlock = in.numlock;
  }
  line = "exit\tpid=" + std::to_string(GetCurrentProcessId()) + "\tframes=" + std::to_string(n) + "\treason=" + reason +
         "\tfirst_go_ms=" + std::to_string(first_go_ms) + "\tinput_lines=" + std::to_string(lines) +
         "\tnumlock=" + std::to_string(numlock);
  log_event(line);
  fprintf(stderr, "[fakehost] %s\n", line.c_str());
  // The stdin reader may still be blocked in ReadFile; don't wait for it.
  ExitProcess(code);
}
