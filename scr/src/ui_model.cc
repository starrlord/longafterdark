#include "ui_model.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <iterator>
#include <string_view>

#include "paths.h"

namespace adw::scr {

// ---- text ------------------------------------------------------------------------

namespace {

std::vector<std::string> split_lines(const std::string& text) {
  std::vector<std::string> lines;
  std::string cur;
  for (size_t i = 0; i < text.size(); ++i) {
    char ch = text[i];
    if (ch == '\r') continue;
    if (ch == '\n') {
      lines.push_back(cur);
      cur.clear();
    } else {
      cur += ch;
    }
  }
  lines.push_back(cur);
  return lines;
}

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
  return s.substr(a, b - a);
}

// Letters and digits only, lower-cased: "BORIS (tm)" -> "boristm".
std::string squash(const std::string& s) {
  std::string out;
  for (unsigned char ch : s) {
    if (std::isalnum(ch)) out += (char)std::tolower(ch);
  }
  return out;
}

std::string last_word(const std::string& s) {
  size_t sp = s.find_last_of(' ');
  return sp == std::string::npos ? s : s.substr(sp + 1);
}

// A credits line: "Programming by X", "3D art by Y", "©1996 …", "Original
// music by …" (a "by" before any full stop, early in the line).
bool credit_line(const std::string& l) {
  std::string low;
  for (unsigned char ch : l) low += (char)std::tolower(ch);
  for (const char* p : {"\xC2\xA9", "(c)", "copyright", "original ", "windows version"}) {
    if (low.rfind(p, 0) == 0) return true;
  }
  const size_t stop = low.find_first_of(".!?");
  for (size_t at = low.find("by"); at != std::string::npos && at <= 40; at = low.find("by", at + 1)) {
    if (stop != std::string::npos && stop < at) break;
    const bool start = at == 0 || !std::isalnum((unsigned char)low[at - 1]);
    const bool end = at + 2 >= low.size() || !std::isalnum((unsigned char)low[at + 2]);
    if (start && end) return true;
  }
  return false;
}

// The line's last character that isn't a closing quote, bracket or mark.
char last_meaningful(const std::string& l) {
  size_t i = l.size();
  while (i > 0) {
    unsigned char ch = (unsigned char)l[i - 1];
    if (ch == '"' || ch == '\'' || ch == ')' || ch == ']' || ch == ' ') {
      --i;
      continue;
    }
    // UTF-8 closing quotes (” ’) and ™ end in 0x9D / 0x99 / 0xA2 after E2 80 / E2 84.
    if (ch >= 0x80) {
      size_t j = i - 1;
      while (j > 0 && ((unsigned char)l[j] & 0xC0) == 0x80) --j;
      std::string_view cp(l.data() + j, i - j);
      if (cp == "\xE2\x80\x9D" || cp == "\xE2\x80\x99" || cp == "\xE2\x84\xA2") {
        i = j;
        continue;
      }
    }
    return (char)ch;
  }
  return 0;
}

// Was the break after `line` made only because the original dialog was
// narrow? When the sentence plainly carries on: the next line starts in lower
// case, or this one ends on a word that can't end a sentence ("use with /
// MultiModule"); or this one is a long line (the old dialog's width) that
// stops mid-phrase ("…your available memory / (RAM) will not display",
// "…TARGA, PCX, / 16-color GIF"). A short line, a line ending a sentence and
// credits are taken as meant (poems, "Programming by X / Art by Y"): a missed
// join only leaves an old-style break, a wrong one would run credits together.
bool wrapped(const std::string& line, const std::string& next) {
  if (line.empty() || next.empty()) return false;
  if (std::islower((unsigned char)next[0])) return true;
  static const char* const kDangling[] = {"a", "an", "the", "and", "or", "but", "of", "to", "in", "on", "at",
                                          "for", "with", "by", "from", "as", "is", "are", "was", "that", "your",
                                          "you", "its", "into", "than", "will", "can", "if", "when"};
  const std::string w = last_word(line);
  for (const char* d : kDangling) {
    if (w == d) return true;
  }
  const char end = last_meaningful(line);
  if (!end || std::strchr(".!?:;", end)) return false;
  if (credit_line(line) || credit_line(next)) return false;
  if (next[0] == '(') return true;
  if (end == ',') return std::isdigit((unsigned char)next[0]) != 0;
  return line.size() >= 40;
}

} // namespace

