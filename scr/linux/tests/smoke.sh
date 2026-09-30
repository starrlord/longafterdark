#!/usr/bin/env bash
# Smoke test of the Linux player under Xvfb and Wine:
#
#   bash scr/linux/tests/smoke.sh <player> <adhostwin.exe> [assets root]
#
# For an Ubuntu machine (or CI runner) with Xvfb, xdotool and 64-bit Wine
# (wine64) installed, plus coreutils and procps, and python3 for checks 7,
# 8 and 9 (skipped, with a notice, without it). It exits 0 only when:
#   1. the player presents frames from a host: without assets, the host's
#      test pattern (adhostwin.exe --test-pattern), whose four coloured
#      corner markers must be on the screen; with an assets root, the first
#      module of its catalog, which must put something on the screen;
#   2. the frames keep coming (the screen changes);
#   3. when the host is killed mid-run, the player recovers (a new host,
#      frames again) or ends by itself, within 45 s: it never hangs;
#   4. Shift and Caps Lock don't end it, and its host hears Caps Lock go on
#      and off (CAPS lines); a wake key (a) ends it, with exit status 0,
#      within 10 s;
#   5. on a 16-bit display without MIT-SHM (another pixel format, and the
#      XPutImage way) the same frames show, in the right colours;
#   6. when its host dies while the display is off (the player's
#      --test-display-off simulation of DPMS), the player sleeps, using
#      under a fifth of a core, starts no host until the display is back,
#      and then shows a new host's frames: no busy loop;
#   7. under XScreenSaver's protocol (-root, $XSCREENSAVER_WINDOW) it draws
#      in the window it is given and nowhere else, its host hears no Caps
#      Lock (it takes no input), it follows that window's new size, and ends
#      on SIGTERM (XScreenSaver's stop) with status 0;
#   8. as a preview (--window-id), its host hears no Caps Lock either, and
#      it ends by itself when its window is destroyed: no hang;
#   9. full screen on two monitors (RandR), the frames are on the primary
#      one and the other is black, and a move over the other one ends it;
#  10. no host is left running afterwards, each time.
# Checks 5 to 9 (runs 3 to 7) always use the test pattern: they test the
# player, not a module. The screen is read from the X server itself
# (Xvfb -fbdir keeps it in a file), so no screenshot tool is needed.
# Everything runs in private X servers, a private Wine prefix and a temp
# folder; nothing of the user's is read or written.
#
# Env: SMOKE_WINEPREFIX=<dir> (use that prefix instead of making one),
# SMOKE_FIRST_FRAME=<s> (first-frame wait, default 180: a new prefix is made
# first), SMOKE_KEEP=1 (keep the temp folder and say where), AD_WINE_BIN.
set -u

say() { echo "smoke: $*"; }
die() { say "error: $*" >&2; exit 2; }

