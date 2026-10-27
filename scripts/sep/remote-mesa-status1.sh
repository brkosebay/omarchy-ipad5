#!/bin/sh
# Device-side, one-shot J71t Mesa status probe for tri53-mesa1-cc0b92c36c.
#
# This touches only the two volatile Mesa rail-enable bits and performs one
# 23-byte, full-duplex SPI status transaction.  It never talks to SEP, captures
# a frame, uploads a patch, or writes sensor/persistent storage.
set -eu

ENTRY=tri53-mesa1-cc0b92c36c
D=/sys/kernel/debug/mesa-a9
TMP=/tmp/mesa-status1.$$
ARMED=0

cleanup()
{
	rc=$?
	trap - EXIT HUP INT TERM
	if [ "$ARMED" -eq 1 ]; then
		printf 'off\n' >"$D/power" 2>/dev/null || true
		printf '0\n' >"$D/arm" 2>/dev/null || true
	fi
	rm -f "$TMP"
	exit "$rc"
}
trap cleanup EXIT HUP INT TERM

case " $(cat /proc/cmdline) " in
*" linux_a9.entry=$ENTRY "*) ;;
*) echo "STOP: wrong entry (expected $ENTRY)" >&2; exit 2 ;;
esac
case " $(cat /proc/cmdline) " in
*' mesa_a9.enable=1 '*) ;;
*) echo 'STOP: mesa_a9.enable=1 is absent' >&2; exit 2 ;;
esac
case " $(cat /proc/cmdline) " in
*' sep_a9.enable=1 '*) echo 'STOP: SEP driver must remain disabled' >&2; exit 2 ;;
*) ;;
esac

for f in state arm power status irq-count; do
	test -e "$D/$f" || {
		echo "STOP: missing $D/$f" >&2
		exit 2
	}
done

# Read the two Mesa enable bits through the regulator class. The D2365
# debugfs regmap on this kernel exposes only register 0 (range 0-0), and a
# search for "any regmap with 0x312/0x319" picks the CS42L81 codec (spi1.0)
# instead (2026-10-27 16:52 run). regulator_is_enabled_regmap() reads
# 0x312 (LDO7, 3V0) and 0x319 (LDO15, 1V8) bit 0 from the PMIC's own regmap.
find_reg()
{
	found=
	for r in /sys/class/regulator/regulator.*; do
		test "$(cat "$r/name" 2>/dev/null)" = "$1" || continue
		test -z "$found" || {
			echo "STOP: more than one regulator named $1" >&2
			exit 2
		}
		found=$r
	done
	test -n "$found" || {
		echo "STOP: regulator $1 not found" >&2
		exit 2
	}
	echo "$found"
}
REG_3V0=$(find_reg mesa-3v0)
REG_1V8=$(find_reg mesa-1v8)
echo "pmic_regulators 3v0=$REG_3V0 1v8=$REG_1V8"

require_rail_bits()
{
	expected=$1
	phase=$2
	S312=$(cat "$REG_3V0/state")
	S319=$(cat "$REG_1V8/state")
	printf 'pmic_%s reg312_3v0=%s reg319_1v8=%s\n' "$phase" "$S312" "$S319"
	case "$expected:$S312:$S319" in
	0:disabled:disabled | 1:enabled:enabled) ;;
	*)
		echo "STOP: Mesa rail bits are not $expected during $phase" >&2
		exit 3
		;;
	esac
}

INITIAL=$(cat "$D/state")
printf '%s\n' "$INITIAL"
printf '%s\n' "$INITIAL" |
	grep -qx 'armed=0 arm_used=0 powered=0 rail_1v8=0 rail_3v0=0'
printf '%s\n' "$INITIAL" |
	grep -qx 'spi_mode=0 mode_supported=1 configured=0 bus_hz=8000000 xfer_hz=8000000'
printf '%s\n' "$INITIAL" |
	grep -Eq '^irq=0 irq_requested=0 irq_enabled=0 irq_count=0 last_irq_jiffies=0$'
printf '%s\n' "$INITIAL" |
	grep -qx 'status_attempts=0 last_status_err=0 last_status_valid=0'
require_rail_bits 0 baseline

printf '1\n' >"$D/arm"
ARMED=1
printf 'on\n' >"$D/power"

ON=$(cat "$D/state")
printf '%s\n' "$ON"
printf '%s\n' "$ON" |
	grep -qx 'armed=1 arm_used=1 powered=1 rail_1v8=1 rail_3v0=1'
printf '%s\n' "$ON" |
	grep -qx 'status_attempts=0 last_status_err=0 last_status_valid=0'
require_rail_bits 1 powered

# This cat is the only sensor transaction in the script.  Never repeat it in
# this boot, even if the transport or header validation fails.
cat "$D/status" >"$TMP"
cat "$TMP"
grep -qx 'spi_error=0 valid=1 attempts=1' "$TMP"
grep -qx 'tx=80 10 00 07 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00' "$TMP"

AFTER=$(cat "$D/state")
printf '%s\n' "$AFTER"
printf '%s\n' "$AFTER" |
	grep -qx 'status_attempts=1 last_status_err=0 last_status_valid=1'

printf 'off\n' >"$D/power"
require_rail_bits 0 powered_off
printf '0\n' >"$D/arm"
ARMED=0

FINAL=$(cat "$D/state")
printf '%s\n' "$FINAL"
printf '%s\n' "$FINAL" |
	grep -qx 'armed=0 arm_used=1 powered=0 rail_1v8=0 rail_3v0=0'
printf '%s\n' "$FINAL" |
	grep -qx 'status_attempts=1 last_status_err=0 last_status_valid=0'

rm -f "$TMP"
trap - EXIT HUP INT TERM
echo 'RESULT mesa-status=pass'
