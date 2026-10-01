// Unit tests for the interaction work (INTERACTION.md §5.1, §6.6, §7, §8):
// GetAsyncKeyState's bit 0 and the MOUSE button bitmask, the wake flag,
// KERNEL32's files and profiles through the Vfs overlay, the desktop seed
// read from a delete-on-close file another handle keeps open, the
// configure-script parser, its PICK and PRESS, and the real-window handle map. Linked into
// adw_win32_tests.
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "adw/core/text.h"
#include "check.h"
#include "win32/config_script.hh"
#include "win32/display.hh"
#include "win32/ini_store.hh"
#include "win32/realui.hh"
#include "win32/runtime.hh"
#include "win32/shim_families.hh"
#include "win32/vfs.hh"

using namespace adw;
using namespace adw::win32;

namespace {

struct Rt {
  VirtualClock clock{VirtualClock::Mode::fixed_step, 16667};
  InputState input;
  std::unique_ptr<Runtime> rt;
  uint32_t buf = 0;
  Rt() {
    RuntimeOptions o;
    o.heap_size = 16u << 20;
    o.call_budget = 10'000'000;
    rt = std::make_unique<Runtime>(o, clock, &input);
    register_all_shims(rt->shims());
    buf = rt->heap().alloc(4096, true);
  }
  uint32_t call(const char* dll, const char* name, std::initializer_list<uint32_t> args) {
    ShimEntry& e = rt->shims().get(dll, name);
    return rt->call_guest(rt->shims().thunk_address(e), args, e.conv);
  }
  // A guest string at buf + off.
  uint32_t str(uint32_t off, const std::string& s) {
    write_cstr(rt->mem(), buf + off, s, s.size() + 1);
    return buf + off;
  }
};

}  // namespace

// GetAsyncKeyState: bit 15 while down, bit 0 once per press ("pressed since
// the last call"), per VK; the mouse buttons come from the MOUSE bitmask.
TEST(user32_async_key_bit0_and_mouse_buttons) {
  Rt t;
  auto async = [&](uint32_t vk) { return t.call("USER32.DLL", "GetAsyncKeyState", {vk}); };
  auto sync = [&](uint32_t vk) { return t.call("USER32.DLL", "GetKeyState", {vk}); };
  CHECK_EQ(async(VK_SPACE), 0u);
  t.input.keys.set(VK_SPACE);
  CHECK_EQ(async(VK_SPACE), 0xFFFF8001u);  // down, and pressed since the last call
  CHECK_EQ(async(VK_SPACE), 0xFFFF8000u);  // still down, no new press
  CHECK_EQ(sync(VK_SPACE), 0xFFFF8000u);   // GetKeyState has no "pressed" bit
  CHECK_EQ(async('A'), 0u);                // per VK
  t.input.keys.reset(VK_SPACE);
  CHECK_EQ(async(VK_SPACE), 0u);
  t.input.keys.set(VK_SPACE);
  CHECK_EQ(async(VK_SPACE), 0xFFFF8001u);  // a second press
  // Buttons: 1 left, 2 right, 4 middle.
  t.input.mouse_buttons = kMouseRight;
  CHECK_EQ(async(VK_RBUTTON), 0xFFFF8001u);
  CHECK_EQ(async(VK_RBUTTON), 0xFFFF8000u);
  CHECK_EQ(async(VK_LBUTTON), 0u);
  CHECK_EQ(async(VK_MBUTTON), 0u);
  CHECK_EQ(sync(VK_RBUTTON), 0xFFFF8000u);
  t.input.mouse_buttons = kMouseLeft | kMouseMiddle;
  t.input.mouse_button = true;
  CHECK_EQ(async(VK_RBUTTON), 0u);
  CHECK_EQ(async(VK_LBUTTON), 0xFFFF8001u);
  CHECK_EQ(async(VK_MBUTTON), 0xFFFF8001u);
  // Caps Lock's toggle is GetKeyState's bit 0.
  t.input.caps = true;
  CHECK_EQ(sync(VK_CAPITAL) & 1, 1u);
  t.input.caps = false;
  CHECK_EQ(sync(VK_CAPITAL) & 1, 0u);
}

