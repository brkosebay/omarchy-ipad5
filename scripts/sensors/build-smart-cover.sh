#!/usr/bin/env bash
# Offline only: Smart Cover wake round 1 (docs/power/smart-cover-test-plan.md).
#
#   scripts/sensors/build-smart-cover.sh owner   checks on a9/sensors, patch export
#   scripts/sensors/build-smart-cover.sh merge   detached test merge into a9/nextint
#   scripts/sensors/build-smart-cover.sh build   Image.gz + DTBs of that merge
#   scripts/sensors/build-smart-cover.sh check   checks on the merge and its build
#   scripts/sensors/build-smart-cover.sh all     the four in order
#
# Everything runs in the sensors worktree (/src/wt/sensors). `merge` leaves it
# detached at a9/nextint with a9/sensors as an uncommitted, unstaged overlay
# (new files intent-to-add), so build-kernel.sh records it as SOURCE.patch;
# `owner` switches back to a9/sensors. Logs: build/power/smart-cover-r1/.
# Kernel: build/kernel-sensors/.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/build/power/smart-cover-r1"
BASE=df595c3247   # a9/sensors before this round
mkdir -p "$OUT"

# Shared by `owner` and `check`: compiler, schema, DTB and mock checks.
CHECKS='
O=/work/build/power/smart-cover-r1/$PHASE
mkdir -p "$O"
export PYTHONPATH=/work/build/periph/schema-lib
export PATH=/work/build/periph/schema-tools:/work/build/periph/schema-lib/bin:$PATH
make -s LLVM=1 ARCH=arm64 olddefconfig
make LLVM=1 ARCH=arm64 W=1 -j"${KERNEL_JOBS:-3}" drivers/gpio/gpio-apple-d2365.o \
  apple/s8003-j71t.dtb apple/s8003-j71t-smart-cover.dtb > "$O/w1.log" 2>&1
if grep -iE "(warning|error)( \(|:)" "$O/w1.log" | grep -v simple_bus_reg; then
  echo "FAIL new W=1 diagnostics"; exit 1
fi
./scripts/checkpatch.pl --strict --no-tree -f drivers/gpio/gpio-apple-d2365.c > "$O/checkpatch-file.log"
make -s LLVM=1 ARCH=arm64 dt_binding_check \
  DT_SCHEMA_FILES="gpio/apple,d2365-pmic-gpio.yaml:mfd/apple,pmic.yaml:input/gpio-keys.yaml" \
  > "$O/bindings.log" 2>&1
S=Documentation/devicetree/bindings/processed-schema.json
D=arch/arm64/boot/dts/apple
for d in s8003-j71t s8003-j71t-smart-cover; do
  { dt-validate -s $S $D/$d.dtb 2>&1 || true; } | { grep -v "from schema" || true; } |
    sed "s/.*\.dtb: //" | sort > "$O/validate-$d.log"
done
comm -13 "$O/validate-s8003-j71t.log" "$O/validate-s8003-j71t-smart-cover.log" > "$O/validate-new.log"
if [ -s "$O/validate-new.log" ]; then cat "$O/validate-new.log"; echo "FAIL new dt-validate errors"; exit 1; fi
python3 -B /work/scripts/sensors/check-smart-cover-dtb.py $D > "$O/dtb-properties.log"
python3 -B /work/scripts/sensors/test-smart-cover.py drivers/gpio/gpio-apple-d2365.c > "$O/mock-tests.log"
cat "$O/dtb-properties.log" "$O/mock-tests.log"
echo "PASS $PHASE checks: W=1, checkpatch, dt_binding_check, no new dt-validate errors"
'

