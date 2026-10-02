// USER — windows, DCs of windows, input polling, time, rectangles, text
// formatting, resources (strings, bitmaps), messages and timers, and the
// dialog/menu surface the saver never shows (API_SURFACE.md §2 "USER", 158
// imports; SelectPalette/RealizePalette live with GDI in gdi16.cc).
//
// The window model is what a screen saver's world needs: the desktop, the
// saver window the lane hands OLDMOD16 (full screen, the DC it draws on),
// and windows modules create themselves (WMORPH, MESSAGE3, SLIDE) with their
// window procedures called for the messages USER sends synchronously
// (WM_CREATE, WM_DESTROY, SendMessage, DispatchMessage). The message queue
// holds posted messages and timer ticks against the virtual clock; nothing
// blocks — but an application task's GetMessage and WaitMessage wait for a
// message, ending the frame (user16_set_app_task, shim_families16.hh: the
// ne16 lane's Windows 3.1 screen savers), and its windows get what Windows
// 3.1's USER sent an application's: CreateWindow's WM_SIZE and WM_MOVE,
// GetMessage's WM_PAINT, BeginPaint's WM_ERASEBKGND, DefWindowProc's
// WM_CLOSE and WM_PAINT. The cursor (SetCursor, ShowCursor, ClipCursor) is
// state only, never the host's.
//
// The synthetic desktop (a Program Manager window and its PROGMAN.INI groups,
// for ADXPL40's and ADXPL310's desktop-icon gatherers) appears on the first
// EnumWindows, and icons are images of the guest's own resources or the
// host's drawing; see README.md "The synthetic desktop and icons".
//
// Input (input16.hh, INTERACTION.md §5.2): modules poll GetAsyncKeyState/
// GetKeyState/GetCursorPos, which read the host's InputState (KEY/CAPS/
// NUMLOCK/MOUSE lines; VK_LBUTTON/VK_RBUTTON/VK_MBUTTON from the MOUSE
// bitmask; GetKeyState's toggle bit for VK_CAPITAL and VK_NUMLOCK). The lane
// also hands each KEY line to the WH_KEYBOARD hooks (SetWindowsHook(Ex),
// chained by DefHookProc/CallNextHookEx, most recent first) and, unless one
// consumed it, posts it to the saver window as WM_KEYDOWN/WM_KEYUP — MOUSE
// lines as mouse messages — in an input queue read after the posted one,
// tagged with the line's number, so a module that takes the blanker's
// messages (LUNATIC: FindWindow("Sleep", NULL), PeekMessage(PM_REMOVE)) plays
// and the lane can report what it consumed. PeekMessage/GetMessage honour
// their hwnd and min/max filters; TranslateMessage makes WM_CHAR (US layout).
//
// Configure mode (dialogs16.hh) wraps many of these shims: a handle from the
// real-window range then names a real dialog or control.
//
// The guest pump (user16_dispatch_guest, shim_families16.hh): the ne16
// lane's Intermission protocol runs the message loop INTERMIS ran between
// two saver calls — the guest's posted messages to their window procedures,
// then the due timers — and counts what the guest posts to its own task
// (StepReport16::task_posts). The AD3 path never calls it.
//
// Window queries answer from the synthetic desktop: GetWindow/GetNextWindow
// walk the Z order, EnumChildWindows calls back for each descendant (the
// desktop's: its top-level windows and theirs), GetMenu is the Program
// Manager's menu bar or a top-level window's menu handle, GetWindowTask is
// this task for its windows and the shell's for the Program Manager.
//
// wsprintf/wvsprintf: a %s whose far pointer reads nothing (SWTEXT's
// configure dialog passes a near one) is "" and logged, never a fault.
//
// Known gaps, deliberately left (no module of the 202 in the five releases,
// nor Star Wars Screen Entertainment's 14, needs more; API_SURFACE.md §2
// USER):
//   * in the saver, dialogs and menus are refused (configure mode makes
//     dialogs real; menus stay refused).
//   * GetMessage never blocks outside an application task: with an empty
//     queue it returns a WM_NULL (WaitMessage returns at once).
//   * GetTopWindow is 0, EnumTaskWindows calls nothing back, and GetWindow's
//     GW_OWNER is 0 (no owners are tracked).
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <map>

#include "adw/core/log.h"
#include "win16/dialogs16.hh"
#include "win16/dos16.hh"
#include "win16/gdi16.hh"
#include "win16/input16.hh"
#include "win16/modules16.hh"
#include "win16/shim_families16.hh"
#include "win32/vfs.hh"

