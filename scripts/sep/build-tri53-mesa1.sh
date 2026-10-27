#!/usr/bin/env bash
# Build the first J71t Mesa hardware-validation kernel from a9/tri53-sep7.
#
# The result contains only the inert Mesa client, two allowlisted D2365
# enable-only regulators, and board-local SPI2 wiring without a sensor IRQ.
# It does not capture frames or send any SEP/SBIO command.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BASE=56c339590811a04a7e3eb94e33f75546692e1265

"$ROOT/scripts/build-kernel.sh" --run tri53-sep7 '
set -euo pipefail

BASE='"$BASE"'
test "$(git rev-parse HEAD)" = "$BASE"
test -z "$(git status --porcelain)"

git worktree prune
if [ ! -d /src/wt/tri53-mesa1 ]; then
	if git show-ref --verify --quiet refs/heads/a9/tri53-mesa1; then
		MESA_HEAD=$(git rev-parse a9/tri53-mesa1)
		if [ "$MESA_HEAD" != "$BASE" ]; then
			test "$(git rev-parse a9/tri53-mesa1^)" = "$BASE"
			test "$(git log -1 --format=%s a9/tri53-mesa1)" = \
				'arm64: apple: add bounded J71t Mesa status transport'
		fi
		git worktree add /src/wt/tri53-mesa1 a9/tri53-mesa1
	else
		git worktree add -b a9/tri53-mesa1 /src/wt/tri53-mesa1 "$BASE"
	fi
fi
'

"$ROOT/scripts/build-kernel.sh" --run tri53-mesa1 '
set -euo pipefail

if [ "$(git rev-parse HEAD)" = "$(git rev-parse a9/tri53-mesa1 2>/dev/null || true)" ] &&
   git log -1 --format=%s | grep -q "^arm64: apple: add bounded J71t Mesa status transport$"; then
	test -z "$(git status --porcelain)"
	exit 0
fi

test "$(git rev-parse a9/tri53-sep7)" = "'"$BASE"'"
test "$(git rev-parse HEAD)" = "'"$BASE"'"
test -z "$(git status --porcelain)"
test -f /work/build/kernel-tri53-sep7/config
cp /work/build/kernel-tri53-sep7/config .config

mkdir -p drivers/mesa
cp /work/drivers/mesa/mesa-a9.c drivers/mesa/mesa-a9.c
cp /work/drivers/mesa/Kconfig drivers/mesa/Kconfig
cp /work/drivers/mesa/Makefile drivers/mesa/Makefile
cp /work/drivers/mesa/apple-d2365-mesa.c drivers/regulator/apple-d2365-mesa.c
cp /work/drivers/mesa/s8003-j71t-mesa-status.dtsi \
   arch/arm64/boot/dts/apple/s8003-j71t-mesa-status.dtsi

python3 - <<"PY"
from pathlib import Path

def append_once(path, text):
    p = Path(path)
    current = p.read_text()
    if text not in current:
        p.write_text(current.rstrip() + "\n\n" + text)

append_once("drivers/Kconfig", "source \"drivers/mesa/Kconfig\"\n")
append_once("drivers/Makefile", "obj-y += mesa/\n")
append_once("drivers/regulator/Makefile",
            "obj-$(CONFIG_REGULATOR_APPLE_D2365_MESA) += apple-d2365-mesa.o\n")

kconfig = Path("drivers/regulator/Kconfig")
text = kconfig.read_text()
block = """config REGULATOR_APPLE_D2365_MESA
\tbool \"Apple D2365 Mesa enable-only supplies\"
\tdepends on OF && REGMAP
\thelp
\t  Enable-only D2365 supplies for the J71t Mesa sensor.  The driver
\t  accepts only registers 0x0312 and 0x0319, bit zero.

"""
if "config REGULATOR_APPLE_D2365_MESA\n" not in text:
    marker = "config REGULATOR_APPLE_D2365_TOUCH\n"
    if marker not in text:
        raise SystemExit("missing D2365 touch Kconfig anchor")
    kconfig.write_text(text.replace(marker, block + marker, 1))

board = Path("arch/arm64/boot/dts/apple/s8003-j71t.dts")
include = "#include \"s8003-j71t-mesa-status.dtsi\"\n"
text = board.read_text()
if include not in text:
    board.write_text(text.rstrip() + "\n\n" + include)
PY

scripts/config -e REGULATOR_APPLE_D2365_MESA -e APPLE_MESA_A9
make LLVM=1 ARCH=arm64 olddefconfig >/dev/null
grep -qx CONFIG_REGULATOR_APPLE_D2365_MESA=y .config
grep -qx CONFIG_APPLE_MESA_A9=y .config

git add drivers/Kconfig drivers/Makefile drivers/mesa \
        drivers/regulator/Kconfig drivers/regulator/Makefile \
        drivers/regulator/apple-d2365-mesa.c \
        arch/arm64/boot/dts/apple/s8003-j71t.dts \
        arch/arm64/boot/dts/apple/s8003-j71t-mesa-status.dtsi
git diff --cached --check
git commit -m "arm64: apple: add bounded J71t Mesa status transport"
'

KERNEL_MEMORY="${KERNEL_MEMORY:-4g}" KERNEL_JOBS="${KERNEL_JOBS:-4}" \
	"$ROOT/scripts/build-kernel.sh" --tree tri53-mesa1
