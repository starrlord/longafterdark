// The X side of the player: the window it draws in and how a frame gets
// there, as the Windows saver presents (scr/src/saver.cc) where X allows.
//
// Where:
//  * full screen (the default): a window of its own over the primary
//    monitor (monitors.h), asked to be full screen, and a black window over
//    each other monitor (the Windows saver's "primary monitor only"), the
//    pointer hidden over them too;
//  * -w: an ordinary window, 640x480 times --scale;
//  * -root: the window XScreenSaver draws its hacks in, found as its vroot.h
//    finds it: $XSCREENSAVER_WINDOW (hex or decimal), else a top-level
//    window carrying __SWM_VROOT (a virtual root), else the root window.
//    When $XSCREENSAVER_WINDOW is set but names no window the player stops
//    with a message: it never draws on the real root beneath XScreenSaver's
//    window;
//  * -window-id: someone else's window (xscreensaver-settings' preview).
// A window it did not make is followed through its StructureNotify events
// (a new size; its end ends the player).
//
// How:
//  * each frame is fitted into the window keeping its shape (fit_rect, the
//    Windows saver's letterbox: bars at the sides or above and below, drawn
//    black), scaled nearest-neighbour (pixels.h) into an image in the
//    window's own visual: any TrueColor visual at 16, 24 or 32 bits per
//    pixel by its channel masks; other visuals are refused with a message;
//  * the image goes to the server through MIT-SHM when the server shares
//    memory with the player: a segment only its owner can use (0600),
//    marked for removal as soon as the server has attached it (attached or
//    not), detached and removed on every failure path and at every resize;
//    a frame is written into it only once the server has read the last
//    (ShmCompletion), so a frame is never torn. Otherwise, and when XShm
//    fails once, XPutImage into a pixmap, shown with one copy (Xlib splits
//    a big XPutImage into several requests, which the screen could show
//    half done);
//  * a frame byte-identical to the one on the screen (a host repeats its
//    last when the module drew nothing) costs a compare, not a redraw.
#pragma once

#include <X11/Xlib.h>
#include <X11/extensions/XShm.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "frame_parser.h"
#include "geometry.h"
#include "monitors.h"
#include "pixels.h"

namespace lad {

enum class WindowMode {
  fullscreen,   // the saver: a full-screen window of its own (the default)
  window,       // -w: an ordinary window (640x480, or times --scale)
  root,         // -root: XScreenSaver's window, the virtual root, or the root window
  embed,        // -window-id: someone else's window (XScreenSaver's preview)
};

// A window id as XScreenSaver hands one over ($XSCREENSAVER_WINDOW,
// --window-id 0x%X): hex after "0x" or "0X", else decimal digits, with
// spaces around allowed. False for anything else, for 0 and for a value
// past 32 bits (X ids are 29-bit).
bool parse_window_id(const char* s, unsigned long* out);

class Presenter {
 public:
  using Clock = std::chrono::steady_clock;

  ~Presenter();
  // Opens (or adopts) the window; false with *error when it can't, or when
  // its visual is one the player can't draw in.
  bool open(Display* d, WindowMode mode, unsigned long embed_id, int scale, std::string* error);
  void close();

  Window window() const { return win_; }
  int screen() const { return screen_; }
  bool owns_window() const { return own_; }
  // A point of the root window in the window's own coordinates (the
  // window's place asked of the server again after it has moved).
  PointI root_to_window(PointI p);
  // Full screen: the black windows over the other monitors, from where the
  // window really is (a window manager may put it on another monitor than
  // the one asked for): one over each monitor it doesn't overlap, none over
  // one it does. Done when it opens, and again when it is mapped and each
  // time it moves or changes size.
  void update_covers();
  Atom wm_delete() const { return wm_delete_; }
  int width() const { return win_w_; }
  int height() const { return win_h_; }
  // What the window is and how the player draws in it, for the log.
  std::string describe() const;

  // The events the presenter follows: the window's ConfigureNotify, Expose
  // and DestroyNotify, and the XShm completion. True when the event was its
  // own; *gone when the window has been destroyed.
  bool handle_event(const XEvent& ev, bool* gone);
  // A frame waiting for a completion that never came is drawn after 250 ms.
  void tick(Clock::time_point now);
  std::optional<Clock::time_point> next_tick() const;

  // The emulated screen of the host whose frames come next: where they will
  // land (frame_rect) before the first of them arrives.
  void set_screen(SizeI emu);
  // Frames fill the whole window instead of keeping their shape (--stretch,
  // for a module with a screen of its own); set before set_screen.
  void set_stretch(bool on) { stretch_ = on; }
  // Where frames land in the window: the letterbox of the last frame, else
  // of the screen set, else the whole window.
  RectI frame_rect() const;

