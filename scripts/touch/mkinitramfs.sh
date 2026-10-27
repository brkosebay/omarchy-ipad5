#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
python3 -B "$ROOT/scripts/touch/stage-firmware.py"
exec python3 -B "$ROOT/scripts/touch/mkinitramfs.py" "$@"
