# Installing Long After Dark

**Long After Dark** is a screen saver for Windows that runs the original
modules of After Dark, of LucasArts' Star Wars Screen Entertainment, of
Delrina's Intermission and its Opus 'n Bill, Flintstones, Far Side and
Dilbert collections, and of Sierra's Johnny Castaway, unchanged, under x86
emulation. It knows twenty releases, 429 modules:

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
| `tng` | Star Trek: The Next Generation Screen Saver (1994) | CD image, 5.8 MB |
| `castaway` | Screen Antics: Johnny Castaway (1992) | floppy image (ZIP), 1.3 MB |
| `opus` | Opus 'n Bill Screen Saver (1993) | install files (3 ZIPs, one per disk), 2.8 MB |
| `opusroad` | Opus 'n Bill: On the Road Again! (1994) | install files (ZIP), 4.4 MB |
| `flintstones` | The Flintstones Screen Saver Collection (1994) | install files (3 ZIPs, one per disk, in a tar), 129.3 MB |
| `intermission` | Intermission 4.0 (1993) | three floppy images (ZIP), 3.3 MB |

Star Trek: The Screen Saver is After Dark 2.0 (version 2.0b) with 16 Star
Trek modules, on two floppies. Marvel Comics Screen Posters (After Dark
2.0d, one module of 36 posters) and Snoopy's Screen Savers came on two
floppies each, the Looney Tunes on two floppies or a CD, ScreamSavers and
the Disney Collection on three floppies. ScreamSavers (Binary Software) and
Snoopy's Screen Savers (Image Smith) are other companies' modules for After
Dark: ScreamSavers shipped the After Dark engine it licensed, and Snoopy's
modules were made for an After Dark already installed, so Long After Dark
supplies the sound library they would have found there.

Star Trek: The Next Generation Screen Saver (1994) is Berkeley Systems'
After Dark 3.0 release of 13 modules of the series, on a CD with After
Dark 3.0's installer: Data Dances, Encounters, Nanites, Officer's Review,
Personnel Files, Starbase, Science Stations, Tachyon Particle Field, The
Borg, Starfleet Messages, Counselor Troi, Warp Effect and Worf's Weapons,
with their music.

Screen Antics: Johnny Castaway (1992) is Sierra On-Line's, made by Dynamix
(Jeff Tunnell Productions), on one floppy, and was sold as "the world's
first storytelling screen saver": the days of Johnny, a castaway on a tiny
desert island with one palm tree. It is no After Dark or Intermission
module but a Windows 3.1 screen saver program of its own, `SCRANTIC.SCR`,
which Long After Dark runs unchanged, the way Windows 3.1 ran it.

