#!/usr/bin/env bash
# Export committed a9/display patches, without a workspace commit or push.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
mkdir -p "$ROOT/patches/display/round1"
"$ROOT/scripts/build-kernel.sh" --run display '
set -e
test "$(git branch --show-current)" = a9/display
git diff --quiet
git diff --cached --quiet
test "$(git merge-base hoolock HEAD)" = "$(git rev-parse hoolock)"
out=/work/patches/display/round1
test -z "$(find "$out" -name "*.patch" -print -quit)"
git format-patch hoolock..HEAD -o "$out"
for patch in "$out"/*.patch; do
    scripts/checkpatch.pl --no-tree --strict --ignore FILE_PATH_CHANGES "$patch"
done
index=$(mktemp)
rm "$index"
trap '\''rm -f "$index"'\'' EXIT
export GIT_INDEX_FILE="$index"
git read-tree hoolock
for patch in "$out"/*.patch; do
    git apply --cached "$patch"
done
test "$(git write-tree)" = "$(git rev-parse HEAD^{tree})"
git log --oneline hoolock..HEAD
'
(
    cd "$ROOT/patches/display/round1"
    shasum -a 256 ./*.patch > SHA256SUMS
    for patch in ./*.patch; do basename "$patch"; done > series
)
