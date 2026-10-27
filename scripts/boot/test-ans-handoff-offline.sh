#!/usr/bin/env bash
# Round 8 production C and rescue fixtures, never device access.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
docker run --rm --platform linux/arm64 -v linux-a9-src:/src:ro -v "$ROOT:/work:ro" \
  linux-a9-kbuild python3 -B -I /work/scripts/boot/test-ans-handoff-offline.py "$@"
