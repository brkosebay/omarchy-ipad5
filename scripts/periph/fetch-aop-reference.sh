#!/usr/bin/env bash
# Host-only partial IPSW extraction. Does not contact or write the iPad.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/build/periph/aop-r1/ipsw"
mkdir -p "$OUT"
ipsw download ipsw -d iPad6,11 -b 20H350 -y -f -o "$OUT" \
    --pattern '(aopfw[^/]*\.im4p|BuildManifest\.plist)$'
