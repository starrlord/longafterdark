# Installing Long After Dark

**Long After Dark** is a screen saver for Windows that runs the original
modules of After Dark, of LucasArts' Star Wars Screen Entertainment and of
Delrina's The Far Side and Dilbert collections, unchanged, under x86
emulation. It knows fourteen releases, 314 modules:

| id | Release | Internet Archive download |
|---|---|---|
| `deluxe` | After Dark 4.0 Deluxe (1996) | CD image, 381.7 MB |
| `ad10` | After Dark 10th Anniversary (1999) | CD image, 143.3 MB |
| `ad32` | After Dark 3.2 (1995) | CD image, 58.8 MB |
| `tt` | Totally Twisted After Dark (1995) | CD image, 37.9 MB |
| `simpsons` | The Simpsons Screen Saver (1994) | install files (ZIP), 2.6 MB |
| `swse` | Star Wars Screen Entertainment (1994) | CD image, 6.9 MB |
| `startrek` | Star Trek: The Screen Saver (1992) | two floppy images, 2.8 MB |
| `marvel` | Marvel Comics Screen Posters (1993) | install files (ZIP), 1.9 MB |
| `snoopy` | Snoopy's Screen Savers (1994) | install files (ZIP), 1.9 MB |
| `looney` | The Looney Tunes Screen Saver (1995) | install files (ZIP), 2.8 MB |
| `screams` | ScreamSavers (1995) | install files (ZIP), 3.3 MB |
| `disney` | The Disney Collection Screen Saver (1995) | install files (ZIP), 3.4 MB |
| `farside` | The Far Side Screen Saver Collection (1994) | install files (5 ZIPs, one per disk), 5.5 MB |
| `dilbert` | Scott Adams' Dilbert Screen Saver Collection (1994) | install files (ZIP), 4.3 MB |

Star Trek: The Screen Saver is After Dark 2.0 (version 2.0b) with 16 Star
Trek modules, on two floppies. Marvel Comics Screen Posters (After Dark
2.0d, one module of 36 posters) and Snoopy's Screen Savers came on two
floppies each, the Looney Tunes on two floppies or a CD, ScreamSavers and
the Disney Collection on three floppies. ScreamSavers (Binary Software) and
Snoopy's Screen Savers (Image Smith) are other companies' modules for After
Dark: ScreamSavers shipped the After Dark engine it licensed, and Snoopy's
modules were made for an After Dark already installed, so Long After Dark
supplies the sound library they would have found there.

Star Wars Screen Entertainment is not an After Dark release, though it is
sometimes listed as "After Dark Star Wars": its 14 modules were made for
Delrina's Intermission screen saver engine, which Long After Dark stands in
for as it does for After Dark's. The Far Side Screen Saver Collection (14
modules, five floppies) and Scott Adams' Dilbert Screen Saver Collection
(16 modules, four floppies) are Delrina's own Intermission releases: most
of their modules are animations, played by Intermission's own animation
player, which runs unchanged as the modules do.

Requirements: 64-bit Windows on an x64 PC. It was developed on Windows 11.
On Linux, see [LINUX.md](LINUX.md): it runs there under Wine, with a
player of its own.

## The programs

