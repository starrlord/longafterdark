// Unit tests of the Linux player's window-free parts. Build and run from
// the repository root, on Linux, with every scr/linux/*.cc but main.cc:
//
//   g++ -std=c++20 -O1 -Wall -Wextra -I importer -I <dir of adw_version.h>
//       scr/linux/tests/unit.cc $(ls scr/linux/*.cc | grep -v '/main.cc$')
//       importer/minijson.cc -lX11 -lXext -lXrandr -lpthread -o /tmp/lad_unit
//   /tmp/lad_unit
//
// Exit 0 when every check passes. No X display or Wine is needed.
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "../catalog.h"
#include "../frame_parser.h"
#include "../geometry.h"
#include "../host.h"
#include "../input_rules.h"
#include "../keymap.h"
#include "../monitors.h"
#include "../options.h"
#include "../pixels.h"
#include "../present.h"
#include "../restart.h"
#include "../sound.h"
#include "../status.h"
#include "../wine.h"

using namespace lad;

namespace {

int g_failures = 0, g_checks = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    ++g_checks;                                                        \
    if (!(cond)) {                                                     \
      ++g_failures;                                                    \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
    }                                                                  \
  } while (0)

std::string p8(int w, int h, uint8_t fill = 7) {
  std::string s = "P8\n" + std::to_string(w) + " " + std::to_string(h) + "\n";
  s += std::string(768, '\x01');
  s += std::string((size_t)w * h, (char)fill);
  return s;
}

void test_frame_parser() {
  {
    FrameParser p;
    RawFrame f;
    std::string a = p8(3, 2);
    // Fed a byte at a time: a frame only once all of it is in.
    for (size_t i = 0; i + 1 < a.size(); ++i) {
      p.feed(&a[i], 1);
      CHECK(!p.take(f));
    }
    p.feed(&a[a.size() - 1], 1);
    CHECK(p.take(f));
    CHECK(f.width == 3 && f.height == 2 && f.format == 8 && f.palette.size() == 768 && f.pixels.size() == 6);
    CHECK(f.pixels[5] == 7);
    CHECK(!p.take(f));
  }
  {
    // Garbage before and between frames costs a resync, not the stream.
    FrameParser p;
    RawFrame f;
    std::string s = "hello world\n" + p8(2, 2, 1) + "Pxyz" + p8(4, 1, 2);
    p.feed(s.data(), s.size());
    CHECK(p.take(f) && f.width == 2 && f.pixels[0] == 1);
    CHECK(p.take(f) && f.width == 4 && f.pixels[0] == 2);
    CHECK(p.resyncs() >= 2);
  }
  {
    // Headers no host sends are refused at once.
    FrameHeader h;
    auto hdr = [&](const std::string& s) {
      return parse_frame_header(reinterpret_cast<const uint8_t*>(s.data()), s.size(), h);
    };
    CHECK(hdr("P8\n0 480\n") == HeaderResult::invalid);
    CHECK(hdr("P8\n640 0\n") == HeaderResult::invalid);
    CHECK(hdr("P8\n-1 480\n") == HeaderResult::invalid);
    CHECK(hdr("P8\n+640 480\n") == HeaderResult::invalid);
    CHECK(hdr("P8\n8193 1\n") == HeaderResult::invalid);
    CHECK(hdr("P8\n3000000 1\n") == HeaderResult::invalid);
    CHECK(hdr("P8\n8192 8192\n") == HeaderResult::invalid);   // past 4096x4096 pixels
    CHECK(hdr("P8\n4096 4096\n") == HeaderResult::ok);
    CHECK(hdr("P8\n640x 480\n") == HeaderResult::invalid);
    CHECK(hdr("P8\n64") == HeaderResult::need_more);
    CHECK(hdr("P6\n2 2\n256\n") == HeaderResult::invalid);
    CHECK(hdr("P6\n2 2\n255\n") == HeaderResult::ok && h.format == 6 && h.body_start == 11);
    CHECK(hdr("P8\n" + std::string(5000, ' ')) == HeaderResult::invalid);   // no header is that long
  }
  {
    FrameParser p;
    RawFrame f;
    std::string s = "P6\n2 1\n255\n" + std::string("\x10\x20\x30\x40\x50\x60", 6);
    p.feed(s.data(), s.size());
    CHECK(p.take(f) && f.format == 6 && f.pixels.size() == 6 && f.palette.empty() && f.pixels[3] == 0x40);
  }
}

void test_status() {
  HostStatus s;
  CHECK(parse_status_line("STATUS 12 flags=0x21 applied=3 eaten=2 src=1", s));
  CHECK(s.frames == 12 && s.flags == 0x21 && s.input_applied == 3 && s.input_eaten == 2 && s.source == 1);
  CHECK(parse_status_line("STATUS 0 flags=0x20 applied=0 eaten=0 src=0\r", s) && s.flags == 0x20);
  HostStatus keep = s;
  CHECK(!parse_status_line("STATUS 1 flags=0x21 applied=3 eaten=2", s));
  CHECK(!parse_status_line("STATUS 1 flags=21 applied=3 eaten=2 src=1", s));
  CHECK(!parse_status_line("STATUS 1 flags=0x21 applied=-3 eaten=2 src=1", s));
  CHECK(!parse_status_line("STATUS 1 flags=0x21 applied=3 eaten=2 src=1 x", s));
  CHECK(!parse_status_line("STATUSX 1 flags=0x21 applied=3 eaten=2 src=1", s));
  CHECK(!parse_status_line("[adhostwin] STATUS 1 flags=0x21 applied=3 eaten=2 src=1", s));
  CHECK(!parse_status_line("STATUS 1 flags=0x1ffffffff applied=3 eaten=2 src=1", s));
  CHECK(s.frames == keep.frames && s.flags == keep.flags);

  HostCapabilities c = parse_capabilities(
      "lanes=pe32,ne16 configure=pe32,ne16 abis=afterdark,intermission status=1 state=1 seed=1 audio=1 numlock=1\r\n");
  CHECK(c.known && c.numlock && c.takes_numlock_lines() && c.takes_numlock_env());
  CHECK(c.runs("ne16", "intermission") && c.runs("pe32", "") && !c.runs("x86", "afterdark"));
  HostCapabilities old = parse_capabilities("lanes=pe32,ne16 status=1");
  CHECK(old.known && !old.numlock && !old.takes_numlock_lines() && !old.takes_numlock_env());
  CHECK(old.runs("ne16", "afterdark") && !old.runs("ne16", "intermission"));
  HostCapabilities none = parse_capabilities("usage: adhostwin <module>");
  CHECK(!none.known && !none.takes_numlock_lines() && none.takes_numlock_env() && none.runs("ne16", "intermission"));
  CHECK(!parse_capabilities("lanes=ne16 numlock=true").numlock);
}

