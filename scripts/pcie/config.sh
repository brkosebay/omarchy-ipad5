#!/usr/bin/env bash
# Reset the pcie tree's .config to HoolockLinux config_16k plus
# patches/pcie/pcie.config (olddefconfig). scripts/pcie/build.sh already
# merges the fragment on every build; use this only to start from scratch.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
"$ROOT/scripts/build-kernel.sh" --run pcie '
set -e
cp /work/configs/hoolock-16k.config .config
scripts/kconfig/merge_config.sh -m -O . .config /work/patches/pcie/pcie.config >/dev/null
make LLVM=1 ARCH=arm64 olddefconfig >/dev/null
mkdir -p /work/build/kernel-pcie
cp .config /work/build/kernel-pcie/config.merged
'
"$ROOT/scripts/pcie/check-config.sh" "$ROOT/build/kernel-pcie/config.merged"
