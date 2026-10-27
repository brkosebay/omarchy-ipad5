# J71t charging: Round 3 implementation and evidence

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-22. **Verified statically in Ghidra and offline tests; charging on
Linux remains to be verified on the device.** No implementation agent
accessed the iPad. Apple binaries, calibration and raw decompilations remain
under `build/`.

## Reference and implementation

**Verified by the orchestrator:** test 1 discharged at about -535 mA on the
Mac port. It captured charger status Discharging and type Unknown, but did
not capture input_current_limit. On the same unit and port under iOS,
`build/firmware/private/IOService.txt` reports USB host 5000 mV, accessory
current/base and PMUConfiguration 2400 mA, ChargerConfiguration 2500 mA,
IsCharging Yes, +1704 mA battery current and 3918 mV. Relevant lines are
7650/7659/7679, 13504/13512/13516/13517/13534/13536/13545/13547.

The new J71t driver implements those **2400 mA input / 2500 mA battery
limits** as a bounded experiment. It requires an explicit userspace request
for the owner-verified Mac source. It does not infer a 2400 mA entitlement
from VBUS presence. The next-boot commands and restoration test are in
[test-plan.md](test-plan.md).

The separate J172 policy is unchanged. J71t now has its own charger driver
and a read-only CBTL1610 supplier. Both supply nodes expose `online`; the
charger exposes actual `input_current_limit` and `constant_charge_current`
readback. It is classified as a USB supply. Reading input current describes
the programmed setting, not a measured current draw or a negotiated budget.

## Register-by-register verification

Source kernelcache SHA256:
`c6b95fcf0efe52ee9258454d01d14adc9b0cb922d94c54a04a4ce11d1ae81e0a`.
Shared Ghidra plus focused independent projects over the exact extracted
Mach-O sections were used. The focused projects fill function-definition
gaps without changing or stopping the shared server.

| Register | Verified iOS operation and evidence | Linux use |
|---|---|---|
| D2365 0xa00 bit 1 | `fffffff006bc0038` RMWs only this bit from the below-75-mA flag; `fffffff006bc0550` sets it on input shutdown | Temporarily suspend input around budget changes; preserve all other bits; clear for enabled input; restore the saved bit |
| D2365 0xa03 | `fffffff006bbfed0` writes the calibrated input code; encoder `fffffff006bbff6c` | This unit's CBAT maps 2400 mA to `0xba`; snapshot and verify writes/readback |
| D2365 0xa08 | `fffffff006bbfd64` converts battery-current target to approximately 50-mA steps, capped by attach-time maximum | Write `0x32` for 2500 mA; snapshot, verify and restore |
| D2365 0xa0c low six bits | Attach `fffffff006bbf868` reads this as the maximum charge-current code | Read only; reject if below `0x32` |
| D2365 0x18a | Status-bank offset 0x0a; bits 1:0 charging, bit 2 termination, bit 4 timeout, corroborated by `fffffff006bc03d0` | Read only; termination/timeout prevent or revoke the experiment |
| Tristar 0x0f | `fffffff005ff4500` reads and publishes TriStar REV | Read only; require this unit's observed `0x8a` |
| Tristar 0x0c | Strict getVBUSDetect `fffffff005ff498c`: `(byte & 3) == 3`, false on error | Read only, live before policy operations and polled every 250 ms |

The register-0xa00 bit operation is verified; the descriptive name
input suspend is **inferred** from its callers. Linux follows that same
operation and does not write the rest of the byte as an invented constant.
No charge-voltage, watchdog, interrupt, maximum-cap or factory-data writes
are introduced.

The D2365 power-source vtable is `fffffff006fd48d0`: +0xd8 is the calibrated
input setter and +0x100 the battery-current setter. The ARMPMU policy at
`fffffff005f11e28` passes current requests through the source objects and
publishes `PMUConfiguration` via `fffffff005f124b4`; this connects the
registry's input budget to the input-setting calculation rather than to the
distinct battery-current register. Raw outputs are in
`build/periph/pmu-local/`, `d2365-local/` and `ghidra-round3/`.

## Calibration and transport

**Verified:** CBAT is a resolved 24-byte ADT record. It is staged privately
as `apple/j71t-CBAT.bin`; the kernel uses `request_firmware_direct()`, so a
missing record cannot wait for a sysfs helper. Unsupported/missing data
leaves telemetry available and rejects charging writes. No unit-specific
calibration words are embedded in source, patches or DT.

The exact signed Q16 high-range formula, its Ghidra entry points and the
Linux validation bounds are documented in [cbat-recon.md](cbat-recon.md).
For this unit the integer calibrated target is 2400 mA, giving byte `0xba`.
That equality is incidental, not a reason to skip CBAT.

**Verified:** J71t has CBTL1610 at I2C0 address 0x1a. AppleTriStar's read
wrapper `fffffff005fead50`, AppleARMIICDevice command construction and the
hardware transfer at `fffffff006071bd8` establish an 8-bit selector followed
by **STOP**, then a separate read START. Linux uses two adapter-locked
SMBus BYTE operations to match it. The pointer selector is not a Tristar
configuration-data write. The driver never consumes event register 0x0d.
Details and shared function renames are in [tristar-recon.md](tristar-recon.md).

## Bounded policy and restoration

Writing `2400000` to charger/input_current_limit explicitly authorizes the
measured Mac source. Other nonzero requests are rejected. A zero request
revokes the experiment and restores the captured handoff settings.

Before any budget write the driver requires valid CBAT, strict live VBUS,
a present battery at 10–40 degrees C and 3.0–4.2 V, no termination/timeout,
and a hardware cap allowing 2500 mA. It snapshots 0xa00/0xa03/0xa08,
asserts input suspension, writes and verifies the two budgets, rechecks the
guards, clears suspension, and rechecks again. Unrelated control bits are
preserved. The thresholds are conservative Linux test-policy choices,
**not a claim to reproduce iOS's full thermal/voltage curve**.

While active, a one-second worker rechecks battery/source/status/cap and
all owned settings. Tristar presence changes also schedule it immediately.
A disconnect, read error, guard/integrity failure or suspend revokes the
request. Restoration asserts suspension first, verifies both saved budgets,
then restores only the saved suspend bit. On failure it retains the snapshot
and retries every 100 ms while keeping input suspended when possible.
Shutdown/removal retries three times and logs a critical error if the bus
cannot restore. A failed suspend keeps retry work alive. A permanently
unresponsive bus cannot provide a guaranteed restoration; no fabricated
success is reported.

Reconnection/resume never automatically re-arms charging. Polling can miss
very short disconnect/reconnect transients; use only the measured Mac port
for this iteration. The complete accessory enumeration/current-entitlement
path remains **unresolved**, so automatic high-current policy on other
sources is not implemented.

## Validation and remaining device check

`python3 -B scripts/periph/test-charging.py` compiles the actual driver
calibration, guard and transaction functions against a faulted regmap with
ASan/UBSan. **Verified:** 56 fault/guard cases pass, including partial writes,
readback mismatch, guard loss, failed/persistent rollback, quiesced budget
writes, preservation of unrelated bits and exact restoration. The real
private CBAT is checked without printing its words. Workqueue/lifetime
and PM behavior were reviewed separately; this is not a hardware test.

**Still to verify on the iPad:** successful CBTL1610 probe, baseline input
budget, requested register readbacks, positive battery current under the
same load/SOC, and restoration/disconnect behavior. About +1.7 A is the iOS
comparison target, not a guaranteed draw. If the battery remains negative
despite verified budgets, capture the log and state; no other PMIC registers
are authorized by the current evidence.
