#!/usr/bin/env bash
# Exercise ipad-als-brightness and ipad-lid on a live Omarchy session with no
# sensor and no cover: the QEMU VM that qemu-smoke.sh --keep leaves (default),
# or any session OMARCHY_TEST_SSH points at. The hardware is stood in for by
#   - a fake sysfs IIO tree (the light sensor) and a fake backlight with a fake
#     brightnessctl, so nothing real is dimmed,
#   - a uinput device that reports SW_LID (scripts/omarchy/qemu/vm-switch.py),
#     so ipad-lid reads a real /dev/input node, EVIOCGSW and input_event records,
# while Hyprland (DPMS), the udev rule, the user units and the bar are real.
# Needs the layer installed first:
#   OMARCHY_HOT_SSH=scripts/omarchy/qemu/vm-ssh.sh scripts/omarchy/hot-apply.sh
#
#   test-als-lid-vm.sh [--shots DIR]     screenshots of the bar (grim) into DIR
#
# It stops the real ipad-als-brightness and ipad-lid units for the run and
# starts them again at the end, never touches the user's own switch state or
# conf (it uses its own XDG_CONFIG_HOME/XDG_STATE_HOME), and never suspends
# (ipad-sleep is replaced by a logging script). Exit status 1 if anything failed.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SSH="${OMARCHY_TEST_SSH:-$HERE/vm-ssh.sh}"
SHOTS=""
while [ $# -gt 0 ]; do
	case "$1" in
	--shots) SHOTS="$2"; shift ;;
	-h | --help) sed -n '2,/^set -u/p' "$0" | sed '$d;s/^# \{0,1\}//'; exit 0 ;;
	*) echo "unknown arg: $1" >&2; exit 2 ;;
	esac
	shift
done
[ -z "$SHOTS" ] || mkdir -p "$SHOTS"

fails=0
ok() { echo "ok   $*"; }
bad() { echo "FAIL $*"; fails=$((fails + 1)); }
r() { "$SSH" "$@"; }
# check NAME CMD...: CMD runs on the VM; it must succeed (within 5 s, polled).
check() {
	local name=$1 cmd=$2 i
	for i in 1 2 3 4 5 6 7 8 9 10; do
		if r "$ENVSET $cmd" >/dev/null 2>&1; then ok "$name"; return 0; fi
		sleep 0.5
	done
	bad "$name"
	return 1
}
shot() { [ -z "$SHOTS" ] || "$HERE/vm-shot.sh" "$SHOTS/$1.png" >/dev/null; }

T=/tmp/als-lid-vm
# One environment for everything run by hand: the fakes and a private conf and
# state directory; XDG_RUNTIME_DIR and Hyprland's are the session's own.
ENVSET="export XDG_RUNTIME_DIR=/run/user/\$(id -u) XDG_CONFIG_HOME=$T/config XDG_STATE_HOME=$T/state IPAD_ALS_BACKLIGHT_ROOT=$T/backlight IPAD_ALS_BRIGHTNESSCTL=$T/brightnessctl IPAD_LID_SLEEP=$T/ipad-sleep; sig=\$(ls -t \$XDG_RUNTIME_DIR/hypr 2>/dev/null | head -1); export HYPRLAND_INSTANCE_SIGNATURE=\$sig; sock=\$(ls -t \$XDG_RUNTIME_DIR/wayland-[0-9] 2>/dev/null | head -1); export WAYLAND_DISPLAY=\${sock##*/};"

echo "## setup"
r "bash -s" <<'REMOTE'
set -e
T=/tmp/als-lid-vm
export XDG_RUNTIME_DIR=/run/user/$(id -u)
sudo pkill -f "$T/vm-switch.p[y]" 2>/dev/null || true
pkill -f "ipad-als-brightness --mock-ii[o]" 2>/dev/null || true
for f in ipad-als-brightness ipad-lid; do
  [[ -r $XDG_RUNTIME_DIR/$f.json ]] && kill "$(jq -r .pid "$XDG_RUNTIME_DIR/$f.json")" 2>/dev/null || true