void test_decide() {
  const auto t0 = InputClock::now();
  auto st = [](bool running, bool have, uint32_t flags, uint64_t applied, uint64_t eaten) {
    OwnerStatus s;
    s.running = running;
    s.have = have;
    s.rec.flags = flags;
    s.rec.input_applied = applied;
    s.rec.input_eaten = eaten;
    return s;
  };
  const OwnerStatus idle = st(true, true, ADWS_READY, 4, 0);
  const OwnerStatus playing = st(true, true, ADWS_READY | ADWS_INTERACTIVE, 4, 4);
  auto key = [](int vk) { return InputEvent{InputKind::key_down, vk}; };
  // Exempt keys never wake; any other key does when nothing plays.
  for (int vk : {VK_SHIFT, VK_CONTROL, VK_CAPITAL, VK_NUMLOCK, VK_LSHIFT, VK_RCONTROL}) {
    CHECK(decide(key(vk), idle, {5, false, t0}, t0) == Verdict::forward);
  }
  CHECK(decide(key('A'), idle, {5, false, t0}, t0) == Verdict::exit);
  CHECK(decide(key('A'), playing, {5, false, t0}, t0) == Verdict::forward);
  // Alt/F10 and switching away always end it, a game or not.
  CHECK(decide(InputEvent{InputKind::syskey_down, VK_MENU}, playing, {}, t0) == Verdict::exit);
  CHECK(decide(InputEvent{InputKind::syskey_down, VK_F10}, playing, {}, t0) == Verdict::exit);
  CHECK(decide(InputEvent{InputKind::deactivate}, playing, {}, t0) == Verdict::exit);
  // Releases never do.
  CHECK(decide(InputEvent{InputKind::key_up, 'A'}, idle, {}, t0) == Verdict::forward);
  CHECK(decide(InputEvent{InputKind::button_up}, idle, {}, t0) == Verdict::forward);
  // The wheel: an exit when idle, the game's (ignored) when playing.
  CHECK(decide(InputEvent{InputKind::wheel}, idle, {}, t0) == Verdict::exit);
  CHECK(decide(InputEvent{InputKind::wheel}, playing, {}, t0) == Verdict::forward);
  // The move threshold.
  CHECK(decide(InputEvent{InputKind::move, 0, 10.0}, idle, {5, false, t0}, t0) == Verdict::forward);
  CHECK(decide(InputEvent{InputKind::move, 0, 10.5}, idle, {5, false, t0}, t0) == Verdict::exit);
  CHECK(decide(InputEvent{InputKind::move, 0, 500}, playing, {5, false, t0}, t0) == Verdict::forward);
  // No host, or nothing sent: nobody to ask.
  CHECK(decide(key('A'), st(false, false, 0, 0, 0), {5, false, t0}, t0) == Verdict::exit);
  CHECK(decide(key('A'), idle, {0, false, t0}, t0) == Verdict::exit);
  // Stale (the line before this one not yet applied: a Caps Lock press may
  // be starting a game): hold, then decide on what the host says.
  const OwnerStatus stale = st(true, true, ADWS_READY, 3, 0);
  CHECK(decide(key('A'), stale, {5, false, t0}, t0) == Verdict::hold);
  CHECK(decide(key('A'), st(true, false, 0, 0, 0), {5, false, t0}, t0) == Verdict::hold);   // nothing published yet
  CHECK(decide(key('A'), st(true, true, ADWS_READY, 5, 0), {5, true, t0}, t0) == Verdict::exit);    // applied, not eaten
  CHECK(decide(key('A'), st(true, true, ADWS_READY, 5, 5), {5, true, t0}, t0) == Verdict::forward); // eaten
  CHECK(decide(key('A'), stale, {5, true, t0}, t0 + std::chrono::milliseconds(299)) == Verdict::hold);
  CHECK(decide(key('A'), stale, {5, true, t0}, t0 + std::chrono::milliseconds(300)) == Verdict::exit);
  // Key-filter: hold even when not stale.
  const OwnerStatus filter = st(true, true, ADWS_READY | ADWS_KEY_FILTER, 4, 0);
  CHECK(decide(key('A'), filter, {5, false, t0}, t0) == Verdict::hold);
  CHECK(decide(InputEvent{InputKind::button_down}, filter, {5, false, t0}, t0) == Verdict::hold);
  CHECK(exit_reason(key(0x41)) == "key vk=0x41");
  CHECK(exit_reason(InputEvent{InputKind::move}, 12, -3) == "move dx=12 dy=-3");
}

void test_lines_and_mapping() {
  CHECK(key_line(0x41, true) == "KEY 65 1");
  CHECK(key_line(0x14, false) == "KEY 20 0");
  CHECK(caps_line(true) == "CAPS 1" && numlock_line(false) == "NUMLOCK 0");
  CHECK(mouse_line(10, 20, 1 | 2 | 4) == "MOUSE 10 20 7");
  const RectI whole{0, 0, 1280, 720};
  CHECK((map_to_frame({0, 0}, whole, {640, 480}) == PointI{0, 0}));
  CHECK((map_to_frame({1279, 719}, whole, {640, 480}) == PointI{639, 479}));
  CHECK((map_to_frame({640, 360}, whole, {640, 480}) == PointI{320, 240}));
  const RectI box = fit_rect(640, 480, 1280, 720);   // pillarboxed
  CHECK((box == RectI{160, 0, 960, 720}));
  CHECK((map_to_frame({159, 10}, box, {640, 480}) == PointI{0, 6}));    // left of the frame: clamped
  CHECK((map_to_frame({5000, -5}, box, {640, 480}) == PointI{639, 0}));
}

