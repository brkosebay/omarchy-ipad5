# Charge7: source detection, attach lifetime and input limits

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-27. Built offline from `a9/usb-host5b` `2801e838fc9a` in the Docker
worktree `/src/wt/charge`, branch `a9/charge`, tip
`a8aa6fea363394f3d21f2eef7525998884b4cf3f` (three commits). Entry:
`charge7-a8aa6fea36`. No iPad access, deployment or
workspace git commits. Kernel commit and payload hashes are recorded in
`build/charge7/manifest.json`; exported kernel commits are in `patches/charge7/`.
Payload SHA256: `fdb9ffc43951ce51c92f518dd241d074646b0d9133aed5dac73273b950e460f2`.

**Status: offline implementation and tests pass; hardware validation is pending.
The full requested voltage-triggered weak-source protection is NOT implemented.**
There is a conservative response to an observed lower PMU input setting, but
its electrical cause is unknown. A complete collapse can still be mistaken for
a detach and restart the ramp. This entry is an experiment, not evidence that
arbitrary weak USB ports can supply the board maximum. A second limitation is
that a VBUS pulse shorter than all polling observations can be missed.

## iOS evidence

Binary: iOS 16.7.10 J71t kernelcache, SHA256
`c6b95fcf0efe52ee9258454d01d14adc9b0cb922d94c54a04a4ce11d1ae81e0a`.
Addresses below are full unslid virtual addresses. Shared Ghidra was used;
small missing function bodies were read with Ghidra `read_memory` and decoded
with Capstone. Earlier focused Ghidra output also exists under
`build/periph/d2365-local/`. New raw extracts remain under `build/charge7/re/`.

| Verified path | Address | Finding |
| --- | --- | --- |
| Board host ceiling | `fffffff005f15504` | `usb-input-limit-max`, default 1000 mA; J71t ADT supplies 2400 mA. The USBHost accessory item uses the platform ceiling, separate from gadget enumeration/extra-power accounting. See [the charging study](ds-charging-policy.md). |
| Analog monitor | `fffffff005ff5a08` | Under the Tristar lock, route reg `0x01` to D+/D−, wait 5 ms, sample, restore. For monitor 4/5, reg `0x03` is `9/a` or `b/c` by orientation for 40 ms, then cleared before restoring `0x01`. |
| Apple divider decoding | `fffffff005de44c4`, `fffffff005de450c`, table `fffffff005476a30` | Thresholds 1500/2200/2880 mV. Current table is 500, 1000, 1000, 2100, 2400, **3000**, 1000, 1000, 1000 mA. Linux bounds the 3000 entry to this board's 2400 maximum. Do not assume a brick's printed wattage determines its sampled signature. |
| BC1.2 primary | `fffffff005de33e4` | Ordinary USB connect type 0: D− in inclusive range 326–899 mV requests secondary detection. |
| BC1.2 secondary | `fffffff005de35dc` | For ordinary USB, D+ below 326 mV selects DCP, otherwise CDP. The item initializer `fffffff005de3424` gives either 1500 mA for connect type 0 (table `fffffff0054769f8`). |
| Detection orchestration | `fffffff005de8328` | Passive Apple divider detection precedes primary and secondary measurements. |
| Input setter | `fffffff006bbfed0`, `fffffff006bc0038` | Read `0xa03`, calibrated encoding, RMW only `0xa00[1]` from the below-75-mA flag, write `0xa03`. |
| Battery charge setter | `fffffff006bbfd64` | Write `0xa08`, bounded by attach-time `0xa0c[5:0]`. Existing Linux order is battery setting then input setting, without an input-disable transaction ritual. |
| Generic ramp/back-off | `fffffff005f1273c` (`armpmu_ramp_input_current`) | Initial 100 mA, bounded approximately 100 mA steps, source method `+0xc0` for input limiting, method `+0xf0` for the next step, input setter `+0xd8`. Downward limiting records a back-off and schedules reevaluation; source reporting waits about 500 ms after a step. |
| Back-off storage | `fffffff005f0c87c` (`armpmu_record_input_backoff`) | Store source expansion `+0x1c`, attempt measured input-current method `+0x138`, arm a 300-second reset timer. Reset action begins at `fffffff005f12cc8` and clears the stored back-off. |
| D2365 vtable | `fffffff006fd48d0` | `+0xc0 → fffffff006bc038c`: **`mov w0,#0; ret`**. This backend never reports input limiting through the generic ramp's callback. `+0x138 → fffffff006bc026c` returns unsupported (`0xe00002c7`), so measured input current is unavailable too. |
| Charger status | `fffffff006bba31c`, `fffffff006bc03d0` | Read-only status bank `0x180..0x19d`; `0x18a[2]` termination, `[4]` timeout. No verified voltage-collapse meaning for another bit was found. |
| Charger IRQ dispatch | `fffffff006bbf868`, handler `fffffff006bbfae0` | Attach registers masks at `fffffff005915d86` and `fffffff005915d68`; handler either publishes a source event or reads the status bank and publishes a charger event. No verified droop interrupt was identified. No IRQ/mask writes were added. |
| VBUS detection | `fffffff005ff498c` | Tristar `0x0c`: strict requires both low bits; non-strict accepts either. This establishes presence, **not a calibrated voltage or a VINDPM threshold**. |
| Optional voltage hook | `fffffff005febc58` | Tries `function-vbus_voltage`, otherwise falls back to digital detection. That property is absent from this board's ADT. PMU ADC channel 1 is the Tristar pin monitor, not a proven VBUS monitor. |

