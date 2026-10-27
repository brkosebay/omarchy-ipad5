#!/usr/bin/env bash
# Offline round-2 checks: host arithmetic test (ASan/UBSan), W=1 rebuild of
# the new objects, binding and DTB schema, DTB identity. No device access.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TREE="${TREE:-display}"
"$ROOT/scripts/build-kernel.sh" --run "$TREE" '
set -e
clang -Wall -Wextra -Werror -fsanitize=undefined,address -I drivers/gpu/drm/adbe \
    /work/scripts/display/test-adbe-addr.c -o /tmp/adbe-test-addr
/tmp/adbe-test-addr
# Recompile only the new objects so their W=1 output is isolated.
rm -f drivers/gpu/drm/adbe/*.o
make LLVM=1 ARCH=arm64 W=1 drivers/gpu/drm/adbe/ 2>&1 | tee /tmp/adbe-w1.log
if grep -E "warning|error" /tmp/adbe-w1.log; then
	echo "FAIL: W=1 diagnostics in drivers/gpu/drm/adbe"
	exit 1
fi
echo "W=1 clean: drivers/gpu/drm/adbe"
export PYTHONPATH=/work/build/periph/schema-lib
export PATH=/work/build/periph/schema-tools:/work/build/periph/schema-lib/bin:$PATH
test -x /work/build/periph/schema-lib/bin/dt-doc-validate
make LLVM=1 ARCH=arm64 dt_binding_check DT_SCHEMA_FILES=display/adbe0,s8000.yaml
make LLVM=1 ARCH=arm64 dtbs_check DT_SCHEMA_FILES=display/adbe0,s8000.yaml
# Round 2 must not change the device tree: compare against the round-1 tip.
git diff --exit-code 95149d28070371295295cf3fd2da0739c479a41a HEAD -- arch/arm64/boot/dts
echo "DTS unchanged since round 1 (95149d2807)"
'
