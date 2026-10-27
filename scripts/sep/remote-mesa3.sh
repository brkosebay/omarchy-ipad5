#!/bin/sh
# Device-side integrated Touch ID path: SBIO patch fetch -> volatile Mesa patch
# upload -> one real sensor frame. Mesa4 additionally relays that frame through
# the real MatchMode and image-processing chain. The frame never leaves kernel
# memory in either mode.
set -eu

ENTRY=${ENTRY:?set ENTRY to the exact tri53-mesa3 entry}
case "$ENTRY" in
tri53-mesa3-??????????) ACTION=capture; RESULT_NAME=mesa3 ;;
tri53-mesa4-??????????) ACTION=match; RESULT_NAME=mesa4 ;;
*) exit 2 ;;
esac

S=/sys/kernel/debug/sep-a9
M=/sys/kernel/debug/mesa-a9
X=/tmp/mesa3-xfer.$$
ARMED=0
REG_3V0=
REG_1V8=

rail_bits()
{
    s30=$(cat "$REG_3V0/state")
    s18=$(cat "$REG_1V8/state")
    printf 'mesa_rails_%s 3v0=%s 1v8=%s\n' "$2" "$s30" "$s18"
    case "$1:$s30:$s18" in
    0:disabled:disabled | 1:enabled:enabled) return 0 ;;
    *) echo 'STOP: unexpected Mesa regulator state' >&2; return 3 ;;
    esac
}

cleanup()
{
    rc=$?
    trap - EXIT HUP INT TERM
    if [ "$ARMED" -eq 1 ]; then
        printf '0\n' >"$M/arm" 2>/dev/null || rc=3
        cat "$M/state" || rc=3
        rail_bits 0 cleanup || rc=3
    fi
    rm -f "$X"
    test "$rc" -eq 0 || echo "RESULT $RESULT_NAME=fail rc=$rc" >&2
    exit "$rc"
}
trap cleanup EXIT
trap 'exit 130' HUP INT TERM

CMDLINE=" $(cat /proc/cmdline) "
case "$CMDLINE" in *" linux_a9.entry=$ENTRY "*) ;; *) echo 'STOP: wrong entry'; exit 2 ;; esac
case "$CMDLINE" in *' mesa_a9.enable=1 '*) ;; *) echo 'STOP: Mesa driver not enabled'; exit 2 ;; esac
case "$CMDLINE" in *' sep_a9.enable=1 '*) ;; *) echo 'STOP: SEP driver not enabled'; exit 2 ;; esac
case "$CMDLINE" in *' systemd.debug_shell='* | *' ignore_loglevel '* | *' loglevel='*) echo 'STOP: forbidden debug cmdline'; exit 2 ;; esac
test "$(printf '%s\n' "$CMDLINE" | tr ' ' '\n' | grep -c '^pcie_apple_h9p.ans_keep_power=1$')" -eq 1
for f in status endpoints fifo sbio ool xfer arts-start; do test -e "$S/$f"; done
for f in state arm power patch-hex patch capture; do test -e "$M/$f"; done
if [ "$ACTION" = match ]; then test -e "$M/match"; fi

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

echo '== require the proven ARTS+xARM gate and eight endpoints'
cat "$S/arts-start"; cat "$S/status"; cat "$S/endpoints"; cat "$S/fifo"
grep -q 'ran=1 result=2 ret=0 ep=2 manifest_len=6157' "$S/arts-start" || { echo 'STOP: MANIFEST gate failed'; exit 1; }
grep -q '^tx=10 rx=17 tx_busy=0 dropped=0 ' "$S/status" || { echo 'STOP: combined counters differ'; exit 1; }
grep -q 'endpoints_complete=8 published=2' "$S/status" || { echo 'STOP: endpoint gate differs'; exit 1; }
grep -Eq '^sbio_ep=-1 sbio_ready=0( |$)' "$S/status" || { echo 'STOP: SBIO is not pristine'; exit 1; }
grep -Eq '^0x08 sbio +mailbox 4-4 +4-4 +complete max_send=0x4000 max_recv=0x4000$' "$S/endpoints" || { echo 'STOP: SBIO advertisement differs'; exit 1; }
grep -q '^ap->sep snd_sts=.* (empty)$' "$S/fifo"
grep -q '^sep->ap recv_sts=.* (empty)$' "$S/fifo"

echo '== publish SBIO and initialize GenericTransfer'
echo '08' >"$S/sbio"
echo '08 4000 4000' >"$S/ool"
sleep 0.2
grep -q '^tx=14 rx=21 tx_busy=0 dropped=0 ' "$S/status" || { echo 'STOP: SBIO OOL counters differ'; exit 1; }
echo '08 73 01000000' >"$S/xfer"
sleep 0.2
cat "$S/xfer"; cat "$S/status"; cat "$S/fifo"
grep -q '^out_len=0$' "$S/xfer"
grep -q '^ep=0x08 cmd=0x73 ret=0 sep_status=0x0$' "$S/xfer"
grep -Eq '^sbio_ep=8 sbio_ready=1( |$)' "$S/status"

