#!/usr/bin/env bash
# Offline negative control: the new slot fixtures MUST fail against Round 8b.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/build/boot/round8c"
mkdir -p "$OUT"
# Refuse a moving or misidentified old integration overlay.
"$ROOT/scripts/build-kernel.sh" --run boot '
cd /src/wt/bootr8b-check
git diff --binary > /tmp/round8c-regression-source.patch
cmp /tmp/round8c-regression-source.patch /work/build/boot/round8b/kernel.patch
test "$(git rev-parse HEAD)" = "$(cat /work/build/boot/round8b/integration-base.txt)"
'
for kind in allocation retirement; do
    case "$kind" in
    allocation)
        name=test_slot_owner_and_local_retirement_errors_never_reset
        log=regression-round8b.log
        expected='!ans.used_req[0] && !retires && !schedules'
        ;;
    retirement)
        name=test_controller_stop_before_dma_and_failed_slot_quarantine
        log=regression-round8b-retirement.log
        expected='ans.used_req[0]==BIT(14) && schedules==old_schedules'
        ;;
    esac
    if "$ROOT/scripts/boot/test-ans-io-offline.sh" --source /src/wt/bootr8b-check \
        "ProductionTests.$name" > "$OUT/$log" 2>&1; then
        echo "FAIL: Round 8b unexpectedly passed $kind regression" >&2
        exit 1
    fi
    grep -qF "$expected" "$OUT/$log"
    grep -qF 'Assertion' "$OUT/$log"
    grep -qF 'FAILED (errors=1)' "$OUT/$log"
    echo "PASS: Round 8b $kind regression reproduced (expected assertion failure)"
done
