#!/usr/bin/env bash
# Showcase bring-up, Mac side. Orchestrator only: never part of a build or an
# implementation-agent test. Run it after a DFU cold boot of the showcase
# payload (docs/showcase.md):
#
#   source build/showcase/entries.env
#   scripts/boot.sh pongo
#   scripts/boot.sh linux "$SHOWCASE_PAYLOAD"
#   scripts/showcase/bring-up.sh
#
# Steps, each bounded in time; running it again skips what is already done:
#   entry      the booted kernel is $SHOWCASE_ENTRY: cmdline, release, CPU OPP table
#   aop        the four private AOP files by hash (installed when different); the
#              deferred cold trigger exactly as leg A (trigger-cold.sh, 60 s hold),
#              once per boot; then streaming, calibration, the SPI0 skip, no
#              CH3TD/crashlog, the IIO accelerometer
#   rotate     iio-sensor-proxy has the accelerometer; ipad-autorotate runs on it
#   cpu        the 2160 MHz hold (transient unit linux-a9-oc-hold, stock voltage,
#              guard on); cpuinfo_max_freq 2160000; an 8 s two-worker load runs
#              at 2160 MHz per the guard's hardware-decoded clock
#   als        ipad-als-brightness on and running
#   fastfetch  the iPad config installed; its CPU and Wi-Fi lines
# Never: a default entry, an entry upload to /boot, kexec, voltage offsets.
#
#   --stop-cpu     stop the 2160 MHz hold (back to stock clocks) and exit
#   --governor G   performance|schedutil while held (default: leave it, schedutil)
# SHOWCASE_SSH_WAIT (default 300) is how long to wait for SSH after the boot;
# SHOWCASE_RUN_DIR and OMARCHY_SSH are for scripts/showcase/tests/test-bring-up.py.
# Exit 0 only when no step FAILed (WARN still passes). Log and captures:
# build/showcase/runs/<UTC>/.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT" || exit 2
SSH=${OMARCHY_SSH:-$ROOT/scripts/omarchy/ssh.sh}
STOP=0
GOVERNOR=
while [ $# -gt 0 ]; do
	case $1 in
	--stop-cpu) STOP=1 ;;
	--governor) GOVERNOR=${2:-}; shift ;;
	-h | --help) sed -n '2,/^set -uo/p' "$0" | sed '$d;s/^# \{0,1\}//'; exit 0 ;;
	*) echo "unknown argument: $1" >&2; exit 2 ;;
	esac
	shift
done
case $GOVERNOR in '' | performance | schedutil) ;; *) echo "--governor performance|schedutil" >&2; exit 2 ;; esac
if [ ! -s build/showcase/entries.env ]; then
	echo "build/showcase/entries.env missing: run scripts/showcase/package.py" >&2
	exit 2
fi
# shellcheck source=/dev/null
. build/showcase/entries.env
: "${SHOWCASE_ENTRY:?}" "${SHOWCASE_DIR:?}" "${SHOWCASE_ROOT_FIRMWARE:?}" "${SHOWCASE_RELEASE:?}"
RUN=${SHOWCASE_RUN_DIR:-$ROOT/build/showcase/runs/$(date -u +%Y%m%dT%H%M%SZ)}
mkdir -p "$RUN"
exec > >(tee -a "$RUN/bring-up.log") 2>&1

KHZ=2160000
HOLD=/root/a9-showcase
UNIT=linux-a9-oc-hold
# Expanded on the iPad, for the user's systemd and D-Bus session.
# shellcheck disable=SC2016
USER_ENV='export XDG_RUNTIME_DIR=/run/user/$(id -u); export DBUS_SESSION_BUS_ADDRESS=unix:path=$XDG_RUNTIME_DIR/bus;'

# Every remote call gets a hard bound (SIGALRM, exit 142), so nothing waits forever.
bounded() { perl -e 'alarm shift @ARGV; exec @ARGV or die "exec: $!\n"' "$@"; }
rsh() { local t=$1; shift; bounded "$t" "$SSH" "$@"; }

