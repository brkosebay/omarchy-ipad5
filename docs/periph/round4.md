# Round 4 peripherals report

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-22. Kernel `c7c3eb2d34`; 50 exported patches from `6831bc701`.
The implementation agent accessed no device. Touch remains with its separate
workstream and its recon file is unchanged.

## Hardware evidence from Round 3

**Verified by the orchestrator's integration boot:** manual 2400-mA input
programming changed the battery from −699 mA to steady +1.168…1.179 A within
33 seconds. Voltage rose from 3.855 to 3.874 V and SOC from 36 to 37 percent.
This confirms positive charging with the calibrated `0xba` input code and
2500-mA battery-current ceiling; it does not establish a constant +1.7 A
outcome under Linux. Evidence: `docs/test-logs/2026-10-22-charging-*.txt`.

**Verified:** FearA USI patch build 1312 loaded and LE discovery worked. The
later management capture uses this unit's real address. The boot warning
shows that the address was initially the ROM default; the saved capture
cannot establish whether a manual management command or later property fixup
changed it. ALS and battery telemetry also worked in that integration boot.

## Automatic charging

**Verified in Ghidra and the private IOService dump:** this Mac's iOS budget
comes from `IOAccessoryUSBConnectShim`'s USBHost source at 2400 mA. Its analog
BrickID contribution is zero, with both data pins at 39 mV. The actual host
protocol is a zero-length vendor request: type `0x40`/`0xc0`, request `0x40`,
**wIndex = signed extra active mA, wValue = signed suspend mA**. The active
iOS resolver uses 500 + extra when both base and extra are nonzero; the J71t
platform ceiling is 2400 mA. The USB hook records received fields and publishes
a bounded allowance to Tristar. It does not infer host identity from VBUS.

**Implemented and verified offline:** charging starts automatically from a
fresh source allowance. An observed host grant can authorize 2400 mA;
configured ordinary USB is capped at 500 mA, enumeration at 100 mA. USB reset,
disconnect, suspend, rebind and failed control transfers revoke elevated
host authorization. Resume requires a fresh elevated grant, a conservative
Linux difference from iOS's cached resume policy.

For an unenumerated charger, Tristar temporarily selects the verified D+/D−
monitor route, waits 5 ms, obtains fresh D2365 channel-1 ADC samples, and
restores the exact original route after each sample. The iOS resistor table
selects 500/1000/2100/2400 mA, with its generic 3000-mA entry capped at J71t's
2400 mA. Source type is `Apple_Brick_ID`; BC1.2 DCP classification is not
claimed. Unknown unenumerated USB falls back to 500 mA. A missing USB-controller
snapshot permits only 100 mA and no elevated analog allowance.

The ADC requires completion bit `0x156[0]` observed low before its one-byte
channel-1 trigger at `0x4000`, then a new completion before reading `0x4001`.
No interrupt masks or ACK registers are written. Uncertain trigger delivery,
polling errors and timeout poison further ADC use until reboot. No verified
abort/idle register exists. First-boot ADC ownership after a two-second delay
is **inferred**; completion polling with the inherited IRQ mask is **pending
device verification**. A USB reset can race the bounded analog measurement;
post-sample state checks discard its allowance and the exact mux is restored.

Private CBAT supplies the middle/high calibration branches. This unit's
nominal budgets produce input codes 500→`0x20`, 1000→`0x47`, 2100→`0xa0`,
2400→`0xba`. The calibrated 500-mA fallback reads back **475000 µA**; the
nominal source allowance remains 500000 µA. Below the ADT's 1000-mA charging
threshold, Linux selects input-only operation with battery charge current 0.
This is a conservative Linux policy, not a reconstruction of every iOS
thermal/charge curve.

All previous VBUS/revision, private-calibration, battery presence, 10–40 °C,
3.0–4.2 V, charge termination/timeout and hardware current-cap guards remain.
Writes still affect only `0xa00[1]`, `0xa03`, `0xa08`, plus the independently
verified temporary Tristar mux/ADC trigger. Every charger write is checked;
rollback retains and retries the original handoff snapshot. Hardware/I/O
faults latch automatic charging off. Manual `input_current_limit` writes are
now source-bounded caps; zero disables automatic policy and restores handoff.
They cannot force 2400 mA on an unknown source.

**Remaining device uncertainty:** HoolockRD uses Apple VID `05ac`, PID `4142`.
Whether macOS sends the Apple grant to those Linux descriptors is unknown.
If it does, automatic 2400 mA is implemented end to end. If it does not, the
safe 500-mA configured fallback applies; identifying the host-driver matching
requirement remains necessary before automatic Mac high-current charging can
be called hardware-verified. No product-ID substitution or forced grant was
introduced. See [source reconstruction](round4-source-recon.md),
[ADC evidence](round4-adc-recon.md) and [USB interface/tests](round4-usb.md).

## DT, Bluetooth and desktop interfaces

The J71t-only DT patch changes PMIC `#size-cells` to zero and consistently
uses address-only RTC/NVMEM/charger registers. A separate NVMEM size property
preserves its length; the provider retains legacy two-cell support for other
boards. J71t NVMEM is read-only. **Verified from OF control flow:** this avoids
attempting CPU address translation through the I2C parent that produced the
bad-cell-count warnings. Their absence is **pending a boot check**. See
[PMIC and UPower evidence](round4-power-ux.md).

Bluetooth now programs and reads back the real address after Patchram reset,
before checking for a ROM default. It prefers a valid DT identity, otherwise
direct-loads the private six-byte `apple/j71t-BMac.bin` staged from this unit's
resolved Bluetooth `local-mac-address`. Failure rejects setup promptly. Only
controller RAM changes. The TX-power quirk still skips the unsupported command;
its established-workaround diagnostic moves to debug level, while other
broken-command warnings remain. Both fixes are **pending device verification**;
see [Bluetooth report](round4-bt.md).

`usb` and `charger` expose USB type, online and system scope; `usb/current_max`
reports the allowed nominal budget, and charger input/current attributes report
programmed hardware limits. The existing battery exposes charge-now/full,
voltage/current, capacity, temperature, health and cycle count for UPower's
charge-unit backend. No measured energy or design capacity was fabricated.
`charge_now` plausibility and an actual UPower session still need capture.

## Build and validation

**Verified offline:** full Image.gz and 104 Apple DTBs build with a clean
worktree; all 50 mail patches replay exactly from the baseline to the head.
Scoped bindings and DT checks pass; optional yamllint is unavailable. New
patches pass strict checkpatch. Kernel/DT verification checks source-policy
linkage, PMIC cell layout and J71t-only board opt-in.

Tests compile actual driver functions with ASan/UBSan: 114 Bluetooth assertions,
160 USB assertions, 56 original charging fault/guard cases plus new
calibrated-budget/source/automatic-policy/fault-latch cases, all 4096 ADC raw
values, classifier boundaries and 13 mux fault paths. Fifteen firmware tests
and two initramfs regressions pass. These validate code paths with mocks;
they do not validate hardware timing or host-protocol delivery.

Firmware verification matches 13 peripherals files. Other workstreams' two
touch files are retained without changes. Static btmgmt remains included.
The initramfs contains HoolockRD plus the overlay in **one gzip member**,
preserving `/init` through m1n1. Provenance, artifact hashes and full test
results are in `build/periph/round4-final.json`.

```sh
TREE=periph scripts/mkpayload.sh build/initramfs-fw.gz
```

The next boot procedure is [test-plan.md](test-plan.md). Capture automatic
source/current behavior before any manual limit or address changes.
