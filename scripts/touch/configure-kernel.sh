#!/usr/bin/env bash
# Offline only: preserve the touch tree's config and enable required parents.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
exec "$ROOT/scripts/build-kernel.sh" --run touch '
if [ ! -f .config ]; then cp /work/configs/hoolock-16k.config .config; fi
scripts/config -e INPUT -e INPUT_EVDEV -e INPUT_TOUCHSCREEN \
    -e REGULATOR -e GPIOLIB -e SPI -e SPI_MASTER -e OF \
    -e TOUCHSCREEN_APPLE_N1 -e SPI_APPLE_S8000 \
    -e COMMON_CLK_APPLE_S8000_TOUCH -e REGULATOR_APPLE_D2365_TOUCH \
    -e MFD_SIMPLE_MFD_I2C -e I2C_APPLE \
    -d DEBUG_INFO -d DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT -e DEBUG_INFO_NONE
make LLVM=1 ARCH=arm64 olddefconfig >/dev/null
for symbol in INPUT_EVDEV INPUT_TOUCHSCREEN REGULATOR TOUCHSCREEN_APPLE_N1 \
    SPI_APPLE_S8000 COMMON_CLK_APPLE_S8000_TOUCH REGULATOR_APPLE_D2365_TOUCH \
    MFD_SIMPLE_MFD_I2C I2C_APPLE; do
    grep -qx "CONFIG_${symbol}=y" .config || { echo "missing built-in: $symbol" >&2; exit 1; }
done
echo "PASS: all touch drivers and parent subsystems built in; debug info off"
'
