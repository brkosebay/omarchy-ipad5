#!/usr/bin/env bash
# Export the committed round-2 a9/display patches (on top of round 1),
# without a workspace commit or push.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
mkdir -p "$ROOT/patches/display/round2"
"$ROOT/scripts/build-kernel.sh" --run display '
set -e
round1=95149d28070371295295cf3fd2da0739c479a41a
test "$(git branch --show-current)" = a9/display
git diff --quiet
git diff --cached --quiet
git merge-base --is-ancestor "$round1" HEAD
out=/work/patches/display/round2
test -z "$(find "$out" -name "*.patch" -print -quit)"
git format-patch -M "$round1"..HEAD -o "$out"
for patch in "$out"/*.patch; do
    # The workspace attribution trailer is not a kernel signature; drop
    # only that line so every other signature stays checked.
    sed "/^Co-Authored-By: /d" "$patch" |
        scripts/checkpatch.pl --no-tree --strict --ignore FILE_PATH_CHANGES -
done
index=$(mktemp)
rm "$index"
trap '\''rm -f "$index"'\'' EXIT
export GIT_INDEX_FILE="$index"
git read-tree "$round1"
for patch in "$out"/*.patch; do
    git apply --cached "$patch"
done
test "$(git write-tree)" = "$(git rev-parse HEAD^{tree})"
git log --oneline "$round1"..HEAD
'
(
    cd "$ROOT/patches/display/round2"
    shasum -a 256 ./*.patch > SHA256SUMS
    for patch in ./*.patch; do basename "$patch"; done > series
)
