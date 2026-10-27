#!/usr/bin/env bash
# Host-only source installation/object build in a9/power. Never contacts iPad.
# Adds the new disabled default to .config, compiles only the observer object.
# Does not enable the observer, build a boot payload or boot.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
docker run --rm --platform linux/arm64 \
    -v linux-a9-src:/src -v "$ROOT:/work" linux-a9-kbuild bash -eu -c '
cd /src/wt/power
test "$(git branch --show-current)" = a9/power
python3 - <<"PY"
from pathlib import Path
p = Path("drivers/soc/apple")
src = Path("/work/scripts/power/kernel/apple-a9-dvfs-ro.c").read_bytes()
(p / "apple-a9-dvfs-ro.c").write_bytes(src)
config = p / "Kconfig"
s = config.read_text()
if "config APPLE_A9_DVFS_RO\n" not in s:
    s = s.replace("endmenu\n", """config APPLE_A9_DVFS_RO
\ttristate "Read-only Apple S8003 CPU DVFS observer"
\tdepends on ARCH_APPLE && ARM64 && DEBUG_FS && OF
\thelp
\t  Default-off debugfs snapshot of CPU state and PSINFO words.
\t  Requires enable=1 and CAP_SYS_RAWIO. No hardware write path.
\t  Does not provide temperatures or decode PMIC rail voltages.

endmenu
""")
    config.write_text(s)
makefile = p / "Makefile"
s = makefile.read_text()
line = "obj-$(CONFIG_APPLE_A9_DVFS_RO) += apple-a9-dvfs-ro.o\n"
if line not in s:
    makefile.write_text(s + "\n" + line)
PY
make LLVM=1 ARCH=arm64 olddefconfig >/dev/null
make LLVM=1 ARCH=arm64 CONFIG_APPLE_A9_DVFS_RO=m drivers/soc/apple/apple-a9-dvfs-ro.o
mkdir -p /work/build/power/dvfs-ro
llvm-objdump -dr drivers/soc/apple/apple-a9-dvfs-ro.o > /work/build/power/dvfs-ro/observer.dis
llvm-nm -u drivers/soc/apple/apple-a9-dvfs-ro.o > /work/build/power/dvfs-ro/undefined-symbols.txt
git diff --check
'
