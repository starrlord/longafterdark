#include "geometry.h"

#include <algorithm>
#include <cwchar>
#include <set>
#include <string>

#include "catalog.h"

namespace adw::scr {

namespace {
// Truncate, then round to the nearest multiple of 8.
int snap8(double v) { return (((int)v + 4) / 8) * 8; }
} // namespace

SizeI emulated_screen_size(double display_aspect, double scale, int base_w, int base_h) {
  if (!(scale > 0.0)) scale = 1.0;
  int h = snap8(base_h * scale);
  double a = std::max(display_aspect > 0.0 ? display_aspect : 0.0, (double)base_w / base_h);
  int w = std::min(snap8(h * a), snap8(base_w * scale) * 2);
  return {w, h};
}

RectI fit_rect(int src_w, int src_h, int dst_w, int dst_h) {
  if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) return {0, 0, std::max(dst_w, 0), std::max(dst_h, 0)};
  // Compare cross-products in 64-bit to pick the limiting axis exactly.
  long long lhs = (long long)dst_w * src_h, rhs = (long long)dst_h * src_w;
  int w, h;
  if (lhs > rhs) {            // destination is wider: full height, pillarbox
    h = dst_h;
    w = (int)(((long long)dst_h * src_w + src_h / 2) / src_h);
  } else {                    // destination is taller (or equal): full width, letterbox
    w = dst_w;
    h = (int)(((long long)dst_w * src_h + src_w / 2) / src_w);
  }
  w = std::min(w, dst_w);
  h = std::min(h, dst_h);
  return {(dst_w - w) / 2, (dst_h - h) / 2, w, h};
}

SizeI own_screen(std::string_view abi, SizeI catalog_screen) {
  // The catalog's word first (parse_catalog keeps only a size a host renders
  // and the saver reads back): the importer knows what a release's modules
  // compose.
  if (catalog_screen.w > 0 && catalog_screen.h > 0) return catalog_screen;
  // Intermission's scenes are 640x480 (IMIMXPLY centres them on whatever
  // screen it is given): that is the whole of what they draw.
  if (abi == kIntermissionAbi) return SizeI{640, 480};
  return SizeI{};
}

ModuleScreen module_screen(SizeI own, double display_aspect, double scale) {
  if (own.w > 0 && own.h > 0) return ModuleScreen{own, true};
  return ModuleScreen{emulated_screen_size(display_aspect, scale), false};
}

ModuleScreen module_screen(std::string_view abi, double display_aspect, double scale) {
  return module_screen(own_screen(abi), display_aspect, scale);
}

RectI seed_source(const ModuleScreen& screen, int w, int h) {
  if (!screen.fixed) return RectI{0, 0, std::max(w, 0), std::max(h, 0)};
  return fit_rect(screen.emu.w, screen.emu.h, w, h);
}

std::vector<SeedShotPlan> plan_seed_shots(const std::vector<ModuleScreen>& screens, int mw, int mh,
                                          size_t* left_out) {
  // Each screen once (sets: a catalog may hold any number): After Dark's
  // first (the first one given; its file keeps the name it has always had),
  // then modules' own sizes, 640x480 (Intermission's, Star Trek's,
  // ScreamSavers' and Marvel's) and then the smallest (the fewest bytes),
  // whatever order they came in.
  auto own_before = [](const SizeI& a, const SizeI& b) {
    const bool a_vga = a == SizeI{640, 480}, b_vga = b == SizeI{640, 480};
    if (a_vga != b_vga) return a_vga;
    const long long a_px = (long long)a.w * a.h, b_px = (long long)b.w * b.h;
    return a_px != b_px ? a_px < b_px : a < b;
  };
  std::set<SizeI> follows;
  std::set<SizeI, decltype(own_before)> owns(own_before);
  const ModuleScreen* follow = nullptr;
  for (const ModuleScreen& ms : screens) {
    if (ms.fixed) {
      owns.insert(ms.emu);
    } else {
      if (!follow) follow = &ms;
      follows.insert(ms.emu);
    }
  }
  // One that follows the display and kMaxOwnSeedShots of their own at most.
  std::vector<ModuleScreen> each;
  if (follow) each.push_back(*follow);
  for (auto it = owns.begin(); it != owns.end() && each.size() < (follow ? 1 : 0) + kMaxOwnSeedShots; ++it) {
    each.push_back(ModuleScreen{*it, true});
  }
  if (left_out) *left_out = follows.size() + owns.size() - each.size();
  std::vector<SeedShotPlan> plan;
  for (const ModuleScreen& ms : each) {
    SeedShotPlan p{ms, seed_source(ms, mw, mh)};
    // The first earlier picture it matches owns the file: one that shares a
    // file comes after the picture it shares.
    for (size_t j = 0; j < plan.size() && p.same < 0; ++j) {
      if (plan[j].src == p.src && plan[j].screen.emu == ms.emu) p.same = (int)j;
    }
    plan.push_back(p);
  }
  return plan;
}

