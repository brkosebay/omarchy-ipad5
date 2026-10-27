#!/usr/bin/env bash
# Flip the polarity of the Smart Cover hall(s) in a built dtb, without a kernel
# rebuild: the third cell of the `gpios` property of the gpio-keys nodes in
# s8003-j71t-periph.dtsi (GPIO_ACTIVE_LOW = 1, GPIO_ACTIVE_HIGH = 0).
#
#   scripts/sensors/cover-polarity.sh IN.dtb active-high|active-low [lid|aux|both] [OUT.dtb]
#
#   lid   PMIC GPIO 0, SW_LID           (default)
#   aux   PMIC GPIO 10, SW_MACHINE_COVER
#   both
#
# SW_LID must read 1 with the cover closed. If it reads 1 with the cover OPEN
# (ipad-lid --watch), the guess in the dtsi was wrong: use active-high. Make the
# change permanent in the dtsi (J71T_COVER0_ACTIVE / J71T_COVER1_ACTIVE).
# Needs docker (the dtc of the kernel build image); prints the property before
# and after.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
IN="${1:?usage: cover-polarity.sh IN.dtb active-high|active-low [lid|aux|both] [OUT.dtb]}"
WANT="${2:?active-high or active-low}"
WHICH="${3:-lid}"
OUT="${4:-${IN%.dtb}-cover-$WANT.dtb}"
case "$WANT" in
active-low) FLAG=1 ;;
active-high) FLAG=0 ;;
*) echo "polarity: active-low or active-high" >&2; exit 2 ;;
esac
case "$WHICH" in
lid) NODES="/smart-cover/lid-switch" ;;
aux) NODES="/smart-cover-aux/cover-switch" ;;
both) NODES="/smart-cover/lid-switch /smart-cover-aux/cover-switch" ;;
*) echo "which: lid, aux or both" >&2; exit 2 ;;
esac
cp "$IN" "$OUT"
docker run --rm --platform linux/arm64 -v "$(cd "$(dirname "$OUT")" && pwd):/d" linux-a9-kbuild bash -c '
set -e
f=/d/'"$(basename "$OUT")"'
for n in '"$NODES"'; do
	before=$(fdtget -t x "$f" "$n" gpios)
	set -- $before
	fdtput -t x "$f" "$n" gpios "$1" "$2" '"$FLAG"'
	echo "$n gpios: <$before> -> <$(fdtget -t x "$f" "$n" gpios)>"
done'
echo "wrote $OUT"