std::string tidy_about(const std::string& about, const std::string& display_name) {
  // Paragraphs: runs of non-blank lines.
  std::vector<std::vector<std::string>> paras;
  std::vector<std::string> cur;
  for (const std::string& raw : split_lines(about)) {
    std::string l = trim(raw);
    if (l.empty()) {
      if (!cur.empty()) paras.push_back(std::move(cur));
      cur.clear();
    } else {
      cur.push_back(l);
    }
  }
  if (!cur.empty()) paras.push_back(std::move(cur));

  // The opening line naming the module ("Flying Toasters!", "BORIS (tm)").
  if (!paras.empty() && paras.front().size() == 1) {
    std::string a = squash(paras.front().front()), b = squash(display_name);
    if (!a.empty() && !b.empty() && (a.rfind(b, 0) == 0 || b.rfind(a, 0) == 0) && a.size() <= b.size() + 6) {
      paras.erase(paras.begin());
    }
  }

  std::string out;
  for (const auto& p : paras) {
    std::string text;
    for (size_t i = 0; i < p.size(); ++i) {
      text += p[i];
      if (i + 1 < p.size()) text += wrapped(p[i], p[i + 1]) ? " " : "\n";
    }
    // "sentence.  Next" -> "sentence. Next"
    std::string squeezed;
    for (char ch : text) {
      if (ch == ' ' && !squeezed.empty() && squeezed.back() == ' ') continue;
      squeezed += ch;
    }
    if (!out.empty()) out += "\n\n";
    out += squeezed;
  }
  return out;
}

std::wstring duration_label(int minutes) {
  if (minutes <= 0) return L"Never";
  if (minutes % 60 == 0) {
    int h = minutes / 60;
    return std::to_wstring(h) + (h == 1 ? L" hour" : L" hours");
  }
  return std::to_wstring(minutes) + (minutes == 1 ? L" minute" : L" minutes");
}

std::vector<DurationChoice> duration_choices(int current) {
  static const int kPresets[] = {1, 2, 3, 5, 10, 15, 20, 30, 45, 60, 120};
  std::vector<int> mins(std::begin(kPresets), std::end(kPresets));
  if (current > 0 && std::find(mins.begin(), mins.end(), current) == mins.end()) {
    mins.insert(std::upper_bound(mins.begin(), mins.end(), current), current);
  }
  std::vector<DurationChoice> out;
  for (int m : mins) out.push_back({m, duration_label(m)});
  out.push_back({0, duration_label(0)});
  return out;
}

PerMonitorChoice per_monitor_choice(bool random, bool all_monitors, int monitors) {
  PerMonitorChoice c;
  c.shown = random && monitors > 1;
  c.enabled = c.shown && all_monitors;
  return c;
}

std::wstring rotation_summary(size_t checked, size_t total, long long runnable, long long distinct) {
  if (total == 0) return L"";
  if (checked == 0) return L"None in rotation";
  // Every row checked is "All 84", but "1" when the list shows one row
  // (Marvel Comics Screen Posters alone: "All 1" reads wrong).
  const std::wstring all = (total == 1 ? L"" : L"All ") + std::to_wstring(total);
  const std::wstring selected = checked == total ? all + L" selected"
                                                 : std::to_wstring(checked) + L" of " + std::to_wstring(total) + L" selected";
  if (runnable >= 0 && (size_t)runnable < checked) return selected + L" · " + std::to_wstring(runnable) + L" can run now";
  if (distinct > 0 && (size_t)distinct < checked) return selected + L" · " + std::to_wstring(distinct) + L" distinct";
  if (checked == total) return all + L" in rotation";
  return std::to_wstring(checked) + L" of " + std::to_wstring(total) + L" in rotation";
}

std::wstring rotation_tip(size_t checked, long long distinct, const std::wstring& lead) {
  std::wstring t;
  if (checked > 0 && distinct > 0 && (size_t)distinct < checked) {
    // Copies of one module alone: nothing takes turns.
    t = L"A module that is on several of the releases checked plays once in each pass, so " +
        (distinct == 1 ? std::wstring(L"1 module is in rotation.")
                       : std::to_wstring(distinct) + L" different modules take turns.");
  }
  if (!lead.empty()) {
    if (!t.empty()) t += L"\n\n";
    t += lead + L" plays first (your settings name it), then the rotation.";
  }
  return t;
}

