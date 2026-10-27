#!/usr/bin/env bash
# Export the committed a9/vdec round-1 series; no workspace commit or push.
# Verifies the working tree is clean, formats every commit since `hoolock`,
# checkpatches each one, and proves the series reproduces the trees exactly.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TREE="${TREE:-vdec}"
ROUND="${ROUND:-round1}"
OUT="patches/vdec/$ROUND"
# Round 3+ commits end with a "Co-Authored-By:" trailer (BAD_SIGN_OFF form).
IGNORE="${CHECKPATCH_IGNORE:-FILE_PATH_CHANGES}"

"$ROOT/scripts/build-kernel.sh" --run "$TREE" "
set -e
test \"\$(git branch --show-current)\" = a9/vdec
git diff --quiet
git diff --cached --quiet
base=\$(git merge-base hoolock HEAD)
test \"\$base\" = \"\$(git rev-parse hoolock)\"
git format-patch \"\$base\"..HEAD -o /work/$OUT
for p in /work/$OUT/*.patch; do
	scripts/checkpatch.pl --no-tree --strict --ignore $IGNORE \"\$p\" || exit 1
done
index=\$(mktemp); rm \"\$index\"
GIT_INDEX_FILE=\"\$index\" git read-tree HEAD
for p in \$(ls -r /work/$OUT/*.patch); do
	GIT_INDEX_FILE=\"\$index\" git apply --cached --reverse \"\$p\" || exit 1
done
test \"\$(GIT_INDEX_FILE=\"\$index\" git write-tree)\" = \"\$(git rev-parse hoolock^{tree})\"
rm \"\$index\"
git log --oneline \"\$base\"..HEAD
"
(
	cd "$ROOT/$OUT"
	shasum -a 256 ./*.patch > SHA256SUMS
	ls ./*.patch | sed -e 's|.*/||' > series
	echo "exported $(wc -l < series) patches to $OUT"
)