echo '== fetch the SensorID-0x3350 volatile patch: SBIO 0x5f type 2'
echo '08 5f 0200' >"$S/xfer"
cat "$S/xfer" >"$X"
# Do not duplicate the proprietary patch blob into the run log.
sed -n '1p;3,$p' "$X"
grep -q '^ep=0x08 cmd=0x5f ret=0 sep_status=0x0$' "$X" || { echo 'STOP: patch fetch rejected'; exit 1; }
OUT_LEN=$(sed -n 's/^out_len=//p' "$X")
case "$OUT_LEN" in ''|*[!0-9]*) echo 'STOP: invalid patch length'; exit 1 ;; esac
test "$OUT_LEN" -gt 0 && test "$OUT_LEN" -le 65536 || { echo 'STOP: patch length outside 1..65536'; exit 1; }
sed -n '2p' "$X" | grep -Eq '^[0-9a-f]+$' || { echo 'STOP: patch reply is not hex'; exit 1; }
HEX_CHARS=$(sed -n '2p' "$X" | wc -c)
HEX_CHARS=$((HEX_CHARS - 1))
test "$HEX_CHARS" -eq $((OUT_LEN * 2)) || { echo 'STOP: patch hex length mismatch'; exit 1; }
grep -q '^ap->sep snd_sts=.* (empty)$' "$S/fifo"
grep -q '^sep->ap recv_sts=.* (empty)$' "$S/fifo"

echo '== arm Mesa, stage patch without powering the sensor'
INITIAL=$(cat "$M/state"); printf '%s\n' "$INITIAL"
printf '%s\n' "$INITIAL" | grep -q '^armed=0 arm_used=0 powered=0 rail_1v8=0 rail_3v0=0$'
printf '%s\n' "$INITIAL" | grep -q '^spi_mode=1 mode_supported=1 configured=0 bus_hz=8000000 xfer_hz=8000000$'
rail_bits 0 baseline
sleep 1
printf '1\n' >"$M/arm"
ARMED=1
sed -n '2p' "$X" >"$M/patch-hex"
STAGED=$(cat "$M/state"); printf '%s\n' "$STAGED"
printf '%s\n' "$STAGED" | grep -q "^patch_staged=$OUT_LEN patch_half=-1 patch_attempted=0 patch_loaded=0 "

echo '== one powered session: upload patch and wait for a finger'
printf 'on\n' >"$M/power"
rail_bits 1 powered
printf 'apply\n' >"$M/patch"
PATCHED=$(cat "$M/state"); printf '%s\n' "$PATCHED"
printf '%s\n' "$PATCHED" | grep -Eq '^patch_staged=0 patch_half=-1 patch_attempted=1 patch_loaded=1 patch_version=[0-9]+ patch_result=0$' || { echo 'STOP: Mesa patch did not load'; exit 1; }

echo 'PLACE A FINGER FLAT ON TOUCH ID NOW (30 second window)'
printf 'once\n' >"$M/$ACTION"
CAPTURED=$(cat "$M/state"); printf '%s\n' "$CAPTURED"
printf '%s\n' "$CAPTURED" | grep -Eq '^capture_attempted=1 capture_complete=1 capture_len=19668 capture_result=0 capture_polls=[1-9][0-9]* frame_retained=0$' || { echo 'STOP: no complete Mesa frame'; exit 1; }
if [ "$ACTION" = match ]; then
    printf '%s\n' "$CAPTURED" | grep -Eq '^relay_attempted=1 relay_complete=1 relay_result=0 relay_failed_cmd=0(x0)? relay_sep_status=0(x0)? relay_reply_len=192$' || { echo 'STOP: Mesa frame did not complete the SBIO relay'; exit 1; }
fi

echo '== success; power down and prove no frame was retained'
printf '0\n' >"$M/arm"
ARMED=0
FINAL=$(cat "$M/state"); printf '%s\n' "$FINAL"
rail_bits 0 final
printf '%s\n' "$FINAL" | grep -q '^armed=0 arm_used=1 powered=0 rail_1v8=0 rail_3v0=0$'
printf '%s\n' "$FINAL" | grep -q 'capture_attempted=1 capture_complete=1 capture_len=19668 capture_result=0 .* frame_retained=0$'
cat "$S/status"; cat "$S/fifo"
grep -Eq '^sbio_ep=8 sbio_ready=1( |$)' "$S/status"
grep -q '^ap->sep snd_sts=.* (empty)$' "$S/fifo"
grep -q '^sep->ap recv_sts=.* (empty)$' "$S/fifo"
rm -f "$X"
if [ "$ACTION" = match ]; then
    echo "RESULT mesa4=pass patch_bytes=$OUT_LEN frame_bytes=19668 sbio_reply_bytes=192"
else
    echo "RESULT mesa3=pass patch_bytes=$OUT_LEN frame_bytes=19668"
fi