// WM_CLOSE or SC_CLOSE to the saver window raises the wake flag; other
// messages and other windows do not.
TEST(user32_wake_on_close) {
  Rt t;
  uint32_t saver = host_window(*t.rt);
  CHECK(!saver_wake_requested(*t.rt));
  t.call("USER32.DLL", "PostMessageA", {saver, WM_USER, 0, 0});
  t.call("USER32.DLL", "PostMessageA", {saver, WM_SYSCOMMAND, SC_MINIMIZE, 0});
  CHECK(!saver_wake_requested(*t.rt));
  t.call("USER32.DLL", "PostMessageA", {saver, WM_SYSCOMMAND, SC_CLOSE | 2, 0});
  CHECK(saver_wake_requested(*t.rt));
  Rt u;
  u.call("USER32.DLL", "SendMessageA", {host_window(*u.rt), WM_CLOSE, 0, 0});
  CHECK(saver_wake_requested(*u.rt));
}

// KERNEL32's file and profile shims on a memory overlay: writes copy up and
// stay in memory, deletes and renames follow the layers, FindFirstFileA
// merges, and profiles combine the WIN.INI seeds with the files.
TEST(kernel32_files_and_profiles_on_the_overlay) {
  wchar_t tmp[MAX_PATH];
  GetTempPathW(MAX_PATH, tmp);
  std::string lower = narrow(tmp) + "adw_k32_" + std::to_string(GetCurrentProcessId());
  CreateDirectoryW(widen(lower).c_str(), nullptr);
  {
    FILE* f = _wfopen(widen(lower + "\\LOW.DAT").c_str(), L"wb");
    fputs("lower", f);
    fclose(f);
  }
  Rt t;
  auto& mem = t.rt->mem();
  t.rt->vfs().mount_overlay("C:\\AFTERDRK", lower, "");
  t.rt->vfs().mount_overlay("C:\\WINDOWS", "", "");
  // Write a new file, read it back.
  uint32_t name = t.str(0, "C:\\AFTERDRK\\SAVE.DAT");
  uint32_t h = t.call("KERNEL32.DLL", "CreateFileA", {name, GENERIC_READ | GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0});
  CHECK(h != 0xFFFFFFFF);
  t.str(100, "hello world");
  CHECK_EQ(t.call("KERNEL32.DLL", "WriteFile", {h, t.buf + 100, 11, t.buf + 200, 0}), 1u);
  CHECK_EQ(mem.read_u32l(t.buf + 200), 11u);
  CHECK_EQ(t.call("KERNEL32.DLL", "SetFilePointer", {h, 5, 0, FILE_BEGIN}), 5u);
  CHECK_EQ(t.call("KERNEL32.DLL", "SetEndOfFile", {h}), 1u);
  CHECK_EQ(t.call("KERNEL32.DLL", "GetFileSize", {h, 0}), 5u);
  CHECK_EQ(t.call("KERNEL32.DLL", "CloseHandle", {h}), 1u);
  std::string back;
  CHECK(t.rt->vfs().read_file("C:\\AFTERDRK\\SAVE.DAT", &back) && back == "hello");
  // _lcreat / _lwrite / _lread.
  uint32_t lc = t.call("KERNEL32.DLL", "_lcreat", {t.str(300, "C:\\WINDOWS\\LUN.DAT"), 0});
  CHECK(lc != 0xFFFFFFFF);
  CHECK_EQ(t.call("KERNEL32.DLL", "_lwrite", {lc, t.buf + 100, 5}), 5u);
  // Guest counts and offsets never size a host allocation: a 4 GB _hread of
  // a 5-byte file reads its 5 bytes; a write 2 GB out is a full disk
  // (Vfs::kMaxFileSize), not a 2 GB buffer.
  CHECK_EQ(t.call("KERNEL32.DLL", "_llseek", {lc, 0, 0}), 0u);
  CHECK_EQ(t.call("KERNEL32.DLL", "_hread", {lc, t.buf + 3000, 0xFFFFFFF0u}), 5u);
  CHECK_EQ(read_cstr(mem, t.buf + 3000).substr(0, 5), std::string("hello"));
  CHECK_EQ(t.call("KERNEL32.DLL", "_llseek", {lc, 0x7FFFFF00u, 0}), 0x7FFFFF00u);
  CHECK_EQ(t.call("KERNEL32.DLL", "WriteFile", {lc, t.buf + 100, 1, t.buf + 200, 0}), 0u);
  CHECK_EQ(mem.read_u32l(t.buf + 200), 0u);
  CHECK_EQ(t.rt->last_error(), uint32_t(ERROR_DISK_FULL));
  CHECK_EQ(t.call("KERNEL32.DLL", "GetFileSize", {lc, 0}), 5u);
  CHECK_EQ(t.call("KERNEL32.DLL", "_lclose", {lc}), 0u);
  // Copy, move, delete; the lower's file can be read and copied, not deleted.
  CHECK_EQ(t.call("KERNEL32.DLL", "CopyFileA", {t.str(400, "C:\\AFTERDRK\\LOW.DAT"), t.str(500, "C:\\AFTERDRK\\COPY.DAT"), 1}),
           1u);
  CHECK(t.rt->vfs().read_file("C:\\AFTERDRK\\COPY.DAT", &back) && back == "lower");
  CHECK_EQ(t.call("KERNEL32.DLL", "DeleteFileA", {t.buf + 400}), 0u);
  CHECK_EQ(t.rt->last_error(), uint32_t(ERROR_ACCESS_DENIED));
  CHECK_EQ(t.call("KERNEL32.DLL", "MoveFileA", {t.buf + 500, t.str(600, "C:\\AFTERDRK\\MOVED.DAT")}), 1u);
  CHECK_EQ(t.call("KERNEL32.DLL", "DeleteFileA", {t.buf + 600}), 1u);
  CHECK_EQ(t.call("KERNEL32.DLL", "CreateDirectoryA", {t.str(700, "C:\\AFTERDRK\\SAVES"), 0}), 1u);
  CHECK_EQ(t.call("KERNEL32.DLL", "GetFileAttributesA", {t.buf + 700}), uint32_t(FILE_ATTRIBUTE_DIRECTORY));
  // FindFirstFileA merges the layers: LOW.DAT (lower), SAVE.DAT, SAVES (upper).
  std::vector<std::string> found;
  uint32_t fh = t.call("KERNEL32.DLL", "FindFirstFileA", {t.str(800, "C:\\AFTERDRK\\*.*"), t.buf + 1024});
  CHECK(fh != 0xFFFFFFFF);
  if (fh != 0xFFFFFFFF) {
    do found.push_back(read_cstr(mem, t.buf + 1024 + 44));
    while (t.call("KERNEL32.DLL", "FindNextFileA", {fh, t.buf + 1024}));
    CHECK_EQ(t.rt->last_error(), uint32_t(ERROR_NO_MORE_FILES));
    t.call("KERNEL32.DLL", "FindClose", {fh});
  }
  CHECK(found == (std::vector<std::string>{".", "..", "LOW.DAT", "SAVE.DAT", "SAVES"}));
  CHECK_EQ(t.call("KERNEL32.DLL", "FindFirstFileA", {t.str(900, "C:\\NOWHERE\\*.*"), t.buf + 1024}), 0xFFFFFFFFu);
  CHECK_EQ(t.rt->last_error(), uint32_t(ERROR_PATH_NOT_FOUND));
  // Profiles: WIN.INI's seeds, a written MODULES.INI, key enumeration.
  uint32_t out = t.buf + 2048;
  CHECK(t.call("KERNEL32.DLL", "GetProfileStringA",
               {t.str(1400, "Berkeley Systems"), t.str(1500, "AD Ini Files"), t.str(1600, ""), out, 256}) > 0);
  CHECK_EQ(read_cstr(mem, out), std::string("C:\\WINDOWS"));
  CHECK_EQ(t.call("KERNEL32.DLL", "WritePrivateProfileStringA",
                  {t.str(1400, "Messages"), t.str(1500, "customA"), t.str(1600, "HI THERE"), t.str(1700, "MODULES.INI")}),
           1u);
  CHECK_EQ(t.call("KERNEL32.DLL", "GetPrivateProfileStringA",
                  {t.buf + 1400, t.buf + 1500, t.str(1800, "def"), out, 256, t.buf + 1700}),
           8u);
  CHECK_EQ(read_cstr(mem, out), std::string("HI THERE"));
  CHECK(t.rt->vfs().read_file("C:\\WINDOWS\\MODULES.INI", &back) && back == "[Messages]\r\ncustomA=HI THERE\r\n");
  CHECK_EQ(t.call("KERNEL32.DLL", "GetPrivateProfileStringA", {t.buf + 1400, 0, t.buf + 1800, out, 256, t.buf + 1700}),
           8u);  // "customA\0" + the final NUL
  CHECK_EQ(t.call("KERNEL32.DLL", "WritePrivateProfileStringA", {t.buf + 1400, t.buf + 1500, 0, t.buf + 1700}), 1u);
  CHECK_EQ(t.call("KERNEL32.DLL", "GetPrivateProfileStringA",
                  {t.buf + 1400, t.buf + 1500, t.buf + 1800, out, 256, t.buf + 1700}),
           3u);  // deleted: the default
  // Nothing reached the disk.
  CHECK(GetFileAttributesW(widen(lower + "\\SAVE.DAT").c_str()) == INVALID_FILE_ATTRIBUTES);
  CHECK(t.rt->shims().unimplemented_called().empty());
  close_guest_files(*t.rt);
  DeleteFileW(widen(lower + "\\LOW.DAT").c_str());
  RemoveDirectoryW(widen(lower).c_str());
}