void test_geometry() {
  CHECK((emulated_screen_size(16.0 / 9.0, 1.0) == SizeI{856, 480}));
  CHECK((emulated_screen_size(4.0 / 3.0, 1.0) == SizeI{640, 480}));
  CHECK((emulated_screen_size(3.0 / 4.0, 1.0) == SizeI{640, 480}));   // never narrower than 4:3
  CHECK((emulated_screen_size(32.0 / 9.0, 1.0) == SizeI{1280, 480}));  // capped at twice 4:3
  CHECK((own_screen("intermission") == SizeI{640, 480}));
  CHECK((own_screen("afterdark") == SizeI{}));
  CHECK((own_screen("afterdark", {640, 480}) == SizeI{640, 480}));
  CHECK(module_screen({640, 480}, 16.0 / 9.0, 1.0).fixed);
  CHECK((module_screen({}, 16.0 / 9.0, 1.0).emu == SizeI{856, 480}));
  // --lines 720 (the Windows Resolution setting's 1.5): After Dark's follow
  // it, a module's own screen doesn't.
  CHECK((module_screen({}, 16.0 / 9.0, 1.5).emu == SizeI{1280, 720}));
  CHECK((module_screen({}, 4.0 / 3.0, 1.5).emu == SizeI{960, 720}));
  CHECK((module_screen({}, 1366.0 / 768.0, 1.0).emu == SizeI{856, 480}));
  CHECK((module_screen(own_screen("intermission"), 16.0 / 9.0, 1.5) == ModuleScreen{{640, 480}, true}));
  CHECK((module_screen(own_screen("afterdark", {640, 480}), 21.0 / 9.0, 1.5).emu == SizeI{640, 480}));
  // The letterbox: a 4:3 screen pillarboxed on 16:9, After Dark's filling
  // it (a line of bar above and below from the snap to 8), a 4:3 screen
  // letterboxed on 5:4, a preview's 320x240 shrunk into a small pane.
  CHECK((fit_rect(640, 480, 1280, 720) == RectI{160, 0, 960, 720}));
  CHECK((fit_rect(856, 480, 1280, 720) == RectI{0, 1, 1280, 718}));
  CHECK((fit_rect(1280, 720, 1280, 720) == RectI{0, 0, 1280, 720}));
  CHECK((fit_rect(640, 480, 1280, 1024) == RectI{0, 32, 1280, 960}));
  CHECK((fit_rect(320, 240, 200, 150) == RectI{0, 0, 200, 150}));
  CHECK((fit_rect(320, 240, 400, 200) == RectI{66, 0, 267, 200}));
  CHECK((fit_rect(640, 480, 1, 1) == RectI{0, 0, 1, 1}));
  CHECK((kPreviewScreen == SizeI{320, 240}));
  // A window over a monitor: the one holding the monitor's centre.
  const RectI left{0, 0, 1280, 720}, right{1280, 0, 1280, 720}, both{0, 0, 2560, 720};
  CHECK(!contains_centre(left, right) && contains_centre(right, right) && contains_centre(both, right));
  CHECK(contains_centre(left, left) && !contains_centre(right, left));
  CHECK(contains_centre(RectI{0, 0, 1, 1}, RectI{0, 0, 1, 1}) && !contains_centre(RectI{0, 0, 0, 0}, left));
  CHECK(contains_centre(RectI{-1920, 0, 1920, 1080}, RectI{-1920, 0, 1920, 1080}));
}

