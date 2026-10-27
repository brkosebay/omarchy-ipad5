#!/usr/bin/env bash
# Actual production reset/map/timeout C with faulted MMIO and CQ fixtures.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
docker run --rm --platform linux/arm64 -v linux-a9-src:/src:ro -v "$ROOT:/work:ro" \
  linux-a9-kbuild python3 -B -I /work/scripts/boot/test-ans-io-offline.py "$@"
