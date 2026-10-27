# Charge9: attach defaults, idle USB notifications and source identity

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-27. Branch `a9/charge9`, based on `a9/charge8`
`824a8bda2cd69634884d8830224a41aa3ceecd93`. Kernel commit
`16a34c8432c61937366964d493b82f387e7d51cc`; entry `charge9-16a34c8432`.
The existing `/src/wt/charge` worktree was reused, with debug info disabled.
No device access, push or workspace git commit was performed. This is an
**offline-tested implementation; round 9 hardware tests remain NOT RUN**.

## What the device capture establishes

Input evidence: [round 8 result](../release/OMISSIONS.md),
[TSV](../release/OMISSIONS.md), and
[dmesg/state](../release/OMISSIONS.md), workspace evidence
commit `26713fd8`. `scripts/periph/charge9-trace-fixture.py` reads all 226 TSV
rows and dmesg, verifies the four failure sequences, and emits numeric inputs
and source hashes under `build/charge9/tests/capture.*` for the C harnesses.
The prior userspace request was retired and `override_ua=0` throughout.

1. Mac USB-A cable reached SDP and 2400 mA. This established that calibration,
   the ordinary ramp and the generic host ceiling worked in round 8.
2. Fast Mac→wall swap had a Tristar `other -> device` at 138.490879 and a new
   ID at 144.507381, while the sampled VBUS stayed high. Round 8 advanced the
   epoch only on measured VBUS loss, retaining the old SDP identity and ceiling.
3. On clean wall replug, input code `0x20` was programmed at 275.801237; the
   attach-default `0x02` was reported at 275.827219, **26 ms later**. The old
   backoff path selected a nominal 500 mA step but used
   `min(calibrated_floor_code, observed_code)`: it deliberately programmed 0x02
   again. Thus `step_ua=500000` and `live_ua=100000` could persist indefinitely.
4. A complete BC1.2 result was published at 276.150036: type **3, CDP** in the
   Linux enum, allowance 1500 mA, passive D+/D− 39/49 mV. The first C-cable ID
   arrived at 276.425873. The TSV still shows that BC result at **281.08**; it
   was erased by the failing CC refresh at **281.386450**, not immediately by
   the ID exchange. Later CC samples reached 1624–1628 mV while source type
   returned to Unknown and the live setting remained 100 mA.
5. Unplugged battery status was Full despite negative current, including
   −666000 µA. The gauge's current/status helper tests its full flag before
   the current sign. J71t uses the cache-only HDQ backend, with a 30-second
   measurement refresh, so status correction must also work on the cached path.

## What raised usb-event

There is exactly one writer that increments Tristar's cancellation generation:
`apple_tristar_usb_event()`, subscribed to `gadget_current_notifier`. The ID-bus
implementation does not call it. Automatic GetID and pin sampling both hold
`ts->lock`, so automatic ID traffic cannot overlap a running conversion.

Round 8 incremented the generation on **every** notification, even when
connected/configured/host_grant were all false. Concrete kernel producers are
`usb_gadget_set_state()` (including NOTATTACHED), `gadget_current_set_base()`
(including a zero draw), and `usb_gadget_current_budget_suspend()`. These notify
unconditionally. DWC2 reset/disconnect paths call the gadget disconnect callback,
which can set a zero draw, and publish NOTATTACHED. D+/D− routing and BC pulses
can therefore feed line-state/accounting notifications back into detection.

The new harness executes those **actual gadget-core functions**: three idle
STATE/BASE/SUSPEND notifications raise the old generation three times and
invalidate a sample, with no USB data session. Through the new callback, the
same notifications leave the cancellation generation unchanged. Injecting
those events during every mocked ADC conversion now permits a complete BC
classification. A real short data session beginning and ending between two
checks still advances the generation twice and discards the sample; configured
traffic excludes routing entirely.

The old device log did not record notifier origin or the relevant DWC2 IRQ.
Consequently, the exact live IRQ/producer cannot be selected retrospectively;
routing-induced idle notifications are the supported explanation, not a newly
measured hardware trace. Round 9 tags every producer and records total/idle
counts, the data generation and the last origin in `source_epoch:`. This makes
that remaining distinction observable during the orchestrator's test.