// The .scr's desktop capture is a delete-on-close file it keeps open (share
// read + delete): Display::seed must be able to read it, and a P6 of exactly
// the screen size is used 1:1.
TEST(display_seed_delete_on_close) {
  VirtualClock clock{VirtualClock::Mode::fixed_step, 16667};
  RuntimeOptions o;
  o.heap_size = 16u << 20;
  Runtime rt(o, clock);
  Screen screen(32, 16);
  Display& d = rt.attach_display(screen);
  wchar_t tmp[MAX_PATH], file[MAX_PATH];
  GetTempPathW(MAX_PATH, tmp);
  GetTempFileNameW(tmp, L"sd", 0, file);
  HANDLE h = CreateFileW(file, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
  CHECK(h != INVALID_HANDLE_VALUE);
  // Left half pure red, right half pure blue: both statics, landing exactly.
  std::string p6 = "P6\n32 16\n255\n";
  for (int y = 0; y < 16; y++)
    for (int x = 0; x < 32; x++) p6 += x < 16 ? std::string("\xFF\x00\x00", 3) : std::string("\x00\x00\xFF", 3);
  DWORD put = 0;
  WriteFile(h, p6.data(), DWORD(p6.size()), &put, nullptr);
  FlushFileBuffers(h);
  std::string what;
  CHECK(d.seed(narrow(file), &what));
  CHECK(what.find("P6") != std::string::npos && what.find("scaled") == std::string::npos);
  CHECK_EQ(int(d.bits()[0]), 249);                       // pure red
  CHECK_EQ(int(d.bits()[size_t(5) * d.pitch() + 31]), 252);  // pure blue
  CloseHandle(h);
  CHECK(GetFileAttributesW(file) == INVALID_FILE_ATTRIBUTES);  // gone with its last handle
}

// The configure script: blocks split by NEXT, FILE/ANSWER queues, and a
// line number for anything malformed.
TEST(config_script_parse) {
  ConfigScript s;
  std::string err;
  CHECK(s.parse("# test\nTEXT 101 HELLO  FROM TEST\nCHECK 1003 1\nCLICK 1\nNEXT\nSELECT 0x69 2\nMULTI 7 1,3\n"
                "FILE C:\\pics\\a.jpg\nANSWER IDYES\nANSWER 7\n",
                &err));
  CHECK(s.scripted());
  CHECK_EQ(*s.message_box(L"q"), IDYES);
  CHECK_EQ(*s.message_box(L"q"), 7);
  CHECK(!s.message_box(L"q"));  // none left, not hidden: show it
  CHECK(*s.file_dialog() == L"C:\\pics\\a.jpg");
  CHECK(!s.file_dialog());
  s.set_hidden(true);
  CHECK_EQ(*s.message_box(L"q"), IDCANCEL);  // hidden and unanswered
  CHECK(s.file_dialog()->empty());
  ConfigScript bad;
  CHECK(!bad.parse("TEXT 101 ok\nCHECK 5 9\n", &err));
  CHECK(err.find("line 2") != std::string::npos);
  CHECK(!bad.parse("JUMP 3\n", &err));
  CHECK(!bad.parse("ANSWER MAYBE\n", &err));
  CHECK(!bad.parse("PICK 204\n", &err));
  CHECK(err.find("PICK needs") != std::string::npos);
}

// PICK selects the item with that text (any case) wherever a sorted list box
// or combo box holds it, and notifies as SELECT does; an item that is not
// there, or a control of another class, fails (the runner logs it).
TEST(config_script_pick) {
  ConfigScript s;
  std::string err;
  CHECK(s.parse("PICK 204 [-h-]\nPICK 205 two\nPICK 204 [NOPE]\nPICK 206 x\n", &err));
  HWND dlg = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"", WS_POPUP, -32000, -32000, 200, 200, nullptr, nullptr,
                             GetModuleHandleW(nullptr), nullptr);
  HWND lb = CreateWindowExW(0, L"LISTBOX", L"", WS_CHILD | LBS_SORT | LBS_HASSTRINGS, 0, 0, 100, 100, dlg,
                            reinterpret_cast<HMENU>(uintptr_t(204)), nullptr, nullptr);
  HWND cb = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | CBS_DROPDOWNLIST | CBS_SORT | CBS_HASSTRINGS, 0, 100, 100,
                            100, dlg, reinterpret_cast<HMENU>(uintptr_t(205)), nullptr, nullptr);
  CreateWindowExW(0, L"BUTTON", L"", WS_CHILD, 0, 150, 50, 20, dlg, reinterpret_cast<HMENU>(uintptr_t(206)), nullptr,
                  nullptr);
  CHECK(dlg && lb && cb);
  for (const wchar_t* t : {L"[..]", L"[-h-]", L"[SUB]", L"[-c-]"}) SendMessageW(lb, LB_ADDSTRING, 0, LPARAM(t));
  for (const wchar_t* t : {L"one", L"two", L"three"}) SendMessageW(cb, CB_ADDSTRING, 0, LPARAM(t));
  ConfigScript::Action a;
  a.kind = ConfigScript::Action::Kind::pick;
  a.id = 204;
  a.text = L"[-H-]";
  wchar_t sel[32] = {};
  CHECK(ConfigScript::apply(dlg, a));
  LRESULT i = SendMessageW(lb, LB_GETCURSEL, 0, 0);
  SendMessageW(lb, LB_GETTEXT, WPARAM(i), LPARAM(sel));
  CHECK(i >= 0 && std::wstring(sel) == L"[-h-]");
  a.id = 205;
  a.text = L"two";
  CHECK(ConfigScript::apply(dlg, a));
  i = SendMessageW(cb, CB_GETCURSEL, 0, 0);
  SendMessageW(cb, CB_GETLBTEXT, WPARAM(i), LPARAM(sel));
  CHECK(i >= 0 && std::wstring(sel) == L"two");
  a.id = 204;
  a.text = L"[NOPE]";
  LRESULT before = SendMessageW(lb, LB_GETCURSEL, 0, 0);
  CHECK(!ConfigScript::apply(dlg, a));
  CHECK_EQ(SendMessageW(lb, LB_GETCURSEL, 0, 0), before);
  a.id = 206;
  a.text = L"x";
  CHECK(!ConfigScript::apply(dlg, a));  // a button
  a.id = 207;
  CHECK(!ConfigScript::apply(dlg, a));  // missing
  DestroyWindow(dlg);
}