AssetCounts count_assets(const Catalog& c, const std::vector<bool>& present) {
  AssetCounts a;
  a.total = c.modules.size();
  for (size_t i = 0; i < c.modules.size(); ++i) {
    if (i < present.size() && !present[i]) ++a.missing;
  }
  // Releases with modules listed (an entry of the packages array whose
  // modules are all gone is not "installed" in any sense the user sees).
  std::vector<bool> used(c.releases.size(), false);
  for (const Module& m : c.modules) {
    if (m.release >= 0 && m.release < (int)used.size()) used[m.release] = true;
  }
  int only = -1;
  for (size_t r = 0; r < used.size(); ++r) {
    if (!used[r]) continue;
    ++a.releases;
    only = (int)r;
  }
  // A catalog from before packages has no release to name ("Other modules").
  if (a.releases == 1 && c.releases[only].id != "other") a.release = c.releases[only].title;
  return a;
}

std::wstring assets_summary(const AssetCounts& a) {
  if (a.total == 0) return L"Nothing imported yet";
  std::wstring t = std::to_wstring(a.total) + (a.total == 1 ? L" module" : L" modules");
  if (a.releases > 1) t += L" from " + std::to_wstring(a.releases) + L" releases";
  else if (!a.release.empty()) t += L" from " + widen(a.release);
  else t += L" imported";
  if (a.missing) t += L" · " + std::to_wstring(a.missing) + L" missing — import again to restore";
  return t;
}

std::wstring welcome_text() {
  // Twelve releases, one of them not After Dark's: the product name stays
  // "Long After Dark", the releases are named for what they are. (Star Trek:
  // The Screen Saver, the Simpsons, Marvel Comics Screen Posters, Snoopy's
  // Screen Savers, the Looney Tunes (also on a CD), ScreamSavers and the
  // Disney Collection came on floppies: "discs" covers them.)
  return L"The screen saver runs the original modules of After Dark and Star Wars Screen Entertainment from your "
         L"own discs.\n\n"
         L"Import them from any of your discs (twelve releases are supported), a disc image, or the Internet Archive "
         L"download. They are copied to your computer once; nothing else is needed.";
}

// ---- string-slider stops ---------------------------------------------------------

int VisualStops::run_of_stop(int raw_stop) const {
  for (int i = 0; i < count(); ++i) {
    if (raw_stop >= first[i] && raw_stop <= last[i]) return i;
  }
  return count() ? (raw_stop < first.front() ? 0 : count() - 1) : 0;
}

VisualStops visual_stops(const Control& c) {
  VisualStops v;
  const int n = c.stop_count();
  for (int s = 0; s < n; ++s) {
    if (v.count() && c.items[s] == v.labels.back()) {
      v.last.back() = s;
      continue;
    }
    v.labels.push_back(c.items[s]);
    v.first.push_back(s);
    v.last.push_back(s);
  }
  for (int i = 0; i < v.count(); ++i) {
    const int lo = c.value_of_stop(v.first[i]), hi = c.value_of_stop(v.last[i]);
    auto inside = [&](int value) { return c.stop_of(value) >= v.first[i] && c.stop_of(value) <= v.last[i]; };
    v.value.push_back(v.first[i] == v.last[i] ? lo : inside(c.def) ? c.def : c.has_bold && inside(c.bold_value) ? c.bold_value : hi);
  }
  return v;
}

// ---- thumbnails --------------------------------------------------------------------

ThumbQuality judge_thumb(const unsigned char* bgr, int w, int h) {
  ThumbQuality q;
  const size_t n = (size_t)std::max(0, w) * std::max(0, h);
  if (!bgr || n == 0) return q;
  std::vector<double> lum(n);
  std::vector<int> hist(4096, 0);
  double sum = 0, sum2 = 0;
  for (size_t i = 0; i < n; ++i) {
    const unsigned char b = bgr[i * 3], g = bgr[i * 3 + 1], r = bgr[i * 3 + 2];
    const double l = 0.114 * b + 0.587 * g + 0.299 * r;
    lum[i] = l;
    sum += l;
    sum2 += l * l;
    ++hist[(r >> 4) << 8 | (g >> 4) << 4 | (b >> 4)];
  }
  const double mean = sum / n;
  q.stddev = std::sqrt(std::max(0.0, sum2 / n - mean * mean));
  const size_t min_count = std::max<size_t>(1, n / 500);
  for (int c : hist) q.colours += (size_t)c >= min_count;
  size_t flat = 0;
  for (double l : lum) flat += std::fabs(l - mean) <= 10;
  q.flat = (double)flat / n;
  q.good = q.stddev >= kThumbMinStddev && q.colours >= kThumbMinColours && q.flat <= kThumbMaxFlat;
  q.score = q.good ? q.stddev * std::log2(1.0 + q.colours) * (1.05 - q.flat) : 0;
  return q;
}

// ---- layout ----------------------------------------------------------------------