Download `LongAfterDark-<version>-x64.zip` from the
[latest release](https://github.com/starrlord/longafterdark/releases/latest)
and unzip it anywhere. The programs aren't code-signed yet, so Windows may
warn that they come from an unknown publisher: click **More info**, then
**Run anyway**. Or build it from source as [BUILDING.md](BUILDING.md)
describes: `bash tools/package.sh` stages the same files in
`build/dist/LongAfterDark/`. The zip's folder holds:

- **LongAfterDark.scr**: the screen saver and its settings window.
- **adhostwin.exe**: the emulator. The screen saver starts one for each
  monitor.
- **adimport.exe**: copies the modules from your discs.
- **README.txt**: a short version of this page.
- **LICENSE.txt** and the **licenses** folder: this project's licence and
  those of the code built into the programs.

Keep the three programs in one folder: the screen saver looks for the other
two next to itself. No file of any of the releases is included. You import
them from your own copy (and are responsible for sourcing them legally).

## 1. Import your releases

Double-click `adimport.exe`, or open the screen saver's settings and click
**Import…**. Then pick a source:

- **A disc or floppy image:** `.iso`, `.bin`, `.img`, `.ima`, `.vfd` or
  `.flp`, or a `.zip` of the install files, or of a release's floppy
  images. If you have a release's floppies as separate images, select them
  all, such as the Simpsons' two or Star Trek's two, or the ZIP they came
  in. A `.zip` that keeps each disk's files in a folder of its own
  (`DISK1`, `DISK2`, …, as the Internet Archive's copies of ScreamSavers,
  Marvel and Snoopy do) is read as all its disks together.
- **A drive or folder:** the CD itself, or a folder copied from it (for
  floppies, one folder holding the files of every disk, or one holding
  nothing but a `DISK1`, `DISK2`, … folder per disk).
- **A download from the Internet Archive:** a list of the fourteen releases
  with their sizes, plus one entry that fetches every release not imported
  yet. An interrupted download resumes, and each file is checked against its
  published MD5 before it is used.

The importer works out which release it was given, checks every file against
that release's known MD5s, and installs it beside the releases already
imported, which it leaves untouched. Import as many as you like.

Star Wars Screen Entertainment verifies only as the build on its CD: the
CD, its ISO or Redump BIN image, the ZIP of its files, or
`adimport --download swse`. The floppy sets found online (the US five-disk
set and the German edition) are other builds: they fail verification (exit
code 3) unless imported with `adimport --no-verify` from a command prompt.

Star Trek: The Screen Saver came on two floppies. Import both images
together, in either order
(`adimport --image disk1.img --image disk2.img`), or the ZIP they came in
(such as the Internet Archive's `afterdark-20b_startrek.zip`), a folder
holding both disks' files, or `adimport --download startrek`. One disk
alone is refused: the importer says it needs every install disk (for disk 2
alone, that the image is install disk 2 of 2). A folder
where After Dark 2.0 was installed (`C:\AFTERDRK`) is no source: the
importer needs the install disks.

Marvel Comics Screen Posters, Snoopy's Screen Savers, the Looney Tunes,
ScreamSavers and the Disney Collection have no image of their disks online:
the Internet Archive's ZIP of each one's install files (the one the download
fetches, and likely the copy you have) is its known copy, and verifies as
the known ZIP. A flat folder or ZIP of the same files, or one kept in
`DISK<n>` folders, verifies file by file. Every install disk is needed:
disk 1 alone of ScreamSavers, Marvel or Snoopy is refused ("needs every
install disk"), and another disk alone is not a known release. The Looney
Tunes' April CD (`LOONEY_T`) verifies too; its August CD (`LTW320CD`, also
online as `LOONEY.zip`) carries After Dark 3.2's engine files and fails
verification (exit code 3) unless imported with `adimport --no-verify`.
The notes a previous owner left in some of these copies (serial numbers)
are never opened.

The Far Side Screen Saver Collection and Dilbert have no image of their
floppies online either. The Far Side's only intact copy is a 1994
bulletin-board copy of its five floppies, a ZIP of each disk's files
(`PNX-FSC1.ZIP` to `PNX-FSC5.ZIP`), which `adimport --download farside`
fetches: select all five together and they verify as its known copy. The
floppy images in the Internet Archive item named for the release are
damaged, so an import from them fails. Dilbert's known copies are the
Internet Archive's ZIP of its four floppies' files (`DilbertS.zip`, which
the download fetches) and the same disks as four ZIPs, one per disk
(`DILBERT1.ZIP` to `DILBERT4.ZIP`). A folder or ZIP of the same files, flat
or in `DISK<n>` folders, verifies file by file. Every install disk is
needed: disk 1 alone is refused ("needs every install disk"), and another
disk alone is not a known release. The notes those bulletin-board copies
carry beside the release's files are never opened.

From a command prompt, with the ids from the table above:

```
adimport --image "C:\Images\After Dark 3.2.iso"
adimport --image disk1.img --image disk2.img
adimport --image afterdark-20b_startrek.zip
adimport --image "C:\Downloads\After Dark - Scream Savers.zip"
adimport --image PNX-FSC1.ZIP --image PNX-FSC2.ZIP --image PNX-FSC3.ZIP --image PNX-FSC4.ZIP --image PNX-FSC5.ZIP
adimport --from E:\
adimport --from C:\Copies\Snoopy
adimport --download ad10
adimport --download swse
adimport --download disney
adimport --download all
adimport --list-packages
adimport --remove tt
```

`--list-packages` shows which releases are imported, and `--remove <id>`
deletes one. `adimport --help` lists every option. The exit code is 0 on
success, 1 on an error such as a file that cannot be written, 2 when the
source is not a known release, 3 when verification fails, 4 on a network
error and 5 when you cancel.

## 2. Covers

With two or more releases imported, the settings window shows their box
covers above the module list. Click covers to list only the modules of
those releases (with none selected, it lists them all); right-click a cover
for **Show only …** and **Show all releases**. An import fetches the
release's cover picture from the Internet (checked against its published
MD5) or uses the art on the disc; a cover it cannot get is drawn as a plain
box with the release's title, and the import still succeeds. While a
release still shows such a plain cover, **Get the covers** (in the settings
window or the importer) fetches the pictures; from a command prompt,
`adimport --refresh-covers` does the same.

To use a picture of your own, right-click a cover → **Change cover…** (or
**Change cover…** next to the release in the importer), or run
`adimport --set-cover <id> <picture>`. Any picture Windows can read will do
(PNG, JPEG, GIF, BMP, TIFF); it stays on this computer.
`adimport --clear-cover <id>` goes back to the original cover.

## 3. Install the screen saver

- **For yourself:** right-click `LongAfterDark.scr` → **Install**. Windows
  makes it the current screen saver where it is and opens Screen Saver
  Settings, so leave the folder where it is.
- **For every user:** copy `LongAfterDark.scr`, `adhostwin.exe` and
  `adimport.exe` to `C:\Windows\System32`, then choose **Long After Dark** in
  Screen Saver Settings (Settings → Personalization → Lock screen → Screen
  saver).

Right-click → **Test** runs it full screen at once. Double-clicking the `.scr`
does the same: that is what Windows does with screen savers.

## 4. Choose what it shows

**Settings…** in Screen Saver Settings (or right-click `LongAfterDark.scr` →
**Configure**) opens the settings window. Pick one module or **Random** and
the modules it rotates through, how often it changes, the resolution, the
monitors to use and the sound. A live preview shows the selected module with
its options. With two or more releases imported, click their covers to list
only those releases (§2).

In Random, two or more monitors show the same module and change it
together. For a different module on each, check **A different module on
each monitor** under **Change module every**; it is there only when two or
more monitors are connected, and greyed while **Monitors** is **Primary
monitor only**.

Some modules have options of their own, shown as buttons among the module's
settings: Fish World's **Select Fish…**, the messages of the message
modules, Art Critic's **Pictures** and others. A button opens the module's
own window, as the original control panels did. What you set there is saved
by the module at once, so the settings window's **Cancel** does not undo it.

Each Star Wars Screen Entertainment module has all its options behind one
such button, **Configure...**, which opens the module's own settings window
(sound, music, picture quality, text speed and the like). The modules keep
those settings in their `SWSE.INI`, under `state\swse\WINDOWS\` in your
data folder (see below); deleting `state\swse` brings back the disc's
defaults. The **Resolution** setting does not apply to them: they compose
their scenes for a 640×480 screen, so they always get one, scaled to fit
your monitor in its 4:3 shape (with bars at the sides on a widescreen
monitor, above and below on a 5:4 or portrait one). Check **Stretch to fit
the screen (no black bars)**, under **Resolution** and **Monitors**, to
have them, and every other module that always gets 640×480, fill the
monitor instead, stretched out of their shape. The live preview shows it
as you check it; After Dark's own modules fill the screen either way.

The Far Side's and Dilbert's modules have one such button too,
**Configure...**. For an animation it opens Intermission's own "Animation
Player Options" (sound effects and music on or off, colour options); for
their other modules, the module's own window, such as the banner text of
The Far Side's Pterodactyl. Those settings are kept in `ANTSW.INI`, under
`state\farside\WINDOWS\` or `state\dilbert\WINDOWS\` in your data folder,
and apply from the next run. Like the Star Wars modules, these always get
640×480, scaled to fit.

Several Star Trek modules compose their scenes for a 640×480 screen too,
so the **Resolution** setting does not apply to any of them either: they
always get 640×480, scaled to fit as the Star Wars modules are. Two of
them have buttons. Communications' **Edit Custom...** edits your own
message (choose "Custom" under **Message** to show it), and Sounder's
**Sounds..** picks the folder of `.WAV` files Sounder plays: besides the
module's own folder, its list of drives offers `[-h-]`, where your own
drives are (`H:\C\...` is your `C:`, in short 8.3 names). The folder you
pick is kept, drive and all, for every later run. Pick one near a drive's
root: as in DOS, its short path after the drive letter must fit in 63
characters. After Dark's Globe ("Map..." in After Dark 4.0 Deluxe and 3.2)
reaches your drives the same way.

Several ScreamSavers modules and Marvel's poster module compose their
pictures for 640×480 too, so all the ScreamSavers modules and Marvel's
always get 640×480, scaled to fit. Marvel's
module has two buttons: **Saver..** chooses the posters it shows, in order
or at random, with or without their captions (it keeps your choice in its
own catalog file, in your data folder's `state\marvel\`), and
**Posters...** installs a poster as wallpaper, or shows its description
(**Info...**). The wallpaper it makes, and the one Create Poster On Wakeup
makes, stay inside the emulated PC: your own desktop never changes. The
Looney Tunes' Messages has **Edit Custom...**, where you type the message
Foghorn, Elmer or Speedy says when **Custom Message** is chosen.

**Sound** is on by default. Only the primary monitor's screen saver plays
it, at the default volume (50): the modules' wave effects, their MIDI
music (through Windows' MIDI synthesizer, normally the Microsoft GS
Wavetable Synth), the Simpsons' speech and the Star Trek modules' sounds
(which the original could also play on the PC speaker; here they always go
through Windows' sound). Snoopy's modules play through a sound library of
Long After Dark's own, which does what After Dark's did. Marvel's module
is silent, as it always was. In the settings window,
**Sound** (Primary monitor / Off) and **Volume** (0–100) change that. For
Star Wars Screen Entertainment, Volume reaches the effects through
Intermission's own volume setting, and the music as the Windows mixer's
synthesizer slider did, since Intermission itself set only the effects'
volume. The Far Side's and Dilbert's modules play wave effects (a few have
none) and no music. **Preview** plays sound with the values you have not saved yet; the
small live preview never does.

## Ending it, and playing

Any key except Shift, Ctrl, Caps Lock and Num Lock, a click, the mouse
wheel, moving the mouse or switching away (the Windows key, Alt+Tab,
Ctrl+Alt+Del) ends the screen saver. Caps Lock never does: in some modules
it changes something (it scares the fish, changes the colours, or in many
Looney Tunes and Disney modules moves on to the next scene) or starts a
game, as in Rodger Dodger, You Bet Your Head, Simpsons Trivia, Mime Hunt,
Frankenscreen, Marbles and Pinocchio (the Wishing Star: Pinocchio follows
the mouse).

While a game is playing, keys, clicks and the mouse belong to it, and the
pointer stays on the primary monitor. Press Caps Lock again to stop playing
(the next key or move then ends the screen saver), or press Alt to end it at
once. Locking the computer (Win+L) ends the screen saver whether a game is
playing or not. Only the primary monitor plays; the others keep running on
their own.

Num Lock never ends the screen saver either. In Star Trek's **Final Exam**
it starts the Starfleet Academy exam: type the number of your answer (1 to
4, on the top row or the keypad). Num Lock again stops the exam, and
moving the mouse ends it, and the screen saver with it.

## Where your files are

Everything is in `%LOCALAPPDATA%\LongAfterDark` (paste that into Explorer's
address bar):

| Folder or file | What it holds |
|---|---|
| `assets\win\` | the imported modules, one folder per release, the module list `catalog-win.json`, and the releases' box covers (`covers\`) |
| `downloads\` | Internet Archive downloads, reused if you import the same release again |
| `settings.ini` | the screen saver's settings |
| `state\` | what the modules save themselves (message texts, chosen pictures, high scores, the Star Wars, Far Side and Dilbert modules' settings, Sounder's folder, Marvel's poster choices), per release |
| `thumbs\` | the settings window's module pictures |
| `logs\saver-last.log` | how the last screen saver run went and why it ended |

## Updating

Close the settings window and make sure the screen saver is not running
(Windows locks running programs), then replace the three programs with the
new versions. Imported releases, downloads and settings are kept.
`adimport --version` says which version you have, and so does each
program's Properties → Details in Explorer.

## Not done yet

- **Speed.** Each module's pace follows a model of a mid-1990s PC (Swirling
  Magic's too-fast pace is fixed); not every module has been compared with
  the original yet. Marvel's poster transitions (wipes, irises, blinds and
  the like) show at once where the original swept them over about half a
  second.
- **Chameleon** (Totally Twisted and 10th Anniversary): after about half a
  minute a stray icon covers the "Accessories" label, a known difference
  not fixed yet.
- No installer or code signing yet.