namespace adw::win16 {

namespace {

constexpr const char* U = "USER";
constexpr int16_t kGwlWndProc = -4;  // GWL_WNDPROC (winuser.h hides it on 64-bit builds)

struct Wnd16 {
  std::string cls, title;
  uint32_t proc = 0;
  RECT rect{};  // screen coordinates
  uint16_t parent = 0, hinst = 0, id = 0;
  uint32_t style = 0, exstyle = 0;
  bool visible = true, enabled = true, invalid = false;
  bool erase = false;  // an application task's window: BeginPaint sends WM_ERASEBKGND first
  std::map<int16_t, uint16_t> words;
  std::map<std::string, uint16_t> props;
};

struct Class16 {
  std::string name;
  uint32_t proc = 0;
  uint16_t style = 0, hinst = 0, atom = 0;
  uint16_t icon = 0, cursor = 0, background = 0;
  int16_t cls_extra = 0, wnd_extra = 0;
};

// An icon (or cursor) image: what DrawIcon paints. Handles are kIconFirst +
// 4k — multiples of 4, so they name no selector (…6/…7), no GDI object
// (gdi16.hh: 0x2000–0x7FFC) and no window. The images are resources of the
// guest's own modules (LoadIcon, ExtractIcon), bits a module hands CreateIcon,
// or the synthetic desktop's (desktop_icon()): never the host's system icons,
// whose art differs between Windows versions. LoadIcon(NULL, IDI_*) keeps
// returning the image-less kSystemIcon it always did.
struct Icon16 {
  int w = 32, h = 32;
  std::vector<uint8_t> mask;     // w*h, top-down: 1 = the screen shows through (AND bit set)
  std::vector<COLORREF> color;   // w*h: the XOR colour where the mask is 0
  // LoadIcon's: the module's resource, one shared handle however often it is
  // loaded (as Win16 did); DestroyIcon leaves it alone.
  bool shared = false;
};
constexpr uint16_t kIconFirst = 0x8000, kIconLimit = 0xC000;
constexpr uint16_t kSystemCursor = 0x0F00, kSystemIcon = 0x0F04;

// The synthetic desktop's one application window (PACKAGES.md §7.3): a
// Windows 3.1 Program Manager. Its HWND lies below the range the window
// counter hands out (0x0400 up), so creating it renumbers no other window.
constexpr uint16_t kProgmanHwnd = 0x0380;
// Its menu bar (GetMenu): a handle no selector, GDI object, icon or window has.
constexpr uint16_t kProgmanMenu = 0x0F08;

struct Timer16 {
  uint16_t hwnd = 0, id = 0;
  uint32_t elapse_ms = 0, proc = 0;
  uint64_t due_us = 0;
};

struct Msg16 {
  uint16_t hwnd = 0, msg = 0, wparam = 0;
  uint32_t lparam = 0;
  uint64_t seq = 0;  // input messages (input16.hh): the input line they came from
  uint32_t age = 0;  // steps an input message has waited
  bool host = false; // posted by the host (user16_post_host): the lane's pump dispatches it
};

// A WH_KEYBOARD hook (SetWindowsHook / SetWindowsHookEx). `token` is what
// the guest holds (SetWindowsHook's result, the HHOOK): DefHookProc(&token)
// and CallNextHookEx(token) call the hook installed before this one.
struct Hook16 {
  uint16_t type = 0;
  uint32_t proc = 0;
  uint32_t token = 0;
};
constexpr uint32_t kHookTokenBase = 0x484B0000;  // "HK": no selector the LDT hands out

struct UserState : RuntimeState16 {
  std::map<uint16_t, Wnd16> windows;
  std::map<std::string, Class16> classes;  // by upper-case name
  std::vector<Timer16> timers;
  std::deque<Msg16> queue;
  // Input messages (input16.hh): read after the posted queue, as Win16 read
  // the system queue after an application's posted messages.
  std::deque<Msg16> input;
  // Hooks, oldest first (the chain runs from the back).
  std::vector<Hook16> hooks;
  uint16_t next_hook = 1;
  // Tagged messages the guest removed this step, and whether it dispatched
  // each back to the saver window.
  std::vector<std::pair<Msg16, bool>> removed;
  uint64_t queue_reads = 0;  // saver-queue reads with removal and a key range
  uint32_t host_posted = 0;  // host-posted messages in `queue` (user16_post_host)
  bool wake = false;         // WM_CLOSE / SC_CLOSE posted to the saver window
  uint32_t task_posts = 0;   // task messages user16_dispatch_guest removed (StepReport16)
  uint16_t last_task_msg = 0;
  uint32_t bad_wsprintf_ptrs = 0;  // %s arguments read as "" (format16), the first few logged
  uint16_t next_hwnd = 0x0400, next_atom = 0xC000, next_timer = 1;
  uint16_t desktop = 0, saver = 0, focus = 0, active = 0, capture = 0;
  int cursor_count = 0;
  std::array<bool, 256> async_seen{};
  uint32_t last_msg_time = 0;
  // (HBRUSH)(COLOR_xxx + 1) → the brush made for it (FillRect & co.).
  std::map<uint16_t, uint16_t> sys_brushes;
  std::map<uint16_t, Icon16> icons;
  // LoadIcon's shared handles, by (hinst, group id): "#<n>" or the upper-cased
  // name. The image they were made from is kept weakly, so a module freed
  // and another loaded at the same hinst gets its own.
  struct LoadedIcon {
    uint16_t handle = 0;
    std::weak_ptr<loader::ne::Image> image;
  };
  std::map<std::pair<uint16_t, std::string>, LoadedIcon> loaded_icons;
  uint16_t progman = 0;  // the synthetic Program Manager, once the desktop exists
  // An application task's message loop (user16_set_app_task).
  bool app_task = false;
  uint64_t waits = 0;  // GetMessage/WaitMessage waits that ended a frame or moved time on
  // The cursor's emulated state (never the host's): SetCursor's handle,
  // ShowCursor's count, ClipCursor's rectangle.
  uint16_t cursor = 0;
  bool clipped = false;
  RECT16 clip{0, 0, 0, 0};
  std::string last_box;  // the last MessageBox's caption and text (user16_last_message_box)
};

UserState& us(Runtime16& rt) {
  UserState& s = rt.state<UserState>();
  if (!s.desktop) {
    Wnd16 d;
    d.cls = "#32769";
    Screen* sc = nullptr;
    (void)sc;
    int w = rt.display() ? rt.display()->width() : 640, h = rt.display() ? rt.display()->height() : 480;
    d.rect = RECT{0, 0, w, h};
    d.style = WS_POPUP | WS_VISIBLE;
    s.desktop = s.next_hwnd;
    s.next_hwnd += 4;
    s.windows[s.desktop] = d;
  }
  return s;
}
UserState& us(Call16& c) { return us(c.rt); }

Wnd16* wnd(Call16& c, uint16_t h) {
  auto& w = us(c).windows;
  auto it = w.find(h);
  return it == w.end() ? nullptr : &it->second;
}

int screen_w(Runtime16& rt) { return rt.display() ? rt.display()->width() : 640; }
int screen_h(Runtime16& rt) { return rt.display() ? rt.display()->height() : 480; }

RECT16 rrect(Call16& c, uint32_t fp) { return read16<RECT16>(c.rt, fp); }
void wrect(Call16& c, uint32_t fp, const RECT16& r) { write16(c.rt, fp, r); }

// A window procedure: LRESULT FAR PASCAL (HWND, UINT, WPARAM, LPARAM).
uint32_t call_wndproc(Runtime16& rt, uint32_t proc, uint16_t hwnd, uint16_t msg, uint16_t wp, uint32_t lp) {
  if (!proc) return 0;
  trace("user16", "wndproc %04X:%04X(hwnd %04X, msg %04X, %04X, %08X)", proc >> 16, proc & 0xFFFF, hwnd, msg, wp, lp);
  return rt.call_far(proc, {w16(hwnd), w16(msg), w16(wp), l16(lp)});
}

uint32_t send(Runtime16& rt, uint16_t hwnd, uint16_t msg, uint16_t wp, uint32_t lp) {
  UserState& s = us(rt);
  auto it = s.windows.find(hwnd);
  if (it == s.windows.end()) return 0;
  return call_wndproc(rt, it->second.proc, hwnd, msg, wp, lp);
}

// DestroyWindow: WM_DESTROY, then the window is gone (not the desktop or the
// lane's saver window). False when there is no such window.
bool destroy_window(Runtime16& rt, uint16_t h) {
  UserState& s = us(rt);
  if (!s.windows.count(h) || h == s.saver || h == s.desktop) return false;
  send(rt, h, WM_DESTROY, 0, 0);
  s.windows.erase(h);
  // Its timers die with it, as Windows' did.
  s.timers.erase(std::remove_if(s.timers.begin(), s.timers.end(), [&](const Timer16& t) { return s.app_task && t.hwnd == h; }),
                 s.timers.end());
  return true;
}

// Whether a message is there for GetMessage(hwnd 0, no filter) in an
// application task: posted, input, a window to paint, a timer due.
bool app_message_ready(Runtime16& rt) {
  UserState& s = us(rt);
  if (!s.queue.empty() || !s.input.empty()) return true;
  for (const auto& [h, w] : s.windows) {
    if (w.invalid && w.visible && w.proc) return true;
  }
  const uint64_t now = rt.peek_us();
  for (const Timer16& t : s.timers) {
    if (now >= t.due_us) return true;
  }
  return false;
}

// An application task's wait for a message (GetMessage, WaitMessage; Windows
// 3.1 gave the CPU to other tasks there): the frame ends here when the lane
// can end it (Runtime16::yield_frame), so the next frame looks again with
// virtual time moved on; else virtual time moves to the next timer or audio
// event at once. Nothing that could ever come: a fatal error, never a hang.
void app_wait(Runtime16& rt) {
  UserState& s = us(rt);
  s.waits++;
  if (!rt.yield_frame()) {
    const uint64_t now = rt.peek_us();
    uint64_t next = rt.audio_due();
    for (const Timer16& t : s.timers) {
      if (t.due_us > now) next = std::min(next, t.due_us);
    }
    if (next == UINT64_MAX) {
      throw GuestError16(GuestError16::Kind::fatal, "the task waits for a message, and nothing can come");
    }
    rt.wait_until_us(next);
  }
  rt.deliver_due_audio();
}

// ---- wsprintf ----------------------------------------------------------------------------------------

// Formats per Win16 wsprintf: %[-][#][0][width][.prec][l]{d,i,u,x,X,c,s,%};
// int-sized arguments are WORDs, 'l' ones DWORDs, %s a FAR pointer. `next`
// hands out argument words in order. A %s whose far pointer names no
// readable memory reads as "" (the first few are logged): SWTEXT's
// configure dialog passes a NEAR pointer (SWTEXT 1:06C9 pushes 0E24h alone),
// so the selector half is the stack word after it — the dialog's HWND16,
// which on Windows 95 happened to be some readable selector and printed
// garbage, and here, a real window's 0xC000.., is no selector at all.
std::string format16(Runtime16& rt, const std::string& fmt, const std::function<uint16_t()>& next) {
  std::string out;
  for (size_t i = 0; i < fmt.size(); i++) {
    char ch = fmt[i];
    if (ch != '%') {
      out.push_back(ch);
      continue;
    }
    if (++i >= fmt.size()) break;
    if (fmt[i] == '%') {
      out.push_back('%');
      continue;
    }
    bool left = false, alt = false, zero = false, lng = false;
    for (; i < fmt.size(); i++) {
      if (fmt[i] == '-') left = true;
      else if (fmt[i] == '#') alt = true;
      else if (fmt[i] == '0') zero = true;
      else break;
    }
    int width = 0, prec = -1;
    while (i < fmt.size() && isdigit(uint8_t(fmt[i]))) width = width * 10 + (fmt[i++] - '0');
    if (i < fmt.size() && fmt[i] == '.') {
      prec = 0;
      i++;
      while (i < fmt.size() && isdigit(uint8_t(fmt[i]))) prec = prec * 10 + (fmt[i++] - '0');
    }
    if (i < fmt.size() && (fmt[i] == 'l' || fmt[i] == 'L')) {
      lng = true;
      i++;
    } else if (i < fmt.size() && (fmt[i] == 'h' || fmt[i] == 'H')) {
      i++;
    }
    if (i >= fmt.size()) break;
    char t = fmt[i];
    std::string field;
    auto num = [&]() -> uint32_t {
      uint32_t lo = next();
      return lng ? lo | (uint32_t(next()) << 16) : lo;
    };
    switch (t) {
      case 'd':
      case 'i': {
        uint32_t v = num();
        int32_t sv = lng ? int32_t(v) : int16_t(v);
        field = std::to_string(sv);
        break;
      }
      case 'u':
        field = std::to_string(num());
        break;
      case 'x':
      case 'X': {
        char b[16];
        snprintf(b, sizeof(b), t == 'x' ? "%x" : "%X", num());
        field = (alt ? (t == 'x' ? "0x" : "0X") : "") + std::string(b);
        break;
      }
      case 'c':
        field = std::string(1, char(num()));
        break;
      case 's': {
        uint32_t fp = next();
        fp |= uint32_t(next()) << 16;
        try {
          field = rt.read_str(fp);
        } catch (const GuestError16&) {
          if (us(rt).bad_wsprintf_ptrs++ < 8) {
            log("win16: wsprintf %%s argument %04X:%04X is not a readable far pointer: read as \"\"", fp >> 16, fp & 0xFFFF);
          }
          field.clear();
        }
        if (prec >= 0 && field.size() > size_t(prec)) field.resize(size_t(prec));
        break;
      }
      default:
        field = std::string(1, t);
        break;
    }
    if (prec > 0 && t != 's' && t != 'c' && int(field.size()) < prec) field.insert(0, size_t(prec) - field.size(), '0');
    if (int(field.size()) < width) {
      size_t pad = size_t(width) - field.size();
      if (left) field.append(pad, ' ');
      else field.insert(0, pad, zero ? '0' : ' ');
    }
    out += field;
  }
  return out;
}

// ---- resources ---------------------------------------------------------------------------------------

std::string load_string(Runtime16& rt, uint16_t hinst, uint16_t id) {
  Module16* m = rt.modules().by_handle(hinst);
  if (!m || !m->image) return {};
  auto table = m->image->string_table();
  auto it = table.find(id);
  return it == table.end() ? std::string() : it->second;
}

uint16_t sys_metric(Runtime16& rt, int16_t i) {
  switch (i) {
    case SM_CXSCREEN:
    case SM_CXFULLSCREEN:
    case SM_CXMAXIMIZED:
      return uint16_t(screen_w(rt));
    case SM_CYSCREEN:
    case SM_CYMAXIMIZED:
      return uint16_t(screen_h(rt));
    case SM_CYFULLSCREEN:
      return uint16_t(screen_h(rt) - 20);
    case SM_CXVSCROLL:
    case SM_CYHSCROLL:
    case SM_CXHSCROLL:
    case SM_CYVSCROLL:
    case SM_CYVTHUMB:
    case SM_CXHTHUMB:
      return 16;
    case SM_CYCAPTION:
    case SM_CYMENU:
      return 20;
    // The caption's bitmaps (system menu box, minimize/maximize buttons): the
    // caption less its two border lines. INS and Bad Dog lay out the window
    // chrome they draw with these and the OBM_* bitmaps (system_bitmap()).
    case SM_CXSIZE:
    case SM_CYSIZE:
      return 18;
    case SM_CXMINTRACK:
      return 112;
    case SM_CYMINTRACK:
      return 27;
    case SM_CXDOUBLECLK:
    case SM_CYDOUBLECLK:
      return 4;
    // Desktop icon grid (WIN.INI IconSpacing / IconVerticalSpacing defaults):
    // CHAM, HOMEREAT and INS lay the icons they gather out on it.
    case SM_CXICONSPACING:
    case SM_CYICONSPACING:
      return 75;
    case SM_CXBORDER:
    case SM_CYBORDER:
      return 1;
    case SM_CXDLGFRAME:
    case SM_CYDLGFRAME:
    case SM_CXFRAME:
    case SM_CYFRAME:
      return 4;
    case SM_CXICON:
    case SM_CYICON:
    case SM_CXCURSOR:
    case SM_CYCURSOR:
      return 32;
    case SM_MOUSEPRESENT:
      return 1;
    case SM_CXMIN:
      return 112;
    case SM_CYMIN:
      return 27;
    case SM_CMOUSEBUTTONS:
      return 2;
    default:
      return 0;
  }
}

// Windows 95 default system colours.
uint32_t sys_color(int16_t i) {
  static const COLORREF k[] = {
      RGB(192, 192, 192), RGB(0, 128, 128), RGB(0, 0, 128),     RGB(128, 128, 128), RGB(192, 192, 192),
      RGB(255, 255, 255), RGB(0, 0, 0),     RGB(0, 0, 0),       RGB(0, 0, 0),       RGB(255, 255, 255),
      RGB(192, 192, 192), RGB(192, 192, 192), RGB(128, 128, 128), RGB(0, 0, 128),   RGB(255, 255, 255),
      RGB(192, 192, 192), RGB(128, 128, 128), RGB(128, 128, 128), RGB(0, 0, 0),     RGB(192, 192, 192),
      RGB(255, 255, 255), RGB(0, 0, 0),     RGB(223, 223, 223), RGB(0, 0, 0),       RGB(255, 255, 225)};
  return i >= 0 && size_t(i) < sizeof(k) / sizeof(k[0]) ? k[i] : 0;
}

// LoadBitmap(NULL, OBM_*): the Windows 3.1 system bitmaps a module draws a
// window's chrome with — INS (Itchy & Scratchy fight over a window it draws),
// OBJETS and Bad Dog. Drawn here in the static colours, the Windows 3.1 look
// (a raised grey button with a black glyph, the system-menu box's bar), never
// the host's own art, which differs between Windows versions. Caption bitmaps
// are SM_CXSIZE × SM_CYSIZE, scroll arrows SM_CXVSCROLL × SM_CYVSCROLL. Any
// other id is 0, as before.
uint16_t system_bitmap(Runtime16& rt, uint16_t id) {
  enum Glyph { kNone, kUp, kDown, kLeft, kRight, kUpDown, kBar };
  int size = 0;
  Glyph glyph = kNone;
  bool pressed = false, button = true;
  switch (id) {
    case 32754: size = 18, glyph = kBar, button = false; break;  // OBM_CLOSE (the system-menu box)
    case 32749: size = 18, glyph = kDown; break;                  // OBM_REDUCE
    case 32748: size = 18, glyph = kUp; break;                    // OBM_ZOOM
    case 32747: size = 18, glyph = kUpDown; break;                // OBM_RESTORE
    case 32746: size = 18, glyph = kDown, pressed = true; break;  // OBM_REDUCED
    case 32745: size = 18, glyph = kUp, pressed = true; break;    // OBM_ZOOMD
    case 32744: size = 18, glyph = kUpDown, pressed = true; break;  // OBM_RESTORED
    case 32753: size = 16, glyph = kUp; break;                    // OBM_UPARROW
    case 32752: size = 16, glyph = kDown; break;                  // OBM_DNARROW
    case 32751: size = 16, glyph = kRight; break;                 // OBM_RGARROW
    case 32750: size = 16, glyph = kLeft; break;                  // OBM_LFARROW
    case 32743: size = 16, glyph = kUp, pressed = true; break;    // OBM_UPARROWD
    case 32742: size = 16, glyph = kDown, pressed = true; break;  // OBM_DNARROWD
    case 32741: size = 16, glyph = kRight, pressed = true; break;  // OBM_RGARROWD
    case 32740: size = 16, glyph = kLeft, pressed = true; break;  // OBM_LFARROWD
    default: return 0;
  }
  if (!rt.display()) return 0;
  win32::Display& d = *rt.display();
  const int w = size, h = size;
  const COLORREF black = RGB(0, 0, 0), dark = RGB(128, 128, 128), face = RGB(192, 192, 192),
                 white = RGB(255, 255, 255);
  std::vector<COLORREF> px(size_t(w * h), face);
  auto set = [&](int x, int y, COLORREF c) {
    if (x >= 0 && y >= 0 && x < w && y < h) px[size_t(y * w + x)] = c;
  };
  if (button) {
    for (int i = 0; i < w; i++) set(i, h - 1, black);
    for (int i = 0; i < h; i++) set(w - 1, i, black);
    if (pressed) {
      for (int i = 0; i < w - 1; i++) set(i, 0, dark);
      for (int i = 0; i < h - 1; i++) set(0, i, dark);
    } else {
      for (int i = 0; i < w - 1; i++) set(i, 0, white), set(i, h - 2, dark);
      for (int i = 0; i < h - 1; i++) set(0, i, white), set(w - 2, i, dark);
      set(w - 2, 0, white);
    }
  }
  // The glyph: a black triangle (base 7, 4 rows) centred on the face, one
  // pixel down and right when pressed.
  int cx = (w - 1) / 2 + (pressed ? 1 : 0), cy = (h - 1) / 2 + (pressed ? 1 : 0);
  auto tri = [&](Glyph g, int x0, int y0, int rows) {
    for (int r = 0; r < rows; r++) {
      int half = rows - 1 - r;  // widest row first
      for (int k = -half; k <= half; k++) {
        switch (g) {
          case kDown: set(x0 + k, y0 - rows / 2 + r, black); break;
          case kUp: set(x0 + k, y0 + rows / 2 - r, black); break;
          case kRight: set(x0 - rows / 2 + r, y0 + k, black); break;
          case kLeft: set(x0 + rows / 2 - r, y0 + k, black); break;
          default: break;
        }
      }
    }
  };
  switch (glyph) {
    case kUp:
    case kDown:
    case kLeft:
    case kRight:
      tri(glyph, cx, cy, 4);
      break;
    case kUpDown:
      tri(kUp, cx, cy - 3, 3);
      tri(kDown, cx, cy + 3, 3);
      break;
    case kBar: {  // the system-menu box: a white bar with a black edge and a dark shadow
      int x0 = 3, x1 = w - 5, y0 = h / 2 - 2, y1 = h / 2 + 1;
      for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) set(x, y, (y == y0 || y == y1 || x == x0 || x == x1) ? black : white);
      for (int x = x0 + 1; x <= x1 + 1; x++) set(x, y1 + 1, dark);
      for (int y = y0 + 1; y <= y1 + 1; y++) set(x1 + 1, y, dark);
      break;
    }
    case kNone:
      break;
  }
  Gdi16& g = rt.state<Gdi16>();
  uint16_t hb = g.create_device_bitmap(w, h, 8);
  Obj16* o = g.get(hb, G16::bitmap);
  if (!o || !o->bmp.bits) return 0;
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      o->bmp.bits[size_t(y) * o->bmp.stride + size_t(x)] = uint8_t(d.nearest_index(px[size_t(y * w + x)], true));
  return hb;
}

uint16_t key_state(Call16& c, int vk, bool async) {
  const InputState& in = c.rt.input();
  vk &= 0xFF;
  bool down = in.keys.test(size_t(vk));
  // The mouse buttons from the MOUSE bitmask (1 left, 2 right, 4 middle).
  if (vk == VK_LBUTTON) down = in.mouse_button || (in.mouse_buttons & 1);
  if (vk == VK_RBUTTON) down = in.mouse_buttons & 2;
  if (vk == VK_MBUTTON) down = in.mouse_buttons & 4;
  uint16_t r = down ? 0x8000 : 0;
  if (async) {
    UserState& s = us(c);
    if (down && !s.async_seen[size_t(vk)]) r |= 1;
    s.async_seen[size_t(vk)] = down;
  } else if ((vk == VK_CAPITAL && in.caps) || (vk == VK_NUMLOCK && in.numlock)) {
    r |= 1;  // toggled: the CAPS and NUMLOCK lines (Final Exam starts its exam on Num Lock's)
  }
  return r;
}

// ---- icons ---------------------------------------------------------------------------------------------

uint16_t add_icon(UserState& s, Icon16 ic) {
  for (uint32_t h = kIconFirst; h < kIconLimit; h += 4) {
    if (!s.icons.count(uint16_t(h))) {
      s.icons[uint16_t(h)] = std::move(ic);
      return uint16_t(h);
    }
  }
  return 0;
}

Icon16* find_icon(UserState& s, uint16_t h) {
  auto it = s.icons.find(h);
  return it == s.icons.end() ? nullptr : &it->second;
}

uint32_t le16(std::string_view d, size_t o) { return o + 2 <= d.size() ? uint8_t(d[o]) | (uint8_t(d[o + 1]) << 8) : 0; }
uint32_t le32(std::string_view d, size_t o) { return le16(d, o) | (le16(d, o + 2) << 16); }

// An RT_ICON resource: a DIB (BITMAPINFOHEADER or the Win 3.0-era
// BITMAPCOREHEADER) of twice the icon's height, XOR image then AND mask, both
// bottom-up with DWORD-aligned rows.
bool decode_icon_dib(std::string_view d, Icon16* out) {
  uint32_t hdr = le32(d, 0);
  int w, h2, bpp, ncolors, rgb_size;
  if (hdr == 12) {
    w = int(le16(d, 4)), h2 = int(le16(d, 6)), bpp = int(le16(d, 10));
    ncolors = bpp <= 8 ? 1 << bpp : 0;
    rgb_size = 3;
  } else if (hdr >= 40) {
    w = int(int32_t(le32(d, 4))), h2 = int(int32_t(le32(d, 8))), bpp = int(le16(d, 14));
    uint32_t used = le32(d, 32);
    ncolors = bpp <= 8 ? (used && used <= (1u << bpp) ? int(used) : 1 << bpp) : 0;
    rgb_size = 4;
  } else {
    return false;
  }
  int h = h2 / 2;
  if (w <= 0 || h <= 0 || w > 256 || h > 256 || (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 24 && bpp != 32)) return false;
  size_t pal = hdr, xor_off = pal + size_t(ncolors) * size_t(rgb_size);
  size_t xor_stride = ((size_t(w) * size_t(bpp) + 31) / 32) * 4, and_stride = ((size_t(w) + 31) / 32) * 4;
  size_t and_off = xor_off + xor_stride * size_t(h);
  if (and_off + and_stride * size_t(h) > d.size()) return false;
  out->w = w;
  out->h = h;
  out->mask.assign(size_t(w) * size_t(h), 1);
  out->color.assign(size_t(w) * size_t(h), 0);
  auto pal_rgb = [&](int i) -> COLORREF {
    if (i >= ncolors) return 0;
    size_t o = pal + size_t(i) * size_t(rgb_size);
    return RGB(uint8_t(d[o + 2]), uint8_t(d[o + 1]), uint8_t(d[o]));  // RGBQUAD/RGBTRIPLE are B, G, R
  };
  for (int y = 0; y < h; y++) {
    size_t xr = xor_off + size_t(h - 1 - y) * xor_stride, ar = and_off + size_t(h - 1 - y) * and_stride;
    for (int x = 0; x < w; x++) {
      COLORREF c = 0;
      switch (bpp) {
        case 1:
          c = pal_rgb((uint8_t(d[xr + size_t(x) / 8]) >> (7 - x % 8)) & 1);
          break;
        case 4:
          c = pal_rgb((uint8_t(d[xr + size_t(x) / 2]) >> (x % 2 ? 0 : 4)) & 0xF);
          break;
        case 8:
          c = pal_rgb(uint8_t(d[xr + size_t(x)]));
          break;
        default: {
          size_t o = xr + size_t(x) * size_t(bpp / 8);
          c = RGB(uint8_t(d[o + 2]), uint8_t(d[o + 1]), uint8_t(d[o]));
        }
      }
      size_t i = size_t(y) * size_t(w) + size_t(x);
      out->color[i] = c;
      out->mask[i] = uint8_t((uint8_t(d[ar + size_t(x) / 8]) >> (7 - x % 8)) & 1);
    }
  }
  return true;
}

// An RT_GROUP_ICON's pick for an 8-bit display: 32×32 if there is one, the
// deepest colour format up to 8 bits per pixel.
bool load_group_icon(const loader::ne::Image& img, const loader::ne::Resource& group, Icon16* out) {
  std::string_view g = img.resource_data(group);
  uint32_t n = le16(g, 4);
  int best = -1, best_score = -1;
  for (uint32_t i = 0; i < n && 6 + 14 * (i + 1) <= g.size(); i++) {
    size_t e = 6 + 14 * i;
    int w = uint8_t(g[e]) ? uint8_t(g[e]) : 256, colors = uint8_t(g[e + 2]);
    int bits = int(le16(g, e + 6));
    if (!bits) bits = colors == 2 ? 1 : colors == 16 ? 4 : 8;
    int score = (w == 32 ? 1000 : 0) + (bits <= 8 ? bits * 10 : 0);
    if (score > best_score) best_score = score, best = int(i);
  }
  if (best < 0) return false;
  uint16_t id = uint16_t(le16(g, 6 + 14 * size_t(best) + 12));
  const loader::ne::Resource* r = img.find_resource(loader::ResId::of(loader::rt::icon), loader::ResId::of(id));
  return r && decode_icon_dib(img.resource_data(*r), out);
}

// The synthetic Program Manager's class icon, drawn here: a window with a
// navy title bar and four program icons in its client area, in the 16 VGA
// colours every palette keeps.
Icon16 desktop_icon() {
  Icon16 ic;
  ic.mask.assign(32 * 32, 1);
  ic.color.assign(32 * 32, 0);
  auto px = [&](int x, int y, COLORREF c) {
    ic.mask[size_t(y) * 32 + size_t(x)] = 0;
    ic.color[size_t(y) * 32 + size_t(x)] = c;
  };
  for (int y = 3; y <= 28; y++) {
    for (int x = 1; x <= 30; x++) {
      bool frame = y == 3 || y == 28 || x == 1 || x == 30 || y == 9;
      px(x, y, frame ? RGB(0, 0, 0) : y < 9 ? RGB(0, 0, 128) : RGB(255, 255, 255));
    }
  }
  for (int y = 4; y <= 7; y++)
    for (int x = 2; x <= 5; x++) px(x, y, RGB(192, 192, 192));
  static const COLORREF k[4] = {RGB(255, 0, 0), RGB(0, 128, 0), RGB(0, 0, 255), RGB(128, 128, 0)};
  for (int i = 0; i < 4; i++) {
    int x0 = 6 + (i % 2) * 13, y0 = 12 + (i / 2) * 8;
    for (int dy = 0; dy < 6; dy++)
      for (int dx = 0; dx < 7; dx++) px(x0 + dx, y0 + dy, k[i]);
  }
  return ic;
}

// ---- the synthetic desktop (PACKAGES.md §7.3) --------------------------------------------------------------
//
// ADXPL40's and ADXPL310's desktop-icon gatherers (CHAM, HOMEREAT, INS) find
// the icons they animate through EnumWindows (visible top-level windows with
// a class icon and a title) and Program Manager's groups (PROGMAN.INI
// [Groups] → .GRP names); with none, they GlobalAlloc 0 bytes, GlobalLock
// fails and the module stops with "Out of memory". The desktop they find is
// the saver window plus a Windows 3.1 Program Manager (class "Progman", a
// class icon drawn by desktop_icon(), a title), and PROGMAN.INI naming five
// groups. It comes into being on the first EnumWindows, so a module that
// never enumerates windows — every Deluxe one; BADDOG3 looks for PROGMAN with
// FindWindow — sees the machine it always saw.
void ensure_desktop(Runtime16& rt) {
  UserState& s = us(rt);
  if (s.progman) return;
  Class16 k;
  k.name = "Progman";
  k.style = CS_DBLCLKS;
  k.icon = add_icon(s, desktop_icon());
  k.cursor = kSystemCursor;
  k.background = COLOR_APPWORKSPACE + 1;
  s.classes[upper16(k.name)] = k;
  Wnd16 w;
  w.cls = k.name;
  w.title = "Program Manager";
  w.rect = RECT{40, 30, 440, 330};
  w.style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_VISIBLE;
  s.windows[kProgmanHwnd] = w;
  s.progman = kProgmanHwnd;
  seed_program_manager(rt);
}

// Z order of the children of `parent` (0 = the top-level windows): the
// windows in creation order, Program Manager at the bottom.
std::vector<uint16_t> z_order(UserState& s, uint16_t parent) {
  std::vector<uint16_t> out;
  bool top = parent == 0 || parent == s.desktop;
  for (auto& [h, w] : s.windows) {
    if (h == s.desktop || (top && h == s.progman)) continue;
    if (top ? (w.parent == 0 || w.parent == s.desktop) : w.parent == parent) out.push_back(h);
  }
  if (top && s.progman) out.push_back(s.progman);
  return out;
}

// GetWindow(hwnd, cmd): GW_HWNDFIRST/LAST/NEXT/PREV (0..3) among the window's
// siblings in Z order, GW_CHILD (5) its topmost child, GW_OWNER (4; none are
// tracked) 0. GetNextWindow is its NEXT/PREV half.
uint16_t window_rel(UserState& s, uint16_t h, uint16_t cmd) {
  auto wi = s.windows.find(h);
  if (wi == s.windows.end()) return 0;
  if (cmd == 5) {
    std::vector<uint16_t> kids = z_order(s, h == s.desktop ? 0 : h);
    return kids.empty() ? 0 : kids.front();
  }
  std::vector<uint16_t> sib = z_order(s, wi->second.parent == s.desktop ? 0 : wi->second.parent);
  auto it = std::find(sib.begin(), sib.end(), h);
  if (it == sib.end()) return 0;
  switch (cmd) {
    case 0: return sib.front();
    case 1: return sib.back();
    case 2: return it + 1 == sib.end() ? 0 : *(it + 1);
    case 3: return it == sib.begin() ? 0 : *(it - 1);
    default: return 0;
  }
}

// A window's descendants, each followed by its own, in Z order.
void descendants(UserState& s, uint16_t parent, std::vector<uint16_t>* out, int depth = 0) {
  if (depth > 32) return;
  for (uint16_t k : z_order(s, parent == s.desktop ? 0 : parent)) {
    out->push_back(k);
    descendants(s, k, out, depth + 1);
  }
}

}  // namespace

