#!/usr/bin/env bash
# Pull the personalized sep-firmware IMG4 off the iPad (Linux side).
#
# iOS keeps its boot firmware in the APFS container, not in a GPT partition,
# so there is no file to copy. The image sits at a stable offset per restore:
# on 20H350 / j71t that is 0x00e0ce0000, 3,414,298 bytes.
#
# Default is a single bounded read at that offset: no full-device scan, no
# page-cache pressure. Two earlier full scans coincided with the iPad dropping
# off USB, so --scan is opt-in and chunked.
#
# Usage:
#   scripts/sep/get-sep-img4.sh [out.img4]            # bounded read (fast)
#   scripts/sep/get-sep-img4.sh [out.img4] --scan     # re-locate after a restore
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
IP="${IPAD_IP:-172.16.42.1}"
OUT="$ROOT/build/sep/sep.j71t.img4"
SCAN=0

for arg in "$@"; do
	case "$arg" in
	--scan) SCAN=1 ;;
	*) OUT="$arg" ;;
	esac
done

KNOWN_OFF=0xe0ce0000
KNOWN_LEN=3414298

SSH=(ssh -p 22 -o BatchMode=yes -o ConnectTimeout=8 root@"$IP")
SCP=(scp -P 22 -q -o ConnectTimeout=8)

if [ "$SCAN" = 1 ]; then
	"${SCP[@]}" "$ROOT/scripts/sep/find-img4.py" root@"$IP":/root/
	"${SSH[@]}" "python3 /root/find-img4.py /dev/nvme0n1p1 --scan --extract sepi /root/sep.j71t.img4"
else
	"${SSH[@]}" "dd if=/dev/nvme0n1p1 of=/root/sep.j71t.img4 bs=4096 skip=$((KNOWN_OFF / 4096)) count=$((KNOWN_LEN / 4096 + 1)) 2>/dev/null"
fi

"${SCP[@]}" root@"$IP":/root/sep.j71t.img4 "$OUT"
python3 - "$OUT" <<'EOF'
import sys
d = open(sys.argv[1], "rb").read()
ok = d[0] == 0x30 and b"IMG4" in d[:16] and b"sepi" in d[:32]
print("%s: %d bytes, IMG4=%s, sepi=%s, IM4M=%s" % (
    sys.argv[1], len(d), b"IMG4" in d[:16], b"sepi" in d[:32], b"IM4M" in d))
sys.exit(0 if ok else 1)
EOF