// Every pixel format the player draws in, and the scaler writing into it:
// exact bytes, both byte orders, and never a byte outside the image rows.
void test_pixels() {
  PixelFormat pf;
  std::string why;
  CHECK(make_pixel_format(kTrueColor, 24, 32, false, 0xFF0000, 0xFF00, 0xFF, &pf, &why));
  CHECK(pf.bytes == 4 && pf.fill == 0 && pf.shift[0] == 16 && pf.bits[1] == 8 && pf.shift[2] == 0);
  CHECK(pack_rgb(pf, 255, 0, 0) == 0xFF0000 && pack_rgb(pf, 1, 2, 3) == 0x010203);
  CHECK(describe_format(pf) == "TrueColor, depth 24, 32 bits per pixel, RGB 8-8-8, LSB first");
  // Depth 32 (ARGB): the alpha bits set, so a compositor shows it opaque.
  CHECK(make_pixel_format(kTrueColor, 32, 32, false, 0xFF0000, 0xFF00, 0xFF, &pf, &why));
  CHECK(pf.fill == 0xFF000000u && pack_rgb(pf, 255, 0, 0) == 0xFFFF0000u && pack_rgb(pf, 0, 0, 0) == 0xFF000000u);
  // BGR.
  CHECK(make_pixel_format(kTrueColor, 24, 32, false, 0xFF, 0xFF00, 0xFF0000, &pf, &why));
  CHECK(pack_rgb(pf, 255, 0, 0) == 0x0000FF && pack_rgb(pf, 0, 0, 255) == 0xFF0000);
  CHECK(describe_format(pf).find("BGR 8-8-8") != std::string::npos);
  // 5-6-5, 5-5-5, 10-10-10.
  CHECK(make_pixel_format(kTrueColor, 16, 16, false, 0xF800, 0x7E0, 0x1F, &pf, &why) && pf.bytes == 2);
  CHECK(pack_rgb(pf, 255, 255, 255) == 0xFFFF && pack_rgb(pf, 255, 0, 0) == 0xF800 && pack_rgb(pf, 0, 255, 0) == 0x07E0);
  CHECK(pack_rgb(pf, 128, 128, 128) == 0x8410);
  CHECK(make_pixel_format(kTrueColor, 15, 16, false, 0x7C00, 0x3E0, 0x1F, &pf, &why) && pf.fill == 0);
  CHECK(pack_rgb(pf, 255, 0, 0) == 0x7C00 && pack_rgb(pf, 255, 255, 255) == 0x7FFF);
  CHECK(make_pixel_format(kTrueColor, 30, 32, false, 0x3FF00000, 0xFFC00, 0x3FF, &pf, &why) && pf.fill == 0);
  CHECK(pack_rgb(pf, 255, 255, 255) == 0x3FFFFFFF && pack_rgb(pf, 255, 0, 0) == 0x3FF00000 &&
        pack_rgb(pf, 0, 0, 128) == 514);
  CHECK(make_pixel_format(kTrueColor, 24, 24, true, 0xFF0000, 0xFF00, 0xFF, &pf, &why) && pf.bytes == 3 && pf.msb_first);
  // Refused, each with a reason naming the visual.
  CHECK(!make_pixel_format(kPseudoColor, 8, 8, false, 0, 0, 0, &pf, &why) && why.find("8-bit PseudoColor") != std::string::npos);
  CHECK(!make_pixel_format(kDirectColor, 24, 32, false, 0xFF0000, 0xFF00, 0xFF, &pf, &why) &&
        why.find("DirectColor") != std::string::npos);
  CHECK(!make_pixel_format(kTrueColor, 8, 8, false, 0xE0, 0x1C, 0x3, &pf, &why) && why.find("8 bits per pixel") != std::string::npos);
  CHECK(!make_pixel_format(kTrueColor, 24, 32, false, 0xFF0000, 0xFF0000, 0xFF, &pf, &why));   // overlapping
  CHECK(!make_pixel_format(kTrueColor, 24, 32, false, 0xF0F000, 0xF00, 0xFF, &pf, &why));      // a hole
  CHECK(!make_pixel_format(kTrueColor, 16, 16, false, 0xFF0000, 0xFF00, 0xFF, &pf, &why));     // past the depth
  CHECK(!make_pixel_format(kTrueColor, 24, 32, false, 0, 0xFF00, 0xFF, &pf, &why));            // no red
  CHECK(!make_pixel_format(kTrueColor, 24, 32, false, 0x1FFFF0000ul, 0xFF00, 0xFF, &pf, &why));

  // A 2x2 P8 frame: red, green / blue, white.
  RawFrame f;
  f.width = f.height = 2;
  f.format = 8;
  f.palette.assign(768, 0);
  const uint8_t pal[4][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}};
  for (int i = 0; i < 4; ++i) {
    for (int c = 0; c < 3; ++c) f.palette[(size_t)i * 3 + c] = pal[i][c];
  }
  f.pixels = {0, 1, 2, 3};
  // An image with padding after each row's pixels and a guard after the
  // last row: the scaler may touch only the pixels.
  struct Img {
    std::vector<uint8_t> mem;
    size_t stride;
    int w, h, bytes;
    Img(int w_, int h_, int bytes_) : stride((size_t)w_ * bytes_ + 5), w(w_), h(h_), bytes(bytes_) {
      mem.assign(stride * h + 64, 0xAB);
    }
    uint8_t* at(int x, int y) { return mem.data() + (size_t)y * stride + (size_t)x * bytes; }
    bool guards_intact() const {
      for (int y = 0; y < h; ++y) {
        for (size_t i = (size_t)w * bytes; i < stride; ++i) {
          if (mem[(size_t)y * stride + i] != 0xAB) return false;
        }
      }
      for (size_t i = stride * h; i < mem.size(); ++i) {
        if (mem[i] != 0xAB) return false;
      }
      return true;
    }
  };
  auto u32 = [](const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; };
  {
    FrameScaler s;
    CHECK(make_pixel_format(kTrueColor, 24, 32, false, 0xFF0000, 0xFF00, 0xFF, &pf, &why));
    Img im(4, 4, 4);
    s.scale(f, pf, im.mem.data(), im.stride, im.w, im.h);
    // Each frame pixel becomes a 2x2 block (little-endian host: the value as stored).
    CHECK(u32(im.at(0, 0)) == 0xFF0000 && u32(im.at(1, 1)) == 0xFF0000 && u32(im.at(2, 0)) == 0x00FF00);
    CHECK(u32(im.at(3, 1)) == 0x00FF00 && u32(im.at(0, 2)) == 0x0000FF && u32(im.at(3, 3)) == 0xFFFFFF);
    CHECK(im.guards_intact());
    // Downscaled into 1x1 and scaled to odd sizes: the guards hold.
    Img one(1, 1, 4);
    s.scale(f, pf, one.mem.data(), one.stride, 1, 1);
    CHECK(u32(one.at(0, 0)) == 0xFF0000 && one.guards_intact());
    Img odd(7, 5, 4);
    s.scale(f, pf, odd.mem.data(), odd.stride, 7, 5);
    CHECK(u32(odd.at(6, 4)) == 0xFFFFFF && u32(odd.at(3, 2)) == 0xFF0000 && u32(odd.at(3, 3)) == 0x0000FF &&
          odd.guards_intact());
  }
  {
    // 16 bits per pixel, both byte orders.
    FrameScaler s;
    CHECK(make_pixel_format(kTrueColor, 16, 16, false, 0xF800, 0x7E0, 0x1F, &pf, &why));
    Img lsb(3, 3, 2);
    s.scale(f, pf, lsb.mem.data(), lsb.stride, 3, 3);
    CHECK(lsb.at(0, 0)[0] == 0x00 && lsb.at(0, 0)[1] == 0xF8);   // red, LSB first
    CHECK(lsb.at(2, 2)[0] == 0xFF && lsb.at(2, 2)[1] == 0xFF);   // white
    CHECK(lsb.guards_intact());
    CHECK(make_pixel_format(kTrueColor, 16, 16, true, 0xF800, 0x7E0, 0x1F, &pf, &why));
    Img msb(3, 3, 2);
    s.scale(f, pf, msb.mem.data(), msb.stride, 3, 3);
    CHECK(msb.at(0, 0)[0] == 0xF8 && msb.at(0, 0)[1] == 0x00 && msb.guards_intact());
    CHECK(msb.at(2, 0)[0] == 0x07 && msb.at(2, 0)[1] == 0xE0);   // green
  }
  {
    // 24 bits per pixel (packed), both byte orders; 32 MSB first.
    FrameScaler s;
    CHECK(make_pixel_format(kTrueColor, 24, 24, false, 0xFF0000, 0xFF00, 0xFF, &pf, &why));
    Img lsb(5, 2, 3);
    s.scale(f, pf, lsb.mem.data(), lsb.stride, 5, 2);
    CHECK(lsb.at(0, 0)[0] == 0 && lsb.at(0, 0)[1] == 0 && lsb.at(0, 0)[2] == 255);   // red: B G R bytes
    CHECK(lsb.at(0, 1)[0] == 255 && lsb.at(0, 1)[2] == 0 && lsb.guards_intact());    // blue
    CHECK(make_pixel_format(kTrueColor, 24, 24, true, 0xFF0000, 0xFF00, 0xFF, &pf, &why));
    Img msb(5, 2, 3);
    s.scale(f, pf, msb.mem.data(), msb.stride, 5, 2);
    CHECK(msb.at(0, 0)[0] == 255 && msb.at(0, 0)[1] == 0 && msb.at(0, 0)[2] == 0 && msb.guards_intact());
    CHECK(make_pixel_format(kTrueColor, 24, 32, true, 0xFF0000, 0xFF00, 0xFF, &pf, &why));
    Img m32(2, 2, 4);
    s.scale(f, pf, m32.mem.data(), m32.stride, 2, 2);
    CHECK(m32.at(1, 0)[0] == 0 && m32.at(1, 0)[1] == 0 && m32.at(1, 0)[2] == 255 && m32.at(1, 0)[3] == 0);   // green
    CHECK(m32.guards_intact());
  }
  {
    // P6 into BGR and into 10-10-10.
    RawFrame rgb;
    rgb.width = 2;
    rgb.height = 1;
    rgb.format = 6;
    rgb.pixels = {255, 0, 0, 0, 0, 255};
    FrameScaler s;
    CHECK(make_pixel_format(kTrueColor, 24, 32, false, 0xFF, 0xFF00, 0xFF0000, &pf, &why));
    Img im(2, 1, 4);
    s.scale(rgb, pf, im.mem.data(), im.stride, 2, 1);
    CHECK(u32(im.at(0, 0)) == 0x0000FF && u32(im.at(1, 0)) == 0xFF0000 && im.guards_intact());
    CHECK(make_pixel_format(kTrueColor, 30, 32, false, 0x3FF00000, 0xFFC00, 0x3FF, &pf, &why));
    Img deep(4, 2, 4);
    s.scale(rgb, pf, deep.mem.data(), deep.stride, 4, 2);
    CHECK(u32(deep.at(0, 0)) == 0x3FF00000 && u32(deep.at(3, 1)) == 0x3FF && deep.guards_intact());
  }
  {
    // A frame whose buffers are shorter than its header says, an unknown
    // format, a stride too small, a zero size: nothing written.
    FrameScaler s;
    CHECK(make_pixel_format(kTrueColor, 24, 32, false, 0xFF0000, 0xFF00, 0xFF, &pf, &why));
    RawFrame bad = f;
    bad.pixels.resize(3);
    Img im(4, 4, 4);
    s.scale(bad, pf, im.mem.data(), im.stride, 4, 4);
    bad = f;
    bad.palette.resize(700);
    s.scale(bad, pf, im.mem.data(), im.stride, 4, 4);
    bad = f;
    bad.format = 5;
    s.scale(bad, pf, im.mem.data(), im.stride, 4, 4);
    s.scale(f, pf, im.mem.data(), 8, 4, 4);   // 8 bytes a row can't hold 4 pixels
    s.scale(f, pf, im.mem.data(), im.stride, 0, 4);
    bool untouched = true;
    for (uint8_t b : im.mem) untouched = untouched && b == 0xAB;
    CHECK(untouched);
  }
  {
    // A big downscale and upscale keep to the image too.
    RawFrame big;
    big.width = 640;
    big.height = 480;
    big.format = 8;
    big.palette.assign(768, 200);
    big.pixels.assign((size_t)640 * 480, 1);
    FrameScaler s;
    CHECK(make_pixel_format(kTrueColor, 16, 16, false, 0xF800, 0x7E0, 0x1F, &pf, &why));
    Img small(123, 77, 2), large(1001, 751, 2);
    s.scale(big, pf, small.mem.data(), small.stride, small.w, small.h);
    s.scale(big, pf, large.mem.data(), large.stride, large.w, large.h);
    CHECK(small.guards_intact() && large.guards_intact());
    const uint16_t grey = (uint16_t)pack_rgb(pf, 200, 200, 200);
    uint16_t got;
    memcpy(&got, large.at(1000, 750), 2);
    CHECK(got == grey);
  }
}

