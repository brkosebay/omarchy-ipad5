#!/usr/bin/env bash
# Exercise auto-rotate and the rotation lock on the live Omarchy session, from
# the Mac, over SSH (scripts/omarchy/ssh.sh; the orchestrator runs it, agents
# only write it into test plans). docs/omarchy/tablet-usage.md has the plan.
#
#   scripts/omarchy/test-autorotate.sh              mock: no accelerometer needed
#   scripts/omarchy/test-autorotate.sh --hardware   real sensor: turn the iPad when told
#   scripts/omarchy/test-autorotate.sh --shots DIR  also save a grim screenshot per step
#
# OMARCHY_TEST_SSH replaces the ssh wrapper; the QEMU VM that qemu-smoke.sh
# --keep leaves is scripts/omarchy/qemu/vm-ssh.sh. HOLD=15 is how many seconds
# --hardware waits at each position.
#
# Mock mode starts its own ipad-autorotate on a FIFO (the real unit is stopped
# for the run and restarted after), feeds it all four orientations, then checks
# after each that Hyprland's monitor transform and touchscreen transform are
# the expected ones, that the lock holds the screen and releases it, that a
# manual rotation sticks, and that an open on-screen keyboard survives a turn.
# It puts the orientation and the lock back as it found them. Exit status 1 if
# anything failed.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SSH="${OMARCHY_TEST_SSH:-$ROOT/scripts/omarchy/ssh.sh}"
MODE=mock
SHOTS=""
while [ $# -gt 0 ]; do
	case "$1" in
	--mock) MODE=mock ;;
	--hardware) MODE=hardware ;;
	--shots) SHOTS="$2"; shift ;;
	-h | --help) sed -n '2,/^set -e/p' "$0" | sed '$d;s/^# \{0,1\}//'; exit 0 ;;
	*) echo "unknown arg: $1" >&2; exit 2 ;;
	esac
	shift
done
[ -z "$SHOTS" ] || mkdir -p "$SHOTS"

# Everything runs on the iPad in one bash; the steps come back as ok/FAIL lines
# and SHOT lines (which the Mac side turns into screenshot files).
"$SSH" "MODE=$MODE SHOTS=$([ -n "$SHOTS" ] && echo 1 || echo 0) HOLD=${HOLD:-15} bash -s" <<'REMOTE' | {
set -uo pipefail
export XDG_RUNTIME_DIR=/run/user/$(id -u) OMARCHY_PATH=${OMARCHY_PATH:-/usr/share/omarchy}
export DBUS_SESSION_BUS_ADDRESS=unix:path=$XDG_RUNTIME_DIR/bus
sig=$(ls -t "$XDG_RUNTIME_DIR/hypr" 2>/dev/null | head -1)
[ -n "$sig" ] || { echo "FAIL Hyprland isn't running"; exit 1; }
export HYPRLAND_INSTANCE_SIGNATURE=$sig
sock=$(ls -t "$XDG_RUNTIME_DIR"/wayland-[0-9] 2>/dev/null | head -1)
[ -n "$sock" ] && export WAYLAND_DISPLAY=${sock##*/}

fails=0
ok() { echo "ok   $*"; }
bad() { echo "FAIL $*"; fails=$((fails + 1)); }
expect() { # name got want
  if [ "$2" = "$3" ]; then ok "$1: $2"; else bad "$1: got '$2', want '$3'"; fi
}
transform_of() { case $1 in portrait) echo 0 ;; landscape-flipped) echo 1 ;; portrait-flipped) echo 2 ;; landscape) echo 3 ;; esac; }
live_transform() { hyprctl monitors -j | jq -r '.[0].transform'; }
live_touch() { hyprctl getoption input:touchdevice:transform -j | jq -r '.int'; }
shot() { # label
  [ "$SHOTS" = 1 ] || return 0
  command -v grim >/dev/null || return 0
  grim /tmp/ar-shot.png 2>/dev/null && { echo "SHOT $1"; base64 </tmp/ar-shot.png; echo "ENDSHOT"; }
}
# Wait up to 12 s for ipad-rotate to say $1.
wait_for() {
  for _ in $(seq 60); do
    [ "$(ipad-rotate)" = "$1" ] && return 0
    sleep 0.2
  done
  return 1
}
check_orientation() { # label want
  local want=$2 t
  t=$(transform_of "$want")
  sleep 1 # the reload lands after the state file is written
  expect "$1: ipad-rotate" "$(ipad-rotate)" "$want"
  expect "$1: monitor transform" "$(live_transform)" "$t"
  expect "$1: touchscreen transform" "$(live_touch)" "$t"
  shot "$1-$want"
}

