#!/usr/bin/env bash
# Offline host build; no device access. Reuse the audio toolchain pattern.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/build/touch/device"
mkdir -p "$OUT"
docker run --rm --platform linux/arm64 \
    -v "$ROOT/scripts/touch:/src:ro" -v "$OUT:/out" alpine:3.22 sh -euc '
    apk add --no-cache gcc musl-dev linux-headers >/dev/null
    gcc -static -O2 -Wall -Wextra -Werror -o /out/n1-input-info /src/n1-input-info.c
    strip /out/n1-input-info
    if /out/n1-input-info /dev/null; then exit 1; fi
'
file "$OUT/n1-input-info"
