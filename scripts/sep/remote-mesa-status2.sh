#!/bin/sh
# Device-side Mesa2: one armed status transfer, automatic rail teardown.
# ENTRY must be the exact packaged tri53-mesa2-<sha> name (see next-run doc).
set -eu
ENTRY=${ENTRY:?set ENTRY to the exact tri53-mesa2 entry}
case "$ENTRY" in tri53-mesa2-??????????) ;; *) exit 2 ;; esac
D=/sys/kernel/debug/mesa-a9
TMP=/tmp/mesa-status2.$$
ARMED=0
REG_3V0=
REG_1V8=

rail_bits()
{
    S312=$(cat "$REG_3V0/state")
    S319=$(cat "$REG_1V8/state")
    printf 'pmic_%s reg312_3v0=%s reg319_1v8=%s\n' "$2" "$S312" "$S319"
    case "$1:$S312:$S319" in
    0:disabled:disabled | 1:enabled:enabled) return 0 ;;
    *) echo 'STOP: unexpected Mesa regulator readback' >&2; return 3 ;;
    esac
}

cleanup()
{
    rc=$?
    trap - EXIT HUP INT TERM
    if [ "$ARMED" -eq 1 ]; then
        # arm=0 is permitted after auto-disarm too, and retries any failed
        # regulator disable. Never issue another status read here.
        printf '0\n' >"$D/arm" 2>/dev/null || rc=3
        cat "$D/state" || rc=3
        rail_bits 0 cleanup || rc=3
    fi
    rm -f "$TMP"
    test "$rc" -eq 0 || echo "RESULT mesa-status2=fail rc=$rc" >&2
    exit "$rc"
}
trap cleanup EXIT
trap 'exit 130' HUP INT TERM

CMDLINE=" $(cat /proc/cmdline) "
case "$CMDLINE" in *" linux_a9.entry=$ENTRY "*) ;; *) echo 'STOP: wrong entry' >&2; exit 2 ;; esac
case "$CMDLINE" in *' mesa_a9.enable=1 '*) ;; *) exit 2 ;; esac
case "$CMDLINE" in *' sep_a9.enable=1 '* | *' systemd.debug_shell='* | *' loglevel='* | *' ignore_loglevel '*) exit 2 ;; esac
test "$(printf '%s\n' "$CMDLINE" | tr ' ' '\n' | grep -c '^pcie_apple_h9p.ans_keep_power=1$')" -eq 1
for f in state arm power status irq-count; do test -e "$D/$f"; done

# Use the regulator class, never an arbitrary debugfs regmap. The PMIC
# debugfs range is 0-0; the codec also has registers 0x312/0x319.
find_reg()
{
    found=
    for r in /sys/class/regulator/regulator.*; do
        test "$(cat "$r/name" 2>/dev/null)" = "$1" || continue
        test -z "$found" || return 2
        found=$r
    done
    test -n "$found" || return 2
    echo "$found"
}
REG_3V0=$(find_reg mesa-3v0)
REG_1V8=$(find_reg mesa-1v8)
echo "pmic_regulators 3v0=$REG_3V0 1v8=$REG_1V8"
INITIAL=$(cat "$D/state")
printf '%s\n' "$INITIAL"
printf '%s\n' "$INITIAL" | grep -qx 'armed=0 arm_used=0 powered=0 rail_1v8=0 rail_3v0=0'
printf '%s\n' "$INITIAL" | grep -qx 'spi_mode=1 mode_supported=1 configured=0 bus_hz=8000000 xfer_hz=8000000'
printf '%s\n' "$INITIAL" | grep -qx 'status_max_attempts=1 experiment=mesa2'
printf '%s\n' "$INITIAL" | grep -qx 'irq=0 irq_requested=0 irq_enabled=0 irq_count=0 last_irq_jiffies=0'
printf '%s\n' "$INITIAL" | grep -qx 'status_attempts=0 last_status_err=0 last_status_valid=0'
rail_bits 0 baseline
# Match Apple's board power-off interval before the only power-up, including
# when entered by warm kexec. Both physical enables were just read as off.
sleep 1
rail_bits 0 baseline_settled

ARMED=1
printf '1\n' >"$D/arm"
printf 'on\n' >"$D/power"
ON=$(cat "$D/state")
printf '%s\n' "$ON"
printf '%s\n' "$ON" | grep -qx 'armed=1 arm_used=1 powered=1 rail_1v8=1 rail_3v0=1'
rail_bits 1 powered

# Exactly one status read. The kernel performs one transfer and then powers
# off/disarms before returning text, including on transport/header failures.
cat "$D/status" >"$TMP"
cat "$TMP"
FINAL=$(cat "$D/state")
printf '%s\n' "$FINAL"
rail_bits 0 powered_off
printf '%s\n' "$FINAL" | grep -qx 'armed=0 arm_used=1 powered=0 rail_1v8=0 rail_3v0=0'
printf '%s\n' "$FINAL" | grep -qx 'status_attempts=1 last_status_err=0 last_status_valid=0'
grep -qx 'spi_error=0 valid=1 attempts=1' "$TMP"
grep -qx 'tx=80 10 00 07 00 00 00 ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff' "$TMP"
ARMED=0
echo 'RESULT mesa-status2=pass'
