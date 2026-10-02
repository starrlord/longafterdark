# Long After Dark on Linux

Long After Dark runs on 64-bit Linux too. Its emulator, `adhostwin.exe`,
and its importer, `adimport.exe`, are the same Windows programs as on
Windows, run under Wine. A Linux program of its own, `longafterdark`, runs
the emulator and shows what it draws: full screen, in a window, or as one
of XScreenSaver's display modes. It keeps the Windows screen saver's rules
for ending it and for the modules' games, and the picture keeps its shape.

## How it differs from Windows

The same as on Windows:

* all sixteen releases and their 328 modules, imported by the same importer
  and checked file by file against the originals;
* the modules' sound effects, their MIDI music and the Simpsons' voices,
  from one player only (the primary monitor's);
* the Caps Lock games (Officer's Review among them), and Star Trek's Final
  Exam with Num Lock;
* the picture's shape: After Dark modules get a screen of the display's
  shape, 480 lines high, or 720 with `--lines 720` (the Windows setting
  **Sharp — 720 lines**); Star Wars, Far Side, Dilbert, Star Trek (both
  releases), ScreamSavers and Marvel modules, and Johnny Castaway, always get 640×480, with black bars at the
  sides on a widescreen display, or stretched to fill it with `--stretch`
  (the Windows setting **Stretch to fit the screen**);
* what the modules save themselves (message texts, high scores, the Star
  Wars and Star Trek modules' settings files, how far Johnny Castaway's
  story has gone) is kept from run to run.

Different:

* **No settings window.** You choose with the options below, or under
  XScreenSaver in its settings. There are no box covers, and no live
  preview besides XScreenSaver's.
* **No module options.** The modules' own sliders and choices, and buttons
  such as Fish World's **Select Fish…**, the **Configure...** of a Star
  Wars, Far Side or Dilbert module or Johnny Castaway's **Setup...**,
  aren't available: every module runs with its defaults.
* **One monitor on its own.** `longafterdark` plays on the primary monitor
  and keeps the others black, as the Windows setting **Primary monitor
  only** does; XScreenSaver runs one on every monitor, all showing the same
  module unless you check **A different module on each monitor**, as on
  Windows.
* **No games under XScreenSaver.** XScreenSaver ends the screen saver on
  any key or mouse move; the games are played in `longafterdark`'s own
  full-screen mode.
* **Your files** are in other places ([Your files](#your-files)).

## What you need

* A 64-bit PC (x86-64) running Linux with glibc 2.35 or newer: Ubuntu
  22.04, Debian 12, Fedora 36, Linux Mint 21 or later.
* An X11 desktop, on a display of 15, 16, 24 or 32 bits per pixel (an
  8-bit, 256-colour display is refused). In a Wayland session it would run
  through Xwayland like any X11 program (not tried yet); XScreenSaver needs
  an X11 session.
* 64-bit Wine. Tried with Wine 9.0 (Ubuntu 24.04), 8.0 (Debian 12) and
  6.0.3 (Ubuntu 22.04). A 32-bit Wine prefix is refused; the default one is
  64-bit.
* The X11 libraries libX11, libXext and libXrandr, and the CA certificates,
  which the importer's downloads need. Desktops have them all.
* For the modules' text to look as it did on Windows: Microsoft's core
  fonts. Without them Wine uses similar fonts of its own.
* For the modules' MIDI music: a MIDI synthesizer on ALSA's sequencer, such
  as FluidSynth ([Music](#music)). Without one the music is silent; the
  sound effects and voices still play.

On Debian and Ubuntu:

```bash
sudo apt install wine wine64 libx11-6 libxext6 libxrandr2 ca-certificates
sudo apt install ttf-mscorefonts-installer       # the fonts (Ubuntu: multiverse; Debian: contrib)
sudo apt install fluidsynth fluid-soundfont-gm   # the music
```

On other distributions, install the same things from their packages:
64-bit Wine, the three X libraries, the CA certificates, FluidSynth with a
General MIDI sound font, and Microsoft's core fonts (or run
`winetricks corefonts`).

### Music

The modules play their music through Windows' MIDI mapper, which Wine sends
to a synthesizer on ALSA's sequencer. Debian's and Ubuntu's `fluidsynth`
package runs FluidSynth as a service of your own, with the General MIDI
sound font of `fluid-soundfont-gm`. Turn it on once:

```bash
systemctl --user enable --now fluidsynth
```

Start the synthesizer before Long After Dark: the emulator looks for MIDI
devices when it starts.

## Installing

1. Download `LongAfterDark-<version>-linux-x64.zip` from the
   [releases](https://github.com/starrlord/longafterdark/releases) (1.1.0
   and older are for Windows only), or the newest build of `main`,
   `LongAfterDark-linux-x64.zip`, from the
   [latest-main](https://github.com/starrlord/longafterdark/releases/tag/latest-main)
   pre-release, and unzip it where it will stay. It holds a folder,
   `LongAfterDark`:

   | File | What it is |
   |---|---|
   | `longafterdark` | the Linux player and screen saver |
   | `adhostwin.exe` | the emulator, which `longafterdark` runs under Wine |
   | `adimport.exe` | the importer, which copies the modules from your discs |
   | `longafterdark.xml` | the player's entry for XScreenSaver's settings |
   | `LongAfterDark.scr` | the Windows screen saver (for Windows) |
   | `README.txt`, `LICENSE.txt`, `licenses/` | a short guide, and the licences |

2. Keep the programs together: `longafterdark` looks for `adhostwin.exe`
   and `adimport.exe` in the folder it is really in. To run it by name,
   put a symbolic link to it in a folder on your `PATH`; it follows the
   link to find them:

   ```bash
   sudo ln -s "$PWD/LongAfterDark/longafterdark" /usr/local/bin/longafterdark
   ```

   A copy of `longafterdark` on its own elsewhere can't find the emulator.

To update, unzip the new release over the folder while `longafterdark` is
not running. Your imports, downloads and the modules' saved state are kept
elsewhere.

## Importing your releases

`longafterdark --import` runs the importer under Wine with the options that
follow it; with none, it opens the importer's window:

```bash
cd LongAfterDark
./longafterdark --import                                # the importer's window
./longafterdark --import --download simpsons            # from the Internet Archive (2.6 MB)
./longafterdark --import --download all                 # all sixteen releases (664 MB), those imported already too
./longafterdark --import --image ~/Downloads/afterdark-20b_startrek.zip
./longafterdark --import --image ~/Downloads/"After Dark - Scream Savers.zip"   # its disks in DISK1, DISK2, ... folders
./longafterdark --import --image disk1.img --image disk2.img
./longafterdark --import --from /media/$USER/AFTERDARK  # a mounted CD, or a copy of one
./longafterdark --import --list-packages                # which releases are imported
./longafterdark --list                                  # the modules, with their ids
```

It hands the importer your paths in the form a Windows program needs,
keeps Wine's messages quiet, and stops a new Wine prefix from offering to
install Mono and Gecko, which nothing here needs (`wine adimport.exe`
works too, given Windows paths such as `Z:\home\you\disc.iso`). The
release ids are `deluxe`, `ad10`, `ad32`, `tt`, `simpsons`, `swse`,
`startrek`, `marvel`, `snoopy`, `looney`, `screams`, `disney`, `farside`,
`dilbert`, `tng` and `castaway`. The
sources the importer takes (a disc or floppy image; a ZIP or 7z of the
install files, of the floppy images, or of the install disks in `DISK1`,
`DISK2`, … folders; a folder; or a download), and how it checks them, are in
[INSTALL.md](INSTALL.md#1-import-your-releases) (read `adimport` there as
`./longafterdark --import`); `./longafterdark --import --help` lists its
options.

The importer puts the releases in the Wine prefix's
`%LOCALAPPDATA%\LongAfterDark\assets` ([Your files](#your-files)), where
`longafterdark` finds them. To keep them in a folder of your choice, name
that folder both times, before `--import`:

```bash
./longafterdark --assets-dir ~/LongAfterDark-assets --import --download simpsons
./longafterdark --assets-dir ~/LongAfterDark-assets
```

or set `AD_ASSETS_DIR` to it for both.

The first Wine program you run makes the Wine prefix, `~/.wine` (or
`$WINEPREFIX`), which takes from a few seconds to a minute.

## Running it

```bash
./longafterdark                    # full screen: every imported module in turn, 5 minutes each
./longafterdark toasters           # Flying Toasters! only
./longafterdark -w                 # in a 640x480 window
./longafterdark -w -s 2 simptriv   # Simpsons Trivia in a 1280x960 window
./longafterdark --cycle 120        # a new module every 2 minutes
./longafterdark -r toasters        # Flying Toasters! first, then all the others in turn
./longafterdark --lines 720        # After Dark modules at 720 lines
./longafterdark --no-sound
```

A module is named by its id (`ad40.toasters`), its name
(`"Flying Toasters!"`), its path in the catalog, or its id without the
release (`toasters`); `--list` shows the ids and names. A name that fits
several modules is refused with a list of them: use one's id. With nothing
imported, it shows the emulator's test pattern.

### Options

| Option | What it does |
|---|---|
| `-f`, `--fullscreen` | Full screen on the primary monitor, as a screen saver (the default); the other monitors go black. |
| `-w`, `--window` | In a window of 640×480. |
| `-s`, `--scale <n>` | The window `n` times as large (1 to 8). |
| `--lines 480` or `720` | The screen After Dark modules get: 480 lines (the default) or 720, as wide as the display's shape. Star Wars, Far Side, Dilbert, Star Trek, ScreamSavers and Marvel modules, and Johnny Castaway, always get 640×480. |
| `--stretch` | Star Wars, Far Side, Dilbert, Star Trek, ScreamSavers and Marvel modules, and Johnny Castaway, fill the window, stretched, instead of keeping their 4:3 shape with black bars. XScreenSaver's settings call it **Stretch to fit the screen (no black bars)**; its small preview always keeps the shape. |
| `--module <module>` | The module to run, named as above: the same as naming it at the end of the line. XScreenSaver's settings write it this way. |
| `-r`, `--random` | Every module in turn even when one is named (it plays first). |
| `--cycle <seconds>` | How long each module plays in the rotation (default 300; 0 never changes it). |
| `--different-modules` | Under XScreenSaver (`--root`): each monitor's player goes through the modules in a random order of its own, rather than all of them showing the same module ([How it runs there](#how-it-runs-there)). It changes nothing in the other modes. |
| `--fps <n>` | Frames per second (1 to 240; default 60). |
| `--sound`, `--no-sound` | Sound on (the default) or off. |
| `--volume <0-100>` | The modules' volume (default 50). |
| `--assets-dir <folder>` | Where the imported releases are (default `$AD_ASSETS_DIR`, else the Wine prefix's). |
| `--test-pattern` | The emulator's test pattern instead of a module. |
| `-l`, `--list` | Lists the imported modules. |
| `--import <options>` | Runs the importer under Wine with the options after it. |
| `--verbose` | Prints what the player and the emulator do. |
| `-v`, `--version`, `-h`, `--help` | The version; every option. |
| `--root` (or `-root`) | XScreenSaver's mode ([XScreenSaver](#xscreensaver)). |
| `-window-id <id>` | Draws in another program's window: XScreenSaver's preview. |

## Ending it, and playing

Full screen, the rules are the Windows screen saver's:

* Any key except Shift, Ctrl, Caps Lock and Num Lock, a click, the mouse
  wheel or moving the mouse ends it, on any monitor, and so do switching to
  another window and a screen locker taking over. It never grabs the
  keyboard, so screen lockers keep working.
* Caps Lock never ends it. In some modules it does something (it scares
  the fish) or starts a game: Rodger Dodger, You Bet Your Head, Simpsons
  Trivia, Mime Hunt, Frankenscreen, Marbles, Magic Turtle's editor, How to
  Draw… While a game is playing, keys, clicks and the mouse belong to it,
  the pointer stays in the picture, and the module isn't changed. Press
  Caps Lock again to stop playing (the next key or move then ends the
  screen saver), or Alt or F10 to end it at once.
* Num Lock never ends it either. In Star Trek's Final Exam it starts the
  exam: type the number of your answer, on the top row or the keypad.
  Moving the mouse ends the exam, and the screen saver with it.

In a window (`-w`) the keys, clicks and moves go to the module, except Alt,
F10, keys pressed while Alt is held and the mouse wheel, which do nothing
there. Esc or `q` closes the window when no game is playing, its close
button always does, and so does a module that ends by itself (Final Exam,
when the mouse moves during the exam). Under XScreenSaver, XScreenSaver
decides: any key or mouse move ends it (or asks for your password, when it
locks). Caps Lock and Num Lock don't reach the module there or in the
preview, so no game starts behind XScreenSaver's password prompt.

## XScreenSaver

[XScreenSaver](https://www.jwz.org/xscreensaver/) can run `longafterdark`
as one of its display modes, on every monitor, whenever you're idle. It
needs an X11 session (at the login screen: **GNOME on Xorg**, **Ubuntu on
Xorg** or **Plasma (X11)**; XScreenSaver doesn't work under Wayland) in
which the `xscreensaver` daemon runs, instead of the desktop's own screen
saver. Its settings program is `xscreensaver-settings`
(`xscreensaver-demo` in XScreenSaver 5, such as Ubuntu 22.04's).

### Setting it up

1. Install XScreenSaver (`sudo apt install xscreensaver`), and have its
   daemon start with your session
   ([below](#starting-xscreensaver-with-your-session)). Put `longafterdark`
   on your `PATH` with the symbolic link above, or write its full path in
   step 3.
2. Copy its entry into XScreenSaver's configuration folder, so that
   XScreenSaver's settings show its description and options:

   ```bash
   sudo cp longafterdark.xml /usr/share/xscreensaver/config/
   ```

3. Add it to the `programs:` list in `~/.xscreensaver`. That file holds all
   your XScreenSaver settings: edit it, and never copy anything over it. If
   you have none yet, open `xscreensaver-settings` and change any setting
   (**Mode**, for one), which writes it. Close `xscreensaver-settings`, then
   add this line right after the line `programs:`, with a text editor:

   ```
   "Long After Dark"  longafterdark --root  \n\
   ```

   Every line of that list ends with `\n\`. XScreenSaver notices the change
   by itself.
4. Open `xscreensaver-settings` and choose **Long After Dark** in the list:
   the preview plays in the pane, and **Settings...** shows its options. To
   show nothing else, set **Mode** to **Only One Screen Saver**.

### Starting XScreenSaver with your session

The `xscreensaver` daemon has to run in your session, and the desktop's own
screen blanking and lock have to be off, or they blank the screen before
XScreenSaver does. XScreenSaver's manual (`man xscreensaver`) says how for
each desktop; in short:

1. **Start it when you log in.** GNOME, KDE Plasma, Xfce, MATE, Cinnamon,
   LXQt and LXDE start what `~/.config/autostart` lists, so put an entry
   for it there:

   ```bash
   mkdir -p ~/.config/autostart
   cat > ~/.config/autostart/xscreensaver.desktop <<'EOF'
   [Desktop Entry]
   Type=Application
   Name=XScreenSaver
   Exec=xscreensaver -no-splash
   EOF
   ```

   It runs from your next login on; `xscreensaver -no-splash &` starts it
   now. (One dash: XScreenSaver 5, such as Ubuntu 22.04's, refuses
   `--no-splash`.) With a window manager alone (i3, Openbox), start
   `xscreensaver -no-splash` from the window manager's own startup file
   instead. XScreenSaver's systemd unit (`systemctl --user enable
   xscreensaver`) is no substitute: on Ubuntu 22.04 and Debian 12 it
   doesn't start with the session, and on Ubuntu 24.04 only with GNOME and
   KDE.
2. **Turn off the desktop's own blanking and lock**, since XScreenSaver's
   settings take their place (when to blank, lock and power off the
   monitor):
   * GNOME: in **Settings**, the screen blank, the automatic screen lock
     and the automatic suspend (under **Power** and **Privacy**);
   * KDE Plasma: in **System Settings**, the automatic lock
     (**Screen Locking**) and the screen energy saving and suspend
     (**Power Management**);
   * Xfce, MATE, Cinnamon and others: their own screen saver or locker
     (`xfce4-screensaver`, `mate-screensaver`, `cinnamon-screensaver`,
     `light-locker`), turned off in their settings or removed, as the
     manual says.

### How it runs there

* **Settings...** has the module to show (**Module**: an id from
  `longafterdark --list`, such as `ad40.toasters`; empty, every module in
  turn), how often the module changes (0: never), **A different module on
  each monitor** (below), **Stretch to fit the screen (no black bars)**,
  the resolution of After Dark modules, **Sound** and **Volume**. They are
  saved on the entry's line in `~/.xscreensaver`, as the options
  `--module`, `--cycle`, `--different-modules`, `--stretch`, `--lines 720`,
  `--no-sound` and `--volume`, and shown again next time.
* XScreenSaver's own **Cycle After** setting (10 minutes unless you change
  it) also stops `longafterdark` and starts it again. Unless a module is
  chosen, it comes back on the module the clock has reached (below), the
  same one if its turn isn't over; with **A different module on each
  monitor**, on another one. Set it to 0 to leave the changes to Long After
  Dark's own setting.
* To show one module only, choose it under **Module**, or write it on the
  entry's line as an option: `longafterdark --root --module ad40.toasters`.
  Not as a bare word (`longafterdark --root ad40.toasters`): that works
  until you save the entry's **Settings...**, which drops a word
  `xscreensaver-settings` doesn't know.
* On several monitors each has its own `longafterdark`, and only the one
  on the primary monitor plays sound. They show the same module and change
  it together, without a word between them: the order and the changes
  follow the clock, so with the default 5 minutes the module changes at
  :00, :05, :10 and so on (while the monitor is off, a change waits for it
  to come back on). The first module plays until the next change, or, when
  that is less than 10 seconds away, until the change after it. With 0
  under **Change module every** they all keep one module. A module that
  fails on one monitor gives way there to the next one early, and the next
  change brings the monitors together again.
* With **A different module on each monitor** (`--different-modules`),
  each monitor's player goes through the modules in a random order of its
  own, changing module a cycle after it last did.
* The preview in `xscreensaver-settings` is always silent, whatever
  **Sound** says, and plays a small 320×240 picture at 30 frames a second.
  Its emulator, and the Wine processes it starts, run at a low priority
  (nice 10), as XScreenSaver runs the screen saver itself.
* When XScreenSaver stops it (a key, the mouse, a lock), the module's sound
  is stopped properly first.
* Without XScreenSaver, don't use `--root` yourself: it draws on the desktop
  background, beneath your windows.

## Your files

| What | Where |
|---|---|
| The imported releases | The Wine prefix's `%LOCALAPPDATA%\LongAfterDark\assets`, which is `~/.wine/drive_c/users/<you>/AppData/Local/LongAfterDark/assets` (with Wine 6, such as Ubuntu 22.04's: `~/.wine/drive_c/users/<you>/Local Settings/Application Data/LongAfterDark/assets`), or the folder `--assets-dir` or `AD_ASSETS_DIR` names. |
| Internet Archive downloads | The Wine prefix's `%LOCALAPPDATA%\LongAfterDark\downloads`, beside the default `assets`. |
| What the modules save themselves (message texts, high scores, Star Wars' `SWSE.INI`, Star Trek's `AD_PREFS.INI`), per release | `~/.local/share/longafterdark/state` (that is, `$XDG_DATA_HOME/longafterdark/state`; `AD_SCR_STATE` names another folder). Deleting a release's folder there brings back its defaults. |
| What you chose | The command line, or the entry's line in `~/.xscreensaver`. |

`~/.wine` stands for `$WINEPREFIX` when you set it.

## Troubleshooting

* **"no modules imported … showing the test pattern"**: import a release
  first, or point `--assets-dir` at your imports. The message says where
  it looked.
* **"cannot find adhostwin.exe"**: this `longafterdark` isn't in the
  release folder. Run the one there, or a symbolic link to it.
* **"Wine is needed …"**: install Wine, or name its loader in
  `AD_WINE_BIN`.
* **"the Wine prefix … is 32-bit"**: the emulator is a 64-bit program. Use
  a 64-bit prefix: a new `WINEPREFIX`, made without `WINEARCH=win32`.
* **A download fails with "WinHTTP error (12157)"**: install the CA
  certificates (`ca-certificates`).
* **A download stalls**: stop the importer (Ctrl+C in the terminal, or
  close its window) and start it again.
* **No music**: start a MIDI synthesizer ([Music](#music)), then Long After
  Dark.
* **"… could not be started"** on the screen: the emulator failed three
  times in a row (it ended within 5 seconds of starting, or without a
  frame), the last time without showing the module (its files damaged,
  say, or Wine unable to run it). The player keeps trying, every 30
  seconds at most, and the first frame replaces the message; a rotation
  moves on to its next module.
* **To see why**, for that or a black screen: run it in a terminal with
  `--verbose` to see what the player and the emulator say.
  `AD_SCR_LOG=<file>` and `AD_SCR_HOSTLOG=<file>` write the same to files
  (for XScreenSaver, set them where the daemon starts). Wine's own messages
  are off unless you set `WINEDEBUG` (for example `WINEDEBUG=err+all`).
* **Slow, or busy, on a remote display (`ssh -X`)**: the player can't share
  memory with a remote X server, so it sends every frame through the X
  protocol. It works, at a higher CPU cost.
* **"… no TrueColor visual …"**: the display is in 256-colour mode; switch
  it to 24 bits.

## Environment

| Variable | What it does |
|---|---|
| `WINEPREFIX` | The Wine prefix the emulator and the importer run in (default `~/.wine`). |
| `AD_ASSETS_DIR` | The imported releases' folder, as `--assets-dir`. |
| `AD_SCR_STATE` | Where the modules' saved state goes (default `$XDG_DATA_HOME/longafterdark/state`, that is `~/.local/share/longafterdark/state`). |
| `AD_SCR_SOUND=0` | Sound off, whatever the options say. |
| `AD_SCR_LOG=<file>` | Adds the player's log to that file. |
| `AD_SCR_HOSTLOG=<file>` | Adds the emulator's messages to that file (otherwise they are dropped). |
| `AD_HOST_EXE`, `AD_IMPORT_EXE` | The emulator and the importer, when they are not beside `longafterdark`. |
| `AD_WINE_BIN` | The Wine loader (default: `wine`, else `wine64`, on the `PATH`). |
| `WINEDEBUG`, `WINEDLLOVERRIDES` | Passed on to Wine when set. Otherwise the player sets `WINEDEBUG=-all`, and `WINEDLLOVERRIDES=mscoree=d;mshtml=d` so that a new prefix doesn't ask to install Mono and Gecko. |

`longafterdark` exits with 0 when it ends normally (a key, the mouse, its
window closed, or a signal such as XScreenSaver's), 1 on an error (no Wine,
no emulator, no display, a 32-bit prefix, a display it can't draw on, or a
module's "could not be started" still on the screen when it ends), and 2
for a command line it can't use (an unknown option, a missing or bad
value, two modules) or a module name that matches no module, or several.

## How it works

`longafterdark` runs `adhostwin.exe` under Wine for one module at a time
and reads its frames from the emulator's output, the protocol the Windows
screen saver uses too ([DESIGN.md](DESIGN.md) §1,
[host/core/README.md](../host/core/README.md)). It asks for 60 frames a
second, and none while the monitor is powered off. It restarts an emulator
that ends, stalls for 20 seconds or shows nothing within 90, for as long as
it runs (never while the monitor is off): a quarter of a second after a
good run, half a second after a failed one (one that showed no frame, or
ended within 5 seconds of its start), twice as long after each further
failed run in a row, 30 seconds at most. After three failed runs in a row a
rotation moves on to its next module, and if the last run showed not a
single frame, the screen says the module could not be started, until a
frame comes. It hands the emulator its paths in Windows form, through the
Wine prefix's drives. Full screen, it puts a black window of its own over
each monitor its picture doesn't cover, wherever the window manager puts
its window; a click or a move there counts as on the picture. When it
ends, it tells the emulator to quit first, so that the sound stops
cleanly. Its input rules are those of [INTERACTION.md](INTERACTION.md) §4,
and §4.5 there says where Linux differs.

## Building it

To build the programs and the Linux zip from source, on Ubuntu 24.04 or
later (Ubuntu 22.04's CMake is too old for the Windows programs, though
the player alone builds there):

```bash
sudo apt install cmake git curl unzip xz-utils zip g++ libx11-dev libxext-dev libxrandr-dev wine wine64
bash tools/bootstrap.sh      # once: the compiler and libraries, into third_party/
bash tools/package.sh        # build/dist/LongAfterDark and build/dist/LongAfterDark-linux-x64.zip
bash tools/build-player.sh   # or the player alone: build/linux/longafterdark
```

[BUILDING.md](BUILDING.md#building-on-linux) has the rest: the cross build,
the tests under Wine and the player's own tests.

## Status

The Linux player is new. It has been tried on Ubuntu 22.04 and 24.04 and
Debian 12 in containers, with Xvfb and Xorg's dummy driver (up to three
monitors), Openbox, i3 and Xfce's session, and XScreenSaver 5.45, 6.06 and
6.08. Not tried yet:

* sound on a real sound card (the effects, and MIDI through FluidSynth);
* real monitors: several of them, and their power saving (DPMS), which
  was only simulated;
* GNOME, KDE Plasma and other desktops that composite or hold back new
  windows' focus, and Wayland sessions (Xwayland);
* keyboard layouts other than US English: keys the US layout doesn't have
  (such as ü or ß) reach no module, though they still end the screen
  saver.

Full screen, it finds the monitors when it starts: one plugged in while it
runs isn't blacked out. Under Wine the importer's window may not take files
dropped on it (use its buttons instead), and its icons can show as empty
boxes.

The modules themselves run as on Windows, with the same known differences
([README.md](../README.md#status): Marvel's poster transitions, and
Chameleon's stray icon).
