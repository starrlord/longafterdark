# adimport's windows (`adw_import_gui`)

`adimport --gui` (and `adimport` started from Explorer with no arguments)
shows these windows. They are themed with `adw_ui` (`common/ui`),
the library the settings dialog of `LongAfterDark.scr` also uses, so both apps
look the same and follow light mode, dark mode and high contrast live. The
design is `docs/COVERS.md` §4.

`adimport.cc` builds a `gui::Request` from its arguments and returns
`gui::run(request)` as its exit code (`gui.h`, frozen by COVERS.md §8.2).
This directory never defines or changes the `adimport` target; the
importer's `CMakeLists.txt` adds it and links `adimport` to
`adw_import_gui`.

## The windows

Every window is a top-level window titled exactly "Long After Dark"
(the header band under the title bar carries the page's own name). Each
page is its own window: the next one opens where the last one was, and
the last one closes after it. There is never a task dialog or a message
box.

| Page | What it shows | Command ids |
|---|---|---|
| **Sources** (`--gui` with no source) | The header "Import a release"; an intro (`sources_intro`) that names every release of the registry while nothing is imported, and once something is gives their number in words instead ("Long After Dark runs the original Windows modules of twelve releases. Choose where to copy them from."), so the page keeps room for the list; the **Installed** releases, each with its cover (48×60), "N modules · verified against the original disc" (the original disks for a floppy release; "verified against the known ZIP" for a release known by the ZIP of its install files, `verified_words`, by the md5 the import came from) and a **Change cover…** link (its accessible name names the release: "Change the cover of After Dark 3.2…"); while any of them still shows a generated cover (an install from before covers, or an offline import), "N releases have no cover picture yet." with **Get the covers** (a progress window over `refresh_covers` for those releases, then what it got, then Sources again); three cards: **A disc image…** ("An ISO image of a CD or floppy images (.img), or a ZIP of them or of the install files; select every disk of a set." — its file dialog lists "Disc and floppy images, and ZIPs of them or of install files"; a ZIP of a set's floppy images, such as Star Trek's from the Internet Archive, is one file, and so is a ZIP that keeps a release's disks in `DISK<n>` folders, such as ScreamSavers'; Star Wars Screen Entertainment's set is five, though only floppies of its CD's build verify: the earlier builds' floppy sets found online fail verification (3) unless adimport runs with `--no-verify`), **A drive or folder…**, **Download from the Internet Archive…** (sizes; hidden when nothing can be downloaded); a folder that is no known release gets the caution "That is not a disc Long After Dark knows.", with `identify_folder`'s reason (for a folder whose `DISK<n>` folders sit beside anything else, first that it read them as they are, as the CLI's log says); "Files are copied to …"; **Cancel** (**Close** once something changed) | 101, 102, 103, 104 (Get the covers); 400 + registry index for Change cover…; IDCANCEL |
| **Downloads** (from 103) | One card per release in registry order, fourteen today (limited by `--package`): cover, title, "CD image · 381.7 MB" / "Install files (ZIP) · 2.6 MB" / "2 floppy disk images · 2.8 MB" (the first copy's kind and size, every image of a floppy set counted: Star Wars Screen Entertainment's is "CD image · 6.9 MB", Star Trek's the two floppies, and the five later releases' "Install files (ZIP)"), "Imported · verified against the original disc" (a floppy release's: "disks"; a release known by its ZIP: "the known ZIP") / "Not imported yet", "already downloaded" (in `--download-dir`, else the default folder; a floppy set only when every image is there); **Every release not imported yet** when two or more are not; the md5 and downloads-folder note; **Back** | 200 + i, 299, IDCANCEL (Back, to Sources) |
| **Progress** | The phase ("Downloading … from the Internet Archive (2 of 4)", "Copying …", "Getting the cover art" for `Progress::Phase::cover`, and for all of `--gui --refresh-covers` / Get the covers), a Fluent progress bar (determinate, or a sweep when the total is unknown), "123.4 MB of 400.0 MB (5.0 MB/s)" and the current item (the file being copied, verified or downloaded, or the cover's source; nothing when the step names none, never a log line). A step with nothing to count (a cover from the disc, the finishing step) shows no amount; a download with no bytes yet shows "Starting…", as each release of **Every release not imported yet** does under its own title until its first bytes come. **Cancel** greys and says "Cancelling…" until the worker has stopped; the window cannot be closed otherwise | IDCANCEL |
| **Result** | A glyph (done E930 in the accent colour, partly done E7BA in caution, failed EA39 in critical), the heading and the text today's message boxes had ("The image matched the known image of …", and for a release known by the ZIP of its install files "The ZIP matched the known ZIP of …"); for a failure "Nothing was changed." and **Copy details** (the message and every mismatched file, to the clipboard) | IDOK (Done, default), 501 |
| **Cover** (`--gui --change-cover <id>`, or Change cover… on Sources) | The release's cover at 192×240 with where it came from ("Box front · Wikisimpsons", "Installer art from your disc", "Your own picture", "No picture yet"); **Choose a picture…** (`set_cover`), **Use the original cover** (`clear_cover`, when your own picture is in use; the line under it says what the original is: "Original: Box front · Wayback Machine"), **Download the original cover** (`refresh_covers`; shown only when a better download exists and downloads are allowed, with what it gets under it: "Gets the Box front · Wayback Machine", and "your picture stays the cover" while one is set); the result in the window (caution or critical for problems); "Pictures stay on this computer, in …" | 601, 602, 603, IDOK (Done, default, Esc) |

Keyboard: Tab and Shift+Tab move between controls (the lists are part of
the order), Enter presses the focused card or button (else the default),
Esc is Cancel/Back/Done, Space presses a button. A focused control that
is scrolled out of sight is scrolled into view.

Layout (COVERS.md §3.3): a client area 640 DIP wide (at least 560, less
only when the work area is narrower), 24-DIP margins, 16-DIP gaps, 32-DIP
controls, and a height fitted to the page and clamped to the work area. A
list longer than the window scrolls inside its card (the thin Windows 11
scroll bar). When the Sources page is too tall for the work area, its
installed list shows as many whole rows as fit, two at the least (eleven of
twelve at 150% on a 2560x1440 monitor, six at 100% on a 1080-line screen),
else as many as fit down to one (the part of a row that shows says there
are more: ten releases and more at 150% on a 1080-line screen), and scrolls
in its card; only when even one row does not fit does the whole body
scroll, with the list at its full height, so only one thing ever scrolls.
Everything is laid out again on `WM_DPICHANGED` (per-monitor DPI v2), and
on a theme change (`ui::is_theme_change`) the window reloads its palette,
title bar and scroll bars.

## Exit codes

`gui::run` returns an `adw::import::Status` (the process's exit code):

* **0** when anything was imported or any cover changed during the session;
* else the **first failure** of an import (2 source invalid, 3 verify
  failed, 4 network, 1 error);
* else **5**: nothing changed (Cancel, or the cover window closed without a
  change; a cover error is shown in the window and also ends in 5).

`--gui --refresh-covers [<id> | all]` shows only the Progress page over
`refresh_covers` and then a Result page ("Got 2 covers", "Got 1 of 2 covers",
"The cover downloads failed", a line per release): 0 when a cover changed,
else the first failure (4 when a download failed), else 5. The settings
dialog's **Get the covers** runs it with `all` and reads the catalog again on
0. `gui::Request` also carries `--download-dir` (downloads, cover pictures,
"already downloaded") and `--force`.

`AD_GUI_AUTOCLOSE=1` skips the Result page (the tests). A cancelled single
import shows no Result page either.

Imports pass `Request::cover_download` on as `ImportOptions::cover_download`,
and the cover window as `CoverOptions::allow_download`.

## Test hooks

* **Screenshots.** `AD_IMPORT_TEST_SCREENSHOT=<png>` with
  `AD_IMPORT_TEST_SCREENSHOT_STATE=key=value;…` renders one page without
  it ever appearing on screen (parked off every monitor, cloaked, drawn
  with `PrintWindow`), from synthetic state: no import, no network. The
  page gets its full height, as on a monitor large enough for it, unless
  `workarea=` says otherwise (a real window that the system clamps is laid
  out again for the client it got). The
  installed releases and covers are read from `--dest`. Keys:
  `page=sources|downloads|progress|result|error|cover`, `theme=light|dark|hc`,
  `dpi=<n>`, `focus=<command id>`, `progress=<0..1>|marquee`,
  `phase=download|check_image|copy|verify|cover|finalize`,
  `result=single|several|partial|covers`, `job=covers` (the progress page of
  "Get the covers"), `package=<id>` (the cover page),
  `status=ok|error|network|running` (the cover page), `caution=1` (Sources'
  "not a disc Long After Dark knows"), `workarea=<w>x<h>` (DIPs; default unlimited),
  `dpichange=<n>` (a monitor change, without `dpi=`), `themechange=light|dark|hc`
  (a live theme change after opening), `report=<path>` (the client area in
  the picture, a pixel of the body's margin and `pal.base`; on Sources also
  `list=<shown>,<whole>,<row>`, the installed list's heights in pixels).
* **File dialogs.** `AD_IMPORT_TEST_PICK=<path>[|<path>…]` answers the
  file and folder dialogs (Sources' 101/102, the cover window's 601)
  without opening them.

## Tests

* Every mode points `AD_LOCALAPPDATA` at a scratch folder first, so no
  window and no `adimport` it starts takes a default from the user's data
  folder.
* `import.gui_model`: the file dialogs' own code with no window (the
  options each gets, accepted by the shell's `IFileOpenDialog` as set, and a
  result array of files and a folder read back as their paths; showing the
  dialog itself is the shell's and is not driven), and the wording and data
  the windows show (phases, the
  amount line, installed rows with module counts, the Internet Archive
  list — twelve releases, Star Wars Screen Entertainment's card, Star Trek's
  "2 floppy disk images · 2.8 MB", "already downloaded" only once both of its
  images are there, "Every release not imported yet" over the ten a
  scratch install of Deluxe and Totally Twisted lacks, the five later
  releases' "Install files (ZIP)" cards — the
  result texts, the cover origin line ("verified against the original
  disks" for a floppy release, "verified against the known ZIP" for a
  release known by its ZIP, and its result "The ZIP matched the known ZIP
  of …"), the Sources intro (every title while nothing is imported, then
  the count in words: "ten releases" with ten in the registry, "twelve
  releases" with twelve), the
  exit-code tally), over a scratch assets tree; never creates the assets
  folder.
* `import.gui_shots`: every page × light/dark/high contrast × 100/150/200%
  through the screenshot hook, over a scratch install with synthetic
  covers (set with `adimport --set-cover`), every release of the registry
  installed; each picture exists, its DPI is the one asked for, and its
  body margin is `pal.base`. Plus focus rings,
  a marquee cover phase, partial results, cover errors and a running
  cover action, a short work area, and live theme and DPI changes. The
  Sources list, read from the hook's report with cover downloads off: at
  least eleven whole rows of twelve at a 2560x1392 DIP work area and 150%,
  five at 1920x1032 and 100%, and the fallback at 640x520.
  Nothing is shown on the desktop, so it is not labelled `gui`.
* `import.gui_flow` (label `gui`: real windows, briefly, in every default
  `ctest` run; `ctest -LE gui` or `AD_IMPORT_SKIP_GUI_TESTS=1` skips it,
  exit 77): Sources → 103 →
  Back → Cancel is exit 5 with each page a new window and nothing written;
  a live theme change keeps the window working; the same walk with no
  `--dest` creates nothing in the data folder; on a scratch install whose
  cover is still generated, Sources' Change cover… link has the release's
  accessible name (read back through MSAA) and Get the covers (104) goes
  through Progress and Result back to Sources, and `--gui --refresh-covers`
  alone ends on its Result page (both with `AD_COVER_DOWNLOAD=0`: nothing is
  fetched, exit 5); "Change cover" with a synthetic picture is exit 0 and
  writes a new `tile.png`;
  Done with no change is exit 5.
* Closing a window while its worker runs (Cancel, Esc, the close box, or
  the window going away) cancels the worker's `CancelToken`, which closes
  whatever WinHTTP is waiting on, so a download stops at once; a page
  destroyed with its worker still running hides itself and keeps messages
  flowing until the worker has stopped.
* `import.cli`'s `AD_GUI_TESTS=1` block (the importer's) presses the same
  ids with `TDM_CLICK_BUTTON` and must pass unchanged.

Screenshots taken for review live under `research/win/ui/covers/importer/`
(gitignored).
