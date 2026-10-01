# LongAfterDark.scr — the Windows screen saver

`LongAfterDark.scr` is the screen saver of **Long After Dark**. It is a
plain Win32 screen saver (no MFC/ATL/.NET) and never runs After Dark code itself: for each monitor it
starts `adhostwin.exe`, which emulates the original module, and shows the
frames the host streams back over a pipe (`docs/DESIGN.md` §1). The
settings dialog edits `%LOCALAPPDATA%\LongAfterDark\settings.ini` using the
module list in `catalog-win.json` (§6a).

## Install

`LongAfterDark.scr` needs `adhostwin.exe` in the same folder (and
`adimport.exe` there too, for the dialog's **Import…** button). Keep the three
files together in either of these places:

* **Any folder:** right-click `LongAfterDark.scr` → **Install**. Windows makes
  it the current screen saver in place and opens Screen Saver Settings.
* **System-wide:** copy all three files to `%WINDIR%\System32`. Long After
  Dark then appears in the Screen Saver list for every user.

Screen Saver Settings lists it as "Long After Dark": the `.scr`'s string
resource 1 (`IDS_DESCRIPTION`), which Windows shows instead of the file name.

The modules come from your own discs: any of the fourteen releases (eleven of
After Dark modules: After Dark 4.0 Deluxe, After Dark 3.2, Totally Twisted
After Dark, After Dark 10th Anniversary, The Simpsons Screen Saver, Star Trek:
The Screen Saver, which is After Dark 2.0b on two floppies, Marvel Comics
Screen Posters, Snoopy's Screen Savers, The Looney Tunes Screen Saver,
ScreamSavers and The Disney Collection Screen Saver; and three whose modules
run on Delrina's Intermission engine: LucasArts' Star Wars Screen
Entertainment, and Delrina's The Far Side Screen Saver Collection and Scott
Adams' Dilbert Screen Saver Collection), from
the disc (or the floppies), an image of it, a copy of its files, or the
Internet Archive download. Click **Import…** in the settings dialog (or run `adimport.exe`) to
copy them to `%LOCALAPPDATA%\LongAfterDark\assets\win`. Until then the saver
shows "No modules imported".

## The data folder

Everything the saver keeps is under `%LOCALAPPDATA%\LongAfterDark`: the
imported `assets\`, `settings.ini`, the modules' `state\`, the dialog's
`thumbs\` and the last-exit log in `logs\`. `AD_LOCALAPPDATA` stands in for
`%LOCALAPPDATA%` (tests use it for a scratch folder); on the secure desktop,
with a thin environment, the saver asks `SHGetKnownFolderPath` instead.

The saver finds the folder the way `adhostwin.exe` and `adimport.exe` do
(`host/core`'s header-only `adw/core/data_root.h`; see
`host/core/README.md`, "The data folder"), so all three agree on it.
`app_data_root()` in `paths.h` only names the folder (the first file written
there makes it), and with `AD_SETTINGS` and `AD_ASSETS_DIR` both set the
saver keeps nothing there.

**Names.** The saver's windows are of class `LongAfterDarkSaver` (the `/s`
and `/p` windows), `LongAfterDarkLivePreview` and
`LongAfterDarkThumbnailQueue`; its temporary files are
`%TEMP%\LongAfterDark-preview-<pid>.ini` and the desktop captures,
`%TEMP%\LongAfterDark-seed-<pid>-<window>.ppm` for a window that may start
with a module that follows the display (an After Dark module) and
`…-<window>-640x480.ppm` for one that may start with a module that has a
640×480 screen of its own (an Intermission, Star Trek, ScreamSavers or
Marvel module; the first
file serves both where the two pictures are the same: **Starting from the
desktop**).

## Command line

These are the standard screen saver switches. Case doesn't matter, `-` works
like `/`, and the window handle can be written as `/p 1234`, `/p:1234` or `/p1234`.

| Switch | What it does |
|---|---|
| `/s` | Full screen, with one topmost window per monitor. A key, a click, the wheel or a nudge of the mouse ends it (see **Ending the saver and playing** below). |
| `/p <HWND>` | Live preview inside that window at 320×240 (for every module: see **Emulated screen**). It exits when the window goes away. |
| `/c[:HWND]` or nothing | Opens the settings dialog. |
| `/a` | Ignored (Windows 9x password change). |

## Ending the saver and playing

The rules are the 1996 After Dark 4 saver's (`docs/INTERACTION.md`
§4), with one addition, Alt:

* **Not playing:** any key except Shift, Ctrl, Caps Lock and Num Lock, any
  click, the wheel or a nudge of the mouse (more than 10 pixels) ends the
  saver, and so does switching away (the Windows key, Alt+Tab,
  Ctrl+Alt+Del). Caps Lock and Num Lock never do: in some modules Caps Lock
  does something (it scares the fish, changes the colours) or starts a game,
  and Num Lock starts Final Exam's exam.
* **Playing** (after Caps Lock in Rodger Dodger, You Bet Your Head, Simpsons
  Trivia, Mime Hunt, Frankenscreen, Marbles, RPS, Magic Turtle's editor,
  How to Draw…; after Num Lock in Final Exam, Star Trek: The Screen Saver's
  Starfleet Academy exam): keys, clicks and the mouse belong to the game.
  Press Caps Lock again to stop playing (Num Lock, in Final Exam; the next
  key or move then ends the saver), or press **Alt** (or F10) to end it at
  once. Final Exam's answers are the number keys; a move of the mouse ends
  its exam, and the module asks its host to wake the saver (as After Dark
  2.0 let it), which ends the saver too (`input: wake`). While a game runs
  the pointer shows when the module asks for one, stays on the primary
  monitor, and the randomizer waits before switching modules (on every
  monitor, unless `DifferentPerMonitor=1`: see **Settings**).
* Locking the session (Win+L, Ctrl+Alt+Del then Lock, an idle-lock policy) or
  disconnecting it ends the saver, playing or not: a lock sends no
  deactivation, and the saver, a game and its sound would otherwise run on
  behind the lock screen.
* Only the primary monitor plays; the others keep running on their own.

How: the window on the primary monitor is the **input owner**. Only its host
gets input: each key as `KEY <vk> <0|1>`, then `CAPS <0|1>` when the Caps Lock
toggle changed and `NUMLOCK <0|1>` when the Num Lock toggle changed (each
checked on every key down and up, and every 250 ms), clicks
and moves as `MOUSE <x> <y> <buttons>` in the host's emulated-screen
coordinates (moves coalesced, one per `GO`). Every host starts with the Caps
Lock toggle in `ADCAPS` and the Num Lock toggle in `ADNUMLOCK`, since modules
latch them when they start (Final Exam begins its exam on a change of Num
Lock). Num Lock reaches a host only when its `--capabilities` says
`numlock=1` (`HostCapabilities` in `dialog_support.h`): a host without it
would ignore a `NUMLOCK` line without numbering it, and every input line
after it would then carry a number one higher in the saver's count than in
the host's (the holds below would wait for numbers it never reaches).
Unless the rotation waits for the host's answer (it holds a module of
another ABI than After Dark's: **What Random leaves out** below), the first
hosts start before it (the saver never waits for it on this account), so
they get `ADNUMLOCK` anyway, which a host without the toggle ignores; once a
host has answered without `numlock=1`, its hosts start without it (the
first ones too, when they waited for the answer). Each host
publishes a status record (interactive, cursor, rotate-ok, key-filter, wake;
the number of the last input line it applied and the highest it consumed) in
a one-page shared section passed as `ADSTATUSHANDLE` (`adw/core/status.h`),
and the saver reads it as each of the owner's frames arrives. For a key or
click that would end the saver, `decide()` (`input_rules.h`, a pure function
with unit tests) answers at once, or, when the host has not yet stepped with
the input sent just before (a Caps Lock press a moment ago may be starting a
game) or may consume input without playing (a keyboard hook, a module that
reads the saver window's queue), waits for the host's verdict, at most
300 ms. The owner's host dying ends such a wait at once. Why each run ended
is logged (`input: key vk=0x41`, `input: syskey vk=0x12`, `input: move dx=…
dy=…`, `input: deactivated fg=<exe>`, `input: session locked`, `input: wake`,
`test-exit`); the toggles the owner's host is told of are logged too
(`input: caps 1 -> owner (n=12)`, `input: numlock 1 -> owner (n=14)`), and
each spawn line says what the host started with (`caps=0 numlock=1`;
`numlock=-1`: none).

**The last-exit log.** Every `/s` run rewrites `logs\saver-last.log` next to
`settings.ini` (`AD_SCR_LASTLOG` overrides): the start (build, monitors, the
module, the host's capabilities), every spawn, host exit, respawn, rotation
and play start/end, and the exit reason, at most 200 lines (the first ones and
the latest ones are kept). It is always on, so a report of the saver ending
early comes with its cause.

## The settings dialog

`/c` opens a Windows 11-style window (plain Win32, no extra runtime), in the
light or dark app mode Windows is set to, with the user's accent colour, and
in the system colours under high contrast. It follows per-monitor DPI, can be
resized, minimized and maximized. Its title bar shows only the caption
buttons (the caption "Long After Dark" is still there for the taskbar, Alt+Tab
and screen readers); the header under it names the window: the moon, "Long
After Dark" (in the moon's navy in light mode) and "Screen saver settings", over a
soft indigo glow, with a scatter of stars across the band in dark mode and a
few faint sparkles in light mode. In a wide window everything stays in a
column at most 1240 DIP wide, centred.

* **The box-cover strip** (`docs/COVERS.md` §1), when two or more
  releases (packages) are imported: one 4:5 box cover per release across the
  top, oldest release first (Star Trek, Marvel, Simpsons, Star Wars, Snoopy,
  Looney Tunes, ScreamSavers, 3.2, Totally Twisted, Disney, Deluxe, 10th
  Anniversary), each with its short title under it (64×80 DIP covers; 48×60
  without captions when the window is under 760 DIP tall). Seven fit the
  first-open window side by side, and eight compact ones the smallest. The
  twelve's regular covers never all fit side by side (they need 1240 DIP;
  the tiles area stops growing at 1024, with the column at 1240), so in a
  window 760 DIP or more tall their row scrolls: seven covers at a time in
  the first-open window, five in one as narrow, nine in a large one. Their
  compact covers scroll in a window under 1120 DIP wide: eight at a time in
  the smallest, nine from 960 DIP wide, ten from 1032 (so in a first-open
  window whose height the screen's work area clamps under 760 DIP, as on
  1920×1080 at 125% or 150%, or 1366×768), eleven from 1104; from 1120 DIP
  wide all twelve show side by side. A
  caption may use its cover's whole window (the cell and its focus margins),
  so every release's short title fits whole at every scale; a longer one
  would be drawn at 11 or 10 DIP before it is ellipsized.
  A cover is the release's `tile.png` from the catalog (`packages[].cover`,
  written by adimport), else a generated one: the night sky, the moon and
  the release's title. Clicking covers filters the list to those releases;
  several can be selected, and **none selected means all**. Selected covers
  have an accent ring and a check badge; while a filter is on, the others
  dim (with a hairline outline, so a dark cover keeps its shape on the dark
  base). Beside the strip a status line in `text2` ("Click covers to filter
  the list", "Showing 2 of 5 releases", a polite live region for Narrator)
  and a **Show all** link; with no filter on and a release still showing a
  generated cover (an install from before covers, or an offline import), a
  **Get the covers** link instead, which runs `adimport.exe --gui
  --refresh-covers all` the way Import… runs (exit 0 reloads the catalog;
  nothing fetches covers on its own). Each cover is a real toggle button
  ("The Simpsons Screen Saver, 15 screen savers, check box"; its tooltip
  ends "Right-click to change its cover."): the arrow keys, Home and End
  move between them (one tab stop), Space toggles, Shift+F10 opens its menu:
  **Show only …**, **Show all releases** and **Change cover…** (runs
  `adimport.exe --gui --change-cover <id>` the way Import… runs; exit 0
  reloads the catalog, keeping every check, the filter and unsaved values). With more releases than fit, the row scrolls by whole
  covers (chevrons at the ends, the wheel, or the keyboard focus): every stop
  shows as many whole covers, as many as fit between the two chevrons, and
  nothing of the others, so no cover or caption is ever cut and nothing lies
  under a chevron. Covers that all fit start at the column's edge; a row
  that scrolls starts after the left chevron's place at every stop (empty
  while the row is unscrolled), so each stop shows its covers in the same
  places. Each chevron keeps one place whatever the scroll position (the
  left one at the strip's left edge, the right one just past the last cover
  a stop shows, 4 DIP clear of the covers and their focus rings), so
  repeated clicks keep scrolling, and its place is empty at the stop where
  it hides (the left one's unscrolled, the right one's at the last stop): a
  click too many lands on no cover. A cover that takes the keyboard focus, or that
  Space toggles while the chevrons have scrolled it away, comes into view,
  and with a filter saved (or kept through a reload) the row opens scrolled
  to the first selected cover. The
  window opens at 1040×836 DIP with the strip (at least 680 tall; 716 without it).
* **Single module / Random** at the top left chooses what the saver plays.
  Below it, the **module list**, grouped by release (the release's title and
  its number of modules, oldest release first), with a hairline and 12 DIP of
  space before each group after the first. A title too long for the list
  beside its count, and beside the "Coming soon" pill when the group has one,
  is ellipsized ("Star Wars Screen Entertain… 14" in the narrowest window, in
  Random; The Looney Tunes Screen Saver's title there too, and The Disney
  Collection Screen Saver's there and, in Random, in the first-open window):
  the count and the pill always show whole, the whole title is the
  header's tooltip, and screen readers hear it whole. Rows show the
  module's own name; a module several
  releases ship is listed under each of them. Only when one release has two
  builds under one name does the lane show: the Classic one reads "Bad Dog!
  (Classic)" (two alike of one lane: their file stems, "(BADDOG3)"). The
  count above the list reads "15 of 202" while filtered. A filter never
  changes the chosen single module: when it hides its release, no row is
  selected, Single still saves that module, and the details show a module
  the list does show instead (`details_after_filter` in `releases.h`: the
  one shown before while it is still listed, else the first row); clicking
  a row chooses it as usual, and **Show all** brings the chosen row back,
  selected and in the details. Random behaves the same way. A filter that
  leaves no rows (a release with no modules listed) shows "No modules to
  show" and greys Preview. Random's checks are kept for
  every release, shown or not; each group header's checkbox checks its
  release, and Select all, Clear and the rotation line work on the rows
  shown. Every module has the same rounded tile: the module file's own icon
  (pixel art scaled sharply to the same share of the tile at every DPI, on
  the icon's own backdrop colour when it has one, else night blue), else a
  thumbnail of it running (see below), else, until there is one, the night
  sky with the moon. A few Classic names the Windows 3.x control panel cut
  short are shown whole ("Strange Attractors", "Confetti Factory", "Slide
  Show", "OM Appliances"); ids and settings.ini are unchanged. The list opens
  at the top when the selected module shows there, else with it about
  mid-list, and never with a row or group header cut at the top edge (it is
  placed with `LVM_ENSUREVISIBLE`, which lands on a row's top exactly;
  `LVM_SCROLL` rounds to a "line" that is neither a row nor a header). In
  Random mode each row has a checkbox for the rotation, and each group header
  one for the whole group (a dash while it is mixed). From the keyboard the
  list's menu does the same (Shift+F10 or the Apps key on a row, or a
  right-click on a row or header: **Check all in Totally Twisted**, **Clear
  all in Totally Twisted**), and screen readers hear the group's state in its
  name ("Totally Twisted After Dark, 4 of 13 in rotation"; a group of one,
  "Marvel Comics Screen Posters, 1 in rotation"); under the list, the
  rotation line with **Select all** and **Clear** (each greyed while it would
  change nothing), and **Change module every**, which only Random uses.
  Under it, with two or more monitors, comes **A different module on each
  monitor** (`DifferentPerMonitor`, see **Settings**), its row taking 40 DIP
  from the list: with one monitor the row isn't there, and it comes and goes
  as a monitor is plugged in or out while the window is open. It is greyed,
  keeping its check, while Monitors is **Primary monitor only**, and OK
  saves it as it stands, hidden or greyed too. The
  rotation line counts the checked rows shown ("All 15 in rotation", "12 of
  15 in rotation"; a list of one module, "1 in rotation" or "1 selected · 0
  can run now", never "All 1"); when some of them are the same
  module on several releases, which plays once per pass, it says how many
  different ones rotate: "All 202 selected · 129 distinct" (its tooltip
  explains, and for copies of one module alone ends "…, so 1 module is in
  rotation."). When settings.ini names a module to play first in front of its
  Randomize list (see **Settings**), that row carries a **Plays first**
  badge in Random (screen readers hear "…, plays first"), and the rotation
  line's tooltip names it. In Single module mode the checklist is put aside
  (see below) and those rows go.
* The **details** of the selected module (never one the filter hides; see
  above): a **live preview** that runs it in
  its own `adhostwin.exe` at a real screen's size (an After Dark module at
  480 lines at the preview box's own 16:9 aspect — 856x480, or 848x480
  where the box's whole pixels come out a little taller — whatever the
  Resolution setting; a module with a screen of its own, an Intermission,
  Star Trek, ScreamSavers or Marvel module, at its 640x480, pillarboxed in
  the wide preview as on
  a widescreen monitor: see **Emulated screen**),
  shown scaled down, with the dialog's current,
  unsaved values; changing a value restarts it. Pointing at it shows the
  module's name along its foot. Under it the About text (tidied: the
  original's hard line breaks are rejoined; drawn with plain anti-aliasing)
  and credits. While more of the About is below, its last line and a half
  fade out (to 80%, never under high contrast) and a **More** link at its
  foot scrolls on by a page. Beside it the module's tile, name and controls,
  in a column at most 360 DIP wide (a wide window gives the room to the
  preview, up to 560 DIP). Under the name a chip with its release's short
  title ("Deluxe"; its tooltip names the other releases with the same bytes,
  "Also on: …"), and "Coming soon" or "File missing" when that applies. A
  name too long for the column is set in a smaller face, or on two lines
  with its chips under them. The controls
  scroll by whole rows, so none is ever cut: the first row below the fold
  shows faded, and the scroll thumb stays visible. Tab reaches every row and
  scrolls it in. A module button (for example Fish's "Select Fish…") is a
  real button when the host can open the module's own settings windows (see
  **Module buttons** below); otherwise it is a read-only row: its name, and
  "Not available in this version" with an info glyph; the reason is its
  tooltip. **Restore defaults** sits just under the
  last row when they all fit (at the column's foot, level with the credits'
  first line, when they scroll) and is enabled once a value differs from the
  catalog's default.
* **Resolution** and **Monitors**, each at the start of its half of the card
  (dropdowns at most 280 DIP wide; Resolution, 480 or 720 lines, is for the
  modules that follow the display alone: an Intermission, Star Trek,
  ScreamSavers or Marvel module always runs at its own 640×480, see
  **Emulated screen**), then across the card **Stretch to fit the screen
  (no black bars)** (`StretchToFit`: those 640×480 modules fill each monitor,
  stretched, in `/s` and the settings window's live preview, never in `/p`;
  modules that follow the display are unaffected), and under
  it **Sound** ("Primary
  monitor" / "Off") and **Volume** (a 0–100 slider with its value at the end
  of its label row; screen readers call it "Volume"; greyed, with its label,
  while Sound is Off), then the note "Sound plays from the primary monitor’s
  screen saver." (`docs/AUDIO.md` §9; see **Sound** below). The
  slider is adw_ui's `init_slider`: Right and Up raise it by 1, Left and
  Down lower it, Page Up / Page Down by 10, Home / End to 0 / 100.
* The footer: **Import…** with a line saying what is imported ("314
  modules from 14 releases", or "84 modules from After Dark 4.0 Deluxe"),
  then the credit, then **Preview** (full screen, of the module the details show; greyed for
  a module this host can't run yet, or whose file is missing, and while the
  details show none), **OK** and **Cancel**. The credit, "Made With Love by
  StarrLord", is one link to <https://github.com/starrlord/longafterdark>
  (`layout_footer_credit` in `ui_model.h`) in the dialog's link look (adw_ui's
  `ButtonRole::subtle`, as **Show all**: a fill under the pointer and a
  deeper one pressed, the focus ring round its box, a hand pointer), its text
  in the caption face of the assets line: "Made With Love by" in `text2`,
  "StarrLord" in the accent text colour, underlined under the pointer (the
  hover cue high contrast keeps, whose hover fill is the window colour). It
  sits in the free space between the assets line's text and Preview, centred
  there and on the buttons, and shows only when its whole box fits with
  24 DIP clear of both: beside "314 modules from 14 releases" (as long as
  the twelve releases' "284 modules from 12 releases") it fits the
  first-open window at every scale, and the narrowest one at 100% only (at
  the other scales it gives way there: the line is a digit longer than the
  seven releases' "232 modules from 7 releases", beside which it fitted at
  five of the seven scales), while an assets
  line too long for the room (files missing; one release's long title in the
  narrowest window) hides it, never clipped. If it had the focus, the
  keyboard moves on to the next control (Preview) as Tab would, and the
  default button with it, so Enter there runs Preview rather than OK; if
  it hid while the window was inactive, the same happens when the window is
  activated again (the dialog manager gives the focus back to it first). Tab
  reaches it after Import… and before Preview; a click, Enter or
  Space opens the page in the default browser (`ShellExecuteW` "open"); its
  tooltip is the address; screen readers hear "Made With Love by StarrLord"
  and, as its description (UI Automation's help text), "Opens
  https://github.com/starrlord/longafterdark in your browser". The assets
  line's window is only as wide as its text, so the two never overlap. The
  welcome's footer (no Import…) shows it too: it has more room there.

Until the modules are imported the details card is one welcome: a picture
across its top (the night sky, the moon and two flying toasters), "Welcome to
Long After Dark", what importing does (the original modules of After Dark and
Star Wars Screen Entertainment, from any of the fourteen releases' discs, an image,
or the Internet Archive; `welcome_text` in `ui_model.h`) and an **Import a
release…** button (the importer's window is "Import a release"). The
footer's Import is hidden meanwhile (it is the same command), its line reads
"Nothing imported yet" (the credit beside it as ever), Single/Random are greyed, and the list shows a few
faint placeholder rows and "Your modules appear here after import". "Long
After Dark" stays the product's name; the words for the releases fit all fourteen
(not every one is After Dark's, and Star Trek, the Simpsons, Marvel, Snoopy's
Screen Savers, the Looney Tunes (also on a CD), ScreamSavers, the Disney
Collection, The Far Side and Dilbert came on floppies, which "discs" covers).

**Thumbnails.** A module with no icon of its own is shown by a square of one
of its own frames: a third of the screen's height around the busiest part of
the picture (all of a small sprite, with a margin), at the crop's own size
(96 to 256 px), saved as `thumbs\<id>.v2.png` next to `settings.ini` (so
`%LOCALAPPDATA%\LongAfterDark\thumbs`; `AD_SCR_THUMBS` overrides it). Of
frames 45, 120, 240 and 400 the most detailed is kept, and only one worth
showing: some contrast (luminance standard deviation 12 or more), at least 3
colours (4 bits a channel, each on 0.2% of it), and not almost all one flat
tone (at most 95% within 10 of the mean): a blank, near-black or white
screen, a speck, a line of text, a two-tone blob or sparse line art (specks
at tile size) keeps the moon; a 16-colour sprite on black, a maze or a soft
pattern is kept. Two things take them:

* the live preview, of the module it shows (when it moves on early, the best
  frame so far);
* in the background, every module still without a picture: when the dialog
  opens with any missing, and after an import. One at a time, in the list's
  order, each in a host of its own on a 640x480 screen (the module's own
  rule at 4:3 and 480 lines, which is 640x480 for an After Dark, an
  Intermission and a Star Trek module alike; a catalog "screen" of another
  size would be that size) at idle priority for
  up to 12 seconds, never shown; paused while the full-screen Preview runs,
  stopped while an import runs, and never for a module this host can't run.
  `AD_SCR_THUMBGEN=0` turns this off.

The version in the name goes up when the way they are taken improves, so
older ones are taken again (and the old file is removed). Nothing else
writes there, and deleting the folder only brings the moons back until they
are taken again.

When the dialog opens it asks the host what it can do, without running any
module: `adhostwin.exe --capabilities` prints one line such as
`lanes=pe32,ne16 configure=pe32,ne16 abis=afterdark,intermission status=1 state=1 seed=1 audio=1 numlock=1`.
`lanes=` are the lanes built in; `abis=` the module ABIs it runs (a catalog
entry's `abi`: absent for After Dark's, `"intermission"` for Star Wars Screen
Entertainment's IMX modules, which are on lane `ne16` too); a host that
prints no `abis=` predates them and runs After Dark's alone. `numlock=1` is
a host that keeps a Num Lock toggle beside Caps Lock (`NUMLOCK` lines and
`ADNUMLOCK`, see **Ending the saver and playing**); the dialog itself sends
no input, so only the saver acts on it. No module's
preview or thumbnail starts until the answer is in (`dialog_support.h`,
`module_run`). The answer stands while the dialog is open: after an import
or a cover change the host is asked again only if it hasn't answered yet,
so across the new catalog the live preview and the background thumbnails
carry on and the module buttons stay live. A module whose lane or ABI the
host doesn't list is dimmed and marked **Coming soon**: its preview says so
instead of starting it, no thumbnail is taken of it, its buttons are
read-only rows, Preview is greyed, and its release's header gets a **Coming
soon** pill when all of its modules are. They stay in the list (and in the rotation when
checked, their checkboxes dimmed like the row), so they play as soon as a
host that runs them is installed; meanwhile the rotation line says how many
can run ("All 32 selected · 18 can run now"). The saver's Random leaves them
out when its rotation holds a module of another ABI than After Dark's
(**What Random leaves out**, under **How it runs a module**); otherwise such
a module is tried like any other, and skipped after three failed starts.

A host too old to answer is taken to run everything. The host exits 3 only
for a module whose lane isn't built in ("valid module whose lane is not
built into this adhostwin", `host/core/include/adw/core/host.h`), and a
module whose preview or thumbnail exits 3 before a frame becomes **Coming
soon** by itself (the dialog keeps a set of such ids until the next
catalog; its lane's and ABI's other modules are left as they are): that is
how a host too old to answer shows a lane it lacks, one module at a time. A
host that lacks a module's ABI fails the module with exit 1, as it does a
damaged module ("… couldn’t start"); only its `abis=` answer says why.

**Module buttons** (`docs/INTERACTION.md` §6.3). A module's own
buttons ("Select Fish…", "Custom", "Edit / Select", "Pictures", the Star Wars
modules' "Configure...", Star Trek's "Edit Custom..." and "Sounds.."…) open the module's own dialogs, as the original
control panels did. A button row is a real button when `--capabilities` lists
the module's lane under `configure=`, the host runs the module (its lane and
ABI, above) and the module file is there. Pressing it runs
`adhostwin --configure <module> --button <index> --owner <this window>`
(`CREATE_NO_WINDOW`, with `AD_ASSETS_DIR`, the dialog's current values, saved
or not, as `ADCVSET`, and `ADSTATE`): the module's dialogs are real windows
owned by the settings window. They open over it: the host lays the module's
640×480 screen over the settings window (inside that monitor's work area),
so a dialog that places itself on its screen, as Marvel's Saver.., Lunatic
Fringe's Keys... and the Star Wars modules' Configure... do, is centred on
the settings window rather than at the monitor's top left
(`docs/INTERACTION.md` §6.2, Placement). Meanwhile the settings window is
disabled (as the After Dark 3 control panel disabled itself), the live
preview paused, and only one button runs at a time. As a module's dialog
closes, the settings window takes the activation back (the host hands it
over; it is not left behind another application's window). When the host exits, whatever its exit
(a crash included), the window is enabled again and brought forward, the
live preview starts the module afresh (it reads what the module saved at its
start) and its thumbnail is taken again. Under the button: "Nothing to set
here" when the module showed nothing (exit 4), "Couldn’t open this option
(code N)" for 1, 3 or 5 (and a crash's code, in hex). After a run of Messages
4.0's **Custom** or Message Mayhem's **Edit Custom** while their Message popup
isn't on "Custom", the row says "Choose “Custom” under Message: to show it";
nothing is switched for you. What a module keeps itself is saved by the
module at once, so the dialog's **Cancel** does not undo it (true of the 1996
control panels too); the button's tooltip says so.

**Sound** (`docs/AUDIO.md` §9). The modules' sound is on by
default (`Sound=1`, `Volume=50`, After Dark's own default), and exactly one
host plays it: the one behind the **primary monitor's window** of a `/s` run
(the input owner, `App::owner()` in `saver.cc`). That host gets `ADSOUND=1`
and `ADVOLUME=<Volume>`; `ADVOLUME` is After Dark's volume slider, handed to
the modules (the host adds no gain of its own). Every other host is started
with `ADSOUND=0`, and `ADAUDIOOUT` and `ADVOLUME` removed, so nothing
inherited can turn its sound on: the other monitors' hosts, `/p` (the Control
Panel thumbnail), the dialog's live preview and its thumbnails, and
`--configure` / `--capabilities` (`sound.h`; `add_host_defaults` makes any
spawn that says nothing about `ADSOUND` silent). The rules:

* **Preview** is a `/s`, so it plays, with the dialog's current values,
  saved or not (they reach it through the preview's settings file). The live
  preview in the details card never does.
* **Rotation:** each of the primary window's hosts plays; the one it
  replaces is stopped first (so there is never a moment with two, and the
  log's `spawn … sound=1` / `sound host stops` lines pair up).
* **Monitors changing:** when the primary monitor changes, the window that
  was the primary's and still runs starts its module again without sound
  ("sound: window=N is no longer the primary monitor's"), and the new
  primary window's next host (its next rotation or respawn) plays.
* **Waking:** the sound host is sent `QUIT` first, before the windows are
  hidden or any other host hears it ("wake: QUIT to the sound host window=N
  first"), and every host that plays is stopped with a 400 ms grace instead
  of 150 ms (on waking, at a rotation, at a monitor change), so it can
  silence its device and send MIDI all-notes-off before it is terminated.
* `SoundMonitor=primary` is reserved: "primary" is the only value; another
  one (a later version's) is kept in the file and played as primary.
* `AD_SCR_SOUND=0` turns sound off for every host whatever the settings say
  ("sound: off (AD_SCR_SOUND=0)" in the log). The scr tests set it.
* A capture the saver is given (`ADAUDIOOUT` in its environment, with
  `ADAUDIOLIVE=0` to keep it off the device: `adw/core/audio.h`) reaches
  the sound host only.

**Module state.** The modules' own settings and data (MODULES.INI, the
Messages texts, high scores…) live in `state\` next to `settings.ini`, one
folder per package (`INTERACTION.md` §7). Every host the saver or the dialog
starts gets `ADSTATE=<that folder>` (`AD_SCR_STATE` overrides): `/s`, `/p`,
the live preview, thumbnails, `--configure`. Deleting a package's folder
there restores its modules' defaults.

## Settings (`settings.ini`)

```ini
[Saver]
Module=random            ; a catalog id, or random
Randomize=ad40.toasters,ad40.fish   ; Random's subset (empty = every module)
RandomizeSaved=ad40.fish ; the dialog's Random checklist kept while Module names one module ("-" = none checked)
DurationMin=5            ; Random switches module this often; 0 = never
Scale=1.0                ; 1.0 = 480-line emulated screen, 1.5 = 720-line (modules that follow the display; Intermission, Star Trek, ScreamSavers and Marvel modules: always 640x480)
Monitors=all             ; or primary (the other monitors stay black)
DifferentPerMonitor=0    ; 1: in a rotation each monitor follows its own order; 0 or missing: the same module on all, switching together
StretchToFit=0           ; 1: 640x480 modules (Intermission, catalog screen) fill each monitor instead of keeping 4:3 with bars; not in /p
StartFromDesktop=1       ; 0: /s starts every module on black (no desktop capture); no UI
Collections=simpsons,tt  ; the strip's filter: release ids; empty or missing = every release
Sound=1                  ; 0: no sound from any module (1, or on/yes/true, is the default)
Volume=50                ; 0..100, After Dark's volume slider (ADVOLUME)
SoundMonitor=primary     ; reserved: the primary monitor's screen saver plays

[Module.ad40.toasters]   ; control values by catalog index, sent as ADCVSET
0=50
```

The saver rotates modules when `Module=random` (or there is no `Module` key)
or when `Randomize` lists any modules. With both a named `Module` and a
`Randomize` list, the named module plays first and the list rotates after it.
A named `Module` with an empty `Randomize` shows just that module.

On several monitors a rotation plays the same module on all of them, and
they switch together: every window follows one shuffle bag with one clock
(`SharedRotation` in `settings.h`), which belong to the saver rather than to
a window, so a monitor plugged in while it runs starts on the module the
others play. While the primary monitor's module plays a game, the rotation
waits on every monitor; then they all switch, and the next module gets a
full interval. A module one monitor's host fails three times in a row is
skipped on every monitor (`skip window=N module=X after 3 failures, on
every monitor`), except while the primary monitor plays it as a game (`…
waits: the primary monitor's module is interactive`; the failing monitor
keeps trying) and when that monitor has failed every module in turn (`…:
not on the other monitors, every module failed here in turn`; it retries
every 30 s, and the others carry on). `DifferentPerMonitor=1` gives each
window a rotation and a clock of its own, seeded apart, so each monitor
goes through the modules in its own order, and only the primary's rotation
waits for a game. The last-exit log says which (`rotation: the same module
on every monitor, switching together`, or `…: a different module on each
monitor (DifferentPerMonitor=1)`). The dialog's OK writes the key, as `1`
or `0`, into a file that lacks it.

In Random the saver plays `Randomize` (or every module) limited to the
releases in `Collections` (`effective_rotation` in `releases.h`), and a
module that several selected releases ship byte for byte (catalog `sameAs`)
plays once per pass, as the first copy in catalog order. Ids of releases
that aren't imported are ignored (and stay in the file until the next OK);
all of them selected is the same as none. If nothing checked is left in the
selected releases (only a hand-edited file can do that), `Collections` gives
way and the log says `rotation: Collections ignored (nothing checked in
them)`. A `Module` leading the list still plays first, whatever the filter.
The dialog writes `Collections` on OK while the strip shows (empty when
nothing, or everything, is selected); with one release it leaves the key as
it was.

The dialog maps its two choices onto that (`apply_dialog_choice` in
`settings.h`):

* **Random** saves `Module=random` with the checked modules of every
  release, shown or not (all checked saves an empty list, so newly imported
  modules join in). If the file had a named `Module` leading its list, that
  `Module` is kept and the checked modules are always written out, since an
  empty list would turn it into a single module. Random with nothing checked
  in the releases shown is refused ("Check at least one module in the
  releases shown").
* **Single module** saves the chosen `Module` (the row last clicked, or the
  one the file named; still that one while a filter hides its row and the
  details show another) and an empty `Randomize`
  (a list would make the saver rotate). The checklist is kept in
  `RandomizeSaved=<id>,…` (only while it is not "all"; nothing checked is
  written `RandomizeSaved=-`, since an empty list means "all"), which the
  saver ignores and the dialog shows checked again next time, so switching
  to a single module and back loses nothing, not even a checklist left
  empty.

Control values are what the host receives: a slider's number, a popup's item
index, 0/1 for a checkbox. For a string slider (labelled stops such as
Never / Rarely / Often / Always) it is the chosen stop's value from the
catalog's `values` table. Module buttons (for example Critic's "Pictures")
carry no value: the module keeps what they set in its own files (see
**Module buttons** above). Where a
Classic string slider repeats a label on adjacent stops (the original
control panel appends a stop: "Never, Once, Twice, Always, Always, Always"),
the run is one stop on the slider. A stored value inside a run is shown as
that stop and kept as it is; choosing the stop stores the catalog's default
when it lies in the run, else the appended stop's value, else the run's
last. **Change module every**
offers 1 minute to 2 hours and Never (`DurationMin=0`); a value the file holds
that isn't one of those is offered as well, so it survives OK unchanged.

The dialog updates the file in place. Keys, sections and comments it doesn't
know about are left alone. `Sound`, `Volume` and `DifferentPerMonitor` are
read leniently (`on`, `off`, `075`; a volume outside 0–100 is clamped;
anything unreadable is the default) and a value that already says what OK
saves is left as written.

**Import…** starts `adimport.exe --gui` with `CREATE_NO_WINDOW` (it is a
console program; this keeps a console window from appearing behind its own
dialogs on Windows before 11 24H2) and reads its exit code as
`adw::import::Status`. 0 reloads the catalog, keeping the user's checks.
5 (cancelled) and 1–4 (failed; adimport has already said why) change
nothing, since imports are atomic, so the list stays as it is and the assets
line says so.

**Preview** runs `LongAfterDark.scr /s` on exactly what the dialog shows
(the module in the details, unsaved edits included), from a throwaway
`%TEMP%\LongAfterDark-preview-<dialog pid>.ini` passed as `AD_SETTINGS`
(with `AD_SCR_SETTINGS_IS_TEMP=1`). The preview deletes that file as soon as
it has read it, so nothing is left behind even when the dialog closes while
a preview runs. The dialog also deletes it if the preview fails to start or
ends, and when it opens it sweeps files left by dialogs that are no longer
running. Only files named that way are ever deleted.

## How it runs a module

* **Emulated screen:** by the module's own screen, from its catalog entry
  (`screen` when it has one, else its `abi`), one rule for every
  host the saver and the settings dialog start (`own_screen` and
  `module_screen` in `geometry.h`): the `/s` windows (each host of a
  rotation, which switches between the kinds freely), Preview (a `/s`), the
  dialog's live preview and its thumbnails. The host gets it through
  `ADSCREENW`/`ADSCREENH`, together with `ADSTREAM=1`, `ADCVSET` and
  `AD_ASSETS_DIR`, and the frame is letterboxed to keep the monitor's aspect.
  * Modules that follow the display (After Dark's, without `screen`):
    640×480 × `Scale` (the Resolution setting), widened
    to the monitor's aspect ratio. It is never narrower than 4:3, both axes
    are multiples of 8, and the width is capped at twice the 4:3 width.
  * Modules with a screen of their own: that screen on every monitor,
    whatever the Resolution setting. Intermission modules (Star Wars Screen
    Entertainment, `"abi": "intermission"`) have 640×480 by their ABI; a
    catalog entry gives any module one with `"screen": "WxH"` (`"640x480"`
    for Star Trek: The Screen Saver's, ScreamSavers' and Marvel Comics Screen
    Posters' modules, After Dark modules all), which comes first (a catalog
    without the field keeps the ABI's rule). Such modules compose fixed
    scenes: the Intermission modules centre theirs on a larger screen, and
    of the Star Trek modules The Mission draws its scene at the top left
    beside a grey band and Final Exam, Sickbay, Scotty's Files and Ship
    Panels sit small in the middle; so at 720 lines, say, the scene sat small
    with bars around it; at their own size the letterbox scales the frame to
    fit the monitor, keeping its shape: a 4:3 one at its full height on a
    monitor at least 4:3 wide (bars at the sides on a widescreen), at its
    full width on a narrower one (5:4, portrait: bars above and below).
    Their clicks and moves are mapped into that frame (Final Exam's mouse
    move included), and a game's cursor clip is that frame. With
    `StretchToFit=1` the frame is the whole monitor instead (`frame_rect`,
    `geometry.h`), out of its shape, and so are the mapping, the clip and the
    part of the desktop a seed takes (`seed_source`); the log's spawn line
    ends `stretch=1`. The catalog
    parser takes `screen` only as `<w>x<h>` with 1 to 5 decimal digits
    either side (either `x`; leading zeros count, so `000640x480` is none,
    and no axis can overflow), each axis 1..8192 and at most 4096×4096
    pixels in all, the frames the stream parser reads back; anything else is
    no screen of its own.
  * `/p` is 320×240 for every module: the host renders an output that small
    through a guest display of at least 640×480 (`host/ne16`, "Small
    screens"), which a 640×480 scene of a module's own fills.
  * The settings dialog's live preview runs After Dark modules at 480 lines
    at the preview box's own 16:9 aspect, 856×480 (848×480 where the box's
    whole pixels come out a little taller; it ignores the Resolution
    setting), modules with a screen of their own at that screen (640×480),
    pillarboxed, or filling the box while Stretch to fit is checked; its thumbnails take every module at 640×480 (a module's own
    screen of another size at that size).
* **Pacing:** the first `GO` goes out with the spawn (the host waits only
  250 ms for it before frame 0). After that a clock thread wakes on every
  display refresh (`DwmFlush`), or on a 60 Hz high-resolution timer where DWM
  can't pace it. On each tick it sends `GO`, but only once the previous frame
  has arrived and the window has taken it, so a host never renders frames
  nobody sees. An unanswered `GO` is repeated only if the stream parser had
  to throw away garbage in the meantime; a slow step or a slow module init is
  simply waited out. When a new frame is byte-identical to the one on screen,
  nothing is redrawn.
* **Display off:** when Windows powers the display off, no more `GO`s are
  sent, so the hosts sit idle until it comes back on.
* **Monitors changing:** a monitor can be plugged in, unplugged, switched to
  another mode or rearranged while `/s` runs, for example a dock, a
  projector, or a DisplayPort monitor dropping out in deep sleep. On
  `WM_DISPLAYCHANGE` the saver waits for the burst to settle (500 ms), then
  re-plans its windows against the monitors now present (`plan_relayout` in
  `geometry.h`). A window whose monitor is unchanged stays as it is. One whose
  monitor changed mode or position but kept its aspect ratio (so the host's
  emulated screen size is the same) is moved, and its host carries on; so is
  one whose host runs a module with a screen of its own (an Intermission,
  Star Trek, ScreamSavers or Marvel module), whose 640×480 is the same on
  every monitor (such
  windows go last, after the ones whose size matches a monitor, so neither
  kind restarts for the other). A
  monitor that is new, or whose aspect changed under an After Dark module,
  gets a new window and host, which in a rotation starts on the module the
  other windows play (unless `DifferentPerMonitor=1`). A window's next host
  (its rotation's next module, a respawn) takes its module's size on the
  monitor it is on then.
  Windows for monitors that are gone are closed along with their hosts. None
  of this counts as the user coming back: Windows moves the cursor off a
  monitor that goes away or changes mode, often by far more than the
  10-pixel threshold, so from the first `WM_DISPLAYCHANGE` until the
  re-plan a move only sets where the threshold counts from ("input: move
  while the display settles" in `AD_SCR_LOG`); after it, it counts from
  wherever the cursor then is.
* **Scaling** (`present.h`): each `/s` window draws through Direct2D. The
  frame is uploaded as a 32-bit bitmap (an 8-bit frame through its palette)
  and scaled on the GPU the way GDI's `HALFTONE` scales an upscale: every
  pixel of the frame a crisp block of even size, with a column or row
  blending two blocks only where they meet, by how much of each it covers
  (the frame repeated to the next whole multiple with nearest neighbour,
  then drawn to the window linearly, a shrink of less than one pixel per
  block; a downscale is filtered with high-quality cubic). That costs about
  1.7 ms per frame for 856×480 → 3840×2160, where GDI's `HALFTONE` took
  27 ms in software, so a 4K monitor gets the good filter too. The window
  never waits for the vertical blank itself (`D2D1_PRESENT_OPTIONS_IMMEDIATELY`:
  the pacer already paces). GDI's `StretchDIBits` is the fallback: when
  Direct2D can't make its render target or fails (logged as `present
  window=N: direct2d failed (…) -> gdi`), after a device lost more than
  three times in one window, when Direct2D averages over 8 ms a frame
  (a machine without a usable GPU), and for `/p`. GDI uses `HALFTONE` where
  it's affordable and switches a window whose upscales average more than
  8 ms to the hardware-accelerated `COLORONCOLOR` (nearest neighbour).
  Downscaling (the preview) always uses HALFTONE. While a window shows a
  message ("could not be started"), GDI draws it; the black between modules
  is Direct2D's.
  `AD_SCR_PRESENT=gdi` (or `d2d`, which also covers `/p`) picks the way;
  `AD_SCR_STRETCH=nearest` forces nearest neighbour either way and
  `AD_SCR_STRETCH=halftone` the smooth filter. `AD_SCR_LOG` says how each
  window presents (`present window=0: direct2d, …`) and what it cost
  (`stats … present_ms_avg=… present=d2d`).
* **Starting from the desktop:** before any `/s` window appears, each
  monitor that will run a host is captured (`BitBlt` with `CAPTUREBLT`) and
  shrunk with `HALFTONE` to its window's emulated size, written as a P6 to
  `%TEMP%`. That size is the first host's, which its module decides, and
  that module is drawn only when the rotation is built (after the host's
  answer when Random waits for it), so the window gets a picture for each
  screen its first module may have (`first_module_screens` in `releases.h`:
  each own screen once, `plan_seed_shots` in `geometry.h`), all from the one
  capture, and the first host takes the one of its own screen. For an After
  Dark module (one that follows the display) it is the whole monitor at its
  After Dark size, in `LongAfterDark-seed-<pid>-<window>.ppm`, as ever; for a
  module with a screen of its own (an Intermission, Star Trek, ScreamSavers
  or Marvel module), the part of the monitor its 640×480 frame will cover, shrunk to 640×480,
  in `…-<window>-640x480.ppm`, one picture for both (a screen of another size
  would have its own, `…-<window>-<W>x<H>.ppm`). A window that can only
  start with such a module (it chosen alone, or the only releases imported
  among Star Wars Screen Entertainment, Star Trek: The Screen Saver,
  ScreamSavers and Marvel Comics Screen Posters)
  gets just `…-<window>-640x480.ppm`, and one that can only start with an
  After Dark module just `…-<window>.ppm`. On a 4:3 monitor at 480 lines the
  two pictures are the same (the whole monitor at 640×480), so a window that
  may start with either kind gets just `…-<window>.ppm`, which a 640×480
  module of its own takes too. However many screens of their own a catalog
  gives its modules, a window gets three pictures at most (`plan_seed_shots`):
  the After Dark one, the 640×480 one and the smallest of the other sizes; a
  first host whose screen got none starts on black (`seed window=N: none
  taken at WxH` in the logs, after a line that counts the screens left out).
  The pictures are made and written one at a time, so only one is in memory
  beside the monitor's capture. Each file is
  created delete-on-close and kept open while the saver runs, so it
  disappears when the saver ends, however it ends. Only each window's first
  host gets one (`ADSEEDIMG`), so the module starts on the desktop as the
  1996 saver's did; respawns, rotations, `/p`, the live preview and
  thumbnails start black. `StartFromDesktop=0` turns this off. The capture
  never leaves those files and the hosts' memory.
* **Watchdog:** a host that exits, stops sending frames for 20 s, or sends
  no first frame within 90 s is restarted with backoff. After three failed
  starts without a frame, the window says "“Name” could not be started (host
  exit code N)" instead of staying black; the real host exits 3 for a module
  whose lane it doesn't have yet. When rotating, such a module is skipped (on
  every monitor, unless `DifferentPerMonitor=1`: see **Settings**), and
  if every module fails in turn, retries slow to one every 30 s. Every host
  runs in a kill-on-close Job (created inside it, so
  not even one being started can be left behind), so none can outlive the saver.
* **What Random leaves out:** a host too old for a module ABI (Star Wars
  Screen Entertainment's Intermission modules on a host without
  `abis=…intermission`) would run such a module into errors, a black screen
  three times over on every pass. So when the rotation holds a module of
  another ABI than After Dark's (`rotation_needs_capabilities` in
  `releases.h`), `/s` and `/p` ask the host first (`--capabilities`; the
  first hosts wait for the answer, 2 s at most, and without one the rotation
  keeps every module; a late answer is logged, and by default a monitor
  added later joins the rotation the others play, built before the answer,
  while with `DifferentPerMonitor=1` its window's own rotation goes by it),
  and Random leaves
  out every module whose lane or ABI the host doesn't list
  (`rotation_for_host`; the logs say "rotation: left out N module(s) this
  host can't run", counting what the rotation would have held: a list of
  two loses at most two); a Randomize list of only such modules gives way to
  every module the host can run, and a named Module leading the list is left
  out too. When the host can run none of the modules imported, nothing
  plays: the window says "None of the modules imported can run on this Long
  After Dark host (adhostwin.exe)." ("No module can run on this host" in
  `/p`) instead of starting them one after another into errors. The answer
  comes to the saver's thread, not to one of its windows, so a monitor
  change while it is awaited (a window retired, another made) can't lose
  it. Any other rotation never waits (`/s` only logs the answer): an After
  Dark rotation on a host without the Classic lane still tries the Classic
  modules (see **Watchdog**). A module chosen on its own is always tried
  (and says why it can't start).

## Environment overrides

| Variable | Effect |
|---|---|
| `AD_HOST_EXE` | host to run instead of `adhostwin.exe` next to the .scr |
| `AD_IMPORT_EXE` | importer instead of `adimport.exe` next to the .scr |
| `AD_ASSETS_DIR` | assets root; the catalog is `<root>\win\catalog-win.json` |
| `AD_SETTINGS` | settings file instead of `%LOCALAPPDATA%\LongAfterDark\settings.ini` |
| `AD_LOCALAPPDATA` | the folder the data folder `LongAfterDark` is in, instead of `%LOCALAPPDATA%` (host/core's `data_root.h`; an absolute path) |
| `AD_SCR_LOG` | append a diagnostic log: spawns, respawns, rotations, present timing |
| `AD_SCR_HOSTLOG` | append the hosts' stderr to this file (it is discarded otherwise) |
| `AD_SCR_STATE` | the modules' state folder passed to every host as `ADSTATE` (default: `state` next to the settings file) |
| `AD_SCR_LASTLOG` | the last-exit log (default: `logs\saver-last.log` next to the settings file) |
| `AD_SCR_STRETCH` | `halftone` or `nearest`; `auto` is the default |
| `AD_SCR_PRESENT` | `gdi`: draw with GDI's `StretchDIBits` only; `d2d`: Direct2D for `/p` too, and never given up for its cost (only when it fails); the default is Direct2D for `/s` with GDI as the fallback (see **Scaling**) |
| `AD_SCR_NO_DWM` | pace with the 60 Hz timer only |
| `AD_SCR_THUMBS` | where the settings dialog keeps module thumbnails (default: `thumbs` next to the settings file) |
| `AD_SCR_THUMBGEN` | `0`: the settings dialog takes no thumbnails in the background (the live preview still takes its own) |
| `AD_SCR_SOUND` | `0` (or `off`, `no`, `false`): no host makes sound, whatever `settings.ini` says (every scr test sets it). Anything else leaves it to the settings |

These are test hooks, used by the smoke tests. They are compiled only into
**`LongAfterDark-test.scr`**, which the build makes next to
`LongAfterDark.scr` from the same sources with `AD_SCR_TEST_HOOKS=1`
(`src/test_hooks.h`); it is never packaged. The `LongAfterDark.scr` that
`package.sh` stages, and that users install, never reads any of them, so a
variable left in an environment can't keep the full-screen saver from
ending, stand in for the monitors or the input, or make it write files
(`scr_resources` checks that the file has none of their names). To run a
hook against a real build, use the build tree's `LongAfterDark-test.scr`
with `AD_HOST_EXE` pointing at its `adhostwin.exe`.

| Variable | Effect |
|---|---|
| `AD_SCR_TESTEXIT_AFTER_FRAMES=N` | exit 0 after every host window has presented N frames. If the "not imported" or "host missing" message was shown instead, exit 10 or 11; if a module "could not be started", exit 12; if the host can run none of the modules imported (**What Random leaves out**), exit 13. |
| `AD_SCR_TEST_IGNORE_INPUT` | input doesn't end the run |
| `AD_SCR_TEST_INPUT=<file>` | drive the input rules with a script instead of real input (which is then ignored), with synthetic Caps Lock and Num Lock toggles (both off at the start) so a test never touches the real ones: `WAIT <ms>`, `FRAMES <n>` (the owner shows n more frames), `KEY <vk> <0\|1>` (`KEY 20 1` flips the synthetic Caps Lock, `KEY 144 1` the synthetic Num Lock), `SYSKEY <vk> <0\|1>`, `CAPSSTATE <0\|1>`, `NUMLOCKSTATE <0\|1>` (set without a key: the saver notices within 250 ms), `BUTTON <1\|2\|4> <0\|1>`, `WHEEL`, `MOVE <dx> <dy>` (the synthetic cursor starts mid-owner), `DEACTIVATE`, `DISPLAYCHANGE` (`WM_DISPLAYCHANGE` to the owner window), `CLIPLOG` / `STATUSLOG` (log the cursor clip / the owner's status record), `LOG <text>`; `#` comments (`input_rules.h`) |
| `AD_SCR_TEST_ROTATE_MS` | rotation interval in ms, used instead of `DurationMin` |
| `AD_SCR_TEST_SEED` | the rotations' seed (0 to 4294967295) instead of one from the tick count and the process id, so a test knows the order: every window follows a `Rotation` with that seed, or with `DifferentPerMonitor=1` window N's own with the seed + N × 7919 |
| `AD_SCR_TEST_STALL_MS`, `AD_SCR_TEST_FIRSTFRAME_MS` | watchdog timeouts |
| `AD_SCR_TEST_DISPLAY_OFF_MS` | after 5 frames, behave as if the display powered off for this long |
| `AD_SCR_TEST_DISPLAY_ON` | take the console display to be on whatever Windows reports (`display state 0 taken as on` in `AD_SCR_LOG`): with the monitors asleep the saver pauses every host, and no `/s` smoke test would see a frame. Set it in the environment that runs the tests (the smoke tests pass it on); `AD_SCR_TEST_DISPLAY_OFF_MS` still simulates a power-off |
| `AD_SCR_TEST_CAPTURE=<dir>` | each window writes what it shows when it has presented the frames in `AD_SCR_TEST_CAPTURE_FRAMES=<k>,…` (default 30): `window<N>-frame<K>.png` at its client size (the frame scaled into its letterbox as the window drew it, Direct2D or GDI, re-drawn off screen by `render_frame_bgr`) and `window<N>-frame<K>-host.png` (the host's frame as it came); `AD_SCR_LOG` gets a `capture window=N frame=K ok …` line for each |
| `AD_SCR_TEST_MONITORS` | `x,y,w,h[,p];…` monitors to use instead of the real ones (`,p` marks the primary). `\|` separates the layouts reported after each successive display change (`parse_staged_monitors` in `geometry.h`). The settings dialog counts them too, for **A different module on each monitor**, taking the next layout at each `WM_DISPLAYCHANGE` it gets |
| `AD_SCR_TEST_OPEN_LOG=<file>` | the settings dialog's credit link appends `open<TAB><url>` here. The test build never opens a page itself, whatever its environment (it logs `dialog: open <url> (the test build opens nothing)`); only `LongAfterDark.scr` calls `ShellExecuteW` |
| `AD_SCR_TEST_SCREENSHOT=<png>` | `/c` renders the settings dialog to this PNG and exits (0, or 1 if it couldn't). The window is created hidden, parked off every monitor and cloaked, never activated or focused (`WS_EX_NOACTIVATE`: no keystroke meant for another window can reach it), and drawn with `PrintWindow`: it never appears on screen. |
| `AD_SCR_TEST_SCREENSHOT_STATE` | `key=value;…` for the screenshot: `theme=light\|dark\|hc`, `module=<id>`, `mode=single\|random`, `dpi=<n>` (lay out at that DPI), `dpichange=<n>` (send `WM_DPICHANGED` as if dragged to such a monitor), `size=<w>x<h>` (client, DIPs), `focus=list\|slider\|ok\|single\|random\|duration\|preview\|strip\|sound\|volume\|credit\|permonitor` (draw that control's focus ring; `strip`: the first selected cover, else the first, scrolled into view), `sound=off` (the Sound dropdown at Off: Volume greyed), `volume=<0..100>`, `monitors=primary` (the Monitors dropdown at Primary monitor only), `different=1\|0` (**A different module on each monitor** checked or not), `wait=<ms>` and `frames=<n>` (how long to let the live preview run), `hover=preview` (the pointer over the live preview), `hover=strip:<id>` (that release's cover hovered), `hover=credit` and `pressed=credit` (the footer's credit under the pointer, and held down), `collections=<id>,…` (the strip's filter, as if those covers had been clicked), `thumbgen=1\|wait` (take missing thumbnails in the background; `wait`: until all are taken, within `wait`), `report=<file>` (write where the list shows in the picture, the card's colour and whether anything straddles the list's top edge; `strip=x,y,w,h` where the strip's tiles area shows, `strip_mode=regular\|compact\|hidden`, the base colour and how many rows the list shows; the strip's scroll position, `strip_first=`, its last stop, `strip_max_first=`, and how many covers a stop shows, `strip_slots=`, each cover's window, `tile<i>=x,y,w,h` (or `hidden` while it lies outside the strip, not shown), the chevrons, `chevron_left=` and `chevron_right=`, and the status line, `strip_status=`; each group's accessible name, `group<g>=`, and its title as the header drew it, `drawn<g>=` (whole, or ellipsized); what the host said, `caps=`, the modules "Coming soon", `soon=`, and the module the details show, `details=`, with its chip, `badge=`, whether its buttons are live, `button_live=`, and Preview enabled, `preview_enabled=`; the footer's credit, `credit=x,y,w,h` (its link's box, or `hidden`), `credit_lead=` and `credit_name=` (its two texts), with the assets line's text, `assets_text=`, and Preview, `preview_button=`; the monitors the dialog counts, `monitors=`, and **A different module on each monitor**: where it shows, `per_monitor=x,y,w,h` (or `hidden`), whether it is enabled and checked, `per_monitor_enabled=` and `per_monitor_checked=`, and whether its text fits whole beside its box, `per_monitor_fits=`, with **Change module every**'s dropdown, `duration=`, and the list's card, `list_card=`; all in the picture's pixels). A `size=` taller or wider than this machine's screen is honoured (the window's maximum tracking size is lifted off screen), so the regular strip can be captured at 150% and up. With `theme=hc`, `AD_UI_TEST_HC_SCHEME=nightsky\|aquatic\|desert\|dusk` stands one of Windows 11's contrast themes in for the system colours (the hook passes it to adw_ui's `set_test_hc_scheme`; the library itself reads no environment) |

## Build and test

```sh
AD_BUILD_DIR=build/win-scr AD_COMPONENTS="host/core;host/loader;scr" bash tools/build.sh
```

This builds `LongAfterDark.scr` (the screen saver that ships),
`LongAfterDark-test.scr` (the same with the test hooks, which the smoke tests
run) and runs the tests. With `host/loader` in the
build the dialog shows each module's own icon; without it, thumbnails and the moon.
The dialog's look (palettes, fonts, the custom-drawn controls, the header
band, cover drawing and the off-screen capture) is `adw_ui`
(`common/ui`, `docs/COVERS.md` §3), which `adimport.exe`'s windows
share; a build whose `AD_COMPONENTS` leaves `common/ui` out pulls it in.
The `.scr` reads host status records through core's header-only
`adw/core/status.h`, and finds the data folder with
`adw/core/data_root.h` (no core library is linked); with `host/core` in the
build, the opt-in real-module tests below also get `adhostwin.exe`.

No test touches the user's data folder: `scr_unit` and `scr_smoke` point
`AD_LOCALAPPDATA` at a scratch folder for themselves and everything they
start, and the `catalog` suite's look at this machine's generated catalog
only reads it.

* **Unit tests:** `scr_unit_*` cover argument parsing, the P8/P6 stream parser
  (including headers claiming frames past 8192 on an axis or 4096×4096 in
  all, which are resynced past rather than waited on), the settings.ini
  round-trip (`DifferentPerMonitor` too: missing is 0, read leniently,
  written after `Monitors` in a new file, a value already saying so left as
  written, the dialog's two choices leaving it alone) and the dialog's
  Random / single-module rules, the catalog
  (including the generated catalog's string sliders, units and buttons, the
  module ABI, `abi` (absent, empty or not a string is `afterdark`), a
  module's own screen, `screen` (`"640x480"`, either `x`; absent, not a
  string, not `<w>x<h>` with 1 to 5 decimal digits either side (six or
  more are none whatever their value, `000640x480`, so an axis past what an
  `int` holds, `4294967936x480`, never wraps round to a size), an axis
  outside 1..8192 or more than 4096×4096 pixels is none; the catalog's size
  before the ABI's), the six-release fixture's 14 Intermission modules, the
  seven-release fixture's four Star Trek modules (After Dark's ABI, lane
  `ne16`, 640x480 each), the twelve-release fixture's ScreamSavers and
  Marvel modules (the same, as a ScreamSavers module's entry will be) and
  the other new releases' without a screen, and a check of this machine's
  generated catalog when there is one), geometry (with each module's emulated screen,
  `module_screen` over `own_screen`: an After Dark module's exactly as
  before, an Intermission module's 640x480 on every display at every
  Resolution setting and a Star Trek module's catalog 640x480 the same, a
  screen of another size kept as given, its frame scaled to fit (full
  height beside bars on a widescreen, full width between bars on a 5:4 or
  portrait monitor); the part of a monitor each kind's desktop seed is
  taken from, `seed_source`;
  and a window's seed pictures, `plan_seed_shots`: one for each screen its
  module may have, After Dark's first, at 16:9 and 720 lines, on
  4:3 at 480 lines (where the 640x480 one shares the first's file) and
  at 720, 5:4 and portrait, for either kind alone, a Star Trek and an
  Intermission module's one picture, and one of another size its own;
  three at most among 70 screens in any order: 640x480 before smaller
  ones, then the smallest, the rest counted as left out, and one that
  follows the display, the first given; and the monitors
  `AD_SCR_TEST_MONITORS` stages, `parse_staged_monitors`),
  window re-planning on monitor changes (`layout`: also a window running an
  Intermission or a Star Trek module kept whatever the new aspect, and
  matched after the windows of a monitor's own size, so neither kind
  restarts for the other), rotation (also between an After Dark module and
  an Intermission or a Star Trek one, host by host: each its module's size,
  the one of its own kept through a monitor change and the After Dark one
  replaced; and the one every monitor follows, `SharedRotation`: a
  `Rotation`'s order with the same seed, one step per move, the wait for
  the primary monitor's game, the skip on every monitor and its two
  exceptions, a named module first),
  frame conversion, the environment block (with `ADNUMLOCK`: the
  toggle for a host that keeps one or hasn't answered yet, none for one
  that answered without `numlock=1`, whatever is inherited), the dialog's helpers (`dialog`:
  adimport's exit codes, the preview-file names and sweep, and starting
  `fakeimport.exe` without a console window), and its presentation (`ui`:
  About tidying (including the catalog's own mid-phrase breaks, and credits
  and verse left alone), the duration choices and summary lines, the window
  layout at 100/125/150/200% and several sizes (inside the window, nothing
  overlapping, on the 4-DIP grid, 200% = 100% doubled, the large-window
  caps, the content column capped and centred, "Change module every" under
  the list in Random only, and under it "A different module on each
  monitor" (`per_monitor_choice`: in Random with two or more monitors,
  greyed under Primary monitor only; its row taking 40 DIP from the list alone,
  at 100–200%, five sizes), the two-line title, the links' text on the card
  edge; the footer's credit, `layout_footer_credit`, measured in the real caption
  face at 100–250% beside the assets line's texts: whenever it shows, clear
  of the assets line's text and of Preview by 24 DIP, centred between them
  and on the buttons, the phrase on one line inside its box; hidden only
  without room, which one release's long title or the assets line at its
  longest leaves none of in the narrowest window; shown at the first-open
  size with fourteen releases, "314 modules from 14 releases"), the settings panel's rows (whole-row extents, read-only and
  unlabelled rows), string-slider stops with repeated labels (and
  `boldStop`), the thumbnail crop and quality gate, the names shown whole
  (every package's copy, `moduleName`-keyed, suffix kept), `--capabilities`
  parsing (`abis=`: absent is `afterdark` alone, `abis=` alone is none; a
  host that didn't answer runs everything; `numlock=1`, and only that, is a
  host that keeps the Num Lock toggle: `NUMLOCK` lines only to one that said
  so, `ADNUMLOCK` to every host but one that answered without it) and the
  probe against `fakehost.exe` with and without `abis=` and `numlock=1`, what the dialog does with a module
  (`module_run`: waiting while the host is asked, "Coming soon" for a lane or
  ABI it doesn't list or for that module's own exit 3, which never spreads to
  the rest of its lane or ABI), the group header's layout (`ellipsize`,
  `layout_group_header`: the count and the pill always whole), the button
  notes, and live button rows), the releases (`releases`, COVERS.md §1: parsing
  `packages[]` with every cover origin and the old-catalog fallback;
  `Collections` round trip and normalization, and the key left as written;
  `effective_rotation`'s filter, `sameAs` dedupe, empty fallback and lead;
  the list by release with "(Classic)" and file stems, counts and "Also on";
  what the details show after a filter change (`details_after_filter`);
  the strip's words and the assets line; `layout_strip` at every scale
  100–250% with 1 to 12 releases, both forms, seven widths, every scroll
  stop: whole tiles only, as many at every stop (all of them, or the slots
  between the chevrons), none of them, focus ring included, under a chevron
  or outside the strip, the first one shown at the area's edge, or in a row
  that scrolls just past the left chevron's zone at every stop, the chevrons
  beside the tiles and each at one place across the stops, its place empty
  where it hides (the left one's unscrolled, the right one's at the last
  stop), every step one pitch, the 4-DIP grid
  and 200% = 100% doubled; `layout_window` with the strip:
  compact under 760 DIP, the status box clear of the tiles, and the columns
  keeping today's heights; the captions' shrink rule, and every registry
  short title ("Star Wars", "Star Trek", "Looney Tunes" and "ScreamSavers" included) measured in the real caption face at
  100–250%, each fitting whole; six releases (`tests/fixtures/catalog-six.json`:
  the order, the Star Wars group, what Random plays on a host that can't run
  their modules (none of them, a lead of theirs left out, a list of only
  theirs giving way), `rotation_needs_capabilities`, `rotation_for_host`
  (what the saver plays with the host's answer, how many it leaves out,
  counted against the rotation and not the catalog, and nothing to play
  when theirs alone are imported), `first_module_screens` (which screens
  a window's first module may have, for its desktop seeds), and "Star Wars Screen
  Entertainment" in the narrowest list at 100–250%, measured in the real
  faces: its count and pill whole, the After Dark titles never ellipsized
  without a pill); seven releases (`tests/fixtures/catalog-seven.json`: Star
  Trek: The Screen Saver first, its group, "36 modules from 7 releases",
  its modules in Random without waiting for the host (After Dark's ABI),
  their 640x480, `first_module_screens` with them (theirs and Star Wars'
  one screen, one seed picture), and seven covers: whole in the first-open
  window and in the smallest one (compact covers) at 100–250%, scrolling in
  a window as narrow but 760 DIP or more tall: five tiles at each stop, the
  last stop the third); twelve releases (`tests/fixtures/catalog-twelve.json`:
  the seven and Marvel Comics Screen Posters, Snoopy's Screen Savers, The
  Looney Tunes Screen Saver, ScreamSavers and The Disney Collection Screen
  Saver among them by date, their groups, "46 modules from 12 releases",
  "Showing 11 of 12 releases", Random over them without waiting for the
  host, ScreamSavers' and Marvel's modules at their catalog 640x480 on every
  display at every Resolution setting and the other new releases' following
  the display, `first_module_screens` and a monitor's seed pictures with
  them; twelve covers at 100–250%: regular ones never all side by side, the
  first-open window seven at a time over six stops, one as narrow but 760
  DIP or more tall five over eight, a large one nine over four; compact ones
  eight over five in the smallest window, nine from 960 DIP wide, ten from
  1032 (the first-open width, its height clamped under 760 DIP), eleven
  from 1104, and all twelve side by side from 1120, left-aligned, with no
  chevron; each stop clear of the status box; and
  every release's title in the group header of the narrowest and the
  first-open list, measured in the real faces: the count and the pill always
  whole, and without a pill only the long titles ellipsized, never the
  After Dark ones or the short ones, and none in the first-open list in
  Single);
  a catalog whose 66 modules each give a screen of their own, all one
  file: every screen a first module may have, three seed pictures (a
  window's After Dark one, 640x480, the smallest other), a lead of the
  largest left out, and one chosen alone its own), the input rules (`input`: the whole `decide()` table, exempt
  keys, holds and their 300 ms limit, key-filter verdicts, Alt, the wheel,
  move thresholds, a host dying mid-hold; `MOUSE` mapping into the
  letterboxed frame; the `NUMLOCK` line; the `AD_SCR_TEST_INPUT` grammar,
  `NUMLOCKSTATE` and the synthetic toggles included) and `seed` (P6
  encoding, the delete-on-close seed file readable only with share-delete and
  gone with its handle, the name of the one taken for a module's own
  screen, a real capture, the pictures handed over one by one in order,
  each made only once the one before was taken (a picture repainted while
  the first is taken shows in the second), a shot without a size and a
  part off the monitor none, and a capture that can't be made taking every
  shot without a picture, the last-exit log's cap and
  rewrite, the state/log paths, `StartFromDesktop`), and `paths` (the data
  folder, each case on a scratch `AD_LOCALAPPDATA`: every default location
  in `LongAfterDark` and what it holds found; nothing made until the first
  save; nothing made there with `AD_SETTINGS` and `AD_ASSETS_DIR` set,
  whatever is used; the base trimmed with a trailing separator tolerated, a
  blank `AD_LOCALAPPDATA` giving way to `LOCALAPPDATA`, and the known folder
  when neither is set). `sound` (`AUDIO.md` §9): `Sound`,
  `Volume` and `SoundMonitor` read as written or leniently (on/off/yes/no,
  clamped, unreadable = default, the last duplicate wins), their defaults
  written into a file that predates them with its unknown keys kept, round
  trips from scratch and onto a file, a value already saying the same left as
  written, and the dialog's choices never touching them; who gets sound
  (`sound_for`: only `/s`'s primary window's host with `Sound=1`, never the
  other monitors', `/p`, the live preview, thumbnails or tools, and never
  under `AD_SCR_SOUND=0`); the spawn environment (`ADSOUND=1 ADVOLUME=<v>`
  with an inherited capture kept, or `ADSOUND=0` with `ADAUDIOOUT` and
  `ADVOLUME` removed, through the real environment block with hostile
  inherited values); `AD_SCR_SOUND`'s spellings; the stop graces. The
  `layout` checks (`ui`) include the Sound row: inside the options card, in
  the Resolution/Monitors columns, the readout at the slider's end, the note
  under both. `present` (`present.h`): the palette expansion, and both
  ways' filters off screen (Direct2D on its software rasterizer, GDI): at
  4.5 times, nearest leaves a hard edge while the smooth filter blends the
  one column where two pixels meet (by half, on Direct2D), an exact
  multiple is crisp either way, 1:1 is exact, and the bars are black.
* **`scr_resources`:** what Windows reads from the file, without a window:
  the name `LongAfterDark.scr`, string 1 "Long After Dark", the version
  resource (product "Long After Dark", `OriginalFilename`
  `LongAfterDark.scr`) and the manifest's identity, in both programs; and
  that `LongAfterDark.scr` holds none of the test hooks' names (UTF-16 or
  plain) while `LongAfterDark-test.scr` holds them all.
* **`scr_host_stream`:** runs the host plumbing against `fakehost.exe` with no
  windows involved, including the stop graces: a host that takes 220 ms after
  `QUIT` (`FAKEHOST_QUIT_DELAY_MS`) ends on its own when it was started with
  `ADSOUND=1` and is terminated otherwise.
* **`scr_smoke_*`:** run the real `.scr` against `fakehost.exe`, a stand-in host
  that speaks the protocol with a palette-cycling test pattern (it logs its
  parent's pid, so a test can tell the dialog's own hosts from the saver's,
  and `FAKEHOST_LANES=pe32` makes it exit 3 for a module in a `CLASSIC`
  folder, as a host without the Classic lane does; `FAKEHOST_ABIS` sets its
  `abis=`, today's host's `afterdark,intermission` by default, `none` leaving
  the key out as an older host does, which fails an `.IMX` module with exit
  1; `FAKEHOST_EXIT3_MODULE=<text>` makes one module exit 3;
  `FAKEHOST_CAPS_DELAY_MS` makes `--capabilities` answer late;
  `FAKEHOST_NUMLOCK=0` leaves `numlock=1` out, as a host from before the Num
  Lock toggle, to which a `NUMLOCK` line is a line it doesn't know). Like the real host it
  numbers `KEY`/`CAPS`/`NUMLOCK`/`MOUSE` lines and logs them, ignores a line
  it doesn't know without numbering it (logged as `unknown`), goes interactive on
  `CAPS 1` (eating keys and clicks while it is), publishes the status record
  through `ADSTATUSHANDLE`, checks `ADSEEDIMG` the way a host opens it,
  answers `--capabilities` and fakes `--configure` (logging its owner, whether
  that owner is disabled, and its environment; `FAKEHOST_CONFIGURE_EXIT` or
  `_EXIT_FILE` pick the exit, `crash` included). Its start lines carry
  `ADSTATE`, `ADCAPS`, `ADNUMLOCK`, `ADSEEDIMG`, `ADSTATUSHANDLE`, and `ADSOUND`,
  `ADVOLUME` and `ADAUDIOOUT` (fakehost makes no sound), and its exit lines
  the Num Lock toggle it ended with. They use the
  fixture catalog and settings under `tests/fixtures`. The placeholder module
  files are created at test time and contain no After Dark bytes. Covered:
  `/s` on every monitor or the primary only, `/p` in a hidden parent (and
  exiting when it is destroyed), the not-imported and host-missing messages,
  respawn after an exit or a stall, rotation (including a named module that
  leads a Randomize list), a module that never starts, the display-off pause,
  P6 with garbage between frames, monitors changing under `/s`
  (`display-change`, staged with `AD_SCR_TEST_MONITORS`), and the settings
  dialog driven by control ID: `config` (controls, and the checklist kept
  through a single-module session), `config-lead` (a leading Module survives
  OK, and Random with nothing checked is refused), `config-live` (the live
  preview runs the selected module at a real screen's size with the
  dialog's values, restarts when one changes, and leaves no host behind),
  `config-classic` (a host without the Classic lane: one probe, the module
  shown as coming soon and never started, not even for a thumbnail;
  its chip names the release, never the lane), `config-abi` (six off-screen
  renders with `tests/fixtures/catalog-six.json`: on today's host the Star
  Wars modules run, with a live Configure...; on a host without `abis=` they
  are coming soon, never started, not even for a thumbnail, with their button
  read-only and Preview greyed; one module's exit 3, from its preview or from
  its thumbnail, makes that module alone coming soon; at the narrowest window
  in Random the long title is drawn ellipsized, with and without its pill,
  and the others whole),
  `config-thumbs` (the background thumbnails: one for every module it can
  run, each in a host of its own at 640x480, none for a missing file, no host
  left behind), `list-top` (72 off-screen renders of the dialog, with a
  catalog shaped like the real one, across 100–200%, three window sizes, both
  modes and four selected modules: nothing straddles the list's top edge,
  checked on the picture's pixels; 24 of them with four releases, so with the
  strip, regular and compact, in light, dark and high contrast, some under a
  filter), `config-collections` (the strip driven by control ID against
  `tests/fixtures/catalog-releases.json`, the real catalog's shape with
  placeholder names and cover tiles drawn at test time: the list regroups,
  Random with nothing checked in the releases shown is refused, OK writes
  `Collections` and a `Randomize` of every release's checks, the next dialog
  restores the filter, a filter keeps the chosen single module while the
  details show the first listed one, and with one release the strip is
  hidden and the key left as written), `config-details` (the details follow
  the filter in Single and Random and never show a hidden module; a filter
  alone never changes the saved module, a click on a row does; **Show all**
  brings the chosen row back; a release with no modules shows "No modules to
  show" with Preview greyed; the caption is "Long After Dark"),
  `data-root` (with only a scratch `AD_LOCALAPPDATA`: `/s` runs from
  `LongAfterDark`, hosts and last-exit log included; the dialog finds
  everything there and saves back to it; nothing else is made in the
  base), `config-cover`
  ("Change cover…" from a cover's own menu against `fakeimport.exe`: its
  arguments, no console window, Import… and the item greyed while it runs,
  exit 0 reloads the catalog keeping the filter, exit 5 doesn't; across the
  reload the host isn't asked again and a pe32 module's live preview runs
  on in the same host),
  `rotate-collections` (`/s` with `Collections`: only those releases play,
  and a byte-identical copy once), `rotate-monitors` (four `/s` runs on two
  monitors staged off every real one, the order fixed by
  `AD_SCR_TEST_SEED`: both windows start each module of the seeded bag
  together; a module fakehost can't start is skipped on both, once, and both
  start the next one at once; a monitor plugged in joins on the module the
  other plays, then switches with it; with `DifferentPerMonitor=1` each
  follows a rotation of its own), `config-monitors` (the dialog driven by
  control ID on staged monitors: with two and a file without the key, **A
  different module on each monitor** shows under **Change module every**,
  enabled and unchecked, greyed keeping its check under Primary monitor
  only, hidden in Single, and OK writes `DifferentPerMonitor=1` keeping the
  rest; the next dialog shows it checked, and Cancel writes nothing; with one
  monitor it isn't there and OK keeps the file's value, and a monitor
  plugged in brings it; then seven off-screen renders, light and dark at
  100% and 150%, greyed, focused, the smallest window and one monitor: under
  **Change module every** and clear of it and of the list's card, its text
  whole beside its box, and with one monitor the list keeps that room),
  `rotate-abi` (`/s` on one monitor staged
  off every real one, and `/p`, with the six releases: on a host without
  `abis=` no Intermission module is started, "left out 14", even for a list
  of only theirs ("left out 2"), and a list of one of theirs and one After
  Dark module plays that one alone ("left out 1"); with theirs alone
  imported nothing starts and the window says why, `/s` and `/p`; on today's
  host they rotate), `rotate-abi-wait` (the host answering late,
  `FAKEHOST_CAPS_DELAY_MS`: past the 2 s wait the first host starts when it
  runs out, the rotation keeps theirs and the late answer is only logged;
  within it, a monitor changing mode retires the window that was up when the
  saver asked, and the answer still reaches the new one, which leaves theirs
  out), `screen-abi` (an Intermission module's hosts are asked for 640x480
  at the 720-line setting, where an After Dark module's get 1280x720: `/s`
  on a 16:9 monitor staged off every real one rotating between the two, the
  size following each switch and the first host started on the desktop
  captured at its own size where the desktop can be read back; the window
  showing the 640x480 frame at the monitor's full height, bars at the sides
  only, in `AD_SCR_TEST_CAPTURE`'s picture; a monitor turning 4:3 moves the
  Intermission module's window with its host and replaces the After Dark
  module's; `/p` at 320x240; the dialog's live preview and thumbnails at
  640x480, and Preview's `/s` too), `screen-field` (the same for a module
  whose catalog entry gives `"screen": "640x480"`, with
  `tests/fixtures/catalog-seven.json`: a Star Trek module's hosts at 640x480
  at the 720-line setting, rotating with an After Dark module's 1280x720
  (two seed pictures planned, where the ABIs alone gave one: each planned
  picture logs a line, taken or not, so this holds on a desktop that can't
  be read back too) and with a Star Wars module both 640x480; The Mission's frame at the
  monitor's full height, bars at the sides only; a monitor turning 4:3 moves
  its window with its host; `/p` at 320x240; the dialog's live preview at
  640x480, a thumbnail at the module's own size (800x600 in the test's copy
  of the catalog, which a 4:3 capture would not give), and Preview's `/s`
  at 640x480; and with the dialog open on Final Exam from a catalog without
  the field, previewed at 856x480, an import that only adds its `"screen"`
  replaces that host with one at 640x480, the live preview's target
  comparing the screen too), `seed-screens` (a catalog whose modules each
  give a screen of their own, eleven sizes beside After Dark's, a module of
  a size left out leading a list of all the others: three pictures planned,
  each logging a line taken or not, a line counting the nine left out, the
  first host started without a seed; where the desktop can be read back,
  the three pictures' sizes and parts and that host's `none taken` line),
  `config-twelve` (twelve releases, `tests/fixtures/catalog-twelve.json`:
  seven off-screen renders at the first-open size, the smallest and one as
  narrow but 760 DIP tall, at 100% and 150%, light, dark and high contrast,
  unscrolled, at a stop in the middle and at the last: as many covers shown
  as the layout says, each wholly in the strip and clear of the chevrons
  (unscrolled, of the left one's place too) and the status line, the others
  outside the strip, and nothing else drawn in the strip, checked on the
  picture's pixels; the dialog driven by control ID at its first-open size
  (clamped to the monitor's work area), as many covers at a time as
  `layout_window` gives its client: the last cover, not shown, takes the
  focus and scrolls into view, the left chevron takes it away a stop a
  click, Space on it filters the list to its release and brings it back,
  **Show all** shows all twelve, the left chevron back to the first stop
  leaves its place empty, reopened with that filter saved the row opens at
  its last stop, and from seven releases an import (`fakeimport.exe`
  leaving the twelve-release catalog) turns seven covers side by side into
  twelve, unscrolled beside a right chevron, "46 modules from 12 releases";
  a ScreamSavers and a Marvel module, After Dark modules with `"screen":
  "640x480"`, previewed at 640x480 where the others get the box's 16:9 480
  lines, and `/s` at the 720-line setting on a 16:9 monitor rotating
  between a ScreamSavers and a Disney module, 640x480 and 1280x720 host by
  host), `config-credit` (the footer's credit
  in the dialog driven by control ID: shown, named "Made With Love by
  StarrLord" for UI Automation with the address as its help text, after
  Import… and before Preview in the tab order; a click, Enter and Space each
  record one request for the page, `AD_SCR_TEST_OPEN_LOG`, and open nothing;
  with Star Wars Screen Entertainment alone imported, focused in a wide
  window and then hidden by narrowing it to its minimum: the focus on
  Preview, which holds the default-button state the link gave up, so Enter
  runs Preview's `/s` and the dialog stays open, having saved nothing;
  off-screen renders at 100% and 150%, light, dark and high contrast, at
  the first-open size and the narrowest: clear of the assets line's text and
  of Preview by 24 DIP, centred between them, on the buttons' line; hidden
  beside the assets line at its longest in the narrowest window; there in
  the welcome), `import` (against
  `fakeimport.exe`: no console window, and exits 5, 2 and 0; the first
  import, from the welcome, asks the host after it and before the module
  shown starts, and the module's button comes alive) and
  `preview-settings` (the temporary settings file is swept, deleted by the
  preview, and not left behind when the dialog closes mid-preview). The
  interaction tests drive `/s` with `AD_SCR_TEST_INPUT`: `input-play` (Shift,
  Ctrl and Num Lock never exit, Num Lock reaching the host as one `NUMLOCK 1`;
  Caps Lock starts the game; arrows, clicks,
  the wheel and moves are the module's; the cursor shows and the clip is set
  while playing, released after; Caps Lock again, a small move after the
  re-baseline, then a key exits; numbered input lines; the last-exit log),
  `numlock` (against fakehost with `numlock=1`: every host started with
  `ADNUMLOCK`, the toggle then; `KEY 144` sends `NUMLOCK 1` right after its
  key line and a change without a key `NUMLOCK 0` within 250 ms, numbered as
  the host numbers its lines; a host started while it is on gets
  `ADNUMLOCK=1` and no line; and with `FAKEHOST_NUMLOCK=0` no `NUMLOCK` line
  at all, nothing the host doesn't know, and no `ADNUMLOCK` for the hosts
  started after its answer; Num Lock never ends the saver),
  `input-alt` (Alt ends a game), `input-wake` (the host's wake flag ends it),
  `input-rotate` (rotation waits while playing), `input-monitors` (two staged
  monitors: only window 0's host gets input, and each first host its
  monitor's capture at its emulated size), `input-display-change` (a
  monitor gone: the moves Windows makes before the re-plan don't end the
  saver, a nudge after it doesn't either, a real move does), `present`
  (a staged 1920×1080 monitor, through Direct2D and then GDI: which way the
  log says, the window's middle lit on the screen itself, read back from
  the desktop, and `AD_SCR_TEST_CAPTURE`'s pictures: sizes, black bars,
  and the host's pixels at their blocks' centres), `seed` (respawned hosts start
  black, no capture file is left, `StartFromDesktop=0`), `config-buttons` (the
  live button, `--owner` = the dialog, its unsaved values, `ADSTATE`, the
  dialog disabled until the host exits, enabled again after a crash, the
  notes, the live preview restarted each time); every test checks that each
  host got `ADSTATE=<settings folder>\state`. Sound (`AUDIO.md` §9):
  `sound` (two staged monitors with `Sound=1 Volume=35`, rotating, and a
  hostile inherited `ADSOUND=1` and `ADAUDIOOUT`: every host of the primary
  window got `ADSOUND=1 ADVOLUME=35` and the capture, every other
  `ADSOUND=0` without them, never two sound hosts at once; the primary moved
  to the other monitor: the old owner's module restarted silent and the new
  owner's next host played; then `Sound=0`, `AD_SCR_SOUND=0` and `/p`: no
  host played), `sound-wake` (hosts that take 220 ms after `QUIT`: every
  sound host, rotated away or at the end, ended on its own within its 400 ms
  grace, the silent ones rotated away were cut off at 150 ms, and on waking
  the sound host heard `QUIT` first) and `config-sound` (the dialog: Sound and
  Volume from a file without them, 0–100 page 10, named "Volume" for UI
  Automation, driven by Page Up/Up/Right/Left/Page Down, greyed with its
  label's mnemonic gone while Off; Preview's `/s` played the unsaved volume
  on its primary window's host alone while the live preview, thumbnails and
  the capabilities probe stayed silent; OK wrote `Sound=0 Volume=61
  SoundMonitor=primary` keeping the rest, and the next dialog showed them).

Two opt-in tests run the real `adhostwin.exe` (the build's own, with its
lanes) on real modules: set `AD_E2E=1` and `AD_E2E_ASSETS=<an assets root>`
(a scratch import; never the user's). `e2e-rodger`: Rodger Dodger `/s`,
scripted Caps Lock and arrows, no exit while playing, Caps Lock again, then a
key exits with the reasons in both logs. `e2e-dosshell`: DOS Shell `/S`
(the switch Windows passes) for 3600 frames on one monitor staged off every
real one, so it never covers the screens (started with `CreateProcess`,
never `ShellExecute`), no `input:` or `host-` line before `test-exit`, and
the last-exit log next to `AD_SETTINGS`.

A third, `e2e-sound`, needs `AD_SCR_SOUND_E2E=1` as well and a host whose
`--capabilities` says `audio=1` (it skips otherwise): the real host on two
staged monitors with `Sound=1 Volume=40`, capturing to a WAV with
`ADAUDIOLIVE=0`, so nothing is ever played on the audio device. The primary
window's host ran with sound and the other without (one `[audio]` summary
in the hosts' stderr), the sound host heard `QUIT` first, the WAV's header
was patched at its clean end, and it holds Burns' speech (a 100 ms window
above −40 dBFS within the run's 1200 frames; he first speaks about 11 s in).

**Sound in the tests.** Sound is on by default and the GUI smoke tests run
`/s`, so CMake gives every scr test `AD_SCR_SOUND=0` (the `ENVIRONMENT`
property, added to every test in `CMakeLists.txt`). The sound tests take it
out of the environment of the saver they start, and run `fakehost.exe`,
which makes no sound; `e2e-sound` is the one test in which a real host runs
with sound on, and it only captures.

The smoke tests open real windows. `/s` briefly covers every monitor, and the
settings dialog is driven by control ID. They are labelled `gui`: skip them
with `ctest -LE gui`, or set `AD_SCR_SKIP_GUI_TESTS=1` to have them report
SKIP. They also skip themselves when there is no interactive desktop. While
the monitors are asleep (display powered off), a `/s` pauses its hosts and
the tests that run one never see a frame: set `AD_SCR_TEST_DISPLAY_ON=1` for
the run (the tests pass it on to the saver).

### By hand, before a release

These change the machine's own settings or need a person at the monitors,
so no test does them; run them once on the packaged `LongAfterDark.scr`
(`build\dist\LongAfterDark`), with the releases imported. Windows starts
the saver itself here, so `AD_SCR_LOG` is off (unless it is in your user
environment): look in the last-exit log (`logs\saver-last.log` under the
data folder) for what happened.

1. **Install.** Right-click `LongAfterDark.scr` → **Install**. Screen Saver
   Settings opens with "Long After Dark" chosen (`HKCU\Control
   Panel\Desktop\SCRNSAVE.EXE` names the file where it is). The small
   preview shows the chosen module (`/p`: a live 320×240 frame, not black,
   no window of its own); switching to another saver and back restarts it,
   and closing Screen Saver Settings leaves no `LongAfterDark.scr` or
   `adhostwin.exe` running.
2. **Settings… and Preview there.** **Settings…** opens the settings
   window (`/c:<hwnd>`, owned by Screen Saver Settings); **Preview** runs
   `/s` full screen on every monitor and a key ends it.
3. **The timeout.** With **Wait: 1 minute** and nothing touched, the saver
   starts by itself; a move of the mouse ends it. With "On resume, display
   logon screen" on, it ends at the lock screen.
4. **System32.** As an administrator copy `LongAfterDark.scr`,
   `adhostwin.exe` and `adimport.exe` to `%WINDIR%\System32`: "Long After
   Dark" is in the list for every user, and a user who has never opened it
   gets the not-imported message until they import.
5. **Monitors.** With `/s` running on two monitors, unplug one (or turn it
   off, for a DisplayPort monitor that drops out) and plug it back in, and
   change one's resolution in Settings → Display (Win+P works without a
   mouse): the saver keeps running and covers exactly the monitors there
   are ("relayout monitors=…" in the last-exit log), and doesn't end
   because Windows moved the cursor. In Random, with **Change module every**
   at 1 minute, both monitors show the same module and switch together;
   with **A different module on each monitor** checked, each shows its own.
6. **Display power-off.** With Power & sleep → Screen set to 1 minute and
   the saver's wait shorter: the display goes off with the saver running,
   the hosts pause ("display off: pausing hosts") and resume when it comes
   back, and the saver is still running.
7. **Uninstall.** Choose another screen saver, delete the three files: no
   `LongAfterDark` process is left and nothing else is changed.
