#!/usr/bin/env bash
# Seed the a9/sensors tree's .config (from the periph tree's, which already has
# TSL2583 and the D2365 GPIO driver) and merge scripts/sensors/kernel.config.
# Run before the first `scripts/build-kernel.sh --tree sensors`; safe to rerun.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
exec "$ROOT/scripts/build-kernel.sh" --run sensors '
if [ ! -f .config ]; then
	if [ -f /src/wt/periph/.config ]; then
		cp /src/wt/periph/.config .config
	else
		cp /work/configs/hoolock-16k.config .config
		scripts/kconfig/merge_config.sh -m .config /work/scripts/periph/kernel.config
	fi
fi
scripts/kconfig/merge_config.sh -m .config /work/scripts/sensors/kernel.config
make LLVM=1 ARCH=arm64 olddefconfig
grep -E "^CONFIG_(TSL2583|GPIO_APPLE_D2365|KEYBOARD_GPIO_POLLED|IIO)=" .config
'