uint16_t user16_saver_window(Runtime16& rt) {
  UserState& s = us(rt);
  if (!s.saver) {
    Wnd16 w;
    w.cls = "WindowsScreenSaverClass";
    w.title = "Screen Saver";
    w.rect = RECT{0, 0, screen_w(rt), screen_h(rt)};
    w.style = WS_POPUP | WS_VISIBLE;
    s.saver = s.next_hwnd;
    s.next_hwnd += 4;
    s.windows[s.saver] = w;
    s.active = s.focus = s.saver;
  }
  return s.saver;
}

bool user16_class(Runtime16& rt, std::string_view name, Class16View* out) {
  UserState& s = us(rt);
  auto it = s.classes.find(upper16(name));
  if (it == s.classes.end()) return false;
  const Class16& k = it->second;
  *out = Class16View{k.name, k.proc, k.style, k.hinst, k.background, k.cursor, k.cls_extra, k.wnd_extra};
  return true;
}

uint32_t user16_sys_color(int index) { return sys_color(int16_t(index)); }

// ---- saver-window input (input16.hh) ---------------------------------------------------------------------

bool user16_has_keyboard_hook(Runtime16& rt) {
  for (const Hook16& h : us(rt).hooks) {
    if (h.type == WH_KEYBOARD) return true;
  }
  return false;
}

bool user16_keyboard_hooks(Runtime16& rt, uint16_t vk, uint32_t lparam, uint64_t seq) {
  UserState& s = us(rt);
  if (s.hooks.empty()) return false;
  // The most recent hook; it reaches the older ones through DefHookProc/CallNextHookEx.
  Hook16 top = s.hooks.back();
  uint32_t r = rt.call_far(top.proc, {w16(HC_ACTION), w16(vk), l16(lparam)});
  trace("input16", "WH_KEYBOARD %04X:%04X(HC_ACTION, vk %02X, %08X) for input seq %llu -> %08X%s", top.proc >> 16,
        top.proc & 0xFFFF, vk, lparam, (unsigned long long)seq, r, r ? ": consumed" : "");
  return r != 0;
}

void user16_post_input(Runtime16& rt, uint16_t msg, uint16_t wparam, uint32_t lparam, uint64_t seq) {
  UserState& s = us(rt);
  uint16_t saver = user16_saver_window(rt);
  s.input.push_back({saver, msg, wparam, lparam, seq});
  trace("input16", "input seq %llu posted to the saver window: msg %04X, %04X, %08X", (unsigned long long)seq, msg,
        wparam, lparam);
}

// ---- host-posted messages (sound16.cc: MM_WOM_*, MM_MCINOTIFY) -------------------------------------------

void user16_post_host(Runtime16& rt, uint16_t hwnd, uint16_t msg, uint16_t wparam, uint32_t lparam) {
  UserState& s = us(rt);
  Msg16 m{hwnd, msg, wparam, lparam};
  m.host = true;
  s.queue.push_back(m);
  s.host_posted++;
  trace("user16", "host posted msg %04X (%04X, %08X) to %04X", msg, wparam, lparam, hwnd);
}

int user16_dispatch_host(Runtime16& rt, int max) {
  UserState& s = us(rt);
  int n = 0;
  while (s.host_posted && n < max) {
    auto it = std::find_if(s.queue.begin(), s.queue.end(), [](const Msg16& m) { return m.host; });
    if (it == s.queue.end()) {
      s.host_posted = 0;
      break;
    }
    Msg16 m = *it;
    s.queue.erase(it);
    s.host_posted--;
    n++;
    // A window that is gone (or none: a task message) has nobody to take it.
    if (!m.hwnd || !s.windows.count(m.hwnd)) {
      trace("user16", "host message %04X for %04X dropped: no such window", m.msg, m.hwnd);
      continue;
    }
    trace("user16", "host message %04X (%04X, %08X) dispatched to %04X", m.msg, m.wparam, m.lparam, m.hwnd);
    send(rt, m.hwnd, m.msg, m.wparam, m.lparam);
  }
  return n;
}

bool user16_window_exists(Runtime16& rt, uint16_t hwnd) { return hwnd && us(rt).windows.count(hwnd) != 0; }

void user16_set_app_task(Runtime16& rt, bool on) { us(rt).app_task = on; }

void user16_post_message(Runtime16& rt, uint16_t hwnd, uint16_t msg, uint16_t wparam, uint32_t lparam) {
  us(rt).queue.push_back({hwnd, msg, wparam, lparam});
  trace("user16", "posted msg %04X (%04X, %08X) to %04X for the guest", msg, wparam, lparam, hwnd);
}

