# Round 6 peripherals report

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-22. Kernel `64221b189e` on `a9/periph`; 55 exported patches from
`6831bc701`. No device access or host-repo commits by this implementation
agent. No changes to firmware selection, PMIC register meanings or other
workstreams.

## Device evidence and diagnosis

**Verified in `docs/test-logs/integ10/charging.txt`:** the Round 5 owner
request succeeds and charges at +1.155…1.165 A across 90 seconds. Charge-now
recovers and increases from 3635000 to 3655000 µAh. Before the owner request,
USB offers nominal 500 mA, but charger registers retain the input 100000 /
battery 2600000 handoff limits and no automatic source-budget message appears.
The initial cache refresh, battery status and thermal temperature report EIO
at about 0.81 seconds.

**Verified in source and regression tests:** the policy already requeued its
worker every second. However, a missing battery provider, cache-derived
PRESENT=0, or raw unavailable battery-property error could set `faulted`
before any charger write. Subsequent workers skipped activation while that
latch remained set. The owner request explicitly clears the latch and thus
could succeed later. This startup path reproduces the observed behavior;
its exact first return code on integ10 is **inferred**, because that build
logged neither the skipped decision nor its internal fault state.

## Changes

- The cache-only gauge tracks whether a complete first safety sample has
  succeeded: flags, capacity, current/status, voltage and temperature. Until
  then all cached properties, including presence, return ENODATA. Raw errors
  remain in the cache/debug output. First-sample failures retry after one
  second instead of waiting the regular 30-second interval. Optional charge,
  cycle or time estimates do not prevent readiness. After readiness, genuine
  mandatory read errors retain their existing reporting and regular interval;
  stale values are never treated as safe measurements.
- The charger treats an absent provider or ENODATA from its actual battery
  guard as EAGAIN. It makes no writes and keeps its bounded one-second
  reevaluation. Once the source and gauge are valid, nominal 500 mA selects
  input 0x20 (475000 µA on this unit) and battery 0x08 (400000 µA) without
  an owner request or external notification. Known absence, out-of-range
  temperature/voltage, PMIC/status/readback errors, timeout, hardware cap,
  checked transactions and rollback remain guarded as before.
- Deferred policy decisions are now logged on a change and every 30 seconds
  while persistent. The first-sample wait says `charging policy waiting for
  valid battery/source sample; retry in 1000 ms`. Invalid calibration, owner
  disable, guard deferral and a latched fault each have an explicit decision
  message. Successful activation retains the source/owner budget log.
- The power-supply thermal adapter converts unavailable temperature ENODATA
  into EAGAIN. The existing thermal core already retries EAGAIN quietly
  without its error/backoff path. Successful Celsius conversion and genuine
  runtime errors remain unchanged. No placeholder temperature is supplied.

**Retained:** the owner's supported positive input-limit request, including
2400000 on the measured Mac port, and its revocation/restore behavior. The
400-mA fallback is a battery-current ceiling; actual net battery current can
still be negative if system load consumes the available input power. No Mac
host identity or extra source allowance is inferred from link speed/VBUS.

## Verification and next boot

**Verified offline:** actual charger functions pass the existing 56
fault/guard cases and prior owner/calibration/lifecycle tests, plus 65
consecutive no-data boot ticks, first-valid-sample activation without any
owner action, missing-provider/property cases, real temperature/voltage
boundaries, bounded log frequency and retained genuine-fault latches.

Actual gauge refresh/getter, battery uevent, power-supply thermal adapter and
thermal retry functions pass ASan/UBSan with failures at all five mandatory
sample stages, one-second retries, 100 quiet thermal rechecks per fault,
optional errors, successful recovery and post-readiness errors. The prior
charge-now optional/error/recovery tests are retained in that harness.

The clean Image.gz and 104 Apple DTBs build. All 55 patches replay exactly;
new patches 53–55 pass strict checkpatch. Kernel/DT linkage and the unchanged
single-gzip initramfs are verified. Artifact hashes and tested source hashes
are in `build/periph/round6-final.json`.

**Pending device verification:** automatic 475000 / 400000 limits once the
first sample is valid, the new decision messages and quiet initial HDQ/thermal
handling. Owner override and numeric charge-now are already verified on
integ10 and are retained, but should receive a regression check.

```sh
TREE=periph scripts/mkpayload.sh build/initramfs-fw.gz
```

Follow [test-plan.md](test-plan.md), capturing the automatic limits before
any owner write. Touch and other integration tests remain with their agents.