names=() states=() details=()
note() {
	names+=("$1") states+=("$2") details+=("$3")
	printf '%-4s %-9s %s\n' "$2" "$1" "$3"
}
summary() {
	local i fails=0
	for i in "${!states[@]}"; do
		[ "${states[$i]}" = FAIL ] && fails=$((fails + 1))
	done
	{
		echo
		echo "== showcase bring-up: $SHOWCASE_ENTRY ($(date -u +%H:%M:%SZ), log ${RUN#"$ROOT"/})"
		for i in "${!names[@]}"; do
			printf '%-4s %-9s %s\n' "${states[$i]}" "${names[$i]}" "${details[$i]}"
		done
		if [ "$fails" = 0 ]; then echo "RESULT: PASS"; else echo "RESULT: FAIL ($fails step(s))"; fi
	} | tee "$RUN/summary.txt"
	[ "$fails" = 0 ]
}
step() { printf '\n== %s\n' "$*"; }

# ---------------------------------------------------------------- ssh
step "waiting for SSH (up to ${SHOWCASE_SSH_WAIT:-300} s)"
deadline=$((SECONDS + ${SHOWCASE_SSH_WAIT:-300}))
until rsh 25 true >/dev/null 2>&1; do
	if [ "$SECONDS" -ge "$deadline" ]; then
		note ssh FAIL "no SSH within ${SHOWCASE_SSH_WAIT:-300} s"
		summary
		exit 1
	fi
	sleep 5
done

if [ "$STOP" = 1 ]; then
	step "stopping the 2160 MHz hold"
	rsh 40 "sudo -n systemctl stop $UNIT 2>/dev/null; sudo -n python3 -B $HOLD/a9-oc-hold.py status" >"$RUN/cpu-stop.json"
	if python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); g=d["guard"]
sys.exit(0 if g.get("armed") == "0" and g.get("dirty") == "0" and g.get("configured") == "0" else 1)' "$RUN/cpu-stop.json" 2>/dev/null; then
		note cpu PASS "hold stopped; guard at stock ($(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["guard"].get("reason"))' "$RUN/cpu-stop.json"))"
	else
		note cpu FAIL "stock not confirmed; see $RUN/cpu-stop.json"
	fi
	summary
	exit
fi

# ---------------------------------------------------------------- entry
step "entry"
rsh 30 'bash -s' >"$RUN/identity.txt" 2>&1 <<'REMOTE'
cat /proc/cmdline
uname -r
cat /proc/sys/kernel/random/boot_id
python3 - <<'PY'
from pathlib import Path
for d in sorted(Path('/proc/device-tree/opp-table').iterdir()):
    if d.is_dir() and (d / 'opp-hz').exists():
        value = lambda name: int.from_bytes((d / name).read_bytes(), 'big')
        print('opp', d.name, value('opp-hz'), value('opp-level'), int((d / 'turbo-mode').exists()))
PY
REMOTE
entry=$(python3 - "$RUN/identity.txt" "$SHOWCASE_ENTRY" "$SHOWCASE_RELEASE" <<'PY'
import sys
lines = open(sys.argv[1]).read().splitlines()
entry, release = sys.argv[2], sys.argv[3]
bad = []
try:
    words, running, boot_id = lines[0].split(), lines[1], lines[2]
except IndexError:
    print('BAD no identity from the iPad')
    sys.exit()
for token in (f'linux_a9.entry={entry}', 'apple_soc_cpufreq.a9_tuning=1',
              'apple_soc_cpufreq.a9_passive_millic=90000', 'systemd.mask=maintainer@example.invalid',
              'pcie_apple_h9p.ans_keep_power=1', 'aop_force_cold=1', 'aop_defer=1', 'aop_run=2'):
    if words.count(token) != 1:
        bad.append(f'{token} x{words.count(token)}')
if any('debug_shell' in w for w in words):
    bad.append('debug_shell present')
if running != release:
    bad.append(f'release {running}, want {release}')
opps = sorted(tuple(map(int, l.split()[2:5])) for l in lines[3:] if l.startswith('opp '))
want = sorted([(hz, i + 1, 0) for i, hz in enumerate((300000000, 396000000, 600000000, 912000000,
              1200000000, 1512000000, 1800000000))] + [(2160000000, 14, 1)])
if opps != want:
    bad.append(f'CPU OPP table is not the showcase one ({len(opps)} OPPs, max {max(opps)[0] if opps else 0})')
print('BAD ' + '; '.join(bad) if bad else f'OK boot_id {boot_id}')
PY
)
if [ "${entry%% *}" = OK ]; then
	note entry PASS "$SHOWCASE_ENTRY, $SHOWCASE_RELEASE, OPPs stock+2160, ${entry#OK }"
else
	note entry FAIL "${entry#BAD }"
	summary
	exit 1
fi