std::string user16_last_message_box(Runtime16& rt) { return us(rt).last_box; }

uint64_t user16_app_waits(Runtime16& rt) { return us(rt).waits; }

uint16_t user16_main_window(Runtime16& rt, uint16_t hinst) {
  UserState& s = us(rt);
  for (const auto& [h, w] : s.windows) {
    if (h != s.saver && h != s.desktop && w.hinst == hinst && !(w.style & WS_CHILD)) return h;
  }
  return 0;
}

int user16_dispatch_guest(Runtime16& rt, int max) {
  UserState& s = us(rt);
  int n = 0;
  // The guest's posted messages, in queue order; the ones a window
  // procedure posts meanwhile join the end and are taken too (up to max),
  // as INTERMIS's loop kept dispatching while its queue held something.
  while (n < max) {
    auto it = std::find_if(s.queue.begin(), s.queue.end(), [](const Msg16& m) { return !m.host; });
    if (it == s.queue.end()) break;
    Msg16 m = *it;
    s.queue.erase(it);
    n++;
    if (!m.hwnd) {
      s.task_posts++;
      s.last_task_msg = m.msg;
      trace("user16", "task message %04X (%04X, %08X) taken by the guest pump", m.msg, m.wparam, m.lparam);
      continue;
    }
    if (!s.windows.count(m.hwnd)) {
      trace("user16", "guest message %04X for %04X dropped: no such window", m.msg, m.hwnd);
      continue;
    }
    trace("user16", "guest message %04X (%04X, %08X) dispatched to %04X", m.msg, m.wparam, m.lparam, m.hwnd);
    if (m.msg == WM_TIMER && m.lparam) rt.call_far(m.lparam, {w16(m.hwnd), w16(m.msg), w16(m.wparam), l16(rt.tick_count())});
    else send(rt, m.hwnd, m.msg, m.wparam, m.lparam);
  }
  // Then the timers that are due, once each (GetMessage's WM_TIMER, which
  // DispatchMessage hands to the TIMERPROC, else to the window). A callback
  // may set or kill timers: each is looked up again before it fires, and
  // fires only if still due — one an earlier callback set again (SetTimer on
  // its hwnd and id resets it, as Windows did) waits for its new time, as
  // GetMessage's own timer check (take) would have it.
  std::vector<std::pair<uint16_t, uint16_t>> due;
  uint64_t now = rt.peek_us();
  for (const Timer16& t : s.timers) {
    if (now >= t.due_us) due.push_back({t.hwnd, t.id});
  }
  for (const auto& [hwnd, id] : due) {
    if (n >= max) break;
    auto t = std::find_if(s.timers.begin(), s.timers.end(), [&](const Timer16& x) { return x.hwnd == hwnd && x.id == id; });
    if (t == s.timers.end() || now < t->due_us) continue;
    uint32_t proc = t->proc;
    t->due_us = now + uint64_t(std::max<uint32_t>(t->elapse_ms, 1)) * 1000;
    n++;
    if (proc) {
      trace("user16", "timer %u of %04X: TIMERPROC %04X:%04X", id, hwnd, proc >> 16, proc & 0xFFFF);
      rt.call_far(proc, {w16(hwnd), w16(WM_TIMER), w16(id), l16(rt.tick_count())});
    } else if (hwnd && s.windows.count(hwnd)) {
      trace("user16", "timer %u: WM_TIMER to %04X", id, hwnd);
      send(rt, hwnd, WM_TIMER, id, 0);
    }
  }
  return n;
}

StepReport16 user16_end_step(Runtime16& rt, uint32_t keep_steps) {
  UserState& s = us(rt);
  StepReport16 r;
  for (const auto& [m, dispatched] : s.removed) {
    if (!dispatched) r.consumed = std::max(r.consumed, m.seq);
  }
  if (r.consumed) trace("input16", "input up to seq %llu consumed from the saver window's queue", (unsigned long long)r.consumed);
  s.removed.clear();
  // Input nobody took within the step it arrived in: the saver window had it,
  // unless the lane keeps it a while longer for a suspended DRAWFRAME that may
  // still take it (keep_steps more steps; input16.hh).
  size_t before = s.input.size();
  for (Msg16& m : s.input) {
    if (m.seq) m.age++;
  }
  s.input.erase(std::remove_if(s.input.begin(), s.input.end(),
                               [&](const Msg16& m) { return m.seq != 0 && m.age > keep_steps; }),
                s.input.end());
  r.dropped = uint32_t(before - s.input.size());
  for (const Msg16& m : s.input) {
    if (m.seq && (!r.pending || m.seq < r.pending)) r.pending = m.seq;
  }
  r.queue_reads = s.queue_reads;
  r.wake = s.wake;
  r.task_posts = s.task_posts;
  r.last_task_msg = s.last_task_msg;
  return r;
}

