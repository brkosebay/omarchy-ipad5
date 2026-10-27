#!/usr/bin/env bash
# Reproduce the offline Round 6 integration without commits or branch changes.
# Existing unrelated checkouts/diffs are refused, never reset or cleaned.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CHECK_TREE="${CHECK_TREE:-bootr6-check}"
[[ "$CHECK_TREE" =~ ^[a-z0-9][a-z0-9_-]*$ ]] || { echo 'invalid CHECK_TREE' >&2; exit 1; }
PREPARE_ONLY=0
case "${1:-}" in --prepare-only) PREPARE_ONLY=1 ;; '') ;; *) echo 'usage: build-round6-check.sh [--prepare-only]' >&2; exit 2 ;; esac
mkdir -p "$ROOT/build/boot/round6"
PREP_CMD=$(cat <<'CONTAINER'
set -euo pipefail
base=359fe60627c88163f793487ba8bb6b9bea823b6e
boot_ref=b48c325d1a1dc8c10f821a6b8dff9811cb1f6d40
log=/work/build/boot/round6
merged=$(git merge-tree --write-tree "$boot_ref" "$base")
git diff --binary "$base" "$merged" >"$log/integration.patch"
printf '%s\n' "$merged" >"$log/integration-tree.txt"
path="/src/wt/$CHECK_TREE"
if [ ! -e "$path" ]; then
    git worktree add --detach "$path" "$base"
    git -C "$path" apply "$log/integration.patch"
fi
cd "$path"
test "$(git rev-parse HEAD)" = "$base"
# A temporary expected index compares every source file, including the new
# watchdog, without staging anything into the real checkout's index.
expected_index=$(mktemp)
rm -f "$expected_index"
trap 'rm -f "$expected_index"' EXIT
GIT_INDEX_FILE="$expected_index" git read-tree "$merged"
GIT_INDEX_FILE="$expected_index" git update-index --refresh
GIT_INDEX_FILE="$expected_index" git diff-files --quiet || {
    echo 'check tree differs from the pinned merged source; refusing' >&2; exit 1;
}
test -z "$(GIT_INDEX_FILE="$expected_index" git ls-files --others --exclude-standard)"
# Do not use a newer integration config silently.
printf '%s  %s\n' 11ca4951fa6a89e5ff317a3e11fa09354c4afc69a1ff9724697e3fa196a64138 \
    /work/build/kernel-bootr5b/config | sha256sum -c -
cp /work/build/kernel-bootr5b/config .config
scripts/config -e APPLE_J71T_BOOT_WATCHDOG
make LLVM=1 ARCH=arm64 olddefconfig >/dev/null
echo "verified detached source $base + $merged; no integration commit"
CONTAINER
)
"$ROOT/scripts/build-kernel.sh" --run boot "export CHECK_TREE='$CHECK_TREE'
$PREP_CMD"
[ "$PREPARE_ONLY" = 1 ] || "$ROOT/scripts/build-kernel.sh" --tree "$CHECK_TREE"