# ---------------------------------------------------------------- aop
step "AOP: private firmware"
aop_ok=1
fw_names="accel-calibration.bin boot-ecid.bin ipad6faop-20H350.bin ipad6faop_ext-20H350.bin"
if ! (cd "$SHOWCASE_ROOT_FIRMWARE" && shasum -a 256 -c SHA256SUMS >/dev/null); then
	note firmware FAIL "local $SHOWCASE_ROOT_FIRMWARE fails its SHA256SUMS"
	aop_ok=0
else
	want=$(sort "$SHOWCASE_ROOT_FIRMWARE/SHA256SUMS")
	got() { rsh 30 "cd /lib/firmware/apple/aop 2>/dev/null && sudo -n sha256sum $fw_names 2>/dev/null" | sort; }
	if [ "$(got)" = "$want" ]; then
		note firmware PASS "4 private AOP files match"
	else
		rsh 20 'sudo -n install -d -m 0755 /lib/firmware/apple/aop'
		for file in $fw_names; do
			rsh 30 "sudo -n tee /lib/firmware/apple/aop/$file >/dev/null && sudo -n chmod 0600 /lib/firmware/apple/aop/$file" \
				<"$SHOWCASE_ROOT_FIRMWARE/$file"
		done
		if [ "$(got)" = "$want" ]; then
			note firmware PASS "4 private AOP files installed"
		else
			note firmware FAIL "/lib/firmware/apple/aop differs after install"
			aop_ok=0
		fi
	fi
fi

step "AOP: deferred cold trigger (leg A)"
release=$(rsh 20 'cat /sys/bus/platform/devices/210800000.aop/cold_release' 2>/dev/null | tr -d '\r')
case $release in
"defer=1 triggered=0" | "defer=2 triggered=0")
	if [ "$aop_ok" = 1 ]; then
		AOP_CAPTURE_DIR="$RUN/aop-trigger" AOP_CAPTURE_HOLD_SECONDS=60 \
			bounded 480 scripts/periph/aop/trigger-cold.sh showcase-cold >"$RUN/aop-trigger.txt" 2>&1
		echo "trigger-cold.sh exit $? (capture $RUN/aop-trigger)"
		grep -E 'SPI0 ' "$RUN/aop-trigger/dmesg.txt" >"$RUN/spi0-lines.txt" 2>/dev/null
		trigger="triggered now"
	else
		trigger="not triggered (firmware)"
	fi
	;;
"defer="*" triggered=1") trigger="already triggered this boot" ;;
*) trigger="cold_release '${release:-unreadable}'" ;;
esac
echo "$trigger"

step "AOP: streaming and IIO"
rsh 60 'bash -s' >"$RUN/aop-verify.txt" 2>&1 <<'REMOTE'
state=/sys/kernel/debug/apple-a9-aop-active/state
echo "== state1"; sudo -n cat "$state"
sleep 3
echo "== state2"; sudo -n cat "$state"
echo "== dmesg"; sudo -n dmesg | grep -E 'SPI0 reset decision=|SPI0 CH3TD|AOP crashlog'
echo "== iio"
for d in /sys/bus/iio/devices/iio:device*; do
	[ "$(cat "$d/name" 2>/dev/null)" = apple-a9-aop-accel ] || continue
	echo "dev $d $(cat "$d/in_accel_x_raw") $(cat "$d/in_accel_y_raw") $(cat "$d/in_accel_z_raw")"
done
REMOTE
aop=$(python3 - "$RUN/aop-verify.txt" <<'PY'
import re, sys
text = open(sys.argv[1]).read()
parts = dict(re.findall(r'^== (\w+)\n(.*?)(?=^== |\Z)', text, re.S | re.M))
bad = []
def fields(line):
    return dict(w.split('=', 1) for w in line.split() if '=' in w)
accel = []
for name in ('state1', 'state2'):
    lines = parts.get(name, '').splitlines()
    head = fields(lines[0]) if lines else {}
    if head.get('phase') != 'streaming':
        bad.append(f'{name} phase={head.get("phase")}')
    if 'iop_crashed=0' not in parts.get(name, ''):
        bad.append(f'{name} iop_crashed')
    line = next((l for l in lines if l.startswith('accel cal_sent=')), '')
    accel.append(fields(line))
for a in accel:
    want = dict(cal_sent='3', cal_unsupported='1', cal_state='2', iio='1', report_errors='0')
    if any(a.get(k) != v for k, v in want.items()):
        bad.append('accel ' + ' '.join(f'{k}={a.get(k)}' for k in want))
        break
r = [int(a.get('reports', -1)) for a in accel]
if not r[0] < r[1]:
    bad.append(f'reports not increasing {r}')
log = parts.get('dmesg', '')
if log.count('SPI0 reset decision=skip') != 1 or 'decision=run' in log:
    bad.append('SPI0 decision lines: ' + '; '.join(l.split(': ', 1)[-1][:60] for l in log.splitlines() if 'decision=' in l))
if 'SPI0 CH3TD' in log:
    bad.append('SPI0 CH3TD on a cold boot')
if 'AOP crashlog' in log:
    bad.append('AOP crashlog')
dev = [l.split() for l in parts.get('iio', '').splitlines() if l.startswith('dev ')]
if not dev:
    bad.append('no apple-a9-aop-accel IIO device')
if bad:
    print('BAD ' + '; '.join(bad))
else:
    print(f'OK streaming, reports {r[0]}->{r[1]}, cal 3/1/2, {dev[0][1].rsplit("/", 1)[1]} xyz {" ".join(dev[0][2:5])}')
PY
)
if [ "${aop%% *}" = OK ]; then
	note aop PASS "$trigger; ${aop#OK }"
