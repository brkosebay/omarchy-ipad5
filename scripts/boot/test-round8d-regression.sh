#!/usr/bin/env bash
# Offline negative controls: retained-EN shutdown (8c) and local resets (8b).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/build/boot/round8d"
mkdir -p "$OUT"
"$ROOT/scripts/build-kernel.sh" --run boot '
for round in 8b 8c; do
  cd /src/wt/bootr${round}-check
  git diff --binary > /tmp/round8d-old-source.patch
  cmp /tmp/round8d-old-source.patch /work/build/boot/round${round}/kernel.patch
  test "$(git rev-parse HEAD)" = "$(cat /work/build/boot/round${round}/integration-base.txt)"
done
'
if "$ROOT/scripts/boot/test-ans-handoff-offline.sh" --source /src/wt/bootr8c-check \
    HandoffTests.test_shutdown_shst_then_disabled_controller_before_port_reset \
    > "$OUT/regression-round8c.log" 2>&1; then
    echo 'FAIL: Round 8c unexpectedly disabled the outgoing controller' >&2
    exit 1
fi
grep -qF 'master==1 && writes==1 && reads==2 && !dump' "$OUT/regression-round8c.log"
grep -qF 'Assertion' "$OUT/regression-round8c.log"
grep -qF 'FAILED (errors=1)' "$OUT/regression-round8c.log"
echo 'PASS: Round 8c retained-EN shutdown regression reproduced (expected assertion failure)'
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
