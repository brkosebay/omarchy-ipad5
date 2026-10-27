#!/usr/bin/env bash
# Host only. Run after the full build, never concurrently in the aop tree.
# Override only this translation unit's IIO visibility; don't churn .config.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
"$ROOT/scripts/build-kernel.sh" --run aop '
test "$(git branch --show-current)" = a9/aop
variant=$(mktemp /tmp/aop-ring-iio-XXXXXX.h)
trap '\''rm -f "$variant"'\'' EXIT
for mode in off module cma-off; do
    cat > "$variant" <<EOF
#undef CONFIG_IIO
#undef CONFIG_IIO_MODULE
EOF
    if [ "$mode" = module ]; then
        echo "#define CONFIG_IIO_MODULE 1" >> "$variant"
    fi
    if [ "$mode" = cma-off ]; then
        echo "#undef CONFIG_CMA" >> "$variant"
    fi
    make LLVM=1 ARCH=arm64 W=1 KCFLAGS="-include $variant" \
        drivers/soc/apple/a9-aop-ring-reader.o drivers/soc/apple/a9-aop-ring-target.o
    if llvm-nm -u drivers/soc/apple/a9-aop-ring-reader.o drivers/soc/apple/a9-aop-ring-target.o | grep -q "iio_"; then
        echo "Unexpected IIO dependency in $mode reader" >&2
        exit 1
    fi
    echo "PASS: IIO $mode reader has no IIO symbol dependencies"
    if [ "$mode" = cma-off ]; then
        if llvm-nm -u drivers/soc/apple/a9-aop-ring-reader.o | grep -q "cma_"; then
            echo "Unexpected CMA dependency in CMA-disabled reader" >&2
            exit 1
        fi
        echo "PASS: CMA-disabled reader has no CMA symbol dependencies"
    fi
done
make LLVM=1 ARCH=arm64 W=1 drivers/soc/apple/a9-aop-ring-reader.o drivers/soc/apple/a9-aop-ring-target.o
'