orig_orientation=$(ipad-rotate)
orig_lock=$(ipad-rotate lock)
unit_was_active=$(systemctl --user is-active ipad-autorotate.service 2>/dev/null || true)
daemon_pid=""
cleanup() {
  [ -n "$daemon_pid" ] && kill "$daemon_pid" 2>/dev/null
  ipad-rotate lock off >/dev/null 2>&1
  [ "$orig_lock" = locked ] && ipad-rotate lock on >/dev/null 2>&1
  ipad-rotate "$orig_orientation" >/dev/null 2>&1
  [ "$unit_was_active" = active ] && systemctl --user start ipad-autorotate.service 2>/dev/null
  rm -f /tmp/ar-shot.png /tmp/ar-test.fifo /tmp/ar-test.log
}
trap cleanup EXIT

echo "## what this device has"
ipad-autorotate --list 2>&1 | sed 's/^/     /'
echo "     unit: $(systemctl --user is-active ipad-autorotate.service 2>&1) (condition: $(systemctl --user show -p ConditionResult --value ipad-autorotate.service 2>&1))"
echo "     starting at $orig_orientation, $orig_lock"

if [ "$MODE" = mock ]; then
  echo "## mock: four orientations (sensor name -> screen)"
  systemctl --user stop ipad-autorotate.service 2>/dev/null
  ipad-rotate lock off >/dev/null
  # Start from a position the first step will change.
  [ "$orig_orientation" = portrait ] && ipad-rotate landscape >/dev/null || ipad-rotate portrait >/dev/null
  sleep 2
  rm -f /tmp/ar-test.fifo
  setsid ipad-autorotate --mock /tmp/ar-test.fifo >/tmp/ar-test.log 2>&1 &
  daemon_pid=$!
  sleep 1
  kill -0 "$daemon_pid" 2>/dev/null && ok "mock daemon running (pid $daemon_pid)" || { bad "mock daemon didn't start"; cat /tmp/ar-test.log; }
  feed() { echo "$1" >/tmp/ar-test.fifo; }
  for pair in "right-up landscape" "normal portrait" "left-up landscape-flipped" "bottom-up portrait-flipped" "right-up landscape"; do
    set -- $pair
    t0=$(date +%s.%N)
    feed "$1"
    if wait_for "$2"; then ok "$1 -> $2 in $(awk -v a="$(date +%s.%N)" -v b="$t0" 'BEGIN { printf "%.1f", a - b }') s"; else bad "$1 never turned to $2 (now $(ipad-rotate)); log: $(tail -3 /tmp/ar-test.log | tr '\n' '|')"; fi
    check_orientation "$1" "$2"
    sleep 2.5 # the daemon's minimum interval between turns
  done

  echo "## mock: undefined and flicker"
  feed undefined; sleep 2
  expect "undefined changes nothing" "$(ipad-rotate)" landscape
  printf 'normal\nright-up\n' >/tmp/ar-test.fifo; sleep 3
  expect "flicker inside the debounce is ignored" "$(ipad-rotate)" landscape

  echo "## mock: rotation lock"
  ipad-rotate lock on >/dev/null
  expect "lock reads back" "$(ipad-rotate lock)" locked
  expect "lock in the state file" "$(grep '^rotation_lock=' ~/.local/state/linux-a9/display | cut -d= -f2)" on
  sleep 1
  feed normal; sleep 4
  expect "locked: sensor normal does not turn the screen" "$(ipad-rotate)" landscape
  ipad-tablet rotate-lock # the bar button's action: toggles to unlocked
  expect "ipad-tablet rotate-lock unlocks" "$(ipad-rotate lock)" unlocked
  if wait_for portrait; then ok "unlock turns to how it is held (normal -> portrait)"; else bad "unlock did not turn the screen (now $(ipad-rotate))"; fi
  check_orientation "unlock" portrait
  sleep 2.5
  ipad-tablet rotate-lock
  expect "ipad-tablet rotate-lock locks" "$(ipad-rotate lock)" locked
  ipad-rotate lock off >/dev/null

  echo "## mock: manual rotation, on-screen keyboard"
  ipad-rotate landscape >/dev/null; sleep 3
  expect "manual rotate holds with the sensor unchanged" "$(ipad-rotate)" landscape
  sleep 3
  expect "...and still after a few seconds" "$(ipad-rotate)" landscape
  ipad-osk show >/dev/null 2>&1; sleep 2
  osk_before=$(ipad-osk status 2>&1 | head -1)
  shot "osk-landscape"
  feed bottom-up # the sensor was last at normal, so this is a turn
  wait_for portrait-flipped || bad "no turn with the keyboard up (now $(ipad-rotate))"
  sleep 4
  osk_after=$(ipad-osk status 2>&1 | head -1)
  shot "osk-portrait-flipped"
  expect "keyboard still shown after the turn" "$osk_after" "$osk_before"
  ipad-osk hide >/dev/null 2>&1

  echo "## mock: raw accelerometer lines (classifier)"
  sleep 2.5
  feed "accel -9.8 0.3 0.4"; wait_for landscape && ok "accel -x (right-up) -> landscape" || bad "accel -x: now $(ipad-rotate)"
  sleep 2.5
  feed "accel 9.8 0.3 0.4"; wait_for landscape-flipped && ok "accel +x (left-up) -> landscape-flipped" || bad "accel +x: now $(ipad-rotate)"
  sleep 2.5
  feed "accel 0.2 0.3 9.8"; sleep 3
  expect "accel flat (z only) changes nothing" "$(ipad-rotate)" landscape-flipped
  echo "     daemon log:"; sed 's/^/       /' /tmp/ar-test.log | tail -12