else
	note aop FAIL "$trigger; ${aop#BAD }"
fi

# ---------------------------------------------------------------- rotate
step "auto-rotate"
rsh 120 'bash -s' >"$RUN/rotate.txt" 2>&1 <<REMOTE
$USER_ENV
systemctl is-active -q iio-sensor-proxy || sudo -n systemctl start iio-sensor-proxy
acc=
for _ in \$(seq 20); do
	acc=\$(timeout 5 busctl get-property net.hadess.SensorProxy /net/hadess/SensorProxy net.hadess.SensorProxy HasAccelerometer 2>/dev/null)
	[ "\$acc" = "b true" ] && break
	sleep 1
done
echo "proxy=\$(systemctl is-active iio-sensor-proxy)"
echo "accelerometer=\${acc:-none}"
s=\$(ipad-autorotate --status 2>/dev/null)
if ! systemctl --user is-active -q ipad-autorotate || ! printf '%s' "\$s" | grep -q '"sensor": "'; then
	echo "restart=yes"
	timeout 20 systemctl --user restart ipad-autorotate
	for _ in \$(seq 15); do
		sleep 1
		s=\$(ipad-autorotate --status 2>/dev/null)
		printf '%s' "\$s" | grep -q '"sensor": "' && break
	done
fi
echo "unit=\$(systemctl --user is-active ipad-autorotate)"
echo "status=\$s"
REMOTE
rotate=$(python3 - "$RUN/rotate.txt" <<'PY'
import json, sys
kv = dict(l.split('=', 1) for l in open(sys.argv[1]).read().splitlines() if '=' in l)
try:
    status = json.loads(kv.get('status') or '{}')
except ValueError:
    status = {}
bad = []
if kv.get('accelerometer') != 'b true':
    bad.append(f'iio-sensor-proxy ({kv.get("proxy")}) HasAccelerometer={kv.get("accelerometer")}')
if kv.get('unit') != 'active':
    bad.append(f'ipad-autorotate {kv.get("unit")}')
if status.get('source') != 'proxy' or not status.get('sensor'):
    bad.append(f'autorotate source={status.get("source")} sensor={status.get("sensor")}')
if bad:
    print('BAD ' + '; '.join(bad))
elif status.get('locked'):
    print(f'LOCK sensor={status["sensor"]}, rotation lock ON (ipad-rotate lock off)')
else:
    print(f'OK proxy has the accelerometer; autorotate sensor={status["sensor"]} screen={status.get("orientation")}'
          + (' (restarted)' if kv.get('restart') else ''))
PY
)
case ${rotate%% *} in
OK) note rotate PASS "${rotate#OK }" ;;
LOCK) note rotate WARN "${rotate#LOCK }" ;;
*) note rotate FAIL "${rotate#BAD }" ;;
esac

