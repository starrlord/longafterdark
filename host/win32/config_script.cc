#include "win32/config_script.hh"

#include <dwmapi.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>

#include "adw/core/env.h"
#include "adw/core/log.h"
#include "adw/core/text.h"

namespace adw::win32 {

namespace {

constexpr UINT kRunMessage = WM_APP + 0x3AD;  // posted: run this dialog's block
constexpr UINT_PTR kTimeoutTimer = 0x3AD1, kForceTimer = 0x3AD2;

std::string upper(std::string_view s) {
  std::string o(s);
  for (char& c : o) c = char(toupper(uint8_t(c)));
  return o;
}

std::wstring from_1252(std::string_view s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(1252, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(1252, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}

bool parse_int(std::string_view s, int* out) {
  std::string t(s);
  if (t.empty()) return false;
  char* end = nullptr;
  long v = strtol(t.c_str(), &end, 0);
  if (!end || *end) return false;
  *out = int(v);
  return true;
}

std::wstring class_of(HWND h) {
  wchar_t buf[64] = {};
  GetClassNameW(h, buf, 64);
  return buf;
}

bool is_class(HWND h, const wchar_t* name) { return _wcsicmp(class_of(h).c_str(), name) == 0; }

}  // namespace

struct ConfigScript::Attached {
  ConfigScript* owner = nullptr;
  WNDPROC old = nullptr;
  size_t block = SIZE_MAX;
  bool ran = false, cancelled = false;
  std::function<void(HWND)> force_close;
};

namespace {
std::map<HWND, ConfigScript::Attached>& attached() {
  static std::map<HWND, ConfigScript::Attached> m;
  return m;
}
}  // namespace

ConfigScript::ConfigScript() = default;

ConfigScript::~ConfigScript() {
  // Dialogs outliving the runner lose their subclass (they are gone by now in practice).
  for (auto it = attached().begin(); it != attached().end();) {
    if (it->second.owner == this) {
      if (IsWindow(it->first)) SetWindowLongPtrW(it->first, GWLP_WNDPROC, LONG_PTR(it->second.old));
      it = attached().erase(it);
    } else {
      ++it;
    }
  }
}

bool ConfigScript::load_env(const Env& env, std::string* error) {
  hidden_ = env.flag("ADCONFIGHIDDEN");
  dump_ = env.flag("ADCONFIGDUMP");
  if (const std::string* t = env.get("ADCONFIGTIMEOUTMS"); t && !t->empty()) {
    int v = 0;
    if (parse_int(*t, &v) && v > 0) timeout_ms_ = uint32_t(v);
    else log("ADCONFIGTIMEOUTMS='%s' is not a positive number; using %u", t->c_str(), timeout_ms_);
  }
  const std::string* path = env.get("ADCONFIGSCRIPT");
  if (!path || path->empty()) return true;
  HANDLE h = CreateFileW(widen(*path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    if (error) *error = "cannot read ADCONFIGSCRIPT '" + *path + "'";
    return false;
  }
  std::string text;
  char buf[4096];
  DWORD got = 0;
  while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got) text.append(buf, got);
  CloseHandle(h);
  // A UTF-8 script (with or without a BOM) is taken as UTF-8.
  if (text.size() >= 3 && uint8_t(text[0]) == 0xEF && uint8_t(text[1]) == 0xBB && uint8_t(text[2]) == 0xBF)
    text.erase(0, 3);
  return parse(text, error);
}

bool ConfigScript::parse(std::string_view text, std::string* error) {
  scripted_ = true;
  blocks_.assign(1, {});
  next_block_ = 0;
  answers_.clear();
  files_.clear();
  int lineno = 0;
  size_t pos = 0;
  auto fail = [&](const std::string& why) {
    if (error) *error = "ADCONFIGSCRIPT line " + std::to_string(lineno) + ": " + why;
    return false;
  };
  auto utf8_or_1252 = [](std::string_view s) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()), nullptr, 0);
    if (n <= 0) return from_1252(s);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
  };
  while (pos < text.size()) {
    size_t nl = text.find('\n', pos);
    if (nl == std::string_view::npos) nl = text.size();
    std::string_view line = text.substr(pos, nl - pos);
    pos = nl + 1;
    lineno++;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
    if (line.empty() || line[0] == '#') continue;
    size_t sp = line.find_first_of(" \t");
    std::string verb = upper(line.substr(0, sp));
    std::string_view rest = sp == std::string_view::npos ? std::string_view() : line.substr(sp + 1);
    while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t')) rest.remove_prefix(1);
    auto split1 = [&](std::string_view s, std::string_view* tail) {
      size_t k = s.find_first_of(" \t");
      std::string_view head = s.substr(0, k);
      *tail = k == std::string_view::npos ? std::string_view() : s.substr(k + 1);
      while (!tail->empty() && (tail->front() == ' ' || tail->front() == '\t')) tail->remove_prefix(1);
      return head;
    };
    Action a;
    a.line = lineno;
    if (verb == "NEXT") {
      blocks_.emplace_back();
      continue;
    }
    if (verb == "FILE") {
      if (rest.empty()) return fail("FILE needs a path");
      files_.push_back(utf8_or_1252(rest));
      continue;
    }
    if (verb == "ANSWER") {
      static const std::pair<const char*, int> kIds[] = {{"IDOK", IDOK},       {"IDCANCEL", IDCANCEL},
                                                          {"IDABORT", IDABORT}, {"IDRETRY", IDRETRY},
                                                          {"IDIGNORE", IDIGNORE}, {"IDYES", IDYES},
                                                          {"IDNO", IDNO}};
      std::string r = upper(rest);
      int v = 0;
      bool ok = false;
      for (auto& [n, id] : kIds) {
        if (r == n) {
          v = id;
          ok = true;
        }
      }
      if (!ok && !parse_int(r, &v)) return fail("ANSWER needs IDOK, IDCANCEL, IDYES, IDNO, IDABORT, IDRETRY, IDIGNORE or a number");
      answers_.push_back(v);
      continue;
    }
    std::string_view tail;
    std::string_view id_s = split1(rest, &tail);
    if (!parse_int(id_s, &a.id)) return fail(verb + " needs a control id");
    if (verb == "TEXT") {
      a.kind = Action::Kind::text;
      a.text = utf8_or_1252(tail);
    } else if (verb == "CHECK") {
      a.kind = Action::Kind::check;
      if (!parse_int(tail, &a.value) || a.value < 0 || a.value > 2) return fail("CHECK needs 0, 1 or 2");
    } else if (verb == "SELECT") {
      a.kind = Action::Kind::select;
      if (!parse_int(tail, &a.value)) return fail("SELECT needs an index");
    } else if (verb == "PICK") {
      a.kind = Action::Kind::pick;
      a.text = utf8_or_1252(tail);
      if (a.text.empty()) return fail("PICK needs the item's text");
    } else if (verb == "MULTI") {
      a.kind = Action::Kind::multi;
      std::string list(tail);
      size_t p = 0;
      while (p <= list.size()) {
        size_t c = list.find(',', p);
        if (c == std::string::npos) c = list.size();
        int v = 0;
        if (!parse_int(std::string_view(list).substr(p, c - p), &v)) return fail("MULTI needs i,j,…");
        a.items.push_back(v);
        p = c + 1;
      }
    } else if (verb == "CLICK") {
      a.kind = Action::Kind::click;
      if (!tail.empty()) return fail("CLICK takes only an id");
    } else if (verb == "PRESS") {
      a.kind = Action::Kind::press;
      if (!tail.empty()) {
        std::string_view ys;
        std::string_view xs = split1(tail, &ys);
        if (!parse_int(xs, &a.x) || !parse_int(ys, &a.y) || a.x < 0 || a.y < 0)
          return fail("PRESS takes an id, or an id and a point x y in the control");
        a.at = true;
      }
    } else {
      return fail("unknown action '" + verb + "'");
    }
    blocks_.back().push_back(std::move(a));
  }
  return true;
}