done
rm -rf $T && mkdir -p $T/config/linux-a9 $T/state $T/backlight/lcd $T/iio/iio:device0
echo 1000 >$T/backlight/lcd/max_brightness
echo 500 >$T/backlight/lcd/brightness
echo 300 >$T/iio/iio:device0/in_illuminance_input
cat >$T/brightnessctl <<'EOF'
#!/bin/bash
# fake brightnessctl: -q -d NAME set N | -s set 10% | -r
bl=/tmp/als-lid-vm/backlight/lcd
cur=$(<$bl/brightness)
case " $* " in
*" -r "*) [[ -r $bl/saved ]] && cur=$(<$bl/saved); new=$cur ;;
*)
  v=${@: -1}
  [[ " $* " == *" -s "* ]] && echo "$cur" >$bl/saved
  if [[ $v == *% ]]; then new=$(( ${v%\%} * 1000 / 100 )); else new=$v; fi ;;
esac
echo "$new" >$bl/brightness
echo "brightnessctl $*" >>/tmp/als-lid-vm/calls.log
EOF
cat >$T/ipad-sleep <<'EOF'
#!/bin/bash
echo "ipad-sleep" >>/tmp/als-lid-vm/calls.log
EOF
cat >$T/lock <<'EOF'
#!/bin/bash
touch /tmp/als-lid-vm/locked
EOF
chmod +x $T/brightnessctl $T/ipad-sleep $T/lock
cat >$T/config/linux-a9/als-brightness.conf <<'EOF'
smooth_secs = 0
poll_hz = 5
min_interval = 0.4
ramp_pct_per_s = 100000
override_secs = 5
EOF
cat >$T/config/linux-a9/lid.conf <<EOF
settle = 0.1
suspend_delay = 1.0
lock = on
lock_cmd = $T/lock
EOF
touch $T/calls.log
systemctl --user stop ipad-als-brightness.service ipad-lid.service 2>/dev/null || true
echo "units stopped"
REMOTE
r "cat >$T/vm-switch.py" <"$HERE/vm-switch.py"

echo "## layer installed"
check "ipad-als-brightness and ipad-lid are installed" "command -v ipad-als-brightness && command -v ipad-lid"
check "the user units exist" "systemctl --user cat ipad-als-brightness.service ipad-lid.service"
check "logind ignores a lid switch" "test \"\$(busctl get-property org.freedesktop.login1 /org/freedesktop/login1 org.freedesktop.login1.Manager HandleLidSwitch)\" = 's \"ignore\"'"
check "the sensor units are skipped, not failed, without hardware" "! systemctl --user is-failed ipad-als-brightness.service ipad-lid.service"
check "the cover unit's condition says no switch yet" "! ipad-lid --available"
check "the menu has the sensor entries" "grep -q trigger.toggle.ipad.autobright-on ~/.config/omarchy/extensions/omarchy-menu.jsonc && grep -q trigger.toggle.ipad.cover-sleep-on ~/.config/omarchy/extensions/omarchy-menu.jsonc"
check "hypridle dims through ipad-als-brightness" "grep -q 'ipad-als-brightness dim' ~/.config/hypr/hypridle.conf"

echo "## auto-brightness (fake sensor, fake backlight)"
r "$ENVSET nohup ipad-als-brightness --mock-iio $T/iio >$T/als.log 2>&1 </dev/null &" >/dev/null 2>&1 || true
check "the daemon writes its status file (the bar button's cue)" "test -e \$XDG_RUNTIME_DIR/ipad-als-brightness.json"
sleep 3
check "off by default: the backlight is untouched" "test \$(cat $T/backlight/lcd/brightness) = 500"
shot als-off
r "$ENVSET ipad-als-brightness on" >/dev/null
check "on: it follows 300 lux (curve: about 73 %)" "b=\$(cat $T/backlight/lcd/brightness); test \$b -gt 700 -a \$b -lt 770"
sleep 2.5
shot als-on
r "echo 5 >$T/iio/iio:device0/in_illuminance_input"
check "5 lux: dim (about 18 %)" "b=\$(cat $T/backlight/lcd/brightness); test \$b -gt 150 -a \$b -lt 210"
r "echo 950 >$T/backlight/lcd/brightness"
sleep 1
r "echo 700 >$T/iio/iio:device0/in_illuminance_input"
sleep 1
check "a manual change pauses it" "test \$(cat $T/backlight/lcd/brightness) = 950"
check "...and it resumes" "b=\$(cat $T/backlight/lcd/brightness); test \$b -gt 880 -a \$b -lt 920 && grep -q 'changed by hand' $T/als.log"
r "$ENVSET ipad-als-brightness dim" >/dev/null
check "idle dim (hypridle's command) dims and holds" "test \$(cat $T/backlight/lcd/brightness) = 100"
r "echo 4 >$T/iio/iio:device0/in_illuminance_input"
sleep 2
check "...it stays dim while dimmed" "test \$(cat $T/backlight/lcd/brightness) = 100"
r "$ENVSET ipad-als-brightness undim" >/dev/null
check "...undim restores and releases it" "b=\$(cat $T/backlight/lcd/brightness); test \$b -gt 150 -a \$b -lt 210"
r "$ENVSET ipad-als-brightness off" >/dev/null
r "pkill -f 'ipad-als-brightness --mock-ii[o]'" >/dev/null 2>&1
check "the status file is gone when the daemon stops" "! test -e \$XDG_RUNTIME_DIR/ipad-als-brightness.json"

