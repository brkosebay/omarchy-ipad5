#!/usr/bin/env bash
# Offline arithmetic, schema, and source checks. No device access.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TREE="${TREE:-display}"
"$ROOT/scripts/build-kernel.sh" --run "$TREE" '
set -e
clang -Wall -Wextra -Werror -fsanitize=undefined,address -I drivers/video \
    /work/scripts/display/test-scratch.c -o /tmp/adbe-test-scratch
/tmp/adbe-test-scratch
export PYTHONPATH=/work/build/periph/schema-lib
export PATH=/work/build/periph/schema-tools:/work/build/periph/schema-lib/bin:$PATH
test -x /work/build/periph/schema-lib/bin/dt-doc-validate
make LLVM=1 ARCH=arm64 dt_binding_check DT_SCHEMA_FILES=display/adbe0,s8000.yaml
make LLVM=1 ARCH=arm64 dtbs_check DT_SCHEMA_FILES=display/adbe0,s8000.yaml
'
