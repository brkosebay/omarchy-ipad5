#!/usr/bin/env bash
# Host only. One incremental integration Image/DTB build, seeded from round 5.
# No device operations and no commits (commit the installed delta on a9/power first).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/build/power/oc-r6"
mkdir -p "$OUT"
[[ $# = 0 ]] || { echo 'usage: build-round6.sh' >&2; exit 2; }
python3 -B "$ROOT/scripts/power/validate-round6.py" > "$OUT/validation.log" 2>&1
tail -5 "$OUT/validation.log"
"$ROOT/scripts/build-kernel.sh" --run power '
test "$(git branch --show-current)" = a9/power
test -z "$(git status --porcelain)"
cmp /work/scripts/power/kernel/apple-a9-tuning.h drivers/cpufreq/apple-a9-tuning.h
base=f53bf861ca
old=49c0339866
tree=/src/wt/power-r6-integ
cache=/src/wt/power-r5-integ
git merge-base --is-ancestor 20ddcb7f1c HEAD
git diff "$old" HEAD --binary > /work/build/power/oc-r6/kernel.patch
if [ ! -d "$tree" ]; then
    git worktree add --detach "$tree" "$base"
    # Copy only ignored build products, never tracked source or the .git link.
    # This preserves round 5 and avoids another full integration compilation.
    git -C "$cache" ls-files --others --ignored --exclude-standard -z |
        rsync -a --from0 --files-from=- "$cache/" "$tree/"
fi
test "$(git -C "$tree" rev-parse HEAD)" = "$(git rev-parse "$base")"
test -z "$(git -C "$tree" branch --show-current)"
if [ -z "$(git -C "$tree" status --porcelain)" ]; then
    git -C "$tree" apply --index /work/build/power/oc-r6/kernel.patch
else
    git -C "$tree" diff --cached --binary > /work/build/power/oc-r6/applied.patch
    cmp /work/build/power/oc-r6/kernel.patch /work/build/power/oc-r6/applied.patch
    git -C "$tree" diff --exit-code
fi
git rev-parse HEAD > /work/build/power/oc-r6/power-head.txt
git merge-tree --write-tree "$base" HEAD > /work/build/power/oc-r6/merge-tree.txt
test "$(git -C "$tree" write-tree)" = "$(cat /work/build/power/oc-r6/merge-tree.txt)"
git -C "$tree" diff --cached --check
git diff 20ddcb7f1c HEAD > /work/build/power/oc-r6/round6.patch
scripts/checkpatch.pl --no-tree --strict --show-types --no-signoff \
    /work/build/power/oc-r6/round6.patch > /work/build/power/oc-r6/checkpatch.log
# Reuse the exact known-good round-5 integration configuration.
cp /work/build/kernel-power-r5-integ/config /work/build/power/oc-r6/integration-input.config
cp /work/build/power/oc-r6/integration-input.config "$tree/.config"
cd "$tree"
scripts/config -d DEBUG_INFO -d DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT -e DEBUG_INFO_NONE -d THERMAL_EMULATION
make LLVM=1 ARCH=arm64 olddefconfig >/dev/null
make LLVM=1 ARCH=arm64 syncconfig >/dev/null
python3 -B /work/scripts/power/seed-round6-cache.py
make -j4 LLVM=1 ARCH=arm64 Image.gz dtbs
out=/work/build/kernel-power-r6-integ
mkdir -p "$out/dtbs"
cp arch/arm64/boot/Image.gz "$out/"
cp arch/arm64/boot/dts/apple/*.dtb "$out/dtbs/"
cp .config "$out/config"
git log -1 --format="%H %s" > "$out/COMMIT"
git status --short > "$out/DIRTY"
echo "done: $(ls "$out/dtbs" | wc -l) dtbs"
' 2>&1 | tee "$OUT/integrated-build.log"
python3 -B "$ROOT/scripts/power/check-round6-build.py" > "$ROOT/docs/power/evidence/overclock-round6-build.json"