[ $# -ge 2 ] || die "usage: smoke.sh <player> <adhostwin.exe> [assets root]"
PLAYER=$(readlink -f -- "$1") || die "no player $1"
HOST=$(readlink -f -- "$2") || die "no host $2"
ASSETS=""
[ $# -ge 3 ] && { ASSETS=$(readlink -f -- "$3") || die "no assets root $3"; }
[ -x "$PLAYER" ] || die "$PLAYER is not an executable"
[ -f "$HOST" ] || die "$HOST is not a file"
for t in Xvfb xdotool od md5sum pgrep timeout; do
  command -v "$t" >/dev/null 2>&1 || die "$t is needed (Ubuntu: sudo apt install xvfb xdotool procps coreutils)"
done
# python3 makes the windows runs 5 and 6 draw in, and sets run 7's two RandR
# monitors (checks 7, 8 and 9); without it they are skipped.
HAVE_PY=1
command -v python3 >/dev/null 2>&1 || HAVE_PY=""
WINE=${AD_WINE_BIN:-}
[ -n "$WINE" ] || WINE=$(command -v wine || command -v wine64 || true)
[ -n "$WINE" ] || die "Wine is needed (Ubuntu: sudo apt install wine64)"
FIRST=${SMOKE_FIRST_FRAME:-180}

TMP=$(mktemp -d /tmp/lad-smoke.XXXXXX)
XVFB_PID=""
WIN_PID=""
PL=""
# A process running the host: under Wine its command line starts with the
# host's path (this script's own command line merely contains it).
HOST_RE="^$(printf '%s' "$HOST" | sed 's/[][\.*^$+?(){}|]/\\&/g')( |$)"
hosts() { pgrep -f -- "$HOST_RE" 2>/dev/null; }
stop_x() {
  [ -n "$XVFB_PID" ] && { kill "$XVFB_PID" 2>/dev/null; wait "$XVFB_PID" 2>/dev/null; }
  XVFB_PID=""
}
stop_window() {
  [ -n "$WIN_PID" ] && { kill "$WIN_PID" 2>/dev/null; wait "$WIN_PID" 2>/dev/null; }
  WIN_PID=""
}
cleanup() {
  [ -n "$PL" ] && { pkill -9 -P "$PL" 2>/dev/null; kill -9 "$PL" 2>/dev/null; wait "$PL" 2>/dev/null; }
  pkill -9 -f -- "$HOST_RE" 2>/dev/null
  WINEPREFIX="$WINEPREFIX" timeout 20 wineserver -k >/dev/null 2>&1
  stop_window
  stop_x
  if [ -n "${SMOKE_KEEP:-}" ]; then say "kept $TMP"; else rm -rf "$TMP"; fi
}
trap cleanup EXIT

export WINEPREFIX=${SMOKE_WINEPREFIX:-$TMP/prefix}
export WINEDEBUG=-all WINEDLLOVERRIDES="mscoree=d;mshtml=d"
export AD_HOST_EXE="$HOST" AD_SCR_STATE="$TMP/state" XDG_DATA_HOME="$TMP/data" AD_SCR_SOUND=0
export AD_SCR_LOG="$TMP/player.log" AD_SCR_HOSTLOG="$TMP/host.log"
unset AD_ASSETS_DIR XSCREENSAVER_WINDOW
[ -n "$ASSETS" ] && export AD_ASSETS_DIR="$ASSETS"

# ---- an X server, its screen kept in a file (XWD format): start_x <depth> [Xvfb args]
# (800x600, or $SIZE)
NX=0
start_x() {
  local depth=$1
  shift
  stop_x
  NX=$((NX + 1))
  local fbdir="$TMP/fb$NX"
  mkdir -p "$fbdir"
  Xvfb -displayfd 3 -screen 0 "${SIZE:-800x600}x$depth" -fbdir "$fbdir" -nolisten tcp "$@" 3>"$TMP/display$NX" \
    2>"$TMP/xvfb$NX.log" &
  XVFB_PID=$!
  local _
  for _ in $(seq 1 100); do [ -s "$TMP/display$NX" ] && break; sleep 0.1; done
  [ -s "$TMP/display$NX" ] || die "Xvfb did not start: $(tail -3 "$TMP/xvfb$NX.log")"
  export DISPLAY=":$(head -1 "$TMP/display$NX")"
  FB="$fbdir/Xvfb_screen0"
  for _ in $(seq 1 50); do [ -s "$FB" ] && break; sleep 0.1; done
  [ -s "$FB" ] || die "Xvfb keeps no screen file (-fbdir)"
  read -r -a HDR <<< "$(od -An -tu4 --endian=big -N 100 "$FB" | tr -s ' \n' '  ')"
  HSIZE=${HDR[0]}; SW=${HDR[4]}; SH=${HDR[5]}; BYTEORDER=${HDR[7]}; BPP=${HDR[11]}; BPL=${HDR[12]}; NCOL=${HDR[19]}
  case $BPP in 16 | 32) ;; *) die "the X screen is $BPP bits per pixel, not 16 or 32" ;; esac
  DATA=$((HSIZE + NCOL * 12))
  # Each channel's shift and width, from the visual's masks.
  local c m s b
  for c in 0 1 2; do
    m=${HDR[$((14 + c))]}
    s=0 b=0
    [ "$m" -gt 0 ] || die "the X screen's visual has no channel masks"
    while [ $((m & 1)) = 0 ]; do m=$((m >> 1)); s=$((s + 1)); done
    while [ $((m & 1)) = 1 ]; do m=$((m >> 1)); b=$((b + 1)); done
    CS[$c]=$s
    CB[$c]=$b
  done
  say "X server $DISPLAY, ${SW}x${SH}, ${BPP} bits per pixel $*"
}

