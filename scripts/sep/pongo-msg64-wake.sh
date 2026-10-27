#!/usr/bin/env bash
# Orchestrator only. Run after `pongo-sep.sh --xnu ...` prints the discovery
# end marker and while the same fresh-DFU pongoOS session is still active.
#
# The 16:42 run exposed an AP outbox-underflow fault: the old `sep stat`
# sampled an empty RX data register after printing clean status. This runner
# first proves the patched status command stays fault-free through the prior
# retry interval, then (and only then) sends one AP->SEP word:
#
#   sep stat
#   [65-second passive hold]
#   sep stat
#   sep msg64 0 0 e 0 0        # only if both stats are fault-free and TX empty
#   sep stat
#
# No OOL, wrapper-mask, PMGR, RUN, NMI, fault clear, or second mailbox write.
# Never run it twice in one boot once AP->SEP64 was printed.
#
# Usage: scripts/sep/pongo-msg64-wake.sh [OUT]
# Exit: 0 = fault-free preflight and AP->SEP FIFO drained
#       1 = pongoOS was not reached and no write marker appeared; may retry once
#       3 = incomplete output; do not retry if the write marker appeared
#       4 = AP->SEP FIFO was not empty; nothing sent, cold-reset
#       5 = the single 64-bit word remained queued; cold-reset
#       6 = RX overflow/underflow fault existed before send; nothing sent
#       7 = RX overflow/underflow fault appeared after send; cold-reset
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PONGOTERM="$ROOT/build/bin/pongoterm"
LOG="$ROOT/build/sep/pongo-sep.log"
OUT="${1:-$ROOT/build/sep/$(date +%Y%m%d-%H%M)-pongo-msg64-wake.log}"

[ -x "$PONGOTERM" ] || {
	echo "pongo-msg64-wake: no pongoterm at $PONGOTERM" >&2
	exit 1
}
mkdir -p "$(dirname "$LOG")" "$(dirname "$OUT")"

tmp=$(mktemp -d)
fifo="$tmp/in"
raw="$tmp/out"
mkfifo "$fifo"
"$PONGOTERM" <"$fifo" >"$raw" 2>&1 &
pid=$!
exec 9>"$fifo"

finish_capture() {
	exec 9>&-
	kill "$pid" 2>/dev/null || true
	wait "$pid" 2>/dev/null || true
	{
		echo "== pongo-msg64-wake $(date '+%F %T')"
		cat "$raw"
	} >>"$LOG"
	cp "$raw" "$OUT"
	rm -f "$fifo" "$raw"
	rmdir "$tmp"
	echo "log: $OUT"
}

printf 'sep stat\n' >&9
sleep 3
echo "pongo-msg64-wake: passive underflow hold 65 s" >&2
sleep 65
printf 'sep stat\n' >&9
sleep 5

python3 -B -I - "$raw" <<'PY'
import re
import sys
from pathlib import Path

text = Path(sys.argv[1]).read_text(errors="replace")
stats = re.findall(
    r"sep stat: recv_sts=([0-9a-f]+) snd_sts=([0-9a-f]+) total=(\d+)", text
)
if not stats:
    print("pongo-msg64-wake: no pongoOS status output; no word sent")
    sys.exit(1)
if len(stats) != 2:
    print(f"pongo-msg64-wake: passive preflight incomplete (stats={len(stats)}); no word sent")
    sys.exit(3)
if text.count("fifo_peek=0") != 2:
    print("pongo-msg64-wake: Pongo build lacks safe-stat marker; no word sent")
    sys.exit(6)

for index, (recv_hex, _, _) in enumerate(stats, 1):
    recv = int(recv_hex, 16)
    if recv & 0xC0000:
        print(
            "pongo-msg64-wake: RX fault before send "
            f"at stat {index} (recv_sts={recv:08x}, "
            f"overflow={bool(recv & 0x40000)}, underflow={bool(recv & 0x80000)})"
        )
        print("RESULT pongo-msg64=rx-fault-before")
        sys.exit(6)

snd = int(stats[-1][1], 16)
if not (snd & 0x20000):
    print(f"pongo-msg64-wake: AP->SEP FIFO busy before send (snd_sts={snd:08x})")
    print("RESULT pongo-msg64=busy-before")
    sys.exit(4)

print(
    "pongo-msg64-wake: passive preflight clean "
    f"recv={int(stats[0][0], 16):08x}->{int(stats[1][0], 16):08x} "
    f"ring={stats[0][2]}->{stats[1][2]}"
)
PY
preflight_rc=$?
if [ "$preflight_rc" -ne 0 ]; then
	finish_capture
	exit "$preflight_rc"
fi

printf 'sep msg64 0 0 e 0 0\n' >&9
sleep 1
printf 'sep stat\n' >&9
sleep 5
finish_capture

python3 -B -I - "$OUT" <<'PY'
import re
import sys
from pathlib import Path

text = Path(sys.argv[1]).read_text(errors="replace")
sent = "AP->SEP64 ep=0 tag=0 op=e param=0 data=0 raw=e0000" in text
refused = re.search(
    r"sep msg64: AP->SEP FIFO not empty \(snd_sts=([0-9a-f]+)\); not sent", text
)
stats = re.findall(
    r"sep stat: recv_sts=([0-9a-f]+) snd_sts=([0-9a-f]+) total=(\d+)", text
)

if refused:
    print(f"pongo-msg64-wake: FIFO raced busy (snd_sts={refused.group(1)}); nothing sent")
    print("RESULT pongo-msg64=busy-before")
    sys.exit(4)
if not sent or len(stats) != 3:
    print(f"pongo-msg64-wake: final output incomplete (sent={sent}, stats={len(stats)})")
    sys.exit(3)
if text.count("fifo_peek=0") != 3:
    print("pongo-msg64-wake: safe-stat marker missing after send")
    sys.exit(7)

recv_after = int(stats[-1][0], 16)
snd_after = int(stats[-1][1], 16)
print(f"pongo-msg64-wake: final recv_sts={recv_after:08x} snd_sts={snd_after:08x}")
print(f"pongo-msg64-wake: ring total {stats[0][2]} -> {stats[1][2]} -> {stats[2][2]}")
if recv_after & 0xC0000:
    print(
        "RESULT pongo-msg64=rx-fault-after "
        f"overflow={bool(recv_after & 0x40000)} underflow={bool(recv_after & 0x80000)}"
    )
    sys.exit(7)
if snd_after & 0x20000:
    print("RESULT pongo-msg64=drained")
    sys.exit(0)
print("RESULT pongo-msg64=stuck")
sys.exit(5)
PY