void test_window_ids() {
  unsigned long id = 0;
  CHECK(parse_window_id("0x1A00005", &id) && id == 0x1A00005);
  CHECK(parse_window_id("0x1a00005", &id) && id == 0x1A00005);
  CHECK(parse_window_id(" 0X2c ", &id) && id == 0x2C);
  CHECK(parse_window_id("27262981", &id) && id == 27262981);
  CHECK(parse_window_id("12\n", &id) && id == 12);
  for (const char* bad : {"", " ", "0", "0x", "0x0", "-5", "+5", "12a", "0x12g", "1 2", "0x100000000", "99999999999",
                          "010x"}) {
    CHECK(!parse_window_id(bad, &id));
  }
  CHECK(!parse_window_id(nullptr, &id));
}

void test_keymap() {
  CHECK(vk_for_keysym(XK_a) == 0x41 && vk_for_keysym(XK_Z) == 0x5A && vk_for_keysym(XK_7) == 0x37);
  CHECK(vk_for_keysym(XK_KP_1) == 0x61 && vk_for_keysym(XK_KP_0) == 0x60);   // VK_NUMPAD1, VK_NUMPAD0
  CHECK(vk_for_keysym(XK_KP_End) == 0x23 && vk_for_keysym(XK_KP_Down) == 0x28);   // Num Lock off
  CHECK(vk_for_keysym(XK_KP_Begin) == 0x0C && vk_for_keysym(XK_KP_Enter) == 0x0D);
  CHECK(vk_for_keysym(XK_KP_Multiply) == 0x6A && vk_for_keysym(XK_KP_Divide) == 0x6F);
  CHECK(vk_for_keysym(XK_Num_Lock) == 0x90 && vk_for_keysym(XK_Caps_Lock) == 0x14);
  CHECK(vk_for_keysym(XK_Shift_R) == 0x10 && vk_for_keysym(XK_Control_L) == 0x11);
  CHECK(vk_for_keysym(XK_F10) == 0x79 && vk_for_keysym(XK_F12) == 0x7B);
  CHECK(vk_for_keysym(XK_comma) == 0xBC && vk_for_keysym(XK_minus) == 0xBD && vk_for_keysym(XK_bracketleft) == 0xDB);
  CHECK(vk_for_keysym(XK_Print) == 0x2C && vk_for_keysym(XK_Super_L) == 0x5B);
  CHECK(vk_for_keysym(XK_eacute) == 0);
  CHECK(is_system_key(XK_Alt_L, 0) && is_system_key(XK_F10, 0) && is_system_key(XK_a, Mod1Mask));
  CHECK(!is_system_key(XK_a, 0) && !is_system_key(XK_a, ShiftMask) && !is_system_key(XK_ISO_Level3_Shift, 0));
}

void test_catalog() {
  CHECK((screen_of("640x480") == SizeI{640, 480}) && (screen_of("640X480") == SizeI{640, 480}));
  CHECK((screen_of("000640x480") == SizeI{}) && (screen_of("640x") == SizeI{}) && (screen_of("0x480") == SizeI{}));
  CHECK((screen_of("8193x10") == SizeI{}) && (screen_of("8192x8192") == SizeI{}) && (screen_of("") == SizeI{}));
  Catalog c;
  auto add = [&](const char* id, const char* name, const char* path, const char* same = "") {
    Module m;
    m.id = id;
    m.display_name = m.name = name;
    m.path = path;
    m.same_as = same;
    c.modules.push_back(m);
  };
  add("ad40.toasters", "Flying Toasters!", "FILES/AD40/TOASTERS.AD");
  add("ad10.toasters", "Flying Toasters! (10th Anniversary)", "packages/ad10/AD10TH/TOASTERS.AD", "ad40.toasters");
  add("classic.fish", "Fish", "FILES/CLASSIC/FISH.AD");
  add("ad40.fish", "Fish World", "FILES/AD40/FISH.AD");
  add("startrek.final", "Final Exam", "packages/startrek/AFTERDRK/FINAL.AD");
  std::vector<const Module*> several;
  CHECK(resolve_module(c, "AD40.Toasters", &several) == &c.modules[0]);
  CHECK(resolve_module(c, "toasters", &several) == &c.modules[0]);   // its copy is the same module
  CHECK(resolve_module(c, "Final Exam", &several) == &c.modules[4]);
  CHECK(resolve_module(c, "packages\\startrek\\AFTERDRK\\final.ad", &several) == &c.modules[4]);
  CHECK(resolve_module(c, "fish", &several) == &c.modules[2]);   // the name "Fish" before the id suffix
  CHECK(resolve_module(c, "burns", &several) == nullptr && several.empty());
  add("simpsons.fish", "Fish", "packages/simpsons/SIMPSONS/FISH.AD");
  CHECK(resolve_module(c, "fish", &several) == nullptr && several.size() == 2);   // ambiguous
}