echo "## Smart Cover (uinput switch, real Hyprland)"
r "sudo nohup python3 $T/vm-switch.py $T/cover.fifo >$T/vm-switch.log 2>&1 </dev/null &" >/dev/null 2>&1 || true
check "the virtual switch exists" "grep -q created $T/vm-switch.log && pgrep -f $T/vm-switch.p[y]"
check "udev gave the user access to it (uaccess rule)" "ipad-lid --list | grep -q 'iPad Smart Cover'"
check "ipad-lid --available" "ipad-lid --available"
check "--list reads the state (EVIOCGSW): open" "ipad-lid --list | grep -q 'SW_LID=0'"
r "$ENVSET nohup ipad-lid >$T/lid.log 2>&1 </dev/null &" >/dev/null 2>&1 || true
sleep 1
check "the daemon sees the device" "grep -q 'watching iPad Smart Cover' $T/lid.log"
dpms() { echo "hyprctl monitors -j | jq -e 'all(.[]; .dpmsStatus == $1)' >/dev/null"; }
cover() { r "timeout 5 bash -c 'echo $1 >$T/cover.fifo'"; }
cover closed
check "SW_LID=1 is seen (but nothing is switched on)" "grep -q 'SW_LID = 1' $T/lid.log"
sleep 1
check "off by default: the display stays on" "$(dpms true)"
check "off by default: no lock" "! test -e $T/locked"
cover open
sleep 0.6
r "$ENVSET ipad-lid on" >/dev/null
cover closed
check "closed: lock command ran (lock = on)" "test -e $T/locked"
check "closed: Hyprland DPMS is off" "$(dpms false)"
check "closed: the backlight is 0" "test \$(cat $T/backlight/lcd/brightness) = 0"
check "closed: the hold flag for auto-brightness" "test -e \$XDG_RUNTIME_DIR/linux-a9/lid-closed"
check "closed: no hyprctl errors" "! grep -q 'hyprctl dispatch failed' $T/lid.log"
# (no screenshot while closed: grim waits forever for a frame from a DPMS-off output)
cover open
check "open: Hyprland DPMS is back on" "$(dpms true)"
check "open: the backlight is back" "test \$(cat $T/backlight/lcd/brightness) -gt 100"
check "open: the hold flag is gone" "! test -e \$XDG_RUNTIME_DIR/linux-a9/lid-closed"
shot cover-open
r "echo >$T/calls.log; $ENVSET ipad-lid suspend on" >/dev/null
cover closed
check "suspend on: the fake ipad-sleep runs once after the delay" "test \$(grep -c ipad-sleep $T/calls.log) = 1"
cover open
r "$ENVSET ipad-lid suspend off; ipad-lid off" >/dev/null
sleep 0.5
check "a real suspend was never run (no ipad-sleep beyond the fake)" "test \$(grep -c ipad-sleep $T/calls.log) = 1"
cover "cover2 1"
sleep 0.6
check "the second hall is logged and ignored" "grep -q 'SW_MACHINE_COVER = 1' $T/lid.log && $(dpms true)"
cover "cover2 0"

echo "## cleanup"
r "export XDG_RUNTIME_DIR=/run/user/\$(id -u); [[ -r \$XDG_RUNTIME_DIR/ipad-lid.json ]] && kill \$(jq -r .pid \$XDG_RUNTIME_DIR/ipad-lid.json); echo quit >$T/cover.fifo; sleep 0.5; sudo pkill -f $T/vm-switch.p[y]; systemctl --user start ipad-als-brightness.service ipad-lid.service; rm -rf $T" >/dev/null 2>&1
echo "$([ $fails = 0 ] && echo 'RESULT: all passed' || echo "RESULT: $fails FAILED")"
[ $fails = 0 ]