# ---------------------------------------------------------------- cpu
step "CPU: 2160 MHz hold"
cpu_status() { rsh 30 "sudo -n python3 -B $HOLD/a9-oc-hold.py status" 2>/dev/null; }
held() { python3 -c 'import json,sys; sys.exit(0 if json.load(sys.stdin).get("held") else 1)' 2>/dev/null; }
cpu_fail=
rsh 20 "sudo -n install -d -m 0700 $HOLD"
for file in scripts/power/ladder.py scripts/power/a9_thermal.py scripts/showcase/device/a9-oc-hold.py; do
	name=${file##*/}
	sum=$(shasum -a 256 "$file" | cut -d' ' -f1)
	if [ "$(rsh 20 "sudo -n sha256sum $HOLD/$name 2>/dev/null" | cut -d' ' -f1)" != "$sum" ]; then
		rsh 30 "sudo -n tee $HOLD/$name.tmp >/dev/null && sudo -n mv $HOLD/$name.tmp $HOLD/$name" <"$file"
		[ "$(rsh 20 "sudo -n sha256sum $HOLD/$name" | cut -d' ' -f1)" = "$sum" ] || cpu_fail="copy of $name failed"
	fi
done
if [ -z "$cpu_fail" ]; then
	active=$(rsh 20 "systemctl is-active $UNIT" 2>/dev/null)
	if [ "$active" = active ] && cpu_status | held; then
		echo "already holding $KHZ"
		started="already held"
	else
		[ "$active" = active ] && rsh 30 "sudo -n systemctl stop $UNIT"
		rsh 40 "sudo -n python3 -B $HOLD/a9-oc-hold.py preflight" >"$RUN/cpu-preflight.json" 2>&1
		if ! python3 -c 'import json,sys; sys.exit(0 if json.load(open(sys.argv[1]))["ok"] else 1)' "$RUN/cpu-preflight.json" 2>/dev/null; then
			cpu_fail="preflight: $(python3 -c 'import json,sys
d = json.load(open(sys.argv[1]))
print("; ".join("%s: %s" % (c["check"], c["detail"]) for c in d["checks"] if not c["ok"]))' "$RUN/cpu-preflight.json" 2>/dev/null || head -c 300 "$RUN/cpu-preflight.json")"
		else
			rsh 30 "sudo -n systemctl reset-failed $UNIT 2>/dev/null; sudo -n systemd-run --unit=$UNIT --collect \
				--description='linux-a9 showcase 2160 MHz hold' -p Nice=-15 -p TimeoutStopSec=15 \
				/usr/bin/python3 -B $HOLD/a9-oc-hold.py hold --khz $KHZ ${GOVERNOR:+--governor $GOVERNOR}"
			started="started"
			for _ in $(seq 15); do
				sleep 1
				cpu_status | held && break
			done
		fi
	fi
fi
if [ -z "$cpu_fail" ]; then
	cpu_status >"$RUN/cpu-status.json"
	cpu_check=$(python3 - "$RUN/cpu-status.json" "$KHZ" <<'PY'
import json, sys
try:
    d = json.load(open(sys.argv[1]))
except ValueError:
    print('BAD no status from a9-oc-hold.py')
    sys.exit()
khz = sys.argv[2]
p, g, daemon = d['policy'], d['guard'], d.get('daemon') or {}
bad = []
if not d.get('held'):
    bad.append(f'guard not holding: armed={g.get("armed")} reason={g.get("reason")} daemon={daemon.get("state")} {daemon.get("why", "")}')
for key, want in (('cpuinfo_max_freq', khz), ('scaling_max_freq', khz), ('boost', '1')):
    if p.get(key) != want:
        bad.append(f'{key}={p.get(key)}')
print('BAD ' + '; '.join(bad) if bad else
      f'OK cpuinfo_max_freq {p["cpuinfo_max_freq"]}, {p["scaling_governor"]}, delta_uv {g["delta_uv"]}, '
      f'{g["profile"]} {int(g["passive_millic"]) // 1000} C')
PY
)
	if [ "${cpu_check%% *}" != OK ]; then
		cpu_fail=${cpu_check#BAD }
	else
		rsh 90 "sudo -n python3 -B $HOLD/a9-oc-hold.py load-test --seconds 8" >"$RUN/cpu-load.json" 2>&1
		load=$(python3 - "$RUN/cpu-load.json" <<'PY'
import json, sys
try:
    d = json.loads(open(sys.argv[1]).read().strip().splitlines()[-1])
except (ValueError, IndexError):
    print('BAD load-test gave no result')
    sys.exit()
text = (f'{d["at_target"]}/{d["samples"]} samples at {d["target_khz"] // 1000} MHz, '
        f'median {int(d["median_khz"]) // 1000} MHz, hottest {d["hottest_millic"] / 1000:.1f} C')
print(('OK ' if d['ok'] else 'BAD ') + text + ('' if d['ok'] else f' {d.get("error") or ""}'))
PY
)
		if [ "${load%% *}" = OK ] && cpu_status | held; then
			note cpu PASS "$started; ${cpu_check#OK }; load ${load#OK }"
		else
			cpu_fail="${cpu_check#OK }; load ${load#* }"
		fi
	fi
fi
[ -n "$cpu_fail" ] && note cpu FAIL "$cpu_fail (journal: sudo journalctl -u $UNIT)"

# ---------------------------------------------------------------- als
step "auto-brightness"
rsh 60 'bash -s' >"$RUN/als.txt" 2>&1 <<REMOTE
$USER_ENV
ls /sys/bus/iio/devices/iio:device*/in_illuminance_input /sys/bus/iio/devices/iio:device*/in_illuminance_raw 2>/dev/null | head -n 1 | sed 's/^/sensor=/'
if ipad-als-brightness enabled; then
	echo "enabled=on"
else
	timeout 10 ipad-als-brightness on >/dev/null 2>&1
	if ipad-als-brightness enabled; then echo "enabled=switched-on"; else echo "enabled=off"; fi
fi
if ! systemctl --user is-active -q ipad-als-brightness; then
	echo "restart=yes"
	timeout 20 systemctl --user restart ipad-als-brightness
	sleep 3
fi
echo "unit=\$(systemctl --user is-active ipad-als-brightness)"
echo "status=\$(timeout 10 ipad-als-brightness status 2>&1 | tr '\n' ' ' | cut -c1-200)"
REMOTE
als=$(python3 - "$RUN/als.txt" <<'PY'
import sys
kv = dict(l.split('=', 1) for l in open(sys.argv[1]).read().splitlines() if '=' in l)
bad = [m for m, ok in ((f'no IIO light sensor', kv.get('sensor')),
                       (f'auto-brightness {kv.get("enabled")}', kv.get('enabled') in ('on', 'switched-on')),
                       (f'ipad-als-brightness {kv.get("unit")}', kv.get('unit') == 'active')) if not ok]
if bad:
    print('BAD ' + '; '.join(bad))
else:
    print(('WAS ' if kv['enabled'] == 'switched-on' else 'OK ') + 'on, ' + kv.get('status', '').strip()[:120])
PY
)
case ${als%% *} in
OK) note als PASS "${als#OK }" ;;
WAS) note als WARN "was off, switched on; ${als#WAS on, }" ;;
*) note als FAIL "${als#BAD }" ;;
esac

