#include "args.h"

#include <cwctype>

namespace adw::scr {

namespace {

// HWNDs arrive as decimal. They are 32-bit values sign-extended on x64, so
// a launcher that printed one as a signed int ("-1234") must still work;
// "0x" hex is accepted as a courtesy for hand-typed tests.
bool parse_hwnd(const std::wstring& s, uintptr_t& out) {
  size_t i = 0;
  while (i < s.size() && iswspace(s[i])) ++i;
  bool neg = false;
  if (i < s.size() && (s[i] == L'-' || s[i] == L'+')) { neg = s[i] == L'-'; ++i; }
  int base = 10;
  if (i + 1 < s.size() && s[i] == L'0' && (s[i + 1] == L'x' || s[i + 1] == L'X')) { base = 16; i += 2; }
  size_t start = i;
  unsigned long long v = 0;
  for (; i < s.size(); ++i) {
    wchar_t c = s[i];
    int d;
    if (c >= L'0' && c <= L'9') d = c - L'0';
    else if (base == 16 && c >= L'a' && c <= L'f') d = c - L'a' + 10;
    else if (base == 16 && c >= L'A' && c <= L'F') d = c - L'A' + 10;
    else break;
    v = v * base + d;
  }
  if (i == start) return false;
  while (i < s.size() && iswspace(s[i])) ++i;
  if (i != s.size()) return false;
  long long sv = neg ? -(long long)v : (long long)v;
  out = (uintptr_t)sv;
  return true;
}

std::wstring lower(std::wstring s) {
  for (auto& c : s) c = (wchar_t)towlower(c);
  return s;
}

// A Windows switch: its letter alone, or with an HWND glued on ("/P1234",
// "/c:1234"). A longer word ("/start", or "/sizes" mistyped) is not one.
bool classic_switch(const std::wstring& t, Mode* m) {
  const wchar_t c = (wchar_t)towlower(t[1]);
  if (c == L's') *m = Mode::run;
  else if (c == L'p' || c == L'l') *m = Mode::preview;   // "/l" is the Win3.x-era spelling
  else if (c == L'c') *m = Mode::settings;
  else if (c == L'a') *m = Mode::password;
  else return false;
  if (t.size() == 2) return true;
  const wchar_t n = t[2];
  return n == L':' || n == L'=' || n == L'-' || n == L'+' || iswdigit(n) || iswspace(n);
}

// "1920x1080": a number of one to five digits either side of an 'x'.
bool parse_size(const std::wstring& s, int& w, int& h) {
  const size_t x = s.find_first_of(L"xX");
  if (x == std::wstring::npos) return false;
  auto number = [](const std::wstring& d, int& out) {
    if (d.empty() || d.size() > 5) return false;
    int v = 0;
    for (wchar_t c : d) {
      if (c < L'0' || c > L'9') return false;
      v = v * 10 + (c - L'0');
    }
    out = v;
    return true;
  };
  return number(s.substr(0, x), w) && number(s.substr(x + 1), h);
}

std::wstring size_text(int w, int h) { return std::to_wstring(w) + L"x" + std::to_wstring(h); }

} // namespace

Args parse_args(const std::vector<std::wstring>& argv) {
  Args a;
  bool classic = false, window = false, help = false, sized = false;
  std::wstring classic_text;   // the Windows switch as written
  std::wstring other;          // the first unknown switch or stray word
  auto fail = [&](std::wstring e) {
    if (a.error.empty()) a.error = std::move(e);
  };
  for (size_t k = 0; k < argv.size(); ++k) {
    const std::wstring& t = argv[k];
    if (t.size() < 2 || (t[0] != L'/' && t[0] != L'-')) {
      if (other.empty()) other = t;
      continue;
    }
    // Long After Dark's own: the name up to a ':' or '=', which starts its value.
    const size_t from = t.compare(0, 2, L"--") == 0 ? 2 : 1;
    const size_t sep = t.find_first_of(L":=", from);
    const std::wstring name = lower(t.substr(from, sep == std::wstring::npos ? std::wstring::npos : sep - from));
    const bool glued = sep != std::wstring::npos;
    // A switch's value: glued on, else the next argument unless that is a switch.
    auto value = [&](std::wstring* out) {
      if (glued) {
        *out = t.substr(sep + 1);
        return true;
      }
      if (k + 1 < argv.size() && !argv[k + 1].empty() && argv[k + 1][0] != L'/' && argv[k + 1][0] != L'-') {
        *out = argv[++k];
        return true;
      }
      return false;
    };
    if (name == L"help" || name == L"?") {
      help = true;
      continue;
    }
    if (name == L"window" || name == L"random") {
      if (glued) fail(L"/" + name + L" takes no value (\"" + t + L"\").");
      if (name == L"window") window = true;
      else a.random = true;
      continue;
    }
    if (name == L"size") {
      std::wstring v;
      int w = 0, h = 0;
      if (!value(&v) || !parse_size(v, w, h)) {
        fail(v.empty() ? L"/size needs the window's size in pixels, such as /size 1920x1080."
                       : L"/size wants the window's size in pixels as WIDTHxHEIGHT, such as 1920x1080, not \"" + v +
                             L"\".");
      } else if (sized && (w != a.width || h != a.height)) {
        fail(L"One /size at a time (" + size_text(a.width, a.height) + L" and " + size_text(w, h) + L").");
      } else if (w < kWindowMinW || h < kWindowMinH) {
        fail(L"/size " + size_text(w, h) + L" is too small: " + size_text(kWindowMinW, kWindowMinH) + L" at least.");
      } else if (w > kWindowMaxW || h > kWindowMaxH) {
        fail(L"/size " + size_text(w, h) + L" is too large: " + size_text(kWindowMaxW, kWindowMaxH) + L" at most.");
      } else {
        sized = true;
        a.width = w;
        a.height = h;
      }
      continue;
    }
    if (name == L"module") {
      std::wstring v;
      if (!value(&v) || v.empty()) {
        fail(L"/module needs a module: its id, such as ad40.toasters, or its name in quotes, such as "
             L"\"Flying Toasters!\".");
      } else if (!a.module.empty() && a.module != v) {
        fail(L"One module at a time (\"" + a.module + L"\" and \"" + v + L"\").");
      } else {
        a.module = v;
      }
      continue;
    }
    Mode m;
    if (from == 1 && classic_switch(t, &m)) {
      if (classic) continue;   // the first one decides
      classic = true;
      classic_text = t;
      a.mode = m;
      // HWND: glued ("/p1234"), after ':'/'=' ("/c:1234"), or the next token.
      std::wstring rest = t.substr(2);
      if (!rest.empty() && (rest[0] == L':' || rest[0] == L'=')) rest.erase(0, 1);
      uintptr_t h = 0;
      if (!rest.empty()) {
        if (parse_hwnd(rest, h)) { a.hwnd = h; a.has_hwnd = true; }
      } else if (k + 1 < argv.size() && parse_hwnd(argv[k + 1], h)) {
        a.hwnd = h;
        a.has_hwnd = true;
        ++k;
      }
      continue;
    }
    if (other.empty()) other = t;   // an unknown switch
  }

  if (help) {
    a.mode = Mode::help;
    a.error.clear();
    return a;
  }
  if (window) {
    if (classic) fail(L"/window runs in a window, not as " + classic_text + L": leave one of them out.");
    if (!other.empty()) {
      fail(other.size() > 1 && (other[0] == L'/' || other[0] == L'-')
               ? L"There is no switch \"" + other + L"\"."
               : L"\"" + other + L"\" is not a switch: a value goes right after its switch (/size 1920x1080), "
                                 L"and a name with spaces in quotes (/module \"Flying Toasters!\").");
    }
    a.mode = Mode::window;
  } else if (sized || !a.module.empty() || a.random) {
    fail(L"/size, /module and /random go with /window.");
  }
  if (a.mode == Mode::preview && (!a.has_hwnd || a.hwnd == 0)) a.valid = false;
  return a;
}

std::wstring usage_text() {
  return L"LongAfterDark.exe /window [/size WxH] [/module <module>] [/random]\n"
         L"\n"
         L"/window: in an ordinary window titled \"Long After Dark\" instead of the full screen (for OBS's "
         L"Window Capture, say). It stays open, whatever the keyboard and mouse do, until you close it.\n"
         L"/size WxH: the size of the picture inside the window, in pixels: 1280x720 unless you say "
         L"(160x120 to 7680x4320).\n"
         L"/module <module>: that module, by its id (ad40.toasters) or by its name as the settings window "
         L"lists it, in quotes when it has spaces (\"Flying Toasters!\").\n"
         L"/random: the modules in turn, those Random plays in the settings window and as often; with "
         L"/module, that one first.\n"
         L"With neither /module nor /random, the window shows what the screen saver would.\n"
         L"\n"
         L"/s: the screen saver, full screen.\n"
         L"/c, or nothing: the settings window.\n"
         L"/p <window>, /a <window>: what Windows itself uses (the preview in Screen Saver Settings; a "
         L"Windows 95 password).\n"
         L"/help or /?: this message.\n"
         L"\n"
         L"Windows starts a .scr with /S and nothing else, so type these after LongAfterDark.exe, the same "
         L"program, as in:\n"
         L".\\LongAfterDark.exe /window /size 1920x1080 /random";
}

std::wstring usage_message(const std::wstring& problem) {
  return problem + L"\n\n"
                   L"LongAfterDark.exe /window [/size WxH] [/module <module>] [/random], as in:\n"
                   L".\\LongAfterDark.exe /window /size 1920x1080 /random\n"
                   L"\n"
                   L"LongAfterDark.exe /help says what each switch does.";
}

} // namespace adw::scr