## Changes

**D2365 policy.** Exact code 0x02, with the expected charge register and input
still enabled, is treated as an attach reset during a **15-second** settling
window after a source epoch or first ownership snapshot. The capture's six-second
phase-to-ID delay motivates a window longer than five seconds. The worker
reapplies the guarded current step without setting an adaptive cap, delaying
for 60 seconds or tripping the ordinary repeated-ownership-change latch.
`attach_resets` counts successful repairs. A pending ramp timer cannot skip
repair of the current step.

For other unexplained lower settings, the conservative policy may still choose
a lower nominal rung, but it now writes that rung's **calibrated code**. It no
longer silently labels a 100 mA code as the 500 mA step. “Assert the step” means
its calibrated setting: this unit's nominal 500 mA rung normally reads 475000 µA.
Repairs happen at worker execution, not atomically with a hardware reset.
Transport errors or failed battery/source guards can defer them; the driver
cannot guarantee a write succeeded during failed I/O. No VBUS droop measurement
or verified VINDPM regulation is inferred from an input-setpoint change.

**Convergent classification.** Idle notifications wake work but do not cancel
samples. An active USB snapshot, or the end of a previously observed active
snapshot, still advances cancellation generation. Role hardware serialization,
strict VBUS/orientation checks, restoration ordering and ADC poisoning remain.
No PHY or descriptor sequence is changed.

A separately validated BC source item (`bc_ua`, `bc_type`) lasts for its source
epoch. The first valid ID refines the same source, retains that item, and
schedules one classification after 300 ms of settling; repeated identical IDs
do not keep postponing it. CC uses at most three samples to obtain two matching
advertisements, permitting one unsettled first reading, with at least 20 ms
between reads. `cc_first_mv` helps interpret a debounce failure. Once BC is
known, CC refresh does not repeat the D+/D− measurements or BC pulses. A failed
CC refresh removes the CC elevation while retaining the proven BC allowance.

**BC1.2 plus USB-C.** These allowances are not added. The 1500 mA BC item is a
baseline and qualification for the CC item. The verified C→Lightning cable's
1624–1628 mV samples exceed the 1230 mV boundary for a 3000 mA advertisement.
With completed DCP/CDP evidence, Linux selects that qualified CC allowance;
the charger bounds it to the board's 2400000 ceiling and ramps normally.
Without charging-port qualification, a 3 A CC result is capped at 1500 mA;
1500 mA uses the existing calibrated nominal 1000 mA rung.

The iOS evidence remains the same kernelcache SHA256
`c6b95fcf0efe52ee9258454d01d14adc9b0cb922d94c54a04a4ce11d1ae81e0a`:

- `fffffff005de4024`: CC thresholds ≤659 / 660..1229 / ≥1230 mV yield
  0 / 1500 / 3000 mA source items.
- `fffffff005e236c0`: detector initialization stores its qualification argument
  at `+0xd0`; `fffffff005de8328` supplies the charging-port type there.
- `fffffff005e23a90`: constructs/debounces the CC item, reducing a 3000 mA item
  to 1500 if that qualification argument is zero. It does not universally cap
  a qualified CC source at the BC item's 1500 mA.
- `fffffff005f15504`: board `usb-input-limit-max` supplies the final ceiling.
  The field/ADC route evidence is retained in [charge8](charge8.md).

There is no fixed high-current grant from the cable ID and no USB-PD negotiation.

**Source epochs.** A valid changed six-byte ID, or a transition from an existing
phase to device/empty, starts a new epoch even with continuously sampled VBUS.
It clears computer/brick/BC identity, stale gadget grants, retry state and the
old ID baseline; the charger revokes per-attach user/adaptive/disable state and
restarts from the floor. The first ID after that phase boundary is the new
epoch's initial identity, avoiding a duplicate epoch for that same observation.
Ordinary USB suspend/reset and idle notifications do not change epochs.
Observed VBUS removal retains its existing behavior. A same-ID swap with no
observable VBUS or phase edge remains indistinguishable; this is not a guarantee
for every sub-poll physical event.