# ---------------------------------------------------------------- fastfetch
step "fastfetch"
config=scripts/omarchy/rootfs/tablet/fastfetch.jsonc
sum=$(shasum -a 256 "$config" | cut -d' ' -f1)
# shellcheck disable=SC2016 # expanded on the iPad
remote=$(rsh 20 'f=~/.config/fastfetch/config.jsonc; [ -e "$f" ] && { sha256sum "$f" | cut -d" " -f1; head -n 1 "$f"; }')
ff_note=
case $remote in
"$sum"$'\n'*) ff_note="config current" ;;
'' | *$'\n''// linux-a9'*)
	rsh 30 'install -d ~/.config/fastfetch && cat > ~/.config/fastfetch/config.jsonc.tmp && mv ~/.config/fastfetch/config.jsonc.tmp ~/.config/fastfetch/config.jsonc' <"$config"
	[ "$(rsh 20 'sha256sum ~/.config/fastfetch/config.jsonc' | cut -d' ' -f1)" = "$sum" ] && ff_note="config installed"
	;;
*) ff_note="user's own config kept" ;;
esac
rsh 60 "$USER_ENV fastfetch --logo none --pipe 2>/dev/null" >"$RUN/fastfetch.txt" 2>&1
cpu_line=$(grep -m 1 'Twister' "$RUN/fastfetch.txt" | sed 's/^[^A]*Apple/Apple/')
wifi_line=$(grep -m 1 'Wi-Fi' "$RUN/fastfetch.txt" | sed 's/^[^W]*Wi-Fi/Wi-Fi/')
if [ -z "$ff_note" ] || [ -z "$cpu_line" ]; then
	note fastfetch FAIL "${ff_note:-config install failed}; ${cpu_line:-no CPU line}"
elif [ "$ff_note" = "user's own config kept" ] || [[ $cpu_line != *"@ 2160 MHz"* ]] || [[ $wifi_line != *"GHz connected"* ]]; then
	note fastfetch WARN "$ff_note; $cpu_line | ${wifi_line:-no Wi-Fi line}"
else
	note fastfetch PASS "$ff_note; $cpu_line | $wifi_line"
fi

summary