int dip(int dips, int dpi) { return (int)std::lround(dips * (double)dpi / 96.0); }

namespace {

// A rectangle designed in DIPs, converted edge by edge so neighbours that
// share an edge in DIPs share it in pixels too.
struct Scaler {
  int dpi;
  int px(double d) const { return (int)std::lround(d * dpi / 96.0); }
  Rc rc(double x, double y, double w, double h) const {
    int x0 = px(x), y0 = px(y);
    return Rc{x0, y0, px(x + w) - x0, px(y + h) - y0};
  }
};


// Header 48: the 32-DIP moon and the name on one line just under the title
// bar (which shows only its buttons), 12 DIP above the content.
constexpr int kMargin = 24, kGap = 16, kHeaderH = 48, kFooterH = 72, kCardPad = 20;
constexpr int kControlH = 32, kLabelH = 20;
constexpr int kCaptionH = 16;   // one line of the caption face

// ui_widgets.h focus_margin(), which this file can't include (it has no windows).
int focus_px(int dpi) { return std::max(3, (int)std::lround(3.0 * dpi / 96.0)); }

} // namespace

bool blank_label(const std::string& name) {
  return std::all_of(name.begin(), name.end(), [](char ch) { return ch == ' ' || ch == ':' || ch == '\t'; });
}

// ---- the module list's group headers ------------------------------------------------

std::wstring ellipsize(const std::wstring& text, int room, const std::function<int(const std::wstring&)>& measure) {
  if (measure(text) <= room) return text;
  // The first n characters (never half a surrogate pair), without trailing
  // spaces, and the ellipsis.
  auto cut = [&](size_t n) {
    if (n > 0 && n < text.size() && text[n - 1] >= 0xD800 && text[n - 1] <= 0xDBFF) --n;
    std::wstring s = text.substr(0, n);
    while (!s.empty() && s.back() == L' ') s.pop_back();
    return s + L"…";
  };
  if (measure(cut(0)) > room) return L"";
  // cut(lo) fits and cut(hi) doesn't (the whole text didn't, so neither
  // does the whole text with an ellipsis): the longest start that fits.
  size_t lo = 0, hi = text.size();
  while (lo + 1 < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    if (measure(cut(mid)) <= room) lo = mid;
    else hi = mid;
  }
  return cut(lo);
}

GroupHeaderLayout layout_group_header(const GroupHeaderInput& in,
                                      const std::function<int(const std::wstring&)>& measure_title) {
  GroupHeaderLayout L;
  // The count and the pill are placed first; the title has what is left.
  int limit = in.right;
  if (in.pill_w > 0) {
    L.pill_x = in.pill_right - in.pill_w;
    limit = std::min(limit, L.pill_x - in.gap);
  }
  const int room = limit - in.left - in.gap - in.count_w;
  L.title = ellipsize(in.title, room, measure_title);
  L.ellipsized = L.title != in.title;
  L.title_w = L.title.empty() ? 0 : measure_title(L.title);
  L.count_x = in.left + L.title_w + (L.title.empty() ? 0 : in.gap);
  return L;
}

GroupHeaderInput group_header_frame(int list_w, int dpi, bool random, bool scrolls) {
  GroupHeaderInput in;
  // Each length rounded on its own, as the dialog's Theme::px does them.
  in.left = dip(16, dpi) + (random ? dip(kListBoxDip, dpi) + dip(12, dpi) : 0);
  in.right = list_w - dip(16, dpi);
  in.gap = dip(8, dpi);
  in.pill_right = list_w - dip(scrolls ? 12 : 4, dpi) - dip(8, dpi);
  return in;
}

// ---- the box-cover strip ---------------------------------------------------------------

StripMetrics strip_metrics(bool compact) {
  // Regular: 64x80 art at (+16, +4), a 16-DIP caption 4 under it; compact:
  // 48x60 at (+8, +4), no caption. 8 DIP between cells, 12 under the band.
  if (compact) return StripMetrics{48, 60, 64, 68, 8, 4, 0, 72, 80};
  return StripMetrics{64, 80, 96, 108, 16, 4, 16, 104, 120};
}

namespace {

// Overflowing, the tiles that show at a time: as many whole cells as fit
// between the two chevrons' zones, at least one.
int strip_slots(const StripInput& in, const StripMetrics& m) {
  const double room = in.w - 2 * (kStripChevronW + kStripChevronGap) + (m.pitch - m.cell_w);
  return std::max(1, (int)std::floor(room / m.pitch + 1e-9));
}

} // namespace

