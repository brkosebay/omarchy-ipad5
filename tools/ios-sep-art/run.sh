#!/usr/bin/env bash
# Read the 20-byte SEP ART from a jailbroken iPad and pull it to
# build/sep/art/art.bin. ORCHESTRATOR ONLY. It calls only GetART: no
# Set/Clear/Purge method and no NVRAM writer. The kernel's GetART failure
# path can re-submit the volatile ART page to the running SEP.
#
# Usage:
#   tools/ios-sep-art/run.sh
#
# The binary is installed under /var/mobile/linux-a9/ios-sep-art and run as
# mobile first. If IOServiceOpen needs root, export IPAD_PW and the runner
# falls back to sudo -S (the entitlement, not root, is the kext's check).
#
# Nothing under build/ is committed.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/build/sep/art"
LOG="$OUT/run.log"
BIN="$ROOT/build/ios-sep-art/ios-sep-art"
REMOTE=/var/mobile/linux-a9/ios-sep-art

mkdir -p "$OUT"

sh "$ROOT/tools/ios-sep-art/build.sh"

ios() { "$ROOT/scripts/ipad-ssh.sh" "$@"; }
scp_to() { IPAD_SCP=1 "$ROOT/scripts/ipad-ssh.sh" "$1" "mobile@127.0.0.1:$2"; }
scp_from() { IPAD_SCP=1 "$ROOT/scripts/ipad-ssh.sh" "mobile@127.0.0.1:$1" "$2"; }

ios "mkdir -p $REMOTE" || {
    echo "STOP: no iOS SSH (is the iPad booted and iproxy available?)" >&2
    exit 1
}
scp_to "$BIN" "$REMOTE/"
ios "chmod +x $REMOTE/ios-sep-art"

set +e
ios "$REMOTE/ios-sep-art --probe" >"$LOG" 2>&1
rc=$?
if [ "$rc" -eq 0 ]; then
    ios "$REMOTE/ios-sep-art $REMOTE/art.bin" >>"$LOG" 2>&1
    rc=$?
fi
if [ "$rc" -ne 0 ] && [ -n "${IPAD_PW:-}" ]; then
	mobile_log="$OUT/mobile.log"
	mv "$LOG" "$mobile_log"
    {
        echo "--- mobile run failed (rc=$rc), retrying with sudo ---"
        cat "$mobile_log"
    } >"$LOG"
    printf '%s\n' "$IPAD_PW" |
        ios "sudo -S -p '' $REMOTE/ios-sep-art --probe" >>"$LOG" 2>&1
    rc=$?
    if [ "$rc" -eq 0 ]; then
        printf '%s\n' "$IPAD_PW" |
            ios "sudo -S -p '' $REMOTE/ios-sep-art $REMOTE/art.bin" >>"$LOG" 2>&1
        rc=$?
    fi
fi
set -e

cat "$LOG"
if [ "$rc" -ne 0 ]; then
    echo "STOP: GetART failed (rc=$rc); see $LOG" >&2
    exit 1
fi

scp_from "$REMOTE/art.bin" "$OUT/art.bin"
chmod 600 "$OUT/art.bin"

echo
echo "[*] pulled $OUT/art.bin"
wc -c <"$OUT/art.bin" | tr -d ' ' | xargs -I{} echo "    len={} bytes"
shasum -a 256 "$OUT/art.bin" | sed 's/^/    sha256=/'

echo
echo "pongoOS (fresh DFU, host-side file):"
echo "    scripts/sep/probe-sep.sh pongo"
echo "    scripts/sep/pongo-sep.sh --xnu art status $OUT/art.bin"
echo
echo "or in an interactive pongoOS shell:"
echo "    /send $OUT/art.bin"
echo "    sep art"
echo "    sep xnu art status"
echo "    sep stat"
