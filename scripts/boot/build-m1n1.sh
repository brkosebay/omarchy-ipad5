#!/usr/bin/env bash
# Build m1n1 for warm reboots: HoolockLinux `idevice` (pinned, same base as
# scripts/pcie/build-m1n1.sh) + patches/pcie/m1n1 (NVMe HMB, Wi-Fi
# calibration) + patches/boot/m1n1 (EL3 PSCI, re-entry).
#
# The PSCI part is off unless the payload carries the variable psci=1
# (scripts/boot/mkpayload.sh adds it), so this m1n1 is a drop-in for
# build/m1n1-pcie/m1n1.bin.
#
# Output: build/m1n1-boot/m1n1.bin (+ proxyclient/, the matching proxy tools)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
VOLUME="linux-a9-src"
IMAGE="linux-a9-m1n1build" # created by scripts/pcie/build-m1n1.sh
BASE="${BASE:-d5a10ac52a}"
OUT="$ROOT/build/m1n1-boot"
mkdir -p "$OUT"

docker image inspect "$IMAGE" >/dev/null 2>&1 ||
	{ echo "no $IMAGE image: run scripts/pcie/build-m1n1.sh once" >&2; exit 1; }

docker run --rm --platform linux/arm64 -v "$VOLUME:/src" -v "$ROOT:/work" \
	-v "$OUT:/out" -e BASE="$BASE" "$IMAGE" bash -c '
set -euo pipefail
git config --global --add safe.directory "*"
git config --global user.name linux-a9
git config --global user.email linux-a9@localhost
cd /src
if [ ! -d m1n1-boot/.git ]; then
	git clone -q --branch idevice https://github.com/HoolockLinux/m1n1.git m1n1-boot
fi
cd m1n1-boot
git fetch -q --depth 50 origin idevice || true
git checkout -q --detach "$BASE"
git clean -qfdx
git am -q /work/patches/pcie/m1n1/*.patch /work/patches/boot/m1n1/*.patch
git submodule update -q --init --depth 1
make -j"$(nproc)" USE_CLANG=0 ARCH= CHAINLOADING=1 build/m1n1.bin >/dev/null
cp build/m1n1.bin /out/m1n1.bin
rm -rf /out/proxyclient && cp -r proxyclient /out/proxyclient # for scripts/boot/m1n1-dev.sh
git log --oneline -3 > /out/COMMIT
echo "built: $(ls -la /out/m1n1.bin)"
'
