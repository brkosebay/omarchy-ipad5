#!/usr/bin/env bash
# Host-only public firmware references. Never contacts the iPad or flashes them.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/build/power/oc-r3/bootrefs"
mkdir -p "$OUT"
ipsw download ipsw -d iPad6,11 -b 20H350 -y -f --decrypt -o "$OUT" \
    --pattern '(iBoot|LLB)\.j71t\.RELEASE\.im4p$'
python3 -B "$ROOT/scripts/power/round3-evidence.py"
