#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
mkdir -p "$ROOT/patches/periph" "$ROOT/build/periph"
exec "$ROOT/scripts/build-kernel.sh" --run periph '
test -z "$(git status --porcelain)"
git format-patch --no-signature --no-cover-letter 6831bc701..HEAD -o /work/patches/periph
python3 /work/scripts/periph/check-patches.py
git log --reverse --format="%h %s" 6831bc701..HEAD > /work/build/periph/kernel-commits.txt
git diff --stat 6831bc701..HEAD > /work/build/periph/kernel-diffstat.txt
git diff --check 6831bc701..HEAD
'
