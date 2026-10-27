#!/usr/bin/env bash
# Explicitly run by the orchestrator. All remote operations are reads.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
exec python3 -B "$ROOT/scripts/periph/pull_firmware.py" "$@"