Star Wars Screen Entertainment is not an After Dark release, though it is
sometimes listed as "After Dark Star Wars": its 14 modules were made for
Delrina's Intermission screen saver engine, which Long After Dark stands in
for as it does for After Dark's. Intermission 4.0 (1993, three floppies)
is that engine's own release, with 54 modules of its own: graphic effects,
effects on your desktop's picture and a few animations. Delrina's own
collections for it are the Opus 'n Bill Screen Saver (1993, 16 modules of
Berkeley Breathed's Opus the penguin and Bill the Cat, three floppies),
Opus 'n Bill: On the Road Again! (1994, 16 more, four floppies), The
Flintstones Screen Saver Collection (1994, 15 modules, three floppies), The
Far Side Screen Saver Collection (14 modules, five floppies) and Scott
Adams' Dilbert Screen Saver Collection (16 modules, four floppies): most of
their modules are animations, played by Intermission's own animation
player, which runs unchanged as the modules do. Intermission 5.0 is not
online as a product of its own; its engine came with On the Road Again and
with Dilbert.

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
- **LongAfterDark.exe**: the same program again, under the name that
  shows it in a window
  ([Show it in a window](#show-it-in-a-window-a-be-right-back-screen)).
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
  `.flp`, or a `.zip` or `.7z` of the install files, or of a release's
  floppy images. If you have a release's floppies as separate images, select them
  all, such as the Simpsons' two or Star Trek's two, or the ZIP they came
  in. A `.zip` that keeps each disk's files in a folder of its own
  (`DISK1`, `DISK2`, …, as the Internet Archive's copies of ScreamSavers,
  Marvel and Snoopy do) is read as all its disks together, and a `.zip` or
  `.7z` whose files all sit in one folder (as its copies of On the Road
  Again and Intermission 4.0 do) as that folder.
- **A drive or folder:** the CD itself, or a folder copied from it (for
  floppies, one folder holding the files of every disk, or one holding
  nothing but a `DISK1`, `DISK2`, … folder per disk).
- **A download from the Internet Archive:** a list of the twenty releases
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

Johnny Castaway came on one floppy. Import its image (`.img` or `.ima`),
the ZIP or `.7z` it came in, or a folder holding the floppy's files: the
Internet Archive's copy, which `adimport --download castaway` fetches, is a
ZIP of the image, and The Good Old Days Floppy Collection's is a `.7z` of
it. The importer never runs the floppy's installer: it expands the program
and its data from the floppy's compressed files, as the installer did, and
checks them against the release.

Delrina's four other releases came on floppies too. The Opus 'n Bill
Screen Saver is online only as bulletin-board copies of its three
floppies, a ZIP of each disk's files: `OPUS1NTA.ZIP` to `OPUS3NTA.ZIP`
(the download), `ONBSBS-1.ZIP` to `ONBSBS-3.ZIP` and `OPUS-1.ZIP` to
`OPUS-3.ZIP`; select the three of one copy together. A copy of its revised
build of November 1993 (`WC!OPUS1.ZIP` to `WC!OPUS3.ZIP`, with Censored
Toasters) imports too, as that build. On the Road Again's copy is the
Internet Archive's ZIP of its four floppies' files, all in one folder,
which `adimport --download opusroad` fetches. The Flintstones' June 1994
build is online only as three ZIPs, `FLINTST1.ZIP` to `FLINTST3.ZIP`,
inside a 129.3 MB archive (a tar) of a 1994 shareware collection:
`adimport --download flintstones` fetches that archive and takes out those
three ZIPs and nothing else, and three ZIPs you have already import as
they are. The May 1994 build most copies online hold (`FLINT1.ZIP` to
`FLINT3.ZIP`) imports as that build, with nine modules: its Prehistoric
Vehicles animation is damaged in every known copy, so it is left out.
Intermission 4.0's three floppy images (`ITM4W-D1.IMA` to `ITM4W-D3.IMA`)
import loose, together, or in the Internet Archive's ZIP of them, which
the download fetches. For each of them every install disk is needed, and
the notes and programs the bulletin boards put beside the files are never
opened or run.

From a command prompt, with the ids from the table above:

```
adimport --image "C:\Images\After Dark 3.2.iso"
adimport --image disk1.img --image disk2.img
adimport --image afterdark-20b_startrek.zip
adimport --image "C:\Downloads\After Dark - Scream Savers.zip"
adimport --image PNX-FSC1.ZIP --image PNX-FSC2.ZIP --image PNX-FSC3.ZIP --image PNX-FSC4.ZIP --image PNX-FSC5.ZIP
adimport --image 000580_jonny_castaway.7z
adimport --image OPUS1NTA.ZIP --image OPUS2NTA.ZIP --image OPUS3NTA.ZIP
adimport --image "Intermission 4.0.zip"
adimport --from E:\
adimport --from C:\Copies\Snoopy
adimport --download ad10
adimport --download swse
adimport --download disney
adimport --download flintstones
adimport --download all
adimport --list-packages
adimport --remove tt
```

`--list-packages` shows which releases are imported, and `--remove <id>`
deletes one (in the settings window, right-click its cover → **Remove …**:
see [Covers](#2-covers)). `adimport --help` lists every option. The exit code is 0 on
success, 1 on an error such as a file that cannot be written, 2 when the
source is not a known release, 3 when verification fails, 4 on a network
error and 5 when you cancel.

## 2. Covers

With two or more releases imported, the settings window shows their box
covers above the module list, every one of them: on two rows when they
don't fit on one (in a short window, smaller covers without their names;
only in the smallest one do they scroll). Click covers to list only the
modules of those releases (with none selected, it lists them all);
right-click a cover for **Show only …** and **Show all releases**. An import fetches the
release's cover picture from the Internet (checked against its published
MD5) or uses the art on the disc; a cover it cannot get is drawn as a plain
box with the release's title, and the import still succeeds. While a
release still shows such a plain cover, **Get the covers** (in the settings
window or the importer) fetches the pictures; from a command prompt,
`adimport --refresh-covers` does the same.

To use a picture of your own, right-click a cover → **Change cover…** (or
click the release's cover in the importer → **Change cover…**), or run
`adimport --set-cover <id> <picture>`. Any picture Windows can read will do
(PNG, JPEG, GIF, BMP, TIFF); it stays on this computer.
`adimport --clear-cover <id>` goes back to the original cover.

To remove a release from this computer, right-click its cover →
**Remove …** (or click its cover in the importer → **Remove …**): a window
asks first, then deletes its modules. Its cover is kept, and you can import
the release again at any time. Close any screen saver that is showing one
of its modules first (the window says so if a file of it is in use).

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

The modules of Delrina's own releases (Intermission 4.0, the two Opus 'n
Bill releases, the Flintstones, The Far Side and Dilbert) have one such
button too, **Configure...**. For an animation it opens Intermission's own
"Animation Player Options" (sound effects and music on or off, colour
options); for their other modules, the module's own window, such as the
banner text of The Far Side's Pterodactyl. Those settings are kept in
`ANTSW.INI`, under `state\<id>\WINDOWS\` in your data folder (such as
`state\farside\WINDOWS\`, with the ids from the table above), and apply
from the next run. Like the Star Wars modules, these always get 640×480,
scaled to fit. Intermission 4.0's Picture Show shows the `.BMP` pictures
of a folder: pick one with its **Configure...** (your own drives are under
`H:\`, in short 8.3 names); until then it shows black, since the emulated
Windows folder it starts with holds no pictures. Its Palette Animator shows
only black on its own: Intermission made it the background of a
MultiSaver group, as in The Machine (Palette). The **Configure...** of
Paradise and The Machine (Palette) opens Intermission's Morph and
MultiSaver editors, which save
the morph or the group itself, under `state\intermission\SAVER\`.

Thirty-six of Intermission 4.0's 54 modules have a second setting,
**Speed**. Intermission ran its modules as fast as the PC allowed, and
these (Dragon Kites, Ant Mine, Wriggly, Fireworks and most of the other
effects) move one small step each time they are called, so how fast they
moved depended on the PC they ran on (the speed options a few of them have
set only how big a step is). **Speed** picks the pace of the emulated PC
for that module: **Normal** is our estimate of a typical PC of 1993 (a 486
running Windows 3.1), and **Slowest**, **Slow**, **Fast** and **Fastest**
run the module at about a quarter, half, twice and four times that pace,
Fastest being the emulated PC at its full speed. Each slider starts where
the module's motion is calm: most at **Normal**, Dragon Kites, Ping and
Bricks at **Slowest**, Wriggly and Snow Flakes at **Slow**, and Space
Shark, whose steps are heavy, at **Fast**. It is kept for each module with
its other settings (**Restore defaults** puts it back where it started);
the live preview restarts with it when you let go of the slider, and
**Preview** and the screen saver use it. Intermission 4.0's other 18
modules keep time by the clock, so they have no **Speed**: its animations
(Golfing Ants, Rapping Pig, Cowboy Singer, Einstein, Flying and the
others), the morph Paradise, and modules such as Timepiece, Maze, Photo
Shoot and Orbs. The other releases' modules have none either.

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

The Next Generation's modules always get 640×480 too, scaled to fit:
several of them (Science Stations, Encounters, Personnel Files, Officer's
Review, Starfleet Messages) compose their scenes for that screen. Starfleet
Messages has one button, **Edit Custom...**, which edits your own message
(choose "Custom" under **Text** to show it). Officer's Review is a game:
press Caps Lock to start the exam, type the number of each answer, and
press Caps Lock again to stop.

Johnny Castaway always gets 640×480 too, scaled to fit: the program paints
its scene in the middle of the screen, as it did on a 640×480 screen of
1992. It has one button, **Setup...** (Windows 3.1's Control Panel's name
for it), which opens the program's own settings window: **Sounds**,
**Load Background**, **Start of day** and **Password**. It keeps those
settings, and how far its story has gone, in its `SCRANTIC.INI`, under
`state\castaway\WINDOWS\` in your data folder, as a real install kept
them, so the story goes on from one run to the next; deleting
`state\castaway` starts it over. Keys and the mouse never reach it: they
end the screen saver as they do any module's.

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
volume. The modules of Delrina's own releases play wave effects (a few
have none) and no music, but for Intermission 4.0's Rapping Pig, whose song
is MIDI. **Preview** plays sound with the values you have not saved yet; the
small live preview never does.

The **Look** menu changes how the screen saver draws the modules' pictures.
Its link, at the end of the **Stretch to fit the screen** row, says what you
chose: **Look: Sharp pixels**, or **Look: CRT monitor, ambient glow**, say
(a narrow window shortens it; the menu always shows everything). Click it,
or press Alt+K, to open the menu. It has two groups, **Look** and **Bars**,
with the current choice of each checked; pick one and the menu closes, and
**OK** saves it. They start at **Sharp pixels** and **Black**, the way it
has always drawn them, and as long as they stay there nothing changes. The
other choices draw with your graphics card, through Direct3D 11. **Look**:

- **Sharp pixels** (the default): every pixel of the module's picture a
  crisp block.
- **CRT monitor**: the picture as a 1990s VGA monitor showed it, with a
  soft beam, faint scanlines, a fine mask and a little glow. It suits every
  module, and looks best on a 1440p or 4K monitor: its scanlines need about
  three of the monitor's pixels for each line of the picture, as a 480-line
  picture gets on a 1440p monitor (and a 720-line one on a 4K monitor).
  With fewer, as on a 1080p monitor, they are left out, since they would
  beat against the monitor's own pixels, and the rest of the look stays.
- **Curved CRT monitor**: the same, behind curved glass: the picture bows
  out a little, with rounded corners that are a little darker.
- **Smooth**: redraws the picture with an edge-following upscaler
  (Super-xBR), so outlines come out as curves and clean diagonals instead
  of steps. It suits flat cartoon art with dark outlines best, such as The
  Far Side's, Dilbert's, Disney's, the Looney Tunes' or the Simpsons'
  modules, the more so on a large monitor. Dithered backgrounds (Johnny
  Castaway's sea and sky) turn into a wormy texture, photographs and
  halftones (Marvel's posters, the Star Wars stills) gain little and look a
  little painted, and single-pixel stars come out softer and dimmer: for
  those, keep Sharp pixels or a CRT.
- **Shader preset:** and the preset's file name, such as **Shader preset:
  crt-lottes.slangp**, only while `settings.ini` names a shader preset of
  your own ([below](#your-own-shader-preset-advanced)).

**Bars**, the menu's second group, is what fills the bars beside a picture
that doesn't fill the screen: **Black** (the default), or **Ambient glow**,
a blurred, dimmed copy of the picture. It is for the modules that always
get 640×480 (Intermission's, Star Wars' among them, both Star Trek
releases', ScreamSavers', Marvel's and Johnny Castaway) on a widescreen
monitor, and for After Dark's own on an ultrawide or portrait one; with
**Stretch to fit the screen** checked, the 640×480 modules have no bars.

The live preview in the settings window, its module pictures and the small
preview in Screen Saver Settings always show the plain picture, with black
bars; **Preview** shows the look you chose, as the screen saver and its
window ([below](#show-it-in-a-window-a-be-right-back-screen)) do. A PC whose
graphics can't draw the look (Direct3D 11 is needed), or draw it fast
enough, shows sharp pixels and black bars instead.

## Ending it, and playing

Any key except Shift, Ctrl, Caps Lock and Num Lock, a click, the mouse
wheel, moving the mouse or switching away (the Windows key, Alt+Tab,
Ctrl+Alt+Del) ends the screen saver. Caps Lock never does: in some modules
it changes something (it scares the fish, changes the colours, or in many
Looney Tunes, Disney and Next Generation modules moves on to the next
scene) or starts a game, as in Rodger Dodger, You Bet Your Head, Simpsons
Trivia, Officer's Review, Mime Hunt, Frankenscreen, Marbles and Pinocchio
(the Wishing Star: Pinocchio follows the mouse).

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

## Show it in a window (a "be right back" screen)

`LongAfterDark.exe` is the screen saver again, under another name. With
`/window` it shows the modules in an ordinary window instead of full
screen, for OBS, or any other program that captures a window, to show
while you are away. From PowerShell, in the programs' folder:

```powershell
.\LongAfterDark.exe /window /size 1920x1080 /random
```

| Switch | What it does |
|---|---|
| `/window` | An ordinary window titled **Long After Dark**, with a taskbar button, which you can move, resize, minimize and maximize. |
| `/size WxH` | The size of the picture inside the window, in pixels whatever the display scaling: 1280x720 unless you say, from 160x120 to 7680x4320. The window may be larger than the screen. |
| `/module <module>` | That module: its id, such as `ad40.toasters`, or its name as the settings window lists it, in quotes if it has spaces (`"Flying Toasters!"`). A name several releases share needs the id (the program lists the ids). |
| `/random` | The modules **Random** plays in the settings window (those checked there, in the releases selected), changing as often as **Change module every** says. With `/module`, that module plays first. |
| `/help` or `/?` | A message listing the switches. |

The switches go in any order, and `-` works as well as `/`. With neither
`/module` nor `/random`, the window shows what the screen saver would. Each
module's own settings (its options, Intermission 4.0's Speed), the
Resolution, **Stretch to fit the screen**, the **Look** menu and the sound
apply as they do to the screen saver; the monitor settings don't, as it is
one window. Every module starts on black, never on a picture of your
desktop. With `/window`, a switch it doesn't know, a size out of range or a
module it can't find opens a message saying what is wrong, and nothing
runs; without `/window`, a switch it doesn't know is skipped and the
settings window opens, as Windows' own screen savers do.

Windows starts a `.scr` with `/S` and nothing else, whatever is written
after its name (from PowerShell, a shortcut and Start-Process alike), so
these switches work only with `LongAfterDark.exe`. Run without switches,
it opens the settings window, as the `.scr` does.

The window stays open whatever the keyboard and the mouse do: no key,
click or move closes it, Caps Lock and Num Lock start no game, and the
pointer is never hidden or held. Close it with its close button, Alt+F4,
or **Close window** on its taskbar button's right-click menu (clicking the
taskbar button only minimizes and restores it). While it is open, the
window holds a "display required" power request, as a video player does,
which Windows documents as keeping the display on even without input
(`powercfg /requests`, as an administrator, lists it). That should also
keep the screen saver from starting and the computer from locking after a
while, though this has not been tested on a real desktop.
On battery, a laptop with Modern Standby ends the request's
"system required" part five minutes after the sleep timeout.
Win+L, Sleep, the power button and a laptop's lid still do what they do.
Several windows can be open at once, each with its own switches.

Resizing the window scales the picture to the new size. Once the size
settles, a module whose screen follows the display's shape (After Dark's)
starts again in the new shape; a module with a 640x480 screen of its own
(Intermission's, both Star Trek releases', ScreamSavers', Marvel's, Johnny
Castaway) keeps its shape, with bars, unless **Stretch to fit the screen**
is checked.

In OBS, add a **Window Capture** source:

- **Window:** `[LongAfterDark.exe]: Long After Dark`;
- **Capture Method:** **Windows 10 (1903 and up)**;
- **Window Match Priority:** **Match title, otherwise find window of same
  type**, so that the source finds the window again each time you open it
  (the settings window and the program's messages are titled Long After
  Dark too, but only window-mode windows are of this type);
- **Capture Cursor:** unticked, as the window never hides the pointer,
  which would otherwise show on the stream whenever it rests over the
  window.

The window plays sound as the primary monitor's screen saver does (Sound
and Volume in the settings window). For the stream to have it, add an
**Application Audio Capture** source for the same window.

## Your own shader preset (advanced)

The screen saver can also draw the modules through a RetroArch shader
preset, a `.slangp` file such as those of libretro's
[slang-shaders](https://github.com/libretro/slang-shaders), run by
[librashader](https://github.com/SnowflakePowered/librashader). Long After
Dark ships neither, and supports librashader no further than loading it:
what a preset draws, and whether it runs at all, is up to the preset and
librashader.

1. Download librashader's 64-bit Windows build from its releases page
   (`librashader-x86_64-windows-….zip`), version 0.5.0 or later (0.5.1 and
   0.12.0 were tried; a later one that changes librashader's C interface is
   refused), and put the `librashader.dll` from it next to
   `LongAfterDark.scr`, or in a folder named `librashader` in
   `%LOCALAPPDATA%\LongAfterDark`. It needs the Microsoft Visual C++
   2015–2022 x64 runtime and DirectX's `D3DX9_43.dll` (the DirectX
   End-User Runtime), which many PCs have already.
2. Keep the preset with the shader and picture files it names, which it
   finds by their place relative to itself: take the whole slang-shaders
   folder, say, as its presets name files in its other folders.
3. With the settings window closed, add a line naming the preset under
   `[Saver]` in `settings.ini` (in `%LOCALAPPDATA%\LongAfterDark`), such as
   `ShaderPreset=C:\Shaders\slang-shaders\crt\crt-lottes.slangp` (a
   relative path is taken from that folder).
4. Open the settings window: the **Look** menu now offers **Shader preset:
   crt-lottes.slangp**. Choose it, and try it with **Preview**.

Not every preset suits the modules' 480-line pictures: crt-royale, for
one, takes them for interlaced ones, and their thin lines flicker.

A preset needs graphics of Direct3D feature level 11.0 or later. The first
time a preset runs, librashader can take some seconds to build it, and the
screen saver shows sharp pixels meanwhile; librashader keeps what it built
in a folder of its own, `%LOCALAPPDATA%\librashader`, so the next start is
quicker (delete the folder to clear it).

Without a `librashader.dll` it can use, or with a preset that can't be read
or doesn't compile, the screen saver draws sharp pixels with black bars
instead, as it does when a preset is too heavy for the graphics card, and
the log of its last run, `logs\saver-last.log` in the same folder as
`settings.ini`, says why.

## Where your files are

Everything is in `%LOCALAPPDATA%\LongAfterDark` (paste that into Explorer's
address bar):

| Folder or file | What it holds |
|---|---|
| `assets\win\` | the imported modules, one folder per release, the module list `catalog-win.json`, and the releases' box covers (`covers\`) |
| `downloads\` | Internet Archive downloads, reused if you import the same release again |
| `settings.ini` | the screen saver's settings |
| `state\` | what the modules save themselves (message texts, chosen pictures, high scores, the Intermission modules' settings, Sounder's folder, Marvel's poster choices, Johnny Castaway's settings and story), per release |
| `thumbs\` | the settings window's module pictures |
| `logs\saver-last.log` | how the last screen saver run went and why it ended |
| `librashader\` | `librashader.dll`, if you put it there for a shader preset ([above](#your-own-shader-preset-advanced)) |

## Updating

Close the settings window and make sure the screen saver (and any window
of it) is not running (Windows locks running programs), then replace the
programs, `LongAfterDark.exe` with them, with the new versions. Imported
releases, downloads and settings are kept.
`adimport --version` says which version you have, and so does each
program's Properties → Details in Explorer.

## Not done yet

- **Speed.** Each module's pace follows a model of a mid-1990s PC (Swirling
  Magic's too-fast pace is fixed); not every module has been compared with
  the original yet. The **Normal** of Intermission 4.0's **Speed** slider
  is an estimate of a 1993 PC, not measured on one. Marvel's poster
  transitions (wipes, irises, blinds and the like) show at once where the
  original swept them over about half a second.
- **Chameleon** (Totally Twisted and 10th Anniversary): after about half a
  minute a stray icon covers the "Accessories" label, a known difference
  not fixed yet.
- **Johnny Castaway's Password** (in its **Setup...**) has no effect:
  input ends it at once, without asking for the password.
- **The Flintstones' DictaBird** recorded from a microphone, which the
  emulated PC doesn't offer: it shows only its own line, "Sound Support Not
  Available For FM-DictaBird", as on a 1994 PC that could not record.
- **Modules that took the mouse.** Nine of the modules of On the Road
  Again and the Flintstones, like three of The Far Side's and Dilbert's,
  took the mouse and keyboard under Intermission instead of ending; here
  they run as ordinary screen savers, and input ends them.
- No installer or code signing yet.
