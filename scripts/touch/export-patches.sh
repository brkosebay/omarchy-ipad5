#!/usr/bin/env bash
# Offline only. Export the clean a9/touch branch and verify the complete series.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
mkdir -p "$ROOT/patches/touch"
exec "$ROOT/scripts/build-kernel.sh" --run touch '
set -euo pipefail
test "$(git branch --show-current)" = a9/touch
test -z "$(git status --porcelain)" || { echo "touch tree is dirty" >&2; exit 1; }
base=$(git rev-parse 6831bc701a)
git merge-base --is-ancestor "$base" HEAD
for patch in /work/patches/touch/*.patch; do
    test ! -f "$patch" || rm -- "$patch"
done
git format-patch --full-index --no-signature -o /work/patches/touch "$base"..HEAD
printf "%s\n" "$base" >/work/patches/touch/BASE
git log -1 --format="%H %s" >/work/patches/touch/HEAD
find /work/patches/touch -maxdepth 1 -name "*.patch" -printf "%f\n" | sort >/work/patches/touch/series
# Verify sequential applicability in a private temporary index; preserve the
# real checkout/index and compare the exported result with the source tree.
export GIT_INDEX_FILE="$PWD/.touch-export-index"
trap '\''rm -f "$GIT_INDEX_FILE"'\'' EXIT
git read-tree "$base"
while IFS= read -r patch; do
    git apply --cached "/work/patches/touch/$patch"
done </work/patches/touch/series
test "$(git write-tree)" = "$(git rev-parse HEAD^{tree})"
echo "PASS: exported series applies to BASE and exactly reconstructs HEAD"
'