void register_user16(Runtime16& rt) {
  Shim16Registry& r = rt.shims();

  // ---- time and input ----
  r.impl(U, "GetTickCount", [](Call16& c) { c.ret32(c.rt.tick_count()); });
  r.impl(U, "GetCurrentTime", [](Call16& c) { c.ret32(c.rt.tick_count()); });
  r.impl(U, "GetMessageTime", [](Call16& c) { c.ret32(us(c).last_msg_time); });
  r.impl(U, "GetKeyState", [](Call16& c) { c.ret(key_state(c, c.sw(), false)); });
  r.impl(U, "GetAsyncKeyState", [](Call16& c) { c.ret(key_state(c, c.sw(), true)); });
  r.impl(U, "GetCursorPos", [](Call16& c) {
    uint32_t p = c.ptr();
    const InputState& in = c.rt.input();
    POINT16 pt = in.mouse_seen ? POINT16{int16_t(in.mouse_x), int16_t(in.mouse_y)}
                               : POINT16{int16_t(screen_w(c.rt) / 2), int16_t(screen_h(c.rt) / 2)};
    write16(c.rt, p, pt);
  });
  r.impl(U, "GetMessagePos", [](Call16& c) {
    const InputState& in = c.rt.input();
    c.ret32((uint32_t(uint16_t(in.mouse_y)) << 16) | uint16_t(in.mouse_x));
  });
  // The cursor is emulated state only, never the host's: SetCursor returns
  // the cursor before it, ShowCursor the display count, ClipCursor keeps the
  // rectangle GetClipCursor reports (NULL: the whole screen).
  r.impl(U, "SetCursor", [](Call16& c) {
    UserState& s = us(c);
    uint16_t old = s.cursor;
    s.cursor = c.w();
    c.ret(old);
  });
  r.impl(U, "ShowCursor", [](Call16& c) {
    UserState& s = us(c);
    s.cursor_count += c.w() ? 1 : -1;
    c.ret(uint16_t(int16_t(s.cursor_count)));
  });
  r.impl(U, "ClipCursor", [](Call16& c) {
    uint32_t p = c.ptr();
    UserState& s = us(c);
    s.clipped = p != 0;
    if (p) s.clip = rrect(c, p);
    trace("user16", "ClipCursor(%s): emulated state only", p ? "a rectangle" : "NULL");
  });
  r.impl(U, "GetClipCursor", [](Call16& c) {
    uint32_t p = c.ptr();
    UserState& s = us(c);
    wrect(c, p, s.clipped ? s.clip : RECT16{0, 0, int16_t(screen_w(c.rt)), int16_t(screen_h(c.rt))});
  });
  // InitApp(hInstance): the task's message queue (USER made it here): there.
  r.impl(U, "InitApp", [](Call16& c) {
    c.w();
    c.ret(1);
  });
  r.impl(U, "LoadCursor", [](Call16& c) {
    c.w();
    c.ptr();
    c.ret(kSystemCursor);  // a cursor handle nobody draws
  });
  r.impl(U, "DestroyCursor", [](Call16& c) {
    c.w();
    c.ret(1);
  });
  // ---- icons (see Icon16) ----
  // LoadIcon(hinst, name): the module's RT_GROUP_ICON; a system icon (hinst
  // 0), or a name the module lacks, is the image-less handle it always was.
  // A resource icon is loaded once: every LoadIcon of it returns the same
  // handle, as Win16 did, so a module that asks per frame or per dialog does
  // not use up the handle range (4096) or memory.
  r.impl(U, "LoadIcon", [](Call16& c) {
    uint16_t hinst = c.w();
    uint32_t name = c.ptr();
    Module16* m = hinst ? c.rt.modules().by_handle(hinst) : nullptr;
    if (m && m->image) {
      loader::ResId id = res_id(c.rt, name);
      UserState& s = us(c);
      auto key = std::make_pair(hinst, id.is_string ? upper16(id.str) : "#" + std::to_string(id.num));
      auto it = s.loaded_icons.find(key);
      if (it != s.loaded_icons.end()) {
        if (it->second.image.lock() == m->image && find_icon(s, it->second.handle)) return c.ret(it->second.handle);
        s.icons.erase(it->second.handle);  // made for a module since freed
        s.loaded_icons.erase(it);
      }
      const loader::ne::Resource* g =
          c.rt.modules().find_resource(m, loader::ResId::of(uint16_t(loader::rt::group_icon)), id);
      Icon16 ic;
      if (g && load_group_icon(*m->image, *g, &ic)) {
        ic.shared = true;
        if (uint16_t h = add_icon(s, std::move(ic))) {
          s.loaded_icons[key] = {h, m->image};
          return c.ret(h);
        }
      }
    }
    c.ret(kSystemIcon);
  });
  r.impl(U, "CopyIcon", [](Call16& c) {
    c.w();
    uint16_t h = c.w();
    Icon16* ic = find_icon(us(c), h);
    if (!ic) return c.ret(h);  // the image-less system icon copies as itself
    Icon16 copy = *ic;
    copy.shared = false;  // a copy is the caller's own
    c.ret(add_icon(us(c), std::move(copy)));
  });
  r.impl(U, "DestroyIcon", [](Call16& c) {
    UserState& s = us(c);
    uint16_t h = c.w();
    Icon16* ic = find_icon(s, h);
    if (ic && !ic->shared) s.icons.erase(h);  // a shared resource icon stays
    c.ret(1);
  });
  // CreateIcon(hinst, w, h, planes, bitsPixel, lpANDbits, lpXORbits): device-
  // dependent bits, WORD-aligned top-down rows; a multi-plane image carries
  // each plane's row in turn. 8-bit indices are hardware palette entries.
  r.impl(U, "CreateIcon", [](Call16& c) {
    c.w();
    int16_t w = c.sw(), h = c.sw();
    uint8_t planes = uint8_t(c.w()), bpp = uint8_t(c.w());
    uint32_t and_bits = c.ptr(), xor_bits = c.ptr();
    if (w <= 0 || h <= 0 || w > 256 || h > 256 || !planes || !bpp || !and_bits || !xor_bits ||
        (bpp != 1 && bpp != 4 && bpp != 8) || (planes > 1 && bpp != 1) || planes > 4) {
      return c.ret(0);
    }
    Icon16 ic;
    ic.w = w;
    ic.h = h;
    ic.mask.assign(size_t(w) * size_t(h), 1);
    ic.color.assign(size_t(w) * size_t(h), 0);
    uint32_t and_stride = uint32_t(((w + 15) / 16) * 2);
    uint32_t plane_stride = uint32_t(((w * bpp + 15) / 16) * 2);
    static const COLORREF kVga[16] = {
        RGB(0, 0, 0),     RGB(128, 0, 0),   RGB(0, 128, 0),   RGB(128, 128, 0), RGB(0, 0, 128),   RGB(128, 0, 128),
        RGB(0, 128, 128), RGB(192, 192, 192), RGB(128, 128, 128), RGB(255, 0, 0), RGB(0, 255, 0), RGB(255, 255, 0),
        RGB(0, 0, 255),   RGB(255, 0, 255), RGB(0, 255, 255), RGB(255, 255, 255)};
    Gdi16& g = c.rt.state<Gdi16>();
    for (int y = 0; y < h; y++) {
      std::vector<uint8_t> arow(and_stride), xrow(size_t(plane_stride) * planes);
      c.rt.read_bytes(Runtime16::huge_add(and_bits, uint32_t(y) * and_stride), arow.data(), arow.size());
      c.rt.read_bytes(Runtime16::huge_add(xor_bits, uint32_t(y) * plane_stride * planes), xrow.data(), xrow.size());
      for (int x = 0; x < w; x++) {
        size_t i = size_t(y) * size_t(w) + size_t(x);
        ic.mask[i] = uint8_t((arow[size_t(x) / 8] >> (7 - x % 8)) & 1);
        int v = 0;
        if (bpp == 8) {
          ic.color[i] = g.index_rgb(xrow[size_t(x)]);
          continue;
        }
        if (bpp == 4) {
          v = (xrow[size_t(x) / 2] >> (x % 2 ? 0 : 4)) & 0xF;
        } else {
          for (int p = 0; p < planes; p++) v |= ((xrow[size_t(p) * plane_stride + size_t(x) / 8] >> (7 - x % 8)) & 1) << p;
        }
        ic.color[i] = planes == 1 && bpp == 1 ? (v ? RGB(255, 255, 255) : RGB(0, 0, 0)) : kVga[v & 0xF];
      }
    }
    c.ret(add_icon(us(c), std::move(ic)));
  });
  // DrawIcon(hdc, x, y, hicon): where the mask is clear the icon's colour, as
  // the DC's palette draws it; elsewhere what was there.
  r.impl(U, "DrawIcon", [](Call16& c) {
    uint16_t hdc = c.w();
    int16_t x = c.sw(), y = c.sw();
    uint16_t hi = c.w();
    Icon16* ic = find_icon(us(c), hi);
    Gdi16& g = c.rt.state<Gdi16>();
    HDC d = g.host_dc(hdc);
    if (!ic || !d || !g.dc_surface(hdc)) return c.ret(1);
    uint8_t* bits = nullptr;
    uint32_t stride = 0;
    HDC s = g.scratch(ic->w, ic->h, &bits, &stride);
    if (!s) return c.ret(1);
    BitBlt(s, 0, 0, ic->w, ic->h, d, x, y, SRCCOPY);
    GdiFlush();
    for (int yy = 0; yy < ic->h; yy++) {
      for (int xx = 0; xx < ic->w; xx++) {
        size_t i = size_t(yy) * size_t(ic->w) + size_t(xx);
        if (!ic->mask[i]) bits[size_t(yy) * stride + size_t(xx)] = GetRValue(g.key(hdc, ic->color[i]));
      }
    }
    BitBlt(d, x, y, ic->w, ic->h, s, 0, 0, SRCCOPY);
    c.ret(1);
  });
  // ExtractIcon(hinst, lpszExeFileName, nIconIndex): the index-th group icon
  // of a Win16 module on the guest disk (0xFFFF: how many there are); 0
  // when the file has none or is no NE module.
  r.impl("SHELL", "ExtractIcon", [](Call16& c) {
    c.w();
    std::string file = c.rt.read_str(c.ptr());
    uint16_t index = c.w();
    std::string host = c.rt.vfs().to_host(c.rt.vfs().full_path(file));
    if (host.empty()) return c.ret(0);
    try {
      loader::ne::Image img = loader::ne::Image::from_file(host);
      std::vector<const loader::ne::Resource*> groups;
      for (const loader::ne::Resource& res : img.resources()) {
        if (!res.type.is_string && res.type.num == loader::rt::group_icon) groups.push_back(&res);
      }
      if (index == 0xFFFF) return c.ret(uint16_t(groups.size()));
      Icon16 ic;
      if (index < groups.size() && load_group_icon(img, *groups[index], &ic)) return c.ret(add_icon(us(c), std::move(ic)));
    } catch (const std::exception&) {
    }
    c.ret(0);
  });
  r.impl(U, "MessageBeep", [](Call16&) {});

  // ---- windows ----
  r.impl(U, "GetDesktopWindow", [](Call16& c) { c.ret(us(c).desktop); });
  r.impl(U, "GetActiveWindow", [](Call16& c) { c.ret(us(c).active); });
  r.impl(U, "SetActiveWindow", [](Call16& c) {
    uint16_t h = c.w();
    uint16_t old = us(c).active;
    us(c).active = h;
    c.ret(old);
  });
  r.impl(U, "GetFocus", [](Call16& c) { c.ret(us(c).focus); });
  r.impl(U, "SetFocus", [](Call16& c) {
    uint16_t h = c.w();
    uint16_t old = us(c).focus;
    us(c).focus = h;
    c.ret(old);
  });
  r.impl(U, "GetCapture", [](Call16& c) { c.ret(us(c).capture); });
  // Mouse capture is bookkeeping only (input is polled); EcoLogic releases
  // it when its countdown ends.
  r.impl(U, "SetCapture", [](Call16& c) {
    uint16_t h = c.w();
    uint16_t old = us(c).capture;
    us(c).capture = h;
    c.ret(old);
  });
  r.impl(U, "ReleaseCapture", [](Call16& c) {
    us(c).capture = 0;
    c.ret(0);
  });
  r.impl(U, "IsWindow", [](Call16& c) { c.ret_bool(wnd(c, c.w()) != nullptr); });
  r.impl(U, "IsWindowVisible", [](Call16& c) {
    Wnd16* w = wnd(c, c.w());
    c.ret_bool(w && w->visible);
  });
  r.impl(U, "IsWindowEnabled", [](Call16& c) {
    Wnd16* w = wnd(c, c.w());
    c.ret_bool(w && w->enabled);
  });
  r.impl(U, "IsIconic", [](Call16& c) {
    c.w();
    c.ret(0);
  });
  r.impl(U, "GetParent", [](Call16& c) {
    Wnd16* w = wnd(c, c.w());
    c.ret(w ? w->parent : 0);
  });
  r.impl(U, "GetClientRect", [](Call16& c) {
    uint16_t h = c.w();
    uint32_t p = c.ptr();
    Wnd16* w = wnd(c, h);
    RECT16 out{0, 0, 0, 0};
    if (w) out = RECT16{0, 0, int16_t(w->rect.right - w->rect.left), int16_t(w->rect.bottom - w->rect.top)};
    wrect(c, p, out);
  });
  r.impl(U, "GetWindowRect", [](Call16& c) {
    uint16_t h = c.w();
    uint32_t p = c.ptr();
    Wnd16* w = wnd(c, h);
    wrect(c, p, w ? to_rect16(w->rect) : RECT16{0, 0, 0, 0});
  });
  r.impl(U, "ClientToScreen", [](Call16& c) {
    uint16_t h = c.w();
    uint32_t p = c.ptr();
    Wnd16* w = wnd(c, h);
    POINT16 pt = read16<POINT16>(c.rt, p);
    if (w) {
      pt.x = int16_t(pt.x + w->rect.left);
      pt.y = int16_t(pt.y + w->rect.top);
    }
    write16(c.rt, p, pt);
  });
  r.impl(U, "ScreenToClient", [](Call16& c) {
    uint16_t h = c.w();
    uint32_t p = c.ptr();
    Wnd16* w = wnd(c, h);
    POINT16 pt = read16<POINT16>(c.rt, p);
    if (w) {
      pt.x = int16_t(pt.x - w->rect.left);
      pt.y = int16_t(pt.y - w->rect.top);
    }
    write16(c.rt, p, pt);
  });
  r.impl(U, "GetDC", [](Call16& c) {
    uint16_t h = c.w();
    Gdi16& g = c.rt.state<Gdi16>();
    uint16_t dc = g.create_screen_dc(h);
    Wnd16* w = wnd(c, h);
    if (dc && w && (w->rect.left || w->rect.top)) SetViewportOrgEx(g.host_dc(dc), w->rect.left, w->rect.top, nullptr);
    // The DC origin GetDCOrg reports: the window's corner on the screen.
    if (Dc16* d = g.dc(dc)) {
      d->org_x = w ? int16_t(w->rect.left) : 0;
      d->org_y = w ? int16_t(w->rect.top) : 0;
    }
    c.ret(dc);
  });
  r.impl(U, "ReleaseDC", [](Call16& c) {
    c.w();
    c.rt.state<Gdi16>().release_dc(c.w());
    c.ret(1);
  });
  r.impl(U, "BeginPaint", [](Call16& c) {
    uint16_t h = c.w();
    uint32_t ps = c.ptr();
    Wnd16* w = wnd(c, h);
    uint16_t dc = c.rt.state<Gdi16>().create_screen_dc(h);
    // PAINTSTRUCT: hdc, fErase, rcPaint, fRestore, fIncUpdate, rgbReserved[16] (32 bytes).
    uint8_t zero[32] = {};
    c.rt.write_bytes(ps, zero, sizeof(zero));
    c.rt.wr16(ps, dc);
    uint16_t must_erase = 1;
    if (w && w->erase && us(c).app_task) {
      // An application task's window: WM_ERASEBKGND with the DC first, as
      // BeginPaint sent it; fErase says whether the window left it undone.
      w->erase = false;
      must_erase = send(c.rt, h, WM_ERASEBKGND, dc, 0) ? 0 : 1;
      w = wnd(c, h);
    }
    c.rt.wr16(ps + 2, must_erase);
    RECT16 rc{0, 0, 0, 0};
    if (w) rc = RECT16{0, 0, int16_t(w->rect.right - w->rect.left), int16_t(w->rect.bottom - w->rect.top)};
    write16(c.rt, ps + 4, rc);
    if (w) w->invalid = false;
    c.ret(dc);
  });
  r.impl(U, "EndPaint", [](Call16& c) {
    c.w();
    uint32_t ps = c.ptr();
    c.rt.state<Gdi16>().release_dc(c.rt.rd16(ps));
  });
  r.impl(U, "InvalidateRect", [](Call16& c) {
    Wnd16* w = wnd(c, c.w());
    c.ptr();
    const bool erase = c.w() != 0;
    if (w) w->invalid = true;
    if (w && erase) w->erase = true;
  });
  r.impl(U, "ValidateRect", [](Call16& c) {
    Wnd16* w = wnd(c, c.w());
    if (w) w->invalid = false;
  });
  r.impl(U, "UpdateWindow", [](Call16& c) {
    uint16_t h = c.w();
    Wnd16* w = wnd(c, h);
    if (w && w->invalid && w->proc) send(c.rt, h, WM_PAINT, 0, 0);
  });
  r.impl(U, "GetSystemMetrics", [](Call16& c) { c.ret(sys_metric(c.rt, c.sw())); });
  r.impl(U, "GetSysColor", [](Call16& c) { c.ret32(sys_color(c.sw())); });
  r.impl(U, "SystemParametersInfo", [](Call16& c) { c.ret(0); });
  // Percent of the USER/GDI heaps free: plenty (modules refuse to start below a threshold).
  r.impl(U, "GetFreeSystemResources", [](Call16& c) {
    c.w();
    c.ret(90);
  });
  r.impl(U, "RegisterClass", [](Call16& c) {
    uint32_t wc = c.ptr();
    // WNDCLASS16: style, lpfnWndProc (far), cbClsExtra, cbWndExtra, hInstance,
    // hIcon, hCursor, hbrBackground, lpszMenuName (far), lpszClassName (far).
    Class16 k;
    k.style = c.rt.rd16(wc);
    k.proc = c.rt.rd32(wc + 2);
    k.cls_extra = int16_t(c.rt.rd16(wc + 6));
    k.wnd_extra = int16_t(c.rt.rd16(wc + 8));
    k.hinst = c.rt.rd16(wc + 10);
    k.icon = c.rt.rd16(wc + 12);
    k.cursor = c.rt.rd16(wc + 14);
    k.background = c.rt.rd16(wc + 16);
    uint32_t name = c.rt.rd32(wc + 22);
    k.name = (name >> 16) ? c.rt.read_str(name) : "#" + std::to_string(name & 0xFFFF);
    UserState& s = us(c);
    k.atom = s.next_atom++;
    s.classes[upper16(k.name)] = k;
    c.ret(k.atom);
  });
  r.impl(U, "UnregisterClass", [](Call16& c) {
    uint32_t name = c.ptr();
    c.w();
    std::string n = (name >> 16) ? c.rt.read_str(name) : "#" + std::to_string(name & 0xFFFF);
    c.ret_bool(us(c).classes.erase(upper16(n)) != 0);
  });
  r.impl(U, "GetClassInfo", [](Call16& c) {
    c.w();
    uint32_t name = c.ptr(), out = c.ptr();
    std::string n = (name >> 16) ? c.rt.read_str(name) : "#" + std::to_string(name & 0xFFFF);
    auto it = us(c).classes.find(upper16(n));
    if (it == us(c).classes.end()) return c.ret(0);
    uint8_t zero[26] = {};
    c.rt.write_bytes(out, zero, sizeof(zero));
    c.rt.wr16(out, it->second.style);
    c.rt.wr32(out + 2, it->second.proc);
    c.rt.wr16(out + 6, uint16_t(it->second.cls_extra));
    c.rt.wr16(out + 8, uint16_t(it->second.wnd_extra));
    c.rt.wr16(out + 10, it->second.hinst);
    c.rt.wr16(out + 12, it->second.icon);
    c.rt.wr16(out + 14, it->second.cursor);
    c.rt.wr16(out + 16, it->second.background);
    c.rt.wr32(out + 22, name);
    c.ret(1);
  });
  auto create_window = [](Call16& c, uint32_t exstyle) {
    uint32_t cls = c.ptr(), title = c.ptr();
    uint32_t style = c.l();
    int16_t x = c.sw(), y = c.sw(), w = c.sw(), h = c.sw();
    uint16_t parent = c.w(), menu = c.w(), hinst = c.w();
    uint32_t param = c.ptr();
    UserState& s = us(c);
    std::string cn = (cls >> 16) ? c.rt.read_str(cls) : "#" + std::to_string(cls & 0xFFFF);
    Wnd16 wn;
    wn.cls = cn;
    wn.title = c.rt.read_str(title);
    wn.style = style;
    wn.exstyle = exstyle;
    wn.parent = parent;
    wn.hinst = hinst;
    wn.id = menu;
    wn.visible = style & WS_VISIBLE;
    if (x == int16_t(CW_USEDEFAULT)) x = 0, y = 0;
    if (w == int16_t(CW_USEDEFAULT)) w = int16_t(screen_w(c.rt)), h = int16_t(screen_h(c.rt));
    RECT pr{0, 0, 0, 0};
    if (Wnd16* p = wnd(c, parent); p && (style & WS_CHILD)) pr = p->rect;
    wn.rect = RECT{pr.left + x, pr.top + y, pr.left + x + w, pr.top + y + h};
    auto k = s.classes.find(upper16(cn));
    if (k != s.classes.end()) wn.proc = k->second.proc;
    uint16_t hwnd = s.next_hwnd;
    s.next_hwnd += 4;
    s.windows[hwnd] = wn;
    trace("user16", "CreateWindow(\"%s\", \"%s\") -> %04X", cn.c_str(), wn.title.c_str(), hwnd);
    if (wn.proc) {
      // WM_CREATE with a CREATESTRUCT in a scratch block: lpCreateParams,
      // hInstance, hMenu, hwndParent, cy, cx, y, x, style, lpszName,
      // lpszClass, dwExStyle — 34 bytes.
      uint16_t hb = c.rt.global().alloc(0, 34);
      uint32_t cs = c.rt.global().lock(hb);
      if (cs) {
        c.rt.wr32(cs, param);
        c.rt.wr16(cs + 4, hinst);
        c.rt.wr16(cs + 6, menu);
        c.rt.wr16(cs + 8, parent);
        c.rt.wr16(cs + 10, uint16_t(h));
        c.rt.wr16(cs + 12, uint16_t(w));
        c.rt.wr16(cs + 14, uint16_t(y));
        c.rt.wr16(cs + 16, uint16_t(x));
        c.rt.wr32(cs + 18, style);
        c.rt.wr32(cs + 22, title);
        c.rt.wr32(cs + 26, cls);
        c.rt.wr32(cs + 30, exstyle);
      }
      uint32_t r = call_wndproc(c.rt, wn.proc, hwnd, WM_CREATE, 0, cs);
      c.rt.global().free(hb);
      if (int16_t(r) == -1) {
        s.windows.erase(hwnd);
        return c.ret(0);
      }
      // An application task's window gets what Windows' CreateWindow sent
      // after WM_CREATE — WM_SIZE (SIZE_RESTORED) and WM_MOVE — and, shown,
      // is all invalid: GetMessage brings its WM_PAINT, BeginPaint its
      // WM_ERASEBKGND (SCRANTIC centres its scene by the WM_SIZE it got).
      if (s.app_task && s.windows.count(hwnd)) {
        call_wndproc(c.rt, wn.proc, hwnd, WM_SIZE, SIZE_RESTORED, (uint32_t(uint16_t(h)) << 16) | uint16_t(w));
        if (s.windows.count(hwnd)) call_wndproc(c.rt, wn.proc, hwnd, WM_MOVE, 0, (uint32_t(uint16_t(y)) << 16) | uint16_t(x));
        if (Wnd16* n = wnd(c, hwnd); n && n->visible) n->invalid = n->erase = true;
      }
    }
    c.ret(hwnd);
  };
  r.impl(U, "CreateWindow", [create_window](Call16& c) { create_window(c, 0); });
  r.impl(U, "CreateWindowEx", [create_window](Call16& c) {
    uint32_t ex = c.l();
    create_window(c, ex);
  });
  r.impl(U, "DestroyWindow", [](Call16& c) { c.ret_bool(destroy_window(c.rt, c.w())); });
  r.impl(U, "ShowWindow", [](Call16& c) {
    uint16_t h = c.w();
    int16_t cmd = c.sw();
    Wnd16* w = wnd(c, h);
    bool was = w && w->visible;
    if (w) w->visible = cmd != SW_HIDE;
    c.ret_bool(was);
  });
  r.impl(U, "MoveWindow", [](Call16& c) {
    uint16_t h = c.w();
    int16_t x = c.sw(), y = c.sw(), w = c.sw(), hh = c.sw();
    c.w();
    if (Wnd16* wn = wnd(c, h)) wn->rect = RECT{x, y, x + w, y + hh};
    c.ret(1);
  });
  r.impl(U, "SetWindowPos", [](Call16& c) { c.ret(1); });
  r.impl(U, "BringWindowToTop", [](Call16& c) { c.ret(1); });
  r.impl(U, "EnableWindow", [](Call16& c) {
    uint16_t h = c.w(), en = c.w();
    Wnd16* w = wnd(c, h);
    bool was = w && !w->enabled;
    if (w) w->enabled = en != 0;
    c.ret_bool(was);
  });
  r.impl(U, "GetWindowText", [](Call16& c) {
    uint16_t h = c.w();
    uint32_t buf = c.ptr();
    int16_t n = c.sw();
    Wnd16* w = wnd(c, h);
    c.ret(w && n > 0 ? uint16_t(c.rt.write_str(buf, w->title, size_t(n))) : 0);
  });
  r.impl(U, "GetWindowTextLength", [](Call16& c) {
    Wnd16* w = wnd(c, c.w());
    c.ret(w ? uint16_t(w->title.size()) : 0);
  });
  r.impl(U, "SetWindowText", [](Call16& c) {
    uint16_t h = c.w();
    std::string t = c.rt.read_str(c.ptr());
    if (Wnd16* w = wnd(c, h)) w->title = t;
  });
  r.impl(U, "GetClassName", [](Call16& c) {
    uint16_t h = c.w();
    uint32_t buf = c.ptr();
    int16_t n = c.sw();
    Wnd16* w = wnd(c, h);
    c.ret(w && n > 0 ? uint16_t(c.rt.write_str(buf, w->cls, size_t(n))) : 0);
  });
  r.impl(U, "GetDlgCtrlID", [](Call16& c) {
    Wnd16* w = wnd(c, c.w());
    c.ret(w ? w->id : 0);
  });
  r.impl(U, "GetWindowWord", [](Call16& c) {
    uint16_t h = c.w();
    int16_t i = c.sw();
    Wnd16* w = wnd(c, h);
    if (!w) return c.ret(0);
    if (i == -6) return c.ret(w->hinst);   // GWW_HINSTANCE
    if (i == -8) return c.ret(w->parent);  // GWW_HWNDPARENT
    if (i == -12) return c.ret(w->id);     // GWW_ID
    auto it = w->words.find(i);
    c.ret(it == w->words.end() ? 0 : it->second);
  });
  r.impl(U, "SetWindowWord", [](Call16& c) {
    uint16_t h = c.w();
    int16_t i = c.sw();
    uint16_t v = c.w();
    Wnd16* w = wnd(c, h);
    if (!w) return c.ret(0);
    uint16_t old = w->words[i];
    w->words[i] = v;
    c.ret(old);
  });
  r.impl(U, "GetWindowLong", [](Call16& c) {
    uint16_t h = c.w();
    int16_t i = c.sw();
    Wnd16* w = wnd(c, h);
    if (!w) return c.ret32(0);
    if (i == kGwlWndProc) return c.ret32(w->proc);
    if (i == GWL_STYLE) return c.ret32(w->style);
    if (i == GWL_EXSTYLE) return c.ret32(w->exstyle);
    uint32_t lo = w->words.count(i) ? w->words[i] : 0, hi = w->words.count(int16_t(i + 2)) ? w->words[int16_t(i + 2)] : 0;
    c.ret32(lo | (hi << 16));
  });
  r.impl(U, "SetWindowLong", [](Call16& c) {
    uint16_t h = c.w();
    int16_t i = c.sw();
    uint32_t v = c.l();
    Wnd16* w = wnd(c, h);
    if (!w) return c.ret32(0);
    uint32_t old = 0;
    if (i == kGwlWndProc) {
      old = w->proc;
      w->proc = v;
    } else if (i == GWL_STYLE) {
      old = w->style;
      w->style = v;
    } else if (i == GWL_EXSTYLE) {
      old = w->exstyle;
      w->exstyle = v;
    } else {
      old = uint32_t(w->words[i]) | (uint32_t(w->words[int16_t(i + 2)]) << 16);
      w->words[i] = uint16_t(v);
      w->words[int16_t(i + 2)] = uint16_t(v >> 16);
    }
    c.ret32(old);
  });
  r.impl(U, "SetProp", [](Call16& c) {
    uint16_t h = c.w();
    uint32_t name = c.ptr();
    uint16_t v = c.w();
    Wnd16* w = wnd(c, h);
    if (!w) return c.ret(0);
    w->props[upper16((name >> 16) ? c.rt.read_str(name) : "#" + std::to_string(name & 0xFFFF))] = v;
    c.ret(1);
  });
  r.impl(U, "GetProp", [](Call16& c) {
    uint16_t h = c.w();
    uint32_t name = c.ptr();
    Wnd16* w = wnd(c, h);
    if (!w) return c.ret(0);
    auto it = w->props.find(upper16((name >> 16) ? c.rt.read_str(name) : "#" + std::to_string(name & 0xFFFF)));
    c.ret(it == w->props.end() ? 0 : it->second);
  });
  r.impl(U, "RemoveProp", [](Call16& c) {
    uint16_t h = c.w();
    uint32_t name = c.ptr();
    Wnd16* w = wnd(c, h);
    if (!w) return c.ret(0);
    std::string k = upper16((name >> 16) ? c.rt.read_str(name) : "#" + std::to_string(name & 0xFFFF));
    auto it = w->props.find(k);
    uint16_t v = it == w->props.end() ? 0 : it->second;
    if (it != w->props.end()) w->props.erase(it);
    c.ret(v);
  });
  r.impl(U, "FindWindow", [](Call16& c) {
    uint32_t cls = c.ptr(), title = c.ptr();
    std::string cn = cls ? ((cls >> 16) ? c.rt.read_str(cls) : "#" + std::to_string(cls & 0xFFFF)) : "";
    std::string tn = title ? c.rt.read_str(title) : "";
    // The AD 2/3 blanker window (input16.hh): the saver window stands in for it.
    if (cls && ieq16(cn, kBlankerClass16) && !title) {
      uint16_t saver = us(c).saver;
      trace("input16", "FindWindow(\"%s\", NULL) -> the saver window %04X", cn.c_str(), saver);
      return c.ret(saver);
    }
    for (auto& [h, w] : us(c).windows) {
      if ((!cls || ieq16(w.cls, cn)) && (!title || w.title == tn)) return c.ret(h);
    }
    c.ret(0);
  });
  // EnumChildWindows(hwndParent, lpEnumFunc, lParam): BOOL FAR PASCAL
  // f(HWND, LPARAM) for each of the window's descendants (each followed by
  // its own, in Z order) until one returns 0; the desktop's are the
  // top-level windows (the synthetic desktop comes into being, as for
  // EnumWindows) and theirs. JAWAS (1:3B3D) asks for its window's parent's.
  // A handle naming no window, 0 included, enumerates nothing: FALSE.
  r.impl(U, "EnumChildWindows", [](Call16& c) {
    uint16_t h = c.w();
    uint32_t proc = c.ptr();
    uint32_t lp = c.l();
    UserState& s = us(c);
    if (!h || !s.windows.count(h)) return c.ret(0);
    if (h == s.desktop) ensure_desktop(c.rt);
    std::vector<uint16_t> list;
    descendants(s, h, &list);
    for (uint16_t k : list) {
      if (!wnd(c, k)) continue;  // destroyed by an earlier callback
      trace("user16", "EnumChildWindows(%04X) -> %04X:%04X(%04X, %08X)", h, proc >> 16, proc & 0xFFFF, k, lp);
      if (!(c.rt.call_far(proc, {w16(k), l16(lp)}) & 0xFFFF)) break;
    }
    c.ret(1);
  });
  r.impl(U, "EnumTaskWindows", [](Call16& c) { c.ret(1); });
  r.impl(U, "GetTopWindow", [](Call16& c) { c.ret(0); });
  // GetNextWindow(hwnd, GW_HWNDNEXT | GW_HWNDPREV): GetWindow's two
  // sibling steps (JAWAS walks back through the Z order with it, 1:3A24).
  r.impl(U, "GetNextWindow", [](Call16& c) {
    uint16_t h = c.w(), cmd = c.w();
    c.ret(cmd == 2 || cmd == 3 ? window_rel(us(c), h, cmd) : 0);
  });
  // GetMenu(hwnd): the window's menu bar — none on the saver's popup window
  // or a child window; the menu handle a module's own top-level window was
  // created with; the synthetic Program Manager's menu bar (JAWAS asks while
  // it sizes up the windows on the desktop, 1:3DFE).
  r.impl(U, "GetMenu", [](Call16& c) {
    uint16_t h = c.w();
    UserState& s = us(c);
    Wnd16* w = wnd(c, h);
    if (!w) return c.ret(0);
    if (h == s.progman) return c.ret(kProgmanMenu);
    c.ret((w->style & WS_CHILD) ? 0 : w->id);
  });
  // GetWindowTask(hwnd): the task that made the window — this one for the
  // saver window, the module's own windows and (configure mode) the real
  // dialogs, the shell's for the synthetic Program Manager and the desktop
  // (JAWAS's EnumWindows callback skips its own task's windows, 1:3B6D), 0
  // for a handle that names no window.
  r.impl(U, "GetWindowTask", [](Call16& c) {
    uint16_t h = c.w();
    UserState& s = us(c);
    if (h && (h == s.progman || h == s.desktop)) return c.ret(kernel16_shell_task(c.rt));
    if (wnd(c, h) || real_window16(c.rt, h)) return c.ret(kernel16_current_task(c.rt));
    c.ret(0);
  });
  r.impl(U, "GetLastActivePopup", [](Call16& c) { c.ret(c.w()); });
  // GetNextQueueWindow(hwnd, bNext): a stub row in the interface tables; BUGS
  // walks the queue's windows with it. There is no other window to find.
  r.add(U, 274, "GetNextQueueWindow", Conv16::pascal_, true, 4, [](Call16& c) { c.ret(0); });
  // EnumWindows(lpEnumFunc, lParam): BOOL FAR PASCAL f(HWND, LPARAM) for each
  // top-level window in Z order until one returns 0. The first call brings
  // the synthetic desktop into being (ensure_desktop).
  r.impl(U, "EnumWindows", [](Call16& c) {
    uint32_t proc = c.ptr();
    uint32_t lp = c.l();
    ensure_desktop(c.rt);
    for (uint16_t h : z_order(us(c), 0)) {
      if (!wnd(c, h)) continue;  // destroyed by an earlier callback
      trace("user16", "EnumWindows -> %04X:%04X(%04X, %08X)", proc >> 16, proc & 0xFFFF, h, lp);
      if (!(c.rt.call_far(proc, {w16(h), l16(lp)}) & 0xFFFF)) break;
    }
    c.ret(1);
  });
  // GetWindow(hwnd, cmd): window_rel.
  r.impl(U, "GetWindow", [](Call16& c) {
    uint16_t h = c.w(), cmd = c.w();
    c.ret(window_rel(us(c), h, cmd));
  });
  // GetClassWord(hwnd, index): the window class's GCW_* fields.
  r.impl(U, "GetClassWord", [](Call16& c) {
    uint16_t h = c.w();
    int16_t i = c.sw();
    Wnd16* w = wnd(c, h);
    if (!w) return c.ret(0);
    auto it = us(c).classes.find(upper16(w->cls));
    if (it == us(c).classes.end()) return c.ret(0);
    const Class16& k = it->second;
    switch (i) {
      case -14:  // GCW_HICON
        return c.ret(k.icon);
      case -12:  // GCW_HCURSOR
        return c.ret(k.cursor);
      case -10:  // GCW_HBRBACKGROUND
        return c.ret(k.background);
      case -16:  // GCW_HMODULE
        return c.ret(k.hinst);
      case -18:  // GCW_CBWNDEXTRA
        return c.ret(uint16_t(k.wnd_extra));
      case -20:  // GCW_CBCLSEXTRA
        return c.ret(uint16_t(k.cls_extra));
      case -26:  // GCW_STYLE
        return c.ret(k.style);
      case -32:  // GCW_ATOM
        return c.ret(k.atom);
      default:
        return c.ret(0);
    }
  });
  r.impl(U, "GetWindowPlacement", [](Call16& c) {
    uint16_t h = c.w();
    uint32_t p = c.ptr();
    Wnd16* w = wnd(c, h);
    // WINDOWPLACEMENT16 (22 bytes): length, flags, showCmd, ptMin, ptMax, rcNormal.
    uint8_t buf[22] = {};
    buf[0] = 22;
    buf[4] = SW_SHOWNORMAL;
    RECT16 rc = w ? to_rect16(w->rect) : RECT16{0, 0, 0, 0};
    memcpy(buf + 14, &rc, 8);
    c.rt.write_bytes(p, buf, sizeof(buf));
    c.ret(1);
  });
  r.impl(U, "AdjustWindowRect", [](Call16&) {});

  // ---- messages ----
  r.impl(U, "DefWindowProc", [](Call16& c) {
    uint16_t h = c.w();
    uint16_t msg = c.w();
    // An application task's window (user16_set_app_task): WM_CLOSE destroys
    // it and WM_PAINT validates it, as Windows' DefWindowProc did.
    if (us(c).app_task && msg == WM_CLOSE) {
      destroy_window(c.rt, h);
      return c.ret32(0);
    }
    if (us(c).app_task && msg == WM_PAINT) {
      if (Wnd16* w = wnd(c, h)) w->invalid = w->erase = false;
      return c.ret32(0);
    }
    c.ret32(msg == WM_ERASEBKGND ? 1 : 0);
  });
  r.impl(U, "CallWindowProc", [](Call16& c) {
    uint32_t proc = c.ptr();
    uint16_t h = c.w(), msg = c.w(), wp = c.w();
    uint32_t lp = c.l();
    uint32_t v = call_wndproc(c.rt, proc, h, msg, wp, lp);
    c.ret32(v);
  });
  r.impl(U, "SendMessage", [](Call16& c) {
    uint16_t h = c.w(), msg = c.w(), wp = c.w();
    uint32_t lp = c.l();
    uint32_t v = send(c.rt, h, msg, wp, lp);
    c.ret32(v);
  });
  r.impl(U, "PostMessage", [](Call16& c) {
    uint16_t h = c.w(), msg = c.w(), wp = c.w();
    uint32_t lp = c.l();
    UserState& s = us(c);
    // A module asking the blanker to close (INTERACTION.md §3.4 ADWS_WAKE).
    if (h && h == s.saver && (msg == WM_CLOSE || (msg == WM_SYSCOMMAND && (wp & 0xFFF0) == SC_CLOSE))) {
      if (!s.wake) trace("input16", "the guest posted %s to the saver window: wake", msg == WM_CLOSE ? "WM_CLOSE" : "SC_CLOSE");
      s.wake = true;
    }
    s.queue.push_back({h, msg, wp, lp});
    c.ret(1);
  });
  r.impl(U, "PostAppMessage", [](Call16& c) {
    c.w();
    uint16_t msg = c.w(), wp = c.w();
    uint32_t lp = c.l();
    us(c).queue.push_back({0, msg, wp, lp});
    c.ret(1);
  });
  r.impl(U, "PostQuitMessage", [](Call16& c) { us(c).queue.push_back({0, WM_QUIT, c.w(), 0}); });
  // Takes the next message that passes the filters (hwnd 0 = any; min = max
  // = 0 = any number) — posted messages first, then input messages, then due
  // timers, the order Win16's GetMessage read them in — into MSG16: hwnd,
  // message, wParam, lParam, time, pt (18 bytes).
  auto take = [](Call16& c, uint32_t out, uint16_t hwnd, uint16_t lo, uint16_t hi, bool remove) -> bool {
    UserState& s = us(c);
    auto pass = [&](const Msg16& m) {
      if (hwnd && m.hwnd != hwnd && m.msg != WM_QUIT) return false;
      if (lo == 0 && hi == 0) return true;
      return lo <= hi ? (m.msg >= lo && m.msg <= hi) : (m.msg >= lo || m.msg <= hi);
    };
    // Reading the saver window's queue with removal for keys (input16.hh key-filter).
    if (remove && (hwnd == 0 || hwnd == s.saver) &&
        ((lo == 0 && hi == 0) || (lo <= hi ? (lo <= WM_KEYDOWN && WM_KEYDOWN <= hi) : (WM_KEYDOWN >= lo || WM_KEYDOWN <= hi)))) {
      s.queue_reads++;
    }
    Msg16 m;
    bool have = false;
    for (std::deque<Msg16>* q : {&s.queue, &s.input}) {
      auto it = std::find_if(q->begin(), q->end(), pass);
      if (it == q->end()) continue;
      m = *it;
      if (remove) {
        q->erase(it);
        if (m.host && s.host_posted) s.host_posted--;
        if (m.seq) {
          s.removed.push_back({m, false});
          trace("input16", "input seq %llu (msg %04X, %04X, %08X) removed from the queue", (unsigned long long)m.seq, m.msg,
                m.wparam, m.lparam);
        }
      }
      have = true;
      break;
    }
    if (!have && s.app_task) {
      // An application task's windows to paint, after the input and before
      // the timers, as Win16's GetMessage made WM_PAINT; it stays until the
      // window is validated (BeginPaint, DefWindowProc).
      for (const auto& [h, w] : s.windows) {
        Msg16 pm{h, WM_PAINT, 0, 0};
        if (w.invalid && w.visible && w.proc && pass(pm)) {
          m = pm;
          have = true;
          break;
        }
      }
    }
    if (!have) {
      uint64_t now = c.rt.peek_us();
      for (auto& t : s.timers) {
        Msg16 tm{t.hwnd, WM_TIMER, t.id, t.proc};
        if (now >= t.due_us && pass(tm)) {
          m = tm;
          if (remove) t.due_us = now + uint64_t(std::max<uint32_t>(t.elapse_ms, 1)) * 1000;
          have = true;
          break;
        }
      }
    }
    if (!have) return false;
    s.last_msg_time = c.rt.tick_count();
    const InputState& in = c.rt.input();
    c.rt.wr16(out, m.hwnd);
    c.rt.wr16(out + 2, m.msg);
    c.rt.wr16(out + 4, m.wparam);
    c.rt.wr32(out + 6, m.lparam);
    c.rt.wr32(out + 10, s.last_msg_time);
    // pt: where the cursor was (only input messages ever carry one here).
    c.rt.wr32(out + 14, m.seq ? (uint32_t(uint16_t(in.mouse_y)) << 16) | uint16_t(in.mouse_x) : 0);
    return true;
  };
  r.impl(U, "PeekMessage", [take](Call16& c) {
    uint32_t out = c.ptr();
    uint16_t hwnd = c.w(), lo = c.w(), hi = c.w();
    uint16_t flags = c.w();
    c.ret_bool(take(c, out, hwnd, lo, hi, flags & PM_REMOVE));
  });
  r.impl(U, "GetMessage", [take](Call16& c) {
    uint32_t out = c.ptr();
    uint16_t hwnd = c.w(), lo = c.w(), hi = c.w();
    while (!take(c, out, hwnd, lo, hi, true)) {
      if (!us(c).app_task) {
        // Nothing queued and nothing can arrive while we wait: a WM_NULL.
        uint8_t zero[18] = {};
        c.rt.write_bytes(out, zero, sizeof(zero));
        break;
      }
      // An application task waits for its message (app_wait).
      app_wait(c.rt);
    }
    c.ret_bool(c.rt.rd16(out + 2) != WM_QUIT);
  });
  // WaitMessage: an application task waits until a message is there
  // (app_wait); for anything else it returns at once, as GetMessage's WM_NULL does.
  r.impl(U, "WaitMessage", [](Call16& c) {
    while (us(c).app_task && !app_message_ready(c.rt)) app_wait(c.rt);
  });
  // WM_KEYDOWN/WM_SYSKEYDOWN → WM_CHAR/WM_SYSCHAR ahead of everything else
  // in the input queue, the US layout's character (input16.hh), Shift and
  // Caps Lock from the host's input state.
  r.impl(U, "TranslateMessage", [](Call16& c) {
    uint32_t m = c.ptr();
    uint16_t h = c.rt.rd16(m), msg = c.rt.rd16(m + 2), wp = c.rt.rd16(m + 4);
    uint32_t lp = c.rt.rd32(m + 6);
    if (msg != WM_KEYDOWN && msg != WM_SYSKEYDOWN) return c.ret(0);
    const InputState& in = c.rt.input();
    int ch = vk_to_char(uint8_t(wp), in.keys.test(VK_SHIFT) || in.keys.test(VK_LSHIFT) || in.keys.test(VK_RSHIFT), in.caps);
    if (ch < 0) return c.ret(0);
    UserState& s = us(c);
    // The key's input line, when it was one of ours.
    uint64_t seq = 0;
    for (auto it = s.removed.rbegin(); it != s.removed.rend(); ++it) {
      if (it->first.hwnd == h && it->first.msg == msg && it->first.wparam == wp && it->first.lparam == lp) {
        seq = it->first.seq;
        break;
      }
    }
    s.input.push_front({h, uint16_t(msg == WM_KEYDOWN ? WM_CHAR : WM_SYSCHAR), uint16_t(ch), lp, seq});
    c.ret(1);
  });
  r.impl(U, "DispatchMessage", [](Call16& c) {
    uint32_t m = c.ptr();
    uint16_t h = c.rt.rd16(m), msg = c.rt.rd16(m + 2), wp = c.rt.rd16(m + 4);
    uint32_t lp = c.rt.rd32(m + 6);
    UserState& s = us(c);
    // An input message handed back to the saver window was not consumed.
    if (h && h == s.saver) {
      for (auto& [rm, dispatched] : s.removed) {
        if (!dispatched && rm.hwnd == h && rm.msg == msg && rm.wparam == wp && rm.lparam == lp) {
          dispatched = true;
          trace("input16", "input seq %llu dispatched back to the saver window", (unsigned long long)rm.seq);
          break;
        }
      }
    }
    uint32_t v = 0;
    if (msg == WM_TIMER && lp) v = c.rt.call_far(lp, {w16(h), w16(msg), w16(wp), l16(c.rt.tick_count())});
    else v = send(c.rt, h, msg, wp, lp);
    c.ret32(v);
  });
  r.impl(U, "GetQueueStatus", [](Call16& c) {
    UserState& s = us(c);
    uint16_t qs = s.queue.empty() ? 0 : 0x0008;  // QS_POSTMESSAGE
    for (const Msg16& m : s.input) {
      if (m.msg >= WM_KEYFIRST && m.msg <= WM_KEYLAST) qs |= 0x0001;  // QS_KEY
      else if (m.msg == WM_MOUSEMOVE) qs |= 0x0002;                   // QS_MOUSEMOVE
      else if (m.msg >= WM_MOUSEFIRST && m.msg <= WM_MOUSELAST) qs |= 0x0004;  // QS_MOUSEBUTTON
    }
    c.ret32((uint32_t(qs) << 16) | qs);
  });
  // Key or button input waiting.
  r.impl(U, "GetInputState", [](Call16& c) {
    for (const Msg16& m : us(c).input) {
      if ((m.msg >= WM_KEYFIRST && m.msg <= WM_KEYLAST) || (m.msg > WM_MOUSEMOVE && m.msg <= WM_MOUSELAST)) return c.ret(1);
    }
    c.ret(0);
  });
  r.impl(U, "SetTimer", [](Call16& c) {
    uint16_t h = c.w(), id = c.w(), elapse = c.w();
    uint32_t proc = c.ptr();
    UserState& s = us(c);
    if (!h) id = s.next_timer++;
    s.timers.erase(std::remove_if(s.timers.begin(), s.timers.end(),
                                  [&](const Timer16& t) { return t.hwnd == h && t.id == id; }),
                   s.timers.end());
    s.timers.push_back({h, id, elapse, proc, c.rt.peek_us() + uint64_t(std::max<uint16_t>(elapse, 1)) * 1000});
    c.ret(id ? id : 1);
  });
  r.impl(U, "KillTimer", [](Call16& c) {
    uint16_t h = c.w(), id = c.w();
    auto& t = us(c).timers;
    size_t before = t.size();
    t.erase(std::remove_if(t.begin(), t.end(), [&](const Timer16& x) { return x.hwnd == h && x.id == id; }), t.end());
    c.ret_bool(t.size() != before);
  });

  // ---- hooks (input16.hh): WH_KEYBOARD only; every other kind is refused ----
  // SetWindowsHook(idHook, lpfn) → the token DefHookProc(…, &token) chains
  // with; SetWindowsHookEx(idHook, lpfn, hInstance, hTask) → the HHOOK.
  auto add_hook = [](Call16& c, int16_t type, uint32_t proc) -> uint32_t {
    if (type != WH_KEYBOARD || !proc) {
      trace("input16", "hook type %d refused", type);
      return 0;
    }
    UserState& s = us(c);
    Hook16 h{uint16_t(type), proc, kHookTokenBase | s.next_hook++};
    if (!s.next_hook) s.next_hook = 1;
    s.hooks.push_back(h);
    trace("input16", "WH_KEYBOARD hook %04X:%04X installed (token %08X, %zu in the chain)", proc >> 16, proc & 0xFFFF,
          h.token, s.hooks.size());
    return h.token;
  };
  r.impl(U, "SetWindowsHook", [add_hook](Call16& c) {
    int16_t type = c.sw();
    uint32_t proc = c.ptr();
    c.ret32(add_hook(c, type, proc));
  });
  r.impl(U, "SetWindowsHookEx", [add_hook](Call16& c) {
    int16_t type = c.sw();
    uint32_t proc = c.ptr();
    c.w();  // hInstance
    c.w();  // hTask: the lane has one task
    c.ret32(add_hook(c, type, proc));
  });
  r.impl(U, "UnhookWindowsHook", [](Call16& c) {
    int16_t type = c.sw();
    uint32_t proc = c.ptr();
    auto& hooks = us(c).hooks;
    auto it = std::find_if(hooks.begin(), hooks.end(), [&](const Hook16& h) { return h.type == type && h.proc == proc; });
    if (it == hooks.end()) return c.ret(0);
    trace("input16", "hook %04X:%04X removed", proc >> 16, proc & 0xFFFF);
    hooks.erase(it);
    c.ret(1);
  });
  r.impl(U, "UnhookWindowsHookEx", [](Call16& c) {
    uint32_t token = c.l();
    auto& hooks = us(c).hooks;
    auto it = std::find_if(hooks.begin(), hooks.end(), [&](const Hook16& h) { return h.token == token; });
    if (it == hooks.end()) return c.ret(0);
    trace("input16", "hook %08X removed", token);
    hooks.erase(it);
    c.ret(1);
  });
  // DefHookProc(code, wParam, lParam, lplpfnNextHook) and CallNextHookEx(hhook,
  // code, wParam, lParam): the hook installed before the one `token` names.
  auto call_next = [](Call16& c, uint32_t token, int16_t code, uint16_t wp, uint32_t lp) -> uint32_t {
    auto& hooks = us(c).hooks;
    auto it = std::find_if(hooks.begin(), hooks.end(), [&](const Hook16& h) { return h.token == token; });
    if (it == hooks.end() || it == hooks.begin()) return 0;
    Hook16 next = *std::prev(it);
    trace("input16", "hook %08X chains to %04X:%04X", token, next.proc >> 16, next.proc & 0xFFFF);
    return c.rt.call_far(next.proc, {w16(uint16_t(code)), w16(wp), l16(lp)});
  };
  r.impl(U, "DefHookProc", [call_next](Call16& c) {
    int16_t code = c.sw();
    uint16_t wp = c.w();
    uint32_t lp = c.l();
    uint32_t pp = c.ptr();
    uint32_t token = pp ? c.rt.rd32(pp) : 0;
    uint32_t v = call_next(c, token, code, wp, lp);
    c.ret32(v);
  });
  r.impl(U, "CallNextHookEx", [call_next](Call16& c) {
    uint32_t token = c.l();
    int16_t code = c.sw();
    uint16_t wp = c.w();
    uint32_t lp = c.l();
    uint32_t v = call_next(c, token, code, wp, lp);
    c.ret32(v);
  });
  r.impl(U, "RegisterClipboardFormat", [](Call16& c) { c.ret(0xC000); });
  r.impl(U, "GlobalDeleteAtom", [](Call16& c) { c.ret(0); });
  r.impl(U, "WinHelp", [](Call16& c) { c.ret(0); });
  r.impl(U, "MessageBox", [](Call16& c) {
    c.w();
    std::string text = c.rt.read_str(c.ptr()), cap = c.rt.read_str(c.ptr());
    uint16_t type = c.w();
    log("win16 MessageBox \"%s\": %s", cap.c_str(), text.c_str());
    us(c).last_box = "\"" + cap + "\": " + text;
    static const uint16_t kAnswer[] = {IDOK, IDOK, IDIGNORE, IDNO, IDNO, IDCANCEL};
    c.ret((type & 0xF) < 6 ? kAnswer[type & 0xF] : IDOK);
  });
  // Configuration UI: the saver never shows it.
  r.impl(U, "DialogBox", [](Call16& c) { c.ret(uint16_t(-1)); });
  r.impl(U, "DialogBoxParam", [](Call16& c) { c.ret(uint16_t(-1)); });
  r.impl(U, "EndDialog", [](Call16&) {});

  // ---- rectangles ----
  r.impl(U, "SetRect", [](Call16& c) {
    uint32_t p = c.ptr();
    RECT16 rc{c.sw(), 0, 0, 0};
    rc.top = c.sw();
    rc.right = c.sw();
    rc.bottom = c.sw();
    wrect(c, p, rc);
  });
  r.impl(U, "SetRectEmpty", [](Call16& c) { wrect(c, c.ptr(), RECT16{0, 0, 0, 0}); });
  r.impl(U, "CopyRect", [](Call16& c) {
    uint32_t d = c.ptr(), s = c.ptr();
    wrect(c, d, rrect(c, s));
    c.ret(1);
  });
  r.impl(U, "OffsetRect", [](Call16& c) {
    uint32_t p = c.ptr();
    int16_t dx = c.sw(), dy = c.sw();
    RECT16 rc = rrect(c, p);
    rc.left = int16_t(rc.left + dx);
    rc.right = int16_t(rc.right + dx);
    rc.top = int16_t(rc.top + dy);
    rc.bottom = int16_t(rc.bottom + dy);
    wrect(c, p, rc);
  });
  r.impl(U, "InflateRect", [](Call16& c) {
    uint32_t p = c.ptr();
    int16_t dx = c.sw(), dy = c.sw();
    RECT16 rc = rrect(c, p);
    rc.left = int16_t(rc.left - dx);
    rc.right = int16_t(rc.right + dx);
    rc.top = int16_t(rc.top - dy);
    rc.bottom = int16_t(rc.bottom + dy);
    wrect(c, p, rc);
  });
  r.impl(U, "IntersectRect", [](Call16& c) {
    uint32_t d = c.ptr(), a = c.ptr(), b = c.ptr();
    RECT ra = to_rect(rrect(c, a)), rb = to_rect(rrect(c, b)), out{};
    BOOL ok = ::IntersectRect(&out, &ra, &rb);
    wrect(c, d, to_rect16(out));
    c.ret_bool(ok);
  });
  r.impl(U, "UnionRect", [](Call16& c) {
    uint32_t d = c.ptr(), a = c.ptr(), b = c.ptr();
    RECT ra = to_rect(rrect(c, a)), rb = to_rect(rrect(c, b)), out{};
    BOOL ok = ::UnionRect(&out, &ra, &rb);
    wrect(c, d, to_rect16(out));
    c.ret_bool(ok);
  });
  r.impl(U, "IsRectEmpty", [](Call16& c) {
    RECT16 rc = rrect(c, c.ptr());
    c.ret_bool(rc.right <= rc.left || rc.bottom <= rc.top);
  });
  r.impl(U, "EqualRect", [](Call16& c) {
    RECT16 a = rrect(c, c.ptr()), b = rrect(c, c.ptr());
    c.ret_bool(memcmp(&a, &b, sizeof(a)) == 0);
  });
  r.impl(U, "PtInRect", [](Call16& c) {
    RECT16 rc = rrect(c, c.ptr());
    uint32_t pt = c.l();
    int16_t x = int16_t(pt), y = int16_t(pt >> 16);
    c.ret_bool(x >= rc.left && x < rc.right && y >= rc.top && y < rc.bottom);
  });

  // ---- drawing ----
  auto brush_of = [](Call16& c, uint16_t hbr) -> uint16_t {
    Gdi16& g = c.rt.state<Gdi16>();
    if (g.get(hbr, G16::brush)) return hbr;
    // (HBRUSH)(COLOR_xxx + 1): a system colour brush.
    if (hbr && hbr <= 31) {
      auto& made = us(c).sys_brushes;
      auto it = made.find(hbr);
      if (it != made.end() && g.get(it->second, G16::brush)) return it->second;
      uint16_t b = g.create_brush(sys_color(int16_t(hbr - 1)));
      made[hbr] = b;
      return b;
    }
    return 0;
  };
  r.impl(U, "FillRect", [brush_of](Call16& c) {
    uint16_t hdc = c.w();
    RECT rc = to_rect(rrect(c, c.ptr()));
    uint16_t hbr = brush_of(c, c.w());
    Gdi16& g = c.rt.state<Gdi16>();
    HDC d = g.host_dc(hdc);
    Obj16* b = g.get(hbr, G16::brush);
    Dc16* dc = g.dc(hdc);
    if (!d || !b || !dc) return c.ret(0);
    // FillRect paints with the given brush, not the DC's.
    uint16_t saved = dc->s.brush;
    c.rt.charge_pixels(int64_t(std::max<LONG>(rc.right - rc.left, 0)) * std::max<LONG>(rc.bottom - rc.top, 0));
    g.sync_brush(hdc, hbr);
    ::PatBlt(d, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, PATCOPY);
    g.sync_brush(hdc, saved);
    c.ret(1);
  });
  r.impl(U, "FrameRect", [brush_of](Call16& c) {
    uint16_t hdc = c.w();
    RECT rc = to_rect(rrect(c, c.ptr()));
    uint16_t hbr = brush_of(c, c.w());
    Gdi16& g = c.rt.state<Gdi16>();
    HDC d = g.host_dc(hdc);
    Obj16* b = g.get(hbr, G16::brush);
    if (!d || !b) return c.ret(0);
    HBRUSH real = CreateSolidBrush(g.key(hdc, b->color));
    ::FrameRect(d, &rc, real);
    DeleteObject(real);
    c.ret(1);
  });
  r.impl(U, "InvertRect", [](Call16& c) {
    uint16_t hdc = c.w();
    RECT rc = to_rect(rrect(c, c.ptr()));
    HDC d = c.rt.state<Gdi16>().host_dc(hdc);
    if (d) ::InvertRect(d, &rc);
  });
  r.impl(U, "DrawFocusRect", [](Call16& c) {
    uint16_t hdc = c.w();
    RECT rc = to_rect(rrect(c, c.ptr()));
    HDC d = c.rt.state<Gdi16>().host_dc(hdc);
    if (d) ::DrawFocusRect(d, &rc);
  });
  r.impl(U, "DrawText", [](Call16& c) {
    uint16_t hdc = c.w();
    uint32_t s = c.ptr();
    int16_t n = c.sw();
    uint32_t rp = c.ptr();
    uint16_t fmt = c.w();
    Gdi16& g = c.rt.state<Gdi16>();
    HDC d = g.host_dc(hdc);
    if (!d) return c.ret(0);
    std::string text = n < 0 ? c.rt.read_str(s) : c.rt.read_str(s, size_t(n));
    RECT rc = to_rect(rrect(c, rp));
    g.sync(hdc);
    int h = ::DrawTextA(d, text.data(), int(text.size()), &rc, fmt);
    if (fmt & DT_CALCRECT) wrect(c, rp, to_rect16(rc));
    c.ret(uint16_t(h));
  });
  r.impl(U, "TabbedTextOut", [](Call16& c) {
    uint16_t hdc = c.w();
    int16_t x = c.sw(), y = c.sw();
    uint32_t s = c.ptr();
    int16_t n = c.sw(), ntabs = c.sw();
    uint32_t tabs = c.ptr();
    int16_t origin = c.sw();
    Gdi16& g = c.rt.state<Gdi16>();
    HDC d = g.host_dc(hdc);
    if (!d || n <= 0) return c.ret32(0);
    std::string text = c.rt.read_str(s, size_t(n));
    std::vector<INT> t;
    for (int i = 0; i < ntabs && tabs; i++) t.push_back(int16_t(c.rt.rd16(tabs + 2u * uint32_t(i))));
    g.sync(hdc);
    LONG v = ::TabbedTextOutA(d, x, y, text.data(), int(text.size()), int(t.size()), t.empty() ? nullptr : t.data(), origin);
    c.ret32(uint32_t(v));
  });
  r.impl(U, "ScrollDC", [](Call16& c) {
    uint16_t hdc = c.w();
    int16_t dx = c.sw(), dy = c.sw();
    uint32_t scroll = c.ptr(), clip = c.ptr();
    c.w();
    uint32_t upd = c.ptr();
    HDC d = c.rt.state<Gdi16>().host_dc(hdc);
    if (!d) return c.ret(0);
    RECT rs{}, rc{}, ru{};
    if (scroll) rs = to_rect(rrect(c, scroll));
    if (clip) rc = to_rect(rrect(c, clip));
    BOOL ok = ::ScrollDC(d, dx, dy, scroll ? &rs : nullptr, clip ? &rc : nullptr, nullptr, &ru);
    if (upd) wrect(c, upd, to_rect16(ru));
    c.ret_bool(ok);
  });

  // ---- strings and resources ----
  r.impl(U, "_wsprintf", [](Call16& c) {
    uint32_t buf = c.ptr();
    std::string fmt = c.rt.read_str(c.ptr());
    std::string s = format16(c.rt, fmt, [&c] { return c.w(); });
    c.ret(uint16_t(c.rt.write_str(buf, s, 1024)));
  });
  r.impl(U, "wvsprintf", [](Call16& c) {
    uint32_t buf = c.ptr();
    std::string fmt = c.rt.read_str(c.ptr());
    uint32_t args = c.ptr();
    std::string s = format16(c.rt, fmt, [&c, &args] {
      uint16_t v = c.rt.rd16(args);
      args += 2;
      return v;
    });
    c.ret(uint16_t(c.rt.write_str(buf, s, 1024)));
  });
  r.impl(U, "lstrcmp", [](Call16& c) {
    std::string a = c.rt.read_str(c.ptr()), b = c.rt.read_str(c.ptr());
    c.ret(uint16_t(CompareStringA(MAKELCID(LANG_ENGLISH, SORT_DEFAULT), 0, a.data(), int(a.size()), b.data(), int(b.size())) - 2));
  });
  r.impl(U, "lstrcmpi", [](Call16& c) {
    std::string a = c.rt.read_str(c.ptr()), b = c.rt.read_str(c.ptr());
    c.ret(uint16_t(CompareStringA(MAKELCID(LANG_ENGLISH, SORT_DEFAULT), NORM_IGNORECASE, a.data(), int(a.size()), b.data(),
                                  int(b.size())) - 2));
  });
  auto ansi_case = [&](const char* name, bool up) {
    r.impl(U, name, [up](Call16& c) {
      uint32_t p = c.ptr();
      if (!(p >> 16)) {
        char ch = char(p & 0xFF);
        ch = char(up ? toupper(uint8_t(ch)) : tolower(uint8_t(ch)));
        return c.ret32((p & 0xFFFFFF00) | uint8_t(ch));
      }
      std::string s = c.rt.read_str(p);
      for (char& ch : s) ch = char(up ? toupper(uint8_t(ch)) : tolower(uint8_t(ch)));
      c.rt.write_bytes(p, s.data(), s.size());
      c.ret32(p);
    });
  };
  ansi_case("AnsiUpper", true);
  ansi_case("AnsiLower", false);
  r.impl(U, "AnsiPrev", [](Call16& c) {
    uint32_t start = c.ptr(), cur = c.ptr();
    c.ret32((cur & 0xFFFF) > (start & 0xFFFF) ? cur - 1 : start);
  });
  r.impl(U, "LoadString", [](Call16& c) {
    uint16_t h = c.w(), id = c.w();
    uint32_t buf = c.ptr();
    int16_t n = c.sw();
    std::string s = load_string(c.rt, h, id);
    c.ret(n > 0 ? uint16_t(c.rt.write_str(buf, s, size_t(n))) : 0);
  });
  r.impl(U, "LoadBitmap", [](Call16& c) {
    uint16_t h = c.w();
    uint32_t name = c.ptr();
    if (!h && (name >> 16) == 0) return c.ret(system_bitmap(c.rt, uint16_t(name)));
    Module16* m = c.rt.modules().by_handle(h);
    const loader::ne::Resource* res =
        c.rt.modules().find_resource(m, loader::ResId::of(uint16_t(loader::rt::bitmap)), res_id(c.rt, name));
    if (!res) return c.ret(0);
    std::string_view data = m->image->resource_data(*res);
    uint16_t hb = c.rt.global().alloc(0, uint32_t(data.size()));
    GlobalBlock* b = c.rt.global().find(hb);
    if (!b) return c.ret(0);
    c.rt.mem().memcpy(b->base, data.data(), data.size());
    uint16_t bmp = gdi16_bitmap_from_dib(c.rt, 0, uint32_t(b->sel) << 16);
    c.rt.global().free(hb);
    c.ret(bmp);
  });
}

}  // namespace adw::win16