# One pixel as "r g b" (0..255 each).
pixel() {
  local n=$((BPP / 8)) b v=0 i c out=""
  read -r -a b <<< "$(od -An -tu1 -j $((DATA + $2 * BPL + $1 * n)) -N $n "$FB")"
  if [ "$BYTEORDER" = 0 ]; then
    for ((i = n - 1; i >= 0; i--)); do v=$(((v << 8) | b[i])); done
  else
    for ((i = 0; i < n; i++)); do v=$(((v << 8) | b[i])); done
  fi
  for c in 0 1 2; do
    local top=$(((1 << CB[c]) - 1))
    out="$out $(((((v >> CS[c]) & top) * 255 + top / 2) / top))"
  done
  echo $out
}
is_color() {   # is_color x y R|G|B|Y|K
  local r g b
  read -r r g b <<< "$(pixel "$1" "$2")"
  case $3 in
    R) [ "$r" -gt 170 ] && [ "$g" -lt 90 ] && [ "$b" -lt 90 ] ;;
    G) [ "$g" -gt 150 ] && [ "$r" -lt 90 ] && [ "$b" -lt 90 ] ;;
    B) [ "$b" -gt 150 ] && [ "$r" -lt 90 ] && [ "$g" -lt 90 ] ;;
    Y) [ "$r" -gt 170 ] && [ "$g" -gt 170 ] && [ "$b" -lt 90 ] ;;
    K) [ $((r + g + b)) -lt 30 ] ;;
  esac
}
screen_sum() { tail -c +$((DATA + 1)) "$FB" | md5sum | cut -c1-32; }
lit_points() {   # how many of a 8x6 grid of points are not black
  local n=0 x y r g b
  for y in 50 150 250 350 450 550; do
    for x in 50 150 250 350 450 550 650 750; do
      read -r r g b <<< "$(pixel $x $y)"
      [ $((r + g + b)) -gt 60 ] && n=$((n + 1))
    done
  done
  echo $n
}
# The test pattern's corner markers (red top-left, green top-right, blue
# bottom-left, yellow bottom-right: host/core/README.md; a 40th of the
# frame's shorter side, 6 px or more) inside the rectangle x y w h where
# its frame is drawn, by default the screen: looked at 2 px in from each
# corner, inside the marker at any of these sizes.
markers() {
  local x=${1:-0} y=${2:-0} w=${3:-$SW} h=${4:-$SH}
  is_color $((x + 2)) $((y + 2)) R && is_color $((x + w - 3)) $((y + 2)) G &&
    is_color $((x + 2)) $((y + h - 3)) B && is_color $((x + w - 3)) $((y + h - 3)) Y
}
wait_markers() {   # wait_markers <seconds> [x y w h]: the markers there (the scan line may cross one: retried)
  local t=$1 i
  shift
  for i in $(seq 1 $((t * 5))); do markers "$@" && return 0; sleep 0.2; done
  return 1
}
frames_shown() {
  if [ -n "$ASSETS" ]; then [ "$(lit_points)" -ge 2 ]; else markers; fi
}
host_pid() {   # the player's running host (not the --capabilities probe)
  local p
  # Its child, when the Wine loader runs the program in the process it was
  # started as (Debian's, Ubuntu's and WineHQ's packages do); else any
  # process running the host (the X server, the prefix and this host path
  # are this test's own).
  for p in $(pgrep -P "$PL" 2>/dev/null) $(hosts); do
    tr '\0' ' ' < "/proc/$p/cmdline" 2>/dev/null | grep -q -- "--capabilities" && continue
    tr '\0' ' ' < "/proc/$p/cmdline" 2>/dev/null | grep -qE -- "$HOST_RE" || continue
    echo "$p"
    return
  done
}
alive() { kill -0 "$PL" 2>/dev/null; }
# The player's log from the line where the run began (mark_log).
mark_log() { LOGSTART=$(wc -l < "$TMP/player.log" 2>/dev/null || echo 0); }
log_since() { tail -n +$((LOGSTART + 1)) "$TMP/player.log" 2>/dev/null; }
wait_log() {   # wait_log <pattern> <seconds>
  local i
  for i in $(seq 1 $(($2 * 10))); do log_since | grep -q -- "$1" && return 0; sleep 0.1; done
  return 1
}
cpu_ticks() { awk '{print $14 + $15}' "/proc/$1/stat" 2>/dev/null; }   # user + system, in clock ticks
fail() {
  say "FAIL: $*"
  say "--- the player's log (last lines):"
  tail -25 "$TMP/player.log" 2>/dev/null
  say "--- its stderr:"
  tail -10 "$TMP/player.err" 2>/dev/null
  exit 1
}
pass() { say "ok: $*"; }
no_host_left() {   # no_host_left <what>
  local _
  for _ in $(seq 1 50); do [ -z "$(hosts)" ] && break; sleep 0.1; done
  [ -z "$(hosts)" ] || fail "$1: a host is still running after the player ended"
  pass "$1: no host left"
}
# no_caps <what>: Caps Lock on and off reaches no host of a player that takes
# no input (XScreenSaver's window, a preview), as a Windows /p host hears
# none; the player reads the lock every 250 ms, so a second is enough.
no_caps() {
  xdotool key Caps_Lock; sleep 0.4
  xdotool key Caps_Lock; sleep 1
  ! log_since | grep -q "input: caps" || fail "$1: its host heard Caps Lock (a CAPS line), though the player takes no input"
  pass "$1: Caps Lock reached no host"
}

