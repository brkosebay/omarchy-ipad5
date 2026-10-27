#!/usr/bin/env bash
# Build m1n1 (HoolockLinux `idevice`, pinned) with patches/pcie/m1n1/*.patch,
# which copies the runtime ADT /arm-io/nvme-mmu0 "sart-region" into the DT's
# nvme-hmb node. Without it the pcie kernel keeps NVMe off (safe, but no disk).
#
# Same build as HoolockLinux's CI (make CHAINLOADING=1), but native arm64 gcc.
# Output: build/m1n1-pcie/m1n1.bin (use with scripts/pcie/mkpayload-pcie.sh)
#
# Cost: a ~1 GB builder image (fedora + rustup) and ~10 MB of source in the
# linux-a9-src volume. `docker rmi linux-a9-m1n1build` frees the image.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
VOLUME="linux-a9-src"
IMAGE="linux-a9-m1n1build"
BASE="${BASE:-d5a10ac52a}" # HoolockLinux/m1n1 idevice, 2026-08-31
OUT="$ROOT/build/m1n1-pcie"
mkdir -p "$OUT"

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
	docker build --platform linux/arm64 -t "$IMAGE" - <<'EOF'
FROM fedora:latest
RUN dnf -y install gcc make git python3 curl diffutils findutils && dnf clean all
RUN curl -sSf https://sh.rustup.rs | sh -s -- -y --profile minimal \
        --target aarch64-unknown-none-softfloat
ENV PATH=/root/.cargo/bin:$PATH
EOF
fi

docker run --rm --platform linux/arm64 -v "$VOLUME:/src" -v "$ROOT:/work" \
	-v "$OUT:/out" -e BASE="$BASE" "$IMAGE" bash -c '
set -euo pipefail
git config --global --add safe.directory "*"
git config --global user.name linux-a9
git config --global user.email linux-a9@localhost
cd /src
if [ ! -d m1n1-pcie/.git ]; then
	git clone -q --branch idevice https://github.com/HoolockLinux/m1n1.git m1n1-pcie
fi
cd m1n1-pcie
git fetch -q --depth 50 origin idevice || true
git checkout -q --detach "$BASE"
git clean -qfdx
git am -q /work/patches/pcie/m1n1/*.patch
git submodule update -q --init --depth 1
make -j"$(nproc)" USE_CLANG=0 ARCH= CHAINLOADING=1 build/m1n1.bin >/dev/null
cp build/m1n1.bin /out/m1n1.bin
git log --oneline -2 > /out/COMMIT
echo "built: $(ls -la /out/m1n1.bin)"
'