owner() {
	"$ROOT/scripts/build-kernel.sh" --run sensors '
	O=/work/build/power/smart-cover-r1
	# Only the overlay that `merge` left behind may be discarded.
	if [ -z "$(git branch --show-current)" ] && [ -f $O/integration-base ] &&
	   [ "$(git rev-parse HEAD)" = "$(cat $O/integration-base)" ] &&
	   git diff --binary | cmp -s - $O/integration.patch; then
	  git reset -q --hard
	  git switch -q -f a9/sensors
	fi
	test "$(git branch --show-current)" = a9/sensors
	test -z "$(git status --porcelain --untracked-files=no)"
	rm -rf /work/patches/sensors/smart-cover-r1
	git format-patch -q -o /work/patches/sensors/smart-cover-r1 '"$BASE"'..HEAD
	for p in /work/patches/sensors/smart-cover-r1/*.patch; do
	  echo "== ${p##*/}"
	  ./scripts/checkpatch.pl --strict "$p" | grep -E "^(WARNING|ERROR|CHECK)|^total" || true
	done > $O/checkpatch-series.log
	cat $O/checkpatch-series.log
	PHASE=owner; '"$CHECKS"
}

merge() {
	"$ROOT/scripts/build-kernel.sh" --run sensors '
	O=/work/build/power/smart-cover-r1
	test "$(git branch --show-current)" = a9/sensors
	test -z "$(git status --porcelain --untracked-files=no)"
	git rev-parse a9/sensors > $O/owner-tip
	git rev-parse a9/nextint > $O/integration-base
	git switch -q --detach a9/nextint
	git merge -q --no-commit --no-ff a9/sensors
	# a9/nextint itself carries the PMIC interrupt block between two "gpio:"
	# keys (its periph x pcie merge); a9/sensors has the block once, on the
	# periph context. Its copy is the correct one and also fixes nextint.
	Y=Documentation/devicetree/bindings/mfd/apple,pmic.yaml
	test "$(git diff a9/sensors a9/nextint -- $Y | grep -c "^[-+] ")" = 2
	git checkout a9/sensors -- $Y
	test "$(grep -c "^  gpio:" $Y)" = 1
	test "$(grep -c "^  interrupts:" $Y)" = 1
	git diff --cached --check
	new=$(git diff --cached --name-only --diff-filter=A)
	git reset -q
	git add -N $new
	git diff --binary > $O/integration.patch
	cp /src/wt/nextint/.config .config
	scripts/config -e KEYBOARD_GPIO -e KEYBOARD_GPIO_POLLED -e GPIO_APPLE_D2365
	make -s LLVM=1 ARCH=arm64 olddefconfig
	echo "merged a9/sensors $(cut -c1-10 $O/owner-tip) into a9/nextint $(cut -c1-10 $O/integration-base)"'
}

build() {
	KERNEL_JOBS="${KERNEL_JOBS:-3}" KERNEL_LOCALVERSION=-a9-smart-cover \
		"$ROOT/scripts/build-kernel.sh" --tree sensors > "$OUT/integration-build.log" 2>&1
	tail -1 "$OUT/integration-build.log"
}

check() {
	"$ROOT/scripts/build-kernel.sh" --run sensors '
	O=/work/build/power/smart-cover-r1
	test "$(git rev-parse HEAD)" = "$(cat $O/integration-base)"
	git diff --binary | cmp - $O/integration.patch
	PHASE=integration; '"$CHECKS"
	K="$ROOT/build/kernel-sensors"
	grep -q -- '-a9-smart-cover' "$K/RELEASE"
	cmp "$K/SOURCE.patch" "$OUT/integration.patch"
	test -s "$K/dtbs/s8003-j71t-smart-cover.dtb"
	grep -qx 'CONFIG_GPIO_APPLE_D2365=y' "$K/config"
	grep -qx 'CONFIG_KEYBOARD_GPIO=y' "$K/config"
	echo "PASS integration kernel $(cat "$K/RELEASE")"
}

case "${1:-all}" in
owner) owner ;;
merge) merge ;;
build) build ;;
check) check ;;
all) owner; merge; build; check ;;
*) echo "usage: $0 [owner|merge|build|check|all]" >&2; exit 2 ;;
esac