# A plain window standing in for XScreenSaver's (or a preview pane), made
# through libX11 from python3 so no compiler is needed: grey, at x y w h.
# SIGUSR1 moves and resizes it to the second rectangle, SIGUSR2 destroys it.
cat >"$TMP/window.py" <<'EOF'
import ctypes, signal, sys, time
x = ctypes.CDLL("libX11.so.6")
x.XOpenDisplay.restype = ctypes.c_void_p
x.XOpenDisplay.argtypes = [ctypes.c_char_p]
V, UL = ctypes.c_void_p, ctypes.c_ulong
x.XDefaultScreen.argtypes = [V]
x.XRootWindow.argtypes = [V, ctypes.c_int]
x.XRootWindow.restype = UL
x.XCreateSimpleWindow.argtypes = [V, UL, ctypes.c_int, ctypes.c_int, ctypes.c_uint, ctypes.c_uint, ctypes.c_uint, UL, UL]
x.XCreateSimpleWindow.restype = UL
x.XMapWindow.argtypes = x.XDestroyWindow.argtypes = [V, UL]
x.XMoveResizeWindow.argtypes = [V, UL, ctypes.c_int, ctypes.c_int, ctypes.c_uint, ctypes.c_uint]
x.XSync.argtypes = [V, ctypes.c_int]
a = [int(v) for v in sys.argv[1:9]]
d = x.XOpenDisplay(None)
if not d:
    sys.exit(2)
w = x.XCreateSimpleWindow(d, x.XRootWindow(d, x.XDefaultScreen(d)), a[0], a[1], a[2], a[3], 0, 0, 0x404040)
x.XMapWindow(d, w)
x.XSync(d, 0)
print(hex(w), flush=True)
todo = []
signal.signal(signal.SIGUSR1, lambda s, f: todo.append("resize"))
signal.signal(signal.SIGUSR2, lambda s, f: todo.append("destroy"))
signal.signal(signal.SIGTERM, lambda s, f: todo.append("quit"))
while "quit" not in todo:
    time.sleep(0.05)
    while todo and todo[0] != "quit":
        what = todo.pop(0)
        if what == "resize":
            x.XMoveResizeWindow(d, w, a[4], a[5], a[6], a[7])
        elif what == "destroy" and w:
            x.XDestroyWindow(d, w)
            w = 0
        x.XSync(d, 0)
EOF
start_window() {   # start_window x y w h  x2 y2 w2 h2: sets WIN (its id)
  stop_window
  python3 "$TMP/window.py" "$@" >"$TMP/window.id" 2>"$TMP/window.err" &
  WIN_PID=$!
  local _
  for _ in $(seq 1 50); do [ -s "$TMP/window.id" ] && break; sleep 0.1; done
  WIN=$(head -1 "$TMP/window.id")
  [ -n "$WIN" ] || die "could not make a window: $(tail -2 "$TMP/window.err")"
}

# ---- the Wine prefix, made before the player so its first frame needn't wait
if [ ! -f "$WINEPREFIX/system.reg" ]; then
  say "making a Wine prefix in $WINEPREFIX"
  start_x 24
  timeout 300 "$WINE" wineboot -i >"$TMP/wineboot.log" 2>&1
  timeout 60 wineserver -w
  [ -f "$WINEPREFIX/system.reg" ] || die "wineboot made no prefix: $(tail -3 "$TMP/wineboot.log")"