No iOS VINDPM enable/threshold setting was proved. A constant-false callback
does not prove that the silicon lacks an autonomous analog regulation loop;
it means this investigation cannot rely on one as verified protection. In
particular, do not label the existing post-attach `0xa03` changes as AICL or
collapse events. The device captures show changes, not their cause.

The four new understood Ghidra functions were renamed (ramp, back-off record,
BC primary and secondary). The post-rename `save` command returned failure;
the shared process was left running. Persistence of those renames is not
claimed. The source addresses and extracts above do not depend on the names.

## Implementation

Analog sampling now runs through `apple_s8000_usb_role_monitor()`, holding the
same hardware mutex as host entry/exit for the entire operation. Lock order is
instance → role hardware → Tristar → gadget budget. The callback never queries
or sets the USB role. Host role, shutdown and a role transition exclude the
callback. A failed route/DCP-control restore returns EUCLEAN and blocks a later
host entry until a guarded callback repairs it.

Before and after each ADC sample, the driver checks the gadget snapshot and
an atomic USB-event generation. Connected, configured or host-granted gadgets
exclude the monitor. An intervening event discards the sample; `brick_sampled`
is set only after a complete valid classification and a final strict VBUS
check. Reg `0x03` is always cleared before the saved reg `0x01` is restored.
The old uncertain-ADC-completion poison remains latched; unplugging cannot
make an uncertain conversion fresh. No active USB traffic is deliberately
sampled, and no USB descriptor, NCM/ECM/ACM configuration or PHY sequence changed.

The physical source epoch advances on a successful `0x0c` read with **neither
VBUS bit present**, after a previously present source. USB suspend, reset,
configuration budget changes and I²C failures do not advance it. The epoch is
read through an internal supplier API with a held power-supply reference and
device link. The charger checks it each worker tick and again in its guards.
Even if Tristar sees detach and reattach between two charger ticks, the new
epoch clears the old user override and all attach-specific timers before the
new source is armed.

On measured removal, Tristar clears divider result/sample status, sticky
computer identity, gadget extra grant/base/configuration accounting, camera
ID/adapter/handshake state, and requests release of an accessory-owned host
role. The gadget accounting helper performs **no** unbind, endpoint or PHY I/O.
The accessory controller still owns all role changes. A single observed empty
VBUS interval now invalidates the old power handshake, rather than waiting
for its three-poll unplug debounce.

| State/event | Result |
| --- | --- |
| Unknown device-role source | 500 mA floor; wait for guarded analog detection or evidence of a USB host. |
| USB host seen, same epoch | SDP identity stays across link PM/reset. Use board `usb-input-limit-max`; gadget `current_max` remains its separate USB accounting value. |
| Apple divider | Detected table ceiling, capped by the board. |
| BC1.2 | DCP/CDP 1500 mA ceiling; the existing supported ladder selects **1000 mA**, not an invented 1500 mA calibration. A proven BC source retains its classification if a link later appears. |
| Camera adapter | ACA, 2100 mA after the existing power handshake; 500 mA before success. No generic host elevation applies to ACA. |
| Manual host without ACA | No charging from the iPad's own host port. |
| Real detach/new epoch | Restore old handoff snapshot, revoke user cap/disable, clear holding/sample/ramp/retry/back-off state, then start the new source at the floor. |
| USB link PM/reset | Preserve source epoch, detected ceiling, user cap and adaptive state. An interrupted analog sample is discarded. |
| Thermal/gauge/ownership guard | Existing 45/42 °C hysteresis, 3.0–4.4 V pack sanity, status/cap checks, transport grace and bounded ownership fault retry remain. |

Every source is bounded by the board ceiling. A positive sysfs
`input_current_limit` write keeps the existing supported-value validation and
acts as a per-attach cap; it cannot raise a brick/ACA above its detected
ceiling. Zero disables until the next physical source epoch or a new positive
write. Positive requests made during deferred battery guards remain pending.

