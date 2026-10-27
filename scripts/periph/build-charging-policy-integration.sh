#!/usr/bin/env bash
# Exact boot overlay + USB + charging policy in a task-owned detached tree.
# --check-only validates replay/provenance without allocating another checkout.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CHECK_ONLY=0
if [ "${1:-}" = --check-only ]; then CHECK_ONLY=1; shift; fi
ROUND="${1:-round8e}"
case "$ROUND" in round8e|round8f) ;; *) echo "usage: $0 [--check-only] [round8e|round8f]" >&2; exit 2 ;; esac
[ $# -le 1 ] || exit 2
"$ROOT/scripts/build-kernel.sh" --run periph '
set -euo pipefail
python3 -B -I /work/scripts/periph/check-charging-policy-integration.py
'
[ "$CHECK_ONLY" = 0 ] || exit 0
"$ROOT/scripts/build-kernel.sh" --run periph '
set -euo pipefail
round='"$ROUND"'
# Do not silently combine an unbuilt boot revision with its older config/image.
cmp "/work/build/boot/$round/integration.patch" "/work/build/kernel-bootr${round#round}-check/SOURCE.patch"
base=$(git rev-parse d5822a7f71cd)
patch="/work/build/periph/charging-policy/integration-$round.patch"
if [ ! -d /src/wt/chargingpolicy-check ]; then
 git worktree add --detach /src/wt/chargingpolicy-check "$base"
fi
cd /src/wt/chargingpolicy-check
test "$(git rev-parse HEAD)" = "$base"
test -z "$(git branch --show-current)"
test -z "$(git ls-files --others --exclude-standard)"
if [ -n "$(git status --porcelain)" ]; then
 existing=$(mktemp)
 git diff --binary > "$existing"
 cmp "$existing" "$patch"
 rm "$existing"
else
 git apply --check "$patch"
 git apply --intent-to-add "$patch"
fi
cp "/work/build/kernel-bootr${round#round}-check/config" .config
make LLVM=1 ARCH=arm64 olddefconfig >/dev/null
cmp .config "/work/build/kernel-bootr${round#round}-check/config"
'
HEAD=$(cat "$ROOT/patches/periph/charging-policy/HEAD")
KERNEL_JOBS="${KERNEL_JOBS:-1}" KERNEL_LOCALVERSION="-a9-charging-policy-${HEAD:0:10}" \
 "$ROOT/scripts/build-kernel.sh" --tree chargingpolicy-check
cmp "$ROOT/build/periph/charging-policy/integration-$ROUND.patch" \
 "$ROOT/build/kernel-chargingpolicy-check/SOURCE.patch"
"$ROOT/scripts/build-kernel.sh" --run periph '
set -euo pipefail
round='"$ROUND"'
tmp=$(mktemp -d)
trap '\''rm -rf "$tmp"'\'' EXIT
pmic=/soc/i2c@20a110000/pmic@3c/charger@180
old="/work/build/kernel-bootr${round#round}-check/dtbs/s8003-j71t.dtb"
new=/work/build/kernel-chargingpolicy-check/dtbs/s8003-j71t.dtb
for property in apple,usb-input-limit-max-microamp apple,owner-input-limit-microamp; do
 test "$(fdtget -t u "$new" "$pmic" "$property")" = 2400000
done
cp "$new" "$tmp/new.dtb"
for property in apple,usb-input-limit-max-microamp apple,owner-input-limit-microamp; do
 fdtput -d "$tmp/new.dtb" "$pmic" "$property"
done
dtc -I dtb -O dts -s -o "$tmp/old.dts" "$old" 2>/dev/null
dtc -I dtb -O dts -s -o "$tmp/new.dts" "$tmp/new.dtb" 2>/dev/null
cmp "$tmp/old.dts" "$tmp/new.dts"
echo "PASS: exact boot overlay/config; only two J71t charging DT properties changed"
'