namespace {
// What a window shows, apart from where: a current window `a` can take slot
// `b` when their roles match and its host can carry on there (the size the
// slot's monitor gives, or a size of its module's own that no monitor
// changes). `sized_only` leaves the fixed-size windows out.
bool same_role(const ScreenSlot& a, const ScreenSlot& b, bool sized_only = false) {
  if (a.runs_host != b.runs_host || a.message != b.message) return false;
  if (!a.runs_host) return true;
  if (sized_only) return !a.fixed && a.emu == b.emu;
  return a.fixed || a.emu == b.emu;
}
} // namespace

RelayoutPlan plan_relayout(const std::vector<ScreenSlot>& current, const std::vector<ScreenSlot>& next) {
  RelayoutPlan p;
  p.reuse.assign(next.size(), -1);
  std::vector<bool> taken(current.size(), false);
  // Pass 1: the same monitor as before (and nothing else about it changed).
  for (size_t n = 0; n < next.size(); ++n) {
    for (size_t c = 0; c < current.size(); ++c) {
      if (!taken[c] && current[c].rc == next[n].rc && same_role(current[c], next[n])) {
        p.reuse[n] = (int)c;
        taken[c] = true;
        ++p.kept;
        break;
      }
    }
  }
  // Pass 2: a window whose host can carry on where it is sent, the ones of
  // the slot's size first and then the ones of a size of their own (those
  // fit any slot, so they must not take one a sized window needs).
  for (bool sized_only : {true, false}) {
    for (size_t n = 0; n < next.size(); ++n) {
      if (p.reuse[n] >= 0) continue;
      for (size_t c = 0; c < current.size(); ++c) {
        if (!taken[c] && same_role(current[c], next[n], sized_only)) {
          p.reuse[n] = (int)c;
          taken[c] = true;
          ++p.moved;
          break;
        }
      }
    }
  }
  for (size_t n = 0; n < next.size(); ++n) {
    if (p.reuse[n] < 0) ++p.created;
  }
  p.retire.resize(current.size());
  for (size_t c = 0; c < current.size(); ++c) {
    p.retire[c] = !taken[c];
    if (!taken[c]) ++p.retired;
  }
  return p;
}

std::vector<StagedMonitor> parse_staged_monitors(std::wstring_view spec, size_t layout) {
  std::vector<std::wstring_view> layouts;
  for (size_t p = 0; !spec.empty();) {
    const size_t bar = spec.find(L'|', p);
    layouts.push_back(spec.substr(p, bar == std::wstring_view::npos ? std::wstring_view::npos : bar - p));
    if (bar == std::wstring_view::npos) break;
    p = bar + 1;
  }
  std::vector<StagedMonitor> v;
  if (layouts.empty()) return v;
  const std::wstring_view l = layouts[std::min(layout, layouts.size() - 1)];
  bool have_primary = false;
  for (size_t p = 0; p < l.size();) {
    const size_t semi = l.find(L';', p);
    const std::wstring m(l.substr(p, semi == std::wstring_view::npos ? std::wstring_view::npos : semi - p));
    p = semi == std::wstring_view::npos ? l.size() : semi + 1;
    long x = 0, y = 0, w = 0, h = 0;
    wchar_t flag[8] = {};
    const int n = swscanf(m.c_str(), L" %ld , %ld , %ld , %ld , %7ls", &x, &y, &w, &h, flag);
    if (n < 4 || w <= 0 || h <= 0) continue;
    const bool primary = n == 5 && flag[0] == L'p' && !have_primary;
    have_primary |= primary;
    v.push_back({{(int)x, (int)y, (int)w, (int)h}, primary});
  }
  if (!v.empty() && !have_primary) v.front().primary = true;
  return v;
}

} // namespace adw::scr
