#!/usr/bin/env bash
# Export the committed round-4 a9/display patch (on top of round 3),
# without a workspace commit or push.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
mkdir -p "$ROOT/patches/display/round4"
"$ROOT/scripts/build-kernel.sh" --run display '
set -e
base=1b83789aa87469552a06d46e70b48e9dccf2b71e
test "$(git branch --show-current)" = a9/display
git diff --quiet
git diff --cached --quiet
git merge-base --is-ancestor "$base" HEAD
out=/work/patches/display/round4
test -z "$(find "$out" -name "*.patch" -print -quit)"
git format-patch -M "$base"..HEAD -o "$out"
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
git read-tree "$base"
for patch in "$out"/*.patch; do
    git apply --cached "$patch"
done
test "$(git write-tree)" = "$(git rev-parse HEAD^{tree})"
git log --oneline "$base"..HEAD
'
(
    cd "$ROOT/patches/display/round4"
    shasum -a 256 ./*.patch > SHA256SUMS
    for patch in ./*.patch; do basename "$patch"; done > series
)