StripLayout layout_strip(const StripInput& in) {
  StripLayout S;
  const int n = std::max(0, in.tiles);
  S.mode = n == 0 ? StripMode::hidden : in.compact ? StripMode::compact : StripMode::regular;
  if (n == 0) return S;
  const StripMetrics m = strip_metrics(in.compact);
  Scaler s{std::max(48, in.dpi)};
  const int gap = m.pitch - m.cell_w;
  const double zone = kStripChevronW + kStripChevronGap;
  S.overflow = n > 1 && n * m.pitch - gap > in.w + 0.001;
  S.slots = S.overflow ? std::min(n - 1, strip_slots(in, m)) : n;
  S.max_first = n - S.slots;
  S.first = std::clamp(in.first, 0, S.max_first);
  // Overflowing, the view is the slots between the two chevrons' zones at
  // every scroll position: the tiles start after the left chevron's zone
  // (unscrolled too, where that zone stays empty) and the view ends with the
  // last slot, where the right chevron's zone starts. So a stop shows `slots`
  // tiles, the next one's cell never fitting, and each slot keeps its place
  // whatever the stop. Without overflow the tiles start at the column's edge.
  const double lead = S.overflow ? zone : 0;
  const double vl = in.x + lead, vr = S.overflow ? vl + S.slots * m.pitch - gap : in.x + in.w;
  S.area = s.rc(in.x, in.y, in.w, m.cell_h);
  S.view = s.rc(vl, in.y, vr - vl, m.cell_h);
  for (int i = 0; i < n; ++i) {
    const double cx = in.x + lead + (i - S.first) * m.pitch;
    S.cells.push_back(s.rc(cx, in.y, m.cell_w, m.cell_h));
    S.arts.push_back(s.rc(cx + m.art_x, in.y + m.art_y, m.art_w, m.art_h));
    S.captions.push_back(m.caption_h ? s.rc(cx, in.y + m.art_y + m.art_h + 4, m.cell_w, m.caption_h) : Rc{});
    S.whole.push_back(cx >= vl - 0.001 && cx + m.cell_w <= vr + 0.001);
  }
  // Chevrons centred on the art (a little taller in the compact form, so
  // they stay on the 4-DIP grid), each kStripChevronGap clear of the tiles
  // and at one place whatever the scroll position: a pointer left on one
  // keeps clicking it, and its place stays empty at the stop where it hides
  // (the left one's unscrolled, the right one's at the last stop), so a
  // click too many lands on no cover.
  const double ch = in.compact ? 28 : kStripChevronW;
  const double cy = in.y + m.art_y + (m.art_h - ch) / 2.0;
  if (S.first > 0) S.chevron_left = s.rc(in.x, cy, kStripChevronW, ch);
  if (S.first < S.max_first) S.chevron_right = s.rc(vr + kStripChevronGap, cy, kStripChevronW, ch);
  return S;
}

int strip_first_showing(const StripInput& in, int tile) {
  StripInput t = in;
  StripLayout S = layout_strip(t);
  if (tile < 0 || tile >= (int)S.cells.size() || S.whole[tile]) return S.first;
  // Earlier than the view: it becomes the first. Later: the least scroll that shows it.
  if (tile < S.first) return tile;
  for (int f = S.first + 1; f <= S.max_first; ++f) {
    t.first = f;
    if (layout_strip(t).whole[tile]) return f;
  }
  return S.max_first;
}

int strip_caption_room(int cell_w_px, int dpi) { return cell_w_px + 2 * focus_px(std::max(48, dpi)); }

int strip_caption_size(int room, const std::function<int(int)>& width_at) {
  for (int size : kStripCaptionSizes) {
    if (width_at && width_at(size) <= room) return size;
  }
  return kStripCaptionSizes[std::size(kStripCaptionSizes) - 1];
}