  // True when this window holds the centre of the X display's primary
  // monitor (the one sound instance among XScreenSaver's per-monitor
  // windows); *detail says why, for the log.
  bool covers_primary_monitor(std::string* detail) const;

  // Draws a new frame; false when it was the same as the one shown (nothing
  // redrawn). A frame that comes while the server still reads the last one
  // is drawn when it has.
  bool present(const RawFrame& f);
  // Draws the window again (Expose).
  void redraw();
  // Black, with `text` in the middle ("" for just black); frames replace it.
  void show_message(const std::string& text);
  void clear() { show_message(""); }

  // The pointer over the window (a window of its own): hidden (the saver's
  // default) or an arrow.
  void set_cursor_visible(bool visible);
  // A pointer grab's confine_to while a game plays: an InputOnly child over
  // the frame (the Windows saver's ClipCursor to the frame rectangle),
  // mapped by this call and following the letterbox; None when the window
  // is not the player's own. hide_clip_window() unmaps it.
  Window clip_window();
  void hide_clip_window();

 private:
  bool choose_format(Visual* v, int depth, std::string* why);
  bool create_own_window(WindowMode mode, int scale, std::string* error);
  std::string covers_text() const;
  bool adopt_window(Window w, const std::string& what, std::string* error);
  void load_font();
  // The image for a frame of this size: -1 when it can't be made, 0 when
  // the one there fits, 1 when it was made anew (bars to draw again).
  int layout(int frame_w, int frame_h);
  bool alloc_image(int w, int h);
  bool alloc_shm_image(int w, int h, std::string* why);
  void free_image();
  void draw_frame();
  void put_image();
  void show_image();
  void fill_bars();
  void draw_message();
  void place_clip_window();
  bool busy() const { return puts_outstanding_ > 0; }

  Display* d_ = nullptr;
  int screen_ = 0;
  Window win_ = 0;
  bool own_ = false;
  WindowMode mode_ = WindowMode::fullscreen;
  std::string target_;              // "a full-screen window", "XScreenSaver's window 0x... ($XSCREENSAVER_WINDOW)"
  Atom wm_delete_ = 0;
  GC gc_ = nullptr;
  Visual* visual_ = nullptr;
  int depth_ = 0;
  Colormap own_colormap_ = 0;
  PixelFormat pf_{};
  unsigned long black_ = 0, white_ = 0;
  int win_w_ = 0, win_h_ = 0;
  RectI placed_{};                  // a window of its own: where it was put (full screen: the primary monitor)
  // Full screen: the monitors, and the black windows over those the window
  // leaves.
  bool cover_ = false;
  std::vector<MonitorInfo> monitors_;
  struct Cover {
    RectI rect;
    Window w = 0;
  };
  std::vector<Cover> covers_;
  bool covers_logged_ = false;      // describe() has told the first ones; changes are logged
  PointI origin_{};                 // the window's place on the root window...
  bool origin_known_ = false;       // ...when known (asked again after a ConfigureNotify)

  // The image: the size of the frame's letterbox rectangle.
  XImage* image_ = nullptr;
  int img_w_ = 0, img_h_ = 0;
  RectI fit_{};
  bool shm_ok_ = false;             // the server has MIT-SHM and it has not failed
  bool use_shm_ = false;            // the image is in a shared segment
  Pixmap pixmap_ = 0;               // XPutImage's way: frames are put in it, then copied to the window whole
  XShmSegmentInfo shm_{};           // image_->obdata points here: kept as long as the image
  int shm_completion_ = -1;         // the completion event's type
  int puts_outstanding_ = 0;        // XShmPutImage requests not yet completed
  Clock::time_point put_at_{};
  bool pending_ = false;            // last_ is newer than the image: drawn on completion
  bool missing_completion_logged_ = false;
  FrameScaler scaler_;

  RawFrame last_;                   // the frame on the screen (or waiting for it)
  bool have_last_ = false;
  SizeI screen_emu_{};
  bool stretch_ = false;   // set_stretch

  std::string message_;
  bool showing_message_ = false;
  XFontStruct* font_ = nullptr;
  bool small_font_ = false;
  Cursor blank_cursor_ = 0, arrow_cursor_ = 0;
  bool cursor_visible_ = true;
  Window clip_win_ = 0;
  bool clip_mapped_ = false;
};

}  // namespace lad
