// Window-free pieces of the settings dialog's presentation, kept here so the
// tests can pin them: how a module's About text is tidied for display, the
// "Change module every" choices, the summary lines, and the layout of the
// whole window at any DPI (config_dialog.cc applies it).
#pragma once

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

#include "catalog.h"

namespace adw::scr {

// ---- text ------------------------------------------------------------------------

// The catalog's About text as the dialog shows it. The originals were
// hard-wrapped for a narrow 1990s dialog and usually open with the module's
// name, so: the opening name line is dropped (the dialog shows the name
// already), lines broken mid-sentence only to fit the old dialog are
// rejoined, and the intended breaks (poems, credits, "Windows version by:"
// lists) are kept.
// Paragraphs are separated by one blank line; typewriter double spaces
// after a full stop become one.
std::string tidy_about(const std::string& about, const std::string& display_name);

// "Change module every" (DurationMin): the offered choices, in order. A value
// the file holds that isn't one of the presets is offered too, in its place,
// so opening and saving the dialog never changes it.
struct DurationChoice {
  int minutes = 0;             // 0 = never
  std::wstring label;          // "5 minutes", "1 hour", "Never"
};
std::vector<DurationChoice> duration_choices(int current_minutes);
std::wstring duration_label(int minutes);

// "A different module on each monitor" (Settings::different_per_monitor),
// under "Change module every" in Random: shown only while there is more than
// one monitor (with one there is nothing to tell apart, so the row isn't
// there and the list keeps its room; it comes and goes with a monitor
// plugged in or out), and enabled while every monitor plays (Monitors: All
// monitors); with "Primary monitor only" it is greyed and keeps its check.
// However it shows, OK saves it as it stands, so the file's value stays until
// the user changes it.
struct PerMonitorChoice {
  bool shown = false, enabled = false;
  bool operator==(const PerMonitorChoice&) const = default;
};
PerMonitorChoice per_monitor_choice(bool random, bool all_monitors, int monitors);

// The Random mode line under the module list: "All 84 in rotation" /
// "12 of 84 in rotation" / "None in rotation". `runnable` is how many of the
// checked modules this adhostwin can run now (-1: not known yet). When some
// can't (their lane or module ABI isn't built in), the line says so rather
// than promising a rotation the saver can't play: "All 84 selected · 23 can
// run now".
// `distinct` is how many different modules those checks are (-1: not
// counted): a module several releases ship byte for byte plays once per pass
// (COVERS.md §1.8), so with copies checked the line says what rotates, "All
// 202 selected · 129 distinct". What can't run is said first.
// A list of one module (Marvel Comics Screen Posters, alone or filtered to)
// is "1 in rotation" or "1 selected · 0 can run now", never "All 1".
std::wstring rotation_summary(size_t checked, size_t total, long long runnable = -1, long long distinct = -1);

// The rotation line's tooltip ("" = none): that identical copies play once
// per pass (when `distinct` < `checked`), and which module the file names to
// play first (`lead`, a display name; "" when none).
std::wstring rotation_tip(size_t checked, long long distinct, const std::wstring& lead);

// The assets line in the footer: "202 modules from 5 releases", or with one
// release "84 modules from After Dark 4.0 Deluxe", with " · 2 missing —
// import again to restore" when files are gone, or "Nothing imported yet"
// when the catalog is empty (COVERS.md §1.7). It is a status line, so there
// is no closing full stop.
struct AssetCounts {
  size_t total = 0, releases = 0, missing = 0;
  std::string release;   // the title of the one release there is ("" otherwise, or when it has none of its own)
};
AssetCounts count_assets(const Catalog& c, const std::vector<bool>& present);
std::wstring assets_summary(const AssetCounts& a);

// The not-imported welcome's text (under "Welcome to Long After Dark"): what
// importing does, for every release (eleven of After Dark modules, and Star
// Wars Screen Entertainment).
std::wstring welcome_text();

// ---- string-slider stops ---------------------------------------------------------
// The original control panel appends a stop to many Classic string sliders
// (ABI.md §2.10.5, boldStop), so labels repeat on adjacent stops: "Never,
// Once, Twice, Always, Always, Always". Moving the thumb between two of those
// changes nothing the user can see, so the dialog shows each run of equal
// labels as one stop. Stored values are never rewritten behind the user's
// back: a value inside a run shows that run, and choosing a run stores the
// catalog's default when it lies in the run, else the appended (bold) stop's
// value when that does, else the run's last value (so 100 stays 100).
struct VisualStops {
  std::vector<std::string> labels;   // one per run, in order
  std::vector<int> first, last;      // the run's raw stops (Control::items indices)
  std::vector<int> value;            // what choosing the run stores
  int count() const { return (int)labels.size(); }
  int run_of_stop(int raw_stop) const;
  int run_of_value(const Control& c, int v) const { return run_of_stop(c.stop_of(v)); }
};
VisualStops visual_stops(const Control& c);

// ---- thumbnails --------------------------------------------------------------------
// A module with no icon of its own gets a list tile taken from a frame of it
// running: the third-of-the-screen square where the most stands out from
// the background (a toaster is found wherever it flies, and fills the
// tile; a bright sprite wins over dim lines), none while the frame is blank.
struct ThumbCrop {
  int x = 0, y = 0, side = 0;        // in frame pixels
  double busy = 0;                   // share of the square not in the background colour, 0..1
};
// `rgb_at(x, y)` returns 0xRRGGBB. Pure, for the tests.
template <typename RgbAt>
ThumbCrop choose_thumb_crop(int w, int h, RgbAt rgb_at);

// Is a crop worth keeping, and how much is there to see in it? `bgr` is
// top-down rows of w*3 bytes. Kept only with some contrast (luminance
// standard deviation of at least kThumbMinStddev), some colour
// (kThumbMinColours 4-bit-per-channel colours, each on at least 0.2% of the
// pixels) and not all but one flat tone (at most kThumbMaxFlat of the pixels
// within 10 of the mean luminance): that turns away a blank or near-black
// screen, a white one, a speck, a line or two of text, a two-tone blob and
// sparse line art (specks at tile size), but keeps a 16-colour sprite on
// black, a maze and a soft pattern. `score` ranks the keepers (0 for the rest).
struct ThumbQuality {
  double stddev = 0;
  int colours = 0;
  double flat = 1;
  bool good = false;
  double score = 0;
};
inline constexpr double kThumbMinStddev = 12, kThumbMaxFlat = 0.95;
inline constexpr int kThumbMinColours = 3;
ThumbQuality judge_thumb(const unsigned char* bgr, int w, int h);

// ---- layout ----------------------------------------------------------------------
// Rectangles in client pixels. Everything is designed on a 4/8-DIP grid at
// 96 DPI and scaled by dpi/96; `layout_window` is pure arithmetic so the tests
// can run it at any DPI and size.

struct Rc {
  int x = 0, y = 0, w = 0, h = 0;
  int right() const { return x + w; }
  int bottom() const { return y + h; }
  bool empty() const { return w <= 0 || h <= 0; }
  bool contains(const Rc& o) const { return o.x >= x && o.y >= y && o.right() <= right() && o.bottom() <= bottom(); }
  bool overlaps(const Rc& o) const { return x < o.right() && o.x < right() && y < o.bottom() && o.y < bottom(); }
  bool operator==(const Rc&) const = default;
};

// Design sizes, DIPs.
inline constexpr int kDesignClientW = 1040, kDesignClientH = 680;   // first-open client size
inline constexpr int kMinClientW = 900, kMinClientH = 600;          // the window can't shrink past this
// With the box-cover strip (two or more releases, COVERS.md §1.2): the
// regular band on top of those, and a compact band below kStripCompactBelow.
inline constexpr int kDesignClientHStrip = 800, kMinClientHStrip = 680, kStripCompactBelow = 760;

// ---- the box-cover strip (COVERS.md §1.2, §1.3) ------------------------------------
// One 4:5 tile per release across the top of the content column, left-aligned
// (a row that overflows starts after the left chevron's zone), with a status
// box at the column's right edge. DIPs.
enum class StripMode { hidden, regular, compact };
struct StripMetrics {
  int art_w, art_h;       // the cover (4:5)
  int cell_w, cell_h;     // art + margins (+ caption row)
  int art_x, art_y;       // the art's offset in the cell
  int caption_h;          // 0: no caption (compact)
  int pitch;              // cell + gap
  int band;               // cells + the 12-DIP gap under them
};
StripMetrics strip_metrics(bool compact);
inline constexpr int kStripStatusW = 200, kStripStatusGap = 16, kStripChevronW = 24;
// A chevron's zone is the button and this gap, which keeps it clear of the
// tiles beside it (of a tile's focus ring too, drawn a focus margin past its cell).
inline constexpr int kStripChevronGap = 4;

struct StripInput {
  int tiles = 0;
  bool compact = false;
  double x = 0, y = 0, w = 0;   // the tiles area (the column less the status box), DIPs
  int first = 0;                // scroll position: the first tile shown (clamped to the stops)
  int dpi = 96;
};
struct StripLayout {
  StripMode mode = StripMode::hidden;
  int first = 0, max_first = 0;     // scroll position (whole tiles) and its last stop
  bool overflow = false;            // more tiles than the area holds side by side: the row scrolls
  int slots = 0;                    // how many tiles show at a time (all of them without overflow)
  Rc area;                          // the tiles area
  Rc view;                          // where tiles show: the area, or overflowing, the slots between the chevrons' zones
  std::vector<Rc> cells, arts, captions;   // per tile (captions empty when compact)
  std::vector<bool> whole;          // the cell lies wholly inside `view`: the tile shows (the others don't, at all)
  Rc chevron_left, chevron_right;   // 24-DIP buttons at an end with more beyond it (empty otherwise)
};
// Pure: whole-cell scrolling. Overflowing, the row shows `slots` tiles at
// every scroll stop, as many whole cells as fit between the two chevrons'
// zones. Tile i sits at x + lead + (i - first) * pitch, where lead is the
// left chevron's zone whenever the row overflows (unscrolled too, where that
// zone stays empty: each slot keeps one place across the stops), and 0
// otherwise (the tiles left-aligned on the column). Each chevron has one
// place whatever the scroll position: the left one at the area's left edge,
// the right one just past the last slot, and no tile shows in either place
// at any stop (a click too many on a chevron that hides lands on no cover).
// A tile the view can't hold whole is not shown, so no cover or caption is
// ever cut and nothing lies under a chevron.
StripLayout layout_strip(const StripInput& in);
// The scroll position that brings `tile` wholly into view, moving as little as can be.
int strip_first_showing(const StripInput& in, int tile);
// A regular tile's caption (the release's shortTitle) may use the tile's whole
// window: its cell and the focus margin on each side (px), wider than the cell
// so "10th Anniversary" fits at every scale. A title that still doesn't fit is
// drawn a size smaller (11, then 10 DIP), and only then ellipsized.
inline constexpr int kStripCaptionSizes[] = {12, 11, 10};
int strip_caption_room(int cell_w_px, int dpi);
// The first of kStripCaptionSizes at which the text fits `room` px
// (`width_at(size)`: its width in px at `size` DIP); the last when none does.
int strip_caption_size(int room, const std::function<int(int)>& width_at);

// Caps for large windows (DIPs): Windows 11 keeps setting controls to about
// this width, and a preview larger than this is no longer a glance. Past
// kContentMaxW the whole content (header, cards, footer buttons) stops
// growing and is centred, the rest of the width left as margin.
inline constexpr int kControlsMaxW = 360, kComboMaxW = 280, kPreviewMaxW = 560;
inline constexpr int kContentMaxW = 1240;
// "Change module every"'s dropdown, beside its label under the list (Random).
inline constexpr int kDurationW = 148;
// The Volume readout ("100"), right-aligned at the end of its label row.
inline constexpr int kVolumeValueW = 40;
inline constexpr int kLinkPad = 8;  // a text link's box reaches this far past its text

struct LayoutInput {
  int client_w = 0, client_h = 0;   // pixels
  int dpi = 96;
  // Random mode: under the list, the rotation line (summary, Select all,
  // Clear) and "Change module every", which only Random uses.
  bool random = true;
  // ...and under them, on a PC with several monitors, "A different module on
  // each monitor" (per_monitor_choice().shown).
  bool per_monitor = false;
  // Text widths (DIPs) of "Select all" and "Clear", so that their text, not
  // their boxes, lines up with the list card's right edge. 0 = a typical width.
  int link_all_w = 0, link_none_w = 0;
  // The module's name on one line, or (too long for the column even in the
  // smaller face) wrapped onto two, with its chips under them.
  int title_lines = 1;
  // The box-cover strip: one tile per release when there are two or more
  // (fewer: no strip), scrolled to `strip_first`.
  int strip_tiles = 0;
  int strip_first = 0;
};

struct WindowLayout {
  int dpi = 96;
  // Regions (painted by the dialog itself).
  Rc header, body, footer;          // header band, content, footer band (full width)
  Rc content;                       // the column everything is laid out in (at most kContentMaxW, centred)
  Rc list_card, details_card, options_card;
  // Header content.
  Rc logo, title;                   // app icon; the title text box (title + tagline)
  // Left column.
  Rc mode;                          // the segmented Single/Random control (both halves)
  Rc mode_single, mode_random;
  Rc modules_label, modules_count;  // "Modules" and its count, above the list card
  Rc list;                          // the list view inside list_card
  Rc rotation_summary, check_all, check_none;   // Random only (empty otherwise)
  Rc duration_label, duration;      // "Change module every", under them (Random only)
  Rc per_monitor;                   // "A different module on each monitor", under that (LayoutInput::per_monitor)
  // Details card.
  Rc preview;                       // live preview, 16:9
  Rc about, credits;                // under the preview
  Rc controls;                      // the right-hand column (icon, name, controls), at most kControlsMaxW
  Rc module_icon, module_title, module_badge;
  Rc panel;                         // per-module controls (IDC_PANEL), grown by the focus margin
                                    // on each side so the controls' faces line up with `controls`
  Rc defaults;                      // "Restore defaults" at the column's foot, centred on the credits'
                                    // first line (the dialog lifts it to just under the last row of
                                    // settings when they all fit); its glyph sits on the column's
                                    // edge (the box starts kLinkPad before)
  // Options card: two rows of two, each labelled and at the start of its half
  // of the card -- Resolution and Monitors, then Sound (a dropdown) and
  // Volume (a slider with its readout at the end of its label row) -- and a
  // caption line under them saying where sound plays (AUDIO.md §9).
  Rc scale_label, scale, monitors_label, monitors;
  Rc sound_label, sound, volume_label, volume_value, volume, sound_note;
  // Footer.
  Rc assets, import, preview_button, ok, cancel;
  // The box-cover strip, between the header and the two columns (hidden:
  // all empty, and the columns start right under the header).
  StripMode strip_mode = StripMode::hidden;
  Rc strip;                         // the tiles area, as tall as the cells
  Rc strip_status;                  // the status box: the column's right edge, centred on the art
  StripInput strip_in;              // to lay the tiles out again at another scroll position
  StripLayout tiles;
};

// px for a DIP length at `dpi` (rounded to nearest).
int dip(int dips, int dpi);
WindowLayout layout_window(const LayoutInput& in);

// ---- the footer's credit ---------------------------------------------------------------
// "Made With Love by StarrLord" in the footer, in the free space between the
// assets line ("284 modules from 12 releases") and Preview: one link to the
// project's page in the dialog's link look (adw_ui's ButtonRole::subtle, as
// "Show all" and "Get the covers": its box kLinkPad past its text, a fill on
// hover and a deeper one pressed, the focus ring around it, a hand pointer),
// its text in the caption face, like the assets line beside it: the lead in
// text2, the name in the accent text colour, underlined under the pointer.
// It is centred in that space and on the footer's buttons, and shows only
// when all of its box fits with kCreditGapDip clear of the assets line's text
// and of Preview: never clipped, never touching them.
inline constexpr wchar_t kFooterCreditLead[] = L"Made With Love by";
inline constexpr wchar_t kFooterCreditName[] = L"StarrLord";
inline constexpr wchar_t kFooterCreditUrl[] = L"https://github.com/starrlord/longafterdark";
inline constexpr int kCreditGapDip = 24;    // clear of the assets line's text and of Preview (the footer's gap)
inline constexpr int kCreditLinkHDip = 24;  // the link's box, as tall as the strip's "Show all"

struct FooterCreditInput {
  int assets_right = 0;   // px: where the assets line's text ends (its widest line)
  int lead_w = 0;         // px: kFooterCreditLead in the caption face
  int space_w = 0;        // px: a space in it
  int name_w = 0;         // px: kFooterCreditName in it
  int line_h = 0;         // px: a line of it
};
struct FooterCreditLayout {
  bool shown = false;
  Rc box;                 // the link's box: the phrase and kLinkPad each side (hover fill, focus ring, clicks)
  Rc lead, name;          // the two parts of the phrase, on one line, a space apart
};
// Pure: from the window's layout (its footer: Preview) and the texts' sizes.
FooterCreditLayout layout_footer_credit(const WindowLayout& L, const FooterCreditInput& in);

// Rows of the per-module settings panel (IDC_PANEL), top to bottom, for the
// module's controls in catalog order. Labels sit above sliders and dropdowns,
// with the value readout right-aligned on the label row; a dropdown whose
// catalog name is blank gets no label row. A module button the host can open
// (`live_buttons`, INTERACTION.md §6.3) is a push button (`input`) with a
// line for a note under it (`value`); one it can't is a read-only row: its
// name, and a note under it (`input`).
// The panel scrolls by whole rows: `top`/`bottom` bound each row.
struct PanelRow {
  Rc label, input, value;   // empty when the row has no such part
  int top = 0, bottom = 0;  // the row's extent, px
};
struct PanelLayout {
  std::vector<PanelRow> rows;
  Rc empty_note;            // "This module has no settings."; empty otherwise
  int content_h = 0;        // total height (scrolls past the panel when larger)
};
// `note_h`: a note's height in px (0 = one caption line). `live_buttons`
// (optional, one flag per control) marks the button rows that are live.
PanelLayout layout_panel(const std::vector<Control>& controls, int width, int dpi, int note_h = 0,
                         const std::vector<bool>* live_buttons = nullptr);
// A live module button's width: its text plus padding, within the column.
inline constexpr double kPanelButtonMinDip = 120, kPanelButtonPadDip = 24;
// A catalog name with nothing to show: only spaces and colons.
bool blank_label(const std::string& name);

// ---- the module list's group headers ------------------------------------------------
// `text` cut to fit `room` px with an ellipsis after it ("Star Wars Screen
// Entert…"), `measure` giving a string's width in px: the text itself when
// it fits, "" when not even the ellipsis does. Spaces before the ellipsis go.
std::wstring ellipsize(const std::wstring& text, int room, const std::function<int(const std::wstring&)>& measure);

// A release's header in the module list (the dialog paints it): its title,
// the number of its modules `gap` after it, and at the right, when none of
// its modules can run, the "Coming soon" pill. The title gives way,
// ellipsized, so that the count and the pill always show whole: "Star Wars
// Screen Entertainment" in the narrowest list keeps its "14" (a title never
// runs into them). Pixels: the title starts at `left`; the title and the
// count end by `right`; the pill (`pill_w` wide; 0: none) ends at
// `pill_right`, `gap` clear of the count.
struct GroupHeaderInput {
  std::wstring title;
  int left = 0, right = 0;
  int count_w = 0;
  int gap = 0;
  int pill_w = 0, pill_right = 0;
};
struct GroupHeaderLayout {
  std::wstring title;       // as drawn: whole, or ellipsized ("" when not even "…" fits)
  bool ellipsized = false;
  int title_w = 0;          // its width
  int count_x = 0;          // where the count starts
  int pill_x = 0;           // where the pill starts (with a pill)
};
GroupHeaderLayout layout_group_header(const GroupHeaderInput& in,
                                      const std::function<int(const std::wstring&)>& measure_title);
// A row's checkbox in Random, and a group header's (DIPs).
inline constexpr int kListBoxDip = 20;
// A group header's frame across a list `list_w` px wide, as the dialog lays
// it out: the title from 16 DIP in (in Random, after the group's checkbox and
// 12 DIP), the title and count ending 16 DIP short of the right edge, the
// pill 8 DIP short of the scroll bar's gutter (12 DIP while the list
// scrolls, else 4), `gap` 8 DIP. The title and the widths are left to fill in.
GroupHeaderInput group_header_frame(int list_w, int dpi, bool random, bool scrolls);

// ---- template definitions ---------------------------------------------------------

template <typename RgbAt>
ThumbCrop choose_thumb_crop(int w, int h, RgbAt rgb_at) {
  ThumbCrop best;
  if (w <= 0 || h <= 0) return best;
  const int small = std::min(w, h);
  const int third = std::max(1, small / 3);
  auto key = [](unsigned rgb) { return (int)(((rgb >> 20) & 0xF) << 8 | ((rgb >> 12) & 0xF) << 4 | ((rgb >> 4) & 0xF)); };
  // The background: the commonest colour of the whole frame.
  std::vector<int> hist(4096, 0);
  const int gstep = std::max(1, small / 96);
  for (int y = 0; y < h; y += gstep) {
    for (int x = 0; x < w; x += gstep) ++hist[key(rgb_at(x, y))];
  }
  const int bg = (int)(std::max_element(hist.begin(), hist.end()) - hist.begin());
  const int br = (bg >> 8) * 17, bgg = ((bg >> 4) & 0xF) * 17, bb = (bg & 0xF) * 17;
  // How much a pixel stands out from the background (0..1): a bright sprite
  // counts for more than a dim line or a speck of noise.
  auto salience = [&](unsigned rgb) {
    if (key(rgb) == bg) return 0.0;
    const int d = std::abs((int)((rgb >> 16) & 0xFF) - br) + std::abs((int)((rgb >> 8) & 0xFF) - bgg) +
                  std::abs((int)(rgb & 0xFF) - bb);
    return std::min(1.0, d / 255.0);
  };
  auto busy_in = [&](int x0, int y0, int side) {
    const int step = std::max(1, side / 24);
    int n = 0;
    double busy = 0;
    for (int y = y0; y < y0 + side; y += step) {
      for (int x = x0; x < x0 + side; x += step, ++n) busy += salience(rgb_at(x, y));
    }
    return n ? busy / n : 0.0;
  };
  // Where the picture is: the clearly salient samples' bounding box, less
  // stray specks (the outer 4% on each side).
  std::vector<int> xs, ys;
  for (int y = 0; y < h; y += gstep) {
    for (int x = 0; x < w; x += gstep) {
      if (salience(rgb_at(x, y)) >= 0.35) {
        xs.push_back(x);
        ys.push_back(y);
      }
    }
  }
  if (xs.empty()) {
    // Nothing that stands out: at least whatever differs from the background.
    for (int y = 0; y < h; y += gstep) {
      for (int x = 0; x < w; x += gstep) {
        if (key(rgb_at(x, y)) != bg) {
          xs.push_back(x);
          ys.push_back(y);
        }
      }
    }
  }
  if (xs.empty()) return ThumbCrop{(w - third) / 2, (h - third) / 2, third, 0.0};
  std::sort(xs.begin(), xs.end());
  std::sort(ys.begin(), ys.end());
  const size_t lo = xs.size() * 4 / 100, hi = xs.size() - 1 - xs.size() * 4 / 100;
  const int bx0 = xs[lo], bx1 = xs[hi] + gstep, by0 = ys[lo], by1 = ys[hi] + gstep;
  const int extent = std::max(bx1 - bx0, by1 - by0);
  // Something compact (a sprite, a painting on a wall): all of it, framed
  // with a little margin, never closer than a quarter of the screen.
  if (extent * 5 / 4 <= small / 2) {
    const int side = std::clamp(extent * 5 / 4, std::max(1, small / 4), std::max(1, small / 2));
    const int x0 = std::clamp((bx0 + bx1) / 2 - side / 2, 0, w - side), y0 = std::clamp((by0 + by1) / 2 - side / 2, 0, h - side);
    return ThumbCrop{x0, y0, side, busy_in(x0, y0, side)};
  }
  // Spread over the screen: the most salient third-height square.
  const int side = third;
  const int step = std::max(1, side / 24);
  double best_score = -1;
  for (int iy = 0; iy <= 6; ++iy) {
    for (int ix = 0; ix <= 12; ++ix) {
      const int x0 = (w - side) * ix / 12, y0 = (h - side) * iy / 6;
      const double frac = busy_in(x0, y0, side);
      // Nearer the centre wins a close call.
      const double dx = (x0 + side / 2.0) / w - 0.5, dy = (y0 + side / 2.0) / h - 0.5;
      const double score = frac * (1.0 - 0.3 * std::sqrt(dx * dx + dy * dy));
      if (score > best_score) {
        best_score = score;
        best = ThumbCrop{x0, y0, side, frac};
      }
    }
  }
  // Then centred on what stands out in that square (the grid is coarse).
  double sx = 0, sy = 0, n = 0;
  for (int y = best.y; y < best.y + side; y += step) {
    for (int x = best.x; x < best.x + side; x += step) {
      const double s = salience(rgb_at(x, y));
      sx += s * x;
      sy += s * y;
      n += s;
    }
  }
  if (n > 0) {
    const int x0 = std::clamp((int)(sx / n) - side / 2, 0, w - side), y0 = std::clamp((int)(sy / n) - side / 2, 0, h - side);
    best = ThumbCrop{x0, y0, side, busy_in(x0, y0, side)};
  }
  return best;
}

} // namespace adw::scr