std::string tmpdir() {
  char t[] = "/tmp/lad_unit_XXXXXX";
  return mkdtemp(t);
}

void test_wine_paths() {
  const std::string d = tmpdir();
  const std::string pfx = d + "/pfx";
  CHECK(make_dirs(pfx + "/dosdevices", 0755) && make_dirs(pfx + "/drive_c/users/me", 0755));
  CHECK(symlink("../drive_c", (pfx + "/dosdevices/c:").c_str()) == 0);
  CHECK(symlink("/", (pfx + "/dosdevices/z:").c_str()) == 0);
  CHECK(symlink("/dev/null", (pfx + "/dosdevices/c::").c_str()) == 0);   // a raw device entry, never a drive
  CHECK(to_windows_path(pfx + "/drive_c/users/me", pfx) == "C:\\users\\me");
  CHECK(to_windows_path(pfx + "/drive_c/users/me/new file.txt", pfx) == "C:\\users\\me\\new file.txt");
  CHECK(to_windows_path(pfx + "/drive_c", pfx) == "C:\\");
  CHECK(to_windows_path("/usr/share", pfx) == "Z:\\usr\\share");
  CHECK(to_windows_path(pfx + "/drive_c/../drive_c/users", pfx) == "C:\\users");
  unlink((pfx + "/dosdevices/z:").c_str());
  CHECK(to_windows_path("/usr/share", pfx) == "\\\\?\\unix\\usr\\share");
  // The win dir rule.
  const std::string root = d + "/assets";
  CHECK(make_dirs(root + "/win/packages", 0755));
  CHECK(win_assets_dir(root) == root + "/win");
  CHECK(win_assets_dir(root + "/win") == root + "/win");
  CHECK(win_assets_dir(d + "/nothing") == d + "/nothing/win");
  // Assets where adimport puts them under Wine.
  const std::string imp = pfx + "/drive_c/users/me/AppData/Local/LongAfterDark/assets/win";
  CHECK(make_dirs(imp, 0755));
  std::ofstream(imp + "/catalog-win.json") << "{\"modules\":[]}";
  setenv("USER", "me", 1);
  unsetenv("AD_ASSETS_DIR");
  AssetsSearch s = find_assets("", pfx);
  CHECK(s.has_catalog && !s.explicit_root && s.root == pfx + "/drive_c/users/me/AppData/Local/LongAfterDark/assets");
  AssetsSearch e = find_assets(d + "/nothing", pfx);
  CHECK(!e.has_catalog && e.explicit_root);
  std::string cmd = "rm -rf '" + d + "'";
  CHECK(system(cmd.c_str()) == 0);
}

void test_sound() {
  CHECK(sound_for(true, 70, HostRole::saver, true, false).on);
  CHECK(sound_for(true, 70, HostRole::saver, true, false).volume == 70);
  CHECK(!sound_for(true, 70, HostRole::saver, false, false).on);    // not the primary monitor's
  CHECK(!sound_for(true, 70, HostRole::preview, true, false).on);   // previews are silent
  CHECK(!sound_for(false, 70, HostRole::saver, true, false).on);
  CHECK(!sound_for(true, 70, HostRole::saver, true, true).on);      // AD_SCR_SOUND=0
  CHECK(sound_for(true, 170, HostRole::saver, true, false).volume == 100);
  EnvChanges on{{"ADSOUND", "0"}}, off;
  add_sound_env(on, sound_for(true, 30, HostRole::saver, true, false));
  CHECK(on.size() == 2 && on[0] == std::make_pair(std::string("ADSOUND"), std::string("1")) && on[1].second == "30");
  add_sound_env(off, SoundChoice{});
  CHECK(off.size() == 3 && off[0].second == "0" && off[1].first == "ADVOLUME" && off[1].second.empty() &&
        off[2].first == "ADAUDIOOUT" && off[2].second.empty());
}

// The host's stdin protocol, against a shell script standing in for the
// host: it records its stdin, and prints one frame per GO it reads (with a
// STATUS line on stderr first).
void test_host_protocol() {
  const std::string d = tmpdir();
  const std::string rec = d + "/stdin.txt";
  const std::string script =
      "exec 3>'" + rec + "'\n"
      "n=0\n"
      "while IFS= read -r line; do\n"
      "  printf '%s\\n' \"$line\" >&3\n"
      "  case \"$line\" in\n"
      "    GO) printf 'STATUS %d flags=0x21 applied=%d eaten=0 src=1\\n' $n $n >&2\n"
      "        { printf 'P8\\n2 1\\n'; head -c 770 /dev/zero; }\n"
      "        n=$((n+1));;\n"
      "    QUIT) exit 0;;\n"
      "  esac\n"
      "done\n";
  HostProcess h;
  HostProcess::Spec spec;
  spec.wine = "/bin/sh";
  spec.exe = "-c";
  spec.args = {script};
  std::string err;
  const auto period = std::chrono::milliseconds(20);
  CHECK(h.start(spec, period, &err));
  auto pump = [&](int ms) {
    const auto until = Clock::now() + std::chrono::milliseconds(ms);
    RawFrame f;
    int frames = 0;
    while (Clock::now() < until) {
      std::vector<pollfd> fds;
      size_t base = 0;
      h.add_pollfds(fds, &base);
      poll(fds.data(), fds.size(), 5);
      h.on_poll(fds, base);
      while (h.take_frame(f)) ++frames;
      h.maybe_send_go(Clock::now());
    }
    return frames;
  };
  // Input lines are numbered in order; a MOUSE line with the same buttons
  // replaces the one still waiting and keeps its number.
  CHECK(h.send_input("KEY 65 1") == 1);
  CHECK(h.send_input("MOUSE 1 1 0") == 2);
  CHECK(h.send_input("MOUSE 2 2 0") == 2);
  CHECK(h.send_input("MOUSE 3 3 1") == 3);
  CHECK(h.send_input("CAPS 1") == 4);
  const int frames = pump(500);
  // Paced: about one GO per period, one in flight, never faster.
  CHECK(frames >= 10 && frames <= 26);
  CHECK(h.gos_sent() <= (uint64_t)frames + 1);
  HostStatus s;
  CHECK(h.status(&s) && (s.flags & ADWS_INTERACTIVE) && s.source == 1);
  h.stop(1000);
  CHECK(h.reaped() && WIFEXITED(h.exit_status()) && WEXITSTATUS(h.exit_status()) == 0);   // QUIT, not a signal
  std::ifstream in(rec);
  std::stringstream ss;
  ss << in.rdbuf();
  const std::string got = ss.str();
  CHECK(got.rfind("GO\nKEY 65 1\nMOUSE 2 2 0\nMOUSE 3 3 1\nCAPS 1\n", 0) == 0);
  CHECK(got.find("MOUSE 1 1 0") == std::string::npos);
  CHECK(got.size() >= 5 && got.compare(got.size() - 5, 5, "QUIT\n") == 0);
  // A host that dies ends the stream at once (no hang).
  HostProcess dead;
  spec.args = {"exit 3"};
  CHECK(dead.start(spec, period, &err));
  const auto t0 = Clock::now();
  while (!dead.ended() && Clock::now() - t0 < std::chrono::seconds(5)) {
    std::vector<pollfd> fds;
    size_t base = 0;
    dead.add_pollfds(fds, &base);
    poll(fds.data(), fds.size(), 50);
    dead.on_poll(fds, base);
    dead.check_exit();
  }
  CHECK(dead.ended());
  dead.stop(100);
  CHECK(WIFEXITED(dead.exit_status()) && WEXITSTATUS(dead.exit_status()) == 3);
  // A missing program is an error at start, not a silent host.
  HostProcess missing;
  spec.wine = "/nonexistent/wine";
  CHECK(!missing.start(spec, period, &err) && err.find("cannot run") != std::string::npos);
  // The host above ended on QUIT; one deaf to QUIT, stdin's end and SIGTERM
  // is ended with SIGKILL, and its status is the player's doing.
  CHECK(!h.signalled() && !dead.signalled());
  HostProcess deaf;
  spec.wine = "/bin/sh";
  spec.args = {"trap '' TERM; exec sleep 30"};
  CHECK(deaf.start(spec, period, &err));
  deaf.stop(100);
  CHECK(deaf.reaped() && deaf.signalled() && WIFSIGNALED(deaf.exit_status()) && WTERMSIG(deaf.exit_status()) == SIGKILL);
  std::string cmd = "rm -rf '" + d + "'";
  CHECK(system(cmd.c_str()) == 0);
}