**Gauge status.** Only `apple,j71t-bq27540` cached STATUS is adjusted. Negative
measured current returns Discharging even at full capacity. With no source
online, zero or stale positive cached current returns Not charging. Online
nonnegative Full remains Full. Missing/failed online reads remain errors when
needed. The source query happens after releasing the gauge mutex, avoiding the
charger→gauge / gauge→charger lock inversion. No gauge read cadence or writes
were added; current measurements can remain up to 30 seconds old.

The PMIC write whitelist is unchanged: charger 0xa03/0xa08/0xa00 bit 1, plus
the existing guarded ADC trigger 0x4000=1. No syscfg/NVRAM/boot-firmware access,
QEMU process control, or device command was used.

## Verification and reproduction

```sh
scripts/build-kernel.sh --run charge 'python3 -B -I /work/scripts/periph/prepare-charge9.py'
scripts/periph/check-charge9.sh
scripts/periph/build-charge9.sh
python3 -B -I scripts/periph/verify-charge9.py
```

The checks execute before the full build; generated kernel objects are cleaned
only after packaging to limit Docker disk use. Offline results:

- Fresh W=1 builds: Tristar, charger, bq27xxx, gadget core and role switch,
  zero warnings/errors. Strict checkpatch: zero errors, warnings or checks.
- Actual gadget-core idle-notifier regression and new filter, brief data-session
  cancellation, active-session exclusion and pthread role-lock serialization.
- Captured Mac ramp, missed-VBUS fast swap, held unplug and clean wall replug;
  repeated 26 ms resets, six-second identity settling, BC-before-ID retention,
  1624/1625 mV CC upgrade and failed-refresh BC fallback.
- ASan/UBSan policy/detector/ADC/gauge harnesses; all 30 source/ceiling cases,
  calibration/thermal/gauge/transport guards, 17 transaction rollback fault
  points, source epoch/override tests, ADC poison and late completion tests.
- Gauge captured Full/negative unplug/wall cases, offline zero/positive cache,
  online Full/Charging, missing source, source I/O error and reference release.
- Existing accessory policy (73 checks), ID-bus core regressions and a bounded
  fake-sysfs logger check (both diagnostic lines, one 12-column TSV row) pass.

Build/package output is in `build/charge9/`. Hardware charging, warm-kexec,
actual notifier origins and the desktop icon remain NOT RUN for this entry.
Use the [orchestrator test plan](charge9-test-plan.md). VBUS-voltage-triggered
backoff remains unsupported; no battery-current outcome is inferred from mocks.

## Artifacts and build result

Payload: `build/charge9/m1n1-linux-charge9-16a34c8432.bin`. SHA256:
`ea88ab8ffed69ccbb4b4716863344838e1f6d29762f6b3424d9841f4e675b289`.

Entry directory: `build/charge9/entry/`. All hashes are in
`build/charge9/manifest.json` and `build/charge9/entry/SHA256SUMS`.

- `Image.gz`: `dc8747f03023766cd07a20258192e7db8264b3a003bd70db95070a5627a6cd13`
- `dtb`: `2a047c0b39cb1b46a94efee78198a635cefc9298b9dba2cd00f0591a7a870c6f`
- `initramfs.gz`: `4681b31dc77dd8050427c4d60073630fb480a1112d94edf68002438ba61755c3`

The first full build ended with compiler exit 137 in the unrelated brcmfmac
`firmware.o`, without a compiler diagnostic. This is consistent with resource
pressure, not a demonstrated source error. That log is retained as
`build/charge9/build-attempt1.log`. Retrying with `KERNEL_JOBS=2` reused completed
objects and built Image.gz and 105 DTBs successfully. The successful log is
`build/charge9/build.log`. W=1 and strict-checkpatch logs remain separately
recorded. The final tree is clean; generated objects were cleaned after
packaging. Docker reported 7.2 GiB free, matching the initial check for round 9.