// PRESS clicks where a user's click lands: the system's hit test from the
// dialog down (children first, the top of the z-order first) passes over a
// frame that answers WM_NCHITTEST with HTTRANSPARENT (Intermission's ANT3DBOX
// over its check boxes) and over a disabled control, and stops at an opaque
// one; the window found gets WM_LBUTTONDOWN/UP at the point, and what it posted
// is handled before PRESS returns. A point outside every client area (a list
// box's scroll bar) is refused, as are malformed lines.
TEST(config_script_press) {
  ConfigScript s;
  std::string err;
  CHECK(s.parse("PRESS 101\nPRESS 0x66 3 4\n", &err));
  CHECK(!s.parse("PRESS 101 3\n", &err) && err.find("PRESS takes") != std::string::npos);
  CHECK(!s.parse("PRESS 101 -1 4\n", &err));
  CHECK(!s.parse("PRESS\n", &err));
  // The parent counts the BN_CLICKED it gets (sent by a button, or posted).
  static int clicked[8];
  static int frame_clicks;
  for (int& n : clicked) n = 0;
  frame_clicks = 0;
  WNDCLASSEXW pc{sizeof(pc)};
  pc.lpfnWndProc = [](HWND h, UINT m, WPARAM w, LPARAM l) -> LRESULT {
    if (m == WM_COMMAND && HIWORD(w) == BN_CLICKED && LOWORD(w) >= 100 && LOWORD(w) < 108) clicked[LOWORD(w) - 100]++;
    return DefWindowProcW(h, m, w, l);
  };
  pc.hInstance = GetModuleHandleW(nullptr);
  pc.lpszClassName = L"AdwPressParent";
  RegisterClassExW(&pc);
  // A frame transparent to the hit test, as ANTSW's are.
  WNDCLASSEXW fc{sizeof(fc)};
  fc.lpfnWndProc = [](HWND h, UINT m, WPARAM w, LPARAM l) -> LRESULT {
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    if (m == WM_LBUTTONDOWN) frame_clicks++;
    return DefWindowProcW(h, m, w, l);
  };
  fc.hInstance = GetModuleHandleW(nullptr);
  fc.lpszClassName = L"AdwPressFrame";
  RegisterClassExW(&fc);
  // A control of its own that posts its BN_CLICKED, as ANTSW's check box does.
  WNDCLASSEXW cc{sizeof(cc)};
  cc.lpfnWndProc = [](HWND h, UINT m, WPARAM w, LPARAM l) -> LRESULT {
    if (m == WM_LBUTTONUP)
      PostMessageW(GetParent(h), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(h), BN_CLICKED), LPARAM(h));
    return DefWindowProcW(h, m, w, l);
  };
  cc.hInstance = GetModuleHandleW(nullptr);
  cc.lpszClassName = L"AdwPressPoster";
  RegisterClassExW(&cc);
  HWND dlg = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"AdwPressParent", L"", WS_POPUP | WS_VISIBLE, -32000, -32000,
                             400, 300, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  auto child = [&](const wchar_t* cls, DWORD style, int x, int y, int w, int h, int id) {
    return CreateWindowExW(0, cls, L"", WS_CHILD | WS_VISIBLE | style, x, y, w, h, dlg, reinterpret_cast<HMENU>(uintptr_t(id)),
                           GetModuleHandleW(nullptr), nullptr);
  };
  // Created first, so on top: a transparent frame over 101 and 102, an opaque
  // one (a notifying static: HTCLIENT) over 103.
  HWND frame = child(L"AdwPressFrame", 0, 0, 0, 200, 100, 100);
  HWND opaque = child(L"STATIC", SS_NOTIFY, 0, 150, 200, 50, 107);
  HWND b101 = child(L"BUTTON", BS_AUTOCHECKBOX, 10, 10, 100, 20, 101);
  HWND b102 = child(L"BUTTON", BS_AUTOCHECKBOX | WS_DISABLED, 10, 40, 100, 20, 102);
  HWND b103 = child(L"BUTTON", BS_AUTOCHECKBOX, 10, 160, 100, 20, 103);
  HWND poster = child(L"AdwPressPoster", 0, 10, 70, 100, 20, 104);
  HWND list = child(L"LISTBOX", WS_VSCROLL | LBS_NOTIFY | LBS_DISABLENOSCROLL, 250, 10, 100, 80, 105);
  CHECK(dlg && frame && opaque && b101 && b102 && b103 && poster && list);
  CHECK(GetWindow(dlg, GW_CHILD) == frame);
  ConfigScript::Action a;
  a.kind = ConfigScript::Action::Kind::press;
  RECT r{};
  GetWindowRect(b101, &r);
  LRESULT ht = 0;
  CHECK(ConfigScript::hit_window(dlg, POINT{r.left + 5, r.top + 5}, &ht) == b101 && ht == HTCLIENT);
  GetWindowRect(frame, &r);
  CHECK(ConfigScript::hit_window(dlg, POINT{r.right - 5, r.bottom - 5}, &ht) == dlg);  // the frame alone: through it
  a.id = 101;
  CHECK(ConfigScript::apply(dlg, a));
  CHECK_EQ(SendMessageW(b101, BM_GETCHECK, 0, 0), LRESULT(BST_CHECKED));
  CHECK_EQ(clicked[1], 1);
  CHECK_EQ(frame_clicks, 0);
  a.id = 102;  // disabled: the click goes past it, to the dialog
  CHECK(ConfigScript::apply(dlg, a));
  CHECK_EQ(SendMessageW(b102, BM_GETCHECK, 0, 0), LRESULT(BST_UNCHECKED));
  a.id = 103;  // under the opaque frame, which takes the click
  CHECK(ConfigScript::apply(dlg, a));
  CHECK_EQ(SendMessageW(b103, BM_GETCHECK, 0, 0), LRESULT(BST_UNCHECKED));
  CHECK_EQ(clicked[3], 0);
  a.id = 104;  // its posted BN_CLICKED is handled before PRESS returns
  CHECK(ConfigScript::apply(dlg, a));
  CHECK_EQ(clicked[4], 1);
  // At a point in the list's client area; and in its scroll bar: refused.
  RECT lc{};
  GetClientRect(list, &lc);
  a.id = 105;
  a.at = true;
  a.x = 5;
  a.y = 5;
  CHECK(ConfigScript::apply(dlg, a));
  a.x = lc.right + 3;
  CHECK(!ConfigScript::apply(dlg, a));
  a.id = 106;  // missing
  CHECK(!ConfigScript::apply(dlg, a));
  DestroyWindow(dlg);
  for (const wchar_t* c : {L"AdwPressParent", L"AdwPressFrame", L"AdwPressPoster"}) UnregisterClassW(c, GetModuleHandleW(nullptr));
}

// Real windows get small guest handles with a zero high word (Windows 95's
// HWNDs were 16-bit), both ways, and stop resolving once destroyed.
TEST(realui_handle_map) {
  VirtualClock clock{VirtualClock::Mode::fixed_step, 16667};
  RuntimeOptions o;
  o.heap_size = 16u << 20;
  Runtime rt(o, clock);
  RealUi& ui = real_ui(rt);
  HWND w = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"x", WS_POPUP, -32000, -32000, 10, 10, nullptr, nullptr,
                           GetModuleHandleW(nullptr), nullptr);
  CHECK(w != nullptr);
  uint32_t g = ui.guest(w);
  CHECK(RealUi::is_real(g));
  CHECK_EQ(g >> 16, 0u);
  CHECK_EQ(ui.guest(w), g);
  CHECK(ui.real(g) == w);
  CHECK_EQ(ui.guest(nullptr), 0u);
  CHECK(!RealUi::is_real(0x00010010));  // emulated windows
  CHECK_EQ(ui.window_text(g), std::string("x"));
  DestroyWindow(w);
  CHECK(ui.real(g) == nullptr);
}
