# Long After Dark — collections, covers and the shared UI

This file specifies four pieces of work that ship together:

1. **The box-cover strip** in the settings dialog. One tile per installed
   release sits at the top of the window. Clicking tiles filters the module
   list, and the list is grouped by release instead of by lane.
2. **The cover pipeline** in the importer. For each package it captures a
   default cover, lets the user replace it with an image of their own,
   stores both as PNGs under the assets root, and describes them in the
   catalog.
3. **`adw_ui`**, a small static library. It holds the settings dialog's
   Windows 11 theming, now shared by `LongAfterDark.scr` and `adimport.exe`.
4. **The importer GUI restyle**. Every `adimport --gui` window follows the
   light, dark and high-contrast modes live, matching the settings dialog.

DESIGN.md §9 gives the contract in brief. §8 of this file assigns the work
to three implementers (T, C and S) whose files do not overlap.

**Status: implemented** (2026-09-26). This file keeps the specification as
it was written; §9 records what the implementations settled or added, and
the component READMEs (`scr`, `importer`, `importer/gui`, `common/ui`) are
authoritative for how each part behaves.

**Out of scope for this pass:**
* **Module interaction** (INTERACTION.md: module-owned dialogs, message
  input, games that take the keyboard). The user moved it to a follow-up
  after everything else. Nothing here depends on it or changes it. The
  settings dialog's module-button rows and their tests stay exactly as they
  are.
* **The "Long After Dark" rename.** It came after this pass, which kept
  every name, path, window title and class name of the time. They have since
  been renamed, and this file uses the new ones: the window title "Long
  After Dark", the data folder `%LOCALAPPDATA%\LongAfterDark` (DESIGN.md
  §6) and the saver `LongAfterDark.scr`.
* **The documentation and wording pass** (a later pass of its own). A few strings
  listed here changed because the lanes disappear from the UI; everything
  else was left to that pass, which has since been done.

Terms. A **release** is a package in the importer's registry (PACKAGES.md
§2): `deluxe`, `ad10`, `ad32`, `tt`, `simpsons`, since the sixth release
`swse` (Star Wars Screen Entertainment), since the seventh `startrek`
(Star Trek: The Screen Saver), and, added together as the eighth to the
twelfth, `marvel`, `snoopy`, `looney`, `screams` and `disney` (Marvel
Comics Screen Posters, Snoopy's Screen Savers, The Looney Tunes Screen
Saver, ScreamSavers, The Disney Collection Screen Saver), and as the
thirteenth and fourteenth, Delrina's `farside` and `dilbert` (The Far Side
Screen Saver Collection, Scott Adams' Dilbert Screen Saver Collection), as
the fifteenth `tng` (Star Trek: The Next Generation Screen Saver), as
the sixteenth Sierra On-Line's `castaway` (Screen Antics: Johnny
Castaway), and as the seventeenth to the twentieth Delrina's `opus`,
`opusroad`, `flintstones` and `intermission` (Opus 'n Bill Screen Saver,
Opus 'n Bill: On the Road Again!, The Flintstones Screen Saver Collection,
Intermission 4.0). The UI calls a release by
its `title`, or by its `shortTitle` where space is tight. A **cover** is the
art that stands for a release. A **tile** is the cover rendered as a 4:5
portrait PNG for display. The **filter** is the set of releases whose tiles
are selected.

## 0. Decisions at a glance