// When a host starts again (restart.h: the Windows saver's rule).
void test_restart() {
  using std::chrono::milliseconds;
  using std::chrono::seconds;
  const auto quick = milliseconds(300);
  {
    // A module that never shows a frame, alone: 0.5, 1, 2, 4, 8, 16, then 30
    // s between tries, and the tries never stop; the message from the third
    // failed run on.
    RestartRule r;
    const long want[] = {500, 1000, 2000, 4000, 8000, 16000, 30000, 30000, 30000, 30000};
    for (int i = 0; i < 10; ++i) {
      const RestartStep s = r.host_ended(0, quick, false, 1);
      CHECK(s.delay == milliseconds(want[i]) && s.message == (i >= 2) && !s.skip);
    }
    CHECK(r.failures() == 10);
  }
  {
    // Short runs with frames are failed runs, without the message; a run
    // without a frame after three of them brings it, and restarts go on.
    RestartRule r;
    CHECK(!r.host_ended(90, milliseconds(1900), false, 1).message);
    CHECK(!r.host_ended(79, milliseconds(1900), false, 1).message);
    const RestartStep third = r.host_ended(40, milliseconds(1900), false, 1);
    CHECK(!third.message && third.delay == milliseconds(2000));
    const RestartStep fourth = r.host_ended(0, milliseconds(1), false, 1);
    CHECK(fourth.message && !fourth.skip && fourth.delay == milliseconds(4000));
    // Frames for 5 s or more is a good run: the count starts again, 250 ms.
    const RestartStep good = r.host_ended(300, seconds(6), false, 1);
    CHECK(!good.message && good.delay == milliseconds(250) && r.failures() == 0);
    CHECK(r.host_ended(1000, milliseconds(4999), false, 1).delay == milliseconds(500));   // frames, under 5 s
    CHECK(r.host_ended(1, seconds(5), false, 1).delay == milliseconds(250));              // 5 s with frames
    CHECK(r.host_ended(0, seconds(60), false, 1).delay == milliseconds(500));             // long, no frame
  }
  {
    // A rotation of three: a module is skipped after three failed runs (with
    // the message when the last had no frame), the next starts 250 ms later.
    RestartRule r;
    for (int i = 0; i < 2; ++i) CHECK(!r.host_ended(0, quick, true, 3).skip);
    RestartStep s = r.host_ended(0, quick, true, 3);
    CHECK(s.skip && s.message && s.delay == milliseconds(250) && r.failures() == 0 && r.dead_modules() == 1);
    for (int i = 0; i < 2; ++i) r.host_ended(0, quick, true, 3);
    s = r.host_ended(0, quick, true, 3);
    CHECK(s.skip && s.delay == milliseconds(250) && r.dead_modules() == 2);
    // Every module skipped without a frame: 30 s before the next try.
    for (int i = 0; i < 2; ++i) r.host_ended(0, quick, true, 3);
    s = r.host_ended(0, quick, true, 3);
    CHECK(s.skip && s.message && s.delay == seconds(30) && r.dead_modules() == 3);
    // A frame ends the run of dead modules; a module skipped after short
    // runs with frames is no dead one, and has no message.
    r.frame_shown();
    CHECK(r.dead_modules() == 0);
    for (int i = 0; i < 2; ++i) r.host_ended(5, quick, true, 3);
    s = r.host_ended(5, quick, true, 3);
    CHECK(s.skip && !s.message && r.dead_modules() == 0 && s.delay == milliseconds(250));
  }
  {
    // A host that can't be spawned: a second more per failure, 30 s at most;
    // another module counts afresh.
    RestartRule r;
    CHECK(r.spawn_failed() == milliseconds(1000) && r.spawn_failed() == milliseconds(2000));
    for (int i = 0; i < 40; ++i) r.spawn_failed();
    CHECK(r.spawn_failed() == milliseconds(30000));
    r.new_module();
    CHECK(r.failures() == 0 && r.host_ended(0, quick, false, 1).delay == milliseconds(500));
  }
}

