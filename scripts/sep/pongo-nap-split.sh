#!/usr/bin/env bash
# Orchestrator only. Pongo-side SEP nap split (docs/sep/xart-handshake.md, SEP
# thread 2026-10-26): after `pongo-sep.sh --xnu status` has booted sepOS and the
# discovery capture is quiet, snapshot PMGR/SEP CPU/mailbox registers, send ONE
# endpoint-0 wake word (op 0x0e), snapshot again. No OOL, no second word, no bootm.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PONGO_TERM="$ROOT/build/bin/pongoterm"
OUT="${1:-$ROOT/docs/sep/logs/$(date +%Y%m%d-%H%M)-pongo-nap-split.log}"
mkdir -p "$(dirname "$OUT")"
peeks=(
	"peek 20e080400" "peek 20da00028" "peek 20da00044" "peek 20da00b88"
	"peek 20da04000" "peek 20da04004" "peek 20da04008" "peek 20da04020"
)
lines=("sep stat" "${peeks[@]}" "sep msg 0 0 e 0 0" "sep stat" "${peeks[@]}")
# Feed the commands with small gaps, keep the link open 12 s for replies.
{
	for l in "${lines[@]}"; do printf '%s\n' "$l"; sleep 0.3; done
	sleep 12
} | "$PONGO_TERM" > "$OUT" 2>&1 &
pid=$!
waited=0
while kill -0 "$pid" 2>/dev/null && [ "$waited" -lt 30 ]; do
	sleep 1; waited=$((waited + 1))
done
kill "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true
echo "log: $OUT"
