#!/usr/bin/env bash
# Offline: preserve HoolockRD cpio, append firmware/tools, emit ONE gzip stream.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
exec python3 -B "$ROOT/scripts/periph/mkinitramfs.py" "$@"