fi

# What to run. Only options every player version understands.
ARGS=(-f --no-sound)
if [ -n "$ASSETS" ]; then
  MOD=$("$PLAYER" --list 2>/dev/null | awk '/^  [a-z0-9]+\.[a-z0-9_]+ / {print $1; exit}')
  [ -n "$MOD" ] || die "no module in $ASSETS ($PLAYER --list)"
  ARGS+=("$MOD")
  say "running $MOD from $ASSETS"
else
  ARGS+=(--test-pattern)
  say "running the host's test pattern"
fi

launch() {   # launch <player args...>: in the background, as PL
  "$PLAYER" "$@" >"$TMP/player.out" 2>"$TMP/player.err" &
  PL=$!
}
start_player() {
  launch "${ARGS[@]}"
  local t=0
  while ! frames_shown; do
    alive || fail "the player exited before showing a frame"
    [ $t -ge $((FIRST * 2)) ] && fail "no frame on the screen within $FIRST s"
    sleep 0.5
    t=$((t + 1))
  done
  pass "frames on the screen after about $((t / 2)) s"
  local s1 s2 i
  s1=$(screen_sum)
  for i in $(seq 1 20); do
    sleep 0.5
    s2=$(screen_sum)
    [ "$s1" != "$s2" ] && break
  done
  [ "$s1" != "$s2" ] || fail "the screen stopped changing: no more frames"
  pass "frames keep coming"
}
end_player() {   # end_player <what>: a wake key ends it, status 0
  xdotool key a
  local i
  for i in $(seq 1 100); do alive || break; sleep 0.1; done
  alive && fail "$1: the wake key did not end the player within 10 s"
  wait "$PL"
  local rc=$?
  PL=""
  [ "$rc" = 0 ] || fail "$1: the player exited with status $rc"
  pass "$1: the wake key ended it (status 0)"
  no_host_left "$1"
}

[ -n "$XVFB_PID" ] || start_x 24

# ---- run 1: the host dies mid-run
say "run 1: the host is killed mid-run"
start_player
HP=$(host_pid)
[ -n "$HP" ] || fail "cannot find the host process"
sleep 1
kill -9 "$HP"
outcome=""
seen=""   # the screen when the new host was first seen
for _ in $(seq 1 90); do
  sleep 0.5
  if ! alive; then outcome=ended; break; fi
  NEW=$(host_pid)
  if [ -n "$NEW" ] && [ "$NEW" != "$HP" ]; then
    S=$(screen_sum)
    if [ -z "$seen" ]; then seen=$S; elif [ "$S" != "$seen" ]; then outcome=recovered; break; fi
  fi
done
case $outcome in
  recovered) pass "the player started a new host and shows its frames" ;;
  ended)
    wait "$PL"; PL=""
    pass "the player ended when its host died"
    no_host_left "run 1"
    ;;
  *) fail "the player hung after its host was killed (no new frames, still running after 45 s)" ;;
esac
[ -n "$PL" ] && end_player "run 1"

# ---- run 2: the input rules
say "run 2: keys"
mark_log
start_player
xdotool key shift; sleep 0.4
xdotool key Caps_Lock; sleep 0.4
xdotool key Caps_Lock; sleep 0.4
alive || fail "Shift or Caps Lock ended the player (they never wake the saver)"
pass "Shift and Caps Lock did not end it"
wait_log "input: caps 1 -> host" 5 && wait_log "input: caps 0 -> host" 5 ||
  fail "run 2: its host did not hear Caps Lock go on and off (no CAPS lines)"
pass "run 2: its host heard Caps Lock go on and off (CAPS lines)"
end_player "run 2"

# ---- run 3: a 16-bit display, without MIT-SHM
say "run 3: a 16-bit display without MIT-SHM"
start_x 16 -extension MIT-SHM
ARGS_SAVED=("${ARGS[@]}")
ARGS=(-f --no-sound --test-pattern)
ASSETS_SAVED=$ASSETS
ASSETS=""
start_player
pass "the test pattern's markers in their colours at 16 bits per pixel"
end_player "run 3"
ARGS=("${ARGS_SAVED[@]}")
ASSETS=$ASSETS_SAVED