else
  echo "## hardware: the real accelerometer"
  if ipad-autorotate --list 2>&1 | grep -q 'HasAccelerometer: False' && ipad-autorotate --list 2>&1 | grep -q 'IIO accelerometer: none'; then
    bad "no accelerometer on this device yet (the kernel driver isn't there); run the mock test instead"
    echo; echo "RESULT: $fails FAILED"; exit 1
  fi
  ipad-rotate lock off >/dev/null
  systemctl --user restart ipad-autorotate.service 2>/dev/null
  echo "     Hold the iPad flat on a table, screen up, for $HOLD s: the screen must not turn."
  start_orientation=$(ipad-rotate)
  ipad-autorotate --probe "$HOLD" 2>&1 | sed 's/^/       /'
  expect "flat on the table changes nothing" "$(ipad-rotate)" "$start_orientation"
  for pair in "home button at the BOTTOM (portrait, upright)|portrait" "home button on the RIGHT (landscape)|landscape" "home button at the TOP (upside down)|portrait-flipped" "home button on the LEFT|landscape-flipped"; do
    echo "     >>> Turn the iPad now: ${pair%|*}. Holding for $HOLD s..."
    ipad-autorotate --probe "$HOLD" 2>&1 | grep -E "accel|orientation|sensor" | tail -4 | sed 's/^/       /'
    expect "${pair%|*}" "$(ipad-rotate)" "${pair#*|}"
    check_orientation "hold" "${pair#*|}"
  done
  echo "     >>> Lock it (ipad-rotate lock on), then turn the iPad $HOLD s: the screen must stay."
  ipad-rotate lock on >/dev/null
  cur=$(ipad-rotate)
  sleep "$HOLD"
  expect "locked: the screen stays" "$(ipad-rotate)" "$cur"
  ipad-rotate lock off >/dev/null
fi

echo
[ "$fails" = 0 ] && echo "RESULT: all passed" || echo "RESULT: $fails FAILED"
[ "$fails" = 0 ]
REMOTE
	n=0
	out=""
	while IFS= read -r line; do
		case "$line" in
		SHOT\ *) out="$SHOTS/${line#SHOT }.png"; : >"$out.b64" ;;
		ENDSHOT) base64 -d <"$out.b64" >"$out" 2>/dev/null || base64 -D <"$out.b64" >"$out"; rm -f "$out.b64"; echo "     screenshot: $out"; out="" ;;
		*) if [ -n "$out" ]; then echo "$line" >>"$out.b64"; else echo "$line"; fi ;;
		esac
	done
}
exit "${PIPESTATUS[0]}"
