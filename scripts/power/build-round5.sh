#!/usr/bin/env bash
# Host-only checks; optional detached integration build. No device operations.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/build/power/oc-r5"
mkdir -p "$OUT"
[[ $# = 0 || ( $# = 1 && $1 = --full ) ]] || { echo 'usage: build-round5.sh [--full]' >&2; exit 2; }
python3 -B "$ROOT/scripts/power/validate-round5.py" 2>&1 | tee "$OUT/validation.log"
"$ROOT/scripts/build-kernel.sh" --run power '
test "$(git branch --show-current)" = a9/power
python3 -B /work/scripts/power/install-round5.py
for pair in "apple-a9-tuning.h drivers/cpufreq/apple-a9-tuning.h" \
    "apple-s8003-thermal.c drivers/thermal/apple-s8003-thermal.c" \
    "s8003-power.h include/linux/soc/apple/s8003-power.h"; do
    read -r src dst <<< "$pair"
    cmp "/work/scripts/power/kernel/$src" "$dst"
done
scripts/kconfig/merge_config.sh -m .config /work/patches/power/power.config >/dev/null
scripts/config -d DEBUG_INFO -d DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT -e DEBUG_INFO_NONE -d THERMAL_EMULATION
make LLVM=1 ARCH=arm64 olddefconfig >/dev/null
make -j4 LLVM=1 ARCH=arm64 drivers/cpufreq/apple-soc-cpufreq.o drivers/thermal/apple-s8003-thermal.o dtbs
scripts/config -d APPLE_A9_TUNING
make LLVM=1 ARCH=arm64 olddefconfig >/dev/null
make -j4 LLVM=1 ARCH=arm64 drivers/cpufreq/apple-soc-cpufreq.o drivers/thermal/apple-s8003-thermal.o
scripts/config -e APPLE_A9_TUNING
make LLVM=1 ARCH=arm64 olddefconfig >/dev/null
make -j4 LLVM=1 ARCH=arm64 drivers/cpufreq/apple-soc-cpufreq.o drivers/thermal/apple-s8003-thermal.o
git diff --check
# Also works before committing the power-owned delta.
git diff ce7e58fd4f -- drivers/cpufreq/apple-a9-tuning.h \
    drivers/thermal/apple-s8003-thermal.c include/linux/soc/apple/s8003-power.h \
    arch/arm64/boot/dts/apple/s8003.dtsi \
    > /work/build/power/oc-r5/round5.patch
scripts/checkpatch.pl --no-tree --strict --show-types --no-signoff \
    /work/build/power/oc-r5/round5.patch > /work/build/power/oc-r5/checkpatch.log || true
! grep -q "^ERROR:" /work/build/power/oc-r5/checkpatch.log
' 2>&1 | tee "$OUT/object-build.log"
[[ ${1:-} = --full ]] || exit 0
"$ROOT/scripts/build-kernel.sh" --run power '
test -z "$(git status --porcelain)"
base=f53bf861ca
old=49c0339866
tree=/src/wt/power-r5-integ
git merge-base --is-ancestor "$old" HEAD
git merge-base --is-ancestor "$old" "$base"
git diff "$old" HEAD --binary > /work/build/power/oc-r5/kernel.patch
test -s /work/build/power/oc-r5/kernel.patch
if [ ! -d "$tree" ]; then
    git worktree add --detach "$tree" "$base"
fi
test "$(git -C "$tree" rev-parse HEAD)" = "$(git rev-parse "$base")"
test -z "$(git -C "$tree" branch --show-current)"
if [ -z "$(git -C "$tree" status --porcelain)" ]; then
    git -C "$tree" apply --index /work/build/power/oc-r5/kernel.patch
else
    git -C "$tree" diff --cached --binary > /work/build/power/oc-r5/applied.patch
    git -C "$tree" diff --exit-code
    if ! cmp -s /work/build/power/oc-r5/kernel.patch /work/build/power/oc-r5/applied.patch; then
        # Refresh only our previous staged power delta in this detached build.
        ! git -C "$tree" diff --cached --name-only | grep -Ev \
            "^(drivers/cpufreq/apple-a9-tuning.h|drivers/thermal/apple-s8003-thermal.c|include/linux/soc/apple/s8003-power.h|arch/arm64/boot/dts/apple/s8003.dtsi)$"
        git -C "$tree" apply -R --index /work/build/power/oc-r5/applied.patch
        git -C "$tree" apply --index /work/build/power/oc-r5/kernel.patch
    fi
fi
git rev-parse HEAD > /work/build/power/oc-r5/power-head.txt
git merge-tree --write-tree "$base" HEAD > /work/build/power/oc-r5/merge-tree.txt
test "$(git -C "$tree" write-tree)" = "$(cat /work/build/power/oc-r5/merge-tree.txt)"
git -C "$tree" diff --cached --check
# Use the integration configuration, without the unrelated uncommitted boot
# fragments in /work. Preserve its exact input for artifact provenance.
cp /src/wt/integ/.config /work/build/power/oc-r5/integration-input.config
cp /work/build/power/oc-r5/integration-input.config "$tree/.config"
cd "$tree"
scripts/kconfig/merge_config.sh -m .config /work/patches/power/power.config >/dev/null
scripts/config -d DEBUG_INFO -d DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT -e DEBUG_INFO_NONE -d THERMAL_EMULATION
make LLVM=1 ARCH=arm64 olddefconfig >/dev/null
' 2>&1 | tee "$OUT/integration-prepare.log"
"$ROOT/scripts/build-kernel.sh" --tree power-r5-integ 2>&1 | tee "$OUT/integrated-build.log"
python3 -B "$ROOT/scripts/power/check-round5-build.py" > "$ROOT/docs/power/evidence/overclock-round5-build.json"