# ---- run 4: the host dies while the display is off (simulated)
say "run 4: the host dies while the display is off (simulated): no busy loop"
start_x 24
mark_log
launch -f --no-sound --test-pattern --test-display-off 15000
wait_log "display off" "$FIRST" || fail "run 4: the display never went off (--test-display-off, after 5 frames)"
HP=$(host_pid)
[ -n "$HP" ] || fail "run 4: cannot find the host process"
kill -9 "$HP"
wait_log "host-exit" 10 || fail "run 4: the player did not see its host die"
sleep 1
HZ=$(getconf CLK_TCK)
T0=$(cpu_ticks "$PL")
sleep 3
T1=$(cpu_ticks "$PL")
[ -n "$T0" ] && [ -n "$T1" ] || fail "run 4: the player is gone"
USED=$(((T1 - T0) * 100 / (HZ * 3)))
[ "$USED" -lt 20 ] || fail "run 4: the player used ${USED}% of a core while its restart waited for the display (a busy loop)"
pass "run 4: ${USED}% of a core while its restart waits for the display"
log_since | grep -q "respawn module" && fail "run 4: a host was started while the display was off"
wait_log "display on" 30 || fail "run 4: the display did not come back on"
wait_log "respawn module" 10 || fail "run 4: no new host once the display was back on"
s1=$(screen_sum)
for _ in $(seq 1 $((FIRST * 2))); do
  sleep 0.5
  [ "$(screen_sum)" != "$s1" ] && markers && break
done
[ "$(screen_sum)" != "$s1" ] && markers || fail "run 4: no new frames once the display was back on"
pass "run 4: once the display was back on, a new host and its frames"
end_player "run 4"

if [ -z "$HAVE_PY" ]; then
  say "skipped runs 5 to 7 (XScreenSaver's window, a preview, two monitors): python3 is not installed"
  say "PASS"
  exit 0
fi

# ---- run 5: XScreenSaver's window
say "run 5: XScreenSaver's protocol (-root, \$XSCREENSAVER_WINDOW)"
start_x 24
# 400x300 at 200,150 (4:3: the 640x480 frame fills it); then 600x300 (the
# frame pillarboxed: 400x300 at 100,0 in it).
start_window 200 150 400 300  200 150 600 300
mark_log
XSCREENSAVER_WINDOW=$WIN launch -root --no-sound --test-pattern
t=0
while ! markers 200 150 400 300; do
  alive || fail "run 5: the player exited before showing a frame"
  [ $t -ge $((FIRST * 2)) ] && fail "run 5: no frame in XScreenSaver's window within $FIRST s"
  sleep 0.5
  t=$((t + 1))
done
pass "run 5: frames in the window XScreenSaver named"
for pt in "100 100" "700 100" "100 500" "700 500" "150 300" "650 300"; do
  is_color $pt K || fail "run 5: the player drew outside XScreenSaver's window (at $pt)"
done
pass "run 5: nothing drawn outside it"
no_caps "run 5"
kill -USR1 "$WIN_PID"
wait_markers 10 300 150 400 300 || fail "run 5: the frames did not follow the window's new size"
is_color 250 300 K && is_color 750 300 K || fail "run 5: no black bars beside the pillarboxed frame"
pass "run 5: followed the window's new size (pillarboxed)"
kill -TERM "$PL"
for _ in $(seq 1 30); do alive || break; sleep 0.1; done
alive && fail "run 5: SIGTERM did not end the player within 3 s"
wait "$PL"
rc=$?
PL=""
[ "$rc" = 0 ] || fail "run 5: SIGTERM: the player exited with status $rc"
pass "run 5: SIGTERM (XScreenSaver's stop) ended it (status 0)"
no_host_left "run 5"

# ---- run 6: a preview whose window goes
say "run 6: a preview (--window-id) whose window is destroyed"
start_window 50 50 320 240  50 50 320 240
mark_log
launch --window-id "$WIN" --test-pattern
t=0
while ! markers 50 50 320 240; do
  alive || fail "run 6: the player exited before showing a frame"
  [ $t -ge $((FIRST * 2)) ] && fail "run 6: no frame in the preview window within $FIRST s"
  sleep 0.5
  t=$((t + 1))
