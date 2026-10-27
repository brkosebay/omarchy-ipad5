#!/usr/bin/env bash
# Offline check of the CT819 lux computation: the kernel's own function
# (a9/sensors, drivers/iio/light/tsl2583.h) runs on the host against this
# unit's LSCI blob and is compared with an independent implementation.
#
#   scripts/sensors/test-ct819-lux.sh [LSCI.bin]    default build/firmware/lib/firmware/apple/j71t-LSCI.bin
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LSCI="${1:-$ROOT/build/firmware/lib/firmware/apple/j71t-LSCI.bin}"
W="$ROOT/build/sensors-work"
mkdir -p "$W/host"
"$ROOT/scripts/build-kernel.sh" --run sensors 'cp drivers/iio/light/tsl2583.h /work/build/sensors-work/host/tsl2583.h' >/dev/null 2>&1
cc -O1 -Wall -Wextra -Wno-unused-function -I "$ROOT/scripts/sensors/shim" -I "$W/host" \
	-o "$W/host/ct819-lux-host-test" "$ROOT/scripts/sensors/ct819-lux-host-test.c"
"$W/host/ct819-lux-host-test" --kunit-cases
"$W/host/ct819-lux-host-test" "$LSCI" | python3 -B -I "$ROOT/scripts/sensors/test-ct819-lux.py" "$LSCI"