| Question | Decision |
|---|---|
| Where the strip goes | A band across the content column, between the header band and the two columns (§1.2) |
| When it shows | Only when **two or more** releases are installed. With one or none it is hidden and the filter is "all" (§1.1) |
| Tile shape and size | 4:5 portrait. Regular art is 64×80 DIP with a caption; compact art is 48×60 DIP with no caption, used when the client is under 760 DIP tall (§1.2) |
| Nothing selected | Means **all releases**. Several tiles can be selected at once. Clicking a tile toggles it (§1.6) |
| Module list | Grouped **by release**, one group per installed release, oldest release first, titled with the release title. The lane never shows (§1.7) |
| Duplicates | Each release's copy is its own row under its own release, shown by `moduleName` (no " (Totally Twisted)" suffix inside a group) (§1.7) |
| Random mode | The saver plays the checked modules **in the selected releases**, and copies with the same bytes (`sameAs`) play once per pass. Each group header's checkbox is its release's "check all" (§1.8) |
| Persistence | `[Saver] Collections=<id>,…` (empty or missing = all), read by the dialog and by the saver (§1.9) |
| Cover order | The user's own picture, then the registry's sources in their listed order (downloads checked by md5, on-disc art with its crop), then a generated cover (§2.1) |
| Where covers live | `<win>\covers\<id>\` (`original.png`, `user.png`, `tile.png`, `cover.json`), never inside a package root and never in the repo (§2.5) |
| Catalog | `packages[].cover` gives origin, tile path, md5, size and label. Catalog version 1, additive (§2.7) |
| CLI | `--set-cover <id> <file>`, `--clear-cover <id>`, `--refresh-covers [<id>\|all] [--force]`, `--no-cover-download`, `--gui --change-cover <id>` (§2.8) |
| Offline | A failed cover never fails an import. The disc art or a generated cover stands in, and `--refresh-covers` upgrades it later (§2.10) |
| Shared theme | `common/ui/`, static library `adw_ui`, namespace `adw::ui`, headers `adw/ui/*.h`. Its API is the scr's `ui_theme.h`, `ui_widgets.h` and `ui_capture.h` moved as they are, plus the additions in §3.2. No Mica (§3) |
| Importer GUI | Custom themed windows replace every task dialog. The window title, the command ids (101/102/103, 200+i, 299, IDCANCEL) and `TDM_CLICK_BUTTON` keep working, so `import.cli`'s GUI block passes unchanged (§4) |
| Who does what | T = `adw_ui` + importer GUI. C = importer non-GUI (registry, covers, catalog, CLI). S = `scr/**` (§8) |

## 1. The settings dialog: the box-cover strip

### 1.1 When it shows

* The strip shows when the catalog's `packages` array has **two or more**
  entries (installed releases).
* With one release installed it is hidden, and the list keeps its single
  release group, titled with that release's title. The filter is "all" and
  the saved `Collections` value is kept as it is. With nothing installed
  (the welcome state) it is hidden too.
* A catalog from before this pass (no `packages` array, no `package` on its
  modules) is read as described in §1.10. It never has two releases, so it
  never shows the strip.

### 1.2 Placement and size

The strip is a band at the very top of the body, directly under the header
band (`kHeaderH` = 48). It spans the content column (`L.content`, at most
1240 DIP, centred). Everything that sits at `top` today (the Single/Random
control and the details card) moves down by the band's height.

| | Regular | Compact |
|---|---|---|
| When | client height ≥ 760 DIP | client height < 760 DIP |
| Tile art | 64 × 80 DIP (4:5) | 48 × 60 DIP (4:5) |
| Cell (art + margins + caption) | 96 × 108 DIP: art at (+16, +4), caption row 16 DIP at art bottom + 4 | 64 × 68 DIP: art at (+8, +4), no caption row |
| Pitch (cell + gap) | 104 DIP | 72 DIP |
| Band height (cells + 12 DIP gap below) | 120 DIP | 80 DIP |

* **Window size.** With the strip, the first-open client size is 1040 × 800
  DIP (today's 1040 × 680 plus the regular band), clamped to the monitor's
  work area. A clamped height under 760 DIP gets the compact strip. With
  the strip the minimum client height is **680 DIP** (compact band 80 +
  today's 600). Without the strip, sizes stay as they are. So at every
  allowed size the two columns get at least as much height as they do today.
* **Tiles** are left-aligned at the content column's left edge, in catalog
  `packages[]` order, which is release-date order, oldest first: Simpsons
  (1994), 3.2, Totally Twisted (1995), Deluxe (1996), 10th Anniversary
  (1999); Star Wars (1994) comes second, after the Simpsons, since the sixth
  release, and Star Trek (1992) first, before them, since the seventh. With
  all twelve: Star Trek, Marvel, Simpsons, Star Wars, Snoopy, Looney Tunes,
  ScreamSavers, 3.2, Totally Twisted, Disney, Deluxe, 10th Anniversary.
  With all fourteen, Far Side (1994-06) comes third, after Marvel, and
  Dilbert (1994-10) seventh, after Snoopy. With all fifteen, the Next
  Generation (1994-10) comes eighth, after Dilbert (the same month, so
  registry order). With all sixteen, Johnny Castaway (1992-12) comes
  second, after Star Trek. With all twenty, Opus 'n Bill (1993-09) comes
  third and Intermission (1993-11) fourth, before Marvel, the Flintstones
  (1994-05) sixth, before Far Side, and On the Road Again (1994-09) tenth,
  after Star Wars. A release without a date comes
  last. (As built for twelve: a row that
  scrolls starts after the left chevron's place, §1.3.)
* **Status box.** A status box 200 DIP wide sits at the column's right
  edge, vertically centred on the tile art (§1.6). The tiles area is the
  column minus that box minus 16 DIP.
* **Caption.** The release's `shortTitle` in `fonts.caption`, one line,
  centred in the cell and ellipsized at the end. Compact tiles have no
  caption; the tooltip and accessible name carry the title.
* **Scaling.** Everything is laid out on the 4-DIP grid and scaled by
  dpi/96 like the rest of `layout_window`. The art at each scale: 64×80 is
  64×80 px at 100%, 80×100 at 125%, 96×120 at 150%, 112×140 at 175%,
  128×160 at 200% and 160×200 at 250%. Compact is 48×60 at 100%, 96×120 at
  200% and 120×150 at 250%. The 640×800 tile PNG (§2.6) is always scaled
  down (by 4 or more), never up.
* **Room.** At the minimum content width (852 DIP) the tiles area holds 6
  regular or 8 compact tiles, so the first five releases always fit without
  scrolling, and so do the six of the sixth release (checked on off-screen
  renders; a seventh would scroll). The seventh release's seven fit the
  first-open window (1040 × 800) and, compact, the smallest one (900 × 680)
  at every scale from 100% to 250%; a window as narrow but 760 DIP or more
  tall shows five of its seven regular tiles whole (a chevron takes 24
  DIP), so its row scrolls, by at most two tiles, to the third
  (`scr_unit_releases`, and the seven-release renders). When the
  tiles need more room than the tiles area has (future releases), the strip
  scrolls (§1.3). It never overlaps the status box.

  **Twelve releases** (as built). Regular covers (a client 760 DIP tall or
  more) never all fit: twelve need 1240 DIP, and the tiles area is at most
  1024 (the column stops at 1240). By window width they show 5 at a time at
  900–935 DIP (8 stops), 6 at 936–1039, 7 at 1040–1143 (the first-open
  window: 6 stops), 8 at 1144–1247 and 9 from 1248 (4 stops). Compact covers
  (a client under 760 DIP tall) show 8 at 900–959 DIP (5 stops), 9 at
  960–1031, 10 at 1032–1103, 11 at 1104–1119, and all twelve side by side
  from 1120 DIP wide, left-aligned, with no chevrons. The 10 is also the
  first-open window whose height the work area clamps under 760 DIP
  (1920×1080 at 125% or 150%, 1366×768 and 1536×864 at 100%, 1920×1200 at
  150%, 2560×1440 at 175%). Seven releases scroll only with regular covers
  in a window under 984 DIP wide (5 or 6 at a time; the first-open window
  still shows all seven with no chevron).

### 1.3 Overflow and scrolling

When the tiles need more width than the tiles area has:

* The row scrolls horizontally in **whole cells**, so the first visible
  tile is never cut at the left.
* A 24 DIP chevron button appears at each end that has more tiles beyond
  it: Segoe Fluent Icons `E76B`/`E76C`, `ButtonRole::subtle`, vertically
  centred on the art. It is not a tab stop.
* The tiles fade into the base colour over 24 DIP at an edge with more
  beyond it (`fade_in_right`, and its mirror image for the left edge).
* The mouse wheel over the strip, vertical or horizontal, scrolls one cell
  per 120 units of delta.
* A tile that takes keyboard focus is scrolled fully into view.
* The status box never scrolls.

**As built for twelve releases** (the fades and part-shown tiles above are
gone): no window holds twelve regular covers, so the strip scrolls in the
first-open window, and the design above left a part-shown cover under the
chevron's box with its caption cut ("After D"). Now:

* **Only whole covers show**, the same number at every stop: as many as fit
  between the two chevrons (`StripLayout::slots`; a chevron's zone is 24
  DIP plus a 4-DIP gap, `kStripChevronGap`), every step exactly one cover
  wide. Covers that do not fit are moved just outside the strip window;
  they stay real windows, so the dialog manager and screen readers still
  reach them, and one that takes the focus, or that Space toggles while
  scrolled away, comes into view.
* **Nothing lies under a chevron, and the chevrons stay put.** The left one
  sits at the strip's left edge and the right one just past the last slot,
  4 DIP clear of the covers and their focus rings, in one place at every
  stop; each one's place is empty at the stop where it hides (the left one
  unscrolled, the right one at the last stop), so a click too many lands on
  no cover.
* **A row that scrolls starts after the left chevron's zone** (24 + 4 DIP)
  at every stop, the unscrolled one included, so each slot keeps its place;
  covers that all fit stay left-aligned at the column's edge as before (so
  seven releases in the first-open window look as they did).
* With a saved filter (or one kept through an import reload) the strip
  opens scrolled to the first selected cover.
* The fades are gone: nothing is cut, so nothing fades (`adw_ui`'s
  `fade_in_left` and `fade_in_right` are no longer used by the scr).

**As built for fourteen releases: the covers wrap.** Having to click a
chevron to find the releases past the seventh was the strip's weak point,
so every cover now shows, on as many rows as they need
(`StripInput::wrap`, `strip_grid`, `strip_band` in `ui_model.h`):

* **Rows.** As few as hold every cover. Regular covers fill each row left
  to right, up to what a row holds and at most eight (`kStripMaxCols`): ten
  are 8 and 2 even where a row holds nine, fifteen 8 and 7, sixteen 8 and
  8, twenty 8, 8 and 4. Compact covers,
  which only a short window gets, are as even as can be, so a cap never
  costs it a row: fourteen where a row holds ten are 7 and 7, sixteen 8
  and 8, twenty 10 and 10. The rows are left-aligned on the
  column and `kStripRowGap` (8 DIP) apart, the gap between two covers of a
  row; only the last row may be shorter. The band is the rows of cells and
  the 12-DIP gap under them: 236 DIP for two regular rows, 352 for three,
  156 for two compact ones (one row: 120 and 80, as before).
* **Which form.** The regular covers when the client has the height for
  their rows (the columns keeping what they have over a one-row regular band
  at 760 DIP: client height − band ≥ 640), else the compact covers while
  the columns keep at least their 600-DIP minimum, so the two columns never
  get less height than they have without the strip. Fourteen (or twelve)
  releases at 1104 DIP wide: two regular rows from 876 DIP tall, two compact
  rows from 756; twenty, three regular rows from 992 DIP tall, two compact
  rows (10 and 10) from 756. Only a client too short even for the compact
  rows (the
  minimum 680 DIP, for eight releases and more) falls back to the one
  compact row that scrolls between its chevrons, exactly as above, 8 to 11
  covers at a time; the minimum window size is unchanged.
* **First-open size.** The design height plus the band of the regular rows
  at the first-open width, 1104 DIP, where a row holds eight
  (`design_client_h`): 1104 × 904 DIP for two to eight releases, 1104 × 1020
  for nine to sixteen (two rows of regular covers, the columns at their
  design heights), and 1104 × 1136 for seventeen to twenty-four (three
  regular rows, 352 DIP), clamped to the
  work area as before (a clamped height then gets the compact rows, or the
  scrolling row).
* **The status box** stays at the column's right edge, as tall as a cover,
  centred on the rows of covers (on the 4-DIP grid).
* **Keyboard.** Left and Right still step through every cover (wrapping),
  Home and End go to the first and last; on two rows and more, Up and Down
  go to the cover above or below (the last one when the row below is
  shorter), stopping at the first and last rows.

### 1.4 Tile visuals

A tile is drawn in this order: the hover backdrop, the art with 4-DIP
rounded corners and a hairline border (`ui::draw_cover`, §3.2), the dimming
overlay, the selection ring, the check badge, the caption, and the focus
ring.

| State | Light / dark (colours from the palette) | High contrast |
|---|---|---|
| Normal, no filter | Art at full strength. Caption in `text` | Art as is. Caption in `WindowText` |
| Selected | A 2-DIP ring in `accent`, drawn between 2 and 4 DIP outside the art, radius 8 DIP. A check badge inside the art's top-right corner: an 18-DIP circle (16 compact) inset 2 DIP, filled `accent`, with a 1-DIP white outline at 70% opacity and a `E73E` check in `on_accent` (`fonts.icons_small`). Caption in `text` using `fonts.caption`; the caption is not bold | Ring in `Highlight`. Badge filled `Highlight`, check in `HighlightText`, outline in `Window` |
| Unselected while a filter is active | The art is dimmed by `pal.base` laid over it at alpha 140. Caption in `text3` | No dimming (high contrast allows no tints). The missing ring and badge show the state |
| Hover | A backdrop over the whole cell, radius 6 DIP, in `row_hover`. A dimmed tile's overlay drops to alpha 70 | A 1-DIP ring around the cell in `HotLight` |
| Pressed | The same backdrop in `control_pressed` | Same as hover |
| Keyboard focus (when `keyboard_cues()` is on) | `draw_focus_ring` around the cell, radius 6 DIP | `draw_focus_ring` using the high-contrast palette |
| Cover still generated (no picture yet) | `ui::draw_generated_cover` (§3.2), with the same ring, badge and dimming as any other tile | The same |

Dark mode uses the same rules with the dark palette. The dimming then
blends toward the dark base, so unselected covers recede rather than wash
out.

### 1.5 Structure, keyboard, pointer, screen readers

The strip is built from **real controls**. That way the dialog manager,
MSAA and UIA work with no custom accessibility code.

* **Container.** The strip is a child window `IDC_COVER_STRIP` (1030), with
  `WS_EX_CONTROLPARENT` and window text "Filter by release". It is placed
  before `IDC_MODE_SINGLE` in the z-order, so it is first in the tab order.
* **Tiles.** Each tile is a `BUTTON` with `BS_AUTOCHECKBOX | BS_PUSHLIKE |
  BS_NOTIFY`, id `IDC_COVER_TILE_BASE` (3000) plus its index in
  `packages[]`. It is custom-drawn through `NM_CUSTOMDRAW` sent to the
  container (`CDRF_SKIPDEFAULT`). Its check state is the selection. Like
  every other focusable control, its window is the cell grown by
  `focus_margin()` on each side.
* **Accessible name.** The tile's window text is `<title>, <n> screen
  savers`, for example "The Simpsons Screen Saver, 15 screen savers". Any
  `&` is doubled. The UIA proxy exposes it as a check box with the Toggle
  pattern: "…, check box, checked".
* **Tooltip.** One tooltip control holds a tool per tile: `<title> —
  <n> screen savers`, then "Click to show only this release's screen savers,
  or several releases at once."
* **Tab order.** The first tile has `WS_GROUP`. Only one tile at a time
  has `WS_TABSTOP` (a roving tab stop): when a tile takes focus, the style
  moves to it. Tab therefore lands on the last-focused tile, which is the
  first selected tile when the dialog opens, or else the first tile.
* **Keys.**

  | Key | Action |
  |---|---|
  | Left, Up / Right, Down | Previous or next tile, wrapping (the dialog manager moves within the group, because the tiles do not claim arrow keys) |
  | Home / End | First or last tile (a subclass handles these on `WM_KEYDOWN`) |
  | Space | Toggle the tile (auto check box) |
  | Enter | The dialog's default button (OK), as for any check box |
  | Shift+F10, Apps key | The context menu, placed at the tile's bottom-left |
  | Esc | Cancel, unchanged |

* **"Show all" link.** `IDC_STRIP_SHOW_ALL` (1031) is a text link in
  `ButtonRole::subtle` style. It follows the tiles in tab order and is
  visible only while a filter is active.
* **Status text.** `IDC_STRIP_STATUS` (1032) is a static. When the filter
  changes, the text changes and the dialog raises a polite live-region
  notification: `IAccPropServices::SetHwndProp(LiveSetting_Property_GUID =
  Polite)` once, then `NotifyWinEvent(EVENT_OBJECT_LIVEREGIONCHANGED)` on
  each change. Narrator then reads the new status, for example "Showing 1
  of 5 releases".
* **Context menu** (`WM_CONTEXTMENU` from a tile). A `TrackPopupMenu`
  offers:
  * "Show only <shortTitle>"
  * "Show all releases" (greyed when there is no filter)
  * a separator
  * "Change cover…", which runs §1.11. It is greyed when `adimport.exe` is
    missing or an import or cover change is already running.
  * (As built since fourteen releases) "Remove <shortTitle>…", last, which
    runs §1.11's removal. It is greyed when "Change cover…" is, and while a
    module's own settings window runs.

  In dark mode the menu is dark: call `ui::allow_dark_menus(pal.dark)`
  (§3.2) at startup and on every theme change.

### 1.6 Filter semantics

* **Toggle.** Clicking a tile toggles it. Any number can be selected.
  **Nothing selected means all releases.** Selecting every tile is also
  "all": the strip keeps showing each tile as selected for the rest of the
  session, but the saved value is empty (§1.9).
* **Status box.**
  * With no filter: "Click covers to filter the list" (`strip_status`,
    releases.h) in `text3`.
  * With a filter: "Showing <k> of <n> releases" in `text2`, with the
    "Show all" link under it.
* **Count label.** The modules count above the list (`IDC_MODULES_COUNT`)
  reads "15 of 202" while filtered and "202" otherwise.
* **Rebuilding the list.** A filter change rebuilds the list from the
  catalog, keeping every check (§1.8), the selection and the scroll
  position when the selected row is still shown.
* **Single module mode.** A filter change **never changes the chosen
  module**. If the filter hides the chosen module's release, the list shows
  no selected row. The details card never shows a module the list hides
  (`details_after_filter`, releases.h): after a filter change it shows the
  chosen module while its row is listed (that row stays selected), else the
  module it showed before while that row is listed, else the list's first
  row. When the filter leaves no rows the card reads "No modules to show"
  and Preview is unavailable. Showing a module is not choosing it: the
  chosen module changes only when the user clicks a row, and OK saves
  `Module=<the chosen id>` whatever the filter. **Show all** brings the
  chosen row back, selected.
* **Random mode.** The checklist, the rotation summary, **Select all** and
  **Clear** all work on the rows shown (§1.8). The details card follows the
  same rule as in Single mode.
* **Preview.** Preview runs `/s` from the dialog's current settings,
  including `Collections`, with the module the details card shows.

### 1.7 The module list, by release

* **Groups.** One ListView group per installed release, in `packages[]`
  order, with ids `1 + index`. The header text is the release `title`
  ("After Dark 4.0 Deluxe", "The Simpsons Screen Saver"). There is no "After
  Dark 4", "After Dark Classic" or "Other" group any more. Deluxe's 84
  modules form one group, and Simpsons and Totally Twisted each have their
  own.
* **Rows.** A row shows `moduleName` (catalog; `displayName` when it is
  missing), after the dialog's existing "names shown whole" mapping, which
  is keyed by `moduleName`. Rows are sorted within their group as today
  (case-insensitive).
* **Two rows with one name in a group.** One release can ship two builds
  under the same name. The real catalog has "Bad Dog!" as both an AD4
  (`pe32`) and a Classic (`ne16`) module in both Deluxe and 10th
  Anniversary. When two rows in one group would show the same name
  (case-insensitive), the `ne16` one gets the suffix " (Classic)". If that
  still collides (same lane), the upper-case file stem is added:
  " (BADDOG3)". This is the only place the lane affects anything the user
  reads.
* **Duplicates.** Each release's copy of a module is a separate catalog
  entry, so it is listed under its own release. If Flying Toilets ships on
  Deluxe, 10th Anniversary, 3.2 and Totally Twisted, it appears in each of
  those groups that is shown, always as "Flying Toilets". The group header
  says which release a row belongs to. No row carries the catalog's
  " (<short title>)" suffix.
* **Details card.** It shows `moduleName` as the title. The chip that used
  to say the lane (`IDC_MODULE_BADGE`) is now a neutral chip with the
  release's `shortTitle`. When other entries share the module's bytes
  (`sameAs` either way), the chip's tooltip reads "Also on: <titles>".
* **The lane.** It never shows: no row badge, no group, no chip. The only
  lane-driven state left is "Coming soon" (row dimmed, chip in `caution`)
  for a module whose lane the host lacks, unchanged.
* **Assets line.** The footer line becomes "202 modules from 5 releases",
  or with one release "84 modules from After Dark 4.0 Deluxe". Missing
  files still add " · 2 missing — import again to restore".
  `count_assets`/`assets_summary` and their tests change to match.
  (`AssetCounts` gains `releases` and drops `ad4`/`classic`.)
* **Opening.** The list still opens at the top when the chosen module is
  in the first rows, else with the module mid-list, and never with a row
  or header cut at the top edge. The `list-top` rules apply unchanged.

### 1.8 Random mode

* **Checks are global.** The dialog keeps one checked state per catalog
  id, for every release, including releases the filter hides. The list
  view shows the checks of the rows it has.
* **Per-release check-all.** Each group header's checkbox (already drawn
  for the lane groups) is its release's "check all". It shows a dash while
  the release is mixed.
* **Select all / Clear** act on the rows shown.
* **Rotation summary** counts the rows shown: "All 15 in rotation" or "12
  of 15 in rotation". The existing "· N can run now" rule still applies.
  A list of one module (Marvel Comics Screen Posters, alone or filtered
  to) reads "1 in rotation" (or "1 selected · 0 can run now"), never "All
  1", and a group of one says the same in its screen-reader name ("Marvel
  Comics Screen Posters, 1 in rotation").
* **What the saver plays**, given `Settings` and the catalog (a pure
  function `effective_rotation`):
  1. Start from `Randomize`, or every catalog id when it is empty.
  2. Keep the ids whose `package` is in the effective `Collections` (§1.9).
  3. Walk the result in catalog order and give each id a key: its `sameAs`
     when that is set, else the id itself. Keep only the first id for each
     key. A byte-identical copy checked in two releases therefore plays once
     per pass, as the first copy in catalog order.
  4. If the result is empty, which only a hand-edited file can cause, use
     the result of step 1 alone and log `rotation: Collections ignored
     (nothing checked in them)`.
  5. A leading `Module` (a named `Module` alongside a `Randomize` list)
     still plays first, whatever the filter.
* **Saving.**
  * `Randomize` is written from the global checks, not only the visible
    ones. "Every module checked" (every catalog id) is still saved as an
    empty list.
  * Random is refused when **no shown row** is checked, with the message
    "Check at least one module in the releases shown", which replaces "…
    nothing checked".
  * `RandomizeSaved` keeps its meaning.

### 1.9 Persistence: `settings.ini`

```ini
[Saver]
Collections=simpsons,tt     ; the strip's filter: release (package) ids; empty or missing = all
```

* **Written** by the dialog on OK, as ids in `packages[]` order. It is
  written empty (`Collections=`) when nothing is selected, or when every
  installed release is selected, so releases imported later are included.
  While the strip is hidden, OK leaves the key exactly as it was.
* **Read** by both the dialog and the saver. The *effective* filter drops
  ids that are not installed releases (unknown ids stay in the file until
  the next OK). If the result is empty, or holds every installed release,
  the filter is "all". This one rule also covers the strip being hidden
  (one release installed).
* **Unknown keys and comments** survive, as for every key (`IniFile`).
* `Settings` gains `std::vector<std::string> collections`. It is parsed and
  serialized with the other keys, and the round-trip tests include it.

### 1.10 Catalog fields the front-end reads

* **Per module:** `package`, `packageTitle`, `moduleName`, `sameAs`
  (PACKAGES.md §6, already written by the importer). They are optional.
  When `package` is missing, it is `deluxe` for ids starting `ad40.` or
  `classic.`, else `other`.
* **Top level:** `packages[]` with `id`, `title`, `shortTitle`, `modules`
  and `cover` (§2.7). When `packages` is missing, the dialog makes one
  entry per distinct module `package`, in order of first appearance:
  * title = the first `packageTitle`, else "After Dark 4.0 Deluxe" for
    `deluxe`, else "Other modules";
  * `shortTitle` = the title;
  * no cover (generated).
* **Parsing** stays lenient about all of these and strict about `id` and
  `path`, as today.

### 1.11 "Change cover…" from the dialog

* **Launch.** The context menu starts `adimport.exe --gui --change-cover
  <id>` through the same path as **Import…**: `CREATE_NO_WINDOW`, the
  inherited environment (so `AD_ASSETS_DIR` passes through), and a done
  message.
* **While it runs**, Import… and every "Change cover…" are greyed, as they
  are during an import.
* **Exit code.**
  * 0 means a cover changed: the dialog reloads the catalog, keeping
    checks, filter, selection and unsaved control values, exactly as after
    an import. It then re-reads the tiles; they are cached by `tileMd5`.
  * 5 or any other code changes nothing.
* **The scr never writes under the assets root.**

**As built since fourteen releases: "Remove <shortTitle>…".** A release
can be taken off the computer from its cover, the way its cover is changed:

* **Launch.** The menu's last item starts `adimport.exe --gui --remove
  <id>` the same way. adimport's own window asks first (§4.2, Remove), and
  removes the release with `remove_package` (`--remove <id>`: its folder is
  moved aside under the import lock, the catalog rewritten without it, the
  folder deleted; its cover is kept, `covers\<id>`).
* **Nothing of the release may be open meanwhile**, or its folder can't be
  moved aside (the importer retries the rename for 2 s, then says the
  folder is in use and nothing was removed). So while adimport runs, no
  thumbnail is taken (the queue is emptied, and none is scheduled until it
  exits), the live preview stops if it shows one of the release's modules
  (it shows the night sky), and the item is greyed while a module's own
  settings window runs. Import…, "Change cover…" and "Remove …" are greyed
  as for a cover change.
* **Exit code.** 0: the release is gone, and the dialog reloads the
  catalog as after an import (its modules leave the list, the rotation and
  the details; a chosen module of it gives way to the first row; the filter
  forgets it; one release left hides the strip; the assets line counts what
  is left). Anything else removed nothing (adimport showed why): the
  preview starts again where it stopped.

### 1.12 Screenshot hook and tests (S)

* **`AD_SCR_TEST_SCREENSHOT_STATE`** gains:
  * `collections=<id>,…` (preselect the filter)
  * `focus=strip` (the focus ring on the first selected tile, else the
    first tile)
  * `hover=strip:<id>` (that tile hovered)
* **The report** (`report=`) gains `strip=x,y,w,h` (in picture pixels) and
  `strip_mode=regular|compact|hidden`.
* **Fixtures.**
  * A five-release fixture catalog, `tests/fixtures/catalog-releases.json`:
    the real catalog's shape with placeholder names, `sameAs` duplicates,
    and `packages[].cover` with every origin.
  * Cover tiles are **synthesized at test time**: gradient PNGs with a
    large letter, written with `save_png_bgr`. No After Dark or third-party
    art is ever committed.
* **Unit tests** (`scr_unit_*`, no windows):
  * `Collections` round trip and normalization
  * `effective_rotation`: filter, `sameAs` dedupe, empty fallback, lead
    module
  * `layout_strip`: every scale 100–250% in 25% steps, 1 to 12 releases,
    regular/compact threshold, whole-cell scroll stops, chevron visibility,
    tiles never overlapping the status box, everything on the 4-DIP grid,
    200% equal to 100% doubled
  * `layout_window` with the strip: the columns keep ≥ today's minimum
    heights
  * filter and grouping model: visible ids, group order, counts, status
    strings
  * parsing `packages[]`/`cover` and the old-catalog fallback
  * the new assets line
* **Smoke tests** (label `gui`):
  * `config-collections`: click tiles by control id; the list regroups;
    OK writes `Collections`; Random with a filter writes the global
    `Randomize`; reopening restores the selection; with one release
    installed the strip is hidden.
  * `config-cover`: "Change cover…" against `fakeimport.exe`; exit 0
    reloads, exit 5 does not.
  * `list-top`: extended to the strip at each size, both modes and high
    contrast.
  * A saver `rotate` case with `Collections`.
* **As built for twelve releases:** the report gains `strip_first=`,
  `strip_max_first=`, `strip_slots=`, each cover's window `tile<i>=x,y,w,h`
  (or `hidden` while it lies outside the strip), `chevron_left=`,
  `chevron_right=` and `strip_status=`, all in the picture's pixels; a
  twelve-release fixture (`tests/fixtures/catalog-twelve.json`) with its
  unit suite, whose strip checks pin every count of §1.2 at every scale (the
  same number of covers at every stop, none under a chevron, chevrons that
  do not move, each step one cover, the left chevron's place empty
  unscrolled); and a smoke test, `config-twelve`: off-screen renders at
  several sizes, then the dialog driven by keyboard and chevrons, expecting
  the covers at a time `layout_window` gives its real client size and DPI
  (7 at 1040×800; 10 when the height is clamped), the left chevron's place
  empty after clicking back to the first stop, a saved filter, an import
  going from seven releases to twelve, and ScreamSavers' and Marvel's
  catalog screen.
* **As built for fourteen releases (wrapping):** the report gains
  `strip_rows=` and `strip_cols=`. `scr_unit_releases` pins the wrapped
  layout at every scale for 1 to 14 releases (every cover whole on its row
  and column, the fewest and evenest rows, nothing scrolling, no two
  covers' focus rings touching, a row that fits laid out as before), which
  form each window size gets (regular rows, compact rows or the scrolling
  row), the first-open heights (836, 952) and the status box centred on the
  rows. `config-twelve` renders two rows of regular covers (1040×952) and
  of compact ones (1040×800, 900×800) and the scrolling row (900×680);
  driven, it expects every cover in the first-open window and Down going to
  the cover under the first, then shrinks the window to its smallest and
  drives the scrolling row as before. `config-remove`: "Remove Simpsons…"
  against `fakeimport.exe`, with the live preview on one of the Simpsons'
  modules: it stops while adimport runs; exit 5 changes nothing and the
  preview starts again, exit 0 reloads without the release
  (`tests/fixtures/catalog-releases-no-simpsons.json`). `config-cover`
  reaches "Change cover…" one item further up.

## 2. Covers: the importer pipeline

### 2.1 Sources and their order

The tile shows the first of these that exists:

1. **The user's own picture**, set with `--set-cover` or "Change cover…".
2. **The original**: the best of the package's **registry cover sources**
   captured so far. Sources are tried in the order the registry lists
   them, which differs per package (§2.3). A source is one of:
   * `download`: an HTTPS URL, with an **md5 and size** checked before use,
     and an optional crop;
   * `disc`: a file on the import source (the disc, image, ZIP or folder
     being imported), optionally a bitmap resource inside that file, with
     an optional md5 of that file and an optional crop.
3. **A generated cover**, drawn by the front-ends (§3.2). No file is stored
   for it.

A user picture never replaces the original. `--clear-cover` goes back to
the original.

### 2.2 Registry fields (C: `packages.h`)

```cpp
struct Crop { int x = 0, y = 0, w = 0, h = 0; };   // source pixels; w == 0: none

// One place a package's cover can come from (COVERS.md §2.3), in preference order.
struct CoverSource {
  enum class Kind { download, disc };
  Kind kind;
  const char* art;         // "box" | "disc" | "splash" | "panel" — how the tile is rendered (§2.6)
  const char* label;       // for people: "Box front", "Disc label", "Installer art"
  const char* credit;      // for people: "Internet Archive", "Wikisimpsons", "your disc"
  // download
  const char* url;         // https only; redirects followed; the URL as published (never a mirror node)
  const char* md5;         // of the downloaded file (not of the crop); required for downloads
  uint64_t size;           // published size; required for downloads
  const wchar_t* file_name;  // saved as <download dir>\covers\<file_name>
  // disc
  const char* path;        // in the source, as identification located it: "INSTALL/SETUP.BMP", "ADE/PAGE1.BMP", "SETUP.EXE"
  const char* path_md5;    // md5 of that file: a file with other bytes (another pressing) is skipped; "" = any bytes
  uint16_t resource_type;  // 0: the file itself is the picture; 2 (RT_BITMAP): a bitmap resource of an NE/PE file
  uint16_t resource_id;
  Crop crop;               // applied after decoding (and after EXIF orientation)
};
// Package gains:
std::span<const CoverSource> covers;   // tried in this order (empty: generated only)
```

Rules:
* **Validation.** Every download source must have an `https://` URL, a
  32-hex md5 and a nonzero size. Every crop must lie inside the decoded
  picture; a crop that does not is a failed source, logged. `test_packages`
  checks the registry's shape.
* **Where disc paths resolve.** Disc paths are relative to the root that
  identification uses: the disc root for `tree` packages (`ADE/PAGE1.BMP`
  sits outside `FILES`), the install dir's parent for `ad3zip` CDs
  (`INSTALL/SETUP.BMP`), and the ZIP or floppy root for the Simpsons
  (`SETUP.EXE`). Names match case-insensitively through `SourceFs`.
* **Only what the recipe names.** Only the named file is read. PACKAGES.md
  §4.2 I5 holds: the owner's notes on the Simpsons floppy are never opened.
* **Bitmap resources.** An NE or PE `RT_BITMAP` resource is a DIB with no
  file header. The importer finds it with `adw::loader` (`ne::…find_resource`,
  `pe::…find_resource`) and prepends a `BITMAPFILEHEADER`, with `bfOffBits`
  = 14 + header size + palette size, before decoding with WIC.

### 2.3 The registry's cover sources

The values come from `research/win/pkg/covers/<id>/cover.json` (gitignored),
where the research is recorded. Only URLs, md5s, sizes, paths and crops go
into the code; never bytes. The `deluxe` and `tt` `cover.json` files were
still being written when this was designed and never appeared, so the rows
that waited on them were settled from the real images and the Internet
Archive's metadata at implementation (as built, below; the user's own box
photos are those two releases' covers of choice, through `--set-cover`).

| id | 1st | 2nd | 3rd | 4th |
|---|---|---|---|---|
| `deluxe` | download `https://web.archive.org/web/19970720113325id_/http://www.berksys.com:80/products/afterdark/box.deluxe.gif` (the box front from Berkeley Systems' 1997 product page, 162×195), md5 `abb5ea84262cc6a2badea016f69cc7e8`, 19460 B, art `box` ("Box front", Wayback Machine) | disc `ADE/PAGE1.BMP` (the setup wizard's portrait art: "After Dark" and flying toasters on black, 118×226), md5 `945259b85a1f0a5e0aedfad53561c838` (28198 B), art `panel` ("Setup art", your disc) | download `https://archive.org/download/after-dark-4-deluxe/disc.jpg`, md5 `0b5104351ae6fa60da6d48164e8f548e`, 737758 B, 1488×1452 (checked against the item's metadata on 2026-09-26; it is in the same item as the ISO), art `disc` ("Disc label", Internet Archive) | — |
| `ad10` | download `https://archive.org/download/ad10th/01_ad10_cd.jpg`, md5 `e1e77b74ccc9bcd4e5b818f841f0614d`, 376876 B, art `disc` ("Disc label", Internet Archive) | disc `ADE/PAGE1.BMP` (Joliet `/ADE/page1.bmp`), md5 `30f247d08fe8a4fd394658a3c4564a06`, art `panel` ("Setup art", your disc) | — | — |
| `ad32` | disc `INSTALL/SETUP.BMP`, md5 `67e76581f69bcfdb62ef1c7ff61fc426`, **crop 0,0,387,183** (drops the warning and copyright rows), art `splash` ("Installer art", your disc) | download `https://archive.org/download/after-dark-v3_2/disc.jpg`, md5 `68f2aad89dcdc1fdbcb3419ab42077f9`, 349982 B, art `disc` | download `https://archive.org/download/berkeley-systems-after-dark-for-windows/berkeley-systems-after-dark-for-windows.png`, md5 `f2fdf32138f96da06f8d348af059d712`, 1857714 B, art `disc` | — |
| `tt` | download `https://web.archive.org/web/19970720113451id_/http://www.berksys.com:80/products/afterdark/box.twistedL.jpg` (the box front from Berkeley Systems' 1997 product page, 127×162, the retail box's art), md5 `5aa9478d761a580f1d9006ee41d0cd3f`, 26382 B, art `box` ("Box front", Wayback Machine) | download `https://web.archive.org/web/20230422055429id_/https://www.sierrachest.com/gfx/games/AfterDarkTotallyTwisted/box/01_front.JPG` (the box front of Sierra's later edition from The Sierra Chest, 498×599), md5 `3f87e84e3d86d7a2d6f96c86a8102452`, 53679 B, art `box` ("Box front", Wayback Machine) | disc `INSTALL/SETUP.BMP` (the magenta "Totally Twisted" splash, 387×251), **crop 0,0,387,204** (measured: the logo ends at row 197 and the warning text starts at row 211), md5 `3e5eee81009de2a2a432cfabf5d7cfea` (49314 B), art `splash` ("Installer art", your disc) | download `https://archive.org/download/TTW320CD/TTW320CD.tif` (the item's TIFF scan of the disc, 2840×2888, normalized to 2014×2048), md5 `ec19d74fd2ac46d4bc8f6021ad0011e6`, 24632572 B, art `disc` ("Disc label", Internet Archive) |
| `simpsons` | download `https://static.simpsonswiki.com/images/7/72/The_Simpsons_Screen_Saver.png`, md5 `47f4619da9e79b126a4fb45770f0e649`, 1011608 B, art `box` ("Box front", Wikisimpsons) | download `https://web.archive.org/web/20250715075938id_/https://www.whipassgaming.com/images/deadsections/mac/simpsonsfullbox.jpg`, md5 `94db22db1e89b3ff2fdb97e610056602`, 484234 B, **crop 0,0,600,776**, art `box` ("Box front", Wayback Machine) | disc `SETUP.EXE` RT_BITMAP **7500**, file md5 `980867b7ab5394b09adad725d375ba95`, **crop 0,0,387,172** (measured: the art ends at row 171, the first black row before the warning text is 172), art `splash` ("Installer art", your disks) | — |
| `swse` (added with the sixth release) | download `https://web.archive.org/web/19970310054846id_/http://www.presage.com:80/images/pimages/box-starwars.JPEG` (the box front from Presage's own 1997 product page, 150×200), md5 `8b84792a21f3a21e09cc2809d263d9c1`, 30600 B, art `box` ("Box front", Wayback Machine) | download `https://web.archive.org/web/20221205171117id_/https://static.wikia.nocookie.net/starwars/images/9/9a/SWScreenEntertainment.jpg/revision/latest` (Wookieepedia's photo of the same US box, 713×847: the Wayback Machine's byte-exact capture of the stored original), md5 `dc4e541d1a79a46747caf0cb6f425c0a`, 190239 B, art `box` ("Box front", Wayback Machine) | download `https://archive.org/download/swse1/1.jpg` (a scan of the disc label, 1416×1416), md5 `a40d11a61ee0288048bdfdd28e48e57c`, 2156659 B, art `disc` ("Disc label", Internet Archive) | download `https://archive.org/download/cd_AfterDark_Star_Wars_ScreenSaver_for_Win3.1/AfterDark%20Star%20Wars%20-%20CD.jpg` (an older scan of the label, in the exact ISO's item, 1452×1464), md5 `b8ac25eb20a87f44e47ff8ed097698d7`, 948614 B, **crop 20,14,1424,1424** (the disc on white paper), art `disc` ("Disc label", Internet Archive) |
| `startrek` (added with the seventh release) | download `https://archive.org/download/afterdark-20b_startrek_box/Aaa_itemimage.jpg` (the Windows retail box front from the Internet Archive's box scans, 1180×1525: it fills the tile), md5 `157eb04fcc9bdc0d4a831148ca6cc260`, 577239 B, art `box` ("Box front", Internet Archive) | download `https://archive.org/download/afterdark-20b_startrek_box/After%20Dark%202.0b%20-%20Star%20Trek%20-%20Box%20-%20Front.jpg` (the same front at 600 dpi, 4720×6100, normalized to 1585×2048), md5 `f41d1dbdaadaeeb0f9bc0a6aa76754ce`, 9277274 B, art `box` ("Box front", Internet Archive) | download `https://archive.org/download/afterdark-20b_startrek/afterdark-20b_startrek_disk1.jpg` (a scan of disk 1's label, 1090×1145, in the disk images' own item), md5 `dbda3bc66b809f446a17138073579b53`, 569964 B, art `panel` ("Disk label", Internet Archive) | — |
| `marvel` (added with the five of the twelve-release registry) | download `https://archive.org/download/afterdarkmarvelscreenposters/box.jpg` (a photo of the shrink-wrapped Windows box front, 1200×1505, in the item of the flat ZIP), md5 `1b9294c6bd03c5b14ed366cc652c8e7c`, 903667 B, **crop 28,64,1132,1390** (the box, w/h 0.814: it fills the 4:5 tile; the camera's date stamp inside it stays), art `box` ("Box front", Internet Archive) | download `https://archive.org/download/marval-computer/MarvalComputer.jpg` (the 1993 magazine advertisement showing the same box, 2675×4192, normalized to 1307×2048), md5 `f2644bb771fd8cbba1c88e77937b5572`, 3229147 B, art `panel` ("Advertisement", Internet Archive) | — | — |
| `snoopy` (the same) | disc `AD_MODS.BMP` (the picture beside the readme on disk 1 that its installer shows: Image Smith's logo, black and white, 79×175), md5 `9befcefa9fafbb5e7c36ccfe325c607d`, art `panel` ("Setup art", your disks) | — | — | — |
| `looney` (the same) | download `https://web.archive.org/web/19970720113529id_/http://www.berksys.com:80/products/afterdark/box.looneytunesL.jpg` (the box front from Berkeley Systems' 1997 product page, 127×162, like Totally Twisted's from the same page), md5 `e9fa28ed00032bb04a436132a724e98c`, 29771 B, art `box` ("Box front", Wayback Machine) | disc `SETUP.BMP` (the installer splash, 387×221, the same in both builds), md5 `32af5fcb5add7fc88de7b03b5c531563`, **crop 0,0,387,161** (the art ends at row 160; the warning and copyright text start at 164), art `splash` ("Installer art", your disks) | download `https://archive.org/download/berkeley_systems_looney_tunes/16_looney_tunes_CD.jpg` (a scan of the label of the August CD of the same release, 750×734), md5 `8b168e0679d2661889092fc6280d509a`, 369574 B, art `disc` ("Disc label", Internet Archive) | — |
| `screams` (the same) | disc `SETUP.EXE` RT_BITMAP **7500** (the installer's title art, "Stephen Blickenstaff's ScreamSavers", 350×179 at 4 bpp), file md5 `e348fb48b89102903a3b26a3c8aedab3`, **crop 0,0,350,119** (the logo ends at row 114, black to 122, the copyright block from 123), art `splash` ("Installer art", your disks) | — | — | — |
| `disney` (the same) | download `https://web.archive.org/web/19970720111656id_/http://www.berksys.com:80/lite/products/afterdark/box.disneyL.jpg` (the box front from Berkeley Systems' 1997 product page, the only capture, 128×162), md5 `ffcd41dd737b125af0a4e4a1bfee6610`, 20983 B, art `box` ("Box front", Wayback Machine) | disc `SETUP.BMP` (the installer splash, 387×220), md5 `39e1bfb21fdf7fe2396525d13798cf50`, **crop 0,0,387,172** (the warning text band starts at row 172), art `splash` ("Installer art", your disks) | — | — |
| `farside` (added with the two Delrina releases) | download `https://archive.org/download/the-far-side-screen-saver-collection-1-of-5/far-side-software-v0-z46qj0d0a2fc1.webp` (a photo of two boxes on a couch, 1080×813 WebP, in the item of the release's damaged floppy images), md5 `4e7ebc4e9dbced47a3fc37e55054486f`, 270222 B, **crop 546,122,408,508** (the release's box, on the right), art `box` ("Box front", Internet Archive) | — | — | — |
| `dilbert` (the same) | download `https://archive.org/download/dilbert_screensaver_collection/box.jpg` (a photo of the box front, 1200×1600, in the item of the flat ZIP), md5 `aa8fe2600200685504ea9f15feaf4352`, 167593 B, **crop 100,215,965,1315** (the box), art `box` ("Box front", Internet Archive) | disc `INSTALL.BMP` (the picture beside the installer's pages on disk 1: Dogbert, 63×123), md5 `47199572d43fb457b952dcba197a19c5`, art `panel` ("Setup art", your disks) | — | — |
| `tng` (added as the fifteenth release) | download `https://web.archive.org/web/19970720113647id_/http://www.berksys.com:80/products/afterdark/box.stngL.jpg` (the box front from Berkeley Systems' 1997 product page, 126×160, like the Looney Tunes' and the Disney Collection's; the same bytes in the `/lite/` captures), md5 `d7286d0d288c5f8c9982c2a97470a2f8`, 23278 B, art `box` ("Box front", Wayback Machine) | disc `SETUP.BMP` (the installer splash, 387×228), md5 `b54082b26b50a8bf44653fa079f36320`, **crop 0,0,387,168** (above the warning text), art `splash` ("Installer art", your disc) | download `https://archive.org/download/star-trek-the-next-generation-screensaver/Star%20Trek%3A%20The%20Next%20Generation%20-%20screensaver_disc.jpg` (the Internet Archive's photo of the disc, 6000×4000, in the ISO's own item), md5 `419a2ba9a3c762531365cef84d02ef45`, 11258025 B, **crop 1200,270,3600,3600** (the disc, normalized to 2048×2048), art `disc` ("Disc label", Internet Archive) | — |
| `castaway` (added as the sixteenth release) | download `https://archive.org/download/johncast/johnny-castaway-pc-cover.png` (the box front, 204×253, in the Internet Archive's item of the release's art), md5 `d07b6af1d5ffe9e0edaafa11fb9badac`, 24904 B, art `box` ("Box front", Internet Archive) | — | — | — |
| `opus` (added with Delrina's four, as the seventeenth release) | disc `INSTALL.BMP` (the picture beside the installer's pages on disk 1: Opus, 63×123; the same in both builds), md5 `9b3c235183f800bc9c3bc14b718a69e0`, art `panel` ("Setup art", your disks) | — | — | — |
| `opusroad` (the same) | download `https://archive.org/download/OpusNBill_OnTheRoadAgain/OnB-OtRA-Box.jpg` (a scan of the box front, 1256×1593, in the item of the release's ZIP; it fills the tile, no crop), md5 `afc3289af08484ad64542363c5b6e9fb`, 465624 B, art `box` ("Box front", Internet Archive) | — | — | — |
| `flintstones` (the same) | — (no source: the generated cover) | — | — | — |
| `intermission` (the same) | disc `INSTALL.BMP` (the picture beside the installer's pages on disk 1: Intermission's logo, 100×150), md5 `cf21a2a530d09444445c4735d8dd59ca`, art `panel` ("Setup art", your disks) | — | — | — |

Notes:
* **ad32** starts with its disc art because the user chose it: no box scan
  of 3.0 or 3.2 exists. **deluxe** and **tt** start with the box fronts on
  Berkeley Systems' 1997 product pages, through the Wayback Machine (small,
  but a tile is never shown larger than 160×200), **tt** then with the box
  of Sierra's later edition from The Sierra Chest. The art on their discs
  comes next, and a scan of the disc label last (for **tt**, the Internet
  Archive's only larger picture of it: a 24 MB TIFF).
* **ad10.** The user's own box picture (the 10th Anniversary box) is the
  cover of choice, but nothing can download it. It becomes the tile only
  through `--set-cover`. The same goes for the user's Deluxe and Totally
  Twisted box photos (§2.13).
* **simpsons.** The Simpsons art belongs to Fox. It is fetched onto the
  user's machine at import time and never bundled, committed or
  redistributed. The Wikisimpsons and Wayback hosts are third parties; the
  user approved both.
* **swse.** Star Wars Screen Entertainment's art belongs to Lucasfilm and
  LucasArts, and is handled like the Simpsons': fetched at import time,
  never bundled, committed or redistributed. The disc has no picture a disc
  source can reach (its loose files hold only 32×32 icons; everything else
  is inside its ARJ archives), so it has no disc source, and an offline
  import shows the generated cover until `--refresh-covers` fetches one.
  Fandom's live URL for the second picture is not used: it rewrites what it
  serves (WebP, or a JPEG with a header stripped), so the Wayback Machine's
  byte-exact capture stands in for it. Presage's page and Wookieepedia are
  third parties reached through the Wayback Machine, which the registry
  already uses.
* **startrek.** Star Trek: The Screen Saver's art belongs to Paramount and
  Berkeley Systems, and is handled like the Simpsons' and Star Wars': fetched
  at import time, never bundled, committed or redistributed. The box scans
  are by the uploader of the disk images, in a separate item; the disk label
  is in the images' own item, so it survives if the box item goes. The disks
  have no picture a disc source can reach (`SETUP.EXE`, the one plain
  executable, holds only icons; `SPLASH1.BM_`, `BMPRSRC.DL_` and `AD.EX_`
  are KWAJ-compressed), so it has no disc source, and an offline import
  shows the generated cover until `--refresh-covers` fetches one. The label
  is a floppy's, so it takes the art `panel`, drawn as a picture (contained
  on bands, §2.6), not `disc`, which would cut it to a circle: no new art
  value was needed.
* **The five of the twelve-release registry.** Their art belongs to their
  owners and Berkeley Systems (Marvel's, Warner Bros.' for the Looney Tunes,
  Disney's, United Feature Syndicate's Peanuts and Image Smith's, Binary
  Software's and IMPart's for ScreamSavers) and is handled like the
  Simpsons': fetched onto the user's machine at import time, or read from
  the user's own disks, and never bundled, committed or redistributed. No
  new cover-source kind was needed: ScreamSavers' title art is a bitmap
  resource of `SETUP.EXE`, as the Simpsons' is. **marvel**'s disks have no
  picture a disc source can reach (every one is inside its InstallShield
  libraries), so an offline import shows the generated cover until
  `--refresh-covers` fetches one; Berkeley's archived site has no Marvel
  box, and the ad comes second, drawn as a picture. **snoopy**: no box,
  label or manual scan of the Windows release was found online; the Image
  Smith logo on disk 1 is a plain file, and a source reading the Flying
  Ace's About picture out of the installed module (which is compressed on
  the disks) was not built. **looney** and **disney** start with Berkeley's
  small box fronts, like Totally Twisted's, and fall back to their installer
  splashes; the Looney Tunes' third source is the label of its August CD
  (build B, PACKAGES.md §12), the same release's art. **screams**: no box or
  label scan exists online (the Internet Archive, the Wayback Machine's
  captures of Berkeley's and Binary Software's sites, IMPart's later site,
  MobyGames), so its only source is the title art on the user's own disks,
  contained on black bands (aspect 2.94).
* **The two Delrina releases.** The Far Side's art belongs to Gary Larson
  (FarWorks) and Delrina, Dilbert's to Scott Adams (United Feature
  Syndicate) and Delrina; both are handled like the Simpsons': fetched onto
  the user's machine at import time, or read from the user's own disks, and
  never bundled, committed or redistributed. No new cover-source kind was
  needed. **farside**: the only picture of the box online is a photo of it
  beside another box, in the Internet Archive item of the floppy images,
  cropped to it; it is a WebP, which WIC decodes only where Windows' WebP
  codec is installed (§2.6), so without it the generated cover is shown.
  The disks' one picture, the installer's `INSTALL.BMP`, is
  SZDD-compressed, out of a disc source's reach, so there is no disc
  source. **dilbert**: the photo in the flat ZIP's item shows the box
  front; a previous owner's handwritten name on the box stays in the crop.
  Its installer's picture, `INSTALL.BMP`, is a plain file on disk 1, drawn
  as a picture when the photo cannot be fetched. A search of the Wayback
  Machine's captures of Delrina's site found nothing for either.
* **tng.** Star Trek: The Next Generation's art belongs to Paramount, with
  Berkeley Systems', and is handled like the Looney Tunes' and the Disney
  Collection's: the box front from Berkeley's 1997 product page through the
  Wayback Machine, then the installer splash on the user's own disc, cropped
  above its warning text, then the Internet Archive's photo of the disc,
  cropped to the disc; fetched onto the user's machine at import time, or
  read from the user's own disc, and never bundled, committed or
  redistributed.
* **castaway.** Screen Antics: Johnny Castaway's art belongs to Sierra
  On-Line and Dynamix, and is handled like the others': the box front from
  the Internet Archive, fetched onto the user's machine at import time and
  never bundled, committed or redistributed. The floppy has no cover to
  offer (its `LOGO.BMP` and `SLOGO.BMP` are the installer's logos), and the
  one photo of the floppy online, in the item of its KryoFlux dump, is not
  used: its label shows the disk's serial number. An import made offline
  shows the generated cover until `--refresh-covers` fetches the box.
* **Delrina's four more releases.** The Opus 'n Bill releases' art belongs
  to Berkeley Breathed and Delrina, the Flintstones' to Hanna-Barbera and
  Delrina, Intermission's to Delrina; all of it is handled like the
  others': fetched onto the user's machine at import time, or read from
  the user's own disks, and never bundled, committed or redistributed. No
  new cover-source kind was needed. No box, label or manual scan of the
  Opus 'n Bill Screen Saver, the Flintstones or Intermission 4.0 was found
  online (the Internet Archive, the Wayback Machine's Delrina pages; the
  Intermission item holds a screenshot). **opus** and **intermission**
  show the picture their installer shows beside its pages, a plain
  `INSTALL.BMP` on disk 1 (Opus; Intermission's logo), drawn as a picture.
  **opusroad**: the Internet Archive's scan of the box front ("Another
  screen saver by Berkeley Breathed"), in the item of its ZIP; its disks'
  only picture is SZDD-compressed, out of a disc source's reach, so an
  offline import shows the generated cover until `--refresh-covers`
  fetches the box. **flintstones**: its installer's picture is
  SZDD-compressed in both builds, so its registry row lists no source and
  it always shows the generated cover, unless the user sets a picture of
  their own.

### 2.4 Capture during an import

* **New phase.** `Progress::Phase::cover` is inserted **between `verify`
  and `finalize`**, and `phase_name` returns "cover". `Progress::item` names
  the source, for example "Box front from Wikisimpsons". `total` is the
  download size, or 0 for local work.
* **When.** After the stage is verified and before the catalog is rendered
  (PACKAGES.md §5.1 step 4). The capture writes into
  `<win>\covers\<id>.importing-<pid>\`. Cancel is honoured, as for the rest
  of the import, until the first rename.
* **What.** Let *k* be the registry index of the stored original's source:
  `cover.json` `original.source`, taken only when `original.png` exists and
  matches its recorded md5. Otherwise *k* = ∞.
  * The capture tries the sources with index **< k**, in order, and keeps
    the first that succeeds. If none succeeds, the stored original stays.
    If there is none, the cover is generated. A re-import therefore never
    fetches again what it already has, and a better source replaces a
    fallback.
  * **Download sources** use the existing downloader (`download.h`) into
    `<download dir>\covers\<file_name>`. A file already there with the right
    md5 is reused without a request. Covers get their own limits:
    `max_attempts` 2, plus a new `DownloadOptions::timeout_ms` (C adds it;
    15 s to connect and between bytes).
  * **Disc sources** read from the open `SourceFs`.
  * A **network-class failure** (DNS, connect, TLS, or a timeout) skips the
    remaining *download* sources for this run. Disc sources are still tried.
  * `--no-cover-download` (`ImportOptions::cover_download = false`) skips
    download sources entirely.
  * `--refresh-covers` has no import source. It follows the same rule over
    download sources only. With `--force`, *k* = ∞, but a stored original
    is still kept when nothing succeeds.
* **Commit.** With the package swap (PACKAGES.md §5.1 step 5; for Deluxe, its
  own swap), each staged file is moved
  into `covers\<id>\` with `MoveFileExW(MOVEFILE_REPLACE_EXISTING)`, and
  `cover.json` last. `user.png` is never touched by an import. The catalog
  rendered in step 4 already describes the staged cover, with paths under
  `covers/<id>/`.
* **Failure never fails the import.** If every source fails, the cover is
  generated, a log line says why, and `cover.json` records the attempt so
  that `--refresh-covers` knows what to try. The import's exit code and
  `import.json` are exactly what they would be without covers.
* **`--download all`** captures each package's cover within that package's
  own import. An import never touches another package's cover.

### 2.5 On disk

```
<win>\covers\<id>\original.png   the original: decoded, oriented, cropped, long side capped at 2048 px, RGBA PNG
<win>\covers\<id>\user.png       the user's picture, normalized the same way (only after --set-cover)
<win>\covers\<id>\tile.png       the tile shown now: 640×800 opaque PNG rendered from user.png, else original.png
<win>\covers\<id>\cover.json     provenance (below)
<download dir>\covers\<file>     downloaded source files, as fetched (md5-checked), reused later
```

* **Location.** Covers are never inside a package root. Deluxe's `FILES`
  stays byte-identical to its manifest, a package swap never loses a user
  picture, and the per-package atomicity rules of PACKAGES.md §5.1 are
  unchanged.
* **`--remove <id>` keeps `covers\<id>\`.** The catalog lists covers only for
  installed packages, so a leftover cover is invisible and reused on
  re-import.
* **Recovery** (at the start of every operation under `import.lock`):
  * delete `covers\*.importing-*`;
  * delete stray `covers\<id>\*.tmp-*` (every file is written as `<name>.tmp-<pid>`
    and then renamed);
  * rewrite the catalog when anything was recovered.

`cover.json`, version 1:

```json
{ "version": 1, "package": "simpsons", "tool": "adimport 1.3",
  "original": { "origin": "download", "source": 0, "art": "box",
                "label": "Box front", "credit": "Wikisimpsons",
                "url": "https://static.simpsonswiki.com/…/The_Simpsons_Screen_Saver.png",
                "fileMd5": "47f4619da9e79b126a4fb45770f0e649", "fileSize": 1011608, "crop": null,
                "file": "original.png", "md5": "<md5 of original.png>", "width": 600, "height": 776,
                "capturedUtc": "2026-09-26T20:00:00Z" },
  "user": { "file": "user.png", "md5": "…", "width": 962, "height": 1230,
            "name": "cover_supplied.png", "setUtc": "…" },
  "tile": { "file": "tile.png", "md5": "…", "from": "user", "renderer": 1 },
  "attempts": { "lastUtc": "…", "failed": [ { "source": 0, "status": "network", "message": "…" } ] } }
```

* `original` and `user` are each absent when there is none.
* For a disc source, `url`, `fileMd5` and `fileSize` are replaced by
  `"path": "INSTALL/SETUP.BMP"`, `"resource": [2, 7500]` (only for a
  resource) and `"pathMd5"`.
* `user.name` is only the picture's file name, never its folder.
* `tile.renderer` is the version of the tile rules in §2.6. When C changes
  those rules it raises the version. Any catalog rewrite then re-renders
  stale tiles from the stored PNGs, locally and under the lock.

### 2.6 Image processing (C, non-GUI: WIC)

* **Decoding.** Decode with WIC, so any format Windows can read works: BMP,
  PNG, JPEG, GIF (first frame), TIFF, ICO, JPEG XR, plus WebP, HEIF or AVIF
  when their codecs are installed.
  * Apply the JPEG/TIFF EXIF orientation (`/app1/ifd/{ushort=274}`).
  * Convert to 32bpp BGRA.
  * Limits: at most 64 MB of file, 16384 px per side, and at least 32 px per
    side. Anything outside them is `source_invalid`, with a message such as
    "Windows can't read this picture. Save it as PNG or JPEG and try
    again."
* **Normalizing** `original.png`/`user.png`: apply the crop, then scale down
  (never up) so the long side is at most 2048 px, then encode as PNG with
  alpha kept.
* **Tile.** A 640×800 opaque PNG. Scaling uses WIC
  `HighQualityCubic`. When the source has at most 256 colours and the scale
  is ≥ 2, it is first scaled up by the largest whole factor with
  nearest-neighbour, then by the remaining factor with cubic, so small
  installer bitmaps stay crisp.
  * **`disc` art.** Take the centred square of the picture. Draw it as a
    circle of diameter 552 px (86% of 640), centred at (320, 368), with an
    anti-aliased edge, over a vertical gradient from `#262B4F` to `#12152A`
    (the night of the scr's module tiles).
  * **Every other art** (`box`, `splash`, `panel`, and user pictures):
    * If the aspect ratio is within ±15% of 4:5, **fill**: scale to cover
      the tile and crop centred.
    * Otherwise **contain**: scale to fit and centre. Fill the bands with
      the mean colour of the picture's two outermost rows (top and bottom
      bands) or two outermost columns (left and right bands), so a splash
      reads as one poster.
    * Alpha is flattened onto the band colour.

  The rules are pure functions over pixel buffers, so the unit tests can
  pin them without WIC.

### 2.7 Catalog: `packages[].cover`

Every entry of the top-level `packages` array gains `cover`, after
`modules`. Paths are relative to `<win>`, `/`-separated, like module paths.

```json
{ "id": "simpsons", "title": "The Simpsons Screen Saver", "shortTitle": "Simpsons",
  "root": "packages/simpsons", "verified": "image", "importedUtc": "…", "modules": 15,
  "cover": { "origin": "download", "tile": "covers/simpsons/tile.png",
             "tileMd5": "…", "image": "covers/simpsons/original.png",
             "width": 600, "height": 776, "art": "box",
             "label": "Box front", "credit": "Wikisimpsons", "original": "download" } }
```

* `origin` is `user`, `download`, `disc` or `generated`.
* `generated` has no other fields: `{ "origin": "generated" }`.
* `original` is the origin of the original under a user picture
  (`generated` when there is none).
* `width` and `height` are those of `image`.
* **When a file is missing.** A cover whose `tile` file is missing or whose
  `cover.json` is damaged is written as `generated`, and the problem is
  logged. `--refresh-covers` repairs it.
* **What stays the same.** The catalog `version` stays 1 and the generator
  string becomes `adimport 1.2` (`adimport 1.3` since the sixth release,
  PACKAGES.md §6). Nothing about modules changes, so
  `import.catalog_real` and the Deluxe fields stay as they are.
* **Every catalog write includes covers**: imports, `--catalog-only`,
  `--remove` and the cover commands.

### 2.8 CLI (C: `adimport.cc`)

```
adimport --set-cover <id> <picture> [--dest <root>] [--quiet]
adimport --clear-cover <id> [--dest <root>] [--quiet]
adimport --refresh-covers [<id> | all] [--force] [--dest <root>] [--download-dir <dir>] [--quiet]
adimport --gui --change-cover <id> [--dest <root>] [--no-cover-download]
import options (--image/--from/--download): add --no-cover-download
adimport --list-packages      each installed line gains "; cover: <origin> (<label>)"
```

* **Mutual exclusion.** `--set-cover`, `--clear-cover` and
  `--refresh-covers` are modes, like `--catalog-only`, `--list-packages` and
  `--remove`. At most one mode is allowed. A mode takes only the options
  listed for it: otherwise exit 1 with a usage message.
* **`--change-cover <id>`** implies `--gui` and takes no source. It may be
  combined only with `--dest` and `--no-cover-download`.
* **`--refresh-covers`** with no argument means `all`, that is, every
  installed package.
* **Package ids.** An unknown id is exit 1 with "(see --list-packages)". A
  known but not installed id is exit 1: "<title> isn't imported".
* **Exit codes** (`Status`):

  | Command | Exit codes |
  |---|---|
  | `--set-cover` | 0 set · 1 usage, lock held or local I/O · 2 the picture can't be read |
  | `--clear-cover` | 0, including when there was no user picture ("nothing to clear", no catalog write) · 1 |
  | `--refresh-covers` | 0 when every package now has the best cover this run could reach · 4 when some download failed (the previous cover is kept; the output says which) · 1 on an error |

  The "best cover reachable" means only the sources better than its
  current original are tried, or all of them with `--force`, and missing
  or damaged files are repaired.
* **Help.** `--help` documents all of the above.
* **Library calls.** Every cover command goes through the §2.9 library
  calls. The CLI adds only argument parsing and console output.

### 2.9 Library API (C: `covers.h`, frozen for T)

```cpp
// importer/covers.h — owned by C. These declarations are frozen by COVERS.md §2.9
// (T's GUI calls them); C may add to them but not change them.
#pragma once
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "importer.h"   // Progress
#include "packages.h"
#include "status.h"

namespace adw::import {

enum class CoverOrigin { generated, disc, download, user };
const char* cover_origin_name(CoverOrigin o);   // "generated" | "disc" | "download" | "user"

struct CoverInfo {
  std::string package;                       // registry id
  bool installed = false;
  CoverOrigin origin = CoverOrigin::generated;   // what the tile shows now
  std::filesystem::path tile;                // absolute; empty when generated
  std::filesystem::path image;               // absolute: user.png or original.png; empty when generated
  std::string tile_md5;
  int width = 0, height = 0;                 // of `image`
  std::string label, credit;                 // UTF-8, for people ("Box front", "Wikisimpsons"; "Your own picture", "")
  bool has_user = false;                     // a --set-cover picture is in use
  CoverOrigin original = CoverOrigin::generated;  // the original under it
  bool can_download = false;                 // a registry download better than `original` exists
};
// Reads covers\<id>\cover.json and checks its files. No lock; never throws.
CoverInfo cover_info(const std::string& id, const std::filesystem::path& assets_root,
                     std::span<const Package> registry = {});

struct CoverOptions {
  std::filesystem::path assets_root;         // empty = default_assets_root()
  std::filesystem::path download_dir;        // empty = default_download_dir()
  bool allow_download = true;                // false = --no-cover-download
  bool force = false;                        // --refresh-covers --force
  std::function<bool(const Progress&)> progress;   // Phase::cover; return false to cancel
  std::function<void(const std::string&)> log;
  std::span<const Package> registry;         // empty = builtin_packages()
};
struct CoverResult {
  Status status = Status::error;
  std::string message;                       // one line for people
  CoverInfo info;                            // after the operation
  bool changed = false;                      // the tile or its origin changed
};
// All three take import.lock, write atomically, rewrite catalog-win.json when anything
// changed, and never throw.
CoverResult set_cover(const std::string& id, const std::filesystem::path& picture, const CoverOptions& o);
CoverResult clear_cover(const std::string& id, const CoverOptions& o);
// ids empty = every installed package; one result per package, in registry order.
std::vector<CoverResult> refresh_covers(const std::vector<std::string>& ids, const CoverOptions& o);

}  // namespace adw::import
```

`importer.h` also gains the following, frozen for T:
* `Progress::Phase::cover`, placed between `verify` and `finalize`.
* `ImportOptions::cover_download` (bool, default true).

### 2.10 Offline and failures

* **Offline import.** The downloads fail fast (`timeout_ms`, 2 attempts,
  and the rest are skipped after one network-class failure). The disc art,
  or else a generated cover, is used, and the import succeeds.
* **Coming back online.** `--refresh-covers` and "Download the original
  cover" in the GUI try again. Neither the scr nor the importer ever
  fetches covers in the background or on its own.
* **A wrong md5 or size** is refused before the file is used, and the file
  is deleted (the downloader's rule). The next source is tried. A download
  is never accepted by name alone.
* **A damaged or missing stored file** is one whose md5 no longer matches
  `cover.json`. Every catalog write checks.
  * A bad `tile.png` whose source PNG is fine is simply rendered again.
  * A bad `user.png` counts as absent, so the tile falls back to the
    original.
  * A bad `original.png` counts as absent, so the tile falls back to
    generated (or stays on the user picture).
  * `--refresh-covers` then refetches a download original. A disc original
    comes back only with a re-import from the disc; until then refresh tries
    the registry's downloads.
* **Upgrading older installs.** Installs made before this pass have no
  `covers\`, so they show generated covers until either `adimport
  --refresh-covers` (downloads) or a re-import (disc art as well). The
  importer README says so, and the integrator tells the user of the
  command.

### 2.11 GUI: "Change cover…" (T; the pages are specified in §4.2)

`adimport --gui --change-cover <id>` opens only the cover window. The
chooser's installed-release cards open it too.

* **Preview.** The current tile at 192×240 DIP (`ui::draw_cover`). Under it:
  * the origin line: "Box front · Wikisimpsons", "Installer art from your
    disc", "Your own picture" or "No picture yet";
  * "Pictures stay on this computer, in <covers dir>", in `text3`.
* **Buttons** (a column on the right):
  * **Choose a picture…** (accent). `IFileOpenDialog` with the filter
    `*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.tif;*.tiff;*.webp;*.avif;*.heic;*.jxr`,
    then `set_cover`.
  * **Use the original cover**, enabled when `has_user`: `clear_cover`.
  * **Download the original cover**, enabled when `can_download` (which is
    also true when the original is `generated` and the registry lists any
    download): `refresh_covers({id})` with `force` false.
  * **Done** (default; IDOK, Esc).
* **While an action runs**, it runs on a worker thread; the buttons are
  greyed and a themed progress bar shows `Phase::cover`. The result
  message, including an error, appears in the window in `caution`/`critical`
  text, never in a message box.
* **Exit code.** The process exits 0 if any action succeeded with
  `changed`, else 5. Errors are shown in the window; the exit is 5 because
  nothing changed.

### 2.12 IP and repository rules

* No cover bytes, crops or renders are ever committed: not supplied, not
  downloaded, not extracted.
* The code holds only URLs, md5s, sizes, disc paths and crops.
* Tests synthesize their pictures.
* Third-party art (the Simpsons artwork is Fox's) is fetched onto the
  user's machine at import time only.

### 2.13 The user's own pictures

The user's supplied pictures are not bundled. Whether they ever will be is
undecided. They become covers on this machine only through the override,
for example:

```
adimport --set-cover deluxe   "<repo>\research\win\pkg\covers\deluxe\cover_supplied.png"
adimport --set-cover tt       "<repo>\research\win\pkg\covers\tt\cover_supplied.png"
adimport --set-cover ad10     "<repo>\research\win\pkg\covers\ad10\cover_supplied.png"
```

The 10th Anniversary picture is the jewel-case front insert
(`supplied\10thanniv.jpg`, 640×480 with gray side bars), cropped to the
art (482×480). It replaced the earlier 417×500 retail box shot
(`supplied\AD10th.avif`). The user runs these themselves: agents never write under the real data folder
(`%LOCALAPPDATA%\LongAfterDark`).

### 2.14 Tests (C)

**`import.covers`** (new, offline). It uses a synthetic registry
(`ImportOptions::registry` / `CoverOptions::registry`) and the loopback
server from `tests/http_server.h`.

* **Pure tile rules**
  * the fill/contain threshold on both sides of ±15%
  * band colours for top/bottom and left/right bands
  * the disc circle's size, centre and anti-aliased edge
  * the nearest-then-cubic path for ≤ 256-colour sources at scale ≥ 2
  * crops, including one that falls outside the picture (a failed source)
* **Decoding**
  * BMP, PNG, JPEG and GIF files written by the test through WIC
  * a JPEG with EXIF orientation 6, which comes out rotated
  * an NE file with an `RT_BITMAP` resource (`tests/module_builder.h`,
    extended as needed)
  * the size limits: 16×16 is refused, and random bytes are
    `source_invalid`
* **Downloads**
  * the md5 and size check
  * a redirect
  * 404 → next source
  * a wrong md5 → next source, with the file deleted
  * a stalled server → timeout, the remaining downloads skipped, and the
    disc source used
  * reuse from `<download dir>\covers` without a request
* **During an import**
  * `covers\<id>` written and `packages[].cover` in the catalog
  * cancel in `Phase::cover` leaves no `covers\*.importing-*`
  * every cover source failing still gives exit 0 and the same
    `import.json`
  * a re-import fetches nothing it already has, and keeps `user.png`
  * `--remove` keeps `covers\<id>`
  * recovery deletes `*.importing-*` and `*.tmp-*`
  * Deluxe's `FILES` stays byte-identical to its manifest
* **The cover commands**
  * `set_cover`, `clear_cover` and `refresh_covers`, and through
    `adimport.exe` their exit codes
  * not installed, an unreadable picture, the lock held
  * the catalog rewritten, and `changed`
  * refresh upgrading disc → download once the server answers
  * `--force`
  * a damaged `original.png` repaired
  * a raised renderer version re-rendered by `--catalog-only`

**Other test files:**
* `import.cli`: the new modes are mutually exclusive, a mode's extra
  options are refused, and `--no-cover-download` is allowed only with
  imports. It never launches the GUI outside the `AD_GUI_TESTS` block.
* `import.packages`: the registry shape. Every package has at least one
  cover source. Download sources are `https://` with a 32-hex md5, a
  nonzero size and a unique `file_name`. Disc sources name a path, and
  crops are non-negative.

**`import.covers_real`** (opt-in, `AD_E2E=1`):
* It fetches every registry cover download (archive.org, Wikisimpsons,
  Wayback) into a scratch downloads folder and checks the md5 and size of
  each.
* It decodes each one, applies its crop and renders its tile.
* With `AD_E2E_PKG=1`, it also imports the real package images from
  `source_iso` (identified by md5) into a scratch root with
  `--no-cover-download`, so that every disc source is extracted. It checks
  the sizes after cropping (`ad32` 387×183, `simpsons` 387×172, `tt`
  387×204, `ad10` 118×226; as built also `deluxe` 118×226, and with the
  twelve releases `looney` 387×161, `screams` 350×119, `disney` 387×172 and
  `snoopy` 79×175, from the known ZIPs of their install files, and with the
  two Delrina releases `dilbert` 63×123). The Far Side's photo decodes,
  cropped, to 408×508 and Dilbert's to 965×1315.
* It writes `covers-sheet.png` (every tile side by side) into the build
  directory for a person to look at.
* It never writes under `%LOCALAPPDATA%`. The scratch tree is deleted
  unless `AD_E2E_KEEP=1`.

The registry's download URLs must be `https://`. Only the tests'
synthetic registries may use `http://127.0.0.1`.

## 3. The shared UI library: `adw_ui` (T)

### 3.1 Target and layout

```
common/ui/CMakeLists.txt        add_library(adw_ui STATIC …); tests
common/ui/include/adw/ui/theme.h     Palette, Accent, ThemeMode, Fonts, Theme, chrome, drawing, the night sky
common/ui/include/adw/ui/widgets.h   custom-drawn stock controls, badges, overlay scroll bars, progress
common/ui/include/adw/ui/capture.h   off-screen rendering to PNG (the screenshot hooks)
common/ui/include/adw/ui/image.h     decoded pictures, covers, the generated cover
common/ui/src/*.cc
common/ui/tests/test_ui.cc
```

* **Registration.** The top-level `CMakeLists.txt` adds `common/ui`
  to its component loop, after `host/loader` and before `importer` and
  `scr`.
* **Pulling it in.** A component built without it (a restricted
  `AD_COMPONENTS`) pulls it in with `if(NOT TARGET adw_ui)
  add_subdirectory(<path>/common/ui ${CMAKE_BINARY_DIR}/common/ui) endif()`,
  as the importer already does for `adw_loader`.
* **Target properties.** Include dir `common/ui/include` (PUBLIC).
  `_WIN32_WINNT=0x0A00 WINVER=0x0A00` (PUBLIC). Links `gdiplus comctl32
  uxtheme dwmapi windowscodecs ole32 oleacc user32 gdi32` (PUBLIC).
* **Tests.** `ui.theme`, `ui.image` and `ui.capture`. The capture test
  opens only a parked, cloaked, never-activated window.

### 3.2 API (frozen now; S codes against it)

The API is **the scr's `ui_theme.h`, `ui_widgets.h` and `ui_capture.h` as
they are today, moved to `namespace adw::ui`**, with the same names,
signatures and behaviour. T makes the moved implementations by copying the
scr's `.cc` files, and never edits the scr's files. So S's adoption (§8,
S-M2) is a change of includes and namespace, not of calls:

* `adw/ui/theme.h` = scr `ui_theme.h` (Palette … `measure_text`).
* `adw/ui/widgets.h` = scr `ui_widgets.h` (Surface … `paint_overlay_scrollbar`).
* `adw/ui/capture.h` = scr `ui_capture.h` (`park_offscreen`,
  `capture_window_png`, `save_png_bgr`).

**Additions:**

```cpp
// adw/ui/theme.h
// true for the messages after which Theme::load (and a repaint) is due:
// WM_SETTINGCHANGE with lParam "ImmersiveColorSet", WM_SYSCOLORCHANGE, WM_THEMECHANGED,
// WM_DWMCOLORIZATIONCOLORCHANGED.
bool is_theme_change(UINT msg, WPARAM wp, LPARAM lp);
// Popup menus follow the app mode: uxtheme ordinal 135 (SetPreferredAppMode: AllowDark when
// `dark`, else Default) and 136 (FlushMenuThemes). A no-op where they are missing.
void allow_dark_menus(bool dark);
// A card: 8-DIP radius, pal.card, a hairline of pal.card_stroke.
void paint_card(HDC dc, const RECT& r, const Theme& t);
// The header band both apps share: the indigo glow and the stars (none under high contrast),
// the logo (the icon when given, else draw_crescent in its box), `name` in fonts.subtitle
// (the moon's navy RGB(0x1F,0x25,0x5A) in light mode, else pal.text) and `tagline` in
// fonts.body / pal.text2 on the same baseline. The scr's paint_night_band + header, moved.
void paint_header(HDC dc, const RECT& band, const RECT& logo, const RECT& title, HICON logo_icon,
                  const std::wstring& name, const std::wstring& tagline, const Theme& t);

// adw/ui/widgets.h
enum class ButtonRole { standard, accent, subtle, segment_left, segment_right, checkbox,
                        card };   // new: a settings card
// ButtonRole::card: a full-width clickable card (Windows 11 "SettingsCard"): the glyph set with
// set_button_role at the left (fonts.icons), the window text's first line in fonts.body_strong
// and its second line in fonts.caption / text2, a chevron (E76C) at the right; card fill, hover
// control_hover, pressed control_pressed, focus ring around it. Height is the caller's (64 DIP
// typical).
// A Fluent progress bar over a stock msctls_progress32 (which keeps its UIA RangeValue): a
// 1-DIP rail in pal.strong_stroke and a 3-DIP accent bar with round ends; PBS_MARQUEE draws the
// indeterminate bar (a segment sweeping every 2 s). Call once after creating the control.
void subclass_progress(HWND progress, const Theme* t);

// adw/ui/image.h
// A decoded picture: 32-bit premultiplied BGRA, top-down.
struct Image {
  int w = 0, h = 0;
  std::vector<uint8_t> pbgra;
  bool empty() const { return w <= 0 || h <= 0; }
  mutable std::shared_ptr<void> cache;   // draw_image's scaled copy (keyed by size)
};
// Any format WIC reads (first frame). COM must be initialized on the calling thread.
bool load_image(const std::wstring& path, Image& out, std::string* error = nullptr);
// `img` scaled into `dst` (high-quality bicubic; the scaled copy is cached in img.cache),
// clipped to a rounded rectangle of `radius` px, at `alpha` (0..255).
void draw_image(HDC dc, const RECT& dst, const Image& img, float radius = 0, int alpha = 255);
// A release's cover in `art` (4:5): the tile picture, or the generated cover when `tile` is
// null or empty, with 4-DIP rounded corners and a hairline border (pal.card_stroke; WindowText
// under high contrast).
void draw_cover(HDC dc, const RECT& art, const Image* tile, const std::wstring& title, const Theme& t);
// The generated cover (theme-independent, like box art): a vertical gradient #262B4F → #12152A,
// the crescent moon at (0.72 w, 0.22 h) with radius 0.11 w and a soft glow, five sparkles, and
// `title` in white Segoe UI Variable Display Semibold, left-aligned at 0.1 w, bottom-aligned at
// 0.9 h, wrapped to at most 3 lines, sized 0.14 w and shrunk until it fits.
void draw_generated_cover(HDC dc, const RECT& art, const std::wstring& title, int dpi);
```

T may add more to the library (internal helpers, a themed-window base for
the importer), but may **not** change anything listed above.

### 3.3 Visual rules both apps follow

* **Colours.** The WinUI 3 values in `make_palette`, flattened onto two
  surfaces: `base` (the window) and `card`. The accent comes from the user's
  accent palette (`read_accent`). High contrast uses only system colours,
  with no tints and no transparency.
* **Type.** Segoe UI Variable (Text / Small / Display), falling back to
  Segoe UI. The ramp: caption 12, body 14, body strong 14 semibold,
  subtitle 20, title 28. Icons use Segoe Fluent Icons, falling back to
  Segoe MDL2 Assets.
* **Spacing.** A 4/8-DIP grid, 24-DIP window margins, 16-DIP gaps, 20-DIP
  card padding, 32-DIP control height, 8-DIP card radius and 4-DIP control
  radius.
* **Chrome.** `apply_window_chrome`: an immersive dark title bar in dark
  mode, rounded corners, the caption colour equal to `base` (the system
  default under high contrast), and no caption text or icon drawn (the
  window text stays for the taskbar, Alt+Tab and screen readers).
* **No Mica.** The settings dialog paints solid `base`, because GDI cannot
  draw correctly over a system backdrop. Both apps keep solid surfaces.
  An earlier plan for Mica is superseded.
* **Live theme changes.** On `is_theme_change`: `Theme::load`,
  `apply_window_chrome`, `theme_native_control` on the native parts,
  `allow_dark_menus`, and a full redraw. On `WM_DPICHANGED`:
  `Theme::set_dpi`, then re-layout.
* **Focus.** Every focusable control is its visual body grown by
  `focus_margin()`. The focus ring is drawn only when `keyboard_cues()` is
  on.

## 4. The importer GUI restyle (T)

### 4.1 Structure

* **The seam.** `adimport.cc` (C) hands the GUI a `gui::Request` (§8.2)
  and returns `gui::run`'s result as the exit code. The GUI lives in
  `importer/gui/` as static library `adw_import_gui`, which links
  `adw_import` and `adw_ui`.
* **No task dialogs.** Every window is a themed top-level window, never
  `TaskDialogIndirect` and never `MessageBox`.
* **Themes and DPI.** Every window follows the light and dark app modes and
  high contrast **live** (§3.3), and handles per-monitor DPI v2 (the
  manifest already declares it).
* **Title.** The window title is exactly "Long After Dark".
* **Test contract.** `import.cli`'s GUI block finds the window by that
  title and presses buttons with `TDM_CLICK_BUTTON` (= `WM_USER + 102`,
  `wParam` = command id). **Every GUI window answers `TDM_CLICK_BUTTON` by
  running that command**, as a task dialog would. The test waits up to 5 s
  for the previous window to be gone before it clicks again. So each page
  should be a new top-level window (a new HWND), created at the old one's
  position. A page change that keeps the same HWND still passes, but costs
  the test 5 s per click.
* **Exit codes.** They are unchanged (README "GUI"): 0 when anything was
  imported *or any cover changed* during the session, else the first
  failure, and 5 when nothing changed because of a cancel.
  `AD_GUI_AUTOCLOSE=1` still skips the final result page (tests).

### 4.2 Pages

1. **Sources** (the chooser: `adimport --gui` with no source).
   * Header band `paint_header`: the moon, "Import a release" ("Import
     After Dark" until the sixth release) and the tagline "From your discs
     or the Internet Archive".
   * **Installed** section (only when something is installed): one row per
     installed release with its cover at 48×60 (`draw_cover`), its title,
     "N modules · verified: image", and a "Change cover…" link that opens
     §2.11 for that id. (As built for fourteen releases: a grid of covers,
     §9.3.)
   * **Import from**: three `ButtonRole::card` buttons:
     * **101** "A disc image…" (glyph E958)
     * **102** "A drive or folder…" (glyph E8B7)
     * **103** "Download from the Internet Archive…" (glyph E896), whose
       description gives the sizes, as today. It is hidden when nothing is
       downloadable.
   * Footer: "Files are copied to <win dir>", **Cancel** (IDCANCEL; exit 5
     unless something changed).
   * **101 and 102 behave as today.** The file dialog uses the same filter
     and multi-select, and a folder is checked with `identify_folder`. A
     folder that is not a known disc shows a `caution` message on this page
     and stays on it.
2. **Downloads** (from 103).
   * One `ButtonRole::card` per downloadable release, with ids **200+i**
     in registry order (limited by `--package`). Each shows its cover
     (installed) or generated cover at 48×60, the title,
     "CD image · 381.7 MB" / "Install files (ZIP) · 2.6 MB", and "Imported
     (verified: image)" / "Not imported yet", plus "already downloaded"
     when the file is in the downloads folder.
   * **299** "Every release not imported yet" (only when 2 or more are not
     imported), with the count and total size.
   * Footer: the md5 and downloads-folder note, and **Back** (IDCANCEL,
     back to Sources).
3. **Progress**.
   * The phase line: `phase_instruction` wording, "(2 of 4)" when several
     run, plus "Getting the box cover" for `Phase::cover`.
   * A `subclass_progress` bar, determinate or marquee.
   * "123.4 MB of 400.0 MB (5.0 MB/s)" and the current item.
   * **Cancel** (IDCANCEL). It greys and shows "Cancelling…" until the
     worker has stopped, as today.
   * The window cannot be closed while the worker runs, except through
     Cancel.
4. **Result**.
   * A glyph: success E930 in `accent`, partial E7BA in `caution_text`,
     failure EA39 in `critical_text`.
   * A heading ("Imported The Simpsons Screen Saver: 15 modules", "Imported
     3 of 4 releases", or the failure heading), and the body text of today's
     message boxes (files copied, verification, installed releases and
     module count).
   * **Done** (IDOK, default).
   * For a failure: "Nothing was changed." and a "Copy details" link that
     copies the full message and the mismatched files to the clipboard.
5. **Cover**: §2.11.

The layout uses the §3.3 spacing, a client width of 640 DIP (minimum 560;
720 since the sixteen releases, 816 since the twenty, §9.3),
and a height fitted to the page, clamped to the work area. Lists longer
than the window scroll inside a card, using `attach_overlay_scrollbar` on a
child container.

### 4.3 Screenshot hook and tests (T)

* **Hook.** `AD_IMPORT_TEST_SCREENSHOT=<png>` with
  `AD_IMPORT_TEST_SCREENSHOT_STATE=page=sources|downloads|progress|result|error|cover;
  theme=light|dark|hc; dpi=<n>; focus=<command id>; progress=<0..1|marquee>` renders the
  page off-screen (`park_offscreen` + `capture_window_png`) from synthetic
  state, and exits 0.
  * It runs no import and needs no network.
  * The installed and cover data come from a scratch `--dest` tree the test
    builds, with synthetic tiles.
* **`import.gui_shots`** (not labelled `gui`, because nothing is shown):
  every page × light/dark/hc × 100/150/200%. Each PNG exists, and its base
  pixel equals `pal.base` for its theme.
* **`import.gui_flow`** (label `gui`): through `TDM_CLICK_BUTTON`,
  Sources → 103 → Back → Cancel (exit 5), then Change cover on a scratch
  install with a synthetic picture (exit 0 and `tile.png` changed). It
  skips (77) while C's covers are still stubs (`set_cover` answers "not
  implemented").
* **`import.cli`'s GUI block** (C's file, `AD_GUI_TESTS=1`) must pass
  unchanged.

## 5. File inventory today (for the ownership split)

Importer, **GUI**:
* `adimport.cc` lines 418–958: the whole "GUI front-end" section
  (`task_dialog`, `message`, `pick`, `choose_downloads`, `choose_source`,
  `GuiRun`, `progress_callback`, `report_several`, `run_gui`).
* `adimport.manifest` (comctl6, DPI, console policy) and `adimport.rc`
  (manifest and version resource).
* `tests/test_cli.cc` lines 29–86 and 192–223 (`click_through`, the
  `AD_GUI_TESTS` block).

Importer, **non-GUI**: everything else.
* `adimport.cc` lines 1–417 and 959–1020: argument parsing, the console
  front-end and `main`.
* `catalog.*`, `download.*`, `fat.*`, `importer.*`, `install.*`,
  `iso9660.*`, `md5.*`, `minijson.*`, `names.h`, `packages.*`, `source.*`,
  `status.h`, `winutil.h`, `zip.*`.
* `known_files*.inc`, `gen_known_files.py`, `CMakeLists.txt`, `README.md`.
* every other test.

`adimport.cc` mixes the two, so it goes to **C** as a whole. The GUI section
leaves it through the `gui.h` seam (§8.2).

## 6. Sequencing and milestones

The three implementers start together. Each milestone can be recognised
from files on disk, so nobody has to wait for a message.

| Milestone | Who | Content | Recognised by | Unblocks |
|---|---|---|---|---|
| **T-M1** (first) | T | `adw_ui` complete to §3.2 with its tests, registered in the top-level `CMakeLists.txt`. `importer/gui/gui.h` exactly as §8.2, `importer/gui/CMakeLists.txt`, and a working `gui::run` (at first it may be today's task-dialog flow, ported unchanged, so behaviour stays the same) | `common/ui/CMakeLists.txt` and `importer/gui/CMakeLists.txt` exist, and `ui.*` tests pass | S-M2, C-M3 |
| **C-M1** (first) | C | `covers.h` exactly as §2.9. Stubs: `cover_info` reports `generated` (and `installed` from `list_packages`); set, clear and refresh return `Status::error` "covers are not implemented yet". `Progress::Phase::cover`, `ImportOptions::cover_download`, the `--change-cover`/`--no-cover-download` parsing, and the conditional GUI seam (§8.2) | `importer/covers.h` exists | T-M2 |
| S-M1 | S | Catalog/settings/saver/model/layout changes, grouping by release, and the strip control drawn with the scr's own (frozen) theme code. Tests for all of it | — | — |
| C-M2 | C | The full cover pipeline (§2), the CLI, catalog fields, and `import.covers` + `import.covers_real` | — | T's `import.gui_flow` cover case |
| T-M2 | T | Every importer page restyled (§4), and `import.gui_shots` + `import.gui_flow` | — | C-M3 |
| **S-M2** (after T-M1) | S | Adopt `adw_ui`: delete `scr/src/ui_theme.*`, `ui_widgets.*` and `ui_capture.*`; include `adw/ui/*.h` (`using namespace adw::ui` inside `adw::scr` is fine); link `adw_ui`. Swap `paint_night_band`/`paint_card` for `ui::paint_header`/`ui::paint_card`, and draw tiles with `ui::draw_cover`/`draw_generated_cover` and `ui::allow_dark_menus`. All scr tests green, and the `list-top` renders unchanged apart from the intended changes | — | — |
| C-M3 (after T-M1, ideally T-M2) | C | Delete the legacy GUI section and the `ADIMPORT_HAVE_GUI2` conditional from `adimport.cc`. `import.cli` with `AD_GUI_TESTS=1` green against T's GUI | legacy block gone | integration |

**Frozen code.** From the start, S treats the scr's `ui_theme.*`,
`ui_widgets.*` and `ui_capture.*` as frozen. S reads them but does not edit
them: T is copying them, and the two copies must not drift. Whatever new
drawing S needs goes in S's own new files (for example
`scr/src/cover_strip.{h,cc}`). A change S needs in a shared widget is
raised in S's report for T.

## 7. Acceptance (integration, after T, C and S)

* **Build.** A full build with `AD_COMPONENTS="host/loader;common/ui;importer;scr"`
  (plus the lanes for `package.sh`) is warning-clean, and every ctest suite
  is green.
* **Imports.** Import the four new packages into a *copy* of the scratch
  root `build\win-pkg-setup\assets` (never `%LOCALAPPDATA%`), once online
  and once with `--no-cover-download`. Covers come out as in §2.3, the
  catalog has `packages[].cover`, and `FILES` is byte-identical.
* **Settings dialog screenshots** (off-screen) over that copy, in
  light/dark/hc at 100/150/200/250%, with 0, 1, 2 and 5 filters. The strip
  and the release groups match §1.
* **Importer screenshots** of every page (§4.3).
* **Filter and rotation.** `Collections` persists. A `/s` run with
  `AD_SCR_TEST_ROTATE_MS` and a Simpsons-only filter plays only Simpsons
  modules.
* **Hygiene.** No window, process, mounted image or temp file is left
  behind.

## 8. File ownership and seams

### 8.1 Who edits what (strictly disjoint)

| Owner | Files |
|---|---|
| **T** (theme + importer GUI) | `common/ui/**` (new) · the top-level `CMakeLists.txt` · `importer/gui/**` (new: `gui.h`, the GUI sources, `CMakeLists.txt` defining `adw_import_gui` and its tests, `README.md`, `tests/**`) · `importer/adimport.manifest` · `importer/adimport.rc` |
| **C** (importer non-GUI) | `importer/**` **except** `gui/**`, `adimport.manifest` and `adimport.rc`. That includes `adimport.cc` (all of it), `CMakeLists.txt`, `README.md`, `packages.*`, `importer.*`, `catalog.*`, `download.*`, the new `covers.*` and any new image-codec files, `known_files*`, and every file in `tests/`, including `test_cli.cc`, whose GUI block is kept as the GUI's regression contract |
| **S** (screen saver) | `scr/**` |
| nobody in this pass | `host/**`, `tools/**`, `docs/**` (updated at integration), the top-level docs, `.github/**` |

Reading any file is always fine. When an owner needs a change in someone
else's file, it goes in that owner's report, not into the file.

### 8.2 The seams

| Seam | Owned by | Used by | Frozen by |
|---|---|---|---|
| `importer/gui/gui.h` (below) | T | C (`adimport.cc`) | this section |
| `covers.h`; `Progress::Phase::cover`; `ImportOptions::cover_download` | C | T | §2.9 |
| `adw/ui/*.h` | T | S (at S-M2), T's GUI | §3.2 |
| `catalog-win.json` `packages[].cover` and the module fields | C writes | S reads | §2.7, §1.10 |
| `[Saver] Collections` | S | S (dialog and saver) | §1.9 |
| `adimport --gui --change-cover <id>` and its exit codes | C parses, T implements | S launches | §1.11, §2.8, §2.11 |
| `import.cli`'s GUI block (window title, `TDM_CLICK_BUTTON`, ids 101/102/103, 200+i, 299, IDCANCEL) | C (the file) | T must satisfy it | §4.1 |

```cpp
// importer/gui/gui.h — owned by T. Frozen by COVERS.md §8.2: adimport.cc calls it.
#pragma once
#include <filesystem>
#include <optional>
#include <string>

#include "importer.h"   // Source

namespace adw::import::gui {

struct Request {
  std::optional<Source> source;   // --image/--iso/--from/--download given: no chooser
  bool download_all = false;      // --download all (`source` is then the download base)
  std::filesystem::path dest;     // --dest; empty = default_assets_root()
  std::string package;            // --package <id>: limits the download list
  bool no_verify = false;         // --no-verify
  bool cover_download = true;     // false = --no-cover-download
  std::string change_cover;       // --change-cover <id>: only the cover window (§2.11)
};

// Shows the importer's windows (COVERS.md §4) and returns the exit code (adw::import::Status).
// Initializes COM, common controls and GDI+ itself; honours AD_GUI_AUTOCLOSE.
int run(const Request& r);

}  // namespace adw::import::gui
```

C's side of the seam:
* `importer/CMakeLists.txt`:

  ```cmake
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/gui/CMakeLists.txt)
    add_subdirectory(gui)
    target_link_libraries(adimport PRIVATE adw_import_gui)
    target_compile_definitions(adimport PRIVATE ADIMPORT_HAVE_GUI2=1)
  endif()
  ```

* `adimport.cc`: builds a `gui::Request` from its `Args`, calls
  `FreeConsole()` when started from Explorer (as today), and returns
  `gui::run(req)` under `#if ADIMPORT_HAVE_GUI2`. Otherwise it returns
  today's `run_gui(a)`, which C deletes at C-M3.

T's side:
* `importer/gui/CMakeLists.txt` defines only `adw_import_gui` (linking
  `adw_import`, `adw_ui`, `comctl32`, `shell32`, `ole32`) and T's tests. It
  never defines or changes `adimport`.
* T's tests may use `$<TARGET_FILE:adimport>`.

### 8.3 Rules for every implementer

* **Build directories.** Each implementer uses its own, for example
  * `AD_BUILD_DIR=build/win-ui AD_COMPONENTS="host/loader;common/ui;importer"` (T)
  * `AD_BUILD_DIR=build/win-covers AD_COMPONENTS="host/loader;importer"` (C)
  * `AD_SCR_SKIP_GUI_TESTS=1 AD_BUILD_DIR=build/win-strip AD_COMPONENTS="host/loader;scr"` (S)

  with `bash tools/build.sh`.
* **Assets.** Never write under the real data folder
  (`%LOCALAPPDATA%\LongAfterDark`), and
  never let a test take a default location from it: the suites set a
  scratch `AD_LOCALAPPDATA` (`importer/tests/test_util.h`). The
  scratch all-packages root `build\win-pkg-setup\assets` is **read-only**
  for everyone: copy it into your own build dir to write.
* **Offline tests stay offline.** A test that imports with the built-in
  registry passes `--no-cover-download` or uses a registry without cover
  downloads. Only the opt-in `*_real` tests reach the network.
* **Screenshots are taken off-screen** through the hooks. Desktop windows
  are allowed only where a test really needs them. Avoid floods of GUI
  processes, and leave no window, process, image mount or temp file
  behind.
* **Out of scope.** No commits, pushes, stashes, resets or checkouts. No
  renaming ("Long After Dark" comes later). No module-interaction changes
  (INTERACTION.md is a deferred follow-up). No After Dark or third-party
  bytes in the repo.

## 9. As built (integration, 2026-09-26)

What the three implementations added to or settled in this design. The
sections above stay the contract; this records the differences.

### 9.1 Importer, non-GUI (C)

* `phase_name(Phase::cover)` is "cover"; the console says "Getting the box
  cover".
* Additions: `CoverOptions::timeout_ms` (15000), `ImportOptions::cover_timeout_ms`
  and `cover_download_dir`, `DownloadOptions::timeout_ms`, and a
  `ConnectionError` so DNS, connect, TLS and timeout failures are told apart
  from an HTTP error.
* `--download-dir` is accepted with `--image` and `--from` as well: it says
  where their cover downloads go (else
  `%LOCALAPPDATA%\LongAfterDark\downloads\covers`, taken only when a cover
  is actually downloaded).
* `AD_COVER_DOWNLOAD=0` in the environment turns every cover download off
  (imports and refresh), as `--no-cover-download` does. Tests whose command
  lines are fixed use it.
* `import.json` (PACKAGES.md §5.3) reports tool "adimport 1.2" ("adimport
  1.3" since the sixth release).
* A user-picture cover in `packages[].cover` has no `art` (a user picture has
  no art type); `label` is "Your own picture" and `credit` is "". Readers
  treat `art` as optional (the scr does).
* WinHTTP timeouts fire in steps of about 4 s, so the 15 s cover timeout acts
  as about 16 s, and a stalled server costs an import about 33 s before the
  disc art is used.
* `--refresh-covers --force` follows §2.4 literally (*k* = ∞): for `ad32`,
  whose best source is disc art that a refresh cannot read, it replaces the
  installer splash with the disc-label download.
* A catalog write re-renders a stale or damaged tile of any installed
  package (§2.10), a narrow exception to "an import writes only its own
  package".

### 9.2 `adw_ui` (T; integration)

* Additions beyond §3.2: `draw_app_mark`, `CardCover` / `set_card_cover` (a
  cover on a card button), `card_height`, `image_from_bgra`,
  `generated_cover_image` and `settle_for_capture`.
* `settle_for_capture`: a cloaked window that has never been drawn through a
  plain `PrintWindow` captures black with `PW_RENDERFULLCONTENT`. Both
  screenshot hooks (the importer's and, since integration, the scr's) call it
  before `capture_window_png`.
* Added at integration, for the strip: `fade_in_left`, and `fill_round` and
  `stroke_ellipse` overloads with an alpha. They are the strip's former local
  helpers moved as they were; the settings dialog renders pixel-identical.
  (Since the strip shows only whole covers, for twelve releases, the scr no
  longer uses `fade_in_left` or `fade_in_right`, §1.3.)
* `ButtonRole::card` has a 4-DIP corner radius (WinUI's SettingsCard uses the
  control radius); `paint_card` cards keep 8 DIP.

### 9.3 Importer GUI (T; integration)

* Command ids beyond §4.2: **400 +** registry index, "Change cover…" on
  Sources; **501** "Copy details" (Result, error); in the cover window
  **601** Choose a picture…, **602** Use the original cover, **603** Download
  the original cover, and IDOK Done. Since fourteen releases: **300 +**
  registry index, an installed release's cover on Sources, which opens its
  menu, whose items are **400 +** "Change cover…" and **450 +** "Remove
  <shortTitle>…" (not controls: `TDM_CLICK_BUTTON` presses them for an
  installed release); in the Remove window **701** Remove and IDCANCEL
  Cancel.
* Screenshot keys beyond §4.3: `phase`, `result`, `package`, `status`,
  `caution`, `workarea`, `dpichange`, `themechange` and `report` (where the
  client area is in the picture, and `pal.base`; since the twelve releases,
  on Sources also `list=<shown>,<whole>,<row>`: the installed list's height
  as shown, its whole height and a row's, in pixels).
  `AD_IMPORT_TEST_PICK=<path>[|<path>…]` answers the file dialogs.
* On a work area too short for the Sources page its installed list scrolls
  in its card, first shrunk to two rows, then the whole body scrolled; the
  header and the footer (Cancel) stay put. Since the twelve releases the
  list shows as many whole rows as fit, two at the least (eleven of twelve
  at 150% on a 2560×1440 monitor, six at 100% on a 1080-line screen); with
  room for fewer than two, as many as fit, down to one (the part of a row
  that shows says there are more: ten releases and more at 150% on a
  1080-line screen keep the import choices in view); only when even one row
  does not fit does the whole body scroll, with the list at its full
  height, so only one thing ever scrolls. `import.gui_shots` checks it from
  the report's `list=`, with cover downloads off: at least 11 whole rows of
  12 at a 2560×1392 DIP work area and 150%, at least 5 at 1920×1032 and
  100%, and the fallback at 640×520 and 150%. The
  Sources intro names every release only while nothing is imported, and
  then gives their number ("… of twelve releases."). A release known by
  the ZIP of its install files is "verified against the known ZIP", and
  its result reads "The ZIP matched the known ZIP of …". The Downloads page
  has twelve cards.
* Integration fixes: a page the system clamps below the size it asked for
  (the maximum tracking size) is laid out again for the client it got, so the
  footer stays in view; off screen the hook lifts that limit, so a 200%
  screenshot shows the whole page as a large monitor would. The Progress page
  shows no "0.0 MB" amount for a step that has nothing to count (a cover
  from the disc, and since the twelve releases the finishing step too).
  Since the twelve releases, each release of **Every release not imported
  yet** starts at "Starting…" under its own title (never under the previous
  release's "Finishing"), and the item line shows only what the step names
  (a download's file name, the file copied or verified, the cover's source),
  never the last log line.
* The windows use the moon mark, drawn at run time, as their icon;
  `adimport.exe` itself still has no icon resource.
* **As built for fourteen releases: covers on Sources, and removing a
  release.** Fourteen rows of the installed list made Sources almost as
  tall as a 4K screen at 150%. The installed releases are now a grid of
  their covers in the card, 64×80 like the strip's regular ones, each with
  its `shortTitle` and "N modules" in captions under it: as many columns as
  cells of at least 100 DIP fit in the card (five in the 640-DIP window,
  sharing its width), so fourteen releases take three rows (about 430 DIP
  instead of 1064), and the window fits a 1080-line screen at 100%. A cover
  is a push button that opens the release's menu ("Change cover…",
  "Remove <shortTitle>…") under it on a click, Enter or Space, and at the
  pointer on a right-click (Shift+F10 and the Apps key too); screen readers
  call it "After Dark 4.0 Deluxe, 84 modules · verified against the
  original disc", and its tooltip says the same and what a click offers.
  One cover is a tab stop (the first, then the one last focused); Left and
  Right step through them, Up and Down go a row, Home and End to the ends.
  The note under the grid says "Click a cover to change it or to remove the
  release." first. The fallback for a short work area is the list's,
  counted in grid rows: as many whole rows as fit, two at the least, else
  as many as fit, else the whole body scrolls; the report's `list=` is
  `<shown>,<whole>,<row>,<rows>,<cols>`.
  **Remove** (`--gui --remove <id>`, or "Remove…" on a cover): the
  release's cover at 96×120 beside "Remove The Simpsons Screen Saver?",
  what it holds ("15 modules · verified against the original disks") and
  what removing does ("Its 15 modules are deleted from this computer, and
  the screen saver stops showing them. Its cover is kept, and you can
  import the release again at any time."); **Remove** (accent, the
  default) and **Cancel**. Remove runs `remove_package` on a worker under a
  marquee bar, Cancel greyed; done, the window closes: from Sources, back
  to Sources, which no longer shows it and says "Removed The Simpsons
  Screen Saver." once, its Cancel now Close; alone, adimport exits 0. A
  failure (a file of it in use) shows in critical text ("Nothing was
  removed. …"), with **Try again**. A release that isn't installed says so,
  with only Close. Exit codes: 0 when it was removed, else the failure,
  else 5. Screenshot keys: `page=remove` with `package=` and
  `status=error|running`, and `notice=1` on Sources.
* **As built for sixteen releases: six covers to a row.** With every
  release installed, a fourth row of five made Sources too tall for a
  1080-line screen at 100% (`import.gui_shots`: three of the four rows
  shown, the grid scrolling). The window is now 720 DIP wide (560 at the
  least, as before; less only when the work area is narrower), where the
  grid's rule fits six cells, so sixteen releases take three rows of six
  and Sources is about 920 DIP tall. A narrower work area (the tests'
  640-DIP ones) keeps five to a row, four rows, with the short-screen
  fallback as before.
* **As built for twenty releases: seven covers to a row.** Six to a row
  would have put twenty releases on four rows, too tall again for a
  1080-line screen. The window is now 816 DIP wide, where the grid fits
  seven cells, so the twenty take three rows (7, 7 and 6) and Sources keeps
  its height; the narrower work areas keep their fewer columns.
* **As built for twenty releases: the Downloads list held to Sources'
  height.** Its twenty cards (and Every release not imported yet) took the
  whole height of the work area, a screen-tall column at 150% on a 4K
  monitor. The list now shows nine whole cards at the most and scrolls for
  the rest (the scroll bar's room taken from the cards' width, as before),
  so the page is a little less tall than Sources with every release
  installed (895 against 907 DIP at a 1920×1032 work area and 100%) and
  the window keeps its height from one page to the other; a note longer
  than two lines (a long downloads folder) takes its lines from the list.
  On a shorter work area it shows as many whole cards as fit, two at the
  least, else what fits (never less than 80 DIP, as before), as Sources
  does with its rows. A card the keyboard focuses is scrolled into view.
  The screenshot report gives, on Downloads, `list=<shown>,<whole>` and
  `cards=<end>,…` (where each card ends in the list, in pixels).
* The Downloads page reads "already downloaded" from the default downloads
  folder (`gui::Request` has no download folder).
* PACKAGES.md §5.2's "GUI restyle out of scope" is superseded by §4.

### 9.4 Settings dialog (S): choices where §1 is silent

* With every tile selected the status reads "Showing all N releases" and
  **Show all** hides; "a filter is active" means some but not all selected.
* In Random mode too, a module the filter hides stays chosen, but it leaves
  the details card, which shows a listed module instead (§1.6).
* A release whose modules all need a lane the host lacks shows "Coming
  soon" on its group header. The chip names the release, never the lane, so
  the `config-classic` smoke test now expects "Other modules · Coming soon".
* Compact chevrons are 24×28 DIP (on the 4-DIP grid, centred on 60-DIP art).
* Arrow keys are handled in the tiles' subclass (`DLGC_WANTARROWS`) so they
  wrap inside the strip; the behaviour is §1.5's.
* `Collections` with no filter: OK leaves an absent key absent, and writes it
  empty only when it held something.
* §1.5's screen-reader assumption is verified by `config-collections`
  through UI Automation: each tile is a CheckBox with the Toggle pattern and
  the §1.5 name, and the status line's LiveSetting is Polite.
* The dialog's minimum client height with the strip (680 DIP) is kept even
  where the screen is shorter (200% on a 1080-line display): the window
  keeps its minimum and shows the compact strip.

### 9.5 Acceptance run (§7)

* **Build and tests.** A full-tree build (every component) is
  warning-clean. Every non-GUI suite passes; of the opt-in ones,
  `import.covers_real` (with `AD_E2E_PKG=1`) and `pe32.packages` (over the
  scratch root) pass. `import.gui_flow` and `import.cli` with `AD_GUI_TESTS=1`
  pass. The scr's GUI smoke tests skipped (77) at integration because the
  user's session was disconnected (no input desktop); S had run all of them
  green before integration, and the integration's scr changes render
  pixel-identical.
* **Covers on the scratch root** (`build\win-pkg-setup\assets`):
  `--refresh-covers all` fetched and checked the five default downloads;
  re-importing `ad32` and `tt` from their images captured their installer
  splashes over the disc-label downloads (a better source replaces a
  fallback); `--set-cover` put the user's pictures on `deluxe`, `tt` and
  `ad10`. The strip then shows Deluxe (user), 10th Anniversary (user), 3.2
  (installer splash), Totally Twisted (user) and Simpsons (Wikisimpsons box
  front).
* **Rotation.** `/s` over that root with `Collections=simpsons` played only
  the 15 Simpsons modules.
* **Screenshots** (gitignored): `research/win/ui/covers/final_*.png`.
