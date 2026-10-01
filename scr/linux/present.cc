#include "present.h"

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "log.h"
#include "monitors.h"

namespace lad {

namespace {

using namespace std::chrono_literals;

// A completion that has not come this long after its put is taken as lost
// (a put the server refused sends none): the next frame is drawn anyway.
constexpr auto kCompletionWait = 250ms;

// X errors of a request whose failure is handled (XShmAttach, a pixmap):
// errors from its serial on are its own; anything older is someone else's
// and goes on to the handler that was installed.
XErrorHandler g_prev_handler = nullptr;
unsigned long g_trap_serial = 0;
bool g_trapped = false;
int g_trapped_code = 0;
int trap_handler(Display* d, XErrorEvent* e) {
  if (e->serial >= g_trap_serial) {
    if (!g_trapped) g_trapped_code = e->error_code;
    g_trapped = true;
    return 0;
  }
  return g_prev_handler ? g_prev_handler(d, e) : 0;
}
void begin_trap(Display* d) {
  g_trapped = false;
  g_trapped_code = 0;
  g_trap_serial = NextRequest(d);
  g_prev_handler = XSetErrorHandler(trap_handler);
}
// XSync, then the handler back; the error's text ("" when none).
std::string end_trap(Display* d) {
  XSync(d, False);
  XSetErrorHandler(g_prev_handler);
  g_prev_handler = nullptr;
  if (!g_trapped) return "";
  char text[80] = "";
  XGetErrorText(d, g_trapped_code, text, sizeof(text));
  return text[0] ? text : "an X error";
}

bool same_frame(const RawFrame& a, const RawFrame& b) {
  return a.width == b.width && a.height == b.height && a.format == b.format && a.palette == b.palette &&
         a.pixels == b.pixels;
}

std::string hex_id(unsigned long id) {
  char buf[32];
  snprintf(buf, sizeof(buf), "0x%lx", id);
  return buf;
}

// Where -root draws, as XScreenSaver's vroot.h finds it.
bool find_root_target(Display* d, int screen, Window* win, std::string* what, std::string* error) {
  if (const char* env = getenv("XSCREENSAVER_WINDOW"); env && *env) {
    unsigned long id = 0;
    if (!parse_window_id(env, &id)) {
      *error = std::string("XSCREENSAVER_WINDOW=\"") + env +
               "\" is not a window id; not drawing on the root window beneath XScreenSaver's";
      return false;
    }
    *win = (Window)id;
    *what = "XScreenSaver's window " + hex_id(id) + " ($XSCREENSAVER_WINDOW)";
    return true;
  }
  const Window root = RootWindow(d, screen);
  const Atom vroot = XInternAtom(d, "__SWM_VROOT", False);
  Window root_ret = 0, parent = 0, *kids = nullptr;
  unsigned int n = 0;
  Window found = 0;
  if (XQueryTree(d, root, &root_ret, &parent, &kids, &n)) {
    for (unsigned int i = 0; i < n && !found; ++i) {
      Atom type = None;
      int format = 0;
      unsigned long items = 0, after = 0;
      unsigned char* data = nullptr;
      if (XGetWindowProperty(d, kids[i], vroot, 0, 1, False, XA_WINDOW, &type, &format, &items, &after, &data) ==
              Success &&
          data) {
        if (type == XA_WINDOW && format == 32 && items == 1) found = *reinterpret_cast<Window*>(data);
        XFree(data);
      }
    }
    if (kids) XFree(kids);
  }
  if (found) {
    *win = found;
    *what = "the virtual root " + hex_id(found) + " (__SWM_VROOT)";
    return true;
  }
  *win = root;
  *what = "the root window";
  return true;
}

}  // namespace

bool parse_window_id(const char* s, unsigned long* out) {
  if (!s) return false;
  while (*s == ' ' || *s == '\t') ++s;
  const bool hex = s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
  if (hex) s += 2;
  unsigned long long v = 0;
  int digits = 0;
  for (;; ++s, ++digits) {
    int d;
    if (*s >= '0' && *s <= '9') d = *s - '0';
    else if (hex && *s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
    else if (hex && *s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
    else break;
    v = v * (hex ? 16 : 10) + (unsigned)d;
    if (v > 0xFFFFFFFFull) return false;
  }
  while (*s == ' ' || *s == '\t' || *s == '\n') ++s;
  if (*s || digits == 0 || v == 0) return false;
  if (out) *out = (unsigned long)v;
  return true;
}

Presenter::~Presenter() { close(); }

bool Presenter::choose_format(Visual* v, int depth, std::string* why) {
  // The image's bits per pixel and byte order for this depth, from a probe
  // image (what XCreateImage and XShmCreateImage will make).
  XImage* probe = XCreateImage(d_, v, (unsigned)depth, ZPixmap, 0, nullptr, 1, 1, 32, 0);
  if (!probe) {
    if (why) *why = "a visual of depth " + std::to_string(depth) + " that X makes no image for";
    return false;
  }
  const int bpp = probe->bits_per_pixel;
  const bool msb = probe->byte_order == MSBFirst;
  XDestroyImage(probe);
  PixelFormat pf;
  if (!make_pixel_format(v->c_class, depth, bpp, msb, v->red_mask, v->green_mask, v->blue_mask, &pf, why)) return false;
  pf_ = pf;
  visual_ = v;
  depth_ = depth;
  black_ = pack_rgb(pf_, 0, 0, 0);
  white_ = pack_rgb(pf_, 255, 255, 255);
  return true;
}

bool Presenter::open(Display* d, WindowMode mode, unsigned long embed_id, int scale, std::string* error) {
  d_ = d;
  mode_ = mode;
  screen_ = DefaultScreen(d);
  std::string err;
  bool ok = false;
  if (mode == WindowMode::embed) {
    ok = adopt_window((Window)embed_id, "the window " + hex_id(embed_id) + " (--window-id)", &err);
  } else if (mode == WindowMode::root) {
    Window w = 0;
    std::string what;
    ok = find_root_target(d, screen_, &w, &what, &err) && adopt_window(w, what, &err);
  } else {
    ok = create_own_window(mode, scale, &err);
  }
  if (!ok) {
    if (error) *error = err;
    return false;
  }
  XGCValues gv{};
  gv.graphics_exposures = False;
  gv.foreground = white_;
  gv.background = black_;
  gc_ = XCreateGC(d, win_, GCGraphicsExposures | GCForeground | GCBackground, &gv);
  load_font();
  if (own_) {
    // The saver hides the pointer; an arrow when a game asks for one.
    static char zero[8] = {0};
    Pixmap blank = XCreateBitmapFromData(d, win_, zero, 1, 1);
    XColor black{};
    blank_cursor_ = XCreatePixmapCursor(d, blank, blank, &black, &black, 0, 0);
    XFreePixmap(d, blank);
    arrow_cursor_ = XCreateFontCursor(d, XC_left_ptr);
    if (mode == WindowMode::fullscreen) {
      cover_ = true;
      monitors_ = list_monitors(d, screen_);
      update_covers();
    }
  }
  int major = 0, minor = 0;
  Bool pixmaps = False;
  shm_ok_ = XShmQueryExtension(d) && XShmQueryVersion(d, &major, &minor, &pixmaps);
  if (shm_ok_) shm_completion_ = XShmGetEventBase(d) + ShmCompletion;
  XFlush(d);
  return true;
}

bool Presenter::create_own_window(WindowMode mode, int scale, std::string* error) {
  // The default visual when the player can draw in it, else another
  // TrueColor visual of the screen (the window then has a colormap of its
  // own); none: a clear refusal.
  std::string why;
  if (!choose_format(DefaultVisual(d_, screen_), DefaultDepth(d_, screen_), &why)) {
    bool found = false;
    for (int depth : {24, 32, 30, 16, 15}) {
      XVisualInfo vi{};
      std::string ignored;
      if (XMatchVisualInfo(d_, screen_, depth, TrueColor, &vi) && choose_format(vi.visual, vi.depth, &ignored)) {
        found = true;
        break;
      }
    }
    if (!found) {
      *error = "this X display's default visual is " + why +
               ", and it has no TrueColor visual at 16, 24 or 32 bits per pixel: Long After Dark cannot draw on it";
      return false;
    }
  }
  const Window root = RootWindow(d_, screen_);
  RectI r;
  if (mode == WindowMode::fullscreen) {
    const MonitorInfo m = primary_monitor(d_, screen_);
    r = m.rect;
    target_ = "a full-screen window on " + m.how;
  } else {
    const int s = scale > 0 ? scale : 1;
    r = RectI{0, 0, 640 * s, 480 * s};
    target_ = "a window";
  }
  win_w_ = std::max(1, r.w);
  win_h_ = std::max(1, r.h);
  placed_ = r;
  XSetWindowAttributes swa{};
  unsigned long mask = CWBackPixel | CWEventMask | CWBorderPixel;
  swa.background_pixel = black_;
  swa.border_pixel = black_;
  swa.event_mask = StructureNotifyMask | KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask |
                   PointerMotionMask | LeaveWindowMask | ExposureMask | FocusChangeMask;
  if (visual_ != DefaultVisual(d_, screen_)) {
    own_colormap_ = XCreateColormap(d_, root, visual_, AllocNone);
    swa.colormap = own_colormap_;
    mask |= CWColormap;
  }
  win_ = XCreateWindow(d_, root, r.x, r.y, (unsigned)win_w_, (unsigned)win_h_, 0, depth_, InputOutput, visual_, mask,
                       &swa);
  own_ = true;
  XStoreName(d_, win_, "Long After Dark");
  if (XClassHint* hint = XAllocClassHint()) {
    hint->res_name = const_cast<char*>("longafterdark");
    hint->res_class = const_cast<char*>("LongAfterDark");
    XSetClassHint(d_, win_, hint);
    XFree(hint);
  }
  if (XSizeHints* sh = XAllocSizeHints()) {
    // Where it was put (the primary monitor): a window manager making it
    // full screen keeps it on that monitor.
    sh->flags = USPosition | USSize | PPosition | PSize;
    sh->x = r.x;
    sh->y = r.y;
    sh->width = win_w_;
    sh->height = win_h_;
    XSetWMNormalHints(d_, win_, sh);
    XFree(sh);
  }
  wm_delete_ = XInternAtom(d_, "WM_DELETE_WINDOW", False);
  XSetWMProtocols(d_, win_, &wm_delete_, 1);
  if (mode == WindowMode::fullscreen) {
    Atom wm_state = XInternAtom(d_, "_NET_WM_STATE", False);
    Atom fullscreen = XInternAtom(d_, "_NET_WM_STATE_FULLSCREEN", False);
    XChangeProperty(d_, win_, wm_state, XA_ATOM, 32, PropModeReplace, reinterpret_cast<unsigned char*>(&fullscreen), 1);
    // A compositing window manager may leave a full-screen window out of
    // its composition (no extra copy of every frame).
    long bypass = 1;
    XChangeProperty(d_, win_, XInternAtom(d_, "_NET_WM_BYPASS_COMPOSITOR", False), XA_CARDINAL, 32, PropModeReplace,
                    reinterpret_cast<unsigned char*>(&bypass), 1);
  }
  XMapRaised(d_, win_);
  return true;
}

// The Windows saver's "primary monitor only": the other monitors black, the
// pointer hidden over them, and their clicks and moves the player's (the
// App takes them as on its own window). Override-redirect, so no window
// manager places, decorates or focuses them (the keyboard stays with the
// player's window) and nothing stacks above them: so never over a monitor
// the player's window is on, wherever the window manager put it.
void Presenter::update_covers() {
  if (!cover_ || !d_ || !win_) return;
  const Window root = RootWindow(d_, screen_);
  RectI at = placed_;
  Window child = 0;
  int x = 0, y = 0;
  if (XTranslateCoordinates(d_, win_, root, 0, 0, &x, &y, &child)) at = RectI{x, y, win_w_, win_h_};
  const std::vector<RectI> want = monitors_to_cover(monitors_, at);
  bool changed = false;
  for (size_t i = 0; i < covers_.size();) {
    if (std::find(want.begin(), want.end(), covers_[i].rect) == want.end()) {
      XDestroyWindow(d_, covers_[i].w);
      covers_.erase(covers_.begin() + (long)i);
      changed = true;
    } else {
      ++i;
    }
  }
  for (const RectI& r : want) {
    if (std::any_of(covers_.begin(), covers_.end(), [&](const Cover& c) { return c.rect == r; })) continue;
    XSetWindowAttributes a{};
    a.override_redirect = True;
    a.background_pixel = BlackPixel(d_, screen_);
    a.border_pixel = BlackPixel(d_, screen_);
    a.event_mask = KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask;
    a.cursor = blank_cursor_;
    const Window w = XCreateWindow(d_, root, r.x, r.y, (unsigned)r.w, (unsigned)r.h, 0, CopyFromParent, InputOutput,
                                   CopyFromParent, CWOverrideRedirect | CWBackPixel | CWBorderPixel | CWEventMask | CWCursor,
                                   &a);
    if (!w) continue;
    XStoreName(d_, w, "Long After Dark");
    if (XClassHint* hint = XAllocClassHint()) {
      hint->res_name = const_cast<char*>("longafterdark");
      hint->res_class = const_cast<char*>("LongAfterDark");
      XSetClassHint(d_, w, hint);
      XFree(hint);
    }
    XMapRaised(d_, w);
    covers_.push_back(Cover{r, w});
    changed = true;
  }
  // (The first time, describe() tells it.)
  if (changed && covers_logged_) {
    log_line("window: at %d,%d %dx%d%s", at.x, at.y, at.w, at.h,
             covers_.empty() ? "; no monitor left for black" : covers_text().c_str());
  }
  covers_logged_ = true;
}

std::string Presenter::covers_text() const {
  std::string s;
  for (const Cover& c : covers_) {
    s += (s.empty() ? "; black over the other monitors: " : ", ") + std::to_string(c.rect.w) + "x" +
         std::to_string(c.rect.h) + "+" + std::to_string(c.rect.x) + "+" + std::to_string(c.rect.y);
  }
  return s;
}

PointI Presenter::root_to_window(PointI p) {
  if (!origin_known_ && d_ && win_) {
    Window child = 0;
    int x = 0, y = 0;
    if (XTranslateCoordinates(d_, win_, RootWindow(d_, screen_), 0, 0, &x, &y, &child)) {
      origin_ = PointI{x, y};
      origin_known_ = true;
    }
  }
  return PointI{p.x - origin_.x, p.y - origin_.y};
}

bool Presenter::adopt_window(Window w, const std::string& what, std::string* error) {
  XWindowAttributes wa;
  if (!w || !XGetWindowAttributes(d_, w, &wa)) {
    *error = what + " does not exist";
    return false;
  }
  target_ = what;
  win_ = w;
  own_ = false;
  win_w_ = std::max(1, wa.width);
  win_h_ = std::max(1, wa.height);
  screen_ = XScreenNumberOfScreen(wa.screen);
  std::string why;
  if (!choose_format(wa.visual, wa.depth, &why)) {
    *error = "cannot draw in " + what + ": it has " + why;
    return false;
  }
  // Its new sizes and its end (StructureNotify), and what needs drawing
  // again (Expose). Both are events any number of clients may select on
  // a window, so the owner's own selection is untouched.
  XSelectInput(d_, win_, StructureNotifyMask | ExposureMask);
  return true;
}

void Presenter::load_font() {
  // A preview is small: a small font, so a message fits.
  const bool small = win_h_ < 360;
  if (font_ && small == small_font_) return;
  if (font_) XFreeFont(d_, font_);
  font_ = nullptr;
  small_font_ = small;
  static const char* const kLarge[] = {"-misc-fixed-bold-r-normal--18-*-*-*-*-*-iso8859-1", "10x20", "9x15", "fixed"};
  static const char* const kSmall[] = {"6x10", "5x8", "6x13", "fixed"};
  for (const char* name : small ? kSmall : kLarge) {
    if ((font_ = XLoadQueryFont(d_, name))) break;
  }
  if (font_ && gc_) XSetFont(d_, gc_, font_->fid);
}

void Presenter::close() {
  if (!d_) return;
  free_image();
  if (font_) XFreeFont(d_, font_);
  font_ = nullptr;
  if (blank_cursor_) XFreeCursor(d_, blank_cursor_);
  if (arrow_cursor_) XFreeCursor(d_, arrow_cursor_);
  blank_cursor_ = arrow_cursor_ = 0;
  if (gc_) XFreeGC(d_, gc_);
  gc_ = nullptr;
  if (clip_win_) XDestroyWindow(d_, clip_win_);
  clip_win_ = 0;
  for (const Cover& c : covers_) XDestroyWindow(d_, c.w);
  covers_.clear();
  cover_ = false;
  if (own_ && win_) XDestroyWindow(d_, win_);
  win_ = 0;   // a window of someone else's keeps its own selections; ours go with the connection
  if (own_colormap_) XFreeColormap(d_, own_colormap_);
  own_colormap_ = 0;
  XFlush(d_);
  d_ = nullptr;
}

std::string Presenter::describe() const {
  const std::string visual = hex_id(visual_ ? XVisualIDFromVisual(visual_) : 0ul);
  return target_ + ", " + std::to_string(win_w_) + "x" + std::to_string(win_h_) + ", visual " + visual + " " +
         describe_format(pf_) + (shm_ok_ ? ", MIT-SHM" : ", XPutImage (no MIT-SHM)") + covers_text();
}

void Presenter::set_cursor_visible(bool visible) {
  if (!own_ || !d_ || visible == cursor_visible_) return;
  cursor_visible_ = visible;
  XDefineCursor(d_, win_, visible ? arrow_cursor_ : blank_cursor_);
}

// ---- the image -------------------------------------------------------------------

bool Presenter::alloc_image(int w, int h) {
  free_image();
  if (w <= 0 || h <= 0) return false;
  if (shm_ok_) {
    std::string why;
    if (alloc_shm_image(w, h, &why)) return true;
    // Once is enough: a remote display, or a server in another IPC
    // namespace (a container), fails every time.
    shm_ok_ = false;
    log_line("present: MIT-SHM not usable (%s); drawing with XPutImage", why.c_str());
  }
  image_ = XCreateImage(d_, visual_, (unsigned)depth_, ZPixmap, 0, nullptr, (unsigned)w, (unsigned)h, 32, 0);
  if (!image_) return false;
  image_->data = static_cast<char*>(calloc((size_t)image_->bytes_per_line * (size_t)h, 1));
  if (!image_->data) {
    XDestroyImage(image_);
    image_ = nullptr;
    return false;
  }
  img_w_ = w;
  img_h_ = h;
  // Xlib sends an image over 256 KB as several PutImage requests, and the
  // screen could show a frame half new between them: the frame goes into a
  // pixmap and to the window in one copy.
  begin_trap(d_);
  pixmap_ = XCreatePixmap(d_, win_, (unsigned)w, (unsigned)h, (unsigned)depth_);
  if (const std::string error = end_trap(d_); !error.empty()) {
    log_line("present: no %dx%d pixmap (%s); frames go to the window directly", w, h, error.c_str());
    pixmap_ = 0;
  }
  return true;
}

bool Presenter::alloc_shm_image(int w, int h, std::string* why) {
  shm_ = XShmSegmentInfo{};
  shm_.shmid = -1;
  shm_.shmaddr = reinterpret_cast<char*>(-1);
  XImage* img = XShmCreateImage(d_, visual_, (unsigned)depth_, ZPixmap, nullptr, &shm_, (unsigned)w, (unsigned)h);
  if (!img) {
    *why = "XShmCreateImage failed";
    return false;
  }
  const size_t size = (size_t)img->bytes_per_line * (size_t)img->height;
  // Only its owner may attach it (the X server checks the client's
  // credentials against these bits).
  shm_.shmid = shmget(IPC_PRIVATE, size, IPC_CREAT | 0600);
  if (shm_.shmid < 0) {
    *why = std::string("shmget: ") + strerror(errno);
    XDestroyImage(img);   // an XShm image's destroy frees the structure alone
    return false;
  }
  shm_.shmaddr = static_cast<char*>(shmat(shm_.shmid, nullptr, 0));
  if (shm_.shmaddr == reinterpret_cast<char*>(-1)) {
    *why = std::string("shmat: ") + strerror(errno);
    shmctl(shm_.shmid, IPC_RMID, nullptr);
    XDestroyImage(img);
    return false;
  }
  img->data = shm_.shmaddr;
  shm_.readOnly = True;   // the server only reads it (XShmPutImage)
  begin_trap(d_);
  const Status attached = XShmAttach(d_, &shm_);
  const std::string error = end_trap(d_);
  // Marked for removal now, attached or not: it goes with the last
  // attachment, and never outlives the player.
  shmctl(shm_.shmid, IPC_RMID, nullptr);
  if (!attached || !error.empty()) {
    // (A root X server without CAP_IPC_OWNER, in a container, can't attach
    // another user's 0600 segment either: BadAccess.)
    *why = "the X server could not attach the segment" + (error.empty() ? std::string() : " (" + error + ")") +
           ": a remote display, a server in another IPC namespace, or one without the right to";
    shmdt(shm_.shmaddr);
    img->data = nullptr;
    XDestroyImage(img);
    shm_ = XShmSegmentInfo{};
    return false;
  }
  image_ = img;
  use_shm_ = true;
  img_w_ = w;
  img_h_ = h;
  puts_outstanding_ = 0;
  return true;
}

void Presenter::free_image() {
  if (!image_) return;
  if (use_shm_) {
    // In order after any put still queued: the server finishes reading
    // before it lets go; the segment, already marked, goes with it.
    XShmDetach(d_, &shm_);
    shmdt(shm_.shmaddr);
    image_->data = nullptr;
    XDestroyImage(image_);
    shm_ = XShmSegmentInfo{};
    use_shm_ = false;
  } else {
    XDestroyImage(image_);   // frees the calloc'd data too
    if (pixmap_) XFreePixmap(d_, pixmap_);
    pixmap_ = 0;
  }
  image_ = nullptr;
  img_w_ = img_h_ = 0;
  puts_outstanding_ = 0;
  pending_ = false;
}

int Presenter::layout(int frame_w, int frame_h) {
  const RectI fit = stretch_ ? RectI{0, 0, win_w_, win_h_} : fit_rect(frame_w, frame_h, win_w_, win_h_);
  if (image_ && fit == fit_ && img_w_ == fit.w && img_h_ == fit.h) return 0;
  fit_ = fit;
  if (!alloc_image(fit.w, fit.h)) {
    err_line("cannot make an image of %dx%d for the frames", fit.w, fit.h);
    return -1;
  }
  place_clip_window();
  return 1;
}

void Presenter::put_image() {
  if (!image_ || !d_) return;
  if (use_shm_) {
    // One request, read by the server at once; its completion says when.
    XShmPutImage(d_, win_, gc_, image_, 0, 0, fit_.x, fit_.y, (unsigned)img_w_, (unsigned)img_h_, True);
    ++puts_outstanding_;
    put_at_ = Clock::now();
  } else if (pixmap_) {
    XPutImage(d_, pixmap_, gc_, image_, 0, 0, 0, 0, (unsigned)img_w_, (unsigned)img_h_);
    XCopyArea(d_, pixmap_, win_, gc_, 0, 0, (unsigned)img_w_, (unsigned)img_h_, fit_.x, fit_.y);
  } else {
    XPutImage(d_, win_, gc_, image_, 0, 0, fit_.x, fit_.y, (unsigned)img_w_, (unsigned)img_h_);
  }
}

void Presenter::show_image() {
  // The frame there already, shown again (Expose): from the pixmap when
  // there is one, else put again.
  if (!image_ || !d_) return;
  if (!use_shm_ && pixmap_) {
    XCopyArea(d_, pixmap_, win_, gc_, 0, 0, (unsigned)img_w_, (unsigned)img_h_, fit_.x, fit_.y);
  } else {
    put_image();
  }
}

void Presenter::draw_frame() {
  scaler_.scale(last_, pf_, reinterpret_cast<uint8_t*>(image_->data), (size_t)image_->bytes_per_line, img_w_,
                img_h_);
  pending_ = false;
  put_image();
}

void Presenter::fill_bars() {
  if (!gc_) return;
  XSetForeground(d_, gc_, black_);
  const RectI& f = fit_;
  const int right = f.x + f.w, bottom = f.y + f.h;
  if (f.y > 0) XFillRectangle(d_, win_, gc_, 0, 0, (unsigned)win_w_, (unsigned)f.y);
  if (bottom < win_h_) XFillRectangle(d_, win_, gc_, 0, bottom, (unsigned)win_w_, (unsigned)(win_h_ - bottom));
  if (f.x > 0 && f.h > 0) XFillRectangle(d_, win_, gc_, 0, f.y, (unsigned)f.x, (unsigned)f.h);
  if (right < win_w_ && f.h > 0) XFillRectangle(d_, win_, gc_, right, f.y, (unsigned)(win_w_ - right), (unsigned)f.h);
  XSetForeground(d_, gc_, white_);
}

bool Presenter::present(const RawFrame& f) {
  if (!d_ || !win_) return false;
  if (have_last_ && !showing_message_ && same_frame(f, last_)) return false;
  last_ = f;
  have_last_ = true;
  const bool from_message = showing_message_;
  showing_message_ = false;
  const int relaid = layout(f.width, f.height);
  if (relaid < 0) return false;
  if (from_message || relaid > 0) fill_bars();
  // The server still reads the image: this frame goes in once it has.
  if (busy()) {
    pending_ = true;
    return true;
  }
  draw_frame();
  return true;
}

void Presenter::redraw() {
  if (!d_ || !win_) return;
  if (showing_message_ || !have_last_ || !image_) {
    draw_message();
    return;
  }
  fill_bars();
  show_image();   // the image as it is: a frame waiting for a completion follows it
}

void Presenter::show_message(const std::string& text) {
  if (!d_) return;
  message_ = text;
  showing_message_ = true;
  have_last_ = false;   // the next frame is drawn, even one the same as before
  pending_ = false;
  draw_message();
}

void Presenter::draw_message() {
  if (!gc_) return;
  XSetForeground(d_, gc_, black_);
  XFillRectangle(d_, win_, gc_, 0, 0, (unsigned)win_w_, (unsigned)win_h_);
  XSetForeground(d_, gc_, white_);
  if (message_.empty()) return;
  // Wrapped at spaces to fit the window, each line centred.
  const int margin = win_w_ < 400 ? 4 : 16;
  std::vector<std::string> lines;
  std::string line;
  auto width_of = [&](const std::string& s) {
    return font_ ? XTextWidth(font_, s.c_str(), (int)s.size()) : (int)s.size() * 6;
  };
  size_t i = 0;
  while (i < message_.size()) {
    size_t j = message_.find(' ', i);
    if (j == std::string::npos) j = message_.size();
    std::string word = message_.substr(i, j - i);
    std::string next = line.empty() ? word : line + " " + word;
    if (!line.empty() && width_of(next) > win_w_ - 2 * margin) {
      lines.push_back(line);
      line = word;
    } else {
      line = next;
    }
    i = j + 1;
  }
  if (!line.empty()) lines.push_back(line);
  const int lh = font_ ? font_->ascent + font_->descent + 4 : 16;
  int y = win_h_ / 2 - (int)lines.size() * lh / 2 + (font_ ? font_->ascent : 12);
  for (const auto& l : lines) {
    const int x = (win_w_ - width_of(l)) / 2;
    XDrawString(d_, win_, gc_, x < margin ? margin : x, y, l.c_str(), (int)l.size());
    y += lh;
  }
}

// ---- events ----------------------------------------------------------------------

bool Presenter::handle_event(const XEvent& ev, bool* gone) {
  if (!d_) return false;
  if (shm_completion_ >= 0 && ev.type == shm_completion_) {
    const auto& c = reinterpret_cast<const XShmCompletionEvent&>(ev);
    // A completion for a segment since freed (a resize) is stale.
    if (use_shm_ && c.shmseg == shm_.shmseg && puts_outstanding_ > 0 && --puts_outstanding_ == 0 && pending_) {
      draw_frame();
    }
    return true;
  }
  switch (ev.type) {
    case ConfigureNotify:
      if (ev.xconfigure.window != win_) return false;
      origin_known_ = false;   // it may have moved
      if (ev.xconfigure.width != win_w_ || ev.xconfigure.height != win_h_) {
        win_w_ = std::max(1, ev.xconfigure.width);
        win_h_ = std::max(1, ev.xconfigure.height);
        update_covers();
        load_font();
        if (!showing_message_ && have_last_) {
          // The last frame, fitted to the new size.
          if (layout(last_.width, last_.height) >= 0 && image_) {
            fill_bars();
            if (busy()) pending_ = true;
            else draw_frame();
          }
        } else {
          place_clip_window();
          draw_message();
        }
        const RectI r = frame_rect();
        log_line("window: now %dx%d, frames at %d,%d %dx%d", win_w_, win_h_, r.x, r.y, r.w, r.h);
      } else {
        update_covers();   // moved, perhaps onto another monitor
      }
      return true;
    case Expose:
      if (ev.xexpose.window != win_) return false;
      if (ev.xexpose.count == 0) redraw();
      return true;
    case DestroyNotify:
      if (ev.xdestroywindow.window != win_) return false;
      if (gone) *gone = true;
      return true;
    default:
      return false;
  }
}

void Presenter::tick(Clock::time_point now) {
  if (!busy() || now - put_at_ < kCompletionWait) return;
  if (!missing_completion_logged_) {
    missing_completion_logged_ = true;
    log_line("present: no XShm completion %lld ms after a put; drawing on without it",
             (long long)std::chrono::duration_cast<std::chrono::milliseconds>(now - put_at_).count());
  }
  puts_outstanding_ = 0;
  if (pending_ && image_) draw_frame();
}

std::optional<Presenter::Clock::time_point> Presenter::next_tick() const {
  if (!busy() || !pending_) return std::nullopt;
  return put_at_ + kCompletionWait;
}

// ---- layout ----------------------------------------------------------------------

void Presenter::set_screen(SizeI emu) {
  screen_emu_ = emu;
  if (!have_last_ || showing_message_) place_clip_window();
}

RectI Presenter::frame_rect() const {
  if (have_last_ && !showing_message_ && fit_.w > 0 && fit_.h > 0) return fit_;
  if (screen_emu_.w > 0 && screen_emu_.h > 0)
    return stretch_ ? RectI{0, 0, win_w_, win_h_} : fit_rect(screen_emu_.w, screen_emu_.h, win_w_, win_h_);
  return RectI{0, 0, win_w_, win_h_};
}

bool Presenter::covers_primary_monitor(std::string* detail) const {
  if (!d_ || !win_) return false;
  Window child = 0;
  int x = 0, y = 0;
  if (!XTranslateCoordinates(d_, win_, RootWindow(d_, screen_), 0, 0, &x, &y, &child)) {
    if (detail) *detail = "the window is not on this screen";
    return false;
  }
  const RectI me{x, y, win_w_, win_h_};
  char buf[160];
  if (ScreenCount(d_) > 1 && screen_ != 0) {
    // Several X screens (not monitors of one): the first screen's alone.
    snprintf(buf, sizeof(buf), "on X screen %d of %d; sound goes with screen 0", screen_, ScreenCount(d_));
    if (detail) *detail = buf;
    return false;
  }
  const MonitorInfo m = primary_monitor(d_, screen_);
  const bool covers = contains_centre(me, m.rect);
  snprintf(buf, sizeof(buf), "window %dx%d+%d+%d, primary monitor %dx%d+%d+%d: ", me.w, me.h, me.x, me.y, m.rect.w,
           m.rect.h, m.rect.x, m.rect.y);
  if (detail) *detail = buf + m.how;
  return covers;
}

Window Presenter::clip_window() {
  if (!own_ || !d_) return None;
  const RectI r = frame_rect();
  if (!clip_win_) {
    XSetWindowAttributes a{};
    clip_win_ = XCreateWindow(d_, win_, r.x, r.y, (unsigned)std::max(1, r.w), (unsigned)std::max(1, r.h), 0, 0,
                              InputOnly, CopyFromParent, 0, &a);
  } else {
    XMoveResizeWindow(d_, clip_win_, r.x, r.y, (unsigned)std::max(1, r.w), (unsigned)std::max(1, r.h));
  }
  XMapWindow(d_, clip_win_);
  clip_mapped_ = true;
  return clip_win_;
}

void Presenter::hide_clip_window() {
  if (clip_win_ && clip_mapped_) XUnmapWindow(d_, clip_win_);
  clip_mapped_ = false;
}

void Presenter::place_clip_window() {
  // A grab confined to it follows (the server keeps the pointer inside).
  if (!clip_win_ || !clip_mapped_) return;
  const RectI r = frame_rect();
  XMoveResizeWindow(d_, clip_win_, r.x, r.y, (unsigned)std::max(1, r.w), (unsigned)std::max(1, r.h));
}

}  // namespace lad