done
pass "run 6: frames in the preview window"
no_caps "run 6"
kill -USR2 "$WIN_PID"
for _ in $(seq 1 100); do alive || break; sleep 0.1; done
alive && fail "run 6: the player did not end within 10 s of its window being destroyed (hung)"
wait "$PL"
rc=$?
PL=""
[ "$rc" = 0 ] || fail "run 6: the player exited with status $rc"
pass "run 6: its window destroyed, it ended by itself (status 0)"
no_host_left "run 6"

# ---- run 7: full screen on two monitors
say "run 7: full screen on two monitors (RandR): the other one black, a move over it ends the saver"
# 1600x600: two RandR monitors of 800x600, the left one primary (the X
# server's output with it). Xvfb keeps RandR monitors only while it doesn't
# reset (-noreset: the script setting them is its only client then), and
# its root is white (-wr), so black over the other monitor is the player's.
SIZE=1600x600 start_x 24 -noreset -wr
python3 - <<'EOF' || fail "run 7: could not set two RandR monitors"
import ctypes
V, UL, I = ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int
x = ctypes.CDLL("libX11.so.6")
r = ctypes.CDLL("libXrandr.so.2")
class Res(ctypes.Structure):
    _fields_ = [("timestamp", UL), ("config_timestamp", UL), ("ncrtc", I), ("crtcs", ctypes.POINTER(UL)),
                ("noutput", I), ("outputs", ctypes.POINTER(UL)), ("nmode", I), ("modes", V)]
class Mon(ctypes.Structure):
    _fields_ = [("name", UL), ("primary", I), ("automatic", I), ("noutput", I), ("x", I), ("y", I), ("width", I),
                ("height", I), ("mwidth", I), ("mheight", I), ("outputs", ctypes.POINTER(UL))]
x.XOpenDisplay.restype = V
x.XOpenDisplay.argtypes = [ctypes.c_char_p]
x.XDefaultRootWindow.restype = UL
x.XDefaultRootWindow.argtypes = [V]
x.XInternAtom.restype = UL
x.XInternAtom.argtypes = [V, ctypes.c_char_p, I]
x.XSync.argtypes = [V, I]
r.XRRGetScreenResourcesCurrent.restype = ctypes.POINTER(Res)
r.XRRGetScreenResourcesCurrent.argtypes = [V, UL]
r.XRRAllocateMonitor.restype = ctypes.POINTER(Mon)
r.XRRAllocateMonitor.argtypes = [V, I]
r.XRRSetMonitor.argtypes = [V, UL, ctypes.POINTER(Mon)]
d = x.XOpenDisplay(None)
root = x.XDefaultRootWindow(d)
res = r.XRRGetScreenResourcesCurrent(d, root)
if not d or not res or res.contents.noutput < 1:
    raise SystemExit(1)
for name, left, primary in ((b"left", 0, 1), (b"right", 800, 0)):
    m = r.XRRAllocateMonitor(d, 1 if primary else 0)
    m.contents.name = x.XInternAtom(d, name, 0)
    m.contents.primary = primary
    m.contents.x, m.contents.y, m.contents.width, m.contents.height = left, 0, 800, 600
    m.contents.mwidth, m.contents.mheight = 211, 158
    if primary:
        m.contents.outputs[0] = res.contents.outputs[0]
    r.XRRSetMonitor(d, root, m)
x.XSync(d, 0)
EOF
xdotool mousemove 1200 300   # the pointer over the other monitor
launch -f --no-sound --test-pattern
t=0
while ! markers 0 0 800 600; do
  alive || fail "run 7: the player exited before showing a frame"
  [ $t -ge $((FIRST * 2)) ] && fail "run 7: no frame on the primary monitor within $FIRST s"
  sleep 0.5
  t=$((t + 1))
done
sleep 0.5
for pt in "801 1" "1000 150" "1200 300" "1400 450" "1598 598"; do
  is_color $pt K || fail "run 7: the other monitor is not black (at $pt)"
done
markers 0 0 800 600 || fail "run 7: the frames went from the primary monitor"
pass "run 7: the frames on the primary monitor, the other one black"
xdotool mousemove 1300 400
for _ in $(seq 1 50); do alive || break; sleep 0.1; done
alive && fail "run 7: a move over the other monitor did not end the player within 5 s"
wait "$PL"
rc=$?
PL=""
[ "$rc" = 0 ] || fail "run 7: the player exited with status $rc"
pass "run 7: a move over the other monitor ended it (status 0)"
no_host_left "run 7"

say "PASS"
exit 0