All four ADT text dumps (`j71s:3270`, `j71t:3273`, `j72s:3303`, `j72t:3313`)
say `usb-input-limit-max: 0x960`. The common iPad 5 DT node now describes
2400000 µA. J71t's owner-specific property is removed from DT and binding.
This does not enable the J71t peripheral driver on the other three boards;
their compatible/wiring/calibration still need their own validation. No
loader ADT copy is required for this constant.

## What the conservative adaptive path does

If the owned `0xa03` value is lower than the driver's last setting, while the
input-enable bit and battery-current setting still match, the driver chooses
the next lower supported budget, or a lower rung if the observed code calls
for it. It never immediately raises an observed input code below the floor's
calibrated setting. It holds the adaptive cap for 60 seconds, then retries at
most one upward rung per minute, bounded by the detected ceiling and user cap.
New PMU reductions restart that hold. A new physical epoch resets the state.
Other unexpected register changes retain the existing one-reassert/ownership
fault behavior.

`/sys/class/power_supply/charger/charge7_state` reports epoch, numeric USB type,
ceiling, nominal rung, live decoded input setting, user/adaptive caps, back-off
count and `last_backoff=pmic-input-reduced`. It explicitly reports
`vbus_mv=unavailable`. Standard `usb/usb_type` provides the human-readable type.
Activation and back-off logs are rate limited.

This is **not equivalent to voltage-triggered AICL**. It cannot respond to a
sag that leaves `0xa03` unchanged. It has no verified voltage hysteresis, and
cannot prevent a port's overcurrent shutdown. Requirement 4 remains open until
a safe VBUS/collapse indication or the hardware regulation setting is proved.
No new PMU registers were introduced: charger writes remain `0xa03`, `0xa08`,
and `0xa00[1]`; the analog detector retains its existing ADC trigger/result path.
Battery gauge writes remain disabled. No factory/boot storage is touched.

## Offline validation and artifacts

`scripts/periph/check-charge7.sh` runs actual extracted driver functions with
ASan/UBSan, a scripted regmap/Tristar, and a real pthread interleaving for the
role mutex. Cases cover 25 source/ceiling combinations; Mac→brick→Mac;
camera→Mac; detach/reattach between charger ticks; per-attach overrides; link
PM/reset; source-type/role changes; each ADC cancellation; both BC thresholds;
route failure/repair; PMU reduction ladder/hold/retry; thermal/gauge guards;
and 17 injected transaction/rollback failures.

Additional unchanged accessory-policy regression: 73 checks and 8/8 mutation
tests. ID-bus core and 14 independent CRC vectors pass. Four freshly rebuilt
objects (`apple_tristar_power`, `apple_j71t_charger`, USB role driver, gadget
core) pass `W=1` with **zero warnings/errors**. Strict checkpatch has 0 errors,
0 warnings, 0 checks after ignoring only the combined-diff DT split warning
and new-file MAINTAINERS reminder. Full build: Image.gz plus 105 DTBs,
debug info off. Final packaging rechecks clean source, identical USB-host5b
config/initramfs/cmdline (except entry name), all four DT ceilings, payload
markers and SHA256s. Final package/build output is in `build/charge7/`.

The bounded watcher also passes a two-second fixture-sysfs run in Docker.
See `build/charge7/check.log`, `w1.log`, `checkpatch.log`, `watcher-test.tsv`,
and `final-build.log` for the original output. Device checks in
[charge7-test-plan.md](charge7-test-plan.md) remain **NOT RUN**.
Run `python3 -B -I scripts/periph/verify-charge7.py` to recheck the delivered hashes.

The existing `scripts/periph/kernel/*.c` convention is retained. Those two
mirrors now include their USB-host5b baseline as well as charge7; the kernel
diff itself is only relative to `2801e838fc`. `prepare-charge7.py` copies the
mirrors and applies `charge7-support.patch` only to `/src/wt/charge`.

## Porting notes for other A-series boards

The owner's report is that OrchardLinux J71b (iPad 6, T8010) reaches about
425 mA with an out-of-tree `j71b_charge_policy` module. That number is not an
independently measured result here. It could describe a battery-current cap,
not a USB input limit: our nominal 500 mA input rung uses a conservative
400 mA battery-current ceiling and can discharge under system load.

If its PMIC and Tristar are the same family, transfer the separation of
source classification from USB link accounting, per-attach epoch handling,
role-serialized Apple/BC detection, calibrated D2365 input/battery policy and
board ADT `usb-input-limit-max`. Read that board's own ADT, register revision,
CBAT, electrical role gates and battery guard ranges. Do not copy this unit's
calibration, hardcode 2400 mA or assume a T8010 VINDPM register. Inspect the
module's programmed input **and** battery limits before attributing 425 mA to
negotiation. A valid detected ceiling plus the board maximum explains our
current selection; weak-source voltage feedback remains the open part.