bool ConfigScript::apply(HWND dlg, const Action& a) {
  HWND c = GetDlgItem(dlg, a.id);
  switch (a.kind) {
    case Action::Kind::text: {
      if (!c) return false;
      SetWindowTextW(c, a.text.c_str());
      // Single-line edits notify WM_SETTEXT themselves; multi-line ones do not.
      if (GetWindowLongW(c, GWL_STYLE) & ES_MULTILINE)
        SendMessageW(dlg, WM_COMMAND, MAKEWPARAM(a.id, EN_CHANGE), LPARAM(c));
      return true;
    }
    case Action::Kind::check:
      if (!c) return false;
      SendMessageW(c, BM_SETCHECK, WPARAM(a.value), 0);
      SendMessageW(dlg, WM_COMMAND, MAKEWPARAM(a.id, BN_CLICKED), LPARAM(c));
      return true;
    case Action::Kind::select:
      if (!c) return false;
      if (is_class(c, L"ComboBox")) {
        SendMessageW(c, CB_SETCURSEL, WPARAM(a.value), 0);
        SendMessageW(dlg, WM_COMMAND, MAKEWPARAM(a.id, CBN_SELCHANGE), LPARAM(c));
      } else if (is_class(c, L"ListBox")) {
        if (GetWindowLongW(c, GWL_STYLE) & (LBS_MULTIPLESEL | LBS_EXTENDEDSEL)) {
          SendMessageW(c, LB_SETSEL, FALSE, -1);
          SendMessageW(c, LB_SETSEL, TRUE, LPARAM(a.value));
        } else {
          SendMessageW(c, LB_SETCURSEL, WPARAM(a.value), 0);
        }
        SendMessageW(dlg, WM_COMMAND, MAKEWPARAM(a.id, LBN_SELCHANGE), LPARAM(c));
      } else {
        return false;
      }
      return true;
    case Action::Kind::pick: {
      // The item whose whole text this is (ignoring case), wherever a sorted
      // list put it; then as SELECT.
      if (!c) return false;
      bool combo = is_class(c, L"ComboBox");
      if (!combo && !is_class(c, L"ListBox")) return false;
      LRESULT i = SendMessageW(c, combo ? CB_FINDSTRINGEXACT : LB_FINDSTRINGEXACT, WPARAM(-1), LPARAM(a.text.c_str()));
      if (i < 0) return false;
      Action s = a;
      s.kind = Action::Kind::select;
      s.value = int(i);
      return apply(dlg, s);
    }
    case Action::Kind::multi:
      if (!c || !is_class(c, L"ListBox")) return false;
      SendMessageW(c, LB_SETSEL, FALSE, -1);
      for (int i : a.items) SendMessageW(c, LB_SETSEL, TRUE, LPARAM(i));
      SendMessageW(dlg, WM_COMMAND, MAKEWPARAM(a.id, LBN_SELCHANGE), LPARAM(c));
      return true;
    case Action::Kind::click:
      SendMessageW(dlg, WM_COMMAND, MAKEWPARAM(a.id, BN_CLICKED), LPARAM(c));
      return true;
    case Action::Kind::press: {
      // The point, on the screen; the window a user's click there reaches.
      if (!c || !IsWindowVisible(c)) return false;
      RECT cr{};
      GetClientRect(c, &cr);
      POINT pt = a.at ? POINT{a.x, a.y} : POINT{(cr.left + cr.right) / 2, (cr.top + cr.bottom) / 2};
      ClientToScreen(c, &pt);
      LRESULT ht = HTNOWHERE;
      HWND hit = hit_window(dlg, pt, &ht);
      if (!hit || ht != HTCLIENT) return false;
      if (hit != c && !IsChild(c, hit)) {
        log("ADCONFIGSCRIPT line %d: the click on control %d lands on control %d (class %s)", a.line, a.id, GetDlgCtrlID(hit),
            narrow(class_of(hit)).c_str());
      }
      // As the user's click arrives (sent, so it is over before the next
      // action), then what the controls posted (a guest's check box posts its
      // BN_CLICKED), as the dialog handles it before the user's next action.
      POINT cp = pt;
      ScreenToClient(hit, &cp);
      const LPARAM at = MAKELPARAM(cp.x, cp.y);
      SendMessageW(hit, WM_MOUSEMOVE, 0, at);
      SendMessageW(hit, WM_LBUTTONDOWN, MK_LBUTTON, at);
      if (IsWindow(hit)) SendMessageW(hit, WM_LBUTTONUP, 0, at);
      MSG m;
      for (int i = 0; i < 256 && IsWindow(dlg) && PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE | PM_QS_POSTMESSAGE); i++) {
        if (m.message == WM_QUIT) {
          PostQuitMessage(int(m.wParam));
          break;
        }
        if (IsDialogMessageW(dlg, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
      }
      return true;
    }
  }
  return false;
}

HWND ConfigScript::hit_window(HWND top, POINT pt, LRESULT* ht) {
  const LONG style = GetWindowLongW(top, GWL_STYLE);
  RECT r{};
  if (!(style & WS_VISIBLE) || !GetWindowRect(top, &r) || !PtInRect(&r, pt)) return nullptr;
  if (style & WS_DISABLED) {
    if ((style & (WS_CHILD | WS_POPUP)) == WS_CHILD) return nullptr;
    *ht = HTERROR;
    return top;
  }
  POINT cp = pt;
  ScreenToClient(top, &cp);
  RECT cr{};
  GetClientRect(top, &cr);
  if (!(style & WS_MINIMIZE) && PtInRect(&cr, cp)) {
    for (HWND k = GetWindow(top, GW_CHILD); k; k = GetWindow(k, GW_HWNDNEXT)) {
      if (HWND h = hit_window(k, pt, ht)) return h;
    }
  }
  *ht = SendMessageW(top, WM_NCHITTEST, 0, MAKELPARAM(pt.x, pt.y));
  if (int16_t(*ht) == HTTRANSPARENT) return nullptr;
  return top;
}

void ConfigScript::dump_dialog(HWND dlg) {
  wchar_t title[256] = {};
  GetWindowTextW(dlg, title, 256);
  log("[configdump] dialog \"%s\" (class %s)", narrow(title).c_str(), narrow(class_of(dlg)).c_str());
  EnumChildWindows(
      dlg,
      [](HWND c, LPARAM) -> BOOL {
        wchar_t text[256] = {};
        GetWindowTextW(c, text, 256);
        log("[configdump]   id=%d class=%s style=0x%08lX text=\"%s\"", GetDlgCtrlID(c), narrow(class_of(c)).c_str(),
            (unsigned long)GetWindowLongW(c, GWL_STYLE), narrow(text).c_str());
        return TRUE;
      },
      0);
}

void ConfigScript::park(HWND dlg) {
  BOOL cloak = TRUE;
  DwmSetWindowAttribute(dlg, DWMWA_CLOAK, &cloak, sizeof(cloak));
  SetWindowLongW(dlg, GWL_EXSTYLE, GetWindowLongW(dlg, GWL_EXSTYLE) | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW);
  SetWindowPos(dlg, HWND_BOTTOM, -32000, -32000, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

void ConfigScript::attach(HWND dlg, std::function<void(HWND)> force_close) {
  dialogs_++;
  if (hidden_) park(dlg);
  if (dump_) dump_dialog(dlg);
  if (!scripted_ && !hidden_) return;
  Attached a;
  a.owner = this;
  a.force_close = std::move(force_close);
  if (scripted_ && next_block_ < blocks_.size()) a.block = next_block_++;
  a.old = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(dlg, GWLP_WNDPROC));
  attached()[dlg] = std::move(a);
  SetWindowLongPtrW(dlg, GWLP_WNDPROC, LONG_PTR(&ConfigScript::subclass_proc));
  PostMessageW(dlg, kRunMessage, 0, 0);
}

LRESULT CALLBACK ConfigScript::subclass_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
  auto it = attached().find(dlg);
  if (it == attached().end()) return DefWindowProcW(dlg, msg, wp, lp);
  WNDPROC old = it->second.old;
  ConfigScript* self = it->second.owner;
  switch (msg) {
    case kRunMessage: {
      if (it->second.ran) return 0;
      it->second.ran = true;
      size_t block = it->second.block;
      if (block != SIZE_MAX && block < self->blocks_.size()) {
        // Copy: an action may open a nested dialog, which attaches (and
        // may rehash the map) before this loop continues.
        std::vector<Action> actions = self->blocks_[block];
        for (const Action& a : actions) {
          if (!IsWindow(dlg)) break;
          if (!apply(dlg, a)) {
            log("ADCONFIGSCRIPT line %d: control %d is missing or does not fit%s", a.line, a.id,
                a.kind == Action::Kind::pick ? (", or has no item \"" + narrow(a.text) + "\"").c_str() : "");
          }
        }
      }
      if (IsWindow(dlg)) SetTimer(dlg, kTimeoutTimer, self->timeout_ms_, nullptr);
      return 0;
    }
    case WM_TIMER:
      if (wp == kTimeoutTimer) {
        KillTimer(dlg, kTimeoutTimer);
        self->timed_out_ = true;
        log("configure: a dialog was still open %u ms after its script; closing it with IDCANCEL", self->timeout_ms_);
        SetTimer(dlg, kForceTimer, 1000, nullptr);
        SendMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), LPARAM(GetDlgItem(dlg, IDCANCEL)));
        return 0;
      }
      if (wp == kForceTimer) {
        KillTimer(dlg, kForceTimer);
        auto jt = attached().find(dlg);
        if (jt != attached().end() && jt->second.force_close) {
          log("configure: the dialog ignored IDCANCEL; ending it");
          auto fc = jt->second.force_close;
          fc(dlg);
        }
        return 0;
      }
      break;
    case WM_NCDESTROY: {
      SetWindowLongPtrW(dlg, GWLP_WNDPROC, LONG_PTR(old));
      attached().erase(dlg);
      return CallWindowProcW(old, dlg, msg, wp, lp);
    }
    default:
      break;
  }
  return CallWindowProcW(old, dlg, msg, wp, lp);
}

std::optional<int> ConfigScript::message_box(std::wstring_view text) {
  if (!answers_.empty()) {
    int v = answers_.front();
    answers_.pop_front();
    log("configure: message box \"%s\" answered %d by the script", narrow(std::wstring(text)).c_str(), v);
    return v;
  }
  if (hidden_) {
    log("configure: message box \"%s\" has no scripted answer; IDCANCEL", narrow(std::wstring(text)).c_str());
    return IDCANCEL;
  }
  return std::nullopt;
}

std::optional<std::wstring> ConfigScript::file_dialog() {
  if (!files_.empty()) {
    std::wstring f = files_.front();
    files_.pop_front();
    log("configure: file dialog answered \"%s\" by the script", narrow(f).c_str());
    return f;
  }
  if (hidden_) {
    log("configure: file dialog has no scripted answer; cancelled");
    return std::wstring();
  }
  return std::nullopt;
}

}  // namespace adw::win32