WindowLayout layout_window(const LayoutInput& in) {
  WindowLayout L;
  const int dpi = std::max(48, in.dpi);
  L.dpi = dpi;
  Scaler s{dpi};
  const int fm = focus_px(dpi);
  const bool strip = in.strip_tiles >= 2;
  // The client in DIPs; never laid out smaller than the minimum (the window
  // enforces it, and a smaller client just clips).
  const double W = std::max<double>(kMinClientW, in.client_w * 96.0 / dpi);
  const double H = std::max<double>(strip ? kMinClientHStrip : kMinClientH, in.client_h * 96.0 / dpi);
  // The content column: the window less its margins, at most kContentMaxW,
  // centred (on a whole DIP, so edges stay on the grid).
  const double C = std::min<double>(W - 2 * kMargin, kContentMaxW);
  const double x0 = std::floor((W - C) / 2);
  L.content = s.rc(x0, 0, C, H);

  L.header = s.rc(0, 0, W, kHeaderH);
  L.footer = s.rc(0, H - kFooterH, W, kFooterH);
  L.body = s.rc(0, kHeaderH, W, H - kHeaderH - kFooterH);
  L.logo = s.rc(x0, 4, 32, 32);
  L.title = s.rc(x0 + 32 + 12, 4, C - 44, 32);

  // The strip: a band across the column, right under the header. It takes
  // its compact form when the client is short, so the two columns below
  // always keep at least the height they have without it.
  double band = 0;
  if (strip) {
    const bool compact = H < kStripCompactBelow;
    const StripMetrics m = strip_metrics(compact);
    band = m.band;
    L.strip_mode = compact ? StripMode::compact : StripMode::regular;
    const double tiles_w = C - kStripStatusW - kStripStatusGap;
    L.strip = s.rc(x0, kHeaderH, tiles_w, m.cell_h);
    L.strip_status = s.rc(x0 + C - kStripStatusW, kHeaderH + m.art_y, kStripStatusW, m.art_h);
    L.strip_in = StripInput{in.strip_tiles, compact, x0, (double)kHeaderH, tiles_w, in.strip_first, dpi};
    L.tiles = layout_strip(L.strip_in);
  }

  const double top = kHeaderH + band, bottom = H - kFooterH - kGap;

  // Left column: mode, the module list, and (Random) the rotation block.
  const double lw = std::clamp(std::floor((C + 2 * kMargin) * 0.30 / 8) * 8, 296.0, 400.0);
  const double lx = x0;
  L.mode = s.rc(lx, top, lw, kControlH);
  L.mode_single = s.rc(lx, top, lw / 2, kControlH);
  L.mode_random = s.rc(lx + lw / 2, top, lw / 2, kControlH);
  const double label_y = top + kControlH + kGap;
  L.modules_label = s.rc(lx, label_y, lw - 128, kLabelH);
  L.modules_count = s.rc(lx + lw - 120, label_y, 120, kLabelH);
  const double card_y = label_y + kLabelH + 8;
  // Random: the rotation line, then "Change module every" under it, then
  // (several monitors) "A different module on each monitor".
  const bool per_monitor = in.random && in.per_monitor;
  const double per_monitor_h = per_monitor ? 8 + kControlH : 0;
  const double block = in.random ? 8 + kControlH + 8 + kControlH + per_monitor_h : 0;
  const double list_bottom = bottom - block;
  L.list_card = s.rc(lx, card_y, lw, list_bottom - card_y);
  L.list = s.rc(lx + 2, card_y + 4, lw - 4, list_bottom - card_y - 8);
  if (in.random) {
    // "Select all" and "Clear" are text links: Clear's text (not its box)
    // ends on the card's right edge, like the summary's starts on its left.
    const double ry = list_bottom + 8;
    const double none_tw = in.link_none_w > 0 ? in.link_none_w : 34, all_tw = in.link_all_w > 0 ? in.link_all_w : 58;
    const double none_w = none_tw + 2 * kLinkPad, all_w = all_tw + 2 * kLinkPad;
    const double right = lx + lw + kLinkPad;
    L.check_none = s.rc(right - none_w, ry, none_w, kControlH);
    L.check_all = s.rc(right - none_w - 4 - all_w, ry, all_w, kControlH);
    L.rotation_summary = s.rc(lx, ry, std::max(40.0, right - none_w - 4 - all_w + kLinkPad - 12 - lx), kControlH);
    const double dur_y = bottom - per_monitor_h - kControlH;
    L.duration_label = s.rc(lx, dur_y, lw - kDurationW - 12, kControlH);
    L.duration = s.rc(lx + lw - kDurationW, dur_y, kDurationW, kControlH);
    // A checkbox across the column, its box on the column's edge.
    if (per_monitor) L.per_monitor = s.rc(lx, bottom - kControlH, lw, kControlH);
  }

  // Right column: the module's details, then the saver-wide options.
  const double rx = lx + lw + kGap, rw = x0 + C - rx;
  // Two rows of labelled controls and the sound note's caption line.
  const double option_row = kLabelH + 4 + kControlH;
  const double options_h = kCardPad - 4 + option_row + 12 + option_row + 4 + kCaptionH + kCardPad - 4;   // 176
  const double oy = bottom - options_h;
  L.options_card = s.rc(rx, oy, rw, options_h);
  const double dy = top, dh = oy - kGap - top;
  L.details_card = s.rc(rx, dy, rw, dh);

  // Two columns in the details card: the preview with the About text under
  // it, and the module's own column (icon, name, controls), which is never
  // wider than kControlsMaxW. A wide window gives the rest to the preview (up
  // to kPreviewMaxW); what is left over stays as margin on the right.
  const double inner_x = rx + kCardPad, inner_w = rw - 2 * kCardPad;
  const double inner_y = dy + kCardPad, inner_h = dh - 2 * kCardPad;
  double pw = std::floor(inner_w * 0.52 / 16) * 16;
  if (inner_w - pw - 24 > kControlsMaxW) pw = std::floor((inner_w - 24 - kControlsMaxW) / 16) * 16;
  pw = std::clamp(pw, 256.0, (double)kPreviewMaxW);
  // Leave the About text at least ~5 lines under the preview.
  pw = std::min(pw, std::max(160.0, std::floor((inner_h - 16 - 100) * 16 / 9 / 16) * 16));
  const double ph = pw * 9 / 16;
  L.preview = s.rc(inner_x, inner_y, pw, ph);
  const double credits_h = 36;
  L.credits = s.rc(inner_x, inner_y + inner_h - credits_h, pw, credits_h);
  L.about = s.rc(inner_x, inner_y + ph + 16, pw, inner_h - ph - 16);

  const double spare = std::max(0.0, inner_w - pw - 24 - kControlsMaxW);
  const double sx = inner_x + pw + 24 + std::min(spare, 16.0);
  const double sw = std::min<double>(kControlsMaxW, inner_x + inner_w - sx);
  L.controls = s.rc(sx, inner_y, sw, inner_h);
  // The name beside the tile: one line in the subtitle face, or two in a
  // smaller one, with the chips under it either way.
  const bool two = in.title_lines >= 2;
  const double title_h = two ? 44 : 28, head_h = two ? 44 + 4 + 20 : 48;
  L.module_icon = s.rc(sx, inner_y, 48, 48);
  L.module_title = s.rc(sx + 60, inner_y, sw - 60, title_h);
  L.module_badge = s.rc(sx + 60, inner_y + title_h + (two ? 4 : 2), sw - 60, 20);
  // "Restore defaults" at the column's foot, its text on the credits' first
  // line; the controls above it scroll (by whole rows) when they don't all fit.
  const double defaults_y = inner_y + inner_h - credits_h - 8;
  L.defaults = s.rc(sx - kLinkPad, defaults_y, std::min(sw + kLinkPad, 176.0), kControlH);
  const double panel_y = inner_y + head_h + 20;
  {
    const int px0 = s.px(sx) - fm, py0 = s.px(panel_y) - fm;
    L.panel = Rc{px0, py0, s.px(sx + sw) + fm - px0, std::max(1, s.px(defaults_y - 8) - py0)};
  }

  // Options: two rows of labelled controls, each at the start of its half
  // of the card and at most kComboMaxW: Resolution and Monitors, then Sound
  // and Volume; the note on where sound plays under them, across the card.
  const double half = (rw - 2 * kCardPad - 24) / 2;
  const double cw = std::min<double>(kComboMaxW, half);
  for (int row = 0; row < 2; ++row) {
    const double ly = oy + kCardPad - 4 + row * (option_row + 12), cy = ly + kLabelH + 4;
    for (int i = 0; i < 2; ++i) {
      const double x = rx + kCardPad + i * (half + 24);
      Rc label = s.rc(x, ly, cw, kLabelH), control = s.rc(x, cy, cw, kControlH);
      if (row == 0 && i == 0) L.scale_label = label, L.scale = control;
      else if (row == 0) L.monitors_label = label, L.monitors = control;
      else if (i == 0) L.sound_label = label, L.sound = control;
      else {
        L.volume = control;
        L.volume_label = s.rc(x, ly, cw - kVolumeValueW - 8, kLabelH);
        L.volume_value = s.rc(x + cw - kVolumeValueW, ly, kVolumeValueW, kLabelH);
      }
    }
  }
  L.sound_note = s.rc(rx + kCardPad, oy + options_h - (kCardPad - 4) - kCaptionH, rw - 2 * kCardPad, kCaptionH);

  // Footer: Import and the assets line on the left, the actions on the right.
  const double fy = H - kFooterH + (kFooterH - kControlH) / 2;
  const double bw = 96, right = x0 + C;
  L.cancel = s.rc(right - bw, fy, bw, kControlH);
  L.ok = s.rc(right - 2 * bw - 8, fy, bw, kControlH);
  L.preview_button = s.rc(right - 2 * bw - 8 - 24 - 120, fy, 120, kControlH);
  L.import = s.rc(x0, fy, 112, kControlH);
  const double ax = x0 + 112 + 12;
  L.assets = s.rc(ax, H - kFooterH + 12, (right - 2 * bw - 8 - 24 - 120 - 24) - ax, kFooterH - 24);
  return L;
}