// The monitors a full-screen window leaves to black windows (monitors.h).
void test_monitors() {
  auto mon = [](int x, int y, int w, int h, bool primary = false) {
    MonitorInfo m;
    m.rect = RectI{x, y, w, h};
    m.primary = primary;
    return m;
  };
  using Rects = std::vector<RectI>;
  const RectI left{0, 0, 1280, 720};
  CHECK((monitors_to_cover({mon(0, 0, 1280, 720, true), mon(1280, 0, 1280, 720)}, left) == Rects{{1280, 0, 1280, 720}}));
  CHECK(monitors_to_cover({mon(0, 0, 1280, 720, true)}, left).empty());
  // A clone, or a monitor sharing a part of the window's: never covered.
  CHECK(monitors_to_cover({mon(0, 0, 1280, 720, true), mon(0, 0, 1280, 720)}, left).empty());
  CHECK(monitors_to_cover({mon(0, 0, 1280, 720, true), mon(0, 0, 1024, 768), mon(1000, 700, 800, 600)}, left).empty());
  // Left of it (negative x) and above it; one listed twice is covered once.
  CHECK((monitors_to_cover({mon(-1920, 0, 1920, 1080), mon(0, 0, 1280, 720, true), mon(0, -1080, 1920, 1080),
                            mon(-1920, 0, 1920, 1080)},
                           left) == Rects{{-1920, 0, 1920, 1080}, {0, -1080, 1920, 1080}}));
  // Edges that touch don't overlap; an empty rectangle is no monitor.
  CHECK((monitors_to_cover({mon(1280, 0, 800, 600), mon(0, 720, 1280, 720), mon(3000, 0, 0, 600)}, left) ==
         Rects{{1280, 0, 800, 600}, {0, 720, 1280, 720}}));
}

// The command line (options.h), --module above all.
void test_options() {
  auto parse = [](std::vector<std::string> args, Options* out) {
    std::string name = "longafterdark";
    std::vector<char*> argv = {name.data()};
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    *out = Options{};
    // Its complaints go to stderr: out of the way of the tests' output.
    fflush(stderr);
    const int saved = dup(2), devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (devnull >= 0) dup2(devnull, 2);
    const int r = parse_options((int)argv.size() - 1, argv.data(), *out);
    fflush(stderr);
    if (saved >= 0) {
      dup2(saved, 2);
      close(saved);
    }
    if (devnull >= 0) close(devnull);
    return r;
  };
  unsetenv("XSCREENSAVER_WINDOW");
  Options o;
  CHECK(parse({"--module", "ad40.toasters"}, &o) == -1 && o.module == "ad40.toasters" && o.mode == WindowMode::fullscreen);
  CHECK(parse({"--root", "--module", "Flying Toasters!"}, &o) == -1 && o.module == "Flying Toasters!" &&
        o.mode == WindowMode::root);
  CHECK(parse({"toasters"}, &o) == -1 && o.module == "toasters");
  // Empty names none, as leaving it out does.
  CHECK(parse({"--root", "--module", ""}, &o) == -1 && o.module.empty());
  // The same module twice is one; two are refused, however they are named.
  CHECK(parse({"toasters", "--module", "toasters"}, &o) == -1 && o.module == "toasters");
  CHECK(parse({"--module", "toasters", "toasters"}, &o) == -1 && o.module == "toasters");
  CHECK(parse({"--module", "toasters", "fish"}, &o) == kExitUsage);
  CHECK(parse({"fish", "--module", "toasters"}, &o) == kExitUsage);
  CHECK(parse({"--module", "a", "--module", "b"}, &o) == kExitUsage);
  CHECK(parse({"fish", "toasters"}, &o) == kExitUsage);
  CHECK(parse({"--module"}, &o) == kExitUsage);
  // With the modes, and $XSCREENSAVER_WINDOW's.
  CHECK(parse({"--window-id", "0x2a", "--module", "fish"}, &o) == -1 && o.mode == WindowMode::embed &&
        o.window_id == 0x2a && o.module == "fish");
  setenv("XSCREENSAVER_WINDOW", "0x400001", 1);
  CHECK(parse({"--module", "fish"}, &o) == -1 && o.mode == WindowMode::root && o.root_from_env);
  CHECK(parse({"-f", "--module", "fish"}, &o) == -1 && o.mode == WindowMode::fullscreen && !o.root_from_env);
  unsetenv("XSCREENSAVER_WINDOW");
  CHECK(parse({"--cycle", "30", "--lines", "720", "--no-sound", "--volume", "30"}, &o) == -1 && o.cycle_s == 30 &&
        o.lines == 720 && !o.sound && o.volume == 30);
  CHECK(parse({"--lines", "600"}, &o) == kExitUsage && parse({"--bogus"}, &o) == kExitUsage);
}

// A child's nice value: at least the one asked for, never a higher priority
// than the player's (host.h).
void test_nice() {
  errno = 0;
  const int mine = getpriority(PRIO_PROCESS, 0);
  CHECK(errno == 0);
  auto child_nice = [](int nice) {
    SpawnSpec s;
    s.program = "/bin/sh";
    s.argv = {"sh", "-c", "nice"};
    s.pipe_stdin = false;
    s.pipe_stderr = false;
    s.nice = nice;
    Child c;
    std::string err, out;
    if (!spawn_child(s, c, &err)) return -100;
    char buf[64];
    for (int i = 0; i < 500; ++i) {
      pollfd p{c.out, POLLIN, 0};
      poll(&p, 1, 10);
      const ssize_t n = read(c.out, buf, sizeof(buf));
      if (n == 0) break;
      if (n > 0) out.append(buf, (size_t)n);
    }
    close(c.out);
    int st;
    waitpid(c.pid, &st, 0);
    return out.empty() ? -100 : atoi(out.c_str());
  };
  CHECK(child_nice(0) == mine);
  CHECK(child_nice(10) == std::max(mine, 10));
  CHECK(child_nice(5) == std::max(mine, 5));
  CHECK(getpriority(PRIO_PROCESS, 0) == mine);   // the player's own is untouched
  // A host's, and what its program starts.
  const std::string d = tmpdir();
  HostProcess h;
  HostProcess::Spec spec;
  spec.wine = "/bin/sh";
  spec.exe = "-c";
  spec.args = {"sh -c nice > '" + d + "/nice.tmp' && mv '" + d + "/nice.tmp' '" + d + "/nice'; exec cat >/dev/null"};
  spec.nice = 10;
  std::string err;
  CHECK(h.start(spec, std::chrono::milliseconds(20), &err));
  std::string got;
  for (int i = 0; i < 300 && got.empty(); ++i) {
    std::ifstream in(d + "/nice");
    std::getline(in, got);
    if (got.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  h.stop(500);
  CHECK(!got.empty() && atoi(got.c_str()) == std::max(mine, 10));
  std::string cmd = "rm -rf '" + d + "'";
  CHECK(system(cmd.c_str()) == 0);
}

}  // namespace

int main() {
  test_frame_parser();
  test_status();
  test_decide();
  test_lines_and_mapping();
  test_geometry();
  test_pixels();
  test_window_ids();
  test_keymap();
  test_catalog();
  test_wine_paths();
  test_sound();
  test_host_protocol();
  test_restart();
  test_monitors();
  test_options();
  test_nice();
  printf("%d checks, %d failed\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
