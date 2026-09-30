#include "monitors.h"

#include <X11/extensions/Xrandr.h>

#include <algorithm>

namespace lad {

namespace {

std::string atom_name(Display* d, Atom a) {
  if (a == None) return "?";
  char* s = XGetAtomName(d, a);
  if (!s) return "?";
  std::string r(s);
  XFree(s);
  return r;
}

// RandR 1.3: the primary output's CRTC, else the first CRTC driving an
// output (the X server's own choice for its Xinerama screen 0).
bool primary_by_crtcs(Display* d, Window root, MonitorInfo* out) {
  XRRScreenResources* res = XRRGetScreenResourcesCurrent(d, root);
  if (!res) return false;
  bool found = false;
  const RROutput primary = XRRGetOutputPrimary(d, root);
  if (primary != None) {
    if (XRROutputInfo* oi = XRRGetOutputInfo(d, res, primary)) {
      if (oi->crtc != None) {
        if (XRRCrtcInfo* ci = XRRGetCrtcInfo(d, res, oi->crtc)) {
          if (ci->mode != None && ci->width > 0 && ci->height > 0) {
            out->rect = RectI{ci->x, ci->y, (int)ci->width, (int)ci->height};
            out->how = "RandR output " + std::string(oi->name ? oi->name : "?") + " (primary)";
            found = true;
          }
          XRRFreeCrtcInfo(ci);
        }
      }
      XRRFreeOutputInfo(oi);
    }
  }
  for (int i = 0; !found && i < res->ncrtc; ++i) {
    XRRCrtcInfo* ci = XRRGetCrtcInfo(d, res, res->crtcs[i]);
    if (!ci) continue;
    if (ci->mode != None && ci->noutput > 0 && ci->width > 0 && ci->height > 0) {
      out->rect = RectI{ci->x, ci->y, (int)ci->width, (int)ci->height};
      out->how = "RandR CRTC " + std::to_string(i) + " (no primary output set: the first)";
      found = true;
    }
    XRRFreeCrtcInfo(ci);
  }
  XRRFreeScreenResources(res);
  return found;
}

}  // namespace

MonitorInfo primary_monitor(Display* d, int screen) {
  MonitorInfo m;
  m.rect = RectI{0, 0, DisplayWidth(d, screen), DisplayHeight(d, screen)};
  m.how = "the whole screen (no RandR)";
  int event_base = 0, error_base = 0, major = 0, minor = 0;
  if (!XRRQueryExtension(d, &event_base, &error_base) || !XRRQueryVersion(d, &major, &minor)) return m;
  const Window root = RootWindow(d, screen);
  if (major > 1 || (major == 1 && minor >= 5)) {
    int n = 0;
    if (XRRMonitorInfo* mons = XRRGetMonitors(d, root, True, &n)) {
      auto usable = [&](int i) { return mons[i].width > 0 && mons[i].height > 0; };
      int pick = -1;
      const char* why = " (primary)";
      for (int i = 0; i < n && pick < 0; ++i) {
        if (mons[i].primary && usable(i)) pick = i;
      }
      // None flagged: the one holding the primary output (a monitor the
      // user defined over it), else the first.
      if (pick < 0) {
        const RROutput primary = XRRGetOutputPrimary(d, root);
        for (int i = 0; i < n && pick < 0 && primary != None; ++i) {
          for (int o = 0; o < mons[i].noutput && pick < 0; ++o) {
            if (mons[i].outputs[o] == primary && usable(i)) {
              pick = i;
              why = " (holding the primary output)";
            }
          }
        }
      }
      for (int i = 0; i < n && pick < 0; ++i) {
        if (usable(i)) {
          pick = i;
          why = " (none is primary: the first)";
        }
      }
      if (pick >= 0) {
        m.rect = RectI{mons[pick].x, mons[pick].y, mons[pick].width, mons[pick].height};
        m.how = "RandR monitor " + atom_name(d, mons[pick].name) + why + " of " + std::to_string(n);
      }
      XRRFreeMonitors(mons);
      if (pick >= 0) return m;
    }
  }
  if (major > 1 || (major == 1 && minor >= 3)) {
    MonitorInfo c;
    if (primary_by_crtcs(d, root, &c)) return c;
  }
  return m;
}

std::vector<MonitorInfo> list_monitors(Display* d, int screen) {
  std::vector<MonitorInfo> v;
  auto add = [&](const RectI& r, const std::string& how) {
    if (r.w <= 0 || r.h <= 0) return;
    for (const MonitorInfo& m : v) {
      if (m.rect == r) return;
    }
    MonitorInfo m;
    m.rect = r;
    m.how = how;
    v.push_back(m);
  };
  int event_base = 0, error_base = 0, major = 0, minor = 0;
  const bool randr = XRRQueryExtension(d, &event_base, &error_base) && XRRQueryVersion(d, &major, &minor);
  const Window root = RootWindow(d, screen);
  if (randr && (major > 1 || (major == 1 && minor >= 5))) {
    int n = 0;
    if (XRRMonitorInfo* mons = XRRGetMonitors(d, root, True, &n)) {
      for (int i = 0; i < n; ++i) {
        add(RectI{mons[i].x, mons[i].y, mons[i].width, mons[i].height}, "RandR monitor " + atom_name(d, mons[i].name));
      }
      XRRFreeMonitors(mons);
    }
  }
  if (v.empty() && randr && (major > 1 || (major == 1 && minor >= 3))) {
    if (XRRScreenResources* res = XRRGetScreenResourcesCurrent(d, root)) {
      for (int i = 0; i < res->ncrtc; ++i) {
        XRRCrtcInfo* ci = XRRGetCrtcInfo(d, res, res->crtcs[i]);
        if (!ci) continue;
        if (ci->mode != None && ci->noutput > 0) {
          add(RectI{ci->x, ci->y, (int)ci->width, (int)ci->height}, "RandR CRTC " + std::to_string(i));
        }
        XRRFreeCrtcInfo(ci);
      }
      XRRFreeScreenResources(res);
    }
  }
  // The primary as primary_monitor() picks it; without RandR, the whole
  // screen is the one monitor.
  const MonitorInfo primary = primary_monitor(d, screen);
  auto it = std::find_if(v.begin(), v.end(), [&](const MonitorInfo& m) { return m.rect == primary.rect; });
  if (it == v.end()) it = v.insert(v.begin(), primary);
  it->primary = true;
  it->how = primary.how;
  return v;
}

std::vector<RectI> monitors_to_cover(const std::vector<MonitorInfo>& all, const RectI& main) {
  auto overlaps = [](const RectI& a, const RectI& b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
  };
  std::vector<RectI> out;
  for (const MonitorInfo& m : all) {
    const RectI& r = m.rect;
    if (r.w <= 0 || r.h <= 0 || overlaps(r, main)) continue;
    if (std::find(out.begin(), out.end(), r) == out.end()) out.push_back(r);
  }
  return out;
}

}  // namespace lad