FooterCreditLayout layout_footer_credit(const WindowLayout& L, const FooterCreditInput& in) {
  FooterCreditLayout C;
  const int dpi = L.dpi;
  const int gap = dip(kCreditGapDip, dpi), pad = dip(kLinkPad, dpi), box_h = dip(kCreditLinkHDip, dpi);
  if (in.lead_w <= 0 || in.name_w <= 0 || in.line_h <= 0 || L.preview_button.empty()) return C;
  const int text_w = in.lead_w + std::max(0, in.space_w) + in.name_w;
  const int box_w = pad + text_w + pad;
  const int left = in.assets_right + gap, right = L.preview_button.x - gap;
  if (right - left < box_w) return C;
  // Centred in the free space, and on the footer's buttons.
  const int x = left + (right - left - box_w) / 2;
  const int cy = L.preview_button.y + L.preview_button.h / 2;
  const int ty = cy - in.line_h / 2;
  C.shown = true;
  C.box = Rc{x, cy - box_h / 2, box_w, box_h};
  C.lead = Rc{x + pad, ty, in.lead_w, in.line_h};
  C.name = Rc{x + pad + in.lead_w + std::max(0, in.space_w), ty, in.name_w, in.line_h};
  return C;
}

PanelLayout layout_panel(const std::vector<Control>& controls, int width, int dpi, int note_h,
                         const std::vector<bool>* live_buttons) {
  PanelLayout P;
  Scaler s{std::max(48, dpi)};
  const double W = width * 96.0 / s.dpi;
  const double note = note_h > 0 ? note_h * 96.0 / s.dpi : 16;
  double y = 0, end = 0;
  bool any_settable = false;
  for (size_t ci = 0; ci < controls.size(); ++ci) {
    const Control& c = controls[ci];
    PanelRow r;
    any_settable |= c.settable();
    const double top = y;
    const bool live = c.type == ControlType::button && live_buttons && ci < live_buttons->size() && (*live_buttons)[ci];
    // A live button counts as something to set: the panel is not "empty".
    any_settable |= live;
    if (live) {
      // The button (full control height; position_panel fits its width to
      // the text), and a line under it for what the last run left to say.
      r.input = s.rc(0, y, W, kControlH);
      r.value = s.rc(0, y + kControlH + 4, W, note);
      end = y + kControlH + 4 + note;
      r.top = s.px(top);
      r.bottom = s.px(end);
      P.rows.push_back(r);
      y = end + 12;
      continue;
    }
    switch (c.type) {
      case ControlType::slider: {
        // 4 DIP under the label: room for the slider's focus ring.
        const double vw = std::min(W / 2, 136.0);
        r.label = s.rc(0, y, W - vw - 8, kLabelH);
        r.value = s.rc(W - vw, y, vw, kLabelH);
        r.input = s.rc(0, y + kLabelH + 4, W, kControlH);
        end = y + kLabelH + 4 + kControlH;
        break;
      }
      case ControlType::popup:
        if (!blank_label(c.name)) {
          r.label = s.rc(0, y, W, kLabelH);
          y += kLabelH + 4;
        }
        r.input = s.rc(0, y, W, kControlH);
        end = y + kControlH;
        break;
      case ControlType::checkbox:
        r.input = s.rc(0, y, W, kControlH);
        end = y + kControlH;
        break;
      case ControlType::button:
        // Read-only: the name, and why it can't be used here.
        r.label = s.rc(0, y, W, kLabelH);
        r.input = s.rc(0, y + kLabelH + 2, W, note);
        end = y + kLabelH + 2 + note;
        break;
      default:
        r.label = s.rc(0, y, W, kLabelH);
        end = y + kLabelH;
        break;
    }
    r.top = s.px(top);
    r.bottom = s.px(end);
    P.rows.push_back(r);
    y = end + (c.type == ControlType::checkbox ? 8 : 12);
  }
  if (!any_settable) {
    P.empty_note = s.rc(0, y, W, kLabelH);
    end = y + kLabelH;
  }
  P.content_h = s.px(end);
  return P;
}

} // namespace adw::scr
