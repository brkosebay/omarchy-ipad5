# Round 5: unavailable charge-now samples

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-22. No device access or commits were performed by this workstream.

## Cause and change

**Verified:** `docs/test-logs/exp03-arm.log` records kernel
`90aa435537bb` reporting `charge_now` error `-5` at 0.891225 and 0.896088
seconds. The cache is initialized to `-ENODATA`, so an untouched startup
cache alone does not explain these `-EIO` errors.

The asynchronous cache refresh independently records the result of the
optional charge measurement. A failed read records its transport error in
`cache.charge_now.error`; the cached getter previously returned that error
directly. The power-supply core's `add_prop_uevent()` skips `-ENODATA`, but
an `-EIO` aborts the complete battery uevent. Thus one unavailable optional
measurement could suppress otherwise valid battery telemetry for userspace.

**Implemented:** only the cached `CHARGE_NOW` getter now reports
`-ENODATA` when its individual sample is unavailable. The original error
remains in the internal cache. Successful measurements, including a real
zero, remain available through `charge_now` and the battery uevent. A later
successful refresh restores the property automatically. An old numeric
value retained internally after a failed read is not exposed as a fresh
sample, and no replacement value is fabricated.

Gauge-wide errors and errors for other properties are unchanged. No gauge
register, transport, polling interval, charging control, design capacity or
energy conversion was changed. The property remains advertised so UPower
can use a successful charge-based sample.

**Unresolved:** the saved log does not establish whether the failed HDQ
charge read was transient or whether this unit needs a different charge
register mapping. The J71t cache profile uses the existing BQ27545 NAC
mapping; there is insufficient evidence to change it or permanently remove
the property. The next device test must capture a numeric sample before
claiming that mapping is verified.

## Validation

**Passed:** the actual `bq27xxx_battery.o` compiles in the periph kernel
tree. `git diff --check` passes, and strict checkpatch reports zero errors,
warnings or checks for this change. The log is
`build/periph/round5-battery-check.log`.

The host regression harness extracts the actual cache initialization,
cache-update, cached getter and power-supply `add_prop_uevent()` functions
from exported kernel sources; it does not duplicate their implementation.
ASan/UBSan checks passed for startup no-data, six optional read errors,
successful recovery, valid zero, unchanged gauge-wide and other-property
errors, omitted unavailable uevent fields, and subsequently published
successful charge samples. Hardware HDQ transfers are outside this test.

The tested battery source SHA256 is
`e3a7ba8d49db0f782de5ef56a45d01f6a1dcc9c1225b552b421476f0da17a375`.
Exact source snapshots and generated test artifacts are under
`build/periph/round5-battery/`.

Reproduction:

```sh
scripts/build-kernel.sh --run periph \
  'python3 /work/scripts/periph/round5-battery.py; make ARCH=arm64 LLVM=1 drivers/power/supply/bq27xxx_battery.o'
python3 -B scripts/periph/test-round5-battery.py
```

The orchestrator owns the final complete kernel build, commit and patch
export. For the next device test, capture the full battery uevent and
`charge_now`, `charge_full`, `capacity`, `voltage_now` and `current_now`
together, then repeat after a cache-refresh interval. While the optional
sample is unavailable, its direct sysfs read may truthfully return
`ENODATA`; the other valid uevent fields should still publish. No device
commands were run here.
