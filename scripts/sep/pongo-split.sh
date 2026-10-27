#!/usr/bin/env bash
# pongoOS half of the SEP nap-wake split (docs/sep/xart-handshake.md section 7,
# docs/sep/linux-live-test.md section 8). Run once, in the same pongoOS session,
# after `scripts/sep/pongo-sep.sh --xnu status` booted sepOS and finished its
# discovery capture, and before bootm:
#
#   1. sep stat, then peek the SEP PMGR word and SEP +0x28 +0x44 +0xb88
#      +0x4008 +0x4020
#   2. sep msg 0 0 e 0 0      one raw 0x00000000000e0000 (ep 0 op 0x0e), the
#                             only write; pongoOS logs every SEP->AP reply
#   3. wait 1 s on the host, peek the same six registers again, sep stat
#      (ring entries beyond the first stat's total are this window's replies)
#
# Needs the patched pongoOS (build/prebuilt/pongo/Pongo-a9sep.bin). The output
# is appended to build/sep/pongo-sep.log and, if given, written to OUT.
# Never retried automatically: once "AP->SEP ep=0 tag=0 op=e" was printed, the
# word is in the FIFO and a second run would queue another one.
#
# Usage: scripts/sep/pongo-split.sh [OUT]
# Exit: 0 both peek sets captured (verdict on the RESULT line)
#       1 pongoterm never reached pongoOS, nothing sent: may be re-run once
#       3 the message may have been sent but the output is incomplete: do not
#         re-run; cold-reset after reading the log
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PONGOTERM="$ROOT/build/bin/pongoterm"
LOG="$ROOT/build/sep/pongo-sep.log"
OUT="${1:-}"
REGS="0x20e080400 0x20da00028 0x20da00044 0x20da00b88 0x20da04008 0x20da04020"

[ -x "$PONGOTERM" ] || { echo "pongo-split: no pongoterm at $PONGOTERM" >&2; exit 1; }
mkdir -p "$(dirname "$LOG")"

tmp=$(mktemp -d)
fifo="$tmp/in"
raw="$tmp/out"
mkfifo "$fifo"
"$PONGOTERM" <"$fifo" >"$raw" 2>&1 &
pid=$!
exec 9>"$fifo"
peeks() { for r in $REGS; do printf 'peek %s\n' "$r"; done; }
{ printf 'sep stat\n'; peeks; printf 'sep msg 0 0 e 0 0\n'; } >&9
sleep 1
{ peeks; printf 'sep stat\n'; } >&9
sleep 3
exec 9>&-
kill "$pid" 2>/dev/null
wait "$pid" 2>/dev/null
{ echo "== pongo-split $(date '+%F %T')"; cat "$raw"; } >>"$LOG"
[ -n "$OUT" ] && cp "$raw" "$OUT"

python3 -B -I - "$raw" "$REGS" <<'PY'
import re
import sys
from pathlib import Path

text = Path(sys.argv[1]).read_text(errors='replace')
regs = sys.argv[2].split()
names = {'0x20e080400': 'pmgr', '0x20da00028': 'sep28', '0x20da00044': 'sep44',
         '0x20da00b88': 'sepb88', '0x20da04008': 'snd4008', '0x20da04020': 'rcv4020'}
sent = 'AP->SEP ep=0 tag=0 op=e param=0 data=0' in text
peeks = re.findall(r'(0x2[0-9a-f]+): ([0-9a-f]+) \(', text)
if not sent and not peeks:
    print('pongo-split: no pongoOS output; nothing was sent (may re-run once)')
    sys.exit(1)
if not sent or len(peeks) != 2 * len(regs) or [a for a, _ in peeks] != regs + regs:
    print(f'pongo-split: incomplete output (sent={sent}, {len(peeks)} peeks); do not re-run')
    print(text)
    sys.exit(3)
before = {names[a]: int(v, 16) for a, v in peeks[:len(regs)]}
after = {names[a]: int(v, 16) for a, v in peeks[len(regs):]}
for label, d in (('before', before), ('after ', after)):
    print(f'pongo-split: {label} ' + ' '.join(f'{k}={v:08x}' for k, v in d.items()))
empty = lambda v: bool(v & 0x20000)
msg = r'ep=(\w+) tag=(\w+) op=(\w+) param=(\w+) data=(\w+)'
replies = re.findall(r'SEP->AP ' + msg, text)
# New ring entries: the second sep stat's ring beyond the first one's total.
stats = list(re.finditer(r'sep stat: recv_sts=\w+ snd_sts=\w+ total=(\d+)', text))
if len(stats) == 2:
    n = int(stats[1].group(1)) - int(stats[0].group(1))
    ring = re.findall(r'sep stat: ring ' + msg, text[stats[1].end():])
    fresh = ring[len(ring) - n:] if 0 < n <= len(ring) else []
    print(f'pongo-split: sep stat total {stats[0].group(1)} -> {stats[1].group(1)}')
else:
    fresh = []
    print(f'pongo-split: expected 2 sep stat headers, got {len(stats)}')
for r in replies:
    print('pongo-split: SEP->AP ep=%s tag=%s op=%s param=%s data=%s' % r)
for r in fresh:
    print('pongo-split: new ring ep=%s tag=%s op=%s param=%s data=%s' % r)
# Safe `sep stat` never samples FIFO data; replies appear only through the IRQ
# handler's live print or the software ring.
wake = [r for r in replies + fresh
        if int(r[0], 16) == 0 and int(r[2], 16) in (0x0f, 0x15)]
print(f'pongo-split: pmgr before={before["pmgr"]:08x} after={after["pmgr"]:08x}'
      f' ({"handoff 4f0000ff" if before["pmgr"] == after["pmgr"] == 0x4f0000ff else "NOT 4f0000ff throughout"})')
print(f'pongo-split: AP->SEP FIFO before={"empty" if empty(before["snd4008"]) else "NOT empty"}'
      f' after={"empty" if empty(after["snd4008"]) else "NOT empty"}'
      f'; SEP->AP after={"empty" if empty(after["rcv4020"]) else "NOT empty"}')
if not empty(before['snd4008']):
    verdict = 'fifo-busy-before'
elif not empty(after['snd4008']):
    verdict = 'stuck'
elif wake:
    verdict = 'wake'
else:
    verdict = 'drained-no-wake'
print(f'RESULT pongo-wake={verdict}')
PY
rc=$?
rm -f "$fifo" "$raw"
rmdir "$tmp"
exit "$rc"
